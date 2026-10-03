#!/bin/sh
# =============================================================
# build-usb-stick.sh — assemble the AD4 Klipper USB flash stick.
#
# Run this on your build machine (the Mac). It copies the exact set of
# files the SoC boot path needs onto a FAT32 USB stick, so that
# "plug stick in + power the printer" does the whole flash + SoC hack
# automatically (see flashforge_init.sh).
#
# Usage:
#   ./build-usb-stick.sh /Volumes/MYSTICK
#
# The stick must already be formatted FAT32 and mounted. On macOS, unplug
# and re-plug after building (FAT does not preserve the +x bit, but the SoC
# chmods the script at boot).
#
# What lands on the stick (all at the disk root):
#   flashforge_init.sh   the SoC-side flash+net+bridge script
#   screwflag            empty file (skips machine-type check)
#   IAPCommand           factory ARM flasher (verified md5; does the GPIO strapping)
#   klipper.hex          positioned Klipper @ 0x000C0000 (type-05 iHEX)
#   factory-HD.hex       known-good full restore image
#   ad4-serial-bridge.py MCU /dev/ttyS1 <-> TCP :5000 bridge
#   ad4-net.conf         SOC_IP=192.168.10.10  BRIDGE_PORT=5000
#   ad4-daemon.sh        persistent Klipper runtime (eMMC)
#   ad4-install-persistent.sh    eMMC boot-hook installer (runs as root)
#   ad4-uninstall-persistent.sh  full revert to stock
#   PERSISTENT           (default ON) empty flag -> install persistent hook.
#                        Build with NO_PERSISTENT=1 to make a one-shot stick.
# =============================================================
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
STICK="${1:-}"
[ -n "$STICK" ] || { echo "usage: $0 <mounted-stick-path>"; exit 1; }
[ -d "$STICK" ] || { echo "ERROR: $STICK not a directory (mount the stick first)"; exit 1; }

# --- sources --------------------------------------------------
IAP_SRC="$REPO/work/ctrl/IAPCommand"
KIP_HEX="$REPO/stm32isp-ad4/klipper-ad4.hex"
FAC_HEX="$REPO/stm32isp-ad4/Adventurer4-HD-IAP-2.3.0-20230216.hex"
BRIDGE="$REPO/soc-hack/ad4-serial-bridge.py"
INIT="$REPO/flashdisk/flashforge_init.sh"

[ -x "$IAP_SRC" ] || { echo "ERROR: $IAP_SRC missing"; exit 1; }
[ -f "$KIP_HEX" ] || { echo "ERROR: $KIP_HEX missing (run the build)"; exit 1; }
[ -f "$FAC_HEX" ] || { echo "ERROR: $FAC_HEX missing"; exit 1; }
[ -f "$BRIDGE" ]  || { echo "ERROR: $BRIDGE missing"; exit 1; }
[ -f "$INIT" ]    || { echo "ERROR: $INIT missing"; exit 1; }

# --- verify the flasher is the known-good factory binary ------
MD5_WANT="919864f702cf3c07665234ab8de7896e"
MD5_GOT="$(md5 -q "$IAP_SRC" 2>/dev/null || md5sum "$IAP_SRC" | cut -d' ' -f1)"
[ "$MD5_GOT" = "$MD5_WANT" ] || {
  echo "ERROR: IAPCommand md5 $MD5_GOT != $MD5_WANT (refusing to build stick)"; exit 1; }
echo "IAPCommand md5 OK: $MD5_GOT"

# --- copy -----------------------------------------------------
echo "==> building stick at $STICK"
cp -f "$INIT"   "$STICK/flashforge_init.sh"
cp -f "$IAP_SRC" "$STICK/IAPCommand"
cp -f "$KIP_HEX" "$STICK/klipper.hex"
cp -f "$FAC_HEX" "$STICK/factory-HD.hex"
cp -f "$BRIDGE"  "$STICK/ad4-serial-bridge.py"
# persistent runtime package
cp -f "$HERE/ad4-daemon.sh"               "$STICK/ad4-daemon.sh"
cp -f "$HERE/ad4-install-persistent.sh"   "$STICK/ad4-install-persistent.sh"
cp -f "$HERE/ad4-uninstall-persistent.sh" "$STICK/ad4-uninstall-persistent.sh"
: > "$STICK/screwflag"
cat > "$STICK/ad4-net.conf" <<'EOF'
# AD4 SoC network (static; the RPi host is 192.168.10.1)
SOC_IP=192.168.10.10
SOC_NET=24
BRIDGE_PORT=5000
EOF
# persistent install flag: ON by default (durable). NO_PERSISTENT=1 makes a
# one-shot stick (works for the power window only, no eMMC changes).
if [ "${NO_PERSISTENT:-}" = "1" ]; then
  rm -f "$STICK/PERSISTENT"
  echo "one-shot stick (no eMMC changes)"
else
  : > "$STICK/PERSISTENT"
  echo "persistent stick (eMMC boot hook, survives power)"
fi
chmod +x "$STICK/flashforge_init.sh" 2>/dev/null || true

echo
echo "==> stick contents:"
ls -la "$STICK"
echo
echo "DONE. Next:"
echo "  1. (Optional) revert to stock any time:  touch /path/to/stick/RESTORE"
echo "  2. Plug the stick into the AD4, power it on. It flashes + sets up"
echo "     the SoC network (192.168.10.10) + starts the MCU bridge on :5000."
echo "  3. On the RPi3 (192.168.10.1):  ./ad4-rpi-setup.sh"
