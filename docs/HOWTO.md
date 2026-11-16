# How-To Guide: Loading & Building OpenWrt for the Cisco Aironet 3702i

This guide explains both **how to load a pre-built `openwrt-3702i.bin` image** onto a Cisco Aironet AIR-CAP3702I (via safe TFTP RAM-boot or persistent NOR flash) and **how to compile `openwrt-3702i.bin` from source** on `ld-branch`.

---

## Hardware & Prerequisites

- **Target Hardware:** Cisco Aironet 3702i (`AIR-CAP3702I-x-K9`, Freescale P1020/P1023 dual-core `e500v2` @ 800 MHz, 512 MiB DDR3, 64 MiB NOR flash).
- **Console Access:** Cisco RJ-45 rollover serial cable (`9600 8N1`, no flow control).
- **Network / Power:** 802.3at/af PoE+ switch or injector, plus a TFTP server on your workstation (e.g., `192.168.1.153`) reachable from the AP's Ethernet port.

---

## Part 1: Loading `openwrt-3702i.bin` onto Your 3702i

### Option A: Safe TFTP RAM-Boot (Non-Destructive Testing)

RAM-booting loads OpenWrt directly into DDR3 without touching your existing Cisco IOS flash image. A power cycle returns the AP to ROMMON / IOS.

1. **Enter the ROMMON `ap:` prompt:**
   - Connect your serial terminal (`minicom -D /dev/ttyUSB0 -b 9600` or `screen /dev/ttyUSB0 9600`).
   - Power-cycle the AP and repeatedly press `ESC` when `Hit ESC to abort` or `Loading...` appears, or hold the physical **MODE** button until the status LED turns amber.
2. **Configure networking and boot over TFTP:**
   ```text
   ap: set IP_ADDR 192.168.1.243
   ap: set NETMASK 255.255.255.0
   ap: set DEFAULT_ROUTER 192.168.1.1
   ap: ether_init
   ap: tftp_init
   ap: boot tftp://192.168.1.153/openwrt-3702i.bin
   ```
   *(Replace `192.168.1.153` with your TFTP server IP and `192.168.1.243` with a free IP on your subnet.)*
3. **Wait ~35 seconds** for Linux (`smp: Brought up 1 node, 2 CPUs`) and `procd` to finish booting, then press `Enter` on the serial console.
   - Default OpenWrt LAN IP: `192.168.1.50` (configured on `br-lan` / `eth0`).
   - Default Wi-Fi SSID: `OpenWRT` (5 GHz `VHT80` on `phy0-ap0` and 2.4 GHz `HT20` on `phy1-ap0`).

### Option B: Installing `openwrt-3702i.bin` to NOR Flash (Standalone Cold Boot)

> [!WARNING]
> Back up your `flash:` contents or keep a copy of your Cisco IOS image before deleting files from `flash:`.

1. At the `ap:` prompt, initialize flash, Ethernet, and TFTP:
   ```text
   ap: flash_init
   ap: set IP_ADDR 192.168.1.243
   ap: set NETMASK 255.255.255.0
   ap: set DEFAULT_ROUTER 192.168.1.1
   ap: ether_init
   ap: tftp_init
   ap: dir flash:
   ```
2. Ensure at least ~8 MB is free on `flash:` (delete old `ap3g2-k9w7-mx...` IOS images if needed), then copy `openwrt-3702i.bin` to `flash:`:
   ```text
   ap: copy tftp://192.168.1.153/openwrt-3702i.bin flash:/openwrt-3702i.bin
   ap: set BOOT flash:/openwrt-3702i.bin
   ap: unset MANUAL_BOOT
   ap: boot
   ```
   The AP will now cold-boot into OpenWrt automatically on every PoE power cycle.

---

## Part 2: Compiling `openwrt-3702i.bin` from Source

### 1. Clone the Repository (`ld-branch`)
```bash
git clone -b ld-branch https://github.com/leifdavisson/cisco-openwrt.git
cd cisco-openwrt
```

### 2. Host Build Dependencies (Ubuntu / Debian)
```bash
sudo apt-get update
sudo apt-get install -y build-essential git python3 python3-serial fakeroot \
  xz-utils cpio device-tree-compiler u-boot-tools
```

### 3. Prepare the OpenWrt `mpc85xx/p1020` SDK & Kernel Tree
The out-of-tree kernel modules (`p1023_fman_im.ko`, `radio_reset.ko`, `mwlwifi.ko`, `mwl8k.ko`) and `boot/smp` pre-kernel stage build against the OpenWrt 23.05.5 `mpc85xx/p1020` Linux `5.15.167` kernel and `powerpc-openwrt-linux-musl-` toolchain located under `sdk/`.

If `sdk/` is not yet populated in your checkout, download/extract the OpenWrt 23.05.5 `mpc85xx/p1020` SDK/ImageBuilder into `sdk/` and ensure the kernel build tree (`linux-5.15.167`) and `backports-6.1.110-1` headers are prepared.

### 4. Build All Drivers & Repack `openwrt-3702i.bin`
`tools/repack_openwrt_image.py` automates the entire build and packaging pipeline:
- Compiles `driver/p1023_fman_im.ko` (zero-copy FMan v2 Ethernet) and `driver/radio_reset.ko` (256 KB L2 cache + GPIO radio reset)
- Compiles `driver/mwlwifi/mwlwifi.ko` (88W8864 5 GHz 802.11ac with PowerPC Big-Endian & SMP `tx_csd` fixes)
- Compiles `driver/mwl8k/mwl8k.ko` (88W8764 2.4 GHz 802.11n with lockless `MWL8K_CMD_BASTREAM` + `ap_tx_ampdu=true`)
- Compiles the `dts/cisco-3702i.dts` Device Tree and `boot/smp` pre-kernel CPU1 spin-table stage (`boot/smp/bootpg.S`, `boot/smp/shim.S`)
- Packs everything into a Cisco ROMMON MZIP image at `/home/$USER/Downloads/openwrt-3702i.bin`

Run:
```bash
python3 tools/repack_openwrt_image.py
```
When complete, `/home/$USER/Downloads/openwrt-3702i.bin` (~7.0 MB) is ready to serve via TFTP.

---

## External Project References & Attribution

- **[OpenWrt Project](https://openwrt.org/)** — [GitHub Repository](https://github.com/openwrt/openwrt) | [mpc85xx Target Documentation](https://openwrt.org/docs/techref/targets/mpc85xx)
- **[mwlwifi (Marvell 88W8864 mac80211 Driver)](https://github.com/kaloz/mwlwifi)** — Upstream 802.11ac driver and firmware repository.
- **[Linux Kernel `mwl8k` Driver & Firmware](https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwl8k.html)** — [linux-firmware `mwl8k` directory](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mwl8k)

## License

Unless otherwise specified by upstream kernel/driver components (GPLv2), original scripts and documentation in this repository are licensed under the **GNU Affero General Public License v3.0 (GNU AGPLv3)**.
