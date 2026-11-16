# Cisco Aironet 2602i — first assessment

**Contribution by:** Marco Mata, 2026-11-15/16. **Status: boots and stays installed, wired only.**
OpenWrt 25.12.6 is installed on `flash:` of one test unit and survives a cold boot. There is no Wi-Fi
driver, no saved-settings area and no per-unit MAC yet, so this is not a usable access point.

The 2602i belongs to the same Cisco `ap3g2` family as the 3702i documented in this repository and
turned out to be close enough that the 3702i work carries over with a two-line device-tree change.

## 1. Hardware compared with the 3702i

| | 2602i (`AIR-CAP2602I-E-K9`) | 3702i (`AIR-CAP3702I-E-K9`) |
|---|---|---|
| Board string | `Tide MB - 32MB of flash` | `Tide XL MB - 40MB of flash` / `Montserrat Board` |
| Bootloader | `C2600 Boot Loader (AP3G2-BOOT-M) 12.4(25e)JAY` | `C3700 Boot Loader (AP3G2-BOOT-M) 15.2(4)JB` |
| Cisco software | `ap3g2-k9w8` 15.3(3)JF14 — same image family | same |
| `PVR` / `SVR` (`ap: dump_regs`) | `0x80212151` / `0x80fc0011` | identical |
| `PORPLLSR` | `0x43431408` | identical |
| RAM | 256 MiB (`188398K/60928K`) | 512 MiB |
| NOR flash seen by Linux | 32 MiB, JEDEC `0x00c2:0x227e` (Macronix) | 64 MiB, `0x0001:0x2301` |
| Free space on `flash:` (stock) | 8 592 896 bytes | 18 808 320 bytes |
| ROMMON text in RAM (`ap: memory`) | `0x06000000` | `0x0a000000` |
| Radios according to IOS | 2 × `8764` | `8764` + `8864` |
| Radio PCI IDs under Linux | `9000:01:00.0` = `11ab:8650`, `a000:03:00.0` = `11ab:8624` (64 KiB BARs) | `11ab:b050`, `11ab:b024` |
| PCIe root ports | 2 (`9000:`, `a000:`) | 3 |
| Ethernet MAC source | ROMMON prints `Reading cookie from SEEPROM` | `MAC_ADDR=` record in the ROMMON environment |
| PoE class | 3 | 4 |

## 2. What works (OpenWrt 25.12.6)

Image: `tools/build_image.py 25 … --no-overlay --dtb dts/cisco-2602i.dtb`, load/entry `0x01600000`.

### 2.1 RAM boot

Loaded with `tools/xmodem_boot.py` (734 s at 115200 baud):

```text
File "xmodem:image.bin" uncompressed and installed, entry point: 0x1600000
executing...
[    0.000000] Hardware name: Cisco Aironet 2602i (AIR-CAP2602I) e500v2 0x80212151 P1020 RDB
[    0.000000] phys_mem_size     = 0x10000000
[    0.826643] Memory: 236436K/262144K available (...)
[   20.007456] e8000000.flash: Found 1 x16 devices at 0x0 in 16-bit bank. Manufacturer ID 0x0000c2 Chip ID 0x00227e
[   20.546642] mtd: partition "full_flash" extends beyond the end of device "e8000000.flash" -- size truncated to 0x2000000
[   35.401015] procd: - init -
```

* Same ROMMON procedure as the 3702i: `ESC` during `flashfs fsck` stops at `ap:`, `set_baud 115200`,
  `boot xmodem:` works, the image window (`0x01600000` + 8.2 MiB) is below the ROMMON at `0x06000000`.
* Kernel 6.12.112 reaches userspace with 256 MiB.
* `radio_reset.ko` (unchanged, same GPIO pulse as on the 3702i) releases both radios:
  `GPDAT 0x0f8f0000 → 0x5fef0000`, then both endpoints enumerate with the IDs in the table above.

### 2.2 Installed on `flash:`

`python3 tools/xmodem_boot.py <image> --install --name openwrt-2602i.bin` — the first run of the
packaged installer on real hardware:

```text
[*] current BOOT=flash:/ap3g2-k9w8-mx.153-3.JF14/ap3g2-k9w8-mx.153-3.JF14
[*] copying 8208031 bytes to flash:/openwrt-2602i.bin at 115200 baud (about 14 minutes for 8 MiB)
[+] transfer done in 962s
[+] installed. To go back to the Cisco software:  ap: set BOOT flash:/ap3g2-k9w8-mx.153-3.JF14/ap3g2-k9w8-mx.153-3.JF14
[*] booting from flash, capturing console for 140s
...
Loading "flash:/openwrt-2602i.bin"...***********************
File "flash:/openwrt-2602i.bin" uncompressed and installed, entry point: 0x1600000
```

