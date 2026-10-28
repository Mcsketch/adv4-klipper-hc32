# Klipper-mode persistent boot on the NA unit (2026-10-27; stickless eMMC boot since 2026-10-28)

> **Current state:** see the last section, "Update 2026-10-28 (2)". The
> sections before it are the history, kept for the failures they record.

Status at pause (22:45 MDT): the MCU runs Klipper 020d25e. The host stack on
prodesk is installed and running. The SoC's cold boot into `ad4-daemon.sh`
FAILED to bring up the network. The printer is currently reachable only
through the rescue stick (below).

## What is installed and working

**Host (prodesk, 192.168.1.10):**
- `~/klipper` branch `ad4-socket` (commit 802101f, `serialhdl` accepts
  `socket://`), venv `~/klippy-env`.
- `~/moonraker` + `~/moonraker-env`, Mainsail in `/opt/mainsail`.
- `~/printer_data/config/{printer.cfg,mainsail.cfg,moonraker.conf}`;
  `[mcu] serial: socket://192.168.1.219:5000`.
- systemd `klipper` + `moonraker` enabled (homeserver commit 9be89ec).
  Mainsail at http://192.168.1.10:8095 via Caddy.
- klippy retries the socket until the SoC bridge is up. Not yet verified
  against the MCU (no temps / endstops / heater test done).

**SoC `/opt/ad4-klipper/`:** installed via `ad4-install-persistent.sh`
(install-status `installed rc=0`). `/opt/auto_run.sh` line 101 is the gated
launch (`enable` flag -> daemon, else vendor UI). Backups:
`/opt/auto_run.sh.ad4-orig` (pre-patch, with dropbear),
`/opt/auto_run.sh.factory` (original). `enable` is currently REMOVED.

## What failed

1. Reboot with `enable` set (~22:05): SoC never appeared on the LAN for 18 min.
   No ARP for wlan0 or eth0. The 360 s no-IP fallback did not visibly fire.
   The daemon log is on tmpfs (`/var/log` -> `/tmp`) and was lost.
2. Stickless boot with `enable` removed: the vendor UI hangs at its splash
   and brings up no WiFi. **The vendor UI is not a fallback while Klipper is
   on the MCU.** The daemon's "reboot to the vendor UI" fallback is therefore
   useless and must be replaced.

Root cause of (1) is NOT known. The same commands work when run by hand
or from the stick (rescue log: associated in 4 s, DHCP .219 at 16 s uptime).
Differences to check: daemon runs later in rcS and backgrounded (`&`);
output redirected into the tmpfs log; no `mkdir -p /var/run/wpa_supplicant`;
`kill_vendor`/`pid_alive` scanning /proc during boot.

## The vendor USB hook (`/opt/auto_run.sh` lines 11-76)

Mounts `/dev/sda1` (vfat) on `/mnt` at ~7 s uptime and runs
`/mnt/flashforge_init.sh Adventurer4 0012`. **If it exits 0, auto_run.sh
runs `sleep 100000`**, i.e. the boot parks there and the gated
daemon/UI line and line-102 dropbear never run. A non-zero exit continues
the normal boot.

## Rescue stick (works, in the printer now)

FlashForge stick (vfat, label FLASHFORGE). `flashforge_init.sh` = rescue v2
(copy in `work/n32/rescue-flashforge_init.sh`). It removes `enable`, starts
dropbear, eth0 udhcpc, `wpa_supplicant -Dnl80211` + `enable_network all` +
wlan0 udhcpc, logs to `/tmp/rescue.log` and `/mnt/rescue.log`, exits 0 (parks).
It never touches ttyS1, NRST or the IAP. Result: SSH on 192.168.1.219 ~40 s
after power-on. The repo stick script is kept as `flashforge_init.sh.repo`;
`NO_FLASH` is still there.

## Next session (exact order)

1. Printer on with the rescue stick: SSH `root@192.168.1.219`
   (prodesk: `sshpass -f ~/.ad4-root`).
2. Quick win to print: from the rescue shell start the bridge
   (`/opt/ad4-klipper/ad4-bridge 5000 /dev/ttyS1 115200 &`), then on prodesk
   `sudo systemctl restart klipper` and verify temps, `QUERY_ENDSTOPS`,
   a short heater test. Optionally make the rescue script start the bridge,
   which is a working "Klipper mode" on its own.
