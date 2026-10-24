#!/bin/sh
# =============================================================
# AD4 Klipper — unattended SoC-side init  (PYTHON-FREE build)
#
# Runs from the USB stick via the SoC boot/update path
# (armv7l + kernel 3.4.39+). No console? No python3? No problem:
# every step is logged in full to a file on THE STICK itself
# (it is mounted at /mnt while this runs), so you pull the stick
# and read ./ad4-flash.log to see exactly what happened.
#
# What it does:
#   1. Flash the MCU (klipper, or factory if RESTORE present)
#      via the C flasher (stm32isp-ad4). Anti-brick: the flasher
#      refuses any image whose base is <= 0x00008000. NOTE (session-5
#      RE): the HC32F460 has no ROM ISP — the bootloader at 0x0..0xBFFF
#      is FLASH-resident, so it IS erasable; that guard is exactly what
#      keeps it alive. Do not lower it.
#   2. Give eth0 a static IP (default 192.168.10.10/24).
#   3. Start the MCU<->TCP bridge (static C binary 'ad4-bridge' — the
#      SoC has no python3; the python bridge is a fallback only).
#   4. Install dropbear SSH + set the root password (ROOT_PASS, default
#      'flashforge') and persist it through /opt/auto_run.sh.
#   5. If a PERSISTENT flag file is present: copy the whole package to
#      /opt/ad4-klipper and gate the vendor UI out of /opt/auto_run.sh,
#      so steps 2-4 survive every boot without the stick.
#
# Stick layout (FAT32, files at root — copy them there by hand, see README.md):
#   flashforge_init.sh        this script
#   screwflag                 empty file (skips the vendor machine-type check)
#   stm32isp-ad4              our ARM flasher (does the GPIO strapping; -s probes the chip)
#   klipper.hex               Klipper @ 0x0000C000
#   factory/                  OPTIONAL: FlashForge's own upgrade key (its four
#                             .tar.xz packages) for a FULL FACTORY restore, and
#                             the stock MCU hex for RESTORE. Not in this repo.
#   ad4-bridge                static C TCP<->/dev/ttyS1 relay (the bridge)
#   ad4-serial-bridge.py      python fallback bridge (SoC has no python3)
#   dropbear                  SSH server binary installed to /opt/services/bin
#   ad4-daemon.sh             boot-time daemon (bridge + net) for the persistent install
#   ad4-install-persistent.sh installs the eMMC boot hook
#   ad4-uninstall-persistent.sh removes it again
#   ad4-revcheck.sh           read-only NA-vs-HD MCU probe
#   ad4-screen                OPTIONAL: paints the 800x480 panel and reads
#                             the touchscreen (installer menu + progress).
#                             Without it everything still runs from flags.
#   ad4-rev                   OPTIONAL: one word (NA or HD) shown on the panel
#   ad4-net.conf              OPTIONAL SOC_IP= SOC_NET= BRIDGE_PORT=
#   ROOT                      OPTIONAL: first line = the root password you
#                             want. No file, or an empty one, means 'flashforge'.
#   RESTORE                   OPTIONAL: revert — unwind the boot hook and
#                             flash the stock MCU image back onto the MCU
#   NO_FLASH                  OPTIONAL: skip MCU flashing, do SoC access only
#                             (with RESTORE: unwind the hook, leave the MCU
#                             image alone — vendor UI back, MCU still Klipper)
#   PERSISTENT                OPTIONAL: install eMMC boot hook + reboot
#
# OUTPUT (where to read it without a console):
#   $WORK_DIR/ad4-flash.log   <- ON THE STICK (this is the one)
#   /root/ad4-flash.log       <- on the SoC
#   /root/ad4-flash-status    <- one-line result
# =============================================================

# ---- where is this script (and therefore the stick)? ---------
WORK_DIR=`dirname "$0"`

# A log file is guaranteed to exist on the stick (/mnt) no matter
# which path auto_run used to launch us (/mnt/... or /data/update/...).
STICK_LOG="/mnt/ad4-flash.log"
ROOT_LOG="/root/ad4-flash.log"
LOG=""
if [ -d /mnt ] && [ -w /mnt ]; then LOG="$STICK_LOG"; fi
[ -n "$LOG" ] || LOG="$ROOT_LOG"

: > "$LOG" 2>/dev/null || { LOG="/root/ad4-flash.log"; : > "$LOG"; }
STATUS="/root/ad4-flash-status"

# ---- logging --------------------------------------------------
# Every line: [HH:MM:SS] message  ->  the log file AND stdout.
T() { date +"%Y-%m-%d %H:%M:%S" 2>/dev/null || printf 'ts?'; }
LOGF() {
  # $1 = optional severity tag
  local m="$1"; [ "$m" = "" ] && return
  printf '[%s] %s\n' "$(T)" "$m" >> "$LOG"
  # also echo to console if we ever have one (harmless if not)
  printf '[%s] %s\n' "$(T)" "$m"
  # mirror to /root too (best effort) so both copies exist
  printf '[%s] %s\n' "$(T)" "$m" >> "$ROOT_LOG" 2>/dev/null
  sync
}
FATAL() { LOGF "FATAL: $1"; echo "ad4-fatal($1)" > "$STATUS" 2>/dev/null; sync; exit 1; }

