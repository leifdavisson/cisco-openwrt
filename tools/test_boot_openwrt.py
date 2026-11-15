#!/usr/bin/env python3
"""
test_boot_openwrt.py - Test RAM booting an OpenWrt MZIP image over TFTP from ROMMON.
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting
"""

import os
import sys
import time
import serial

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.environ.get("SERIAL_PORT", "/dev/ttyUSB0")
BAUD = 9600
TFTP_IP = os.environ.get("TFTP_IP", "192.168.1.153")
IMAGE = "openwrt-3702i.mzip"
LOG_FILE = os.path.join(REPO_DIR, "docs", "boot_openwrt_test.log")

def send_paced(ser, text, char_delay=0.01):
    for ch in text:
        ser.write(ch.encode("latin1"))
        time.sleep(char_delay)
    ser.write(b"\r\n")

def send_cmd_wait_prompt(ser, cmd, timeout=5.0):
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
    print(f"[*] Connecting to {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.2)

    # Wake and clear prompt
    ser.write(b"\r\n")
    time.sleep(0.3)
    buf = ser.read(ser.in_waiting or 1).decode("latin1", errors="replace")
    if "ap:" not in buf:
        ser.write(b"\r\n")
        time.sleep(0.5)
        buf = ser.read(ser.in_waiting or 1).decode("latin1", errors="replace")
        if "ap:" not in buf:
            print(f"[!] Not at 'ap:' prompt (received: {repr(buf)}).")
            ser.close()
            sys.exit(1)

    print("[+] Confirmed ROMMON 'ap:' prompt!")

    # Setup network in ROMMON step-by-step
    send_cmd_wait_prompt(ser, "set IP_ADDR 192.168.1.50")
    send_cmd_wait_prompt(ser, "set NETMASK 255.255.255.0")
    send_cmd_wait_prompt(ser, "set DEFAULT_ROUTER 192.168.1.1")
    send_cmd_wait_prompt(ser, "tftp_init")
    send_cmd_wait_prompt(ser, "ether_init", timeout=6.0)
    send_cmd_wait_prompt(ser, f"arp {TFTP_IP}", timeout=6.0)

    # Issue boot command
    boot_cmd = f"boot tftp://{TFTP_IP}/{IMAGE}"
    print(f"\n[*] Executing RAM Boot Command:\n    {boot_cmd}\n")
    send_paced(ser, boot_cmd)

    # Capture console output to screen and log file
    print(f"[*] Streaming bootloader & kernel console output to {LOG_FILE} (monitoring for 120s)...")
    start = time.time()
    with open(LOG_FILE, "w", encoding="utf-8", errors="replace") as log:
        while time.time() - start < 120:
            if ser.in_waiting:
                chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
                sys.stdout.write(chunk)
                sys.stdout.flush()
                log.write(chunk)
                log.flush()
            time.sleep(0.05)

    print("\n[*] Finished monitoring period.")
    ser.close()

if __name__ == "__main__":
    main()
