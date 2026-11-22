#!/usr/bin/env python3
"""
reboot_to_rommon.py - Reboot Cisco AP into ROMMON prompt via serial console.
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
"""

import os
import sys
import time
import serial

PORT = os.environ.get("SERIAL_PORT", "/dev/ttyUSB0")
BAUD = 9600
ENABLE_PASS = os.environ.get("CISCO_ENABLE_PASS", "Cisco")

def main():
    print(f"[*] Connecting to {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.2)

    # Wake up prompt
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.read(ser.in_waiting or 1)

    ser.write(b"enable\r\n")
    time.sleep(0.3)
    out = ser.read(ser.in_waiting or 1).decode("latin1", errors="replace")
    if "Password:" in out:
        ser.write((ENABLE_PASS + "\r\n").encode("latin1"))
        time.sleep(0.3)
        ser.read(ser.in_waiting or 1)

    print("[*] Sending 'reload' command...")
    ser.write(b"reload\r\n")
    time.sleep(0.8)
    r = ser.read(ser.in_waiting or 1).decode("latin1", errors="replace")
    if "Proceed with reload" in r or "[confirm]" in r:
        ser.write(b"\r\n")
    elif "Save?" in r or "[yes/no]" in r:
        ser.write(b"no\r\n")
        time.sleep(0.5)
        ser.write(b"\r\n")

    print("[*] Monitoring for bootloader interrupt prompt...")
    start = time.time()
    interrupted = False

    while time.time() - start < 60:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            text = chunk.decode("latin1", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()

            if "Interrupt within 5 seconds" in text or "abort boot process" in text:
                print("\n[!] DETECTED INTERRUPT WINDOW! Sending break / escape...")
                for _ in range(5):
                    ser.write(b"\x1b\x03\r\n") # Esc + Ctrl-C + Enter
                    time.sleep(0.05)
                interrupted = True

            if "ap:" in text:
                print("\n[+] SUCCESS: Reached ROMMON 'ap:' prompt!")
                ser.close()
                return

    ser.close()
    if not interrupted:
        print("\n[!] Did not catch interrupt prompt.")

if __name__ == "__main__":
    main()