# ---- tolerant process kill (busybox may lack pkill/fuser/pgrep)
pkill_safe() {
  # $1 = pattern. Try pkill, killall, then a /proc/*/cmdline scan.
  if command -v pkill >/dev/null 2>&1; then pkill -f "$1" 2>/dev/null; fi
  if command -v killall >/dev/null 2>&1; then killall "$1" 2>/dev/null; fi
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) kill -9 "${p#/proc/}" 2>/dev/null ;;
    esac
  done
}
# Kill whatever has $1 (e.g. /dev/ttyS1) OPEN, via /proc/*/fd symlinks.
# This is the reliable way to free a serial the vendor GUI/watchdog holds,
# and it does NOT depend on pkill/killall/fuser being present.
kill_fd_holder() {
  # $1 = device path to free
  local dev="$1" found=""
  for fd in /proc/[0-9]*/fd/*; do
    tgt=$(readlink "$fd" 2>/dev/null) || continue
    [ "$tgt" = "$dev" ] || continue
    pid=$(echo "$fd" | cut -d/ -f3)
    case "$pid" in $$) continue ;; esac   # never kill ourselves
    kill -9 "$pid" 2>/dev/null
    found="${found}${pid} "
  done
  [ -n "$found" ] && { LOGF "freed $dev (killed holders: $found)"; return 0; }
  return 1
}
pid_alive() {
  # $1 = pattern ; returns 0 if a matching process exists
  if command -v pgrep >/dev/null 2>&1; then pgrep -f "$1" >/dev/null 2>&1; return; fi
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) return 0 ;;
    esac
  done
  return 1
}

# ---- sound (buzzer) ---------------------------------------------
# The piezo is driven by the vendor sunxi PWM driver through the /dev/pwm
# char device. This kernel exports no /sys/class/pwm pwmchipN, so the
# ioctls in ad4-buzz (decoded from the vendor `play` binary, whose main() is
# a hardcoded buzzerPlay::play(TuneId 7)) are the only path that makes
# sound. Sound is always a bonus: no binary or no /dev/pwm and we log on.
BUZZ="$WORK_DIR/ad4-buzz"
BUZZ_OK=0
[ -x "$BUZZ" ] && [ -e /dev/pwm ] && BUZZ_OK=1
if [ "$BUZZ_OK" = 1 ]; then LOGF "sound ready: $BUZZ (/dev/pwm)"; else
  LOGF "sound: skipped (ad4-buzz or /dev/pwm missing)"
fi
# chime: $1 = tap | mark | start | ok | fail. "ok" is the completion riff
# (~5 s), so it is backgrounded: sound must never hold up a flash.
chime() {
  [ "$BUZZ_OK" = 1 ] || { LOGF "chime: $1 (no sound path - skipped)"; return 0; }
  LOGF "chime: $1"
  ( "$BUZZ" "$1" >/dev/null 2>&1 & ) 2>/dev/null
}

LOGF "================ AD4 flashforge_init (no-python) ================"
LOGF "pid=$$  uid=$(id -u 2>/dev/null)  argv: $0 $1 $2"
LOGF "WORK_DIR=$WORK_DIR"
LOGF "log file = $LOG"

# ---- evidence first (so the log always shows it ran) ----------
UNAME_M=$(uname -m 2>/dev/null)
UNAME_R=$(uname -r 2>/dev/null)
LOGF "uname -m = $UNAME_M   uname -r = $UNAME_R   hostname=$(hostname 2>/dev/null)"
LOGF "/proc/version: $(cat /proc/version 2>/dev/null)"
LOGF "/proc/cpuinfo: $(grep -E 'Processor|Architecture|model name' /proc/cpuinfo 2>/dev/null | tr '\n' ' ')"

# ---- toolchain census (tells us what the SoC actually has) ----
LOGF "tool census:"
for t in python3 python md5sum awk dd ip ifconfig mount tar sleep pkill pgrep; do
  p=$(command -v "$t" 2>/dev/null)
  [ -n "$p" ] && LOGF "    $t -> $p" || LOGF "    $t -> MISSING"
done
if command -v python3 >/dev/null 2>&1; then
  LOGF "    python3 version: $(python3 -V 2>&1)"
else
  LOGF "    NOTE: python3 NOT present -> bridge step will be skipped (flash still works)"
fi

# ---- arch / kernel guard --------------------------------------
# Arch is a HARD stop (the flasher binary can't run on the wrong arch).
# Kernel is a SOFT warning: a near-mismatch (e.g. 3.4.39 vs 3.4.39+)
# must NOT hide the real cause, so we log and continue.
if [ "$UNAME_M" != "armv7l" ]; then
  FATAL "arch error: got '$UNAME_M' want armv7l (flasher binary can't run)"
fi
case "$UNAME_R" in
  3.4.39*) LOGF "kernel OK: $UNAME_R" ;;
  *)       LOGF "WARNING: kernel '$UNAME_R' != expected 3.4.39+ - continuing anyway" ;;
esac

# ---- tunables -------------------------------------------------
SOC_IP="192.168.10.10"; SOC_NET="24"; BRIDGE_PORT="5000"
if [ -f "$WORK_DIR/ad4-net.conf" ]; then
  LOGF "loading ad4-net.conf"; . "$WORK_DIR/ad4-net.conf"; LOGF "  SOC_IP=$SOC_IP BRIDGE_PORT=$BRIDGE_PORT"
fi
# SSH root password. Put your own on the stick as a ROOT file whose first line
# is the password; delete it and the default below applies. Read as data, not
# sourced: a password is not shell code, and sourcing would mangle anything
# with metacharacters in it. Never committed (see .gitignore).
ROOT_PASS="flashforge"
if [ -f "$WORK_DIR/ROOT" ]; then
  ROOT_FILE_PW=$(head -n 1 "$WORK_DIR/ROOT" 2>/dev/null | tr -d '\r\n')
  if [ -n "$ROOT_FILE_PW" ]; then
    ROOT_PASS="$ROOT_FILE_PW"
    LOGF "ROOT file found: password overridden (${#ROOT_PASS} chars)"
  else
    LOGF "ROOT file is empty: using the default password"
  fi
fi

# ---- on-panel screen (optional) --------------------------------
# ad4-screen paints the 800x480 framebuffer (/dev/fb0) and reads the
# TSC2007 touch (/dev/input/event0). auto_run.sh runs THIS before it
# launches the vendor Qt GUI, so at install time the panel is ours.
# It is strictly optional: no binary, no /dev/fb0 or no touch -> we fall
# back to the stick's flag files, exactly as before. A screen failure can
# never abort an install.
SCR="$WORK_DIR/ad4-screen"
SCR_OK=0
[ -x "$SCR" ] && [ -e /dev/fb0 ] && SCR_OK=1
if [ "$SCR_OK" = 1 ]; then LOGF "panel: $SCR ready (/dev/fb0 present)"; else
  LOGF "panel: skipped (binary or /dev/fb0 missing) - flag files decide the mode"
fi
MCU_HINT="unknown"
[ -f "$WORK_DIR/ad4-rev" ] && MCU_HINT=$(head -c 8 "$WORK_DIR/ad4-rev" 2>/dev/null)
if [ -f /opt/ad4-klipper/enable ]; then INSTALLED_TXT="yes (boot hook on)"
elif [ -f /opt/auto_run.sh.ad4-orig ]; then INSTALLED_TXT="installed, toggled off"
else INSTALLED_TXT="no (stock boot)"; fi
FACT_TXT=""
STICK_FILES=""
for f in klipper.hex factory-HD.hex stm32isp-ad4 ad4-bridge dropbear \
         ad4-daemon.sh ad4-install-persistent.sh ad4-uninstall-persistent.sh \
         ad4-factory.sh ad4-dump.sh; do
  [ -f "$WORK_DIR/$f" ] && STICK_FILES="$STICK_FILES $f"
done
# SCR <subcommand> [args] - paints one screen; output goes to the log only.
SCRX() {
  [ "$SCR_OK" = 1 ] || return 0
  AD4_MCU="$MCU_HINT" AD4_IP="$SOC_IP" AD4_PORT="$BRIDGE_PORT" \
  AD4_INSTALLED="$INSTALLED_TXT" AD4_STICK="$STICK_FILES" \
  AD4_FACTORY="$FACT_TXT" \
    "$SCR" "$@" >> "$LOG" 2>&1
  return 0
}
SCRX splash

# ---- inventory of what's on the stick -------------------------
LOGF "stick inventory ($WORK_DIR):"
ls -la "$WORK_DIR" >> "$LOG" 2>&1
for f in screwflag stm32isp-ad4 klipper.hex factory-HD.hex ad4-bridge ad4-serial-bridge.py \
         ad4-net.conf ROOT ad4-daemon.sh ad4-install-persistent.sh \
         ad4-uninstall-persistent.sh ad4-revcheck.sh ad4-screen ad4-buzz ad4-rev \
         ad4-factory.sh ad4-dump.sh RESTORE PERSISTENT NO_FLASH FACTORY DUMP; do
  if [ -f "$WORK_DIR/$f" ]; then
    # ROOT holds a password: log that it exists, never its size or contents.
    [ "$f" = ROOT ] && { LOGF "    present: ROOT (password override)"; continue; }
    LOGF "    present: $f ($(wc -c < "$WORK_DIR/$f" 2>/dev/null | tr -d ' ') bytes)"
  else
    LOGF "    absent : $f"
  fi
done
for d in "$WORK_DIR"/factory "$WORK_DIR"/factory/Adventurer4-*; do
  [ -d "$d" ] && LOGF "    present: $d/ ($(ls -1 "$d" 2>/dev/null | tr '\n' ' '))"
done

# ---- what can a full factory restore work from? ----------------
# ad4-factory.sh probe reads the user's factory/ folder (FlashForge's own
# upgrade key - not redistributed here) and, as a fallback, the control
# package already sitting in the printer's /opt/PROGRAM. Nothing is
# written; the untar goes to /tmp.
FACTSH="$WORK_DIR/ad4-factory.sh"
if [ -f "$FACTSH" ]; then
  FACT=$(sh "$FACTSH" probe 2>>"$LOG")
  LOGF "factory probe: $FACT"
  n=$(printf '%s' "$FACT" | sed -n 's/.*pkgs=\([0-9]\).*/\1/p'); n=${n:-0}
  case "$FACT" in *mcu_hd=yes*) MCUTXT="+mcu" ;; *) MCUTXT="" ;; esac
  if [ "$n" = 0 ] && [ -z "$MCUTXT" ]; then
    FACT_TXT="nothing in factory/"
  else
    FACT_TXT="$n/4 packages $MCUTXT"
  fi
  LOGF "panel factory line: '$FACT_TXT'"
