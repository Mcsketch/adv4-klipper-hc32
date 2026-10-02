#!/bin/bash
# =============================================================
# ad4-rpi-setup.sh — one-shot RPi3 Klipper-host setup for the
# Adventurer 4 (MCU reached over Ethernet via the SoC bridge).
#
# Run ON the RPi3 as root, after the USB stick has already flashed
# the MCU and started the SoC bridge. This:
#   1. Sets the wired NIC to a static 192.168.10.1/24 (SoC is .10).
#   2. Preflights the link (ping the SoC, open :5000).
#   3. Installs deps, enables the HW UART, clones + builds Klipper.
#   4. Drops in the printer cfg pointing [mcu] at /dev/adv4_mcu.
#   5. Installs a systemd unit that presents SoC:5000 as a local PTY.
#   6. Enables klipper + the PTY; optionally Moonraker+Fluidd.
#
# Usage:
#   sudo ./ad4-rpi-setup.sh              # through to Klipper
#   sudo ./ad4-rpi-setup.sh --moonraker  # also install Moonraker+Fluidd
#   sudo ./ad4-rpi-setup.sh --wifi       # (advanced) keep wifi instead of eth
#
# Re-runnable: it is idempotent (checks before each step).
# =============================================================
set -euo pipefail

SOC_IP="192.168.10.10"
PI_IP="192.168.10.1"
NETMASK="/24"
BRIDGE_PORT="5000"
PTY="/dev/adv4_mcu"
SVC_PTY="ad4-mcu-pty"
KDIR="/opt/klipper"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

step() { printf '\n\033[1;32m=== %s ===\033[0m\n' "$1"; }

# --- 0. root check -------------------------------------------
[ "$(id -u)" = "0" ] || { echo "ERROR: run as root (sudo)"; exit 1; }

# --- 1. static IP on the wired NIC ---------------------------
step "1. Static IP on wired NIC ($PI_IP$NETMASK, SoC at $SOC_IP)"
# Detect the active wired interface (eth0 on modern Pi OS; enxf... on systemd-udev).
IFACE="$(ls /sys/class/net | grep -E '^(eth|enx|end)' | grep -v lo | head -1 || true)"
[ -n "$IFACE" ] || { echo "ERROR: no wired NIC found (is the cable plugged in?)"; exit 1; }
echo "Using wired interface: $IFACE"

# Disable any DHCP on it via systemd-networkd (present on Bookworm) if available.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active systemd-networkd >/dev/null 2>&1; then
  cat > "/etc/systemd/network/10-ad4-ad4link.network" <<EOF
[Match]
Name=$IFACE

[Network]
Address=$PI_IP$NETMASK
EOF
  systemctl restart systemd-networkd || true
fi
# Belt-and-suspenders: drop + add the address directly (works even without networkd).
ip addr flush dev "$IFACE" 2>/dev/null || true
ip addr add "$PI_IP$NETMASK" dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# --- 2. preflight the link -----------------------------------
step "2. Preflight: reach the SoC at $SOC_IP:$BRIDGE_PORT"
ok=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  if ping -c1 -W1 "$SOC_IP" >/dev/null 2>&1; then ok=1; break; fi
  sleep 1
done
if [ "$ok" = "1" ]; then
  echo "SoC reachable at $SOC_IP"
else
  echo "ERROR: cannot reach $SOC_IP."
  echo "  - Is the USB stick still in the printer and it powered on? (it sets the SoC IP)"
  echo "  - Is the Ethernet cable in the RPi's $IFACE and the printer's ethernet port?"
  exit 1
fi
# Confirm the bridge port is open (the flash script starts it on :5000).
if (exec 3<>/dev/tcp/"$SOC_IP"/"$BRIDGE_PORT") 2>/dev/null; then
  echo "SoC bridge port :$BRIDGE_PORT open"
else
  echo "WARNING: :$BRIDGE_PORT not open yet — the SoC bridge may still be starting."
  echo "  (Continue, but Klipper will retry the MCU until the bridge is up.)"
fi

# --- 3. deps + UART ------------------------------------------
step "3. Install deps + enable HW UART"
apt-get update -qq
apt-get install -y -qq python3-pip python3-venv git build-essential socat screen make gcc
# Free the HW UART from Bluetooth.
CFG=/boot/firmware/config.txt
[ -f "$CFG" ] || CFG=/boot/config.txt
grep -q 'enable_uart' "$CFG" || printf 'enable_uart=1\ndtoverlay=disable-bt\n' >> "$CFG"
echo "  (if you changed config.txt, reboot later to apply UART; not needed for the eth bridge)"

