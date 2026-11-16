# Throughput: why Wi-Fi was slow, what is fixed, what is left

**Contribution by:** Marco Mata, 2026-11-16. Measurements on an `AIR-CAP3702I-E-K9` and an
`AIR-CAP2602I-E-K9` running OpenWrt 25.12.6 (Linux 6.12.112) side by side on one gigabit switch.
The two defects fixed here are in the device tree and in the boot environment, not in 25.12.6:
they apply to the 23.05.5 image in exactly the same way.

## 1. Starting point

Bench report from the repository author (client `wlp9s0`, iperf3 server on a wired host):

| Path | Result | Expected |
|---|---|---|
| Wired host ↔ wired host | 935–940 Mbit/s | line rate — the bench is not the bottleneck |
| 5 GHz (`radio0`, mwlwifi), PHY 585/351 Mbit/s at −54 dBm | **30–71 Mbit/s**, heavy TCP retransmissions, VHT40 faster than VHT80 | 200–350 Mbit/s |
| 2.4 GHz (`radio1`, mwl8k), −60 dBm | **1.2–14.6 Mbit/s**, MCS 1 from the AP, 1-second stalls | 30–70 Mbit/s |

A PHY rate of 585 Mbit/s with 30–71 Mbit/s of payload means the radio link is not what limits
5 GHz. The measurements below explain most of it without involving a Wi-Fi client at all.

## 2. Measurements

All transfers are 80–100 MiB of zeros served from tmpfs by `uhttpd` and fetched with
`uclient-fetch -O /dev/null`; both ends terminate TCP on the AP's own CPU.

| Test | L2 cache off (as shipped) | L2 cache on |
|---|---|---|
| 3702i → 2602i over Ethernet | 120 Mbit/s, receiver CPU 95 % | 165–183 Mbit/s |
| 2602i → 3702i over Ethernet | 125 Mbit/s, receiver CPU 100 % | 165–175 Mbit/s |
| 3702i loopback (no NIC) | 171 Mbit/s | 200–229 Mbit/s |
| 2602i loopback (no NIC) | 194 Mbit/s | 238 Mbit/s |
| `dd` of 100 MiB from tmpfs | 213–296 MB/s | 308 MB/s |

CPU split on the receiver during the Ethernet test (L2 off): user 25 %, system 42 %, softirq 33 %;
59 141 frames in 7 397 NAPI polls.

Reading these: the AP cannot terminate more than about 120 Mbit/s of TCP on a 1 Gbit/s link, and
only 171 Mbit/s when no network hardware is involved. The limit is the CPU, for three reasons —
two of them were defects.

## 3. Defect 1 — the L2 cache was never enabled (fixed)

```text
l2cache: L2CTL=0x10000000 (disabled, size field 1)      <- both units, as booted by ROMMON
l2cache: L2CTL now 0xd0000000 (ENABLED)
```

On MPC85xx the 256 KiB front-side L2 cache is switched on by the boot loader (U-Boot prints
`L2: 256 KiB enabled`); Linux only uses the controller for EDAC. Cisco's ROMMON hands over with it
off, so everything ran on the 32 KiB L1 caches.

Fix: `driver/l2cache.c`, a 60-line module that performs U-Boot's sequence (`L2CTL = L2E | L2I`
with interrupts off). `tools/build_image.py` loads it from `/etc/modules-boot.d/00-l2cache`
(`l2cache enable=1`), i.e. by `kmodloader` before preinit. Effect: +35…45 % on the Ethernet tests
above. Enabled at run time on both units and again at boot on the 3702i; no instability seen, but
the longest run so far is under an hour.

## 4. Defect 2 — the 2.4 GHz radio had no interrupt (fixed)

`/proc/interrupts` on the 3702i, 2.4 GHz interface up:

```text
 17:     342368   OpenPIC     4 Level     mwlwifi
 18:          0   OpenPIC     5 Level     mwl8k        <- never fires
```

The device tree routed INTA of the controller at `0xa000` to MPIC source 5. The P1023 routes that
controller's INTx to external interrupts 0–3 (controller `0x9000` → 4–7, which is why `mwlwifi`
on source 4 works). With `mwl8k` forced onto source 0:

```text
 19:       2452   OpenPIC     0 Level     mwl8k        <- ~50/s with a beaconing AP interface
```

| | INTA → MPIC 5 (before) | INTA → MPIC 0 (after) |
|---|---|---|
| mwl8k interrupts | 0 | ≈50/s idle AP |
| 2.4 GHz scan | 0 BSS | 9+ BSS, −53 … −83 dBm |
| `failed to set key (1, ff:ff:ff:ff:ff:ff) to hardware (-110)` | on every start | gone |

Without the interrupt the driver only completed firmware *commands* through the 200 ms
`OPC_DONE` polling fallback; RX and TX-done events were never serviced. That is the MCS 1 /
stalling behaviour in the bench report.

Fix: `dts/cisco-3702i.dts` and `dts/cisco-2602i.dts`, `pcie@e000a000` interrupt-map
`&mpic 0 2 / 1 2 / 2 2 / 3 2`. INTA is verified on hardware; INTB–D follow the SoC convention and
are not exercised by these boards.

