# Klipper-mode persistent boot on the NA unit (2026-10-27, paused)

Status at pause (22:45 MDT): the MCU runs Klipper 020d25e. The host stack on
prodesk is installed and running. The SoC's cold boot into `ad4-daemon.sh`
FAILED to bring up the network. The printer is currently reachable only
through the rescue stick (below).

## What is installed and working

**Host (prodesk, 192.168.1.10):**
- `~/klipper` branch `ad4-socket` (commit 802101f, `serialhdl` accepts
  `socket://`), venv `~/klippy-env`.
- `~/moonraker` + `~/moonraker-env`, Mainsail in `/opt/mainsail`.
- `~/printer_data/config/{printer.cfg,mainsail.cfg,moonraker.conf}`;
  `[mcu] serial: socket://192.168.1.219:5000`.
- systemd `klipper` + `moonraker` enabled (homeserver commit 9be89ec).
  Mainsail at http://192.168.1.10:8095 via Caddy.
- klippy retries the socket until the SoC bridge is up. Not yet verified
  against the MCU (no temps / endstops / heater test done).

**SoC `/opt/ad4-klipper/`:** installed via `ad4-install-persistent.sh`
(install-status `installed rc=0`). `/opt/auto_run.sh` line 101 is the gated
launch (`enable` flag -> daemon, else vendor UI). Backups:
`/opt/auto_run.sh.ad4-orig` (pre-patch, with dropbear),
`/opt/auto_run.sh.factory` (original). `enable` is currently REMOVED.

## What failed

1. Reboot with `enable` set (~22:05): SoC never appeared on the LAN for 18 min.
   No ARP for wlan0 or eth0. The 360 s no-IP fallback did not visibly fire.
   The daemon log is on tmpfs (`/var/log` -> `/tmp`) and was lost.
2. Stickless boot with `enable` removed: the vendor UI hangs at its splash
   and brings up no WiFi. **The vendor UI is not a fallback while Klipper is
   on the MCU.** The daemon's "reboot to the vendor UI" fallback is therefore
   useless and must be replaced.

Root cause of (1) is NOT known. The same commands work when run by hand
or from the stick (rescue log: associated in 4 s, DHCP .219 at 16 s uptime).
Differences to check: daemon runs later in rcS and backgrounded (`&`);
output redirected into the tmpfs log; no `mkdir -p /var/run/wpa_supplicant`;
`kill_vendor`/`pid_alive` scanning /proc during boot.

## The vendor USB hook (`/opt/auto_run.sh` lines 11-76)

Mounts `/dev/sda1` (vfat) on `/mnt` at ~7 s uptime and runs
`/mnt/flashforge_init.sh Adventurer4 0012`. **If it exits 0, auto_run.sh
runs `sleep 100000`**, i.e. the boot parks there and the gated
daemon/UI line and line-102 dropbear never run. A non-zero exit continues
the normal boot.

## Rescue stick (works, in the printer now)

FlashForge stick (vfat, label FLASHFORGE). `flashforge_init.sh` = rescue v2
(copy in `work/n32/rescue-flashforge_init.sh`). It removes `enable`, starts
dropbear, eth0 udhcpc, `wpa_supplicant -Dnl80211` + `enable_network all` +
wlan0 udhcpc, logs to `/tmp/rescue.log` and `/mnt/rescue.log`, exits 0 (parks).
It never touches ttyS1, NRST or the IAP. Result: SSH on 192.168.1.219 ~40 s
after power-on. The repo stick script is kept as `flashforge_init.sh.repo`;
`NO_FLASH` is still there.

## Next session (exact order)

1. Printer on with the rescue stick: SSH `root@192.168.1.219`
   (prodesk: `sshpass -f ~/.ad4-root`).
2. Quick win to print: from the rescue shell start the bridge
   (`/opt/ad4-klipper/ad4-bridge 5000 /dev/ttyS1 115200 &`), then on prodesk
   `sudo systemctl restart klipper` and verify temps, `QUERY_ENDSTOPS`,
   a short heater test. Optionally make the rescue script start the bridge,
   which is a working "Klipper mode" on its own.
3. Fix the daemon cold path: persistent trace log in `$BASE/boot.log`
   (`set -x`), `mkdir -p /var/run/wpa_supplicant`, wait for wlan0, and
   replace the vendor-UI fallback (no longer valid) with a retry of the
   WiFi bring-up. Optionally run it via `setsid` from the gated line.
4. Test loop: `touch /opt/ad4-klipper/enable`, pull the stick, power cycle.
   If no SSH within ~3 min: stick in, power cycle, read `boot.log`.
