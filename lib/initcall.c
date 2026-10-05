// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2013 The Chromium OS Authors.
 */

#include <efi.h>
#include <initcall.h>
#include <log.h>
#include <relocate.h>
#include <asm/global_data.h>
#include <video_font_8x16.h>

DECLARE_GLOBAL_DATA_PTR;

static ulong calc_reloc_ofs(void)
{
#ifdef CONFIG_EFI_APP
	return (ulong)image_base;
#endif
	/*
	 * Sandbox is relocated by the OS, so symbols always appear at
	 * the relocated address.
	 */
	if (IS_ENABLED(CONFIG_SANDBOX) || (gd->flags & GD_FLG_RELOC))
		return gd->reloc_off;

	return 0;
}

/**
 * initcall_is_event() - Get the event number for an initcall
 *
 * func: Function pointer to check
 * Return: Event number, if this is an event, else 0
 */
static int initcall_is_event(init_fnc_t func)
{
	ulong val = (ulong)func;

	if ((val & INITCALL_IS_EVENT) == INITCALL_IS_EVENT)
		return val & INITCALL_EVENT_TYPE;

	return 0;
}

/*
 * To enable debugging. add #define DEBUG at the top of the including file.
 *
 * To find a symbol, use grep on u-boot.map
 */
/* TB710FU: the on-screen text log's geometry. The panel ABL leaves scanning
 * out is 3200x2000; the log starts below the progress digits so both can be
 * read in one photo of the screen.
 */
#define TB_LOG_X		8
#define TB_LOG_TOP		420
#define TB_LOG_STRIDE		3200

static int tb_log_y = TB_LOG_TOP;
static ulong tb_log_n;

void tb_screen_log(const char *s, int scale);
void tb_screen_text(int x, int y, const char *s, int scale);
void tb_screen_log_reset(void);
void tb_screen_log_hex(ulong v, int scale);

/*
 * TB710FU bring-up progress display. The ABL splash framebuffer at
 * 0xD5100000 is still scanned out by the display pipeline while U-Boot
 * runs (panel is 3200x2000). Draw the current initcall index there:
 * two big digit bands (stride 3200 and 2000, 2x scaled) plus a
 * stride-independent tick bar on row 0. Also mirrors "<idx>:<func>"
 * into no-map RAM at 0x9b09c000 for later readout.
 */
#define TB_FB_BASE	0xD5100000UL
#define TB_PROG_BASE	0x9b09c000UL
#define TB_COLOR_WHITE	0xFFFFFFFF
#define TB_COLOR_BLACK	0xFF000000

static const unsigned char tb_font[16][16] = {
	{/* 0 */ 0x00, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xd6, 0xd6, 0xc6, 0xc6, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00},
	{/* 1 */ 0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00},
	{/* 2 */ 0x00, 0x00, 0x7c, 0xc6, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00},
	{/* 3 */ 0x00, 0x00, 0x7c, 0xc6, 0x06, 0x06, 0x3c, 0x06, 0x06, 0x06, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* 4 */ 0x00, 0x00, 0x0c, 0x1c, 0x3c, 0x6c, 0xcc, 0xfe, 0x0c, 0x0c, 0x0c, 0x1e, 0x00, 0x00, 0x00, 0x00},
	{/* 5 */ 0x00, 0x00, 0xfe, 0xc0, 0xc0, 0xc0, 0xfc, 0x06, 0x06, 0x06, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* 6 */ 0x00, 0x00, 0x38, 0x60, 0xc0, 0xc0, 0xfc, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* 7 */ 0x00, 0x00, 0xfe, 0xc6, 0x06, 0x06, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00},
	{/* 8 */ 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* 9 */ 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x06, 0x06, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00},
	{/* a */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00},
	{/* b */ 0x00, 0x00, 0xe0, 0x60, 0x60, 0x78, 0x6c, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* c */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc0, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* d */ 0x00, 0x00, 0x1c, 0x0c, 0x0c, 0x3c, 0x6c, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00},
	{/* e */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00},
	{/* f */ 0x00, 0x00, 0x1c, 0x36, 0x32, 0x30, 0x78, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, 0x00, 0x00, 0x00},
};

static void tb_put_hex(char **p, ulong v, int nd)
{
	const char h[] = "0123456789abcdef";
	int i;

	for (i = nd - 1; i >= 0; i--)
		*(*p)++ = h[(v >> (i * 4)) & 0xf];
}

