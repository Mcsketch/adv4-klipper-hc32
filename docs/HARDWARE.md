# Hardware: Adventurer 4, NA mainboard

Mainboard FFP0187_Main_Board_V1.0.3, MCU Nation N32G455 (Cortex-M4F,
STM32F103-compatible peripherals), 12 MHz crystal. Every pin below was found
in FlashForge's stock NA firmware (disassembled at base 0x08010000) and
then checked on a running printer. [host/printer.cfg](../host/printer.cfg)
is the working config.

## MCU link

| | |
|---|---|
| MCU UART | USART1 (PA9/PA10), 115200 8N1 |
| SoC side | `/dev/ttyS1` |
| MCU reset | SoC GPIO34 (NRST) |
| Boot strap | SoC GPIO35 (driven by IAPCommand) |
| Bootloader | 0x08000000-0x0800FFFF, FlashForge's, never written |
| Application | from 0x08010000 |

Only 115200 works: the SoC UART clock cannot make faster rates exactly, and
Klipper's `CONFIG_SERIAL_BAUD` falls back to 250000 unless
`CONFIG_LOW_LEVEL_OPTIONS=y`, which the defconfig sets.

## Pins

| Function | Pin | Notes |
|---|---|---|
| X step / dir / enable | PA8 / PC9 / !PA11 | |
| Y step / dir / enable | PB14 / !PB13 / !PB15 | |
| Z step / dir / enable | PA15 / PA12 / !PC11 | |
| E step / dir / enable | PC7 / !PC6 / !PC8 | |
| X endstop | ^PC3 | at max (220) |
| Y endstop | ^PC1 | at min (4) |
| Z endstop | ^PB5 | at the **top** |
| TMC2209 UART | PC10 | one wire, muxed |
| TMC mux select | PB0, PB1, PB10, PB11 | X: !PB0 PB1 PB10 !PB11; Y: all low; E: !PB0 PB1 !PB10 !PB11 |
| Hot end heater / sensor | PA1 / PA0 | 4.7 k pull-up |
| Bed heater / sensor | PA7 / PA5 | 4.7 k pull-up |
| Part cooling fan | PA6 | stock firmware software-PWMs it from M106 |
| Hot end heatsink fan | PC2 | `heater_fan`, on above 50 °C |
| Mainboard fan | PC5 | `controller_fan` |
| Chamber fan | PB9 | `fan_generic` |
| Chamber light | PA4 | `[output_pin light]`, software PWM 0-100 % (stock `M144`-`M146`) |

Z's driver is not on the TMC UART bus (fixed 1/8 step). Reading every mux
state at UART addresses 0-3 finds only X, Y, E and a fourth TMC2209 on
PB10 alone (all other select pins low). The stock firmware configures that
fourth chip like X, but it gets no step pulses from X, Y or Z, so Klipper
leaves it alone. Every chip's IOIN reads ENN high whatever the enable pins
do, so ENN can't map channels to motors; step counts (MSCNT) can.

Known issue: Z whines while moving under Klipper (not on stock). The pins
the stock firmware drives for Z (PB4 low; PC4 high, tried) match or made no
difference, so it is not fixed.

## Calibrated values (one printer)

| Axis | Microsteps | rotation_distance | Notes |
|---|---|---|---|
| X | 16 | 33.9 | run 0.55 A |
| Y | 16 | 60 | run 0.66 A |
| Z | 8 | 6.25 | endstop at 254.7 above the bed; `max_z_velocity` 5 (10 stalls going up) |
| E | 16 | 33.5 | run 1.38 A |

TMC currents are the stock firmware's boot settings.

Thermistors: 100 k NTC, curves fitted to the stock firmware's lookup tables
(`ad4_hotend`, `ad4_bed` in printer.cfg). See
[reference/n32-thermistor.md](reference/n32-thermistor.md).

## SoC

Allwinner V3S, busybox, kernel 3.4.39, root filesystem on eMMC (ext4,
persistent), `/tmp` in RAM. Ethernet eth0, WiFi wlan0 (RTL8188FU), panel on
`/dev/fb0` with a resistive touchscreen, piezo buzzer on a PWM.
