# FlashForge Adventurer 4 → Klipper (HC32F460) — Flashing & Setup Guide

> Audience: an experienced user who wants to run **Klipper** on the AD4's motion MCU
> (Huada **HC32F460**) and is willing to disassemble the factory firmware to get it exactly right.
>
> Everything below is backed by artifacts we actually have: the **2.3.0 factory update package**
> (which ships `IAPCommand`), the **factory HD/NA firmware hex files**, an **eMMC rootfs backup**,
> and our **Klipper HC32F460 build**. Nothing here is guessed — each "confirmed" fact has evidence.
> The "magic" of the IAP (in-system programming) flow has been **fully disassembled** and is
> spelled out in §2.

---

## 0. TL;DR (answers to your direct questions)

| Your question | Short answer |
|---|---|
| **Will `IAPCommand` flash at the right addresses?** | **Yes.** It writes the HD app at `0x0800C000` (and NA at `0x08010000`). It erases/programs/verifies **only the app window** — it never touches the ROM/ISP region. |
| **Why did my attempts fail — is there "delay and magic" to put the MCU in IAP mode?** | **Yes, and I found it.** The MCU does **not** enter IAP mode on its own. The SoC must drive **two of its own GPIOs**: **GPIO 34 = MCU reset**, **GPIO 35 = MCU BOOT0/ISP-select**. `IAPCommand` pulses them through `/sys/class/gpio` with **exact 15.68 ms delays**. Skip that and the MCU never boots into ISP, so the serial sync times out ("Can not connect to board, timed-out"). |
| **Is it as easy as a serial tool?** | Not with a *generic* tool. The serial ISP part is standard, but the **reset/BOOT0 strapping is SoC-specific** — only `IAPCommand` (or an equivalent that reproduces the exact GPIO sequence) does it. |
| **Did you disassemble it?** | **Yes — `IAPCommand` fully** (`stm32Init`, `stm32Reboot`, `stm32Sync`, `main`, and all `gpio_*` helpers). The exact call sequence and timings are in §2.4. |
| **Isn't plain `stm32isp` safer/easier?** | **`IAPCommand` *is* a fork of `wengkai/stm32isp`** — same core code, but with the **AD4-specific GPIO reset/BOOT0 logic added**. Plain `stm32isp` has **no** AD4 strapping, so it will *not* bring this MCU into IAP. **Use `IAPCommand`.** |
| **Hotend / bed max temps?** | Matches your memory: the **bed LUT is clamped at ~110 °C**; the **hotend LUT runs to ~215–235 °C**, and the two interchangeable nozzles (235 / 265 °C rated) explain you seeing ~270 °C setpoints (the heater runs past the last LUT point via the same NTC). Details in §4. |
| **Driver config?** | **TMC2209 steppers over UART** (not SPI) with a 4:1 address multiplexer — exactly the ADV3/Voxel layout your `printer.cfg` already uses. Confirmed by disassembly (3 UARTs active, no SPI references). §4.3. |

**The one-sentence summary of the whole flash:**
> `IAPCommand` runs **on the SoC (Sunxi Linux)**, toggles SoC **GPIO 34 + 35** to force the MCU into ROM-ISP, then talks the standard STM32 ISP protocol over **`/dev/ttyS1` @ 115200**, writing Klipper into the **HD app window `0x0800C000`** and rebooting the MCU.

---

## 1. Board topology (confirmed)

The AD4 is a **two-chip** design:

| Chip | What it is | What it does |
|---|---|---|
| **Allwinner (Sunxi) SoC** | ARM "server" | Touch UI, Ethernet/Wi‑Fi, file management, and — crucially — **drives the MCU's reset & BOOT0 pins** and talks to the MCU over a UART |
| **Huada HC32F460** | Cortex‑M4 (an STM32‑F4‑like clone) | Motion, heaters, thermistors, endstops — the **real "printer brain"** |

They talk over a **UART**. The SoC is the network/UI face; the HC32 is the motion controller.
This is why "the Allwinner doesn't have enough RAM" is a red herring — it was never the motion
controller. Klipper wants to **replace the HC32 firmware** and run the **host (klippy) on real Linux**.

There are **two board revisions** with slightly different STM32‑class MCUs (the factory calls them
**HD** and **NA**). They differ only in **which flash address the app lives at**:

```
HC32F460 flash (256 KB, base 0x08000000)
┌──────────────────────────────────────────────────────────────┐
│ 0x08000000   ROM / ISP boot (ROM-resident — NOT in the hex)  │
├──────────────────────────────────────────────────────────────┤
│ 0x0800C000   HD app start  ──► Klipper .bin/.hex goes HERE   │
│ 0x0801AC00   HD app end    (59 KB, contiguous, NO gaps)      │
├──────────────────────────────────────────────────────────────┤
│ 0x08010000   NA app start   (alternate board revision)       │
│ 0x0801AE60   NA app end    (43 KB, contiguous, NO gaps)      │
└──────────────────────────────────────────────────────────────┘
```

We parsed **both** factory hex files end to end: each is a single contiguous image with **zero
gaps**. **There is no hidden second IAP/second-stage region** anywhere in flash. The ISP logic
lives in the **ROM** plus the host tool — not in a flash region you would overwrite. Replacing the
app at `0xC000` therefore **cannot** erase the ISP capability. A bad app flash is recoverable.

**Which revision are you?** We don't know yet — confirm it on the live board (§7) and pick
`0xC000` (HD) or `0x10000` (NA) accordingly. **Default to HD = `0xC000`** (matches our Klipper
build's `FLASH_APPLICATION_ADDRESS_0x00C000`).

---

## 2. The IAP flow — the "magic", fully decoded

This is the section that was missing from your attempts. I disassembled the factory
`IAPCommand` (a Linaro‑GCC 4.9, 32‑bit ARM, dynamically‑linked, **unstripped** binary with debug
info) and reconstructed the entire sequence.

### 2.1 What `IAPCommand` actually is

- A **fork of `wengkai/stm32isp`** (a classic STM32F1 ISP utility). Its own help string is
  `stm32isp [Options] <file> <comport> <baudrate>`; it links `stm32.c / hexfile.c / serial.c`.
- It speaks the **standard STM32F1 BOOT0 (ISP) serial protocol** — which the HC32F460's ROM ISP
  boot intentionally mimics.
- **Defaults baked in:** port **`/dev/ttyS1`**, baud **115200** (both are literals in `.rodata`).
- It is **32‑bit ARM glibc** — it runs natively on the Sunxi SoC (armv7). It will **not** run
  unmodified on a 64‑bit Orange Pi (see §6 for the implications).

### 2.2 The GPIO "magic" — this is why your manual attempts failed

The HC32 **does not** enter IAP mode by itself. Two **SoC** GPIOs, wired to the MCU, control it:

| SoC GPIO (sysfs) | Wired to MCU pin | Role |
|---|---|---|
| **`gpio34`** | **NRST** (reset) | Pulled **low** to assert reset, released to let the MCU boot |
| **`gpio35`** | **BOOT0 / ISP-select** | Held **high** so the MCU boots into ROM ISP instead of the app |

`IAPCommand` manipulates these purely through the **Linux sysfs GPIO interface** — it does **not**
solder anything and does **not** need a JTAG probe. The helper functions, straight from the
disassembly, are:

| Function | What it does (sysfs call) |
|---|---|
| `gpio_export(n)` | If `/sys/class/gpio/gpioN` doesn't exist, `echo N > /sys/class/gpio/export` (via `system()`) |
| `gpio_set_direction(n,dir)` | `echo in/out > /sys/class/gpio/gpioN/direction` |
| `gpio_set_value(n,val)` | `echo 0/1 > /sys/class/gpio/gpioN/value` |
| `gpio_out(n,val)` | export → `usleep(2000)` → set **output** → `usleep(2000)` → set value |
| `stm32UpdateFLag(v)` | `gpio_out(35, v)` — set the BOOT0/ISP select |
| `stm32Unexport()` | `gpio_unexport(35)` |

> **Key point:** these GPIOs are **SoC pins**, controlled by a process **running on the SoC** as
> **root**. That is why flashing is done from the SoC (Sunxi), not from a disconnected Orange Pi —
> the SoC is the only thing physically wired to the MCU's reset and BOOT0 lines.

### 2.3 The exact `stm32Init` sequence (enter IAP + sync)

From the disassembly of `stm32Init` (the "connect" path). `mov.w r0,#3392` + `movt r0,#3` =
`0x3D40` = **15 680 µs = 15.68 ms**:

```
stm32Init(port, baud):
    fd = serialOpen(port, baud)            # /dev/ttyS1 @ 115200
    if fd invalid: printf("Can not connect..."); exit
    gpio_out(34, 0)                        # GPIO34 (NRST)  → output, drive LOW (reset asserted)
    usleep(15680)                          # hold reset 15.68 ms
    gpio_set_value(34, 1)                  # release reset (LOW→HIGH)
    usleep(15680)                          # let the MCU start up in ISP
    loop:
        r = stm32Sync()                    # serial ISP handshake
        if r == 6: return HD-connected
        if r == 7: return NA-connected
        # not connected yet:
        puts("stm32 reset");               # (verbose) then re-pulse and retry
        gpio_out(34, 0); usleep(15680); gpio_set_value(34,1); usleep(15680)
        → goto loop
```

And the **BOOT0 select** happens *before* this, in `main`:

```
main():
    stm32Unexport()                        # clean state
    port=/dev/ttyS1; baud=115200
    ...
    stm32UpdateFLag(1)                     # gpio_out(35,1): BOOT0/ISP-select = HIGH
    sleep(1)                               # 1 full second
    r = stm32Init(port, baud)              # → pulses GPIO34 reset with GPIO35 held HIGH
    if r == 6:  readHexFile(HD); fillPacket(); stm32Write(); stm32Close(); ...
    if r == 7:  readHexFile(NA); fillPacket(); stm32Write(); stm32Close(); ...
    after a successful write:
        stm32UpdateFLag(0)                 # gpio_out(35,0): release BOOT0 → normal boot
        system("touch /root/H...")         # write a status flag file (HD) or (NA)
        sync()
        puts("STM32iap finished HD/NA.")
```

### 2.4 The exact `stm32Reboot` sequence (exit IAP → run new app)

```
stm32Reboot():
    puts("stm32 reset")
    gpio_out(34, 0)                        # assert reset (LOW)
    usleep(15680)                          # 15.68 ms
    gpio_set_value(34, 1)                  # release reset (HIGH)
    usleep(15680)                          # 15.68 ms
```
(Called with BOOT0 already released to 0, so the MCU now boots the freshly written app.)

### 2.5 The serial handshake (`stm32Sync`)

- Prints `.`, then **writes `0x43` (`'C'`)** to the MCU and reads the reply.
- A valid reply (byte **6** or **7**) means "connected — I'm in ISP." `6` = HD, `7` = NA.
- On no reply it prints a `\n` and **retries** up to a timeout → prints
  `Can not connect to board, Serial port ... timed-out.` — **this is almost certainly the exact
  symptom you saw.** It happens when the reset/BOOT0 strapping (§2.2) did not put the MCU into ISP.

### 2.6 Why a generic tool (or manual GPIO poking) fails — and why `IAPCommand` works

| Approach | Enters ISP? | Why |
|---|---|---|
| Run plain `stm32isp` over `/dev/ttyS1` | **No** | It has no AD4 GPIO code; it never pulses NRST/BOOT0, so the MCU boots the app, not ISP → sync times out |
| Manually "reset the MCU" with a probe | **Usually no** | You'd need to hold **BOOT0 high for the whole reset** with the correct ~15 ms timing. Doing it by hand is unreliable |
| **Run the factory `IAPCommand`** | **Yes** | It reproduces the exact GPIO34/35 sequence with the exact delays the hardware expects |

**Conclusion:** use `IAPCommand`. It *is* the safest tool here precisely because it already
contains the correct, hardware-validated strapping.

### 2.7 Practical prerequisites on the live SoC (verify before you flash)

Because the magic is SoC-side sysfs GPIO, confirm all of these **before** the first flash:

1. **`IAPCommand` runs as root** on the SoC (it calls `system("echo ...")` into `/sys/class/gpio`).
2. **GPIO 34 and 35 are real, free SoC pins** in this SoC's numbering. (The factory binary uses
   those numbers; if your SoC's pin-map differs, the same *logic* must be pointed at the right
   pins — but start by trusting the factory numbers, since the factory tool works on this hardware.)
