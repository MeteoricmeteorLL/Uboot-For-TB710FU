// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 * (C) Copyright 2002-2006
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2002
 * Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Marius Groeger <mgroeger@sysgo.de>
 */

#include <config.h>
#include <api.h>
#include <bootstage.h>
#include <cpu_func.h>
#include <linux/arm-smccc.h>
#include <cyclic.h>
#include <display_options.h>
#include <exports.h>
#ifdef CONFIG_MTD_NOR_FLASH
#include <flash.h>
#endif
#include <hang.h>
#include <image.h>
#include <irq_func.h>
#include <lmb.h>
#include <log.h>
#include <net.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <u-boot/crc.h>
#include <binman.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <efi_loader.h>
#include <env.h>
#include <env_internal.h>
#include <fdtdec.h>
#include <ide.h>
#include <init.h>
#include <dm/uclass.h>
#include <initcall.h>
#include <kgdb.h>
#include <irq_func.h>
#include <led.h>
#include <malloc.h>
#include <mapmem.h>
#include <miiphy.h>
#include <mmc.h>
#include <mux.h>
#include <nand.h>
#include <of_live.h>
#include <onenand_uboot.h>
#include <pvblock.h>
#include <scsi.h>
#include <asm/system.h>
#include <cli.h>
#include <string.h>
#include <part.h>
#include <gzip.h>
#include <blk.h>
#include <linux/libfdt.h>
#include <video_console.h>

/* lib/initcall.c */
void tb_screen_hexline(int y, ulong v);
void tb_screen_log_reset(void);
void tb_screen_fixed_hex(int y, ulong v, int scale);
void tb_screen_log_hex(ulong v, int scale);
void tb_screen_log(const char *s, int scale);
void *tb_fdt_load(void);
void tb_logv(const char *label, ulong v);
void tb_mark(int idx);

/* TB710FU: the patched device-tree copy, recorded for the hand-off at the end
 * of board_init_r (the buffers themselves are block-local). */
static void *tb_kfdt __maybe_unused;

/* TB710FU: one bit of cross-boot state, for when neither the panel nor USB can
 * be believed.  ABL reads the BCB in misc when it boots, so writing a one-shot
 * command there is a signal the host can see without any cooperation from the
 * payload that follows.  512 bytes is the BCB's command/status area; the slot
 * records are in devinfo. */
static int __maybe_unused tb_misc_bcb(int set)
{
	char buf[512];
	int di;

	memset(buf, 0, sizeof(buf));
	if (set) {
		const char *cmd = "bootonce-bootloader";

		for (di = 0; cmd[di] && di < 31; di++)
			buf[di] = cmd[di];
	}

	for (di = 0; di < 128; di++) {
		struct blk_desc *d = blk_get_dev("scsi", di);
		struct disk_partition info;
		int pi;

		if (!d)
			continue;
		part_init(d);
		for (pi = 1; pi <= 64; pi++) {
			if (part_get_info(d, pi, &info))
				break;
			if (!strcmp((const char *)info.name, "misc"))
				return blk_dwrite(d, info.start, 1, buf);
		}
	}

	return -ENODEV;
}

/* TB710FU: the Gunyah hypervisor watchdog. XBL/ABL leaves it armed and nothing
 * in our payload pings it, so the hypervisor resets the VM about 30 seconds
 * after handoff. This is the same SMCCC interface the kernel's gunyah_wdt
 * driver uses (vendor hypervisor owner 6); disabling it lets a panel or a
 * kernel log outlive 30 seconds. Bit 1 of the control value is reserved by
 * Gunyah and has to stay set, so "disable" is 2, not 0. */
#define TB_GHWDT_FID(f)		(0x86000000UL | (f))
#define TB_GHWDT_CONTROL	TB_GHWDT_FID(0x0005UL)
#define TB_GHWDT_STATUS		TB_GHWDT_FID(0x0006UL)
#define TB_GHWDT_PING		TB_GHWDT_FID(0x0007UL)
#define TB_GHWDT_WDT_OFF	2UL

static void __maybe_unused tb_ghwdt_off(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(TB_GHWDT_STATUS, 0, 0, 0, 0, 0, 0, 0, &res);
	tb_logv("GHWDT ST", (ulong)res.a0);
	/* Green if the hypervisor watchdog service answered, red if
	 * it did not -- the one fact that decides whether the SMCCC
	 * interface exists on this board. */
	tb_mark(res.a0 ? 7 : 0);
	if (res.a0)
		return;			/* no service, or it refused */

	arm_smccc_smc(TB_GHWDT_CONTROL, TB_GHWDT_WDT_OFF, 0, 0, 0, 0, 0, 0,
		      &res);
	tb_logv("GHWDT OFF", (ulong)res.a0);
}
void tb_logv2(const char *label, ulong a, ulong b);
u64 tb_ram_rd64(ulong addr);
ulong tb_ram_nonzero(ulong start, ulong end);
void tb_ram_fill(ulong start, ulong end, u64 val);
void tb_ram_count(ulong start, ulong end, u64 val, ulong *hits, ulong *zeros);
ulong tb_ram_find(const char *needle, ulong start, ulong end);
void tb_ram_text(ulong addr, int max);
void tb_probe_dump(int scale);
ulong tb_ram_latest_log(ulong start, ulong end, ulong *ts);
void tb_ram_text_ring(ulong base, ulong size, ulong start, int len);

/* TB710FU: where the initramfs lives inside linboot, and how long it is.
 * The layout and the "TBIRD" header are written by mainline-kernel/tools/
 * pack_combined.py: 16 MiB of kernel, then magic[8] + u32 length + u32 rsvd,
 * then the gzip stream.  The length is read rather than compiled in, because a
 * hardcoded one silently truncated a rebuilt ramdisk: the kernel was handed a
 * truncated gzip stream, never unpacked init, and died at "Attempted to kill
 * init" -- with nothing on the panel to say why. */
#define TB_IR_BASE	0xad000000UL
#define TB_IR_HDR	16

static void __maybe_unused tb_initrd_get(ulong *start, ulong *len)
{
	const u8 *h = (const u8 *)TB_IR_BASE;

	if (h[0] == 'T' && h[1] == 'B' && h[2] == 'I' && h[3] == 'R' &&
	    h[4] == 'D') {
		*start = TB_IR_BASE + TB_IR_HDR;
		*len = (ulong)h[8] | ((ulong)h[9] << 8) |
		       ((ulong)h[10] << 16) | ((ulong)h[11] << 24);
		return;
	}

	/* No header: an image from before this change. */
	*start = TB_IR_BASE;
	*len = 1046178UL;
}

/* TB710FU: the APSS watchdog, so a diagnostic screen can outlive the
 * bootloader's own patience.  Register offsets are the standard qcom-wdt set
 * (drivers/watchdog/qcom-wdt.c, kpss match data):
 *     +0x4  WDT_RST   write 1 to pet
 *     +0x8  WDT_EN
 *     +0xC  WDT_BARK
 *     +0x10 WDT_BITE
 * Nothing has been petting it: sm8650.dtsi carries no watchdog node and
 * CONFIG_QCOM_WDT=m never loads.  The pet write is gated on bark/bite looking
 * like a real watchdog pair, so a peripheral that happens to live at this
 * address is left alone. */
#define TB_WDT_BASE	0x17C10000UL

static void __maybe_unused tb_wdt_show(void)
{
	volatile u32 *w = (volatile u32 *)TB_WDT_BASE;

	tb_logv2("WDT EN BK", w[2], w[3]);
	tb_logv("WDT BITE", w[4]);
}

static void __maybe_unused tb_wdt_pet_forever(void)
{
	volatile u32 *w = (volatile u32 *)TB_WDT_BASE;

	while (1) {
		struct arm_smccc_res res;

		/* The hypervisor's watchdog, over SMCCC: this is the one the
		 * kernel's gunyah_wdt driver talks to, and its 32-second cap is
		 * the window this screen keeps getting reset in. */
		arm_smccc_smc(TB_GHWDT_PING, 0, 0, 0, 0, 0, 0, 0, &res);

		/* And the APSS one, in case that is what is armed. */
		if (w[3] && w[4] && w[3] < w[4])
			*(volatile u32 *)(TB_WDT_BASE + 4) = 1;

		for (volatile int i = 0; i < 200000; i++)
			;
	}
}

