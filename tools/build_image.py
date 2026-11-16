#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
build_image.py - Build a bootable OpenWrt image for the Cisco Aironet 3702i from an
official OpenWrt `enterasys_ws-ap3710i-initramfs-kernel.bin`.

Same technique as repack_openwrt_image.py / repack_openwrt25_image.py (swap the DTB,
rebuild the embedded initramfs in place, pack as MZIP segment type 0), with every
site-specific value on the command line so the result is safe to boot on a production LAN:

  * static IP or DHCP client, never a DHCP server
  * Wi-Fi drivers included on request, radios left disabled until configured
  * the unit's real Ethernet MAC (read from the ROMMON environment) instead of the
    driver's fixed fallback
  * settings saved on the `rootfs_data` JFFS2 overlay (hook taken from repack_openwrt_image.py)
  * sysupgrade blocked (it would overwrite the Cisco flashfs)

This is the script that produced the 25.12.6 image verified in docs/OPENWRT_25_PORT.md:

    python3 tools/build_image.py 25 \\
        --official openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin \\
        --sdk ~/openwrt-sdk-25.12.6-mpc85xx-p1020_gcc-14.3.0_musl.Linux-x86_64 \\
        --ip 192.168.1.5 --gw 192.168.1.1 --wifi --out openwrt-3702i.bin

The kernel modules must already be built against the same SDK (see docs/OPENWRT_25_PORT.md, section 3).
The 23.05.5 constants are included for completeness; only "25" was run with this script.
"""
import argparse
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RELEASES = {
    # load = link address of that release's simpleImage wrapper (tools/wrapper_info.py)
    "25": dict(gcc="gcc-14.3.0", kver="6.12.112", xz_off=0x15000, xz_size=8121652,
               irf_off=0x1044B80, irf_max=4376803, size_off=0x1471468, load=0x01600000, stage_off=0x7D6000),
    "23": dict(gcc="gcc-12.3.0", kver="5.15.167", xz_off=0x14000, xz_size=6919172,
               irf_off=8849408, irf_max=3785548, size_off=0xC0CB50, load=0x01500000, stage_off=0x6B0000),
}

# Per-model differences. "overlay": a JFFS2 rootfs_data partition exists in the device tree.
MODELS = {
    "3702i": dict(dtb="dts/cisco-3702i.dtb", overlay=True, radios=("mwlwifi", "mwl8k")),   # 88W8864 + 88W8764
    "2602i": dict(dtb="dts/cisco-2602i.dtb", overlay=False, radios=("mwl8k",)),            # 2 x 88W8764
}

# Modules of the WS-AP3710i image that this board cannot use; removed to make room.
UNUSED_MODULES = ("ath9k", "ath9k_hw", "ath9k_common", "ath", "usbcore", "ehci-hcd", "ehci-platform",
                  "ehci-fsl", "usb-common", "fsl-mph-dr-of", "ppp_async", "ppp_generic", "pppoe", "pppox", "slhc")
UNUSED_AUTOLOAD = ("ath9k", "usb-common", "usb-core", "usb-ehci", "usb2", "usb2-fsl", "fsl-mph-dr-of",
                   "ppp", "pppoe", "pppox", "slhc")

MAC_HOOK = """# The per-unit Ethernet MAC: on the 3702i it is the MAC_ADDR= record in the manufacturing block
# inside mtd "rommon_env". Without it p1023_fman_im uses the same 02:00:00:37:02:01 on every AP.
# (The 2602i keeps its MAC in an I2C SEEPROM cookie instead - not read yet, see docs/AP2602_NOTES.md.
# Do not scan the whole flash for MAC_ADDR=: free flashfs blocks hold stale copies that disappear.)
cisco_fman_mac() {
\t. /lib/functions.sh
\t. /lib/functions/system.sh
\tlocal mac
\tmac=$(mtd_get_mac_ascii rommon_env MAC_ADDR 2>/dev/null)
\t[ -n "$mac" ] && printf 'marvell\\np1023_fman_im mac_str=%s\\n' "$mac" > /etc/modules.d/20-p1023-fman
}

boot_hook_add preinit_main cisco_fman_mac
"""

NO_SYSUPGRADE = """# Cisco Aironet (ap3g2): OpenWrt lives in one file on the Cisco flashfs that only ROMMON can write.
# A generic sysupgrade would write an Enterasys flash layout over the Cisco filesystem and bootloader.
REQUIRE_IMAGE_METADATA=1

