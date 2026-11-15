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
- Working: OpenWrt 23.05.5 (Linux 5.15.167) RAM-boots via ROMMON TFTP; 1 Gbps Ethernet (`driver/p1023_fman_im.c`), SSH, LuCI at `192.168.1.50/24`.
- In progress: PCIe Wi-Fi radios (BAR reads still `0xffffffff`; see `driver/radio_reset.c`, `driver/pci_diag.c`).
- Pending: flash persistence.
- Details: [docs/OPENWRT_PORTING_LAB_NOTEBOOK.md](docs/OPENWRT_PORTING_LAB_NOTEBOOK.md). Helper scripts live in `tools/` (`powercycle_and_boot.py`, `poe_bounce_to_rommon.py`).
- Upstream: [OpenWrt](https://openwrt.org/), [mwlwifi](https://github.com/kaloz/mwlwifi).

## License
Licensed under the [GNU Affero General Public License v3.0 (GNU AGPLv3)](LICENSE).
