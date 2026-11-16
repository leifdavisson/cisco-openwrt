# SPDX-License-Identifier: GPL-2.0-or-later OR AGPL-3.0-or-later
obj-m += p1023_fman_im.o
obj-m += pci_diag.o
obj-m += radio_reset.o
obj-m += atmu_diag.o
obj-m += l2cache.o
ccflags-y += -DCONFIG_FMAN_UCODE_EMBEDDED

REPO_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
KDIR ?= $(REPO_DIR)/sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167
CROSS ?= $(REPO_DIR)/sdk/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-
export STAGING_DIR := $(REPO_DIR)/sdk/staging_dir

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) modules
	$(CROSS)strip --strip-unneeded p1023_fman_im.ko

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) clean
