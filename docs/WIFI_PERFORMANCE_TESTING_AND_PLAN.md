<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  License: GNU AGPLv3 (GNU Affero General Public License v3.0)
-->
# Cisco Aironet 3702i Wi-Fi Performance Root-Cause Analysis & Execution Plan

This document details the exact root causes of the Wi-Fi and wired throughput bottlenecks on the Cisco Aironet 3702i (`ld-branch`), backed by live packet-path and hardware register profiling, along with a step-by-step fix and testing plan for a lower-tier AI.

---

## 1. Safe Test Environment & Rules

> [!CAUTION]
> **NEVER disconnect or modify `enp8s0` (`192.168.1.153`)** — it is the host's primary uplink.

- **Wi-Fi client interface:** `wlp9s0` (`192.168.1.244` when connected to `OpenWRT`).
- **Disable client Wi-Fi power-save before benchmarking:**
  When `wlp9s0` has `Power save: on`, Intel `iwlwifi` throttles client TX power to `1.00 dBm` (1.25 mW). Disabling power-save raises `wlp9s0` TX power to `21.00 dBm` (125 mW):
  ```bash
  nmcli connection modify bc4b773d-9198-4b13-8ab5-eaedca45b483 802-11-wireless.powersave 2
  nmcli connection up bc4b773d-9198-4b13-8ab5-eaedca45b483
  iw dev wlp9s0 info   # Verify: txpower 21.00 dBm, Power save: off
  ```
- **iperf3 server:** `192.168.1.245:5201` (Proxmox LXC). Always bind `iperf3` to `wlp9s0` (`-B 192.168.1.244`).
- **Console access:** Serial `/dev/ttyUSB0` at `9600` baud via `tools/serial_cmd.py`.

---

## 2. Proven Root Causes (From Live Profiling)

### Root Cause A: 98.6% of Download Packet Drops Happen at `eth0` (`p1023_fman_im.c`), NOT on Wi-Fi
Live packet counter comparison across `eth0`, `phy0-ap0`, and `wlp9s0` proved where packets are being lost:
- **UDP 250 Mbps Download (7s):**
  - Server (`192.168.1.245`) sent: `151,155` datagrams (`250 Mbits/sec`)
  - AP `eth0` (`p1023_fman_im`) received: `125,363` packets (**`25,792` dropped on the wire before `eth0` driver!**)
  - AP `phy0-ap0` (`mwlwifi`) transmitted: `125,328` packets (**99.97% delivery through `mwlwifi`!**)
  - Client `wlp9s0` received: `124,754` datagrams (`206 Mbits/sec`)
- **4-Stream TCP Download (5s):**
  - `eth0` received `48,855` packets → `phy0-ap0` transmitted `48,850` → `wlp9s0` received `48,839`, while TCP recorded `162` retransmissions. Over 93% of the dropped TCP segments were tail-dropped by the FMan hardware RX ring before Linux saw them.
- **Why `eth0` tail-drops on bursts:**
  1. **100% single-core `softirq` saturation at ~206 Mbps:** `/proc/stat` showed `+700` `softirq` ticks in `7.00s` (`100.0%` CPU on `cpu0`) during the 250 Mbps UDP test because CPU 1 is offline (`No cpu-release-addr for cpu 1`) and every RX packet is copied (`memcpy`) out of **uncached (`_PAGE_NO_CACHE`) `dma_alloc_coherent()` memory**.
  2. **128-entry RX ring (`RX_RING_SIZE = 128`, ~192 KB) + 250 µs timer polling:** A 1 Gbps TCP window burst from the switch (`>128` packets in `1.5 ms`) overflows the 128-descriptor ring while `cpu0` is busy in `mwlwifi`.
  3. **Delayed `QD_OFFSET_OUT` return:** Updating `QD_OFFSET_OUT` at the end of `p1023_napi_poll()` holds up to 64 of the 128 RX buffers until all `napi_gro_receive()` calls finish.

