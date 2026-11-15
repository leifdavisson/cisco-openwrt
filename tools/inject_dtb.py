#!/usr/bin/env python3
"""
inject_dtb.py - Patch a PowerPC zImage wrapper with a custom Device Tree Blob (DTB).
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting
"""

import sys
import os
import struct

def inject(raw_zimage_path, dtb_path, output_path):
    with open(raw_zimage_path, "rb") as f:
        zimage = bytearray(f.read())

    with open(dtb_path, "rb") as f:
        dtb = f.read()

    # Find the embedded DTB in the zImage wrapper
    dtb_offset = zimage.find(b"\xd0\x0d\xfe\xed")
    if dtb_offset == -1:
        raise ValueError("Could not find DTB magic 0xd00dfeed in zImage!")

    (orig_dtb_size,) = struct.unpack(">I", zimage[dtb_offset + 4 : dtb_offset + 8])
    print(f"[*] Found existing DTB at offset {hex(dtb_offset)} ({dtb_offset})")
    print(f"[*] Original declared DTB size: {orig_dtb_size} bytes")

    # Find where the payload / code begins after the zero-padded DTB area
    scan_start = dtb_offset + orig_dtb_size
    next_nonzero = len(zimage)
    for i in range(scan_start, min(scan_start + 65536, len(zimage))):
        if zimage[i] != 0:
            next_nonzero = i
            break

    max_dtb_space = next_nonzero - dtb_offset
    dtb_len = len(dtb)

    print(f"[*] Available space for DTB before payload: {max_dtb_space} bytes (up to {hex(next_nonzero)})")
    print(f"[*] New DTB size: {dtb_len} bytes")

    if dtb_len > max_dtb_space:
        raise ValueError(f"DTB too large! {dtb_len} > {max_dtb_space}")

    # Clear old DTB area with zeros
    zimage[dtb_offset:next_nonzero] = b"\x00" * max_dtb_space
    # Inject new DTB
    zimage[dtb_offset : dtb_offset + dtb_len] = dtb

    with open(output_path, "wb") as f:
        f.write(zimage)

    print(f"[+] Custom kernel created: {output_path} ({os.path.getsize(output_path)} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <raw_zimage> <new_dtb> <output_zimage>")
        sys.exit(1)
    inject(sys.argv[1], sys.argv[2], sys.argv[3])
