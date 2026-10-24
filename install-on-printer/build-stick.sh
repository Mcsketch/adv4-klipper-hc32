#!/bin/sh
# =============================================================
# build-stick.sh — assemble the AD4 USB stick from THIS folder.
#
#   ./build-stick.sh /Volumes/MYSTICK
#   NO_PERSISTENT=1 ./build-stick.sh /Volumes/MYSTICK        # no eMMC changes
#   NO_FLASH=1      ./build-stick.sh /Volumes/MYSTICK        # don't touch MCU
#   RESTORE=1       ./build-stick.sh /Volumes/MYSTICK        # reflash factory MCU
#   FACTORY=1       ./build-stick.sh /Volumes/MYSTICK        # full stock restore
#   DUMP=1          ./build-stick.sh /Volumes/MYSTICK        # read MCU flash out, no writes
#
# Stick must already be FAT32 and mounted. Everything it needs is already
# in this directory, so the folder is the single source of truth - except
# factory/, which holds FlashForge's own firmware and is yours to fill in
# (see factory/README.txt on the stick).
#
# With no flag file at all the stick defaults to KLIPPERIZE, and the panel
# menu lets anyone tap a different choice: klipperize / restore mcu /
# full factory / dump / skip.
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
         ad4-factory.sh ad4-dump.sh klipper.hex; do
  cp -f "$HERE/$f" "$STICK/$f"
done
cp -f "$ISP_ARM" "$STICK/stm32isp-ad4"
: > "$STICK/screwflag"

# factory-HD.hex is a convenience copy of FlashForge's MCU image, so a plain
# RESTORE works with no setup. It is NOT required: without it the same image
# is resolved from factory/ or off the printer's own /opt/PROGRAM/control.
if [ -f "$HERE/factory-HD.hex" ]; then
  cp -f "$HERE/factory-HD.hex" "$STICK/factory-HD.hex"
else
  rm -f "$STICK/factory-HD.hex"
  echo "no factory-HD.hex here: RESTORE will resolve it from factory/ or the printer"
fi

# --- factory/: the user's own copy of FlashForge's upgrade key ---
# FULL FACTORY needs FlashForge's four .tar.xz packages. They are their
# copyrighted firmware, so this repo does not carry them - the user drops
# them here. Never overwrite what is already on the stick.
mkdir -p "$STICK/factory"
if [ ! -f "$STICK/factory/README.txt" ]; then
  cat > "$STICK/factory/README.txt" <<'EOF'
FULL FACTORY RESTORE - put FlashForge's own upgrade key here.

Copy the contents of FlashForge's "Adventurer4-2.2.4-2.3-20230216" folder
into this factory/ folder, i.e. these four files:

    control-*.tar.xz     the two MCU images (HD and NA)
    kernel-*.tar.xz      uImage + script.bin on the boot partition
    software-*.tar.xz    the vendor GUI and the stock /opt/auto_run.sh
    library-*.tar.xz     ffstartup-arm, ffstartup.cfg, the Qt fonts

The version number in each name does not matter - the restore matches on the
prefix and takes whatever is there.

Dropping the whole Adventurer4-... folder in here also works; the restore
script descends into it. start.img / end.img are NOT used - they are raw
eMMC images for a recovery tool, and the restore is a package swap.

This firmware is FlashForge's, not ours: it is not in this repository and
you have to supply it yourself. Without it, FULL FACTORY still reflashes
the MCU from the copy already in the printer's /opt/PROGRAM/control, and
leaves the rest of the SoC alone.
EOF
fi

# Root password: a ROOT file here (first line = the password) is copied over
# as-is. No file, and the installer's own default of 'flashforge' applies.
# Never committed - see .gitignore.
if [ -f "$HERE/ROOT" ]; then
  cp -f "$HERE/ROOT" "$STICK/ROOT"
  echo "ROOT copied to the stick (root password overridden)"
else
  rm -f "$STICK/ROOT"
  echo "no ROOT file here: the printer keeps the default password (flashforge)"
fi

# --- mode flags ------------------------------------------------
if [ "${NO_PERSISTENT:-}" = "1" ]; then rm -f "$STICK/PERSISTENT"; echo "one-shot stick (no eMMC changes)"
else : > "$STICK/PERSISTENT"; echo "persistent stick (eMMC boot hook, survives power)"; fi
if [ "${NO_FLASH:-}" = "1" ]; then : > "$STICK/NO_FLASH"; echo "NO_FLASH (will NOT touch the MCU)"
else rm -f "$STICK/NO_FLASH"; fi
if [ "${RESTORE:-}" = "1" ]; then : > "$STICK/RESTORE"; echo "RESTORE (reflashes the stock MCU image, unwinds the hook)"
else rm -f "$STICK/RESTORE"; fi
# FACTORY wipes /opt/PROGRAM back to stock. The panel refuses to auto-run it,
# so a FACTORY stick still needs a deliberate double tap on the printer.
if [ "${FACTORY:-}" = "1" ]; then : > "$STICK/FACTORY"; echo "FACTORY (full stock restore - needs factory/ filled in)"
else rm -f "$STICK/FACTORY"; fi
if [ "${DUMP:-}" = "1" ]; then : > "$STICK/DUMP"; echo "DUMP (reads the MCU flash out through klipper, changes nothing)"
else rm -f "$STICK/DUMP"; fi
chmod +x "$STICK/flashforge_init.sh" 2>/dev/null || true

# --- strip macOS metadata (the SoC must never see ._*) ---------
find "$STICK" -name '._*' -delete 2>/dev/null || true
rm -f "$STICK/.DS_Store" 2>/dev/null || true
rm -rf "$STICK/.Spotlight-V100" "$STICK/.fseventsd" 2>/dev/null || true

echo
echo "==> factory/ contents (FULL FACTORY reads these):"
ls -la "$STICK/factory"
echo
echo "==> stick contents:"
ls -la "$STICK"
echo
echo "DONE. Plug the stick in, power the printer, then from the Mac:"
echo "  ssh root@192.168.10.10"
echo "Pull the stick and read ad4-flash.log off it if anything looks off."
