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

| Test | Baseline (L2 OFF) | L2 ON + Ring Size | Current (`wlp9s0` Power-Save OFF, 21 dBm) |
| :--- | :--- | :--- | :--- |
| `eth0` `wget` (`100m.bin`) | 156.28 Mbps | **226.94 Mbps** | **226.94 Mbps** |
| 5 GHz `VHT80` 1-stream DOWN | 31.9 Mbits/sec | 97.5 Mbits/sec | **179 Mbits/sec (192 Mbits/sec peak)** |
| 5 GHz `VHT80` 4-stream DOWN | 70.9 Mbits/sec | **132 Mbits/sec** | 115–120 Mbits/sec (limited by `eth0` 128-BD ring drops) |
| 5 GHz `VHT80` 4-stream UP | 44.0 Mbits/sec | **198 Mbits/sec** | 146–160 Mbits/sec |
| 5 GHz `VHT80` UDP 250M DOWN | 141 Mbits/sec (43% loss) | 203 Mbits/sec (18% loss) | **206 Mbits/sec (17% loss, 98.6% at `eth0`)** |
| 2.4 GHz `HT20` 1-stream DOWN (10s) | 5.64 Mbits/sec (crashed @ 6s) | 14.7 Mbits/sec (crashed @ 6s) | **6.91–10.5 Mbits/sec (full 10s, 0 crashes)** |

---

## 4. Step-by-Step Execution Path for Lower AI

### Task 1: Fix Big-Endian `vht_max_rx_mcs` & Restore `wmb()` / IRQ Mask Toggle in `driver/mwlwifi`
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

### Task 2: Fix `eth0` (`driver/p1023_fman_im.c`) Burst Drops & Uncached `memcpy`
1. Increase `RX_RING_SIZE` from `128` to **`512`** (`768 KB` buffer ring, absorbing full 4-stream TCP window bursts) and `TX_RING_SIZE` from `128` to **`256`**.
2. In `p1023_napi_poll()`, update `bd->status = cpu_to_be16(RXBD_EMPTY)` and write `QD_OFFSET_OUT` **before** calling `napi_gro_receive()` (or every 8 packets) so FMan gets free descriptors back immediately.
3. Reduce `p1023_poll_timer_fn` interval from `250000` ns (250 µs) to `100000` ns (100 µs).
4. *(Phase 2)* Replace `dma_alloc_coherent()` bounce buffers with cached `napi_alloc_skb()` + `dma_map_single()` / `dma_unmap_single()` (since `IM_MODE_GBL = 0x20000000` is enabled in `rx_pram`/`tx_pram`).

### Task 3: Fix 2.4 GHz (`driver/mwl8k/mwl8k.c`) AMPDU Initialization in AP Mode
1. In `mwl8k_sta_add()` (`driver/mwl8k/mwl8k.c:5414`), set `MWL8K_STA(sta)->is_ampdu_allowed = true` when `sta->deflink.ht_cap.ht_supported` is true in the `else` (`priv->ap_fw`) branch.
2. In `mwl8k_ampdu_action()` (`driver/mwl8k/mwl8k.c:5580`), reduce `msleep(1000)` to `msleep(50)` so ADDBA negotiation does not stall and drop QoS data frames for 1–2 seconds.

### Task 4: Rebuild, Repack, RAM-Boot, and Verify
```bash
# 1. Build modules (p1023_fman_im.ko, mwlwifi.ko, mwl8k.ko) and repack /home/leifdavisson/Downloads/openwrt-3702i.bin
# 2. Bounce PoE and TFTP RAM-boot openwrt-3702i.bin
# 3. Verify wlp9s0 power_save is off and run benchmarks:
iw dev wlp9s0 info
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 5 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -P 4 -t 5 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -P 4 -t 5
iperf3 -c 192.168.1.245 -B 192.168.1.244 -u -b 250M -t 5 -R
```
