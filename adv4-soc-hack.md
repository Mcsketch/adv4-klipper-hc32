# Flashforge Adventurer 4 — Klipper Over the SoC (No Soldering)

How to run Klipper on an Adventurer 4 **without soldering anything**, using the
SoC as a serial↔network bridge and (optionally) its touch screen, while a
Raspberry Pi on the other end of the Ethernet link is the real Klipper host.

Everything in this document that is marked **verified** was read directly from
the working-machine backup (`FFAD4-BKUP.zip` — a full eMMC image: rootfs,
`/data`, and the kernel boot log). Everything else is design or to-be-confirmed
on hardware.

---

## 1. The architecture (no soldering)

The key fact that makes "no soldering" possible: **the SoC is already wired to
the MCU.** In the working machine the MCU sits on the SoC's **uart0, exposed as
`/dev/ttyS1`** (verified in the boot log: `uart0: ttyS1 at MMIO 0x1c28000`).
So the MCU is already reachable by software, and the SoC already has a network
port. We do not touch the MCU's pins at all — we just take its serial traffic
off the vendor app and hand it to Klipper over the wire.

```
   ┌─────────────────────┐        Ethernet         ┌───────────────────────┐
   │  Raspberry Pi        │  <══════════════════►   │  SoC (sun50i V3S)      │
   │                       │  100/1000 MbE,         │                        │
   │  Klipper (klippy)     │  any protocol:         │  ad4-serial-bridge.py  │
   │  Moonraker / Fluidd   │  TCP (default)         │  /dev/ttyS1  ⇄  :5000  │
   │  /dev/adv4_mcu (PTY)  │  or UDP                │                        │
   └─────────────────────┘                        │  + TSC2007 touch→fb0   │
            socat (pty)                            │  + eth0 (gmac0)         │
                                                     │  + USB host, wlan0     │
                                                     └───────────┬────────────┘
                                                                 │ UART 250000
                                                                 ▼
                                                          ┌───────────────┐
                                                          │ HC32F460 MCU   │
                                                          │ (Klipper fw)   │
                                                          └───────────────┘
```

Division of labor:

- **RPi** = Klipper host. Runs `klippy`, Moonraker, Fluidd/Mainsail. Reaches the
  MCU through a local PTY that `socat` maps to the SoC's TCP/UDP port.
- **SoC** = bridge (and, later, touch UI). It owns the MCU's serial link and
  its own Ethernet port. It does **no** motion math — it is a dumb, fast byte
  pump. Kept otherwise idle while printing.
- **MCU** = runs Klipper firmware, exactly as if the RPi were wired to it.

Why the SoC and not the RPi talks to the MCU: the **SoC is the only chip wired
to the MCU's NRST/BOOT0** (needed for flashing, §7). Giving the SoC the MCU
serial link too means one device does both flashing and bridging, and the RPi
stays a clean, swappable Klipper host with no soldered MCU connection.

No soldering: the RPi↔SoC link is the printer's own Ethernet port (or a
patch cable if you route the SoC's RJ45 to the RPi). The SoC↔MCU link is the
factory UART. Nothing new is wired.

---

## 2. What the SoC can drive (verified from the working machine's boot log)

From `dmesg-start.txt` of the working printer:

| Subsystem | What the SoC exposes | Evidence |
|---|---|---|
| **MCU serial** | `/dev/ttyS1` (uart0) @ 250000 | `uart0: ttyS1 at MMIO 0x1c28000` |
| **Ethernet** | `eth0` = **gmac0** GbE, real PHY (`00441400`) | `gmac0 gmac0: eth0: PHY ID 00441400 at 0` |
| **WiFi** | `wlan0` (RTW, mac `94:a4:08:83:df:07`) | `RTW: rtw_ndev_init(wlan0)` |
| **Touch** | **TSC2007** driver, input event | `TSC2007 TouchScreen Driver probe`, `/dev/input/event0,1`, `/dev/touchscreen/ucb1x00` |
| **Display** | framebuffer **`/dev/fb0`** (DISP, 400 MHz dclk) | `console`/`[DISP]` blocks, `/dev/fb0` referenced 1582× |
| **USB host** | EHCI + OHCI host controllers | `sunxi-ehci/ohci sunxi-ehci/ohci.1`, `sunxi-ohci... bus 2` |
| **USB mass storage** | `usb-storage` + `sda1/sda2` | `Initializing USB Mass Storage driver` |
| **i2c** | `twi0` @ 100 kHz, `twi1` | `twi_chan_cfg() [twi0]` / `[twi1]` |
| **PWM** | `pwm module init!`, `/dev/pwm` | `pwm module init!` |
| **Backlight** | via PWM (`/dev/pwm`) | `Backlight Compensation` UVC map + pwm |
| **Power (PMIC)** | AXP20x (`axp20_board`, CHIP ID 0x41) | `AXP (CHIP ID: 0x41) detected` |
| **Console** | `ttyS0` (uart2) @ 115200 — **not** the MCU | `console [ttyS0] enabled`, `uart2: ttyS0` |
| **SoC** | Allwinner **sun8i / sun50i V3S**, kernel 3.4.39+ armv7l | `Machine: sun8i`, `Linux version 3.4.39+` |

