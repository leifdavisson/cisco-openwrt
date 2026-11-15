#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
poe_bounce_to_rommon.py - Bounce PoE on the Juniper switch port and stop at the
Cisco ROMMON 'ap:' prompt (does NOT boot OpenWrt). Reuses switch settings from
powercycle_and_boot.py.
"""
import sys
import time
import serial

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from powercycle_and_boot import run_switch_cmd, POE_INT, PORT, BAUD  # noqa: E402


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print(f"[*] Disabling PoE on {POE_INT}")
    print(run_switch_cmd(f"configure; set poe interface {POE_INT} disable; commit; exit"))
    time.sleep(5.0)
    ser.reset_input_buffer()
    print(f"[*] Re-enabling PoE on {POE_INT}")
    print(run_switch_cmd(f"configure; delete poe interface {POE_INT} disable; commit; exit"))

    buf = ""
    t0 = time.time()
    last_esc = 0.0
    while time.time() - t0 < 150:
        c = ser.read(512).decode("latin1", errors="replace")
        if c:
            sys.stdout.write(c)
            sys.stdout.flush()
            buf += c
        if time.time() - last_esc > 0.3:
            ser.write(b"\x1b")  # keep interrupting the autoboot countdown
            last_esc = time.time()
        if buf.rstrip().endswith("ap:"):
            print("\n[+] At ROMMON ap: prompt")
            return 0
    print("\n[!] Timed out waiting for ap: prompt")
    return 1


if __name__ == "__main__":
    sys.exit(main())
