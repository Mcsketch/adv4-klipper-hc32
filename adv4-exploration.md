# Flashforge Adventurer 4 — Firmware Exploration Report

Facts extracted from the official firmware package (V2.2.4 / control 2.3.0, build
`20230216`) and the eMMC backup. Target board: **HC32F460 (HD revision)**.

> **SETTLED 2026-10-22 — heater/sensor map (PROVEN by firmware RE + live heating):**
> hotend heater **PA1** (TMRA2 Ch2, fw clamps 250/300C) · bed heater **PA7**
> (TMRA1 Ch5, fw clamps 200/130C) · hotend NTC **PA0** (ADC1 CH0) · bed NTC
> **PA5** (ADC1 CH5) · hotend fan **PC2** (spins >50C) · chamber fan **PB9**
> (TMRA4). ADC DR is **right-aligned 12-bit** (`& 0xFFF`; the old `>>4` was
> wrong). **PC10 = USART1_TX @9600, NOT a TMC UART.** PC0 is NOT an ADC input
> in the factory design. Stepper/endstop pins remain TRACED GUESSES.
> Live proof: M140 S60 → bed 14.9→36.6C in 60s at power 1.0; M104 S60 →
> hotend 60.17C, PID settled 3.4%; both sensors read ~14.5C at ~13C room.
> RE artifacts: `/tmp/heater-re/` (hd.bin, hd.asm, luts.txt); asm addr
> − 0x08000000 = runtime, − 0x0800C000 = file offset.

---

## 1. System architecture

| Layer | Hardware | Software |
|---|---|---|
| Motion / thermistor / driver MCU | **HC32F460** (HDSC / Huatong, Cortex-M4), HD rev | Proprietary bare-metal app, vendor BSP |
| Host / UI / G-code parser | Sunxi SoC (armv7l), kernel `3.4.39+` | Qt/C++ app `adventurer4-arm` v2.2.4, launched by a Go supervisor `ffstartup-arm` |

- The **MCU has no G-code engine**. It receives motion/temperature commands over serial
  (`/dev/ttyS1` at 115200 on the SoC) and runs a state machine plus embedded power-loss G-code.
- The **SoC owns**: G-code parsing, UI, file management, the nozzle-calibration model,
  and the MCU's **reset GPIO strapping** (GPIO34=NRST proven; GPIO35 = IAP-mode
  strap, proven live on the NA unit 2026-10-27, see adv4-iap-protocol.md) — required for flashing.
- Neither binary contains Marlin, Repetier, or any third-party firmware signature
  (verified by string scan: 0 hits).

### 1.1 MCU firmware identification

- Built on **HDSC's official HC32F460 standard peripheral driver library**. Source paths
  embedded in the binary (`hd.bin`, 60 416 bytes, load base `0x0800C000`):

```
source\driver\src\hc32f460_adc.c
source\driver\src\hc32f460_clk.c
source\driver\src\hc32f460_dmac.c
source\driver\src\hc32f460_efm.c
source\driver\src\hc32f460_exint_nmi_swi.c
source\driver\src\hc32f460_gpio.c
source\driver\src\hc32f460_interrupts.c
source\driver\src\hc32f460_pwc.c
source\driver\src\hc32f460_timer0.c
source\driver\src\hc32f460_timera.c
source\driver\src\hc32f460_usart.c
source\driver\src\hc32f460_wdt.c
```

- No RTOS, no G-code parser, no company/developer signature inside the MCU image.
- The vendor (Flashforge) signs the **SoC** layer: `"Flashforge Adventurer 4"`,
  `"Machine Type: Flashforge Adventurer 4"`, `support@flashforge.com`, version `2.2.4 20230216`.
- SoC app: **Qt (4.8.6-era toolchain), armv7l, glibc 2.19, dynamically linked, unstripped
  with full debug info** — its complete symbol table is readable (class names listed in §6).

---

## 2. Flash memory map (HD and NA revisions)

Base addresses were read directly from the two factory hexes (their type-04
extended-address records), not assumed:

| Region | Address | Content |
|---|---|---|
| Vendor bootloader ("ISP") | `0x00000000`–`0x0000BFFF`? | **Correction 2026-10-20: NOT ROM-resident.** The HC32F460 has no STM32-style ROM ISP — this is a **flash-resident vendor bootloader**; its extent is *inferred* from the app vectors at `0xC000` (confirm via SWD dump, `adv4-swd-dump.md`). Never written by us. The entry condition "NRST pulse with BOOT0 HIGH" is **unverified** — the HC32F460 has no BOOT0 pin; the bootloader decides, likely from a GPIO sample or flash flag |
| Application (**HD / H32**) | **`0x0000C000`** | `Adventurer4-HD-IAP-2.3.0-20230216.hex`, 60 416 B |
| Application (**NA / N32**) | **`0x08010000`** | `Adventurer4-NA-IAP-2.3.0-20230216.hex`, 44 640 B |

