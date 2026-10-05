/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * relocate - common relocation function for AArch64 U-Boot
 *
 * (C) Copyright 2013
 * Albert ARIBAUD <albert.u.boot@aribaud.net>
 * David Feng <fenghua@phytium.com.cn>
 */

#include <asm-offsets.h>
#include <config.h>
#include <elf.h>
#include <linux/linkage.h>
#include <asm/macro.h>

/*
 * void relocate_code(addr_moni)
 *
 * This function relocates the monitor code.
 * x0 holds the destination address.
 */
ENTRY(relocate_code)
	stp	x29, x30, [sp, #-32]!	/* create a stack frame */
	mov	x29, sp

	/* TB710FU: mark 2 = relocate_code entered, stack frame live */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #2
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

	str	x0, [sp, #16]
	/*
	 * Copy u-boot from flash to RAM
	 */
	adrp	x1, __image_copy_start		/* x1 <- address bits [31:12] */
	add	x1, x1, :lo12:__image_copy_start/* x1 <- address bits [11:00] */
	subs	x9, x0, x1			/* x9 <- Run to copy offset */
	b.eq	relocate_done			/* skip relocation */
	/*
	 * Don't ldr x1, __image_copy_start here, since if the code is already
	 * running at an address other than it was linked to, that instruction
	 * will load the relocated value of __image_copy_start. To
	 * correctly apply relocations, we need to know the linked value.
	 *
	 * Linked &__image_copy_start, which we know was at
	 * CONFIG_TEXT_BASE, which is stored in _TEXT_BASE, as a non-
	 * relocated value, since it isn't a symbol reference.
	 */
	ldr	x1, _TEXT_BASE		/* x1 <- Linked &__image_copy_start */
	subs	x9, x0, x1		/* x9 <- Link to copy offset */

	adrp	x1, __image_copy_start		/* x1 <- address bits [31:12] */
	add	x1, x1, :lo12:__image_copy_start/* x1 <- address bits [11:00] */
	adrp	x2, __image_copy_end		/* x2 <- address bits [31:12] */
	add	x2, x2, :lo12:__image_copy_end	/* x2 <- address bits [11:00] */
copy_loop:
	ldp	x10, x11, [x1], #16	/* copy from source address [x1] */
	stp	x10, x11, [x0], #16	/* copy to   target address [x0] */
	cmp	x1, x2			/* until source end address [x2] */
	b.lo	copy_loop
	str	x0, [sp, #24]

	/* TB710FU: mark 3 = image copied to the relocation address */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #3
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16


	/*
	 * Fix .rela.dyn relocations
	 */
	adrp	x2, __rel_dyn_start		/* x2 <- address bits [31:12] */
	add	x2, x2, :lo12:__rel_dyn_start	/* x2 <- address bits [11:00] */
	adrp	x3, __rel_dyn_end		/* x3 <- address bits [31:12] */
	add	x3, x3, :lo12:__rel_dyn_end	/* x3 <- address bits [11:00] */

	/* TB710FU: fixup loop state (see fixloop) */
	mov	x5, #0
	mov	x12, #0
	mov	x13, #0x100000
	mov	x15, #0
fixloop:
	ldp	x0, x1, [x2], #16	/* (x0,x1) <- (SRC location, fixup) */
	ldr	x4, [x2], #8		/* x4 <- addend */
	and	x1, x1, #0xffffffff
	cmp	x1, #R_AARCH64_RELATIVE
	bne	fixnext

	/* relative fix: store addend plus offset at dest location */
	add	x0, x0, x9
	add	x4, x4, x9

	/* TB710FU: a genuine fixup target is 8-byte aligned and inside the
	 * relocated copy. Build #19 hit an alignment fault on a target ending in
	 * 0x2, which a real relocation table never contains, so the loop is not
	 * reading a clean table - reject anything that fails either test, count
	 * it (x5) and remember the first rejected target (x15) for the screen.
	 * x6/x7 = dest start/end, x12 = iterations, x13 = iteration cap. */
	tst	x0, #7
	b.ne	fixskip
	ldr	x6, [sp, #16]
	ldr	x7, [sp, #24]
	cmp	x0, x6
	b.lo	fixskip
	cmp	x0, x7
	b.hs	fixskip
	str	x4, [x0]
	b	fixnext
fixskip:
	add	x5, x5, #1
	cbnz	x15, fixnext
	mov	x15, x0
fixnext:
	add	x12, x12, #1

	/* TB710FU: progress bar. One pixel per 16 iterations on row 1150, so a
	 * stall inside this loop is visible as a bar that stops growing.
	 * x10 = framebuffer base, x11 = row offset, x8 = scratch. */
	tst	x12, #0xf
	b.ne	fixnobar
	movz	x10, #0xD5F0, lsl #16		/* 0xD5100000 + 1150 * 12800 */
	movk	x10, #0xA000
	movz	x11, #0xFFFF, lsl #16		/* bright red, visible on black */
	lsr	x8, x12, #4
	lsl	x8, x8, #2
	str	w11, [x10, x8]
fixnobar:
	cmp	x12, x13
	b.hs	fixdone
	cmp	x2, x3
	b.lo	fixloop
fixdone:

	/* TB710FU: show how many fixups were skipped and how many ran.
	 * Row 1050 = skipped, row 1080 = iterations. */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	mov	x0, #1050
	mov	x1, x5
	bl	tb_screen_hexline
	mov	x0, #1080
	mov	x1, x12
	bl	tb_screen_hexline

	/* row 1110: the first fixup target that was rejected (0 = none) */
	stp	x0, x1, [sp, #-16]!
	mov	x0, #1110
	mov	x1, x15
	bl	tb_screen_hexline
	ldp	x0, x1, [sp], #16

	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

relocate_done:

	/* TB710FU: mark 4 = relocation fixups done */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #4
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

	switch_el x1, 3f, 2f, 1f
	bl	hang
3:	mrs	x0, sctlr_el3
	b	0f
2:	mrs	x0, sctlr_el2
	b	0f
1:	mrs	x0, sctlr_el1
0:
	/* TB710FU: mark 6 = EL switch done, SCTLR read */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #6
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16
	tbz	w0, #2, 5f	/* skip flushing cache if disabled */
4:	ldp	x0, x1, [sp, #16]

	/* TB710FU: mark 11 = about to flush the dcache over the copy */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #11
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

	bl	__asm_flush_dcache_range

	/* TB710FU: mark 12 = dcache flush returned */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #12
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

	bl     __asm_flush_l3_dcache

	/* TB710FU: invalidate the i-cache *after* the d-cache has been written
	 * back, which is the order self-modifying code requires. */
	mrs	x0, sctlr_el1
	tbz	w0, #12, 9f
	ic	iallu
	isb	sy
9:

	/* TB710FU: mark 13 = l3 flush returned, about to ret */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #13
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16


	/* TB710FU: mark 7 = cache flush done, about to return */
	stp	x0, x1, [sp, #-16]!
	stp	x2, x3, [sp, #-16]!
	stp	x4, x5, [sp, #-16]!
	str	x9, [sp, #-16]!
	mov	x0, #7
	bl	tb_mark
	ldr	x9, [sp], #16
	ldp	x4, x5, [sp], #16
	ldp	x2, x3, [sp], #16
	ldp	x0, x1, [sp], #16

5:	ldp	x29, x30, [sp],#32
	ret
ENDPROC(relocate_code)
