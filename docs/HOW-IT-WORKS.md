# How it works

## The printer

The Adventurer 4 has two computers:

- **SoC**: an Allwinner V3S running Linux (busybox, kernel 3.4) from eMMC.
  It runs FlashForge's touchscreen UI, WiFi, Ethernet and the USB port.
- **MCU** on the mainboard: an N32G455 (NA board) or HC32F460 (HD board).
  It drives the steppers, heaters and fans. Its USART1 is wired to the SoC's
  `/dev/ttyS1`; two SoC GPIOs drive its reset (GPIO34) and a boot strap
  (GPIO35).

The MCU has a FlashForge bootloader at 0x08000000 and the application from
0x08010000. FlashForge's updater `IAPCommand` (on the SoC, in
`/opt/PROGRAM/control/2.3.0/`) resets the MCU into that bootloader through
the GPIOs and sends a new application over ttyS1 in 1 KiB frames. It takes
two hex files, one for each board type, and writes the one that matches the
chip it finds.

Klipper runs the same way the stock firmware does: built for 0x08010000
(`CONFIG_STM32_FLASH_START_10000`), USART1 at 115200, 12 MHz crystal. The
bootloader jumps to it as it would to FlashForge's application.

## The USB stick hook

At every boot, FlashForge's `/opt/auto_run.sh` mounts a USB stick and, if
the stick has `flashforge_init.sh` at its top level, runs it **before** the
UI starts. Exit code 0 makes auto_run park (`sleep 100000`); anything else
lets the boot continue. Both sticks use exactly this hook, so at the time
they run, ttyS1 and the touchscreen are free.

## Install sequence

Ordered so that every failure leaves a printer that boots:

| Step | If it fails |
|---|---|
| 1. Checks: board marker, kit md5s, IAPCommand md5, stick md5s, ttyS1 free | Stops. Nothing changed. |
| 2. Panel prompt, 2 min timeout | CANCEL / timeout: normal boot, nothing changed. |
| 3. Copy runtime to `/opt/ad4-klipper` (via `.new`, then rename) | Stops. Printer still boots stock. |
| 4. Rewrite the launch line in `/opt/auto_run.sh` (new file, `sh -n` check, rename) | Stops. The original is untouched. |
| 5. SSH, root password | |
| 6. IAPCommand: Klipper in the NA slot, stock HD image in the HD slot | Stops with "Flash failed". Power-cycle with the stick to retry. |
| 7. Create `/opt/ad4-klipper/klipper-mode`, retire the stick script, reboot | |

Until step 7 creates `klipper-mode`, the new launch line still starts the
stock UI. An HD board that slips through step 1 gets FlashForge's own HD
image in step 6, so it ends up stock, not bricked.

The launch line the install writes:

```sh
if [ -f /opt/ad4-klipper/klipper-mode ]; then /opt/ad4-klipper/klipper-boot.sh & else /bin/dropbear -R -p 22; /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg & fi
```

## Why the MCU cannot be bricked this way

- IAPCommand never writes the bootloader, only the application area.
- IAPCommand reaches the bootloader with a hardware reset and strap, so it
  works whatever the application is: stock, Klipper, or half-written.
- A failed or interrupted write leaves the bootloader waiting; the next
  IAPCommand run writes a complete image.

Known hazard, handled: entering the bootloader **erases the application**.
That is why nothing in this project starts IAPCommand without a complete
image to follow, and why a flash failure tells you to retry rather than
power off and walk away.

## Klipper mode at boot

`/opt/ad4-klipper/klipper-boot.sh`, from the launch line:

1. `ad4-screen boot` (a 4 s pixel-art animation with the jingle, in the
   background), then dropbear (SSH).
2. ttyS1 to 115200 8N1 raw (IAPCommand leaves it in even parity), then
   `ad4-bridge 5000 /dev/ttyS1 115200`: one TCP client at a time, bytes
   passed through both ways, the UART reopened if the client drops.
   Then `ad4-cam CAM_PORT` in a respawn loop: the camera as MJPEG at
   `http://PRINTER:8080/stream` and `/snapshot`. It opens the camera only
   while someone is watching.