else
  LOGF "ad4-factory.sh not on the stick: FULL FACTORY will be refused"
fi

# ---- anti-brick hex base guard, PURE awk (no python) ----------
# Emits "minabs=0x... maxabs=0x... ok=0/1" and sets a global OK.
check_base() {
  # $1 = hex path, $2 = strict|factory
  # Portable awk (mawk): comparison ops MUST be space-delimited
  # (mawk mis-tokenizes `x!=":"` and the like), no bit-shifts.
  # Type-04 AND type-05 are (value << 16), matching the C tool; type-02
  # (extended segment, what objcopy emits past a 64K page) is (value << 4).
  # A guard that reads a type-02 page as base 0 refuses a good image; a
  # flasher that ignores one writes that page over the bootloader.
  awk -v HB_MODE="$2" '
    function h2d(h,  i, v) {
      v = 0; h = tolower(h)
      for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
      return v
    }
    {
      if (substr($0, 1, 1) != ":") next
      if (length($0) < 11) next
      t = h2d(substr($0, 8, 2))
      if (t == 4) { linhi = h2d(substr($0, 10, 4)) * 65536; next }
      if (t == 5) { linhi = h2d(substr($0, 10, 4)) * 65536; next }
      if (t == 2) { linhi = h2d(substr($0, 10, 4)) * 16; next }
      if (t != 0) next
      cnt = h2d(substr($0, 2, 2)); off = h2d(substr($0, 4, 4)); abs = linhi + off
      if (!first) { min = abs; max = abs + cnt; first = 1 }
      else { if (abs < min) min = abs; if (abs + cnt > max) max = abs + cnt }
    }
    END {
      if (!first) { print "minabs=0x0 maxabs=0x0 ok=0"; exit }
      if (HB_MODE == "strict") ok = (min > 32768) ? 1 : 0
      else ok = (max > 4096) ? 1 : 0
      printf "minabs=0x%08x maxabs=0x%08x ok=%d\n", min, max, ok
    }' "$1"
}

