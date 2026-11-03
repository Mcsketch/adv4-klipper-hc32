# HC32F460 (HD mainboard): experimental, unverified

berthy456's port of Klipper to the HC32F460 and the first installer for it,
kept as they were when this repository switched to the NA board. Nobody here
has an HD board, so none of it has been run on one since. The install and
rollback sticks refuse HD boards.

| | |
|---|---|
| `klipper-port/` | HC32F460 MCU code for Klipper (`src-hc32f460`, vendor driver library, `board.config`) and `make-positioned-ihex.py` |
| `stm32isp-ad4/` | the SoC-side flasher and probes written for the HD bootloader |
| `installer/` | the original USB-stick installer (`flashforge_init.sh`, daemon, persistent install). Paths inside still say `install-on-printer/` and `tools/` |
| `printer-hc32.cfg` | the HD config, pins not verified |
| `KLIPPER-AD4-FLASH-GUIDE.md` | the original guide |

Never flash any of this onto an NA (N32G455) printer.

To bring HD support to the sticks, the parts that change are the board check
in `stick/install/flashforge_init.sh`, a Klipper build for the HD slot, and an
HD `printer.cfg`. IAPCommand already writes HD boards. Open an issue first.
