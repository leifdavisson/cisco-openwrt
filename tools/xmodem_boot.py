#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
xmodem_boot.py - RAM-boot an image on a Cisco 3702i over the console cable only.

No TFTP server, no network and no firewall rules needed: the ROMMON has an XMODEM
"file system", so `boot xmodem:<name>` loads an MZIP image straight into RAM.
At 115200 baud an 8 MiB image takes about 12 minutes (~11 kB/s, XMODEM-1K).

Preconditions:
  * the AP sits at the ROMMON `ap:` prompt at 9600 baud (tools/poe_bounce_to_rommon.py)
  * `ether_init` / `arp` have NOT been run since power-on. If the Ethernet (DPAA) was
    initialised, `boot xmodem:` answers "Rebooting system to reset DPAA..." and the
    bootloader restarts and autoboots whatever BOOT points at.

Usage:
    # try an image from RAM (nothing is written to the AP)
    python3 tools/xmodem_boot.py ~/Downloads/openwrt-3702i.bin --log boot.log

    # install it: copy to flash:, point BOOT at it, boot (the Cisco software stays on flash)
    python3 tools/xmodem_boot.py ~/Downloads/openwrt-3702i.bin --install --wait-rommon

With --wait-rommon the script waits for you to power-cycle the AP and stops the
bootloader itself (ESC during "flashfs fsck"), so no prior ROMMON session is needed.

