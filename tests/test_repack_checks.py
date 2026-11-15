# SPDX-License-Identifier: AGPL-3.0-or-later
"""Safety checks for Cisco 3702i DTS and OpenWrt image repacking parameters."""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def test_dts_uses_rommon_ccsrbar() -> None:
    """Ensure cisco-3702i.dts maps CCSR at 0xe0000000 (matching ROMMON TLB1)."""
    dts_text = (REPO_ROOT / "dts" / "cisco-3702i.dts").read_text(encoding="utf-8")
    assert "soc@e0000000" in dts_text
    assert "/soc@e0000000/serial@4500:9600n8" in dts_text


def test_repack25_load_addr_avoids_self_overwrite() -> None:
    """Ensure repack_openwrt25_image.py packs above 0x015613cc so vmlinux decompression won't overwrite itself."""
    script_text = (REPO_ROOT / "tools" / "repack_openwrt25_image.py").read_text(encoding="utf-8")
    match = re.search(r"pack-raw\s+\{OUTPUT_RAW\}\s+\{FINAL_CISCO_BIN\}\s+(0x[0-9a-fA-F]+)", script_text)
    assert match is not None, "Could not find pack-raw load address in repack_openwrt25_image.py"
    load_addr = int(match.group(1), 16)
    # ROMMON valid RAM window is [0x00003000, 0x06000000], and 25.12.6 vmlinux memsz ends at ~0x015613cc
    assert 0x01800000 <= load_addr < 0x06000000, f"Unsafe load_addr 0x{load_addr:08x}"
