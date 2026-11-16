# Cisco Aironet 3702i Wi-Fi & Ethernet Performance: Status, Ceiling Analysis & Next Steps

## 1. Executive Summary & Benchmark Progression

Across commits `9d823f8`, `6208d58`, `758d106`, `ae76708`, `892a68f`, and our zero-copy `p1023_fman_im.c` DMA upgrade on `ld-branch`, we systematically eliminated the CPU, cache, interrupt, Big-Endian DMA, and Ethernet bounce-buffer bottlenecks on the **Cisco Aironet 3702i** (`Freescale P1020/P1023`, dual-core `e500v2` @ 800 MHz, OpenWrt 23.05.5 `linux-5.15.167`).

### Verified Hardware Benchmark Progression (`wlp9s0` <-> `phy0-ap0`/`phy1-ap0` <-> `br-lan` <-> `eth0`)

| Test Case | Initial Baseline (`UP`, No L2, Broken BE/DMA) | After L2 Cache + BE/DMA + FMan Ring=512 (`758d106`) | After Dual-Core SMP + RPS + `ap_tx_ampdu=0` (`892a68f`) | After Zero-Copy FMan DMA (`p1023_fman_im.c`) | Total Gain vs. Baseline |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **5 GHz (`VHT80`) 1-Stream TCP DOWN** | `31.9 Mbits/sec` (1154 retries) | `124 Mbits/sec` (0 retries) | **244 Mbits/sec** (`248` sender, 5 retries @ `VHT-MCS 5`) | **201–244 Mbits/sec** (`201` @ `MCS 4`, `244` @ `MCS 5`) | **7.6x** |
| **5 GHz (`VHT80`) 4-Stream TCP DOWN** | `70.9 Mbits/sec` | `126 Mbits/sec` | **243 Mbits/sec** (`258` sender @ `VHT-MCS 5`) | **202–243 Mbits/sec** (`202` @ `MCS 4`, `243` @ `MCS 5`) | **3.4x** |
| **5 GHz (`VHT80`) 4-Stream TCP UP** | `44.0 Mbits/sec` | `97.9 Mbits/sec` (0 retries) | **185 Mbits/sec** (`204` sender, 0 retries) | **166–185 Mbits/sec** (`176` sender, 0 retries) | **4.2x** |
| **5 GHz (`VHT80`) UDP 300M DOWN** | `141 Mbits/sec` (`0.227 ms` jitter) | `152 Mbits/sec` (`0.097 ms` jitter) | **252 Mbits/sec** (`0.049 ms` jitter, 15% drop at `eth0`) | **216–252 Mbits/sec** (**0% drop at `eth0`**, `rx_err=0`, `tx_drop=0`) | **1.8x** |
| **2.4 GHz (`HT20`) 1-Stream TCP DOWN** | `0.00 Mbits/sec` (IRQ dead / 15s BA stall) | `12.6 Mbits/sec` (4s burst, then 15s stall) | **6.08–10.5 Mbits/sec** (full 10s, 0 stalls) | **6.08–10.5 Mbits/sec** (full 10s, 0 stalls) | **Fixed (0 -> 10.5 Mbps)** |
| **2.4 GHz (`HT20`) 4-Stream TCP UP** | `0.00 Mbits/sec` (IRQ dead / 15s BA stall) | `0.00 Mbits/sec` (stalled in `mwl8k_fw_lock`) | **4.05–6.29 Mbits/sec** (full 10s, 0 retries) | **4.05–6.29 Mbits/sec** (full 10s, 0 retries) | **Fixed (0 -> 6.29 Mbps)** |
| **`eth0` Hardware / Driver Drops (300M UDP)** | High ring exhaustion (`RX_RING=64`) | `19,337 / 128,053` (**15.1%** dropped at `eth0`) | `19,337 / 128,053` (**15.1%** dropped at `eth0`) | **0 driver drops / 0 RX errors** across `574,921` RX & `206,596` TX pkts | **Eliminated (15.1% -> 0%)** |

---

## 2. Completed Architectural Fixes (`ld-branch`)