static void tb_fb_char_at(int x, int y, char c, int stride, int scale)
{
	int gi = -1, row, col;
	unsigned *fb = (unsigned *)TB_FB_BASE;

	if (c >= '0' && c <= '9')
		gi = c - '0';
	else if (c >= 'a' && c <= 'f')
		gi = c - 'a' + 10;
	if (gi < 0)
		return;

	for (row = 0; row < 16; row++) {
		unsigned char bits = tb_font[gi][row];

		for (col = 0; col < 8; col++) {
			unsigned v = (bits & (0x80 >> col)) ?
					TB_COLOR_WHITE : TB_COLOR_BLACK;
			unsigned *line = fb + (ulong)(y + row * scale) * stride
						 + x + col * scale;
			int s, t;

			for (s = 0; s < scale; s++)
				for (t = 0; t < scale; t++)
					line[(ulong)s * stride + t] = v;
		}
	}
}

static void tb_fb_clear_at(int x, int y, int w, int h, int stride, int scale)
{
	unsigned *fb = (unsigned *)TB_FB_BASE;
	int row, col;

	for (row = 0; row < h * scale; row++) {
		unsigned *line = fb + (ulong)(y + row) * stride + x;

		for (col = 0; col < w * scale; col++)
			line[col] = TB_COLOR_BLACK;
	}
}

static void tb_progress(init_fnc_t func)
{
	static char *rp = (char *)TB_PROG_BASE;
	static ulong idx;
	char buf[24];
	char *q = buf;
	int i;

	tb_put_hex(&q, idx, 2);
	*q++ = ':';
	tb_put_hex(&q, (ulong)func, 8);
	*q++ = '\n';
	for (i = 0; i < (q - buf); i++)
		*rp++ = buf[i];
	*rp = 0;

	/* TB710FU: the on-screen marker was removed. It redrew a 714x144 pixel
	 * "index:address" string for every F-sequence entry - about 27 MiB of
	 * strongly ordered framebuffer stores before relocation - and that alone
	 * was enough to run the boot past the watchdog window, which is what made
	 * the relocation look flaky. The RAM marker above still records the
	 * sequence, and lib/initcall.c's text log is the channel used for reading
	 * state off the screen.
	 */
	idx++;
}

int initcall_run_list(const init_fnc_t init_sequence[])
{
	ulong reloc_ofs;
	const init_fnc_t *ptr;
	enum event_t type;
	init_fnc_t func;
	int ret;

	for (ptr = init_sequence; func = *ptr, func; ptr++) {
		reloc_ofs = calc_reloc_ofs();
		type = initcall_is_event(func);

		if (type) {
			if (!CONFIG_IS_ENABLED(EVENT))
				continue;
			debug("initcall: event %d/%s\n", type,
			      event_type_name(type));
		} else if (reloc_ofs) {
			debug("initcall: %p (relocated to %p)\n",
			      (char *)func - reloc_ofs, (char *)func);
		} else {
			debug("initcall: %p\n", (char *)func - reloc_ofs);
		}

		tb_progress(func);
		tb_progress(func);
		tb_progress(func);
		ret = type ? event_notify_null(type) : func();
		if (ret)
			break;
	}

	if (ret) {
		if (CONFIG_IS_ENABLED(EVENT)) {
			char buf[60];

			/* don't worry about buf size as we are dying here */
			if (type) {
				sprintf(buf, "event %d/%s", type,
					event_type_name(type));
			} else {
				sprintf(buf, "call %p",
					(char *)func - reloc_ofs);
			}

			printf("initcall failed at %s (err=%dE)\n", buf, ret);
		} else {
			printf("initcall failed at call %p (err=%d)\n",
			       (char *)func - reloc_ofs, ret);
		}

		return ret;
	}

	return 0;
}

/*
 * TB710FU: one coloured block per call site, laid out in a row low on the
 * screen (the assembly paths have no console and no driver model, so this is a
 * raw framebuffer write and nothing else). Colours in index order:
 *   0 green  1 yellow  2 blue  3 white  4 cyan  5 magenta  6 orange  7 red
 */