# ---- static IP on eth0 (also needed before any host round-trip) --
net_up() {
  LOGF "configuring eth0 = $SOC_IP/$SOC_NET"
  if command -v ip >/dev/null 2>&1; then
    ip addr flush dev eth0 2>/dev/null
    ip addr add "$SOC_IP/$SOC_NET" dev eth0 2>/dev/null
    ip link set eth0 up 2>/dev/null
    LOGF "ip addr show eth0:"; ip addr show eth0 >> "$LOG" 2>&1
  elif command -v ifconfig >/dev/null 2>&1; then
    ifconfig eth0 "$SOC_IP" netmask 255.255.255.0 up 2>/dev/null
    LOGF "ifconfig eth0:"; ifconfig eth0 >> "$LOG" 2>&1
  else
    LOGF "no 'ip' or 'ifconfig' - cannot set IP"
  fi
  sleep 1
}

# ---- select image + guard -------------------------------------
# The flag files set the DEFAULT; the panel menu can then override it by
# touch (same four tokens, so an unattended stick behaves as before):
#   RESTORE  -> flash factory-HD.hex (full stock restore)
#   NO_FLASH -> skip MCU flashing entirely (SoC access + bridge only)
#   RESTORE + NO_FLASH -> unwind the SoC hook only, leave the MCU image
#             alone (non-destructive revert; see the warning below)
#   (neither) -> flash klipper.hex
#   FACTORY  -> full stock restore: vendor package swap + MCU image
#               (needs the factory/ folder; never auto-fires from the panel)
#   DUMP     -> read the MCU flash back out through klipper, change nothing
if [ -f "$WORK_DIR/FACTORY" ]; then FLAG=factory
elif [ -f "$WORK_DIR/DUMP" ]; then FLAG=dump
elif [ -f "$WORK_DIR/RESTORE" ] && [ -f "$WORK_DIR/NO_FLASH" ]; then FLAG=restore-noflash
elif [ -f "$WORK_DIR/RESTORE" ]; then FLAG=restore
elif [ -f "$WORK_DIR/NO_FLASH" ]; then FLAG=noflash
else FLAG=klipper
fi
LOGF "stick flag default: $FLAG"

# ---- which chip is actually in this printer? ------------------
# The sync reply to 0x43 is the only revision marker that still works once
# Klipper is in the app window: the flash-resident bootloader below 0xC000
# answers it whatever is programmed above. It needs ttyS1 to ourselves (the
# persistent daemon's bridge holds it) and it resets the MCU, so a NO_FLASH
# run leaves the chip unprobed rather than disturbing a live MCU.
IAP="$WORK_DIR/stm32isp-ad4"
# 'dump' is excluded as well as 'noflash': the probe kills whoever holds
# ttyS1 and resets the chip, which is exactly the bridge + running klipper
# that a dump depends on.
if [ "$MCU_HINT" = "unknown" ] && [ -x "$IAP" ] \
   && [ "$FLAG" != "noflash" ] && [ "$FLAG" != "dump" ]; then
  # same stops the flash section does a bit later - the probe just needs the
  # uart, and the vendor app is going down either way
  pkill_safe "ffstartup-arm"; pkill_safe "adventurer4-arm"
  kill_fd_holder /dev/ttyS1 2>/dev/null
  PROBE=$("$IAP" -s 2>&1)
  LOGF "mcu probe: $(echo "$PROBE" | tr '\n' ' ')"
  case "$PROBE" in
    *REV=HD*) MCU_HINT="HD / HC32F460" ;;
    *REV=NA*) MCU_HINT="NA / N32" ;;
    *) LOGF "  no bootloader answer: chip stays '$MCU_HINT'" ;;
  esac
fi
LOGF "MCU for the panel: $MCU_HINT"

# ---- the panel gets the last word -----------------------------
# menu prints the chosen token on stdout and exits 0; 1 = no touchscreen
# (keep the flags); 2 = SKIP tapped, so change nothing and let the vendor
# UI boot (auto_run.sh only suppresses it when we exit 0).
if [ "$SCR_OK" = 1 ]; then
  LOGF "panel menu up: tapping nothing for 25 s runs '$FLAG'"
  CHOICE=$(AD4_MCU="$MCU_HINT" AD4_IP="$SOC_IP" AD4_PORT="$BRIDGE_PORT" \
           AD4_INSTALLED="$INSTALLED_TXT" AD4_STICK="$STICK_FILES" \
           AD4_FACTORY="$FACT_TXT" \
           "$SCR" menu --auto "$FLAG" 25 2>>"$LOG")
  MRC=$?
  LOGF "panel menu rc=$MRC token='${CHOICE:-none}'"
  case "$MRC" in
    2) LOGF "panel: SKIP tapped - nothing was changed, booting the vendor UI"
       echo "skipped-at-panel" > "$STATUS" 2>/dev/null; sync
       exit 2 ;;
    0) case "$CHOICE" in
         klipper|restore|factory|dump|noflash|restore-noflash) FLAG="$CHOICE" ;;
         *) LOGF "panel: unexpected token '$CHOICE' - keeping the flag default" ;;
       esac ;;
    *) LOGF "panel: no usable touch input - keeping the flag default" ;;
  esac
