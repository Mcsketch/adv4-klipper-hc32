#!/bin/bash
# =============================================================
# ad4-mac-setup.sh — one-shot MAC Klipper-host setup for the
# Adventurer 4 (MCU reached over Ethernet via the SoC C bridge).
#
# Runs on the Mac. After the USB stick has flashed the MCU and the
# SoC bridge is up on :5000, this:
#   1. Sets the wired NIC to static 192.168.10.1/24 (SoC is .10).
#   2. Preflights the link (ping SoC, open :5000).
#   3. Prepares the Klipper venv + chelper (if not done) and the
#      printer cfg pointing [mcu] at /dev/adv4_mcu.
#   4. Starts the socat PTY (SoC:5000 -> /dev/adv4_mcu) + klippy.
#
# Idempotent: safe to re-run.
#
# Usage:
#   ./ad4-mac-setup.sh            # set NIC + preflight + start
#   ./ad4-mac-setup.sh --no-nic   # skip NIC reconfig (already cabled/IP'd)
# =============================================================
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
KDIR="/Users/admin/klipper-hc32"
SOC_IP="192.168.10.10"
MAC_IP="192.168.10.1"
BRIDGE_PORT="5000"
LINKDIR="$HOME/adv4/links"
PTY="$LINKDIR/mcu"
CFG="$KDIR/printer.cfg"
PY="$KDIR/venv/bin/python"

step(){ printf '\n\033[1;32m=== %s ===\033[0m\n' "$1"; }
die(){  printf '\033[1;31mERROR: %s\033[0m\n' "$1"; exit 1; }

# --- 0. basics ------------------------------------------------
command -v socat >/dev/null 2>&1 || die "socat missing (brew install socat)"
[ -d "$KDIR" ] || die "Klipper repo not at $KDIR"
[ -x "$PY" ]   || die "venv missing (run klippy setup first)"

# --- 1. NIC static IP -----------------------------------------
if [ "${1:-}" != "--no-nic" ]; then
  step "1. Static IP on the wired NIC ($MAC_IP/24, SoC at $SOC_IP)"
  # pick a non-loopback interface; user can override with IFACE=en0
  IFACE="${IFACE:-$(ifconfig -a | awk '/^[a-z]+: /{gsub(":","",$1); print $1}' | grep -vE '^(lo|utun|awdl|bridge|en[0-9])' | head -1)}"
  # fall back to en0/en1
  [ -n "$IFACE" ] || IFACE="en0"
  echo "Using interface: $IFACE"
  sudo ifconfig "$IFACE" inet $MAC_IP/24 up
  sleep 1
  ipconfig getifaddr "$IFACE" 2>/dev/null
else
  step "1. (skipped: --no-nic)"
fi

# --- 2. preflight --------------------------------------------
step "2. Preflight: reach the SoC at $SOC_IP:$BRIDGE_PORT"
ok=0
for i in $(seq 1 10); do
  ping -c1 -t1 "$SOC_IP" >/dev/null 2>&1 && { ok=1; break; }
  sleep 1
done
[ "$ok" = "1" ] || die "cannot reach $SOC_IP (cable in? stick inserted & printer on? NIC IP set?)"
echo "SoC reachable at $SOC_IP"
if nc -z "$SOC_IP" "$BRIDGE_PORT" 2>/dev/null; then
  echo "SoC bridge port :$BRIDGE_PORT open"
else
  echo "WARNING: :$BRIDGE_PORT not open yet (bridge may still be starting). Continuing."
fi

# --- 3. klippy deps (idempotent) ------------------------------
step "3. Klippy deps + chelper"
"$PY" -c "import greenlet,cffi,serial" 2>/dev/null || "$KDIR/venv/bin/pip" install -q greenlet cffi pyserial flask
# build c_helper.so if missing (uses the macos-compat stubs)
if [ ! -f "$KDIR/klippy/chelper/c_helper.so" ]; then
  ( cd "$KDIR/klippy/chelper" && \
    gcc -Wall -g -O2 -shared -fPIC -Imacos-compat -o c_helper.so \
      pyhelper.c serialqueue.c stepcompress.c steppersync.c itersolve.c \
      trapq.c pollreactor.c msgblock.c trdispatch.c kin_cartesian.c \
      kin_corexy.c kin_corexz.c kin_delta.c kin_deltesian.c kin_polar.c \
      kin_rotary_delta.c kin_winch.c kin_extruder.c kin_shaper.c \
      kin_idex.c kin_generic.c ) || die "chelper build failed"
  echo "built c_helper.so"
fi
[ -f "$KDIR/klippy/chelper/c_helper.so" ] && echo "c_helper.so present"

# --- 4. printer cfg (use repo HC32 pinout, redirect serial) ----
step "4. Printer config -> $PTY"
SRC_CFG="$REPO/printer-hc32.cfg"
if [ -f "$SRC_CFG" ]; then cp -f "$SRC_CFG" "$CFG"; echo "copied $SRC_CFG"; fi
[ -f "$CFG" ] || { printf '[printer]\nkinematics: none\n\n[mcu]\nserial: %s\nbaud: 115200\n' "$PTY" > "$CFG"; }
# copy the include files printer-hc32.cfg references (adv3-macros, fluidd, KAMP)
# they live under work/kv-next/config/ in the repo; klippy resolves them relative
# to printer.cfg, so they must sit next to it.
INCLUDES="$REPO/work/kv-next/config"
if [ -d "$INCLUDES" ]; then
  for inc in adv3-macros.cfg fluidd.cfg KAMP_Settings.cfg KAMP; do
    [ -e "$INCLUDES/$inc" ] && cp -rf "$INCLUDES/$inc" "$(dirname "$CFG")/" && echo "  copied include: $inc"
  done
fi
# redirect [mcu] serial + baud (PTy baud is cosmetic; real 250000 on SoC)
awk -v pty="$PTY" '
  /^\[mcu\]/{inmcu=1; print; next}
  /^\[/{inmcu=0}
  inmcu && /^serial:/{print "serial: " pty; next}
  inmcu && /^baud:/{print "baud: 115200"; next}
  {print}
' "$CFG" > "$CFG.tmp" && mv "$CFG.tmp" "$CFG"
echo "[mcu] serial: $PTY  (PTY baud 115200 cosmetic; SoC enforces 250000)"

# --- 5. start -------------------------------------------------
"$HERE/ad4-mac-start.sh"
step "DONE — klippy + PTY running. Tune with:  $HERE/ad4-mac-tune.py"
