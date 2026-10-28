#!/bin/sh
# =============================================================
# ad4-daemon.sh — persistent Klipper runtime on the AD4 SoC.
#
# Installed to /opt/ad4-klipper/ by ad4-install-persistent.sh and
# launched from rcS (INSTEAD of the vendor ffstartup-arm watchdog)
# when /opt/ad4-klipper/enable is present.
#
# It does, every boot:
#   1. Keep the vendor stack OFF (nothing respawns it, because our
#      hook replaces ffstartup-arm's launch — but belt & suspenders).
#   2. Set eth0 to the static IP (192.168.10.10/24 default).
#   3. Start the MCU serial<->network bridge on :5000 (self-heals:
#      if it dies, the watchdog loop restarts it).
#   4. OPTIONAL MCU self-heal: if /opt/ad4-klipper/mcu-watchdog exists,
#      periodically probe the MCU over the bridge and re-flash
#      klipper-ad4.hex if it stops answering (only if the flasher is
#      present). Off by default.
#   5. Keep WiFi up: wpa_supplicant on /etc/wpa_supplicant.conf + udhcpc on
#      wlan0. On a stock printer the vendor GUI does this; with the GUI gated
#      off, a WiFi-only printer would otherwise be unreachable.
#   NA / N32 units: the daemon NEVER flashes the MCU (stm32isp-ad4 is HD-only
#      and the vendor IAP erases the app on sync). ad4-rev decides, see below.
#
# Tunables live in /opt/ad4-klipper/ad4-net.conf (SOC_IP/SOC_NET/
# BRIDGE_PORT), same keys as the one-shot disk version.
# =============================================================

BASE="/opt/ad4-klipper"
LOG="/var/log/ad4-klipper.log"

SOC_IP="192.168.10.10"
SOC_NET="24"
BRIDGE_PORT="5000"
[ -f "$BASE/ad4-net.conf" ] && . "$BASE/ad4-net.conf"

BRIDGE_C="$BASE/ad4-bridge"                 # static C bridge (no-python, preferred)
BRIDGE_PY="$BASE/ad4-serial-bridge.py"      # python3 bridge (fallback if C missing)
HEX="$BASE/klipper.hex"
IAP="$BASE/stm32isp-ad4"
WATCHDOG="$BASE/mcu-watchdog"
WATCH_INTERVAL=60          # seconds between MCU health probes
MAX_FAIL=3                 # consecutive fails before re-flash

log() { echo "$(date '+%F %T') $*" >>"$LOG" 2>/dev/null; }

# ---- busybox-safe process helpers ----------------------------
# This SoC's busybox has NO pgrep/pkill (verified live 2026-10-21);
# using them made start_bridge think the bridge never came up and
# restart-loop every 10 s. Scan /proc/*/cmdline instead (same trick
# flashforge_init.sh uses). fuser IS present.
kill_pat() {
  # $1 = pattern; kills every process whose cmdline contains it (never us)
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) [ "${p#/proc/}" != "$$" ] && kill -9 "${p#/proc/}" 2>/dev/null ;;
    esac
  done
}
pid_alive() {
  # $1 = pattern
  for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    case `tr '\0' ' ' < "$p/cmdline" 2>/dev/null` in
      *"$1"*) return 0 ;;
    esac
  done
  return 1
}

# ---- 1) keep vendor stack off --------------------------------
# Our hook already replaced ffstartup-arm's launch, but if a previous
# boot left one running (or the hook was partially reverted), make sure
# the stock app is not holding /dev/ttyS1.
kill_vendor() {
  kill_pat ffstartup-arm
  kill_pat adventurer4-arm
  # NOT `fuser -k /dev/ttyS1`: wpa_supplicant / wpa_cli inherit the GUI's
  # ttyS1 fd (seen live 2026-10-27, NA unit) and killing them drops WiFi.
  # They never write to the port, so they are harmless holders.
}

# ---- 2) static IP (idempotent) -------------------------------
set_ip() {
  ip addr flush dev eth0 2>/dev/null
  ip addr add "$SOC_IP/$SOC_NET" dev eth0 2>/dev/null
  ip link set eth0 up 2>/dev/null
}

# ---- 2b) WiFi (idempotent) -----------------------------------
# On a stock printer the vendor GUI starts WiFi: `wpa_supplicant -Dnl80211
# -iwlan0 -c/etc/wpa_supplicant.conf -B`, `wpa_cli -i wlan0 enable_network
# all` (the saved network sits in that file with disabled=1), then udhcpc.
# Command lines taken from the live NA unit 2026-10-27. With the GUI gated
# off the daemon has to do it, or a WiFi-only printer is unreachable.
WIFI_CONF="/etc/wpa_supplicant.conf"
wifi_up() {
  [ -f "$WIFI_CONF" ] || return 0
  [ -d /sys/class/net/wlan0 ] || return 0
  if ! pid_alive wpa_supplicant; then
    ifconfig wlan0 up 2>/dev/null
    wpa_supplicant -Dnl80211 -iwlan0 -c"$WIFI_CONF" -B >>"$LOG" 2>&1
    sleep 2
    wpa_cli -i wlan0 enable_network all >/dev/null 2>&1
    log "wifi: wpa_supplicant started on wlan0 ($WIFI_CONF)"
  fi
  if ! pid_alive "udhcpc -i wlan0"; then
    # -b: keep retrying in the background, stay resident so the lease renews
    udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid >>"$LOG" 2>&1
    log "wifi: udhcpc started on wlan0"
  fi
}