The two revisions live at **different base addresses** (HD low, NA high); the
flasher selects the right one from the sync reply. **Correction 2026-10-20:
the reply map is `0x06` = NA, `0x07` = HD** — live-confirmed (the HD board
answers `0x07`); earlier text here had it swapped.

Rules:

- **Write only the app window for the detected revision** (HD `0x0000C000`, NA
  `0x08010000`). Never `0x00000000`. The flasher hard-refuses any image base at
  or below `0x00008000`, so it can never clobber the reset/ISP region.
- A corrupted app is **expected to be recoverable**: the ISP bootloader is
  flash-resident at `0x0..0xBFFF` (inferred) and untouched by app-window
  writes, so the next reset (+ whatever GPIO35 does) re-enters it and the
  factory image can be re-flashed in one command. **Caveat: this recovery has
  been observed to fail after repeated flashing** (the "no sync" state in
  STATUS.md §3) — the exact re-entry guarantee is not proven.
- Klipper hex for the HD (H32) board: build the firmware for the same window the HD
  image uses and re-encode the start:
  `arm-none-eabi-objcopy -I binary -O ihex --change-address=0xC000 klipper.bin klipper.hex`.
  Verify the result's base (the flasher prints it) matches the HD window before flashing.

### 2.1 ISP entry — the GPIO dance (decoded from `IAPCommand`)

`IAPCommand` is a fork of `stm32isp`. Its `main()` sequence:

```text
stm32Unexport()                     # reset sysfs GPIO state
port = /dev/ttyS1 ; baud = 115200
gpio_out(35, 1)                     # "ISP-select" = HIGH (SoC GPIO 35) —
                                    #   role UNPROVEN: HC32F460 has no BOOT0 pin;
                                    #   whether GPIO35 even reaches the MCU is unverified
sleep(1)                            # hold 1 s
r = stm32Init(port, baud)
      └─ stm32Sync loop:
           send 0x43 ('C'), read reply
           reply 6  => NA revision, connected   # (corrected: earlier text had 6=HD)
           reply 7  => HD revision, connected   # live-confirmed: HD board answers 0x07
           no reply => gpio_out(34,0); usleep(15680);
                       gpio_out(34,1); usleep(15680);   # NRST pulse, SoC GPIO 34
                       retry until timeout
           ("Can not connect to board, Serial port ... timed-out." = strapping failed)
if r == 6:  flash NA hex            # (corrected mapping: 6=NA, 7=HD)
if r == 7:  flash HD hex
# after a verified write:
gpio_out(35, 0)                     # clear "ISP-select" -> normal app boot (role unproven)
touch /root/<status-flag>           # HD or NA flag file
sync()
```

Exact `stm32Reboot()` (exit ISP, run new app):

```text
gpio_out(34, 0)                     # assert NRST (LOW)
usleep(15680)                       # 15.68 ms
gpio_out(34, 1)                     # release NRST (HIGH)
usleep(15680)
```

Key facts:

- IAPCommand holds **GPIO 35 HIGH before/during the NRST pulse (GPIO 34)** —
  that is what the binary does (proven). **Interpreting GPIO35 as "BOOT0" is
  NOT proven**: the HC32F460 has no BOOT0 pin, and the GPIO35→MCU connection
  has never been traced. NRST pulse width: **15.68 ms LOW, then 15.68 ms
  HIGH** (proven from the binary). The 1 s hold is what the binary does;
  whether it is *required* was never A/B-tested.
- Serial sync byte `0x43`; reply **`6` (NA) / `7` (HD)** — corrected mapping,
  live-confirmed — also performs **revision auto-detection**, which is why the
  factory `run.sh` passes **both** hex files:
  `IAPCommand Adventurer4-NA-IAP-….hex Adventurer4-HD-IAP-….hex`.
- The GPIOs are driven via Linux sysfs (`/sys/class/gpio`), which is why the tool must run
  **on the SoC as root** — a generic `stm32isp` on another board has no access to these lines
  and times out.

---

## 3. Verified pins