platform_check_image() {
\techo "sysupgrade is not supported on this Cisco AP - reinstall through ROMMON (docs/QUICK_INSTALL.md)" >&2
\treturn 1
}

platform_do_upgrade() {
\techo "sysupgrade is not supported on this Cisco AP" >&2
\treturn 1
}
"""


def run(*argv: str) -> None:
    print("[CMD]", " ".join(shlex.quote(a) for a in argv))
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"[!] command failed\n{res.stdout}\n{res.stderr}")


def write(path: str, text: str, mode: int = 0o644) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    os.chmod(path, mode)


def network_config(args: argparse.Namespace) -> str:
    head = ("config interface 'loopback'\n\toption device 'lo'\n\toption proto 'static'\n"
            "\toption ipaddr '127.0.0.1'\n\toption netmask '255.0.0.0'\n\n"
            "config device\n\toption name 'br-lan'\n\toption type 'bridge'\n\tlist ports 'eth0'\n\n"
            "config interface 'lan'\n\toption device 'br-lan'\n")
    if args.dhcp:
        return head + "\toption proto 'dhcp'\n"
    return head + (f"\toption proto 'static'\n\toption ipaddr '{args.ip}'\n\toption netmask '{args.mask}'\n"
                   f"\toption gateway '{args.gw}'\n\toption dns '{args.dns or args.gw}'\n")


DHCP_CONFIG = """config dnsmasq
\toption domainneeded '1'
\toption localise_queries '1'
\toption rebind_protection '1'
\toption local '/lan/'
\toption domain 'lan'
\toption leasefile '/tmp/dhcp.leases'
\toption resolvfile '/tmp/resolv.conf.d/resolv.conf.auto'
\toption localservice '1'

config dhcp 'lan'
\toption interface 'lan'
\toption ignore '1'

config odhcpd 'odhcpd'
\toption maindhcp '0'
\toption leasefile '/tmp/hosts/odhcpd'
\toption leasetrigger '/usr/sbin/odhcpd-update'
\toption loglevel '4'
"""


def rc_local(kver: str, radios: tuple) -> str:
    text = f"""# Cisco Aironet: fall back to loading the Ethernet driver by hand if kmodloader did not.
[ -d /sys/class/net/eth0 ] || {{
    insmod /lib/modules/{kver}/marvell.ko 2>/dev/null
    insmod /lib/modules/{kver}/p1023_fman_im.ko 2>/dev/null
    /etc/init.d/network reload
}}
"""
    if radios:
        drivers = "".join(f"insmod /lib/modules/{kver}/{name}.ko 2>/dev/null\n" for name in radios)
        text += f"""
# Release the radios from reset, then load their drivers (interfaces stay disabled until configured).
modprobe mac80211 2>/dev/null
insmod /lib/modules/{kver}/radio_reset.ko 2>/dev/null
for d in 9000:01:00.0 a000:03:00.0; do
    [ -e /sys/bus/pci/devices/$d/enable ] && echo 1 > /sys/bus/pci/devices/$d/enable
