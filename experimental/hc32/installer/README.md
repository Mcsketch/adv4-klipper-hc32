# install-on-printer

One folder = one USB stick. Everything needed to take a stock Adventurer 4
to Klipper (and back) lives here; `flashdisk/` is the older scratch copy.

## Build the stick

Format the stick **FAT32** and copy this folder's contents to its root:

```sh
cp -r install-on-printer/* /Volumes/MYSTICK/
```

Then add what only you can supply — FlashForge's own firmware, in `factory/`
(see the next section) — and optionally a `ROOT` file for the root password.

`build-stick.sh /Volumes/MYSTICK` does the same copy plus a few sanity checks
(ARM binaries, `klipper.hex` load address). It is optional; the stick is just
files on a FAT32 volume.

Flag files you can drop on the stick to pick a mode without the touchscreen:

| file | effect |
| :-- | :-- |
| *(none)* | klipperize: flash `klipper.hex`, set up SSH + bridge |
| `PERSISTENT` | also install the eMMC boot hook, so it survives power cycles |
| `NO_FLASH` | do not touch the MCU — SoC access + bridge only |
| `RESTORE` | unwind the boot hook and flash the stock MCU image back |
| `RESTORE` + `NO_FLASH` | unwind the boot hook only, leave the MCU image alone |
| `FACTORY` | full stock restore (needs `factory/`; the panel still asks twice) |
| `DUMP` | read the MCU flash out through Klipper, write nothing |

With no flag at all the stick klipperizes, and the touchscreen menu lets
anyone tap a different choice: klipperize / restore mcu / full factory /
dump / skip.

## factory/: FlashForge's firmware, supplied by you

FlashForge's firmware is theirs, so it is **not in this repository**. Put it
in a `factory/` folder on the stick — either the four packages or the whole
`Adventurer4-2.2.4-2.3-20230216` folder dropped in as-is (the restore
descends into it):

```
factory/control-*.tar.xz     the two MCU images (HD and NA)
factory/kernel-*.tar.xz      uImage + script.bin on the boot partition
factory/software-*.tar.xz    the vendor GUI and the stock /opt/auto_run.sh
factory/library-*.tar.xz     ffstartup-arm, ffstartup.cfg, the Qt fonts
```

The version in each name does not matter — the restore matches on the prefix
and takes whatever it finds. If no package is there, the MCU image is still
available: the printer keeps a copy of both hexes in `/opt/PROGRAM/control/`,
and the installer falls back to that. So a plain RESTORE works even with an
empty `factory/`.

`start.img` / `end.img` are not used — they are raw eMMC images for a
recovery tool, and this restore is a package swap.

## At boot the stick does, in order

1. kills the vendor app so `/dev/ttyS1` is free,
2. reads the chip revision from the stick's `ad4-rev` file (the old
   `stm32isp-ad4 -s` probe is gone since 2026-10-27: syncing the IAP erases the app),
3. puts the menu on the touchscreen (KLIPPERIZE / RESTORE / SKIP); the stick's
   flag files are the default and the fallback when there is no panel,
4. flashes the MCU through `stm32isp-ad4`, our own flasher (it refuses any
   record below `0x0000C000` — the HC32F460 bootloader is flash-resident,
   that guard is what keeps the board alive),
5. sets `eth0` to `192.168.10.10/24`,
6. starts `ad4-bridge` (TCP `:5000` <-> `/dev/ttyS1` @115200),
7. installs `dropbear` + sets the root password,
8. with the `PERSISTENT` flag: copies the package to `/opt/ad4-klipper`,
   gates the vendor UI out of `/opt/auto_run.sh`, and reboots.

The piezo beeps on every accepted touch, and a finished flash plays the
opening guitar hook on the buzzer. That note table is generated, not
hand-written: `python3 -I tools/gen-riff.py indochine-l_aventurier.midi`.
It transposes the line up an octave on its own — the PWM divider cannot
represent a period longer than ~2.7 ms, so notes under ~370 Hz come back as
clicks while every ioctl still reports success.

Everything is logged to `ad4-flash.log` **on the stick** — pull it and read
that file if the printer seems unresponsive.

## Factory apps

Neutralization is one `awk` wrap of the vendor launch line in
`/opt/auto_run.sh`:

```sh
if [ -f /opt/ad4-klipper/enable ]; then /opt/ad4-klipper/ad4-daemon.sh & \
else /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg & fi
```

So the toggle is the flag file, no re-patch needed:

```sh
rm    /opt/ad4-klipper/enable   # vendor UI back on next boot
touch /opt/ad4-klipper/enable   # Klipper daemon back
```

In Klipper mode `ad4-daemon.sh` starts the bridge on `:5000`, sets `eth0`
static, and keeps WiFi up itself (`wpa_supplicant` on
`/etc/wpa_supplicant.conf`, `enable_network all`, then `udhcpc` on `wlan0`),
because on a stock printer the vendor GUI is what does that. It never kills
`wpa_supplicant` to free `/dev/ttyS1`: that process holds an inherited handle
and never writes. On an NA / N32 board the daemon never flashes the MCU; the
stick's `ad4-rev` decides, and the only flash path there is the vendor
`IAPCommand` (`work/n32/iap-flash.sh`).

Three revert paths, deepest first. Each is a file on the stick (or nothing at
all), so `build-stick.sh` is never required to choose one:

