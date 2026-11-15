# Cisco Aironet 2602i — first assessment

**Contribution by:** Marco Mata, 2026-11-15. **Status: exploratory.** OpenWrt runs from RAM on this
model; it cannot be installed or used as an access point yet. Nothing below has been written to the
unit's flash.

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

## 2. What works (OpenWrt 25.12.6, RAM boot)

Image: `tools/build_image.py 25 … --no-overlay --dtb dts/cisco-2602i.dtb`, loaded with
`tools/xmodem_boot.py` (734 s at 115200 baud), load/entry `0x01600000`.

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
* Kernel 6.12.112 reaches userspace with 256 MiB; shell on the serial console.
* `p1023_fman_im` registers `eth0` and brings the bridge up. **Link and traffic were not tested**
  (the switch port was administratively down); the MAC is the driver's fallback because this tree has
  no `rommon_env` partition.
* `radio_reset.ko` (unchanged, same GPIO pulse as on the 3702i) releases both radios:
  `GPDAT 0x0f8f0000 → 0x5fef0000`, then both endpoints enumerate with the IDs in the table above.

## 3. What does not work yet

1. **Wi-Fi.** Neither `11ab:8650` nor `11ab:8624` is in a driver's ID table. IOS calls both radios
   `8764`, so the candidate is `mwl8k` with the 88W8764 AP firmware plus two new PCI IDs — untested.
   On the 3702i the 8764 path still relies on command polling and returns no scan results on 25.12.6,
   so that item has to be solved first; it decides Wi-Fi on the 2602i entirely.
2. **Flash layout / installation.** Per-128 KiB-block scan of the 32 MiB chip from Linux (read-only):

   | Range | Content |
   |---|---|
   | `0x00000000–0x00080000` | 4 blocks with only a flashfs block header |
   | `0x00080000–0x02000000` | 252 blocks with data (Cisco flashfs) |

   There is no empty region like the 3702i's `0x02800000–0x03c00000`, so the JFFS2 overlay design
   cannot be copied. ROMMON also reports a second, empty 12 MiB `flashfs[1]`, and the bootloader
   itself has to live somewhere: both must be outside the 32 MiB that Linux currently sees (second
   chip / chip-select or a banked upper half) — **not located yet**. Until that is mapped the
   device tree keeps the flash read-only.
3. **Space on `flash:`.** An 8.2 MiB image fits in the 8.6 MiB that are free with about 380 KiB to
   spare; a persistent install would probably mean deleting the Cisco recovery image first.
4. **Ethernet MAC.** Needs the SEEPROM cookie (I2C) instead of the ROMMON environment record.

## 4. Suggested next steps

1. Bring the switch port up and confirm Ethernet traffic.
2. Solve the `mwl8k` interrupt line on the 3702i, then try `mwl8k` with IDs `0x8650` / `0x8624`.
3. Find the second flash region (compare ROMMON `flashfs[1]` geometry with the eLBC chip-selects),
   then decide where settings can live.
4. Only then attempt `copy xmodem: flash:` + `set BOOT` on a 2602i.