/* Where the kernel keeps its log, from the System.map of the tree that built the
 * flashed Image: printk_rb_static and the text_data_ring fields inside it. */
#define TB_KERNEL_TEXT_VA	0xffffffc080000000ULL	/* _text as linked */
#define TB_LOG_RB_OFF		0x23b4498UL	/* printk_rb_static */
#define TB_LOG_RB_SIZEBITS	0x30
#define TB_LOG_RB_DATA		0x38
#define TB_LOG_RB_HEAD		0x40
#define TB_LOG_TAIL_BYTES	2000

/* Exact kernel addresses from the same System.map. The first three are file
 * residents zeroed at link time and never covered by the canary, so whatever
 * they hold at runtime was written by the kernel itself. */
#define TB_K_BSS_START		0x2916000UL	/* __bss_start, in .bss */
#define TB_K_LOG_BUF		0x292c170UL	/* __log_buf, in .bss */
#define TB_K_BOOT_ARGS		0x2395000UL	/* boot_args[] */
#define TB_K_IDMAP_PGD		0x2010000UL	/* idmap_pg_dir[] */
#define TB_K_KIMAGE_VOFFSET	0x1f74000UL	/* kimage_voffset */

/* Canary addresses. The gap at 0xaa915a00 is the linker's own padding between
 * the end of the image file (0x2915a00) and __bss_start (0x2916000): it is not
 * in the file, not in .bss, not written by the gunzip, not covered by the
 * kernel's later memset, and inside the region the v9 control run proved comes
 * back through a reset. 0xaa9e1000 is just past the image; 0x8f000000 is the
 * address the stub's record lived at and never retained. */
#define TB_PRB_GAP		0xaa915a40UL
#define TB_PRB_POST		0xaa9e1000UL
#define TB_PRB_OLD		0x8f000000UL

/* printk's head_lpos at link time: -(1 << 17), the text ring's size. Any other
 * value means printk advanced it, i.e. it formatted a record. */
#define TB_LOG_LPOS_INIT	0xfffffffffffe0000ULL

/* FNV-1a-32 over the first megabyte of the gunzipped kernel, from the file
 * that was flashed: a read path that returns this from RAM is exact. */
#define TB_ORACLE_LEN		0x100000UL
#define TB_ORACLE_FNV		0x2e502d46UL

/* The MMU is the one piece of state this project has been guessing at, so
 * read it instead: current_el() and get_sctlr() give this boot's level and
 * its SCTLR, and TTBR0/TCR say whether there is a translation table behind
 * it at all. */
static ulong tb_mrs_ttbr0_el1(void)
{
	ulong v;

	__asm__ volatile("mrs %0, ttbr0_el1" : "=r" (v) : : "cc");
	return v;
}

static ulong tb_mrs_tcr_el1(void)
{
	ulong v;

	__asm__ volatile("mrs %0, tcr_el1" : "=r" (v) : : "cc");
	return v;
}

static u32 tb_fnv1a32(ulong addr, ulong len)
{
	u32 h = 2166136261U;
	ulong i;

	for (i = 0; i < len; i++) {
		h ^= *(volatile unsigned char *)(addr + i);
		h *= 16777619U;
	}
	return h;
}

/* Mirror of tb_ram_rd64(): a byte at a time, so the store and the load are
 * the same width and a mismatch cannot come from the access size. */
static void tb_ram_wr64(ulong addr, u64 val)
{
	int i;

	for (i = 0; i < 8; i++)
		*(volatile unsigned char *)(addr + i) = (val >> (8 * i)) & 0xff;
}

int cleanup_before_linux(void);
int qcom_get_ram_banks(phys_addr_t *start, phys_size_t *size, int max);

/* XBL reports usable RAM as many small banks, cut apart by its own carveouts. */
#define TB_RAM_BANKS	16
/* A device tree reg property holding that many (address, size) pairs. */
#define TB_RAM_REGS	(TB_RAM_BANKS * 2)

/* /memory carries a unit address ("memory@a0000000") in these trees, so
 * fdt_path_offset("/memory") misses it; look the node up by name. */
static int tb_memory_node(const void *fdt)
{
	int node;

	for (node = fdt_first_subnode(fdt, 0); node >= 0;
	     node = fdt_next_subnode(fdt, node)) {
		const char *nm = fdt_get_name(fdt, node, NULL);

		if (nm && !strncmp(nm, "memory", 6))
			return node;
	}
	return -1;
}
#include <serial.h>
#include <video_font_8x16.h>
#include <status_led.h>
#include <stdio_dev.h>
#include <timer.h>
#include <trace.h>
#include <watchdog.h>
#include <xen.h>
#include <asm/sections.h>
#include <dm/root.h>
#include <dm/ofnode.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <wdt.h>
#include <asm-generic/gpio.h>
#include <relocate.h>

DECLARE_GLOBAL_DATA_PTR;

ulong monitor_flash_len;

__weak int board_flash_wp_on(void)
{
	/*
	 * Most flashes can't be detected when write protection is enabled,
	 * so provide a way to let U-Boot gracefully ignore write protected
	 * devices.
	 */
	return 0;
}

__weak int cpu_secondary_init_r(void)
{
	return 0;
}

static int initr_trace(void)
{
#ifdef CONFIG_TRACE
	trace_init(gd->trace_buff, CONFIG_TRACE_BUFFER_SIZE);
#endif

	return 0;
}

static int initr_reloc(void)
{
	/* tell others: relocation done */
	gd->flags |= GD_FLG_RELOC | GD_FLG_FULL_MALLOC_INIT;

	return 0;
}

#if defined(CONFIG_ARM) || defined(CONFIG_RISCV)
/*
 * Some of these functions are needed purely because the functions they
 * call return void. If we change them to return 0, these stubs can go away.
 */
static int initr_caches(void)
{
	/* Enable caches */
	enable_caches();
	return 0;
}
#endif

__weak int fixup_cpu(void)
{
	return 0;
}

static int initr_reloc_global_data(void)
{
#ifdef __ARM__
	monitor_flash_len = _end - __image_copy_start;
#elif defined(CONFIG_RISCV)
	monitor_flash_len = (ulong)_end - (ulong)_start;
#elif !defined(CONFIG_SANDBOX) && !defined(CONFIG_NIOS2)
	monitor_flash_len = (ulong)__init_end - gd->relocaddr;
#endif
#if defined(CONFIG_MPC85xx) || defined(CONFIG_MPC86xx)
	/*
	 * The gd->cpu pointer is set to an address in flash before relocation.
	 * We need to update it to point to the same CPU entry in RAM.
	 * TODO: why not just add gd->reloc_ofs?
	 */
	gd->arch.cpu += gd->relocaddr - CONFIG_SYS_MONITOR_BASE;

	/*
	 * If we didn't know the cpu mask & # cores, we can save them of
	 * now rather than 'computing' them constantly
	 */
	fixup_cpu();
#endif
#ifdef CONFIG_SYS_RELOC_GD_ENV_ADDR
	/*
	 * Relocate the early env_addr pointer unless we know it is not inside
	 * the binary. Some systems need this and for the rest, it doesn't hurt.
	 */
	gd->env_addr += gd->reloc_off;
#endif

	/*
	 * For CONFIG_OF_EMBED case the FDT is embedded into ELF, available by
	 * __dtb_dt_begin. After U-Boot ELF self-relocation to RAM top address
	 * it is worth to update fdt_blob in global_data
	 */
	if (IS_ENABLED(CONFIG_OF_EMBED))
		fdtdec_setup_embed();

#ifdef CONFIG_EFI_LOADER
	/*
	 * On the ARM architecture gd is mapped to a fixed register (r9 or x18).
	 * As this register may be overwritten by an EFI payload we save it here
	 * and restore it on every callback entered.
	 */
	efi_save_gd();

	efi_runtime_relocate(gd->relocaddr, NULL);

#endif
	/*
	 * We are done with all relocations change the permissions of the binary
	 * NOTE: __start_rodata etc are defined in arm64 linker scripts and
	 * sections.h. If you want to add support for your platform you need to
	 * add the symbols on your linker script, otherwise they will point to
	 * random addresses.
	 *
	 */
	if (IS_ENABLED(CONFIG_MMU_PGPROT)) {
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_rodata),
				 (size_t)(uintptr_t)(__end_rodata - __start_rodata),
				 MMU_ATTR_RO);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_data),
				 (size_t)(uintptr_t)(__end_data - __start_data),
				 MMU_ATTR_RW);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__text_start),
				 (size_t)(uintptr_t)(__text_end - __text_start),
				 MMU_ATTR_RX);
	}

	return 0;
}