Consequences:

- **Touch is SoC-driven (TSC2007).** The SoC owns the touch panel; you do not
  need to re-wire it. The RPi never needs a direct touch link — the SoC can
  render a UI on `/dev/fb0` and read touch, or simply act as a bridge (below).
- **`eth0` is real wired GbE.** The RPi can talk to the SoC over the printer's
  Ethernet port with no extra hardware. This is the "not connected to any
  network, only speaking to the RPi" path you asked for: cable RPi↔SoC directly
  (or via the board's RJ45), no router, no internet.
- **USB is a host.** A USB camera (`/dev/video0`, `uvcvideo`), a USB stick
  (`sda1/sda2`), or a USB-serial adapter all work on the SoC's USB port.
- **`ttyS0` is the console, `ttyS1` is the MCU.** Do not point Klipper at
  `ttyS0` — that is the SoC's own serial console.

---

## 3. How to use Klipper once the MCU is flashed

Three pieces of software:

1. **Klipper firmware on the MCU** — already covered in
   `KLIPPER-AD4-FLASH-GUIDE.md` / `stm32isp-ad4/` (§7 here for flashing).
2. **The SoC bridge** — `soc-hack/ad4-serial-bridge.py` + `ad4-bridge.sh`.
   Takes the MCU's `/dev/ttyS1` and exposes it on a TCP (or UDP) port.
3. **The RPi host** — Klipper + Moonraker + Fluidd, with `socat` presenting the
   remote port as a local PTY.

### 3.1 SoC side (bridge)

```sh
# one-time: copy soc-hack/ to the SoC (USB stick or scp), then:
cd /root/ad4-soc-hack
./ad4-bridge.sh start          # stops vendor stack, starts TCP bridge on :5000
./ad4-bridge.sh status
```

`ad4-bridge.sh start` does, in order:

1. `pkill` the vendor app (`ffstartup-arm`, `adventurer4-arm`) and
   `fuser -k /dev/ttyS1` so nothing else owns the MCU link.
2. Runs `ad4-serial-bridge.py --tcp 0.0.0.0:5000 --uart /dev/ttyS1 --baud 250000`.
   The bridge opens the tty **raw, 8N1, 250000**, flushes it, and pumps bytes
   both directions until the client disconnects (one client at a time — Klipper).

`--udp` is available if you measure TCP as a bottleneck (see §6 on latency).

### 3.2 RPi side (Klipper host)

```sh
# on the RPi, as root:
./rpi-setup.sh 192.168.1.20      # 1.20 = the SoC's eth0 IP
systemctl enable --now ad4-mcu-pty
```

`rpi-setup.sh` installs Klipper, enables the hardware UART, and installs a
`ad4-mcu-pty.service` that runs:

```
socat TCP4:192.168.1.20:5000,reconnect,keepalive,nodelay \
        OPEN:/dev/adv4_mcu,raw,echo=0,nonblock
```

This makes the remote MCU appear as a **local serial device `/dev/adv4_mcu`**,
which is exactly what Klipper's MCU layer expects.

### 3.3 Klipper config (the only line that differs from a wired RPi build)

In `/etc/klippy/printer.cfg`:

```ini
[mcu]
serial: /dev/adv4_mcu        # the socat PTY, not /dev/serial0
baud: 250000
restart_method: command
```

(The rest of `printer.cfg` is identical to the wired build — see
`printer-hc32.cfg`. The MCU pin/thermistor section does **not** change, because
from the MCU's point of view nothing changed: it still sees a 250000-baud
serial peer, it just happens to be a TCP socket on the other end of the SoC.)

Then `systemctl restart klipper`. On first connect Klipper does its MCU clock
sync; if that succeeds over the bridge, the printer is live.

---

## 4. Using the SoC's touch UI, USB, and network

You said you want to use the SoC's touch screen for info and settings, keep the
SoC **off any real network** (only talking to the RPi), and use USB/network on
the SoC. How each maps to the hardware above:

### 4.1 Touch + display (SoC-local, no soldering)

The SoC already runs a TSC2007 touch and a `/dev/fb0` framebuffer. Two ways to
use them:

- **Option A — SoC as a thin Fluidd/Mainsail client (simplest).** Run a tiny
  framebuffer UI (or a kiosk) on the SoC pointed at the RPi's Moonraker over the
  same Ethernet link. Touch input comes from the TSC2007 (`/dev/input/event0`),
  display goes to `/dev/fb0`. No MCU work on the SoC; all motion/state is on the
  RPi. This is the least custom code.
- **Option B — SoC renders its own panel UI.** A small Qt/lvgl/fbcon app reads
  temperature/position from Moonraker (or, minimally, directly from the MCU
  through the bridge) and shows it on `/dev/fb0`, with TSC2007 touch for
  buttons. More code, but fully offline and self-contained on the SoC.

Either way the **touch is driven by the SoC**, so you never wire the touch to
the RPi. The RPi only ever talks to the MCU (via the bridge) and to the SoC's
UI (via the network).

### 4.2 USB (SoC host)

- **USB camera** → `/dev/video0` (already probed by `uvcvideo`). Use for a live
  view rendered to `/dev/fb0` on the SoC, or streamed over the Ethernet link.
- **USB stick** → `sda1/sda2`. This is also the **unattended flash** medium
  (§7) and a place to put the factory restore hex + Klipper hex.
- **USB-serial adapter** (optional). If you ever *do* want the RPi to have a
  direct serial MCU link, a USB-serial dongle on the SoC's USB port plus the
  two UART wires is the fallback — but that is the soldering path, not needed here.

### 4.3 Network (SoC, offline-only)

The SoC has both `eth0` (wired GbE) and `wlan0` (WiFi). For the "not connected
to any network, only speaking to the RPi" goal:

- Use **`eth0`** for the RPi↔SoC bridge. Wire the SoC's RJ45 to the RPi (direct
  crossover/auto-MDIX cable, or the board's port). Put a static IP on both
  (SoC `192.168.1.20`, RPi `192.168.1.10`, both `/24`).
- **Disable `wlan0`** (and any cloud/`sz3dp.com` telemetry the vendor app
  tries to reach) so the SoC talks to exactly one peer: the RPi.
- The Ethernet link can speak **any protocol** — the bridge uses TCP by default
  (reliable, ordered, reassembles; best for Klipper's timing-sensitive MCU
  protocol). UDP is offered if you later measure that throughput/latency needs
  it. Nothing forces a specific application protocol; it is a byte pipe.

---

## 5. Latency / real-time note (why TCP is the default)

Klipper's MCU protocol is timing-sensitive: after the host sends a command it
expects the MCU's reply (and periodic clock-sync) promptly; a too-slow or
dropped response makes Klipper drop the MCU and error. The bridge is a
**byte pump**, so added latency is one Ethernet hop plus Python `select()`.
On a direct RPi↔SoC cable that is well under a millisecond in each direction —
comfortably inside budget. Keep the SoC otherwise idle (vendor app stopped,
no CPU-heavy UI) while printing. If you push high message rates, switch the
bridge to `--udp` (fewer handshake bytes, no retransmit stalls) and/or raise the
SoC's scheduler priority for the bridge process. This is the one thing to
**verify live**: run a normal print and watch for `MCU '...': too many serial
timeouts` in Klippy.

---

## 6. The forked `stm32isp` (GPIO dance built in) — done

You asked me to begin a forked `esp32isp`/`stm32isp` for this board with the
GPIO strapping built in. It is written, compiles clean, and its HEX parser is
validated against the factory hexes. Source: **`stm32isp-ad4/`**.

- `stm32isp-ad4.c` — self-contained stm32isp fork. The **only** delta over a
  generic stm32isp is the SoC GPIO dance (the part a stock tool cannot do):
  - export **GPIO 34 = NRST**, **GPIO 35 = BOOT0** (SoC sysfs);
  - `BOOT0=HIGH`, `sleep(1)`, then a **NRST pulse** (15.68 ms LOW / 15.68 ms
    HIGH — `15680` µs, matching the factory `IAPCommand`);
  - open `/dev/ttyS1` @115200, send `0x43` with **retry** (re-pulse NRST on
    no-answer — the reason stock tools time out);
  - standard ROM-ISP ops (get-PID `0x02`, erase `0x44`, program `0x46`,
    read-back `0x11`, jump `0x08`);
  - **safety guard**: refuses any image whose base is ≤ `0x00008000` (never
    clobber the reset/ROM-ISP region at `0x00000000`, which is what keeps every
    state recoverable);
  - post-write: `BOOT0=LOW` + NRST pulse to boot the new app; writes
    `/root/H32` (HD) or `/root/N32` (NA) exactly like the factory tool.
- `Makefile` — native build on the SoC, or `make CROSS=arm-linux-gnueabihf-`.
- `flash-mcu.sh` — SoC driver: `./flash-mcu.sh klipper|factory|<image.hex> [-v]`.
- `Adventurer4-HD-IAP-2.3.0-20230216.hex` (the stock image, for restore).

**Revision reality (verified by parsing the factory hexes):**

| Image | Base (from the hex) | Size |
|---|---|---|
| `Adventurer4-HD-IAP-2.3.0-20230216.hex` | **0x0000C000** | 60 416 B |
| `Adventurer4-NA-IAP-2.3.0-20230216.hex` | **0x08010000** | 44 640 B |

The two revisions sit at **different base addresses**; the factory `IAPCommand`
picks between them from the sync reply (byte 6 ⇒ HD, 7 ⇒ NA) — hence the
`<na.hex> <hd.hex>` two-argument model, which `flash-mcu.sh` and the unattended
script reproduce. Your board is the **HD** (H32) revision, so the Klipper hex
must be built for the same window the HD image uses.

### 6.1 Flashing the Klipper hex (no soldering — it goes through the SoC)

The MCU is flashed **through the SoC** (it owns NRST/BOOT0), so flashing never
needs the RPi or any soldered leads:

```sh
# on the SoC, printer powered, vendor app can be up or down:
cd /root/ad4-soc-hack
./flash-mcu.sh klipper        # flash the Klipper hex
./flash-mcu.sh factory        # restore the stock image (always available)
```

---

## 7. Recovery (documented)

Two independent recovery layers. You cannot brick the MCU, because the flasher
never writes the MCU's ROM-ISP region.

**A. MCU recovery (no hardware, always available).**
If a Klipper image misbehaves (printer won't run, Klippy drops the MCU, etc.):

1. Power the printer.
2. On the SoC: `./flash-mcu.sh factory` (or the unattended `RESTORE` path, §8).
   This re-straps the MCU into ROM-ISP and writes back
   `Adventurer4-HD-IAP-2.3.0-20230216.hex`. Because the ROM-ISP at `0x00000000`
   is never touched, the MCU will always re-enter ISP and boot the stock image.
3. Reboot the printer → stock firmware runs again.

The unattended version: on a USB stick at the disk root, place the factory hex
plus a `RESTORE` flag file; the SoC's boot/update path runs
`flashforge_init.sh`, which detects `RESTORE` and flashes the factory image
(see `flashdisk/` and the unattended section of `adv4-exploration.md`).

**B. SoC recovery (the Linux on the eMMC).**
The SoC itself can be re-flashed independently of the MCU, because its eMMC is
a standard image and we hold a **full verified backup** of a working machine:

- `FFAD4-BKUP.zip` is a complete eMMC image (rootfs `mmcblk0p2`, data
  `mmcblk0p3`, MBR with verified partition offsets). It is the restore source
  for the SoC's Linux if a SoC-side hack (bridge, UI, network) ever breaks
  boot.
- Restore = write the backed-up eMMC image back to the SoC's `mmcblk0`
  (via the USB-emmc path the backup was taken with, or the board's recovery).
  Because the MCU is separate and recoverable on its own (§A), SoC and MCU
  recovery do not depend on each other.

**What to keep (never delete):**

- `Adventurer4-HD-IAP-2.3.0-20230216.hex` — stock MCU image.
- `FFAD4-BKUP.zip` — stock SoC eMMC image.
- The Klipper hex + `printer-hc32.cfg` — the target state.

---

## 8. Open items (verify on hardware)

| # | Item | How to confirm |
|---|---|---|
| 1 | Klipper MCU connect over the bridge | `systemctl restart klipper`; watch for `too many serial timeouts` |
| 2 | Bridge latency adequate | normal print; if timeouts, switch `--udp` and raise bridge priority |
| 3 | MCU serial baud the Klipper build actually uses | set `[mcu] baud:` to match `CONFIG_..._BAUDRATE` in the Klipper fw (default 250000) |
| 4 | SoC UI path (Option A vs B, §4.1) | build whichever; touch = `/dev/input/event0`, display = `/dev/fb0` |
| 5 | Hotend thermistor pin / curves | `probe.cfg` sweep (see `adv4-exploration.md` §10) — unchanged by this bridge |
| 6 | Exactly which of the two HD base addresses the Klipper build targets | match the HD image window (0x0000C000) — build Klipper for that, verify with the flasher's read-back |
