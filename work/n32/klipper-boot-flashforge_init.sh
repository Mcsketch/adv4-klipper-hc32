#!/bin/sh
# AD4 KLIPPER BOOT STICK v3 (2026-10-28). Runs from /opt/auto_run.sh's USB hook
# (~7 s after power-on) and turns the SoC into the Klipper serial bridge:
#   eth0  static 192.168.100.20/24 (lab switch, no DHCP; Klipper host .10)
#   wlan0 wpa_supplicant + udhcpc (home LAN, SSH convenience only)
#   dropbear SSH, ttyS1 115200 8N1 raw, ad4-bridge on TCP :5000.
# It never returns: auto_run.sh would otherwise `sleep 100000` and then pulse
# GPIO34 (MCU NRST) and start the vendor UI ~27.7 h later, mid-print.
# Never touches the IAP strap (GPIO35) or NRST, never flashes anything.
# Logs: /tmp/ad4-boot.log and a copy on the stick (/mnt/ad4-boot.log).
LOG=/tmp/ad4-boot.log
exec >>$LOG 2>&1
echo "=== klipper boot start `date` uptime `cat /proc/uptime`"
rm -f /opt/ad4-klipper/enable
mkdir -p /var/run/wpa_supplicant
/bin/dropbear -R -p 22; echo "dropbear rc=$?"
ifconfig eth0 up
ip addr add 192.168.100.20/24 dev eth0; echo "eth0 static rc=$?"
stty -F /dev/ttyS1 115200 raw -echo -parenb cs8 -cstopb clocal -crtscts; echo "stty rc=$?"
setsid /opt/ad4-klipper/ad4-bridge 5000 /dev/ttyS1 115200 >/tmp/ad4-bridge.log 2>&1 </dev/null &
echo "bridge started"
i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 90 ]; do sleep 1; i=$((i+1)); done
ifconfig wlan0 up
wpa_supplicant -Dnl80211 -iwlan0 -c/etc/wpa_supplicant.conf -B; echo "wpa_supplicant rc=$?"
sleep 2
wpa_cli -i wlan0 enable_network all
udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid; echo "wlan0 udhcpc rc=$?"
sleep 5
ifconfig eth0 | grep "inet addr"; ifconfig wlan0 | grep "inet addr"
stty -F /dev/ttyS1 | head -1
ps | grep -v "\["
echo "=== klipper boot up `date`"
cp $LOG /mnt/ad4-boot.log; sync
while :; do sleep 86400; done
