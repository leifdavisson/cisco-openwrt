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

## Project Status (OpenWrt port — `ld-branch`)
- **Working & Verified on Hardware:**
  - **OpenWrt 23.05.5 (Linux 5.15.167)** boots from RAM or NOR flash (`flash:/openwrt-3702i.bin`, entry `0x01500000`).
  - **Dual-Core SMP (`2 CPUs`, `160.00 BogoMIPS`) + 256 KB L2 Cache + `HID1[ABE]`:** Pre-kernel [`boot/smp`](boot/smp) stage releases Core 1 from `EEBPCR` boot hold-off into an ePAPR spin-table at `0x0ff00800`, enables `HID1[ABE|ASTME|MBDD]`, and turns on the P1020/P1023 256 KB L2 cache.
  - **1 Gbps Wired Ethernet (`226.94 Mbps` single-core `wget`):** [`driver/p1023_fman_im.c`](driver/p1023_fman_im.c) (`RX_RING_SIZE=512`, `TX_RING_SIZE=256`, 100 µs NAPI timer) + `marvell.ko` bridged on `br-lan`, with RPS + IRQ affinity splitting Ethernet RX (`CPU1`) from Bridge + Wi-Fi TX (`CPU0`).
  - **Dual-Band PCIe Wi-Fi Radios (AP Mode, SSID `OpenWRT`):**
    - `radio0` (`phy0`, `9000:01:00.0`, `11ab:b050`): Marvell 88W8864 5 GHz 802.11ac (`VHT80`, channel 36) via `driver/mwlwifi/mwlwifi.ko` + `88W8864.bin` — **244 Mbits/sec** (1-stream TCP down), **243 Mbits/sec** (4-stream TCP down, 258 sender), **185 Mbits/sec** (4-stream TCP up, 204 sender), **252 Mbits/sec** (UDP down, `0.049 ms` jitter).
    - `radio1` (`phy1`, `a000:03:00.0`, `11ab:b024`): Marvell 88W8764 2.4 GHz 802.11n (`HT20`, channel 6, MPIC IRQ `&mpic 0 2`) via [`driver/mwl8k/mwl8k.c`](driver/mwl8k/mwl8k.c) + `fmimage_8764_ap-1.fw` — **6.91–10.5 Mbits/sec** continuous (6s `fw_lock` stall fixed).
  - **Flash Persistence (`/overlay`):** 4 MiB JFFS2 `rootfs_data` MTD partition (`0x03800000`–`0x03c00000` in [`dts/cisco-3702i.dts`](dts/cisco-3702i.dts)) mounted onto `/` via `overlayfs` + `pivot_root` in `/lib/preinit/75_mount_overlay`.
- **Remaining Roadblocks / Next Actions:**
  - **5 GHz `VHT-MCS 5` (`520 MBit/s`) RF/Rate Ceiling (~250 Mbps wall):** Audit `mwlwifi` `fixed_rate`/`ratetable` debugfs and Cisco per-unit RF calibration/power tables (`Q5.bin`/`E5.bin`) + FEM LNA/PA GPIOs so AP TX steps up to `VHT-MCS 8/9` (`780–866.7 MBit/s` → 400+ Mbps).
  - **2.4 GHz `mwl8k` ADDBA TX-Ring Drain Deadlock:** Fix `waiting for tx rings to drain (82 -> 20 pkts)` in `mwl8k_fw_lock()` when `mwl8k_check_ba()` runs during `IEEE80211_AMPDU_TX_START`.
  - **`p1023_fman_im.c` Zero-Copy DMA (`dma_map_single`):** Replace `rx_bufs`/`tx_bufs` bounce-buffer `memcpy()` with zero-copy `dma_map_single()` skb rings to eliminate the 15% UDP burst tail-drops on `eth0`.
  - **Verify cold-reboot JFFS2 retention** across ROMMON `flashfs fsck`.
- **Details:**
  - [Open WIFI_PERFORMANCE_TESTING_AND_PLAN.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md)
  - [Open WIFI_PERFORMANCE.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/WIFI_PERFORMANCE.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/WIFI_PERFORMANCE.md)
  - [Open SMP.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/SMP.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/SMP.md)
  - [Open OPENWRT_PORTING_LAB_NOTEBOOK.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/OPENWRT_PORTING_LAB_NOTEBOOK.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/OPENWRT_PORTING_LAB_NOTEBOOK.md)
  - [Open LOWER_AI_EXECUTION_PLAN.md](file:///home/leifdavisson/projects/cisco-openwrt/docs/LOWER_AI_EXECUTION_PLAN.md) (file:///home/leifdavisson/projects/cisco-openwrt/docs/LOWER_AI_EXECUTION_PLAN.md)
- **Upstream:** [OpenWrt](https://openwrt.org/), [mwlwifi](https://github.com/kaloz/mwlwifi), [Linux `mwl8k` firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k).

## License
Licensed under the [GNU Affero General Public License v3.0 (GNU AGPLv3)](LICENSE).
