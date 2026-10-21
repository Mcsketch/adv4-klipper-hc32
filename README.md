# Flashforge Adventurer 4 → Klipper — Complete Engineering Dossier

Klipper support for the Flashforge Adventurer 4 / 4 Pro **HC32F460 (Hollystone HD)** MCU:
the open-source MCU flasher, the SoC↔MCU serial bridge, the complete Klipper board port,
and the unattended USB-stick / persistent eMMC install path.

Self-documenting folder. All artifacts verified against:
- `FFAD4-BKUP.zip` (7.8 GB eMMC image of a working printer)
- `control-2.3.0.tar.xz` (factory updater package)
- Direct disassembly of `IAPCommand` (stm32isp fork)
- Raw Intel-HEX parsing of factory MCU images

## Layout

| Path | What |
|---|---|
| `flasher/` | `stm32isp-ad4.c` — self-contained stm32isp fork with the AD4 SoC GPIO strapping (NRST=GPIO34 proven; GPIO35 driven, role unproven) + the HC32 vendor 1030-byte IAP packet protocol. `flash-mcu.sh` — unattended flash runbook. |
| `bridge/` | SoC-side userland: `ad4-bridge` (C, MCU UART → TCP 5000), `ad4-serial-bridge.py`, `klip_probe.c` (Klipper identify liveness probe), `rom_sweep.c`, `ad4-identify-probe.py`, plus Mac-side Fluid/klippy setup scripts. |
| `klipper-port/` | The Klipper board files: `src-hc32f460/` (main/serial/gpio/pwm/adc/Kconfig/Makefile), `lib-hc32f460/` (vendor driver), `board.config` (working `.config`), `make-positioned-ihex.py`. |
| `flash-images/` | Validated Intel-HEX ready to flash (checksums verified). |
| `docs/` | Flash guide, IAP protocol write-up, STATUS. |

## Documents

