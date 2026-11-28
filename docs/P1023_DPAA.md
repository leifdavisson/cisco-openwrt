# P1023 DPAA Ethernet backend

`p1023_fman_dpaa` uses the P1023 FMan's normal QMan/BMan data path instead
of Independent Mode. It has been RAM-booted and tested on an
AIR-CAP3702I-E-K9. Other models, including the 3602I, have not been verified.
The existing IM backend remains the default for the current SDK/repack tools.

## Data path

- RX uses 512 BMan buffers backed by DMA-mapped page-pool pages. A received
  page becomes an SKB; a replacement goes to BMan before the SKB reaches the
  stack. Pages are recycled only after the stack releases them.
- TX maps the SKB head and fragments directly. A 16-entry scatter/gather
  table replaces the packet copy. The SKB and DMA mappings remain alive
  until QMan confirms transmission or rejects the enqueue.
- Portal interrupts schedule NAPI. RX runs on CPU 0 and TX confirmations
  on the last affine portal (CPU 1 on the tested board). BQL and the normal
  netdev stop/wake contract handle queue pressure. There is no TX polling
  timer or enqueue retry loop.
- The hardware parser supplies RX checksum status and IPv4/IPv6 TCP/UDP
  TX checksums. Encapsulated packets support SG; their checksums remain in
  software. MTU is limited to 1500. TSO is not advertised.

`cisco_dpaa_bus` establishes the portal LAW and QMan initiator BAR before
publishing the controllers and portals. It preserves ROMMON's existing
window when suitable. `p1023_dpaa_hw.h` contains the board's FMan, parser
and dTSEC setup; it reuses the existing microcode header.

Three kernel **driver** patches are required:

1. `991-p1023-qbman-resources.patch`: respect QMan 2.0's 64 congestion
   groups, BMan 2.0's eight pools, and the actual pool-channel mask.
2. `992-p1023-bman-rcr-cache-order.patch`: on e500, an RCR entry spans two
   32-byte cache lines. Flush the second line before publishing VALID in
   the first; otherwise BMan can consume stale buffer addresses.
3. `993-p1023-dpaa-kconfig.patch`: select the external driver's dependencies.
   In Linux 5.15, `PAGE_POOL` is hidden; a fragment alone does not enable it.

These patches touch only Freescale hardware drivers and their Kconfig.

The contribution was also compile-checked against a full upstream Linux
5.15.167 build with the three patches and OpenWrt GCC 12.3.0: both DPAA
modules passed modpost/linking, and the alternative board DT compiled
without warnings. That check does not replace booting a complete rebuilt
OpenWrt image; the hardware results below use Linux 5.15.132.

## Build and image integration

Start with the board's prepared OpenWrt kernel source/build tree. Apply the
patches before configuring it. Example using the repository's SDK layout:

```sh
repo=$PWD
export ARCH=powerpc
export STAGING_DIR="$repo/sdk/staging_dir"
export CROSS_COMPILE="$STAGING_DIR/toolchain-powerpc_8548_gcc-12.3.0_musl/bin/powerpc-openwrt-linux-musl-"
export KDIR="$repo/sdk/build_dir/target-powerpc_8548_musl/linux-mpc85xx_p1020/linux-5.15.167"

for patch in "$repo"/patches/99*-p1023-*.patch; do
    patch --fuzz=0 -p1 -d "$KDIR" < "$patch"
done
cd "$KDIR"
scripts/kconfig/merge_config.sh -m .config "$repo/driver/dpaa.config"
make olddefconfig
make -j"$(nproc)" vmlinux modules
cd "$repo"
make -C driver ETHERNET=dpaa KDIR="$KDIR" CROSS="$CROSS_COMPILE"
dtc -I dts -O dtb -o /tmp/cisco-3702i-dpaa.dtb dts/cisco-3702i-dpaa.dts
```

For a full OpenWrt source build, apply these patches in the target's kernel
patch series and merge the fragment into its kernel configuration instead
of editing a disposable build directory. Use the same board/SMP kernel
configuration and packaging steps as the existing image build.

`PHYS_64BIT` changes the DMA address type, although the CPU/kernel remain
32-bit. **Rebuild the kernel and every module together**, including the radio
modules. The new Ethernet modules cannot be dropped into the existing
prebuilt SDK/ImageBuilder image. The current `repack_openwrt*_image.py` tools
still package the IM backend; they do not perform this kernel rebuild.

Package the rebuilt kernel, all matching modules and the DPAA DTB using the
existing Cisco/SMP image wrapper. Replace the IM module/autoload entry with:

```text
marvell
fsl_pq_mdio
cisco_dpaa_bus
p1023_fman_dpaa
```