1. **Dual-Core SMP (`boot/smp` Stage in `tools/repack_openwrt_image.py` — Commit `ae76708`):**
   - Cisco's IOS ROMMON leaves CPU1 (`e500v2` core 1) held in reset (`EEBPCR = 0x00000001`) and leaves the 256 KB L2 cache disabled (`L2CTL = 0x00000000`), while jumping to the ELF entry with only a 64 MiB TLB1 CAM (`MAS2=0, MAS3=0x00000015`).
   - Integrated Marco Mata's (`origin/fix/openwrt25-load-address`) pre-kernel `boot/smp` stage (`boot/smp/shim.S`, `boot/smp/bootpg.S`) at image offset `0x6B0000` (runtime `0x01bb0000`) with `/memreserve/ 0x0ff00000 0x00001000` and `cpu-release-addr = <0x0 0x0ff00800>` in `dts/cisco-3702i.dts`.
   - Verified both 800 MHz cores online (`smp: Brought up 1 node, 2 CPUs`, `160.00 BogoMIPS`) and configured `/etc/rc.local` to pin `mwlwifi`/`mwl8k` PCIe IRQs (`17`, `18`) to `CPU0` (`smp_affinity=1`) and steer `phy0-ap0`/`phy1-ap0` RX softirqs to `CPU1` (`rps_cpus=2`).
2. **256 KB L2 Cache Activation (`driver/radio_reset.c` & `boot/smp/shim.S` — Commit `6208d58`):**
   - Enabled and invalidated the 256 KB L2 cache (`L2CTL = 0x90000000`), cutting per-packet softirq overhead in half.
3. **Zero-Copy DMA in `driver/p1023_fman_im.c`:**
   - Replaced the `dma_alloc_coherent` bounce buffers (`rx_bufs` / `tx_bufs` requiring a full-frame `memcpy` on every RX and TX packet) with zero-copy `dma_map_single()` / `dma_unmap_single()` over `rx_skbs[512]` and `tx_skbs[256]`, paired with `100 us` `hrtimer` NAPI polling and batched `QD_OFFSET_OUT` doorbell updates every 8 descriptors.
   - Verified on hardware: across `574,921` RX packets (`775.3 MiB`) and `206,596` TX packets (`211.9 MiB`)—including a `300 Mbits/sec` UDP flood of `259,138` packets—`p1023_fman_im` recorded **`rx_err=0` and `tx_drop=0`**, delivering `573,624` packets directly to `phy0-ap0` with `0` netdev drops.
4. **PowerPC Big-Endian & DMA Ordering Fixes in `driver/mwlwifi` (`patches/mwlwifi-powerpc-fixes.patch` — Commit `758d106`):**
   - Fixed the 32-bit `(u32 *)` pointer dereference on the 16-bit `vht_cap.vht_mcs.rx_mcs_map` field in `driver/mwlwifi/hif/fwcmd.c` (lines 2199 & 2287), which on Big-Endian PowerPC shifted the VHT RX MCS map into the upper 16 bits and fed garbage (`tx_mcs_map`) to the 88W8864 firmware's rate adaptation engine.
   - Restored `dma_wmb()` before `NEXT_TXD` assignment in `driver/mwlwifi/hif/pcie/8864/tx.c` and restored the `MACREG_A2HRIC_BIT_MASK` IRQ re-arm toggle in `driver/mwlwifi/hif/pcie/pcie.c`.
5. **2.4 GHz `mwl8k` MPIC Interrupt Mapping & `ap_tx_ampdu` Deadlock Guard (`commits 9d823f8, 6208d58, 892a68f`):**
   - Added the missing `interrupt-map` entries (`&mpic 0 2 / 1 2 / 2 2 / 3 2`) under `pci1: pcie@a000` in `dts/cisco-3702i.dts` so `mwl8k` (`a000:03:00.0`) receives IRQ 18 (`MWL8K_CMD_SET_AID` no longer times out).
   - Guarded `mwl8k_bss_info_changed_ap()` against calling `mwl8k_fw_lock()` every 6 seconds when no ERP/HT parameters changed.
   - Added module parameter `ap_tx_ampdu` (default `false`) in `driver/mwl8k/mwl8k.c` to prevent AP-mode TX A-MPDU initiation from triggering the 15-second `mwl8k_fw_lock()` -> `mwl8k_tx_wait_empty()` deadlock.

---

## 3. Technical Ceiling Analysis: What Is Failing & Remaining Architectural Roadblocks

Now that `eth0` drops **0%** of frames at 300 Mbps (`rx_err=0, tx_drop=0`) and `CPU1` has >60% idle headroom during peak Wi-Fi transfers, **the host CPU and wired Ethernet are proven not to be the bottleneck**. Our throughput ceiling is now strictly dictated by the PHY rates selected by the two Marvell radio firmwares:

### Roadblock A: 5 GHz (`mwlwifi` 88W8864) Stuck at `VHT-MCS 4/5` (`390–520 MBit/s` PHY) & `-72 dBm` Uplink RSSI
- **Symptom / What Is Failing:**
  1. `iw dev phy0-ap0 station dump` shows that when the client (`wlp9s0`, 2x2 802.11ac) is 1 meter from the AP and receives the AP's signal at **`-47 to -50 dBm`**, the AP reports the client's uplink RSSI as **`-72 dBm`** (~22–25 dB weaker than the downlink path).
  2. Because the AP hears the client at `-72 dBm` (and/or lacks calibrated per-channel TX power tables), the 88W8864 firmware (`88W8864.bin`) caps downlink TX rate adaptation at **`VHT-MCS 4` (`390.0 MBit/s`) to `VHT-MCS 5` (`520.0 MBit/s`)** (`80MHz short GI VHT-NSS 2`) instead of `VHT-MCS 8/9` (`780.0–866.7 MBit/s`).
  3. At `390.0 MBit/s` (`VHT-MCS 4`), 52% 802.11ac MAC efficiency yields **201–204 Mbits/sec TCP / 216 Mbits/sec UDP**. At `520.0 MBit/s` (`VHT-MCS 5`), 48% MAC efficiency yields **243–244 Mbits/sec TCP / 252 Mbits/sec UDP**.
  4. Compounding this: because the laptop's Intel `iwlwifi` card hears the AP's beacons at a strong `-48 dBm`, `iwlwifi`'s dynamic Transmit Power Control (TPC) automatically lowers the laptop's `txpower` from `21.00 dBm` down to **`1.00 dBm`** after association (`iw dev wlp9s0 info`), causing the AP's rate adaptation to drop from `VHT-MCS 5` (`244 Mbps`) to `VHT-MCS 4` (`201 Mbps`).
- **Architectural Root Cause:**
  - On the Cisco 3702i (`Tide XL`), the 88W8864 uses external 5 GHz front-end modules (FEMs: external PAs, LNAs, and RX/TX switches/attenuators) and relies on per-board calibration data stored in Cisco NOR flash (`flash:/ap3g2-k9w7-mx.../Q5.bin` or radio EEPROM/OTP).
  - Upstream `mwlwifi` loads the generic `88W8864.bin` firmware with **empty `txpwrlmt` tables** (`/sys/kernel/debug/ieee80211/phy0/mwlwifi/txpwrlmt` is 0 bytes), and `mwlwifi` does not parse or load Cisco's `Q5.bin` / `E5.bin` calibration tables via `HostCmd_CMD_802_11_RF_TX_POWER` / `HostCmd_CMD_SET_PWR_LIMIT`.

### Roadblock B: 2.4 GHz (`mwl8k` 88W8764) Stuck at Legacy `MCS 1` (`13.0 MBit/s` PHY) When `ap_tx_ampdu=0`
- **Symptom / What Is Failing:**
  1. When `ap_tx_ampdu=1` is enabled in `driver/mwl8k/mwl8k.c`, the 88W8764 AP firmware (`fmimage_8764_ap-1.fw`) negotiates `MCS 15` (`130.0 MBit/s` 2x2 HT20) and bursts at `12.6+ Mbits/sec`, **but** as soon as `mac80211` invokes `mwl8k_ampdu_action(IEEE80211_AMPDU_TX_START)`, the deferred worker `mwl8k_check_ba()` calls `mwl8k_fw_lock()`.
  2. `mwl8k_fw_lock()` stops all `mac80211` queues and calls `mwl8k_tx_wait_empty()`, which polls for all 4 hardware TX queues to drain to 0 packets. Under active TCP traffic, the firmware holds 20–82 packets in the TX ring waiting for BA/airtime scheduling and never drains to 0, causing a **15-second timeout** (`waiting for tx rings to drain (82 -> 20 pkts)`) during which all frames are dropped (`0.00 bits/sec`).
  3. With `ap_tx_ampdu=0` (our current default in `892a68f`), the 15-second deadlock is completely eliminated, but `fmimage_8764_ap-1.fw`'s internal rate adaptation only transmits non-A-MPDU frames at **`MCS 1` (`13.0 MBit/s`)**, capping 2.4 GHz TCP throughput at **6.08–10.5 Mbits/sec DOWN / 4.05–6.29 Mbits/sec UP**.
