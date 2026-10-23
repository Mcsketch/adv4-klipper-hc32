#!/bin/sh
# =============================================================
# build-stick.sh — assemble the AD4 USB stick from THIS folder.
#
#   ./build-stick.sh /Volumes/MYSTICK
#   AD4_ROOT_PASS='mypw' ./build-stick.sh /Volumes/MYSTICK   # custom root pw
#   NO_PERSISTENT=1 ./build-stick.sh /Volumes/MYSTICK        # no eMMC changes
#   NO_FLASH=1      ./build-stick.sh /Volumes/MYSTICK        # don't touch MCU
#   RESTORE=1       ./build-stick.sh /Volumes/MYSTICK        # reflash factory MCU
#
# Stick must already be FAT32 and mounted. Everything it needs is already
# in this directory, so the folder is the single source of truth.
# =============================================================
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
STICK="${1:-}"
[ -n "$STICK" ] || { echo "usage: $0 <mounted-stick-path>"; exit 1; }
[ -d "$STICK" ] || { echo "ERROR: $STICK not a directory (mount the stick first)"; exit 1; }

# --- sanity checks on the payloads -----------------------------
# Our own flasher replaces the vendor IAPCommand: it is a transcription of
# that binary's exact HC32 protocol, so there is no vendor blob to pin an
# md5 against any more. It lives with its source one directory up; rebuild
# it from there (see the header of stm32isp-ad4.c) before building the stick.
ISP_ARM="$HERE/../stm32isp-ad4/stm32isp-ad4_arm"
[ -f "$ISP_ARM" ] || { echo "ERROR: $ISP_ARM missing - rebuild it with zig"; exit 1; }
case "$(file -b "$ISP_ARM")" in
  *ARM*) echo "stm32isp-ad4_arm OK (ARM, $(wc -c < "$ISP_ARM" | tr -d ' ') bytes, sha256 $(shasum -a 256 "$ISP_ARM" | cut -d' ' -f1))" ;;
  *) echo "ERROR: $ISP_ARM is not an ARM binary"; exit 1 ;;
esac

for b in ad4-bridge ad4-screen ad4-buzz dropbear; do
  [ -f "$HERE/$b" ] || { echo "ERROR: $HERE/$b missing"; exit 1; }
  case "$(file -b "$HERE/$b")" in
    *ARM*) echo "$b OK (ARM)" ;;
    *) echo "ERROR: $b is not an ARM binary"; exit 1 ;;
  esac
done

# klipper.hex must be positioned above the bootloader (min abs addr > 0x8000).
# Same address math as the on-SoC guard in flashforge_init.sh: type-04/05 set
# the high 16 bits, type-02 sets a segment (value << 4), type-00 records add
# their own 16-bit offset. objcopy emits type-02 for images that straddle a
# 64K page; missing that record reads the second page as base 0x00000000.
MINABS=$(awk '
  function h2d(h,  i, v) { v=0; h=tolower(h)
    for (i=1;i<=length(h);i++) v = v*16 + index("0123456789abcdef", substr(h,i,1)) - 1
    return v }
  { if (substr($0,1,1) != ":") next
    if (length($0) < 11) next
    t = h2d(substr($0,8,2))
    if (t == 4 || t == 5) { hi = h2d(substr($0,10,4)) * 65536; next }
    if (t == 2) { hi = h2d(substr($0,10,4)) * 16; next }
    if (t != 0) next
    a = hi + h2d(substr($0,4,4))
    if (!seen || a < min) { min = a; seen = 1 } }
  END { if (!seen) print "-1"; else printf "%d\n", min }' "$HERE/klipper.hex")
[ "${MINABS:-0}" -gt 32768 ] || {
  echo "ERROR: klipper.hex starts at 0x00000000-ish (min=$MINABS) - that would";
  echo "       overwrite the FLASH-resident bootloader. Rebuild it positioned:";
  echo "       tools/make-positioned-ihex.py out/klipper.bin 0xC000"; exit 1; }
printf "klipper.hex min abs addr 0x%08X, sha256 %s\n" "$MINABS" \
       "$(shasum -a 256 "$HERE/klipper.hex" | cut -d' ' -f1)"

# --- copy ------------------------------------------------------
echo "==> building stick at $STICK"
for f in flashforge_init.sh ad4-daemon.sh ad4-install-persistent.sh \
         ad4-uninstall-persistent.sh ad4-revcheck.sh ad4-setrootpw.sh \
         ad4-serial-bridge.py ad4-bridge ad4-screen ad4-buzz ad4-net.conf dropbear \
         klipper.hex factory-HD.hex; do
  cp -f "$HERE/$f" "$STICK/$f"
done
cp -f "$ISP_ARM" "$STICK/stm32isp-ad4"
: > "$STICK/screwflag"

# Root password: only written when overridden, so the value never lands in git.
if [ -n "${AD4_ROOT_PASS:-}" ]; then
  printf 'ROOT_PASS=%s\n' "$AD4_ROOT_PASS" > "$STICK/ad4-access.conf"
  echo "ad4-access.conf written (ROOT_PASS overridden)"
else
  rm -f "$STICK/ad4-access.conf"
  echo "using default root password (flashforge)"
fi

# --- mode flags ------------------------------------------------
if [ "${NO_PERSISTENT:-}" = "1" ]; then rm -f "$STICK/PERSISTENT"; echo "one-shot stick (no eMMC changes)"
else : > "$STICK/PERSISTENT"; echo "persistent stick (eMMC boot hook, survives power)"; fi
if [ "${NO_FLASH:-}" = "1" ]; then : > "$STICK/NO_FLASH"; echo "NO_FLASH (will NOT touch the MCU)"
else rm -f "$STICK/NO_FLASH"; fi
if [ "${RESTORE:-}" = "1" ]; then : > "$STICK/RESTORE"; echo "RESTORE (reflashes factory-HD.hex, unwinds the hook)"
else rm -f "$STICK/RESTORE"; fi
chmod +x "$STICK/flashforge_init.sh" 2>/dev/null || true

# --- strip macOS metadata (the SoC must never see ._*) ---------
find "$STICK" -name '._*' -delete 2>/dev/null || true
rm -f "$STICK/.DS_Store" 2>/dev/null || true
rm -rf "$STICK/.Spotlight-V100" "$STICK/.fseventsd" 2>/dev/null || true

echo
echo "==> stick contents:"
ls -la "$STICK"
echo
echo "DONE. Plug the stick in, power the printer, then from the Mac:"
echo "  ssh root@192.168.10.10"
echo "Pull the stick and read ad4-flash.log off it if anything looks off."
