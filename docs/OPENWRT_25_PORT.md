# OpenWrt 25.12.6 (Linux 6.12) on the Cisco Aironet 3702i

**Contribution by:** Marco Mata — second test bench, 2026-11-15
**Builds on:** the 23.05.5 port in this repository (device tree, `p1023_fman_im` Ethernet driver,
radio bring-up, MZIP packaging, JFFS2 overlay). All of that is the work of the repository author;
this document only covers what was needed to move from 23.05.5 to 25.12.6 and what a second unit showed.

| | Test unit used here |
|---|---|
| Model | `AIR-CAP3702I-E-K9` (board "Montserrat" / "Tide XL MB", 40 MB flash format) |
| Stock software | lightweight `ap3g2-k9w8` 15.3(3)JF14, never converted to autonomous |
| Bootloader | `C3700 Boot Loader (AP3G2-BOOT-M) 15.2(4)JB` |
| Power / console | 802.3at PoE, console 9600 8N1 |

---

## 1. Summary

| Item on 25.12.6 | Result | How it was checked |
|---|---|---|
| Boot wrapper + kernel 6.12.112 | **works** | serial log to `procd: - init -`, RAM boot |
| Root cause of "reset after `executing...`" | **wrong load address** (see §2) | `tools/wrapper_info.py` + boot test |
| Ethernet (`p1023_fman_im.ko`, `marvell.ko`) | **works**, 1000 Mb/s, builds unmodified | ping, SSH, LuCI |
| Real Ethernet MAC instead of `02:00:00:37:02:01` | **works** with a preinit hook (§5) | `mtd_get_mac_ascii rommon_env MAC_ADDR` |
| `radio_reset.ko` | **works**, builds unmodified | both endpoints enumerate |
| 5 GHz 88W8864 (`mwlwifi`) | **registers, scans** after a 4-line mac80211 fix (§4) | `iw phy`, passive scan: 16 BSS |
| 2.4 GHz 88W8764 (`mwl8k`) | **registers** after the same fix; **scan returns 0 BSS** | see §4.2, same open IRQ item as on 23.05 |
| AP mode / client traffic on 25.12.6 | **not tested** | — |
| Boot from `flash:`, unattended cold boot | **works** (§6) | PoE cycle without console: ping after 114 s |
| JFFS2 overlay persistence | **works** across 2 power cycles (§6) | file + `uci commit` survive |

One-line fix for the boot problem, in `tools/repack_openwrt25_image.py`:

```diff
-    run(f"python3 {mkcisco} pack-raw {OUTPUT_RAW} {FINAL_CISCO_BIN} 0x02000000")
+    run(f"python3 {mkcisco} pack-raw {OUTPUT_RAW} {FINAL_CISCO_BIN} {WRAPPER_LINK_ADDR:#010x}")   # 0x01600000
```

---

## 2. Why 25.12.6 reset right after `executing...`

### 2.1 The boot wrapper is not position independent

The first ~86 KiB of OpenWrt's `…-initramfs-kernel.bin` is the PowerPC `simpleImage` boot wrapper
(`arch/powerpc/boot`: `crt0.S`, `simpleboot.c`, XZ decompressor, embedded DTB). Its start-up code tests
`GOT[0]`: non-zero means "linked `-pie`, relocate myself", zero means "run where I was linked".
In both official images `GOT[0] == 0` and there is no relocation table, so the wrapper only runs
at its link address. The words in front of the GOT are absolute pointers into the wrapper itself,
which gives that address away:

| Release | Kernel | Wrapper link address | Kernel `PT_LOAD` filesz (loaded at 0) | memsz |
|---|---|---|---|---|
| 23.05.5 | 5.15.167 | **`0x01500000`** | `0x00cadd08` (12.7 MiB) | `0x00ce92b4` |
| 25.12.6 | 6.12.112 | **`0x01600000`** | `0x01523590` (21.1 MiB) | `0x015613cc` |

The 6.12 initramfs kernel no longer fits below `0x01500000`, which is presumably why the link address
moved up by 1 MiB between the releases. (The uImage header of the official 25.12.6 file still says
load/entry `0x01500000`; that field is not what the wrapper was linked for.)

Reproduce on any official image:

