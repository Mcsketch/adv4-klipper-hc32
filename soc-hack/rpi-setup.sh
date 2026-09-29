#!/bin/bash
#
# rpi-setup.sh — configure a Raspberry Pi as the Klipper host for the Adventurer 4,
# reaching the MCU through the SoC's serial<->network bridge (zero soldering).
#
# Topology:
#   RPi (this)  --ethernet-->  SoC (ad4-serial-bridge on :5000)  --UART-->  MCU
#   RPi presents the remote MCU as a local PTY:  /dev/adv4_mcu  (via socat)
#   Klipper:  [mcu] serial: /dev/adv4_mcu
#
# Run on the RPi as root.
#
# Usage:
#   ./rpi-setup.sh <SOC_IP>
#   e.g. ./rpi-setup.sh 192.168.1.20
#
# Then:  ./rpi-setup.sh start-pty   (bring up the socat PTY, e.g. in a systemd unit)
#         ./rpi-setup.sh stop-pty

set -eu

SOC_IP="${1:-}"
SOC_PORT="${SOC_PORT:-5000}"
PTY="/dev/adv4_mcu"
SVC=ad4-mcu-pty
KDIR=/opt/klipper

show_topology() {
cat <<EOF
  [RPi Klippy] --(/dev/adv4_mcu PTY via socat)--(ethernet)--[SoC bridge :$SOC_PORT]--(UART)--[MCU]

  SoC must be running:  ad4-bridge.sh start      (TCP on :$SOC_PORT)
  RPi PTY:             $PTY  (socat)
  Klipper [mcu]:        serial: $PTY   baud: 250000
EOF
}

need_ip() {
  if [ -z "$SOC_IP" ]; then
    echo "ERROR: pass the SoC IP.  Example:  $0 192.168.1.20" >&2
    exit 1
  fi
}

cmd="${2:-all}"
case "$cmd" in
  all)
    need_ip
    show_topology
    echo "=== 1. base packages ==="
    apt-get update
    apt-get -y install python3-pip python3-venv git build-essential socat screen
    echo "=== 2. enable hardware UART (free it from Bluetooth) ==="
    if grep -q 'enable_uart' /boot/config.txt 2>/dev/null || grep -q 'enable_uart' /boot/firmware/config.txt 2>/dev/null; then :; else
      # modern Raspberry Pi OS uses /boot/firmware/config.txt
      for f in /boot/firmware/config.txt /boot/config.txt; do
        [ -f "$f" ] && printf 'enable_uart=1\ndtoverlay=disable-bt\n' >> "$f"
      done
    fi
    echo "  (reboot to apply UART changes, if you changed them)"
    echo "=== 3. Klipper (Moonraker/Fluidd optional, commented below) ==="
    if [ ! -d "$KDIR" ]; then
      git clone https://github.com/Klipper3d/klipper "$KDIR"
      ( cd "$KDIR" && ./scripts/install-linux.sh )
    fi
    echo "  -> point [mcu] serial at $PTY in /etc/klippy/printer.cfg"
    echo "  -> Moonraker + Fluidd:  /opt/klippy-env/bin/python3 -m venv /opt/moonraker-env ; ..."
    echo "=== 4. systemd unit for the socat PTY (ad4-mcu-pty.service) ==="
    install_pty_unit
    echo "  enable + start:  systemctl enable --now $SVC"
    echo
    echo "=== NEXT ==="
    echo "  1) On the SoC:  ad4-bridge.sh start"
    echo "  2) On this RPi: systemctl enable --now $SVC"
    echo "  3) Edit /etc/klippy/printer.cfg:  [mcu] serial: $PTY"
    echo "  4) systemctl restart klipper   ->   FIRMWARE_RESTART"
    ;;
  install-pty)
    need_ip
    install_pty_unit
    echo "now: systemctl enable --now $SVC"
    ;;
  start-pty)
    need_ip
    systemctl enable --now "$SVC"
    sleep 1
    systemctl status "$SVC" --no-pager
    ;;
  stop-pty)
    systemctl stop "$SVC" || true
    ;;
  *)
    echo "usage: $0 <SOC_IP> {all|install-pty|start-pty|stop-pty}"
    ;;
esac

install_pty_unit() {
  local u="/etc/systemd/system/$SVC.service"
  cat > "$u" <<EOF
[Unit]
Description=Adventurer 4 MCU serial-over-ethernet PTY (socat -> $PTY)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
# Present the SoC TCP bridge ($SOC_IP:$SOC_PORT) as a local serial PTY.
ExecStart=/usr/bin/socat TCP4:$SOC_IP:$SOC_PORT,reconnect,keepalive,nodelay OPEN:$PTY,raw,echo=0,nonblock
Restart=on-failure
RestartSec=2
EOF
  # If you ran the SoC bridge with --udp instead, use this line:
  #   ExecStart=/usr/bin/socat UDP4:$SOC_IP:$SOC_PORT OPEN:$PTY,raw,echo=0,nonblock
  echo "wrote $u"
}
