#!/bin/sh
# iap-flash.sh — run ON THE AD4 SoC (busybox sh). Flashes <hex> into the MCU through the
# vendor IAPCommand; the hex goes in the NA slot (this unit is NA / N32G455). Proven
# 2026-10-27 restoring stock (adv4-iap-protocol.md). Detached from the ssh session so a
# WiFi drop cannot kill it mid-write. REMEMBER: the sync erases the app before writing.
# Preconditions: vendor GUI stopped (ffstartup-arm + adventurer4-arm) and nothing else on
# /dev/ttyS1; /tmp/iap holds IAPCommand (md5 919864f702cf3c07665234ab8de7896e), the HD
# hex and <hex>. Afterwards IAPCommand leaves /dev/ttyS1 in even parity: run
#   stty -F /dev/ttyS1 -parenb
# before anything reopens the port, or reboot.
# Usage: sh iap-flash.sh <hex-for-NA-slot> [hd-hex]
HEX=$1; HD=${2:-Adventurer4-HD-IAP-2.3.0-20230216.hex}
[ -n "$HEX" ] || { echo "usage: $0 <na-hex> [hd-hex]"; exit 2; }
cd /tmp/iap || exit 9
[ -f "$HEX" ] && [ -f "$HD" ] || { echo "ABORT: missing $HEX or $HD"; exit 6; }
if ps | grep -v grep | grep -E 'adventurer4-arm|ffstartup|IAPCommand|nasync'; then echo "ABORT: process busy"; exit 8; fi
[ "$(md5sum IAPCommand | cut -d' ' -f1)" = 919864f702cf3c07665234ab8de7896e ] || { echo "ABORT: IAPCommand md5 mismatch"; exit 7; }
echo "--- launch uptime $(cut -d' ' -f1 /proc/uptime): NA=$HEX HD=$HD"
nohup setsid sh -c "./IAPCommand '$HEX' '$HD'; echo EXIT=\$? >> iap.log" > iap.log 2>&1 < /dev/null &
sleep 2
t=0
while ps | grep -v grep | grep -q IAPCommand; do
  sleep 5; t=$((t+5))
  if [ $t -ge 300 ]; then echo "STILL RUNNING after 300s"; break; fi
done
echo "--- after ${t}s"; echo "--- iap.log ($(wc -l < iap.log) lines)"
head -12 iap.log; echo ...; tail -6 iap.log
echo "--- uart"; grep '^1:' /proc/tty/driver/uart
stty -F /dev/ttyS1 -a | tr ' ' '\n' | grep -qx parenb && echo "NOTE: ttyS1 left in even parity -> stty -F /dev/ttyS1 -parenb"
exit 0