__weak int arch_initr_trap(void)
{
	return 0;
}

#if defined(CONFIG_SYS_INIT_RAM_LOCK) && defined(CONFIG_E500)
static int initr_unlock_ram_in_cache(void)
{
	unlock_ram_in_cache();	/* it's time to unlock D-cache in e500 */
	return 0;
}
#endif

static int initr_barrier(void)
{
#ifdef CONFIG_PPC
	/* TODO: Can we not use dmb() macros for this? */
	asm("sync ; isync");
#endif
	return 0;
}

static int initr_malloc(void)
{
	ulong start;

#if CONFIG_IS_ENABLED(SYS_MALLOC_F)
	debug("Pre-reloc malloc() used %#x bytes (%d KB)\n", gd->malloc_ptr,
	      gd->malloc_ptr / 1024);
#endif
	/* The malloc area is immediately below the monitor copy in DRAM */
	/*
	 * This value MUST match the value of gd->start_addr_sp in board_f.c:
	 * reserve_noncached().
	 */
	start = gd->relocaddr - TOTAL_MALLOC_LEN;
	gd_set_malloc_start(start);
	mem_malloc_init(start, TOTAL_MALLOC_LEN);
	return 0;
}

static int initr_of_live(void)
{
	if (CONFIG_IS_ENABLED(OF_LIVE)) {
		int ret;

		bootstage_start(BOOTSTAGE_ID_ACCUM_OF_LIVE, "of_live");
		ret = of_live_build(gd->fdt_blob,
				    (struct device_node **)gd_of_root_ptr());
		bootstage_accum(BOOTSTAGE_ID_ACCUM_OF_LIVE);
		if (ret)
			return ret;
	}

	return 0;
}

#ifdef CONFIG_DM
static int initr_dm(void)
{
	int ret;

	oftree_reset();

	/* Drop the pre-reloc driver model and start a new one */
	gd->dm_root = NULL;
#ifdef CONFIG_TIMER
	gd->timer = NULL;
#endif
	bootstage_start(BOOTSTAGE_ID_ACCUM_DM_R, "dm_r");
	ret = dm_init_and_scan(false);
	bootstage_accum(BOOTSTAGE_ID_ACCUM_DM_R);
	if (ret)
		return ret;

	return dm_autoprobe();
}
#endif

static int initr_dm_devices(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_TIMER_EARLY)) {
		ret = dm_timer_init();
		if (ret)
			return ret;
	}

	if (IS_ENABLED(CONFIG_MULTIPLEXER)) {
		/*
		 * Initialize the multiplexer controls to their default state.
		 * This must be done early as other drivers may unknowingly
		 * rely on it.
		 */
		ret = dm_mux_init();
		if (ret)
			return ret;
	}

	return 0;
}

static int initr_bootstage(void)
{
	bootstage_mark_name(BOOTSTAGE_ID_START_UBOOT_R, "board_init_r");

	return 0;
}

__weak int power_init_board(void)
{
	return 0;
}

static int initr_announce(void)
{
	debug("Now running in RAM - U-Boot at: %08lx\n", gd->relocaddr);
	return 0;
}

static int __maybe_unused initr_binman(void)
{
	int ret;

	ret = binman_init();
	if (ret)
		printf("binman_init failed:%d\n", ret);

	return ret;
}

#if defined(CONFIG_MTD_NOR_FLASH)
__weak int is_flash_available(void)
{
	return 1;
}

static int initr_flash(void)
{
	ulong flash_size = 0;
	struct bd_info *bd = gd->bd;

	if (!is_flash_available())
		return 0;

	puts("Flash: ");

	if (board_flash_wp_on())
		printf("Uninitialized - Write Protect On\n");
	else
		flash_size = flash_init();

	print_size(flash_size, "");
#ifdef CONFIG_SYS_FLASH_CHECKSUM
	/*
	 * Compute and print flash CRC if flashchecksum is set to 'y'
	 *
	 * NOTE: Maybe we should add some schedule()? XXX
	 */
	if (env_get_yesno("flashchecksum") == 1) {
		const uchar *flash_base = (const uchar *)CFG_SYS_FLASH_BASE;

		printf("  CRC: %08X", crc32(0,
					    flash_base,
					    flash_size));
	}
#endif /* CONFIG_SYS_FLASH_CHECKSUM */
	putc('\n');

	/* update start of FLASH memory    */
#ifdef CFG_SYS_FLASH_BASE
	bd->bi_flashstart = CFG_SYS_FLASH_BASE;
#endif
	/* size of FLASH memory (final value) */
	bd->bi_flashsize = flash_size;

#if defined(CONFIG_SYS_UPDATE_FLASH_SIZE)
	/* Make a update of the Memctrl. */
	update_flash_size(flash_size);
#endif

#if defined(CONFIG_OXC) || defined(CONFIG_RMU)
	/* flash mapped at end of memory map */
	bd->bi_flashoffset = CONFIG_TEXT_BASE + flash_size;
#elif CONFIG_SYS_MONITOR_BASE == CFG_SYS_FLASH_BASE
	bd->bi_flashoffset = monitor_flash_len;	/* reserved area for monitor */
#endif
	return 0;
}
#endif

#ifdef CONFIG_CMD_NAND
/* go init the NAND */
static int initr_nand(void)
{
	puts("NAND:  ");
	nand_init();
	printf("%lu MiB\n", nand_size() / 1024);
	return 0;
}
#endif

#if defined(CONFIG_CMD_ONENAND)
/* go init the NAND */
static int initr_onenand(void)
{
	puts("NAND:  ");
	onenand_init();
	return 0;
}
#endif

#ifdef CONFIG_MMC
static int initr_mmc(void)
{
	puts("MMC:   ");
	mmc_initialize(gd->bd);
	return 0;
}
#endif

#ifdef CONFIG_PVBLOCK
static int initr_pvblock(void)
{
	puts("PVBLOCK: ");
	pvblock_init();
	return 0;
}
#endif

/*
 * Tell if it's OK to load the environment early in boot.
 *
 * If CONFIG_OF_CONTROL is defined, we'll check with the FDT to see
 * if this is OK (defaulting to saying it's OK).
 *
 * NOTE: Loading the environment early can be a bad idea if security is
 *       important, since no verification is done on the environment.
 *
 * Return: 0 if environment should not be loaded, !=0 if it is ok to load
 */
static int should_load_env(void)
{
	if (IS_ENABLED(CONFIG_OF_CONTROL))
		return ofnode_conf_read_int("load-environment", 1);

	if (IS_ENABLED(CONFIG_DELAY_ENVIRONMENT))
		return 0;

	return 1;
}

static int initr_env(void)
{
	/* TB710FU: 1 builds the ramoops dump-and-hang diagnostic instead of
 * booting the kernel; 0 builds the normal memory-boot image.
 */
#define TB_DIAG_RAMOOPS 0

/* TB710FU: env storage access (MMC) hangs bring-up (CLK_STUB);
	 * run with defaults. The code below is unreachable. */
	env_set_default("TB710FU bring-up", 0);
	return 0;
	/* initialize environment */
	if (should_load_env())
		env_relocate();
	else
		env_set_default(NULL, 0);

	env_import_fdt();

	if (IS_ENABLED(CONFIG_OF_CONTROL))
		env_set_hex("fdtcontroladdr",
			    (unsigned long)map_to_sysmem(gd->fdt_blob));

	#if (IS_ENABLED(CONFIG_SAVE_PREV_BL_INITRAMFS_START_ADDR) || \
						IS_ENABLED(CONFIG_SAVE_PREV_BL_FDT_ADDR))
		save_prev_bl_data();
	#endif

	/* Initialize from environment */
	image_load_addr = env_get_ulong("loadaddr", 16, image_load_addr);

	return 0;
}

