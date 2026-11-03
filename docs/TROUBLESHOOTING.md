# Troubleshooting

Logs on the stick: `ad4-klipper.log` (every step) and `iap.log` (the
flasher). On the printer in Klipper mode: `/tmp/ad4-boot.log`,
`/tmp/ad4-bridge.log`, `/tmp/ad4-screen.log`. On the host:
`~/printer_data/logs/klippy.log`.

## The stick does nothing

The stock UI starts as if no stick were there.

- The stick must be FAT32, and `flashforge_init.sh` must be at its top
  level, not inside a folder. Unzipping into a folder named after the zip is
  the usual cause.
- The script ran before and renamed itself to `flashforge_init.sh.done`.
  Unzip the release again.
- Try another stick. Some large or USB 3 sticks are not detected.

## Messages on the panel

| Message | Meaning | Changed? |
|---|---|---|
| HD board | HC32F460 mainboard; the sticks support NA only | no |
| Stock kit missing | FlashForge's MCU kit in `/opt/PROGRAM/control` is missing or fails its md5 list | no |
| Untested kit | IAPCommand is not the 2.3.0 version this was tested with | no |
| Stick damaged | a file on the stick fails `MD5SUMS`; unzip again | no |
| Printer busy | something holds the MCU port at boot (see the log) | no |
| Cancelled | CANCEL tapped, or nobody answered in two minutes | no |
| No touchscreen answered | the panel could not be opened | no |
| Boot hook failed | `/opt/auto_run.sh` could not be rewritten; the original is untouched | runtime copied, boots stock |
| Flash failed | IAPCommand did not finish | MCU has no application: power-cycle with the stick to retry |
| HD board (after flashing) | IAPCommand found an HD chip and wrote FlashForge's HD image | printer boots stock |

After "Flash failed" the stick is not retired, so the next power-on runs it
again. If it fails twice, keep both logs and use the rollback stick.

## Klipper: "Unable to connect" / "mcu 'mcu': Unable to open port"

1. Does the panel show the printer's address, and does it match
   `[mcu] serial: socket://IP:5000`?
2. From the host: `nc -vz PRINTER_IP 5000`. Refused: the bridge is not
   running; SSH in and read `/tmp/ad4-boot.log` and `/tmp/ad4-bridge.log`.
3. `klippy.log` says `socket://` is not a valid port, or "could not open
   port": the host patch is missing (Klipper was updated). Run
   `host/install-host.sh PRINTER_IP` again.
4. Only one client at a time: a second Klipper, or a leftover `nc`, holds
   the bridge.

## Klipper: "Lost communication with MCU" after the printer was off

Expected: the MCU restarted. Press FIRMWARE_RESTART. With `MOONRAKER` set in
`ad4-klipper.conf`, the panel does it within ~20 s; if it does not, check
Moonraker's `trusted_clients` includes the printer's address.

## Klipper: "Timer too close"

The MCU link is going over WiFi or a congested network. Use Ethernet,
ideally a direct cable or a quiet switch, and point `socket://` at the
printer's Ethernet address.

## The panel stays on the splash screen

`MOONRAKER` is wrong or Moonraker refuses the printer: see
`/tmp/ad4-screen.log`, add the printer to `trusted_clients`.

## Extruder clicks or under-extrudes

The stock hot end tops out near 4.8 mm³/s. Keep the slicer's max volumetric
speed at 3 mm³/s.

## Back to the stock UI for one boot

`ssh root@PRINTER 'rm /opt/ad4-klipper/klipper-mode; sync; reboot -f'`.
The stock UI starts but cannot talk to an MCU running Klipper. To really go
back, use the [rollback stick](ROLLBACK.md); to return to Klipper,
`touch /opt/ad4-klipper/klipper-mode` and reboot.