### Root Cause B: Big-Endian VHT MCS Map Bug in `driver/mwlwifi/hif/fwcmd.c` (Blocks VHT MCS 8 & 9 / 256-QAM)
In `mwl_fwcmd_set_new_stn_add()` (`driver/mwlwifi/hif/fwcmd.c:2199-2201`) and `mwl_fwcmd_set_new_stn_add_sc4()` (`line 2287`):
```c
pcmd->peer_info.vht_max_rx_mcs =
    cpu_to_le32((*((u32 *)&sta->deflink.vht_cap.vht_mcs.rx_mcs_map)) | rx_mcs_map_mask);
```
- `rx_mcs_map` is `__le16` (`0xfffa` = 2 streams, MCS 0–9), followed in memory by `__le16 rx_highest` (`0x0000`).
- On **Big-Endian PowerPC (`P1020`)**, `*(u32 *)&rx_mcs_map` loads `rx_mcs_map` into the **high 16 bits (`0xfaff0000`)** and ORs `rx_mcs_map_mask` (`0x0000fff0`) into the **low 16 bits**, writing `0xfff0` into the low 16 bits of `vht_max_rx_mcs`!
- That tells the 88W8864 firmware that Streams 1 & 2 only support **MCS 0–7 (`00` bits in `0xfff0`) and NEVER MCS 8–9 (256-QAM)**, capping the 5 GHz PHY rate at `VHT-MCS 6/7` (`585 MBit/s`) instead of `VHT-MCS 9` (`866.7 MBit/s`) even at `-51 dBm` RSSI.

### Root Cause C: Two Regressions in `driver/mwlwifi` Hot-Path Patch
1. `driver/mwlwifi/hif/pcie/8864/tx.c`: Removing `wmb()` inside `pcie_tx_skb()` allowed weakly-ordered PowerPC stores to mark `tx_desc->status = EAGLE_TXD_STATUS_FW_OWNED` before `tx_desc->pkt_ptr` was visible in RAM.
2. `driver/mwlwifi/hif/pcie/pcie.c`: Removing `writel(0x00, ...)` before `writel(MACREG_A2HRIC_BIT_MASK, ...)` in `pcie_irq_enable()` prevented the 88W8864 interrupt mask register from re-asserting its IRQ line when an event bit was already set.

### Root Cause D: 2.4 GHz (`mwl8k`) AMPDU Setup Delay & `is_ampdu_allowed` Bug
- **Fixed on `ld-branch` (`6208d58`):** Skipping redundant `mwl8k_fw_lock()` calls in `mwl8k_bss_info_changed_ap()` eliminated the 6-second `Broken pipe` disconnect; 10-second `iperf3` tests now run to completion (`6.91 Mbits/sec` avg, `10.5 Mbits/sec` peak).
- **Remaining 2.4 GHz issue:** In `driver/mwl8k/mwl8k.c:5405-5416`, `mwl8k_sta_add()` never sets `MWL8K_STA(sta)->is_ampdu_allowed = true` when `priv->ap_fw == true`, and `mwl8k_ampdu_action()` (`line 5580`) calls `msleep(1000)` inside `MAX_AMPDU_ATTEMPTS`, causing `dmesg` to log repeated `Added a new stream` → `1s sleep (dropping TX packets)` → `Remove stream` cycles and keeping PHY TX rate at `MCS 1` (`13.0 MBit/s`).

---

## 3. Observed Benchmark Progression (`ld-branch`)

| Test | Baseline (L2 OFF, 1 CPU) | L2 ON + Ring Size (1 CPU) | `758d106` (1 CPU, `RX_RING=512`) | **2-CPU SMP (`boot/smp`) + RPS + IRQ Affinity** |
| :--- | :--- | :--- | :--- | :--- |
| `eth0` `wget` (`100m.bin`) | 156.28 Mbps | **226.94 Mbps** | — | — |
| 5 GHz `VHT80` 1-stream DOWN | 31.9 Mbits/sec | 97.5 Mbits/sec | 121 Mbits/sec | **244 Mbits/sec (248 sender, 5 retr — 7.7×)** |
| 5 GHz `VHT80` 4-stream DOWN | 70.9 Mbits/sec | 132 Mbits/sec | 130 Mbits/sec | **243 Mbits/sec (258 sender — 3.4×)** |
| 5 GHz `VHT80` 4-stream UP | 44.0 Mbits/sec | 198 Mbits/sec | 153 Mbits/sec | **185 Mbits/sec (204 sender, 0 retr — 4.2×)** |
| 5 GHz `VHT80` UDP DOWN | 141 Mbits/sec (43% loss) | 203 Mbits/sec (18% loss) | 196 Mbits/sec | **252 Mbits/sec (0.049 ms jitter @ 300M)** |
| 2.4 GHz `HT20` 1-stream DOWN (10s) | 5.64 Mbits/sec (crashed @ 6s) | 14.7 Mbits/sec (crashed @ 6s) | 6.91–10.5 Mbits/sec (full 10s) | **6.91–10.5 Mbits/sec (full 10s)** |

---