/* ---------------------------------------------------------------------------
 * TB710FU: one line per fact.
 *
 * The panel only holds about twenty lines at this pitch, so a label on one line
 * and its value on the next pushes some other fact off the top. Everything the
 * boot path reports goes through here instead.
 * ------------------------------------------------------------------------- */
void tb_logv(const char *label, ulong v)
{
	char buf[64];
	const char *hex = "0123456789abcdef";
	int i = 0, j;

	while (label[i] && i < 40) {
		buf[i] = label[i];
		i++;
	}
	buf[i++] = ' ';
	for (j = 0; j < 8; j++)
		buf[i++] = hex[(v >> (28 - 4 * j)) & 0xf];
	buf[i] = 0;
	tb_screen_log(buf, 3);
}

/* Same as tb_logv(), but with two values on the line: the screen holds
 * about 28 lines at this pitch and the hand-off needs twelve readings. */
void tb_logv2(const char *label, ulong a, ulong b)
{
	char buf[80];
	const char *hex = "0123456789abcdef";
	int i = 0, j;

	while (label[i] && i < 32) {
		buf[i] = label[i];
		i++;
	}
	buf[i++] = ' ';
	for (j = 0; j < 8; j++)
		buf[i++] = hex[(a >> (28 - 4 * j)) & 0xf];
	buf[i++] = ' ';
	for (j = 0; j < 8; j++)
		buf[i++] = hex[(b >> (28 - 4 * j)) & 0xf];
	buf[i] = 0;
	tb_screen_log(buf, 3);
}

/* ---------------------------------------------------------------------------
 * TB710FU: post-mortem RAM probe.
 *
 * A kernel that dies before fbcon still leaves its last words behind: printk's
 * ring buffer lives in .bss, and a warm reset does not clear DRAM. On the next
 * boot that window is untouched (the load stops at the end of the image data,
 * which is where .bss begins), so it can be scanned and shown. With no UART
 * this is the only channel that can report a panic before the display is up.
 * ------------------------------------------------------------------------- */
u64 tb_ram_rd64(ulong addr)
{
	u64 v = 0;
	int i;

	/* Byte at a time: this runs with the MMU off, where a wide access to an
	 * unaligned address faults instead of being handled by the hardware. */
	for (i = 0; i < 8; i++)
		v |= (u64)*(volatile unsigned char *)(addr + i) << (8 * i);
	return v;
}

/* Number of non-zero doublewords in [start, end): distinguishes a window the
 * kernel has written to (page tables, log ring) from untouched RAM. */
ulong tb_ram_nonzero(ulong start, ulong end)
{
	ulong a, n = 0;

	for (a = start; a + 8 <= end; a += 8) {
		if (tb_ram_rd64(a))
			n++;
	}
	return n;
}

/* Count doublewords equal to @val and equal to zero in [start, end). */
void tb_ram_count(ulong start, ulong end, u64 val, ulong *hits, ulong *zeros)
{
	ulong a, n = 0, z = 0;

	for (a = start; a + 8 <= end; a += 8) {
		u64 v = tb_ram_rd64(a);

		if (v == val)
			n++;
		else if (!v)
			z++;
	}
	if (hits)
		*hits = n;
	if (zeros)
		*zeros = z;
}

/* Fill [start, end) with @val. Used to leave a canary in the kernel's .bss. */
void tb_ram_fill(ulong start, ulong end, u64 val)
{
	ulong a;

	start = (start + 7) & ~7UL;
	for (a = start; a + 8 <= end; a += 8)
		*(volatile u64 *)a = val;
}

/* First occurrence of @needle in [start, end), or 0. Stepping in words keeps
 * this tolerable over the tens of megabytes the kernel image spans. */
ulong tb_ram_find(const char *needle, ulong start, ulong end)
{
	int n = 0, i;
	ulong a;

	while (needle[n])
		n++;
	if (!n)
		return 0;
	for (a = start; a + n <= end; a += 8) {
		const char *p = (const char *)a;

		for (i = 0; i < n; i++) {
			if (p[i] != needle[i])
				break;
		}
		if (i == n)
			return a;
	}
	return 0;
}

/* The newest printk record in [start, end): every record begins with an 8-byte
 * nanosecond timestamp, so the largest plausible one is the tail of the log.
 * Once the ring has wrapped, the first line is long gone, so this is what
 * finds whatever the kernel was saying when it stopped - no keyword needed. */
