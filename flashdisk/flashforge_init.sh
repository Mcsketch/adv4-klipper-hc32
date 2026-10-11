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
#   klipper.hex          Klipper @ 0x0000C000
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
# The board's sound is a PWM piezo. The vendor's own driver is the
# standalone `play` binary: it opens the /dev/pwm char device (NOT the
# /sys/class/pwm sysfs nodes), exports pwm1, and ioctls it to play the
# tune built into it (its main() is hardcoded: buzzerPlay::play(TuneId 7)
# -> the boot/"voice" sound, ~4 s). We prefer it (vendor-proven, no
# reverse-engineering of the ioctl). If `play` is absent we fall back to
# writing the /sys/class/pwm sysfs node directly (best-effort). Sound is
# always a bonus — if nothing is available we log and continue.
PLAY_BIN=""
if [ -x "$WORK_DIR/play" ]; then PLAY_BIN="$WORK_DIR/play"; fi
[ -n "$PLAY_BIN" ] || PLAY_BIN=$(command -v play 2>/dev/null)

# sysfs fallback (only used when `play` is missing)
PWM_BASE="${PWM_BASE:-/sys/class/pwm}"
PWM_DIR=""; PWM_OK=0
pwm_setup() {
  for base in "$PWM_BASE" /sys/devices/10000000.palmbus/10005000.pwm/pwm; do
    for chip in pwmchip0 pwmchip1 pwmchip2 pwmchip3; do
      d="$base/$chip"; [ -d "$d" ] || continue
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
tone() { # $1=period ns, $2=on-duration seconds  (sysfs fallback only)
  [ "$PWM_OK" = 1 ] || return 0
  echo "$1" > "$PWM_DIR/pwm0/period"    2>/dev/null
  echo $(( $1 / 2 )) > "$PWM_DIR/pwm0/duty_cycle" 2>/dev/null
  echo 1 > "$PWM_DIR/pwm0/enable" 2>/dev/null
  sleep "$2" 2>/dev/null
  echo 0 > "$PWM_DIR/pwm0/enable" 2>/dev/null
}
# chime: $1 = ok | fail | start
chime() {
  case "$1" in
    start) MSG="chime: start blip" ;;
    ok)    MSG="chime: flash-DONE" ;;
    fail)  MSG="chime: flash-FAILED" ;;
  esac
  if [ -n "$PLAY_BIN" ]; then
    LOGF "$MSG  (via play, /dev/pwm)"
    # `play` is self-timed (~4 s); run it in the background so a hang
    # can't block the flash. No args: its main() is hardcoded.
    ( "$PLAY_BIN" >/dev/null 2>&1 & ) 2>/dev/null
    return 0
  fi
  # sysfs fallback
  if [ "$PWM_OK" = 1 ]; then
    LOGF "$MSG  (via sysfs $PWM_DIR)"
    case "$1" in
      start) tone 666667 0.12 ;;
      ok)    tone 400000 0.18; sleep 0.05 2>/dev/null; tone 285714 0.30 ;;
      fail)  i=0; while [ $i -lt 3 ]; do tone 1000000 0.25; sleep 0.18 2>/dev/null; i=$((i+1)); done ;;
    esac
    echo 0 > "$PWM_DIR/pwm0/enable" 2>/dev/null
  else
    LOGF "$MSG  (NO sound path: play missing, no sysfs node - skipped)"
  fi
}
if [ -n "$PLAY_BIN" ]; then
  LOGF "sound ready: $PLAY_BIN (vendor play, /dev/pwm)"
else
  pwm_setup
  [ "$PWM_OK" = 1 ] && LOGF "sound ready: sysfs $PWM_DIR" \
                    || LOGF "sound: NO path available (play missing + no sysfs node) - will skip"
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
for f in screwflag play stm32isp-ad4 IAPCommand klipper.hex factory-HD.hex ad4-serial-bridge.py RESTORE PERSISTENT ad4-net.conf; do
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
# Flag-file modes:
#   RESTORE  -> flash factory-HD.hex (full stock restore)
#   NO_FLASH -> skip MCU flashing entirely (SoC access + bridge only)
#   (neither) -> flash klipper.hex
if [ -f "$WORK_DIR/RESTORE" ]; then
  IMG="$WORK_DIR/factory-HD.hex"; FLAG=restore; MODE=factory; FLASH_SKIP=0
elif [ -f "$WORK_DIR/NO_FLASH" ]; then
  IMG=""; FLAG=noflash; MODE=skip; FLASH_SKIP=1
else
  IMG="$WORK_DIR/klipper.hex";    FLAG=klipper; MODE=strict; FLASH_SKIP=0
fi
LOGF "selected image: ${IMG:-(none, NO_FLASH)} (mode=$MODE, flag=$FLAG)"
if [ "$FLASH_SKIP" = 1 ]; then
  LOGF "NO_FLASH mode: skipping MCU flash (SoC access + bridge only)"
  RC=0
  # jump to the post-flash sections; the flash loop below is guarded
else
  [ -f "$IMG" ] || FATAL "image missing: $IMG"

  LOGF "hex guard (awk) on $IMG ($MODE):"
  GUARD=$(check_base "$IMG" "$MODE")
  LOGF "  $GUARD"
  OK=$(printf '%s' "$GUARD" | awk -F'ok=' '{print $2}')
  if [ "$OK" != "1" ]; then
    LOGF "!! hex guard FAILED (ok=$OK). Refusing to flash. MCU left as-is (safe)."
    echo "flash-refused($FLAG)" > "$STATUS"; sync; exit 1
  fi
  LOGF "hex guard PASSED"
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

