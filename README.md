# Prebuilt OpenWrt images for the Cisco Aironet 3702i and 2602i

This branch only carries binaries. The sources, tools and documentation are on the code branches
of this repository — start with `docs/QUICK_INSTALL.md`.

| File | |
|---|---|
| `openwrt-25.12.6-cisco-3702i.bin` | OpenWrt 25.12.6 (Linux 6.12.112) for `AIR-CAP3702I`, 8 221 579 bytes |
| `openwrt-25.12.6-cisco-2602i.bin` | the same for `AIR-CAP2602I`, 8 221 579 bytes — **read the 2602i section first** |
| `SHA256SUMS` | checksums of both images |
| `licenses/` | licence terms of the two Marvell radio firmware files contained in the images |

Both are generic, like a stock OpenWrt image: DHCP client on the LAN bridge, user `root` without a
password (set one at first login), no SSH key, no DHCP server, radios **disabled** until configured.

## Install

```bash
git fetch origin images
git show origin/images:openwrt-25.12.6-cisco-3702i.bin > openwrt-25.12.6-cisco-3702i.bin
git show origin/images:SHA256SUMS | sha256sum -c --ignore-missing -
python3 tools/xmodem_boot.py openwrt-25.12.6-cisco-3702i.bin --install --wait-rommon
```

Console cable (9600 8N1) and PoE are all that is needed; see `docs/QUICK_INSTALL.md`. The copy
takes about 14 minutes. For the 2602i use the other file name.

> Never use LuCI's "Flash new firmware" page or `sysupgrade` on these APs: OpenWrt is one file on
> Cisco's flash file system and a generic upgrade would overwrite it and the bootloader area.
> The images refuse to do it.

## What is in them

* Built with `tools/build_image.py 25 --model <model> --wifi --dhcp` at commit `3f14c8e` of the
  `merge/ld-branch` branch (the 25.12.6 port merged with `ld-branch`), from the official
  `openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin` (kernel and user space
  unchanged) with the device trees and kernel modules of this repository.
* Both CPU cores, L2 cache enabled, gigabit Ethernet (zero-copy FMan driver from `ld-branch`).
* 3702i: `mwlwifi` (5 GHz, 88W8864, with the big-endian VHT rate fix) and `mwl8k` (2.4 GHz, 88W8764).
  2602i: `mwl8k` on both radios (2 × 88W8764, 802.11n).
* 3702i only: settings are saved on a 4 MiB flash area and survive reboots.

## 2602i: nothing is saved

On the 2602i **every setting is lost at reboot** — address, password, Wi-Fi configuration.
What is known: the 32 MiB of flash that Linux sees are completely occupied by Cisco's flash file
system (which holds the OpenWrt image itself), so there is no free region for a settings area like
the one used on the 3702i. The bootloader reports a second, empty 12 MiB file system
(`flashfs[1]`) that has not been located from Linux yet. **Why exactly, and where that second
region is, still has to be investigated** (`docs/AP2602_NOTES.md`, section 3).

Consequences:

* This generic image is good for trying the hardware. After every power cycle it is a fresh
  OpenWrt with the radios off.
* For real use, build your own image with the settings inside:
  `tools/build_image.py 25 --model 2602i --wifi --dhcp --pubkey … --ap-ssid … --ap-key-file …
  --ap-country …` (`docs/AP2602_NOTES.md`, section 2.7). The access point then starts by itself.
* Ethernet MAC: after an unattended power-on the unit's own address was used (seen on one unit).
  If yours comes up as `02:00:00:37:02:01`, pass the address the bootloader prints
  (`Base Ethernet MAC address`) to the builder with `--mac`.

## How they were tested

These two files are rebuilds for publication: generic settings, and kernel modules compiled so that
they do not contain the build machine's paths. **Neither file has been booted yet.** Images built
from the same sources with the same tool were tested as follows:

* 3702i, same options (`--wifi --dhcp`): installed on two `AIR-CAP3702I-E-K9`, one of them straight
  from Cisco factory state. Both boot from `flash:` with 2 CPUs and both radios, and answer on the
  network 81 s / 91 s after an unattended power cycle. With these drivers one 802.11ac phone on
  5 GHz linked at 780–867 Mbit/s and received 155–213 Mbit/s of UDP forwarded from the cable
  (limited by the sender).
* 2602i, with a static address and an SSH key built in: installed on two `AIR-CAP2602I-E-K9`, cold
  boot in 110–115 s with 2 CPUs. One phone connected to each radio in access-point mode (WPA2);
  an internet speed test on the phone gave about 130 Mbit/s on 5 GHz and 36 Mbit/s on a crowded
  2.4 GHz channel.

Known problems: a wired flood above what the AP can forward to Wi-Fi stalls Ethernet receive until
the flood stops (`docs/OPENWRT_25_PORT.md`, section 9.3). Not tested: long uptimes, several clients,
range, other hardware revisions.

## Licences

OpenWrt and the Linux kernel are GPL-2.0; the corresponding source is OpenWrt release v25.12.6
plus the device trees, drivers and build script in this repository. The two radio firmware files
are redistributed unmodified under the Marvell terms in `licenses/`.
