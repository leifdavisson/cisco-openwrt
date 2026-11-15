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


def test_repack25_load_addr_matches_wrapper_link_address() -> None:
    """The 25.12.6 simpleImage wrapper is not PIE: ROMMON must load it at its link address, above the kernel."""
    script_text = (REPO_ROOT / "tools" / "repack_openwrt25_image.py").read_text(encoding="utf-8")
    link = re.search(r"^WRAPPER_LINK_ADDR\s*=\s*(0x[0-9a-fA-F]+)", script_text, re.M)
    memsz = re.search(r"^KERNEL_MEMSIZE\s*=\s*(0x[0-9a-fA-F]+)", script_text, re.M)
    assert link is not None and memsz is not None
    load_addr = int(link.group(1), 16)
    # Verified on hardware (AIR-CAP3702I-E-K9, 2026-11-15) and with tools/wrapper_info.py
    assert load_addr == 0x01600000, f"25.12.6 wrapper is linked at 0x01600000, not 0x{load_addr:08x}"
    assert int(memsz.group(1), 16) < load_addr < 0x06000000
    assert "pack-raw {OUTPUT_RAW} {FINAL_CISCO_BIN} {WRAPPER_LINK_ADDR:#010x}" in script_text
