# AD4 → Klipper — Getting Started (the easy path)

The shortest safe path from a stock Adventurer 4 to a Klipper that moves.
**No soldering. One USB stick. One script on the RPi.** Every step below is a
single command from a script already in this repo. If a step fails, jump to
[Recovery](#recovery) — the MCU can never be bricked (see
[Why you can't brick it](#why-you-cant-brick-it)).

> Target board: **HC32F460 (HD revision)**. The NA/N32 variant is a separate
> firmware track — see `adv4-n32-thermistor.md`.

---

## What you need

| Piece | Role |
|---|---|
| The AD4, powered | SoC (Sunxi V3S) is wired to the MCU on `/dev/ttyS1` and owns the GPIO strapping pins — **it does the flashing** |
| **RPi3** (primary host) | Runs Klipper (klippy/moonraker/fluidd). Orange Pi works too |
| A FAT32 USB stick | Carries the flash+setup payload |
| 1 Ethernet cable | RPi3 ⇄ AD4, direct (no router, no internet) |
| This repo | On the Mac (to build the stick) and the RPi3 (for `ad4-rpi-setup.sh`) |

**Subnet (static, no DHCP):**

| Device | IP |
|---|---|
| AD4 SoC `eth0` | **192.168.10.10/24** (set automatically by the stick) |
| RPi3 wired NIC | **192.168.10.1/24** (set by `ad4-rpi-setup.sh`) |

**Topology:**
```
[RPi3: Klipper] --/dev/adv4_mcu (socat PTY)--> (ethernet 192.168.10.x) --> [SoC bridge :5000] --UART--> [MCU]
```
The SoC is **offline** (only talks to the RPi3). The MCU is released from the
vendor app and driven by Klipper through the bridge.

> **The RPi3 runs Klipper; it does NOT flash the MCU.** The Pi has no access
> to the MCU's NRST/BOOT0 pins — only the AD4's SoC does. So: the USB stick
> does the flashing + SoC setup (Steps 1–2); the RPi3 brings up Klipper
> (Steps 3–4).

---

## Step 1 — Build the USB stick (on your Mac)

```sh
cd <repo>/flashdisk
./build-usb-stick.sh /Volumes/MYSTICK     # stick already FAT32 + mounted
```

The builder verifies the flasher's md5 and the Klipper hex's base address
before copying, then lays down: `flashforge_init.sh`, `IAPCommand`,
`klipper.hex` (positioned at `0x0000C000`), `factory-HD.hex`,
`ad4-serial-bridge.py`, `ad4-net.conf`, `screwflag`. See `flashdisk/README.md`
for the anti-brick details.

---

## Step 2 — Flash + hack the SoC (plug the stick in, power the printer)

Plug the stick into the AD4 and power it on. The SoC's stock boot/update path
executes `flashforge_init.sh`, which:

1. stops the vendor stack,
2. flashes the MCU with `klipper.hex` (guarded: refuses any image that would
   touch the ROM-ISP region),
3. sets `eth0` to **192.168.10.10/24**,
4. starts the MCU bridge on **TCP :5000**.

Wait ~30–60 s. You can unplug the stick once it's done (the bridge and network
stay up for that boot). Result is logged to `/root/ad4-flash-status` on the SoC.

---

## Step 3 — Set up the RPi3 (one script)

**Distro:** Raspberry Pi OS **Bookworm Lite (headless), 64-bit** via Raspberry
Pi Imager (OS Customization: hostname e.g. `ad4`, enable SSH, key auth). No
desktop — Fluidd runs in your laptop's browser; the Pi only runs klippy +
moonraker + the socat PTY (saves the Pi3's 1 GB RAM). Fallback if a dep ever
fails: **32-bit Bookworm Lite (armhf)** — widest wheel coverage.

On the RPi3, with the repo synced there and the Ethernet cable plugged in:

```sh
cd <repo>/soc-hack
sudo ./ad4-rpi-setup.sh             # add --moonraker for the web UI (Fluidd)
```

It: sets the wired NIC to **192.168.10.1/24**, preflights the link (pings the
SoC, opens :5000), installs deps + builds Klipper, drops in the printer config
(`[mcu] serial: /dev/adv4_mcu`), and enables the socat PTY + klipper services.

---

## Step 4 — First contact

```sh
tail -f /var/log/klippy.log         # expect MCU to connect, no "timed out"
```

In Fluidd/Moonraker: `FIRMWARE_RESTART`, then `G28` (home) → `G1 X10 F300`.
Watch it move. **Stop and test manually here** (the iterative loop).

---

## Recovery

- **Revert to stock (no solder, no SSH):** put a `RESTORE` file on the USB
  stick (`touch RESTORE`), re-plug it, power the printer. It flashes
  `factory-HD.hex`.
- **MCU won't start / bad flash:** the ROM ISP at `0x00000000` was never
  written, so re-strapping (re-running the flash) always recovers it.
- **Bring the stock printer UI back** (after a manual `ad4-bridge.sh stop`):
  `AD4_RESTART_VENDOR=1 ./ad4-bridge.sh stop` on the SoC.
- **SoC bricked (rare):** re-flash the stock SoC rootfs from `FFAD4-BKUP.zip`.
  The bridge/network changes are runtime-only; nothing on the SoC eMMC is
  modified.

## Why you can't brick it

1. The MCU flasher **only writes the app window** and the flash script
   **refuses any image whose lowest address is ≤ 0x00008000**, so the **ROM
   ISP stays intact** — the MCU can always re-enter ISP by repeating the GPIO
   strapping.
2. The stock image ships in the repo (`Adventurer4-HD-IAP-…hex`); a `RESTORE`
   file on the stick is one reboot from "back to stock."
3. All SoC-side changes (bridge + static IP) are **runtime-only**; no eMMC
   mutation, so the stock printer is always recoverable.

---

## Where the details live

- **`flashdisk/README.md`** — the anti-brick guard, the iHEX type-05 fix, stick layout
- **`KLIPPER-AD4-FLASH-GUIDE.md`** — the full flashing deep-dive (IAPCommand, GPIO strapping, two roles)
- **`adv4-soc-hack.md`** — the SoC bridge architecture (no-solder RPi↔SoC↔MCU)
- **`adv4-n32-thermistor.md`** — the NA/N32 chip track (thermistor LUTs, KV flasher review)
- **`adv4-touch-ui.md`** — driving the SoC touchscreen as the Klipper UI