The `Command … took 9990 ms` lines were never real delays: the wait was shortened to 200 ms but
the message still subtracted from the 10 s constant. With the interrupt working the same arithmetic
printed 9800 ms. The base is corrected, so the line now only appears for genuinely slow commands.

## 5. Measured with a client: 5 GHz is limited by packets per second, not by the radio

One 802.11ac phone associated to the 3702i on channel 36 / VHT80 (WPA2-PSK, L2 cache on).
Traffic generated on the AP itself with a small UDP sender (`sendto()` in a loop to the client).

| Path | Packet size | Delivered | Packets/s | AP CPU | Notes |
|---|---|---|---|---|---|
| 5 GHz → phone | 1400 B | 58–59 Mbit/s | ≈5 100 | 100 % (softirq 37 %) | link 650 Mbit/s VHT-MCS 7 NSS 2, −40 dBm |
| 5 GHz → phone | 200 B | 8.6 Mbit/s | ≈5 400 | 100 % | same packet rate as with 1400 B |
| Ethernet → 2602i | 1400 B | 136 Mbit/s | ≈11 800 | 100 % | same generator |

During the 15 s run: 76 221 frames queued to the station, **0 retries, 0 failures**, `tx_dropped` 0,
`mwlwifi` 15 900 interrupts/s (about three per frame), no EDAC/PCI errors, CCMP keys uploaded to
the radio (`flags=0x1`, `rx_decrypt: false`) — so no software crypto.

The radio link is healthy and idle most of the time; the AP cannot hand it more than about 5 400
frames per second whatever their size. That is ≈185 µs of CPU per frame on the Wi-Fi transmit path
against ≈85 µs for the same sender over Ethernet, all on one 800 MHz core. A bridged TCP download
pays the Ethernet receive cost on top, which lands in the 30–71 Mbit/s range of the bench report.

Idle latency to the phone was 118–488 ms (30 pings, none lost); with a phone in power-save this
says little and was not investigated. 2.4 GHz was not tested with a client.

## 6. Remaining limit — one CPU core (not fixed)

```text
smp: Bringing up secondary CPUs ...
No cpu-release-addr for cpu 1
smp: failed starting cpu 1 (rc -2)
smp: Brought up 1 node, 1 CPU
```

Linux on e500 starts secondary cores through the ePAPR spin table that U-Boot leaves behind.
ROMMON keeps core 1 in boot hold-off and provides no spin table, and the OpenWrt kernel has no CPU
hotplug (`/sys/devices/system/cpu/cpu1` does not exist), so the core cannot be added after boot.
It has to be released before the kernel starts: a small pre-kernel stage that installs a boot page
(BPTR), a spin table and the secondary start-up code, sets the core's bit in EEBPCR, and a
`cpu-release-addr` / `enable-method = "spin-table"` pair in the device tree. Not attempted yet.

## 7. Remaining limit — the Ethernet driver (not fixed)

`p1023_fman_im.c` drives the FMan in independent mode: no interrupt (250 µs hrtimer → NAPI), one
copy per frame in each direction (`skb_put_data` on RX, bounce buffer on TX), 64-entry rings, no
checksum offload. That is adequate for bring-up and is not the first-order problem — loopback
without any NIC is only 1.4× faster than the wire — but it adds to the per-packet cost that the
single core has to carry for every Wi-Fi frame.

## 8. Not yet explained

* Bench report: the client hears the AP at −54 dBm while the AP reports the client at −76 dBm. A
  351 Mbit/s uplink PHY rate is not possible at a true −76 dBm, so this looks like RSSI scaling in
  the driver rather than a deaf receiver — unverified.
* Both radios run generic Marvell firmware. Cisco IOS loads its own radio images and per-domain
  power tables (`8004.img`, `Q2.bin`/`Q5.bin`, `E2.bin`/`E5.bin` in the IOS directory;
  `loading Power Tables from ram:/Q5.bin. Class = E` at boot), and the radio MAC addresses read
  back as placeholders (`…:50:ff:ff`, `…:50:00:00`; on the 2602i even multicast addresses). Whether
  per-unit calibration is missing, and what it costs in range, has not been measured.
  **Keep a copy of the IOS directory before cleaning a unit's flash** — it is the only reference
  for this.

## 9. What to re-test

With an image built from this branch (`l2cache` loaded, corrected device tree):

1. `cat /proc/interrupts` — both `mwlwifi` and `mwl8k` must count; `dmesg | grep l2cache` must
   say `ENABLED`.
2. The same iperf3 runs as in section 1, with `top` on the AP during the run. If the AP sits at
   ~100 % in softirq/system during the 5 GHz test, the single core is the limit and section 6 is
   the next step; if it does not, the radio side (section 8) is.
3. 2.4 GHz again: MCS index and stalls should be gone or the interrupt fix did not take.

The only client measurement on this bench is section 5 (one phone, 5 GHz, AP-generated UDP).
Sections 3 and 4 are verified by the AP-side numbers only; a proper iperf3 run through the AP on
both bands is still needed.
