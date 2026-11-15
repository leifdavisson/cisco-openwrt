# Cisco Aironet 3702i Standalone Conversion & Open-Source Integration

This repository contains tools, configuration templates, and instructions for converting a **Cisco Aironet 3702i** (`AIR-CAP3702I-A-K9`, platform `ap3g2`) from controller-dependent **Lightweight (k9w8)** firmware into a **Standalone Autonomous (k9w7)** access point that integrates with home networks and [OpenWrt](https://openwrt.org/) gateways.

## External Project References & Attribution
- [OpenWrt Official Project](https://openwrt.org/)
- [OpenWrt Table of Hardware (ToH)](https://openwrt.org/toh/start)
- [Cisco Aironet 3700 Series Documentation](https://www.cisco.com/c/en/us/support/wireless/aironet-3700-series/series.html)
- [pySerial Library](https://pyserial.readthedocs.io/)

## Hardware Overview
- **SoC:** Freescale QorIQ P1020 (PowerPC dual-core @ 800 MHz)
- **Radios:** Marvell 88W8864 (5 GHz 802.11ac) & Marvell 88W8764 (2.4 GHz 802.11n)
- **Architecture:** `mpc85xx` (e500v2)
- **Console:** 9600 baud, 8N1 via `/dev/ttyUSB0`

## Repository Files
- [`tftp_server.py`](tftp_server.py): Zero-dependency Python TFTP server for serving the autonomous tar image.
- [`serial_monitor.py`](serial_monitor.py): Interactive serial console utility for monitoring and configuring the AP.
- [`ap_setup.cfg`](ap_setup.cfg): Production-ready standalone dual-band Wi-Fi configuration with WPA2-PSK and Web GUI.

## Project Status (OpenWrt port)
- **Working:**
  - **OpenWrt 23.05.5 (Linux 5.15.167)** cold-boots standalone from NOR flash (`flash:/openwrt-3702i.bin`, entry `0x01500000`).
  - **1 Gbps Wired Ethernet:** `driver/p1023_fman_im.c` + `marvell.ko` bridged on `br-lan` (DHCP client in dumb AP mode), with SSH (`dropbear`) and LuCI Web UI (`uhttpd`).
  - **Dual-Band PCIe Wi-Fi Radios (AP Mode, SSID `OpenWRT`):**
    - `radio0` (`phy0`, `9000:01:00.0`, `11ab:b050`): Marvell 88W8864 5 GHz 802.11ac (`VHT80`, channel 36) via `driver/mwlwifi/mwlwifi.ko` + `88W8864.bin`.
    - `radio1` (`phy1`, `a000:03:00.0`, `11ab:b024`): Marvell 88W8764 2.4 GHz 802.11n (`HT20`, channel 6) via `driver/mwl8k/mwl8k.ko` + `fmimage_8764_ap-1.fw`.
  - **Flash Persistence (`/overlay`):** 4 MiB JFFS2 `rootfs_data` MTD partition (`0x03800000`–`0x03c00000` in [`dts/cisco-3702i.dts`](dts/cisco-3702i.dts)) mounted onto `/` via `overlayfs` + `pivot_root` in `/lib/preinit/75_mount_overlay`.
- **In Progress / Next Actions:**
  - **Verify cold-reboot JFFS2 retention:** Confirm Cisco ROMMON `flashfs fsck` does not overwrite the `0x03800000`–`0x03c00000` JFFS2 erase blocks across cold reboots; if `flashfs fsck` touches them, move persistence to a `flashfs` archive or out-of-`flashfs` region.
  - **`mwl8k` hardware IRQ pin:** Replace the `OPC_DONE` polling fallback on `pci1` (`a000:03:00.0`) with the exact MPIC interrupt line to eliminate the `Command ... took 9990 ms` log notices.
  - **OpenWrt 25.12.6 (Linux 6.12.112) port (`tools/repack_openwrt25_image.py`):** Fix bootwrapper load/entry address (`0x01500000` vs `0x02000000`) and early UART console crash (`BOOT FROM RAM` reset).
- **Details:** [Open OPENWRT_PORTING_LAB_NOTEBOOK.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/OPENWRT_PORTING_LAB_NOTEBOOK.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/OPENWRT_PORTING_LAB_NOTEBOOK.md) and [Open LOWER_AI_EXECUTION_PLAN.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/LOWER_AI_EXECUTION_PLAN.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/LOWER_AI_EXECUTION_PLAN.md). Helper scripts live in `tools/` (`repack_openwrt_image.py`, `powercycle_and_boot.py`, `poe_bounce_to_rommon.py`).
- **Upstream:** [OpenWrt](https://openwrt.org/), [mwlwifi](https://github.com/kaloz/mwlwifi), [Linux `mwl8k` firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k).

## License
Licensed under the [GNU Affero General Public License v3.0 (GNU AGPLv3)](LICENSE).
