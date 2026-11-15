// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * pci_diag.c - P1020 pinmux / I2C controller / GPIO audit
 * Author: Antigravity Pair Programmer
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");

#define CCSR_BASE 0xe0000000UL

static int __init pci_diag_init(void)
{
	void __iomem *ccsr;
	int i;

	pr_info("=== PCI_DIAG: P1020 PINMUX / I2C / GPIO AUDIT ===\n");

	ccsr = ioremap(CCSR_BASE, 0x100000);
	if (!ccsr) {
		pr_err("Failed to ioremap CCSR!\n");
		return 0;
	}

	pr_info("PORPLLSR  = 0x%08x\n", in_be32(ccsr + 0xe0000));
	pr_info("PORDEVSR  = 0x%08x\n", in_be32(ccsr + 0xe000c));
	pr_info("PMUXCR    = 0x%08x\n", in_be32(ccsr + 0xe0060));
	pr_info("PMUXCR2   = 0x%08x\n", in_be32(ccsr + 0xe0064));
	pr_info("DEVDISR   = 0x%08x\n", in_be32(ccsr + 0xe0070));

	for (i = 0; i < 2; i++) {
		void __iomem *b = ccsr + 0x3000 + i * 0x100;
		pr_info("I2C%d: ADR=%02x FDR=%02x CR=%02x SR=%02x DFSRR=%02x\n", i,
			ioread8(b + 0), ioread8(b + 4), ioread8(b + 8),
			ioread8(b + 0xc), ioread8(b + 0x14));
	}

	pr_info("GPIO GPDIR=0x%08x GPODR=0x%08x GPDAT=0x%08x\n",
		in_be32(ccsr + 0xf000), in_be32(ccsr + 0xf004),
		in_be32(ccsr + 0xf008));

	iounmap(ccsr);
	return 0;
}

static void __exit pci_diag_exit(void)
{
}

module_init(pci_diag_init);
module_exit(pci_diag_exit);