### 3.1 HC32F460 ADC capability (from the chip HAL `adc.c`)

- Valid ADC pins: **PA0–PA7, PB0, PB1, PC0–PC5** (16 channels).
- **PB7 is NOT an ADC pin** — the stock-template `sensor_pin: PB7` (hotend) cannot work on this
  chip; Klipper aborts with "Not a valid ADC pin".

### 3.2 Klipper port serial pin options (from `src/hc32f460/Kconfig`)

| Option | Pins | Status in current config |
|---|---|---|
| `HC32F460_SERIAL_PA7_PA8` | PA7 / PA8 | **selected — CONFLICTS with X `step_pin: PA8`** |
| `HC32F460_SERIAL_PA13_PA14` | PA13 / PA14 | **HALLUCINATION — do not use** (added by a previous agent claiming the vendor link/ROM ISP lives on the JTAG pins; false: PA13/PA14 are SWDIO/SWCLK with **no USART alternate function**, and the real vendor link is USART2 on PA9/PA10 — see note below). Its hex was silent, as expected |
| `HC32F460_SERIAL_PA3_PA2` | PA3 / PA2 | **free — recommended** |
| `HC32F460_SERIAL_PH2_PB10` | PH2 / PB10 | PB10 = TMC mux select A — no |
| `HC32F460_SERIAL_PA15_PA9` | PA15 / PA9 | PA15 = Z step — no |
| `HC32F460_SERIAL_PC0_PC1` | PC0 / PC1 | PC1 = Y endstop — no |
| `HC32F460_SERIAL_PA9_PA10` | PA9 / PA10 | **the PROVEN vendor SoC↔MCU line** (session 5 firmware RE: USART2, TX=PA9/FSEL 36, RX=PA10/FSEL 37, 115200, DMA RX). Our klipper-hc32 fork adds this option; `out/autoconf.h` confirms the flashed hex used it. PA9/PA10 are otherwise unused on the board, so this is the correct choice for talking to the SoC without new wiring |

Port clock: `CONFIG_CLOCK_FREQ=200000000` (200 MHz). `CONFIG_WANT_TMCUART=y` (TMC2209-over-UART
supported by the port, `tmcuart.c`). The port does **not** implement `temperature_mcu`.

### 3.3 Board pins (traced on the physical mainboard; heater/sensor rows now
corrected by firmware RE + live tests — the trace alone was NOT authoritative)

| Function | Pin | Note |
|---|---|---|
| X step / dir / enable | PA8 / !PC9 / !PA11 | |
| Y step / dir / enable | PB14 / !PB13 / !PB15 | |
| Z step / dir / enable | PA15 / !PA12 / !PC11 | |
| E step / dir / enable | PC7 / PC6 / !PC8 | |
| X endstop | ^PC3 | |
| Y endstop | ^PC1 | |
| Z endstop | !PB5 | |
| Hotend heater | PA1 (PROVEN) | fw RE: PORT_SetFunc AF4 @0x800f1e2 → TMRA2 Ch2; fw clamps 250/300C. Live: M104 S60 reached 60.17C. was: PA1 "traced" — now confirmed |
| Bed heater | **PA7** (PROVEN) | fw RE: AF4 @0x800f0d6 → TMRA1 Ch5; fw clamps 200/130C. Live: M140 S60 → 14.9→36.6C in 60s. **was: PC4 — disproven by fw RE** |
| Hotend thermistor | **PA0** (PROVEN) | ADC1 CH0; fw converts R = 4700·adc/(4095−adc) — byte-for-byte Klipper's Thermistor formula → pullup 4700 correct. **was: "bed thermistor" — wrong** |
| Bed thermistor | **PA5** (PROVEN) | ADC1 CH5; bed LUT keyed on RAW ADC (no ohm conversion). **was: "part fan PA5" — wrong** |
| Hotend fan | PC2 (PROVEN) | `heater_fan`, auto-starts >50C (observed live); it is the heatsink fan (NA unit, 2026-10-28) |
| Part fan | **unknown — probe** | **was: PA5 — that is the bed NTC.** Not PB9 either: PB9 is the chamber fan (2026-10-28, below) |
| Controller fan | PC5 | GUESS (traced) |
| Chamber fan | PB9 (PROVEN) | fw RE: AF4 @0x8011036 → TMRA4. `M106 S255` on PB9 spun the chamber fan (NA unit, same PCB, 2026-10-28) |
| LED | **unknown — probe** | stock template: PB12 (unconfirmed) |
| TMC2209 UART (shared) | ~~PC10~~ **unknown** | **DISPROVEN 2026-10-22:** fw RE shows PC10 = AF32 USART1_TX @9600 (@0x8016c9c), not a TMC UART. Real TMC config pins unknown; tmc2209 blocks disabled in printer.cfg |
| Mux select A / B | PB10 / PB1 | GUESS — tied to the disproven PC10 mux theory |
| Mux channel map | X = ch0, E = ch1, Y = ch2, Z = ch3 | GUESS — same |
| MCU ↔ SoC UART | SoC `/dev/ttyS1` @ 115200 | uses MCU USART0; becomes **inactive for Klipper** after the serial move to PA3/PA2 |

