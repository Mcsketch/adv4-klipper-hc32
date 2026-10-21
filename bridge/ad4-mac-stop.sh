#!/bin/bash
# =============================================================
# ad4-mac-stop.sh — stop klippy + socat PTY (+ moonraker) on the Mac.
# =============================================================
set -uo pipefail
KDIR="/Users/admin/klipper-hc32"
RUN="$KDIR/run"
step(){ printf '\033[1;32m=== %s ===\033[0m\n' "$1"; }

step "stop ad4 mac processes"
for f in moonraker.pid klippy.pid socat.pid; do
  [ -f "$RUN/$f" ] && kill "$(cat "$RUN/$f")" 2>/dev/null && echo "  killed $f"
done
pkill -f "TCP4:192.168.10.10:5000" 2>/dev/null
pkill -f "klippy/klippy.py" 2>/dev/null
pkill -f "moonraker" 2>/dev/null
sleep 1
rm -f /dev/adv4_mcu "$RUN"/*.pid 2>/dev/null
echo "stopped. (the SoC bridge keeps running until power-cycle or the flash script restarts it)"
