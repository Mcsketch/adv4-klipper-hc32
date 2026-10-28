# AD4 HD board — full MCU flash dump via SWD

_Created 2026-10-20. **Executed on Caleb's NA unit 2026-10-27** (results in that section); before that, everything marked
PROVEN below comes from firmware RE / live strace; everything else is plan or
inference._

## 2026-10-27 — Caleb's unit is the **NA** board (Nation N32G455), not HD

Board photo (`Z:\_shared\20261027_191616.jpg`): silkscreen `FFP0187_Main_Board_V1.0.3 2021-8-28`;
**U6 = "Nation N32G455 REL7 HBB32001"** — LQFP64 Cortex-M4F (REL7 = 64 pin, 512 KB flash, LQFP,
−40..105 °C). Everything HC32F460-specific in this file (flash at `0x0`, app at `0xC000`, PSPCR
security bits, the `work/hd.bin` compare) does **not** apply to this unit. `docs/caleb-unit/*`
called the unit HD/H32 through 10-24; that was assumed, never measured, and is corrected.

### J16 — dedicated SWD header (no pad hunting)
White 4-pin JST-XH-style header **J16** beside U6, label printed next to each pin. `VCC` is the
pin nearest the `J16` text / C147 / TOP_FAN side; `GND` is nearest the `FFP0187_Main_Board` text.

| J16 pin | ST-Link clone (by its silkscreen label) |
|---|---|
| VCC | **leave unconnected** — the printer powers its own MCU |
| SWCLK | SWCLK |
| SWDIO | SWDIO |
| GND | GND |

Caleb's clone, by its silkscreen (2×5, odd pins one row, even pins the other): 1 RST, 2 SWCLK, 3 SWIM,
4 SWDIO, 5 GND, 6 GND, 7 3.3V, 8 3.3V, 9 5V, 10 5V. Used: **2 → SWCLK, 4 → SWDIO, 5 → GND**. Nothing on 1, 3, 7–10.

J16 carries no NRST, so the clone's `RST` wire has nowhere to go. Connect-under-reset fallback is
the SoC-side `gpio34` hold-low (see "Reset control").

### N32G455 memory map (replaces the HC32F460 one for this unit)
| Region | Address | Note |
|---|---|---|
| Main flash | `0x08000000..0x0807FFFF` (512 KB) | aliased at `0x0` when booted from flash |
| Vendor IAP bootloader | `0x08000000..0x0800FFFF` (64 KB, inferred) | the NA IAP hex starts at `0x08010000` |
| App | `0x08010000..` | NA 2.3.0 hex = 44640 B, SP `0x20003b78`, reset `0x080101ad` |
| SRAM | `0x20000000` (144 KB) | |
| Option bytes | `0x1FFFF800` | RDP byte `0xA5` = unprotected. **Never write**: clearing RDP = mass erase |
| ROM bootloader | system memory | N32G45x **has** an STM32-style ROM ISP (BOOT0 = SoC gpio35) — a recovery path the HC32 lacks |
| CPUID | `0xE000ED00` | expect `0x410FC24x` (Cortex-M4) |
| SW-DP IDCODE | — | `0x2BA01477`, same DP as the HC32F460, so `ad4-swd.cfg` keeps it |

Scripts: `work/swd/swd-probe.sh` (CPUID, DBGMCU id, vectors at `0x08000000`/`0x08010000`, option
bytes; every read in `catch`), `work/swd/swd-dump.sh` (512 KB from `0x08000000` + option bytes,
`0x08010000` region compared to `work/Adventurer4-NA-IAP-2.3.0-20230216.bin`, fill check).

### Executed 2026-10-27 — results (read-only, nothing written)
Run from the HP laptop: ST-Link V2 clone (`V2J27S6`, 0483:3748) on J16, forwarded into WSL with
`usbipd` (bus id 1-2), OpenOCD 0.12, 480 kHz, printer running its stock UI. Target voltage 3.27 V.

