#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
extract_cisco_flashfs.py - Extract and SHA-512 verify Cisco Aironet 3702i radio
calibration and firmware files (B5.bin, Q5.bin, E5.bin, 8004.img, 8006.img, etc.)
from a raw 64 MiB NOR flash dump (backups/full_flash_64m.bin).

Cisco flashfs geometry on 3702i (56 MiB flashfs_low partition, 128 KiB erase blocks):
  - Offset 0x00..0x1f: 32-byte erase block header
  - Offset 0x20..0x1ffff: Up to 252 sectors of 0x208 (520) bytes each:
      [0x000..0x001] u16be file_id
      [0x002..0x003] u16be payload_len (<= 0x200)
      [0x004..0x203] payload bytes
      [0x204..0x205] u16be seq_num
      [0x206..0x207] u16be crc16
"""

import argparse
import bz2
import hashlib
import os
import struct
import sys


def scan_flashfs(flash_bytes, partition_end=0x03800000):
    by_fid = {}
    for blk in range(0, min(len(flash_bytes), partition_end), 0x20000):
        magic = struct.unpack(">I", flash_bytes[blk : blk + 4])[0]
        if magic == 0xFFFFFFFF or (magic >> 16) != 0:
            continue
        for sec in range(252):
            soff = blk + 0x20 + sec * 0x208
            if soff + 0x208 > blk + 0x20000:
                break
            fid, plen = struct.unpack(">HH", flash_bytes[soff : soff + 4])
            if fid in (0, 0xFFFF) or plen == 0 or plen > 0x200:
                continue
            seq, _crc = struct.unpack(">HH", flash_bytes[soff + 0x204 : soff + 0x208])
            if seq == 0xFFFF:
                continue
            by_fid.setdefault(fid, {})[seq] = flash_bytes[soff + 4 : soff + 4 + plen]
    return by_fid


def rebuild_file(chunks):
    if not chunks:
        return None
    max_seq = max(chunks.keys())
    if len(chunks) != max_seq + 1:
        return None
    out = bytearray()
    for seq in range(max_seq + 1):
        if seq not in chunks:
            return None
        out.extend(chunks[seq])
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description="Extract and SHA-512 verify Cisco 3702i flashfs radio files.")
    parser.add_argument("--input", default="backups/full_flash_64m.bin", help="Path to 64 MiB NOR flash dump")
    parser.add_argument("--outdir", default="/tmp/cisco_cal", help="Output directory for extracted files")
    parser.add_argument("--decompress", action="store_true", help="Also bzip2-decompress *5.bin and 800*.img files")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    os.makedirs(args.outdir, exist_ok=True)
    by_fid = scan_flashfs(data)

    # Locate the IOS image SHA-512 manifest file (contains 128-hex-char hashes + filenames)
    want_hashes = {}
    for fid, chunks in by_fid.items():
        blob = rebuild_file(chunks)
        if blob and b"Q5.bin" in blob and b"B5.bin" in blob:
            for line in blob.decode("latin1", errors="replace").splitlines():
                parts = line.strip().split()
                if len(parts) == 2 and len(parts[0]) == 128:
                    want_hashes[parts[0]] = parts[1]
            break

    if not want_hashes:
        print("[-] Could not find SHA-512 manifest inside flashfs.", file=sys.stderr)
        return 1

    extracted = 0
    for fid in sorted(by_fid.keys()):
        blob = rebuild_file(by_fid[fid])
        if blob is None:
            continue
        digest = hashlib.sha512(blob).hexdigest()
        if digest in want_hashes:
            name = want_hashes[digest].lstrip("/")
            real_outdir = os.path.realpath(args.outdir)
            out_path = os.path.realpath(os.path.join(real_outdir, name))
            if os.path.commonpath([real_outdir, out_path]) != real_outdir:
                print(f"[-] Skipping unsafe manifest path: {name!r}", file=sys.stderr)
                continue
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, "wb") as out_f:
                out_f.write(blob)
            extracted += 1
            extra = ""
            if args.decompress and blob[:3] == b"BZh":
                raw = bz2.decompress(blob)
                dec_path = out_path + ".raw"
                with open(dec_path, "wb") as dec_f:
                    dec_f.write(raw)
                extra = f" -> decompressed {len(raw)} bytes"
            print(f"[+] VERIFIED SHA-512: {name:26s} (fid=0x{fid:04x}, {len(blob):7d} bytes){extra}")

    print(f"[*] Extracted and SHA-512 verified {extracted}/{len(want_hashes)} manifest files into {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
