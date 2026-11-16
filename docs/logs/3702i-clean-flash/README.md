# 3702i flash clean-up — command and log

Record of how the Cisco software was removed from the `AIR-CAP3702I-E-K9` test unit on 2026-11-16,
with the tool as it is in this repository (its first run on hardware). Performed by Marco Mata.

Before: the AP booted OpenWrt from `flash:/openwrt-3702i-v3.bin`; a raw dump of the 64 MiB flash
had been taken from Linux before the first OpenWrt install. The AP was stopped at the ROMMON `ap:`
prompt with `ESC` (9600 baud, Ethernet not initialised).

```bash
python3 tools/rommon_clean_flash.py --keep openwrt-3702i-v3.bin env_vars private-multiple-fs            # list only
python3 tools/rommon_clean_flash.py --keep openwrt-3702i-v3.bin env_vars private-multiple-fs --delete
```

[`deleted-files.txt`](deleted-files.txt) is the complete output of the second command: 90 files
deleted, 8 directories removed, 0 failures.

```text
removed 30530168 bytes; free before=2364928 after=32923648
on flash:/ now: private-multiple-fs, openwrt-3702i-v3.bin, env_vars
```

Removed: the IOS image directory (`ap3g2-k9w8-mx.153-3.JF14`, including the radio images and
power tables), the recovery image, event logs, saved CAPWAP configuration, and an older OpenWrt
image. The unit was then reinstalled with the generic image and still boots unattended.
