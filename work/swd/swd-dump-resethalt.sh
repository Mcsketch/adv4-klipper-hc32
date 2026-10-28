#!/usr/bin/env bash
# Connect-under-reset dump for an app that sleeps in WFI (Klipper): with DBGMCU DBG_SLEEP clear the
# N32/STM32F1 stops HCLK in sleep and the plain examine fails. J16 has no NRST, so the SoC holds reset:
#   SoC: echo 0 > /sys/class/gpio/gpio34/value   BEFORE starting this script (gpio35 must read 0)
#   SoC: echo 1 > /sys/class/gpio/gpio34/value   when "RELEASE_RESET_NOW" is printed
# DEMCR.VC_CORERESET halts the core at the reset vector before any app code runs; flash is dumped; the
# vector catch is cleared; resume -> app boots. READ-ONLY for flash/option bytes. Only debug registers
# are written: DEMCR (0xE000EDFC) and DBGMCU_CR (0xE0042004: clocks on in sleep, watchdogs frozen while
# halted), same as OpenOCD stm32f1x.cfg does. Usage: ./swd-dump-resethalt.sh [wait_ms=30000]
set -e; cd "$(dirname "$0")"
w=${1:-30000}
ts=$(date +%Y%m%d-%H%M); out=n32-flash-full-$ts.bin; opt=n32-optbytes-$ts.bin
openocd -f ad4-swd.cfg -c init -c targets \
  -c "catch {echo [format {DEMCR before: 0x%08x} [mrw 0xE000EDFC]]}" \
  -c "catch {echo [format {DBGMCU_CR before: 0x%08x} [mrw 0xE0042004]]}" \
  -c "mww 0xE000EDFC 0x01000001" \
  -c "catch {mww 0xE0042004 0x00000307}" \
  -c "echo RELEASE_RESET_NOW" \
  -c "wait_halt $w" -c targets -c "reg pc" -c "reg sp" \
  -c "dump_image $out 0x08000000 0x80000" -c "catch {dump_image $opt 0x1FFFF800 0x10}" \
  -c "mww 0xE000EDFC 0x01000000" \
  -c resume -c shutdown
ls -l "$out" "$opt" 2>/dev/null || true
sha256sum "$out" | tee -a dumps.sha256
echo "--- option bytes (first byte = RDP; 0xa5 = unprotected):"; [ -f "$opt" ] && xxd "$opt"
echo "--- IAP bootloader vectors @0x08000000:"; xxd -l 16 "$out"
echo "--- app vectors @0x08010000:"; xxd -s 0x10000 -l 16 "$out"
kl=../klipper-n32g455-020d25e.bin
if [ -f "$kl" ]; then
  n=$(stat -c %s "$kl")
  echo "--- compare 0x08010000 + $n B against $kl:"
  if cmp <(dd if="$out" bs=1 skip=65536 count=$n 2>/dev/null) "$kl"; then echo "KLIPPER_IMAGE_MATCH"; else echo "KLIPPER_IMAGE_MISMATCH"; fi
  echo "--- non-FF bytes after the image: $(dd if="$out" bs=1 skip=$((65536+n)) 2>/dev/null | tr -d "\377" | wc -c)"
fi
echo "--- bootloader 0..64K vs 2040 dump:"; cmp -n 65536 "$out" n32-flash-full-20261027-2040.bin && echo BOOTLOADER_IDENTICAL
echo "--- byte histogram: 0xff=$(od -An -v -tu1 "$out" | tr -s " " "\n" | grep -c "^255$") 0x00=$(od -An -v -tu1 "$out" | tr -s " " "\n" | grep -c "^0$") / 524288"