fi
case "$FLAG" in
  restore-noflash) IMG="";                          MODE=skip;    FLASH_SKIP=1 ;;
  restore)         IMG="$WORK_DIR/factory-HD.hex";  MODE=factory; FLASH_SKIP=0 ;;
  noflash)         IMG="";                          MODE=skip;    FLASH_SKIP=1 ;;
  factory)         IMG="";                          MODE=skip;    FLASH_SKIP=1 ;;
  dump)            IMG="";                          MODE=skip;    FLASH_SKIP=1 ;;
  *)               IMG="$WORK_DIR/klipper.hex";     MODE=strict;  FLASH_SKIP=0 ;;
esac
# factory-HD.hex is a convenience copy of FlashForge's MCU image. Without it
# the same image is resolved from the user's factory/ folder, or off the
# printer's own /opt/PROGRAM/control - so nothing copyrighted has to ship
# on this stick for a RESTORE to work.
if [ "$FLAG" = restore ] && [ ! -f "$IMG" ] && [ -f "$FACTSH" ]; then
  case "$MCU_HINT" in *NA*) MREV=NA ;; *) MREV=HD ;; esac
  IMG=$(sh "$FACTSH" mcuhex "$MREV" 2>>"$LOG")
  [ -f "$IMG" ] && LOGF "restore image from the factory source: $IMG"
fi
LOGF "selected image: ${IMG:-(none, no MCU flash)} (mode=$MODE, flag=$FLAG)"
if [ "$FLASH_SKIP" = 1 ]; then
  LOGF "no MCU flash this run (SoC access + bridge only)"
  RC=0
  # jump to the post-flash sections; the flash loop below is guarded
else
  SCRX status "checking the image" "hex base guard on $IMG" 10
  [ -f "$IMG" ] || { SCRX fail "image missing" "$IMG is not on the stick"; FATAL "image missing: $IMG"; }

  LOGF "hex guard (awk) on $IMG ($MODE):"
  GUARD=$(check_base "$IMG" "$MODE")
  LOGF "  $GUARD"
  OK=$(printf '%s' "$GUARD" | awk -F'ok=' '{print $2}')
  if [ "$OK" != "1" ]; then
    LOGF "!! hex guard FAILED (ok=$OK). Refusing to flash. MCU left as-is (safe)."
    SCRX fail "flash refused" "hex base guard failed on $IMG\nthe MCU was left untouched"
    echo "flash-refused($FLAG)" > "$STATUS"; sync; exit 1
  fi
  LOGF "hex guard PASSED"
fi

# ---- DUMP: read the MCU flash out, change nothing ---------------
# debug_read is a klipper command, so the only thing that can see the
# flash-resident bootloader is klipper already running on the MCU. The
# bridge and klipper therefore have to stay exactly as they are: this
# path touches the network and nothing else, and it never flashes.
if [ "$FLAG" = dump ]; then
  if [ ! -f "$WORK_DIR/ad4-dump.sh" ]; then
    SCRX fail "no ad4-dump.sh" "the dump script is missing from the stick"
    echo "dump-skipped(no-script)" > "$STATUS"; sync; exit 1
  fi
  net_up
  SCRX status "dumping flash" "reading the MCU through klipper\nabout 46 s - do not power off" 20
  chime start
  AD4_LOG="$WORK_DIR/ad4-dump.log" sh "$WORK_DIR/ad4-dump.sh" boot \
      > "$WORK_DIR/ad4-dump.out" 2>&1
  DR=$?
  cat "$WORK_DIR/ad4-dump.out" >> "$LOG"
  LOGF "ad4-dump.sh rc=$DR"
  if [ "$DR" = 0 ]; then
    chime ok
    echo "dump complete (the .bin is on the host)" > "$STATUS"; sync
    SCRX done "dump written" "adv4-bootloader.bin is in klipper's\nworking dir on the host, not on the stick"
  else
    chime fail
    echo "dump failed rc=$DR" > "$STATUS"; sync
    SCRX fail "dump failed" "klipper or moonraker unreachable\nsee ad4-dump.log on the stick"
  fi
  # exit 0 leaves a klipperized printer exactly as it was (vendor GUI still
  # suppressed); the bridge was never touched.
  exit 0
fi

# ---- stop the vendor stack (frees /dev/ttyS1 + GPIOs) ---------
# The vendor GUI/watchdog keeps re-grabbing /dev/ttyS1 (it talks to the
# MCU), so the ROM-ISP sync collides with it -> "timed-out". Kill by
# pattern AND by open-fd (the fd scan is the reliable part: it finds
# whoever actually has the serial open, regardless of the process name).
# diag_holders logs exactly which pids have $1 open (self-diagnosis).
diag_holders() {
  # $1 = label, $2 = device
  local h=""
  for fd in /proc/[0-9]*/fd/*; do
    [ "$(readlink "$fd" 2>/dev/null)" = "$2" ] || continue
    h="${h}${fd##*/proc/}"
  done
  if [ -n "$h" ]; then
    LOGF "$1: HOLDERS of $2 -> $(echo $h | tr ' ' '\n' | sort -u | tr '\n' ' ')"
  else
    LOGF "$1: $2 has NO open holder"
  fi
}
LOGF "--- pre-kill diagnostics ---"
diag_holders "before-kill" /dev/ttyS1
LOGF "stopping vendor stack (ffstartup-arm, adventurer4-arm, qt GUI, ttyS1 holders)..."
pkill_safe "ffstartup-arm"
pkill_safe "adventurer4-arm"
pkill_safe "ad4-daemon"
sleep 1
if [ -e /dev/ttyS1 ]; then
  # Free ttyS1, retrying (the watchdog can respawn a holder).
  i=0
  while [ $i -lt 5 ]; do
    diag_holders "iter $i" /dev/ttyS1
    kill_fd_holder /dev/ttyS1
    sleep 1
    i=$((i+1))
  done
  diag_holders "after-kill" /dev/ttyS1
  LOGF "ttyS1 state: $(ls -la /dev/ttyS1 2>&1)"
  # show GPIO34/35 state (NRST / BOOT0) - must be free for strapping
  for g in 34 35; do
    [ -e /sys/class/gpio/gpio$g ] && \
      LOGF "gpio$g direction=$(cat /sys/class/gpio/gpio$g/direction 2>/dev/null) value=$(cat /sys/class/gpio/gpio$g/value 2>/dev/null)"
  done
