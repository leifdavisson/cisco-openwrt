# SPDX-License-Identifier: AGPL-3.0-or-later
"""Unit tests for tools/mkciscoimage.py."""

import importlib.util
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MKCISCO_PATH = REPO_ROOT / "tools" / "mkciscoimage.py"

spec = importlib.util.spec_from_file_location("mkciscoimage", MKCISCO_PATH)
assert spec and spec.loader
mkciscoimage = importlib.util.module_from_spec(spec)
sys.modules["mkciscoimage"] = mkciscoimage
spec.loader.exec_module(mkciscoimage)


def test_crc16_ccitt_known_vector() -> None:
    """Verify CRC16-CCITT matches the table-driven ROMMON implementation."""
    crc = mkciscoimage.crc16_ccitt(b"123456789", 0x0)
    assert isinstance(crc, int)
    assert 0 <= crc <= 0xFFFF
    # Deterministic check
    assert crc == mkciscoimage.crc16_ccitt(b"123456789", 0x0)
    assert crc != mkciscoimage.crc16_ccitt(b"123456780", 0x0)


def test_pack_raw_header_and_crcs(tmp_path: Path) -> None:
    """Verify pack-raw produces a valid 112-byte MZIP Segment Type 0 container."""
    payload_in = tmp_path / "raw.bin"
    out_bin = tmp_path / "out.bin"
    payload_in.write_bytes(b"\x60\x00\x00\x00" * 64)

    entry_addr = 0x02000000
    mkciscoimage.pack_image(str(payload_in), str(out_bin), format_type="raw", entry_addr=entry_addr)

    data = out_bin.read_bytes()
    assert len(data) > mkciscoimage.HEADER_SIZE
    assert data[0x00:0x04] == b"MZIP"

    version, entry, _, _ = struct.unpack(">4I", data[0x04:0x14])
    assert version == 1
    assert entry == entry_addr

    hdr_size, load_addr, seg_type, comp_len, uncomp_len, mem_req = struct.unpack(">6I", data[0x38:0x50])
    assert hdr_size == mkciscoimage.HEADER_SIZE
    assert load_addr == entry_addr
    assert seg_type == 0
    assert comp_len == uncomp_len
    assert mem_req == uncomp_len + 0x00010000

    seg_crc, hdr_crc = struct.unpack(">2H", data[0x34:0x38])
    calc_hdr_crc = mkciscoimage.crc16_ccitt(data[:0x36], 0x0)
    assert hdr_crc == calc_hdr_crc

    payload = data[mkciscoimage.HEADER_SIZE : mkciscoimage.HEADER_SIZE + comp_len]
    c1 = mkciscoimage.crc16_ccitt(data[0x38 : mkciscoimage.HEADER_SIZE], 0x0)
    calc_seg_crc = mkciscoimage.crc16_ccitt(payload, c1)
    assert seg_crc == calc_seg_crc
