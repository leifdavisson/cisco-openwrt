#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
wrapper_info.py - Show where an OpenWrt mpc85xx simpleImage MUST be loaded.

The PowerPC boot wrapper at the front of OpenWrt's `*-initramfs-kernel.bin` is linked
at a fixed address (it is NOT position independent: GOT[0] == 0, no dynamic relocations).
ROMMON has to load it at exactly that address, and the uncompressed kernel (loaded at 0)
has to end below it. This tool reads both numbers out of an official image:

    python3 tools/wrapper_info.py openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin

Known results:
    23.05.5 (Linux 5.15.167): wrapper linked at 0x01500000, kernel load size 0x00cadd08
    25.12.6 (Linux 6.12.112): wrapper linked at 0x01600000, kernel load size 0x01523590
"""
import collections
import lzma
import struct
import sys

UIMAGE_MAGIC = 0x27051956
XZ_MAGIC = b"\xfd7zXZ\x00"
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def _s16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def analyse(image: bytes) -> dict:
    info: dict = {}
    if struct.unpack(">I", image[:4])[0] == UIMAGE_MAGIC:
        info["uimage_load"], info["uimage_entry"] = struct.unpack(">II", image[16:24])
        info["uimage_name"] = image[32:64].rstrip(b"\0").decode("latin1")
        image = image[64:]
    dtb_off = image.find(FDT_MAGIC)
    xz_off = image.find(XZ_MAGIC, dtb_off)
    if dtb_off < 0 or xz_off < 0:
        raise ValueError("not a simpleImage: no embedded DTB / XZ payload found")
    info["dtb_offset"], info["xz_offset"], info["xz_size"] = dtb_off, xz_off, len(image) - xz_off
    info["dtb_slot"] = xz_off - dtb_off

    def word(off: int) -> int:
        return struct.unpack(">I", image[off:off + 4])[0]

    # crt0.S: "b _zimage_start"; there: bcl 20,31,.+4 ; mflr r10 ; addis r11,r10,HA ; lwz r11,LO(r11) -> GOT[0]
    start = word(0) & 0x03FFFFFC
    p_base = start + 4
    got0_off = p_base + (word(start + 8) & 0xFFFF) * 0x10000 + _s16(word(start + 12) & 0xFFFF)
    info["pie"] = word(got0_off) != 0
    # The words just below GOT[0] are link-time absolute pointers into the wrapper itself.
    ptrs = [word(o) for o in range(got0_off - 64 * 4, got0_off, 4)]
    megs = collections.Counter(p >> 20 for p in ptrs if 0x00100000 <= p < 0x10000000)
    info["link_address"] = megs.most_common(1)[0][0] << 20 if megs else None

    vmlinux = lzma.LZMADecompressor(format=lzma.FORMAT_XZ).decompress(image[xz_off:])
    if vmlinux[:4] != b"\x7fELF":
        raise ValueError("XZ payload is not an ELF vmlinux")
    phoff = struct.unpack(">I", vmlinux[28:32])[0]
    phentsize, phnum = struct.unpack(">HH", vmlinux[42:46])
    for i in range(phnum):
        p_type, _off, _vaddr, paddr, filesz, memsz = struct.unpack(">6I", vmlinux[phoff + i * phentsize:][:24])
        if p_type == 1:  # PT_LOAD
            info["kernel_paddr"], info["kernel_loadsize"], info["kernel_memsize"] = paddr, filesz, memsz
            break
    return info


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "rb") as f:
        info = analyse(f.read())
    for key, value in info.items():
        print(f"{key:16} = {value:#010x}" if isinstance(value, int) and not isinstance(value, bool) else f"{key:16} = {value}")
    link = info["link_address"]
    if info["pie"] or link is None:
        print("\nWrapper looks position independent: any load address above the kernel should work.")
        return 0
    ok = info["kernel_loadsize"] < link
    print(f"\n=> pack with:  mkciscoimage.py pack-raw <raw> <out> {link:#010x}")
    print(f"   kernel (0x0..{info['kernel_loadsize']:#x}) {'fits below' if ok else 'OVERLAPS'} the wrapper at {link:#x}")
    if "uimage_load" in info and info["uimage_load"] != link:
        print(f"   note: the uImage header says {info['uimage_load']:#x}; that is not where this wrapper is linked.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
