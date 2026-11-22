# Cisco Aironet 3702i Wi-Fi & Ethernet Performance: Verified Findings, SMP Pipeline & Next Steps

## 1. Verified Hardware Benchmark Matrix (`ld-branch`)

| Test Case (`wlp9s0` <-> `3702i` <-> `eth0`) | Initial Baseline (`UP`, No L2, Broken BE/DMA) | After L2 + BE/DMA + FMan Ring=512 (`758d106`) | After 2-CPU SMP + Zero-Copy FMan + IRQ/RPS Split (`1e06e01`) | Total Gain vs. Baseline |
| :--- | :--- | :--- | :--- | :--- |
| **5 GHz (`VHT80`) 1-Stream TCP DOWN** | `31.9 Mbits/sec` (1154 retries) | `124 Mbits/sec` (0 retries) | **251 Mbits/sec** sender / **247 Mbits/sec** receiver (`VHT-MCS 5/6`, `520–585.1 MBit/s`) | **7.9x** |
| **5 GHz (`VHT80`) 4-Stream TCP DOWN** | `70.9 Mbits/sec` | `126 Mbits/sec` | **243 Mbits/sec** (`258` sender) | **3.4x** |
| **5 GHz (`VHT80`) 4-Stream TCP UP** | `44.0 Mbits/sec` | `97.9 Mbits/sec` (0 retries) | **185 Mbits/sec** (`204` sender, 0 retries) | **4.2x** |
| **5 GHz (`VHT80`) UDP 300M DOWN** | `141 Mbits/sec` (`0.227 ms` jitter) | `152 Mbits/sec` (`0.097 ms` jitter) | **252 Mbits/sec** (`0.049 ms` jitter, **0% dropped at `eth0`**) | **1.8x** |
| **2.4 GHz (`HT20`) 1-Stream TCP DOWN** | `0.00 Mbits/sec` (IRQ dead / 15s stall) | `12.6 Mbits/sec` (4s burst, then 15s stall) | **15.9 Mbits/sec** sender / **15.3 Mbits/sec** receiver (`144.4 MBit/s MCS 15 SGI`, 0 stalls) | **Fixed (0 -> 15.9 Mbps)** |
| **2.4 GHz (`HT20`) 4-Stream TCP UP** | `0.00 Mbits/sec` (IRQ dead / 15s stall) | `0.00 Mbits/sec` (stalled in `mwl8k_fw_lock`) | **4.05–6.29 Mbits/sec** (0 retries, 0 stalls) | **Fixed (0 -> 6.29 Mbps)** |
| **`eth0` Driver Drops (`p1023_fman_im.c`)** | High ring exhaustion (`RX_RING=64`) | `19,337 / 128,053` (**15.1%** dropped at `eth0`) | **0 RX errors / 0 TX drops** across `574,921` RX & `206,596` TX pkts | **Eliminated (15.1% -> 0%)** |

---

## 2. Newly Verified Breakthroughs

### A. 2.4 GHz (`mwl8k`) Lockless `MWL8K_CMD_BASTREAM` (`1e06e01`) — Verified on Hardware
- **Fix:** Updated `mwl8k_post_cmd()` in `driver/mwl8k/mwl8k.c` so `MWL8K_CMD_BASTREAM` serializes on `priv->fw_mutex` **without** calling `mwl8k_fw_lock()` / `mwl8k_tx_wait_empty()`, and flipped `ap_tx_ampdu = true` by default.
- **Hardware Verification:**
  - `dmesg` confirmed `Added a new stream` $\rightarrow$ `Started stream` $\rightarrow$ `Created a BA stream for ... : tid 0` completed in **82 ms** (`2847.567s` $\rightarrow$ `2847.650s`) with zero `waiting for tx rings to drain` messages and clean teardown (`Deleted BA stream index 0`).
  - `iw dev wlp9s0 link` confirmed 2.4 GHz PHY rate stepped up from `13.0 MBit/s MCS 1` to **`144.4 MBit/s MCS 15 short GI`**, raising 1-stream TCP DOWN from `6.08–10.5 Mbits/sec` to **`15.9 Mbits/sec`** across a full 10s run with 0 stalls.

