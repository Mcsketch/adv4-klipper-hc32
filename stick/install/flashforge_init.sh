#!/bin/sh
# Adventurer 4 Klipper INSTALL stick.
#
# Copy this file and the ad4-klipper/ folder to the root of a FAT32 USB stick,
# plug it into the printer and power on. The panel asks once; tap INSTALL.
#   1. checks: NA board (N32G455), FlashForge's MCU kit intact, payload intact
#   2. copies the runtime to /opt/ad4-klipper and makes /opt/auto_run.sh start
#      it when /opt/ad4-klipper/klipper-mode exists (stock UI otherwise)
#   3. installs dropbear SSH and sets the root password
#   4. flashes Klipper into the MCU with FlashForge's own IAPCommand
#   5. creates klipper-mode, renames this script to .done, reboots
# Log: ad4-klipper.log on the stick. Undo: the rollback stick.
STICK=$(cd "$(dirname "$0")" && pwd)
LOG=$STICK/ad4-klipper.log
. "$STICK/ad4-klipper/lib.sh" || exit 1

ETH_ADDR=dhcp
ROOT_PASSWORD=
. "$P/ad4-klipper.conf"

log "==== install stick, $(cat "$P/VERSION" 2>/dev/null), uptime $(cut -d' ' -f1 /proc/uptime)"
BOARD=$(board)
log "board marker: $BOARD; $(uname -m) $(uname -r)"
screen splash

# ---- 1. checks: nothing has been changed yet
[ "$BOARD" = HD ] && fail "HD board" "This printer has the HC32F460 (HD) mainboard. This installer only supports the NA (N32G455) board. Nothing was changed. Remove the stick."
find_kit || fail "Stock kit missing" "FlashForge's MCU kit in /opt/PROGRAM/control is missing or damaged. Update the printer to stock firmware 2.3 first. Nothing was changed. Remove the stick."
[ "$(md5 "$KIT/IAPCommand")" = "$IAP_MD5" ] || fail "Untested kit" "IAPCommand in $KIT is not the 2.3.0 version this installer was tested with. Nothing was changed. Remove the stick."
(cd "$P" && while read -r sum name; do
    [ "$(md5 "$name")" = "$sum" ] || { log "payload md5 mismatch: $name"; exit 1; }
 done <MD5SUMS) || fail "Stick damaged" "A file on the stick does not match MD5SUMS. Copy the release onto the stick again. Nothing was changed."
b=$(busy) && fail "Printer busy" "Something already uses the MCU port: $b. Nothing was changed. Remove the stick."

# ---- 2. ask
case $BOARD in
NA) what="Mainboard: NA (N32G455)" ;;
*)  what="Mainboard: not recorded yet. IAPCommand picks the slot from the chip; on an HD board it rewrites the stock HD firmware and the install stops." ;;
esac
screen confirm "Install Klipper?" "Flashes Klipper onto the mainboard and switches the panel to Klipper. Takes a few minutes. Do not power off while it runs.\n$what" INSTALL 120
rc=$?
case $rc in
0) log "confirmed on the panel" ;;
1) cancel "No touchscreen answered" ;;
*) cancel "Not confirmed" ;;
esac
buzz start

# ---- 3. SoC side; the printer still boots stock until klipper-mode exists
screen status "Installing" "copying files" 5
rm -f "$D/klipper-mode"
rm -rf "$D.new"
mkdir -p "$D.new" || fail "Copy failed" "Could not create $D.new"
for f in ad4-bridge ad4-screen ad4-buzz klipper-boot.sh klipper.hex VERSION; do
    cp "$P/$f" "$D.new/$f" || fail "Copy failed" "$f"
done
# the password stays on the stick, not in a world-readable file on the printer
grep -v '^ROOT_PASSWORD=' "$P/ad4-klipper.conf" >"$D.new/ad4-klipper.conf" || fail "Copy failed" "ad4-klipper.conf"
chmod 755 "$D.new/ad4-bridge" "$D.new/ad4-screen" "$D.new/ad4-buzz" "$D.new/klipper-boot.sh"
rm -rf "$D" && mv "$D.new" "$D" || fail "Copy failed" "Could not move $D.new into place"
set_launch klipper || fail "Boot hook failed" "Could not update /opt/auto_run.sh. The original is unchanged; see the log."
log "auto_run.sh: launch line set (backup $A.pre-ad4-klipper)"

screen status "Installing" "SSH and root password" 10
mkdir -p /opt/services/bin
cp "$P/dropbear" /opt/services/bin/dropbearmulti && chmod 755 /opt/services/bin/dropbearmulti || fail "SSH install failed" "dropbear"
for l in dropbear dropbearkey scp ssh; do ln -sfn /opt/services/bin/dropbearmulti /bin/$l; done
mkdir -p /etc/dropbear
if [ "$ROOT_PASSWORD" = keep ]; then
    log "root password: kept"
else
    if [ -z "$ROOT_PASSWORD" ]; then
        ROOT_PASSWORD=$(head -c 9 /dev/urandom | od -An -tx1 | tr -d ' \n')
        printf '%s\n' "$ROOT_PASSWORD" >"$STICK/ROOT_PASSWORD.txt"
        log "root password: generated, written to ROOT_PASSWORD.txt on the stick"
    fi
    old=$(awk -F: '$1 == "root" { print $2 }' /etc/shadow)
    printf '%s\n%s\n' "$ROOT_PASSWORD" "$ROOT_PASSWORD" | passwd root >/dev/null 2>&1
    new=$(awk -F: '$1 == "root" { print $2 }' /etc/shadow)
    [ -n "$new" ] && [ "$new" != "$old" ] && log "root password: set" || log "root password: passwd made no change"
fi
sync

# ---- 4. MCU
iap "$D/klipper.hex" "$HD_HEX" "Flashing Klipper"
rc=$?
case $rc in
0) log "MCU: Klipper written (NA)" ;;
3) fail "HD board" "IAPCommand found an HD board and rewrote its stock firmware. Klipper was not installed; the printer boots stock. Remove the stick." ;;
*) fail "Flash failed" "The MCU has no working firmware now. Power-cycle with this stick to try again, or use the rollback stick. Log: ad4-klipper.log" ;;
esac

# ---- 5. switch over
touch "$D/klipper-mode"
date >"$D/installed"
retire
log "==== install done, rebooting"
screen done "Klipper installed" "Rebooting into Klipper mode. Remove the stick."
buzz ok
sync
sleep 3
reboot -f
