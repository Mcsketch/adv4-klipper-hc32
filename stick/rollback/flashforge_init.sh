#!/bin/sh
# Adventurer 4 ROLLBACK stick: back to FlashForge's stock firmware.
#
# Copy this file and the ad4-klipper/ folder to the root of a FAT32 USB stick,
# plug it into the printer and power on. The panel asks once; tap RESTORE.
#   1. checks FlashForge's MCU kit in /opt/PROGRAM/control (on every printer)
#   2. flashes the stock MCU firmware with FlashForge's own IAPCommand
#   3. puts the stock launch line back in /opt/auto_run.sh and removes
#      /opt/ad4-klipper and the SSH server this project added
#   4. renames this script to .done and reboots into the stock UI
# Works on a Klipper printer, a half-finished install, or a stock printer,
# on NA and HD boards. FlashForge's files are read from the printer itself;
# none are shipped on the stick. Log: ad4-klipper.log on the stick.
STICK=$(cd "$(dirname "$0")" && pwd)
LOG=$STICK/ad4-klipper.log
. "$STICK/ad4-klipper/lib.sh" || exit 1

log "==== rollback stick, $(cat "$P/VERSION" 2>/dev/null), uptime $(cut -d' ' -f1 /proc/uptime)"
BOARD=$(board)
log "board marker: $BOARD"
screen splash

find_kit || fail "Stock kit missing" "FlashForge's MCU kit in /opt/PROGRAM/control is missing or damaged, so the stock firmware cannot be restored from the printer. Nothing was changed."
b=$(busy) && fail "Printer busy" "Something already uses the MCU port: $b. Nothing was changed."

screen confirm "Restore stock firmware?" "Flashes FlashForge's stock firmware ($(basename "$KIT")) onto the mainboard and removes Klipper from the printer. Takes a few minutes. Do not power off while it runs." RESTORE 120
case $? in
0) log "confirmed on the panel" ;;
1) cancel "No touchscreen answered" ;;
*) cancel "Not confirmed" ;;
esac
buzz start

# Klipper must not start on the next boot, whatever happens below.
rm -f "$D/klipper-mode" "$D/enable"
sync

iap "$NA_HEX" "$HD_HEX" "Restoring stock"
case $? in
0|3) log "MCU: stock firmware written" ;;
*) fail "Flash failed" "The MCU has no working firmware now. Power-cycle with this stick to try again. Log: ad4-klipper.log" ;;
esac

screen status "Restoring stock" "boot script" 100
if grep -qE '/opt/ad4-klipper/|^/bin/dropbear' "$A"; then
    set_launch stock || fail "Boot hook failed" "The MCU is stock again but /opt/auto_run.sh could not be restored. Its backup is $A.pre-ad4-klipper; see the log."
    log "auto_run.sh: stock launch line restored"
fi
rm -rf "$D" "$D.new"
for l in dropbear dropbearkey scp ssh; do
    [ "$(readlink /bin/$l)" = /opt/services/bin/dropbearmulti ] && rm -f /bin/$l
done
rm -f /opt/services/bin/dropbearmulti
log "removed $D and the dropbear SSH server"

retire
log "==== rollback done, rebooting"
screen done "Stock firmware restored" "Rebooting into the FlashForge UI. Remove the stick."
buzz ok
sync
sleep 3
reboot -f
