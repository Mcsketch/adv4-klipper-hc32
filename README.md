# Flashforge Adventurer 4 → Klipper

The Adventurer 4's main board (Hollystone **HC32F460**, "HD" revision) now runs
**Klipper** — no soldering, no extra hardware. The printer's own Allwinner V3S
SoC is reused as the serial bridge and the flasher, over the stock wiring.

This repo holds the whole port: the Klipper MCU build files, the SoC-side
bridge + flashing tools, the factory-firmware reverse-engineering that settled
the pin map, and the USB-stick install path.

## Status — 2026-10-22

**The printer is running Klipper right now.** Fluidd: `http://127.0.0.1:8081/`
(strata keeps :8080). Proven live this session:

| Test | Result |
|---|---|
| Hotend heat (M104 S60) | 15 → **60.2 °C**, PID settled at 3.4 % power |
| Bed heat (M140 S60) | 15 → **36.6 °C in 60 s** at full power |
| Hotend fan (PC2) | auto-starts above 50 °C |
| Reflash cycle | vendor `IAPCommand` over `/dev/ttyS1`, MCU `identify` answers after every flash |

Pin map below is **proven by disassembling the factory firmware**
(`control-2.3.0`) and then confirmed empirically — earlier guesses corrected
in `adv4-exploration.md`.

