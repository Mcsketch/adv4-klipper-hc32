#!/bin/sh
# Klipper host side, for a host that already runs Klipper + Moonraker
# (KIAUH or similar):
#   host/install-host.sh PRINTER_IP [KLIPPER_DIR] [CONFIG_DIR]
# 1. applies klipper-socket-serial.patch to KLIPPER_DIR (default ~/klipper)
#    unless it is already there. A Klipper update drops it: run this again.
# 2. installs moonraker-timelapse (~/moonraker-timelapse, linked into
#    ~/moonraker and CONFIG_DIR) and adds [timelapse] to moonraker.conf
# 3. installs printer.cfg into CONFIG_DIR (default ~/printer_data/config)
#    with the printer's address; an existing printer.cfg is kept as
#    printer.cfg.bak-DATE
# 4. restarts moonraker and klipper
set -e
[ -n "$1" ] || { sed -n '2,13p' "$0"; exit 1; }
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
TL=$HOME/moonraker-timelapse
[ -d "$TL" ] || git clone -q https://github.com/mainsail-crew/moonraker-timelapse.git "$TL"
ln -sfn "$TL/component/timelapse.py" "$HOME/moonraker/moonraker/components/timelapse.py"
ln -sfn "$TL/klipper_macro/timelapse.cfg" "$CDIR/timelapse.cfg"
mkdir -p "$HOME/printer_data/timelapse"
if ! grep -q '^\[timelapse\]' "$CDIR/moonraker.conf"; then
    printf '\n[timelapse]\noutput_path: ~/printer_data/timelapse/\nframe_path: /tmp/timelapse/printer\nsnapshoturl: http://127.0.0.1:8095/webcam/snapshot\n' >>"$CDIR/moonraker.conf"
fi
echo "timelapse: $TL, [timelapse] in $CDIR/moonraker.conf"

if [ -f "$CDIR/printer.cfg" ]; then
    b="$CDIR/printer.cfg.bak-$(date +%Y%m%d-%H%M%S)"
    cp -p "$CDIR/printer.cfg" "$b"
    echo "config: old printer.cfg saved as $b"
fi
sed "s/PRINTER_IP/$IP/" "$HERE/printer.cfg" >"$CDIR/printer.cfg"
echo "config: $CDIR/printer.cfg, [mcu] serial socket://$IP:5000"

sudo systemctl restart moonraker klipper
echo "moonraker and klipper restarted. Check: Mainsail/Fluidd console, or ~/printer_data/logs/klippy.log"
