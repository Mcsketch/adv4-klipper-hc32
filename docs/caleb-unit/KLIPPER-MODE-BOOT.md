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

## Superseded next list (2026-10-28)

See the 2026-11-02 update below.

## Update 2026-11-02: endstops, step scaling, homing (NA unit)

### Endstop pins from the stock NA firmware (no case opening)
The stock app (`work/Adventurer4-NA-*`, base 0x08010000) inits its axis GPIO in
fn 0x8018fb4 from a 5 x 40-byte table in RAM at 0x20000540 (step, dir, enable,
endstop port/pin pairs). RAM is filled by the Keil scatter-load table at
0x801acb0 (`__decompress` LZ, src 0x801acd0 -> 0x20000000, 0x648 bytes).
`work/n32/re/axes.py` decompresses it and prints:

| Axis | Step | Dir | Enable | Endstop |
|---|---|---|---|---|
| 0 X | PA8 | PC9 | PA11 | PC3 |
| 1 Y | PB14 | PB13 | PB15 | PC1 |
| 2 Z | PA15 | PA12 | PC11 | PB5 |
| 3 E | PC7 | PC6 | PC8 | - |

Same pins as the reference config, so pins were never the problem.

### What failed
- Hand-pushing the bed onto the Y switch: only 0.6-1 s blips, never a steady
  trip. Driving it with the motor (`FORCE_MOVE` 2 mm steps + `QUERY_ENDSTOPS`
  after each) trips it every time. Use the motor, not a hand.
- Z at `VELOCITY=5` under the old scaling (really 10 mm/s) only grunted: the
  lead screw stalls lifting the head that fast. `max_z_velocity` is now 5.
- The reference `position_endstop: 151.3` / `homing_positive_dir: false` is
  wrong for this unit: nothing trips between 3 mm and ~253 mm above the bed.

### What worked: measured step scaling
The NA board's drivers are not at 1/32 (X/Y) and 1/16 (Z). Commanded a known
`FORCE_MOVE`, measured with a ruler:

| Axis | Commanded | Moved | Now |
|---|---|---|---|
| Y | 20 mm | ~145 mm | `microsteps: 4`, `rotation_distance: 30` (verified 136 mm back to the switch) |
| X | 30 mm | ~120 mm | `microsteps: 8`, `rotation_distance: 33.9` (verified 120 mm back to the switch) |
| Z | 4 mm x3 | ~8 mm each | `microsteps: 8`, `rotation_distance: 6.25`, `dir_pin: PA12` (+Z was down) |

Z 140 mm commanded then measured ~141 mm, confirming Z.

### Where the switches are
- X: right end (`position_endstop: 220`, max).
- Y: bed at the back (`position_endstop: 4`, min).
- Z: **top**, ~255 mm above the bed. `homing_positive_dir: true`,
  `position_endstop: 250` (deliberately low until the paper test),
  `homing_speed: 5`, `second_homing_speed: 2`.

`homing_override` now homes only the axes named (`G28 Z` homes Z alone; bare
`G28` still does Z, X, Y). `G28 Z`, `G28 X`, `G28 Y` all home cleanly.
Live config: `work/n32/printer-n32.cfg`.

## Update 2026-11-02 (2): TMC2209 over UART, Z calibrated

Stock NA fw configures the drivers at boot (fn 0x801263c): UART4 TX on **PC10**
@9600, mux select **PB1/PB10**, PB0 and PB11 low during config. Registers per
channel: GCONF, IHOLD_IRUN, TPOWERDOWN, CHOPCONF (mres 1/16), PWMCONF; StealthChop.
Klipper `[tmc2209]` with `uart_pin: PC10` works (single-wire reads OK). Channel
map proven by enabling one motor at a time and reading IOIN.enn:

| (PB1,PB10) | Motor | Stock run / hold |
|---|---|---|
| (1,1) | X | 0.55 / 0.22 A |
| (0,0) | Y | 0.66 / 0.22 A |
| (1,0) | E | 1.38 / 0.72 A |
| (0,1) | none of X/Y/Z/E enables it | 0.55 / 0.22 A |

**Z is not on the UART bus**: no channel follows PC11. Its driver stays at the
1/8 default. Klipper now runs X/Y/E at 1/16 SpreadCycle (X rd 33.9, Y rd 60),
X/Y verified 50 mm commanded = 50 mm. Z at 3 mm/s is smooth both ways (whir is
normal); the earlier stutter was at 5 mm/s and likely lost steps (Z10 read
28 mm above the bed before, 15 mm after a clean re-home). Cause not proven.

Paper test at X110 Y100: Z `position_endstop: 254.7`, `position_max: 254.7`,
`position_min: -2`. Re-homed and re-checked at Z0: correct drag.

PID (2026-11-02): extruder 210 C Kp 16.720 Ki 6.192 Kd 11.286; bed 60 C Kp 68.060
Ki 7.960 Kd 145.478 (SAVE_CONFIG). Extruder direction confirmed (`dir_pin: !PC6`).

