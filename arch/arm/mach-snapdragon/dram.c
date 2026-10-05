// SPDX-License-Identifier: GPL-2.0+
/*
 * Memory layout parsing for Qualcomm.
 */

#define LOG_CATEGORY LOGC_BOARD
#define pr_fmt(fmt) "QCOM-DRAM: " fmt

#include <asm-generic/unaligned.h>
#include <dm.h>
#include <log.h>
#include <sort.h>
#include <soc/qcom/smem.h>

#define SMEM_USABLE_RAM_PARTITION_TABLE 402
#define RAM_PART_NAME_LENGTH            16
#define RAM_NUM_PART_ENTRIES            32
#define CATEGORY_SDRAM 0x0E
#define TYPE_SYSMEM 0x01

static struct {
	phys_addr_t start;
	phys_size_t size;
} prevbl_ddr_banks[CONFIG_NR_DRAM_BANKS] __section(".data") = { 0 };

struct smem_ram_ptable_hdr {
	u32 magic[2];
	u32 version;
	u32 reserved;
	u32 len;
} __attribute__ ((__packed__));

struct smem_ram_ptn {
	char name[RAM_PART_NAME_LENGTH];
	u64 start;
	u64 size;
	u32 attr;
	u32 category;
	u32 domain;
	u32 type;
	u32 num_partitions;
	u32 reserved[3];
	u32 reserved2[2]; /* The struct grew by 8 bytes at some point */
} __attribute__ ((__packed__));

struct smem_ram_ptable {
	struct smem_ram_ptable_hdr hdr;
	u32 reserved;     /* Added for 8 bytes alignment of header */
	struct smem_ram_ptn parts[RAM_NUM_PART_ENTRIES];
} __attribute__ ((__packed__));

int dram_init(void)
{
	/*
	 * gd->ram_base / ram_size have been setup already
	 * in qcom_parse_memory().
	 */
	return 0;
}

static int ddr_bank_cmp(const void *v1, const void *v2)
{
	const struct {
		phys_addr_t start;
		phys_size_t size;
	} *res1 = v1, *res2 = v2;

	if (!res1->size)
		return 1;
	if (!res2->size)
		return -1;

	return (res1->start >> 24) - (res2->start >> 24);
}

/* This has to be done post-relocation since gd->bd isn't preserved */
static void qcom_configure_bi_dram(void)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		gd->bd->bi_dram[i].start = prevbl_ddr_banks[i].start;
		gd->bd->bi_dram[i].size = prevbl_ddr_banks[i].size;
		debug("Bank[%d]: start = %#011llx, size = %#011llx\n",
		      i, gd->bd->bi_dram[i].start, gd->bd->bi_dram[i].size);
		if (!prevbl_ddr_banks[i].size)
			break;
	}
}

int dram_init_banksize(void)
{
	qcom_configure_bi_dram();

	return 0;
}

/* TB710FU: hand the SMEM usable-RAM table to the kernel boot path. ram_size is
 * a span across these banks, so handing the kernel one region would also claim
 * whatever hole sits between them (XBL keeps its own structures in one). */
int qcom_get_ram_banks(phys_addr_t *start, phys_size_t *size, int max)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS && i < max; i++) {
		start[i] = prevbl_ddr_banks[i].start;
		size[i] = prevbl_ddr_banks[i].size;
		if (!size[i])
			break;
	}
	return i;
}