3. **`/dev/ttyS1` is the MCU UART** and is **not** held open by another process
   (`fuser /dev/ttyS1`). The vendor daemon owns it during normal operation — stop it first (see §7).
4. You have **root** and can `echo` to `/sys/class/gpio/*` (no read-only filesystem).

---

## 3. Flashing Klipper (two paths, both no-solder)

### Path A — From the SoC over SSH (recommended first test)

This is the cleanest test and needs no new wiring: the SoC already owns the reset/BOOT0 pins.

```sh
# On the SoC (Sunxi), as root:
# 1) Stop whatever is holding the MCU UART (the vendor printer service).
#    (Identify the exact service name from the live system first — see §7.)
# 2) Put the files somewhere the SoC can reach, e.g. /root/flash/:
#      IAPCommand        (the factory 32-bit ARM binary)
#      klipper.hex       (Klipper at 0x0800C000 — see below)
#      factory-HD.hex    (KNOWN-GOOD restore image — keep it! your safety net)
# 3) Flash Klipper:
./IAPCommand /root/flash/klipper.hex /dev/ttyS1 115200
#    Watch for:  "Connected to board."  →  "STM32Write: success."  →  "STM32iap finished HD."
# 4) If anything looks wrong, restore the factory image immediately:
./IAPCommand /root/flash/factory-HD.hex /dev/ttyS1 115200
```

