#!/usr/bin/env python3
"""
serial_cmd.py - Execute a command on the live OpenWrt console and print the output.
"""
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
BAUD = 9600

def run_cmd(cmd, timeout=5.0):
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    ser.reset_input_buffer()

    # Send command
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()

    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("latin1", errors="replace")
            buf += chunk
            # Check if we got back to the prompt (after echoing command)
            if "root@OpenWrt:" in buf and len(buf.split("\n")) > 1:
                # Wait briefly for any remaining prompt chars
                time.sleep(0.1)
                if ser.in_waiting:
                    buf += ser.read(ser.in_waiting).decode("latin1", errors="replace")
                break
        time.sleep(0.05)
    ser.close()
    return buf

if __name__ == "__main__":
    cmd = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "uname -a"
    out = run_cmd(cmd)
    print(out)
