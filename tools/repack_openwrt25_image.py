#!/usr/bin/env python3
"""
repack_openwrt25_image.py - OpenWrt 25.12.6 (Linux 6.12.112) initramfs & kernel repacker for Cisco 3702i.
License: GNU AGPLv3.
"""

import argparse
import os
import sys
import struct
import shutil
import subprocess

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK25 = "/tmp/owrt25/openwrt-sdk-25.12.6-mpc85xx-p1020_gcc-14.3.0_musl.Linux-x86_64"
KDIR25 = os.path.join(SDK25, "build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-6.12.112")
BP25 = os.path.join(SDK25, "build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/mac80211-smallbuffers/backports-6.18.55")
CROSS25 = os.path.join(SDK25, "staging_dir/toolchain-powerpc_8548_gcc-14.3.0_musl/bin/powerpc-openwrt-linux-musl-")
STRIP_TOOL = CROSS25 + "strip"

RAW_ZIMAGE = "/tmp/owrt25/raw25.bin"
DTB_FILE = os.path.join(REPO_DIR, "dts/cisco-3702i.dtb")
FMAN_KO = os.path.join(REPO_DIR, "driver/p1023_fman_im.ko")
RADIO_RESET_KO = os.path.join(REPO_DIR, "driver/radio_reset.ko")
MWLWIFI_KO = os.path.join(REPO_DIR, "driver/mwlwifi/mwlwifi.ko")
MWL8K_KO = os.path.join(REPO_DIR, "driver/mwl8k/mwl8k.ko")
MARVELL_KO = "/tmp/owrt25/marvell.ko"
FW_8864 = "/tmp/mwl_fw/lib/firmware/mwlwifi/88W8864.bin"
FW_8764 = os.path.join(REPO_DIR, "firmware_dl/fmimage_8764_ap-1.fw")

WORK_DIR = "/tmp/owrt25_repack"
ROOTFS_DIR = os.path.join(WORK_DIR, "rootfs")
VMLINUX_BIN = os.path.join(WORK_DIR, "vmlinux")
NEW_VMLINUX_BIN = os.path.join(WORK_DIR, "vmlinux_mod")
OUTPUT_RAW = os.path.join(WORK_DIR, "openwrt-cisco-raw.bin")
FINAL_CISCO_BIN = os.path.expanduser("~/Downloads/openwrt-3702i.bin")

# Constants from OpenWrt 25.12.6 (ws-ap3710i-initramfs-kernel.bin)
ORIG_VMLINUX_XZ_OFFSET = 0x15000      # 86,016
ORIG_VMLINUX_XZ_SIZE   = 8121652      # 8,207,668 - 86,016
INITRAMFS_OFFSET       = 0x1044b80    # 17,058,688 in vmlinux
INITRAMFS_MAX_SIZE     = 4376803      # Original initramfs.lzma size
SIZE_FIELD_OFFSET      = 0x1471468    # File offset of __initramfs_size in vmlinux
KVER = "6.12.112"


def run(cmd, cwd=None):
    print(f"[CMD] {cmd}")
    res = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[!] Error executing: {cmd}")
        print(f"Stdout:\n{res.stdout}")
        print(f"Stderr:\n{res.stderr}")
        sys.exit(1)
    return res.stdout