- **Architectural Root Cause:**
  - `mwl8k_check_ba()` -> `mwl8k_create_ba()` (`MWL8K_CMD_BASTREAM`) wraps the firmware command in `mwl8k_fw_lock()`, which was designed for channel/radio reconfiguration (`mwl8k_tx_wait_empty()`), **not** live per-TID BlockAck stream creation during active traffic.

---

## 4. Concrete Next Troubleshooting Actions

Execute the following targeted steps to clear Roadblocks A and B:

### Action 1: Test Fixed High VHT-MCS Rates on 5 GHz (`mwlwifi` `fixed_rate` Debugfs)
Verify whether the 88W8864 RF path can cleanly sustain `VHT-MCS 7/8/9` (`650–866.7 MBit/s`) when rate adaptation is bypassed via `mwlwifi`'s debugfs `fixed_rate` register:
```bash
# On the AP serial console or SSH:
# Format of fixed_rate in mwlwifi: bit 8 = use fixed rate, bits [7:4] = NSS-1 (1 for 2SS), bits [3:0] = MCS (7, 8, or 9)
# Also inspect mwlwifi debugfs info and firmware command handling for fixed_rate:
cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/info
echo 0x0117 > /sys/kernel/debug/ieee80211/phy0/mwlwifi/fixed_rate
```
- **What this proves:** If forcing `VHT-MCS 7/8` pushes 5 GHz TCP throughput from `244 Mbits/sec` to **350–450+ Mbits/sec** with low retries, then the 5 GHz PA/FEM hardware is already working and the `VHT-MCS 4/5` cap is purely an uplink RSSI (`-72 dBm`) / firmware rate-table (`HostCmd_CMD_SET_FIXED_RATE` / `HostCmd_CMD_802_11_TX_RATE_QUERY`) issue.

### Action 2: Extract & Inspect Cisco 3702i Radio Calibration Files (`Q5.bin` / `Q2.bin` / CPLD Registers)
Inspect the Cisco IOS image directory in NOR flash (`backups/full_flash_64m.bin`) and CPLD registers (`0xffdf0000`) to identify the 5 GHz LNA/FEM control bits and per-channel calibration tables:
```bash
# On the host workstation:
strings -n 6 backups/full_flash_64m.bin | grep -E '8864|8764|txpwr|fem|lna|cal' | head -n 40
```
- Compare CPLD register values (`0xffdf0000`–`0xffdf003f`) dumped during native Cisco IOS operation (when both radios are active) against `driver/radio_reset.c` (`CPLD_RADIO_RESET = 0x1e`) to check if an additional CPLD register enables the external 5 GHz LNA / RX switch.

### Action 3: Lockless `MWL8K_CMD_BASTREAM` for 2.4 GHz (`driver/mwl8k/mwl8k.c`)
Modify `mwl8k_check_ba()` in `driver/mwl8k/mwl8k.c` so that `MWL8K_CMD_BASTREAM` (`BA_FLAG_CREATE`) is issued using mutex serialization (`mutex_lock(&priv->fw_mutex)` when `priv->fw_state == FW_STATE_INIT_COMPLETE`) **without** calling `mwl8k_tx_wait_empty()`:
```c
/* In driver/mwl8k/mwl8k.c: mwl8k_check_ba() */
/* Instead of calling mwl8k_fw_lock(hw) -> mwl8k_tx_wait_empty(hw), acquire
 * priv->fw_mutex directly to serialize the command buffer, send
 * mwl8k_create_ba() (MWL8K_CMD_BASTREAM), and call ieee80211_start_tx_ba_cb_irqsafe()
 * while hardware TX rings continue draining normally.
 */
```
- Test with `echo 1 > /sys/module/mwl8k/parameters/ap_tx_ampdu` on the running AP and run `iperf3` on 2.4 GHz (`phy1-ap0`) to verify `MCS 15` (`130 MBit/s` HT20) A-MPDU aggregation without the 15-second TX-drain stall.

---

## External Project References & Attribution
- **[OpenWrt Project](https://openwrt.org/)** — [GitHub Repository](https://github.com/openwrt/openwrt) | [mpc85xx Target Docs](https://openwrt.org/docs/techref/targets/mpc85xx)
- **[mwlwifi Driver & Firmware](https://github.com/kaloz/mwlwifi)** — Upstream Marvell 88W8864 driver repository.
- **[Linux Wireless `mwl8k` Documentation](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwl8k.html)** — Upstream kernel driver for Marvell 88W8764.
- **[ESnet iperf3](https://iperf.fr/)** — [GitHub Repository](https://github.com/esnet/iperf)