3. Fix the daemon cold path: persistent trace log in `$BASE/boot.log`
   (`set -x`), `mkdir -p /var/run/wpa_supplicant`, wait for wlan0, and
   replace the vendor-UI fallback (no longer valid) with a retry of the
   WiFi bring-up. Optionally run it via `setsid` from the gated line.
4. Test loop: `touch /opt/ad4-klipper/enable`, pull the stick, power cycle.
   If no SSH within ~3 min: stick in, power cycle, read `boot.log`.

## Update 2026-10-28: printing works via the boot stick + wired link

- **Boot path in use:** the stick's `flashforge_init.sh` is now the Klipper
  boot script (`work/n32/klipper-boot-flashforge_init.sh`, md5 c56f7529...).
  It sets eth0 192.168.100.20/24, WiFi + SSH, `stty` ttyS1 115200 8N1 raw,
  starts `ad4-bridge` on :5000, then blocks forever. Bridge is up ~13 s after
  power-on. The rescue v2 script stays on the stick as `flashforge_init.sh.rescue2`.
- **Why it blocks forever:** after an exit 0, auto_run.sh runs `sleep 100000`
  (27.7 h), then exports GPIO34 (MCU NRST) as an output and starts the vendor UI.
  That would reset the MCU mid-print.
- **Wired, not WiFi:** over WiFi (both ends on WiFi, 8188fu `rtw_power_mgnt=2`)
  ping was 6-113 ms and Klipper's clock sync drifted until the MCU shut down
  with "Timer too close" during a heater test. The printer's eth0 is on the lab
  switch (192.168.100.0/24, no DHCP). Wired srtt is 2 ms with zero variance.
  printer.cfg now uses `serial: socket://192.168.100.20:5000`
  (backup `printer.cfg.bak-20261028`).
- **Bridge baud bug:** `bridge/ad4-bridge.c` passes the plain number 115200 to
  `cfsetispeed`/`cfsetospeed`, which expect `B115200`. The call fails silently
  and the port keeps its old speed (9600 after a cold boot). It only worked
  before because the vendor UI had already set 115200. The boot script works
  around it with `stty`; fix the C (map to `B115200`) and rebuild.
- **`reboot` is ignored** while rcS is blocked in the hook. Use
  `sync; reboot -f`, or power-cycle.
- **Verified 2026-10-28:** identify, temperatures 19 °C ambient, QUERY_ENDSTOPS
  all open, hotend held 45 ±0.3 °C and bed 40 ±0.5 °C for 3 min, and survived
  a forced SoC reboot.
