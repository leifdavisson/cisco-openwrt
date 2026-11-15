#!/usr/bin/env python3
"""
Set boot manual and reload to ROMMON prompt on Cisco AP3702.
License: GNU AGPLv3
"""
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600

def set_manual_and_reload():
    print(f"[*] Connecting to {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=1)

    def send_cmd(cmd, wait=1.0):
        ser.write(cmd.encode() + b"\r\n")
        time.sleep(wait)
        out = b""
        while ser.in_waiting:
            out += ser.read(ser.in_waiting)
            time.sleep(0.05)
        return out.decode("latin1", errors="replace")

    # Wake up console
    send_cmd("")
    send_cmd("")

    # Enable mode
    out = send_cmd("enable")
    if "Password:" in out:
        send_cmd("Cisco1234!")

    # Configure boot manual
    print("[*] Configuring 'boot manual'...")
    send_cmd("config t")
    send_cmd("boot manual")
    send_cmd("end")

    # Save config
    print("[*] Saving configuration...")
    send_cmd("write memory", wait=2.0)

    # Verify show boot
    boot_status = send_cmd("show boot")
    print(f"[*] show boot output:\n{boot_status}")
    if "Manual Boot:         yes" not in boot_status:
        print("[!] Warning: Manual Boot might not be set to yes.")
    else:
        print("[+] Manual Boot successfully set to 'yes'!")

    # Reload AP
    print("[*] Sending 'reload' command...")
    ser.write(b"reload\r\n")
    time.sleep(1)
    out = ser.read(ser.in_waiting or 100).decode("latin1", errors="replace")
    print(out, end="")
    if "Proceed with reload" in out or "confirm" in out or "[" in out:
        ser.write(b"\r\n")
    else:
        time.sleep(1)
        out = ser.read(ser.in_waiting or 100).decode("latin1", errors="replace")
        print(out, end="")
        ser.write(b"\r\n")

    print("\n[*] Waiting for reboot and ROMMON prompt 'ap:'...")
    start_time = time.time()
    buf = ""
    while time.time() - start_time < 90:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            sys.stdout.write(chunk)
            sys.stdout.flush()
            buf += chunk
            if "ap:" in buf[-20:]:
                print("\n[+] Reached ROMMON prompt 'ap:'!")
                ser.close()
                return True
        time.sleep(0.1)

    print("\n[!] Timed out waiting for ROMMON prompt.")
    ser.close()
    return False

if __name__ == "__main__":
    success = set_manual_and_reload()
    sys.exit(0 if success else 1)
