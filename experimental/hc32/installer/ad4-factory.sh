#!/bin/sh
# =============================================================
# ad4-factory.sh - full factory restore for the Adventurer 4.
#
#   ad4-factory.sh probe  [SRC]    say what is available, write nothing
#   ad4-factory.sh mcuhex HD|NA    print the MCU image path, write nothing
#   ad4-factory.sh run    [SRC]    do the restore
#
# SRC is where the user put FlashForge's own upgrade key. Default
# $WORK_DIR/factory, which may be the four .tar.xz files directly or
# the Adventurer4-2.2.4-2.3-20230216 folder dropped in as-is.
#
# WHY THE FIRMWARE IS NOT IN THIS REPO: the packages and the MCU .hex
# files are FlashForge's copyrighted firmware, so they are not
# redistributed here. The user supplies them (that is what factory/ is
# for), and as a fallback the two MCU hexes are read back off the
# printer itself - /opt/PROGRAM/control/<ver>/ still holds both.
#
# The procedure mirrors the vendor's own flashforge_init.sh: untar into
# /opt/PROGRAM/<pkg>/temp, check md5sum.list, delete the previous
# version directory, move temp into place, run its run.sh. What each
# package actually puts back:
#   control   the MCU image (we flash it ourselves - see below)
#   kernel    uImage + script.bin on /dev/mmcblk0p1, 8188fu.ko
#   software  the vendor GUI and, importantly, the STOCK /opt/auto_run.sh
#   library   ffstartup-arm, ffstartup.cfg, the Qt fonts
#
# One deliberate difference from the vendor: the control package's
# run.sh flashes the MCU with their IAPCommand binary. We flash it with
# stm32isp-ad4 first and skip that run.sh, so the same anti-brick hex
# guard that protects the flash-resident bootloader also covers a
# factory restore.
#
# Destructive by design: the previous version directory of each package
# is deleted before the new one is moved in, and the stock auto_run.sh
# replaces whatever boot hook the klipper install put there.
# =============================================================

WORK_DIR=$(cd "$(dirname "$0")" && pwd)
LOGF=${AD4_LOG:-/tmp/ad4-factory.log}
say() { printf '[factory] %s\n' "$1"; printf '%s\n' "$1" >> "$LOGF" 2>/dev/null; }
# Same, but on stderr: mcu_hex/ctl_unpack run inside $( ), where stdout is the
# return value, so a log line there would come back glued to the hex path.
note() { printf '[factory] %s\n' "$1" >&2; printf '%s\n' "$1" >> "$LOGF" 2>/dev/null; }

MODE="$1"; shift 2>/dev/null
SRC=""
NO_MCU=0; NO_SOC=0
while [ -n "$1" ]; do
  case "$1" in
    --src)    SRC="$2"; shift 2 ;;
    --no-mcu) NO_MCU=1; shift ;;
    --no-soc) NO_SOC=1; shift ;;
    *)        SRC="$1"; shift ;;
  esac
done

# ---- locate the upgrade key ----------------------------------
# Accept factory/, factory/Adventurer4-*/, or a path given on the
# command line, and accept the tarballs sitting at either level.
find_src() {
  cand="$SRC"
  [ -n "$cand" ] || cand="$WORK_DIR/factory"
  if [ ! -d "$cand" ]; then
    for d in "$WORK_DIR"/Adventurer4-* "$WORK_DIR"/factory/Adventurer4-*; do
      [ -d "$d" ] && { cand="$d"; break; }
    done
  else
    # a folder holding the official folder: descend one level
    for d in "$cand"/Adventurer4-*; do
      [ -d "$d" ] && { cand="$d"; break; }
    done
  fi
  printf '%s' "$cand"
}
SRC=$(find_src)

