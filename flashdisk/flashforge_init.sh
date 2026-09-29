#!/bin/sh
# =============================================================
# AD4 Klipper unattended flash — run from USB disk by the SoC
# boot/update path (same contract as the factory script:
# armv7l + kernel 3.4.39+, machine check skipped via screwflag).
#
# Disk layout (FAT32, files at the disk root):
#   flashforge_init.sh   this script
#   screwflag            empty file (skips machine-type check)
#   IAPCommand           factory ARM binary, OR the stm32isp-ad4 fork named
#                        "IAPCommand" (accepts the same two-hex argv)
#   klipper.hex          Klipper, built for the HD (H32) app window
#   factory-HD.hex       known-good restore image (never delete)
#   RESTORE              OPTIONAL: if present, flashes factory-HD.hex
# =============================================================
set -x
WORK_DIR=`dirname $0`

[ "$(uname -m)" != "armv7l"   ] && { echo "arch error";   exit 1; }
[ "$(uname -r)" != "3.4.39+" ] && { echo "kernel error"; exit 1; }

# 1) Stop the vendor stack so nothing holds /dev/ttyS1 or the GPIOs.
pkill -f ffstartup-arm   2>/dev/null
pkill -f adventurer4-arm 2>/dev/null
sleep 2

# 2) Free the MCU UART (belt and suspenders).
fuser -k /dev/ttyS1 2>/dev/null
sleep 1

# 3) Image select: RESTORE flag => factory, else Klipper.
if [ -f "$WORK_DIR/RESTORE" ]; then
    IMG="$WORK_DIR/factory-HD.hex"; FLAG=restore
else
    IMG="$WORK_DIR/klipper.hex";    FLAG=klipper
fi

# 4) Flash. Same image in both slots => rev-agnostic
#    (IAPCommand auto-selects HD/NA from the sync reply 6/7;
#     tty/baud come from its internal defaults, as in the factory run.sh).
"$WORK_DIR/IAPCommand" "$IMG" "$IMG"
RC=$?

# 5) Record outcome for later inspection.
echo "$FLAG rc=$RC" > /root/ad4-flash-status 2>/dev/null || true
sync
exit $RC
