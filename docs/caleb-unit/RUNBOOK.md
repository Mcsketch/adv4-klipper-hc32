# Caleb's AD4 on Klipper: runbook (standing instructions)

State as of 2026-11-03. This is the doc to read first. It describes the install
as it runs now; `KLIPPER-MODE-BOOT.md` is the dated history behind it. When
something here changes, edit this file in place. Do not append corrections.

## 1. What is running

| Part | Where | What |
|---|---|---|
| MCU | NA board, **N32G455** (not the HD/HC32F460 the repo README describes) | mainline Klipper `020d25e`, flashed 2026-10-27 through the vendor IAP |
| SoC | Allwinner V3S in the printer, eth0 `192.168.100.20` (lab switch), WiFi `192.168.1.219` | boots `/opt/ad4-klipper/klipper-boot.sh`: `ad4-bridge` TCP :5000 to `/dev/ttyS1` @115200, `ad4-screen klipper` panel |
| Host | prodesk (`192.168.1.10`, lab side `192.168.100.10`) | `~/klipper` branch `ad4-socket` (`802101f`, accepts `socket://`), Moonraker on loopback :7125, Mainsail `http://192.168.1.10:8095` via Caddy |
| Config | `prodesk:~/printer_data/config/printer.cfg` | repo copy `work/n32/printer-n32.cfg`; keep them identical (copy after every change, commit) |
| Slicer | Elegoo Slicer on the ASRock | machine "AD4 0.4 Nozzle (Caleb)", process "AD4 PLA Standard", filament "AD4 Rapid PLA+" |

The MCU link is **wired on purpose**. Over WiFi, Klipper's clock sync drifted
until "Timer too close". Do not move it to WiFi.

## 2. Access

- Host: `ssh prodesk`. Klipper/Moonraker are systemd units `klipper` and
  `moonraker`. G-code from a shell:
  `curl -s -X POST "localhost:7125/printer/gcode/script?script=<urlencoded>"`.
  Status: `curl -s "localhost:7125/printer/objects/query?print_stats&extruder&heater_bed&toolhead"`.
- SoC (from prodesk): `sshpass -f ~/.ad4-root ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@192.168.100.20`.
  Dropbear makes a new host key every boot, hence the options.
  The password file is mode 600. Never print it.
- Emergency stop: `curl -s -X POST localhost:7125/printer/emergency_stop`.

## 3. Power-on to ready

1. Power on. The SoC is up in ~22 s: eth0, bridge, panel, then WiFi.
2. Klipper on the host loses the MCU on every printer power cycle. The panel
   POSTs `firmware_restart` by itself (at most once per 20 s), so Klipper
   returns to `ready` unattended. If not: FIRMWARE_RESTART in Mainsail.
3. Heater or MCU faults are not auto-restarted. Read `klippy.log` first.

Boot switch: `/opt/auto_run.sh` line 101 runs `klipper-boot.sh` when
`/opt/ad4-klipper/klipper-mode` exists. Undo: `rm` that flag,
`sync; reboot -f` (the vendor UI returns but cannot drive an MCU running
Klipper). Boot log: `/tmp/ad4-boot.log` (tmpfs, lost on reboot).

## 4. Printing

1. Slice in Elegoo Slicer with the presets above. Upload via the physical
   printer (OctoPrint host type, `192.168.1.10:8095`), or scp to
   `prodesk:~/printer_data/gcodes/`.
2. Start G-code is `PRINT_START BED=.. EXTRUDER=..`: preheat nozzle to 150,
   home, bed wait, nozzle wait, purge line at Y10 Z0.3 from X20 to X120.
   End is `PRINT_END`: retract 2, lift 10, bed forward (Y200), heaters off.
3. Start a print: `curl -s -X POST "localhost:7125/printer/print/start?filename=<file>"`.
4. Live overrides (`M221`, `M220`, `SET_PRESSURE_ADVANCE`, `M106`) persist
   after the print. Reset them before the next one (`M221 S100`, `M220 S100`).

**Filament:** Elegoo Rapid PLA+. Preset "AD4 Rapid PLA+": 225 C, flow 1.0,
fan 40 %, bed 55 C, max volumetric 3 mm3/s, retraction 5.5 mm, z-hop 0.4.
This is proven best (section 7). Load: ~690 mm commanded from the gears
catching to filament at the nozzle (long tube). Unload hot:
`G1 E-10 F600`, then `G1 E-90 F300`.

## 5. Calibrated values (all in printer.cfg)

| Item | Value | How it was proven |
|---|---|---|
| X | 1/16, rd 33.9, endstop max 220 | ruler, 50 mm = 50 mm |
| Y | 1/16, rd 60, endstop min 4 | ruler |
| Z | 1/8 (not on the UART bus), rd 6.25, endstop **top** 254.7, `max_z_velocity` 5 | paper test at X110 Y100 |
| E | 1/16, rd 33.5, 1.38 A | 100 mm extrude check |
| TMC2209 | UART PC10, mux PB0/PB1/PB10/PB11 (X, Y, E only) | IOIN reads |
| PID | extruder 210 C, bed 60 C | SAVE_CONFIG block |
| Hotend / bed NTC | PA0 / PA5, fitted curves `ad4_hotend` / `ad4_bed` | stock fw LUTs |
| Heaters | PA1 nozzle, PA7 bed | stock fw |
| Part fan | **PA6** `[fan]` | stock fw M106 handler, spin test |
| Chamber fan | PB9 `[fan_generic]` | by eye |
| Heatsink fan | PC2 `heater_fan` at 50 C | by eye |
| Pressure advance | 0.05, smooth 0.04 | ladder test kept it (section 7) |