```console
$ python3 tools/wrapper_info.py openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin
uimage_load      = 0x01500000
pie              = False
link_address     = 0x01600000
kernel_loadsize  = 0x01523590
kernel_memsize   = 0x015613cc

=> pack with:  mkciscoimage.py pack-raw <raw> <out> 0x01600000
   kernel (0x0..0x1523590) fits below the wrapper at 0x1600000
   note: the uImage header says 0x1500000; that is not where this wrapper is linked.
```

### 2.2 What each load address does

| MZIP load/entry address | 25.12.6 behaviour |
|---|---|
| `0x02000000` (previous script) | Wrapper runs 10 MiB away from its link address; every absolute pointer is wrong → reset right after `executing...` (the reported symptom). |
| `0x01500000` (planned fix in the notebook) | Still not the link address, and the kernel (`0x0..0x01523590`) would overwrite the first 144 KiB of the wrapper. |
| **`0x01600000`** | Link address; kernel ends below it, DTB/heap are above `_end`. **Boots.** |

The other constants in `repack_openwrt25_image.py` were checked against the official image and are
correct: XZ payload at `0x15000` (8 121 652 bytes), initramfs LZMA at vmlinux offset `0x1044b80`
(4 376 803 bytes), `__initramfs_size` at `0x1471468`, DTB slot `0x11280..0x15000` (15 744 bytes).

### 2.3 Verification on hardware

Minimal test image = official 25.12.6 file, only the DTB slot replaced by `dts/cisco-3702i.dtb`,
packed with `pack-raw … 0x01600000`, RAM-booted from ROMMON:

```text
File "xmodem:owrt25.bin" uncompressed and installed, entry point: 0x1600000
executing...
[    0.000000] Hardware name: Cisco Aironet 3702i (AIR-CAP3702I) e500v2 0x80212151 P1020 RDB
[    0.000000] phys_mem_size     = 0x20000000
[    0.000000] Kernel command line: console=ttyS0,9600 earlyprintk root=/dev/ram0 rw pci=realloc
[    0.826644] Memory: 495876K/524288K available (9132K kernel code, 708K rwdata, 7252K rodata, 4556K init, ...)
[   14.196610] e8000000.flash: Found 1 x16 devices at 0x0 in 16-bit bank. Manufacturer ID 0x000001 Chip ID 0x002301
[   22.896424] Creating 3 MTD partitions on "e8000000.flash":
[   25.274373] Run /init as init process
[   37.995759] procd: - init -
Please press Enter to activate this console.
```

Notes from that log: only CPU 0 comes up (`No cpu-release-addr for cpu 1`), `fsl-gianfar … probe failed -12`
is expected (the board uses the FMan MAC), and the CFI probe of the 64 MiB flash takes about 8 s.

---

## 3. Building the 25.12.6 image

1. `openwrt-sdk-25.12.6-mpc85xx-p1020_gcc-14.3.0_musl` and the official
   `…enterasys_ws-ap3710i-initramfs-kernel.bin` (strip the 64-byte uImage header to get `raw25.bin`).
2. `driver/` (`p1023_fman_im.ko`, `radio_reset.ko`) builds against `linux-6.12.112` without changes.
   `drivers/net/phy/marvell.ko` is shipped prebuilt in the SDK kernel tree.
3. The Wi-Fi drivers need the mac80211 backports tree (`backports-6.18.55`). It is **not** in the SDK
   tarball, and the SDK deletes it again after building because `CONFIG_AUTOREMOVE=y` is its default:

   ```bash
   ./scripts/feeds update base && ./scripts/feeds install mac80211 && make defconfig
   sed -i 's/^CONFIG_AUTOREMOVE=y/# CONFIG_AUTOREMOVE is not set/' .config
   make package/mac80211/compile -j"$(nproc)"
   # -> build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/mac80211-{regular,smallbuffers}/backports-6.18.55
   ```
4. Size budget of the repacked image (Ethernet + both Wi-Fi drivers + both firmwares, unused
   `ath9k`/USB/PPP modules removed): initramfs 4 318 724 / 4 376 803 bytes, `vmlinux.xz`
   8 058 304 / 8 121 652 bytes. It fits, with about 58 KiB to spare — strip every `.ko`.

---

## 4. Wi-Fi drivers on mac80211 6.18

### 4.1 Channel-context emulation (both drivers)

Unchanged, both drivers compile but fail to probe on 25.12.6:

```text
WARNING: CPU: 0 PID: 2602 at backports-6.18.55/net/mac80211/main.c:826 … [mac80211]
mwlwifi: mwlwifi hw alloc failed
mwlwifi 9000:01:00.0: probe with driver mwlwifi failed with error -12
mwl8k: ieee80211 alloc failed
mwl8k a000:03:00.0: probe with driver mwl8k failed with error -12
```

Since Linux 6.9, `ieee80211_alloc_hw_nm()` rejects drivers without channel-context operations;
drivers that do not implement them must point at mac80211's emulation helpers (the in-tree `mwl8k`
and OpenWrt's own `mwlwifi` patches do exactly this). Added to `mwl8k_ops` and `mwl_mac80211_ops`:

```c
#ifdef MWL_EMULATE_CHANCTX	/* mac80211 >= 6.9 */
	.add_chanctx		= ieee80211_emulate_add_chanctx,
	.remove_chanctx		= ieee80211_emulate_remove_chanctx,
	.change_chanctx		= ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx	= ieee80211_emulate_switch_vif_chanctx,
#endif
```

`MWL_EMULATE_CHANCTX` is set by the Makefiles only when `$(BP_DIR)/include/net/mac80211.h` declares
`ieee80211_emulate_add_chanctx`, so the 23.05.5 build (backports 6.1.110) is unaffected.
**The 23.05.5 build was not re-run on this bench** — please rebuild it once before merging.

Result on 25.12.6 after `insmod radio_reset.ko`, `mwlwifi.ko`, `mwl8k.ko`:

```text
ieee80211 phy0: fw download complete
ieee80211 phy0: firmware version: 0x702091b
ieee80211 phy0: 2G enabled, 5G enabled
ieee80211 phy0: 4 TX antennas, 4 RX antennas
ieee80211 phy1: 88w8764 v7, a8800f500000, AP firmware 7.4.0.9
```

### 4.2 What was and was not tested

* **5 GHz (`phy0`)**: passive scan returns 16 BSS on channels 60–108 with plausible levels
  (−68 … −98 dBm); `/proc/interrupts` shows `OpenPIC 4 Level mwlwifi` counting.
* **2.4 GHz (`phy1`)**: registers and loads its firmware; every firmware command still logs
  `took 9990 ms` (the `OPC_DONE` polling fallback), and a scan completes in 20 s with **0 BSS**.
  This looks like the already-open "mwl8k hardware IRQ pin" item rather than something new in 25,
  but it was **not compared against 23.05.5 on this unit**, so treat it as unconfirmed.
* `wifi config` on 25.12.6 detects `radio1` as `band '5g'` / `HT40` (the 8764 advertises both bands);
  it has to be set to `2g` by hand.
* Radio MAC addresses come up as `b0:80:0f:50:ff:ff` / `a8:80:0f:50:00:00` on this unit. They look
  like placeholders rather than the per-unit radio MACs — not investigated.
* AP mode, association and throughput were not tested on 25.12.6.

---

## 5. Ethernet MAC address: every AP comes up as `02:00:00:37:02:01`

`p1023_extract_initial_mac()` takes the MAC from `mac_str=`, then the device tree, then the dTSEC
`MACSTNADDR` registers, and finally falls back to a constant. The registers are only programmed if
ROMMON initialised the Ethernet port (`ether_init` / TFTP boot). After a boot from `flash:` or
`xmodem:` the fallback is used, so **all units get the same MAC** — a problem as soon as two of them
share a LAN.

The per-unit MAC is stored by Cisco as an ASCII record in the ROMMON environment, inside the
`rommon_env` MTD partition already defined in the DTS:

```console
root@OpenWrt:~# . /lib/functions.sh; . /lib/functions/system.sh
root@OpenWrt:~# mtd_get_mac_ascii rommon_env MAC_ADDR
xx:xx:xx:xx:xx:xx        # same value ROMMON prints as "Base Ethernet MAC address"
```

A preinit hook hands it to the driver before `kmodloader` runs (`/lib/preinit/09_cisco_fman_mac`):

```sh
cisco_fman_mac() {
	. /lib/functions.sh
	. /lib/functions/system.sh
	local mac
	mac=$(mtd_get_mac_ascii rommon_env MAC_ADDR)
	[ -n "$mac" ] && printf 'marvell\np1023_fman_im mac_str=%s\n' "$mac" > /etc/modules.d/20-p1023-fman
}

boot_hook_add preinit_main cisco_fman_mac
```