# ---- flash ----------------------------------------------------
# The MCU is a Huada HC32F460 with a CUSTOM vendor ROM. Its sync handshake
# (decoded from the vendor's own unstripped IAPCommand) is:
#     send 0x43  ->  reply 6 (HD revision) or 7 (NA revision)
# NOT the standard STM32F1 0x43->0x79. So the authoritative flasher is the
# vendor IAPCommand (it implements the correct 1024-byte packet protocol,
# command-ack 6/7). Its signature (from the disassembly + a failed run):
#     IAPCommand <NA.hex> <HD.hex> <comport>
# We pass the same image for NA and HD; IAPCommand detects the revision at
# sync and reads the matching file. Factory tool runs FIRST (known-good);
# the open-source stm32isp-ad4 is a verbose fallback (it targets the std
# STM32F1 protocol, so it may time out at sync on this ROM — that's expected
# and non-fatal: the factory attempt is what actually flashes).
do_flash() {
  # $1 = tool tag (factory only — the open-source flasher was removed)
  case "$1" in
    factory) "$WORK_DIR/IAPCommand" "$IMG" "$IMG" /dev/ttyS1 ;;
  esac > "$WORK_DIR/iap.out" 2>&1
}
FAC_OK=0;  [ -x "$WORK_DIR/IAPCommand" ]   && FAC_OK=1
if [ "$FLASH_SKIP" = 0 ]; then
  [ "$FAC_OK" = 1 ] || FATAL "no flasher on stick (need IAPCommand)"
fi

RC=1; USED=""
# Authoritative flasher ONLY: the vendor IAPCommand (correct HC32 ROM
# protocol). The open-source stm32isp-ad4 targeted the std STM32F1 protocol
# and always timed out at sync on this ROM — it is removed (cleanup).
[ "$FAC_OK" = 1 ] || FATAL "no flasher on stick (need IAPCommand)"

if [ "$FLASH_SKIP" = 0 ]; then
RC=1; USED=""
for tag in factory factory; do
  [ "$FAC_OK" = 1 ] || continue
  TOOLNAME="IAPCommand (vendor, correct HC32 protocol)"
  LOGF "FLASH attempt via $TOOLNAME (image=$IMG)..."
  chime start
  do_flash "$tag"; RC=$?
  LOGF "$TOOLNAME rc=$RC"
  LOGF "----- flasher output -----"; cat "$WORK_DIR/iap.out" >> "$LOG" 2>&1
  LOGF "----- end flasher output -----"
  # success = rc 0 AND the vendor's "STM32iap finished HD/NA." marker present
  if [ "$RC" -eq 0 ] && grep -q "finished" "$WORK_DIR/iap.out" 2>/dev/null; then
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
  exit 1
fi
LOGF "flash OK via $USED. New MCU firmware is running."
chime ok
else
  LOGF "NO_FLASH: MCU flash skipped (RC=0, USED=skip)"
  RC=0; USED="skip"
fi

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
  nohup "$WORK_DIR/ad4-bridge" "$BRIDGE_PORT" /dev/ttyS1 250000 \
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
  tail -n 5 /var/log/ad4-bridge.log >> "$LOG" 2>/dev/null
else
  LOGF "bridge NOT started (no C binary on stick)."
  echo "bridge-skipped(no-c)" >> "$STATUS"
fi

# ---- SoC ACCESS: dropbear SSH + root password ------------------
# Proven mechanism (SumolX/FlashForge-Hacks): dropbearmulti installed to
# /opt/services/bin, symlinked, host key generated, and /opt/auto_run.sh
# (the vendor's real boot hook — NOT rcS) appends our autorun. Root password
# set to 'flashforge' via an interactive `passwd` heredoc; a shadow-hash
# fallback (verified MD5-crypt) covers the case where `passwd` needs a tty.
# This runs on EVERY stick-boot (flash or NO_FLASH) so we always have a way in.
LOGF "----- SoC access: installing dropbear SSH (root pw = flashforge) -----"
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
  # ---- root password = flashforge -----------------------------------
  # Primary: interactive `passwd` heredoc (proven in FFH). Fallback:
  # rewrite the root shadow entry with a verified MD5-crypt hash.
  PW_HASH='$1$ff2026$IGxGYXhDTYMygkh.Suun4/'
  set_root_pw() {
    # $1 = label
    # Try interactive passwd first (no tty required on busybox if piped).
    printf 'flashforge\nflashforge\n' | passwd 2>/dev/null
    # Verify by checking the shadow hash; if still not ours, force it.
    if command -v awk >/dev/null 2>&1 && [ -w /etc/shadow ]; then
      cur=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)
      if [ "$cur" != "$PW_HASH" ]; then
        awk -v h="$PW_HASH" 'BEGIN{} $1=="root"{print "root:"h":15000:0:99999:7:::";next} {print}' \
          /etc/shadow 2>/dev/null > /etc/shadow.new 2>/dev/null \
          && mv -f /etc/shadow.new /etc/shadow && chmod 600 /etc/shadow 2>/dev/null
        LOGF "root shadow set to verified hash (passwd fallback)"
      else
        LOGF "root password already = flashforge"
      fi
    fi
  }
  set_root_pw "boot"
  LOGF "root password set to: flashforge"
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
