#!/usr/bin/env python3
"""
tools/upload_b64.py - Upload a binary file to OpenWrt using gzip and ucode b64dec.
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import base64
import gzip
import hashlib
import os
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600

def wait_for_prompt(ser, timeout=5.0):
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            c = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            buf += c
            if "root@OpenWrt:" in buf:
                time.sleep(0.05)
                return True, buf
        time.sleep(0.05)
    return False, buf

def upload(local_path, remote_path=None):
    if not os.path.exists(local_path):
        print(f"Error: {local_path} does not exist")
        return False

    with open(local_path, "rb") as f:
        orig_data = f.read()

    orig_md5 = hashlib.md5(orig_data).hexdigest()
    if not remote_path:
        remote_path = "/tmp/" + os.path.basename(local_path)

    gz_data = gzip.compress(orig_data, compresslevel=9)
    b64_str = base64.b64encode(gz_data).decode("ascii")

    print(f"Uploading {local_path} ({len(orig_data)}B -> {len(gz_data)}B gz -> {len(b64_str)} chars b64) to {remote_path}...")

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.reset_input_buffer()

    # Ensure clean prompt
    ser.write(b"\x03\n")
    wait_for_prompt(ser, 2.0)
    ser.reset_input_buffer()

    # Write b64 file using heredoc
    ser.write(b"cat << 'EOF' > /tmp/upload.b64\n")
    time.sleep(0.1)

    # Send b64 in lines of 64 characters
    lines = [b64_str[i:i+64] for i in range(0, len(b64_str), 64)]
    for i, line in enumerate(lines):
        ser.write((line + "\n").encode("ascii"))
        time.sleep(len(line) / 800.0 + 0.02)
        sys.stdout.write(f"\rSending: {i+1}/{len(lines)} lines")
        sys.stdout.flush()

    ser.write(b"\nEOF\n")
    ser.flush()
    ok, pbuf = wait_for_prompt(ser, 5.0)
    if not ok:
        print("\nFailed waiting for prompt after EOF!")
        ser.close()
        return False

    print("\nDecoding with ucode and decompressing...")
    ser.reset_input_buffer()

    decode_cmd = (
        f"ucode -e 'let fs = require(\"fs\"); let f = fs.open(\"{remote_path}.gz\", \"w\"); f.write(b64dec(fs.readfile(\"/tmp/upload.b64\"))); f.close();' "
        f"&& gzip -d < {remote_path}.gz > {remote_path} "
        f"&& rm -f {remote_path}.gz /tmp/upload.b64 "
        f"&& chmod +x {remote_path} "
        f"&& md5sum {remote_path}\n"
    )
    ser.write(decode_cmd.encode("ascii"))
    ser.flush()

    ok, buf = wait_for_prompt(ser, 6.0)
    ser.close()

    for line in buf.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[1] == remote_path and len(parts[0]) == 32:
            remote_md5 = parts[0]
            print(f"Remote MD5: {remote_md5}")
            if remote_md5 == orig_md5:
                print("SUCCESS: Checksum matches perfectly!")
                return True
            else:
                print(f"FAILED: MD5 mismatch! Local: {orig_md5}, Remote: {remote_md5}")
                return False

    print(f"Verification output:\n{buf}")
    return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: upload_b64.py <local_path> [remote_path]")
        sys.exit(1)
    remote = sys.argv[2] if len(sys.argv) > 2 else None
    ok = upload(sys.argv[1], remote)
    sys.exit(0 if ok else 1)
