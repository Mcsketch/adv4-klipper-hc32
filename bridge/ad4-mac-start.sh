#!/bin/bash
# =============================================================
# ad4-mac-start.sh — start klippy + moonraker + Fluidd on the Mac.
# Idempotent: stops any existing instances first, then (re)starts.
#
# Serial chain (proven 2026-10-22):
#   MCU USART2 (PA9/PA10 @115200) <-> SoC /dev/ttyS1
#   SoC: ad4-bridge listens on :5000 (single client; frees the UART
#        when the client is gone, so IAPCommand can flash in between)
#   klippy: [mcu] serial: socket://192.168.10.10:5000 — direct TCP.
#        (No socat PTY: the PTY raced on reconnect. klipper is patched
#        with serial_for_url + fd-type 's' so socket:// works.)
#   moonraker: attaches to klippy via its -a /tmp/klippy_uds API socket
#   Fluidd: ad4-fluidd-proxy.py on :8081 (strata owns :8080 — do not
#        move it; moonraker cors_domains already allow 8081)
#
# gcode tuning port: ~/adv4/links/gcode is a real FIFO with a persistent
# writer holder. A stale symlink to a dead PTY silently swallows every
# command (klippy sees nothing) — this bit us once.
# Send gcode:  printf 'M105\n' > ~/adv4/links/gcode
# Logs: ~/klipper-hc32/klippy.log (appends), run/*.out
# =============================================================
set -uo pipefail

KDIR="/Users/admin/klipper-hc32"
SOC_IP="${SOC_IP:-192.168.10.10}"
BRIDGE_PORT="${BRIDGE_PORT:-5000}"
LINKDIR="$HOME/adv4/links"
GCODE_FIFO="$LINKDIR/gcode"
CFG="$KDIR/printer.cfg"
PY="$KDIR/venv/bin/python3"
RUN="$KDIR/run"
MRDIR="/Users/admin/.moonraker-build/moonraker"
MRCONF="/Users/admin/moonraker/moonraker.conf"
UDS="/tmp/klippy_uds"
FLUIDD_PORT=8081
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$LINKDIR" "$RUN"

step(){ printf '\033[1;32m=== %s ===\033[0m\n' "$1"; }

# --- stop existing -------------------------------------------
step "stop existing ad4 processes (if any)"
"$HERE/ad4-mac-stop.sh" || true

# --- SoC bridge reachability ---------------------------------
step "check SoC bridge $SOC_IP:$BRIDGE_PORT"
if ! nc -z -w 3 "$SOC_IP" "$BRIDGE_PORT"; then
  echo "  WARN: bridge port $BRIDGE_PORT closed — klippy will retry until"
  echo "  the SoC bridge runs:  ssh root@$SOC_IP  (pw: flashforge)"
  echo "  then:  cd /opt/ad4-klipper && ./ad4-bridge $BRIDGE_PORT /dev/ttyS1 115200 &"
fi

# --- gcode FIFO + persistent writer holder --------------------
step "gcode FIFO: $GCODE_FIFO"
rm -f "$GCODE_FIFO"
mkfifo "$GCODE_FIFO"
nohup sh -c "sleep 86400 > $GCODE_FIFO" >/dev/null 2>&1 &
echo $! > "$RUN/fifo.pid"
echo "  holder pid $(cat "$RUN/fifo.pid")"

# --- klippy ---------------------------------------------------
step "start klippy (serial: socket://$SOC_IP:$BRIDGE_PORT)"
nohup "$PY" "$KDIR/klippy/klippy.py" -a "$UDS" -I "$GCODE_FIFO" \
    -l "$KDIR/klippy.log" "$CFG" > "$RUN/klippy.out" 2>&1 &
echo $! > "$RUN/klippy.pid"
sleep 4
if ! kill -0 "$(cat "$RUN/klippy.pid")" 2>/dev/null; then
  echo "  klippy exited early; log tail:"; tail -n 25 "$KDIR/klippy.log" 2>/dev/null
  exit 1
fi
echo "  klippy pid $(cat "$RUN/klippy.pid")  (API socket: $UDS)"

# --- moonraker -------------------------------------------------
step "start moonraker (:7125)"
nohup "$MRDIR/mr-venv/bin/python3" "$MRDIR/moonraker/moonraker.py" \
    -c "$MRCONF" > "$RUN/moonraker.out" 2>&1 &
echo $! > "$RUN/moonraker.pid"
sleep 2
if ! kill -0 "$(cat "$RUN/moonraker.pid")" 2>/dev/null; then
  echo "  moonraker exited early; log tail:"; tail -n 25 "$RUN/moonraker.out" 2>/dev/null
  exit 1
fi
echo "  moonraker pid $(cat "$RUN/moonraker.pid")"

# --- Fluidd ----------------------------------------------------
step "start Fluidd proxy (:${FLUIDD_PORT})"
nohup python3 "$HERE/ad4-fluidd-proxy.py" > "$RUN/fluidd-proxy.out" 2>&1 &
echo $! > "$RUN/fluidd.pid"
sleep 1
echo "  Fluidd: http://127.0.0.1:${FLUIDD_PORT}/   (strata keeps :8080)"

step "status (klippy.log tail)"
tail -n 15 "$KDIR/klippy.log" 2>/dev/null
cat <<EOF

  klippy retries the MCU until the SoC bridge answers.
  Watch:      tail -f $KDIR/klippy.log
  Expect:     'Starting Klippy...' then MCU connect (no 'timed out').
  Tune:       printf 'M105\n' > $GCODE_FIFO
  Stop:       $HERE/ad4-mac-stop.sh
EOF
