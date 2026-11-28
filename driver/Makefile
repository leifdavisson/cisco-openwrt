# SPDX-License-Identifier: GPL-2.0-or-later OR AGPL-3.0-or-later
# The DPAA backend needs a rebuilt kernel and dts/cisco-3702i-dpaa.dts.
ETHERNET ?= im
export ETHERNET
ifeq ($(ETHERNET),dpaa)
ETH_MODULES := cisco_dpaa_bus.o p1023_fman_dpaa.o
ccflags-y += -I$(srctree)/drivers/net/ethernet/freescale/fman
else ifeq ($(ETHERNET),im)
ETH_MODULES := p1023_fman_im.o
else
$(error ETHERNET must be im or dpaa)
endif
obj-m += $(ETH_MODULES)
obj-m += pci_diag.o
obj-m += radio_reset.o
obj-m += atmu_diag.o
ccflags-y += -DCONFIG_FMAN_UCODE_EMBEDDED

REPO_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
KDIR ?= $(REPO_DIR)/sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167
CROSS ?= $(REPO_DIR)/sdk/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-
STAGING_DIR ?= $(REPO_DIR)/sdk/staging_dir
export STAGING_DIR

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) modules
	$(CROSS)strip --strip-unneeded $(ETH_MODULES:.o=.ko)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) clean

.PHONY: all clean dpaa
dpaa:
	$(MAKE) ETHERNET=dpaa all
