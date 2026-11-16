# Prebuilt OpenWrt image for the Cisco Aironet 3702i

This branch only carries binaries. The sources, tools and documentation are on the code branches
of this repository — start with `docs/QUICK_INSTALL.md`.

| File | |
|---|---|
| `openwrt-25.12.6-cisco-3702i.bin` | OpenWrt 25.12.6 (Linux 6.12.112) for `AIR-CAP3702I`, 8 221 579 bytes |
| `SHA256SUMS` | checksum of the image |
| `licenses/` | licence terms of the two Marvell radio firmware files contained in the image |

## Install

```bash
git fetch origin images
git show origin/images:openwrt-25.12.6-cisco-3702i.bin > openwrt-25.12.6-cisco-3702i.bin
git show origin/images:SHA256SUMS | sha256sum -c -
python3 tools/xmodem_boot.py openwrt-25.12.6-cisco-3702i.bin --install --wait-rommon
```

Console cable (9600 8N1) and PoE are all that is needed; see `docs/QUICK_INSTALL.md`.

> Never use LuCI's "Flash new firmware" page or `sysupgrade` on this AP: OpenWrt is one file on
> Cisco's flash file system and a generic upgrade would overwrite it and the bootloader area.
> The image refuses to do it.

## What is in it

* Built with `tools/build_image.py 25 --model 3702i --wifi --dhcp` at commit `10e1f51` of the
  `fix/openwrt25-load-address` branch, from the official
  `openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin` (kernel and user space
  unchanged) with the device tree and kernel modules of this repository.
* Both CPU cores, L2 cache enabled, gigabit Ethernet with the unit's own MAC address.
* `mwlwifi` (5 GHz, 88W8864) and `mwl8k` (2.4 GHz, 88W8764) with their firmware; both radios are
  **disabled** until configured.
* Defaults like stock OpenWrt: DHCP client on the LAN bridge, hostname `cisco3702i`, user `root`
  without a password (set one at first login), no DHCP server.
* Settings are saved on a 4 MiB flash area and survive reboots.

## How it was tested

Installed on one `AIR-CAP3702I-E-K9` whose flash had been emptied (Cisco software removed, settings
area erased): boots from `flash:` with `smp: Brought up 1 node, 2 CPUs`, answers on the network
80 s after an unattended power cycle, generates a wireless configuration with `radio0` = 5 GHz and
`radio1` = 2.4 GHz. Earlier builds of the same tree were measured at 80–120 Mbit/s on 5 GHz
(CPU-limited). Not tested: long uptimes, 2.4 GHz with clients, other hardware revisions.

## Licences

OpenWrt and the Linux kernel are GPL-2.0; the corresponding source is OpenWrt release v25.12.6
plus the device tree, drivers and build script in this repository. The two radio firmware files
are redistributed unmodified under the Marvell terms in `licenses/`.
