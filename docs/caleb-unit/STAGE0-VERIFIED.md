# Stage 0 — SoC access, verified on a second unit

_Recorded 2026-10-24 on an independent Adventurer 4 (HD/H32 revision), firmware
v2.2.4. This is a second-unit confirmation of the `NO_FLASH` path: the MCU was
never flashed and never probed during any step below._

Upstream's `STATUS.md` and `FIRST-RUN-MANUAL.md` disagree about how far the
project got (see "Open contradiction" at the end). Nothing here resolves that
disagreement — it only records what one more unit actually did, with the
evidence for each claim, so the next person can tell verified behaviour from
inference.

## Convention

| Mark | Meaning |
|---|---|
| **VERIFIED** | Observed directly on this unit; the command and its output are given. |
| **INFERRED** | Follows from code or docs but was not observed here. |
| **UNVERIFIED** | Neither observed nor derivable — open question. |

---

## 1. `NO_FLASH` leaves the MCU alone — VERIFIED

Flag file `NO_FLASH` (empty) on the FAT32 stick, no other flag present. Boot
log (`ad4-flash.log`, written to the stick) reported:

```
stick flag default: noflash
panel menu rc=0 token='noflash'
selected image: (none, no MCU flash) (mode=skip, flag=noflash)
no MCU flash this run (SoC access + bridge only)
MCU flash skipped (RC=0, USED=skip)
================ DONE. flag=noflash rc=0 ================
```

The touchscreen menu appears with a **25 s timeout that defaults to `noflash`**;
tapping nothing is the safe action. Matches `flashforge_init.sh` (`FLAG=noflash`
→ `IMG=""; MODE=skip; FLASH_SKIP=1`), which also excludes the MCU probe for this
flag, leaving the chip unprobed rather than disturbing a live MCU.

## 2. Host-side prerequisites that are NOT in the docs

**`python3` is absent from the stock SoC — VERIFIED.** The boot log's tool
census:

```
python3 -> MISSING
python  -> MISSING
pkill   -> MISSING
pgrep   -> MISSING
NOTE: python3 NOT present -> bridge step will be skipped (flash still works)
```

The bridge came up anyway because the **static C binary** `ad4-bridge` was used,
not `ad4-serial-bridge.py`. Any procedure that assumes the Python bridge on a
stock SoC will fail. Ship/keep the C binary.

## 3. Networking — the subnet must match the segment the printer is cabled to

`ad4-net.conf` is sourced by `flashforge_init.sh`, and `SOC_IP`/`SOC_NET` drive
the `ip addr add`. **VERIFIED** — changing `SOC_IP` to an address on the host's
own wired segment worked:

```
configuring eth0 = <SOC_IP>/24
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 ...
    inet <SOC_IP>/24 scope global eth0
```

Reachability from the Klipper host, over a switch rather than a direct cable:

```
3 packets transmitted, 3 received, 0% packet loss
rtt min/avg/max/mdev = 0.146/0.201/0.257/0.045 ms
```

0.2 ms average through a switch. Ports 22 and 5000 both open; ARP resolved to
the SoC's own eth0 MAC, confirming the wired path rather than the WiFi
interface.

**Pitfall worth stating plainly:** the SoC's eth0 and the printer's WiFi are
separate interfaces with separate addresses. Surveying the WiFi subnet for a
free address tells you nothing about the wired segment. Read the *host's* wired
NIC address first and pick `SOC_IP` from that subnet — a wrong guess here costs
a boot cycle and looks exactly like a dead box.

## 4. Services that come up — VERIFIED

```
/mnt/ad4-bridge 5000 /dev/ttyS1 115200
/bin/dropbear -R -p 22
```

`/dev/ttyS1` is the MCU link; `/dev/ttyS0` is the SoC's own console and must not
be used. The bridge presents TCP :5000 ⇄ `/dev/ttyS1` @115200.

**Caveat:** an open bridge proves the *SoC* end of the serial path. It says
nothing about whether the MCU replies — on a stock unit it will not, because
stock firmware does not speak Klipper's protocol. That is expected, not a fault.

