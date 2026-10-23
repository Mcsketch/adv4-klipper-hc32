#!/bin/sh
# =============================================================
# ad4-install-persistent.sh — one-shot install of the PERSISTENT
# Klipper runtime onto the SoC eMMC.
#
# Run by flashforge_init.sh when a `PERSISTENT` flag file is on the
# USB stick (and runs AS ROOT — the SoC boot/update path is root).
# It does NOT need the root SSH password: the disk updater already
# has root.
#
# What it does:
#   1. Copy the daemon package from the stick to /opt/ad4-klipper/
#      (ad4-daemon.sh, ad4-serial-bridge.py, klipper-ad4.hex,
#       stm32isp-ad4, ad4-net.conf, mcu-klipper.ok marker).
#   2. Gate the vendor watchdog in rcS: the line that launches
#      ffstartup-arm becomes  if <enable> then <our daemon> else
#      <vendor> fi .  So in Klipper mode the vendor stack (and its
#      /dev/ttyS1 grab + crash/respawn) never runs; in stock mode it
#      runs exactly as factory. Fully reversible.
#   3. Create /opt/ad4-klipper/enable  (the "we want Klipper" flag).
#   4. Reboot so the patched rcS takes effect cleanly (we must not
#      edit a shell script while the running boot is executing it).
#
# REVERT to stock: put a `RESTORE` file on the stick and re-plug,
#   OR on the SoC:  sh /opt/ad4-klipper/ad4-uninstall-persistent.sh
#   (removes the enable flag + restores the original rcS line; the
#    vendor stack comes back on next boot, MCU is re-flashed to the
#    factory image by the same disk path).
#
# SAFETY: the rcS patch is a literal, single-line, if/else wrap that
#   defaults to the vendor launch — if the patch ever fails or the
#   enable file is absent, the SoC boots exactly as stock. No eMMC
#   data is deleted, the vendor app is never removed, so this can
#   not brick the SoC.
# =============================================================
set -x

WORK_DIR=`dirname $0`
BASE="/opt/ad4-klipper"

# ---- 1) copy the daemon package to eMMC ----------------------
# The SoC has NO python3, so the bridge MUST be the static C binary
# (ad4-bridge). Copy it (required); the python bridge is a belt-and-
# suspenders fallback only. Also copy the Klipper hex under the exact
# name the daemon expects (klipper.hex) — the stick may ship it as
# klipper.hex or klipper-ad4.hex.
mkdir -p "$BASE"
cp -f "$WORK_DIR/ad4-daemon.sh"        "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-bridge"           "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-serial-bridge.py" "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/stm32isp-ad4"         "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-net.conf"         "$BASE/" 2>/dev/null
# bench helpers, so they are still there after the stick is gone
cp -f "$WORK_DIR/ad4-revcheck.sh"      "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-setrootpw.sh"     "$BASE/" 2>/dev/null
# The revert must survive the stick: without the uninstaller AND the stock
# hex on eMMC, `sh /opt/ad4-klipper/ad4-uninstall-persistent.sh` is a no-op
# and the MCU can only be restored by re-plugging the stick.
cp -f "$WORK_DIR/ad4-uninstall-persistent.sh" "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/factory-HD.hex"     "$BASE/" 2>/dev/null
# klipper hex: accept either name on the stick -> klipper.hex on eMMC
if [ -f "$WORK_DIR/klipper.hex" ]; then
  cp -f "$WORK_DIR/klipper.hex" "$BASE/klipper.hex" 2>/dev/null
elif [ -f "$WORK_DIR/klipper-ad4.hex" ]; then
  cp -f "$WORK_DIR/klipper-ad4.hex" "$BASE/klipper.hex" 2>/dev/null
fi
chmod +x "$BASE/ad4-daemon.sh" "$BASE/ad4-bridge" "$BASE/stm32isp-ad4" \
         "$BASE/ad4-uninstall-persistent.sh" 2>/dev/null
sync

# The C bridge is REQUIRED for a no-stick boot (no python3 on SoC).
if [ ! -x "$BASE/ad4-bridge" ]; then
  echo "!! WARNING: ad4-bridge (static C) not on stick; eMMC daemon will have no bridge backend."
