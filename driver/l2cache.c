// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * l2cache.ko - report (and optionally enable) the P1020/P1023 front-side L2 cache.
 *
 * On MPC85xx the L2 cache is switched on by the boot loader (U-Boot: "L2: 256 KiB enabled");
 * Linux only reads the controller for EDAC. Cisco's ROMMON leaves it off for a non-IOS payload.
 *
 *   insmod l2cache.ko            -> print L2CTL
 *   insmod l2cache.ko enable=1   -> invalidate + enable (L2CTL = L2E | L2I), same as U-Boot
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/smp.h>
#include <asm/reg.h>

#define L2_BASE		0xe0020000UL	/* CCSR 0xe0000000 + 0x20000 */
#define L2CTL_L2E	0x80000000
#define L2CTL_L2I	0x40000000
#ifndef HID1_MBDD
#define HID1_MBDD	0x00008000	/* optimised sync, core rev >= 5.0 (as U-Boot sets it) */
#endif

static bool enable;
module_param(enable, bool, 0444);

/* HID1[ABE]: broadcast dcbf/dcbi/icbi to the L2 cache and the other core. U-Boot sets it; ROMMON does not. */
static void set_hid1_abe(void *unused)
{
	u32 hid1 = mfspr(SPRN_HID1) | HID1_ASTME | HID1_ABE;

	if ((mfspr(SPRN_PVR) & 0xff) >= 0x50)
		hid1 |= HID1_MBDD;
	mtspr(SPRN_HID1, hid1);
	isync();
}

static int __init l2cache_init(void)
{
	void __iomem *l2 = ioremap(L2_BASE, 0x1000);
	unsigned long flags;
	u32 before, after;

	if (!l2)
		return -ENOMEM;
	if (enable)
		on_each_cpu(set_hid1_abe, NULL, 1);
	before = in_be32(l2);
	pr_info("l2cache: L2CTL=0x%08x (%s, size field %u), HID1=0x%08lx\n", before,
		(before & L2CTL_L2E) ? "ENABLED" : "disabled", (before >> 28) & 3, mfspr(SPRN_HID1));
	if (enable && !(before & L2CTL_L2E)) {
		local_irq_save(flags);
		mb();
		isync();
		out_be32(l2, L2CTL_L2E | L2CTL_L2I);
		mb();
		isync();
		local_irq_restore(flags);
		after = in_be32(l2);
		pr_info("l2cache: L2CTL now 0x%08x (%s)\n", after,
			(after & L2CTL_L2E) ? "ENABLED" : "still disabled");
	}
	iounmap(l2);
	return 0;
}

static void __exit l2cache_exit(void) { }

module_init(l2cache_init);
module_exit(l2cache_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Report/enable the MPC85xx front-side L2 cache");
