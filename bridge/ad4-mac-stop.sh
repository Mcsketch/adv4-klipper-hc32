#!/bin/bash
# =============================================================
# ad4-mac-stop.sh — stop klippy + moonraker + Fluidd proxy + gcode
# FIFO holder on the Mac. (The SoC bridge keeps running until
# power-cycle or the flash script restarts it.)
# =============================================================
set -uo pipefail
KDIR="/Users/admin/klipper-hc32"
RUN="$KDIR/run"
step(){ printf '\033[1;32m=== %s ===\033[0m\n' "$1"; }

step "stop ad4 mac processes"
for f in fluidd.pid moonraker.pid klippy.pid fifo.pid; do
  [ -f "$RUN/$f" ] && kill "$(cat "$RUN/$f")" 2>/dev/null && echo "  killed $f"
done
pkill -f "klippy/klippy.py" 2>/dev/null
pkill -f "moonraker/moonraker.py" 2>/dev/null
pkill -f "ad4-fluidd-proxy.py" 2>/dev/null
pkill -f "sleep 86400 > $HOME/adv4/links/gcode" 2>/dev/null
sleep 1
rm -f /tmp/klippy_uds "$HOME/adv4/links/gcode" "$RUN"/*.pid 2>/dev/null
echo "stopped. klippy released the SoC bridge, so IAPCommand can flash now."
