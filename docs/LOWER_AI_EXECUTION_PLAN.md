# Playbook: Cisco Aironet 3702i OpenWrt Build, Boot & Performance Troubleshooting (`ld-branch`)

This document provides deterministic, copy-pasteable instructions for building, repacking, RAM-booting, and troubleshooting the **Cisco Aironet 3702i** OpenWrt image on `ld-branch`.

---

## Global Rules & Safety Constraints

1. **NEVER modify or disconnect `enp8s0` (`192.168.1.153`)**: It is the primary wired TFTP/management link to the lab switch. Always bind Wi-Fi `iperf3` client tests explicitly to `wlp9s0` (`-B 192.168.1.244`) targeting the wired namespace IP (`192.168.1.245`).
2. **Serial Access**: Only one process may open `/dev/ttyUSB0` at `9600 8N1` at a time. Always use `python3 tools/serial_cmd.py "<cmd>"` for running commands on the AP console.

---

## Completed Milestones (Steps 1–5 — Verified on Hardware)

- **Step 1 (Dual-Core SMP & L2 Cache — Verified in `ae76708`):**
  - `tools/repack_openwrt_image.py` compiles `boot/smp/shim.S` and `boot/smp/bootpg.S` and places the SMP stage at image offset `0x6B0000` (runtime `0x01bb0000`), enabling the 256 KB L2 cache (`L2CTL = 0x90000000`) and releasing `CPU1` (`smp: Brought up 1 node, 2 CPUs`).
- **Step 2 (Zero-Copy `p1023_fman_im.c` Ethernet Driver — Verified):**
  - `driver/p1023_fman_im.c` uses zero-copy `dma_map_single()` / `dma_unmap_single()` over `rx_skbs[512]` and `tx_skbs[256]` with a `100 us` `hrtimer` NAPI poll. Verified **0 RX errors and 0 TX drops** across 574,921 RX and 206,596 TX benchmark packets.
- **Step 3 (5 GHz `mwlwifi` Big-Endian VHT MCS & DMA Barrier Fixes — Verified in `758d106`):**
  - Fixed the 32-bit pointer cast on `vht_cap.vht_mcs.rx_mcs_map` in `driver/mwlwifi/hif/fwcmd.c` and restored `dma_wmb()` in `driver/mwlwifi/hif/pcie/8864/tx.c`. Achieved **244 Mbits/sec** TCP DOWN and **252 Mbits/sec** UDP DOWN on 5 GHz `VHT80`.
- **Step 4 (2.4 GHz `mwl8k` MPIC IRQs & `ap_tx_ampdu` Guard — Verified in `9d823f8`, `892a68f`):**
  - Mapped `pci1` (`a000:03:00.0`) MPIC IRQs in `dts/cisco-3702i.dts` and added `ap_tx_ampdu=0` (default) in `driver/mwl8k/mwl8k.c` to prevent the 15-second `mwl8k_fw_lock()` TX-drain deadlock. Achieved stable **6.08–10.5 Mbits/sec** TCP DOWN on 2.4 GHz `HT20`.
- **Step 5 (Self-Contained Image Repacker — Verified):**
  - `python3 tools/repack_openwrt_image.py` compiles out-of-tree modules (`radio_reset.ko`, `p1023_fman_im.ko`, `mwlwifi.ko`, `mwl8k.ko`), injects `/etc/rc.local` IRQ/RPS tuning, builds the DTB and `boot/smp` shim, and outputs `/home/leifdavisson/Downloads/openwrt-3702i.bin`.

---

## Standard Build, Repack & RAM-Boot Procedure

