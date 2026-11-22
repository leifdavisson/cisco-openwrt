#!/usr/bin/env python3
"""
powercycle_and_boot.py - Automated PoE Power-Cycle and OpenWrt Boot Runner for Cisco 3702i.
License: GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting

Controls Juniper switch at 192.168.1.3 to bounce PoE on ge-0/0/4 if needed,
catches Cisco 3702i boot in ROMMON, configures network, boots OpenWrt,
loads Marvell 88E1510 PHY and P1023 FMan v2 driver, ensures non-conflicting IP (192.168.1.50),
and verifies SSH/LuCI.
"""

import os
import sys
import time
import gzip
import subprocess
import serial

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.environ.get("SERIAL_PORT", "/dev/ttyUSB0")
BAUD = 9600
TFTP_IP = os.environ.get("TFTP_IP", "192.168.1.153")
IMAGE = "openwrt-3702i.bin"
LOG_FILE = os.path.join(REPO_DIR, "docs", "openwrt_boot.log")

SWITCH_IP = os.environ.get("POE_SWITCH_IP", "192.168.1.3")
SWITCH_USER = os.environ.get("POE_SWITCH_USER", "root")
SWITCH_PASS = os.environ.get("POE_SWITCH_PASS", "")
POE_INT = os.environ.get("POE_INT", "ge-0/0/4")

TARGET_IP = os.environ.get("TARGET_IP", "192.168.1.50")
NETMASK = "255.255.255.0"
GATEWAY = os.environ.get("GATEWAY", "192.168.1.1")

MARVELL_KO = os.path.join(REPO_DIR, "imagebuilder/build_dir/target-powerpc_8548_musl/root-mpc85xx/lib/modules/5.15.167/marvell.ko")
FMAN_KO = os.path.join(REPO_DIR, "driver/p1023_fman_im.ko")

def run_switch_cmd(cmd):
    ssh_base = [
        "ssh", "-o", "StrictHostKeyChecking=accept-new",
        f"{SWITCH_USER}@{SWITCH_IP}",
        f"cli -c '{cmd}'",
    ]
    env = os.environ.copy()
    if SWITCH_PASS:
        env["SSHPASS"] = SWITCH_PASS
        full_cmd = ["sshpass", "-e", *ssh_base]
    else:
        full_cmd = ssh_base
    res = subprocess.run(full_cmd, capture_output=True, text=True, timeout=60, env=env)
    return res.stdout.strip()

def send_paced(ser, text, delay=0.015):
    for ch in text:
        ser.write(ch.encode("latin1"))
        time.sleep(delay)
    ser.write(b"\r\n")

def send_cmd(ser, cmd, timeout=5.0):
    print(f"\n[ROMMON >>] {cmd}")
    send_paced(ser, cmd)
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            sys.stdout.write(c)
            sys.stdout.flush()
            buf += c
            if "ap:" in buf[-10:]:
                break
        time.sleep(0.05)
    return buf

def send_shell_cmd(ser, cmd, wait_time=0.8):
    print(f"\n[OpenWrt >>] {cmd}")
    for ch in cmd:
        ser.write(ch.encode("ascii"))
        time.sleep(0.008)
    ser.write(b"\n")
    time.sleep(wait_time)
    out = ""
    t0 = time.time()
    while time.time() - t0 < 3.0:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            sys.stdout.write(c)
            sys.stdout.flush()
            out += c
            t0 = time.time()
        time.sleep(0.05)
    return out

