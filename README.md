# Flashforge Adventurer 4 (AD4) → Klipper

Klipper support for the Flashforge Adventurer 4 / Adventurer 4 Pro **HC32F460 (Hollystone HD)**
MCU — the open-source MCU flasher, the SoC↔MCU serial bridge, and the complete Klipper board
port. Self-documented: every artifact here was built, tested, and grounded against the factory
firmware.

## Layout

| Path | What |
|---|---|
| `flasher/` | `stm32isp-ad4.c` — self-contained stm32isp fork with the AD4 SoC GPIO strapping (NRST=GPIO34, BOOT0=GPIO35) + the HC32 vendor 1030-byte IAP packet protocol. `flash-mcu.sh` — unattended flash runbook. |
| `bridge/` | SoC-side userland: `ad4-bridge` (C, MCUs UART → TCP 5000), `ad4-serial-bridge.py`, `klip_probe.c` (Klipper identify liveness probe), `rom_sweep.c`, `ad4-identify-probe.py`, plus Mac-side Fluid/klippy setup scripts. |
| `klipper-port/` | The Klipper board files: `src-hc32f460/` (main/serial/gpio/pwm/adc/Kconfig/Makefile), `lib-hc32f460/` (vendor driver), `board.config` (working `.config`), `make-positioned-ihex.py`. |
| `flash-images/` | Validated Intel-HEX ready to flash (checksums verified). |
| `docs/` | Flash guide, IAP protocol write-up, STATUS. |

## Hard-won facts (read before touching anything)

1. **Board revision = HD** (Hollystone HC32F460, marker `/root/H32`). App flash window
   **0x0000C000** (48 K). ROM-ISP at 0x0 is never written → **every state is recoverable**.
2. **Baud: use 115200, not the 250000 default.** Klipper's `CONFIG_SERIAL_BAUD` is gated behind
   `LOW_LEVEL_OPTIONS` (expert, default off). With it off, the vendored kconfiglib marks the
   symbol *invisible* and silently falls back to `default 250000` even though your value is
   stored — `set_value()` returns True, `autoconf.h` gets a stale mtime, nothing detects it.
   **Fix: set both `CONFIG_LOW_LEVEL_OPTIONS=y` AND `CONFIG_SERIAL_BAUD=115200`** in `.config`,
   run `make olddefconfig`, then **verify** `grep 'define CONFIG_SERIAL_BAUD ' out/autoconf.h`
   before flashing. (This silent fallback ate 3 days; full repro in `docs/KLIPPER-AD4-FLASH-GUIDE.md`.)
3. **Build on macOS:** `make -j8 CPP=arm-none-eabi-cpp` — the host `/usr/bin/cpp` is a clang
   wrapper that dies on the `.lds.S` linker-script step.
4. **Klipper↔SoC line = USART2, TX=PA9, RX=PA15** (FSEL36/37, datasheet Table 2-2). Config
   symbol `HC32F460_SERIAL_PA9_PA15`.
5. **ROM-ISP sync** replies 1 byte: `0x07` = HD, `0x06` = NA. The MCU only answers after a fresh
   reset into ISP (BOOT0 high + NRST pulse); after many NRST pulses it can go deaf — a **physical
   power cycle** of the printer clears it.
6. The flasher is **fire-and-forget** in the data phase (byte-for-byte the vendor `IAPCommand`
   flow; the ROM sends no per-frame ack the host consumes). Success = Klipper `identify` on
   `/dev/ttyS1` after reset.

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

## Provenance

- Flasher ported from the factory `IAPCommand` (unstripped stm32isp fork, md5
  `919864f702cf3c07665234ab8de7896e`) + `control-2.3.0/run.sh`; wire format strace-verified.
- Klipper base: upstream `ce7002b` (2026-09-18). Board files are original work in this project.
- All `flash-images/` hexes validated: record checksums (two's complement), type-04 base 0x0000,
  data byte-identical to the built `klipper.bin`.