ulong tb_ram_latest_log(ulong start, ulong end, ulong *ts)
{
	ulong a, best = 0;
	u64 bestv = 0;

	for (a = start; a + 8 <= end; a += 8) {
		u64 v = tb_ram_rd64(a);

		if (v > 100000000ULL && v < 100000000000ULL && v > bestv) {
			bestv = v;
			best = a;
		}
	}
	if (ts)
		*ts = (ulong)bestv;
	return best;
}

static char tb_probe_txt[6144];
static int tb_probe_n;

/* Copy text out of RAM immediately after a hit: loading the kernel over the
 * region would destroy what is being read. The ring is packed records rather
 * than NUL-terminated strings, so stop at a run of unprintable bytes. */
void tb_ram_text(ulong addr, int max)
{
	int n = 0, run = 0;

	if (max > (int)sizeof(tb_probe_txt) - 1)
		max = sizeof(tb_probe_txt) - 1;
	while (n < max) {
		char c = *(volatile char *)addr++;

		if (c == '\n')
			run = 0;
		else if (c < 32 || c > 126) {
			if (++run > 8)
				break;
			c = ' ';
		} else
			run = 0;
		tb_probe_txt[n++] = c;
	}
	tb_probe_txt[n] = 0;
	tb_probe_n = n;
}

/* Copy @len bytes of the circular buffer at @base (size @size, starting at ring
 * offset @start) into the probe buffer, wrapping once. printk's data ring holds
 * a record header between text blocks, so a run of unprintable bytes means "the
 * next record", not "the end" - keep going through it. */
void tb_ram_text_ring(ulong base, ulong size, ulong start, int len)
{
	int n = 0, run = 0;
	ulong i = start;

	if (len > (int)sizeof(tb_probe_txt) - 1)
		len = sizeof(tb_probe_txt) - 1;
	while (n < len) {
		char c = *(volatile char *)(base + (i & (size - 1)));

		i++;
		if (c == '\n')
			run = 0;
		else if (c < 32 || c > 126) {
			if (++run > 64)
				break;
			c = ' ';
		} else
			run = 0;
		tb_probe_txt[n++] = c;
	}
	tb_probe_txt[n] = 0;
	tb_probe_n = n;
}

void tb_probe_dump(int scale)
{
	int i = 0;
	int per;

	if (!tb_probe_n)
		return;
	if (scale < 1)
		scale = 1;
	per = (TB_LOG_STRIDE - TB_LOG_X) / (8 * scale);
	while (i < tb_probe_n) {
		char line[240];
		int n = 0;

		while (n < per - 1 && i < tb_probe_n && tb_probe_txt[i] != '\n')
			line[n++] = tb_probe_txt[i++];
		if (i < tb_probe_n && tb_probe_txt[i] == '\n')
			i++;
		line[n] = 0;
		tb_screen_log(line, scale);
	}
}

/* ---------------------------------------------------------------------------
 * Ramoops: the previous boot's console text, read out of the one place the
 * firmware keeps across a reset.
 *
 * The DT reserves 0xAC300000 + 1 MiB for ramoops. It sits above this boot's
 * gunzip output (0xA8000000 + srclen 0x2915A00 = 0xAB15A00), so nothing done
 * here can have touched it, which is why it is worth a screen of its own.
 *
 * Each zone opens with struct persistent_ram_buffer { u32 sig; u32 start;
 * u32 size; u8 data[]; }. The text is stored contiguously - parity lives in
 * its own area after the data, not interleaved - and persistent_ram keeps
 * start + size <= buffer_size at all times, so the live range never wraps and
 * one linear copy is enough. The console and pmsg zones are stamped with
 * PERSISTENT_RAM_SIG; the dump zones are not, and the console zone is the one
 * that carries a boot that died, because CONFIG_PSTORE_CONSOLE writes every
 * console line there, panic output included.
 */
#define TB_RAMOOPS_BASE	0xAC300000UL
#define TB_RAMOOPS_SIZE	0x100000UL
#define TB_PR_SIG	0x43474244UL	/* PERSISTENT_RAM_SIG: "DBGC" */

/* The dump zones sit at the start of the region and are not stamped with
 * PERSISTENT_RAM_SIG, so a signature search can miss the very report worth
 * having. Search for the text instead: every Oops this kernel can produce
 * carries the same banner. */
