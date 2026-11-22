#!/usr/bin/env python3
"""
Simple TFTP Server for Cisco AP Firmware Flashing
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
Project: Cisco Aironet Standalone Deployment
References:
  - Python Socket Programming: https://docs.python.org/3/library/socket.html
  - Cisco Aironet 3700 Series: https://www.cisco.com/c/en/us/support/wireless/aironet-3700-series/series.html
"""

import os
import sys
import socket
import struct

TFTP_PORT = 69
DEFAULT_DIR = os.path.expanduser("~/Downloads")

# TFTP OpCodes
OP_RRQ = 1
OP_WRQ = 2
OP_DATA = 3
OP_ACK = 4
OP_ERROR = 5

def resolve_safe_path(serve_dir: str, requested_name: str) -> str | None:
    """Resolve requested_name inside serve_dir, returning None on path traversal."""
    real_serve = os.path.realpath(serve_dir)
    clean_name = requested_name.lstrip("/")
    actual_file = clean_name
    if clean_name == "ap3g2-k9w7-tar.default" or not os.path.exists(os.path.join(real_serve, clean_name)):
        for candidate in os.listdir(real_serve):
            if candidate.startswith("ap3g2-k9w7-tar") and candidate.endswith(".tar"):
                actual_file = candidate
                break

    candidate_path = os.path.realpath(os.path.join(real_serve, actual_file))
    if os.path.commonpath([real_serve, candidate_path]) != real_serve:
        return None
    return candidate_path


def start_tftp_server(serve_dir=DEFAULT_DIR, bind_ip="0.0.0.0"):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((bind_ip, TFTP_PORT))
        print(f"[*] TFTP server listening on {bind_ip}:{TFTP_PORT}")
        print(f"[*] Serving files from: {serve_dir}")
        print("[*] Press Ctrl+C to stop.")
    except PermissionError:
        print("[!] Port 69 requires root privileges. Run with sudo or setcap.")
        sys.exit(1)

    while True:
        try:
            data, client_addr = sock.recvfrom(2048)
            if len(data) < 4:
                continue

            opcode = struct.unpack(">H", data[:2])[0]
            if opcode != OP_RRQ:
                # We only serve read requests (downloads from AP)
                continue

            parts = data[2:].split(b"\x00")
            filename = parts[0].decode("latin1", errors="replace")
            filepath = resolve_safe_path(serve_dir, filename)
            if filepath is None:
                print(f"[!] Blocked path traversal attempt from {client_addr}: {filename!r}")
                err = struct.pack(">HH", OP_ERROR, 2) + b"Access violation\x00"
                sock.sendto(err, client_addr)
                continue

            print(f"[+] Client {client_addr} requested: {filename} -> Serving: {filepath}")

            if not os.path.exists(filepath):
                err = struct.pack(">HH", OP_ERROR, 1) + b"File not found\x00"
                sock.sendto(err, client_addr)
                continue

            # Serve file via dedicated socket
            serve_file(filepath, client_addr)
        except KeyboardInterrupt:
            print("\n[*] Stopping TFTP server.")
            break
        except Exception as e:
            print(f"[!] Server error: {e}")

def serve_file(filepath, client_addr):
    xfer_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    xfer_sock.settimeout(5.0)

    block_num = 1
    total_bytes = 0
    file_size = os.path.getsize(filepath)

    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(512)
            packet = struct.pack(">HH", OP_DATA, block_num & 0xFFFF) + chunk

            retries = 5
            acked = False
            while retries > 0:
                xfer_sock.sendto(packet, client_addr)
                try:
                    ack_data, _ = xfer_sock.recvfrom(512)
                    if len(ack_data) >= 4:
                        ack_op, ack_block = struct.unpack(">HH", ack_data[:4])
                        if ack_op == OP_ACK and ack_block == (block_num & 0xFFFF):
                            acked = True
                            break
                except socket.timeout:
                    retries -= 1

            if not acked:
                print(f"[!] Transfer failed to {client_addr} at block {block_num}")
                break

            total_bytes += len(chunk)
            if block_num % 500 == 0 or len(chunk) < 512:
                pct = (total_bytes / file_size) * 100 if file_size else 100
                print(f"    Transferred: {total_bytes // 1024} KB / {file_size // 1024} KB ({pct:.1f}%)")

            if len(chunk) < 512:
                print(f"[+] Transfer complete ({total_bytes} bytes sent).")
                break

            block_num += 1

    xfer_sock.close()

if __name__ == "__main__":
    serve_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR
    start_tftp_server(serve_dir)
