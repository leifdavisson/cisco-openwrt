# Quick install: OpenWrt on a Cisco Aironet 3702i

From a stock Cisco AP to a standalone OpenWrt access point with a console cable, PoE and one
command. Background and measurements: [OPENWRT_25_PORT.md](OPENWRT_25_PORT.md),
[WIFI_PERFORMANCE.md](WIFI_PERFORMANCE.md), [SMP.md](SMP.md).

> **Never use LuCI's "Flash new firmware" page or `sysupgrade` on this AP.**
> OpenWrt here is a single file on Cisco's own flash file system, loaded into RAM by the Cisco
> bootloader. A normal OpenWrt upgrade does not know that layout and would overwrite the Cisco file
> system and the bootloader area. The image refuses to do it; to update, repeat step 3 with a new image.

## What you get

OpenWrt 25.12.6 (Linux 6.12) with both CPU cores, the L2 cache enabled, gigabit Ethernet with the
unit's real MAC address, both radio drivers loaded (radios off until you configure them), and
settings that survive reboots. Open items are listed at the end.

## What you need

| | |
|---|---|
| The AP | `AIR-CAP3702I-x-K9` (tested: `-E-K9`). Lightweight or autonomous, no Cisco password needed. |
| Power | 802.3at PoE switch or injector |
| Console | Cisco RJ45 console cable + USB serial adapter |
| A Linux PC | Python 3.10+ with `pyserial` (`pip install pyserial`) and this repository checked out |

No TFTP server, IP plan or firewall rule is needed for the install.

## 1. Get the image

Prebuilt, as installed on the test unit — from the [`images` branch](../../../tree/images):

```bash
git fetch origin images
git show origin/images:openwrt-25.12.6-cisco-3702i.bin > openwrt-25.12.6-cisco-3702i.bin
git show origin/images:SHA256SUMS | sha256sum -c -
```

Or build it yourself (section 3 of [OPENWRT_25_PORT.md](OPENWRT_25_PORT.md) for the SDK and modules):

```bash
python3 tools/build_image.py 25 --model 3702i --wifi --dhcp \
    --official openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin \
    --sdk ~/openwrt-sdk-25.12.6-mpc85xx-p1020_gcc-14.3.0_musl.Linux-x86_64 \
    --out openwrt-25.12.6-cisco-3702i.bin
```

Use `--ip A.B.C.D --gw A.B.C.1` instead of `--dhcp` to bake in a static address.

## 2. Connect

1. Console cable into the AP's `CONSOLE` port, USB adapter into the PC (usually `/dev/ttyUSB0`).
2. Leave the AP **unpowered** (PoE cable unplugged) for now.

## 3. Install

```bash
python3 tools/xmodem_boot.py openwrt-25.12.6-cisco-3702i.bin --install --wait-rommon
```

When it prints *"power-cycle the AP now"*, plug in the PoE cable. The script then

1. stops the Cisco bootloader at its `ap:` prompt (`ESC` during the flash check),
2. copies the image to `flash:/openwrt-3702i.bin` over the console cable — about 14 minutes; do
   not unplug anything,
3. sets `BOOT=flash:/openwrt-3702i.bin` and prints the old value (your way back),
4. boots it and records the console in `xmodem_boot.log`.

Nothing of the Cisco software is deleted in this step. If the copy fails, `BOOT` is left alone and
the AP still boots Cisco IOS. A healthy boot shows, right after `executing...`:

```text
[smp-shim] L2 cache on, releasing core 1...
[smp-shim] core 1 is in its spin loop
...
smp: Brought up 1 node, 2 CPUs
```

## 4. First login

The AP takes about two minutes to boot (the very first boot a little longer: it formats its 4 MiB
settings area once). It asks your network for an address by DHCP — look for `cisco3702i` in your
router's lease list.

* Web: `http://<address>/` — user `root`, no password. **Set one immediately**
  (System → Administration).
* SSH: `ssh root@<address>`

No DHCP on that network? Use the console (9600 8N1, press Enter):

```sh
uci set network.lan.proto='static'
uci set network.lan.ipaddr='192.168.1.5'
uci set network.lan.netmask='255.255.255.0'
uci set network.lan.gateway='192.168.1.1'
uci set network.lan.dns='192.168.1.1'
uci commit network && /etc/init.d/network reload
```

## 5. Turn on Wi-Fi

`radio0` is the 5 GHz 802.11ac radio, `radio1` the 2.4 GHz 802.11n radio. Both are off until you
enable them. In LuCI: Network → Wireless → Edit. On the command line (set your own country code):

```sh
for r in 0 1; do
    uci set wireless.radio$r.country='US'
    uci set wireless.default_radio$r.ssid='MyNetwork'
    uci set wireless.default_radio$r.encryption='psk2'
    uci set wireless.default_radio$r.key='a-long-passphrase'
    uci set wireless.default_radio$r.disabled='0'
done
uci commit wireless && wifi reload
```

The AP bridges Wi-Fi clients to its Ethernet port; addresses come from your existing router.

## 6. Optional: remove the Cisco software

Once OpenWrt boots reliably you can free the flash for a standalone AP (about 30 MiB on a 3702i).
**This is permanent** — afterwards there is no way back to Cisco IOS unless you have an image of
your own. Power-cycle with the console attached, press `ESC` during boot to reach `ap:`, then:

```bash
python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs            # list only
python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs --delete
```

and type `boot` at the `ap:` prompt (or power-cycle). Never delete `env_vars` (it holds `BOOT`) or
your OpenWrt image. Records of two real runs are in [logs/](logs/).

## Going back to Cisco IOS

Only possible while the Cisco files are still on flash (step 6 not done). Reach `ap:` as above, then:

```text
ap: set BOOT <the old value the installer printed>
ap: boot
```

## If something goes wrong

| Symptom | What to do |
|---|---|
| Script never sees the bootloader | Check the serial port and 9600 8N1; power-cycle again. The bootloader only reacts to `ESC` during its first ~25 seconds. |
| `Rebooting system to reset DPAA...` | `ether_init` was run in this ROMMON session. Power-cycle and start again without it. |
| AP resets right after `executing...` | The image was packed for the wrong load address — section 2 of OPENWRT_25_PORT.md. |
| `[smp-shim] core 1 did NOT come up` | The AP still boots, on one core. Please report the console log. |
| No answer on the network after 3 minutes | Read `xmodem_boot.log`; the console at 9600 baud shows the whole boot. `ESC` at power-on always gets you back to `ap:`. |

The bootloader itself is never modified by any of these steps, so the AP can always be recovered
from the `ap:` prompt.

## Known limits (2026-11-16)

* Wi-Fi throughput is limited by the CPU, not the radio: about 80–120 Mbit/s on 5 GHz in the
  measurements so far. Driver work that should raise it is in progress.
* 2.4 GHz and long uptimes have had little testing; the two-core start-up is one night old.
* `--wait-rommon` has so far only run against a simulated bootloader; the rest of the install
  command has run on real hardware several times.
* The 2602i installs the same way (`--model 2602i`) but cannot save settings yet and its Wi-Fi is
  untested as an access point — see [AP2602_NOTES.md](AP2602_NOTES.md).
