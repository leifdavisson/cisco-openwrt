// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * radio_reset.c - Replay Cisco radio PCIe reset pulse (GPIO 1 & 3 at 0xe000fc00) and rescan PCI.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/io.h>

#define GPIO_BASE	0xe000fc00UL
#define GPDAT		0x08
#define RESET_24G	0x40000000	/* GPIO 1: Hammer 2.4 GHz */
#define RESET_5G	0x10000000	/* GPIO 3: Triggerfish 5 GHz */
#define PWR_GPIO9	0x00400000	/* GPIO 9 */
#define PWR_GPIO10	0x00200000	/* GPIO 10 */

#define L2CTL_BASE	0xe0020000UL
#define L2CTL_L2E	0x80000000U
#define L2CTL_L2I	0x40000000U

static void __init enable_l2_cache(void)
{
	void __iomem *l2;
	u32 val;
	int i;

	l2 = ioremap(L2CTL_BASE, 0x100);
	if (!l2)
		return;

	val = in_be32(l2);
	pr_info("radio_reset: initial L2CTL=0x%08x\n", val);
	if (!(val & L2CTL_L2E)) {
		mb();
		out_be32(l2, val | L2CTL_L2I);
		for (i = 0; i < 10000; i++) {
			if (!(in_be32(l2) & L2CTL_L2I))
				break;
			udelay(1);
		}
		val = in_be32(l2) & ~L2CTL_L2I;
		out_be32(l2, val | L2CTL_L2E);
		mb();
		pr_info("radio_reset: enabled L2 cache, L2CTL=0x%08x\n", in_be32(l2));
	}
	iounmap(l2);
}

static int __init radio_reset_init(void)
{
	void __iomem *gpio;
	struct pci_dev *pdev = NULL;
	struct pci_bus *bus = NULL;
	u32 v;

	enable_l2_cache();

	/* 1. Remove existing endpoints so pci_rescan_bus reallocates BAR0/BAR2 cleanly */
	pdev = pci_get_device(0x11ab, 0xb024, NULL);
	if (pdev) {
		pci_stop_and_remove_bus_device_locked(pdev);
		pci_dev_put(pdev);
	}
	pdev = pci_get_device(0x11ab, 0xb050, NULL);
	if (pdev) {
		pci_stop_and_remove_bus_device_locked(pdev);
		pci_dev_put(pdev);
	}

	/* 2. Assert GPIO 9 & 10 high, then pulse GPIO 1 & GPIO 3 low for 50ms, then high for 256ms */
	gpio = ioremap(GPIO_BASE, 0x100);
	if (!gpio)
		return -ENOMEM;

	v = in_be32(gpio + GPDAT);
	pr_info("radio_reset: initial GPDAT=0x%08x\n", v);
	v |= (PWR_GPIO9 | PWR_GPIO10);
	out_be32(gpio + GPDAT, v & ~(RESET_24G | RESET_5G));
	msleep(50);
	out_be32(gpio + GPDAT, v | (RESET_24G | RESET_5G));
	msleep(256);
	pr_info("radio_reset: post-pulse GPDAT=0x%08x\n", in_be32(gpio + GPDAT));
	iounmap(gpio);

	/* 3. Rescan all PCI buses and re-assign resources */
	pci_lock_rescan_remove();
	while ((bus = pci_find_next_bus(bus)) != NULL)
		pci_rescan_bus(bus);
	pci_unlock_rescan_remove();

	return 0;
}

static void __exit radio_reset_exit(void) {}

module_init(radio_reset_init);
module_exit(radio_reset_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cisco 3702i radio reset pulse + PCI rescan");
