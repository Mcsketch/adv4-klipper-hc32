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

This is the *only* thing you must sort out, and it's where your "easy on
`flashforge_init.sh`" intuition is exactly right. Three facts from the eMMC
dump:

| Fact | Evidence | Consequence |
|---|---|---|
| **Root password is a weak DES hash** | `shadow`: `root:H9.zxVgLFpRG2` (2 chars + 8 base64 = classic 8-byte DES, first 8 chars of the password only) | Small search space, but it **could not be cracked offline** here. Live-verify the password on your unit, or just **set it** (below) — DES means even a 6-char password is the whole thing. |
| **The disk updater already runs as root** | `flashforge_init.sh` runs with `USER=root` (seen in the boot log env dumps) | **You never need the password for the stick path.** And once you have *any* root (even a moment from the stick), you can `echo 'root:NEWPASS' | chpasswd` to set a known password for the PI path. |
| **The vendor web console is not a shell** | `webconsole` is a Qt print/cloud UI (`cloud.sz3dp.com`); no `system()`/`/bin/sh -c` command-exec handler found in the 4.6M-string dump | The `admin`/`ff123456` web login **cannot run arbitrary commands**, so it's not a path to the flash on its own. (It *is* handy for setting the SoC IP and for print jobs.) |

### Recommended way to get the PI a shell (cheapest, uses what we have)

**Do one stick boot** (the normal first run). While the stick is running, the
SoC is root for that window — so grab a shell and install an **SSH key** (no
password needed afterwards):

```sh
# on the RPi, during the stick's power window (SoC is 192.168.10.10):
# generate a key if you don't have one
ssh-keygen -t ed25519 -N "" -f ~/.ssh/ad4-soc
# push it to the SoC's root (password once, if prompted — set a known one first):
sshpass -p 'THE_PWD' ssh root@192.168.10.10 \
  'mkdir -p /root/.ssh && chmod 700 /root/.ssh'
sshpass -p 'THE_PWD' scp -i ~/.ssh/ad4-soc ~/.ssh/ad4-soc.pub \
  root@192.168.10.10:/root/.ssh/authorized_keys
```

Or set a known password once (the DES hash caps it at 8 chars effectively):

```sh
ssh root@192.168.10.10 'echo "root:newpass" | chpasswd'
```

After that, the PI path is key/password auth and fully scriptable.

> **Honest note:** I did *not* crack `H9.zxVgLFpRG2` offline (tried a broad
> wordlist; no hit). If you'd like, I can run a proper DES brute (e.g. hashcat
> `-m 1500` over a custom list) — it's a small keyspace given the 8-byte DES
> cap, but it needs the right candidate list and I'd rather do it than guess.

### If you never want a password at all
The **stick path is already passwordless** (it's root by construction). The PI
path *adds* convenience (remote, no stick) but is only worth it if you'll
re-install or manage the SoC often. For "install once, then forget," the stick
+ persistent hook is the lowest-friction, lowest-credential route — the PI path
is for when you want to manage it remotely.

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
