# AD4 → Klipper — First-Run Manual (the 7 easy steps)

Everything below is a copy-paste or a button click. The scripts do the rest.
**No soldering.** If a step fails, see [If something goes wrong](#if-something-goes-wrong).

> **Read this first:** the printer's SoC does the MCU flashing (it owns the
> reset/BOOT0 pins). The RPi/Orange Pi only *runs* Klipper. The two talk over
> a private Ethernet cable. Your normal home LAN is never touched.

---

## 0. What you'll touch

- **RPi3** (or Orange Pi) — the Klipper host. **Raspberry Pi OS Bookworm Lite,
  64-bit, headless** (pick "Enable SSH" in Raspberry Pi Imager so you can
  log in).
- **1 USB stick**, **FAT32**.
- **1 Ethernet cable** — straight from the RPi to the **printer's** RJ45
  (does **not** go to your router).

**The two IPs (the "exotic" private subnet — won't fight your 192.168.1.x router):**

| Device | IP |
|---|---|
| RPi wired port | **192.168.10.1** |
| Printer SoC | **192.168.10.10** |

If you want something less common still, it's a one-line change in
`soc-hack/ad4-rpi-setup.sh` and `flashdisk/flashforge_init.sh` (the `*_IP=`
lines) — tell me which and I'll set it.

---

## Step 1 — Flash the RPi SD card

1. Raspberry Pi Imager → your SD → **Raspberry Pi OS (Other) → Raspberry Pi
   OS Lite (64-bit)**.
2. OS Customization: **enable SSH**, set a hostname (e.g. `ad4`).
3. Write it to the SD, put it in the RPi, plug in **power + the Ethernet cable
   to the printer's RJ45**.

---

## Step 2 — Put the repo on the RPi

From your Mac (where this repo is):

```sh
rsync -av --exclude work/ ./ /path/to/usb-key/adv4-klip/   # copy to the stick
```
then on the RPi (after it's up):
```sh
ssh pi@<rpi-ip>
sudo cp -r /media/*/adv4-klip /root/adv4-klip
```

> Or keep it simple: leave `adv4-klip/` on the stick, `cd` to it on the RPi.

---

## Step 3 — Make the USB flash key

On your **Mac**, with the stick FAT32 and mounted:

```sh
cd /path/to/adv4-klip/flashdisk
./build-usb-stick.sh /Volumes/MYSTICK
```

It lays down the flash payload and **verifies the flasher's checksum and the
Klipper image's base address** before it copies anything. (This is the
"never bricked" guarantee.)

---

## Step 4 — Flash the printer (this is the "hack")

Unplug the stick from the Mac → **plug it into the printer → power the
printer on.** The printer's stock boot path runs the script on the stick and:

1. flashes the MCU with Klipper,
2. sets the SoC to **192.168.10.10**,
3. starts the MCU-over-Ethernet bridge on port **5000**.

Wait ~30–60 s, then unplug the stick. ✅ **Printer is now Klipper-ready.**

> **No stick handy / want to go over the network?** That's the "RPi pulls it
> over Ethernet" flow — see [Option B](#option-b--rpi-driven-flash-no-usb-stick).

---

## Step 5 — Set up the RPi (one script)

On the RPi:
```sh
cd /root/adv4-klip/soc-hack
sudo ./ad4-rpi-setup.sh            # add  --moonraker  to get the web UI
```
Sets the RPi's IP to **192.168.10.1**, checks the cable is up, installs
Klipper, and starts the MCU link. Done when it prints **DONE**.

---

## Step 6 — First contact

```sh
tail -f /var/log/klippy.log        # MCU connects, no "timed out"
```
Open Fluidd/Mainsail in a browser at **http://192.168.10.1:8080** (Fluidd) —
`FIRMWARE_RESTART`, then `G28`, then `G1 X10 F300`. Watch it move. **Stop here
and test.**

---

## Step 7 — Use Klipper from your normal LAN (the 2nd NIC)

The RPi needs a **second network port** so it can reach the *printer* (private
`.10` subnet) **and** your *laptop/phone* (home LAN) at the same time.

- **RPi3 model B+** has **2 built-in Ethernet ports** → perfect, nothing to buy.
- **RPi3 model B (1 port)** or **Orange Pi** (often 1): add a **USB-Ethernet
  adapter** (or a 2-port USB hub with two adapters). It shows up as `eth1`.

Layout (what `ad4-rpi-setup.sh` already sets for port 1; port 2 = your normal
LAN):

```
  [Printer SoC] 192.168.10.10 ──eth0── [RPi] ──eth1── [home router / your LAN]
```

Put your **home** address on `eth1` (or leave it on DHCP — it just joins your
existing LAN). **Fluidd then works from your laptop** at the RPi's home-LAN IP.
If you want a second port added automatically, say the word and I'll extend the
setup script to label the two ports.

---

## If something goes wrong

- **Printer back to stock (no tools, no SSH):** `touch RESTORE` on the USB
  stick → re-plug → power the printer. It re-flashes the stock image.
- **Klipper won't start / bad flash:** re-run Step 4. The MCU's recovery
  (ROM-ISP) was never touched, so this always works.
- **RPi can't see the printer:** check the cable is RPi↔printer (not RPi↔
  router), and `ping 192.168.10.10` on the RPi.
- **Recovery only, no internet:** nothing here needs the internet — the RPi
  and the printer form their own private network.

---

<a id="option-b--rpi-driven-flash-no-usb-stick"></a>
## Option B — RPi-driven flash (no USB stick in the printer)

*(Study only — not wired up yet.)* To make the **RPi do the flash over the
cable**, the RPi needs a shell on the SoC. What's available today:

| Route | Does it give the RPi a SoC shell? | Notes |
|---|---|---|
| **SSH** | **Yes, if enabled** — this is the "backdoor" | The stock SoC Linux has `ssh`/`telnet` (the old guide's "Path A — over SSH"). The RPi would `ssh root@<soc>` and run the flash command itself. *Live check needed:* confirm the port is open + the root password on your specific unit (usually set on first boot / via the touch network menu). |
| **Vendor touch network menu** | **No** | You *can* type the SoC's IP in on the printer screen, but it's just a config screen — it doesn't open a remote shell to run commands. It helps you *find* the SoC, not control it. |
| **DHS server on the RPi** | **No** (only the IP) | A one-shot DHCP server on the RPi hands the SoC an IP so the RPi can *reach* it — useful for discovery, but it doesn't by itself flash anything. You still need SSH (above) to run the flash. |
| **USB stick** | **n/a — already works** | This is the current, proven path (Steps 3–4). |

**Bottom line:** the cleanest "RPi does everything over the cable" flow is
**RPi runs a DHCP server → learns the SoC's IP → `ssh root@<soc>` → run the
flash command → bring up the bridge → set the static `.10` subnet.** The only
unknown is the live SSH credential/port on your unit — that's the one thing to
verify on the printer before building it out. The USB-stick path is the
fallback that needs zero knowledge of that.
