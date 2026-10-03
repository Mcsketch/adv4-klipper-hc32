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
#       IAPCommand, ad4-net.conf, mcu-klipper.ok marker).
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
mkdir -p "$BASE"
cp -f "$WORK_DIR/ad4-daemon.sh"        "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-serial-bridge.py" "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/klipper-ad4.hex"      "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/IAPCommand"           "$BASE/" 2>/dev/null
cp -f "$WORK_DIR/ad4-net.conf"         "$BASE/" 2>/dev/null
chmod +x "$BASE/ad4-daemon.sh" "$BASE/IAPCommand" 2>/dev/null
sync

# marker: the MCU was just flashed by flashforge_init.sh before we
# got here, so tell the daemon not to re-flash at boot.
touch "$BASE/mcu-klipper.ok"
# optional: enable the MCU watchdog self-heal (commented out = off).
# touch "$BASE/mcu-watchdog"

# ---- 2) gate the vendor watchdog in the boot rc script -------
# Locate the file that launches ffstartup-arm (normally /etc/init.d/rcS).
FFLINE='ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg'
RCS=""
for c in /etc/init.d/rcS /etc/rcS /etc/rc.local /etc/inittab /etc/init.d/rc; do
  [ -f "$c" ] || continue
  if grep -qF "$FFLINE" "$c" 2>/dev/null; then RCS="$c"; break; fi
done
# fallback: search the usual init dirs for the line.
if [ -z "$RCS" ]; then
  RCS="$(grep -rlF "$FFLINE" /etc/init.d/ /etc/ 2>/dev/null | head -1)"
fi
if [ -z "$RCS" ] || [ ! -f "$RCS" ]; then
  echo "!! could not locate the ffstartup launch line; NOT patching."
  echo "   (Klipper runtime will still work from the stick for this"
  echo "    boot; re-run after confirming the rc file name.)"
  echo "patch-skipped" > "$BASE/install-status"
  # do NOT reboot into an unpatched state expecting persistence
  exit 3
fi
echo "patching: $RCS"

# Back up the original (literal, so restore is a simple copy-back).
[ -f "${RCS}.ad4-orig" ] || cp -f "$RCS" "${RCS}.ad4-orig"

# The single-line literal wrap (python3 = safe literal replace).
python3 - "$RCS" <<'PY'
import sys
rc = sys.argv[1]
FF = "ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg"
txt = open(rc, "r", errors="ignore").read()

# If we already patched it (marker present), leave it alone.
MARKER = "/opt/ad4-klipper/ad4-daemon.sh"
if MARKER in txt:
    print("already patched; leaving rcS as-is")
    sys.exit(0)

# Find the line containing the vendor launch.
lines = txt.splitlines(keepends=True)
out = []
patched = False
for ln in lines:
    if FF in ln and not MARKER in ln:
        # preserve the original command as the else-branch
        cmd = ln.strip()
        out.append("if [ -f /opt/ad4-klipper/enable ]; then "
                   "/opt/ad4-klipper/ad4-daemon.sh & else " + cmd + " fi\n")
        patched = True
    else:
        out.append(ln)

if not patched:
    print("!! vendor launch line not found inside", rc)
    sys.exit(2)

open(rc, "w").write("".join(out))
print("patched", rc, "-> daemon gated behind /opt/ad4-klipper/enable")
PY
if [ $? -ne 0 ]; then
  echo "!! rcS patch FAILED; restoring original and aborting install."
  cp -f "${RCS}.ad4-orig" "$RCS"
  echo "patch-failed-restored" > "$BASE/install-status"
  exit 4
fi
sync

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
