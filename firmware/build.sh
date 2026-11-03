#!/bin/sh
# Rebuilds firmware/klipper-n32g455.hex from mainline Klipper.
#   firmware/build.sh [KLIPPER_DIR]     (default: firmware/klipper, cloned)
# Needs: git, make, arm-none-eabi-gcc, python3.
# The shipped hex was built at Klipper 020d25e with arm-none-eabi-gcc 13.2.1
# (Ubuntu 24.04 gcc-arm-none-eabi). A fresh run with that toolchain reproduces
# it byte for byte (md5 below); another toolchain gives a different but
# equivalent image. The clone is shallow on purpose: Klipper embeds
# `git describe`, which is then "020d25e", as in the shipped build.
set -e
cd "$(dirname "$0")"
REV=020d25e7fed80393dc4f1ae1720bdab9d8c0a038
KDIR=${1:-$(pwd)/klipper}
if [ ! -d "$KDIR/.git" ]; then
    git init -q "$KDIR"
    git -C "$KDIR" fetch -q --depth 1 https://github.com/Klipper3d/klipper "$REV"
fi
git -C "$KDIR" checkout -q "$REV"
cp klipper-n32g455.defconfig "$KDIR/.config"
make -C "$KDIR" olddefconfig >/dev/null
make -C "$KDIR" -j"$(nproc)"
# IAPCommand takes Intel HEX. objcopy of the ELF writes the type-04 and
# type-05 records it needs (app base 0x08010000, above the bootloader).
arm-none-eabi-objcopy -O ihex "$KDIR/out/klipper.elf" klipper-n32g455.hex
head -1 klipper-n32g455.hex | grep -q '^:020000040801F1' || { echo "hex does not start at 0x0801xxxx" >&2; exit 1; }
md5sum klipper-n32g455.hex
echo "shipped: 78120aec0409618bed22daef7bb98550"
