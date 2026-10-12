#!/bin/bash
# =============================================================
# ad4-mac-start.sh — start the SoC-bridge PTY + klippy on the Mac.
# Idempotent: stops any existing instances first, then (re)starts.
#
# macOS /dev is root-only, so the PTY *symlinks* live in ~/adv4/links:
#   ~/adv4/links/mcu    <- socat: SoC:5000  (real PTY = /dev/ttysXXX)
#   ~/adv4/links/gcode  <- klippy -I  (gcode tuning port)
# pyserial/klippy open the symlink; it resolves to the real /dev/ttysXXX.
#   socat : SoC:5000  -> links/mcu (raw PTY)
#   klippy: reads printer.cfg ([mcu] serial: links/mcu), -I links/gcode
# Logs: ~/klipper-hc32/klippy.log
# =============================================================
set -uo pipefail

KDIR="/Users/admin/klipper-hc32"
SOC_IP="${SOC_IP:-192.168.10.10}"
BRIDGE_PORT="${BRIDGE_PORT:-5000}"
LINKDIR="$HOME/adv4/links"
PTY="$LINKDIR/mcu"
GCODE_PTY="$LINKDIR/gcode"
CFG="$KDIR/printer.cfg"
PY="$KDIR/venv/bin/python"
RUN="$KDIR/run"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$LINKDIR" "$RUN"

step(){ printf '\033[1;32m=== %s ===\033[0m\n' "$1"; }

# --- stop existing -------------------------------------------
step "stop existing ad4 processes (if any)"
[ -f "$RUN/socat.pid" ] && kill "$(cat "$RUN/socat.pid")" 2>/dev/null
[ -f "$RUN/klippy.pid" ] && kill "$(cat "$RUN/klippy.pid")" 2>/dev/null
pkill -f "TCP4:$SOC_IP:$BRIDGE_PORT" 2>/dev/null
pkill -f "klippy/klippy.py" 2>/dev/null
sleep 1
rm -f "$PTY" "$GCODE_PTY" "$RUN/socat.pid" "$RUN/klippy.pid"

# --- socat PTY ------------------------------------------------
step "start socat PTY: $SOC_IP:$BRIDGE_PORT -> $PTY"
# NOTE: this socat build rejects the 'reconnect' option, so we auto-restart
# socat in a loop instead (the SoC bridge is the persistent side; a drop just
# re-connects here). PTY,raw,echo=0,link=$PTY presents the remote serial as a
# local PTY (real device = /dev/ttysXXX; PTY baud cosmetic, SoC enforces 250000).
cat > "$RUN/socat-loop.sh" <<EOS
while :; do
  socat TCP4:$SOC_IP:$BRIDGE_PORT,keepalive,nodelay PTY,raw,echo=0,link=$PTY
  sleep 1
done
EOS
nohup bash "$RUN/socat-loop.sh" > "$RUN/socat.log" 2>&1 &
echo $! > "$RUN/socat.pid"
sleep 2
if ! kill -0 "$(cat "$RUN/socat.pid")" 2>/dev/null; then
  echo "  socat failed to start; log:"; tail -n 15 "$RUN/socat.log"; exit 1
fi
echo "  socat pid $(cat "$RUN/socat.pid"); MCU link: $PTY"

# --- klippy ---------------------------------------------------
step "start klippy"
rm -f "$KDIR/klippy.log"
# -I $GCODE_PTY : klippy creates a gcode PTY; we get a /dev/adv4_gcode to type
#   G-code into (the 'tuning' port). See ad4-mac-tune.sh.
nohup "$PY" "$KDIR/klippy/klippy.py" -I "$GCODE_PTY" -l "$KDIR/klippy.log" "$CFG" \
    > "$RUN/klippy.out" 2>&1 &
echo $! > "$RUN/klippy.pid"
sleep 4
if ! kill -0 "$(cat "$RUN/klippy.pid")" 2>/dev/null; then
  echo "  klippy exited early; log tail:"; tail -n 25 "$KDIR/klippy.log" 2>/dev/null
  exit 1
fi
echo "  klippy pid $(cat "$RUN/klippy.pid")"
echo "  gcode tuning port: $GCODE_PTY"

step "status (klippy.log tail)"
tail -n 15 "$KDIR/klippy.log" 2>/dev/null
cat <<EOF

  klippy is starting. It will retry the MCU until the SoC bridge answers.
  Watch:      tail -f $KDIR/klippy.log
  Expect:     'Starting Klippy...' then MCU connect (no 'timed out').
  Tune:       $HERE/ad4-mac-tune.sh  (G28, G30, temperature, etc.)
  Stop:       $HERE/ad4-mac-stop.sh
EOF
