#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
rommon_clean_flash.py - Remove the Cisco software from flash: for a standalone OpenWrt AP.

Run it at the ROMMON `ap:` prompt (9600 baud). It walks flash:/, deletes every file and
directory except the names given with --keep, and answers ROMMON's per-file (y/n) prompt.
Without --delete it only lists what it would remove.

    python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs
    python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs --delete

Always keep:
  * your OpenWrt image (the file BOOT points at)
  * env_vars            - the ROMMON environment, including BOOT
  * private-multiple-fs - the unit's certificates (tiny; needed if it ever runs Cisco software again)

THIS IS PERMANENT. Once the IOS image is gone the only way back to Cisco software is to
copy an image you have obtained yourself through ROMMON. The bootloader is not touched.
The bench version of this script ran on an AIR-CAP2602I (free space 0.5 MiB -> 23.8 MiB, AP still
cold-boots OpenWrt): docs/logs/2602i-clean-flash/. This tidied version has not run on hardware yet.
"""
import argparse
import re
import sys
import time

import serial


class Rommon:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, 9600, timeout=0.2)

    def tx(self, text: str, timeout: float = 40.0, until: tuple = ("ap:",)) -> str:
        self.ser.reset_input_buffer()
        for ch in text:
            self.ser.write(ch.encode("latin1"))
            time.sleep(0.012)
        self.ser.write(b"\r")
        buf, t0 = "", time.time()
        while time.time() - t0 < timeout:
            buf += self.ser.read(512).decode("latin1", errors="replace")
            if len(buf) > len(text) + 1 and any(buf.rstrip(" ").endswith(u) for u in until):
                break
        return buf.replace("\r", "")

    def listing(self, path: str) -> tuple:
        out = self.tx(f"dir {path}")
        entries = []
        for line in out.split("\n"):
            m = re.match(r"\s*\d+\s+([d-])[rwx-]+\s+(\d+)\s+<date>\s+(\S+)\s*$", line)
            if m:
                entries.append((m.group(1) == "d", int(m.group(2)), m.group(3)))
        free = re.search(r"(\d+) bytes available", out)
        return entries, int(free.group(1)) if free else None

    def confirm(self, command: str, done_word: str) -> bool:
        reply = self.tx(command, until=("(y/n)?", "ap:"))
        if "(y/n)?" in reply:
            reply += self.tx("y")
        return done_word in reply


def clean(rommon: Rommon, path: str, keep: set, delete: bool, top: bool = True) -> int:
    removed = 0
    for is_dir, size, name in rommon.listing(path)[0]:
        full = f"{path}{name}"
        if top and name in keep:
            print(f"keep    {full} ({size})")
            continue
        if is_dir:
            removed += clean(rommon, full + "/", keep, delete, top=False)
            if delete:
                rommon.confirm(f"rmdir {full}", "emoved")
            print(f"{'rmdir  ' if delete else 'would rmdir'} {full}")
        elif not delete:
            print(f"would delete {full} ({size})")
            removed += size
        elif rommon.confirm(f"delete {full}", "deleted"):
            print(f"delete  {full} ({size})", flush=True)
            removed += size
        else:
            print(f"FAILED  {full}", flush=True)
    return removed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep", nargs="+", required=True, help="names in flash:/ that must stay")
    ap.add_argument("--delete", action="store_true", help="really delete (default: list only)")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    args = ap.parse_args()
    keep = set(args.keep)
    if "env_vars" not in keep:
        print("[!] refusing to run without 'env_vars' in --keep (it holds the BOOT variable)")
        return 1
    rommon = Rommon(args.port)
    if "ap:" not in rommon.tx("", 3.0):
        print("[!] no 'ap:' prompt at 9600 baud")
        return 1
    entries, before = rommon.listing("flash:")
    missing = keep - {name for _, _, name in entries}
    if missing:
        print(f"[!] not on flash:/ : {', '.join(sorted(missing))} - check the names")
        return 1
    total = clean(rommon, "flash:/", keep, args.delete)
    entries, after = rommon.listing("flash:")
    print(f"\n{'removed' if args.delete else 'would remove'} {total} bytes; free before={before} after={after}")
    print("on flash:/ now:", ", ".join(name for _, _, name in entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