| Read | Value |
|---|---|
| CPUID `0xE000ED00` | `0x410FC241` (Cortex-M4 r0p1) |
| DBGMCU_IDCODE `0xE0042000` | `0x8F584511` |
| halted at | `pc 0x08015744`, `msp 0x20003b48` (inside the app) |
| vectors `0x08000000` | `20002908 080001ad 080007ff 080007f7` (vendor IAP bootloader) |
| vectors `0x08010000` | `20003b78 080101ad 08012749 08011d3d` (= NA 2.3.0 hex) |
| `0x00000000` alias | same as `0x08000000` |
| option bytes `0x1FFFF800` | `a5 5a ff ff ff…` → **RDP off, no read protection** |

Dump: `n32-flash-full-20261027-1942.bin` (512 KB, 9.5 s), sha256
`ad97e70503d7f53418387f3f007be60de971f15b9a7c05c8952527d1e420f8b1`, plus
`n32-optbytes-20261027-1942.bin` (16 B). Kept in `prodesk:~/ad4-backup/` (hash re-verified there)
and on the HP in `work/swd/`; not committed. Bytes `0x08010000..0x0801AE5F` are **byte-identical to the
vendor `Adventurer4-NA-IAP-2.3.0-20230216.hex`** (44640 B). Fill: 11137 × `0x00`, 416445 × `0xFF`.

Flash map from the dump:

| Range | Content |
|---|---|
| `0x08000000..0x080014D0` | vendor IAP bootloader, ~5.3 KB (Keil startup; app-base literal `0x08010000` at `0x08001374`; G-code strings `M132 X Y Z A B`, `G1 Z15 F400`, `X-41 Y-40 F30` at `0x0800146C`) |
| `0x08001800..0x0800FFFF` | **leftover Nations factory self-test firmware**, never erased (`HELLO NZ3601 FT/CP COS V1.09 … May 18 2020`, RTC/SD/USB/ADC/COMP tests, `ICC POWER OFF`, `dummby function`); dead code, nothing jumps there |
| `0x08010000..0x0801AE5F` | FlashForge app = NA IAP 2.3.0 |
| `0x0801AE60..0x0807FFFF` | erased (`0xFF`); no per-unit data region anywhere |

Still unknown: whether the bootloader validates the app before jumping (disassemble
`0x08000000..0x08001400`). Writing flash over SWD is **not** attempted and not yet possible with stock
OpenOCD: its `stm32f1x` driver identifies by device id and `0x511` is not in its table, so a restore
over SWD needs a small custom FPEC script or the vendor IAP path. The dump itself is the full-chip
backup those paths would restore from.

### Consequences for the Klipper plan
- The `klipper-fork` branch and the stick's `klipper.hex` (`@ 0x0000C000`) are **HC32F460 builds:
  wrong MCU for this unit. KLIPPERIZE stays untouched.** The NA build recipe is
  `adv4-n32-thermistor.md` §3 (mainline `src/stm32`, 64 KiB bootloader →
  `FLASH_APPLICATION_ADDRESS 0x08010000`); it names N32G452 — select **N32G455**.
- Stock restore for this unit = `Adventurer4-NA-IAP-2.3.0-20230216.hex`, not the HD hex.
  `flashforge_init.sh` ships only `factory-HD.hex`; swap it before any RESTORE.