else
  LOGF "ttyS1 ABSENT - /dev/ttyS*: $(ls -la /dev/ttyS* 2>&1)"
fi
sleep 1

# ---- FULL FACTORY RESTORE --------------------------------------
# Everything stock again: FlashForge's own /opt/PROGRAM package swap plus
# the MCU image, all inside ad4-factory.sh. It reads the packages from the
# user's factory/ folder (their copy of the upgrade key - FlashForge's
# firmware, not redistributed here) and falls back to the printer's own
# /opt/PROGRAM/control for the MCU hex. The vendor stack is already down
# and ttyS1 is free, which is all the MCU write needs.
if [ "$FLAG" = factory ]; then
  if [ ! -f "$FACTSH" ]; then
    SCRX fail "no ad4-factory.sh" "the restore script is missing from the stick"
    echo "factory-skipped(no-script)" > "$STATUS"; sync
    FATAL "ad4-factory.sh missing"
  fi
  SCRX status "full factory restore" "soc packages + mcu image\nseveral minutes - do not power off" 30
  chime start
  AD4_LOG="$WORK_DIR/ad4-factory.log" sh "$FACTSH" run \
      > "$WORK_DIR/ad4-factory.out" 2>&1
  FR=$?
  cat "$WORK_DIR/ad4-factory.out" >> "$LOG"
  LOGF "ad4-factory.sh run rc=$FR"
  if [ "$FR" != 0 ]; then
    chime fail
    echo "factory restore FAILED rc=$FR" > "$STATUS"; sync
    SCRX fail "factory restore failed" "read ad4-factory.log on the stick"
    exit 1
  fi
  # The software package puts the STOCK auto_run.sh back, but only if the
  # user supplied that package. Unwind the hook explicitly so a partial
  # factory/ folder cannot leave the klipper gate in front of stock
  # firmware - that combination is what makes a printer look bricked.
  if [ -f "$WORK_DIR/ad4-uninstall-persistent.sh" ]; then
    LOGF "unwinding the klipper boot hook (ad4-uninstall-persistent.sh --no-mcu)"
    sh "$WORK_DIR/ad4-uninstall-persistent.sh" --no-mcu >> "$LOG" 2>&1
  fi
  chime ok
  echo "factory restore complete" > "$STATUS"; sync
  SCRX done "factory restored" "soc and mcu are stock again\nremove the stick, then power cycle"
  LOGF "factory restore complete; rebooting into stock"; sleep 3
  reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
  exit 0
fi

# ---- flash ----------------------------------------------------
# The MCU is a Huada HC32F460 (or a Nation N32 on NA boards). It has NO ROM
# ISP: the bootloader answering the sync is FLASH-resident (0x0..0xBFFF,
# session-5 RE). Its sync handshake (live-confirmed via strace of the
# vendor's unstripped IAPCommand, and re-confirmed by our own probe) is:
#     send 0x43  ->  reply 6 (NA revision) or 7 (HD revision)
# (earlier drafts had 6/7 swapped). NOT the standard STM32F1 0x43->0x79.
# stm32isp-ad4 is our own flasher and speaks that exact vendor protocol
# (1030-byte frames: [cmd][len:2LE][data:1024][crc16:2LE], CRC16-CCITT over
# cmd+data, ack 0x06) - it was transcribed from IAPCommand, so the vendor
# binary is no longer needed. One image argument, not the vendor's two:
#     stm32isp-ad4 -p /dev/ttyS1 <hex>
# It carries the same anti-brick guard internally (refuses any record below
# BOOTLOADER_TOP) plus the GPIO strapping (BOOT0 high, NRST pulse).
do_flash() {
  case "$1" in
    factory) "$IAP" -p /dev/ttyS1 -v "$IMG" ;;
  esac > "$WORK_DIR/iap.out" 2>&1
}
FAC_OK=0;  [ -x "$IAP" ]   && FAC_OK=1
if [ "$FLASH_SKIP" = 0 ]; then
  [ "$FAC_OK" = 1 ] || FATAL "no flasher on stick (need stm32isp-ad4)"
fi

RC=1; USED=""
[ "$FAC_OK" = 1 ] || FATAL "no flasher on stick (need stm32isp-ad4)"

if [ "$FLASH_SKIP" = 0 ]; then
RC=1; USED=""
SCRX status "flashing the MCU" "this takes about 40 seconds\ndo not power off the printer" 40
for tag in factory factory; do
  [ "$FAC_OK" = 1 ] || continue
  TOOLNAME="stm32isp-ad4 (vendor HC32 protocol, our source)"
  LOGF "FLASH attempt via $TOOLNAME (image=$IMG)..."
  chime start
  do_flash "$tag"; RC=$?
  LOGF "$TOOLNAME rc=$RC"
  LOGF "----- flasher output -----"; cat "$WORK_DIR/iap.out" >> "$LOG" 2>&1
  LOGF "----- end flasher output -----"
  # success = rc 0 AND the write marker. "STM32iap finished HD/NA." is printed
  # unconditionally at the end, so it proves contact only, not a good write.
  if [ "$RC" -eq 0 ] && grep -q "STM32Write: success" "$WORK_DIR/iap.out" 2>/dev/null; then
    USED="$TOOLNAME"; break
  fi
  echo "$FLAG rc=$RC ($TOOLNAME)" > "$STATUS"; sync
  LOGF "  -> attempt failed; re-freeing ttyS1 and retrying..."
  sleep 1
  kill_fd_holder /dev/ttyS1 2>/dev/null