def parse_args():
    parser = argparse.ArgumentParser(description="Repack OpenWrt 25.12.6 image for Cisco Aironet 3702i.")
    parser.add_argument(
        "--dev-unsafe",
        action="store_true",
        default=os.environ.get("OPENWRT_DEV_UNSAFE") == "1",
        help="Enable passwordless root SSH for isolated bench testing.",
    )
    parser.add_argument(
        "--ssh-pubkey",
        default=os.environ.get("OPENWRT_SSH_PUBKEY", ""),
        help="Path to an SSH public key to install into /etc/dropbear/authorized_keys.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    print("=" * 70)
    print("OPENWRT 25.12.6 IMAGE REPACKER FOR CISCO AIRONET 3702i")
    print("=" * 70)

    os.makedirs(WORK_DIR, exist_ok=True)

    # 1. Strip marvell.ko
    src_marvell = os.path.join(KDIR25, "drivers/net/phy/marvell.ko")
    run(f"{STRIP_TOOL} --strip-unneeded -o {MARVELL_KO} {src_marvell}")
    print(f"[+] Marvell PHY driver: {MARVELL_KO} ({os.path.getsize(MARVELL_KO)} bytes)")

    # 2. Build & strip drivers against Linux 6.12.112 + backports-6.18.55
    staging = os.path.join(SDK25, "staging_dir")
    run(f"STAGING_DIR={staging} make -C driver KDIR={KDIR25} CROSS={CROSS25} -j$(nproc)", cwd=REPO_DIR)
    run(f"STAGING_DIR={staging} make -C driver/mwlwifi KDIR={KDIR25} CROSS={CROSS25} BP_DIR={BP25} -j$(nproc)", cwd=REPO_DIR)
    run(f"STAGING_DIR={staging} make -C driver/mwl8k KDIR={KDIR25} CROSS={CROSS25} BP_DIR={BP25} -j$(nproc)", cwd=REPO_DIR)
    run(f"{STRIP_TOOL} --strip-unneeded {RADIO_RESET_KO} {MWLWIFI_KO} {MWL8K_KO}")

    # 3. Extract vmlinux from raw zImage
    with open(RAW_ZIMAGE, "rb") as f:
        f.seek(ORIG_VMLINUX_XZ_OFFSET)
        xz_data = f.read(ORIG_VMLINUX_XZ_SIZE)

    vmlinux_xz = os.path.join(WORK_DIR, "vmlinux.xz")
    with open(vmlinux_xz, "wb") as f:
        f.write(xz_data)
    run(f"xz -d -f -k {vmlinux_xz}")
    print(f"[+] Decompressed {VMLINUX_BIN} ({os.path.getsize(VMLINUX_BIN)} bytes)")

    # 4. Extract initramfs from vmlinux
    with open(VMLINUX_BIN, "rb") as f:
        f.seek(INITRAMFS_OFFSET)
        lzma_data = f.read(INITRAMFS_MAX_SIZE)

    initramfs_lzma = os.path.join(WORK_DIR, "initramfs.cpio.lzma")
    with open(initramfs_lzma, "wb") as f:
        f.write(lzma_data)
    initramfs_cpio = os.path.join(WORK_DIR, "initramfs.cpio")
    run(f"xz -d -f --format=lzma {initramfs_lzma}")

    # 5. Unpack cpio using fakeroot
    if os.path.exists(ROOTFS_DIR):
        shutil.rmtree(ROOTFS_DIR)
    os.makedirs(ROOTFS_DIR, exist_ok=True)
    run(f"fakeroot sh -c 'cd {ROOTFS_DIR} && cpio -idm < {initramfs_cpio}'")

    # 6. Inject drivers, firmware & config
    mod_dir = os.path.join(ROOTFS_DIR, f"lib/modules/{KVER}")
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

    net_cfg = """config interface 'loopback'
\toption device 'lo'
\toption proto 'static'
\toption ipaddr '127.0.0.1'
\toption netmask '255.0.0.0'

config device
\toption name 'br-lan'
\toption type 'bridge'
\tlist ports 'eth0'

config interface 'lan'
\toption device 'br-lan'
\toption proto 'static'
\toption ipaddr '192.168.1.50'
\toption netmask '255.255.255.0'
\toption gateway '192.168.1.1'
\toption dns '192.168.1.1'
"""
    with open(os.path.join(ROOTFS_DIR, "etc/config/network"), "w") as f:
        f.write(net_cfg)

    dhcp_cfg = """config dnsmasq
\toption domainneeded '1'
\toption boguspriv '1'
\toption filterwin2k '0'
\toption localise_queries '1'
\toption rebind_protection '1'
\toption rebind_localhost '1'
\toption local '/lan/'
\toption domain 'lan'
\toption expandhosts '1'
\toption nonegcache '0'
\toption authoritative '1'
\toption readethers '1'
\toption leasefile '/tmp/dhcp.leases'
\toption resolvfile '/tmp/resolv.conf.d/resolv.conf.auto'
\toption localservice '1'

config dhcp 'lan'
\toption interface 'lan'
\toption start '100'
\toption limit '150'
\toption leasetime '12h'
\toption ignore '1'

config odhcpd 'odhcpd'
\toption maindhcp '0'
\toption leasefile '/tmp/hosts/odhcpd'
\toption leasetrigger '/usr/sbin/odhcpd-update'
\toption loglevel '4'
"""
    with open(os.path.join(ROOTFS_DIR, "etc/config/dhcp"), "w") as f:
        f.write(dhcp_cfg)

    if args.dev_unsafe:
        print("[!] WARNING: --dev-unsafe enabled: passwordless root SSH configured.")
        with open(os.path.join(ROOTFS_DIR, "etc/shadow"), "w") as f:
            f.write("root:::0:99999:7:::\ndaemon:*:0:0:99999:7:::\nftp:*:0:0:99999:7:::\nnetwork:*:0:0:99999:7:::\nnobody:*:0:0:99999:7:::\n")
    if args.ssh_pubkey and os.path.isfile(args.ssh_pubkey):
        dropbear_dir = os.path.join(ROOTFS_DIR, "etc/dropbear")
        os.makedirs(dropbear_dir, exist_ok=True)
        auth_keys = os.path.join(dropbear_dir, "authorized_keys")
        shutil.copy2(args.ssh_pubkey, auth_keys)
        os.chmod(auth_keys, 0o600)

    rc_local = f"""# Put your custom commands here that should be executed once
# the system init finished.

insmod /lib/modules/{KVER}/marvell.ko 2>/dev/null || true
insmod /lib/modules/{KVER}/p1023_fman_im.ko 2>/dev/null || true
ip link set eth0 up 2>/dev/null || true
ip link set br-lan up 2>/dev/null || true
ip addr add 192.168.1.50/24 dev br-lan 2>/dev/null || true
ip route add default via 192.168.1.1 dev br-lan 2>/dev/null || true
/etc/init.d/dropbear restart 2>/dev/null || true
/etc/init.d/uhttpd restart 2>/dev/null || true

insmod /lib/modules/{KVER}/radio_reset.ko 2>/dev/null || true
for d in 9000:01:00.0 a000:03:00.0; do
    [ -e /sys/bus/pci/devices/$d/enable ] && echo 1 > /sys/bus/pci/devices/$d/enable
done
insmod /lib/modules/{KVER}/compat.ko 2>/dev/null || true
insmod /lib/modules/{KVER}/cfg80211.ko 2>/dev/null || true
insmod /lib/modules/{KVER}/mac80211.ko 2>/dev/null || true
insmod /lib/modules/{KVER}/mwlwifi.ko 2>/dev/null || true
insmod /lib/modules/{KVER}/mwl8k.ko 2>/dev/null || true
exit 0
"""
    with open(os.path.join(ROOTFS_DIR, "etc/rc.local"), "w") as f:
        f.write(rc_local)
    os.chmod(os.path.join(ROOTFS_DIR, "etc/rc.local"), 0o755)

    # 7. Repack cpio and compress with LZMA
    new_cpio = os.path.join(WORK_DIR, "new_initramfs.cpio")
    run(f"fakeroot sh -c 'cd {ROOTFS_DIR} && find . -mindepth 1 | cpio -o -H newc > {new_cpio}'")
    run(f"xz --format=lzma -e -9 -f -k {new_cpio}")
    new_lzma = os.path.join(WORK_DIR, "new_initramfs.cpio.lzma")
    new_lzma_size = os.path.getsize(new_lzma)
    print(f"[+] Compressed initramfs size: {new_lzma_size} bytes (limit: {INITRAMFS_MAX_SIZE} bytes)")
    if new_lzma_size > INITRAMFS_MAX_SIZE:
        print(f"[!] FATAL: Exceeds {INITRAMFS_MAX_SIZE} bytes!")
        sys.exit(1)

    # 8. Patch vmlinux
    with open(VMLINUX_BIN, "rb") as f:
        vmlinux_data = bytearray(f.read())
    with open(new_lzma, "rb") as f:
        new_initramfs_bytes = f.read()

    vmlinux_data[INITRAMFS_OFFSET : INITRAMFS_OFFSET + INITRAMFS_MAX_SIZE] = b"\x00" * INITRAMFS_MAX_SIZE
    vmlinux_data[INITRAMFS_OFFSET : INITRAMFS_OFFSET + new_lzma_size] = new_initramfs_bytes
    vmlinux_data[SIZE_FIELD_OFFSET : SIZE_FIELD_OFFSET + 4] = struct.pack(">I", new_lzma_size)

    with open(NEW_VMLINUX_BIN, "wb") as f:
        f.write(vmlinux_data)

    # 9. Compress vmlinux with XZ
    new_vmlinux_xz = os.path.join(WORK_DIR, "vmlinux_mod.xz")
    run(f"rm -f {new_vmlinux_xz} && xz --check=crc32 -9 -e -k {NEW_VMLINUX_BIN}")
    new_xz_size = os.path.getsize(new_vmlinux_xz)
    print(f"[+] Compressed vmlinux.xz size: {new_xz_size} bytes (target: {ORIG_VMLINUX_XZ_SIZE} bytes)")
    if new_xz_size > ORIG_VMLINUX_XZ_SIZE:
        print(f"[!] FATAL: vmlinux.xz ({new_xz_size}) > original ({ORIG_VMLINUX_XZ_SIZE})!")
        sys.exit(1)

    with open(new_vmlinux_xz, "rb") as f:
        compressed_payload = bytearray(f.read())
    padding_needed = ORIG_VMLINUX_XZ_SIZE - len(compressed_payload)
    compressed_payload.extend(b"\x00" * padding_needed)

    # 10. Assemble simpleImage wrapper with Cisco 3702i DTB
    with open(RAW_ZIMAGE, "rb") as f:
        wrapper_head = f.read(ORIG_VMLINUX_XZ_OFFSET)
    with open(DTB_FILE, "rb") as f:
        dtb_bytes = f.read()

    dtb_offset = wrapper_head.find(b"\xd0\x0d\xfe\xed")
    assert dtb_offset != -1
    assert dtb_offset + len(dtb_bytes) <= ORIG_VMLINUX_XZ_OFFSET

    wrapper_head_mod = bytearray(wrapper_head)
    wrapper_head_mod[dtb_offset:ORIG_VMLINUX_XZ_OFFSET] = b"\x00" * (ORIG_VMLINUX_XZ_OFFSET - dtb_offset)
    wrapper_head_mod[dtb_offset : dtb_offset + len(dtb_bytes)] = dtb_bytes

    final_raw = bytes(wrapper_head_mod) + bytes(compressed_payload)
    with open(OUTPUT_RAW, "wb") as f:
        f.write(final_raw)

    # 11. Pack for Cisco ROMMON (Segment Type 0)
    # Load at 0x02000000 (32 MiB) so decompressing vmlinux (~22.4 MiB) to 0x00000000
    # does not overwrite the running simpleImage wrapper.
    mkcisco = os.path.join(REPO_DIR, "tools/mkciscoimage.py")
    run(f"python3 {mkcisco} pack-raw {OUTPUT_RAW} {FINAL_CISCO_BIN} 0x02000000")
    print(f"\n[SUCCESS] OpenWrt 25.12.6 image generated: {FINAL_CISCO_BIN} ({os.path.getsize(FINAL_CISCO_BIN)} bytes)")


if __name__ == "__main__":
    main()