#ifdef CONFIG_SYS_MALLOC_BOOTPARAMS
static int initr_malloc_bootparams(void)
{
	gd->bd->bi_boot_params = (ulong)malloc(CONFIG_SYS_BOOTPARAMS_LEN);
	if (!gd->bd->bi_boot_params) {
		puts("WARNING: Cannot allocate space for boot parameters\n");
		return -ENOMEM;
	}
	return 0;
}
#endif

static int initr_status_led(void)
{
	status_led_init();

	return 0;
}

static int initr_boot_led_blink(void)
{
	/* TB710FU bring-up: status LED sits behind the PMIC which is not
	 * usable this early; blinking it hangs the init sequence. */
	return 0;
	if (0)
	status_led_boot_blink();

	led_boot_blink();

	return 0;
}

static int initr_boot_led_on(void)
{
	led_boot_on();

	return 0;
}

#if defined(CONFIG_CMD_NET)
static int initr_net(void)
{
	/* TB710FU: no usable ethernet; eth_initialize() hangs the bring-up. */
	return 0;
}
#if 0
static int initr_net_orig(void)
{
	puts("Net:   ");
	eth_initialize();
#if defined(CONFIG_RESET_PHY_R)
	debug("Reset Ethernet PHY\n");
	reset_phy();
#endif
	return 0;
}
#endif
#endif /* CONFIG_CMD_NET (outer guard, kept by TB710FU edit) */

#ifdef CONFIG_POST
static int initr_post(void)
{
	post_run(NULL, POST_RAM | post_bootmode_get(0));
	return 0;
}
#endif

#if defined(CFG_PRAM)
/*
 * Export available size of memory for Linux, taking into account the
 * protected RAM at top of memory
 */
int initr_mem(void)
{
	ulong pram = 0;
	char memsz[32];

	pram = env_get_ulong("pram", 10, CFG_PRAM);
	sprintf(memsz, "%ldk", (long int)((gd->ram_size / 1024) - pram));
	env_set("mem", memsz);

	return 0;
}
#endif

static int initr_lmb(void)
{
	if (CONFIG_IS_ENABLED(LMB))
		return lmb_init();
	else
		return 0;
}

static int dm_announce(void)
{
	int device_count;
	int uclass_count;

	if (IS_ENABLED(CONFIG_DM)) {
		dm_get_stats(&device_count, &uclass_count);
		printf("Core:  %d devices, %d uclasses", device_count,
		       uclass_count);
		if (CONFIG_IS_ENABLED(OF_REAL))
			printf(", devicetree: %s", fdtdec_get_srcname());
		if (CONFIG_IS_ENABLED(UPL))
			printf(", universal payload active");
		printf("\n");
		if (IS_ENABLED(CONFIG_OF_HAS_PRIOR_STAGE) &&
		    (gd->fdt_src == FDTSRC_SEPARATE ||
		     gd->fdt_src == FDTSRC_EMBED)) {
			printf("Warning: Unexpected devicetree source (not from a prior stage)");
			printf("Warning: U-Boot may not function properly\n");
		}
		if (IS_ENABLED(CONFIG_OF_TAG_MIGRATE) &&
		    (gd->flags & GD_FLG_OF_TAG_MIGRATE))
			/*
			 * U-Boot will silently fail to work after 2023.07 if
			 * there are old tags present
			 */
			printf("Warning: Device tree includes old 'u-boot,dm-' tags: please fix by 2023.07!\n");
	}

	return 0;
}


/* TB710FU: unmistakable on-screen evidence that init finished. */
static void tb_beacon(void)
{
	unsigned *fb = (unsigned *)0xD5100000UL;
	int row, col;

	/* tag block: 200x200 at (1600,100) filled 0x00FF00FF (magenta) */
	for (row = 0; row < 200; row++) {
		unsigned *line = fb + (ulong)(100 + row) * 3200 + 1600;

		for (col = 0; col < 200; col++)
			line[col] = 0xFFFF00FF;
	}
	/* main-loop beacon: 600x200 at (1900,100) solid WHITE */
	for (row = 0; row < 200; row++) {
		unsigned *line = fb + (ulong)(100 + row) * 3200 + 1900;

		for (col = 0; col < 600; col++)
			line[col] = 0xFFFFFFFF;
	}
}

static int run_main_loop(void)
{
	tb_beacon();
#ifdef CONFIG_SANDBOX
	sandbox_main_loop_init();
#endif

	event_notify_null(EVT_MAIN_LOOP);

	/* main_loop() can return to retry autoboot, if so just run it again */
	for (;;)
		main_loop();
	return 0;
}

/*
 * Over time we hope to remove these functions with code fragments and
 * stub functions, and instead call the relevant function directly.
 *
 * We also hope to remove most of the driver-related init and do it if/when
 * the driver is later used.
 *
 * TODO: perhaps reset the watchdog in the initcall function after each call?
 */
static init_fnc_t init_sequence_r[] = {
	initr_trace,
	initr_reloc,
	event_init,
	/* TODO: could x86/PPC have this also perhaps? */
#if defined(CONFIG_ARM) || defined(CONFIG_RISCV)
	initr_caches,
	/* Note: For Freescale LS2 SoCs, new MMU table is created in DDR.
	 *	 A temporary mapping of IFC high region is since removed,
	 *	 so environmental variables in NOR flash is not available
	 *	 until board_init() is called below to remap IFC to high
	 *	 region.
	 */
#endif
	initr_reloc_global_data,
#if defined(CONFIG_SYS_INIT_RAM_LOCK) && defined(CONFIG_E500)
	initr_unlock_ram_in_cache,
#endif
	initr_barrier,
	initr_malloc,
	log_init,
	initr_bootstage,	/* Needs malloc() but has its own timer */
#if defined(CONFIG_CONSOLE_RECORD)
	console_record_init,
#endif
#ifdef CONFIG_SYS_NONCACHED_MEMORY
	noncached_init,
#endif
	initr_of_live,
#ifdef CONFIG_DM
	initr_dm,
#endif
#ifdef CONFIG_ADDR_MAP
	init_addr_map,
#endif
#if defined(CONFIG_ARM) || defined(CONFIG_RISCV) || defined(CONFIG_SANDBOX)
	board_init,	/* Setup chipselects */
#endif
	/*
	 * TODO: printing of the clock inforamtion of the board is now
	 * implemented as part of bdinfo command. Currently only support for
	 * davinci SOC's is added. Remove this check once all the board
	 * implement this.
	 */
#ifdef CONFIG_CLOCKS
	set_cpu_clk_info, /* Setup clock information */
#endif
	initr_lmb,
#ifdef CONFIG_EFI_LOADER
	efi_memory_init,
#endif
#ifdef CONFIG_BINMAN_FDT
	initr_binman,
#endif
#ifdef CONFIG_FSP_VERSION2
	arch_fsp_init_r,
#endif
	initr_dm_devices,
	stdio_init_tables,
	serial_initialize,
	initr_announce,
	dm_announce,
#if CONFIG_IS_ENABLED(WDT)
	initr_watchdog,
#endif
	INIT_FUNC_WATCHDOG_RESET
	arch_initr_trap,
#if defined(CONFIG_BOARD_EARLY_INIT_R)
	board_early_init_r,
#endif
	INIT_FUNC_WATCHDOG_RESET
#ifdef CONFIG_POST
	post_output_backlog,
#endif
	INIT_FUNC_WATCHDOG_RESET
#if defined(CONFIG_PCI_INIT_R) && defined(CONFIG_SYS_EARLY_PCI_INIT)
	/*
	 * Do early PCI configuration _before_ the flash gets initialised,
	 * because PCU resources are crucial for flash access on some boards.
	 */
	pci_init,
#endif
#ifdef CONFIG_ARCH_EARLY_INIT_R
	arch_early_init_r,
#endif
	power_init_board,
#ifdef CONFIG_MTD_NOR_FLASH
	initr_flash,
#endif
	INIT_FUNC_WATCHDOG_RESET
#if defined(CONFIG_PPC) || defined(CONFIG_M68K) || defined(CONFIG_X86)
	/* initialize higher level parts of CPU like time base and timers */
	cpu_init_r,
#endif
#ifdef CONFIG_EFI_LOADER
	efi_init_early,
#endif
#ifdef CONFIG_CMD_NAND
	initr_nand,
#endif
#ifdef CONFIG_CMD_ONENAND
	initr_onenand,
#endif
#ifdef CONFIG_MMC
	initr_mmc,
#endif
#ifdef CONFIG_XEN
	xen_init,
#endif
#ifdef CONFIG_PVBLOCK
	initr_pvblock,
#endif
	initr_env,
#ifdef CONFIG_SYS_MALLOC_BOOTPARAMS
	initr_malloc_bootparams,
#endif
	INIT_FUNC_WATCHDOG_RESET
	cpu_secondary_init_r,
#if defined(CONFIG_ID_EEPROM)
	mac_read_from_eeprom,
#endif
	INITCALL_EVENT(EVT_SETTINGS_R),
	INIT_FUNC_WATCHDOG_RESET
#if defined(CONFIG_PCI_INIT_R) && !defined(CONFIG_SYS_EARLY_PCI_INIT)
	/*
	 * Do pci configuration
	 */
	pci_init,
#endif
	stdio_add_devices,
	jumptable_init,
#ifdef CONFIG_API
	api_init,
#endif
	console_init_r,		/* fully init console as a device */
#ifdef CONFIG_DISPLAY_BOARDINFO_LATE
	console_announce_r,
	show_board_info,
#endif
#ifdef CONFIG_ARCH_MISC_INIT
	arch_misc_init,		/* miscellaneous arch-dependent init */
#endif
#ifdef CONFIG_MISC_INIT_R
	misc_init_r,		/* miscellaneous platform-dependent init */
#endif
	INIT_FUNC_WATCHDOG_RESET
#ifdef CONFIG_CMD_KGDB
	kgdb_init,
#endif
	interrupt_init,
#if defined(CONFIG_MICROBLAZE) || defined(CONFIG_M68K)
	timer_init,		/* initialize timer */
#endif
	initr_status_led,
	initr_boot_led_blink,
	/* PPC has a udelay(20) here dating from 2002. Why? */
#ifdef CONFIG_BOARD_LATE_INIT
	board_late_init,
#endif
#ifdef CONFIG_PCI_ENDPOINT
	pci_ep_init,
#endif
#if defined(CONFIG_CMD_NET)
	INIT_FUNC_WATCHDOG_RESET
	initr_net,
#endif
#ifdef CONFIG_POST
	initr_post,
#endif
	INIT_FUNC_WATCHDOG_RESET
	INITCALL_EVENT(EVT_LAST_STAGE_INIT),
#if defined(CFG_PRAM)
	initr_mem,
#endif
	initr_boot_led_on,
	run_main_loop,
};

