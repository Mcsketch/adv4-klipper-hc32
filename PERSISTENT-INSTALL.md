# AD4 — Persistent Klipper Install (eMMC)

Make the hack **survive power cycles**. Without this, the Klipper runtime
works only while the USB stick is present; on the next power cycle the vendor
watchdog (`ffstartup-arm`) relaunches the stock app, which grabs `/dev/ttyS1`
back and the SoC IP reverts. This document covers the durable version.

Status: **built and wired into the USB stick** (default). Live-verify on the
real SoC before trusting it (see "Verify" below).

---

## Why the one-shot doesn't survive a reboot

The SoC boots via BusyBox init → `/etc/inittab` (`::sysinit:/etc/init.d/rcS`)
→ an rcS startup script that, in order:

1. polls the USB stick, mounts it, and runs `/mnt/flashforge_init.sh` (our
   trigger — this is how the one-shot works);
2. launches the vendor process-watchdog:
   **`/opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg &`**;
3. launches `/opt/services/autorun.sh &`.

`ffstartup-arm` is a **process watchdog** (not a file restorer): it keeps
`adventurer4-arm` alive and, when it starts, that app **opens `/dev/ttyS1` at
115200** — i.e. it **contends with our `:5000` bridge** for the same serial
port. There is **no eMMC/rootfs integrity check** in the image (verified: every
"integrity/md5" string is standard lib help text, not a vendor factory-restore),
and **no cloud firmware self-heal** (`cloud.sz3dp.com` is a print-job cloud,
off by default). So nothing *deletes* our files — but the watchdog *outruns*
our one-shot on every fresh boot.

The fix: at boot, run our runtime **instead of** the vendor watchdog, from the
eMMC, gated behind a flag file.

---

## What "persistent" installs

A small package at **`/opt/ad4-klipper/`** on the eMMC:

