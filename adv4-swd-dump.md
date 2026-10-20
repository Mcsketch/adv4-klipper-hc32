# AD4 HD board — full MCU flash dump via SWD

_Created 2026-10-20. **Method designed, NOT yet executed** — everything marked
PROVEN below comes from firmware RE / live strace; everything else is plan or
inference._

## Why SWD is the only dump path today

- The vendor IAP bootloader (the thing `IAPCommand` / `stm32isp-ad4` talk to on
  `/dev/ttyS1`) speaks a **proprietary 1030-byte frame protocol**
  (`[cmd][len:2LE][data:1024][crc16-ccitt]`, cmd `0x01`=data, `0x04`=final,
  ack `0x06`, sync `'C'`→`0x07`). **No read-back command has been decoded.**
- `stm32isp-ad4 -d` sends STM32 ROM-ISP `CMD_READ (0x11)`, which this
  bootloader does not understand → every read times out → the tool still
  writes the zero-filled buffer, producing the 256 KB of `00` in
  `mcu-dump-full.bin`. **That file is worthless; the ISP dump path cannot work
  until a read command is decoded.**
- The HC32F460 has **no ROM-resident ISP** (unlike STM32F1). The "ISP" is a
  **flash-resident vendor bootloader**, almost certainly at
  `0x00000000..0x0000BFFF` (inference: the factory app's vectors live at
  `0xC000`). Dumping that region over SWD is what unlocks a future ISP read
  implementation.

## What is PROVEN vs INFERRED

| Fact | Status |
|---|---|
| SoC↔MCU link: USART2, TX=PA9 (FSEL 36), RX=PA10 (FSEL 37), 115200, DMA RX | **PROVEN** from factory app disasm (`0x14f64` descriptor: baud literal `0x1c200` at `0x14f74`, consumed by `USART_SetBaudRate` at `0x15144`; FSEL map from HDSC HAL `hc32f460_gpio.h`) |
| SoC GPIO34 pulses MCU NRST (15.68 ms low/high) | **PROVEN** (IAPCommand strace + sysfs value) |
| SoC GPIO35 = MCU "BOOT0" | **NOT PROVEN** — IAPCommand touches gpio35 but its wiring to the MCU is unverified; HC32F460 has no BOOT0 pin at all |
| Vendor bootloader occupies flash `0x0..0xBFFF` | **INFERRED** (app vectors at `0xC000`); the dump will confirm |
| SWD (SWDIO=PA13, SWCLK=PA14) is enabled on the shipped board | **UNVERIFIED** — try it; connect-under-reset is the fallback |
| Debug pads exist / their location on the HD board | **UNKNOWN** — must be found on the PCB |

## Wiring — ST-Link V2 clone → HD board

| ST-Link (2×10 2.54 mm header) | HD board |
|---|---|
| SWDIO (pin 7) | HC32F460 **PA13** (find the pad/via near the MCU; PA13/PA14 usually fan out to a test-point cluster) |
| SWCLK (pin 9) | HC32F460 **PA14** |
| GND (pins 2/4/6/8/10…) | board GND |
| NRST (pin 15) — optional | MCU NRST net (same net the SoC drives via GPIO34) |
| 3.3V (pin 1) — **leave unconnected** | power the board normally; common GND only |

All signals are 3.3 V logic — a stock ST-Link V2 clone is fine. Keep wires
short (< 15 cm); SWD at default 1 MHz (drop `adapter speed` to 100–500 kHz if
clones are flaky).

## Reset control

Two options, either works:

1. **ST-Link owns reset**: wire NRST (pin 15) and use
   `reset_config srst_nogate` / `reset_config srst_connect`.
2. **SoC owns reset** (no extra wire to the MCU): from the SoC,
   ```sh
   echo 34 > /sys/class/gpio/export 2>/dev/null
   echo out > /sys/class/gpio/gpio34/direction
   echo 0 > /sys/class/gpio/gpio34/value   # hold MCU in reset
   # ...start OpenOCD `init` here, then:
   echo 1 > /sys/class/gpio/gpio34/value   # release
   ```
   Hold-low + `init` = connect-under-reset, which beats any firmware that
   disables SWD during app init (the ~ms window before the app runs is yours).

## OpenOCD

There is no stock `hc32f460` target file; a **generic Cortex-M target is enough
for reads** (no flash algorithm needed to `dump_image` from a memory-mapped
flash region):

```sh
openocd -f interface/stlink.cfg -c "transport select swd" \
        -f target/cortex_m.cfg -c "reset_config srst_nogate"
```
then in the telnet/GDB server:
```
init
halt
dump_image hd-flash-full.bin 0x00000000 0x80000
```
(`0x80000` = 512 KB, the full HC32F460 flash.)

If plain attach fails, retry with `-c "reset_config srst_connect"` (assert
reset as part of `init`) or the SoC-GPIO34 hold above.

Alternative tool: `pyocd read 0x0 0x80000 hd-flash-full.bin -t cortex_m`
(generic target; plain memory reads need no flash pack).

## HC32F460 security bits — read before touching

HC32F460 has flash **security bits** (read protection). If SWD attach fails or
reads return all-`00`/all-`FF`, suspect them. **Do NOT "unlock"**: unlocking is
a **mass erase** and destroys the only copy of the vendor bootloader. If
security is on, the dump must come from decoding the vendor ISP protocol
instead — and that is exactly what this dump was supposed to enable, so
preserve the chip.

## Verifying the dump

1. `0x00000000`: a vector table (SP in `0x2000xxxx`/`0x1FFFxxxx`, then a Thumb
   PC) → vendor bootloader present.
2. `0x0000C000`: must match the known factory app (`work/hd.bin`, the 64 KB
   dump already analyzed) — byte-compare `dd if=hd-flash-full.bin bs=1 skip=49152 count=65536 | cmp - work/hd.bin`.
3. Anything between `0x20000..0x7FFFF` is the rest of the factory image
   (config/params regions?).

## What the dump buys us

- **Vendor bootloader code** (`0x0..0xBFFF`, inferred): RE it for a flash-read
  command → ISP-based dumps without opening the printer; and the exact
  boot-decision logic (what it checks instead of a BOOT0 pin — likely a GPIO
  sample or a flash flag) → resolves the GPIO35 question.
- **How it hands off to the app** (VTOR write + jump to `0xC000`?): this is a
  prime suspect for why our klipper image at `0xC000` is silent — if the
  bootloader jumps without setting VTOR, or expects an app header/signature,
  a plain linked-at-`0xC000` image will hang before `main`.
- A **known-good restore image** for the whole chip, not just the app region.