```sh
# 1. on the SoC, no stick — full revert incl. the MCU image
sh /opt/ad4-klipper/ad4-uninstall-persistent.sh

# 2. stick with a RESTORE file — same thing, run for you (it flashes the MCU
#    itself, so it calls the uninstaller with --no-mcu)
touch /Volumes/MYSTICK/RESTORE

# 3. RESTORE + NO_FLASH — vendor UI back, MCU untouched
#    (still Klipper, so the vendor app will not drive the motors)
touch /Volumes/MYSTICK/RESTORE /Volumes/MYSTICK/NO_FLASH
```

The uninstaller is copied to `/opt/ad4-klipper` at install time, so path 1
works with no stick in hand. It restores the MCU with the printer's own
`IAPCommand` kit in `/opt/PROGRAM/control/` when it is there (both hexes; the
tool picks NA or HD from the chip's reply), else, on HD boards only, with
`stm32isp-ad4` and a factory HD hex (its own copy or the stick's `factory/`).
It reads the revision from `ad4-rev` or the vendor's `/root/N32` / `/root/H32`
marker and never syncs the IAP to find out, because that erases the app. The
untouched stock boot file stays at
`/opt/auto_run.sh.ad4-orig`.

## Root password

Default is `flashforge`. To use your own, put a `ROOT` file on the stick whose
first line is the password:

```sh
printf 'mypw\n' > /Volumes/MYSTICK/ROOT
```

Delete the file and the default applies again — the default lives in
`flashforge_init.sh`, so a missing or empty `ROOT` is never an error. The file
is read as data, never sourced, so any character is safe in the password. It
is git-ignored; so is everything under `factory/`.

To change it on an already-installed printer:

```sh
sh ad4-setrootpw.sh 'newpass'          # on the SoC
```

or over SSH from the Mac:

```sh
sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
    'cat > /tmp/ad4-setrootpw.sh' < ad4-setrootpw.sh
sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
    "sh /tmp/ad4-setrootpw.sh 'newpass'"
```

`flashforge_init.sh` re-applies the password on every stick boot, so a lost
password is one stick-boot away.

## Files

| file | what |
| :-- | :-- |
| `flashforge_init.sh` | the SoC-side script the vendor boot path runs from the stick |
| `build-stick.sh` | optional: same copy as above, plus sanity checks |
| `klipper.hex` | Klipper for HC32F460, positioned @ `0x0000C000`, PA9/PA10 @115200 |
| `ad4-factory.sh` | full stock restore: MCU first, then the four packages. `probe` / `mcuhex` / `run` |
| `ad4-dump.sh` | reads the MCU flash back out through Klipper (`check` / `peek` / `boot` / `full`) |
| `ROOT` | optional, yours: first line = the root password you want |
| `stm32isp-ad4` | our ARM flasher (source in `../stm32isp-ad4/`): same HC32 protocol as the vendor tool, plus a `-s` chip probe (DESTRUCTIVE: sync = app erase; refused since 2026-10-27) |
| `ad4-screen` | static ARM panel UI: touchscreen menu, live status while flashing, chip shown in the header |
| `ad4-buzz` / `.c` + `ad4_buzzer.h` | static ARM buzzer player: the `/dev/pwm` ioctls (decoded from the vendor `play` binary) and the cues, including the completion riff |
| `ad4-bridge` / `.c` | static ARM (zig) TCP<->UART relay — the bridge |
| `dropbear` | dropbearmulti 2022.83 (dropbear/dropbearkey/ssh/scp) |
| `ad4-daemon.sh` | boot-time net + bridge, what the persistent hook runs |
| `ad4-install-persistent.sh` | installs the eMMC boot hook, then reboots |
| `ad4-uninstall-persistent.sh` | restores stock boot |
| `ad4-setrootpw.sh` | root password change + SSH persistence check |
| `ad4-revcheck.sh` | DISABLED 2026-10-27: it was never read-only, the IAP erases the app on sync |
| `ad4-net.conf` | `SOC_IP` / `SOC_NET` / `BRIDGE_PORT` |
| `screwflag` | empty; skips the vendor machine-type check |
| `ad4-serial-bridge.py` | python fallback (the SoC has no python3 — dead weight) |

## Rebuilding klipper.hex

```sh
cd ~/klipper-hc32 && make                    # board.config = hc32f460, PA9/PA10, 115200
python3 -I ~/adv4-klip/tools/make-positioned-ihex.py \
        out/klipper.bin 0xC000 ~/adv4-klip/install-on-printer/klipper.hex
```

Do **not** ship `out/klipper-ad4.hex` (what `make` leaves behind). It is `objcopy`
output, and for an image that straddles a 64K page `objcopy` switches to
type-02 *extended segment* records instead of type-04 — a loader that ignores
those writes the second page at `0x00000000`, straight over the flash-resident
bootloader. `make-positioned-ihex.py` emits one type-04 record per page, which
is the layout the factory image and the vendor flasher are proven to consume.

Current `klipper.hex`: 28488 bytes @ `0x0000C000..0x00012F48`,
`sha256 00c3c429d1ff978b2e92bc59c586a88fb41cb4106cc969a9e0ac202d83c218eb`.

## After the stick

```sh
ssh root@192.168.10.10                      # password: whatever you set
sh /opt/ad4-klipper/ad4-revcheck.sh         # MCU identity (kills the bridge)
```

On the Mac the bridge is used directly: `[mcu] serial: socket://192.168.10.10:5000`
(see `../printer-hc32.cfg`).
