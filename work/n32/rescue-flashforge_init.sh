#!/bin/sh
# AD4 RESCUE STICK v2 (2026-10-27). Runs from /opt/auto_run.sh's USB hook on a
# cold boot. Brings the network up itself (eth0 udhcpc; wlan0 wpa_supplicant +
# udhcpc) and starts dropbear SSH, then exits 0 so auto_run.sh parks in its
# `sleep 100000`: no vendor UI, no daemon, nothing else touches /dev/ttyS1.
# Leaves /opt/ad4-klipper intact but disabled. No flashing, no IAP, no NRST.
LOG=/tmp/rescue.log
exec >>$LOG 2>&1
echo "=== rescue start `date` uptime `cat /proc/uptime`"
rm -f /opt/ad4-klipper/enable
echo "rescue stick `date`" > /opt/ad4-klipper/net-fallback
mkdir -p /var/run/wpa_supplicant
/bin/dropbear -R -p 22; echo "dropbear rc=$?"
ifconfig eth0 up
udhcpc -i eth0 -b -p /var/run/udhcpc.eth0.pid; echo "eth0 udhcpc rc=$?"
i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 90 ]; do sleep 1; i=$((i+1)); done
echo "wlan0 after ${i}s; ifaces: `ls /sys/class/net | tr '\n' ' '`"
ifconfig wlan0 up; echo "wlan0 up rc=$?"
wpa_supplicant -v 2>&1 | head -2
for drv in nl80211 wext; do
  wpa_supplicant -D$drv -iwlan0 -c/etc/wpa_supplicant.conf -B
  echo "wpa_supplicant -D$drv rc=$?"
  sleep 2
  if wpa_cli -i wlan0 ping 2>/dev/null | grep -q PONG; then echo "wpa_supplicant alive via $drv"; break; fi
done
wpa_cli -i wlan0 enable_network all
i=0
while [ $i -lt 45 ]; do
  wpa_cli -i wlan0 status 2>/dev/null | grep -q "wpa_state=COMPLETED" && break
  sleep 1; i=$((i+1))
done
echo "--- wpa status after ${i}s"; wpa_cli -i wlan0 status
udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid; echo "wlan0 udhcpc rc=$?"
i=0
while [ $i -lt 30 ]; do ifconfig wlan0 | grep -q "inet addr" && break; sleep 1; i=$((i+1)); done
echo "--- ifconfig after ${i}s"; ifconfig -a
echo "--- route"; route -n
echo "--- ps"; ps
echo "--- dmesg tail"; dmesg | tail -60
echo "=== rescue done `date`"
cp $LOG /mnt/rescue.log
sync
exit 0
