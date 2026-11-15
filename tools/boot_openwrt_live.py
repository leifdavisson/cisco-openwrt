#!/usr/bin/env python3
"""
boot_openwrt_live.py - Automated Bootloader Runner for OpenWrt on Cisco Aironet 3702i.
License: GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting

Flow:
1. Listens on /dev/ttyUSB0 (9600 8N1).
2. Catches AP reboot upon power-cycle and synchronizes at 'ap:' prompt.
3. Automatically sets ROMMON network variables (192.168.1.50 -> 192.168.1.153).
4. Initializes TFTP and Gigabit Ethernet.
5. Issues 'boot tftp://192.168.1.153/openwrt-3702i.bin'.
6. Streams live earlyprintk / Linux kernel console logs to stdout and docs/openwrt_boot.log.
"""

import os
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600
REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TFTP_IP = os.environ.get("TFTP_IP", "192.168.1.153")
IMAGE = "openwrt-3702i.bin"
LOG_FILE = os.path.join(REPO_DIR, "docs", "openwrt_boot.log")

def send_paced(ser, text, delay=0.015):
    for ch in text:
        ser.write(ch.encode("latin1"))
        time.sleep(delay)
    ser.write(b"\r\n")

def send_cmd(ser, cmd, timeout=5.0):
    print(f"[ROMMON >>] {cmd}")
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

def main():
    print(f"[*] Opening serial port {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.2)

    print("\n" + "=" * 70)
    print("[!] WAITING FOR AP POWER-CYCLE REBOOT")
    print("    Please unplug and re-plug the Cisco AP's power or PoE cable now.")
    print("=" * 70 + "\n")

    buf = ""

    # Wait for reboot or existing prompt
    while True:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            sys.stdout.write(c)
            sys.stdout.flush()
            buf += c

            if "Starting system" in buf or "Boot Loader" in buf or "flashfs" in buf:
                ser.write(b"\x1b")

            if "ap:" in buf[-10:]:
                print("\n[+] Detected ROMMON 'ap:' prompt!")
                break
        else:
            # Periodically test if already at prompt
            time.sleep(0.1)

    # Let prompt settle
    time.sleep(0.5)
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.read(ser.in_waiting or 10)

    # Configure network in ROMMON
    print("\n[*] Configuring ROMMON network environment...")
    send_cmd(ser, "set IP_ADDR 192.168.1.50")
    send_cmd(ser, "set NETMASK 255.255.255.0")
    send_cmd(ser, "set DEFAULT_ROUTER 192.168.1.1")
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
    t0 = time.time()
    with open(LOG_FILE, "w", encoding="utf-8", errors="replace") as log:
        while time.time() - t0 < 180.0:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
                sys.stdout.write(chunk)
                sys.stdout.flush()
                log.write(chunk)
                log.flush()
            time.sleep(0.05)

    print("\n[*] Monitoring complete.")
    ser.close()

if __name__ == "__main__":
    main()