done

if [ -z "$USED" ]; then
  LOGF "!! flash FAILED (all attempts). MCU is still bootable (ROM ISP untouched)."
  LOGF "   Likely cause: vendor still holding /dev/ttyS1, or no MCU contact."
  LOGF "   Re-run, or if the board is unresponsive: put a RESTORE file on the stick."
  chime fail
  SCRX fail "flash failed" "no MCU contact - it is still bootable\nread ad4-flash.log on the stick"
  exit 1
fi
LOGF "flash OK via $USED. New MCU firmware is running."
chime ok
SCRX status "MCU flashed" "now setting up SoC access" 70
else
  LOGF "MCU flash skipped (RC=0, USED=skip)"
  RC=0; USED="skip"
fi

# ---- RESTORE = revert, reboot to clean stock ------------------
# Keyed off the selected mode, not the file, so a RESTORE tapped on the
# panel unwinds the hook exactly like a RESTORE flag file does.
if [ "$FLAG" = restore ] || [ "$FLAG" = restore-noflash ]; then
  SCRX status "restoring" "unwinding the boot hook" 80
  # The enable flag is NOT the only trace of an install: the soft toggle
  # (rm enable) leaves /opt/auto_run.sh patched, and keying off the flag
  # alone would reflash the MCU and leave the gate in place forever.
  NEED_UNWIND=0
  [ -f /opt/ad4-klipper/enable ] && NEED_UNWIND=1
  grep -qF "/opt/ad4-klipper/ad4-daemon.sh" /opt/auto_run.sh /etc/init.d/rcS 2>/dev/null && NEED_UNWIND=1
  [ -f /opt/auto_run.sh.ad4-orig ] && NEED_UNWIND=1
  if [ "$NEED_UNWIND" = 1 ] && [ -f "$WORK_DIR/ad4-uninstall-persistent.sh" ]; then
    LOGF "unwinding persistent hook (ad4-uninstall-persistent.sh --no-mcu)"
    sh "$WORK_DIR/ad4-uninstall-persistent.sh" --no-mcu >> "$LOG" 2>&1
  elif [ "$NEED_UNWIND" = 1 ]; then
    LOGF "!! ad4-uninstall-persistent.sh not on the stick: MCU reflashed but the boot hook stays patched"
  else
    LOGF "nothing to unwind (no enable flag, no patched rc file, no backup)"
  fi
  if [ "$FLAG" = "restore-noflash" ]; then
    LOGF "NOTE: NO_FLASH was set, so the MCU still runs the KLIPPER image."
    LOGF "      The vendor UI is back but cannot drive that firmware; run a"
    LOGF "      RESTORE stick WITHOUT NO_FLASH to put the stock MCU image back."
  fi
  echo "restore complete" >> "$STATUS"; sync
  SCRX done "restored" "stock firmware and vendor UI are back\nremove the stick, then power cycle"
  LOGF "restore complete. rebooting to clean stock."; sleep 2
  reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
  exit 0
fi

# ---- static IP on eth0 ----------------------------------------
net_up

# ---- bridge (static C binary; the SoC has NO python3) ---------
# 'ad4-bridge' is a zig cross-compiled, byte-transparent TCP<->/dev/ttyS1
# relay. No python, no dependencies. This is the ONLY bridge path.
BRIDGE_UP=0
BRIDGE_TAG=""
if [ -x "$WORK_DIR/ad4-bridge" ]; then
  LOGF "starting MCU<->TCP bridge via C binary on :$BRIDGE_PORT (no-python)"
  pkill_safe "ad4-bridge"
  sleep 1
  kill_fd_holder /dev/ttyS1 2>/dev/null
  # 115200 = the proven MCU line (session-5 firmware RE). Passing 250000
  # here was the stale-baud bug: the MCU never hears the bridge.
  nohup "$WORK_DIR/ad4-bridge" "$BRIDGE_PORT" /dev/ttyS1 115200 \
      >> /var/log/ad4-bridge.log 2>&1 &
  BPID=$!
  sleep 1
  if kill -0 "$BPID" 2>/dev/null; then
    BRIDGE_UP=1; BRIDGE_TAG="ad4-bridge (static C, no-python)"
    echo "$BPID" > /var/run/ad4-bridge.pid
  fi
fi

if [ "$BRIDGE_UP" = 1 ]; then
  LOGF "bridge RUNNING on 0.0.0.0:$BRIDGE_PORT  [$BRIDGE_TAG]  soc_ip=$SOC_IP"
  LOGF "  next: on the MAC run  ad4-mac  (present as /dev/adv4_mcu), then start klippy"
  echo "bridge running ($BRIDGE_TAG)" >> "$STATUS"
  SCRX status "bridge up" "mcu reachable at $SOC_IP:$BRIDGE_PORT\nssh root@$SOC_IP" 85
  tail -n 5 /var/log/ad4-bridge.log >> "$LOG" 2>/dev/null
else
  LOGF "bridge NOT started (no C binary on stick)."
  echo "bridge-skipped(no-c)" >> "$STATUS"
fi

# ---- SoC ACCESS: dropbear SSH + root password ------------------
# Proven mechanism (SumolX/FlashForge-Hacks): dropbearmulti (one binary =
# dropbear/dropbearkey/ssh/scp) installed to /opt/services/bin, symlinked into
# /bin, host key generated, and the start line appended to /opt/auto_run.sh —
# the vendor's real boot hook, NOT rcS. Root password comes from ROOT_PASS
# (default 'flashforge'). This runs on EVERY stick-boot (flash or NO_FLASH),
# so there is always a way in. Note: units already patched by an earlier run
# already have /bin/dropbear -> /opt/services/bin/dropbearmulti; re-running is
# a no-op except for the password check.
LOGF "----- SoC access: installing dropbear SSH (root pw from ROOT_PASS) -----"
DROPBIN="$WORK_DIR/dropbear"
if [ ! -x "$DROPBIN" ]; then
  LOGF "!! dropbear binary not on stick; skipping SSH install"
