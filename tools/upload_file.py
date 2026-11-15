#!/usr/bin/env python3
r"""
tools/upload_file.py - Upload a local binary file to /tmp/ on the OpenWrt AP via serial console.
Compresses with gzip, streams in hex chunks (\xNN), decompresses on AP, and verifies MD5 checksum.
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import gzip
import hashlib
import os
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600

def upload(local_path, remote_path=None, chunk_size=128):
    if not os.path.exists(local_path):
        print(f"Error: {local_path} does not exist")
        return False

    with open(local_path, "rb") as f:
        orig_data = f.read()

    orig_md5 = hashlib.md5(orig_data).hexdigest()
    if not remote_path:
        remote_path = "/tmp/" + os.path.basename(local_path)

    gz_data = gzip.compress(orig_data, compresslevel=9)
    use_gz = len(gz_data) < len(orig_data)
    data_to_send = gz_data if use_gz else orig_data
    remote_tmp = (remote_path + ".gz") if use_gz else remote_path

    print(f"Uploading {local_path}: {len(orig_data)}B -> {len(data_to_send)}B ({'gzip' if use_gz else 'raw'}), MD5: {orig_md5} -> {remote_path}")

    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    ser.reset_input_buffer()

    # Ensure remote tmp file is cleared
    ser.write(f"rm -f {remote_tmp} {remote_path} && touch {remote_tmp}\n".encode("ascii"))
    time.sleep(0.3)
    ser.reset_input_buffer()

    total_chunks = (len(data_to_send) + chunk_size - 1) // chunk_size
    t_start = time.time()

    for i in range(total_chunks):
        chunk = data_to_send[i * chunk_size : (i + 1) * chunk_size]
        hex_str = "".join(f"\\x{b:02x}" for b in chunk)
        cmd = f"printf '{hex_str}' >> {remote_tmp}\n"
        ser.write(cmd.encode("ascii"))
        ser.flush()

        # Pace at baud rate + buffer processing
        # 9600 baud = ~960 bytes/sec. Wait just enough time for AP to read and process.
        time.sleep(len(cmd) / 960.0 + 0.05)

        pct = (i + 1) * 100 // total_chunks
        sys.stdout.write(f"\rProgress: {pct}% ({i+1}/{total_chunks} chunks, {len(chunk)}B)")
        sys.stdout.flush()

    elapsed = time.time() - t_start
    print(f"\nUpload completed in {elapsed:.1f}s. Decompressing and verifying...")

    time.sleep(0.5)
    ser.reset_input_buffer()

    if use_gz:
        check_cmd = f"gzip -d < {remote_tmp} > {remote_path} && rm -f {remote_tmp} && chmod +x {remote_path} && md5sum {remote_path}\n"
    else:
        check_cmd = f"chmod +x {remote_path} && md5sum {remote_path}\n"

    ser.write(check_cmd.encode("ascii"))
    ser.flush()

    buf = ""
    t0 = time.time()
    while time.time() - t0 < 5.0:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting).decode("latin1", errors="replace")
            if "root@OpenWrt:" in buf:
                break
        time.sleep(0.05)

    ser.close()

    for line in buf.splitlines():
        if remote_path in line and len(line.split()) >= 2:
            remote_md5 = line.split()[0].strip()
            print(f"Remote MD5: {remote_md5}")
            if remote_md5 == orig_md5:
                print("SUCCESS: Checksum matches perfectly!")
                return True
            else:
                print(f"FAILED: MD5 mismatch! Local: {orig_md5}, Remote: {remote_md5}")
                return False

    print(f"Could not verify MD5, output was:\n{buf}")
    return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 upload_file.py <local_path> [remote_path]")
        sys.exit(1)
    remote = sys.argv[2] if len(sys.argv) > 2 else None
    ok = upload(sys.argv[1], remote)
    sys.exit(0 if ok else 1)