fi


# marker: the MCU was just flashed by flashforge_init.sh before we
# got here, so tell the daemon not to re-flash at boot.
touch "$BASE/mcu-klipper.ok"
# optional: enable the MCU watchdog self-heal (commented out = off).
# touch "$BASE/mcu-watchdog"

# ---- 2) gate the vendor watchdog in the boot rc script -------
# Locate the file that launches the vendor startup. Match on the
# SUBSTRING 'ffstartup-arm' — the full command line / flags vary by
# firmware build, so an exact full-line match is too strict.
# PROVEN 2026-10-21 (live SoC): the launch line is in /opt/auto_run.sh
# (rcS does '. /opt/auto_run.sh'); the /etc-only search that ran before
# logged 'patch-skipped' because it never looked in /opt.
FFPATTERN='ffstartup-arm'
MARKER='/opt/ad4-klipper/ad4-daemon.sh'
RCS=""
for c in /opt/auto_run.sh /etc/init.d/rcS /etc/rcS /etc/rc.local /etc/inittab /etc/init.d/rc; do
  [ -f "$c" ] || continue
  if grep -qF "$FFPATTERN" "$c" 2>/dev/null; then RCS="$c"; break; fi
done
# fallback: search the usual init dirs for the pattern.
if [ -z "$RCS" ]; then
  RCS="$(grep -rlF "$FFPATTERN" /etc/init.d/ /etc/ 2>/dev/null | head -1)"
fi
if [ -z "$RCS" ] || [ ! -f "$RCS" ]; then
  echo "!! could not locate the ffstartup-arm launch line; NOT patching."
  echo "   (Klipper runtime will still work from the stick for this"
  echo "    boot; re-run after confirming the rc file name.)"
  echo "patch-skipped" > "$BASE/install-status"
  # do NOT reboot into an unpatched state expecting persistence
  exit 3
fi

# Already patched from a previous run? Leave it alone.
if grep -qF "$MARKER" "$RCS" 2>/dev/null; then
  echo "rcS already patched: $RCS"
else
  echo "patching: $RCS"

  # Back up the original (literal, so restore is a simple copy-back).
  [ -f "${RCS}.ad4-orig" ] || cp -f "$RCS" "${RCS}.ad4-orig"

  # Single-line literal wrap, NO python3 (the SoC has none) -> awk.
  # Wrap the ffstartup-arm line so that, when /opt/ad4-klipper/enable is
  # present, our daemon runs INSTEAD of the vendor startup; otherwise the
  # original command runs and the SoC boots exactly as stock.
  awk -v MARKER="$MARKER" '
    {
      if (index($0, "ffstartup-arm") > 0 && index($0, MARKER) == 0) {
        cmd = $0
        gsub(/^[ \t]+/, "", cmd); gsub(/[ \t]+$/, "", cmd)
        print "if [ -f /opt/ad4-klipper/enable ]; then " MARKER " & else " cmd " fi"
        patched = 1
      } else {
        print $0
      }
    }
    END { if (!patched) { print "!! no ffstartup-arm line to patch inside", ARGV[1] > "/dev/stderr"; exit 2 } }
  ' "$RCS" > "$RCS.ad4-new"
  awkrc=$?
  if [ $awkrc -ne 0 ] || [ ! -s "$RCS.ad4-new" ]; then
    echo "!! rcS patch FAILED (awk rc=$awkrc); restoring original and aborting."
    rm -f "$RCS.ad4-new"
    cp -f "${RCS}.ad4-orig" "$RCS"
    echo "patch-failed-restored" > "$BASE/install-status"
    exit 4
  fi
  mv -f "$RCS.ad4-new" "$RCS"
  chmod a+x "$RCS" 2>/dev/null || true
  echo "patched $RCS -> daemon gated behind /opt/ad4-klipper/enable"
  sync
fi

# ---- 3) enable flag -----------------------------------------
touch "$BASE/enable"
sync

echo "persistent install complete; rebooting."
echo "installed rc=$? rcs=$RCS" > "$BASE/install-status"
sync
# Reboot so the patched rcS runs fresh (never edit a script under a
# running shell; and this makes boot #2 fully autonomous).
sleep 2
reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
exit 0