static int tb_ramoops_seek(const char *needle)
{
	ulong n = 0, off, k;

	while (needle[n])
		n++;
	for (off = 0; off + n < TB_RAMOOPS_SIZE; off++) {
		for (k = 0; k < n; k++)
			if (*(volatile char *)(TB_RAMOOPS_BASE + off + k) !=
			    needle[k])
				break;
		if (k == n)
			return (int)off;
	}
	return -1;
}

void tb_ramoops_dump(int scale)
{
	ulong off;
	int zone = 0;

	int hit;

	/* A crash report beats everything else in the region: render it alone
	 * so the screen has one subject and the tail cannot push it off. */
	hit = tb_ramoops_seek("Unable to handle");
	if (hit < 0)
		hit = tb_ramoops_seek("Kernel panic");
	if (hit >= 0) {
		ulong from = hit > 200 ? (ulong)hit - 200 : 0;
		ulong len = 3400;

		if (from + len > TB_RAMOOPS_SIZE)
			len = TB_RAMOOPS_SIZE - from;
		tb_screen_log("PANIC FOUND", 2);
		tb_logv("PANIC OFF", (ulong)hit);
		tb_ram_text(TB_RAMOOPS_BASE + from, (int)len);
		tb_probe_dump(scale);
		return;
	}

	for (off = 0; off + 12 < TB_RAMOOPS_SIZE; off += 4) {
		volatile u32 *h = (volatile u32 *)(TB_RAMOOPS_BASE + off);
		u32 start, size;
		ulong from, len;

		if (h[0] != TB_PR_SIG)
			continue;
		start = h[1];
		size = h[2];
		if (!size || size > TB_RAMOOPS_SIZE || start > size)
			continue;

		/* The tail is what matters after a crash: it holds the report. */
		from = (ulong)start + size;
		len = 3400;
		if (len > size)
			len = size;
		tb_logv2("RAMOOPS", off, size);
		tb_ram_text(TB_RAMOOPS_BASE + off + 12 + from - len, (int)len);
		tb_probe_dump(scale);
		if (++zone >= 1)	/* one zone already fills a photo */
			break;
	}
	if (!zone)
		tb_screen_log("RAMOOPS none", 2);
}

void tb_mark(int idx)
{
	static const unsigned cols[16] = {
		0xFF00FF00, 0xFFFFFF00, 0xFF0000FF, 0xFFFFFFFF,
		0xFF00FFFF, 0xFFFF00FF, 0xFFFF8000, 0xFFFF0000,
		0xFF008080, 0xFF80FF80, 0xFF8080FF, 0xFF808080,
		0xFF808000, 0xFF800080, 0xFF804000, 0xFF80FFFF,
	};
	unsigned *fb = (unsigned *)TB_FB_BASE;
	unsigned col = cols[idx & 15];
	int r, c;

	/* One horizontal bar per call site, its own row, so a bar can never be
	 * mistaken for its neighbour in a photo of the screen. Cols 400..3100
	 * keep clear of the hex values printed at the left edge. */
	for (r = 0; r < 36; r++) {
		unsigned *line = fb + (ulong)(1200 + (idx & 15) * 44 + r) *
				 TB_LOG_STRIDE + 400;

		for (c = 0; c < 2700; c++)
			line[c] = col;
	}
}

/* ---------------------------------------------------------------------------
 * TB710FU: the on-screen text log.
 *
 * There is no UART on this board, so the panel ABL left scanning out
 * (0xD5100000, 3200x2000) is the only output channel this boot has. Every fact
 * worth reporting becomes one line here, drawn at 3x so a phone camera can read
 * it and prefixed with a call number, so even a half-legible photo still says
 * which line ran last. Glyphs come from U-Boot's own 8x16 console font - the
 * only font in the tree with letters in it.
 */
void tb_screen_text(int x, int y, const char *s, int scale)
{
	unsigned *fb = (unsigned *)TB_FB_BASE;

	for (; *s; s++, x += 8 * scale) {
		const unsigned char *g =
			&video_fontdata_8x16[(unsigned char)*s * 16];
		int row, col, dy, dx;

		for (row = 0; row < 16; row++) {
			unsigned char bits = g[row];

			for (col = 0; col < 8; col++) {
				unsigned v = (bits & (0x80 >> col)) ?
					TB_COLOR_WHITE : TB_COLOR_BLACK;
				unsigned *line = fb + (ulong)(y + row * scale) *
					TB_LOG_STRIDE + x + col * scale;

				for (dy = 0; dy < scale; dy++)
					for (dx = 0; dx < scale; dx++)
						line[(ulong)dy * TB_LOG_STRIDE + dx] = v;
			}
		}
	}
}

