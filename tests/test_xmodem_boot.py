# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run tools/xmodem_boot.py against a simulated Cisco ROMMON on a pseudo-terminal."""

import binascii
import importlib.util
import os
import select
import sys
import threading
from pathlib import Path

import pytest

pytest.importorskip("serial")
pty = pytest.importorskip("pty")

REPO_ROOT = Path(__file__).resolve().parent.parent
OLD_BOOT = "flash:/ap3g2-k9w8-mx.153-3.JF14/ap3g2-k9w8-mx.153-3.JF14"


class FakeRommon(threading.Thread):
    """Just enough of the AP3G2 bootloader: prompt, set, dir, set_baud, copy/boot xmodem:, boot."""

    def __init__(self, master_fd: int) -> None:
        super().__init__(daemon=True)
        self.fd = master_fd
        self.env = {"BOOT": OLD_BOOT, "ENABLE_BREAK": "yes", "MANUAL_BOOT": "no"}
        self.files: dict = {}
        self.booted = ""
        self.commands: list = []

    def out(self, text: str) -> None:
        os.write(self.fd, text.encode("latin1"))

    def read_exact(self, count: int) -> bytes:
        data = b""
        while len(data) < count:
            data += os.read(self.fd, count - len(data))
        return data

    def receive_xmodem(self) -> bytes:
        self.out("Begin the Xmodem or Xmodem-1K transfer now...\n\rCC")
        while not select.select([self.fd], [], [], 0.5)[0]:
            self.out("C")  # like the real bootloader: keep asking until the sender starts
        payload, expected = b"", 1
        while True:
            head = self.read_exact(1)[0]
            if head == 0x04:  # EOT
                os.write(self.fd, b"\x06")
                return payload
            assert head == 0x02, f"expected STX, got {head:#x}"
            seq, inv = self.read_exact(2)
            block = self.read_exact(1024)
            crc = int.from_bytes(self.read_exact(2), "big")
            assert seq == expected & 0xFF and inv == 0xFF - seq
            assert crc == binascii.crc_hqx(block, 0)
            payload += block
            expected += 1
            os.write(self.fd, b"\x06")

    def run(self) -> None:
        line = ""
        while True:
            try:
                char = os.read(self.fd, 1).decode("latin1")
            except OSError:
                return
            if char != "\r":
                line += char
                self.out(char)  # ROMMON echoes
                continue
            command, line = line.strip(), ""
            self.commands.append(command)
            self.out("\n\r")
            if command == "set":
                self.out("".join(f"{k}={v}\n\r" for k, v in sorted(self.env.items())))
            elif command.startswith("set BOOT "):
                self.env["BOOT"] = command[len("set BOOT "):]
            elif command.startswith("set_baud "):
                self.out(f"Setting baud rate to {command.split()[1]}\n")
            elif command == "dir flash:":
                self.out("Directory of flash:/\n\r2    -rwx  287       <date>               info\n\r"
                         "\n\r18808320 bytes available (22350336 bytes used)\n\r")
            elif command.startswith("copy xmodem:"):
                dest = command.split()[2]
                self.files[dest] = self.receive_xmodem()
                self.out(f'\n\rFile "{command.split()[1]}" successfully copied to "{dest}"\n\r')
            elif command.startswith("boot xmodem:") or command == "boot":
                if command == "boot":
                    self.booted = self.env["BOOT"]
                else:
                    self.files["ram"] = self.receive_xmodem()
                    self.booted = "ram"
                self.out('\n\rFile uncompressed and installed, entry point: 0x1600000\n\rexecuting...\n\r'
                         "[    0.000000] Linux version 6.12.112\n\r")
                continue  # no prompt after a boot
            self.out("ap: ")


def run_tool(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, extra: list) -> FakeRommon:
    master, slave = pty.openpty()
    rommon = FakeRommon(master)
    rommon.start()
    image = tmp_path / "image.bin"
    image.write_bytes(os.urandom(5000))
    spec = importlib.util.spec_from_file_location("xmodem_boot", REPO_ROOT / "tools" / "xmodem_boot.py")
    assert spec is not None and spec.loader is not None
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    log = tmp_path / "console.log"
    monkeypatch.setattr(sys, "argv", ["xmodem_boot.py", str(image), "--port", os.ttyname(slave),
                                      "--capture", "1", "--log", str(log), *extra])
    assert tool.main() == 0
    rommon.image = image.read_bytes()  # type: ignore[attr-defined]
    rommon.log = log.read_bytes()  # type: ignore[attr-defined]
    return rommon


def test_ram_boot_sends_the_image_intact(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    rommon = run_tool(monkeypatch, tmp_path, [])
    assert rommon.booted == "ram"
    assert rommon.files["ram"].startswith(rommon.image)  # type: ignore[attr-defined]
    assert rommon.env["BOOT"] == OLD_BOOT, "a RAM boot must not touch the BOOT variable"
    assert b"Linux version" in rommon.log  # type: ignore[attr-defined]


def test_install_copies_then_sets_boot(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    rommon = run_tool(monkeypatch, tmp_path, ["--install", "--name", "openwrt-test.bin"])
    assert rommon.files["flash:/openwrt-test.bin"].startswith(rommon.image)  # type: ignore[attr-defined]
    assert rommon.env["BOOT"] == "flash:/openwrt-test.bin"
    assert rommon.booted == "flash:/openwrt-test.bin"
    copy_at = next(i for i, c in enumerate(rommon.commands) if c.startswith("copy xmodem:"))
    boot_at = rommon.commands.index("set BOOT flash:/openwrt-test.bin")
    assert copy_at < boot_at, "BOOT may only change after the copy succeeded"
    assert "ether_init" not in rommon.commands, "ether_init before a non-network boot resets the bootloader"