Filament load (long tube from the feeder to the head): **~690 mm commanded**
(E rd 33.5 at 1/16) from gears-catch to new filament at the nozzle. Unload from
hot: `G1 E-10 F600`, `G1 E-90 F300` frees a cut stub. Commanded length is in
current E units; if the 100 mm extrude check changes rd, scale it.

## Update 2026-11-03: first print

PID done (values in the live `printer.cfg` SAVE_CONFIG block). Filament load path
from the gears catching to the nozzle is ~690 mm of the long tube.

Macros (in `work/n32/printer-n32.cfg`): `PRINT_START BED= EXTRUDER=` (preheat to
150, home, bed wait, nozzle wait, 10 mm purge line at Z0.3 from X20 to X120 at
Y10), `PRINT_END` (retract 2, lift 10, Y200 = bed forward, heaters off, M84),
and empty `M106`/`M107` because the part-fan pin is unknown.

Slicer: Elegoo preset "AD4 0.4 Nozzle (Caleb)" is Klipper flavor, origin 0,0
(220x200), uploads to Mainsail 192.168.1.10:8095 (octoprint host type). Start
gcode `PRINT_START BED=[bed_temperature_initial_layer_single]
EXTRUDER=[nozzle_temperature_initial_layer]`, end `PRINT_END`. The SUNLU PLA
filament preset has max volumetric speed 3 (see the slip flag below).

First print: 20 mm cube, 0.2 mm layers, 220/55 C, peak 3.0 mm3/s, ~23 min.
Result (Caleb): good. No wall gaps, solid top, a few small jitters on the walls
in a couple of places; a faint dark line at ~80% height (marker ink from the length marks; Caleb judged it fine after a 240 C 100 mm flush). The part fan ran (uncontrolled: pin unknown, M106/M107 no-ops).

## Update 2026-11-03 (2): part fan = PA6

Found offline in the stock NA image (`ad4-re/na.dis`): the MCU's own G-code
dispatcher (fn 0x8017b90) handles M106 (0x6a) by storing S, clamped to 255, at
RAM 0x20000454, and M107 (0x6b) by zeroing it. The 1 ms tick (fn 0x8012da4)
software-PWMs **PA6** from that byte (period 256). PA6 is initialised as a
push-pull output, low, next to PC2 (heatsink fan) in fn 0x8011514.
`printer.cfg` now has `[fan] pin: PA6`; the empty M106/M107 macros are gone.
Spin test with Caleb watching: M106 S255 / S128 / M107 -> the part blower
followed all three. Unconfigured, PA6 floats and the blower ran uncontrolled
during the first cube.

Same tick: **PA4** is a second software PWM, 0-100 %, set by another handler in
fn 0x8017684 and driven high at boot next to the PB9 chamber-fan PWM init.
Possibly the light; untested.

The extruder slip item was closed by Caleb (2026-11-03); slicer max volumetric
speed stays at 3 mm3/s.

## Update 2026-11-03 (3): cooling and the Rapid PLA+ preset

The filament is Elegoo Rapid PLA+. Cube 2 (SUNLU preset: 220 C, flow 0.96,
fan 100 % now that M106 drives PA6) had wall gaps from the bottom up. Fan cut
to 30 % at Z 13 improved the walls at once. The hotend PWM barely moved
(0.31 at fan 100 % vs 0.28 at 30 %), so the heater kept up; the gaps are
cooling/melt, not heater power.

New Elegoo filament preset **"AD4 Rapid PLA+"**: 225 C, flow 1.0, fan 40 %,
max volumetric 3 mm3/s, retraction 5.5 mm, bed 55 C.

### Ladder test (`ladder_RapidPLA.gcode`)
Four 20x20x15 mm blocks printed one after another (bumps on top = piece):

| Piece | Change | Caleb's verdict |
|---|---|---|
| A (1) | preset as is: 225 C, fan 40 %, PA 0.05, z-hop 0.4 | best, good enough |
| B (2) | A at 235 C | close behind A |
| C (3) | B, z-hop off, Z moves at 3 mm/s | very weak: thin walls, choppy lines |
| D (4) | C, pressure advance 0.5 | very weak: thin walls, choppy lines |

Result: keep the preset (piece A). Wall tuning closed by Caleb.
Not isolated: C and D printed last, after ~40 min at 235 C, so print order
and heat creep are confounded with the z-hop/PA changes. Thin walls are
under-extrusion, which z-hop alone should not cause. If thin walls come back
on long prints, suspect heat creep or feed (spool drag, ground filament)
before settings.

## Next (in order)

1. Have a person test the panel's touch buttons.
2. Fix `bridge/ad4-bridge.c` baud (`B115200`).
3. Light (PA4?) and camera: separate tracker entry.
