# Install

About 30 minutes, most of it host setup. The printer part takes two.

## You need

- An Adventurer 4 with the **NA** mainboard (N32G455) on FlashForge
  firmware 2.3. Not sure which board? Run the stick anyway: it checks and
  stops with "HD board" before changing anything if it is the other one.
- A Klipper host: a Raspberry Pi or any Linux machine running Klipper,
  Moonraker and Mainsail or Fluidd (KIAUH sets these up).
- An Ethernet cable from the printer to the host's network, or straight to
  the host. The MCU link must be wired: over WiFi Klipper stops with
  "Timer too close".
- A USB stick formatted FAT32 (one partition, any size).

## 1. Klipper host

```sh
git clone https://github.com/Mcsketch/adv4-klipper-hc32 ad4-klipper
cd ad4-klipper
host/install-host.sh PRINTER_IP
```

`PRINTER_IP` is the address the printer will have on Ethernet: the static
one you put in `ad4-klipper.conf` (step 2), or the one your router hands it
(the panel shows it after the install; run the script again then).

The script:

1. applies `host/klipper-socket-serial.patch` to `~/klipper`, so
   `[mcu] serial:` accepts `socket://HOST:PORT`. A Klipper update through
   Moonraker or `git pull` drops the patch; run the script again after one.
2. copies `host/printer.cfg` to `~/printer_data/config/printer.cfg` with
   your address filled in, keeping the old file as `printer.cfg.bak-DATE`.
   `printer.cfg` has `[include mainsail.cfg]`; change it to `fluidd.cfg`
   if you use Fluidd.
3. restarts Klipper. It reports "Unable to connect" until the printer runs
   Klipper; that is expected.

For the touchscreen panel, the printer must be allowed to talk to Moonraker
without a login. In `moonraker.conf`:

```ini
[authorization]
trusted_clients:
    192.168.100.0/24     # or the printer's address / your LAN
```

The camera: the printer serves it at `http://PRINTER_IP:8080/stream` and
`/snapshot` (`CAM_PORT` below). If your browser can reach the printer, add
to `moonraker.conf`:

```ini
[webcam ad4]
service: mjpegstreamer
stream_url: http://PRINTER_IP:8080/stream
snapshot_url: http://PRINTER_IP:8080/snapshot
aspect_ratio: 16:9
```

If the printer sits on a network only the host reaches (a direct cable),
let the web server in front of Mainsail proxy it and use `/webcam/stream`
and `/webcam/snapshot`. Caddy:

```
handle_path /webcam/* {
    reverse_proxy PRINTER_IP:8080 {
        flush_interval -1
    }
}
```

## 2. The stick

Unzip `ad4-klipper-install-VERSION.zip` onto the empty FAT32 stick, so the
stick's top level holds `flashforge_init.sh` and the `ad4-klipper/` folder.

Then edit `ad4-klipper/ad4-klipper.conf` on the stick:

| Setting | Default | |
|---|---|---|
| `ETH_ADDR` | `dhcp` | or a static `192.168.100.20/24`, or `off` |
| `ETH_GW` | empty | gateway for a static address |
| `WIFI` | `on` | join the WiFi saved in the stock firmware (for SSH, not for Klipper) |
| `MOONRAKER` | empty | `HOST:PORT` of Moonraker. Set it, or the panel only shows the printer's addresses |
| `BRIDGE_PORT` | `5000` | must match `socket://PRINTER_IP:5000` in printer.cfg |
| `CAM_PORT` | `8080` | camera server port; empty turns it off |
| `ROOT_PASSWORD` | empty | empty: a random one is written to `ROOT_PASSWORD.txt` on the stick. `keep`: leave it. Anything else: that password |

## 3. Install

1. Printer off. Stick in the USB port on the front.
2. Power on. After the FlashForge logo the panel shows **Install Klipper?**
   with the board type. Tap **INSTALL**. (CANCEL, or no answer in two
   minutes, boots the printer normally with nothing changed.)
3. A progress bar counts the 42 firmware frames. Do not power off.
4. **Klipper installed**, a short tune, and the printer reboots on its own.
   Take the stick out. The script on the stick is now
   `flashforge_init.sh.done`, so it will not run again.
5. Copy `ROOT_PASSWORD.txt` off the stick if one was generated, then delete
   it from the stick.

The stick's log is `ad4-klipper.log`, and `iap.log` holds the flasher's
output. Keep them if anything went wrong.

## 4. First start

After the reboot (about 25 s) the panel shows Klipper's state, or the
printer's addresses if `MOONRAKER` is empty. In Mainsail/Fluidd, Klipper
should reach `ready` by itself; if not, press FIRMWARE_RESTART.

Whenever the printer is switched off and on, Klipper on the host loses the
MCU. With `MOONRAKER` set, the panel sends `FIRMWARE_RESTART` for you.

SSH: `ssh root@PRINTER_IP`. Settings on the printer:
`/opt/ad4-klipper/ad4-klipper.conf` (reboot the printer after editing).

## 5. Before the first print

The config is right for the hardware but some values come from one printer.
With someone at the printer, hand near the power switch:

1. `G28 X`, then `G28 Y`, then `G28 Z`, one at a time. Z homes **up**: the Z
   endstop is at the top.
2. Z height: home, move to X110 Y100, lower to the bed with a sheet of
   paper, and set `position_endstop` / `position_max` in `[stepper_z]` to
   the distance travelled (254.7 on the reference printer).
3. `PID_CALIBRATE HEATER=extruder TARGET=210`, then
   `PID_CALIBRATE HEATER=heater_bed TARGET=60`, then `SAVE_CONFIG`.
4. Extrude 100 mm at 2 mm/s and measure; adjust the extruder's
   `rotation_distance` if it is off.

## 6. Slicer

Any slicer with a Klipper / generic Marlin flavour works. Settings that
proved out on this printer:

- Start G-code: `PRINT_START BED=[bed_temperature_initial_layer_single] EXTRUDER=[nozzle_temperature_initial_layer]`
- End G-code: `PRINT_END`
- Bed 220 x 200, height 250, nozzle 0.4.
- **Max volumetric speed 3 mm³/s.** The stock hot end and extruder slip
  near 4.8 mm³/s.
- PLA: 215-225 °C, bed 55-60 °C, part fan 30-40 % (100 % gives weak walls),
  retraction 5.5 mm (Bowden).

Upload from the slicer to Moonraker (OctoPrint-compatible host type at the
Mainsail/Fluidd address).
