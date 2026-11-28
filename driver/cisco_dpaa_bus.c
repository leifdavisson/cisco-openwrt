// SPDX-License-Identifier: GPL-2.0-or-later
/* P1023 portals share one 4 MiB DPAA SRAM LAW (target 0x0e). */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define LAW_ENABLE 0x80000000
#define PORTAL_LAW (LAW_ENABLE | (0x0e << 20) | 21)

static const struct of_device_id cisco_dpaa_children[] = {
	{ .compatible = "simple-bus" }, {}
};

static int cisco_dpaa_bus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *laws, *qbman;
	u32 base, attr, start;
	int i, free_slot = -1;

	/* A warm IOS handoff may still point DMA at IOS-owned RAM. */
	qbman = devm_ioremap(dev, 0xe0088000, 0x3000);
	if (!qbman)
		return -ENOMEM;
	if (ioread32be(qbman + 0xc04) || ioread32be(qbman + 0xc24) ||
	    ioread32be(qbman + 0x2c04))
		return dev_err_probe(dev, -EBUSY,
			"QMan/BMan already configured; power-cycle and RAM-boot from ROMMON\n");

	if (of_property_read_u32(dev->of_node, "cisco,portal-base", &start))
		return -EINVAL;
	if (start & 0x3fffff)
		return -EINVAL;
	laws = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(laws))
		return PTR_ERR(laws);

	/* Leave ROMMON's DDR, NOR and PCI windows alone. */
	for (i = 0; i < 12; i++) {
		base = ioread32be(laws + i * 0x20);
		attr = ioread32be(laws + i * 0x20 + 8);
		if (!(attr & LAW_ENABLE)) {
			free_slot = i;
			continue;
		}
		if (((u64)base << 12) < (u64)start + 0x400000 &&
		    ((u64)base << 12) + (1ULL << ((attr & 0x3f) + 1)) > start) {
			/* ROMMON uses an 8 MiB window; the portals need its first 4 MiB. */
			if ((attr & 0xfff00000) == (PORTAL_LAW & 0xfff00000) &&
			    ((u64)base << 12) <= start &&
			    ((u64)base << 12) + (1ULL << ((attr & 0x3f) + 1)) >=
			    (u64)start + 0x400000)
				goto populate;
			return dev_err_probe(dev, -EBUSY, "portal window overlaps LAW%d\n", i);
		}
	}
	if (free_slot < 0)
		return -ENOSPC;
	iowrite32be(start >> 12, laws + free_slot * 0x20);
	iowrite32be(PORTAL_LAW, laws + free_slot * 0x20 + 8);
	if (ioread32be(laws + free_slot * 0x20 + 8) != PORTAL_LAW)
		return -EIO;
	dev_info(dev, "LAW%d: DPAA portals at 0x%08x\n", free_slot, start);

populate:
	/* Match QMan's initiator address to the portal LAW (DQRR stashing). */
	iowrite32be(0, qbman + 0xc80);
	iowrite32be(start, qbman + 0xc84);
	if (ioread32be(qbman + 0xc84) != start)
		return -EIO;
	dev_info(dev, "QMan portal BAR: 0x%08x\n", start);
	return of_platform_populate(dev->of_node, cisco_dpaa_children, NULL, dev);
}

static const struct of_device_id cisco_dpaa_bus_ids[] = {
	{ .compatible = "cisco,p1023-dpaa" }, {}
};
MODULE_DEVICE_TABLE(of, cisco_dpaa_bus_ids);

static struct platform_driver cisco_dpaa_bus_driver = {
	.probe = cisco_dpaa_bus_probe,
	.driver = {
		.name = "cisco_dpaa_bus",
		.of_match_table = cisco_dpaa_bus_ids,
		.suppress_bind_attrs = true,
	},
};

/* QMan/BMan controllers live until reboot, as do their portal mappings. */
static int __init cisco_dpaa_bus_init(void)
{
	struct platform_device *pdev;
	struct device_node *node;
	int ret;

	/* The RDB board code only publishes its built-in bus match table. */
	node = of_find_matching_node(NULL, cisco_dpaa_bus_ids);
	if (!node)
		return -ENODEV;
	pdev = of_platform_device_create(node, NULL, NULL);
	of_node_put(node);
	if (!pdev)
		return -ENODEV;
	ret = platform_driver_register(&cisco_dpaa_bus_driver);
	if (ret)
		platform_device_unregister(pdev);
	return ret;
}
module_init(cisco_dpaa_bus_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cisco P1023 DPAA portal window");
