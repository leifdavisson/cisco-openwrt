#!/usr/bin/env python3
"""
repack_openwrt_image.py - Automated initramfs modification and kernel repacking for Cisco 3702i.
License: GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting

This tool:
1. Extracts the initramfs cpio archive from the base vmlinux kernel
2. Injects the P1023 FMan v2 ethernet driver (p1023_fman_im.ko) and Marvell PHY driver (marvell.ko)
3. Configures static IP 192.168.1.50/24, gateway/DNS 192.168.1.1, dhcp ignore='1'
4. Enables passwordless root SSH and LuCI Web UI
5. Re-packs and compresses the cpio archive with LZMA
6. Patches the vmlinux ELF with the new initramfs and updated size
7. Re-compresses vmlinux with XZ and creates the position-independent simpleImage with cisco-3702i.dtb
8. Packages the final bootable MZIP Segment Type 0 binary for Cisco ROMMON
"""

import os
import sys
import struct
import shutil
import subprocess

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW_ZIMAGE = "/tmp/openwrt-raw.bin"
DTB_FILE = os.path.join(PROJECT_DIR, "dts/cisco-3702i.dtb")
FMAN_KO = os.path.join(PROJECT_DIR, "driver/p1023_fman_im.ko")
RADIO_RESET_KO = os.path.join(PROJECT_DIR, "driver/radio_reset.ko")
MWLWIFI_KO = os.path.join(PROJECT_DIR, "driver/mwlwifi/mwlwifi.ko")
MWL8K_KO = os.path.join(PROJECT_DIR, "driver/mwl8k/mwl8k.ko")
MARVELL_KO = "/tmp/marvell.ko"
FW_8864 = "/tmp/mwl_fw/lib/firmware/mwlwifi/88W8864.bin"
FW_8764 = os.path.join(PROJECT_DIR, "firmware_dl/fmimage_8764_ap-1.fw")
STAGING_DIR = os.path.join(PROJECT_DIR, "sdk/staging_dir")
KDIR = os.path.join(PROJECT_DIR, "sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167")
CROSS = os.path.join(PROJECT_DIR, "sdk/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-")
STRIP_TOOL = CROSS + "strip"

WORK_DIR = "/tmp/owrt_repack"
ROOTFS_DIR = os.path.join(WORK_DIR, "rootfs")
VMLINUX_BIN = os.path.join(WORK_DIR, "vmlinux")
NEW_VMLINUX_BIN = os.path.join(WORK_DIR, "vmlinux_mod")
OUTPUT_RAW = os.path.join(WORK_DIR, "openwrt-cisco-raw.bin")
FINAL_CISCO_BIN = os.path.expanduser("~/Downloads/openwrt-3702i.bin")

# Constants from base vmlinux
ORIG_VMLINUX_XZ_OFFSET = 81920        # 0x14000
ORIG_VMLINUX_XZ_SIZE   = 6919172      # Exactly 6,919,172 bytes
INITRAMFS_OFFSET       = 8849408      # 0x870800 in vmlinux
INITRAMFS_MAX_SIZE     = 3785548      # Original initramfs.lzma size
SIZE_FIELD_OFFSET      = 0xc0cb50     # File offset of __initramfs_size in vmlinux

def run(cmd, cwd=None):
    print(f"[CMD] {cmd}")
    res = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[!] Error executing: {cmd}")
        print(f"Stdout:\n{res.stdout}")
        print(f"Stderr:\n{res.stderr}")
        sys.exit(1)
    return res.stdout

