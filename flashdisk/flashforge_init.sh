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
#      via the C flasher (IAPCommand). Anti-brick: the flasher
#      refuses any image whose base is <= 0x00008000, so the ROM
#      ISP at 0x0000 is never touched => never brickable.
#   2. Give eth0 a static IP (default 192.168.10.10/24).
#   3. Start the MCU<->TCP bridge (ONLY if python3 exists; otherwise
#      it logs and skips — the flash itself never needs python).
#
# Stick layout (FAT32, files at root):
#   flashforge_init.sh   this script
#   screwflag            empty file (skips machine-type check)
#   IAPCommand           C ARM flasher (does the GPIO strapping)
#   klipper.hex          Klipper @ 0x000C0000
#   factory-HD.hex       known-good restore image (never delete)
#   ad4-serial-bridge.py the SoC<->MCU bridge (needs python3)
#   ad4-net.conf         OPTIONAL SOC_IP=... BRIDGE_PORT=...
#   RESTORE              OPTIONAL: flash factory-HD.hex (full revert)
#   PERSISTENT           OPTIONAL: install eMMC boot hook + reboot
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
  # $1 = pattern
  if command -v pkill >/dev/null 2>&1; then pkill -f "$1" 2>/dev/null; return; fi
  if command -v killall >/dev/null 2>&1; then killall "$1" 2>/dev/null; return; fi
  # last resort: scan /proc
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) kill -9 "${p#/proc/}" 2>/dev/null ;;
    esac
  done
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

# ---- buzzer (PWM piezo) ----------------------------------------
# The board's "sound" is NOT a wav file: the vendor GUI (buzzer.cpp)
# drives a piezo via a PWM sysfs node (pwmchip0/pwm0) and fires tone
# sequences on events (boot, print-start, print-end, button presses).
# We drive the SAME node directly to play a "flash done" chime, so no
# vendor code is touched. If the node is missing/locked we just skip
# (the flash is already done by then; sound is a bonus).
# PWM_BASE is overridable for testing on the dev machine.
PWM_BASE="${PWM_BASE:-/sys/class/pwm}"
PWM_DIR=""
PWM_OK=0
pwm_setup() {
  # Find a usable pwm dir that has an enable file (export if needed).
  for base in "$PWM_BASE" /sys/devices/10000000.palmbus/10005000.pwm/pwm; do
    for chip in pwmchip0 pwmchip1 pwmchip2 pwmchip3; do
      d="$base/$chip"
      [ -d "$d" ] || continue
      # if pwm0 not exported yet, try to export it
      if [ ! -d "$d/pwm0" ] && [ -e "$d/export" ]; then
        echo 0 > "$d/export" 2>/dev/null
        i=0; while [ ! -d "$d/pwm0" ] && [ $i -lt 20 ]; do sleep 0.1 2>/dev/null || break; i=$((i+1)); done
      fi
      if [ -e "$d/pwm0/enable" ]; then PWM_DIR="$d"; PWM_OK=1; break; fi
    done
    [ "$PWM_OK" = 1 ] && break
  done
  return $([ "$PWM_OK" = 1 ] && echo 0 || echo 1)
}
tone() {
  # $1 = period in ns (1e9/freq), $2 = on-duration in seconds
  [ "$PWM_OK" = 1 ] || return 0
  echo "$1" > "$PWM_DIR/pwm0/period"    2>/dev/null
  echo $(( $1 / 2 )) > "$PWM_DIR/pwm0/duty_cycle" 2>/dev/null
  echo 1 > "$PWM_DIR/pwm0/enable" 2>/dev/null
  sleep "$2" 2>/dev/null
  echo 0 > "$PWM_DIR/pwm0/enable" 2>/dev/null
}
# chime: $1 = ok | fail | start
chime() {
  [ "$PWM_OK" = 1 ] || { LOGF "buzzer: no PWM node (sound skipped)"; return 0; }
  case "$1" in
    start)
      LOGF "chime: start blip"
      tone 666667 0.12   # 1.5 kHz ~120 ms
      ;;
    ok)
      LOGF "chime: flash-DONE (rising two-tone)"
      tone 400000 0.18   # 2.5 kHz ~180 ms
      sleep 0.05 2>/dev/null
      tone 285714 0.30   # 3.5 kHz ~300 ms  -> pleasant "ding"
      ;;
    fail)
      LOGF "chime: flash-FAILED (low triple buzz)"
      i=0; while [ $i -lt 3 ]; do
        tone 1000000 0.25   # 1.0 kHz ~250 ms
        sleep 0.18 2>/dev/null
        i=$((i+1))
      done
      ;;
  esac
  echo 0 > "$PWM_DIR/pwm0/enable" 2>/dev/null
}
pwm_setup
if [ "$PWM_OK" = 1 ]; then
  LOGF "buzzer ready: $PWM_DIR (PWM piezo, sysfs)"
