# SPDX-License-Identifier: AGPL-3.0-or-later
"""Unit tests for tftp_server.py."""

import importlib.util
import socket
import struct
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TFTP_PATH = REPO_ROOT / "tftp_server.py"

spec = importlib.util.spec_from_file_location("tftp_server", TFTP_PATH)
assert spec and spec.loader
tftp_server = importlib.util.module_from_spec(spec)
sys.modules["tftp_server"] = tftp_server
spec.loader.exec_module(tftp_server)


def test_serve_file_transfers_exact_bytes(tmp_path: Path) -> None:
    """Verify serve_file sends 512-byte DATA blocks and handles ACKs."""
    payload = b"ABCD" * 300  # 1200 bytes -> 3 blocks (512, 512, 176)
    test_file = tmp_path / "test.bin"
    test_file.write_bytes(payload)

    client_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client_sock.bind(("127.0.0.1", 0))
    client_sock.settimeout(3.0)
    client_addr = client_sock.getsockname()

    t = threading.Thread(target=tftp_server.serve_file, args=(str(test_file), client_addr), daemon=True)
    t.start()

    received = bytearray()
    while True:
        pkt, server_addr = client_sock.recvfrom(1024)
        op, block = struct.unpack(">HH", pkt[:4])
        assert op == tftp_server.OP_DATA
        data = pkt[4:]
        received.extend(data)
        client_sock.sendto(struct.pack(">HH", tftp_server.OP_ACK, block), server_addr)
        if len(data) < 512:
            break

    t.join(timeout=2.0)
    client_sock.close()
    time.sleep(0.05)
    assert bytes(received) == payload


def test_resolve_safe_path_blocks_traversal(tmp_path: Path) -> None:
    """Verify resolve_safe_path allows valid files and rejects path traversal."""
    valid = tmp_path / "openwrt-3702i.bin"
    valid.write_bytes(b"ok")
    outside = tmp_path.parent / "secret.txt"
    outside.write_bytes(b"nope")
    try:
        assert tftp_server.resolve_safe_path(str(tmp_path), "openwrt-3702i.bin") == str(valid.resolve())
        assert tftp_server.resolve_safe_path(str(tmp_path), "/openwrt-3702i.bin") == str(valid.resolve())
        assert tftp_server.resolve_safe_path(str(tmp_path), "../secret.txt") is None
        assert tftp_server.resolve_safe_path(str(tmp_path), "../../etc/passwd") is None
    finally:
        outside.unlink(missing_ok=True)