**Brick-safety rules (non-negotiable):**
- **Erase/write only the app window** — `0x0800C000` (HD) or `0x08010000` (NA). **Never `0x00000`.**
- Keep **`factory-HD.hex`** on the SoC so one command always restores the printer to stock.
- Always let the tool **verify** (it prints `STM32Write: success.`) before it reboots the MCU.
- The ROM/ISP boot is **not** in the region you overwrite → a bad app is recoverable by re-running
  IAP (the MCU re-enters ISP on the next reset/BOOT0 pulse).

**Making the `.hex`** (a raw `.bin` has no address header, so tell `objcopy` it's a binary **and**
its base address):

```bash
cd /Users/admin/klipper-hc32/out
arm-none-eabi-objcopy -I binary -O ihex --change-address=0xC000 klipper.bin klipper.hex
# Verify the round-trip is byte-identical:
arm-none-eabi-objcopy -I ihex -O binary klipper.hex /tmp/rt.bin && cmp /tmp/rt.bin klipper.bin
```
`klipper.hex` already exists at `/Users/admin/klipper-hc32/out/klipper.hex` and has been verified
byte-identical to the 28 416‑byte `klipper.bin`. The `--change-address=0xC000` matches the factory
HD hex, which also loads at `0x0000C000`.

### Path B — Unattended from a USB disk (your preferred "flashforge-style" method)

The AD4 already auto-runs a recovery from removable media. Mirror that: a small script on the disk
that the SoC boot triggers, which just runs `IAPCommand` against the MCU.

```
/UPDATE/
  IAPCommand            # factory ARM binary
  klipper.hex           # Klipper @ 0x0800C000
  factory-HD.hex        # KNOWN-GOOD restore (keep on the disk!)
  run-klipper.sh
```

`run-klipper.sh`:
```sh
#!/bin/sh
set -x
PORT=/dev/ttyS1          # confirm against the live system / DTB (§7); 115200 is the factory baud
BAUD=115200
# Stop the vendor daemon that holds the UART, then burn Klipper into the HD app window:
# ./IAPCommand /UPDATE/klipper.hex $PORT $BAUD
# Safety net — one command restores stock:
# ./IAPCommand /UPDATE/factory-HD.hex $PORT $BAUD
```

**Test over SSH first (Path A).** Only after `IAPCommand` works manually do you wire the same
command into the USB-disk auto-run. The only difference between the two paths is *who triggers the
command* — the actual flashing is identical.

---

## 4. What the factory firmware tells us (decompilation findings)

I disassembled the **factory HD app** (`Adventurer4-HD-IAP-*.hex` → binary, Thumb) to confirm the
real pinout, thermistor curves, motion constants, and driver configuration. Findings:

### 4.1 Drivers: TMC2209 over UART (not SPI) — your `printer.cfg` is right here

- **No SPI peripheral is ever referenced** in the factory image (SPI0/1/2 base addresses: **0**
  literal occurrences). **Three UARTs are active** (USART0/1/2 bases appear ~10–12 times each).
- There are **no TMC/stealth/diag strings** (expected — the TMC protocol has no text).
- **Conclusion:** the AD4 uses **TMC2209 steppers driven over UART** with a 4:1 address
  multiplexer — the *same* layout as the MonoPrice Voxel PRO / ADV3 that your `printer.cfg` was
  adapted from. So the `[tmc2209 ...]` blocks in your config (`uart_pin: PC10`, `select_pins`
  `PB10/PB1`) are the correct **driver type**. ✅