static void tbr_draw_char(int x, int y, unsigned char c, int scale,
			  unsigned color)
{
	const unsigned char *g;
	int row, col;
	unsigned *fb = (unsigned *)0xD5100000UL;

	g = &video_fontdata_8x16[(int)c * 16];
	for (row = 0; row < 16; row++) {
		unsigned char bits = g[row];
		int s, t;

		for (s = 0; s < scale; s++) {
			unsigned *line = fb +
				(ulong)(y + row * scale + s) * 3200 + x;

			for (t = 0; t < scale; t++)
				line[t] = (bits & (0x80 >> t)) ?
						color : 0xFF000000;
		}
	}
}

static void tbr_draw_hex(int x, int y, ulong v, int nd, int scale,
			 unsigned color)
{
	const char h[] = "0123456789abcdef";
	int i;

	for (i = 0; i < nd; i++)
		tbr_draw_char(x + i * 18, y, h[(v >> ((nd - 1 - i) * 4)) & 0xf],
			      scale, color);
}

/* TB710FU: render gd->reloc_off and the runtime init_sequence_r so the
 * hanging initcall can be identified offline. Big 2x digits, left edge. */
static void tbr_dump_array(init_fnc_t const *arr)
{
	int i;

	tbr_draw_hex(8, 60, gd->reloc_off, 8, 2, 0xFF00FFFF);
	for (i = 0; i < 44 && 60 + (i + 1) * 40 < 1980; i++) {
		tbr_draw_char(8, 60 + i * 40,
			      "0123456789abcdef"[(i >> 4) & 0xf], 2,
			      0xFF00FFFF);
		tbr_draw_char(26, 60 + i * 40,
			      "0123456789abcdef"[i & 0xf], 2, 0xFF00FFFF);
		tbr_draw_hex(52, 60 + i * 40, (ulong)arr[i], 8, 2,
			     0xFFFFFFFF);
	}
}

