#!/bin/sh
# AD4 Klipper-mode boot from eMMC (2026-10-28). /opt/auto_run.sh runs this in
# the background instead of the vendor UI when /opt/ad4-klipper/klipper-mode
# exists, so the USB stick is no longer needed. Same steps as boot stick v3:
#   eth0  static 192.168.100.20/24 (lab switch, no DHCP; Klipper host .10)
#   wlan0 wpa_supplicant + udhcpc (home LAN)
#   ttyS1 115200 8N1 raw, ad4-bridge on TCP :5000
#   panel: ad4-screen klipper (Moonraker status + touch), respawned
# auto_run.sh starts dropbear itself. Never touches GPIO35 (IAP strap) or
# flashes anything. Undo: rm /opt/ad4-klipper/klipper-mode; reboot -f.
LOG=/tmp/ad4-boot.log
D=/opt/ad4-klipper
exec >>$LOG 2>&1
echo "=== klipper boot (emmc) start `date` uptime `cat /proc/uptime`"
mkdir -p /var/run/wpa_supplicant
$D/ad4-screen splash
ifconfig eth0 up
ip addr add 192.168.100.20/24 dev eth0; echo "eth0 static rc=$?"
stty -F /dev/ttyS1 115200 raw -echo -parenb cs8 -cstopb clocal -crtscts; echo "stty rc=$?"
setsid $D/ad4-bridge 5000 /dev/ttyS1 115200 >/tmp/ad4-bridge.log 2>&1 </dev/null &
echo "bridge started"
setsid sh -c "while :; do $D/ad4-screen klipper 192.168.100.10:8095; sleep 2; done" >/tmp/ad4-screen.log 2>&1 </dev/null &
echo "screen started"
i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 90 ]; do sleep 1; i=$((i+1)); done
ifconfig wlan0 up
wpa_supplicant -Dnl80211 -iwlan0 -c/etc/wpa_supplicant.conf -B; echo "wpa_supplicant rc=$?"
sleep 2
wpa_cli -i wlan0 enable_network all
# udhcpc 0.9.8 started before association never gets an offer (seen 2026-10-28)
i=0
while ! wpa_cli -i wlan0 status | grep -q wpa_state=COMPLETED && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done
echo "wifi associated after ${i}s"
udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid; echo "wlan0 udhcpc rc=$?"
sleep 5
ifconfig eth0 | grep "inet addr"; ifconfig wlan0 | grep "inet addr"
stty -F /dev/ttyS1 | head -1
ps | grep -v "\["
echo "=== klipper boot up `date`"
