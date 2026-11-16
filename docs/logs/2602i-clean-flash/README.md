# 2602i flash clean-up — command, script and log

Record of how the Cisco software was removed from the `AIR-CAP2602I-E-K9` test unit on 2026-11-16
(section 2.3 of [AP2602_NOTES.md](../../AP2602_NOTES.md)). Performed by Marco Mata.

| File | Content |
|---|---|
| [`deleted-files.txt`](deleted-files.txt) | one line per file/directory in the order it was handled: 89 files deleted, 8 directories removed, 3 entries kept, 0 failures |
| [`rommon-console.txt`](rommon-console.txt) | the complete ROMMON console transcript of the run (every `dir`, `delete`, `rmdir` and the bootloader's answers) |

## Before

The AP was running OpenWrt 25.12.6 from `flash:/openwrt-2602i.bin` with `BOOT` pointing at it, and
a raw dump of the 32 MiB flash chip had been taken from Linux. The AP was then power-cycled and
stopped at the ROMMON `ap:` prompt with `ESC` (9600 baud, Ethernet not initialised).

One file was deleted by hand first to learn the prompt format (it is therefore not in the log):

```text
ap: delete flash:/event.r0
Are you sure you want to delete "flash:/event.r0" (y/n)?y
File "flash:/event.r0" deleted
```

## Command

```bash
python3 tools/cleanflash.py openwrt-2602i.bin env_vars private-multiple-fs
```

The arguments are the names in `flash:/` to keep: the OpenWrt image, the ROMMON environment file
(it holds `BOOT`) and the unit's certificate store.

## Script as it was run

This is the bench script, unmodified. [`tools/rommon_clean_flash.py`](../../../tools/rommon_clean_flash.py)
is the same logic tidied up for the repository (argument parsing, list-only by default, refuses to
run without `env_vars` in the keep list); that tidied version has **not** yet been run on hardware.

```python
#!/usr/bin/env python3
"""cleanflash.py KEEP... - at a ROMMON ap: prompt (9600), delete everything on flash: except KEEP names.

Walks directories, answers the per-file (y/n) prompt, removes emptied directories, logs to logs/.
"""
import re, sys, time
import serial

keep = set(sys.argv[1:])
assert keep, "refusing to run without a keep list"
s = serial.Serial("/dev/ttyUSB0", 9600, timeout=0.2)
log = open("logs/cleanflash-%d.log" % time.time(), "w")


def tx(text, timeout=40.0, cr=True, until=("ap:",)):
    s.reset_input_buffer()
    for ch in text:
        s.write(ch.encode("latin1")); time.sleep(0.012)
    if cr:
        s.write(b"\r")
    buf, t0 = "", time.time()
    while time.time() - t0 < timeout:
        buf += s.read(512).decode("latin1", errors="replace")
        tail = buf.rstrip(" ")
        if len(buf) > len(text) + 1 and any(tail.endswith(u) for u in until):
            break
    log.write(buf.replace("\r", "")); log.flush()
    return buf.replace("\r", "")


def listing(path):
    out = tx(f"dir {path}")
    entries = []
    for line in out.split("\n"):
        m = re.match(r"\s*\d+\s+([d-])[rwx-]+\s+(\d+)\s+<date>\s+(\S+)\s*$", line)
        if m:
            entries.append((m.group(1) == "d", int(m.group(2)), m.group(3)))
    free = re.search(r"(\d+) bytes available", out)
    return entries, int(free.group(1)) if free else None


def clean(path, depth=0):
    removed = 0
    entries, _ = listing(path)
    for is_dir, size, name in entries:
        full = f"{path}{name}" if path.endswith("/") else f"{path}/{name}"
        if depth == 0 and name in keep:
            print(f"keep    {full} ({size})"); continue
        if is_dir:
            removed += clean(full + "/", depth + 1)
            r = tx(f"rmdir {full}", until=("ap:", "(y/n)?"))
            if "(y/n)?" in r:
                r += tx("y")
            print(f"rmdir   {full}: {'ok' if 'emoved' in r or 'eleted' in r else r.strip().splitlines()[-2:]}")
        else:
            r = tx(f"delete {full}", until=("(y/n)?", "ap:"))
            if "(y/n)?" in r:
                r += tx("y")
            ok = "deleted" in r
            removed += size if ok else 0
            print(f"{'delete ' if ok else 'FAILED '} {full} ({size})", flush=True)
    return removed


tx("")
_, before = listing("flash:")
total = clean("flash:/")
entries, after = listing("flash:")
print(f"\nremoved {total} bytes; free before={before} after={after}")
print("left on flash:", ", ".join(n for _, _, n in entries))
```

## Result

```text
removed 23234270 bytes; free before=513024 after=23776256
left on flash: private-multiple-fs, openwrt-2602i.bin, env_vars
```

`dir flash:` in the transcript shows the same three entries at the end. Afterwards:

| Check | Result |
|---|---|
| `ap: boot` | `Loading "flash:/openwrt-2602i.bin"...` → OpenWrt 25.12.6, ping after ≈115 s |
| PoE off/on with nobody on the console | back on the network 155 s after power-on |

Not done: `BACKUPBOOT` and `AP_IMAGE_RCV` in the ROMMON environment still name the deleted recovery
image. Returning this unit to Cisco software would need an image obtained elsewhere.