**Still open:** stepper/endstop pins (traced, unverified), TMC UART pins,
part-cooling fan pin, PID re-tune, `stm32isp` rewrite (write+dump for ST-Link
restore), SoC boot persistence (bridge doesn't auto-start after power-cycle).

## Quick start (printer already flashed)

```sh
# Mac — starts klippy + moonraker + Fluidd, idempotent
/Users/admin/adv4-klip/bridge/ad4-mac-start.sh
open http://127.0.0.1:8081/

# raw gcode without the web UI:
printf 'M105\n' > ~/adv4/links/gcode

/Users/admin/adv4-klip/bridge/ad4-mac-stop.sh
```

After a printer **power-cycle** the SoC bridge must be restarted:

```sh
sshpass -p flashforge ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
  'cd /opt/ad4-klipper && nohup ./ad4-bridge 5000 /dev/ttyS1 115200 >/dev/null 2>&1 &'
```

## Reflash the MCU (full cycle)

```sh
cd /Users/admin/klipper-hc32
export PATH="/Users/admin/toolchains/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH"
make -j4 && arm-none-eabi-objcopy -O ihex out/klipper.elf out/klipper-ad4.hex

/Users/admin/adv4-klip/bridge/ad4-mac-stop.sh          # frees ttyS1 (bridge is single-client)
sshpass -p flashforge scp -O out/klipper-ad4.hex root@192.168.10.10:/opt/ad4-klipper/
sshpass -p flashforge ssh root@192.168.10.10 \
  'cd /opt/ad4-klipper && ./IAPCommand klipper-ad4.hex klipper-ad4.hex /dev/ttyS1 115200'
# reset + liveness:
sshpass -p flashforge ssh root@192.168.10.10 \
  'echo 0 > /sys/class/gpio/gpio34/value; sleep 0.3; echo 1 > /sys/class/gpio/gpio34/value; cd /opt/ad4-klipper && ./ad4_identify 115200 /dev/ttyS1'

/Users/admin/adv4-klip/bridge/ad4-mac-start.sh
```

Notes: `IAPCommand` takes **two** hex args (NA + HD images — pass the same
file twice). The bootloader at `0x00000..0x0BFFF` is flash-resident — never
write below `0x0000C000`. Our `stm32isp-ad4` port currently **fake-succeeds**;
the vendor tool is the working flasher until it is rewritten. Full details:
`KLIPPER-AD4-FLASH-GUIDE.md`.

## Pin map (PROVEN 2026-10-22: firmware RE + live tests)

| Function | Pin | Evidence |
|---|---|---|
| Hotend heater | **PA1** | TMRA2 Ch2 (AF4) in fw; fw clamps 250/300 °C; heated to 60 °C live |
| Hotend NTC | **PA0** | ADC1 CH0; fw converts `R = 4700*adc/(4095-adc)` |
| Bed heater | **PA7** | TMRA1 Ch5 (AF4); fw clamps 200/130 °C; heated live |
| Bed NTC | **PA5** | ADC1 CH5; fw LUT keyed on raw ADC |
| Hotend fan | **PC2** | spins up >50 °C (confirmed twice) |
| Chamber fan | **PB9** | TMRA4 in fw |
| SoC↔MCU serial | **PA9/PA10** USART2 @115200 | fw link init + live traffic |
| NOT usable | PC0 (not ADC), PC10 (USART1_TX @9600, not TMC), PA13/PA14 (SWD — kept free for the ST-Link dump) | fw RE |

Thermistors: both 100 k NTC on a 4.7 k pull-up; curves extracted from the
factory LUTs into `printer.cfg` (`ad4_hotend`, `ad4_bed`). One hotend curve
covers both the 240 °C and 265 °C hotends — the fw's second hotend table is
vestigial. ADC is right-aligned 12-bit (`& 0xFFF`).

Steppers/endstops in `printer.cfg` are **traced guesses** — not yet verified.

## Topology

```
Mac (klippy, moonraker :7125, Fluidd :8081)
  └─ TCP ─→ SoC 192.168.10.10 (ad4-bridge :5000, IAPCommand, NRST=gpio34)
                └─ /dev/ttyS1 UART ─→ MCU HC32F460 (Klipper, app @0x0000C000)
```

klippy `[mcu] serial: socket://192.168.10.10:5000` — direct TCP, no socat PTY
(it raced on reconnect). Klipper is patched with `serial_for_url` + fd-type
`'s'` so `socket://` works.

## Repo layout

| Path | What |
|---|---|
| `bridge/` | **current** SoC + Mac scripts: `ad4-mac-start.sh` / `ad4-mac-stop.sh`, `ad4-fluidd-proxy.py`, `ad4-bridge.c`, `ad4_identify.c`, probes |
| `stm32isp-ad4/` | our flasher, source for the vendor `IAPCommand` protocol: writes, verifies, and probes the chip (`-s`); what the stick ships |
| `new-usb-flash/` | USB-stick unattended flash + persistent eMMC install path (panel UI, bridge, buzzer) |
| `klipper-port/` | the Klipper board files (also live in `/Users/admin/klipper-hc32`) |
| `flash-images/` | validated Intel-HEX images |
| `work/` | factory binaries, straces, decoded images (RE evidence) |

## Documents

| File | Purpose |
|---|---|
| `STATUS.md` | current state, what's proven vs pending |
| `GETTING-STARTED.md` | the working day-to-day path (Mac stack) |
| `FIRST-RUN-MANUAL.md` | first-time bring-up from a stock printer |
| `KLIPPER-AD4-FLASH-GUIDE.md` | flashing deep-dive (IAPCommand, GPIO, windows) |
| `adv4-exploration.md` | the firmware RE: how the pin map + LUTs were derived |
| `adv4-soc-hack.md` | SoC architecture: bridge, network, recovery |
| `adv4-iap-protocol.md` | vendor IAP wire format |
| `adv4-swd-dump.md` | ST-Link plan to dump/write MCU flash (pending) |
| `adv4-n32-thermistor.md` | NA/N32 variant track (superseded for HD) |
| `adv4-touch-ui.md` | driving Klipper from the stock touchscreen (plan) |
| `PERSISTENT-INSTALL.md` | surviving reboots on the SoC eMMC (pending) |
| `ALL-FROM-PI.md` | historical RPi workflow (superseded by the Mac stack) |

## Hard-won facts

1. **Baud 115200 only.** SoC uart0 clock is 24 MHz → `setbaud` rejected; the
   line stays at boot-rate 115200. And Klipper's `CONFIG_SERIAL_BAUD` silently
   falls back to 250000 unless `CONFIG_LOW_LEVEL_OPTIONS=y` — always verify
   `out/autoconf.h` before flashing.
2. **Build with the xpack toolchain** (`/Users/admin/toolchains/
   xpack-arm-none-eabi-gcc-14.2.1-1.1`); Homebrew GCC 16.2.0 is broken here.
3. **`IAPCommand` needs two hex args** (NA + HD). Sync reply: `0x07` = HD,
   `0x06` = NA. GPIO34 = NRST (proven); GPIO35 is driven by the vendor tool,
   its role unproven.
4. **MCU recovery without solder:** NRST pulse via `/sys/class/gpio/gpio34`,
   then re-flash; `ad4_identify` is the ground-truth liveness check. A hard
   stuck MCU (no sync at all) clears with a printer power cycle.
5. **klippy.log appends** across restarts — check the tail timestamp before
   trusting a "recent" line.
6. A stale symlink to a dead PTY at `~/adv4/links/gcode` silently swallows
   every gcode command — it must be a real FIFO with the holder process
   (managed by `ad4-mac-start.sh`).

## Recovery

- **MCU bad flash:** re-run the flash cycle (bootloader is never touched).
- **MCU silent:** NRST pulse → `ad4_identify`; if still dead, power-cycle the
  printer, restart the bridge, retry.
- **Back to stock MCU:** `stm32isp-ad4` with the factory hexes in
  `stm32isp-ad4/` (or the USB stick with a `RESTORE` flag — see `new-usb-flash/`).
- **SoC broken:** rewrite the `FFAD4-BKUP.zip` eMMC image.

## Provenance

Flasher ported from the factory `IAPCommand` (unstripped stm32isp fork).
Klipper base: upstream `ce7002b`. Pin map + thermistor LUTs: original RE of
`control-2.3.0` HD firmware in this repo (`/tmp/heater-re/` artifacts).
GitHub: `https://github.com/berthy456/adv4-klipper-hc32` (main = this repo,
`klipper-fork` = klipper tree snapshot).