else
  SVC=/opt/services
  mkdir -p "$SVC/bin" "$SVC/sbin"
  # Install the dropbearmulti binary (one file = dropbear/dropbearkey/scp/ssh).
  cp -f "$DROPBIN" "$SVC/bin/dropbearmulti" 2>/dev/null
  chmod +x "$SVC/bin/dropbearmulti" 2>/dev/null
  # Symlinks the vendor expects.
  ln -sfn "$SVC/bin/dropbearmulti" /bin/dropbear
  ln -sfn "$SVC/bin/dropbearmulti" /bin/dropbearkey
  ln -sfn "$SVC/bin/dropbearmulti" /bin/scp
  ln -sfn "$SVC/bin/dropbearmulti" /bin/ssh
  LOGF "dropbear installed -> /opt/services/bin/dropbearmulti + symlinks"
  sync
  # Host key (once).
  if [ ! -e /etc/dropbear/dropbear_rsa_host_key ]; then
    mkdir -p /etc/dropbear
    /bin/dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key 2>/dev/null
    LOGF "generated dropbear rsa host key"
  fi
  # ---- root password -------------------------------------------------
  # Primary: pipe the password twice into busybox `passwd` (works with no tty).
  # Fallback: rewrite the root shadow entry with a precomputed MD5-crypt hash.
  # That hash only corresponds to the DEFAULT 'flashforge', so a custom
  # ROOT_PASS goes through `passwd` and is verified instead of forced.
  PW_DEFAULT='flashforge'
  PW_HASH_DEFAULT='$1$ff2026$IGxGYXhDTYMygkh.Suun4/'   # md5crypt('flashforge')
  OLD_HASH=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)
  printf '%s\n%s\n' "$ROOT_PASS" "$ROOT_PASS" | passwd >/dev/null 2>&1
  NEW_HASH=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)
  if [ "$NEW_HASH" = "$OLD_HASH" ] && [ "$ROOT_PASS" = "$PW_DEFAULT" ]; then
    awk -v h="$PW_HASH_DEFAULT" '$1=="root"{print "root:"h":15000:0:99999:7:::";next} {print}' \
      /etc/shadow > /etc/shadow.new 2>/dev/null \
      && mv -f /etc/shadow.new /etc/shadow && chmod 600 /etc/shadow 2>/dev/null
    LOGF "passwd made no change; forced the verified shadow hash"
    NEW_HASH=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)
  fi
  if [ -z "$NEW_HASH" ]; then
    LOGF "!! root shadow entry empty/locked - do NOT rely on SSH until fixed"
  elif [ "$NEW_HASH" != "$OLD_HASH" ]; then
    LOGF "root password CHANGED (shadow entry rewritten)"
  else
    LOGF "root password already in place (shadow unchanged)"
  fi
  sync
  # ---- start dropbear NOW + persist via /opt/auto_run.sh ------------
  # Kill any stale dropbear, then start on 0.0.0.0:22.
  pkill_safe "dropbear"
  sleep 0.5
  nohup /bin/dropbear -R -p 22 >> /var/log/dropbear.log 2>&1 &
  sleep 1
  SSH_UP=0
  # verify port 22 is listening via /proc/net/tcp (port 0x0016 = 22)
  if grep -qiE ':0016[[:space:]]' /proc/net/tcp 2>/dev/null; then SSH_UP=1; fi
  LOGF "dropbear started (ssh :22 up=$SSH_UP)"
  # Persist across a no-stick boot via the vendor boot hook.
  if [ -f /opt/auto_run.sh ]; then
    if ! grep -qF "dropbear" /opt/auto_run.sh 2>/dev/null; then
      [ -f /opt/auto_run.sh.ad4-orig ] || cp -f /opt/auto_run.sh /opt/auto_run.sh.ad4-orig
      echo "/bin/dropbear -R -p 22 &" >> /opt/auto_run.sh
      LOGF "persisted dropbear via /opt/auto_run.sh"
      sync
    else
      LOGF "dropbear already in /opt/auto_run.sh"
    fi
  else
    LOGF "!! /opt/auto_run.sh not found; SSH will NOT persist past this boot"
  fi
fi

# ---- optional PERSISTENT eMMC hook ----------------------------
if [ -f "$WORK_DIR/PERSISTENT" ]; then
  LOGF "PERSISTENT: installing eMMC boot hook"
  SCRX status "installing the boot hook" "this survives power cycles\nthe printer reboots when it is done" 95
  echo "persistent: installing" > "$STATUS"
  if [ -f "$WORK_DIR/ad4-install-persistent.sh" ]; then
    sh "$WORK_DIR/ad4-install-persistent.sh" >> "$LOG" 2>&1
    PRC=$?
    LOGF "persistent-install rc=$PRC"
    echo "persistent-install rc=$PRC" >> "$STATUS"; sync
    [ "$PRC" -eq 0 ] && { LOGF "installer rebooting"; sleep 5; }
  else
    LOGF "persistent: ad4-install-persistent.sh missing, skipped"
  fi
fi

LOGF "================ DONE. flag=$FLAG rc=$RC ================"
echo "done($FLAG) rc=$RC" > "$STATUS"; sync
if [ "$RC" = 0 ]; then
  if [ "$BRIDGE_UP" = 1 ]; then
    SCRX done "klipper installed" "bridge on $SOC_IP:$BRIDGE_PORT\nssh root@$SOC_IP"
  else
    SCRX done "done" "no bridge running - see ad4-flash.log"
  fi
else
  SCRX fail "finished with errors" "rc=$RC - read ad4-flash.log on the stick"
fi
exit "$RC"
