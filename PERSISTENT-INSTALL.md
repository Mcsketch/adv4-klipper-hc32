# AD4 — Persistent Klipper Install (eMMC)

Make the SoC side of the hack **survive power cycles**. Status 2026-10-22:
**partially proven** — see the status table before trusting anything.

| Item | Status |
|---|---|
| `/opt/ad4-klipper/` package on eMMC (IAPCommand, ad4-bridge, ad4_identify, hex) | **PROVEN** — survives power cycles, ssh + tools present after plain boot |
| ssh (dropbear) up after a normal power-cycle, no stick | **PROVEN** (root/flashforge) |
| `ad4-bridge :5000` auto-starting at boot | **NOT YET** — start it manually after each power-cycle: `cd /opt/ad4-klipper && ./ad4-bridge 5000 /dev/ttyS1 115200 &` |
| rcS watchdog-gate + `enable` flag installer (below) | **built, not exercised** on this unit |
| USB-stick `flashforge_init.sh` install path | exists; **unproven for the klipper test flow** (the stick was not used for the 2026-10-22 power-cycle tests) |
| `RESTORE` stick (factory re-flash + unwind) | built, **not tested** |

## Why a plain boot isn't enough (design)

The SoC boots BusyBox init → `rcS`, which launches the vendor watchdog
`/opt/PROGRAM/ffstartup-arm`, which keeps `adventurer4-arm` alive — and that
app **opens `/dev/ttyS1` @115200**, contending with our bridge. Verified from
the firmware archive: no eMMC integrity check, no cloud self-heal — nothing
*deletes* our files, but the vendor stack *outruns* a one-shot install.

The designed fix is a flag-gated one-line rcS change:

```sh
if [ -f /opt/ad4-klipper/enable ]; then /opt/ad4-klipper/ad4-daemon.sh & else /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg & fi
```

`enable` present → our daemon (static IP + bridge, self-heal loop). Absent →
stock, exactly as factory. Original rcS line kept at `<rcS>.ad4-orig`.
Note: the runtime bridge in use today is the C `ad4-bridge` (not the older
`ad4-serial-bridge.py` the installer package references).

## Safety model (unchanged, why this can't brick the SoC)

- Single flag-gated `if/else` on one rcS line; no vendor file deleted;
  installer aborts cleanly if the backup or the line match fails.
- MCU flash keeps the anti-brick guard (refuses images based ≤ `0x00008000`,
  the ROM-ISP region).
- Bridge/daemon are user-space; a crash is a no-op.

## Revert to stock

- Stick: `touch RESTORE` on the FAT32 stick, plug in, power on (flashes
  factory HD image + unwinds the hook). **Not yet tested on this unit.**
- Shell: `rm /opt/ad4-klipper/enable; sync; reboot` → vendor UI back (MCU
  keeps Klipper; vendor app just can't talk to it).
- MCU factory image: archived under `FlashForge-Hacks-main/Firmware/`.

## Verify (when you wire the auto-start)

1. Power-cycle, no stick: `ping 192.168.10.10`, `nc -zv 192.168.10.10 5000`.
2. `ad4_identify 115200 /dev/ttyS1` on the SoC answers with a blob.
3. Revert test on a spare stick: `RESTORE` → vendor UI back, stock app holds
   ttyS1.