## 4. Step-by-Step Execution Path for Lower AI

### Task 1: Fix Big-Endian `vht_max_rx_mcs` & Restore `wmb()` / IRQ Mask Toggle in `driver/mwlwifi` — **DONE (`758d106`, `patches/mwlwifi-powerpc-fixes.patch`)**
1. In `driver/mwlwifi/hif/fwcmd.c` (around lines 2199 and 2287), replace the broken `(u32 *)` pointer cast with proper endian conversion:
   ```c
   pcmd->peer_info.vht_max_rx_mcs =
       cpu_to_le32((u32)le16_to_cpu(
           sta->deflink.vht_cap.vht_mcs.rx_mcs_map) | rx_mcs_map_mask);
   ```
2. In `driver/mwlwifi/hif/pcie/8864/tx.c` (`pcie_tx_skb`), restore `dma_wmb()` before `tx_desc->status = cpu_to_le32(EAGLE_TXD_STATUS_FW_OWNED)` and `wmb()` + `writel(MACREG_H2ARIC_BIT_PPA_READY, ...)` per descriptor (or ensure `dma_wmb()` orders descriptor fields before `status`).
3. In `driver/mwlwifi/hif/pcie/pcie.c` (`pcie_irq_enable`), restore the `0x00` → `MACREG_A2HRIC_BIT_MASK` toggle (without `pcie_chk_adapter()`):
   ```c
   writel(0x00, pcie_priv->iobase1 + MACREG_REG_A2H_INTERRUPT_MASK);
   writel(MACREG_A2HRIC_BIT_MASK, pcie_priv->iobase1 + MACREG_REG_A2H_INTERRUPT_MASK);
   ```

### Task 2: Fix `eth0` (`driver/p1023_fman_im.c`) Burst Drops & Uncached `memcpy` — **PARTIAL (`758d106`: `RX_RING=512`, `QD_OFFSET_OUT` batching, 100 µs timer; Phase 2 remaining)**
1. Increase `RX_RING_SIZE` from `128` to **`512`** (`768 KB` buffer ring, absorbing full 4-stream TCP window bursts) and `TX_RING_SIZE` from `128` to **`256`**.
2. In `p1023_napi_poll()`, update `bd->status = cpu_to_be16(RXBD_EMPTY)` and write `QD_OFFSET_OUT` **before** calling `napi_gro_receive()` (or every 8 packets) so FMan gets free descriptors back immediately.
3. Reduce `p1023_poll_timer_fn` interval from `250000` ns (250 µs) to `100000` ns (100 µs).
4. *(Phase 2)* Replace `dma_alloc_coherent()` bounce buffers with cached `napi_alloc_skb()` + `dma_map_single()` / `dma_unmap_single()` (since `IM_MODE_GBL = 0x20000000` is enabled in `rx_pram`/`tx_pram`).

### Task 3: Fix 2.4 GHz (`driver/mwl8k/mwl8k.c`) AMPDU Deadlock in AP Mode — **DIAGNOSED (`ae76708`)**
1. Setting `is_ampdu_allowed = true` prematurely in `mwl8k_sta_add()` caused `mwl8k` to negotiate `MCS 15` (`130.0 MBit/s`), but immediately hit a **15-second TX-ring drain deadlock** (`ieee80211 phy1: waiting for tx rings to drain (82 -> 20 pkts)`) inside `mwl8k_check_ba()` → `mwl8k_fw_lock()`, stalling TCP download to `0.00 bits/sec` (`ae76708` reverted the `mwl8k_sta_add` change and kept `msleep(50)`).
2. See **Section 5.2** below for the exact root cause and fix.

### Task 4: Bring CPU 1 Online (`boot/smp`) + Split Pipeline via RPS & IRQ Affinity — **DONE & VERIFIED (`ae76708`)**
- **SMP Bring-Up (`boot/smp/shim.S` + `bootpg.S` by Marco Mata, integrated into `tools/repack_openwrt_image.py`):**
  Sets `HID1 |= ASTME | ABE | MBDD`, enables the 256 KB L2 cache (`L2CTL`), copies the 4 KiB secondary boot page to `0x0ff00000`, releases Core 1 via `BPTR` + `EEBPCR[CPU1_EN]` into the ePAPR spin-table at `0x0ff00800`, and synchronizes both timebases (`smp: Brought up 1 node, 2 CPUs`, `160.00 BogoMIPS`).
