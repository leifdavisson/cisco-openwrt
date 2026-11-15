REPO_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BP_DIR ?= $(REPO_DIR)/sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/backports-6.1.110-1
KDIR ?= $(REPO_DIR)/sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167
CROSS ?= $(REPO_DIR)/sdk/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-

obj-m := mwl8k.o

NOSTDINC_FLAGS := \
	-I$(src) \
	-I$(BP_DIR)/backport-include \
	-I$(BP_DIR)/backport-include/uapi \
	-I$(BP_DIR)/include \
	-I$(BP_DIR)/include/uapi \
	-DCONFIG_BACKPORT_INTEGRATE \
	-DCPTCFG_MAC80211_DEBUGFS=1 \
	-include $(BP_DIR)/backport-include/backport/backport.h \
	-Wno-unused-result

ccflags-y += $(NOSTDINC_FLAGS)
# mac80211 >= 6.9 (OpenWrt 24.10/25.12 backports) needs the chanctx emulation ops
ccflags-y += $(shell grep -qs ieee80211_emulate_add_chanctx $(BP_DIR)/include/net/mac80211.h && echo -DMWL_EMULATE_CHANCTX)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) KBUILD_MODPOST_WARN=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc CROSS_COMPILE=$(CROSS) clean
