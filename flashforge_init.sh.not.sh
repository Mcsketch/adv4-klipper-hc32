#!/bin/sh

cd "$(dirname "$0")" || exit 1

LOG=flashforge-emmc-backup.log
exec >"$LOG" 2>&1

echo "FlashForge eMMC backup"
date 2>/dev/null || true
uname -a 2>/dev/null || true
ARCH=$(uname -m 2>/dev/null || echo unknown)
echo "ARCH=$ARCH"
echo "PWD=$(pwd)"

FB=./fbfill_arm
case "$ARCH" in
  mips|mipsel|mips*) FB=./fbfill_mips ;;
  arm*|aarch64) FB=./fbfill_arm ;;
esac

paint_fb() {
  COLOR=$1
  if [ -x "$FB" ]; then
    "$FB" --color "$COLOR" \
      --width "${FF_BACKUP_FB_WIDTH:-0}" \
      --height "${FF_BACKUP_FB_HEIGHT:-0}" \
      --bpp "${FF_BACKUP_FB_BPP:-0}" \
      ${FF_BACKUP_FB_ORDER:+--order "$FF_BACKUP_FB_ORDER"} || true
    return
  fi

  case "$COLOR" in
    black) cat /dev/zero >/dev/fb0 2>/dev/null || true ;;
    *) : ;;
  esac
}

fail() {
  echo "ERROR: $1"
  [ -n "${OUT_DIR:-}" ] && echo "ERROR: $1" >>"$OUT_DIR/backup.log" 2>/dev/null || true
  paint_fb red
  sync
  sleep 3600
  exit 1
}

select_source_device() {
  if [ -n "${FF_BACKUP_SOURCE:-}" ]; then
    SOURCE_DEV=$FF_BACKUP_SOURCE
  elif [ -b /dev/mmcblk0 ]; then
    SOURCE_DEV=/dev/mmcblk0
  elif [ -b /dev/mmcblk1 ]; then
    SOURCE_DEV=/dev/mmcblk1
  else
    return 1
  fi

  SOURCE_BASE=$(basename "$SOURCE_DEV")
  SOURCE_SIZE_FILE=/sys/block/$SOURCE_BASE/size
  [ -r "$SOURCE_DEV" ] || return 1
  [ -r "$SOURCE_SIZE_FILE" ] || return 1
  return 0
}

part_path() {
  printf '%s/%s.img.part%03d' "$OUT_DIR" "$SOURCE_BASE" "$1"
}

find_existing_backup_dir() {
  latest=""
  for d in /mnt/emmc_backup/flashforge-emmc-backup-*; do
    [ -d "$d" ] || continue
    if [ -z "$latest" ] || [ "$d" \> "$latest" ]; then
      latest="$d"
    fi
  done

  [ -n "$latest" ] || return 1
  OUT_DIR=$latest
  return 0
}

resume_existing_backup() {
  part=0
  copied=0
  part_sectors=0

  while :; do
    out=$(part_path "$part")
    [ -f "$out" ] || break

    size=$(wc -c < "$out" 2>/dev/null | tr -d ' ')
    case "$size" in
      ''|*[!0-9]*) fail "could not determine size for $out" ;;
    esac

    sectors=$((size / 512))
    remainder=$((size % 512))

    if [ "$remainder" -ne 0 ]; then
      echo "removing partial sector tail from $out size=$size"
      sectors=$((sectors - 1))
      [ "$sectors" -lt 0 ] && sectors=0
      dd if=/dev/null of="$out" bs=512 seek="$sectors" 2>/dev/null || fail "could not truncate partial part $out"
    fi

    if [ "$sectors" -lt "$PART_LIMIT_SECTORS" ]; then
      copied=$((copied + sectors))
      part_sectors=$sectors
      echo "resuming in part=$part part_sectors=$part_sectors copied=$copied"
      return 0
    fi

    copied=$((copied + sectors))
    part=$((part + 1))
    part_sectors=0
  done

  echo "resuming at new part=$part copied=$copied"
  return 0
}

pipe_sum() {
  md5sum | awk '{print $1}'
}