void tb_screen_log_reset(void)
{
	tb_log_y = TB_LOG_TOP;
}

void tb_screen_log(const char *s, int scale)
{
	char buf[96];
	char *p = buf;

	/* The call number on every line is what survives a bad photo. */
	tb_put_hex(&p, tb_log_n++, 2);
	*p++ = ' ';
	while (*s && (p - buf) < (int)sizeof(buf) - 2)
		*p++ = *s++;
	*p = 0;

	tb_fb_clear_at(TB_LOG_X, tb_log_y, 1600, 16 * scale + 8,
		       TB_LOG_STRIDE, 1);
	tb_screen_text(TB_LOG_X, tb_log_y, buf, scale);
	tb_log_y += 16 * scale + 4;

	/* Wrap rather than run off the bottom of the panel: the newest lines are
	 * the ones worth reading, and a photo of the screen only holds about 28. */
	if (tb_log_y > 1900)
		tb_log_y = TB_LOG_TOP;
}

void tb_screen_log_hex(ulong v, int scale)
{
	char txt[9];
	char *p = txt;

	tb_put_hex(&p, v, 8);
	*p = 0;
	tb_screen_log(txt, scale);
}

/* Same as tb_screen_log_hex(), but always on the same row: repeated calls
 * overwrite the value in place instead of scrolling the log. */
void tb_screen_fixed_hex(int y, ulong v, int scale)
{
	char txt[9];
	char *p = txt;

	tb_put_hex(&p, v, 8);
	*p = 0;
	tb_fb_clear_at(TB_LOG_X, y, 8 * 8 * scale + 8, 16 * scale + 8,
		       TB_LOG_STRIDE, 1);
	tb_screen_text(TB_LOG_X, y, txt, scale);
}

/* Called from relocate_64.S, where the arguments arrive as x0 = row, x1 =
 * value. */
void tb_screen_hexline(ulong y, ulong v)
{
	tb_screen_fixed_hex((int)y, v, 3);
}

/* board_init_f() saves the pre-relocation tree and board_init_r() puts it
 * back: U-Boot's relocation adds the offset to every pointer it fixes up, and
 * the tree lives outside the image, so the offset must not reach it. */
static void *tb_saved_fdt;

void tb_fdt_save(void *blob)
{
	tb_saved_fdt = blob;
}

void *tb_fdt_load(void)
{
	return tb_saved_fdt;
}

/* Blank the bottom of the log band. The panel keeps whatever the last boot drew
 * there, and a line the current boot has not reached yet still shows the
 * previous boot's numbering, which reads as a mirage of a longer log. Only the
 * lower part is cleared: the lines above it are redrawn by this boot anyway,
 * and clearing the whole band would be ~21 MB of strongly ordered framebuffer
 * stores, which is the order of store traffic that has already pushed this
 * board's boot past its watchdog window once.
 */
void tb_screen_clear(void)
{
	unsigned *fb = (unsigned *)TB_FB_BASE;
	int row, col;

	for (row = 1100; row < 1990; row++) {
		unsigned *line = fb + (ulong)row * TB_LOG_STRIDE;

		for (col = 0; col < 1720; col++)
			line[col] = TB_COLOR_BLACK;
	}
	tb_log_y = TB_LOG_TOP;
	if (tb_log_n < 14)
		tb_log_n = 0;
}

/* Where the kernel paints its first milestone bar, in a colour it never uses:
 * if this one is still on the panel next boot, the jump happened and the panel
 * was writable - so what failed is the kernel's own first instruction. */
void tb_kmark_jump(void)
{
	unsigned *fb = (unsigned *)TB_FB_BASE;
	int row, col;

	for (row = 30; row < 90; row++) {
		unsigned *line = fb + (ulong)row * TB_LOG_STRIDE + 2000;

		for (col = 0; col < 1200; col++)
			line[col] = 0xFFFF00FF;
	}
}
