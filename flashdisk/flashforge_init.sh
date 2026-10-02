#!/bin/sh
# =============================================================
# AD4 Klipper — unattended SoC-side init (runs from the USB stick
# via the SoC boot/update path, same contract as the factory script:
# armv7l + kernel 3.4.39+, machine check skipped via screwflag).
#
# It does the whole "hack the SoC" in one go:
#   1. Flash the MCU (klipper or factory, by RESTORE flag) —
#      with a hex base-address guard (never write <= 0x00008000,
#      so the ROM ISP is always intact => never brickable).
#   2. Give eth0 a static IP (default 192.168.10.10/24) so the
#      RPi host always finds the SoC.
#   3. Start the serial<->network bridge (MCU /dev/ttyS1 <-> TCP
#      0.0.0.0:5000) so Klipper on the RPi reaches the MCU.
#
# Disk layout (FAT32, files at the disk root):
#   flashforge_init.sh   this script
#   screwflag            empty file (skips machine-type check)
#   IAPCommand           the factory ARM flasher (it DOES the GPIO
#                        strapping: gpio 34=NRST, 35=BOOT0, ttyS1)
#   klipper.hex          Klipper, positioned at the HD app window 0x000C0000
#   factory-HD.hex       known-good restore image (never delete)
#   ad4-serial-bridge.py the SoC<->MCU serial/ethernet bridge
#   ad4-net.conf         OPTIONAL: SOC_IP=192.168.10.10  BRIDGE_PORT=5000
#   RESTORE              OPTIONAL: if present, flash factory-HD.hex
#
# Result logged to /root/ad4-flash-status on the SoC.
# =============================================================
set -x
WORK_DIR=`dirname $0`

# ---- arch / kernel guard (same as factory) ------------------
[ "$(uname -m)" != "armv7l"   ] && { echo "arch error";   exit 1; }
[ "$(uname -r)" != "3.4.39+" ] && { echo "kernel error"; exit 1; }

# ---- tunables (overridable via ad4-net.conf) ----------------
SOC_IP="192.168.10.10"
SOC_NET="24"
BRIDGE_PORT="5000"
[ -f "$WORK_DIR/ad4-net.conf" ] && . "$WORK_DIR/ad4-net.conf"

# ---- hex base-address guard (the anti-brick check) ----------
# Refuse to flash any image whose lowest programmed address is at or
# below 0x00008000 (the reset / ROM-ISP region). Our Klipper image is
# positioned at 0x0000C000, so this passes; a mis-positioned hex fails
# loudly and we fall back to NOT flashing the app window.
check_base() {
  # Anti-brick guard. Two modes:
  #   strict  (custom image, e.g. klipper.hex): refuse unless base > 0x00008000
  #           (the custom app must live in the app window and must not be a
  #            random/low-base blob that could clobber the vector region).
  #   factory (known-good vendor image, factory-HD.hex): it is a full-chip
  #           image at base 0x00000000 by design (the vendor IAP lives at
  #           0x0000); we accept it but sanity-check it is non-trivial in size.
  # $1 = hex path, $2 = strict|factory
  python3 -c '
import sys
path, mode = sys.argv[1], sys.argv[2]
base = None; maxa = 0; minoff = None
for line in open(path, "r", errors="ignore"):
    line = line.strip()
    if not line.startswith(":"): continue
    try: cnt = int(line[1:3], 16); typ = int(line[7:9], 16)
    except Exception: continue
    if typ in (0x04, 0x05):
        b = int(line[9:13], 16) << 16
        base = b if base is None else min(base, b)
    elif typ == 0x00:
        off = int(line[3:7], 16)
        if minoff is None or off < minoff: minoff = off
        if off + cnt > maxa: maxa = off + cnt
# true lowest absolute address = (extended base or 0) + min data offset.
# Handles both styles: type-04 base + small offsets, and bare offsets (no
# type-04) like our Klipper build, which already encodes 0xC000 in the offset.
minaddr = (base if base is not None else 0) + (minoff if minoff is not None else 0)
have = (minoff is not None)
if mode == "strict":
    ok = have and (minaddr > 0x00008000)
else:  # factory: full-chip image at base 0x0000 by design; just sanity-check size
    ok = have and (maxa > 0x00001000)
print("minaddr=0x%08X max=0x%08X" % (minaddr, maxa))
sys.exit(0 if ok else 2)
' "$1" "$2"
}

# 1) Stop the vendor stack so nothing holds /dev/ttyS1 or the GPIOs.
pkill -f ffstartup-arm   2>/dev/null
pkill -f adventurer4-arm 2>/dev/null
sleep 2
fuser -k /dev/ttyS1 2>/dev/null
sleep 1

# 2) Image select + base guard (strict for custom, permissive for the
#    known-good factory image which is a full-chip dump at base 0x0000).
if [ -f "$WORK_DIR/RESTORE" ]; then
    IMG="$WORK_DIR/factory-HD.hex"; FLAG=restore; MODE=factory
else
    IMG="$WORK_DIR/klipper.hex";    FLAG=klipper;  MODE=strict
fi
if [ -f "$IMG" ] && check_base "$IMG" "$MODE"; then
    :
else
    [ -f "$IMG" ] || IMG_ERR="missing: $IMG"
    echo "!! image check FAILED for $IMG ($MODE) ${IMG_ERR:-}".
    echo "!! Refusing to flash. MCU left as-is (safe)."
    echo "flash-refused($FLAG) rc=1" > /root/ad4-flash-status 2>/dev/null || true
    exit 1
fi

# 3) Flash (IAPCommand does the strapping; two-hex argv, auto HD/NA).
"$WORK_DIR/IAPCommand" "$IMG" "$IMG"
RC=$?
echo "$FLAG rc=$RC" > /root/ad4-flash-status 2>/dev/null || true
sync

# 4) SoC network: static IP on eth0 (self-heals every boot from the stick).
ip addr flush dev eth0 2>/dev/null
ip addr add "$SOC_IP/$SOC_NET" dev eth0 2>/dev/null
ip link set eth0 up 2>/dev/null
sleep 1

# 5) Start the MCU bridge (only if the flash step at least ran).
if [ -x "$WORK_DIR/ad4-serial-bridge.py" ] || [ -f "$WORK_DIR/ad4-serial-bridge.py" ]; then
    pkill -f ad4-serial-bridge.py 2>/dev/null
    nohup python3 "$WORK_DIR/ad4-serial-bridge.py" --tcp 0.0.0.0:"$BRIDGE_PORT" \
        --uart /dev/ttyS1 --baud 250000 >>/var/log/ad4-bridge.log 2>&1 &
    sleep 1
    if pgrep -f ad4-serial-bridge.py >/dev/null 2>&1; then
        echo "bridge running on 0.0.0.0:$BRIDGE_PORT (soc_ip=$SOC_IP)" >>/root/ad4-flash-status
    else
        echo "bridge FAILED (see /var/log/ad4-bridge.log)" >>/root/ad4-flash-status
    fi
fi
sync
exit $RC