---

## 6. Installing 25.12.6 to flash

> Do **not** use LuCI's "Flash new firmware" / `sysupgrade` on this board. The running system
> identifies itself through the WS-AP3710i image; a sysupgrade would write an Enterasys flash
> layout over Cisco's `flashfs` and the bootloader area.

The image built in §3 (Ethernet + both Wi-Fi drivers + MAC hook + the repository's JFFS2 overlay hook)
was installed on the test unit entirely over the console cable. The Cisco software was left on
`flash:`; only one file is added and one environment variable changed.

```text
ap: set_baud 115200
ap: copy xmodem:openwrt25.bin flash:/openwrt25.bin
Begin the Xmodem or Xmodem-1K transfer now...
....................................................................
File "xmodem:openwrt25.bin" successfully copied to "flash:/openwrt25.bin"
ap: dir flash:
22   -rwx  8208384   <date>               openwrt25.bin
10599936 bytes available (30558720 bytes used)
ap: set BOOT flash:/openwrt25.bin        (was flash:/ap3g2-k9w8-mx.153-3.JF14/ap3g2-k9w8-mx.153-3.JF14)
ap: boot
Loading "flash:/openwrt25.bin"...*************************************
File "flash:/openwrt25.bin" uncompressed and installed, entry point: 0x1600000
executing...
```

`tools/xmodem_boot.py <image> --install` runs exactly this sequence (it refuses to change `BOOT`
unless ROMMON confirmed the copy). The copy took 13 min 58 s for 8.2 MiB (≈9.9 kB/s while ROMMON
writes the flash). The file occupies 8.2 MiB of the 18.8 MiB that were free.

| Check | Result |
|---|---|
| First boot from `flash:` | JFFS2 mount fails on the Cisco block headers, the hook erases `rootfs_data` once (≈9 s), mounts it and pivots: `[overlay] Mounted JFFS2 overlay on /`, 3.6 MiB free |
| Second boot (power cycle, `boot` typed in ROMMON) | overlay mounted without erase; a test file and a `uci commit` made before the power cycle are present |
| Third boot (PoE off/on, **nobody on the console**) | AP answers ping 114 s after power-on; same file and setting still present |
| Ethernet MAC after a boot from `flash:` | the unit's real address (§5), not the fallback |
| Radios after a boot from `flash:` | `phy0` and `phy1` registered about 100 s after kernel start (loaded from `rc.local`) |

So on this unit ROMMON's `flashfs fsck` did **not** disturb `0x03800000–0x03c00000` across two cold
boots — the open "verify cold-reboot JFFS2 retention" item, answered for 25.12.6 on a 40 MB-format
`-E-K9` unit. Two power cycles are not a long-term test.

Rollback is `ap: set BOOT <previous value>`. Going back to the Cisco software after the overlay was
formatted was **not tested**; the 4 MiB that the overlay uses held only empty `flashfs` blocks on
this unit.

Two things in the image keep a user away from the generic upgrade path: `/lib/upgrade/platform.sh`
makes `sysupgrade` (and therefore LuCI's upload form) fail with an explanation, and LuCI's
"System running in recovery (initramfs) mode — Go to firmware upgrade" banner is removed, because on
this board the system always runs from an initramfs and the banner invites exactly the wrong click.
Both were applied by hand on the running test unit and then added to `tools/build_image.py`; the
image produced by the final revision of that script has the same root file system apart from these
two files but has not itself been booted.

---

## 7. Bench notes that may save time

### 7.1 Reaching ROMMON without credentials or the MODE button

Power-cycle the AP (PoE off/on) and send `ESC` while the bootloader runs `flashfs fsck`. With the
factory default `ENABLE_BREAK=yes` it stops with *"The system boot has been aborted"* at `ap:`.
No `Interrupt within 5 seconds` prompt is needed, and it works on a lightweight (CAPWAP) AP whose
console login is unknown.

### 7.2 RAM boot over the console cable only (`tools/xmodem_boot.py`)

The bootloader has an XMODEM file system:

```text
ap: set_baud 115200
ap: boot xmodem:image.bin
Loading "xmodem:image.bin"...Begin the Xmodem or Xmodem-1K transfer now...
```

An 8.2 MiB image takes 12 min 10 s at 115200 baud (XMODEM-1K, ≈11.2 kB/s). No TFTP server, IP
address or firewall rule is involved, which makes it a good fallback when the AP and the build host
are in different subnets. `copy xmodem:<name> flash:/<name>` works the same way.

The console speed is not restored by the wrapper: its messages and the first kernel line come out
at 115200, the rest at 9600 once the 8250 driver applies `console=ttyS0,9600`.

### 7.3 `boot` after `ether_init` resets the bootloader for non-network sources

```text
ap: ether_init
ap: boot xmodem:image.bin
Rebooting system to reset DPAA...
```

After the Ethernet (DPAA) has been initialised, a `boot` from `xmodem:` (and presumably `flash:`)
restarts the bootloader, which then autoboots whatever `BOOT` points at. Use a fresh `ap:` prompt
for XMODEM boots; `boot tftp://…` is not affected.

### 7.4 TFTP across a stateful firewall

Classic TFTP servers (`tftpd-hpa`) answer from a new ephemeral UDP port, so when the AP and the
server are in different VLANs the firewall sees the reply as an unrelated flow and drops it; ROMMON
only reports `connection timed out`. Either keep both in one L2 segment, allow UDP from the server
to the AP, or use a single-port server (`dnsmasq --enable-tftp --tftp-single-port`).

### 7.5 Differences seen on this unit

| | This unit (`-E-K9`) | Notebook unit (`-A-K9`) |
|---|---|---|
| NOR flash JEDEC ID | `0x0001:0x2301` | `0x0089:0x227e` |
| ROMMON image in RAM (`ap: memory`) | text at `0x0a000000` | `0x06000000` (from `ap3g2-bl-3600`) |
| `SVR` / `PVR` (`ap: dump_regs`) | `0x80fc0011` / `0x80212151` | — |
| Flash contents `0x02800000–0x03c00000` | empty `flashfs` blocks (16-byte headers only) | same |
| `ap: show_pci` before `init_pci` | all four ports "Device not present" | — |

### 7.6 Safety when testing on a production LAN

* The current 23.05 image starts a DHCP client and an **open** SSID `OpenWRT` bridged to the LAN.
  Fine on an isolated bench, not on a home or office network — set a key or `option disabled '1'`.
* 25.12.6's own `wifi config` default is safe: both `wifi-iface` sections get `option disabled '1'`.
* Stock OpenWrt initramfs defaults are `192.168.1.1/24` plus a DHCP server; the repack scripts already
  replace both — keep it that way.

---

## 8. Files in this change

| File | Change |
|---|---|
| `tools/repack_openwrt25_image.py` | load/entry `0x02000000` → `WRAPPER_LINK_ADDR` (`0x01600000`) |
| `tests/test_repack_checks.py` | test now asserts the link address instead of "≥ `0x01800000`" |
| `tools/wrapper_info.py` | new: link address / kernel size of any official simpleImage |
| `tools/xmodem_boot.py` | new: RAM boot through the console cable |
| `driver/mwl8k/mwl8k.c`, `driver/mwl8k/Makefile` | channel-context emulation ops (guarded) |
| `driver/mwlwifi-local.patch` | same two additions, patch regenerated against `db97edf` |
| `tools/build_image.py` | new: parameterised builder that produced the verified 25.12.6 image (static IP or DHCP, MAC hook, overlay, no sysupgrade) |
| `tests/test_xmodem_boot.py` | new: runs `xmodem_boot.py` (RAM boot and `--install`) against a simulated ROMMON on a pty |
| `.github/workflows/ci.yml` | `pyserial` added so that test runs in CI |
| `docs/QUICK_INSTALL.md` | new: short install guide |
| `docs/OPENWRT_25_PORT.md` | this document |
| `README.md` | status, quick-install pointer, sysupgrade warning |

`tools/xmodem_boot.py` and `tools/build_image.py` are cleaned-up versions of the scripts used on the
bench. The XMODEM sender and the ROMMON command sequence are the ones that ran on hardware; the
packaged `--install` / `--wait-rommon` flow has so far only run against the simulated bootloader in
`tests/test_xmodem_boot.py`. `repack_openwrt25_image.py` received the load-address fix but was not
re-run on this bench.