# The version lives in the file name and it moves between firmware drops
# (control-2.3.0.tar.xz, control-2.2.3-20230216.tgz, ...), so match the package
# prefix only and take the newest match. Never hardcode a version here.
pkg_file() {   # $1 = control|kernel|software|library
  ls -1t "$SRC"/$1*.tar.xz "$SRC"/$1*.tar.gz "$SRC"/$1*.tgz 2>/dev/null | head -n 1
}
pkg_ver() {    # $1 = package file, $2 = package name -> "2.3.0"
  v=$(basename "$1"); v=${v#$2}; v=${v%.tar.xz}; v=${v%.tar.gz}; v=${v%.tgz}
  printf '%s' "${v#-}"
}

# ---- the MCU image -------------------------------------------
# Preferred: the control package the user supplied. Fallback: the
# printer's own copy, which survives even a klipperized machine.
CTL_TMP=/tmp/ad4-factory-ctl
CTL_DONE=""
ctl_unpack() {
  # probe asks for both revisions, so unpack once
  [ "$CTL_DONE" = 1 ] && return 0
  f=$(pkg_file control)
  [ -f "$f" ] || return 1
  rm -rf "$CTL_TMP"; mkdir -p "$CTL_TMP"
  tar -xzvf "$f" -C "$CTL_TMP" >/dev/null 2>&1 || { rm -rf "$CTL_TMP"; return 1; }
  if [ -f "$CTL_TMP/md5sum.list" ]; then
    ( cd "$CTL_TMP" && md5sum -s -c md5sum.list ) \
      || { note "control package: md5 MISMATCH"; rm -rf "$CTL_TMP"; return 1; }
  fi
  CTL_DONE=1
  return 0
}

mcu_hex() {    # $1 = HD|NA ; prints a path on stdout, or nothing
  rev="$1"
  if ctl_unpack; then
    h=$(ls -1 "$CTL_TMP"/Adventurer4-"$rev"-IAP-*.hex 2>/dev/null | head -n 1)
    [ -f "$h" ] && { printf '%s' "$h"; return 0; }
  fi
  for d in /opt/PROGRAM/control/*/; do
    [ -d "$d" ] || continue
    h=$(ls -1 "$d"Adventurer4-"$rev"-IAP-*.hex 2>/dev/null | head -n 1)
    [ -f "$h" ] && { note "  no control package: using the printer's own $h"; printf '%s' "$h"; return 0; }
  done
  return 1
}

# ---- mcuhex: resolve one MCU image and nothing else -----------
# Lets a plain RESTORE work without a copyrighted .hex committed to the
# stick: the image comes from factory/ or from the printer's own copy.
if [ "$MODE" = mcuhex ]; then
  h=$(mcu_hex "${2:-HD}")
  [ -f "$h" ] && { printf '%s\n' "$h"; exit 0; }
  exit 1
fi

# ---- probe: what would a restore have to work from? ----------
if [ "$MODE" = probe ]; then
  n=0; have=""
  for p in control kernel software library; do
    f=$(pkg_file "$p")
    if [ -f "$f" ]; then
      n=$((n+1)); have="$have $p:$(pkg_ver "$f" "$p")"
    else
      have="$have $p:-"
    fi
  done
  hd=$(mcu_hex HD); na=$(mcu_hex NA)
  printf 'src=%s pkgs=%d/4%s mcu_hd=%s mcu_na=%s\n' \
    "$SRC" "$n" "$have" "${hd:+yes}" "${na:+yes}"
  exit 0
fi

[ "$MODE" = run ] || {
  echo "usage: $0 probe|mcuhex|run [SRC] [--no-mcu] [--no-soc]"; exit 2
}

say "=== full factory restore ==="
say "source: $SRC"
[ -d "$SRC" ] || say "!! no factory folder found - only the printer's own MCU hex can be used"

# ---- MCU first -------------------------------------------------
# The caller has already stopped the vendor stack, so ttyS1 is free.
if [ "$NO_MCU" = 0 ]; then
  IAP="$WORK_DIR/stm32isp-ad4"
  REV=$( "$IAP" -s 2>/dev/null | sed -n 's/.*REV=\([A-Z][A-Z]\).*/\1/p' )
  [ -n "$REV" ] || REV=HD
  say "mcu revision: $REV"
  IMG=$(mcu_hex "$REV")
  if [ ! -f "$IMG" ]; then
    say "!! no $REV factory hex available (put the control-*.tar.xz in factory/)"
    exit 1
  fi
  say "mcu image: $IMG ($(wc -c < "$IMG" | tr -d ' ') bytes)"
  # Piping the flasher straight into `say` would hide its exit status behind
  # the subshell's, so the write is verified the same way the stick does it:
  # rc 0 AND the STM32Write marker. rc 0 alone only proves contact.
  MO="$WORK_DIR/ad4-factory-mcu.out"
  "$IAP" -p /dev/ttyS1 -v "$IMG" > "$MO" 2>&1
  MRC=$?
  tail -20 "$MO" | while read -r l; do say "  $l"; done
  if [ "$MRC" != 0 ] || ! grep -q "STM32Write: success" "$MO" 2>/dev/null; then
    say "!! MCU write failed (rc=$MRC): the chip keeps its current image"
    exit 1
  fi
  say "mcu flashed"
fi

# ---- SoC packages ---------------------------------------------
do_pkg() {              # $1 = package name
  p="$1"
  f=$(pkg_file "$p")
  [ -f "$f" ] || { say "$p: no package in $SRC - skipped"; return 1; }
  v=$(pkg_ver "$f" "$p")
  rd=/opt/PROGRAM/$p
  say "$p $v: from $f"
  mkdir -p "$rd"
  rm -rf "$rd/temp"; mkdir -p "$rd/temp"
  tar -xzvf "$f" -C "$rd/temp" >/dev/null 2>&1
  if [ $? -ne 0 ]; then say "$p: untar failed"; rm -rf "$rd/temp"; return 1; fi
  sync
  if [ -f "$rd/temp/md5sum.list" ]; then
    ( cd "$rd/temp" && md5sum -s -c md5sum.list )
    if [ $? -ne 0 ]; then say "$p: md5 MISMATCH - package left alone"; rm -rf "$rd/temp"; return 1; fi
  fi
  # drop the previous version, then move the new one into place
  for old in "$rd"/*; do
    case "$old" in "$rd/temp") continue ;; esac
    [ -e "$old" ] && { say "  removing $(basename "$old")"; rm -rf "$old"; }
  done
  sync
  mv "$rd/temp" "$rd/$v" && sync
  say "$p: installed as /opt/PROGRAM/$p/$v"
  if [ "$p" = control ]; then
    say "  control run.sh skipped (the MCU was flashed above with stm32isp-ad4)"
    return 0
  fi
  if [ -f "$rd/$v/run.sh" ]; then
    sh "$rd/$v/run.sh" 2>&1 | tail -30 | while read -r l; do say "  $l"; done
    say "$p: run.sh done"
  fi
  return 0
}

if [ "$NO_SOC" = 0 ]; then
  # vendor order: control, kernel, software, library
  do_pkg control
  do_pkg kernel
  do_pkg software
  do_pkg library

  # update_other from the vendor script: keep the wifi calibration,
  # drop the ntp daemon, make sure the picture dir exists.
  if [ ! -f /opt/art.img ] && [ -e /dev/mtd2 ]; then
    say "backing up wifi calibration (mtd2 -> /opt/art.img)"
    dd if=/dev/mtd2 of=/opt/art.img 2>/dev/null; sync
  fi
  [ -h /etc/rc.d/S98sysntpd ] && rm -f /etc/rc.d/S98sysntpd
  [ -d /data/picture ] || mkdir -p /data/picture
  [ -f "$WORK_DIR/screwflag" ] && cp "$WORK_DIR/screwflag" /opt/
  for pc in "$SRC/pointercal" "$WORK_DIR/pointercal"; do
    [ -f "$pc" ] && [ -d /opt/tslib-1.4/etc ] && { cp "$pc" /opt/tslib-1.4/etc/pointercal; say "pointercal restored"; break; }
  done
  sync
fi

say "factory restore complete"
exit 0