| File | Purpose |
|---|---|
| **`GETTING-STARTED.md`** | **The easy path**: stock → Klipper that moves, in 4 easy steps (build stick → plug in → one RPi script → test), no soldering. Start here. |
| **`FIRST-RUN-MANUAL.md`** | **Copy-paste first-run manual**: flash the RPi SD, copy the repo, make the USB key, flash the printer, one RPi script, use from your LAN (2nd NIC). Minimal tech. |
| **`PERSISTENT-INSTALL.md`** | **Durable install**: eMMC boot hook that runs our Klipper runtime *instead of* the vendor `ffstartup-arm` watchdog every boot (flag-gated, reversible, can't brick). MCU re-flash watchdog (optional). |
| **`ALL-FROM-PI.md`** | **Remote, no-stick install**: the RPi drives the SoC over ssh to run the flash + eMMC hook. Root-shell prerequisite, weak-DES root hash, web-console limits, stick-vs-PI comparison. |
| **`adv4-touch-ui.md`** | **Touchscreen UI**: drive Klipper from the SoC touch (Qt4 + TSLIB + Moonraker client). Hardware verified, build plan. |
| **`adv4-n32-thermistor.md`** | **NA (Nation N32G45x) chip**: ADC/thermistor formulas, NA-hex LUT decode (bed/hotend-std/hotend-hi, verified offsets), Klipper build recipe, and a file:line review of the KV N32 flasher (bricking base, sync byte, parity, recovery gaps). |
| **`adv4-exploration.md`** | Shareable engineering report: all verified facts, addresses, GPIO dance, thermistor tables, SoC hardware map, architecture, flashing, recovery |
| **`adv4-soc-hack.md`** | **No-soldering architecture**: RPi→SoC Ethernet, SoC serial↔network bridge, touch UI, USB, network. How to run Klipper over the bridge. MCU + SoC recovery |
| **`printer-hc32.cfg`** | Klipper `printer.cfg` draft (correct where certain, annotated where live verification is needed) |
| **`KLIPPER-AD4-FLASH-GUIDE.md`** | Original flash guide (pre-fork); still valid for reference |

## Code

| Folder | Contents |
|---|---|
| **`stm32isp-ad4/`** | Complete self-contained C fork of `stm32isp` with the SoC GPIO dance (NRST=GPIO34 **proven**; GPIO35 is driven too but its "BOOT0" role is **unproven**) built in. **Note: its `-d` dump mode cannot work** — it sends STM32 ROM-ISP read (0x11) which the vendor bootloader does not speak; see `adv4-swd-dump.md`. `Makefile` (native + cross). `flash-mcu.sh` (SoC driver). Factory HD+NA hexes for restore. **Compiles clean (gcc -Wall -Wextra, zero warnings)** |
| **`soc-hack/`** | `ad4-bridge.c` + static ARM `ad4-bridge` binary (TCP/serial↔network bridge, no python — the SoC has none; build: `zig cc -target arm-linux-musleabi -std=c11 -O2 -static`). `ad4-serial-bridge.py` (reference python bridge, byte-transparent, tested E2E). `ad4-bridge.sh` (SoC start/stop/status). `ad4-rpi-setup.sh` (**one-shot RPi3 host setup**: static IP + preflight + deps + Klipper + socat PTY). `rpi-setup.sh` (older manual variant) |
| **`flashdisk/`** | **Unattended USB-stick flash** (the easy path) **+ persistent install**: `build-usb-stick.sh` (assembles the FAT32 stick, verifies md5 + base; `PERSISTENT` flag ON by default, `NO_PERSISTENT=1` for one-shot), `flashforge_init.sh` (SoC-side flash+net+bridge; `PERSISTENT`/`RESTORE` flag handling), `ad4-daemon.sh` (eMMC runtime: static IP + self-healing bridge + optional MCU re-flash), `ad4-install-persistent.sh` / `ad4-uninstall-persistent.sh` (eMMC boot-hook install/revert), `ad4-install-from-pi.sh` (remote no-stick install over ssh), `README.md` |
| **`tools/`** | `make-positioned-ihex.py` — emits a clean type-05 iHEX so the flasher writes the app to its absolute window (fixes the objcopy wrap that would otherwise land at 0x0000) |

## Work artifacts (reproducible)

| Path | Description |
|---|---|
| `work/ctrl/IAPCommand` | Factory MCU flasher binary (ARM ELF, unstripped) |
| `work/ctrl/Adventurer4-HD-IAP-2.3.0-20230216.hex` | Stock HD MCU image (base 0x0000C000) |
| `work/ctrl/Adventurer4-NA-IAP-2.3.0-20230216.hex` | Stock NA MCU image (base 0x08010000) |
| `work/bkup/dmesg.txt` | Working-machine SoC boot log (peripheral map) |
| `work/bkup/printer_profile_kv.txt` | Slic3r printer profile from /data |
| `work/hd.bin` | Decoded HD MCU image (binary) |
| `stm32isp-ad4/klipper-ad4.hex` | Klipper MCU image, **positioned at `0x0000C000`** (type-04 iHEX, byte-identical to `klipper.bin`) — the safe flash image the stick uses |
| `klipper-hc32/out/klipper.{bin,hex}` | Upstream Klipper HC32F460 build (app window `0x00C000`, ~28 KB) |

## Key verified facts (summary)

| Item | Value | Source |
|---|---|---|
| SoC | Allwinner V3S (sun8i/sun50i), kernel 3.4.39+, armv7l | dmesg |
| MCU-side UART (session 5, **proven from factory firmware**) | **USART2**, TX=**PA9** (FSEL 36), RX=**PA10** (FSEL 37), **115200**, DMA RX — firmware RE of `control-2.3.0` (descriptor `0x14f64`, init `0x15068`, baud literal `0x1c200`); FSEL map from HDSC HAL. The `PA13/PA14` klipper variant is a hallucination (SWD pins, no USART AF) | factory app disasm + `hc32f460_gpio.h` |
| MCU UART on SoC | `/dev/ttyS1` (uart0) — **effectively 115200 only**: uart0 clock is 24 MHz, below the driver's 30–120 MHz range, so `setbaud()` is rejected (dmesg `sw_uart_check_baudset`); the line stays at the boot-time 115200. **Klipper must be built @115200** (a 250k build is inaudible) | dmesg + IAPCommand |
| Ethernet on SoC | `eth0` = gmac0 GbE, PHY 00441400 | dmesg |
| Touch | TSC2007 → `/dev/input/event0,1` | dmesg |
| Display | `/dev/fb0` | dmesg |
| USB host | EHCI+OHCI (bus 1, bus 2) | dmesg |
| GPIO NRST | SoC GPIO **34** (15.68 ms LOW / 15.68 ms HIGH) | IAPCommand disasm |
| GPIO "BOOT0" | SoC GPIO **35** — IAPCommand writes it, but its connection to the MCU is **UNPROVEN** (HC32F460 has no BOOT0 pin; the boot decision is made by the flash bootloader). Only GPIO34=NRST is confirmed | IAPCommand disasm (role inferred, not verified) |
| HD MCU base | **0x0000C000**, 60 416 B | HD hex type-04 record |
| NA MCU base | **0x08010000**, 44 640 B | NA hex type-04 record |
| Safety invariant | Never write ≤ 0x00008000 | Design (keeps the bootloader recoverable — note: **not** "ROM-ISP"; the HC32F460 has no ROM ISP, the bootloader is flash-resident, inferred at `0x0..0xBFFF`) |
| IAPCommand | stm32isp fork, unstripped, ARM ELF, with debug info | `file` + `strings` |

## Hard-won facts (read before touching anything)

1. **Board revision = HD** (Hollystone HC32F460, marker `/root/H32`). App flash window
   **0x0000C000** (48 K). The bootloader answering the IAP sync is **flash-resident**
   (inferred `0x0..0xBFFF`) — it is *erasable*, so recovery depends on the safety
   invariant: **never write ≤ 0x00008000**. (Earlier drafts called this "ROM-ISP"; wrong —
   the HC32F460 has no ROM ISP.)
2. **Baud: use 115200, not the 250000 default.** Klipper's `CONFIG_SERIAL_BAUD` is gated behind
   `LOW_LEVEL_OPTIONS` (expert, default off). With it off, the vendored kconfiglib marks the
   symbol *invisible* and silently falls back to `default 250000` even though your value is
   stored — `set_value()` returns True, `autoconf.h` gets a stale mtime, nothing detects it.
   **Fix: set both `CONFIG_LOW_LEVEL_OPTIONS=y` AND `CONFIG_SERIAL_BAUD=115200`** in `.config`,
   run `make olddefconfig`, then **verify** `grep 'define CONFIG_SERIAL_BAUD ' out/autoconf.h`
   before flashing. (This silent fallback ate 3 days; full repro in `docs/KLIPPER-AD4-FLASH-GUIDE.md`.)
3. **Build on macOS:** `make -j8 CPP=arm-none-eabi-cpp` — the host `/usr/bin/cpp` is a clang
   wrapper that dies on the `.lds.S` linker-script step.
4. **Klipper↔SoC line = USART2, TX=PA9, RX=PA10** (FSEL36/37, proven from factory firmware
   in session 5; earlier drafts said PA15 — wrong).
5. **IAP sync** replies 1 byte: `0x07` = HD, `0x06` = NA. The MCU only answers after a fresh
   reset into its bootloader (GPIO34 NRST pulse; GPIO35 is driven too but its role is
   unproven); after many NRST pulses it can go deaf — a **physical power cycle** of the
   printer clears it.
6. The flasher is **fire-and-forget** in the data phase (byte-for-byte the vendor `IAPCommand`
   flow; the bootloader sends no per-frame ack the host consumes). Success = Klipper `identify`
   on `/dev/ttyS1` after reset.

## Unattended flash (no soldering, no RPi needed)

```
USB stick (FAT32, root):
  flashforge_init.sh   chmod +x
  screwflag            empty
  IAPCommand           factory binary OR built stm32isp-ad4
  klipper.hex          Klipper for HD window
  factory-HD.hex       restore image
  RESTORE              (optional: present = restore stock)
```
Insert, power on, SoC runs the script. Log → `/root/ad4-flash-status`.

## Flash (from the SoC, `root@192.168.10.10`)

```sh
cd /root
# build the flasher once:  gcc -O2 -o stm32isp-ad4 stm32isp-ad4.c
./stm32isp-ad4 -p /dev/ttyS1 klipper-115200-real.hex      # auto-detects HD from sync 0x07
# verify: probe the app, NOT the bootloader:
/opt/ad4-klipper/klip_probe_arm 115200 /dev/ttyS1 2000    # expect the ~1.5 KB Klipper identify
```

## Test the whole chain (SoC → bridge → Mac → klippy/Fluid)

1. SoC: `/opt/ad4-klipper/ad4-bridge 5000 /dev/ttyS1 115200` (kill any old 250000 instance first).
2. Mac: socat PTY → `tcp:192.168.10.10:5000`; `printer.cfg` serial = the PTY, **baud 115200**
   (must be a standard rate or the `IOSSIOSPEED` ioctl fails on the PTY; the SoC enforces the
   real rate).
3. Start klippy → `identify` handshake → Fluid.

## Recovery

**MCU** (always available, no hardware): `./flash-mcu.sh factory` on the SoC.
**SoC** (if Linux breaks): rewrite `FFAD4-BKUP.zip` eMMC image to `mmcblk0`.
The two are independent — MCU recovery never depends on SoC state and vice versa.

## Provenance

- Flasher ported from the factory `IAPCommand` (unstripped stm32isp fork, md5
  `919864f702cf3c07665234ab8de7896e`) + `control-2.3.0/run.sh`; wire format strace-verified.
- Klipper base: upstream `ce7002b` (2026-09-18). Board files are original work in this project.
- All `flash-images/` hexes validated: record checksums (two's complement), type-04 base 0x0000,
  data byte-identical to the built `klipper.bin`.
