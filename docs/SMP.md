# Second CPU core

**Contribution by:** Marco Mata, 2026-11-16. Verified on an `AIR-CAP3702I-E-K9` with OpenWrt 25.12.6
(Linux 6.12.112): `smp: Brought up 1 node, 2 CPUs`.

## 1. Why Linux only saw one core

```text
smp: Bringing up secondary CPUs ...
No cpu-release-addr for cpu 1
smp: failed starting cpu 1 (rc -2)
```

State left by ROMMON, read from the running system:

| Register | Value | Meaning |
|---|---|---|
| MPIC `FRR` (`+0x41000`) | `0x006b0102` | NCPU field = 1 → two CPUs present |
| ECM `EEBPCR` (`+0x01010`) | `0x01000000` | only `CPU0_EN`: core 1 is still in boot hold-off |
| ECM `BPTR` (`+0x00020`) | `0x8007ffff` | boot page translation enabled, pointing at `0x7ffff000` (no RAM there) |
| GUTS `DEVDISR` (`+0xe0070`) | `0x00000000` | time bases running |

Linux on e500 does not start a secondary core by itself; it expects the firmware to have parked it
in an ePAPR spin table (`cpu-release-addr`), as U-Boot does. ROMMON does not, and the OpenWrt
mpc85xx kernel is built without CPU hotplug, so the core cannot be added after boot either.

## 2. What `boot/smp` does

A small stage that runs on core 0 right after ROMMON's `executing...`, before the OpenWrt boot
wrapper. `tools/build_image.py` appends it to the image (default; `--no-smp` leaves it out).

**How it gets control.** The simpleImage wrapper begins with `b +0x340`. The builder rewrites that
one instruction to branch to the stage, which is placed behind the wrapper's BSS (zero padding in
between keeps the BSS zero) and ends by jumping to `+0x340` itself. The wrapper is otherwise
untouched and still runs at its link address.

| Release | Wrapper load = link address | Stage at |
|---|---|---|
| 25.12.6 | `0x01600000` | `0x01dd6000` (tested) |
| 23.05.5 | `0x01500000` | `0x01bb0000` (tested on 3702i: `smp: Brought up 1 node, 2 CPUs`, `244–258 Mbit/s` 5 GHz TCP with RPS) |

**`boot/smp/shim.S` (core 0):**

0. `HID1 |= ASTME | ABE` (and `MBDD` on core revision ≥ 5.0) on core 0 — see section 2a.
1. L2 cache on: `L2CTL = L2E | L2I` if it is not already enabled (see WIFI_PERFORMANCE.md §3).
2. Copy the 4 KiB secondary boot page to physical `0x0ff00000` and `dcbf` it out of the cache
   (core 1 runs uncached and must see it in RAM).
3. Freeze both time bases: `DEVDISR |= TB0 | TB1` (`0x5000`).
4. `BPTR = 0x80000000 | (0x0ff00000 >> 12)`, then `EEBPCR |= CPU1_EN` (`0x02000000`): core 1
   leaves hold-off and starts at its reset vector, i.e. in the boot page.
5. Wait until the spin-table entry reads "spinning" (`addr_l == 1`).
6. Zero the local time base and restore `DEVDISR`: both cores count from 0 together.
7. Continue into the wrapper. It prints its progress on ROMMON's UART:

```text
executing...
[smp-shim] L2 cache on, releasing core 1...
[smp-shim] core 1 is in its spin loop
```

If core 1 does not answer it prints `core 1 did NOT come up, progress=N` and boots anyway; Linux
then falls back to one CPU.

