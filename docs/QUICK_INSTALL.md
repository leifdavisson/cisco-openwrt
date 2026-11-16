# Quick install: OpenWrt on a Cisco Aironet 3702i

A short path for someone who just wants the AP running OpenWrt. The background, measurements and
open issues are in [OPENWRT_25_PORT.md](OPENWRT_25_PORT.md) and the
[lab notebook](OPENWRT_PORTING_LAB_NOTEBOOK.md).

> **Never use LuCI's "Flash new firmware" page or `sysupgrade` on this AP.**
> OpenWrt here is a single file stored on Cisco's own flash file system and loaded into RAM by the
> Cisco bootloader. A normal OpenWrt upgrade does not know that layout and would overwrite the Cisco
> file system and the bootloader area. Images built with `tools/build_image.py` refuse to do it;
> to update, repeat this guide with a new image.

## What you need

| | |
|---|---|
| The AP | `AIR-CAP3702I-x-K9` (tested: `-E-K9`, 40 MB flash format). Lightweight or autonomous, no password needed. |
| | The 2602i installs the same way but has no Wi-Fi or saved settings yet — see [AP2602_NOTES.md](AP2602_NOTES.md). |
| Power | 802.3at PoE switch or injector |
| Console | Cisco RJ45 console cable + USB serial adapter (9600 8N1) |
| A Linux PC | Python 3.10+ and `pip install pyserial` |
| An image | `openwrt-3702i.bin` built with `tools/build_image.py` (see below) |

No TFTP server, IP plan or firewall rule is needed for the install itself.

## 1. Build the image (once, on the PC)

Follow section 3 of [OPENWRT_25_PORT.md](OPENWRT_25_PORT.md) to get the SDK, the official
WS-AP3710i initramfs image and the kernel modules, then:

```bash
python3 tools/build_image.py 25 \
    --official openwrt-25.12.6-mpc85xx-p1020-enterasys_ws-ap3710i-initramfs-kernel.bin \
    --sdk ~/openwrt-sdk-25.12.6-mpc85xx-p1020_gcc-14.3.0_musl.Linux-x86_64 \
    --ip 192.168.1.5 --gw 192.168.1.1 --wifi \
    --out openwrt-3702i.bin
```

Use `--dhcp` instead of `--ip/--gw` if the AP should ask your router for an address.
The image never runs a DHCP server and keeps both radios off until you configure them.

## 2. Connect

1. Console cable into the AP's `CONSOLE` port, USB adapter into the PC (usually `/dev/ttyUSB0`).
2. Network cable from the PoE switch to the AP — but leave it **unplugged** for now.

## 3. Install

```bash
python3 tools/xmodem_boot.py openwrt-3702i.bin --install --wait-rommon
```

When it says *"power-cycle the AP now"*, plug in the PoE cable (or unplug and replug it).
The script then

1. stops the Cisco bootloader at its `ap:` prompt,
2. copies the image to `flash:/openwrt-3702i.bin` over the console cable (about 14 minutes — do not
   unplug anything),
3. sets `BOOT=flash:/openwrt-3702i.bin` (and prints the old value, which is your way back),
4. boots it and records the console in `xmodem_boot.log`.

Nothing of the Cisco software is deleted. If the copy fails, `BOOT` is left alone and the AP still
boots Cisco IOS.

## 4. First login

About two minutes after power-on the AP answers on the address you chose (or the one your DHCP
server gave it):

* Web: `http://<address>/` — user `root`, no password. **Set one immediately** (System → Administration).
* SSH: `ssh root@<address>`

Settings are saved on a 4 MiB flash area and survive reboots and power cuts. The first boot takes a
little longer because that area is formatted once.

## 5. Turn on Wi-Fi

Both radios are present but disabled. In LuCI: Network → Wireless → Edit, choose a channel, set
**WPA2/WPA3 with a key**, enable. On the command line:

```sh
uci set wireless.default_radio0.ssid='MyNetwork'
uci set wireless.default_radio0.encryption='sae-mixed'
uci set wireless.default_radio0.key='a-long-passphrase'
uci set wireless.default_radio0.disabled='0'
uci commit wireless && wifi reload
```

Status on OpenWrt 25.12.6: the 5 GHz radio (`radio0`) is detected and scans; AP mode on 25.12.6 and
the 2.4 GHz radio (`radio1`) are not confirmed yet — see the status table in
[OPENWRT_25_PORT.md](OPENWRT_25_PORT.md). On 23.05.5 both radios work as access points.

## Optional: remove the Cisco software

Once OpenWrt boots reliably you can free the flash for a standalone AP. This is permanent — there
is no way back to Cisco IOS afterwards unless you have an image of your own. At the `ap:` prompt:

```bash
python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs            # list only
python3 tools/rommon_clean_flash.py --keep openwrt-3702i.bin env_vars private-multiple-fs --delete
```

Never delete `env_vars` (it holds `BOOT`) or your OpenWrt image.

## Going back to Cisco IOS

Power-cycle with the console attached, press `ESC` during boot to reach `ap:`, then:

```text
ap: set BOOT <the old value the installer printed>
ap: boot
```

## If something goes wrong

| Symptom | What to do |
|---|---|
| Script never sees the bootloader | Check the serial port and 9600 8N1; power-cycle again. The bootloader only listens for `ESC` in its first ~25 seconds. |
| `Rebooting system to reset DPAA...` | `ether_init` was run in this ROMMON session. Power-cycle and start again without it. |
| AP resets right after `executing...` | The image was packed for the wrong load address — see section 2 of OPENWRT_25_PORT.md. |
| No answer on the network after 3 minutes | Look at `xmodem_boot.log`; the console at 9600 baud shows the whole boot. `ESC` at power-on always gets you back to `ap:`. |

The bootloader itself is never modified by any of these steps, so the AP can always be recovered
from the `ap:` prompt.