else
  LOGF "buzzer: no PWM node found under $PWM_BASE (sound will be skipped)"
fi

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

# ---- inventory of what's on the stick -------------------------
LOGF "stick inventory ($WORK_DIR):"
ls -la "$WORK_DIR" >> "$LOG" 2>&1
for f in screwflag stm32isp-ad4 IAPCommand klipper.hex factory-HD.hex ad4-serial-bridge.py RESTORE PERSISTENT ad4-net.conf; do
  [ -f "$WORK_DIR/$f" ] && LOGF "    present: $f ($(wc -c < "$WORK_DIR/$f" 2>/dev/null | tr -d ' ') bytes)" \
                        || LOGF "    absent : $f"
done

# ---- anti-brick hex base guard, PURE awk (no python) ----------
# Emits "minabs=0x... maxabs=0x... ok=0/1" and sets a global OK.
check_base() {
  # $1 = hex path, $2 = strict|factory
  # Portable awk (mawk): comparison ops MUST be space-delimited
  # (mawk mis-tokenizes `x!=":"` and the like), no bit-shifts.
  # Type-04 AND type-05 are (value << 16), matching the C tool.
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

# ---- select image + guard -------------------------------------
if [ -f "$WORK_DIR/RESTORE" ]; then
  IMG="$WORK_DIR/factory-HD.hex"; FLAG=restore; MODE=factory
else
  IMG="$WORK_DIR/klipper.hex";    FLAG=klipper; MODE=strict
fi
LOGF "selected image: $IMG (mode=$MODE, flag=$FLAG)"
if [ ! -f "$IMG" ]; then FATAL "image missing: $IMG"; fi

LOGF "hex guard (awk) on $IMG ($MODE):"
GUARD=$(check_base "$IMG" "$MODE")
LOGF "  $GUARD"
OK=$(printf '%s' "$GUARD" | awk -F'ok=' '{print $2}')
if [ "$OK" != "1" ]; then
  LOGF "!! hex guard FAILED (ok=$OK). Refusing to flash. MCU left as-is (safe)."
  echo "flash-refused($FLAG)" > "$STATUS"; sync; exit 1
fi
LOGF "hex guard PASSED"

# ---- stop the vendor stack (frees /dev/ttyS1 + GPIOs) ---------
LOGF "stopping vendor stack (ffstartup-arm, adventurer4-arm)..."
pkill_safe "ffstartup-arm"
pkill_safe "adventurer4-arm"
sleep 2
if [ -e /dev/ttyS1 ]; then
  if command -v fuser >/dev/null 2>&1; then fuser -k /dev/ttyS1 2>/dev/null; fi
  LOGF "ttyS1 present: $(ls -la /dev/ttyS1 2>&1)"
else
  LOGF "ttyS1 ABSENT (expected on some boards) - /dev/ttyS1: $(ls -la /dev/ttyS* 2>&1)"
fi
sleep 1

# ---- flash ----------------------------------------------------
# Prefer the open-source flasher (stm32isp-ad4, verbose) if present;
# otherwise fall back to the factory IAPCommand. Both take two hex
# positional args (na, hd); we pass the chosen image to both and force
# the HD/H32 revision (this board). The base guard above already passed.
FLASHER=""
FLASH_VERB=""
if [ -x "$WORK_DIR/stm32isp-ad4" ]; then
  FLASHER="$WORK_DIR/stm32isp-ad4"; FLASH_VERB="-v"; TOOLNAME="stm32isp-ad4 (open-source)"
elif [ -x "$WORK_DIR/IAPCommand" ]; then
  FLASHER="$WORK_DIR/IAPCommand"; FLASH_VERB=""; TOOLNAME="IAPCommand (factory)"
else
  FATAL "no flasher on stick (need stm32isp-ad4 or IAPCommand)"
fi
LOGF "FLASHING via $TOOLNAME (base guard passed, image=$IMG, mode=$MODE)..."
chime start
# stm32isp-ad4:  -r hd  -p /dev/ttyS1  -v   <na.hex> <hd.hex>
# IAPCommand   :  <img>  <img>
case "$TOOLNAME" in
  *open-source*) "$FLASHER" -r hd -p /dev/ttyS1 $FLASH_VERB "$IMG" "$IMG" > "$WORK_DIR/iap.out" 2>&1 ;;
  *factory*)     "$FLASHER" "$IMG" "$IMG" > "$WORK_DIR/iap.out" 2>&1 ;;
