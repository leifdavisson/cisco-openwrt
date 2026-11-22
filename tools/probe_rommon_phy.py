#!/usr/bin/env python3
"""
probe_rommon_phy.py - Query ROMMON for Ethernet PHY and hardware info.
License: GNU AGPLv3.

Author: Antigravity Pair Programmer
"""

import os
import sys
import time
import subprocess
import serial

SWITCH_IP = os.environ.get("POE_SWITCH_IP", "192.168.1.3")
SWITCH_USER = os.environ.get("POE_SWITCH_USER", "root")
SWITCH_PASS = os.environ.get("POE_SWITCH_PASS", "")
POE_INT = os.environ.get("POE_INT", "ge-0/0/4")
PORT = os.environ.get("SERIAL_PORT", "/dev/ttyUSB0")
BAUD = 9600

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
    res = subprocess.run(full_cmd, capture_output=True, text=True, timeout=20, env=env)
    return res.stdout.strip()

def send_cmd(ser, cmd, timeout=3.0):
    print(f"\n[ROMMON >>] {cmd}")
    for ch in cmd:
        ser.write(ch.encode("latin1"))
        time.sleep(0.01)
    ser.write(b"\r\n")

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
    print(f"[*] Opening serial port {PORT}...")
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.reset_input_buffer()

    print(f"[*] Power-cycling {POE_INT}...")
    run_switch_cmd(f"configure; set poe interface {POE_INT} disable; commit; exit")
    time.sleep(4.0)
    run_switch_cmd(f"configure; delete poe interface {POE_INT} disable; commit; exit")

    print("[*] Waiting for ROMMON 'ap:' prompt...")
    buf = ""
    t0 = time.time()
    while time.time() - t0 < 60.0:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            sys.stdout.write(c)
            sys.stdout.flush()
            buf += c

            if "Interrupt within" in buf or "Boot Loader" in buf:
                ser.write(b"\x1b\r\n")
                time.sleep(0.1)

            if "ap:" in buf[-10:]:
                print("\n[+] Reached ROMMON prompt!")
                break
        time.sleep(0.05)

    time.sleep(0.5)
    ser.write(b"\r\n")
    time.sleep(0.3)
    if ser.in_waiting:
        ser.read(ser.in_waiting)

    print("\n" + "=" * 60)
    print("QUERYING ROMMON HARDWARE & PHY INFORMATION")
    print("=" * 60)

    send_cmd(ser, "?")
    send_cmd(ser, "set")
    send_cmd(ser, "show_cookie")
    send_cmd(ser, "ether_init")

    # Probe all PHY addresses 0 to 8
    for addr in range(8):
        send_cmd(ser, f"phy_show {addr} 0")
        send_cmd(ser, f"phy_show {addr} 2")
        send_cmd(ser, f"phy_show {addr} 3")

    send_cmd(ser, "ether_display Reg")
    send_cmd(ser, "devs")

    ser.close()
    print("\n[*] Probe complete.")

if __name__ == "__main__":
    main()