| Check | Result |
|---|---|
| Boot from `flash:` (started by the installer) | OpenWrt 25.12.6 up, ping after ≈105 s |
| Ethernet | `p1023_fman_im` unchanged: ping, SSH and LuCI over the wire (switch port up, 1 Gb/s) |
| Cold boot, PoE off/on, nobody on the console | back on the network within 155 s, still 25.12.6 from `flash:` |
| `sysupgrade -T` | refused by the image, as intended |
| Cisco software | untouched; the stock `BACKUPBOOT` recovery entry is still in the ROMMON environment |

### 2.3 Standalone: Cisco software removed

With the install proven, everything Cisco was deleted from `flash:` in ROMMON
(`tools/rommon_clean_flash.py --keep openwrt-2602i.bin env_vars private-multiple-fs --delete`:
IOS image directory, recovery image, event logs, saved CAPWAP configuration — 23.2 MiB in about
100 files). A raw dump of the 32 MiB chip was taken from Linux beforehand.

| | Before | After |
|---|---|---|
| Free on `flash:` | 513 024 bytes | 23 776 256 bytes |
| Files in `flash:/` | 18 entries | `openwrt-2602i.bin`, `env_vars`, `private-multiple-fs` |
| `boot` from ROMMON | — | OpenWrt up, ping after ≈115 s |
| Unattended PoE cycle | — | back on the network 155 s after power-on |

`BACKUPBOOT` / `AP_IMAGE_RCV` in the ROMMON environment still name the deleted recovery image;
they were left alone. This step cannot be undone without a Cisco image from elsewhere.

## 3. What does not work yet

1. **Wi-Fi.** Neither `11ab:8650` nor `11ab:8624` is in a driver's ID table. IOS calls both radios
   `8764`, so the candidate is `mwl8k` with the 88W8764 AP firmware plus two new PCI IDs — untested.
   On the 3702i the 8764 path still relies on command polling and returns no scan results on 25.12.6,
   so that item has to be solved first; it decides Wi-Fi on the 2602i entirely.
2. **No saved settings.** Per-128 KiB-block scan of the 32 MiB chip from Linux (read-only), before the
   install:

   | Range | Content |
   |---|---|
   | `0x00000000–0x00080000` | 4 blocks with only a flashfs block header |
   | `0x00080000–0x02000000` | 252 blocks with data (Cisco flashfs, including stale data in free blocks) |

   There is no region outside the flashfs like the 3702i's `0x02800000–0x03c00000`, so the JFFS2
   overlay design cannot be copied and the device tree keeps the whole chip read-only. Every setting
   has to be baked into the image (`--ip`, `--pubkey`, …); changes made at run time are lost on
   reboot. ROMMON also reports a second, empty 12 MiB `flashfs[1]`, and the bootloader itself has to
   live somewhere: both must be outside the 32 MiB that Linux currently sees (second chip /
   chip-select or a banked upper half) — **not located yet**. That region is the natural candidate
   for an overlay.
3. **Ethernet MAC is the driver's fallback `02:00:00:37:02:01`.** ROMMON prints
   `Reading cookie from SEEPROM` before the base MAC, i.e. the address comes from an I2C SEEPROM, not
   from flash. A trap worth recording: before the install, `strings` over the flash found 26
   `MAC_ADDR=xx:..` records with this unit's MAC — they were stale copies of old Cisco files in
   *free* flashfs blocks, and all of them were gone once the 8.2 MiB image had been written. Do not
   build a MAC lookup on a whole-flash scan. Two 2602i units on one LAN will collide until the
   SEEPROM is read.
4. **Flash space next to the Cisco software.** With IOS and the recovery image in place only
   0.5 MiB stays free after the install, so a second image cannot be stored next to the first.
   Removing the Cisco software (§2.3) lifts this.

## 4. Suggested next steps

1. Read the SEEPROM cookie over I2C and feed the MAC to `p1023_fman_im` (`mac_str=`).
2. Solve the `mwl8k` interrupt line on the 3702i, then try `mwl8k` with IDs `0x8650` / `0x8624`.
3. Find the second flash region (compare ROMMON `flashfs[1]` geometry with the eLBC chip-selects)
   and put a `rootfs_data` overlay there.