After the transfer the console is captured to --log. The boot wrapper and the very first
kernel lines still come out at the transfer speed; the script falls back to 9600 baud
(the speed in the DTS bootargs) as soon as the output turns into garbage.
"""
import argparse
import binascii
import sys
import time

import serial

STX, EOT, ACK, CAN = 0x02, 0x04, 0x06, 0x18
BLOCK = 1024


def rommon_cmd(ser: serial.Serial, text: str, wait: float) -> bytes:
    ser.reset_input_buffer()
    for ch in text:
        ser.write(ch.encode("latin1"))
        time.sleep(0.015)
    ser.write(b"\r")
    buf, t0 = b"", time.time()
    while time.time() - t0 < wait:
        buf += ser.read(256)
        if len(buf) > len(text) + 2 and buf.rstrip(b" ").endswith(b"ap:"):
            break  # prompt is back: the command has finished
    return buf


def send_xmodem_1k(ser: serial.Serial, data: bytes) -> None:
    t0 = time.time()
    while ser.read(1) != b"C":  # receiver asks for CRC mode
        if time.time() - t0 > 90:
            raise RuntimeError("receiver never sent 'C' - is ROMMON waiting in 'boot xmodem:'?")
    ser.reset_input_buffer()
    blocks = (len(data) + BLOCK - 1) // BLOCK
    t0 = time.time()
    for i in range(blocks):
        chunk = data[i * BLOCK:(i + 1) * BLOCK].ljust(BLOCK, b"\x1a")
        seq = (i + 1) & 0xFF
        packet = bytes([STX, seq, 0xFF - seq]) + chunk + binascii.crc_hqx(chunk, 0).to_bytes(2, "big")
        for _attempt in range(15):
            ser.write(packet)
            ser.flush()
            reply, t1 = b"", time.time()
            while not reply and time.time() - t1 < 15:
                reply = ser.read(1)
                if reply == b"C":
                    reply = b""
            if reply and reply[0] == ACK:
                break
            if reply and reply[0] == CAN:
                raise RuntimeError(f"receiver cancelled at block {i}")
        else:
            raise RuntimeError(f"block {i}: too many retries")
        if i % 500 == 0:
            elapsed = max(time.time() - t0, 1.0)
            print(f"[*] block {i}/{blocks}  {elapsed:.0f}s  {(i + 1) * BLOCK / elapsed:.0f} B/s", flush=True)
    for _ in range(10):
        ser.write(bytes([EOT]))
        reply = ser.read(1)
        if reply and reply[0] == ACK:
            break
    print(f"[+] transfer done in {time.time() - t0:.0f}s", flush=True)


def capture(ser: serial.Serial, seconds: float, log_path: str, fast_baud: int) -> None:
    ser.timeout = 0.3
    garbage_chunks = 0
    end = time.time() + seconds
    with open(log_path, "ab", buffering=0) as log:
        while time.time() < end:
            chunk = ser.read(512)
            if not chunk:
                continue
            log.write(chunk)
            junk = sum(1 for b in chunk if not (32 <= b < 127 or b in (9, 10, 13, 27)))
            garbage_chunks = garbage_chunks + 1 if junk * 3 > len(chunk) and len(chunk) >= 8 else 0
            if garbage_chunks >= 2:  # wrong speed: flip between the transfer baud and 9600
                ser.baudrate = 9600 if ser.baudrate == fast_baud else fast_baud
                log.write(b"\n===== baud -> %d =====\n" % ser.baudrate)
                garbage_chunks = 0


def wait_for_rommon(ser: serial.Serial, timeout: float = 240.0) -> bool:
    """Send ESC until the bootloader reports the aborted boot, then return at the ap: prompt."""
    print("[*] power-cycle the AP now (unplug / replug PoE) - waiting for the bootloader...", flush=True)
    buf, t0, last = "", time.time(), 0.0
    while time.time() - t0 < timeout:
        buf += ser.read(512).decode("latin1", errors="replace")
        if "aborted" in buf and "ap:" in buf[buf.rfind("aborted"):]:
            time.sleep(1.0)
            ser.write(b"\r")  # flush the echoed ESCs as one unknown command
            time.sleep(1.5)
            ser.reset_input_buffer()
            return True
        if "aborted" not in buf and time.time() - last > 0.3:
            ser.write(b"\x1b")
            last = time.time()
    return False


def rommon_env(ser: serial.Serial) -> dict:
    text = rommon_cmd(ser, "set", 4.0).decode("latin1", errors="replace")
    return dict(line.strip().split("=", 1) for line in text.replace("\r", "").split("\n") if "=" in line)


def install(ser: serial.Serial, data: bytes, name: str, baud: int) -> int:
    """copy xmodem: -> flash:, then BOOT=flash:/<name>. BOOT is only changed after a successful copy."""
    previous_boot = rommon_env(ser).get("BOOT", "")
    print(f"[*] current BOOT={previous_boot}")
    listing = rommon_cmd(ser, "dir flash:", 8.0).decode("latin1", errors="replace")
    if f" {name}" in listing:
        print(f"[!] flash:/{name} already exists - pick another --name or delete it in ROMMON first")
        return 1
    free = [int(tok) for line in listing.split("\n") if "bytes available" in line for tok in line.split()[:1]]
    if free and free[0] < len(data) + 0x40000:
        print(f"[!] only {free[0]} bytes free on flash:, the image needs {len(data)}")
        return 1
    if baud != 9600:
        rommon_cmd(ser, f"set_baud {baud}", 3.0)
        ser.baudrate = baud
    answer = rommon_cmd(ser, f"copy xmodem:{name} flash:/{name}", 5.0)
    if b"Xmodem" not in answer:
        print("[!] ROMMON did not start an XMODEM receive:\n" + answer.decode("latin1", errors="replace"))
        return 1
    ser.timeout = 1
    print(f"[*] copying {len(data)} bytes to flash:/{name} at {baud} baud (about 14 minutes for 8 MiB)", flush=True)
    send_xmodem_1k(ser, data)
    ser.timeout = 0.2
    buf, t0 = b"", time.time()
    while b"successfully copied" not in buf and time.time() - t0 < 180:
        buf += ser.read(256)
    if b"successfully copied" not in buf:
        print("[!] ROMMON did not confirm the copy; BOOT was NOT changed:\n" + buf.decode("latin1", errors="replace"))
        return 1
    if baud != 9600:
        rommon_cmd(ser, "set_baud 9600", 3.0)
        ser.baudrate = 9600
    rommon_cmd(ser, f"set BOOT flash:/{name}", 3.0)
    if rommon_env(ser).get("BOOT") != f"flash:/{name}":
        print("[!] BOOT variable was not accepted")
        return 1
    print(f"[+] installed. To go back to the Cisco software:  ap: set BOOT {previous_boot}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="MZIP image built with mkciscoimage.py pack-raw")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200, help="transfer speed (ROMMON set_baud)")
    ap.add_argument("--capture", type=float, default=180.0, help="seconds of console to record after the transfer")
    ap.add_argument("--log", default="xmodem_boot.log")
    ap.add_argument("--install", action="store_true", help="copy to flash:, set BOOT and boot instead of a RAM boot")
    ap.add_argument("--name", default="openwrt-3702i.bin", help="file name on flash: for --install")
    ap.add_argument("--wait-rommon", action="store_true", help="wait for a power-cycle and stop the bootloader with ESC")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        data = f.read()
    ser = serial.Serial(args.port, 9600, timeout=0.2)
    if b"ap:" not in rommon_cmd(ser, "", 2.0):
        if not args.wait_rommon:
            print("[!] no 'ap:' prompt at 9600 baud - stop the AP in ROMMON first or use --wait-rommon")
            return 1
        if not wait_for_rommon(ser):
            print("[!] the bootloader did not stop - is ENABLE_BREAK=yes and the console at 9600 8N1?")
            return 1
    if args.install:
        rc = install(ser, data, args.name, args.baud)
        if rc != 0:
            return rc
        ser.reset_input_buffer()
        ser.write(b"boot\r")
        print(f"[*] booting from flash, capturing console for {args.capture:.0f}s -> {args.log}", flush=True)
        capture(ser, args.capture, args.log, args.baud)
        ser.close()
        return 0
    if args.baud != 9600:
        rommon_cmd(ser, f"set_baud {args.baud}", 3.0)
        ser.baudrate = args.baud
        if b"ap:" not in rommon_cmd(ser, "", 2.0):
            print(f"[!] lost the prompt after set_baud {args.baud}")
            return 1
    answer = rommon_cmd(ser, "boot xmodem:image.bin", 5.0)
    if b"Xmodem" not in answer:
        print("[!] ROMMON did not start an XMODEM receive:\n" + answer.decode("latin1", errors="replace"))
        return 1
    ser.timeout = 1
    print(f"[*] sending {len(data)} bytes at {args.baud} baud", flush=True)
    send_xmodem_1k(ser, data)
    print(f"[*] capturing console for {args.capture:.0f}s -> {args.log}", flush=True)
    capture(ser, args.capture, args.log, args.baud)
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