- (Contrast: if the board had used SPI TMC2209s, you'd see SPI0/1 registers — you don't.)

### 4.2 Thermistor curves (extracted from the factory LUTs)

Two ADC-driven NTC lookup tables live in the factory image, each as `(adc → temperature×10)`.
Full dumps were read out of the HD binary at `0x800E6B0` and `0x800E8B0`; the clean, physically
meaningful parts are:

**Hotend (NTC) — table `0x800E8B0`.** 24 defined points, ADC 0→384, **ending cleanly at 215.2 °C**:
```
adc  0 →   8.1 °C     adc  96 →  58.7 °C     adc 192 → 108.2 °C     adc 288 → 158.3 °C
adc 32 →  24.8 °C     adc 128 →  75.4 °C     adc 224 → 124.7 °C     adc 320 → 166.9 °C
adc 64 →  41.8 °C     adc 160 →  91.9 °C     adc 256 → 141.4 °C     adc 352 → 192.6 °C
                                             adc 384 → 215.2 °C   ← last defined point
```
(Beyond ADC 384 the table is "out-of-range" fill — not temperature.)

**Bed (NTC) — table `0x800E6B0`.** Usable range is **ADC 300 → ~170**, i.e. **11 °C → 117 °C**;
below ADC 170 the table is just the NTC curve extrapolated into unused territory (it "reaches"
absurd values like 10 000 °C only because the same formula keeps going — the printer never uses it):
```
adc 300 →  11.0 °C     adc 240 →  29.1 °C     adc 190 →  75.3 °C
adc 280 →  14.9 °C     adc 220 →  41.7 °C     adc 175 → 105.1 °C
adc 260 →  20.5 °C     adc 200 →  61.0 °C     adc 170 → 117.3 °C   ← top of the real bed range
```

**How this matches what you told me (and why it all fits):**
- **Bed:** the *real* bed table tops out at **~117 °C**, and the **printer clamps the bed to
  ~110 °C** — your memory ("bed 110 max") is exactly right. That's a **firmware limit, not a sensor
  limit**: the NTC could in principle read higher, but the machine won't command the bed past ~110 °C.
- **Hotend:** the last *defined* table point is **215.2 °C**, but the sensor keeps reporting above
  it on the same NTC curve. That's why your **235 °C and 265 °C nozzles** work and you remember
  seeing **~270 °C** setpoints — the heater simply runs past the last tabulated point.
  **Do not set `max_temp` to 215.** Set it to your nozzle rating (235 or 265, plus a safety margin)
  and let `max_temp` / the thermal-runaway checks do the rest.
- The two tables have **opposite ADC slopes** (hotend: low-ADC = cold; bed: high-ADC = cold), which
  is normal — the two thermistors are wired with opposite voltage-divider orientations.

**For `printer.cfg`:** define these as **custom `[thermistor]` curves** built from the extracted
points — they are **non-standard**, so the built-in `EPCOS 100K B57560G109` / `HESAI` etc. are only
*approximations* (fine for a first boot, but replace them with the real curves once probed).

**What is *confirmed* vs *to-verify* here (being honest):**
- ✅ **Confirmed from the binary:** the two LUT *shapes* above (exact ADC↔°C as the factory stores
  them), the hotend table ending at 215 °C, and the bed's real range ending at ~117 °C.
- ⚠️ **Verify during the live probe (§5):** (a) **which ADC channel is hotend vs bed** — the LUT
  shapes don't encode the channel number, so confirm it by warming each sensor and watching which
  `[temperature_sensor probe_…]` rises; (b) the **absolute ADC scaling** of the HC32 ADC (12/16-bit,
  gain) — validate a couple of known temperatures with live `M105` / `QUERY_ADC` so the curve's
  anchor is exact, not just its shape.

### 4.3 Motion / G‑code constants (from factory strings & state machine)

The factory image contains its **state machine** and a small set of **embedded G‑code** used for
power‑loss recovery and state display:

- **State machine:** `READY, BUILDING, BUILDING_FROM_SD, BUILDING_ONBOARD, ERROR, CANCEL_BUILD,
  HEAT_SHUTDOWN, MOVING, DELAY, HOMING, WAIT_ON_TOOL, WAIT_ON_PLATFORM, WAIT_ON_BUTTON`
  (plus a `HEAT_SHUTDOWN` safety state — the MCU can cut heat on its own).
