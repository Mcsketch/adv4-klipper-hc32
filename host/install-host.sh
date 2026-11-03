#!/bin/sh
# Klipper host side, for a host that already runs Klipper + Moonraker
# (KIAUH or similar):
#   host/install-host.sh PRINTER_IP [KLIPPER_DIR] [CONFIG_DIR]
# 1. applies klipper-socket-serial.patch to KLIPPER_DIR (default ~/klipper)
#    unless it is already there. A Klipper update drops it: run this again.
# 2. installs printer.cfg into CONFIG_DIR (default ~/printer_data/config)
#    with the printer's address; an existing printer.cfg is kept as
#    printer.cfg.bak-DATE
# 3. restarts the klipper service
set -e
[ -n "$1" ] || { sed -n '2,11p' "$0"; exit 1; }
IP=$1
KDIR=${2:-$HOME/klipper}
CDIR=${3:-$HOME/printer_data/config}
HERE=$(cd "$(dirname "$0")" && pwd)

if grep -q 'socket://' "$KDIR/klippy/serialhdl.py"; then
    echo "patch: already in $KDIR"
else
    git -C "$KDIR" apply --check "$HERE/klipper-socket-serial.patch"
    git -C "$KDIR" apply "$HERE/klipper-socket-serial.patch"
    echo "patch: applied to $KDIR (uncommitted; a Klipper update removes it)"
fi

mkdir -p "$CDIR"
if [ -f "$CDIR/printer.cfg" ]; then
    b="$CDIR/printer.cfg.bak-$(date +%Y%m%d-%H%M%S)"
    cp -p "$CDIR/printer.cfg" "$b"
    echo "config: old printer.cfg saved as $b"
fi
sed "s/PRINTER_IP/$IP/" "$HERE/printer.cfg" >"$CDIR/printer.cfg"
echo "config: $CDIR/printer.cfg, [mcu] serial socket://$IP:5000"

sudo systemctl restart klipper
echo "klipper restarted. Check: Mainsail/Fluidd console, or ~/printer_data/logs/klippy.log"
