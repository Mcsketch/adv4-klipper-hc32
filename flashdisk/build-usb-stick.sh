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
#   flashforge_init.sh   the SoC-side flash+net+bridge+SSH script
#   screwflag            empty file (skips machine-type check)
#   IAPCommand           factory ARM flasher (verified md5; does the GPIO strapping)
#   klipper.hex          positioned Klipper @ 0x0000C000 (type-04 iHEX).
#                        Must be built PA9/PA10 (USART2) @115200 — the
#                        proven vendor SoC-MCU line (session-5 firmware RE);
#                        regenerate it with
#                        tools/make-positioned-ihex.py out/klipper.bin 0xC000
#                        after every Klipper rebuild.
#   factory-HD.hex       known-good full restore image
#   ad4-bridge           static C (zig) MCU /dev/ttyS1 <-> TCP :5000 bridge (no python)
#   dropbear             static ARM dropbearmulti (dropbear/dropbearkey/scp/ssh)
#   ad4-net.conf         SOC_IP=192.168.10.10  BRIDGE_PORT=5000
#   ad4-daemon.sh        persistent Klipper runtime (eMMC)
#   ad4-install-persistent.sh    eMMC boot-hook installer (runs as root)
#   ad4-uninstall-persistent.sh  full revert to stock
#   PERSISTENT           (default ON) empty flag -> install persistent hook.
#                        Build with NO_PERSISTENT=1 to make a one-shot stick.
#   NO_FLASH             (optional) empty flag -> do NOT touch the MCU.
#                        Build with NO_FLASH=1 to make a "SoC access + bridge
#                        only" stick (no MCU re-flash).
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
INIT="$REPO/flashdisk/flashforge_init.sh"
PLAY_BIN_SRC="$REPO/play"
BRIDGE_C="$REPO/soc-hack/ad4-bridge"
DROPBEAR_SRC="$REPO/flashdisk/dropbear"

[ -x "$IAP_SRC" ] || { echo "ERROR: $IAP_SRC missing"; exit 1; }
[ -f "$KIP_HEX" ] || { echo "ERROR: $KIP_HEX missing (run the build)"; exit 1; }
[ -f "$FAC_HEX" ] || { echo "ERROR: $FAC_HEX missing"; exit 1; }
[ -f "$INIT" ]    || { echo "ERROR: $INIT missing"; exit 1; }
[ -f "$PLAY_BIN_SRC" ] || { echo "WARNING: 'play' (buzzer) $PLAY_BIN_SRC missing - chime will use sysfs fallback"; }
[ -x "$BRIDGE_C" ] || { echo "ERROR: C bridge $BRIDGE_C missing (rebuild: zig cc ...)" ; exit 1; }
[ -f "$DROPBEAR_SRC" ] || { echo "WARNING: dropbear $DROPBEAR_SRC missing - SSH install will be skipped"; }

# --- verify the flashers --------------------------------------
MD5_WANT="919864f702cf3c07665234ab8de7896e"
MD5_GOT="$(md5 -q "$IAP_SRC" 2>/dev/null || md5sum "$IAP_SRC" | cut -d' ' -f1)"
[ "$MD5_GOT" = "$MD5_WANT" ] || {
  echo "ERROR: IAPCommand md5 $MD5_GOT != $MD5_WANT (refusing to build stick)"; exit 1; }
echo "IAPCommand md5 OK: $MD5_GOT"
# dropbear: must be a static ARM ELF (runs on the SoC, no dynamic linker).
if [ -f "$DROPBEAR_SRC" ]; then
  DB_KIND="$(file -b "$DROPBEAR_SRC" 2>/dev/null)"
  case "$DB_KIND" in
    *"ARM"*) echo "dropbear OK: $DB_KIND" ;;
    *) echo "WARNING: dropbear is not an ARM binary ($DB_KIND) - SSH install will fail"; ;;
  esac
fi
# play (buzzer): must be an ARM ELF that can run on the SoC.
if [ -f "$PLAY_BIN_SRC" ]; then
  PLAY_KIND="$(file -b "$PLAY_BIN_SRC" 2>/dev/null)"
  case "$PLAY_KIND" in
    *"ARM"*) echo "play (buzzer) OK: $PLAY_KIND" ;;
    *) echo "WARNING: play is not an ARM binary ($PLAY_KIND) - chime will use sysfs fallback"; ;;
  esac
fi

# --- copy -----------------------------------------------------
echo "==> building stick at $STICK"
cp -f "$INIT"   "$STICK/flashforge_init.sh"
[ -f "$PLAY_BIN_SRC" ] && cp -f "$PLAY_BIN_SRC" "$STICK/play" && chmod +x "$STICK/play" 2>/dev/null
[ -x "$BRIDGE_C" ] && cp -f "$BRIDGE_C" "$STICK/ad4-bridge" && chmod +x "$STICK/ad4-bridge" 2>/dev/null
[ -f "$DROPBEAR_SRC" ] && cp -f "$DROPBEAR_SRC" "$STICK/dropbear" && chmod +x "$STICK/dropbear" 2>/dev/null
cp -f "$IAP_SRC" "$STICK/IAPCommand"
cp -f "$KIP_HEX" "$STICK/klipper.hex"
cp -f "$FAC_HEX" "$STICK/factory-HD.hex"
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
# NO_FLASH flag: skip MCU flashing entirely (SoC access + bridge only).
if [ "${NO_FLASH:-}" = "1" ]; then
  : > "$STICK/NO_FLASH"
  echo "NO_FLASH stick (will NOT touch the MCU)"
else
  rm -f "$STICK/NO_FLASH"
fi
# Never leave a stale RESTORE flag behind.
rm -f "$STICK/RESTORE"
chmod +x "$STICK/flashforge_init.sh" 2>/dev/null || true

# --- strip macOS metadata (constraint: no ._*/.DS_Store on the SoC) ----
# macOS auto-writes AppleDouble '._*' sidecars when copying to FAT/exFAT;
# remove them (plus .Spotlight/.fseventsd/.DS_Store) so the stick stays clean.
find "$STICK" -name '._*' -delete 2>/dev/null || true
rm -f  "$STICK/.DS_Store" 2>/dev/null || true
rm -rf "$STICK/.Spotlight-V100" "$STICK/.fseventsd" 2>/dev/null || true

echo
echo "==> stick contents:"
ls -la "$STICK"
echo
echo "DONE. Next:"
echo "  1. Plug the stick into the AD4, power it on. It will:"
echo "     - (re)flash the MCU with Klipper (skipped if NO_FLASH flag is set)"
echo "     - set the SoC eth0 to 192.168.10.10"
echo "     - start the C bridge on :5000"
echo "     - install dropbear SSH (root pw = flashforge) and persist it"
echo "  2. On the Mac (eth0 = 192.168.10.1):"
echo "     ssh root@192.168.10.10   (password: flashforge)"
echo "     cd ~/adv4-klip/soc-hack && ./ad4-mac-start.sh"
echo "  3. To skip the MCU flash next time: rebuild with NO_FLASH=1, or:"
echo "     touch /Volumes/FLASHFORGE/NO_FLASH  (before powering on)"
echo "  4. To restore factory MCU firmware any time:"
echo "     touch /Volumes/FLASHFORGE/RESTORE  (before powering on)"
