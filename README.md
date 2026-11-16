# Cisco Aironet 3702i / 3602i OpenWrt Port

Bootable OpenWrt port for the **Cisco Aironet AIR-CAP3702I** (and AIR-CAP3602I) enterprise wireless access points based on the **Freescale/NXP QorIQ P1020 / P1023** (`mpc85xx/p1020`, Dual-Core PowerPC `e500v2` @ 800 MHz).

## Current Status (`ld-branch` — Verified on Hardware)

| Subsystem | Hardware | Driver / Module | Status | Verified Performance / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **SoC / SMP** | Freescale P1020/P1023 (2x e500v2 @ 800 MHz) | `boot/smp` pre-kernel stage + `mpc85xx_smp_defconfig` | **Working (2 CPUs)** | `smp: Brought up 1 node, 2 CPUs` (`160.00 BogoMIPS` total), 256 KB L2 cache enabled (`L2CTL=0x90000000`) |
| **Wired Ethernet (`eth0`)** | DPAA FMan v2 Independent Mode (`dTSEC0` + BCM54616S RGMII) | `driver/p1023_fman_im.c` | **Working (Zero-Copy DMA)** | 1000Base-T Full Duplex; 512 RX / 256 TX zero-copy `dma_map_single()` rings; **0 RX errors, 0 TX drops** across 574,921 RX / 206,596 TX benchmark frames |
| **5 GHz Radio (`phy0-ap0`)** | Marvell `88W8864` 4x4 802.11ac Wave 1 (`9000:01:00.0`) | `driver/mwlwifi` (`88W8864.bin`) | **Working (`VHT80`)** | **244 Mbits/sec** 1-stream TCP DOWN (`7.7x` over baseline), **243 Mbits/sec** 4-stream TCP DOWN, **185 Mbits/sec** 4-stream TCP UP, **252 Mbits/sec** UDP DOWN (`VHT-MCS 5`, `520 MBit/s` PHY) |
| **2.4 GHz Radio (`phy1-ap0`)** | Marvell `88W8764` 4x4 802.11n (`a000:03:00.0`) | `driver/mwl8k` (`fmimage_8764_ap-1.fw`) | **Working (`HT20`)** | MPIC IRQs mapped; `ap_tx_ampdu=0` eliminates 15s `mwl8k_fw_lock()` TX-drain deadlock; **6.08–10.5 Mbits/sec** TCP DOWN, **4.05–6.29 Mbits/sec** TCP UP (0 stalls) |
| **NOR Flash (`mtd0..mtd2`)** | 64 MiB Spansion/Numonyx CFI NOR (`0xec000000`) | `physmap-flash` + JFFS2 overlay | **Working** | Persistent `/overlay` (`rootfs_data`) + standalone `tools/repack_openwrt_image.py` |

## Current Technical Ceiling & Remaining Roadblocks

With both e500v2 CPU cores online, the 256 KB L2 cache active, RPS/IRQ steering configured, and `p1023_fman_im.c` upgraded to zero-copy `dma_map_single()` (`0%` `eth0` packet loss even under 300 Mbps UDP floods), **the SoC CPU and wired Ethernet are no longer the bottleneck**. We are now squarely up against two radio-layer hardware/firmware roadblocks:

1. **5 GHz (`mwlwifi` 88W8864) Capped at `VHT-MCS 4/5` (`390–520 MBit/s` PHY -> `204–252 Mbits/sec` Goodput):**
   - **What is failing:** Even with the client (`wlp9s0`) 1 meter from the AP (`-47 to -50 dBm` client RX RSSI), the AP reports the client's uplink signal at **`-72 dBm`** (~22 dB lower than expected) and caps downlink modulation at `VHT-MCS 4/5` (`390–520 MBit/s` instead of `VHT-MCS 8/9` `780–866.7 MBit/s`). Furthermore, seeing `-48 dBm` beacons from the AP causes client `iwlwifi` dynamic TPC to step down client TX power to `1.00 dBm` unless reset.
   - **Root cause:** Missing Cisco per-unit 5 GHz RF calibration / power-limit tables (`Q5.bin` / `E5.bin` from `flash:` loaded via `CMD_SET_PWR_LIMIT` / `CMD_802_11_RF_ANTENNA`) and/or unconfigured external 5 GHz front-end module (FEM LNA/attenuator) GPIO lines.
