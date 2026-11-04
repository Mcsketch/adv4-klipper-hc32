#!/bin/sh
# Builds the two USB-stick zips in dist/:
#   ad4-klipper-install-VERSION.zip   Klipper install stick
#   ad4-stock-rollback-VERSION.zip    stock firmware rollback stick
# Unzip either onto the root of an empty FAT32 stick. Needs: python3, md5sum.
# The ARM binaries in bin/ and firmware/klipper-n32g455.hex must be current
# (make -C src; firmware/build.sh).
set -e
cd "$(dirname "$0")/.."
VERSION=${VERSION:-$(git describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)}
OUT=dist
rm -rf build/stick "$OUT"
mkdir -p "$OUT"

for f in bin/ad4-bridge bin/ad4-screen bin/ad4-buzz bin/ad4-cam bin/dropbear firmware/klipper-n32g455.hex; do
    [ -s "$f" ] || { echo "missing $f" >&2; exit 1; }
done

# install
s=build/stick/install
mkdir -p "$s/ad4-klipper"
cp stick/install/flashforge_init.sh "$s/"
cp stick/common/lib.sh stick/common/klipper-boot.sh stick/common/ad4-klipper.conf "$s/ad4-klipper/"
cp bin/ad4-bridge bin/ad4-screen bin/ad4-buzz bin/ad4-cam bin/dropbear "$s/ad4-klipper/"
cp firmware/klipper-n32g455.hex "$s/ad4-klipper/klipper.hex"
echo "$VERSION" >"$s/ad4-klipper/VERSION"
# The conf is meant to be edited, so it is not checksummed.
(cd "$s/ad4-klipper" && md5sum ad4-bridge ad4-screen ad4-buzz ad4-cam dropbear klipper-boot.sh klipper.hex lib.sh VERSION >MD5SUMS)

# rollback
r=build/stick/rollback
mkdir -p "$r/ad4-klipper"
cp stick/rollback/flashforge_init.sh "$r/"
cp stick/common/lib.sh bin/ad4-screen bin/ad4-buzz "$r/ad4-klipper/"
echo "$VERSION" >"$r/ad4-klipper/VERSION"

chmod 755 "$s/flashforge_init.sh" "$r/flashforge_init.sh" "$s"/ad4-klipper/ad4-* "$s/ad4-klipper/dropbear" "$r"/ad4-klipper/ad4-*
dist=$(pwd)/$OUT
(cd "$s" && python3 -m zipfile -c "$dist/ad4-klipper-install-$VERSION.zip" flashforge_init.sh ad4-klipper)
(cd "$r" && python3 -m zipfile -c "$dist/ad4-stock-rollback-$VERSION.zip" flashforge_init.sh ad4-klipper)
(cd "$OUT" && md5sum *.zip >MD5SUMS)
ls -l "$OUT"
