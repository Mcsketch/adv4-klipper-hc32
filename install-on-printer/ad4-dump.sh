#!/bin/sh
# =============================================================
# ad4-dump.sh - read the MCU's flash back out, from the printer's panel.
#
#   ad4-dump.sh check          is the host toolchain reachable and ready
#   ad4-dump.sh peek [ADDR]    hexdump 64 bytes into the host log
#   ad4-dump.sh boot           dump the vendor bootloader window (0..0xC000)
#   ad4-dump.sh full           dump all 512 KiB of code flash
#
# Why this goes through the host: the flash-resident IAP bootloader at
# 0x0..0xBFFF has no read command - the stock flasher only writes. The
# only way to see that region is code already running on the MCU, so
# the read is klipper's built-in debug_read (see klipper-extras/
# ad4dump.py), driven over the network from here.
#
# Consequence worth knowing before you press the button: klipper runs on
# the host, so the .bin lands THERE, in klipper's working directory -
# not on the stick and not on the printer. This script can start the
# dump and follow its progress, but it cannot fetch the file back.
#
# Rate is ~1075 B/s: the bootloader window takes about 46 s, the whole
# 512 KiB about 7.5 minutes. Progress comes from moonraker's gcode
# store, which mirrors klipper's respond_info lines.
# =============================================================

WORK_DIR=$(cd "$(dirname "$0")" && pwd)
LOGF=${AD4_LOG:-/tmp/ad4-dump.log}
say() { printf '[dump] %s\n' "$1"; printf '%s\n' "$1" >> "$LOGF" 2>/dev/null; }

HOST=192.168.10.1
PORT=7125
[ -f "$WORK_DIR/ad4-net.conf" ] && . "$WORK_DIR/ad4-net.conf"
HOST=${KLIPPER_HOST:-$HOST}
PORT=${MOONRAKER_PORT:-$PORT}
API="http://$HOST:$PORT"

MODE=${1:-boot}

# busybox wget has no --content-disposition and no urlencoding, so the
# gcode line is escaped by hand. '=' inside the value is harmless:
# moonraker takes everything after the first '=' as the script.
enc() { printf '%s' "$1" | sed 's/ /%20/g'; }

api() {   # $1 = path
  wget -q -O - --timeout=5 "$API$1" 2>/dev/null
}

post() {  # $1 = gcode
  wget -q -O - --timeout=10 --post-data='' \
    "$API/printer/gcode/script?script=$(enc "$1")" 2>/dev/null
}

reachable() {
  r=$(api "/printer/info")
  [ -n "$r" ] || return 1
  printf '%s' "$r"
}

case "$MODE" in
  check)
    r=$(reachable) || { say "moonraker not answering on $HOST:$PORT"; exit 1; }
    say "moonraker up: $(printf '%s' "$r" | sed 's/[",]/ /g' | tr -s ' ' | cut -c1-120)"
    exit 0
    ;;
  peek)
    addr=${2:-0x0}
    say "peek $addr (hexdump goes to the host's klippy.log)"
    post "ADFLASH_PEEK ADDR=$addr COUNT=64" >/dev/null
    say "posted - look at klippy.log on the host"
    exit 0
    ;;
  boot) start=0;        end=0xC000;  name=adv4-bootloader.bin; secs=120 ;;
  full) start=0;        end=0x80000; name=adv4-full-512KiB.bin; secs=600 ;;
  *) echo "usage: $0 check|peek|boot|full"; exit 2 ;;
esac

reachable >/dev/null || { say "!! moonraker not answering on $HOST:$PORT"; exit 1; }
say "dumping $start..$end to $name on the host ($HOST)"
post "ADFLASH_DUMP START=$start END=$end FILE=$name" >/dev/null || {
  say "!! POST failed - is klipper in a state that accepts gcode?"; exit 1; }

# The POST returns before the read finishes; follow the progress lines.
seen=""
deadline=$(( $(awk '{print int($1)}' /proc/uptime) + secs ))
while :; do
  now=$(awk '{print int($1)}' /proc/uptime)
  [ "$now" -lt "$deadline" ] || { say "!! timed out after ${secs}s"; exit 1; }
  sleep 5
  g=$(api "/server/gcode_store?count=6")
  case "$g" in
    *'"message"'*) ;;
    *) continue ;;
  esac
  line=$(printf '%s' "$g" | tr ',' '\n' | grep '"message"' | tail -1 \
         | sed 's/.*"message":"//; s/"$//')
  [ -n "$line" ] && [ "$line" != "$seen" ] && { say "  $line"; seen="$line"; }
  # klipper's respond_info arrives prefixed with "// ", so match anywhere.
  case "$line" in *Wrote*) say "done"; exit 0 ;; esac
done