def send_shell_cmd_prompt(ser, cmd, timeout=5.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            buf += c
            if "root@OpenWrt:" in buf or "root@" in buf[-20:]:
                break
        time.sleep(0.02)
    return buf

def upload_driver(ser, local_path, mod_name):
    import hashlib
    if not os.path.exists(local_path):
        print(f"[!] Warning: {local_path} not found, skipping upload")
        return False
    with open(local_path, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw)
    expected_md5 = hashlib.md5(gz).hexdigest()
    print(f"[*] Uploading module {mod_name} ({len(gz)} bytes compressed from {len(raw)} bytes at 9600 baud)...")
    print(f"[*] Expected MD5: {expected_md5}")

    send_shell_cmd_prompt(ser, f"rm -f /tmp/{mod_name}.ko /tmp/{mod_name}.ko.gz")
    chunk_size = 64
    total_chunks = (len(gz) + chunk_size - 1) // chunk_size

    for idx, i in enumerate(range(0, len(gz), chunk_size)):
        chunk = gz[i:i+chunk_size]
        hex_str = "".join(f"\\x{b:02x}" for b in chunk)
        cmd = f"printf \"{hex_str}\" >> /tmp/{mod_name}.ko.gz"
        send_shell_cmd_prompt(ser, cmd, timeout=3.0)
        if (idx + 1) % 20 == 0 or idx == total_chunks - 1:
            print(f"  Chunk {idx+1}/{total_chunks} uploaded ({min(i + chunk_size, len(gz))}/{len(gz)} bytes)...")

    # Verify MD5 on the AP
    print("[*] Verifying MD5 checksum on AP...")
    res = send_shell_cmd_prompt(ser, f"md5sum /tmp/{mod_name}.ko.gz")
    print(f"  AP output: {res.strip()}")
    if expected_md5 not in res:
        print(f"[!] MD5 mismatch! Expected {expected_md5}")
        return False
    print("[+] MD5 verified! Uncompressing and inserting driver module...")

    send_shell_cmd(ser, f"gunzip -f /tmp/{mod_name}.ko.gz", wait_time=0.8)
    send_shell_cmd(ser, f"insmod /tmp/{mod_name}.ko", wait_time=2.5)
    send_shell_cmd(ser, f"lsmod | grep {mod_name.replace('-', '_')}", wait_time=0.8)
    send_shell_cmd(ser, "dmesg | tail -n 25", wait_time=1.0)
    return True

def main():
    print("=" * 70)
    print("CISCO AIRONET 3702i - AUTOMATED POE RECYCLE & OPENWRT BOOT")
    print("=" * 70)

    print(f"[*] Opening serial port {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.reset_input_buffer()

    # Check if AP is already sitting at the ROMMON ap: prompt
    print("[*] Checking if AP is already sitting at 'ap:' prompt...")
    ser.write(b"\r\n")
    time.sleep(0.4)
    initial_buf = ser.read(ser.in_waiting or 1024).decode("latin1", errors="replace")
    sys.stdout.write(initial_buf)
    sys.stdout.flush()

    got_prompt = "ap:" in initial_buf
    if not got_prompt:
        ser.write(b"\r\n")
        time.sleep(0.4)
        extra = ser.read(ser.in_waiting or 1024).decode("latin1", errors="replace")
        sys.stdout.write(extra)
        sys.stdout.flush()
        if "ap:" in extra:
            got_prompt = True

    if not got_prompt:
        print(f"[*] Power-cycling {POE_INT} on Juniper switch {SWITCH_IP}...")
        print(f"[*] Disabling PoE on {POE_INT}...")
        out_dis = run_switch_cmd(f"configure; set poe interface {POE_INT} disable; commit; exit")
        print(f"[Switch] {out_dis}")

        print("[*] Waiting 4 seconds for capacitors to drain...")
        time.sleep(4.0)

        print(f"[*] Re-enabling PoE on {POE_INT}...")
        out_en = run_switch_cmd(f"configure; delete poe interface {POE_INT} disable; commit; exit")
        print(f"[Switch] {out_en}")

        print("\n" + "=" * 70)
        print("[*] WAITING FOR BOOTLOADER / INTERRUPT PROMPT...")
        print("=" * 70 + "\n")

        buf = ""
        t0 = time.time()

        while time.time() - t0 < 60.0:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
                sys.stdout.write(chunk)
                sys.stdout.flush()
                buf += chunk

                if "Interrupt within" in buf or "Boot Loader" in buf:
                    print("\n[!] Sending escape sequence to interrupt bootloader...")
                    ser.write(b"\x1b\r\n")
                    time.sleep(0.1)

                if "ap:" in buf[-10:]:
                    print("\n[+] Reached ROMMON 'ap:' prompt!")
                    got_prompt = True
                    break
            else:
                time.sleep(0.05)
    else:
        print("\n[+] AP is already sitting at 'ap:' prompt! Skipping power-cycle.")

    if not got_prompt:
        print("\n[!] Warning: 'ap:' prompt not detected, sending newline...")
        ser.write(b"\r\n")
        time.sleep(0.5)

    time.sleep(0.5)
    ser.write(b"\r\n")
    time.sleep(0.3)
    if ser.in_waiting:
        ser.read(ser.in_waiting)

    # Configure network in ROMMON
    print("\n[*] Configuring ROMMON network environment...")
    send_cmd(ser, f"set IP_ADDR {TARGET_IP}")
    send_cmd(ser, f"set NETMASK {NETMASK}")
    send_cmd(ser, f"set DEFAULT_ROUTER {GATEWAY}")
    send_cmd(ser, "tftp_init")
    send_cmd(ser, "ether_init", timeout=8.0)
    send_cmd(ser, f"arp {TFTP_IP}", timeout=6.0)

    # Issue boot command
    boot_cmd = f"boot tftp://{TFTP_IP}/{IMAGE}"
    print("\n" + "=" * 70)
    print(f"[*] LAUNCHING OPENWRT KERNEL: {boot_cmd}")
    print("=" * 70 + "\n")
    send_paced(ser, boot_cmd)

    # Monitor and stream kernel logs
    print(f"[*] Streaming console output to {LOG_FILE} (monitoring for 180 seconds)...")
    t_start = time.time()
    activated = False
    kernel_buf = ""
    last_wake = t_start + 30.0

    with open(LOG_FILE, "w", encoding="utf-8", errors="replace") as log:
        while time.time() - t_start < 180.0:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
                sys.stdout.write(chunk)
                sys.stdout.flush()
                log.write(chunk)
                log.flush()
                kernel_buf += chunk

                triggers = ["Please press Enter to activate this console", "Please press Enter", "root@OpenWrt", "root@"]
                if any(t in kernel_buf for t in triggers) and not activated:
                    print("\n[+] OpenWrt prompt detected! Waiting 8s for init to settle...")
                    time.sleep(8.0)
                    ser.write(b"\r\n")
                    time.sleep(0.5)
                    ser.write(b"\r\n")
                    time.sleep(0.5)

                    # Configure printk to capture informative kernel logs
                    print("\n[*] Setting console printk level to 7...")
                    send_shell_cmd(ser, "echo 7 > /proc/sys/kernel/printk", wait_time=0.4)

                    # Check kernel modules and status
                    print("\n[*] Checking/Loading Marvell PHY and P1023 FMan drivers...")
                    send_shell_cmd(ser, "insmod /lib/modules/5.15.167/marvell.ko 2>/dev/null || true", wait_time=0.5)
                    res = send_shell_cmd(ser, "test -f /lib/modules/5.15.167/p1023_fman_im.ko && echo FILE_EXISTS || echo FILE_MISSING", wait_time=0.4)
                    if "FILE_EXISTS" not in res:
                        upload_driver(ser, FMAN_KO, "p1023_fman_im")
                    send_shell_cmd(ser, "insmod /lib/modules/5.15.167/p1023_fman_im.ko 2>/dev/null || true", wait_time=1.5)

                    send_shell_cmd(ser, "dmesg | tail -n 25", wait_time=0.8)

                    print("\n[*] Ensuring network configuration is active...")
                    send_shell_cmd(ser, f"uci set network.lan.ipaddr='{TARGET_IP}'", wait_time=0.2)
                    send_shell_cmd(ser, f"uci set network.lan.netmask='{NETMASK}'", wait_time=0.2)
                    send_shell_cmd(ser, f"uci set network.lan.gateway='{GATEWAY}'", wait_time=0.2)
                    send_shell_cmd(ser, f"uci set network.lan.dns='{GATEWAY}'", wait_time=0.2)
                    send_shell_cmd(ser, "uci set dhcp.lan.ignore='1'", wait_time=0.2)
                    send_shell_cmd(ser, "uci commit", wait_time=0.3)
                    send_shell_cmd(ser, "passwd -d root", wait_time=0.2)
                    send_shell_cmd(ser, "/etc/init.d/network restart", wait_time=4.0)

                    send_shell_cmd(ser, "ip link set eth0 up 2>/dev/null || true", wait_time=0.8)
                    send_shell_cmd(ser, "ip link set br-lan up 2>/dev/null || true", wait_time=0.5)
                    send_shell_cmd(ser, f"ip addr add {TARGET_IP}/24 dev br-lan 2>/dev/null || true", wait_time=0.4)
                    send_shell_cmd(ser, f"ip route add default via {GATEWAY} dev br-lan 2>/dev/null || true", wait_time=0.4)
                    send_shell_cmd(ser, "/etc/init.d/dropbear restart", wait_time=1.0)
                    send_shell_cmd(ser, "/etc/init.d/uhttpd restart", wait_time=1.0)

                    print("\n[*] Checking interfaces & PHY status...")
                    send_shell_cmd(ser, "cat /proc/p1023_fman_im", wait_time=0.8)
                    send_shell_cmd(ser, "cat /sys/class/net/eth0/carrier", wait_time=0.5)
                    send_shell_cmd(ser, "ip a show dev br-lan", wait_time=0.5)
                    send_shell_cmd(ser, "ip a show dev eth0", wait_time=0.5)
                    send_shell_cmd(ser, "ip route", wait_time=0.5)

                    print(f"\n[*] Testing connectivity to gateway {GATEWAY} from AP...")
                    send_shell_cmd(ser, f"ping -c 3 {GATEWAY}", wait_time=4.0)

                    print(f"\n[*] Testing connectivity to host {TFTP_IP} from AP...")
                    send_shell_cmd(ser, f"ping -c 3 {TFTP_IP}", wait_time=4.0)

                    print("\n[*] Inspecting Hardware, MTD, I2C, GPIO, and PCIe Wi-Fi status...")
                    send_shell_cmd(ser, "cat /proc/mtd", wait_time=0.5)
                    send_shell_cmd(ser, "dmesg | grep -iE 'pci|i2c|gpio'", wait_time=0.8)
                    send_shell_cmd(ser, "ls -la /sys/bus/i2c/devices", wait_time=0.5)
                    send_shell_cmd(ser, "ls -la /sys/class/gpio", wait_time=0.5)

                    print("\n[*] Skipping destructive sysfs GPIO reset loop (preserves PCIe BARs)...")
                    send_shell_cmd(ser, "ls -la /sys/bus/pci/devices", wait_time=0.8)
                    send_shell_cmd(ser, "ls -la /sys/class/net", wait_time=0.5)
                    send_shell_cmd(ser, "iwinfo 2>/dev/null || true", wait_time=0.5)

                    activated = True
                    break

            if not activated and time.time() > last_wake:
                if "executing..." in kernel_buf or "Linux version" in kernel_buf or "Booting" in kernel_buf:
                    ser.write(b"\r\n")
                last_wake = time.time() + 6.0

            time.sleep(0.05)

    print("\n" + "=" * 70)
    print("[*] SERIAL BOOT SEQUENCE COMPLETE")
    print("=" * 70)
    ser.close()

    # Host-side verification
    print(f"\n[*] Host Verification: Probing {TARGET_IP} from host...")
    ping_res = subprocess.run(["ping", "-c", "3", TARGET_IP], capture_output=True, text=True)
    print(f"[Host Ping]\n{ping_res.stdout.strip()}")

    print(f"\n[*] Host Verification: Checking LuCI Web UI on http://{TARGET_IP}...")
    curl_res = subprocess.run(["curl", "-IL", "--connect-timeout", "4", f"http://{TARGET_IP}"], capture_output=True, text=True)
    print(f"[Host LuCI HTTP]\n{curl_res.stdout.strip()}")

    print(f"\n[*] Host Verification: Checking SSH on {TARGET_IP}...")
    ssh_res = subprocess.run([
        "ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=5",
        f"root@{TARGET_IP}", "uname -a"
    ], capture_output=True, text=True)
    print(f"[Host SSH]\n{ssh_res.stdout.strip()}")

if __name__ == "__main__":
    main()
