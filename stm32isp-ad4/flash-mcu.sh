#!/bin/sh
#
# flash-mcu.sh — flash the Adventurer 4 MCU (HC32F460) from the SoC, using the
# stm32isp-ad4 tool. This is the unattended path: it does the GPIO strapping
# (BOOT0=GPIO35, NRST=GPIO34) that a generic stm32isp cannot, then talks to the
# ROM bootloader over /dev/ttyS1 @115200.
#
# SAFETY (why you cannot brick this MCU):
#   - We ONLY ever write the application window. The ROM ISP at 0x00000000 is
#     never touched, so the MCU can ALWAYS re-enter ISP by repeating the
#     strapping dance (even if the app is garbage). stm32isp-ad4 also refuses
#     to write anywhere at or below 0x00008000.
#   - The stock image is preserved (see README: keep a copy of
#     Adventurer4-HD-IAP-2.3.0-20230216.hex) so "flash factory" is always one
#     command away.
#
# Usage:
#   ./flash-mcu.sh klipper        # flash the Klipper hex (klipper-ad4.hex)
#   ./flash-mcu.sh factory        # restore the stock HD image
#   ./flash-mcu.sh factory -v     # verbose
#   ./flash-mcu.sh /path/to/image.hex   # flash an explicit image
#
# Must be run as root on the SoC (GPIO + serial). The printer must be powered.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$HERE/stm32isp-ad4"
SERIAL="/dev/ttyS1"
BAUD="115200"

# Stock HD image (the factory backup we extracted from control-2.3.0).
FACTORY_HEX="$HERE/Adventurer4-HD-IAP-2.3.0-20230216.hex"
# Your Klipper build, converted for the 0xC000 app window (see klipper build
# section in README):  arm-none-eabi-objcopy -I binary -O ihex
#                         --change-address=0xC000 klipper.bin klipper-ad4.hex
KLIPPER_HEX="$HERE/klipper-ad4.hex"

[ "$(id -u)" = 0 ] || { echo "ERROR: run as root (GPIO + $SERIAL)". >&2; exit 1; }
[ -x "$TOOL" ] || { echo "ERROR: tool $TOOL not built. Run: make". >&2; exit 1; }

target="${1:-klipper}"
shift || true

case "$target" in
  klipper)  img="$KLIPPER_HEX" ;;
  factory)  img="$FACTORY_HEX" ;;
  -h|--help) echo "usage: $0 {klipper|factory|<image.hex>} [-v]"; exit 0 ;;
  *)        img="$target" ;;
esac

[ -f "$img" ] || { echo "ERROR: no such image: $img". >&2; exit 1; }

echo "=== flash-mcu.sh ==="
echo "  serial : $SERIAL @ $BAUD"
echo "  image  : $img"
echo "  tool   : $TOOL"
echo
echo "  (the tool does: export GPIO34/35 -> BOOT0 high -> NRST pulse ->"
echo "   open serial -> 0x43 sync -> erase -> program -> verify -> jump)"
echo

"$TOOL" -p "$SERIAL" "$@" "$img"
rc=$?

echo
if [ $rc -eq 0 ]; then
  echo "=== DONE (rc=0). New MCU firmware is running. ==="
  echo "  Marker written: /root/H32 (HD) or /root/N32 (NA)."
else
  echo "=== FAILED (rc=$rc). The MCU is still bootable: ==="
  echo "  1. Re-run the SAME command (it re-straps from scratch)."
  echo "  2. Or restore stock:  $0 factory"
  echo "  The ROM ISP at 0x00000000 was never written, so the device is never bricked."
fi
exit $rc