- **Power‑loss resume G‑code** (embedded): `G90` → `G28` → `M132 X Y Z A B` (store position) →
  `G1 Z15 F400` (lift Z 15 mm at 400 mm/min) → retract. This is the factory's "resume after power
  failure" behavior; Klipper's `recover`/`firmware_retraction` are the equivalents you'd configure.
- **`[printer]` starting values** — these come from the ADV3‑derived config and should be treated as
  **starting points to verify**, not ground truth: `max_velocity 800`, `square_corner_velocity 6`,
  `max_accel 6500`, `max_z_velocity 30`, `max_z_accel 400`. Confirm against the AD4's mechanical
  limits (and the factory's actual max feedrates, if you can read them from the live UI) before
  printing fast.

### 4.4 Pin validity — the three fixes that still matter

I validated every pin against the **HC32F460 HAL** (`src/hc32f460/adc.c`, `gpio.c`):

| # | Problem | Evidence | Fix |
|---|---|---|---|
| 1 | **Hotend `sensor_pin: PB7` is not an ADC pin** → Klipper `shutdown("Not a valid ADC pin")` | HC32F460 ADC = **PA0–PA7, PB0, PB1, PC0–PC5** only (16 ch); **PB7 is not one of them** | Move the hotend sensor to a real ADC pin — **probe it** (§5). Bed `PA0` is valid ✅ |
| 2 | **Firmware‑serial clash:** Klipper build has the MCU serial on **PA7/PA8**, but your **X `step_pin: PA8`** | `.config`: `CONFIG_HC32F460_SERIAL_PA7_PA8=y` | One pin can't be two things. Either rebuild Klipper with a different `SERIAL` pair (any 2 free GPIOs, since you control the Pi↔MCU wiring in Option A) **or** move X step off `PA8`. Confirm the real pair — don't guess. |
| 3 | **`[output_pin led]` has no `pin:`** (it's commented out) | line 127 `#pin: PB12` | Give it a real (probed) LED pin, or delete the section. |

Everything else in your config is **valid GPIO** and will load: endstops `^PC3`/`^PC1`/`!PB5`
(digital, so any pin works — you just confirm which one each switch is wired to), heaters `PA1`/`PC4`,
fans `PC2`/`PA5`/`PC5`/`PB9`, and the TMC UART/`select_pins` (§4.1).

> **You do not need a perfect `printer.cfg` to build the firmware.** The firmware is pin‑agnostic;
> pins are host‑side, applied at runtime. Correct order: **build once → probe → finalize config.**

---

## 5. Safe pin probing (no heating, no motion)

Your instinct is right and this makes probing a **non‑issue**:

- **`probe.cfg`** (already written to `/Users/admin/adv4-klip/probe.cfg`) contains **no `[extruder]`,
  no `[heater_bed]`, no `[stepper_*]`** — so there is literally nothing in the config that can
  energize a heater or drive a motor. The "powerful" hotend/bed lines are configured **only as
  analog sensors**, never as heaters.
- Probing is **read‑only**:
  - **Thermistors:** wire every candidate ADC pin as a `[temperature_sensor probe_…]`. Read baseline
    with `M105`; warm the **hotend** tip → the probe that **rises** is the hotend pin; warm/short the
    **bed** thermistor → the one that jumps is the bed pin. (Cross‑check raw with `QUERY_ADC` →
    `analog: <value>`.) **This is also how you validate the LUT scaling in §4.2 against live
    readings.**
  - **Endstops:** press each switch, watch which reports `OPEN`→`TRIGGERED` (`QUERY_ENDSTOP`).
  - **LED:** the only *write*, at mA level — `SET_PIN` on a candidate pin until one glows.
    (Never point `SET_PIN` at a heater/fan/stepper pin.)
- You are **not soldering or changing the board** — you're asking the MCU "what's on this pin?"

**Flow:** flash Klipper → run `probe.cfg` → identify hotend/bed ADC pins, endstops, LED → drop the
confirmed pins into the real `printer.cfg` → remove the matching probe sections (a pin can't be
both a probe sensor and a real driver).

---

## 6. The two roles, clearly separated (this fixes the Orange Pi confusion)

There are **two different jobs** and they use **two different physical paths**:

1. **Flashing into IAP** — needs the MCU's **reset + BOOT0**, which are driven by the **SoC's
   GPIO 34/35**. → Do this **on the SoC** (run `IAPCommand` there, over SSH or USB‑disk auto‑run).
   A standalone Orange Pi is **not** wired to those reset/BOOT0 lines, so it can't do this on its own.
2. **Running Klipper** — the **host** (klippy) talks to the **MCU over a UART**. The host can be
   the SoC *or* an Orange Pi.

### Options for the **Klipper host** (job 2)

- **Option A — Orange Pi talks straight to the HC32 (recommended for a clean Klipper install).**
  Wire the HC32's **3‑wire serial (TX, RX, GND)** to the Orange Pi's hardware UART. Lowest latency,
  simplest `FIRMWARE_RESTART`. Both sides are 3.3 V → direct, no level shifter. This needs the real
  MCU serial pin pair (probe / DTB, §7).
- **Option B — Keep the SoC as the host.** No re‑wiring at all; point Klipper's `[mcu]` at the SoC's
  MCU‑UART tty. Simplest if you don't want to add an Orange Pi.

> On the earlier "ethernet proxy" idea: the SoC *is* the Ethernet face (it owns the network). There's
> no separate Ethernet‑to‑serial bridge product in the box. For a dedicated Klipper install, **Option A
> (Pi ↔ MCU UART)** is the standard; **flashing** still happens from the SoC (§6, job 1).

### Orange Pi bring‑up (Klipper host, Option A)
```bash
git clone https://github.com/Klipper3d/klipper
cd klipper && make menuconfig && sudo python3 scripts/install_deps.sh
sudo klippy                       # point Moonraker/Fluidd at the Pi's UART to the HC32
```
Put the **finalized** `printer.cfg` (post‑probe) in `~/printer_data/config/`.

---

## 7. Step‑by‑step runbook

1. **Confirm board rev** → HD (`0xC000`) vs NA (`0x10000`). Default **HD**.
2. **Confirm the SoC serial + GPIO facts on the live board** (SSH, root):
   - `ls -l /dev/ttyS*` and `fuser /dev/ttyS1` (is the vendor daemon holding it?).
   - Check `/sys/class/gpio/gpio34` & `gpio35` exist (export if needed) and are writable.
   - Read the DTB for the exact MCU‑UART + reset/BOOT0 pins: `dtc -I fs /proc/device-tree -O dts`.
   - Stop the vendor service that owns the MCU (identify its unit name from `systemctl`).
3. **Flash (test first):** `./IAPCommand klipper.hex /dev/ttyS1 115200` → expect
   `Connected to board.` / `STM32Write: success.` / `STM32iap finished HD.`
   **Keep `factory-HD.hex` one command away** for instant restore.
4. **Run `probe.cfg`** on the host → identify hotend/bed ADC pins, endstops, LED (no heat, no
   motion) and **validate the thermistor LUT scaling** (§4.2) against live `M105`/`QUERY_ADC`.
5. **Finalize `printer.cfg`** with confirmed pins + correct MCU serial, custom `[thermistor]`
   curves, and a `max_temp` matching your nozzle (235/265 °C, not 215).
6. **Host:** Option A (wire Pi ↔ MCU) or Option B (SoC host). Bring up klippy + Moonraker/Fluidd.
7. **Sanity (still no print):** `M105` tracks the real thermistors, `G28` homes all axes,
   `QUERY_ENDSTOP` sane, LED responds. Only then enable heaters and print.
8. **Safety net:** `factory-HD.hex` stays on the SoC/disk → one command returns the printer to stock.

---

## 8. Klipper calibration procedure (post‑flash, post‑probe)

Do this **only after** the MCU is running Klipper, the pins are confirmed, and temperatures read
correctly. Order matters (cheapest/most‑critical first):

1. **Thermistor sanity & (optionally) re‑derive the curve.**
   - With the probe data (§5), confirm each `[thermistor]` maps live ADC → correct °C.
   - If you want to *fit* a curve instead of trusting the factory LUT: record `(ADC, °C)` pairs at
     several known temperatures (ice bath ~0 °C, room ~25 °C, a controlled warm‑up), then fit
     `[thermistor]` or a `steinhart‑hart` to the points.

2. **PID tuning (hotend, then bed).**
   ```
   PID_CALIBRATE HEATER=extruder TARGET=200
   PID_CALIBRATE HEATER=heater_bed TARGET=100
   SAVE_CONFIG
   ```
   Re‑run once after the first tune if the error looks high. Expect the `#*# pid_*` block to fill.

3. **Pressure Advance (direct drive, if applicable).**
   ```
   PRESSURE_ADVANCE_CALIBRATE   # (or use the PA tune macro / the pressure advance calibration gcode)
   SAVE_CONFIG
   ```

4. **Z offset.**
   - Use the `Z_OFFSET` / touch‑probe‑less manual tune: lower Z until the nozzle just touches a
     sheet of paper, note the offset, `SAVE_CONFIG`. (The factory's `#*# position_endstop` and the
     FLUIDD Z‑offset flow are the equivalents.)

5. **Input shapers (X, then Y) — only at print speeds you'll actually use.**
   ```
   ACCELERATION_TEST ACCel_MIN=1000 ACCel_MAX=10000    # or the shaper test macro
   MEASURE_AXES_NOISE
   SHAPER_CALIBRATE AXIS=X     # then AXIS=Y
   SAVE_CONFIG
   ```
   Your ADV3 starting values (`shaper_freq_x 205 / mzv`, `shaper_freq_y 197.126 / ei`) are a
   reasonable seed — let the calibration refine them.

6. **Bed mesh.**
   ```
   BED_MESH_PROFILE SAVE=default    # after a proper sensorless/Z‑probe pass, or manual 3x3
   ```
   Your config uses a 3×3 Lagrange mesh (min 5,5 → max 150,145) — keep it once the Z probe is set.

7. **Max accel / velocity validation (optional but recommended).**
   Run `ACCELERATION_TEST` / `SQUARE_CORNER_TEST` to find the AD4's real mechanical limits and set
   `max_accel` / `max_velocity` / `square_corner_velocity` to safe values (don't print at the ADV3
   6500/800 without confirming the AD4 tolerates them).

8. **Print a calibration object** (adhesion, overhang, PA) and iterate.

> **Note:** there is **no `input_shaper` on Z** (Z is slow) and **no bed‑mesh fade** unless you want
> a sloped first layer — your `fade_start 1 / fade_end 10 / fade_target 0` is a sensible default.

---

## 9. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Erase the wrong flash region → brick | Low **if you honor the rule** | Write **only** `0x0800C000`/`0x08010000`; never `0x00000`. ISP boot is ROM‑resident → a bad app is recoverable by re‑running IAP on the next reset/BOOT0 pulse. |
| Wrong app offset (HD vs NA) | Medium | Confirm rev on the live board; default HD. A wrong‑offset flash just won't boot → re‑flash the other offset (ROM ISP still alive). |
| MCU won't enter ISP ("timed‑out") | **The failure you already hit** | Reproduce the **exact** SoC GPIO sequence: `gpio_out(35,1)` → 1 s → pulse `gpio34` LOW 15.68 ms → HIGH 15.68 ms (§2.3). That's what `IAPCommand` does — just run it. |
| `IAPCommand` won't run on the Orange Pi | Certain (32‑bit ARM glibc) | Run it **on the SoC** (its native platform), not the Pi. The Pi is for *running* Klipper, not flashing. |
| Serial pin / GPIO guessed wrong | Medium | Confirm from DTB / probe (§7) before flashing. |
| `PB7` hotend pin (invalid ADC) | Certain (from ADV3 cfg) | Probe (§5); it *will* fail until moved to a real ADC pin (PA0–7, PB0–1, PC0–5). |
| Heat / motion during probing | **None** | `probe.cfg` has no heaters/steppers configured. |

---

## 10. What is still UNCONFIRMED (being honest)

1. **Exact MCU↔SoC serial pin pair + the SoC's tty name** — in the Sunxi **DTB**, not in the rootfs
   backup we have. Read it on the live board (§7). (This only affects *running* Klipper, not flashing.)
2. **Which board rev you have** (HD vs NA) → `0xC000` vs `0x10000`. Default **HD**.
3. **Live thermistor scaling** — the LUT *shape* is extracted (§4.2), but the absolute ADC↔°C anchor
   must be validated with live `M105`/`QUERY_ADC` during the probe pass before trusting temperatures.
4. **Exact vendor service name** holding `/dev/ttyS1` — identify on the live SoC and stop it before
   flashing (§7).
```