- **RPS + IRQ Affinity Pipeline Split (now in `/etc/rc.local`):**
  Without RPS, `eth0`'s `hrtimer` NAPI and `mwlwifi`'s `tx_task` tasklet both run on `CPU1` (`3485` softirq ticks on `CPU1` vs `405` on `CPU0`). Routing `IRQ 17/18` (`mwlwifi`/`mwl8k`) to `CPU0` and setting `rps_cpus=1` on `eth0` + `rps_cpus=2` on `phy0-ap0`/`phy1-ap0` splits Ethernet RX (`CPU1`) from Bridge + Wi-Fi TX (`CPU0`), pushing **5 GHz TCP download to 244–258 Mbits/sec** and **UDP to 252 Mbits/sec**!

---

## 5. Technical Ceiling Analysis: What's Failing, Architectural Roadblocks & Next Actions

With both 800 MHz e500v2 cores online (`CPU0` at ~93% softirq, `CPU1` at ~36% softirq during the 20s benchmark suite), **1-stream TCP (`244 Mbps`), 4-stream TCP (`243 Mbps`), and 300M UDP (`252 Mbps`) all hit the exact same ~250 Mbits/sec wall**. Here are the three architectural roadblocks causing this ceiling and the exact troubleshooting steps to clear them:

### 5.1 Roadblock #1: 5 GHz PHY Rate Ceiling (`VHT-MCS 5`, `520 MBit/s` TX) & Missing Cisco RF Calibration
- **What's Failing:**
  `iw dev phy0-ap0 station dump` during the 250 Mbps runs shows:
  - Client `wlp9s0` hears AP at **`-48 dBm`** (excellent signal, right next to the AP), yet the AP hears the client (`21 dBm` TX) at **`-72 dBm`**!
  - AP transmits at **`520.0 MBit/s VHT-MCS 5 80MHz short GI VHT-NSS 2`** while receiving at `650.0 MBit/s VHT-MCS 7`.
  - At `520 MBit/s` PHY rate, ~48% 802.11ac MAC/A-MPDU efficiency imposes a hard air-interface ceiling of **~250 Mbits/sec** — matching our observed `244–252 Mbits/sec`.
- **Why:**
  1. **Generic Marvell `88W8864.bin` vs Cisco Per-Unit Calibration (`Q5.bin` / `E5.bin`) & External FEM:** Cisco IOS loads board-specific power/calibration tables (`loading Power Tables from ram:/Q5.bin. Class = E`) and configures the external 5 GHz RF Front-End Module (PA/LNA/attenuator GPIOs). Without the Cisco power table or LNA GPIO config, the 88W8864 sees ~20 dB of RX attenuation (`-72 dBm` vs `-48 dBm`) and its internal rate control refuses to step up to 256-QAM (`VHT-MCS 8/9`, `780–866.7 MBit/s`).
  2. **Firmware Rate Table (`ratetable` / `vht_max_rx_mcs`):** Verify what rate table the 88W8864 firmware actually programmed for `aid=1` after our `vht_max_rx_mcs` endianness fix.
- **Concrete Next Troubleshooting Actions:**
  ```bash
  # 1. Inspect the active 88W8864 firmware rate table and power limits for station 1:
  python3 tools/serial_cmd.py "cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/ratetable; cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/txpwrlmt"

  # 2. Test forcing VHT-MCS 8 / MCS 9 (2SS, 80 MHz, SGI) via debugfs fixed_rate
  #    to prove whether 256-QAM works cleanly at -48 dBm RX on the client:
  python3 tools/serial_cmd.py "cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/fixed_rate"
  ```
  - Extract `Q5.bin` / `E5.bin` from the Cisco IOS archive to compare against `dts/cisco-3702i.dts` power tables and check GPIO states (`0xe000fc00`) under Cisco IOS vs OpenWrt.

### 5.2 Roadblock #2: 2.4 GHz (`mwl8k`) TX-Ring Drain Deadlock (`waiting for tx rings to drain (82 -> 20 pkts)`)
- **What's Failing:**
  When `mwl8k` attempts to start an A-MPDU BlockAck stream on AP firmware (`7.4.0.9`), `dmesg` logs:
  ```text
  [  431.389570] ieee80211 phy1: Started stream for 16:3f:13:46:ec:97 0
  [  441.499131] ieee80211 phy1: waiting for tx rings to drain (82 -> 20 pkts)
  [  446.675836] ieee80211 phy1: Created a BA stream for 16:3f:13:46:ec:97 : tid 0
  ```
  During those 15 seconds (`431.38s` → `446.67s`), TCP throughput drops to **`0.00 bits/sec`** (`Broken pipe`).