**`boot/smp/bootpg.S` (core 1, linked at `0xfffff000`):** set `HID1` as in step 0; zero the time base and set `HID0[TBEN]`;
publish the spin-table entry at page offset `0x800` (`addr_l = 1`); spin until the OS writes an
entry address; set `PIR`; create a 64 MiB 1:1 mapping around the entry point in TLB1 entry 1
(the "initial mapped area" Linux's `fsl_booke_entry_mapping.S` looks for with `tlbsx`); `rfi` to
the entry with `r3` from the table and `r7 = 64 MiB`. The reset vector at `0xffc` branches to the
start of the page. Offset `0x900` holds a progress counter for debugging.

### 2a. HID1[ABE] — required, and easy to miss

ROMMON leaves `HID1 = 0x43020000` (PLL bits + `RFXE`). On e500 the cache-management instructions
(`dcbf`, `dcbi`, `icbi`, …) are only broadcast on the bus — and therefore only reach the L2 cache
and the other core — when `HID1[ABE]` (address broadcast enable) is set. U-Boot sets
`ASTME | ABE` (plus `MBDD` on rev ≥ 5.0) on every core of a multi-core e500; Linux only ORs in
`RFXE` and otherwise trusts the firmware. Linux's `flush_icache_range()` relies on `icbi` being
seen by all CPUs, so two cores without ABE can execute stale instructions after module loading or
code patching.

It showed up here as a symptom in the stage itself. The first version did not touch HID1 and did
not enable the L2 cache; it worked. With the L2 cache enabled first, the same code printed

```text
[smp-shim] L2 cache on, releasing core 1...
[smp-shim] core 1 did NOT come up, progress=0
[    0.739943] smp: Brought up 1 node, 2 CPUs
```

— core 1 was in fact spinning (Linux found it, and the time bases turned out to be synchronised:
`tbskew: cpu1 - cpu0 = 41 ticks … measurement window 110 ticks`, the same value in both
directions, i.e. IPI latency rather than an offset), but core 0 kept reading the pre-release
contents of the spin table: its `dcbf` was not broadcast, so the L2 copy of that line was never
dropped. Both `shim.S` and `bootpg.S` now set HID1 first; `driver/l2cache.c` does the same on
every online CPU for images built with `--no-smp`.

**Device tree** (`dts/cisco-3702i.dts`, `dts/cisco-2602i.dts`):

```dts
/memreserve/ 0x0ff00000 0x00001000;
...
PowerPC,P1020@1 {
	...
	enable-method = "spin-table";
	cpu-release-addr = <0x0 0x0ff00800>;
};
```

`0x0ff00000` (255 MiB) is RAM on both the 512 MiB 3702i and the 256 MiB 2602i, far from the kernel
image, the wrapper and its heap.

## 3. Result

```text
[    0.624542] smp: Bringing up secondary CPUs ...
[    0.743692] smp: Brought up 1 node, 2 CPUs
```

```console
# cat /sys/devices/system/cpu/online
0-1
# grep -E 'processor|bogomips' /proc/cpuinfo
processor	: 0
bogomips	: 80.00
processor	: 1
bogomips	: 80.00
total bogomips	: 160.00
```

IPIs and local timer interrupts count on both CPUs; no clocksource or scheduler-clock warnings.

| Test on the 3702i (L2 cache on in every row) | 1 core | 2 cores |
|---|---|---|
| TCP to/from a single-core 2602i over Ethernet | 165–183 Mbit/s | 191–203 Mbit/s |
| 5 GHz → phone, one UDP sender, 1400 B | 59 Mbit/s, 5 100 frames/s, CPU 100 % | 80 Mbit/s, 6 870 frames/s, CPU 64 % of two cores |
| 5 GHz → phone, one UDP sender, 200 B | 5 400 frames/s | 6 970 frames/s |
| 5 GHz → phone, two UDP senders, 1400 B | — | **118 Mbit/s, 10 100 frames/s**, CPU 100 % of two cores |

Link 650 Mbit/s (VHT-MCS 7, NSS 2), 0 retries and 0 failures in all Wi-Fi rows. A single sender is
still limited by one core (the transmit path runs in the sender's context); the gain shows once the
work is spread, which is what bridged traffic from several clients looks like.

The Ethernet row is limited by the far end, which was still running on one core.

## 4. Limits of what was tested

* One unit (3702i), one evening. Uptime with two cores so far: minutes, not days.
* The version in the repository (HID1 + L2 cache in the stage) is the one installed to flash on the
  test unit. Its boot from `flash:`:

  ```text
  Loading "flash:/openwrt-3702i-v3.bin"...
  [smp-shim] L2 cache on, releasing core 1...
  [smp-shim] core 1 is in its spin loop
  [    0.739940] smp: Brought up 1 node, 2 CPUs
  [   24.922433] l2cache: L2CTL=0x90000000 (ENABLED, size field 1), HID1=0x4302b000
  ```

  `/proc/interrupts` then shows `mwlwifi` on MPIC 4 and `mwl8k` on MPIC 0 from the device tree,
  both AP interfaces up, no `failed to set key`. The throughput table above was measured on the
  earlier RAM-booted version of the stage (without HID1/L2 in it, L2 enabled by `l2cache.ko`);
  it was not repeated on the final image.
* Not run on the 2602i yet (same SoC IDs and the same ROMMON register state, so it is expected to
  work; the device tree is prepared).
* 23.05.5: only the address constants are in the builder.
* Interrupt affinity is whatever the kernel chose (all `mwlwifi`/`mwl8k` interrupts landed on
  CPU 1); no tuning of affinity, RPS or the drivers was done.