- **Not verified yet** (fans since done, see below): fans, endstop triggering, homing and motion directions,
  Z endstop position (151.3 is the reference unit's value), extruder rotation
  distance. Do these with someone at the printer before the first print.
- **After every printer power cycle,** Klipper on the host shows an error.
  Press FIRMWARE_RESTART in Mainsail. (Superseded: the panel now does this
  automatically, see below.)
- The eMMC daemon path (`enable` flag) is still broken and unused; `enable`
  is removed on every stick boot.

## Update 2026-10-28 (2): stickless boot from eMMC, panel, fans

**Boot path (no USB stick):**
- `work/n32/ad4-klipper-boot.sh` is installed as `/opt/ad4-klipper/klipper-boot.sh`.
  `/opt/auto_run.sh` line 101 now reads
  `if [ -f /opt/ad4-klipper/klipper-mode ]; then /opt/ad4-klipper/klipper-boot.sh & elif [ -f /opt/ad4-klipper/enable ]; then ...daemon & else ffstartup & fi`.
  Backup: `/opt/auto_run.sh.pre-klipper-mode`.
  Undo: `rm /opt/ad4-klipper/klipper-mode; sync; reboot -f` (vendor UI returns,
  but it cannot drive an MCU that runs Klipper).
- Order: panel splash -> eth0 192.168.100.20/24 -> `stty` ttyS1 115200 raw ->
  `setsid ad4-bridge 5000` -> `setsid` respawn loop for
  `ad4-screen klipper 192.168.100.10:8095` -> wait for wlan0 -> wpa_supplicant
  -> **wait for `wpa_state=COMPLETED`** -> `udhcpc -b`. Log: `/tmp/ad4-boot.log`.
- The script is backgrounded, so rcS is not parked: dropbear (line 102)
  starts and `reboot` works again.
- The stick's script is renamed `flashforge_init.sh.klipper-boot-v3` (inert).
  Keep the stick as a rescue path: rename a script back to `flashforge_init.sh`.

**What failed on the way:**
- udhcpc 0.9.8 started before WiFi associates never gets an offer, and it has
  no `-t`/`-T` to retry. wlan0 came up with no IP. Fix: poll `wpa_cli status`
  for COMPLETED first (up to 60 s), then start udhcpc.
- `ad4-screen klipper` segfaulted in `memcpy`: the back buffer was copied with
  `stride * max(h, fh)` bytes, which overruns the 800x480 fb mmap when the
  panel is rotated. Fix: `stride * (rot ? fh : h)`.
- Moonraker listens on loopback only; the panel reaches it through Caddy on
  :8095 instead.

**Verified 2026-10-28:** a forced SoC reboot and a real cold power cycle with
no stick. Up in ~22 s: eth0 .20, WiFi .219 (DHCP-reserved), bridge, panel.
The panel sees "Lost communication" / "Unable to open" / "Unable to connect"
and POSTs `/printer/firmware_restart` (at most once per 20 s); Klipper returned
to ready unattended. Heater and MCU faults are not auto-restarted.

**Panel (`ad4-screen klipper HOST:PORT`):** state chip, nozzle/bed temps, job
file/progress/time, WiFi IP footer. Buttons: PAUSE/RESUME, CANCEL (confirm
tap), PREHEAT (M104 S210, M140 S60), COOLDOWN, FIRMWARE RESTART. Touch taps
are not yet tested by a person.

**Fans (this unit, same FFP0187 PCB as the reference unit):**
- `M106 S255` with `[fan] pin: PB9` spun the **chamber** fan. PB9 = chamber
  fan, which settles the "chamber vs part: GUESS" row in `adv4-exploration.md`.
  printer.cfg now has `[fan_generic chamber_fan]` on PB9.
- PC2 (`heater_fan`) came on at ~57 °C nozzle and drives the **heatsink** fan
  (confirmed by eye).
- **The part-cooling fan pin is still unknown.** With no `[fan]`, slicer
  M106/M107 print "Unknown command" and are skipped, so prints have no part
  cooling. Next: find it in the M106 handler of the stock NA image
  (`Adventurer4-NA-IAP-2.3.0-20230216`, offline). Do not probe with SET_PIN.

**MCU link stays wired (re-measured 2026-10-28):** prodesk -> printer over
WiFi pinged 4.8/8.6/44 ms (mdev 6.4); wired 0.14/0.17/0.27 ms. Klipper keeps
the G-code on the host and streams moves to the MCU ~0.1-0.25 s ahead, so a
WiFi stall stops the print; nothing is stored on the printer. The 8188fu
driver loads with `rtw_power_mgnt=2` (iwconfig still reports "Power
Management:off"). Decision: eth0 stays on the lab switch. Later option: a
dedicated Pi as the Klipper host, wired to the AD4.

**ST-Link is not needed in normal use.** Flashing goes through the stock IAP
over ttyS1 (`IAPCommand`). SWD cannot attach while Klipper idles in `wfi`,
and connect-under-reset does not work on the N32's debug port. Take it off
J16 before closing the case; keep it for bootloader recovery only.

**Slicer (Elegoo Slicer, OrcaSlicer family):** physical printer host type
OctoPrint, host `192.168.1.10:8095` (Moonraker's OctoPrint-compatible upload),
G-code flavor Klipper, bed 220x200. Start G-code: M140/M104 (slicer temps),
M107, G90, M82, G28, M190, M109, G92 E0. End: G91, G1 E-3, G1 Z5, G90, M104 S0,
M140 S0, M107, M84. The start G-code homes, so do not print until homing is
verified.

**Working config:** `work/n32/printer-n32.cfg` (copy of the live
`~/printer_data/config/printer.cfg`).

## Next (in order)

With someone at the printer:
1. `QUERY_ENDSTOPS` while pressing each switch by hand (X, Y, Z).
2. `G28 X`, `G28 Y`, `G28 Z` one at a time, hand on the power switch; flip
   `dir_pin` if an axis moves away from its switch.
3. `Z_ENDSTOP_CALIBRATE` (151.3 is the reference unit's value), `SAVE_CONFIG`.
4. `PID_CALIBRATE HEATER=extruder TARGET=210`, then `HEATER=heater_bed TARGET=60`.
5. Extrude 100 mm and measure to check `rotation_distance` 33.5.
6. First print.

Offline: part-fan pin from firmware RE; fix `bridge/ad4-bridge.c` baud
(`B115200`); have a person test the panel's touch buttons.