### 1. Compile Updated Kernel Modules & Repack `openwrt-3702i.bin`
```bash
KDIR=/home/leifdavisson/projects/openwrt/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167
CROSS=/home/leifdavisson/projects/openwrt/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-
export PATH="/home/leifdavisson/projects/openwrt/staging_dir/toolchain-powerpc_8548_gcc-12.3.0_musl/bin:$PATH"
export STAGING_DIR="/home/leifdavisson/projects/openwrt/staging_dir"

# Build p1023_fman_im.ko and radio_reset.ko
make -C "$KDIR" ARCH=powerpc CROSS_COMPILE="$CROSS" M="$PWD/driver" modules

# Build mwl8k.ko (if driver/mwl8k/mwl8k.c was modified)
make -C "$KDIR" ARCH=powerpc CROSS_COMPILE="$CROSS" M="$PWD/driver/mwl8k" \
  NOSTDINC_FLAGS="-I/home/leifdavisson/projects/openwrt/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/backports-6.1.110-1/backport-include -I/home/leifdavisson/projects/openwrt/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/backports-6.1.110-1/include -I/home/leifdavisson/projects/openwrt/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/backports-6.1.110-1/include/uapi -include backport/backport.h" \
  KBUILD_MODPOST_WARN=1 modules
cp driver/mwl8k/mwl8k.ko build/modules/mwl8k.ko

# Repack ~/Downloads/openwrt-3702i.bin and copy to /srv/tftp/
python3 tools/repack_openwrt_image.py
```

### 2. Boot `openwrt-3702i.bin` Over TFTP via PoE Bounce
```bash
python3 tools/powercycle_and_boot.py
sleep 15
python3 tools/serial_cmd.py "dmesg -n 1; dmesg | grep -E 'p1023|smp:'; ifconfig eth0; iwinfo"
```

---

## Step 6 (Next): Clear 5 GHz `VHT-MCS 4/5` & `-72 dBm` Uplink RSSI Ceiling

### Objective
Determine why the 88W8864 AP reports client uplink RSSI at `-72 dBm` (when the client is 1 meter away and hears the AP at `-48 dBm`) and why downlink modulation caps at `VHT-MCS 4/5` (`390–520 MBit/s` PHY -> `201–252 Mbits/sec` goodput) instead of `VHT-MCS 8/9` (`780–866.7 MBit/s`).

### Troubleshooting Commands
```bash
# 1. Inspect mwlwifi debugfs state and test fixed_rate on phy0:
python3 tools/serial_cmd.py "cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/info; cat /sys/kernel/debug/ieee80211/phy0/mwlwifi/fixed_rate"

# 2. Check client wlp9s0 txpower (reset if iwlwifi TPC dropped it to 1.00 dBm):
nmcli connection up OpenWRT ifname wlp9s0
iw dev wlp9s0 info
iw dev wlp9s0 link

# 3. Run 5 GHz iperf3 benchmark (ALWAYS bind to 192.168.1.244):
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -R
```

---

## Step 7 (Next): Clear 2.4 GHz `mwl8k` `MWL8K_CMD_BASTREAM` Lockup & `MCS 1` Ceiling

### Objective
Enable 2.4 GHz TX A-MPDU (`MCS 15`, `130 MBit/s` HT20) without triggering the 15-second `mwl8k_fw_lock()` -> `mwl8k_tx_wait_empty()` deadlock.

### Troubleshooting Commands
```bash
# 1. Test current 2.4 GHz stability with ap_tx_ampdu=0 (default):
nmcli dev wifi connect OpenWRT ifname wlp9s0 bssid 64:DC:72:D0:00:00
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -R

# 2. After refactoring mwl8k_check_ba() in driver/mwl8k/mwl8k.c to post MWL8K_CMD_BASTREAM
#    without calling mwl8k_tx_wait_empty(), enable ap_tx_ampdu at runtime and re-test:
python3 tools/serial_cmd.py "echo 1 > /sys/module/mwl8k/parameters/ap_tx_ampdu"
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -R
python3 tools/serial_cmd.py "iw dev phy1-ap0 station dump"
```

---

## External Project References & Attribution
- [OpenWrt Official Project](https://openwrt.org/) — [GitHub Repository](https://github.com/openwrt/openwrt)
- [OpenWrt mpc85xx Target Documentation](https://openwrt.org/docs/techref/targets/mpc85xx)
- [Marvell mac80211 Driver (`mwlwifi`) Repository](https://github.com/kaloz/mwlwifi)
- [Linux Kernel `mwl8k` Driver & Firmware (`linux-firmware`)](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k)
- [ESnet `iperf3` Network Benchmark Tool](https://iperf.fr/)
