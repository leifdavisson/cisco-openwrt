# Playbook: Cisco Aironet 3702i OpenWrt Build, Boot & Performance Verification (`ld-branch`)

This document provides deterministic, copy-pasteable instructions for building, repacking, RAM-booting, extracting Cisco `flashfs` calibration data, and benchmarking the **Cisco Aironet 3702i** on `ld-branch`.

---

## Global Rules & Safety Constraints

1. **NEVER modify or disconnect `enp8s0` (`192.168.1.153`)**: It is the primary wired TFTP/management link to the lab switch. Always bind Wi-Fi `iperf3` client tests explicitly to `wlp9s0` (`-B 192.168.1.244`) targeting `192.168.1.245`.
2. **NEVER passes `--timeout` to `tools/serial_cmd.py`**: `serial_cmd.py` forwards all CLI arguments directly to the remote BusyBox shell.
3. **NEVER `rmmod mwlwifi` on a live AP without a full reboot + `radio_reset`**: `mwlwifi`'s `pcie_reset()` hangs the 88W8864 on the P1023 PCIe root complex unless GPIO 3 is pulsed by `radio_reset.ko`. Always test `mwlwifi.ko` changes via a clean PoE TFTP RAM-boot.

---

## Completed & Hardware-Verified Milestones

- **Dual-Core SMP & 256 KB L2 Cache (`ae76708`):** Both 800 MHz `e500v2` cores online (`smp: Brought up 1 node, 2 CPUs`, `L2CTL = 0x90000000`).
- **Zero-Copy `p1023_fman_im.c` Ethernet (`bee4dd0`):** 512 RX / 256 TX `dma_map_single()` rings; **0 RX errors, 0 TX drops** across 574,921 RX / 206,596 TX benchmark frames (`14%` `CPU0` softirq at 244 Mbps).
- **2.4 GHz `mwl8k` Lockless `MWL8K_CMD_BASTREAM` (`1e06e01`):** `ap_tx_ampdu=true` enabled by default; BA stream creation in **82 ms**, **`144.4 MBit/s MCS 15 short GI`**, **15.9 Mbits/sec** TCP DOWN with 0 stalls.
- **Cisco `flashfs` Calibration Extractor (`tools/extract_cisco_flashfs.py`):** Extracts and SHA-512 verifies all 261 Cisco `flashfs` files (`B5.bin`, `Q5.bin`, `E5.bin`, `8004.img`, `8006.img`, `triggerfish_cpld.img`) from `backups/full_flash_64m.bin`.
- **5 GHz `mwlwifi` 2-Core SMP `tx_csd` Pipeline + Fast `free_txskb` (Built & Repacked in `~/Downloads/openwrt-3702i.bin`):** Splits `br-lan` + `mac80211` (`CPU1`, `eth0` `rps_cpus=2`) from `pcie_8864_tx_skbs` A-MSDU `memcpy` + `IRQ 17` (`CPU0`, via `smp_call_function_single_async`).

---

## Next Step: Boot Repacked `openwrt-3702i.bin` & Benchmark 2-Core SMP Pipeline

### 1. Repack Image (Already Built at `~/Downloads/openwrt-3702i.bin`)
If you modify `driver/`, `driver/mwlwifi/`, `driver/mwl8k/`, or `dts/cisco-3702i.dts`:
```bash
git -C driver/mwlwifi diff > patches/mwlwifi-powerpc-fixes.patch
python3 tools/repack_openwrt_image.py
```

### 2. PoE Power-Cycle & TFTP RAM-Boot
```bash
POE_SWITCH_PASS="<your_switch_password>" python3 tools/poe_bounce_to_rommon.py
python3 - <<'EOF'
import serial, time
s = serial.Serial("/dev/ttyUSB0", 9600, timeout=0.5)
def wait_for(token, timeout=45):
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = s.read(512).decode("latin1", errors="replace")
        if d:
            buf += d
            print(d, end="", flush=True)
            if token in buf:
                return buf
    return buf

for cmd, tok, tmo in [
    ("set IP_ADDR 192.168.1.243", "ap:", 10),
    ("set NETMASK 255.255.255.0", "ap:", 10),
    ("set DEFAULT_ROUTER 192.168.1.1", "ap:", 10),
    ("ether_init", "ap:", 15),
    ("tftp_init", "ap:", 15),
    ("boot tftp://192.168.1.153/openwrt-3702i.bin", "procd: - init -", 150),
]:
    s.write((cmd + "\r\n").encode())
    wait_for(tok, timeout=tmo)
s.close()
EOF
sleep 18
python3 tools/serial_cmd.py "dmesg -n 1; dmesg | grep -E 'smp:|p1023'; iwinfo"
```

### 3. Run 5 GHz & 2.4 GHz Benchmarks + Verify Dual-Core `/proc/stat` Balance
```bash
# 5 GHz VHT80 Benchmark:
nmcli dev wifi rescan ifname wlp9s0 && sleep 3
nmcli dev wifi connect OpenWRT ifname wlp9s0 bssid <AP_5GHZ_BSSID>
iw dev wlp9s0 set power_save off
python3 tools/serial_cmd.py "grep '^cpu' /proc/stat"
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -P 4 -R
iperf3 -c 192.168.1.245 -B 192.168.1.244 -t 10 -P 4
python3 tools/serial_cmd.py "grep '^cpu' /proc/stat; iw dev phy0-ap0 station dump"

# Extract Cisco flashfs radio power tables if needed:
python3 tools/extract_cisco_flashfs.py --decompress --outdir /tmp/cisco_cal
```

---

## External Project References & Attribution
- [OpenWrt Official Project](https://openwrt.org/) — [GitHub Repository](https://github.com/openwrt/openwrt)
- [OpenWrt mpc85xx Target Documentation](https://openwrt.org/docs/techref/targets/mpc85xx)
- [Marvell mac80211 Driver (`mwlwifi`) Repository](https://github.com/kaloz/mwlwifi)
- [Linux Kernel `mwl8k` Driver & Firmware (`linux-firmware`)](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k)
- [ESnet `iperf3` Network Benchmark Tool](https://iperf.fr/)