- **Why:**
  - In `driver/mwl8k/mwl8k.c`, `mwl8k_txq_xmit()` sets `stream->state = AMPDU_STREAM_NEW` and calls `ieee80211_start_tx_ba_session()`.
  - `mac80211` calls `mwl8k_ampdu_action(IEEE80211_AMPDU_TX_START)`, which sets `stream->state = AMPDU_STREAM_IN_PROGRESS` and calls `mwl8k_check_ba()` → `mwl8k_post_pervif_cmd()` → **`mwl8k_fw_lock()`**.
  - `mwl8k_fw_lock()` calls `ieee80211_stop_queues(hw)` and `mwl8k_tx_wait_empty(hw)`, waiting up to 15 seconds for all TX rings (`pending_tx_pkts`) to hit `0`.
  - Meanwhile, any packet arriving in `mwl8k_txq_xmit()` while `stream->state == AMPDU_STREAM_IN_PROGRESS` is **unconditionally dropped** (`Cannot send packet while ADDBA dialog is underway`), and the 20 packets already in the hardware TX ring stall waiting for `mwl8k_tx_start(priv)` or BlockAck window setup!
- **Concrete Next Troubleshooting Actions:**
  1. **Option A (Fastest — Clean HT20 `MCS 15` without ADDBA stalls):** Gate automatic `mwl8k_add_stream()` in `mwl8k_txq_xmit()` (`driver/mwl8k/mwl8k.c:2040`) behind a module parameter `ap_tx_ampdu=0` (default off on AP firmware) so 2.4 GHz runs at `MCS 15` (`130 MBit/s`) without ever triggering `mwl8k_fw_lock()` mid-stream.
  2. **Option B (Fix `MWL8K_CMD_BASTREAM` locking):** Allow `MWL8K_CMD_BASTREAM` (`mwl8k_check_ba` / `mwl8k_create_ba`) to post the firmware command under `mutex_lock(&priv->fw_mutex)` **without** calling `mwl8k_tx_wait_empty(hw)` (matching `mwlwifi`, which never drains TX rings for `HOSTCMD_CMD_BASTREAM`).

### 5.3 Roadblock #3: `p1023_fman_im.c` Single-Copy Bounce Buffers & 15% UDP Burst Tail-Drops
- **What's Failing:**
  During the 300 Mbps UDP download (`task-1090`), `19,337` of `128,053` datagrams (`15%`) were still dropped at `eth0`, and `CPU0` + `CPU1` spent `26.0` core-seconds in `softirq` over the 20s test suite.
- **Why:**
  `driver/p1023_fman_im.c` still copies every RX frame (`skb_put_data` from `priv->rx_bufs`) and every TX frame (`skb_copy_from_linear_data` into `priv->tx_bufs`), and relies on a `100 µs` `hrtimer` instead of zero-copy `dma_map_single()` skb rings.
- **Concrete Next Troubleshooting Actions:**
  1. **Zero-Copy TX (`p1023_start_xmit`):** Map `skb->data` directly via `dma_map_single(&priv->pdev->dev, skb->data, len, DMA_TO_DEVICE)`, store `skb` in `priv->tx_skbs[idx]`, and unmap + `dev_consume_skb_any()` in `p1023_reclaim_tx()`.
  2. **Zero-Copy RX (`p1023_napi_poll`):** Pre-populate `priv->rx_skbs[0..511]` with `netdev_alloc_skb(ndev, PKT_BUF_SIZE + 64)` (64-byte aligned for FMan DMA), map with `dma_map_single(..., DMA_FROM_DEVICE)`, and on RX unmap and pass the `skb` directly to `napi_gro_receive()` while refilling the slot with a fresh `skb`.

---

## 6. Standard Verification Command Suite

```bash
# 1. Build modules + 2-CPU SMP image (/home/leifdavisson/Downloads/openwrt-3702i.bin)
python3 tools/repack_openwrt_image.py --output /home/leifdavisson/Downloads/openwrt-3702i.bin

# 2. Verify 2 CPUs + RPS + wlp9s0 power_save off (21 dBm), then run 5 GHz & 2.4 GHz suite:
python3 tools/serial_cmd.py "grep -E 'processor|bogomips' /proc/cpuinfo; cat /proc/interrupts"
iw dev wlp9s0 info
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 5 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -P 4 -t 5 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -P 4 -t 5
iperf3 -c 192.168.1.245 -B 192.168.1.244 -u -b 300M -t 5 -R
```
