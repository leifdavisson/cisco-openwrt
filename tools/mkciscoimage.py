#!/usr/bin/env python3
"""
mkciscoimage.py - Package and unpack Cisco AP3G2 (Montserrat) MZIP and LZMA firmware images.
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet OpenWrt Porting

Breakthrough Reverse Engineering Discoveries:
  - Both ROMMON (Primary) and Secondary Bootloader use a common 112-byte container header.
  - Header integrity is verified using dual 16-bit CRC16-CCITT checksums:
      * 0x34..0x35: segment_crc16 (chained over header[0x38:112] and compressed payload)
      * 0x36..0x37: header_crc16 (calculated over header[0x00:0x36])
  - Primary Bootloader (ROMMON) loads MZIP containers (magic 0x4D5A4950, type 1, PKZIP) to 0x02003000.
  - Secondary Bootloader loads LZMA containers (magic 0x4C5A4D41, type 3, LZMA) to 0x01003000.
  - Signature verification does not occur in ROMMON or Secondary Bootloader.
"""

import sys
import struct
import zipfile
import lzma
import io

HEADER_SIZE = 112  # 0x70
MAGIC_MZIP = b"MZIP"
MAGIC_LZMA = b"LZMA"

ENTRY_ADDR_MZIP = 0x02003000  # ROMMON load address
ENTRY_ADDR_LZMA = 0x01003000  # Secondary bootloader load address

CRC16_CCITT_TABLE = [
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
]

def crc16_ccitt(data, remainder=0x0):
    crc = (~remainder) & 0xFFFF
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC16_CCITT_TABLE[((crc >> 8) ^ b) & 0xFF]
    return (~crc) & 0xFFFF

CW_TRAILER = (
    b"CW_BEGIN$-gs-openwrt-m$\n"
    b"CW_IMAGE$AP3G2-OPENWRT-M$\n"
    b"CW_FAMILY$AP3G2$\n"
    b"CW_FEATURE$WIRELESS LAN$\n"
    b"CW_MAGIC$$\n"
    b"CW_END$-gs-openwrt-m$\n"
)