---

## 4. Temperature system

### 4.1 MCU side — four sensor curve descriptors

> **Chip scope — read this first.** The offsets in this §4.1 are for the **HD** image
> (HC32F460 / H32, app base `0x0800C000`, `Adventurer4-HD-IAP-…hex`). The **NA** chip
> (Nation N32G45x, app base `0x08010000`) is a *different* MCU whose LUTs live at
> *different* offsets — see `adv4-n32-thermistor.md`. The two mappings are **not**
> contradictory; they are two chips. Do not cross-apply one image's offsets to the other.

A 4-entry word array at VMA `0x08015D10` (file `0x9D10`) selects the per-channel
conversion table. In the **HD** image its four words are the **flash file-offsets** of the
four LUTs — `0x1A6AC, 0x1A88C, 0x1A974, 0x1A4CC` (i.e. VMA `0x0801A6AC / 0x0801A88C /
0x0801A974 / 0x0801A4CC`, base `0x0800C000`) — which is byte-verified and is why the
descriptors below are keyed to those addresses. (In the NA image the same address holds
unrelated data, not a pointer array.) The descriptors (VMA → content):

**Resolved 2026-10-22** — the consumer path was found by disassembly (was: "not
resolvable"; the indirect path is a runtime fetcher):

- LUT lookup `0x8013304(raw, sensor_id, maxtemp)` → fetcher `0x8015cac`; tables are
  8-byte `{key, value}` pairs, binary search over idx 0..59; gate byte `0x1FFF84F2`
  (0 = bypass LUT).
- **sensor_id 0 = HOTEND** (base `0x0801A6AC`): key = **OHMS**, value = °C —
  (102 Ω, 300C), (110, 295), (119, 290) … (10490, 85). **was: "bed, (temp×10, adc)"
  — misparse; the pairs are (ohms, temp).**
- **sensor_id 3 = BED**: key = **RAW ADC** (no ohm conversion), error shutdown >130C.
- Hotend ADC consumer `0x8016d14`: averages 20 DMA samples, converts
  `R = 4700·avg/(4095−avg)` @0x8016d36–42, error shutdown ≥300C.
  Bed ADC consumer `0x80133c0`: 20-sample average, feeds raw ADC to the LUT.
- **ADC1 seq-A channel mask `0x21` = CH0 + CH5 only** → exactly PA0 (hotend) and
  PA5 (bed). PC0 is not sampled by the factory fw.
- The **second hotend table is VESTIGIAL**: code exists (`0x8013490`, called only
  from `0x80184fa`) but its raw globals `0x1FFF8500/02/04` are only initialized
  (3170 / 15458 / 114) and never fed by the ADC. The live hotend always uses
  LUT0 (1 Ω → 300C). **One curve covers both the 240C and 265C hotends.**

Facts and limits (updated):

- Table contents and the consumer chain are now byte-verified + live-confirmed
  (room-temp readings and heater tests match the curves).
- Absolute ADC scaling **RESOLVED**: DR is right-aligned 12-bit (0..4095), pullup
  4700 Ω — the fw ohm formula is identical to Klipper's `Thermistor.calc_temp`.
  The old "LUT scale 0..384" impression came from the (ohms, temp) misparse.
- Klipper fits fitted from these LUTs (in printer.cfg): `ad4_hotend`
  55C/29559Ω, 70C/17285Ω, 260C/189Ω (max err 0.68C over 20–300C); `ad4_bed`
  29C/81221.9Ω, 110C/5200.5Ω, 118C/4214.5Ω (max err 0.53C over 20–130C).

### 4.2 SoC side — nozzle calibration model (the "12"/"14" values)

The per-nozzle two-digit "nozzle temp cal" value is a **target-temperature offset applied by the
SoC**, not stored or used by the MCU. Evidence from `adventurer4-arm` symbols:

```
Config::getExtruderOneTempOffset      Config::getExtruderTwoTempOffset
Config::getExtruderThreeTempOffset    Config::getExtruderFourTempOffset
Config::getExtruderCalibrationValue   Config::getNozzleTempDifferentValue
Config::getAutoCalibrationSwitch      Config::getAutoPoint
NozzleTempCheck::*
SerialObject::getAutoOffsetValue      SerialObject::getResultForAutoOffset
SerialObject::printingAdjustNozzleTemp
```

Flow: user selects nozzle (UI: `select_nozzle1..4.png`) → SoC reads the stored offset for that
nozzle → SoC shifts the target temperature by the offset before sending it to the MCU.
SoC-enforced ceiling strings: `"Extruder can be heated up to 265 "`, `"Nozzle 0-280 "`,
`"0.3-265"`, `"0.4-265"`, `"0.6-265"`.

**Klipper implication:** encode the offset as a host-side macro (per nozzle), and set
`max_temp` to the nozzle rating + margin (265–280), not the 215 °C table limit.

### 4.3 What does not exist

There is **no cabin / chamber / board temperature sensor** anywhere in the MCU or SoC firmware
(verified by full string scan). The four ADC channels are: hotend (standard), hotend (high-temp),
bed, and one secondary/unused channel.

---

## 5. G-code inventory

### 5.1 SoC parser (`adventurer4-arm`)

```
G-codes:  G0 G1 G2 G3 G4 G5 G6 G7 G8 G9 G44 G47 G90 G91 G92 G161 G162
M-codes:  M0 M1 M2 M3 M4 M5 M6 M7 M8 M9 M17 M18 M25 M31 M36 M37 M38 M39
          M47 M63 M103 M104 M105 M107 M112 M113 M114 M117 M118 M119
          M121 M122 M140 M144 M145 M652 M653 M654 M901
```

### 5.2 MCU-embedded G-code (power-loss recovery / state)

| Address (file) | String |
|---|---|
| `0x0EB15` | `G90` |
| `0x0EB19` | `G28` |
| `0x0EB1D` | `M132 X Y Z A B` (store position) |
| `0x0EB32` | `G1 Z15 F400` (Z lift) |
| `0x0EB40` | `M117$` (status string) |

State machine strings: `READY, BUILDING, BUILDING_FROM_SD, BUILDING_ONBOARD, ERROR,
CANCEL_BUILD, HEAT_SHUTDOWN, MOVING, DELAY, HOMING, WAIT_ON_TOOL, WAIT_ON_PLATFORM,
WAIT_ON_BUTTON` (the MCU can cut heat autonomously via `HEAT_SHUTDOWN`).

SoC auto-generated moves (calibration/test patterns) are inventoried in `GCODE-STRINGS.md`.

---

## 6. SoC software stack

| Component | Details |
|---|---|
| `adventurer4-arm` | Qt/C++ main app; UI, G-code parser, control model; unstripped, debug info |
| `ffstartup-arm` | Go supervisor; reads `ffstartup.cfg`, launches `./software/%VERSION%/adventurer4-arm` with `-1;-D;-qws` |
| `ffstartup.cfg` | `LogDir=/opt/finder_rush/log`, `Kernel=kernel`, `Control=control`, `Software=software`, `Library=library`, `Duration=30` |
| MCU link | `/dev/ttyS1`, 115200 |
| Network | `eth0` and `wlan0` managed; WiFi AP mode: SSID `Adventurer 4`, password `12345678` (`rtl_hostapd_2G.conf`); DHCP check via `1slt_dhcp_check()` |
| Vendor updater | `flashforge_init.sh` (disk-based, §8) → `update_control/kernel/software/library` → per-package `run.sh`; `control-2.3.0/run.sh` runs `IAPCommand` on both MCU hexes |
| Boot guard | updater requires `armv7l` + kernel `3.4.39+`; machine check skipped when a `screwflag` file is present |

Notable `Config` / `SerialObject` symbols (SoC control model): `getExtruder*TempOffset`,
`getFlashCloud*`, `getPolarCloud*` (cloud accounts), `hostHeartCheckTemperature`,
`sendPrintLayer`, `readM25`, `printingPersent`, `gpioBufCheckInit`, `ParseXYZEFField`.

### 6.1 SoC hardware map (verified from the working machine's boot log)

From `FFAD4-BKUP.zip` → `dmesg-start.txt` (a full eMMC image of a working printer):

| Subsystem | Node | Boot-log evidence |
|---|---|---|
| SoC | Allwinner sun8i / **sun50i V3S**, armv7l, kernel 3.4.39+ | `Machine: sun8i` |
| **MCU serial** | **`/dev/ttyS1`** (uart0) | `uart0: ttyS1 at MMIO 0x1c28000` |
| SoC console | `/dev/ttyS0` (uart2) — **not** the MCU | `uart2: ttyS0`, `console [ttyS0] enabled` |
| **Ethernet** | `eth0` = **gmac0** GbE, PHY `00441400` | `gmac0: eth0: PHY ID 00441400 at 0` |
| WiFi | `wlan0` (RTW) | `RTW: rtw_ndev_init(wlan0)` |
| **Touch** | **TSC2007** → `/dev/input/event0,1`, `/dev/touchscreen/ucb1x00` | `TSC2007 TouchScreen Driver probe` |
| **Display** | **`/dev/fb0`** (DISP, dclk 400 MHz) | `[DISP]` blocks, `/dev/fb0` |
| **USB host** | EHCI + OHCI (bus 1 + bus 2) | `sunxi-ehci/ohci .1` |
| USB mass storage | `sda1/sda2` | `Initializing USB Mass Storage driver` |
| i2c | `twi0` @100 kHz, `twi1` | `twi_chan_cfg() [twi0]/[twi1]` |
| PWM / backlight | `/dev/pwm` | `pwm module init!`, `Backlight Compensation` |
| PMIC | AXP20x (CHIP ID 0x41) | `axp20_board 0-0034: AXP (CHIP ID: 0x41)` |
| USB camera | `/dev/video0` (`uvcvideo`) | `uvcvideo: Adding mapping ...` |

Key implications (detailed in **`adv4-soc-hack.md`**): the MCU is already wired to the
SoC (`/dev/ttyS1`), so Klipper can reach it **through the SoC with no soldering**; the
touch panel is **SoC-driven** (TSC2007); `eth0` is real wired GbE for the RPi↔SoC link;
`/dev/fb0` + touch give the SoC a local UI. The factory app's MCU baud is 115200 — and
**Klipper must use 115200 on the AD4 too**: the SoC `uart0` clock is 24 MHz, below the
serial driver's 30–120 MHz `setbaud()` range, so 250000 can't actually be programmed on
this line (see `KLIPPER-AD4-FLASH-GUIDE.md`, baud note).

---

## 7. Patching `stm32isp` for this board

> **Status: built.** A complete, compiling fork with the GPIO dance built in now
> lives in **`stm32isp-ad4/`** (`stm32isp-ad4.c`, `Makefile`, `flash-mcu.sh`, and
> the stock `Adventurer4-HD-IAP-2.3.0-20230216.hex`). Its Intel-HEX parser is
> validated against both factory hexes, and the end-to-end serial pump is tested.
> The design below is exactly what that code implements. See
> **`adv4-soc-hack.md` §6** for flashing and recovery.

`IAPCommand` is a `stm32isp` fork; its delta over upstream is small and fully decoded.
A clean fork needs exactly:

1. **GPIO strapping before serial open** (the part generic `stm32isp` lacks):
   ```c
   static void ad4_gpios_init(void) {
       /* SoC sysfs, root required */
       sysfs_gpio_export(34);           /* NRST  */
       sysfs_gpio_export(35);           /* "BOOT0" — role UNPROVEN */
       sysfs_gpio_dir(34, 1);           /* out   */
       sysfs_gpio_dir(35, 1);           /* out   */
       sysfs_gpio_set(35, 1);           /* HIGH — what IAPCommand does; why is unverified */
       sleep(1);                        /* hold — what IAPCommand does; not A/B-tested */
   }
   static void ad4_nrst_pulse(void) {
       sysfs_gpio_set(34, 0);  usleep(15680);
       sysfs_gpio_set(34, 1);  usleep(15680);
   }
   ```
2. **Sync-with-retry**: send `0x43`; on no reply, call `ad4_nrst_pulse()` and retry
   (upstream times out after one attempt). Accept replies `6` (HD) or `7` (NA).
3. **Dual-hex argument model**: `stm32isp-ad4 <na.hex> <hd.hex>` — select the
   file from the sync reply instead of assuming one chip (the factory `run.sh`
   passes exactly these two arguments; port/baud are internal defaults).
4. **Post-write release**: `sysfs_gpio_set(35, 0)` (BOOT0 low) + one `ad4_nrst_pulse()`
   so the MCU boots the new app.
5. **Optional**: status flag file + verbose logging (the value of a custom fork over the
   opaque factory binary).

Everything else (packet framing, flash page writes, verify) is standard `stm32isp` and
unchanged. Roughly 50–80 lines of added C.

---

## 8. Unattended flashing (USB disk)

The factory updater is a script named **`flashforge_init.sh`** placed at the root of the
removable disk. The SoC's boot/update path detects and executes it (parameters
`Adventurer4 0012`). Mirror that layout with our own minimal runner.