esac
RC=$?
LOGF "$TOOLNAME rc=$RC"
LOGF "----- flasher output -----"; cat "$WORK_DIR/iap.out" >> "$LOG" 2>&1
LOGF "----- end flasher output -----"
echo "$FLAG rc=$RC ($TOOLNAME)" > "$STATUS"; sync

if [ "$RC" -ne 0 ]; then
  LOGF "!! flash FAILED rc=$RC. MCU is still bootable (ROM ISP untouched)."
  LOGF "   Re-run the same command, or restore: put a RESTORE file on the stick."
  chime fail
  exit "$RC"
fi
LOGF "flash OK (rc=0). New MCU firmware is running."
chime ok

# ---- RESTORE = full revert, reboot to clean stock -------------
if [ -f "$WORK_DIR/RESTORE" ]; then
  if [ -f /opt/ad4-klipper/enable ] && [ -f "$WORK_DIR/ad4-uninstall-persistent.sh" ]; then
    LOGF "unwinding persistent hook (ad4-uninstall-persistent.sh --no-mcu)"
    sh "$WORK_DIR/ad4-uninstall-persistent.sh" --no-mcu >> "$LOG" 2>&1
  fi
  echo "restore complete" >> "$STATUS"; sync
  LOGF "restore complete. rebooting to clean stock."; sleep 2
  reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null || true
  exit 0
fi

# ---- static IP on eth0 ----------------------------------------
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

# ---- bridge (ONLY if python3 exists) --------------------------
if command -v python3 >/dev/null 2>&1 && [ -f "$WORK_DIR/ad4-serial-bridge.py" ]; then
  LOGF "starting MCU<->TCP bridge on 0.0.0.0:$BRIDGE_PORT (python3 present)"
  pkill_safe "ad4-serial-bridge.py"
  nohup python3 "$WORK_DIR/ad4-serial-bridge.py" --tcp 0.0.0.0:"$BRIDGE_PORT" \
      --uart /dev/ttyS1 --baud 250000 >> /var/log/ad4-bridge.log 2>&1 &
  sleep 1
  if pid_alive "ad4-serial-bridge.py"; then
    LOGF "bridge RUNNING on 0.0.0.0:$BRIDGE_PORT (soc_ip=$SOC_IP)"
    echo "bridge running" >> "$STATUS"
  else
    LOGF "bridge FAILED to start (see /var/log/ad4-bridge.log)"
    echo "bridge-failed" >> "$STATUS"
  fi
else
  LOGF "bridge SKIPPED (python3 absent or no bridge file). Flash still complete."
  echo "bridge-skipped(no-python)" >> "$STATUS"
fi

# ---- optional PERSISTENT eMMC hook ----------------------------
if [ -f "$WORK_DIR/PERSISTENT" ]; then
  LOGF "PERSISTENT: installing eMMC boot hook"
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
exit "$RC"
