/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Secondary-core boot page for the Cisco Aironet 3702i/2602i (P1023-class, e500v2).
 *
 * Cisco's ROMMON leaves core 1 in boot hold-off and provides no ePAPR spin table, so Linux
 * reports "No cpu-release-addr for cpu 1". This 4 KiB page is what U-Boot would normally
 * provide: core 1 comes out of reset at 0xfffffffc (boot page translation points the 4 KiB
 * at 0xfffff000 to this page in RAM), publishes a spin-table entry and waits for the OS.
 *
 * Layout (offsets inside the page):   0x000 code   0x800 spin table (cpu-release-addr)
 *                                     0x900 progress word (debug)   0xffc reset vector
 */
#define TABLE	0x800
#define DBG	0x900

#define SPRN_SRR0	26
#define SPRN_SRR1	27
#define SPRN_TBWL	284
#define SPRN_TBWU	285
#define SPRN_PIR	286
#define SPRN_MAS0	624
#define SPRN_MAS1	625
#define SPRN_MAS2	626
#define SPRN_MAS3	627
#define SPRN_MAS7	944
#define SPRN_HID0	1008
#define SPRN_HID1	1009
#define SPRN_PVR	287

	.text
	.globl _start
_start:
	lis	10, 0xffff
	ori	10, 10, 0xf000		/* r10 = this page as the core sees it */
	li	0, 1
	stw	0, DBG(10)

	/* HID1: address streaming + address broadcast (dcbf/dcbi/icbi must reach the L2 cache and the
	 * other core, Linux relies on it), MBDD on core rev >= 5.0. This is what U-Boot's release.S sets. */
	li	3, 0x3000		/* HID1_ASTME | HID1_ABE */
	mfspr	0, SPRN_PVR
	andi.	0, 0, 0xff
	cmpwi	0, 0x50
	blt	2f
	ori	3, 3, 0x8000		/* HID1_MBDD */
2:	mfspr	0, SPRN_HID1
	or	3, 3, 0
	mtspr	SPRN_HID1, 3
	isync

	/* Time base: the primary froze it (DEVDISR); start from 0 so both cores agree. */
	li	0, 0
	mtspr	SPRN_TBWL, 0
	mtspr	SPRN_TBWU, 0
	mfspr	3, SPRN_HID0
	ori	3, 3, 0x4000		/* HID0[TBEN] */
	mtspr	SPRN_HID0, 3
	isync

	/* ePAPR spin table entry: addr_h, addr_l, r3_h, r3_l, reserved, pir. addr_l = 1 means "spinning". */
	li	0, 0
	stw	0, TABLE+0(10)
	stw	0, TABLE+8(10)
	stw	0, TABLE+12(10)
	mfspr	4, SPRN_PIR
	stw	4, TABLE+20(10)
	li	0, 1
	stw	0, TABLE+4(10)
	li	0, 2
	stw	0, DBG(10)

1:	lwz	11, TABLE+4(10)		/* caches are off on this core: every load hits memory */
	andi.	0, 11, 1
	bne	1b

	li	0, 3
	stw	0, DBG(10)
	lwz	5, TABLE+20(10)
	mtspr	SPRN_PIR, 5
	lwz	3, TABLE+12(10)		/* r3 for the OS */

	/* Initial mapped area: 64 MiB, 1:1, around the entry point, TLB1 entry 1, AS0. */
	rlwinm	6, 11, 0, 0, 5		/* entry & ~(64M - 1) */
	lis	0, 0x1001		/* MAS0: TLBSEL=1, ESEL=1 */
	mtspr	SPRN_MAS0, 0
	lis	0, 0xc000		/* MAS1: V | IPROT, TID=0, TS=0 */
	ori	0, 0, 0x0800		/*       TSIZE = 64M */
	mtspr	SPRN_MAS1, 0
	mtspr	SPRN_MAS2, 6		/* EPN, cacheable */
	ori	7, 6, 0x15		/* RPN | SX | SW | SR */
	mtspr	SPRN_MAS3, 7
	li	0, 0
	mtspr	SPRN_MAS7, 0
	tlbwe
	isync

	li	0, 4
	stw	0, DBG(10)
	mtspr	SPRN_SRR0, 11
	mfmsr	0
	mtspr	SPRN_SRR1, 0
	li	4, 0
	li	5, 0
	li	6, 0
	lis	7, 0x0400		/* r7 = IMA size (ePAPR) */
	li	8, 0
	li	9, 0
	rfi

	.org	0xffc
	b	_start			/* reset vector */
