// SPDX-License-Identifier: AGPL-3.0-or-later
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>

MODULE_LICENSE("GPL");

static int __init atmu_diag_init(void)
{
	static const u32 offs[] = {
		0x0000000, 0x0800000, 0x1000000, 0x1800000,
		0x2000000, 0x2700000, 0x2800000, 0x3000000, 0x3f00000
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		void __iomem *b = ioremap(0xe8000000 + offs[i], 0x1000);
		if (!b)
			continue;
		pr_info("atmu_diag: 0x%08x = %08x %08x %08x %08x\n",
			0xe8000000 + offs[i],
			in_be32(b + 0x00), in_be32(b + 0x04),
			in_be32(b + 0x08), in_be32(b + 0x0c));
		iounmap(b);
	}
	return -ENODEV;
}

static void __exit atmu_diag_exit(void)
{
}

module_init(atmu_diag_init);
module_exit(atmu_diag_exit);
