#!/bin/sh
# =============================================================
# ad4-uninstall-persistent.sh — revert the SoC to STOCK.
#
# Run on the SoC as root (or via the disk path with a RESTORE flag).
# It is the exact inverse of ad4-install-persistent.sh:
#   1. Remove the enable flag (so rcS runs the vendor launch).
#   2. Restore the original rcS from its .ad4-orig backup.
#   3. Stop the Klipper daemon + bridge.
#   4. Re-flash the MCU to the stock image: with the vendor IAPCommand kit
#      in /opt/PROGRAM/control (picks NA/HD itself; proven on the NA unit),
#      else stm32isp-ad4 + a factory HD hex (HD units only). Never probes
#      the chip: syncing the vendor IAP erases the app.
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

# busybox on this SoC has NO pkill/pgrep (verified live 2026-10-21);
# scan /proc/*/cmdline instead (same helper as ad4-daemon.sh).
kill_pat() {
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) [ "${p#/proc/}" != "$$" ] && kill -9 "${p#/proc/}" 2>/dev/null ;;
    esac
  done
}

# 2) restore the original rcS
# PROVEN 2026-10-21 (live SoC): the patched file is /opt/auto_run.sh
# (rcS does '. /opt/auto_run.sh'), so it must be searched FIRST.
FFLINE='ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg'
MARKER="/opt/ad4-klipper/ad4-daemon.sh"
RCS=""
for c in /opt/auto_run.sh /etc/init.d/rcS /etc/rcS /etc/rc.local /etc/inittab /etc/init.d/rc; do
  [ -f "$c" ] || continue
  if grep -qF "$MARKER" "$c" 2>/dev/null; then RCS="$c"; break; fi
done
[ -z "$RCS" ] && RCS="$(grep -rlF "$MARKER" /opt/auto_run.sh /etc/init.d/ /etc/ 2>/dev/null | head -1)"
if [ -n "$RCS" ] && [ -f "$RCS" ]; then
  if [ -f "${RCS}.ad4-orig" ]; then
    cp -f "${RCS}.ad4-orig" "$RCS"
  else
    # no backup: unwrap our if/else line back to the plain vendor launch.
    # The SoC has NO python3, so this is awk (inverse of the installer's
    # single-line wrap: 'if [ -f ...enable ]; then <daemon> & else <cmd> fi').
    awk '
      index($0, "ffstartup-arm") > 0 && index($0, "/opt/ad4-klipper/ad4-daemon.sh") > 0 {
        i = index($0, " else ")
        if (i > 0) {
          rest = substr($0, i + 6)
          sub(/ fi[ \t]*$/, "", rest)
          print "  " rest
          next
        }
      }
      { print }
    ' "$RCS" > "$RCS.ad4-new"
    if [ -s "$RCS.ad4-new" ]; then
      mv -f "$RCS.ad4-new" "$RCS"
    else
      rm -f "$RCS.ad4-new"
      echo "!! unwrap failed on $RCS; enable flag is removed so the"
      echo "   vendor launch still runs (the gate is inert)."
    fi
  fi
  echo "restored $RCS"
fi

# 3) stop our runtime
kill_pat ad4-klipper/ad4-daemon.sh
kill_pat ad4-serial-bridge.py
kill_pat ad4-bridge

# 4) re-flash MCU to stock (best effort; skip if we just flashed factory).
#    The revision comes from a FILE, never from a probe: syncing the vendor
#    IAP erases the application (NA unit, SWD-proven 2026-10-27), so
#    `stm32isp-ad4 -s` must not run here. Trust order: $BASE/ad4-rev (copied
#    from the stick), then the vendor's own marker /root/N32 or /root/H32
#    (touched by its last flash).
#    Preferred tool: the vendor IAPCommand in /opt/PROGRAM/control/*/ with
#    both hexes - it selects the image from the chip's sync reply and is the
#    path proven on the NA unit (work/n32/iap-flash.sh). It leaves ttyS1 in
#    even parity, hence the stty. stm32isp-ad4 is the fallback for HD only.
IAP="$BASE/stm32isp-ad4"
if [ "$SKIP_MCU" != "1" ]; then
  REV=$(head -c 2 "$BASE/ad4-rev" 2>/dev/null)
  [ -n "$REV" ] || { [ -e /root/N32 ] && REV=NA; }
  [ -n "$REV" ] || { [ -e /root/H32 ] && REV=HD; }
  kill_pat ffstartup-arm
  kill_pat adventurer4-arm
  sleep 1
  DONE=0
  for d in /opt/PROGRAM/control/*/; do
    [ -x "$d/IAPCommand" ] || continue
    NAHEX=$(ls -1 "$d"Adventurer4-NA-IAP-*.hex 2>/dev/null | head -n 1)
    HDHEX=$(ls -1 "$d"Adventurer4-HD-IAP-*.hex 2>/dev/null | head -n 1)
    [ -f "$NAHEX" ] && [ -f "$HDHEX" ] || continue
    echo "restoring the stock MCU image with the vendor IAPCommand in $d"
    ( cd "$d" && ./IAPCommand "$(basename "$NAHEX")" "$(basename "$HDHEX")" ) && DONE=1
    stty -F /dev/ttyS1 -parenb 2>/dev/null
    break
  done
  if [ "$DONE" = 1 ]; then
    echo "MCU restored to stock (IAPCommand)"
  elif [ "$REV" = "HD" ] && [ -x "$IAP" ]; then
    FACTORY=""
    for c in "$BASE/factory-HD.hex" /mnt/factory-HD.hex; do
      [ -f "$c" ] && { FACTORY="$c"; break; }
    done
    [ -n "$FACTORY" ] || FACTORY=$(ls -1 /opt/PROGRAM/control/*/Adventurer4-HD-IAP-*.hex 2>/dev/null | head -n 1)
    if [ -f "$FACTORY" ]; then
      "$IAP" -p /dev/ttyS1 "$FACTORY" && echo "MCU restored to stock ($FACTORY)" \
        || echo "MCU restore failed"
    else
      echo "!! no stock HD image found: the boot hook is gone but the MCU keeps its image."
      echo "   Put the control-*.tar.xz in factory/ on the stick and use the RESTORE stick."
    fi
  else
    echo "!! MCU not restored: revision '${REV:-unknown}' and no IAPCommand kit under /opt/PROGRAM/control/."
    echo "   NA (N32) units are restored only through the vendor IAPCommand (adv4-iap-protocol.md)."
  fi
fi

# 5) remove our package (optional; keeps things tidy). Kept so a
#    re-enable is a one-file restore.
# rm -rf "$BASE"   # uncomment to fully remove the Klipper package

sync
echo "uninstall complete; rebooting to stock."
sleep 2
reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
exit 0
