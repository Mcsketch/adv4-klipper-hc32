# AD4 → Klipper — Getting Started (first install)

The shortest safe path from a stock Adventurer 4 to a Klipper that moves.
Everything below uses scripts already in this repo. **No soldering.** If a step
fails, jump to [Recovery](#recovery) — the MCU can never be bricked (see
[Why you can't brick it](#why-you-cant-brick-it)).

> Target board: **HC32F460 (HD revision)**. The NA/N32 variant is a separate
> firmware track — see `adv4-n32-thermistor.md`.

---

## What you need

| Piece | Role |
|---|---|
| The AD4, powered | SoC (Sunxi V3S) is wired to the MCU on `/dev/ttyS1` and owns the GPIO strapping pins — **the SoC does the flashing** |
| **RPi3** (primary host) | The Klipper **host** (runs klippy/moonraker/fluidd). Orange Pi works too |
| 1 Ethernet cable | Host ⇄ SoC `eth0`. Direct — no router, no internet |
| This repo | Synced to both machines (rsync / git) |
| (Optional) USB-serial dongle | Recovery/debug fallback only |

> **RPi3 = Klipper host only. It does not flash the MCU.** The Pi has no
> connection to the MCU's NRST/BOOT0 (GPIO34/35) pins — only the AD4's SoC
> does. So on the RPi3 you run *Klipper* (talks to the MCU over Ethernet via
> the SoC bridge); the *flash* command (`flash-mcu.sh`) runs **on the SoC**.
> "First flashing and Klipper tests with an RPi3" = flash on the SoC (Step 2),
> then bring Klipper up on the RPi3 (Steps 4–5).

**Architecture (final):**
```
[Host: Klipper] --/dev/adv4_mcu (socat PTY)--> (ethernet) --> [SoC bridge :5000] --UART--> [MCU]
```
The SoC is **offline** (only talks to the host). The MCU is released from the
vendor app and driven by Klipper through the bridge.

---

## Step 0 — Pick the SoC's IP (do this once)

SoC console: the SoC serial console is `ttyS0` (the MCU is on `ttyS1` — never
point Klipper at `ttyS0`). Give `eth0` a static IP so the host always finds it:

```sh
# on the SoC (root), static + link-local-friendly address
ip addr flush dev eth0
ip addr add 192.168.1.10/24 dev eth0
ip link set eth0 up
```
Note the SoC IP = **192.168.1.10**. Cable host ⇄ SoC. On the host, set its
wired interface to `192.168.1.20/24` (or use a crossover-free modern NIC; any
link-local pair works). Verify:
```sh
# on the host
ping -c 3 192.168.1.10
```

---

## Step 1 — Build Klipper for the AD4 (on the host or any ARM box)

```sh
git clone https://github.com/Klipper3d/klipper /opt/klipper
cd /opt/klipper
# Configure the HC32F460 target (make menuconfig):
#   Processor: HC32F460
#   Serial pins: PA3 & PA2        # PA7/PA8 CONFLICT with X-step -> do NOT use
#   Application address: 0x00C000 # matches the stock HD app window
make -j$(nproc)
# Convert to a positioned hex at the 0xC000 app window:
arm-none-eabi-objcopy -I binary -O ihex --change-address=0xC000 out/klipper.bin klipper-ad4.hex
```
**Sanity check the base** (must read `0x0000C000`):
```sh
head -1 klipper-ad4.hex        # :02000004000Cxx
```
Put `klipper-ad4.hex` into `stm32isp-ad4/` (the flasher looks for it there).

---

## Step 2 — Flash the MCU (on the SoC, as root)

```sh
cd <repo>/stm32isp-ad4
make                    # build stm32isp-ad4 (native ARM on the SoC)
./flash-mcu.sh klipper  # does: BOOT0(GPIO35) high -> NRST(GPIO34) pulse ->
                        #   /dev/ttyS1 @115200 -> 0x43 sync (reply 6=HD) ->
                        #   erase app window -> program -> verify -> jump
```
On success it writes the marker `/root/H32`. The MCU is now running Klipper
firmware (no motion yet — the host hasn't talked to it).

Restore stock at any time: `./flash-mcu.sh factory`.

---

## Step 3 — Free the MCU link + start the SoC bridge (on the SoC)

```sh
cd <repo>/soc-hack
./ad4-bridge.sh start    # stops the vendor stack, frees /dev/ttyS1,
                         # runs ad4-serial-bridge.py on TCP :5000
./ad4-bridge.sh status   # confirm RUNNING
```
The bridge is byte-transparent (no framing added) so Klipper's MCU protocol
passes through intact.

---

## Step 4 — Set up the host (on the RPi3)

**Distro:** Raspberry Pi OS **Bookworm Lite (headless), 64-bit** via Raspberry Pi Imager
(OS Customization: hostname e.g. `ad4`, enable SSH, key auth). No desktop — Fluidd runs in
your laptop's browser; the Pi only runs klippy + moonraker + the socat PTY (saves the Pi3's
1 GB RAM). Fallback if a dep ever fails: **32-bit Bookworm Lite (armhf)** — widest wheel
coverage. Then cable RPi3 ⇄ SoC and set the RPi3's wired NIC to `192.168.1.20/24`
(Step 0).

```sh
cd <repo>/soc-hack
./rpi-setup.sh 192.168.1.10 all     # installs Klipper deps + socat + the PTY unit
systemctl enable --now ad4-mcu-pty  # presents SoC:5000 as local PTY /dev/adv4_mcu
```
Drop in the printer config:
```sh
cp <repo>/printer-hc32.cfg /etc/klippy/printer.cfg
```
Confirm the `[mcu]` section points at the bridge:
```ini
[mcu]
serial: /dev/adv4_mcu
baud: 250000
```

---

## Step 5 — Start + verify (first contact)

```sh
systemctl restart klipper
tail -f /var/log/klippy.log     # expect MCU to connect, no "timed out"
```
In the Fluidd/Moonraker UI: `FIRMWARE_RESTART`, then `G28` (home) → `G1 X10
F300`. Watch it move. **Stop and test manually here** (the iterative loop).

Moonraker + Fluidd (optional, for the host UI):
```sh
/opt/klippy-env/bin/python3 -m venv /opt/moonraker-env
/opt/moonraker-env/bin/pip install moonraker fluidd
/opt/moonraker-env/bin/moonraker --help   # then enable the moonraker service
```

---

## Recovery

- **MCU won't start / bad flash:** `./flash-mcu.sh factory` (SoC). The ROM ISP
  at `0x00000000` was never written, so re-strapping always works.
- **Bring the stock printer UI back:** `AD4_RESTART_VENDOR=1 ./ad4-bridge.sh stop`
- **SoC bricked (rare):** re-flash the stock SoC rootfs from `flashdisk/` or the
  `FFAD4-BKUP.zip` eMMC image. The bridge changes are runtime-only; nothing on
  the SoC is modified.
- **Full unattended flash from a USB stick:** see `flashdisk/` (drops
  `klipper.hex` + `factory-HD.hex` + `IAPCommand` on a FAT32 stick; the stock
  boot path runs it; add a `RESTORE` file to revert to stock).

## Why you can't brick it

1. The flasher **only writes the app window** and hard-refuses anything at or
   below `0x00008000`, so the **ROM ISP stays intact** — the MCU can always
   re-enter ISP by repeating the GPIO strapping.
2. The stock image ships in the repo (`Adventurer4-HD-IAP-…hex`); `flash-mcu.sh
   factory` is one command from "back to stock".
3. All SoC-side changes are runtime-only (bridge process + socat); no eMMC
   mutation, so the stock printer is always recoverable.
