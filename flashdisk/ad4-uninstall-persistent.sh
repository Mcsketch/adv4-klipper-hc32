#!/bin/sh
# =============================================================
# ad4-uninstall-persistent.sh — revert the SoC to STOCK.
#
# Run on the SoC as root (or via the disk path with a RESTORE flag).
# It is the exact inverse of ad4-install-persistent.sh:
#   1. Remove the enable flag (so rcS runs the vendor launch).
#   2. Restore the original rcS from its .ad4-orig backup.
#   3. Stop the Klipper daemon + bridge.
#   4. (If a factory hex + IAPCommand are present) re-flash the MCU
#      to the stock image, so the whole system is factory-clean.
#   5. Reboot.
#
# Usage: ad4-uninstall-persistent.sh [--no-mcu]
#   --no-mcu: skip the MCU re-flash (use when flashforge_init.sh just
#             flashed the factory image, so we don't double-flash).
#
# This removes only OUR files (/opt/ad4-klipper) and restores the
# vendor rcS line. Nothing else on the SoC is touched.
# =============================================================
set -x
BASE="/opt/ad4-klipper"
[ "$1" = "--no-mcu" ] && SKIP_MCU=1

# 1) disable
rm -f "$BASE/enable" 2>/dev/null

# 2) restore the original rcS
FFLINE='ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg'
MARKER="/opt/ad4-klipper/ad4-daemon.sh"
RCS=""
for c in /etc/init.d/rcS /etc/rcS /etc/rc.local /etc/inittab /etc/init.d/rc; do
  [ -f "$c" ] || continue
  if grep -qF "$MARKER" "$c" 2>/dev/null; then RCS="$c"; break; fi
done
[ -z "$RCS" ] && RCS="$(grep -rlF "$MARKER" /etc/init.d/ /etc/ 2>/dev/null | head -1)"
if [ -n "$RCS" ] && [ -f "$RCS" ]; then
  if [ -f "${RCS}.ad4-orig" ]; then
    cp -f "${RCS}.ad4-orig" "$RCS"
  else
    # no backup: unwrap our if/else line back to the plain vendor launch
    python3 - "$RCS" <<'PY'
import sys
rc=sys.argv[1]
txt=open(rc,errors="ignore").read()
import re
txt=re.sub(r'if \[ -f /opt/ad4-klipper/enable \]; then .*?ffstartup-arm -f /opt/PROGRAM/ffstartup\.cfg[^&]*&?[^f]*fi[ \t]*\n',
           '  /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg &\n', txt)
open(rc,"w").write(txt)
PY
  fi
  echo "restored $RCS"
fi

# 3) stop our runtime
pkill -f ad4-klipper/ad4-daemon.sh 2>/dev/null
pkill -f ad4-serial-bridge.py 2>/dev/null

# 4) re-flash MCU to stock (best effort, needs the files; skip if we
#    just flashed factory)
IAP="$BASE/IAPCommand"
FACTORY="$BASE/factory-HD.hex"
[ -f "$FACTORY" ] || FACTORY="/mnt/factory-HD.hex"
if [ "$SKIP_MCU" != "1" ] && [ -x "$IAP" ] && [ -f "$FACTORY" ]; then
  pkill -f ffstartup-arm 2>/dev/null
  sleep 1
  "$IAP" "$FACTORY" "$FACTORY" && echo "MCU restored to stock" || echo "MCU restore failed"
fi

# 5) remove our package (optional; keeps things tidy). Kept so a
#    re-enable is a one-file restore.
# rm -rf "$BASE"   # uncomment to fully remove the Klipper package

sync
echo "uninstall complete; rebooting to stock."
sleep 2
reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
exit 0