/* Parse memory map from SMEM, return the number of entries */
static int qcom_parse_memory_smem(phys_addr_t *ram_end)
{
	size_t size;
	int i, j = 0, ret;
	struct smem_ram_ptable *ram_ptable;
	struct smem_ram_ptn *p;

	ret = qcom_smem_init();
	if (ret) {
		debug("Failed to initialize SMEM: %d.\n", ret);
		return ret;
	}

	ram_ptable = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_USABLE_RAM_PARTITION_TABLE, &size);
	if (!ram_ptable) {
		debug("Failed to find SMEM partition.\n");
		return -ENODEV;
	}

	/* Check validy of RAM */
	for (i = 0; i < RAM_NUM_PART_ENTRIES && j < CONFIG_NR_DRAM_BANKS; i++) {
		p = &ram_ptable->parts[i];
		if (p->category != CATEGORY_SDRAM || p->type != TYPE_SYSMEM)
			continue;
		if (!p->size && !p->start)
			break;

		prevbl_ddr_banks[j].start = p->start;
		prevbl_ddr_banks[j].size = p->size;
		*ram_end = max(*ram_end, prevbl_ddr_banks[j].start + prevbl_ddr_banks[j].size);
		j++;
	}

	if (j == CONFIG_NR_DRAM_BANKS)
		log_err("SMEM: More than CONFIG_NR_DRAM_BANKS (%u) entries!", CONFIG_NR_DRAM_BANKS);

	return j;
}

static void qcom_parse_memory_dt(const fdt64_t *memory, int banks, phys_addr_t *ram_end)
{
	int i, j;
	if (banks > CONFIG_NR_DRAM_BANKS)
		log_err("Provided more memory banks than we can handle\n");

	for (i = 0, j = 0; i < banks * 2; i += 2, j++) {
		prevbl_ddr_banks[j].start = get_unaligned_be64(&memory[i]);
		prevbl_ddr_banks[j].size = get_unaligned_be64(&memory[i + 1]);
		if (!prevbl_ddr_banks[j].size) {
			j--;
			continue;
		}
		*ram_end = max(*ram_end, prevbl_ddr_banks[j].start + prevbl_ddr_banks[j].size);
	}
}

/**
 * The generic memory parsing code in U-Boot lacks a few things that we
 * need on Qualcomm:
 *
 * 1. It sets gd->ram_size and gd->ram_base to represent a single memory block
 * 2. setup_dest_addr() later relocates U-Boot to ram_base + ram_size, the end
 *    of that first memory block.
 *
 * This results in all memory beyond U-Boot being unusable in Linux when booting
 * with EFI.
 *
 * Since the ranges in the memory node may be out of order, the only way for us
 * to correctly determine the relocation address for U-Boot is to parse all
 * memory regions and find the highest valid address.
 *
 * We can't use fdtdec_setup_memory_banksize() since it stores the result in
 * gd->bd, which is not yet allocated.
 *
 * @fdt: FDT blob to parse /memory node from
 *
 * Return: 0 on success or -ENODATA if /memory node is missing or incomplete
 */
int qcom_parse_memory(const void *fdt)
{
	int offset;
	const fdt64_t *memory;
	int memsize;
	phys_addr_t ram_end = 0;
	int banks;

	/* If we don't get an FDT then try to parse from SMEM */
	if (!fdt) {
		banks = qcom_parse_memory_smem(&ram_end);
		if (banks < 0)
			return banks;
	}

	offset = fdt_path_offset(fdt, "/memory");
	if (offset < 0)
		return -ENODATA;

	memory = fdt_getprop(fdt, offset, "reg", &memsize);
	if (!memory)
		return -ENODATA;

	banks = min(memsize / (2 * sizeof(u64)), (ulong)CONFIG_NR_DRAM_BANKS);

	if (memsize / sizeof(u64) > CONFIG_NR_DRAM_BANKS * 2)
		log_err("Provided more than the max of %d memory banks\n", CONFIG_NR_DRAM_BANKS);

	qcom_parse_memory_dt(memory, banks, &ram_end);

	if (!banks || !prevbl_ddr_banks[0].size)
		return -ENODATA;

	/* Sort our RAM banks -_- */
	qsort(prevbl_ddr_banks, banks, sizeof(prevbl_ddr_banks[0]), ddr_bank_cmp);

	gd->ram_base = prevbl_ddr_banks[0].start;
	gd->ram_size = ram_end - gd->ram_base;

	return 0;
}
