# Flashforge Adventurer 4 → Klipper — Complete Engineering Dossier

Self-documenting folder. All artifacts verified against:
- `FFAD4-BKUP.zip` (7.8 GB eMMC image of a working printer)
- `control-2.3.0.tar.xz` (factory updater package)
- Direct disassembly of `IAPCommand` (stm32isp fork)
- Raw Intel-HEX parsing of factory MCU images

## Documents

| File | Purpose |
|---|---|
| **`GETTING-STARTED.md`** | **First-install howto**: stock → Klipper that moves, in 5 steps, no soldering. Start here. |
| **`adv4-touch-ui.md`** | **Touchscreen UI**: drive Klipper from the SoC touch (Qt4 + TSLIB + Moonraker client). Hardware verified, build plan. |
| **`adv4-exploration.md`** | Shareable engineering report: all verified facts, addresses, GPIO dance, thermistor tables, SoC hardware map, architecture, flashing, recovery |
| **`adv4-soc-hack.md`** | **No-soldering architecture**: RPi→SoC Ethernet, SoC serial↔network bridge, touch UI, USB, network. How to run Klipper over the bridge. MCU + SoC recovery |
| **`printer-hc32.cfg`** | Klipper `printer.cfg` draft (correct where certain, annotated where live verification is needed) |
| **`KLIPPER-AD4-FLASH-GUIDE.md`** | Original flash guide (pre-fork); still valid for reference |

## Code

| Folder | Contents |
|---|---|
| **`stm32isp-ad4/`** | Complete self-contained C fork of `stm32isp` with the SoC GPIO dance (BOOT0=GPIO35, NRST=GPIO34) built in. `Makefile` (native + cross). `flash-mcu.sh` (SoC driver). Factory HD+NA hexes for restore. **Compiles clean (gcc -Wall -Wextra, zero warnings)** |
| **`soc-hack/`** | `ad4-serial-bridge.py` (TCP/UDP serial↔network bridge, byte-transparent, tested E2E). `ad4-bridge.sh` (SoC start/stop/status). `rpi-setup.sh` (RPi Klipper host setup + socat PTY systemd unit) |
| **`flashdisk/`** | Unattended USB-stick flash: `flashforge_init.sh` + `README.md` |

## Work artifacts (reproducible)

| Path | Description |
|---|---|
| `work/ctrl/IAPCommand` | Factory MCU flasher binary (ARM ELF, unstripped) |
| `work/ctrl/Adventurer4-HD-IAP-2.3.0-20230216.hex` | Stock HD MCU image (base 0x0000C000) |
| `work/ctrl/Adventurer4-NA-IAP-2.3.0-20230216.hex` | Stock NA MCU image (base 0x08010000) |
| `work/bkup/dmesg.txt` | Working-machine SoC boot log (peripheral map) |
| `work/bkup/printer_profile_kv.txt` | Slic3r printer profile from /data |
| `work/hd.bin` | Decoded HD MCU image (binary) |

## Key verified facts (summary)

| Item | Value | Source |
|---|---|---|
| SoC | Allwinner V3S (sun8i/sun50i), kernel 3.4.39+, armv7l | dmesg |
| MCU UART on SoC | `/dev/ttyS1` (uart0), factory 115200, Klipper 250000 | dmesg + IAPCommand |
| Ethernet on SoC | `eth0` = gmac0 GbE, PHY 00441400 | dmesg |
| Touch | TSC2007 → `/dev/input/event0,1` | dmesg |
| Display | `/dev/fb0` | dmesg |
| USB host | EHCI+OHCI (bus 1, bus 2) | dmesg |
| GPIO NRST | SoC GPIO **34** (15.68 ms LOW / 15.68 ms HIGH) | IAPCommand disasm |
| GPIO BOOT0 | SoC GPIO **35** (HIGH during reset) | IAPCommand disasm |
| HD MCU base | **0x0000C000**, 60 416 B | HD hex type-04 record |
| NA MCU base | **0x08010000**, 44 640 B | NA hex type-04 record |
| Safety invariant | Never write ≤ 0x00008000 | Design (keeps ROM-ISP recoverable) |
| IAPCommand | stm32isp fork, unstripped, ARM ELF, with debug info | `file` + `strings` |

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

## Recovery

**MCU** (always available, no hardware): `./flash-mcu.sh factory` on the SoC.
**SoC** (if Linux breaks): rewrite `FFAD4-BKUP.zip` eMMC image to `mmcblk0`.
The two are independent — MCU recovery never depends on SoC state and vice versa.
