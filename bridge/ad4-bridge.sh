#!/bin/sh
#
# ad4-bridge.sh — start/stop the SoC's serial<->network bridge to the MCU.
# Run on the SoC as root. Zero soldering: the SoC is already wired to the MCU
# (uart0 == /dev/ttyS1). This frees the MCU from the vendor app and exposes it
# over the SoC's Ethernet port (eth0) for a Klipper host (RPi).
#
# Usage:
#   ./ad4-bridge.sh start      # stop vendor stack, start TCP bridge on 5000
#   ./ad4-bridge.sh stop       # stop the bridge, (optionally) restart vendor
#   ./ad4-bridge.sh status
#   ./ad4-bridge.sh start --udp [--port 5000] [--uart /dev/ttyS1] [--baud 250000]
#
# The RPi side runs socat to present this as a local PTY — see rpi-setup.sh.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BRIDGE="$HERE/ad4-serial-bridge.py"
PIDF=/var/run/ad4-bridge.pid
LOG=/var/log/ad4-bridge.log

ad4_stop_vendor() {
    # Stop the vendor stack so nothing else owns /dev/ttyS1.
    pkill -f ffstartup-arm    2>/dev/null
    pkill -f adventurer4-arm  2>/dev/null
    fuser -k /dev/ttyS1       2>/dev/null
    sleep 1
    echo "[ad4-bridge] vendor stack stopped; /dev/ttyS1 freed"
}

ad4_bridge_stop() {
    if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        kill "$(cat "$PIDF")" 2>/dev/null
        sleep 1
    fi
    rm -f "$PIDF"
    echo "[ad4-bridge] bridge stopped"
}

cmd="${1:-status}"
case "$cmd" in
  start)
    shift
    ad4_stop_vendor
    [ -x "$BRIDGE" ] || { echo "ERROR: $BRIDGE missing". >&2; exit 1; }
    # default to TCP; pass --udp through
    EXTRA=""
    for a in "$@"; do EXTRA="$EXTRA $a"; done
    nohup python3 "$BRIDGE" $EXTRA >>"$LOG" 2>&1 &
    echo $! > "$PIDF"
    sleep 1
    if kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        echo "[ad4-bridge] running (pid $(cat "$PIDF"))"
        tail -n 3 "$LOG"
    else
        echo "[ad4-bridge] FAILED to start; log:"; cat "$LOG"; exit 1
    fi
    ;;
  stop)
    ad4_bridge_stop
    # optionally bring the vendor stack back so the stock UI works again
    if [ "${AD4_RESTART_VENDOR:-0}" = "1" ]; then
        echo "[ad4-bridge] restarting vendor stack..."
        /opt/ffstartup-arm/ffstartup-arm 2>/dev/null &  # best-effort
    fi
    ;;
  status)
    if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
        echo "[ad4-bridge] RUNNING (pid $(cat "$PIDF"))"; tail -n 5 "$LOG"
    else
        echo "[ad4-bridge] not running"
    fi
    ;;
  *)
    echo "usage: $0 {start [--udp] [--port N] [--uart DEV] [--baud B] | stop | status}"
    ;;
esac
