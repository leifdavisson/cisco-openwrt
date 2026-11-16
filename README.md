# Cisco Aironet 3702i / 3602i OpenWrt Port

Bootable OpenWrt port for the **Cisco Aironet AIR-CAP3702I** (and AIR-CAP3602I) enterprise wireless access points based on the **Freescale/NXP QorIQ P1020 / P1023** (`mpc85xx/p1020`, Dual-Core PowerPC `e500v2` @ 800 MHz).

## Current Status (`ld-branch` — Verified on Hardware)

| Subsystem | Hardware | Driver / Module | Status | Verified Performance / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **SoC / SMP** | Freescale P1020/P1023 (2x e500v2 @ 800 MHz) | `boot/smp` pre-kernel stage + `mpc85xx_smp_defconfig` | **Working (2 CPUs)** | `smp: Brought up 1 node, 2 CPUs` (`160.00 BogoMIPS` total), 256 KB L2 cache enabled (`L2CTL=0x90000000`) |
| **Wired Ethernet (`eth0`)** | DPAA FMan v2 Independent Mode (`dTSEC0` + Marvell RGMII) | `driver/p1023_fman_im.c` | **Working (Zero-Copy DMA)** | 1000Base-T Full Duplex; 512 RX / 256 TX zero-copy `dma_map_single()` rings; **0 RX errors, 0 TX drops** across 574,921 RX / 206,596 TX benchmark frames (`14%` `CPU0` softirq at 244 Mbps) |
| **5 GHz Radio (`phy0-ap0`)** | Marvell `88W8864` 4x4 802.11ac Wave 1 (`9000:01:00.0`) | `driver/mwlwifi` (`88W8864.bin` + `patches/mwlwifi-powerpc-fixes.patch`) | **Working (`VHT80`)** | **251 Mbits/sec** sender / **247 Mbits/sec** receiver TCP DOWN (`7.9x` over baseline), **185 Mbits/sec** TCP UP, **252 Mbits/sec** UDP DOWN (`585.1 MBit/s VHT-MCS 6` verified); cross-CPU `tx_csd` SMP pipeline built in `openwrt-3702i.bin` |
| **2.4 GHz Radio (`phy1-ap0`)** | Marvell `88W8764` 4x4 802.11n (`a000:03:00.0`) | `driver/mwl8k` (`fmimage_8764_ap-1.fw`) | **Working (`HT20` `MCS 15` A-MPDU)** | Lockless `MWL8K_CMD_BASTREAM` (`1e06e01`) + `ap_tx_ampdu=true`: BA stream up in **82 ms**, **`144.4 MBit/s MCS 15 short GI`**, **15.9 Mbits/sec** TCP DOWN (0 stalls) |
| **NOR Flash & `flashfs` Tools** | 64 MiB Spansion/Numonyx CFI NOR (`0xe8000000`) | `physmap-flash` + `tools/extract_cisco_flashfs.py` | **Working** | Persistent `/overlay` (`rootfs_data`) + SHA-512 verified extractor for all 261 Cisco `flashfs` files (`B5.bin`, `Q5.bin`, `E5.bin`, `8004.img`, `8006.img`) |

## Key Findings & Technical Ceiling Breakdown