### 8.1 Disk layout (FAT32)

```
/                       ← disk root
  flashforge_init.sh    ← runner (below)
  screwflag             ← empty file: skips the machine-type check
  IAPCommand            ← factory ARM binary (or the stm32isp fork, §7)
  klipper.hex           ← Klipper @ 0x0800C000
  factory-HD.hex        ← known-good restore image (never delete)
  RESTORE               ← if this file exists, the script flashes factory-HD.hex instead
```

### 8.2 `flashforge_init.sh`

```sh
#!/bin/sh
# AD4 Klipper unattended flash — run from USB disk by the SoC boot/update path.
# Same guard contract as the factory script.
set -x
WORK_DIR=`dirname $0`

[ "$(uname -m)"  != "armv7l"   ] && { echo "arch error";  exit 1; }
[ "$(uname -r)"  != "3.4.39+" ] && { echo "kernel error"; exit 1; }

# 1) Stop the vendor stack so nothing holds /dev/ttyS1 or the GPIOs.
pkill -f ffstartup-arm  2>/dev/null
pkill -f adventurer4-arm 2>/dev/null
sleep 2

# 2) Free the MCU UART (belt and suspenders).
fuser -k /dev/ttyS1 2>/dev/null
sleep 1

# 3) Select image: RESTORE flag => factory, else Klipper.
if [ -f "$WORK_DIR/RESTORE" ]; then
    IMG="$WORK_DIR/factory-HD.hex"; FLAG=restore
else
    IMG="$WORK_DIR/klipper.hex";   FLAG=klipper
fi

# 4) Flash. Passing the same image in both slots makes it rev-agnostic
#    (IAPCommand auto-selects HD/NA from the sync reply 6/7).
"$WORK_DIR/IAPCommand" "$IMG" "$IMG"
RC=$?

# 5) Record the outcome for later inspection.
echo "$FLAG rc=$RC" > /root/ad4-flash-status 2>/dev/null || true
sync
exit $RC
```