### B. Exact Root Cause of the ~250 Mbits/sec 5 GHz Ceiling (`/proc/stat` + `ratetable` Proof)
1. **Decoded Live 88W8864 Firmware Rate Table (`ratetable`):**
   - Writing `echo 1 > /sys/kernel/debug/ieee80211/phy0/mwlwifi/ratetable` dumped all 19 sorted rate entries (`idx 0..18`) for AID 1, confirming the firmware rate table includes `2SS VHT-MCS 4` (`idx 8,9`), `MCS 5` (`idx 10,11`), `MCS 6` (`idx 12,13`), `MCS 7` (`idx 14,15`), `MCS 8` (`idx 16,17`), and `MCS 9` (`idx 18`, `866.7 MBit/s`), and confirming `ampdu: true` + `amsdu: true` (`8K` A-MSDU).
   - Stepping the link up to **`585.1 MBit/s VHT-MCS 6 80MHz short GI VHT-NSS 2`** (`fixed_rate = 0x0e / 0x10`, and naturally via rate adaptation once IRQs were split) still plateaued at **`230–251 Mbits/sec`** TCP!
2. **Measured Per-Core `/proc/stat` Utilization During 5 GHz `iperf3`:**
   - With `eth0` `rps_cpus=1` (default): `CPU0` hit **100% softirq (`498 / 500` ticks)** while `CPU1` sat **85% idle (`77` softirq ticks)**.
   - With `eth0` `rps_cpus=2` and `IRQ 17` on `CPU0`: `CPU0` (`eth0` zero-copy NAPI + `IRQ 17`) used **only 31% softirq (`261` ticks, 69% idle)**, while `CPU1` (`br-lan` + `mac80211` + `mwlwifi` `tx_task`) hit **87–97.5% softirq (`741–780` ticks)** and reached **251 Mbits/sec** sender (**247 Mbits/sec** receiver).
   - **Why `mwlwifi` Saturated One Core:** In `driver/mwlwifi/hif/pcie/8864/tx.c`, `pcie_8864_tx_xmit()` called `tasklet_schedule(&pcie_priv->tx_task)` on the current CPU (`CPU1`), forcing `pcie_8864_tx_skbs()` (8 KB A-MSDU allocation + full-packet `memcpy` + `dma_map_single` cache flush) onto the exact same core already running `br-lan` and `mac80211`. Meanwhile, `pcie_non_pfu_tx_done()` pulled every completed SKB's cold DDR3 header back into L1 cache for `memmove()` + `ieee80211_tx_status()` (`sta_info_get_by_addrs()` RCU hash lookup).

### C. Cisco `flashfs` Radio & Calibration Extraction (`tools/extract_cisco_flashfs.py`)
- Built [extract_cisco_flashfs.py](../tools/extract_cisco_flashfs.py) to parse Cisco's 520-byte (`0x208`) `flashfs` sector chain in `backups/full_flash_64m.bin` and **SHA-512 verified 261 extracted files**, including:
  - **`8006.img` (`1,176,352` bytes decompressed) & `8004.img` (`1,189,888` bytes decompressed):** Proved to be Cisco's proprietary **ThreadX Cortex-A9 Split-MAC + CleanAir** radio firmwares (`8006.img version: B4.18.02`), incompatible with upstream `mwlwifi`/`mwl8k` `HostCmd` drivers.
  - **`B5.bin` (`134,320` bytes decompressed), `E5.bin` (`127,312` bytes), `Q5.bin` (`342,672` bytes):** Bzip2-compressed 16-bit signed dBm per-channel regulatory TX power-limit tables (`0x0024` = Ch 36, `0x0028` = Ch 40; `AIR-CAP3702I-A-K9` uses `B5.bin` for Americas/FCC).
  - **`triggerfish_cpld.img` (`2,460` bytes) & `MCU.bin` (`9,031` bytes):** Board CPLD/MCU images.

---

## 3. Ready-to-Verify Optimizations Built Into `~/Downloads/openwrt-3702i.bin`

The repacked image at `~/Downloads/openwrt-3702i.bin` now contains three new optimizations ready for clean cold-boot benchmarking:

1. **2-Core SMP TX Pipeline in `mwlwifi` (`patches/mwlwifi-powerpc-fixes.patch`):**
   - `eth0` `rps_cpus=2` runs `br-lan` + `mac80211` (`pcie_8864_tx_xmit`) on **`CPU1`**.
   - `pcie_8864_schedule_tx()` uses lockless `smp_call_function_single_async(0, &pcie_priv->tx_csd)` to schedule `pcie_priv->tx_task` (`pcie_8864_tx_skbs`: A-MSDU `memcpy` + `dma_map_single`) on **`CPU0`** alongside `eth0` zero-copy NAPI (`14%` CPU) and `IRQ 17`.
2. **Fast-Path `ieee80211_free_txskb()` TX-Done Reclaim (`driver/mwlwifi/hif/pcie/8864/tx.c`):**
   - Skips cold-cacheline `memmove()` + `sta_info_get_by_addrs()` on 63 out of every 64 normal data frames in `pcie_non_pfu_tx_done()`, while sampling 1/64 frames via `ieee80211_tx_status()` so `iw station dump` `tx bitrate` and `last_ack` remain live.
3. **Default `ap_tx_ampdu = true` + Lockless `MWL8K_CMD_BASTREAM` (`driver/mwl8k/mwl8k.c`):**
   - Enables 2.4 GHz `MCS 15` (`144.4 MBit/s`) hardware A-MPDU automatically on boot.

> **Important Hardware Note:** Do **not** run `rmmod mwlwifi` on a live system without re-running `radio_reset` (GPIO 3 reset pulse), because `pcie_reset()` on the 88W8864 hangs the P1023 PCIe bus until power-cycled. Always test `mwlwifi.ko` updates via clean PoE TFTP RAM-boot (`openwrt-3702i.bin`).

---

## 4. Next Troubleshooting & Verification Steps

1. **PoE Power-Cycle & Boot the Repacked SMP-Pipelined Image (`~/Downloads/openwrt-3702i.bin`):**
   ```bash
   POE_SWITCH_PASS='<switch_password>' python3 tools/poe_bounce_to_rommon.py
   # Then boot tftp://192.168.1.153/openwrt-3702i.bin from ap: (or run powercycle_and_boot.py)
   ```
2. **Benchmark the 2-Core SMP `tx_csd` Pipeline on 5 GHz & Verify `/proc/stat` Balance:**
   ```bash
   nmcli dev wifi rescan ifname wlp9s0 && sleep 3
   nmcli dev wifi connect OpenWRT ifname wlp9s0 bssid <AP_5GHZ_BSSID>
   python3 tools/serial_cmd.py "grep '^cpu' /proc/stat"
   iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -R
   iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -P 4 -R
   python3 tools/serial_cmd.py "grep '^cpu' /proc/stat; iw dev phy0-ap0 station dump"
   ```
   - **Expected:** `CPU0` and `CPU1` both share ~50–70% `softirq` instead of `CPU1` hitting 97.5% while `CPU0` sits 69% idle, lifting 5 GHz TCP goodput past the ~251 Mbits/sec single-core wall toward **320–420+ Mbits/sec**.
3. **Translate `B5.bin` / `E5.bin` into `dts/cisco-3702i.dts` `marvell,powertable`:**
   - Add the `mwlwifi` child DT node (`marvell,2ghz;` + `marvell,powertable`) under `pci0` in `dts/cisco-3702i.dts` using the calibrated per-channel values extracted from `B5.bin` / `E5.bin` so `88W8864` uses calibrated EVM-safe power levels for `VHT-MCS 7/8/9` (`650–866.7 MBit/s`).
4. **Enable `HT40` (`noscan=1` / `HT40+`) & Tune `mwl8k` TX Queues on 2.4 GHz:**
   - Verify automatic boot-time `MCS 15` (`144.4 MBit/s`) `HT20` and test `HT40` (`300 MBit/s` PHY) + 4-stream TCP on `phy1-ap0`.

---

## External Project References & Attribution
- **[OpenWrt Project](https://openwrt.org/)** — [GitHub Repository](https://github.com/openwrt/openwrt) | [mpc85xx Target Docs](https://openwrt.org/docs/techref/targets/mpc85xx)
- **[mwlwifi Driver & Firmware](https://github.com/kaloz/mwlwifi)** — Upstream Marvell 88W8864 driver repository.
- **[Linux Wireless `mwl8k` Documentation](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwl8k.html)** — Upstream kernel driver for Marvell 88W8764.
- **[ESnet iperf3](https://iperf.fr/)** — [GitHub Repository](https://github.com/esnet/iperf)