Built-in MDIO support needs no module entry. The DPAA board tree inherits
the original board's SMP reservation and flash layout, removes the unused
eTSEC/MDIO nodes and adds the portals and FMan MDIO. Do not load IM and DPAA
together. The generic `FSL_FMAN`/`FSL_DPAA_ETH` drivers remain disabled.

The additional RAM reservations are BMan FBPR at `0x1c000000` (16 MiB),
QMan FQD at `0x1d000000` (4 MiB), and QMan PFDR at `0x1e000000` (32 MiB).
The board must have 512 MiB RAM. They do not change flash partitions.

Power-cycle into ROMMON, then TFTP/RAM-boot the candidate. A warm IOS handoff
with existing QMan/BMan RAM addresses is rejected to avoid DMA into memory
now owned by Linux. A software kernel reboot is not a substitute for this
power cycle. No flash update is needed to test the driver.

The driver preserves a valid ROMMON MAC. If it is absent, Linux assigns a
random address; set the EEPROM MAC from userspace before bringing the
interface up. The driver does not contain a hardcoded device identity.

## Lifetime and diagnostics

The board modules remain loaded until reboot. Unloading/unbinding and CPU
hotplug are not supported. Interface down/up preserves queues and DMA
buffers, stops the ports, and allows outstanding TX confirmations to drain.
A DMA/queue fault is logged and detaches the interface; recovery requires
rebooting, rather than silently recycling hardware-owned buffers.

`ip -s link show eth0` reports traffic/errors. `/proc/p1023_fman_regs` exposes
the port state, FQ occupancy, QMan/BMan error registers, TX pending/SG counts
and per-CPU NAPI/RX-checksum counters.

## Hardware validation and iperf3

Measurements below were taken on 2026-11-28 with the final driver on a
separate RAM-booted validation image: Linux 5.15.132, OpenWrt GCC 12.3.0,
two 800 MHz e500v2 cores, 512 MiB RAM. AP management and radio services were
running, with no WLAN client traffic during these Ethernet runs. These are
not measurements of a newly packaged image from this branch.

The peer was an iperf3 3.12 server in a VM, connected through a USB Ethernet
adapter passed through at 5000 Mb/s, negotiating 1000 Mb/s full duplex.
The AP ran iperf3 3.17.1. One TCP stream, MTU 1500, 20 measured seconds
after three omitted warmup seconds, three runs per direction; no `-Z`.
Rates are the receiver's reported decimal Mbit/s.

| Direction at AP | Run 1 | Run 2 | Run 3 | Median | Median AP CPU busy |
| --- | ---: | ---: | ---: | ---: | ---: |
| TX | 607.9 | 642.9 | 570.8 | **607.9** | 64.4% |
| RX | 803.6 | 778.0 | 818.9 | **803.6** | 89.9% |

[Per-run results and CPU figures](P1023_DPAA_BENCHMARK.json) include TCP
retransmits. TX retransmits varied from 25 to 3133; the setup does not
establish a line-rate ceiling or a controlled before/after comparison.
CPU busy is the whole AP's `/proc/stat` delta over both cores, including
warmup and sampling overhead, excluding idle/iowait. Thus 50% is one full
core. It is not iperf's own process CPU usage.

Equivalent commands on the AP, with an iperf3 server at `SERVER`:

```sh
iperf3 -c SERVER -J -t 20 -O 3 -P 1
iperf3 -c SERVER -J -t 20 -O 3 -P 1 -R
```

Across the final validation session, including separate tunnel tests,
the interface processed 10,598,867 RX and 7,510,876 TX packets with zero
netdev errors/drops; QMan/BMan error registers remained zero. This does
not mean there were no TCP retransmissions. A ten-second idle sample
showed 2.56% whole-AP CPU busy, with 17 RX-portal and 23 confirmation-portal
NAPI polls. DHCP, SSH, checksum handling, interface down/up and a 16 MiB
byte-identical file round trip were also checked.

## References

- [Linux 5.15 DPAA driver](https://github.com/gregkh/linux/blob/v5.15.132/drivers/net/ethernet/freescale/dpaa/dpaa_eth.c): SG format, confirmation and IRQ/NAPI contracts.
- [NXP P1023 integration](https://github.com/nxp-qoriq/linux/blob/95448dd0dc9b621ae027cbefedaaa7c3d0d3ad2d/drivers/net/ethernet/freescale/sdk_fman/inc/integrations/P1023/dpaa_integration_ext.h): resource limits and errata.
- [NXP P1023 device tree](https://github.com/nxp-qoriq/linux/blob/95448dd0dc9b621ae027cbefedaaa7c3d0d3ad2d/arch/powerpc/boot/dts/fsl/p1023si-post.dtsi): portal geometry and interrupts.
- [U-Boot portal setup](https://github.com/u-boot/u-boot/blob/v2016.11/arch/powerpc/cpu/mpc85xx/portals.c): QMan initiator BAR.