Not yet known: PA4 (a 0-100 % software PWM in the stock fw, maybe the light).

## 6. Hard rules (never)

- Never unlock or clear RDP, mass-erase, or write option bytes. Never connect
  programmer VCC. Never flash the HC32F460 Klipper build onto this N32 unit.
- **Never sync the IAP** (`nasync`, `stm32isp-ad4 -s`, `IAPCommand`) unless a
  complete image write follows in the same run: **the sync erases the app.**
  Flashing only via `work/n32/iap-flash.sh` on the SoC, with the vendor GUI
  down, and only on Caleb's explicit go. Afterwards `stty -F /dev/ttyS1 -parenb`.
- MCU operations only with the printer idle. Never `fuser -k /dev/ttyS1`.
  Never kill `wpa_supplicant`/`wpa_cli` on the SoC.
- Homing and any motion only with Caleb present and after his go for that
  specific motion. Announce any plan change before moving.
- Never point `SET_PIN` at a heater, fan or stepper pin. Find pins in the
  stock firmware (`~/ad4-re/na.bin`, `na.dis`, base 0x08010000), not by
  toggling outputs.
- Keep `NO_FLASH` on the USB stick. Don't tap KLIPPERIZE. No factory reset
  without Caleb saying so. SoC boot changes must stay reversible.
- Do not commit `docs/caleb-unit/UNIT-PRIVATE.md` (git-excluded, holds the
  burned SoC root password) or the untracked `work/Adventurer4-*-IAP-*.bin`.
- `reboot` on the SoC: use `sync; reboot -f`.

## 7. Tuning results: what worked, what not to retry

| Tried | Result |
|---|---|
| Fan 100 % (SUNLU preset) | wall gaps from the bottom up; fan 30-40 % fixed most of it |
| Heater power, fan 100 % vs 30 % | 0.31 vs 0.28 PWM; the heater keeps up, the gaps are cooling/melt |
| Ladder 11-03: A preset / B 235 C / C + no z-hop / D + PA 0.5 | A best, B close, C and D thin weak walls. Keep the preset |
| Extruder slip near 4.8 mm3/s | closed by Caleb; keep max volumetric 3 mm3/s |
| Dark line in cube 1 | marker ink from the length marks, not a clog |

Do not re-propose 235 C, z-hop off or PA 0.5 without new evidence. The ladder
did not isolate the cause of C/D's thin walls: they printed last after
~40 min at 235 C, so heat creep or feed (spool drag, ground filament) is
confounded with the changes. If thin walls show up on long prints, check
those before settings. A proper PA tuning tower (range 0.05-0.4) is the
clean next experiment if corners ever matter.

## 8. Working methods that held up

- Find hardware facts in the stock firmware offline before touching the
  machine. Every pin found this way (endstops, TMC mux, fans) was right; the
  reference unit's config was wrong in several places (Z endstop, step scaling,
  part fan).
- Measure motion with a ruler on short `FORCE_MOVE`s before trusting any
  scaling. Homing was only tried after that.
- For prints, watch with a small Moonraker poller on prodesk (see
  `print_stats`, `toolhead`, `extruder`) instead of guessing.
- Slicing from WSL works: Elegoo's CLI with flattened presets that keep
  `inherits`: `elegoo-slicer.exe --slice 0 --load-settings "machine.json;process.json" --load-filaments filament.json --outputdir out x.stl`.
- `ad4-bridge` (source `install-on-printer/ad4-bridge.c`, binary next to it =
  `/opt/ad4-klipper/ad4-bridge`) sets the UART rate itself (fixed 2026-11-03;
  previous binary kept as `ad4-bridge.pre-baudfix`). Build on prodesk:
  `arm-linux-gnueabi-gcc -std=gnu11 -O2 -Wall -static -o ad4-bridge ad4-bridge.c`.
  Copy to the SoC with `ssh ... 'cat > /tmp/x' < file` (no sftp-server, so
  plain scp fails). Swap only with the printer idle: write `.tmp`, `mv` over,
  kill the old pid, start it like `klipper-boot.sh` line 20. The first
  FIRMWARE_RESTART after a bridge swap can fail with "Failed automated reset"
  (its reset went out while the link was down); run it once more.
- Shell traps: `pkill -f <pattern>` inside `ssh prodesk '...'` matches and kills
  its own shell; use `pkill -f "[p]attern"`. `ssh prodesk` eats `$` in
  double-quoted commands; stage scripts as files and scp them.

## 9. Recovery

- **SoC will not come up:** USB stick (vfat, label FLASHFORGE) holds
  `flashforge_init.sh.rescue2` and `flashforge_init.sh.klipper-boot-v3`.
  Rename one to `flashforge_init.sh`, insert, power-cycle. Rescue v2 brings up
  dropbear + network and parks; it never touches ttyS1, NRST or the IAP.
- **Back to stock firmware:** flash the stock NA hex with `iap-flash.sh`
  (Caleb's go), then remove `klipper-mode` and reboot. Proven 2026-10-27.
- **ST-Link:** bootloader recovery only. SWD cannot attach while Klipper runs
  (`wfi`), and connect-under-reset does not work on the N32.

## 10. Open items

- Panel touch buttons: needs a person to tap them.
- PA4 light and the camera: separate tracker entry.