def main():
    print("=" * 70)
    print("OPENWRT IMAGE REPACKER FOR CISCO AIRONET 3702i")
    print("=" * 70)

    os.makedirs(WORK_DIR, exist_ok=True)

    # 1. Strip marvell.ko if needed
    if not os.path.exists(MARVELL_KO):
        src_marvell = os.path.join(KDIR, "drivers/net/phy/marvell.ko")
        run(f"{STRIP_TOOL} --strip-unneeded -o {MARVELL_KO} {src_marvell}")
    print(f"[+] Marvell PHY driver: {MARVELL_KO} ({os.path.getsize(MARVELL_KO)} bytes)")

    # 2. Recompile p1023_fman_im.ko, radio_reset.ko, mwlwifi.ko, and mwl8k.ko
    print("[*] Building latest ethernet & radio drivers...")
    run(f"make -C driver clean && make -C driver KDIR={KDIR} CROSS={CROSS} -j$(nproc)", cwd=PROJECT_DIR)
    run(f"STAGING_DIR={STAGING_DIR} make -C driver/mwlwifi KDIR={KDIR} CROSS={CROSS} -j$(nproc)", cwd=PROJECT_DIR)
    run(f"STAGING_DIR={STAGING_DIR} make -C driver/mwl8k KDIR={KDIR} CROSS={CROSS} -j$(nproc)", cwd=PROJECT_DIR)
    run(f"{STRIP_TOOL} --strip-unneeded {RADIO_RESET_KO} {MWLWIFI_KO} {MWL8K_KO}")
    print(f"[+] FMan driver: {FMAN_KO} ({os.path.getsize(FMAN_KO)} bytes)")
    print(f"[+] Radio reset driver: {RADIO_RESET_KO} ({os.path.getsize(RADIO_RESET_KO)} bytes)")
    print(f"[+] mwlwifi driver: {MWLWIFI_KO} ({os.path.getsize(MWLWIFI_KO)} bytes)")
    print(f"[+] mwl8k driver: {MWL8K_KO} ({os.path.getsize(MWL8K_KO)} bytes)")

    # 3. Extract vmlinux from raw zimage
    print("[*] Extracting vmlinux from bootwrapper...")
    with open(RAW_ZIMAGE, "rb") as f:
        f.seek(ORIG_VMLINUX_XZ_OFFSET)
        xz_data = f.read(ORIG_VMLINUX_XZ_SIZE)

    vmlinux_xz = os.path.join(WORK_DIR, "vmlinux.xz")
    with open(vmlinux_xz, "wb") as f:
        f.write(xz_data)

    run(f"xz -d -f -k {vmlinux_xz}")
    print(f"[+] Decompressed {VMLINUX_BIN} ({os.path.getsize(VMLINUX_BIN)} bytes)")

    # 4. Extract initramfs from vmlinux
    print("[*] Extracting initramfs cpio from vmlinux...")
    with open(VMLINUX_BIN, "rb") as f:
        f.seek(INITRAMFS_OFFSET)
        lzma_data = f.read(INITRAMFS_MAX_SIZE)

    initramfs_lzma = os.path.join(WORK_DIR, "initramfs.cpio.lzma")
    with open(initramfs_lzma, "wb") as f:
        f.write(lzma_data)

    initramfs_cpio = os.path.join(WORK_DIR, "initramfs.cpio")
    run(f"xz -d -f --format=lzma {initramfs_lzma}")

    # 5. Unpack cpio using fakeroot
    print("[*] Unpacking cpio rootfs using fakeroot...")
    if os.path.exists(ROOTFS_DIR):
        shutil.rmtree(ROOTFS_DIR)
    os.makedirs(ROOTFS_DIR, exist_ok=True)
    run(f"fakeroot sh -c 'cd {ROOTFS_DIR} && cpio -idm < {initramfs_cpio}'")

    # 6. Inject drivers & configuration files into rootfs
    print("[*] Injecting drivers, firmware, and custom configuration...")
    mod_dir = os.path.join(ROOTFS_DIR, "lib/modules/5.15.167")
    os.makedirs(mod_dir, exist_ok=True)
    for unused_mod in (
        "ath9k.ko", "ath9k_hw.ko", "ath9k_common.ko", "ath.ko",
        "usbcore.ko", "ehci-hcd.ko", "ehci-platform.ko", "usb-common.ko",
    ):
        p = os.path.join(mod_dir, unused_mod)
        if os.path.exists(p):
            os.remove(p)
    shutil.copy2(FMAN_KO, os.path.join(mod_dir, "p1023_fman_im.ko"))
    shutil.copy2(MARVELL_KO, os.path.join(mod_dir, "marvell.ko"))
    shutil.copy2(RADIO_RESET_KO, os.path.join(mod_dir, "radio_reset.ko"))
    shutil.copy2(MWLWIFI_KO, os.path.join(mod_dir, "mwlwifi.ko"))
    shutil.copy2(MWL8K_KO, os.path.join(mod_dir, "mwl8k.ko"))

    fw_mwlwifi_dir = os.path.join(ROOTFS_DIR, "lib/firmware/mwlwifi")
    fw_mwl8k_dir = os.path.join(ROOTFS_DIR, "lib/firmware/mwl8k")
    os.makedirs(fw_mwlwifi_dir, exist_ok=True)
    os.makedirs(fw_mwl8k_dir, exist_ok=True)
    shutil.copy2(FW_8864, os.path.join(fw_mwlwifi_dir, "88W8864.bin"))
    shutil.copy2(FW_8764, os.path.join(fw_mwl8k_dir, "fmimage_8764_ap-1.fw"))

    modules_d = os.path.join(ROOTFS_DIR, "etc/modules.d")
    os.makedirs(modules_d, exist_ok=True)
    with open(os.path.join(modules_d, "20-p1023-fman"), "w") as f:
        f.write("marvell\np1023_fman_im\n")

    # Network config
    net_cfg = """config interface 'loopback'
	option device 'lo'
	option proto 'static'
	option ipaddr '127.0.0.1'
	option netmask '255.0.0.0'

config device
	option name 'br-lan'
	option type 'bridge'
	list ports 'eth0'

config interface 'lan'
	option device 'br-lan'
	option proto 'static'
	option ipaddr '192.168.1.50'
	option netmask '255.255.255.0'
	option gateway '192.168.1.1'
	option dns '192.168.1.1'
"""
    with open(os.path.join(ROOTFS_DIR, "etc/config/network"), "w") as f:
        f.write(net_cfg)

    # DHCP config - MUST disable DHCP server on LAN
    dhcp_cfg = """config dnsmasq
	option domainneeded '1'
	option boguspriv '1'
	option filterwin2k '0'
	option localise_queries '1'
	option rebind_protection '1'
	option rebind_localhost '1'
	option local '/lan/'
	option domain 'lan'
	option expandhosts '1'
	option nonegcache '0'
	option authoritative '1'
	option readethers '1'
	option leasefile '/tmp/dhcp.leases'
	option resolvfile '/tmp/resolv.conf.d/resolv.conf.auto'
	option localservice '1'

config dhcp 'lan'
	option interface 'lan'
	option start '100'
	option limit '150'
	option leasetime '12h'
	option ignore '1'

config odhcpd 'odhcpd'
	option maindhcp '0'
	option leasefile '/tmp/hosts/odhcpd'
	option leasetrigger '/usr/sbin/odhcpd-update'
	option loglevel '4'
"""
    with open(os.path.join(ROOTFS_DIR, "etc/config/dhcp"), "w") as f:
        f.write(dhcp_cfg)

    # Passwordless root for SSH
    with open(os.path.join(ROOTFS_DIR, "etc/shadow"), "w") as f:
        f.write("root:::0:99999:7:::\ndaemon:*:0:0:99999:7:::\nftp:*:0:0:99999:7:::\nnetwork:*:0:0:99999:7:::\nnobody:*:0:0:99999:7:::\n")

    # rc.local fallback startup
    rc_local = """# Put your custom commands here that should be executed once
# the system init finished. By default this file does nothing.

insmod /lib/modules/5.15.167/marvell.ko 2>/dev/null || true
insmod /lib/modules/5.15.167/p1023_fman_im.ko 2>/dev/null || true
ip link set eth0 up 2>/dev/null || true
ip link set br-lan up 2>/dev/null || true
ip addr add 192.168.1.50/24 dev br-lan 2>/dev/null || true
ip route add default via 192.168.1.1 dev br-lan 2>/dev/null || true
/etc/init.d/dropbear restart 2>/dev/null || true
/etc/init.d/uhttpd restart 2>/dev/null || true

insmod /lib/modules/5.15.167/radio_reset.ko 2>/dev/null || true
for d in 9000:01:00.0 a000:03:00.0; do
    [ -e /sys/bus/pci/devices/$d/enable ] && echo 1 > /sys/bus/pci/devices/$d/enable
done
insmod /lib/modules/5.15.167/compat.ko 2>/dev/null || true
insmod /lib/modules/5.15.167/cfg80211.ko 2>/dev/null || true
insmod /lib/modules/5.15.167/mac80211.ko 2>/dev/null || true
insmod /lib/modules/5.15.167/mwlwifi.ko 2>/dev/null || true
insmod /lib/modules/5.15.167/mwl8k.ko 2>/dev/null || true
exit 0
"""
    with open(os.path.join(ROOTFS_DIR, "etc/rc.local"), "w") as f:
        f.write(rc_local)
    os.chmod(os.path.join(ROOTFS_DIR, "etc/rc.local"), 0o755)

    # 7. Repack cpio and compress with LZMA
    print("[*] Repacking cpio archive with fakeroot...")
    new_cpio = os.path.join(WORK_DIR, "new_initramfs.cpio")
    run(f"fakeroot sh -c 'cd {ROOTFS_DIR} && find . -mindepth 1 | cpio -o -H newc > {new_cpio}'")

    print("[*] Compressing new initramfs with LZMA (-9 -e)...")
    run(f"xz --format=lzma -e -9 -f -k {new_cpio}")
    new_lzma = os.path.join(WORK_DIR, "new_initramfs.cpio.lzma")
    new_lzma_size = os.path.getsize(new_lzma)
    print(f"[+] Compressed initramfs size: {new_lzma_size} bytes (limit: {INITRAMFS_MAX_SIZE} bytes)")

    if new_lzma_size > INITRAMFS_MAX_SIZE:
        print(f"[!] FATAL: New initramfs size {new_lzma_size} exceeds maximum allowable {INITRAMFS_MAX_SIZE} bytes!")
        sys.exit(1)

    # 8. Patch vmlinux with new initramfs
    print("[*] Patching vmlinux ELF binary...")
    with open(VMLINUX_BIN, "rb") as f:
        vmlinux_data = bytearray(f.read())

    with open(new_lzma, "rb") as f:
        new_initramfs_bytes = f.read()

    # Clear old initramfs area with zeros
    vmlinux_data[INITRAMFS_OFFSET : INITRAMFS_OFFSET + INITRAMFS_MAX_SIZE] = b"\x00" * INITRAMFS_MAX_SIZE
    # Copy new initramfs
    vmlinux_data[INITRAMFS_OFFSET : INITRAMFS_OFFSET + new_lzma_size] = new_initramfs_bytes

    # Update __initramfs_size (big-endian 32-bit int) at SIZE_FIELD_OFFSET
    vmlinux_data[SIZE_FIELD_OFFSET : SIZE_FIELD_OFFSET + 4] = struct.pack(">I", new_lzma_size)

    with open(NEW_VMLINUX_BIN, "wb") as f:
        f.write(vmlinux_data)
    print(f"[+] Patched vmlinux saved: {NEW_VMLINUX_BIN} ({len(vmlinux_data)} bytes)")

    # 9. Compress new vmlinux with XZ
    print("[*] Compressing new vmlinux with XZ (--check=crc32)...")
    new_vmlinux_xz = os.path.join(WORK_DIR, "vmlinux_mod.xz")
    run(f"rm -f {new_vmlinux_xz} && xz --check=crc32 -9 -e -k {NEW_VMLINUX_BIN}")

    new_xz_size = os.path.getsize(new_vmlinux_xz)
    print(f"[+] Compressed vmlinux.xz size: {new_xz_size} bytes (target: {ORIG_VMLINUX_XZ_SIZE} bytes)")

    if new_xz_size > ORIG_VMLINUX_XZ_SIZE:
        print(f"[!] FATAL: New vmlinux.xz ({new_xz_size}) is larger than original ({ORIG_VMLINUX_XZ_SIZE})!")
        sys.exit(1)

    # Pad vmlinux.xz with null bytes (stream padding) to exactly match ORIG_VMLINUX_XZ_SIZE
    with open(new_vmlinux_xz, "rb") as f:
        compressed_payload = bytearray(f.read())

    padding_needed = ORIG_VMLINUX_XZ_SIZE - len(compressed_payload)
    print(f"[*] Applying {padding_needed} bytes of XZ Stream Padding...")
    compressed_payload.extend(b"\x00" * padding_needed)
    assert len(compressed_payload) == ORIG_VMLINUX_XZ_SIZE

    # 10. Assemble bootwrapper simpleImage with cisco-3702i.dtb
    print("[*] Assembling simpleImage wrapper with Cisco 3702i DTB...")
    with open(RAW_ZIMAGE, "rb") as f:
        wrapper_head = f.read(ORIG_VMLINUX_XZ_OFFSET)

    # Patch DTB in wrapper_head (from offset 0xff40 to 0x14000)
    with open(DTB_FILE, "rb") as f:
        dtb_bytes = f.read()

    dtb_offset = wrapper_head.find(b"\xd0\x0d\xfe\xed")
    assert dtb_offset != -1
    print(f"[*] Found DTB magic in wrapper at offset {hex(dtb_offset)}")

    wrapper_head_mod = bytearray(wrapper_head)
    # Clear DTB area up to 0x14000
    wrapper_head_mod[dtb_offset:ORIG_VMLINUX_XZ_OFFSET] = b"\x00" * (ORIG_VMLINUX_XZ_OFFSET - dtb_offset)
    # Place new DTB
    wrapper_head_mod[dtb_offset : dtb_offset + len(dtb_bytes)] = dtb_bytes

    # Combine wrapper head + compressed payload
    final_raw = bytes(wrapper_head_mod) + bytes(compressed_payload)
    assert len(final_raw) == 7001092

    with open(OUTPUT_RAW, "wb") as f:
        f.write(final_raw)
    print(f"[+] Bootwrapper raw binary created: {OUTPUT_RAW} ({len(final_raw)} bytes)")

    # 11. Pack for Cisco ROMMON (Segment Type 0)
    print("[*] Packaging for Cisco ROMMON (Segment Type 0 MZIP)...")
    mkcisco = os.path.join(PROJECT_DIR, "tools/mkciscoimage.py")
    run(f"python3 {mkcisco} pack-raw {OUTPUT_RAW} {FINAL_CISCO_BIN} 0x01500000")
    print(f"\n[SUCCESS] Final bootable image generated: {FINAL_CISCO_BIN}")
    print(f"Size: {os.path.getsize(FINAL_CISCO_BIN)} bytes")

if __name__ == "__main__":
    main()