verify_backup() {
  echo "starting verify stage" | tee -a "$OUT_DIR/verify.log"

  if ! command -v md5sum >/dev/null 2>&1; then
    fail "md5sum is required for verify stage"
  fi

  verified=0
  verify_part=0
  verify_last_percent=-1

  while [ "$verified" -lt "$TOTAL_SECTORS" ]; do
    part_file=$(part_path "$verify_part")
    [ -f "$part_file" ] || fail "missing verify part $part_file"

    size=$(wc -c < "$part_file" 2>/dev/null | tr -d ' ')
    case "$size" in
      ''|*[!0-9]*) fail "could not determine verify size for $part_file" ;;
    esac

    if [ $((size % 512)) -ne 0 ]; then
      fail "verify part is not sector-aligned: $part_file size=$size"
    fi

    part_total=$((size / 512))
    part_offset=0

    while [ "$part_offset" -lt "$part_total" ] && [ "$verified" -lt "$TOTAL_SECTORS" ]; do
      remaining_part=$((part_total - part_offset))
      remaining_total=$((TOTAL_SECTORS - verified))
      count=$CHUNK_SECTORS
      [ "$remaining_part" -lt "$count" ] && count=$remaining_part
      [ "$remaining_total" -lt "$count" ] && count=$remaining_total

      echo "verify skip=$verified count=$count part=$(basename "$part_file") part_skip=$part_offset" | tee -a "$OUT_DIR/verify.log"
      src_sum=$(dd if="$SOURCE_DEV" bs=512 skip="$verified" count="$count" 2>/dev/null | pipe_sum)
      img_sum=$(dd if="$part_file" bs=512 skip="$part_offset" count="$count" 2>/dev/null | pipe_sum)

      if [ "$src_sum" != "$img_sum" ]; then
        fail "verify mismatch at source_sector=$verified part=$(basename "$part_file") part_sector=$part_offset"
      fi

      verified=$((verified + count))
      part_offset=$((part_offset + count))
      verify_percent=$((verified * 100 / TOTAL_SECTORS))

      if [ "$verify_percent" != "$verify_last_percent" ]; then
        if [ $((verify_percent % 2)) -eq 0 ]; then
          paint_fb blue
        else
          paint_fb white
        fi
        verify_last_percent=$verify_percent
      fi

      echo "verify_progress=${verify_percent}% verified_sectors=$verified total_sectors=$TOTAL_SECTORS" | tee -a "$OUT_DIR/verify.log"
    done

    verify_part=$((verify_part + 1))
  done

  [ "$verified" -eq "$TOTAL_SECTORS" ] || fail "verify ended early: $verified of $TOTAL_SECTORS sectors"
  echo "verify complete" | tee -a "$OUT_DIR/verify.log"
}