| file | role |
|---|---|
| `ad4-daemon.sh` | the runtime: static IP on `eth0`, starts+keeps the `:5000` MCU bridge up (self-heals every 10 s), optional MCU re-flash watchdog |
| `ad4-serial-bridge.py` | SoC `/dev/ttyS1` ⇄ TCP `:5000` (Klipper talks to this) |
| `klipper.hex` | positioned Klipper @ `0x0000C000` (used only by the re-flash watchdog, off by default) |
| `IAPCommand` | factory ARM flasher (for the re-flash watchdog) |
| `ad4-net.conf` | `SOC_IP=192.168.10.10  SOC_NET=24  BRIDGE_PORT=5000` |
| `enable` | **the flag**: while present, the boot hook runs our daemon instead of the vendor watchdog |
| `mcu-klipper.ok` | marker: MCU already flashed (so the daemon doesn't re-flash at boot) |
| `mcu-watchdog` | *(optional)* if present, enables periodic MCU health-probe + auto re-flash |
| `/var/log/ad4-klipper.log` | runtime log |

**The boot hook** (the only change to vendor files): the rcS line that launches
`ffstartup-arm` becomes a one-line `if/else`:

```sh
if [ -f /opt/ad4-klipper/enable ]; then /opt/ad4-klipper/ad4-daemon.sh & else /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg & fi
```

So:
- **`enable` present → Klipper mode:** vendor watchdog/app never run, so nothing
  respawns the stock app and nothing grabs `/dev/ttyS1`. Our daemon owns the
  network + bridge.
- **`enable` absent → stock mode:** the vendor line runs **exactly as factory**.

The original rcS line is preserved verbatim at `<rcS>.ad4-orig`, so revert is a
clean copy-back.

---

## How to use it

The USB stick builder drops a `PERSISTENT` flag **by default**, so a normal
build is the durable install:

```sh
cd adv4-klip/flashdisk
./build-usb-stick.sh /Volumes/MYSTICK          # persistent (durable) stick
NO_PERSISTENT=1 ./build-usb-stick.sh /Volumes/STICK   # one-shot stick (no eMMC change)
```

First run is the same as before — **plug the stick in + power the printer.**
The SoC's boot path runs `flashforge_init.sh`, which (with `PERSISTENT`):

1. stops the vendor stack,
2. flashes Klipper to the MCU (anti-brick base guard intact),
3. copies the `/opt/ad4-klipper/` package to the eMMC,
4. gates the rcS watchdog line (backed up first),
5. creates the `enable` flag,
6. **reboots the SoC** — boot #2 is fully autonomous: vendor stack off,
   `eth0=192.168.10.10`, bridge on `:5000`, no stick needed.

Then on the RPi (192.168.10.1): `./soc-hack/ad4-rpi-setup.sh` once, and
Fluidd points at `serial:/dev/adv4_mcu`.

**From then on** the stick can stay unplugged. Power cycles, reboots, even a
soft reset keep Klipper running. The RPi is still the host (klippy, Fluidd);
the SoC just bridges MCU ⇄ network and holds the static IP.

---

## Revert to stock

**From a stick** (easiest, no shell needed): put an empty `RESTORE` file on the
stick, plug in, power on. It flashes the **factory** MCU image **and** unwinds
the persistent hook (restores the original rcS line, removes `enable`), then
reboots to clean FlashForge.

```sh
cd /Volumes/MYSTICK
touch RESTORE        # (and it flashes factory-HD.hex; no PERSISTENT needed)
```

**From the SoC shell** (root, if you have SSH):

```sh
sh /opt/ad4-klip-pkg/ad4-uninstall-persistent.sh
# → disables, restores rcS, stops the runtime, reboots to stock
```

**Quick toggle back to stock without re-flashing the MCU** (keep Klipper on the
MCU, just run the vendor UI for a while):

```sh
rm /opt/ad4-klipper/enable; sync; reboot
```

The MCU still has Klipper; the vendor app will start but will fail to talk to a
Klipper MCU. Re-`enable` + reboot when you want Klipper again. (Use `RESTORE`
to actually bring the vendor MCU firmware back.)

---

## The MCU re-flash watchdog (optional, off by default)

By default the daemon trusts the MCU is already Klipper (`mcu-klipper.ok`) and
does not re-flash. If you want the SoC to **self-heal the MCU too** (e.g. you
accidentally restore the MCU firmware, or the flash corrupts), enable the
watchdog:

```sh
touch /opt/ad4-klipper/mcu-watchdog
```

The daemon then probes the bridge every 60 s; after 3 consecutive dead probes it
runs `IAPCommand klipper.hex` to re-flash, then restarts the bridge. Only enable
this once you've confirmed the flash path is reliable on your unit — it is the
one part that writes to the MCU at runtime.

---

## Safety model (why this can't brick the SoC)

- **No eMMC data is deleted and the vendor app is never removed.** The change is
  a single flag-gated `if/else` on one rcS line. If the patch ever fails, or
  `enable` is absent, the SoC boots **exactly as stock**.
- **The rcS patch is literal and reversible**: the installer backs up the
  original line first and only proceeds if the backup succeeded; on any failure
  it restores the backup and aborts (no reboot, no partial patch).
- **The MCU flash keeps its anti-brick guard** (refuses any image whose lowest
  programmed address is ≤ `0x00008000`, the ROM-ISP region) and **never writes
  `factory-HD.hex` over Klipper** unless `RESTORE` is explicitly requested.
- **The `:5000` bridge and the daemon are user-space processes** — a crash is a
  no-op for the SoC (the watchdog loop restarts them), never a brick.

The only state that's "ours" is `/opt/ad4-klipper/` plus the one rcS line (with
its `.ad4-orig` backup). Removing both returns the SoC to a pristine boot.

---

## Verify on the real SoC (live checks)

1. **After the first (stick) boot + reboot, with the stick unplugged**, from the
   RPi: `ping 192.168.10.10` → should answer. `nc -zv 192.168.10.10 5000` →
   should open (that's the bridge).
2. **Confirm the vendor stack is off:** the SoC should *not* be grabbing
   `/dev/ttyS1` for the stock app (no `adventurer4-arm` holding the port).
3. **Reboot the SoC** (power cycle). Re-check #1. If it still answers with the
   bridge up, persistence is confirmed.
4. **Revert test (optional, on a spare/known-good stick):** `touch RESTORE`,
   reboot, confirm the vendor UI is back and the stock app holds `/dev/ttyS1`.

If the rcS file on your unit differs in name or the vendor launch line is
worded differently, the installer **aborts cleanly** (it will say
`could not locate the ffstartup launch line` in `/root/ad4-flash-status`) and
nothing is patched — adjust the `RCS` candidate list in
`ad4-install-persistent.sh` to match.