- Upstream `src/stm32/stm32f1.c` disables JTAG only and leaves SW-DP enabled (verify in the
  checkout before relying on it); the `[output_pin p_PA13]/[p_PA14]` rule below still applies.

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
| SoC GPIO35 = MCU "BOOT0" | **NOT PROVEN** — IAPCommand touches gpio35 but its wiring to the MCU is unverified; HC32F460 has no BOOT0 pin at all. The N32G455 on the NA board does have BOOT0, so gpio35→BOOT0 is plausible there |
| Vendor bootloader occupies flash `0x0..0xBFFF` | **INFERRED** (app vectors at `0xC000`); the dump will confirm |
| Debug pins are PA13, PA14, PA15, PB3, PB4; PA13=SWDIO, PA14=SWCLK | **CONFIRMED** — three independent sources agree: mainline Klipper `src/hc32f460/main.c` comment, the `en_debug_port_t` enum (5 bits, `ALL_DBG_PIN=0x1F`) in `hc32f460_gpio.h`, and the HC32F460 Rev1.3 datasheet LQFP100 pin table |
| PA13/PA14 **physical** LQFP100 pin numbers | **UNCONFIRMED** — one search result puts PA13 at pin 72; the datasheet and ZLG schematic PDFs are raster scans with no text layer, so neither could be verified. Orient from the package pin-1 dot and count, or find the labelled test points |
| SWD (SWDIO=PA13, SWCLK=PA14) is enabled on the shipped board | **UNVERIFIED** — try it; connect-under-reset is the fallback |
| Debug pads exist / their location on the HD board | **UNKNOWN** — must be found on the PCB. **NA board (Caleb's unit): header J16, see the 2026-10-27 section** |

## Wiring — ST-Link V2 clone → HD board

> **Corrected 2026-10-24.** An earlier revision of this section gave pin
> *numbers* (SWDIO=7, SWCLK=9, NRST=15, 3.3V=1). **Those are the genuine
> ST-LINK/V2 20-pin pod pinout and do NOT apply to the USB-stick clones.**
> The clones carry a **2×5 (10-pin)** header with a different layout, and
> layouts vary between production runs of the same seller. Connect by the
> **silkscreen label**, never by pin position.

| ST-Link clone (label on the case) | HD board |
|---|---|
| `SWDIO` | HC32F460 **PA13** (find the pad/via near the MCU; PA13/PA14 usually fan out to a test-point cluster) |
| `SWCLK` | HC32F460 **PA14** |
| `GND` | board GND |
| `RST` / `NRST` — optional | MCU NRST net (same net the SoC drives via GPIO34) |
| `3.3V` — **leave unconnected** | power the board normally; common GND only |
| `5V` — **leave unconnected** | — |

**Three wires only: SWDIO, SWCLK, GND.** The printer powers its own MCU.
Back-powering a running board through the programmer is the most likely way to
damage it, and these clones expose both 3.3 V and 5 V on the same header.

Before the first connection, confirm the clone's own pinout with a multimeter
in continuity mode — the labels are reliable, the pin *order* is not. Identify
board GND against a known ground (USB shield, mounting screw) the same way.

All signals are 3.3 V logic — a stock ST-Link V2 clone is fine. Keep wires
short (< 15 cm); SWD at default 1 MHz (drop `adapter speed` to 100–500 kHz if
clones are flaky).

## Reset control

Two options, either works:

1. **ST-Link owns reset**: wire the clone's `RST`/`NRST` pin and use
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

## Keeping SWD alive past the first Klipper flash

Mainline Klipper's `armcm_main()` carries a comment the rest of this document
should be read against:

```c
// disable JTAG/SWD on pins PA13, PA14, PA15, PB3, PB4
// SWD still works until the relevant pins are reconfigured. Proprietary
// flash program (XHSC ISP) must be used to reflash afterwards.
PORT_DebugPortSetting(ALL_DBG_PIN, Disable);
```

`PORT_DebugPortSetting(..., Disable)` clears the low 5 bits of `M4_PORT->PSPCR`
(see `hc32f460_gpio.c`), which releases the debug *function* — but per the
upstream comment the pins keep answering **until something reconfigures them as
GPIO**.

The fork's draft `printer.cfg` does exactly that:

```
[output_pin p_PA13]
[output_pin p_PA14]
```

Those two sections are what would finally take SWDIO/SWCLK away. **Delete them
from any config flashed to this unit** unless there is a demonstrated need for
two more GPIOs — it costs nothing and may preserve debug access after Stage 2.

This is *not* a reason to skip the pre-flash dump. PSPCR is still cleared on
every boot, the pins' behaviour in that state is untested on this silicon, and
the fallback (connect-under-reset) remains unproven on this board. Treat it as
insurance on top of the dump, not a replacement for it.

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