# --- 4. Klipper ----------------------------------------------
step "4. Clone + build Klipper (HC32F460 target)"
if [ ! -d "$KDIR" ]; then
  git clone --depth 1 https://github.com/Klipper3d/klipper "$KDIR"
fi
( cd "$KDIR" && ./scripts/install-linux.sh >/tmp/klipper-install.log 2>&1 || {
    echo "Klipper install-linux failed; tail of log:"; tail -n 20 /tmp/klipper-install.log; exit 1; } )
echo "Klipper installed at $KDIR"

# --- 5. printer config ---------------------------------------
step "5. Install printer config ([mcu] -> $PTY)"
ETC=/etc/klippy
mkdir -p "$ETC"
if [ -f "$HERE/../printer-hc32.cfg" ]; then
  cp -f "$HERE/../printer-hc32.cfg" "$ETC/printer.cfg"
  echo "  copied repo printer-hc32.cfg"
elif [ ! -f "$ETC/printer.cfg" ]; then
  cat > "$ETC/printer.cfg" <<EOF
[mcu]
serial: $PTY
baud: 250000

[printer]
kinematics: none
EOF
  echo "  wrote a minimal [mcu]-only config (no kinematics yet)"
fi
# Ensure [mcu] points at the PTY regardless of the source cfg.
if grep -q '^\[mcu\]' "$ETC/printer.cfg"; then
  sed -i -E "s#^serial:.*#serial: $PTY#" "$ETC/printer.cfg"
  sed -i -E "s#^baud:.*#baud: 250000#" "$ETC/printer.cfg"
else
  sed -i "1i [mcu]\nserial: $PTY\nbaud: 250000\n" "$ETC/printer.cfg"
fi
echo "  [mcu] serial: $PTY  (SoC bridge PTY)"

# --- 6. socat PTY systemd unit --------------------------------
step "6. Install systemd unit: SoC:$BRIDGE_PORT -> $PTY (socat)"
cat > "/etc/systemd/system/$SVC_PTY.service" <<EOF
[Unit]
Description=Adventurer 4 MCU serial-over-ethernet PTY (socat -> $PTY)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStartPre=/bin/sh -c 'socat -u TCP4:$SOC_IP:$BRIDGE_PORT,connect-timeout=2 - - 2>/dev/null | head -c1 >/dev/null || true'
ExecStart=/usr/bin/socat TCP4:$SOC_IP:$BRIDGE_PORT,reconnect,keepalive,nodelay OPEN:$PTY,raw,echo=0,nonblock
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload

# --- 7. enable services --------------------------------------
step "7. Enable + start services"
systemctl enable --now "$SVC_PTY"
# klipper may be installed by install-linux.sh as a service; enable it.
if [ -f /lib/systemd/system/klipper.service ] || systemctl cat klipper.service >/dev/null 2>&1; then
  systemctl enable --now klipper
else
  echo "  (klipper.service not found; install-linux.sh may use a different unit name — check with: systemctl list-units | grep -i klipper)"
fi

# --- 8. Moonraker + Fluidd (optional) ------------------------
if [ "${1:-}" = "--moonraker" ]; then
  step "8. Moonraker + Fluidd"
  /opt/klippy-env/bin/python3 -m venv /opt/moonraker-env
  /opt/moonraker-env/bin/pip install --upgrade pip
  /opt/moonraker-env/bin/pip install moonraker fluidd
  echo "  Moonraker installed. Configure /etc/moonraker/moonraker.conf, then:"
  echo "    systemctl enable --now moonraker"
  echo "  Fluidd is reachable at http://$PI_IP:8080 (after you enable the web server)."
fi

step "DONE"
cat <<EOF

  First-contact test (from the RPi3):
    1) systemctl status ad4-mcu-pty klipper
    2) tail -f /var/log/klippy.log        # expect MCU to connect, no "timed out"
    3) In Fluidd/Moonraker UI: FIRMWARE_RESTART, then G28 (home) -> G1 X10 F300

  If the MCU does not connect:
    - Check the SoC bridge is up:  the USB flash script started it on :$BRIDGE_PORT
    - Check the link:  ping $SOC_IP
    - Check [mcu] serial:  grep -A2 '\[mcu\]' $ETC/printer.cfg

  To restore the STOCK printer (on the SoC / via the USB stick):
    - Put a 'RESTORE' file on the USB stick, re-plug, power the printer.
    - Or on the SoC:  <repo>/stm32isp-ad4/flash-mcu.sh factory
EOF
