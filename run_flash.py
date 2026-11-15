#!/usr/bin/env python3
"""
Automated ROMMON Flashing Script for Cisco Aironet 3702i
Licensed under GNU AGPLv3.

Author: Antigravity Pair Programmer
"""

import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600
TFTP_IP = "192.168.1.153"
TAR_FILE = "ap3g2-k9w7-tar.153-3.JPQ5.tar"
EXTRACTED_DIR = "ap3g2-k9w7-mx.153-3.JPQ5"

def send_cmd(ser, cmd, wait=0.5):
    ser.write((cmd + "\r\n").encode("latin1"))
    time.sleep(wait)
    out = ""
    while ser.in_waiting:
        out += ser.read(ser.in_waiting).decode("latin1", errors="replace")
        time.sleep(0.1)
    return out

def main():
    print(f"[*] Opening {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.5)

    # Wake up prompt
    print("[*] Synchronizing prompt...")
    for _ in range(3):
        ser.write(b"\r\n")
        time.sleep(0.3)
        ser.read(ser.in_waiting or 1)

    ser.write(b"\r\n")
    time.sleep(0.5)
    resp = ser.read(ser.in_waiting or 1).decode("latin1", errors="replace")
    print(f"Current response: {repr(resp)}")

    if "ap:" not in resp:
        print("[!] Not at 'ap:' prompt yet. Please ensure the AP is in ROMMON.")
        # Try sending help
        out = send_cmd(ser, "help")
        if "ap:" not in out:
            print("[!] Still not in ROMMON. Exiting.")
            ser.close()
            sys.exit(1)

    print("[+] Confirmed ROMMON 'ap:' prompt!")

    # Set network variables
    print("[*] Configuring network parameters in ROMMON...")
    print(send_cmd(ser, "set IP_ADDR 192.168.1.50"))
    print(send_cmd(ser, "set NETMASK 255.255.255.0"))
    print(send_cmd(ser, "set DEFAULT_ROUTER 192.168.1.1"))
    print(send_cmd(ser, "flash_init"))
    print(send_cmd(ser, "tftp_init"))
    print(send_cmd(ser, "ether_init"))

    # Test ARP
    print("[*] Verifying ARP resolution to TFTP server (192.168.1.153)...")
    arp_out = send_cmd(ser, f"arp {TFTP_IP}", wait=1.5)
    print(arp_out)
    if "is at" not in arp_out:
        print("[!] Warning: Host MAC not resolved via ARP yet. Continuing anyway...")

    # Start TAR extraction
    tar_cmd = f"tar -xtract tftp://{TFTP_IP}/{TAR_FILE} flash:"
    print(f"\n[*] Starting firmware extraction:\n    {tar_cmd}\n")
    ser.write((tar_cmd + "\r\n").encode("latin1"))

    # Monitor loop
    start_time = time.time()
    last_act = time.time()
    buffer = ""

    while True:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            text = chunk.decode("latin1", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            buffer += text
            last_act = time.time()

            # Automatically respond to pagination prompts
            if "--More--" in buffer or "--more--" in buffer:
                ser.write(b" ")
                buffer = ""
            elif "Are you sure" in buffer:
                ser.write(b"y\r\n")
                buffer = ""

            # Check if finished
            if "ap: " in buffer and ("extracting" in buffer or "flash:" in buffer or (time.time() - start_time > 60)):
                if "extracting" not in buffer[-20:]:
                    print("\n[+] Firmware extraction completed!")
                    break

        # Timeout if completely silent for 180 seconds
        if time.time() - last_act > 180:
            print("\n[!] Transfer timed out (no activity for 180 seconds).")
            break

    # Verify flash contents
    print("\n[*] Verifying flash directory...")
    dir_out = send_cmd(ser, f"dir flash:/{EXTRACTED_DIR}", wait=1.0)
    print(dir_out)

    # Set boot variable and boot
    boot_bin = f"flash:/{EXTRACTED_DIR}/{EXTRACTED_DIR}"
    print(f"[*] Setting BOOT variable to: {boot_bin}")
    send_cmd(ser, f"set BOOT {boot_bin}")
    print("[*] Booting new Autonomous image...")
    ser.write(b"boot\r\n")

    # Follow boot output for 60 seconds
    boot_start = time.time()
    while time.time() - boot_start < 60:
        c = ser.read(ser.in_waiting or 1)
        if c:
            sys.stdout.write(c.decode("latin1", errors="replace"))
            sys.stdout.flush()

    ser.close()
    print("\n[+] Flashing automation completed! Connect to console to finish initial configuration.")

if __name__ == "__main__":
    main()