## 5. eMMC is written on every stick boot — VERIFIED, and the docs understate it

`GETTING-STARTED.md` claims "no eMMC mutation". The dropbear step appends a
start line to `/opt/auto_run.sh` (the vendor's boot hook) and rewrites the root
shadow entry:

```
dropbear installed -> /opt/services/bin/dropbearmulti + symlinks
root password CHANGED (shadow entry rewritten)
dropbear started (ssh :22 up=1)
dropbear already in /opt/auto_run.sh
```

The pristine file is preserved as `/opt/auto_run.sh.ad4-orig`. This is
recoverable, but it is a persistent change to the printer's filesystem and
should be documented as such rather than as runtime-only.

Set a non-default root password via a `ROOT` file on the stick (first line =
password, read as data, never sourced, logged as a length only). The default is
otherwise a known string on a box with root SSH.

---

## 6. The finding that matters: an ST-Link may not be required

Upstream treats an SWD dump as the gate before flashing, because
`stm32isp-ad4 -d` cannot read the chip (the vendor bootloader has no decoded
read command — the tool emits STM32 ROM-ISP `CMD_READ 0x11`, which this
bootloader does not implement, and then writes a zero-filled buffer). That part
is correct and unchanged.

But two things on a stock unit reduce what the dump is needed *for*:

**(a) The printer already carries the vendor's own firmware and flasher —
VERIFIED.** `/opt/PROGRAM/control/2.3.0/` contains `IAPCommand` (Flashforge's
MCU flasher), `run.sh` (their official re-burn script), both the HD and NA
IAP hex images, and `md5sum.list`. All four files verified against that
manifest with `md5sum -c` on this unit. The boot log confirms the installer
falls back to these when no `factory/` package is on the stick.

**(b) Per-unit calibration lives on the SoC filesystem, not in MCU flash —
VERIFIED.** `/opt/finder_rush/config/config-AD4.json` holds the 9-point bed
mesh (`autoCalibration.point1..point9`, `status: true`), the extruder
calibration value, per-extruder offsets, the printer serial, lifetime print
time and filament totals. Flashing the MCU does not touch any of it.

Taken together: the region a Klipper flash writes is the application at
`0xC000`+ (the flasher refuses records at or below `0x8000`, and the README
states below `0xC000`), and a vendor-authentic image for exactly that region is
already on the printer.

**INFERRED, not tested:** that restoring with `IAPCommand` + the stock hex
returns a flashed unit to working stock. The logic follows, but no one has
executed it. Anyone relying on this should expect to be the first.

**UNVERIFIED:** whether the MCU holds any unit-specific data of its own. This
cannot be settled without reading the chip, which is the circular problem SWD
exists to solve. The risk looks low — the vendor ships one generic hex to every
HD unit, so per-unit data in the app region would be destroyed by their own
firmware updates — but "looks low" is not "measured".

**What SWD is still good for:** dumping the bootloader at `0x0..0xBFFF`, which
no part of this workflow writes, and which is what would let someone decode a
read command and remove the need for SWD entirely.

---

## Open contradiction in upstream docs (unresolved)

- `STATUS.md` (2026-10-20): Klipper on the MCU is flashed but **silent** — 0/20
  IDENTIFY replies at 115200, 0 at 250000, no ROM sync after ~20 reset cycles.
  Status: blocked pending a power-cycle. "MCU fatigue" is labelled an
  unconfirmed hypothesis.
- `FIRST-RUN-MANUAL.md` (2026-10-22): heaters, sensors and the serial chain are
  proven on-unit with passing temperature tests.

`STATUS.md` was never updated after the later document. Both agree that
**motion is unverified** — stepper, endstop, TMC UART and part-cooling-fan pins
are traced guesses, and `FIRST-RUN-MANUAL.md` says to stop before motion tests.
Nothing in this document tests any of that.

## Scope of this record

Stage 0 only: SoC access with the MCU untouched. No flash was written, no
motion was commanded, and nothing here validates any pin assignment.
