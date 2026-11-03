#!/bin/sh
# Klipper-mode boot. /opt/auto_run.sh starts this in place of the vendor UI
# when /opt/ad4-klipper/klipper-mode exists:
#   ttyS1 115200 8N1 raw  ->  ad4-bridge on TCP :BRIDGE_PORT  ->  Klipper host
#   panel: ad4-screen klipper (Moonraker status + touch), respawned
#   eth0 / wlan0 / dropbear per ad4-klipper.conf
# Never touches the IAP strap or flashes anything.
# Back to the stock UI for one boot: rm /opt/ad4-klipper/klipper-mode; reboot -f
# (the stock UI cannot drive an MCU running Klipper; use the rollback stick).
D=/opt/ad4-klipper
ETH_ADDR=dhcp ETH_GW= WIFI=on MOONRAKER= BRIDGE_PORT=5000
. $D/ad4-klipper.conf
exec >>/tmp/ad4-boot.log 2>&1
echo "=== klipper boot `date` uptime `cut -d' ' -f1 /proc/uptime`"

ip4() { ifconfig "$1" 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p'; }

$D/ad4-screen splash
/bin/dropbear -R -p 22; echo "dropbear rc=$?"

stty -F /dev/ttyS1 115200 raw -echo -parenb cs8 -cstopb clocal -crtscts; echo "stty rc=$?"
setsid $D/ad4-bridge "$BRIDGE_PORT" /dev/ttyS1 115200 >/tmp/ad4-bridge.log 2>&1 </dev/null &
echo "bridge on :$BRIDGE_PORT"

case $ETH_ADDR in
off) ;;
dhcp)
    ifconfig eth0 up
    udhcpc -i eth0 -b -p /var/run/udhcpc.eth0.pid; echo "eth0 udhcpc rc=$?" ;;
*)
    ifconfig eth0 up
    ip addr add "$ETH_ADDR" dev eth0; echo "eth0 $ETH_ADDR rc=$?"
    [ -n "$ETH_GW" ] && route add default gw "$ETH_GW" ;;
esac

if [ -n "$MOONRAKER" ]; then
    setsid sh -c "while :; do $D/ad4-screen klipper $MOONRAKER; sleep 2; done" >/tmp/ad4-screen.log 2>&1 </dev/null &
else
    # No host configured: keep the printer's addresses on the panel.
    ( while :; do
        $D/ad4-screen done "Klipper bridge ready" "eth0 $(ip4 eth0)\nwifi $(ip4 wlan0)\nport $BRIDGE_PORT"
        sleep 15
      done ) >/tmp/ad4-screen.log 2>&1 </dev/null &
fi

if [ "$WIFI" = on ]; then
    mkdir -p /var/run/wpa_supplicant
    i=0
    while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 90 ]; do sleep 1; i=$((i+1)); done
    ifconfig wlan0 up
    wpa_supplicant -Dnl80211 -iwlan0 -c/etc/wpa_supplicant.conf -B; echo "wpa_supplicant rc=$?"
    sleep 2
    wpa_cli -i wlan0 enable_network all
    # udhcpc started before association never gets an offer
    i=0
    while ! wpa_cli -i wlan0 status | grep -q wpa_state=COMPLETED && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done
    echo "wifi associated after ${i}s"
    udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid; echo "wlan0 udhcpc rc=$?"
fi
sleep 5
echo "eth0 `ip4 eth0` wlan0 `ip4 wlan0`"
echo "=== klipper boot up `date`"
