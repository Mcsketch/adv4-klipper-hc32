# AD4 — "All from the PI" (remote, no USB stick)

Run the whole persistent install **from the RPi over the network** — no USB
stick in the printer. The RPi is the operator; the SoC is the executor, because
**only the SoC has the MCU reset/BOOT0 GPIOs** (GPIO 34/35). The flash itself
must run on the SoC; the PI just drives it.

Status: **built** (`flashdisk/ad4-install-from-pi.sh`). The two things that are
*live-verify on your unit* are called out explicitly below — everything else is
the same verified code path as the stick.

---

## The one physical fact that shapes this

Klipper can't be flashed *from* the RPi. The RPi has no wires to the MCU; the
SoC does. So "all from the PI" really means **"the PI ssh's the SoC and makes
the SoC run the flash."** The network path is just a remote-control channel to
the SoC. That's fine and fully automatable — it just needs **a root shell on the
SoC**.

## What the PI does (automated by `ad4-install-from-pi.sh`)

1. `ping` the SoC (reachability).
2. `scp` the persistent package to the SoC's `/opt/ad4-klipper/`
   (`ad4-daemon.sh`, `ad4-serial-bridge.py`, `klipper.hex`, `IAPCommand`,
   `ad4-net.conf`, the installer + uninstaller).
3. `ssh` the SoC and run `ad4-install-persistent.sh` (gates the `rcS` vendor
   watchdog, creates the `enable` flag) — **identical code to the stick path.**
4. `ssh` the SoC and run the flash: `IAPCommand klipper.hex klipper.hex`.
5. `ssh` the SoC → `reboot`. Boot #2 is autonomous (vendor stack off,
   `eth0=192.168.10.10`, bridge on `:5000`).

Then on the RPi: `./soc-hack/ad4-rpi-setup.sh` once, use Fluidd.

---

## The prerequisite: a root shell on the SoC

This is the *only* thing you must sort out. Analysis below uses the **genuine
firmware archive** (V2.2.4-2.3), not the post-hack eMMC dump.

| Fact | Evidence (genuine archive) | Consequence |
|---|---|---|
| **Stock boots NO sshd** | The stock update `run.sh`/`auto_run.sh` are pure updaters (copy + md5-check). `auto_run.sh` only loads the WiFi driver, handles USB updates, sets GPIO34, and launches `ffstartup-arm`. Nothing starts dropbear. | **SSH (dropbear) is a post-hack addition** — it disappears after any firmware update, so it can't be the channel for a genuine/fresh AD4. |
| **telnetd is stock** | The stock base rootfs ships a proper init.d service: `start-stop-daemon -S ... -x /usr/sbin/telnetd -- -F` (you never added it). Whether the `S##telnet` boot symlink is enabled can't be confirmed from the update archive (no base rootfs in it) — **one live test decides it**: `nc -vz <soc-ip> 23`. | If telnet is up on stock, it's the cleanest PI→SoC root channel, gated only by the password. |
| **HTTP WebUI is stock, always up, no auth** | `adventurer4-arm` serves the static `client/` + a small API (`/getHomeMessage`, `switchSet`). No CGI, no file upload, no command exec in the stock package. | Useful to confirm reachability and set the IP, but it **cannot run commands** — not a path to the flash on its own. |
| **WiFi AP is stock** | `rtl_hostapd_2G.conf`: SSID `Adventurer 4`, WPA-PSK `12345678`, channel 6. | The PI can join the printer's own AP — **no LAN or static IP needed at all** to reach the SoC. |
| **Root password is a weak DES hash** | `shadow` (in the update pkg, and baked into the base rootfs): `root:H9.zxVgLFpRG2` (classic 8-byte DES — only the first 8 password chars are used). The `#cp $WORK_DIR/shadow /etc/shadow` line is **commented out**, so the hash lives in the base rootfs and **survives firmware updates** — it's the permanent stock password. | Offline brute is running (see below). Whatever it is, it stays the same after every FW update. |

### Getting the PI a shell

On a **genuine** AD4 there is no SSH. The realistic channels, best first:

1. **telnet** (if up on stock — `nc -vz <soc-ip> 23` decides):
   `telnet <soc-ip>` → `login root` → the DES password (brute in progress).
   The whole install then runs over that one session via
   `flashdisk/ad4-pi-telnet.py` (streams the package base64, verifies md5,
   runs the installer, flashes, reboots — all over telnet, no stick, no SSH).
2. **The one-stick bootstrap** (always works, passwordless): the disk
   `flashforge_init.sh` runs as root by construction. In that window set a
   known password (`echo 'root:NEWPASS' | chpasswd`) or drop an SSH key /
   enable telnetd — after which every *subsequent* install is all-from-PI.