done
{drivers}
# Some units report an invalid radio address (2602i: multicast bit set, hostapd cannot start on it).
# Give those interfaces a locally administered address derived from the Ethernet MAC.
(
    sleep 25
    wifi config
    mid=$(cut -d: -f2-5 /sys/class/net/eth0/address)
    n=0
    for m in /sys/class/ieee80211/phy*/macaddress; do
        [ -e "$m" ] || continue
        o=$(cut -c1-2 "$m")
        [ $((0x$o & 1)) -eq 1 ] && uci -q set wireless.default_radio$n.macaddr="06:$mid:0$n"
        n=$((n + 1))
    done
    uci -q commit wireless
) &
"""
    return text + "\nexit 0\n"


def build_smp_stage(sdk: str, gcc: str, work: str, stage_addr: int, wrapper_entry: int) -> bytes:
    """Assemble boot/smp: the pre-kernel stage (L2 cache on, core 1 released into a spin table)."""
    cross = os.path.join(sdk, f"staging_dir/toolchain-powerpc_8548_{gcc}_musl/bin/powerpc-openwrt-linux-musl-")
    src = os.path.join(REPO, "boot/smp")
    out = os.path.join(work, "smp")
    os.makedirs(out, exist_ok=True)
    run(cross + "gcc", "-c", "-mcpu=8548", "-o", os.path.join(out, "bootpg.o"), os.path.join(src, "bootpg.S"))
    run(cross + "ld", "-Ttext=0xfffff000", "-o", os.path.join(out, "bootpg.elf"), os.path.join(out, "bootpg.o"))
    run(cross + "objcopy", "-O", "binary", os.path.join(out, "bootpg.elf"), os.path.join(out, "bootpg.bin"))
    if os.path.getsize(os.path.join(out, "bootpg.bin")) != 4096:
        sys.exit("[!] secondary boot page is not exactly 4096 bytes")
    run(cross + "gcc", "-c", "-mcpu=8548", f"-DWRAPPER_ENTRY={wrapper_entry:#x}", f"-Wa,-I{out}",
        "-o", os.path.join(out, "shim.o"), os.path.join(src, "shim.S"))
    run(cross + "ld", f"-Ttext={stage_addr:#x}", "-o", os.path.join(out, "shim.elf"), os.path.join(out, "shim.o"))
    run(cross + "objcopy", "-O", "binary", os.path.join(out, "shim.elf"), os.path.join(out, "shim.bin"))
    with open(os.path.join(out, "shim.bin"), "rb") as f:
        return f.read()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("release", choices=sorted(RELEASES))
    ap.add_argument("--official", required=True, help="official enterasys_ws-ap3710i initramfs-kernel.bin")
    ap.add_argument("--sdk", required=True, help="extracted OpenWrt SDK for the same release")
    net = ap.add_mutually_exclusive_group(required=True)
    net.add_argument("--ip", help="static management address")
    net.add_argument("--dhcp", action="store_true", help="DHCP client on br-lan instead of a static address")
    ap.add_argument("--mask", default="255.255.255.0")
    ap.add_argument("--gw")
    ap.add_argument("--dns")
    ap.add_argument("--hostname", help="default: cisco<model>")
    ap.add_argument("--model", choices=sorted(MODELS), default="3702i")
    ap.add_argument("--wifi", action="store_true", help="include the model's Wi-Fi drivers and firmware")
    ap.add_argument("--no-overlay", action="store_true", help="plain initramfs, nothing saved to flash")
    ap.add_argument("--no-smp", action="store_true",
                    help="leave out the pre-kernel stage (boot/smp): one CPU core, L2 cache enabled later by l2cache.ko")
    ap.add_argument("--pubkey", help="install this SSH public key and disable password logins")
    ap.add_argument("--drivers", default=os.path.join(REPO, "driver"), help="directory with the built .ko files")
    ap.add_argument("--fw-8864", default="/tmp/mwl_fw/lib/firmware/mwlwifi/88W8864.bin")
    ap.add_argument("--fw-8764", default=os.path.join(REPO, "firmware_dl/fmimage_8764_ap-1.fw"))
    ap.add_argument("--dtb", help="default: the model's tree in dts/")
    ap.add_argument("--work", default="/tmp/cisco3702i_build")
    ap.add_argument("--out", default=os.path.expanduser("~/Downloads/openwrt-3702i.bin"))
    args = ap.parse_args()
    if args.ip and not args.gw:
        ap.error("--ip needs --gw")

    c = RELEASES[args.release]
    model = MODELS[args.model]
    args.dtb = args.dtb or os.path.join(REPO, model["dtb"])
    args.hostname = args.hostname or f"cisco{args.model}"
    radios = model["radios"] if args.wifi else ()
    overlay = model["overlay"] and not args.no_overlay
    kver, work = c["kver"], args.work
    rootfs = os.path.join(work, "rootfs")
    state = os.path.join(work, "fakeroot.state")
    sdk = os.path.abspath(os.path.expanduser(args.sdk))
    os.environ["STAGING_DIR"] = os.path.join(sdk, "staging_dir")
    strip = os.path.join(sdk, f"staging_dir/toolchain-powerpc_8548_{c['gcc']}_musl/bin/powerpc-openwrt-linux-musl-strip")
    kdir = os.path.join(sdk, f"build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-{kver}")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(rootfs)

    with open(args.official, "rb") as f:
        raw = f.read()
    if struct.unpack(">I", raw[:4])[0] == 0x27051956:
        raw = raw[64:]  # drop the uImage header
    if len(raw) != c["xz_off"] + c["xz_size"]:
        sys.exit("[!] this is not the official image the offsets were taken from")

    # 1. vmlinux out of the wrapper, initramfs out of vmlinux
    with open(os.path.join(work, "vmlinux.xz"), "wb") as f:
        f.write(raw[c["xz_off"]:])
    run("xz", "-d", "-f", "-k", os.path.join(work, "vmlinux.xz"))
    with open(os.path.join(work, "vmlinux"), "rb") as f:
        vmlinux = bytearray(f.read())
    if struct.unpack(">I", vmlinux[c["size_off"]:c["size_off"] + 4])[0] != c["irf_max"]:
        sys.exit("[!] __initramfs_size is not where it is expected")
    with open(os.path.join(work, "initramfs.cpio.lzma"), "wb") as f:
        f.write(vmlinux[c["irf_off"]:c["irf_off"] + c["irf_max"]])
    run("xz", "-d", "-f", "--format=lzma", os.path.join(work, "initramfs.cpio.lzma"))
    cpio_in = shlex.quote(os.path.join(work, "initramfs.cpio"))
    run("fakeroot", "-s", state, "sh", "-c", f"cd {shlex.quote(rootfs)} && cpio -idm --quiet < {cpio_in}")

    # 2. kernel modules and firmware
    moddir = os.path.join(rootfs, "lib/modules", kver)
    for name in UNUSED_MODULES:
        if os.path.exists(os.path.join(moddir, name + ".ko")):
            os.remove(os.path.join(moddir, name + ".ko"))
    autoload = os.path.join(rootfs, "etc/modules.d")
    for entry in os.listdir(autoload):
        if entry.split("-", 1)[-1] in UNUSED_AUTOLOAD or entry in UNUSED_AUTOLOAD:
            os.remove(os.path.join(autoload, entry))
    modules = [os.path.join(kdir, "drivers/net/phy/marvell.ko"), os.path.join(args.drivers, "l2cache.ko"),
               os.path.join(args.drivers, "p1023_fman_im.ko"), os.path.join(args.drivers, "radio_reset.ko")]
    modules += [os.path.join(args.drivers, f"{name}/{name}.ko") for name in radios]
    for name, src in (("mwlwifi", args.fw_8864), ("mwl8k", args.fw_8764)):
        if name in radios:
            os.makedirs(os.path.join(rootfs, "lib/firmware", name), exist_ok=True)
            shutil.copy(src, os.path.join(rootfs, "lib/firmware", name))
    for module in modules:
        run(strip, "--strip-unneeded", "-o", os.path.join(moddir, os.path.basename(module)), module)
    write(os.path.join(autoload, "20-p1023-fman"), "marvell\np1023_fman_im\n")
    # The L2 cache is off after ROMMON (U-Boot would have enabled it): switch it on as early as possible.
    write(os.path.join(rootfs, "etc/modules-boot.d/00-l2cache"), "l2cache enable=1\n")

    # 3. configuration
    write(os.path.join(rootfs, "etc/config/network"), network_config(args))
    write(os.path.join(rootfs, "etc/config/dhcp"), DHCP_CONFIG)
    write(os.path.join(rootfs, "etc/config/system"),
          f"config system\n\toption hostname '{args.hostname}'\n\toption timezone 'UTC'\n\toption ttylogin '0'\n"
          "\toption log_size '128'\n\nconfig timeserver 'ntp'\n\toption enabled '1'\n"
          "\tlist server '0.openwrt.pool.ntp.org'\n\tlist server '1.openwrt.pool.ntp.org'\n")
    if args.pubkey:
        shutil.copy(os.path.expanduser(args.pubkey), os.path.join(rootfs, "etc/dropbear/authorized_keys"))
        os.chmod(os.path.join(rootfs, "etc/dropbear/authorized_keys"), 0o600)
        write(os.path.join(rootfs, "etc/config/dropbear"),
              "config dropbear\n\toption PasswordAuth 'off'\n\toption RootPasswordAuth 'off'\n\toption Port '22'\n")
    write(os.path.join(rootfs, "etc/rc.local"), rc_local(kver, radios), 0o755)

    # 4. board quirks
    write(os.path.join(rootfs, "lib/preinit/09_cisco_fman_mac"), MAC_HOOK)
    write(os.path.join(rootfs, "lib/upgrade/platform.sh"), NO_SYSUPGRADE)
    banner = os.path.join(rootfs, "usr/share/ucode/luci/template/themes/bootstrap/header.ut")
    if os.path.exists(banner):  # LuCI's "recovery (initramfs) mode" warning points at sysupgrade
        with open(banner, encoding="utf-8") as f:
            text = f.read()
        write(banner, text.replace('boardinfo.rootfs_type == "initramfs"', "false"))
    if overlay:
        with open(os.path.join(REPO, "tools/repack_openwrt_image.py"), encoding="utf-8") as f:
            hook = re.search(r'preinit_overlay = """(.*?)"""', f.read(), re.S)
        if hook is None:
            sys.exit("[!] could not find the overlay preinit hook in repack_openwrt_image.py")
        write(os.path.join(rootfs, "lib/preinit/75_mount_overlay"), hook.group(1))

    # 5. initramfs -> vmlinux -> wrapper -> MZIP
    cpio_out = os.path.join(work, "new.cpio")
    run("fakeroot", "-i", state, "sh", "-c",
        f"cd {shlex.quote(rootfs)} && find . -mindepth 1 | cpio -o -H newc --quiet > {shlex.quote(cpio_out)}")
    run("xz", "--format=lzma", "-e", "-9", "-f", "-k", cpio_out)
    with open(cpio_out + ".lzma", "rb") as f:
        initramfs = f.read()
    print(f"[+] initramfs  {len(initramfs)} / {c['irf_max']} bytes")
    if len(initramfs) > c["irf_max"]:
        sys.exit("[!] initramfs does not fit in the original slot")
    vmlinux[c["irf_off"]:c["irf_off"] + c["irf_max"]] = initramfs.ljust(c["irf_max"], b"\0")
    vmlinux[c["size_off"]:c["size_off"] + 4] = struct.pack(">I", len(initramfs))
    with open(os.path.join(work, "vmlinux_mod"), "wb") as f:
        f.write(vmlinux)
    run("xz", "--check=crc32", "-9", "-e", "-f", "-k", os.path.join(work, "vmlinux_mod"))
    with open(os.path.join(work, "vmlinux_mod.xz"), "rb") as f:
        payload = f.read()
    print(f"[+] vmlinux.xz {len(payload)} / {c['xz_size']} bytes")
    if len(payload) > c["xz_size"]:
        sys.exit("[!] vmlinux.xz does not fit in the original slot")
    head = bytearray(raw[:c["xz_off"]])
    with open(args.dtb, "rb") as f:
        dtb = f.read()
    dtb_off = head.find(b"\xd0\x0d\xfe\xed")
    if dtb_off < 0 or dtb_off + len(dtb) > c["xz_off"]:
        sys.exit("[!] DTB does not fit in the wrapper's DTB slot")
    head[dtb_off:] = dtb.ljust(c["xz_off"] - dtb_off, b"\0")
    image = bytearray(bytes(head) + payload.ljust(c["xz_size"], b"\0"))
    if not args.no_smp:
        # The wrapper starts with "b +0x340". Redirect that branch to a stage appended behind the
        # wrapper's BSS (zero padding keeps the BSS zero); the stage ends by jumping to +0x340 itself.
        if struct.unpack(">I", image[:4])[0] != 0x48000340 or len(image) > c["stage_off"] - 0x1400:
            sys.exit("[!] unexpected wrapper layout, cannot attach the SMP stage (use --no-smp)")
        stage = build_smp_stage(sdk, c["gcc"], work, c["load"] + c["stage_off"], c["load"] + 0x340)
        image[:4] = struct.pack(">I", 0x48000000 | c["stage_off"])
        image = image.ljust(c["stage_off"], b"\0") + stage
        print(f"[+] SMP stage  {len(stage)} bytes at {c['load'] + c['stage_off']:#010x}")
    with open(os.path.join(work, "raw.bin"), "wb") as f:
        f.write(image)
    run(sys.executable, os.path.join(REPO, "tools/mkciscoimage.py"), "pack-raw",
        os.path.join(work, "raw.bin"), args.out, f"{c['load']:#010x}")
    print(f"[OK] {args.out}: {os.path.getsize(args.out)} bytes, load/entry {c['load']:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
