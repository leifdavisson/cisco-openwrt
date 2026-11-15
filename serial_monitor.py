#!/usr/bin/env python3
"""
Interactive Serial Console Assistant for Cisco AP Flashing
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet Standalone Deployment
References:
  - pySerial Documentation: https://pyserial.readthedocs.io/
  - Cisco Aironet Bootloader (ROMMON) Reference: https://www.cisco.com/c/en/us/support/wireless/aironet-3700-series/series.html
"""

import sys
import time
import threading
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600

def reader_thread(ser):
    while True:
        try:
            line = ser.readline()
            if line:
                sys.stdout.write(line.decode("latin1", errors="replace"))
                sys.stdout.flush()
        except Exception:
            break

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else BAUD

    print(f"[*] Opening serial console on {port} @ {baud} 8N1...")
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except Exception as e:
        print(f"[!] Error opening {port}: {e}")
        sys.exit(1)

    print("[*] Connected! Type commands and press Enter.")
    print("[*] To exit, press Ctrl+C.\n" + "="*50)

    t = threading.Thread(target=reader_thread, args=(ser,), daemon=True)
    t.start()

    try:
        while True:
            cmd = input()
            ser.write((cmd + "\r\n").encode("latin1"))
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\n[*] Exiting serial console.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