3. **The PI reaches the SoC** without any LAN config: join the printer's own
   **`Adventurer 4` WiFi AP** (WPA-PSK `12345678`), or set a static IP on the
   SoC (touch menu) + the PI on the same subnet, or run the one-shot DHCP
   server.

> The vendor web console (`admin`/`ff123456`) is a print/cloud UI and cannot
> run commands, so it's only useful for confirming reachability / setting the IP.

---

## Getting the SoC an address for the first time (no stick)

The PI path needs to reach the SoC. On a fresh unit the SoC has no
`192.168.10.10` yet. Two ways:

1. **Touch menu (manual, simplest):** on the printer's screen, open the network
   menu and type `192.168.10.10` for the SoC. Then the PI can reach it.
2. **One-shot DHCP server on the PI (semi-automated):** the RPi runs a tiny
   DHCP server on its `eth0` (the cable to the SoC) that hands the SoC an IP in
   the `192.168.10.x` range, so the SoC comes up reachable without you typing
   anything on the screen. (This is the "launch a one-shot dhcp server on the
   pi" idea — it solves *discovery*, then the PI ssh's in. It does not itself
   flash anything.)

Both need the RPi and SoC already cabled together on the private subnet.

---

## Run it

```sh
cd adv4-klip/flashdisk
# key auth (preferred)
SOC_IP=192.168.10.10 SSH_KEY=~/.ssh/ad4-soc ./ad4-install-from-pi.sh
# or password auth
SOC_IP=192.168.10.10 SSHPASS='newpass' ./ad4-install-from-pi.sh
```

Then verify (see `PERSISTENT-INSTALL.md` → Verify):
`ping 192.168.10.10`, `nc -zv 192.168.10.10 5000`, power-cycle, re-check.

---

## Stick vs "all from the PI" — which to use

| | USB stick (default) | All-from-PI |
|---|---|---|
| **Needs** | a FAT32 stick + power the printer | a root shell on the SoC (key/password) + the PI cabled to it |
| **Credentials** | none (root by construction) | SSH key or password |
| **Flash runs** | on the SoC (triggered by the stick) | on the SoC (triggered by the PI over ssh) |
| **First-time IP** | none needed (stick is the trigger) | set via touch menu **or** one-shot DHCP on the PI |
| **Re-install / remote mgmt** | re-plug the stick | re-run the PI script |
| **Brick risk** | identical (same anti-brick guard, same flasher, same hex) | identical |
| **Best for** | "install once, then forget" | managing/re-installing remotely, no stick handy |

The flash itself is **byte-for-byte the same** in both paths (same
`IAPCommand`, same positioned `klipper.hex`, same anti-brick base guard, same
`factory-HD.hex` restore). The difference is purely *how the SoC is told to run
it* — the stick via the vendor boot updater, the PI via ssh. So the PI path
carries no extra bricking risk; it only adds the SSH dependency.

**Bottom line:** use the **stick** for the first install (zero credentials,
already proven, and it leaves you the root window to drop an SSH key for later
remote use). Use **all-from-PI** once you have that key and want remote
install/manage without the stick.

## Root password (DES) — brute status

`root:H9.zxVgLFpRG2` is classic 8-byte DES (only the first 8 password chars are
used). Cracked offline, CPU-only (no GPU — LM Studio untouched), in this order:

| Phase | Space | Size | Result |
|---|---|---|---|
| 1 | full dictionary + vendor words × numeric suffixes | ~4.6M | no match |
| 2 | exhaustive 1–5 char `[a-z0-9]` + 1–4 mixed-case | 77M | no match |
| 3 | targeted vendor list (flashforge/flash/adventurer/ff3dp/ad4… × case + year + digits, ≤8) | 1,275 | **no match** (rules out "flashforge" etc.) |
| 4 | exhaustive 6-char `[a-z0-9]` | 2,176,782,336 | **no match** (fully closed) |
| 5 | 170,456 real words × `0–99999` (≤8 chars) | 17,045,60,000 | **no match** (fully closed) |
| 6 | exhaustive 7-char `[a-z0-9]` | 78,364,164,096 | in progress (~16 cores) |

So the stock root password is a **7–8 char random alphanumeric** (not a word,
not short, not a vendor string). Phase 6 is the last CPU-feasible space; 8-char
mixed/symbol is not crackable on CPU in reasonable time. If Phase 6 misses, the
practical route is the one-stick `chpasswd` (root by construction) — not more
brute.

> "flashforge" (from the web) is **not** the root password — it hashes to
> `H9FTNb7YjvEzM`. It's most likely the vendor cloud/web-portal credential, a
> different thing from the SoC `root` shell password.
