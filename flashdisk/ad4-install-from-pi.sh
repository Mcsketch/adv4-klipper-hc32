#!/bin/sh
# =============================================================
# ad4-install-from-pi.sh — run the WHOLE persistent install from the
# RPi, over the network, with NO USB stick in the printer.
#
# This is the "all from the PI" path. The RPi is the operator; the SoC
# is the executor (only the SoC has the MCU reset/BOOT0 GPIOs, so the
# flash itself must run ON the SoC — the PI just ssh's it).
#
# Prereq: you need a root shell on the SoC. Options, best first:
#   * an SSH key (cleanest) — see ALL-FROM-PI.md for how to get one.
#   * sshpass + password. The SoC root is a WEAK DES hash
#     (H9.zxVgLFpRG2) — it could not be cracked offline, so live-verify
#     the password on your unit (or set it: it's easy once you have any
#     root, e.g. from the disk flashforge_init.sh path).
#   * the vendor web console (admin / ff123456) is a print/cloud UI, NOT
#     a shell — it cannot run arbitrary commands, so it's not a path to
#     the flash on its own.
#
# Usage (on the RPi):
#   SOC_IP=192.168.10.10 ./ad4-install-from-pi.sh
#   SOC_IP=... SSHPASS='the-password' ./ad4-install-from-pi.sh
#   SOC_IP=... SSH_KEY=/path/key ./ad4-install-from-pi.sh
#
# Steps it performs over the network:
#   1. scp the persistent package (daemon, bridge, hex, IAPCommand,
#      net.conf, the SoC-side installer) to /opt/ad4-klipper/ on the SoC
#   2. ssh the SoC and run ad4-install-persistent.sh (gates rcS, creates
#      the enable flag) — same code path as the stick
#   3. ssh the SoC and run the flash (IAPCommand klipper.hex)
#   4. reboot the SoC; boot #2 is autonomous (vendor stack off, bridge up)
#
# NOTE: the RPi must already reach the SoC on the private subnet. If the
# SoC has not been given 192.168.10.10 yet (first time), either set it via
# the touch network menu, or run the one-shot DHCP server (ad4-dhcp-once)
# to hand it an address, then point SOC_IP at it.
# =============================================================
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SOC_IP="${SOC_IP:-192.168.10.10}"
SOC_USER="${SOC_USER:-root}"
SSHPASS="${SSHPASS:-}"
SSH_KEY="${SSH_KEY:-}"
PKG="/opt/ad4-klipper"

# ---- build the ssh/scp invoker --------------------------------
if [ -n "$SSH_KEY" ]; then
  SSH="ssh -i $SSH_KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
  SCP="scp -i $SSH_KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
elif [ -n "$SSHPASS" ]; then
  command -v sshpass >/dev/null 2>&1 || { echo "need sshpass (apt install sshpass)"; exit 1; }
  SSH="sshpass -p \"$SSHPASS\" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
  SCP="sshpass -p \"$SSHPASS\" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
else
  # fall back to an already-authenticated ssh (key in ~/.ssh or agent)
  SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
  SCP="scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
fi
D="$SOC_USER@$SOC_IP"

echo "==> ad4 remote install from PI -> $D (pkg=$PKG)"

# ---- 0) reachability check ------------------------------------
ping -c1 -W2 "$SOC_IP" >/dev/null 2>&1 || { echo "SoC $SOC_IP not reachable. Set its IP (touch menu) or run the one-shot DHCP server first."; exit 1; }
echo "SoC reachable."

# ---- 1) copy the persistent package ---------------------------
FILES="ad4-daemon.sh ad4-serial-bridge.py klipper.hex IAPCommand ad4-net.conf ad4-install-persistent.sh ad4-uninstall-persistent.sh"
SRC="$REPO/flashdisk"
echo "==> copying package to $D:$PKG"
$SSH "$D" "mkdir -p $PKG"
for f in $FILES; do
  case "$f" in
    ad4-serial-bridge.py) p="$REPO/soc-hack/$f" ;;
    klipper.hex)          p="$REPO/stm32isp-ad4/klipper-ad4.hex" ;;
    IAPCommand)           p="$REPO/work/ctrl/$f" ;;
    *)                    p="$SRC/$f" ;;
  esac
  [ -f "$p" ] || { echo "missing local file $p"; exit 1; }
  echo "   scp $f"
  $SCP "$p" "$D:$PKG/$f"
done
$SSH "$D" "chmod +x $PKG/ad4-daemon.sh $PKG/IAPCommand 2>/dev/null; touch $PKG/mcu-klipper.ok"

# ---- 2) install the eMMC boot hook (gates rcS, enables) -------
echo "==> installing persistent hook on the SoC"
$SSH "$D" "sh $PKG/ad4-install-persistent.sh" || { echo "installer rc!=0 (it may have aborted safely); check $D:$PKG/install-status"; }

# ---- 3) flash Klipper to the MCU (runs ON the SoC) ------------
echo "==> flashing Klipper to the MCU (on the SoC)"
$SSH "$D" "cd $PKG && pkill -f ffstartup-arm 2>/dev/null; pkill -f adventurer4-arm 2>/dev/null; sleep 2; ./IAPCommand klipper.hex klipper.hex; echo flash-rc=\$?"

# ---- 4) reboot so the patched rcS runs fresh ------------------
echo "==> rebooting the SoC (boot #2 is autonomous)"
$SSH "$D" "sync; sleep 2; (reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null) &"; true
echo
echo "DONE. When the SoC is back (~30-60s):"
echo "  ping $SOC_IP            # SoC up"
echo "  nc -zv $SOC_IP 5000     # bridge up"
echo "  ./soc-hack/ad4-rpi-setup.sh   # RPi side (once), then use Fluidd"