# ---- 3) (re)start the MCU bridge -----------------------------
# The SoC has NO python3, so the authoritative backend is the static C
# bridge (ad4-bridge). Use it whenever present; only fall back to the
# python bridge if the C binary is missing AND python3 exists.
start_bridge() {
  kill_pat ad4-bridge
  kill_pat ad4-serial-bridge.py
  sleep 1
  if [ -x "$BRIDGE_C" ]; then
    # ad4-bridge <port> <uart> <baud>
    nohup "$BRIDGE_C" "$BRIDGE_PORT" /dev/ttyS1 115200 >>"$LOG" 2>&1 &
    sleep 1
    if pid_alive ad4-bridge >/dev/null 2>&1; then
      log "bridge up (static C) on 0.0.0.0:$BRIDGE_PORT (eth0=$SOC_IP/$SOC_NET)"
    else
      log "C bridge FAILED to start (see $LOG)"
    fi
  elif [ -f "$BRIDGE_PY" ] && command -v python3 >/dev/null 2>&1; then
    nohup python3 "$BRIDGE_PY" --tcp 0.0.0.0:"$BRIDGE_PORT" \
        --uart /dev/ttyS1 --baud 115200 >>"$LOG" 2>&1 &
    sleep 1
    if pid_alive ad4-serial-bridge.py >/dev/null 2>&1; then
      log "bridge up (python3) on 0.0.0.0:$BRIDGE_PORT (eth0=$SOC_IP/$SOC_NET)"
    else
      log "python bridge FAILED to start (see $LOG)"
    fi
  else
    log "NO bridge backend: ad4-bridge missing AND (no ad4-serial-bridge.py or no python3)"
  fi
}

bridge_alive() { pid_alive ad4-bridge >/dev/null 2>&1 || pid_alive ad4-serial-bridge.py >/dev/null 2>&1; }

# ---- 4) MCU health probe + optional re-flash -----------------
# Probes by opening the bridge TCP port and trying to read an MCU
# answer. A full Klipper handshake needs klippy, so we use a lighter
# liveness check: the bridge must be up AND the MCU must have been
# flashed (marker file). Re-flash only when the watchdog is enabled
# AND we see N consecutive dead probes AND the flasher is present.
mcu_probe() {
  # NOTE: busybox ash has NO /dev/tcp (unlike bash) — this probe always
  # fails on the SoC, so the mcu-watchdog re-flash path is effectively
  # unusable there. It stays OFF by default (mcu-watchdog absent).
  (exec 3<>/dev/tcp/127.0.0.1/"$BRIDGE_PORT") 2>/dev/null || return 1
  exec 3>&- 2>/dev/null
  [ -f "$BASE/mcu-klipper.ok" ] || return 1
  return 0
}

# ---- NA / N32 guard ------------------------------------------
# stm32isp-ad4 was written for the HD board (HC32F460) and has never written
# an N32 image; on an NA board the vendor IAP erases the application the
# moment it is synced (SWD-proven 2026-10-27). So the daemon flashes only when
# ad4-rev (copied from the stick by the installer) says HD. An NA unit is
# flashed by hand through the vendor IAPCommand (work/n32/iap-flash.sh).
mcu_flash_allowed() {
  rev=$(head -c 2 "$BASE/ad4-rev" 2>/dev/null)
  [ "$rev" = "HD" ] && return 0
  log "MCU flash blocked: revision '${rev:-unknown}' (stm32isp-ad4 may only flash HD/HC32 units)"
  return 1
}

mcu_reflash() {
  mcu_flash_allowed || return 1
  log "MCU self-heal: re-flashing $HEX"
  kill_vendor
  sleep 1
  if [ -x "$IAP" ] && [ -f "$HEX" ]; then
    "$IAP" -p /dev/ttyS1 "$HEX" >>"$LOG" 2>&1
    if [ $? -eq 0 ]; then
      touch "$BASE/mcu-klipper.ok"
      log "MCU re-flash OK"
    else
      log "MCU re-flash FAILED (rc=$?)"
    fi
  else
    log "MCU self-heal: flasher/hex missing, skipping"
  fi
  start_bridge
}

# ---- main ----------------------------------------------------
log "=== ad4-daemon start (base=$BASE ip=$SOC_IP/$SOC_NET port=$BRIDGE_PORT) ==="
kill_vendor
set_ip
wifi_up

if [ -f "$BASE/mcu-klipper.ok" ]; then
  # installed with the MCU already flashed; don't re-flash at boot
  :
elif [ -f "$BASE/flash-at-install" ] && [ -x "$IAP" ] && [ -f "$HEX" ] && mcu_flash_allowed; then
  # first boot right after install: flash once, then mark ok
  kill_vendor
  "$IAP" -p /dev/ttyS1 "$HEX" >>"$LOG" 2>&1 && touch "$BASE/mcu-klipper.ok"
  log "first-boot flash rc=$?"
fi

start_bridge

fails=0
while :; do
  # self-heal the SoC runtime every 10s
  sleep 10
  ip link show eth0 >/dev/null 2>&1 || set_ip
  ip addr show eth0 2>/dev/null | grep -q "$SOC_IP" || set_ip
  bridge_alive || start_bridge
  wifi_up

  # optional MCU watchdog
  if [ -f "$WATCHDOG" ]; then
    sleep "$WATCH_INTERVAL"
    if mcu_probe; then
      fails=0
    else
      fails=$((fails+1))
      log "MCU probe fail $fails/$MAX_FAIL"
      if [ "$fails" -ge "$MAX_FAIL" ]; then
        mcu_reflash
        fails=0
      fi
    fi
  fi
done