capture_metadata() {
  {
    echo "run_id=$RUN_ID"
    echo "arch=$ARCH"
    echo "backup_dev=$BACKUP_DEV"
    echo "source=$SOURCE_DEV"
    echo "source_base=$SOURCE_BASE"
    echo "total_sectors=$TOTAL_SECTORS"
    echo "sector_size=512"
    echo "chunk_sectors=$CHUNK_SECTORS"
    echo "part_limit_sectors=$PART_LIMIT_SECTORS"
    echo "uname=$(uname -a 2>/dev/null)"
    [ -r /root/version ] && echo "root_version=$(cat /root/version 2>/dev/null)"
  } >"$OUT_DIR/manifest.txt.tmp"
  cat "$OUT_DIR/manifest.txt.tmp" >>"$OUT_DIR/manifest.txt"
  rm -f "$OUT_DIR/manifest.txt.tmp"

  cat /proc/cmdline >"$OUT_DIR/cmdline.txt" 2>/dev/null || true
  cat /proc/mounts >"$OUT_DIR/mounts.txt" 2>/dev/null || true
  cat /proc/partitions >"$OUT_DIR/partitions.txt" 2>/dev/null || true
  cat /proc/mtd >"$OUT_DIR/mtd.txt" 2>/dev/null || true
  dmesg >"$OUT_DIR/dmesg-start.txt" 2>/dev/null || true
  for s in /sys/block/*/size; do
    [ -r "$s" ] && echo "$s $(cat "$s")"
  done >"$OUT_DIR/block-sizes.txt" 2>/dev/null || true
}

mount_backup_partition() {
  mkdir -p /mnt/emmc_backup

  if [ -n "${FF_BACKUP_DEST_DEV:-}" ]; then
    echo "trying configured backup mount: $FF_BACKUP_DEST_DEV"
    mount -t ext4 -o rw,noatime "$FF_BACKUP_DEST_DEV" /mnt/emmc_backup && {
      echo "mounted backup partition: $FF_BACKUP_DEST_DEV"
      BACKUP_DEV=$FF_BACKUP_DEST_DEV
      return 0
    }
  fi

  for dev in /dev/sda2 /dev/sdb2 /dev/sdc2 /dev/sdd2; do
    [ -e "$dev" ] || continue
    echo "trying backup mount: $dev"
    mount -t ext4 -o rw,noatime "$dev" /mnt/emmc_backup && {
      echo "mounted backup partition: $dev"
      BACKUP_DEV=$dev
      return 0
    }
    dmesg | tail -n 40 2>/dev/null || true
  done

  return 1
}

select_source_device || fail "could not detect readable source block device"

if [ ! -x "$FB" ]; then
  echo "WARNING: framebuffer helper missing: $FB"
fi

mount_backup_partition || fail "could not mount ext4 backup partition"

TOTAL_SECTORS=$(cat "$SOURCE_SIZE_FILE")
case "$TOTAL_SECTORS" in
  ''|*[!0-9]*) fail "invalid source sector count: $TOTAL_SECTORS" ;;
esac

CHUNK_SECTORS=${FF_BACKUP_CHUNK_SECTORS:-32768} # 16 MiB at 512 B/sector
PART_LIMIT_SECTORS=${FF_BACKUP_PART_LIMIT_SECTORS:-2097152} # 1 GiB parts
if find_existing_backup_dir; then
  echo "found existing backup directory: $OUT_DIR"
  RUN_ID=$(basename "$OUT_DIR" | sed 's/^flashforge-emmc-backup-//')
else
  RUN_ID=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo unknown-time)
  OUT_DIR=/mnt/emmc_backup/flashforge-emmc-backup-$RUN_ID
  mkdir -p "$OUT_DIR" || fail "could not create backup dir"
fi

echo "backup log started" >>"$OUT_DIR/backup.log"
capture_metadata

resume_existing_backup
last_digit=-1

if [ "$copied" -gt "$TOTAL_SECTORS" ]; then
  fail "existing backup is larger than source device"
fi

if [ "$copied" -eq "$TOTAL_SECTORS" ]; then
  echo "backup already complete" | tee -a "$OUT_DIR/backup.log"
  sync
  verify_backup
  sync
  dmesg >"$OUT_DIR/dmesg-end.txt" 2>/dev/null || true
  paint_fb green
  sleep 3600
  exit 0
fi

while [ "$copied" -lt "$TOTAL_SECTORS" ]; do
  remaining=$((TOTAL_SECTORS - copied))
  count=$CHUNK_SECTORS
  [ "$remaining" -lt "$count" ] && count=$remaining

  if [ $((part_sectors + count)) -gt "$PART_LIMIT_SECTORS" ]; then
    part=$((part + 1))
    part_sectors=0
  fi

  out=$(part_path "$part")
  part_name=$(basename "$out")

  echo "copy skip=$copied count=$count out=$part_name seek=$part_sectors" | tee -a "$OUT_DIR/backup.log"
  dd if="$SOURCE_DEV" of="$out" bs=512 skip="$copied" count="$count" seek="$part_sectors" conv=notrunc || fail "dd failed at sector $copied"

  copied=$((copied + count))
  part_sectors=$((part_sectors + count))
  percent=$((copied * 100 / TOTAL_SECTORS))
  digit=$((percent % 10))

  if [ "$digit" != "$last_digit" ]; then
    if [ $((digit % 2)) -eq 0 ]; then
      paint_fb black
    else
      paint_fb white
    fi
    last_digit=$digit
  fi

  echo "progress=${percent}% copied_sectors=$copied total_sectors=$TOTAL_SECTORS" | tee -a "$OUT_DIR/backup.log"
  sync
done

find "$OUT_DIR" -type f -name "$SOURCE_BASE.img.part*" -exec ls -l {} \; >>"$OUT_DIR/manifest.txt" 2>/dev/null || true
sync
verify_backup
sync
dmesg >"$OUT_DIR/dmesg-end.txt" 2>/dev/null || true
paint_fb green
echo "backup complete: $OUT_DIR" | tee -a "$OUT_DIR/backup.log"
sleep 3600
exit 0