2. **2.4 GHz (`mwl8k` 88W8764) Capped at Legacy `MCS 1` (`13.0 MBit/s` PHY -> `6.08–10.5 Mbits/sec` Goodput):**
   - **What is failing:** Enabling AP-mode TX A-MPDU (`ap_tx_ampdu=1`) allows `mwl8k` to negotiate `MCS 15` (`130 MBit/s`), but `mwl8k_ampdu_action(IEEE80211_AMPDU_TX_START)` -> `mwl8k_check_ba()` calls `mwl8k_fw_lock()`, which blocks for 15 seconds in `mwl8k_tx_wait_empty()` (`waiting for tx rings to drain (82 -> 20 pkts)`) while dropping all live traffic. Disabling `ap_tx_ampdu` (`ap_tx_ampdu=0`, default) prevents the deadlock, but the `fmimage_8764_ap-1.fw` firmware only uses `MCS 1` (`13.0 MBit/s`) for non-aggregated frames.
   - **Root cause:** `mwl8k_check_ba()` / `mwl8k_create_ba()` (`MWL8K_CMD_BASTREAM`) uses `mwl8k_fw_lock()` (which waits for all data TX rings to empty while `mac80211` queues are stopped) instead of posting `MWL8K_CMD_BASTREAM` directly via `mwl8k_post_cmd()` without draining data rings.

## Documentation Index

- **[docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md](docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md)**: Complete root-cause analysis, verified before/after benchmark matrix, technical ceiling breakdown, and concrete troubleshooting steps to clear the remaining 5 GHz and 2.4 GHz roadblocks.
- **[docs/WIFI_PERFORMANCE.md](docs/WIFI_PERFORMANCE.md)**: Detailed hardware/firmware investigation of the 3702i radio and Ethernet data path (Marco Mata + `ld-branch` follow-up).
- **[docs/SMP.md](docs/SMP.md)**: Documentation for the pre-kernel `boot/smp` stage (`boot/smp/shim.S` and `boot/smp/bootpg.S`) that brings up CPU1 (`e500v2` core 1) and enables the 256 KB L2 cache after Cisco IOS ROMMON boots.
- **[docs/LOWER_AI_EXECUTION_PLAN.md](docs/LOWER_AI_EXECUTION_PLAN.md)**: Step-by-step build, repack, TFTP RAM-boot, and troubleshooting playbook.
- **[docs/DRIVER_DESIGN.md](docs/DRIVER_DESIGN.md)**: Architecture of the custom `p1023_fman_im.c` Freescale FMan v2 Independent Mode Ethernet driver and `radio_reset.c` CPLD/GPIO initialization module.
- **[docs/FLASH_INSTALLATION.md](docs/FLASH_INSTALLATION.md)**: Instructions for persisting the OpenWrt image to NOR flash (`flash:`) for cold PoE boot.

## External Project References & Attribution

- **[OpenWrt Project](https://openwrt.org/)** — [Repository](https://github.com/openwrt/openwrt) | [mpc85xx Target Documentation](https://openwrt.org/docs/techref/targets/mpc85xx)
- **[mwlwifi (Marvell 88W8864 mac80211 Driver)](https://github.com/kaloz/mwlwifi)** — Official upstream driver and firmware repository for Marvell 802.11ac chipsets.
- **[Linux Kernel `mwl8k` Driver & Firmware](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwl8k.html)** — [linux-firmware `mwl8k` directory](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k)
- **[iperf3 Network Benchmark Tool](https://iperf.fr/)** — [ESnet iperf3 GitHub Repository](https://github.com/esnet/iperf)

## License

Unless otherwise specified by upstream kernel/driver components (GPLv2), original scripts, documentation, and tooling in this repository are licensed under the **GNU Affero General Public License v3.0 (GNU AGPLv3)**.
