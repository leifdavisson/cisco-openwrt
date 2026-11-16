/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pre-kernel stage, runs on core 0 straight after ROMMON's "executing...", before the
 * OpenWrt boot wrapper. Installs the secondary boot page, releases core 1 into its
 * spin loop with both time bases synchronised, then continues into the wrapper.
 * Uses only r0 and r20-r31; r1-r9 reach the wrapper untouched.
 */
#define BOOTPG_PHYS	0x0ff00000	/* reserved in the device tree (/memreserve/) */
#ifndef WRAPPER_ENTRY
#define WRAPPER_ENTRY	0x01600340	/* target of the wrapper's original first instruction */
#endif
#define SPRN_TBWL	284
#define SPRN_TBWU	285

	.text
	.globl _start
_start:
	bl	0f
0:	mflr	30			/* r30 = runtime address of label 0 */

	/* HID1 on core 0: address streaming + address broadcast (+ MBDD on rev >= 5.0), as U-Boot sets
	 * on every multi-core e500. Without ABE a dcbf/dcbi/icbi does not reach the L2 cache or the other
	 * core: this stage then reads a stale spin table, and Linux would run SMP with unsynchronised
	 * instruction caches. ROMMON leaves HID1 = RFXE only. */
	li	21, 0x3000
	mfspr	0, 287			/* PVR */
	andi.	0, 0, 0xff
	cmpwi	0, 0x50
	blt	11f
	ori	21, 21, 0x8000
11:	mfspr	0, 1009			/* HID1 */
	or	21, 21, 0
	mtspr	1009, 21
	isync

	/* 0. L2 cache: ROMMON leaves it off for a non-IOS payload. Same sequence as U-Boot:
	 *    L2CTL = L2E | L2I (enable + flash invalidate), only if it is not on already. */
	lis	21, 0xe002
	lwz	0, 0(21)
	andis.	0, 0, 0x8000
	bne	10f
	msync
	isync
	lis	0, 0xc000
	stw	0, 0(21)
	msync
	isync
10:
	addi	28, 30, (msg_hello - 0b)
	bl	puts

	/* 1. copy the boot page to its reserved RAM page and push it out of core 0's cache */
	addi	21, 30, (bootpg - 0b)
	lis	22, BOOTPG_PHYS@h
	li	23, 1024
	mtctr	23
	addi	21, 21, -4
	addi	24, 22, -4
1:	lwzu	0, 4(21)
	stwu	0, 4(24)
	bdnz	1b
	li	23, 128			/* 4096 / 32-byte lines */
	mtctr	23
	mr	24, 22
2:	dcbf	0, 24
	addi	24, 24, 32
	bdnz	2b
	sync

	/* 2. freeze both time bases (GUTS DEVDISR: TB0 | TB1) */
	lis	25, 0xe00e
	lwz	26, 0x70(25)		/* r26 = original DEVDISR */
	ori	0, 26, 0x5000
	stw	0, 0x70(25)
	lwz	0, 0x70(25)

	/* 3. boot page translation -> our page, then release core 1 (ECM EEBPCR[CPU1_EN]) */
	lis	27, 0xe000
	lis	0, 0x8000
	ori	0, 0, (BOOTPG_PHYS >> 12)
	stw	0, 0x20(27)
	lwz	0, 0x20(27)
	lwz	0, 0x1010(27)
	oris	0, 0, 0x0200
	stw	0, 0x1010(27)

	/* 4. wait until core 1 reports "spinning" (addr_l == 1) */
	ori	24, 22, 0x804
	lis	23, 0x0400
	mtctr	23
3:	dcbf	0, 24
	lwz	29, 0(24)
	cmpwi	29, 1
	beq	4f
	bdnz	3b
4:	mr	31, 29			/* r31 = final addr_l */

	/* 5. zero our time base and let both run */
	li	0, 0
	mtspr	SPRN_TBWL, 0
	mtspr	SPRN_TBWU, 0
	stw	26, 0x70(25)
	lwz	0, 0x70(25)

	cmpwi	31, 1
	bne	5f
	addi	28, 30, (msg_ok - 0b)
	bl	puts
	b	6f
5:	addi	28, 30, (msg_fail - 0b)
	bl	puts
	ori	24, 22, 0x900
	dcbf	0, 24
	lwz	29, 0(24)
	addi	29, 29, '0'
	bl	putc
	addi	28, 30, (msg_nl - 0b)
	bl	puts

6:	lis	0, WRAPPER_ENTRY@h
	ori	0, 0, WRAPPER_ENTRY@l
	mtctr	0
	bctr

/* r29 = character; ROMMON's UART at CCSR 0xe0004500 */
putc:	lis	20, 0xe000
	ori	20, 20, 0x4500
7:	lbz	0, 5(20)
	andi.	0, 0, 0x20
	beq	7b
	stb	29, 0(20)
	blr

/* r28 = string */
puts:	mflr	27
8:	lbz	29, 0(28)
	cmpwi	29, 0
	beq	9f
	bl	putc
	addi	28, 28, 1
	b	8b
9:	mtlr	27
	lis	27, 0xe000		/* restore r27 = CCSR base for callers */
	blr

msg_hello:	.asciz "\r\n[smp-shim] L2 cache on, releasing core 1...\r\n"
msg_ok:		.asciz "[smp-shim] core 1 is in its spin loop\r\n"
msg_fail:	.asciz "[smp-shim] core 1 did NOT come up, progress="
msg_nl:		.asciz "\r\n"
	.balign	4
bootpg:
	.incbin	"bootpg.bin"
