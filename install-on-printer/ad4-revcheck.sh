#!/bin/sh
# ad4-revcheck.sh — DISABLED 2026-10-27. This was NOT read-only: the vendor IAP
# erases the application pages the moment it is synced (SWD-proven, NA unit).
echo "ad4-revcheck.sh is disabled: syncing the IAP erases the MCU app. Read the chip over SWD (work/swd/swd-probe.sh)." >&2
exit 2
#
# Sequence copied from the vendor IAPCommand (main 0x10a0c, stm32Init 0x11654,
# stm32Sync 0x1152c, gpio_out 0x11800):
#   gpio35 (BOOT0) = 1            -> MCU enters its ROM bootloader
#   gpio34 (NRST)  = 0, 200 ms, 1 -> reset into that bootloader
#   write 'C' (0x43) to the MCU UART, read 1 byte
#       0x07 -> HD  (HC32F460, Hollystone)
#       0x06 -> NA  (Nation N32)
#       none -> no answer (dead MCU, wrong port/baud, or non-stock firmware)
# Every exit path of the vendor main() calls stm32UpdateFLag(0), i.e. BOOT0
# back to 0, so the MCU is always left booting its own firmware. We do the same.
#
# usage: ad4-revcheck.sh [-k] [port] [baud]     (default /dev/ttyS1 115200)
#        -k  kill whatever holds the port first (on a stock printer that is
#            the vendor UI app; it only comes back on reboot)

PORT=/dev/ttyS1
BAUD=115200
KILL=0
for a in "$@"; do
    [ "x$a" = "x-k" ] && KILL=1 && continue
    case "$a" in
        /dev/*) PORT="$a" ;;
        *) BAUD="$a" ;;
    esac
done
NRST=34
BOOT0=35

echo "port=$PORT baud=$BAUD"

echo "-- vendor flash markers (only prove what the LAST flash was) --"
for m in /root/N32 /root/H32; do
    [ -e "$m" ] && echo "   $m exists" || echo "   $m absent"
done

echo "-- port holders --"
HELD=
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    for f in $p/fd/*; do
        [ "x$(readlink $f 2>/dev/null)" = "x$PORT" ] && HELD="$HELD $pid($(cat $p/comm 2>/dev/null))"
    done
done
if [ -n "$HELD" ]; then
    echo "   $PORT is open by:$HELD"
    if [ "$KILL" != 1 ]; then
        echo "   refusing to share the port. Re-run with -k to kill them."
        exit 1
    fi
    for h in $HELD; do kill -9 "${h%%(*}" 2>/dev/null; done
    echo "   killed."
    sleep 1
fi

gpiodir() {
    [ -d "/sys/class/gpio/gpio$1" ] || echo "$1" > /sys/class/gpio/export 2>/dev/null
    [ -w "/sys/class/gpio/gpio$1/direction" ] || { echo "gpio$1 unavailable"; exit 1; }
    echo out > "/sys/class/gpio/gpio$1/direction"
}
gpioset() { echo "$2" > "/sys/class/gpio/gpio$1/value"; }

restore() {
    gpioset $BOOT0 0
    gpioset $NRST 0; sleep 0.2; gpioset $NRST 1
    exec 3>&- 3<&-
}
gpiodir $BOOT0; gpiodir $NRST
trap 'restore' EXIT INT TERM

stty -F "$PORT" $BAUD raw -echo -crtscts -hupcl min 0 time 20 || exit 1
exec 3<>"$PORT"

# BOOT0 high, then reset: the MCU comes up in its ROM bootloader.
gpioset $BOOT0 1
sleep 1
gpioset $NRST 0; sleep 0.2; gpioset $NRST 1
sleep 0.2

# Drop whatever the MCU streamed on boot, then ask it who it is.
dd <&3 of=/dev/null bs=256 count=8 2>/dev/null
REPLY=
n=1
while [ $n -le 3 ]; do
    printf 'C' >&3
    REPLY=$(dd bs=1 count=1 <&3 2>/dev/null | od -An -tx1 | tr -d ' \n')
    [ -n "$REPLY" ] && break
    n=$((n + 1))
done

echo "-- sync reply --"
echo "   raw byte: ${REPLY:-<none>}"
case "$REPLY" in
    07) echo "   ==> HD : HC32F460 (Hollystone)" ;;
    06) echo "   ==> NA : Nation N32" ;;
    *)  echo "   ==> unknown. No ROM-bootloader answer: check the port/baud, or"
        echo "       the MCU is not running stock firmware (a klipper-flashed"
        echo "       MCU does not answer 'C' at all)." ;;
esac