Safety rules (non-negotiable):

- Write **only** the app window for the detected revision (HD `0x0000C000`, NA
  `0x08010000`); the ROM ISP keeps every state recoverable.
- `factory-HD.hex` always travels on the disk; presence of `RESTORE` reverts in one boot.
- The script never touches flash regions other than the IAP app window (and hard-refuses
  any image base ≤ `0x00008000`).

Open item (verify once on hardware): the exact trigger that makes the SoC execute the disk's
`flashforge_init.sh` at boot (the bootloader's removable-media update hook). The name and
argument convention match the factory package, which is the working reference.

---

## 9. RPi-over-Ethernet operation (Klipper host)

> **Two variants.** §9.1 below is the *wired* option (RPi connected to the MCU
> on PA2/PA3). The **no-soldering** option — RPi connected to the **SoC** over
> the printer's own Ethernet port, with the SoC running a serial↔network
> bridge and (optionally) its TSC2007 touch UI — is documented in
> **`adv4-soc-hack.md`** and is the recommended path (zero wiring). The choice:
>
> - **§9.1 (wired MCU)**: RPi drives the MCU directly. Requires the two UART
>   wires on the board and a Klipper rebuild onto PA2/PA3.
> - **`adv4-soc-hack.md` (SoC bridge, no soldering)**: RPi talks to the SoC over
>   Ethernet; the SoC pumps the MCU's existing `/dev/ttyS1` link. No rebuild,
>   no wires, and it reuses the SoC's touch/USB/network for a UI.

### 9.1 Architecture and division of labor

```
[Raspberry Pi] ── UART (PA2/PA3, 250000) ──> [HC32F460 running Klipper]
      │
      └── Ethernet ──> [your LAN] ──> Fluidd/Mainsail (any device)

[Sunxi SoC] ── GPIO 34 (NRST, proven) + GPIO 35 (role unproven) ──> [HC32F460 bootloader]   (flashing only)
[USB disk]  ── flashforge_init.sh ──> [SoC]                  (unattended flash)
```

- The **RPi is the Klipper host**: klippy + Moonraker + Fluidd, reachable over Ethernet.
- The **SoC has no role in operation** once the MCU serial is moved off the SoC's USART0.
  Its only operational role is **flashing** (it alone drives NRST/GPIO35) and, later,
  optionally a UI — which should be a **network UI** (Fluidd/Mainsail pointed at Moonraker)
  or a small web client on the SoC, not a direct MCU link.
- The RPi **cannot enter ISP mode itself** — it does not drive GPIO 34/35. All flashing
  stays on the SoC path (§8). This is a hardware constraint, not a software gap.

### 9.2 MCU-side change required

Rebuild the Klipper firmware with a serial pair that does not collide with board pins:

```
CONFIG_HC32F460_SERIAL_PA7_PA8  ->  off
CONFIG_HC32F460_SERIAL_PA3_PA2  ->  on
```

**Correction 2026-10-20:** `PA9_PA10` is now the **preferred** pair — it is
the proven vendor SoC↔MCU line (USART2), so no new wiring is needed if you
keep the SoC bridge. `PA3_PA2` remains the fallback if you want the SoC fully
out of the loop. (Earlier text called PA3_PA2 "the only free non-SWD pair" —
wrong, and PA13_PA14 is not usable at all: no USART AF on SWD pins.) Wire:

| RPi | MCU | Note |
|---|---|---|
| GPIO14 (TXD) | PA2 or PA3 | if Klipper does not connect, swap the two wires |
| GPIO15 (RXD) | the other of PA2/PA3 | |
| GND | any GND | **GND only — do not inject 3.3 V/5 V**; the MCU is powered by the printer PSU |

Baud: 250 000 (Klipper default; both sides agree). Logic levels are both 3.3 V — no shifter.

### 9.3 Software on the RPi

1. Raspberry Pi OS 64-bit (any Pi 3B+/4/5; 2B works).
2. `sudo apt -y install python3-pip python3-venv git build-essential`
3. `git clone https://github.com/Klipper3d/klipper && cd klipper && ./scripts/install-linux.sh`
4. Enable the hardware UART and release it from Bluetooth (`/boot/config.txt`:
   `enable_uart=1`, `dtoverlay=disable-bt`, or the equivalent in raspi-config) → `/dev/serial0`.
5. `[mcu] serial: /dev/serial0` (see `printer.cfg`).
6. Moonraker + Fluidd (or Mainsail) for the web UI; static IP or your router's DHCP.

### 9.4 What the SoC must provide (flashing path only)

- Nothing at runtime. For the unattended method: the disk from §8.
- If a **dedicated flash mode** is wanted (so a stock-booted printer can be flashed without a
  USB disk every time): a boot flag (e.g. `/data/klipper-mode`) that makes the SoC's rc skip
  launching `adventurer4-arm` and run `flashforge_init.sh` from a fixed path instead. One
  small edit to the boot rc; no driver work.
- For a future SoC UI: run any lightweight web client (or a Chromium kiosk) pointed at
  Moonraker on the RPi — no SoC↔MCU wiring needed at all.

### 9.5 Conflict notes

- While the vendor app runs, it owns `/dev/ttyS1` and may exercise the GPIO strapping at its
  own boot. That only matters for the flash path; the RPi↔MCU link on PA2/PA3 is independent.
- After the serial move, the SoC's old link to the MCU is dead (MCU no longer listens on its
  USART0). Do not rely on the stock UI for temperature/motion — everything goes through Klipper.

---

## 10. Open items (live-board verification)

| # | Item | Method |
|---|---|---|
| 1 | Hotend thermistor pin (and which of the two hotend curves is active) | `probe.cfg`: candidate ADC pins as `[temperature_sensor]`, warm the nozzle |
| 2 | LED pin | `SET_PIN` sweep over candidates (mA-level, safe) |
| 3 | Absolute ADC scaling | `QUERY_ADC` at known temperatures (ice bath, room, warm-up) |
| 4 | Per-nozzle offsets to encode | your "12"/"14" values per fitted nozzle |
| 5 | PA2/PA3 TX/RX order | swap wires if klippy handshake fails |
| 6 | Exact SoC boot trigger for disk `flashforge_init.sh` | one hardware test with the §8 disk |
| 7 | Vendor service name holding `/dev/ttyS1` | `systemctl` / `fuser` on the live SoC |
