# Build

Everything in a release can be rebuilt from this repository. Tested on
Ubuntu 24.04.

## Printer binaries (bin/)

```sh
sudo apt install gcc-arm-linux-gnueabi
make -C src            # bin/ad4-bridge, bin/ad4-screen, bin/ad4-buzz, bin/ad4-cam (static, stripped)
```

| | |
|---|---|
| `ad4-bridge` | `ad4-bridge PORT TTY BAUD`: TCP server, one client, bytes to and from the UART |
| `ad4-screen` | panel: `splash`, `boot` (start-up animation), `confirm`, `status`, `done`, `fail`, `klipper HOST:PORT` (status and menus) |
| `ad4-buzz` | buzzer cues: `tap`, `mark`, `start`, `ok` (the jingle), `fail` |
| `ad4-cam` | `ad4-cam [PORT] [WxH] [FPS]` (default 8080 1280x720 15): camera server, MJPEG at `/stream` and `/snapshot` |

`bin/dropbear` is Dropbear 2022.83 `dropbearmulti`, static ARM, prebuilt.

Panel screenshots without a printer: `make -C src preview` builds a host
version of `ad4-screen` that writes the screens to `build/preview/*.png`.

Buzzer cues without a printer: `python3 -I tools/buzz-preview.py src/ad4_buzzer.h`
writes `build/preview/buzz-*.wav`. The cues are the tables in
`src/ad4_buzzer.h`; keep every note above 400 Hz (the PWM cannot go lower).

## Klipper firmware (firmware/)

```sh
sudo apt install gcc-arm-none-eabi
firmware/build.sh
```

Fetches Klipper `020d25e` (shallow), builds it with
`firmware/klipper-n32g455.defconfig` and writes
`firmware/klipper-n32g455.hex` (`objcopy -O ihex` of the ELF). With
arm-none-eabi-gcc 13.2.1 (Ubuntu 24.04) the result is byte-identical to the
shipped hex, md5 `78120aec0409618bed22daef7bb98550`.

The MCU code is unmodified mainline Klipper. `klipper-n32g455.identify.json`
is the shipped build's data dictionary, for checking a running printer
against it (`tools/klipper-identify.py`).

## Release zips (dist/)

```sh
tools/build-release.sh            # VERSION from git describe
VERSION=v1.0 tools/build-release.sh
```

Writes `dist/ad4-klipper-install-VERSION.zip`,
`dist/ad4-stock-rollback-VERSION.zip` and `dist/MD5SUMS`. The install
stick's `ad4-klipper/MD5SUMS` covers every payload file except the settings
file, and the stick refuses to run if one does not match.

## Testing without flashing

- `sh -n` every script.
- On a printer, the install stick's checks can be run from `/tmp` without
  touching anything: source `lib.sh` with `STICK` and `LOG` pointing into
  `/tmp`, then call `board`, `find_kit`, `frames`, `busy`.
- `set_launch` can be pointed at a copy of `/opt/auto_run.sh` by setting
  `A` after sourcing `lib.sh`. factory → klipper → stock must give back the
  factory file byte for byte.