3. eth0 by DHCP or static address.
4. Once the animation has finished, `ad4-screen klipper HOST:PORT` in a
   respawn loop: polls Moonraker, draws
   the status, sends the button presses, and POSTs `firmware_restart` when
   Klipper reports the MCU lost (at most every 20 s). While Klipper is idle
   it has menus: Print (Moonraker's gcode files, newest first, with the
   slicer's thumbnail, time and grams; starting a print takes two taps), Preheat (PLA, PETG,
   ABS, TPU, cooldown), Filament (load and unload the whole ~690 mm
   Bowden tube, purge 20 mm, through the `LOAD_FILAMENT` / `UNLOAD_FILAMENT`
   macros in printer.cfg; while one runs, a bar from GEARS to NOZZLE follows
   the filament tip in mm, from the extruder's live position) and Tools
   (part and chamber fan, light off / on / 30 %, Move: home and jog, motors off, About with
   firmware restart). During a job the home screen shows the slicer's
   thumbnail, time left, the finish time, layer N / M, progress, elapsed
   time, part fan and filament used (see `improvements/ROADMAP.md`, J3-J5),
   with Pause / Resume, Tune and Cancel. Tune has - / + for speed (10 %
   steps, down only: 60 mm/s is a slicer cap), flow (2 %), Z offset
   (0.02 mm, applied at once), nozzle (5 C) and part fan (10 %); a value
   off its default is amber. The Z offset lasts until Klipper restarts.
   Tune's Objects page draws the bed with each object's outline from
   `exclude_object` (the one printing now in blue) and a button per object;
   two taps skip it (`EXCLUDE_OBJECT`). The last object left cannot be
   skipped: that is Cancel. Needs "label objects" in the slicer.
   Paused, the home screen shows why (the `SET_DISPLAY_TEXT` message, e.g.
   "Next: object 2 of 3" from a by-object plate) with READY, Tune, Cancel
   and Filament; Filament then loads and unloads at the job's temperature
   (`improvements/B2-plate-stops.md`). `PAUSE` lifts clear of every part
   printed so far and brings the bed forward; `RESUME` returns over the part
   before going down.
   After a job ends the home screen shows a card until OK: done, cancelled
   or failed (with Klipper's message), real time against the estimate,
   filament, cost, and REPRINT (two taps). The cost rates are read from
   Moonraker's database; change them from the host with
   `curl -X POST localhost:7125/server/database/item -H "Content-Type: application/json"
   -d '{"namespace":"ad4","key":"cost","value":{"price_kg":14,"density":1.24,"machine_h":0.75}}'`
   ($ per kg, g/cm³, $ per hour of printer time). Without them it shows no cost.
   To swap the panel binary on a running printer, kill the
   `ad4-screen klipper` process, not the `sh -c while` loop around it.
   Its HOME runs `HOME_XYZ`, a macro for `G28` (one axis at a time).
   `host/home_xyz.py` (all three in one move) is off; why is in
   `host/printer.cfg`. A job starting from anywhere, or two minutes without a
   touch, returns it to the home screen.
5. WiFi from `/etc/wpa_supplicant.conf` (the network saved in the stock UI).

Log: `/tmp/ad4-boot.log` (RAM, gone at reboot).

On the host, Klipper opens `socket://PRINTER:5000` through pyserial's
`serial_for_url` (the patch in `host/`). The link is wired because over
WiFi the latency jitter broke Klipper's clock sync ("Timer too close").

## Files on the printer after install

| Path | |
|---|---|
| `/opt/ad4-klipper/` | runtime: bridge, panel, buzzer, boot script, settings, klipper.hex |
| `/opt/ad4-klipper/klipper-mode` | flag: boot Klipper mode. Delete it and reboot for the stock UI (which cannot drive Klipper's MCU; use the rollback stick to really go back) |
| `/opt/auto_run.sh.pre-ad4-klipper` | auto_run.sh as it was before the first install |
| `/opt/services/bin/dropbearmulti`, `/bin/{dropbear,dropbearkey,scp,ssh}` | SSH |
