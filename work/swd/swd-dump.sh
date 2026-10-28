#!/usr/bin/env bash
# Full 512 KB flash dump of the N32G455 (0x08000000..0x0807FFFF) + option bytes, then verify.
# READ-ONLY. Usage: ./swd-dump.sh [cur]
set -e; cd "$(dirname "$0")"
extra=(); [ "$1" = cur ] && extra=(-c "reset_config srst_only srst_nogate connect_assert_srst")
ts=$(date +%Y%m%d-%H%M); out=n32-flash-full-$ts.bin; opt=n32-optbytes-$ts.bin
openocd -f ad4-swd.cfg "${extra[@]}" -c init -c halt \
  -c "dump_image $out 0x08000000 0x80000" -c "catch {dump_image $opt 0x1FFFF800 0x10}" \
  -c resume -c shutdown
ls -l "$out" "$opt" 2>/dev/null || true
sha256sum "$out" | tee -a dumps.sha256
echo "--- option bytes (first byte = RDP; 0xa5 = unprotected):"; [ -f "$opt" ] && xxd "$opt"
echo "--- IAP bootloader vectors @0x08000000 (expect SP 0x2000xxxx then odd Thumb PC in 0x0800xxxx):"; xxd -l 16 "$out"
echo "--- app vectors @0x08010000 (NA 2.3.0 hex begins 783b 0020 ad01 0108):"; xxd -s 0x10000 -l 16 "$out"
na=../Adventurer4-NA-IAP-2.3.0-20230216.bin
if [ -f "$na" ]; then
  echo "--- compare 0x08010000..0x0801AE5F against the vendor NA IAP 2.3.0 app (44640 B):"
  if cmp <(dd if="$out" bs=1 skip=65536 count=44640 2>/dev/null) "$na"; then
    echo "MATCH: app region == vendor NA 2.3.0 app"
  else
    echo "MISMATCH: a different MCU app version is possible, but look at the bytes before trusting this dump"
  fi
fi
echo "--- fill check: all-00 or all-FF means read protection / bad attach, NOT a real dump"
for v in 00 ff; do n=$(xxd -p "$out" | tr -d '\n' | fold -w2 | grep -c "^$v$" || true); echo "  bytes==0x$v: $n / 524288"; done