void board_init_r(gd_t *new_gd, ulong dest_addr)
{

	/*
	 * The pre-relocation drivers may be using memory that has now gone
	 * away. Mark serial as unavailable - this will fall back to the debug
	 * UART if available.
	 *
	 * Do the same with log drivers since the memory may not be available.
	 */
	gd->flags &= ~(GD_FLG_SERIAL_READY | GD_FLG_LOG_READY);

	/*
	 * Set up the new global data pointer. So far only x86 does this
	 * here.
	 * TODO(sjg@chromium.org): Consider doing this for all archs, or
	 * dropping the new_gd parameter.
	 */
	if (CONFIG_IS_ENABLED(X86_64) && !IS_ENABLED(CONFIG_EFI_APP))
		arch_setup_gd(new_gd);

#if !defined(CONFIG_X86) && !defined(CONFIG_ARM) && !defined(CONFIG_ARM64)
	gd = new_gd;
#endif
	gd->flags &= ~GD_FLG_LOG_READY;

	/* TB710FU: reach the loader without full device autoprobe.
	 * Run R-array up to but NOT including initr_dm_devices (index 14), which
	 * calls dm_autoprobe() and hangs probing unrelated drivers. Everything
	 * after this point is brought up explicitly, one step at a time, and
	 * reported on screen as text plus the return code in hex.
	 */
	/* TB710FU: RED = board_init_r was reached, i.e. relocate_code and the
	 * crt0 hand-off both worked. This is the first statement on purpose. */
	{
		unsigned *fb = (unsigned *)0xD5100000UL;
		int r, c;

		for (r = 0; r < 180; r++) {
			unsigned *line = fb + (ulong)(200 + r) * 3200 + 2600;

			for (c = 0; c < 400; c++)
				line[c] = 0xFFFF0000;
		}
	}

	/* TB710FU: run the real init sequence, but only the part before
	 * initr_dm_devices. That entry calls dm_autoprobe(), which hangs probing
	 * unrelated drivers; what we actually need is probed explicitly below.
	 *
	 * initcall_run_list() is used rather than a hand-rolled loop because the
	 * array holds encoded event entries as well as function pointers
	 * (INITCALL_IS_EVENT is a mask over bits 8..63, so testing a function
	 * pointer against it is never false), and because it applies the
	 * relocation offset itself.
	 */
	{
		init_fnc_t seq[64];
		int i, n = 0, rc;

		for (i = 0; i < 63 && init_sequence_r[i]; i++) {
			if (init_sequence_r[i] == (init_fnc_t)initr_dm_devices)
				break;
			/* TB710FU: enable_caches() hangs on this board (the index
			 * marker stopped on it: it is the fourth R entry). U-Boot runs
			 * fine with the MMU and caches exactly as ABL left them - the
			 * relocation itself ran with them off - so this entry is
			 * skipped rather than waited for. */
			if (init_sequence_r[i] == (init_fnc_t)initr_caches)
				continue;
			/* TB710FU: initr_dm ends with dm_autoprobe(), which probes
			 * every driver and hangs on this board. Only the scan is
			 * wanted, and it is done explicitly below. */
			if (init_sequence_r[i] == (init_fnc_t)initr_dm)
				continue;
			seq[n++] = init_sequence_r[i];
		}
		seq[n] = NULL;

		{
			/* TB710FU: show the device tree pointer before and after the
			 * relocation, then force it back to the pre-relocation value.
			 * The DTB lives outside the relocated image, so the offset must
			 * not be applied to it. */
			extern void *tb_fdt_load(void);

			void *saved = tb_fdt_load();

			tb_screen_log("FDT PTR NOW", 3);
			tb_screen_log_hex((ulong)(uintptr_t)gd->fdt_blob, 3);
			tb_screen_log("FDT PTR SAVED", 3);
			tb_screen_log_hex((ulong)(uintptr_t)saved, 3);
			if (saved)
				gd->fdt_blob = saved;
		}
		tb_screen_log_reset();
		extern void board_init_f(ulong);
		tb_screen_log("TB710FU DIAG13", 3);
		tb_screen_log("R ENTRIES BEFORE DM DEV", 3);
		tb_screen_log_hex((ulong)n, 4);

		rc = initcall_run_list(seq);
		tb_screen_log(rc ? "R SEQ FAIL" : "R SEQ OK", 3);
		tb_screen_log_hex((ulong)(unsigned int)rc, 4);
	}


	/* Which device tree is in use, and does it carry the framebuffer node the
	 * video console needs? fdt_src 2 = board hook, 5 = separate (appended). */
	/* TB710FU: the FDT SRC / FDT SIZE / FDT BLOB / FB NODE FOUND lines that
	 * used to sit here are gone. They read the device tree after relocation,
	 * where gd->fdt_blob points at the copy reserve_fdt() made outside the
	 * relocated image, and the log stops right about there - so they are a
	 * suspect, and they are only diagnostics anyway. Storage is what counts.
	 */
	/* TB710FU: the DM scan without the autoprobe that initr_dm would run.
	 * This binds devices from the device tree; probing is left to the explicit
	 * calls below, so only the UFS controller is touched. The prologue mirrors
	 * initr_dm so the driver model starts from a clean state. */
	tb_screen_log("DM INIT AND SCAN", 3);
	{
		int rc;

		oftree_reset();
		gd->dm_root = NULL;
#ifdef CONFIG_TIMER
		gd->timer = NULL;
#endif
		/* TB710FU: dm_init_and_scan() itself hangs, and both of its halves
		 * take the live-tree path when CONFIG_OF_LIVE is on. That is now
		 * off, and the two halves are called separately with a log line
		 * before each and the return code after, so a hang names itself. */
		rc = dm_init(false);
		if (rc)
			tb_logv("DM INIT", (ulong)(unsigned int)rc);

		/* dm_scan() is internal; dm_scan_fdt() plus dm_scan_other() is what
		 * it does, and both are public. */
		rc = dm_scan_fdt(false);
		if (rc)
			tb_logv("DM SCAN FDT", (ulong)(unsigned int)rc);

		rc = dm_scan_other(false);
		if (rc)
			tb_logv("DM SCAN OTHER", (ulong)(unsigned int)rc);

		/* The autoprobe we no longer run used to provide the timer; U-Boot's
		 * delays and timeouts need one, so probe it explicitly. This build
		 * has no timer driver, so -EOPNOTSUPP is expected and not logged. */
		{
			struct udevice *tv = NULL;

			rc = uclass_first_device_err(UCLASS_TIMER, &tv);
			if (rc && rc != -EOPNOTSUPP)
				tb_logv("TIMER", (ulong)(unsigned int)rc);
		}
	}

	{
		struct udevice *uv = NULL;
		int rc = uclass_first_device_err(UCLASS_UFS, &uv);

		tb_logv("UFS", (ulong)(unsigned int)rc);
	}

	{
		struct udevice *sv;
		int rc, n = 0;

		rc = scsi_scan(true);
		tb_logv("SCSI", (ulong)(unsigned int)rc);

		for (uclass_first_device(UCLASS_SCSI, &sv); sv;
		     uclass_next_device(&sv))
			n++;
		tb_logv("SCSI CNT", (ulong)n);
	}

	/* TB710FU: boot the mainline kernel from UFS.
	 *
	 * ABL normally loads the kernel at 0xA8000000 and hands it a device tree
	 * whose /memory node it has already patched with the SMEM usable-RAM
	 * table. Bypassing ABL means doing both here, and both matter:
	 *
	 *  - the image has to land inside the kernel's own linear-map window (the
	 *    tree in recovery_a carries the zero-size placeholder XBL overwrites,
	 *    and 0x91000000 - where earlier builds loaded it - is below
	 *    memstart_addr);
	 *  - /memory has to list every bank XBL calls usable, not one region: they
	 *    are small fragments around its carveouts (the first is 3 MiB), so a
	 *    single or truncated entry leaves the kernel with almost no RAM.
	 *
	 * printk's ring buffer lives in the kernel's .bss and a warm reset leaves
	 * DRAM intact, so the previous boot's log is still at the load address.
	 * Look for the messages a kernel prints as it dies: a wrapped ring has
	 * long overwritten the first line.
	 */
	{
		void *initfdt = (void *)(ulong)get_prev_bl_fdt_addr();
		const void *memreg = NULL;
		int memlen = 0;
		ulong kload = 0xA8000000UL;	/* where ABL itself puts it */
		/* The kernel decompresses to 0xA8000000 + 0x2925A00 = 0xAB925A00,
		 * so the old kload+0x3000000 / kload+0x3800000 sat inside the
		 * kernel image: the gunzip overwrote the device tree before it
		 * was patched, fdt_open_into() failed, and the jump never ran
		 * (the panel showed the magic fine, then BOOT ABORTED).  Put the
		 * buffers past the kernel, below the initramfs at 0xAD000000 and
		 * clear of ramoops at 0xAC300000. */
		ulong rawdtb = 0xAC000000UL;
		ulong kfdt = 0xAC100000UL;
		ulong srclen = 0;
		ulong ir_start = 0, ir_len = 0;	/* initramfs, from /chosen */
		phys_addr_t rb_start[TB_RAM_BANKS];
		phys_size_t rb_size[TB_RAM_BANKS];
		u64 mreg[TB_RAM_REGS];
		u64 rtot = 0;
		int nb = 0, i;
		int rc;

		extern void tb_screen_clear(void);
		extern void tb_kmark_jump(void);

		/* Before anything else, leave a marker the *next* boot can
		 * read: if ABL is rejecting slot B without running it, nothing
		 * we do inside here is observable at all, and this is the only
		 * way to tell that case apart from an early death. */
		tb_logv("BCB SET", (ulong)(unsigned int)tb_misc_bcb(1));

		tb_screen_clear();
		/* After the clear, not before: tb_screen_clear() wipes rows
		 * 1100..1989 and this bar is drawn at row 1420, so painting
		 * first meant painting into the region about to be erased. */
		tb_mark(5);
		tb_logv("UB EL", (ulong)current_el());
		tb_logv("UB SCTLR", get_sctlr());

		/* What the previous boot left, read HERE: every address below the
		 * gunzip's output length (0x2915a00) is about to be overwritten
		 * with the file's own bytes, which is precisely what the old
		 * ladder was reading. boot_args is the one that matters - the
		 * kernel stores x0..x3 there in its second instruction, so it is
		 * x0 at entry, measured by the kernel itself. */
		tb_logv2("PREV BA", tb_ram_rd64(kload + TB_K_BOOT_ARGS),
			 tb_ram_rd64(kload + TB_K_BOOT_ARGS + 8));
		tb_logv("PREV VOFF", tb_ram_rd64(kload + TB_K_KIMAGE_VOFFSET));
		tb_logv2("PREV BSS", tb_ram_rd64(kload + TB_K_BSS_START),
			 tb_ram_rd64(kload + TB_K_LOG_BUF));

		/* 0xaa915a40 is in the padding gap, so what it holds is what
		 * the previous boot stored there and nothing else. */
		tb_logv("C1", tb_ram_rd64(TB_PRB_GAP));
		tb_logv("C2", tb_ram_rd64(TB_PRB_POST));
		tb_logv("C3", tb_ram_rd64(TB_PRB_OLD));

		{
			u64 h = tb_ram_rd64(kload + TB_LOG_RB_OFF +
						TB_LOG_RB_HEAD);
			u32 bits = (u32)tb_ram_rd64(kload + TB_LOG_RB_OFF +
						TB_LOG_RB_SIZEBITS);

			tb_logv2("PREV HLPS", (ulong)(u32)h, (ulong)(u32)(h >> 32));

			/* The text ring sits above the gunzip's output, so nothing
			 * this boot has written can have reached it yet. */
			if (h != TB_LOG_LPOS_INIT && bits >= 12 && bits <= 24) {
				ulong size = 1UL << bits;

				tb_ram_text_ring(kload + TB_K_LOG_BUF, size,
						 ((ulong)h + size - 400) & (size - 1),
						 400);
				tb_probe_dump(2);
			}
		}

		/* The tree ABL handed over is the one XBL patched; U-Boot's own
		 * embedded tree only has the placeholder. */
		if (initfdt && fdt_check_header(initfdt))
			initfdt = NULL;
		if (initfdt) {
			int off = tb_memory_node(initfdt);

			if (off >= 0)
				memreg = fdt_getprop(initfdt, off, "reg", &memlen);
		}
		if (!memreg || memlen < 16)
			memlen = 0;

		if (memlen) {
			for (i = 0; i < memlen / 16 && i < TB_RAM_BANKS; i++) {
				u64 a = fdt64_to_cpu(tb_ram_rd64((ulong)memreg +
								 16 * i));
				u64 sz = fdt64_to_cpu(tb_ram_rd64((ulong)memreg +
								 16 * i + 8));

				if (!sz)
					continue;
				rb_start[nb] = a;
				rb_size[nb] = sz;
				rtot += sz;
				nb++;
			}
		}
		/* A node that adds up to no RAM is the placeholder, not a map. */
		if (nb < 1 || rtot < 0x10000000) {
			nb = qcom_get_ram_banks(rb_start, rb_size, TB_RAM_BANKS);
			rtot = 0;
			for (i = 0; i < nb; i++)
				rtot += rb_size[i];
		}
		tb_logv("RAM TOT", (ulong)rtot);

		{
			struct blk_desc *kdesc = NULL;
			struct disk_partition kinfo;
			int di, brc;

			/* part_get_info_by_name() returns -ENOENT even though the
			 * name shows up in the listing, so compare names directly
			 * with the same call the listing uses. */
			rc = -ENOENT;
			for (di = 0; di < 128 && rc; di++) {
				struct blk_desc *d = blk_get_dev("scsi", di);
				struct disk_partition info;
				int pi;

				if (!d)
					continue;
				part_init(d);
				for (pi = 1; pi <= 64; pi++) {
					if (part_get_info(d, pi, &info))
						break;
					if (!strcmp((const char *)info.name,
						    "linboot")) {
						kdesc = d;
						kinfo = info;
						rc = 0;
						break;
					}
				}
			}

			/* 16 MiB covers the 15 MiB compressed kernel, and it is kept
			 * below /memory's start, which the kernel never looks at. */
			if (!rc) {
				/* 4096-byte blocks: 0x1000 blocks = the 16 MiB that
				 * covers the compressed kernel. */
				brc = blk_dread(kdesc, kinfo.start, 0x1000,
						(void *)0x90000000UL);
				/* blk_dread() returns the block count, not 0. */
				tb_logv("KERNEL READ", (ulong)(unsigned int)brc);
				if (brc != 0x1000)
					rc = -EIO;
			}

			/* TB710FU: the initramfs sits right after the 16 MiB the
			 * kernel was read from. Without it the kernel reaches user
			 * space with nothing to exec and panics: Attempted to kill
			 * init. */
			if (!rc) {
				/* +0x1000 blocks = the 16 MiB the kernel was
				 * read from; 0x800 blocks = 2 MiB is plenty for
				 * the ramdisk. */
				brc = blk_dread(kdesc, kinfo.start + 0x1000, 0x800,
							(void *)0xad000000UL);
				tb_logv("INITRD READ", (ulong)(unsigned int)brc);
				if (brc != 0x800)
					rc = -EIO;
				else {
					tb_logv("INITRD MAGIC",
						(ulong)(unsigned int)*(volatile u32 *)TB_IR_BASE);
					tb_initrd_get(&ir_start, &ir_len);
				}
			}

			if (!rc) {
				srclen = 0x1000000;
				rc = gunzip((void *)kload, 0x4000000,
					    (void *)0x90000000UL, &srclen);
				tb_logv("GUNZIP", (ulong)(unsigned int)rc);
				tb_logv("SRC USED", srclen);

				/* The oracle: a FNV-1a-32 over the first megabyte
				 * of what the gunzip produced, against the value
				 * computed offline from the file that was flashed.
				 * A megabyte of RAM cannot hash to the right number
				 * by accident, so this settles whether the physical
				 * read path is exact - independently of any record
				 * having to survive a reset. */
				if (!rc) {
					u64 k8 = tb_ram_rd64(kload);
					u32 got;

					/* First word is efi_signature_nop
					 * (0xfa405a4d), then the branch to
					 * primary_entry. */
					tb_logv2("K8", (ulong)(u32)k8,
						 (ulong)(u32)(k8 >> 32));
					got = tb_fnv1a32(kload, TB_ORACLE_LEN);
					tb_logv2("ORCL", (ulong)got, TB_ORACLE_FNV);
				}
			}
		}

		if (!rc) {
			struct blk_desc *ddesc = NULL;
			struct disk_partition dinfo;
			int di, brc;

			rc = -ENOENT;
			for (di = 0; di < 128 && rc; di++) {
				struct blk_desc *d = blk_get_dev("scsi", di);
				struct disk_partition info;
				int pi;

				if (!d)
					continue;
				part_init(d);
				for (pi = 1; pi <= 64; pi++) {
					if (part_get_info(d, pi, &info))
						break;
					if (!strcmp((const char *)info.name,
						    "linboot")) {
						ddesc = d;
						dinfo = info;
						rc = 0;
						break;
					}
				}
			}

			if (!rc) {
				brc = /* TB710FU: DTB lives at linboot+0x1800 blocks (24 MiB);
				 * window 0x28 blocks = 160 KiB (thermal tree 146 KB).
				 */
				brc = blk_dread(ddesc, dinfo.start + 0x1800, 0x28,
						(void *)rawdtb);
				if (brc != 0x28)
					rc = -EIO;
			}

			/* The loaded tree is packed solid, so fdt_add_subnode()
			 * fails with -NOSPACE; patch a copy with a megabyte of
			 * slack and hand that to the kernel. */
			if (!rc) {
				rc = fdt_open_into((void *)rawdtb, (void *)kfdt,
						   0x100000);
				if (!rc)
					tb_kfdt = (void *)kfdt;
			}
		}

		if (!rc) {
			void *fdt = (void *)kfdt;
			int off;

			/* /memory: patch the node in place, all banks at once; the
			 * kernel takes any depth-1 node with device_type "memory". */
			off = tb_memory_node(fdt);
			if (off < 0)
				off = fdt_add_subnode(fdt, 0, "memory@a0000000");
			if (off < 0)
				rc = off;
			else {
				for (i = 0; i < nb; i++) {
					mreg[2 * i] = cpu_to_fdt64(rb_start[i]);
					mreg[2 * i + 1] = cpu_to_fdt64(rb_size[i]);
				}
				fdt_setprop_string(fdt, off, "device_type",
						   "memory");
				rc = fdt_setprop(fdt, off, "reg", mreg,
						 nb * 16);
				tb_logv("MEM SET", (ulong)(unsigned int)rc);
			}
		}

		if (!rc) {
			void *fdt = (void *)kfdt;
			int off = fdt_path_offset(fdt, "/chosen");

			if (off < 0)
				off = fdt_add_subnode(fdt, 0, "chosen");
			if (off >= 0) {
				/* No UART on this board, so stdout-path would
				 * only add a console nobody can read. */
				fdt_delprop(fdt, off, "stdout-path");
				if (ir_start) {
					u64 iv[2];

					iv[0] = cpu_to_fdt64(ir_start);
					iv[1] = cpu_to_fdt64(ir_start + ir_len);
					fdt_setprop(fdt, off, "linux,initrd-start",
							    &iv[0], 8);
					fdt_setprop(fdt, off, "linux,initrd-end",
							    &iv[1], 8);
				}
				tb_logv("INITRD LEN", ir_len);
				fdt_setprop_string(fdt, off, "bootargs",
					"console=tty0 loglevel=8 "
					"ignore_loglevel nokaslr "
					"clk_ignore_unused pd_ignore_unused "
					"console=tbfb");
			}

			/* Console on the panel ABL left scanning out, plus the
			 * reservation that keeps the page allocator off it: the
			 * splash buffer sits inside the RAM XBL reports. */
			/* TB710FU: deliberately no simple-framebuffer node.  The
			 * kernel's panel console writes to 0xD5100000 itself and
			 * needs none, while this node killed the boot at ~3 s:
			 * the log stopped right after
			 *   simple-framebuffer 1045.framebuffer: framebuffer at
			 *   0x1045, 0x9001 bytes
			 * i.e. with a nonsense address -- the same signature as the
			 * 3.019 s crash that was open since 追加 28. */
			tb_screen_log("NO FB NODE", 2);

			/* No UART is wired up on this board, so a serial console
			 * can only hang the boot writing into a block whose clocks
			 * nobody enabled; the panel is the console. */
			{
				int u = -1;

				while ((u = fdt_node_offset_by_compatible(fdt, u,
						"qcom,geni-se-uart")) >= 0)
					fdt_setprop_string(fdt, u, "status",
							   "disabled");
				u = -1;
				while ((u = fdt_node_offset_by_compatible(fdt, u,
						"qcom,geni-uart")) >= 0) {
					/*
					 * TB710FU: uart14 carries the WCN7850 bluetooth node.
					 * The geni serial driver enables its clocks at probe,
					 * so leave the node enabled for the BT stack instead
					 * of blanket-disabling every geni uart.
					 */
					if (fdt_subnode_offset(fdt, u, "bluetooth") >= 0)
						continue;
					fdt_setprop_string(fdt, u, "status",
							   "disabled");
				}
			}

			off = fdt_path_offset(fdt, "/reserved-memory");
			if (off < 0) {
				off = fdt_add_subnode(fdt, 0, "reserved-memory");
				if (off >= 0) {
					fdt_setprop_u32(fdt, off,
							"#address-cells", 2);
					fdt_setprop_u32(fdt, off,
							"#size-cells", 2);
					fdt_setprop_empty(fdt, off, "ranges");
				}
			}
			if (off >= 0) {
				/* TB710FU: the cells go into the tree verbatim and are big-endian,
				 * so they must be swapped here.  Without cpu_to_fdt32() they
				 * read back as 0x10d5/0x9001: the kernel then reserved 36 KB
				 * in the wrong place instead of this 25 MiB, the allocator
				 * handed the pages out, and the kernel console overwrote
				 * them (see docs/KNOWN-ISSUES.md §1). */
				u32 reg[4] = { 0, cpu_to_fdt32(0xd5100000),
					       0, cpu_to_fdt32(0x1900000) };
				int fn = fdt_add_subnode(fdt, off,
							 "framebuffer@d5100000");

				if (fn >= 0) {
					fdt_setprop(fdt, fn, "reg", reg,
						    sizeof(reg));
				}

				/* TB710FU: the black box log rings (tb_bb_pa[] in the kernel's
				 * init/main.c) need the same rule: reserved, so the allocator
				 * keeps off them, and still mapped, because the kernel writes
				 * them through __va().  This sits outside the if above on
				 * purpose: a DTB that already declares the framebuffer node
				 * makes fdt_add_subnode fail, and the rings are still ours. */
				{
					u32 bba_reg[4] = { 0, cpu_to_fdt32(0xb0000000),
							   0, cpu_to_fdt32(0x100000) };
					u32 bbb_reg[4] = { 0, cpu_to_fdt32(0xd6a00000),
							   0, cpu_to_fdt32(0x100000) };
					int bba = fdt_add_subnode(fdt, off, "tb-blackbox-a@b0000000");
					int bbb = fdt_add_subnode(fdt, off, "tb-blackbox-b@d6a00000");

					if (bba >= 0)
						fdt_setprop(fdt, bba, "reg", bba_reg, sizeof(bba_reg));
					if (bbb >= 0)
						fdt_setprop(fdt, bbb, "reg", bbb_reg, sizeof(bbb_reg));
				}
			}

			/* Not fatal: a tree without a console description still
			 * boots, and /memory is already in. */
			rc = 0;
		}

		if (!rc) {
			ulong img = tb_ram_rd64(kload + 0x10);

			tb_logv("IMAGE SIZE", img);
			/* .bss must be zero when the kernel starts: it only
			 * clears it in early_map_kernel(), after primary_entry
			 * has already run, and create_init_idmap() reads a .data
			 * variable before that. Filling it with a canary - as
			 * earlier builds did - hands the early page-table code
			 * garbage and kills the kernel before it can print or
			 * even install its vectors. Zero it from the end of the
			 * image data to _end. Only that range: a bogus header
			 * field would otherwise scribble over all of RAM. */
			if (img > srclen && img < 0x4000000)
				tb_ram_fill(kload + srclen, kload + img, 0);
			tb_logv("BSS ZEROED", img > srclen ? img - srclen : 0);

			tb_ram_wr64(TB_PRB_GAP, 0xc0de0001UL);
			tb_ram_wr64(TB_PRB_POST, 0xc0de0002UL);
			tb_ram_wr64(TB_PRB_OLD, 0xc0de0003UL);
			tb_logv("RT1", tb_ram_rd64(TB_PRB_GAP));
			tb_logv("RT2", tb_ram_rd64(TB_PRB_POST));
			tb_logv("RT3", tb_ram_rd64(TB_PRB_OLD));

#if TB_DIAG_RAMOOPS
			{
				extern void tb_ramoops_dump(int scale);
				extern void tb_screen_log_reset(void);

				/* The whole panel goes to the previous boot: this
				 * build exists to be photographed, not to boot. */
				tb_screen_clear();
				tb_screen_log_reset();
				tb_screen_log("RAMOOPS DIAG", 2);
				tb_ramoops_dump(2);
				tb_wdt_show();
				tb_screen_log("PET WDT", 2);
				/* The screen has to outlive the bootloader's
				 * watchdog, or a photo of it is a race. */
				tb_wdt_pet_forever();
			}
#endif
			/* Reached the end: drop the marker, so a normal next
			 * boot now means this routine ran to completion. */

			tb_mark(1);

			tb_kmark_jump();
			tb_screen_log("JUMP", 3);

			/* U-Boot runs with whatever cache state the bootloader
			 * left; stale instruction lines over the payload would
			 * run old code. */
			icache_disable();
			invalidate_icache_all();

			/* The hand-off bootm makes, without the command
			 * interpreter, the environment or the image machinery,
			 * none of which this board's bring-up sets up.
			 *
			 * armv8_switch_to_el2() must not be used here: U-Boot runs
			 * at EL1 on this board and that helper *returns* at EL1
			 * instead of branching, so the kernel was never entered --
			 * the panel showed JUMP followed by BOOT ABORTED on every
			 * attempt.  The arm64 boot protocol is x0 = dtb, x1..x3 = 0,
			 * pc = entry, so call the entry directly. */
			cleanup_before_linux();
			tb_logv("ENTRY", (ulong)kload);
			tb_logv("DTB", (ulong)kfdt);
			tb_screen_log("GO", 3);
			{
				void (*kentry)(void *, void *, void *, void *) =
					(void *)kload;

				kentry((void *)kfdt, NULL, NULL, NULL);
			}
		}
	}

	tb_screen_log("BOOT ABORTED", 3);
	tb_screen_log_hex((ulong)(unsigned int)gd->flags, 3);

	/* Whatever failed above, go into the kernel anyway.  The DT patches in
	 * this routine are for the old in-tree kernel; ours compiles its own
	 * console cmdline in, embeds its own initramfs, and takes /memory from
	 * whatever ABL put in the DT it handed us -- which is exactly what the
	 * direct-ABL path boots with.  The patches also cannot succeed any more:
	 * they write at 0xAB000000/0xAB800000, inside the 43 MB the kernel
	 * decompresses to (0xA8000000 + 0x2925A00 = 0xAB925A00).
	 *
	 * The panel showed JUMP then BOOT ABORTED on every attempt, which is
	 * this fall-through: an interactive prompt with no input device. */
	{
		/* kfdt holds the patched copy when the copy succeeded;
		 * otherwise fall back to the tree ABL handed us. */
		void *dtb = tb_kfdt ? tb_kfdt : (void *)get_prev_bl_fdt_addr();
		void (*kentry)(void *, void *, void *, void *) =
			(void *)0xA8000000UL;

		tb_screen_log("GO", 3);
		tb_logv("ENTRY", (ulong)kentry);
		tb_logv("DTB", (ulong)dtb);
		icache_disable();
		invalidate_icache_all();
		cleanup_before_linux();
		kentry(dtb, NULL, NULL, NULL);
	}

	run_main_loop();
	hang();

	/* NOTREACHED - run_main_loop() does not return */
	hang();
}