def pack_image(input_bin_path, output_path, format_type="mzip", entry_addr=None):
    with open(input_bin_path, "rb") as f:
        raw_code = f.read()

    if b"CW_FAMILY$" not in raw_code:
        print("[*] Appending mandatory Cisco CW_ metadata tags to payload for ROMMON verification...")
        raw_code = raw_code + b"\n" + CW_TRAILER

    uncompressed_len = len(raw_code)
    print(f"[*] Input binary with metadata: {input_bin_path} ({uncompressed_len} bytes)")

    if format_type.lower() == "mzip":
        magic = MAGIC_MZIP
        seg_type = 1
        entry = entry_addr if entry_addr is not None else ENTRY_ADDR_MZIP
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("-", raw_code)
        payload = zip_buf.getvalue()
    elif format_type.lower() in ("raw", "mzip-raw", "uncompressed"):
        magic = MAGIC_MZIP
        seg_type = 0
        entry = entry_addr if entry_addr is not None else 0x01500000
        payload = raw_code
    elif format_type.lower() == "lzma":
        magic = MAGIC_LZMA
        seg_type = 3
        entry = entry_addr if entry_addr is not None else ENTRY_ADDR_LZMA
        # Raw LZMA stream with standard 5-byte props
        payload = lzma.compress(raw_code, format=lzma.FORMAT_ALONE)
    else:
        raise ValueError(f"Unsupported format: {format_type}")

    payload_len = len(payload)
    print(f"[*] Format: {magic.decode()}, Segment Type: {seg_type}, Entry: {hex(entry)}")
    print(f"[*] Compressed payload: {payload_len} bytes")

    hdr = bytearray(HEADER_SIZE)
    hdr[0x00:0x04] = magic
    struct.pack_into(">I", hdr, 0x04, 1)            # Version 1
    struct.pack_into(">I", hdr, 0x08, entry)        # Entry address
    struct.pack_into(">I", hdr, 0x0C, 1)            # Unknown 1
    struct.pack_into(">I", hdr, 0x10, 1)            # Unknown 2

    struct.pack_into(">I", hdr, 0x38, HEADER_SIZE)  # Header size (0x70 / 112)
    struct.pack_into(">I", hdr, 0x3C, entry)        # Load address
    struct.pack_into(">I", hdr, 0x40, seg_type)     # Segment type (1=PKZIP, 3=LZMA)
    struct.pack_into(">I", hdr, 0x44, payload_len)  # Compressed size
    struct.pack_into(">I", hdr, 0x48, uncompressed_len) # Uncompressed size
    if seg_type == 0:
        mem_req = uncompressed_len + 0x00010000  # 64KB BSS clearance
    else:
        mem_req = uncompressed_len + 0x00100000  # 1MB scratch space

    struct.pack_into(">I", hdr, 0x4C, mem_req) # Memory requirement

    # Dual CRC16-CCITT checksums
    c1 = crc16_ccitt(hdr[0x38:HEADER_SIZE], 0x0)
    seg_crc = crc16_ccitt(payload, c1)
    struct.pack_into(">H", hdr, 0x34, seg_crc)

    hdr_crc = crc16_ccitt(hdr[:0x36], 0x0)
    struct.pack_into(">H", hdr, 0x36, hdr_crc)

    print(f"[*] Header CRC16:  0x{hdr_crc:04x}")
    print(f"[*] Segment CRC16: 0x{seg_crc:04x}")

    with open(output_path, "wb") as f:
        f.write(hdr)
        f.write(payload)
        f.write(CW_TRAILER)

    print(f"[+] Output image created: {output_path} ({len(hdr) + len(payload) + len(CW_TRAILER)} bytes)")

def unpack_image(input_path, output_bin_path):
    with open(input_path, "rb") as f:
        data = f.read()

    hdr = data[:HEADER_SIZE]
    magic = hdr[:4]
    payload_len = struct.unpack(">I", hdr[0x44:0x48])[0]
    payload = data[HEADER_SIZE:HEADER_SIZE + payload_len]

    if magic == MAGIC_MZIP:
        zip_buf = io.BytesIO(payload)
        with zipfile.ZipFile(zip_buf, "r") as zf:
            first_file = zf.namelist()[0]
            uncompressed = zf.read(first_file)
    elif magic == MAGIC_LZMA:
        uncompressed = lzma.decompress(payload)
    else:
        raise ValueError(f"Unknown image magic: {magic}")

    with open(output_bin_path, "wb") as f:
        f.write(uncompressed)

    print(f"[+] Extracted {output_bin_path} ({len(uncompressed)} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} pack-mzip <input_bin> <output_mzip> [entry_addr]")
        print(f"       {sys.argv[0]} pack-raw <input_bin> <output_mzip> [entry_addr]")
        print(f"       {sys.argv[0]} pack-lzma <input_bin> <output_lzma> [entry_addr]")
        print(f"       {sys.argv[0]} unpack <input_image> <output_bin>")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd in ("pack", "pack-mzip"):
        entry = int(sys.argv[4], 0) if len(sys.argv) > 4 else ENTRY_ADDR_MZIP
        pack_image(sys.argv[2], sys.argv[3], "mzip", entry)
    elif cmd in ("pack-raw", "pack-uncompressed"):
        entry = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x01500000
        pack_image(sys.argv[2], sys.argv[3], "raw", entry)
    elif cmd == "pack-lzma":
        entry = int(sys.argv[4], 0) if len(sys.argv) > 4 else ENTRY_ADDR_LZMA
        pack_image(sys.argv[2], sys.argv[3], "lzma", entry)
    elif cmd == "unpack":
        unpack_image(sys.argv[2], sys.argv[3])
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)