1. **True Root Cause of the ~250 Mbits/sec 5 GHz Ceiling (Single-Core `mwlwifi` Softirq Pinning — Solved in `patches/mwlwifi-powerpc-fixes.patch`):**
   - Decoding `88W8864`'s live `ratetable` (`idx 0..18`, up to `2SS VHT-MCS 9` `866.7 MBit/s`) and testing `585.1 MBit/s VHT-MCS 6` alongside `/proc/stat` CPU profiling proved why `390`, `520`, and `585.1 MBit/s` PHY rates all plateaued at **244–251 Mbits/sec**:
   - `mwlwifi`'s `pcie_8864_tx_xmit()` called `tasklet_schedule(&pcie_priv->tx_task)` on the current CPU, pinning `pcie_8864_tx_skbs()` (8 KB A-MSDU allocation + `memcpy` + `dma_map_single`) to the exact same core running `br-lan` + `mac80211` (**87–100% softirq on one core while the other core sat 69–86% idle**).
   - **Implemented in `patches/mwlwifi-powerpc-fixes.patch` & repacked into `openwrt-3702i.bin`:**
     - Offloads `pcie_priv->tx_task` from `CPU1` (`br-lan` + `mac80211`, `eth0` `rps_cpus=2`) onto `CPU0` (`eth0` NAPI + `IRQ 17`) via lockless `smp_call_function_single_async(0, &pcie_priv->tx_csd)`.
     - Bypasses cold-cacheline `memmove` + `sta_info_get_by_addrs()` on 63/64 data frames in `pcie_non_pfu_tx_done()` via `ieee80211_free_txskb()`.
2. **Cisco `Q5.bin` / `B5.bin` / `8006.img` Flash Extraction (`tools/extract_cisco_flashfs.py`):**
   - Extracted and SHA-512 verified all 261 Cisco `flashfs` files from `backups/full_flash_64m.bin`.
   - Verified `8006.img` (`1.17 MB`) and `8004.img` (`1.19 MB`) are Cisco's proprietary ThreadX Cortex-A9 Split-MAC + CleanAir firmwares (incompatible with `mwlwifi`/`mwl8k` `HostCmd`), while `B5.bin` (Americas FCC, `134 KB` decompressed), `E5.bin` (ETSI, `127 KB`), and `Q5.bin` (Japan, `342 KB`) are bzip2-compressed 16-bit signed dBm per-channel regulatory TX power tables.
3. **2.4 GHz (`mwl8k`) Lockless `MWL8K_CMD_BASTREAM` (`1e06e01`):**
   - Updating `mwl8k_post_cmd()` to post `MWL8K_CMD_BASTREAM` under `priv->fw_mutex` without `mwl8k_tx_wait_empty()` completely eliminated the 15-second TX-drain deadlock, enabling `ap_tx_ampdu=true` by default (`144.4 MBit/s MCS 15 short GI`).

## Documentation Index

- **[docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md](docs/WIFI_PERFORMANCE_TESTING_AND_PLAN.md)**: Verified benchmark progression, `/proc/stat` CPU saturation proof, `flashfs` calibration findings, and next-step verification commands.
- **[docs/WIFI_PERFORMANCE.md](docs/WIFI_PERFORMANCE.md)**: Hardware/firmware investigation of the 3702i radio and Ethernet data path (Marco Mata + `ld-branch` follow-up).
- **[docs/SMP.md](docs/SMP.md)**: Pre-kernel `boot/smp` stage (`boot/smp/shim.S` and `boot/smp/bootpg.S`) for bringing up `CPU1` and enabling the 256 KB L2 cache.
- **[docs/LOWER_AI_EXECUTION_PLAN.md](docs/LOWER_AI_EXECUTION_PLAN.md)**: Deterministic build, repack, TFTP RAM-boot, and benchmarking playbook.

## External Project References & Attribution

- **[OpenWrt Project](https://openwrt.org/)** — [Repository](https://github.com/openwrt/openwrt) | [mpc85xx Target Documentation](https://openwrt.org/docs/techref/targets/mpc85xx)
- **[mwlwifi (Marvell 88W8864 mac80211 Driver)](https://github.com/kaloz/mwlwifi)** — Official upstream driver and firmware repository for Marvell 802.11ac chipsets.
- **[Linux Kernel `mwl8k` Driver & Firmware](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwl8k.html)** — [linux-firmware `mwl8k` directory](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k)
- **[ESnet iperf3 Network Benchmark Tool](https://iperf.fr/)** — [GitHub Repository](https://github.com/esnet/iperf)

## License

Unless otherwise specified by upstream kernel/driver components (GPLv2), original scripts, documentation, and tooling in this repository are licensed under the **GNU Affero General Public License v3.0 (GNU AGPLv3)**.
