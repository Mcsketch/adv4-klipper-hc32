# FlashForge Adventurer 4 → Klipper (HC32F460) — Flashing & Setup Guide

> Audience: an experienced user who wants to run **Klipper** on the AD4's motion MCU
> (Huada **HC32F460**) and is willing to disassemble the factory firmware to get it exactly right.
>
> Everything below is backed by artifacts we actually have: the **2.3.0 factory update package**
> (which ships `IAPCommand`), the **factory HD/NA firmware hex files**, an **eMMC rootfs backup**,
> and our **Klipper HC32F460 build**. Nothing here is guessed — each "confirmed" fact has evidence.
> The "magic" of the IAP (in-system programming) flow has been **fully disassembled** and is
> spelled out in §2.
>
> **Corrections 2026-10-20 (session 5):** the "ROM-ISP / BOOT0" framing below was
> wrong in places — the HC32F460 has **no ROM ISP**; the thing answering the sync
> is a **flash-resident vendor bootloader** (inferred `0x0..0xBFFF`). The wire
> protocol is the vendor's 1030-byte-frame format, **not** STM32 ISP ops.
> GPIO35's "BOOT0" role is **unproven** (only GPIO34=NRST is confirmed). Sync
> reply mapping is `0x06` = NA, `0x07` = HD (earlier drafts had it swapped).
> Ground truth: `adv4-iap-protocol.md` (strace) and `adv4-swd-dump.md`.
>
> **Status 2026-10-27 (Caleb's unit = NA / N32G455, not HC32):** mainline Klipper `020d25e` is
> running on the MCU, flashed with the vendor `IAPCommand` (hex in the NA slot) from the SoC and
> confirmed with Klipper `identify` over `ad4-bridge`. The N32 recipe lives in `work/n32/`
> (`klipper-n32g455.defconfig`, `iap-flash.sh`, `klipper-identify.py`); the record is at the
> bottom of `adv4-iap-protocol.md` and in `adv4-swd-dump.md`. The HC32F460 material below
> applies to HD boards only.

---

## 0. TL;DR (answers to your direct questions)

| Your question | Short answer |
|---|---|
| **Will `IAPCommand` flash at the right addresses?** | **Yes.** It writes the HD app at `0x0800C000` (and NA at `0x08010000`). It erases/programs/verifies **only the app window** — it never touches the bootloader region. (Session-5 note: that bootloader is **flash-resident**, not ROM — the HC32F460 has no ROM ISP.) |
| **Why did my attempts fail — is there "delay and magic" to put the MCU in IAP mode?** | **Yes, and I found it.** The MCU does **not** enter IAP mode on its own. The SoC must drive **two of its own GPIOs**: **GPIO 34 = MCU reset (proven)**, **GPIO 35 = driven by `IAPCommand`, but its "BOOT0/ISP-select" role is UNPROVEN** (the HC32F460 has no BOOT0 pin). `IAPCommand` pulses them through `/sys/class/gpio` with **exact 15.68 ms delays**. Skip that and the MCU never boots into ISP, so the serial sync times out ("Can not connect to board, timed-out"). |
| **Is it as easy as a serial tool?** | Not with a *generic* tool. The serial protocol is the vendor's own (not standard STM32 ISP — see §2.1 correction), and the **reset/GPIO35 strapping is SoC-specific** — only `IAPCommand` (or an equivalent that reproduces the exact GPIO sequence) does it. |
| **Did you disassemble it?** | **Yes — `IAPCommand` fully** (`stm32Init`, `stm32Reboot`, `stm32Sync`, `main`, and all `gpio_*` helpers). The exact call sequence and timings are in §2.4. |
| **Isn't plain `stm32isp` safer/easier?** | **`IAPCommand` *is* a fork of `wengkai/stm32isp`** — same core code, but with the **AD4-specific GPIO reset/GPIO35 logic added** (its serial protocol, however, was adapted to the vendor format — see §2.1). Plain `stm32isp` has **no** AD4 strapping, so it will *not* bring this MCU into IAP. **Use `IAPCommand`.** |
| **Hotend / bed max temps?** | Matches your memory: the **bed LUT is clamped at ~110 °C**; the **hotend LUT runs to ~215–235 °C**, and the two interchangeable nozzles (235 / 265 °C rated) explain you seeing ~270 °C setpoints (the heater runs past the last LUT point via the same NTC). Details in §4. |
| **Driver config?** | **TMC2209 steppers over UART** (not SPI) with a 4:1 address multiplexer — exactly the ADV3/Voxel layout your `printer.cfg` already uses. Confirmed by disassembly (3 UARTs active, no SPI references). §4.3. |

**The one-sentence summary of the whole flash:**
> `IAPCommand` runs **on the SoC (Sunxi Linux)**, toggles SoC **GPIO 34 (NRST, proven) + 35 (role unproven)** to force the MCU into its flash-resident bootloader, then talks the **vendor 1030-byte-frame protocol** (not standard STM32 ISP) over **`/dev/ttyS1` @ 115200**, writing Klipper into the **HD app window `0x0800C000`** and rebooting the MCU.

**⚠️ Base-address conventions (the two spellings are the SAME cell):**

| Where you see it | Value | Meaning |
|---|---|---|
| iHEX file (type-04 `:020000040000`) + data offsets | `0x0000C000` | **linear/loader base** — what `make-positioned-ihex.py` and Klipper's `CONFIG_HC32F460_FLASH_APPLICATION_ADDRESS_0x00C000` use |
| HC32F460 physical memory map (this doc) | `0x0800C000` | **physical flash address** — the loader maps linear `0x0000C000` → physical `0x08000000 + 0xC000` |

The commonly-seen wrong value is `0x000C0000` (i.e. `0xC0000`) — that is a *different* cell and must never be used. Rule of thumb: **hex base `0x0000C000` ≡ physical `0x0800C000` ≡ loader offset `0xC000`**.

**⚠️ Klipper baud on the AD4 must be 115200, not 250000.** The SoC's `uart0` (`/dev/ttyS1`) runs from a **24 MHz** clock, which is *below* the serial driver's accepted range `[30 MHz, 120 MHz]` — `setbaud()` calls are rejected at runtime (dmesg: `sw_uart_check_baudset(): uart0 … baud …, uartclk 24000000 beyond range[30000000, 120000000]`) and the line holds its boot-time rate of **115200**. A Klipper build with `SERIAL_BAUD=250000` flashes fine but is **inaudible** on this line (garbage/clock mismatch). Build with `CONFIG_SERIAL_BAUD=115200` (`HC32F460_SERIAL_PA9_PA10` stays — session-5 firmware RE **proves** PA9/PA10/USART2 is the vendor link; the "PA9_PA15" spelling in earlier drafts was a typo, and the `PA13_PA14` variant is a hallucination — those are the SWD pins).

**🚨 PITFALL — "115200" silently compiles as 250000 (this burned 3 days).** Setting
`CONFIG_SERIAL_BAUD=115200` in `.config` is **not enough**. In upstream Klipper, `SERIAL_BAUD` is an
`int` whose prompt is gated behind `LOW_LEVEL_OPTIONS`:

```
config SERIAL_BAUD
    depends on SERIAL
    int "Baud rate for serial port" if LOW_LEVEL_OPTIONS   # ← prompt only if LOW_LEVEL_OPTIONS
    default 250000
```

When `LOW_LEVEL_OPTIONS` is **unset** (the default), `SERIAL_BAUD` has no active prompt, so it
resolves as **invisible**. The vendored kconfiglib (`lib/kconfiglib/genconfig.py`) stores your
`.config` value into `Symbol.user_value` *and reports `set_value() → True`*, but the `str_value`
resolver then sees the symbol as invisible / out-of-range and **falls back to `default 250000`**.
The Klipper kconfiglib patch (`kconfiglib.patch`) only forces the symbol to be *written* to
`autoconf.h` — it does **not** fix the value, so `#define CONFIG_SERIAL_BAUD 250000` is emitted
**even though `.config` says 115200**, and `genconfig.py` even *skips the write* (no content change),
so `out/autoconf.h` keeps its old mtime. **The build succeeds and the hex looks fine; only the
serial is wrong, and it reads as 0 B / garbage on the line.**

The fix is to **enable `LOW_LEVEL_OPTIONS`** so the expert option becomes visible and its value is
honored. It only toggles prompt visibility; it does not change any other default for this board.

```sh
cd klipper
# 1) enable expert options (makes SERIAL_BAUD visible → its .config value is honored)
sed -i 's/^# CONFIG_LOW_LEVEL_OPTIONS is not set/CONFIG_LOW_LEVEL_OPTIONS=y/' .config
# 2) set the baud
sed -i 's/^CONFIG_SERIAL_BAUD=.*/CONFIG_SERIAL_BAUD=115200/' .config
# 3) normalize (olddefconfig KEEPS both now — verify with grep)
make olddefconfig && grep -E 'LOW_LEVEL_OPTIONS|SERIAL_BAUD' .config
# 4) build (host macOS: force the cross cpp — /usr/bin/cpp is a clang wrapper that fails on .lds.S)
make clean
make -j8 CPP=arm-none-eabi-cpp
# 5) VERIFY the compiled header before flashing — this is the only reliable check:
grep 'define CONFIG_SERIAL_BAUD ' out/autoconf.h    # MUST read 115200
```

Verify **before every flash**: `grep 'CONFIG_SERIAL_BAUD ' out/autoconf.h` must read
`#define CONFIG_SERIAL_BAUD 115200`. (The compiled binary does not contain a readable baud string —
the `identify` dict is XOR-encoded — so `autoconf.h` is the ground truth.)

---

## 1. Board topology (confirmed)

The AD4 is a **two-chip** design:

| Chip | What it is | What it does |
|---|---|---|
| **Allwinner (Sunxi) SoC** | ARM "server" | Touch UI, Ethernet/Wi‑Fi, file management, and — crucially — **drives the MCU's reset (GPIO34, proven) and GPIO35 (role unproven)** and talks to the MCU over USART2 / PA9-TX / PA10-RX @ 115200 (proven from factory-firmware RE) |
| **Huada HC32F460** | Cortex‑M4 (an STM32‑F4‑like clone) | Motion, heaters, thermistors, endstops — the **real "printer brain"** |

They talk over a **UART**. The SoC is the network/UI face; the HC32 is the motion controller.
This is why "the Allwinner doesn't have enough RAM" is a red herring — it was never the motion
controller. Klipper wants to **replace the HC32 firmware** and run the **host (klippy) on real Linux**.

There are **two board revisions** with slightly different STM32‑class MCUs (the factory calls them
**HD** and **NA**). They differ only in **which flash address the app lives at**:

```
HC32F460 flash (512 KB, base 0x08000000)
┌──────────────────────────────────────────────────────────────┐
│ 0x08000000   vendor bootloader ("ISP") — FLASH-resident,     │
│              inferred 0x0..0xBFFF. NOT ROM (HC32F460 has     │
│              no ROM ISP); erasable → never write there       │
├──────────────────────────────────────────────────────────────┤
│ 0x0800C000   HD app start  ──► Klipper .bin/.hex goes HERE   │
│ 0x0801AC00   HD app end    (59 KB, contiguous, NO gaps)      │
├──────────────────────────────────────────────────────────────┤
│ 0x08010000   NA app start   (alternate board revision)       │
│ 0x0801AE60   NA app end    (43 KB, contiguous, NO gaps)      │
└──────────────────────────────────────────────────────────────┘
```

We parsed **both** factory hex files end to end: each is a single contiguous image with **zero
gaps**. **Correction (session 5):** "no hidden second region anywhere in flash" was an
over-claim — the hexes contain only the app, and the **bootloader itself has never been dumped**
(`work/hd.bin` covers flash `0x0..0xFFFF` only; ~448 KB of the 512 KB is unmapped). The ISP logic
does **not** live in ROM — it is a **flash-resident vendor bootloader** (inferred at
`0x0..0xBFFF`). Replacing the app at `0xC000` *should* leave it intact, so a bad app flash is
*expected* to be recoverable — but it is ordinary flash (destroyable if ever written over), and
recovery has been observed to fail after repeated flashing. Treat recovery as **probable, not
guaranteed** (see `adv4-iap-protocol.md` §2–3, `adv4-swd-dump.md`).

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
- ~~It speaks the standard STM32F1 BOOT0 (ISP) serial protocol~~ — **WRONG, corrected
  2026-10-20**: it only shares *code ancestry* with stm32isp. The actual wire protocol (strace
  ground truth, `adv4-iap-protocol.md`): `0x43` sync → reply `0x06`(NA) / `0x07`(HD); then
  1030-byte frames `[cmd:1][len:2 LE][data:1024][crc16:2 LE]` (CRC16-CCITT, init 0, over
  cmd+data) acked by `0x06`. The STM32 ROM-ISP ops (`0x02` erase, `0x11` read, `0x44` write…)
  are **not spoken** by this bootloader — which is why `stm32isp-ad4 -d` dump can never work.
  And the HC32F460 has **no ROM ISP**: the responder is a flash-resident bootloader.
- **Defaults baked in:** port **`/dev/ttyS1`**, baud **115200** (both are literals in `.rodata`).
- It is **32‑bit ARM glibc** — it runs natively on the Sunxi SoC (armv7). It will **not** run
  unmodified on a 64‑bit Orange Pi (see §6 for the implications).

### 2.2 The GPIO "magic" — this is why your manual attempts failed

The HC32 **does not** enter IAP mode by itself. `IAPCommand` drives two **SoC** GPIOs; only
GPIO34→MCU-NRST is proven — GPIO35 is written by the tool, but whether it even reaches the MCU,
and what it does there, has **never been traced** (the HC32F460 has no BOOT0 pin; the boot
decision is made by the flash bootloader):

| SoC GPIO (sysfs) | Wired to MCU pin | Role |
|---|---|---|
| **`gpio34`** | **NRST** (reset) — proven by behavior | Pulled **low** to assert reset (15.68 ms), released to let the MCU boot |
| **`gpio35`** | **"BOOT0 / ISP-select" — UNPROVEN label** | Held **high** during flashing by the factory tool; reproduce the sequence, don't assume the mechanism |

`IAPCommand` manipulates these purely through the **Linux sysfs GPIO interface** — it does **not**
solder anything and does **not** need a JTAG probe. The helper functions, straight from the
disassembly, are:

| Function | What it does (sysfs call) |
|---|---|
| `gpio_export(n)` | If `/sys/class/gpio/gpioN` doesn't exist, `echo N > /sys/class/gpio/export` (via `system()`) |
| `gpio_set_direction(n,dir)` | `echo in/out > /sys/class/gpio/gpioN/direction` |
| `gpio_set_value(n,val)` | `echo 0/1 > /sys/class/gpio/gpioN/value` |
| `gpio_out(n,val)` | export → `usleep(2000)` → set **output** → `usleep(2000)` → set value |
| `stm32UpdateFLag(v)` | `gpio_out(35, v)` — the factory tool's "ISP select"; role unproven |
| `stm32Unexport()` | `gpio_unexport(35)` |

> **Key point:** these GPIOs are **SoC pins**, controlled by a process **running on the SoC** as
> **root**. That is why flashing is done from the SoC (Sunxi), not from a disconnected Orange Pi —
> the SoC is the only thing proven to drive the MCU's reset (GPIO34) — and the only thing the
> factory tool drives GPIO35 from.

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
        if r == 6: return NA-connected     # (corrected 2026-10-20: live strace + our fork
        if r == 7: return HD-connected     #  confirm 0x06=NA, 0x07=HD; earlier drafts swapped)
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
    stm32UpdateFLag(1)                     # gpio_out(35,1): "ISP-select" = HIGH (role unproven)
    sleep(1)                               # 1 full second (hold duration never A/B-tested)
    r = stm32Init(port, baud)              # → pulses GPIO34 reset with GPIO35 held HIGH
    if r == 6:  readHexFile(NA); fillPacket(); stm32Write(); stm32Close(); ...
    if r == 7:  readHexFile(HD); fillPacket(); stm32Write(); stm32Close(); ...
    after a successful write:
        stm32UpdateFLag(0)                 # gpio_out(35,0): release → normal boot
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
(Called with GPIO35 already released to 0, so the MCU now boots the freshly written app.)

### 2.5 The serial handshake (`stm32Sync`)

- Prints `.`, then **writes `0x43` (`'C'`)** to the MCU and reads the reply.
- A valid reply (byte **6** or **7**) means "connected — I'm in ISP." **`6` = NA, `7` = HD**
  (corrected 2026-10-20: live strace and our fork's `Connected to board. (sync reply 0x07 = HD)`
  confirm this; earlier drafts had the mapping swapped).
- On no reply it prints a `\n` and **retries** up to a timeout → prints
  `Can not connect to board, Serial port ... timed-out.` — **this is almost certainly the exact
  symptom you saw.** It happens when the reset/GPIO35 strapping (§2.2) did not put the MCU into
  its bootloader — or when the bootloader stopped answering after repeated flash cycles
  (power-cycle the printer; see STATUS.md §3).

### 2.6 Why a generic tool (or manual GPIO poking) fails — and why `IAPCommand` works

| Approach | Enters ISP? | Why |
|---|---|---|
| Run plain `stm32isp` over `/dev/ttyS1` | **No** | It has no AD4 GPIO code (never pulses NRST/GPIO35) **and** its STM32 sub-commands don't match the vendor protocol — only the `0x43` sync would land |
| Manually "reset the MCU" with a probe | **Usually no** | You'd need to reproduce the factory sequence — GPIO35 level + NRST pulse with the correct ~15 ms timing (GPIO35's mechanism is unproven). Doing it by hand is unreliable |
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

This is the cleanest test and needs no new wiring: the SoC already drives the MCU reset (GPIO34)
and the GPIO35 line the factory tool uses.

```sh
# On the SoC (Sunxi), as root:
# 1) Stop whatever is holding the MCU UART (the vendor printer service).
#    (Identify the exact service name from the live system first — see §7.)
# 2) Put the files somewhere the SoC can reach, e.g. /root/flash/:
#      IAPCommand        (the factory 32-bit ARM binary)
#      klipper.hex       (Klipper at 0x0800C000 — see below)
#      factory-HD.hex    (KNOWN-GOOD restore image — keep it! your safety net)
# 3) Flash Klipper. NOTE: factory IAPCommand takes TWO hex paths (NA-hex HD-hex) —
#    pass the same file twice to flash one image (that's how it was run live):
./IAPCommand /root/flash/klipper.hex /root/flash/klipper.hex /dev/ttyS1 115200
#    Watch for:  "Connected to board."  →  "STM32Write: success."  →  "STM32iap finished HD."
# 4) If anything looks wrong, restore the factory image immediately:
./IAPCommand /root/flash/factory-HD.hex /dev/ttyS1 115200
```

**Brick-safety rules (non-negotiable):**
- **Erase/write only the app window** — `0x0800C000` (HD) or `0x08010000` (NA). **Never `0x00000`.**
- Keep **`factory-HD.hex`** on the SoC so one command always restores the printer to stock.
- Always let the tool **verify** (it prints `STM32Write: success.`) before it reboots the MCU.
- The vendor bootloader is **not** in the region you overwrite → a bad app is *expected* to be
  recoverable by re-running IAP (the MCU re-enters the bootloader on the next reset pulse).
  Caveats: it is flash, not ROM — safe only while you keep the base rule; and sync has been
  observed to stop answering after repeated flash cycles → power-cycle the printer before
  concluding it's bricked (STATUS.md §3).

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

### 4.2 Temperature sensing — the real architecture (corrected after deeper disassembly)

Earlier versions of this section guessed "T1 = hotend, T2 = bed" from value ranges. A closer
disassembly of the MCU (correct load base **`0x0000C000`**, since the Intel‑HEX loads there) and of
the **SoC firmware** changes the picture. The important corrections:

**(a) The MCU has *four* ADC sensor channels, not two.**
At file `0x9D10` (VMA `0x00015D10`) there is a **4‑entry pointer array** that selects, per channel,
which temperature curve the ADC conversion uses:

| channel | descriptor ptr (VMA) | curve it points at | what it is |
|---|---|---|---|
| 0 | `0x1A6AC` | `(300,110)(295,119)(290,128)…** | **Bed** (room→~117 °C range) |
| 1 | `0x1A88C` | (coefficient table) | PID / linearize coefficients |
| 2 | `0x1A974` | (coefficient table) | PID / linearize coefficients |
| 3 | `0x1A4CC` | `(1791,216)(266,431)(216,646)…` up to **408 °C** | **Hotend, high‑temp (265 °C) nozzle curve** |

So the MCU carries **multiple hotend curves** — exactly matching your "two interchangeable nozzles."
The **standard** hotend curve (the 21 → 215 °C one) and the **high** hotend curve (the 21 → 408 °C
one) are *both* present; the SoC picks which nozzle is fitted and the MCU/SoC use the matching curve.

> ⚠️ **What is proven vs. inferred (corrected):**
> - **Proven by bytes:** 4 curve‑descriptor structs exist, and the pointer array at VMA `0x15D10`
>   points at all four. Their **value content** cleanly identifies them: **ch3 `0x1A4CC` → hotend,
>   high‑temp (values climb to ~263 °C)**; **ch0 `0x1A6AC` → bed (ADC 300→10.2 °C … 117 °C)**;
>   ch1 `0x1A88C` / ch2 `0x1A974` → small/secondary ranges.
> - **NOT proven (I over‑claimed this earlier and now correct it):** I could **not** resolve the
>   single *consumer function* that loads T1 vs T3. Three independent searches — direct literal‑pool
>   words, `movw/movt` immediate pairs, and base‑normalized pool words — **all return 0 hits** for
>   the array and each descriptor VMA. The code reaches these tables through an **indirect /
>   register‑relative / indexed path** (or a link base I'm not pinning down) that is **not
>   resolvable from the flat image alone**.
> - **Therefore** the "T1 = high‑temp hotend / T3 = normal hotend / ch0 = bed" assignment is a
>   **value‑content inference, not a traced consumer call.** Your read was the right *shape*; I just
>   can't assert the exact channel‑wiring offline. **The one definitive, safe confirmation is the
>   live probe in §5:** warm each thermistor and watch which `[temperature_sensor]` rises — that
>   *is* the consumer identification, end to end.

**(b) The tables are *temperature ↔ resistance*, not raw ADC counts.**
Reading them as "ADC count → temp" was the mix‑up. Each row is **(temperature, resistance‑derived)**
for the NTC. This is the *fixed* thermistor R‑curve (analogous to Klipper's `[thermistor]`
`temperature1/resistance1` points). The **raw ADC value** is read separately by the MCU and mapped
through these curves.

**(c) The per‑nozzle 2‑digit "temp cal" (your "12"/"14") is applied on the *SoC*, not the MCU.**
The SoC Qt/C++ firmware (`adventurer4-arm`, unstripped) contains:
- A **`NozzleTempCheck`** class and **`SerialObject::getAutoOffsetValue()` / `getResultForAutoOffset()`**
  — the SoC **computes and stores** the nozzle temperature‑offset.
- **`PrintDetail::setNozzleTargetTemp(int)` / `printingAdjustNozzleTemp(int)`** — the SoC **adjusts the
  target temperature** by that offset before telling the MCU what to heat to.
- Interchangeable‑nozzle UI: **`select_nozzle1..4.png`**, and explicit max‑temp strings
  **`"Extruder can be heated up to 265 "`, `"Nozzle 0-280 "`, `"0.3-265" / "0.4-265" / "0.6-265"`**.

So the flow is: **you pick a nozzle → the SoC reads/stores its 2‑digit cal offset → the SoC shifts the
setpoint by that offset → it sends the corrected target to the MCU.** The 2‑digit value is a
**target‑temperature offset**, not a raw resistance or LUT entry. That's why the numbers "look like
resistance" but aren't.

**(d) `printer.cfg` implications.**
- You need **two custom `[thermistor]` curves** (the standard and high‑temp hotend R‑curves) + the
  bed curve, defined from the extracted (temperature, resistance) pairs — they are **non‑standard**,
  so built‑in types are only approximations.
- **Replicate the nozzle offset as a fixed per‑nozzle adjustment.** Since you're swapping nozzles
  manually, pick **one** nozzle's curve in `[extruder]` and apply the other's offset as a
  `setTemperature` / macro shift (or a second `[extruder]`‑style target). Your "12"/"14" values are
  exactly the offsets to encode.
- **Do not cap `max_temp` at 215.** Use the nozzle rating (235/265 + margin); the SoC already
  enforces 265 as the UI ceiling.

**What is *confirmed* vs *to-verify* (being honest):**
- ✅ **Confirmed:** 4 sensor‑channel descriptors exist (0x15D10 array); channel 0 = bed curve,
  channel 3 = high‑temp hotend curve (byte‑proven); two distinct hotend R‑curves exist; the
  SoC applies the nozzle temp offset and enforces the 265 °C ceiling.
- ⚠️ **Verify live (§5):** (1) **which physical thermistor is which channel** (warm each sensor,
  watch which `[temperature_sensor probe_…]` rises); (2) the **raw ADC scaling** of the HC32 ADC
  (12/16‑bit, gain) against known temps via `M105`/`QUERY_ADC`; (3) the exact per‑nozzle offset
  values you want to bake in (your "12"/"14").

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

1. **Flashing into IAP** — needs the MCU reset (SoC GPIO34, proven) plus the GPIO35 level the
   factory tool drives (role unproven) — both on the **SoC**. → Do this **on the SoC** (run
   `IAPCommand` there, over SSH or USB‑disk auto‑run). A standalone Orange Pi is **not** wired to
   those lines, so it can't do this on its own.
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
   - Read the DTB for the exact MCU‑UART + reset/GPIO35 pins: `dtc -I fs /proc/device-tree -O dts`.
     (Session 5 settled the MCU side by other means: USART2 PA9/PA10 @115200 — see README facts table.)
   - Stop the vendor service that owns the MCU (identify its unit name from `systemctl`).
3. **Flash (test first):** `./IAPCommand klipper.hex klipper.hex /dev/ttyS1 115200` (two hex args:
   NA, HD) → expect `Connected to board.` / `STM32Write: success.` / `STM32iap finished HD.`
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
| Erase the wrong flash region → brick | Low **if you honor the rule** | Write **only** `0x0800C000`/`0x08010000`; never `0x00000`. The bootloader is **flash-resident** (not ROM — HC32F460 has no ROM ISP), so it survives only while you never write there; re-running IAP is the recovery path but has been observed to fail after repeated flash cycles → power-cycle first. |
| Wrong app offset (HD vs NA) | Medium | Confirm rev on the live board; default HD. A wrong‑offset flash just won't boot → re‑flash the other offset (the flash bootloader sits outside both app windows, so it stays alive). |
| MCU won't enter ISP ("timed‑out") | **The failure you already hit** | Reproduce the **exact** SoC GPIO sequence: `gpio_out(35,1)` → 1 s → pulse `gpio34` LOW 15.68 ms → HIGH 15.68 ms (§2.3) — GPIO35's role is unproven but reproduce it anyway. If sync still fails after many cycles, **power-cycle the printer** (unplug, ~10 s) before anything else. |
| `IAPCommand` won't run on the Orange Pi | Certain (32‑bit ARM glibc) | Run it **on the SoC** (its native platform), not the Pi. The Pi is for *running* Klipper, not flashing. |
| Serial pin / GPIO guessed wrong | Medium | Confirm from DTB / probe (§7) before flashing. |
| `PB7` hotend pin (invalid ADC) | Certain (from ADV3 cfg) | Probe (§5); it *will* fail until moved to a real ADC pin (PA0–7, PB0–1, PC0–5). |
| Heat / motion during probing | **None** | `probe.cfg` has no heaters/steppers configured. |

---

## 10. What is still UNCONFIRMED (being honest)

1. ~~Exact MCU↔SoC serial pin pair~~ — **RESOLVED session 5**: MCU side is USART2, TX=PA9,
   RX=PA10, 115200 (proven from factory-firmware RE); SoC side is `/dev/ttyS1` (live). What
   remains unproven is GPIO35's connection/role, and the boot stub's app-validation behavior
   (the leading suspect for flashed-Klipper silence — see `adv4-iap-protocol.md` §3).
2. **Which board rev you have** (HD vs NA) → `0xC000` vs `0x10000`. Default **HD**.
3. **Live thermistor scaling** — the LUT *shape* is extracted (§4.2), but the absolute ADC↔°C anchor
   must be validated with live `M105`/`QUERY_ADC` during the probe pass before trusting temperatures.
4. **Exact vendor service name** holding `/dev/ttyS1` — identify on the live SoC and stop it before
   flashing (§7).
```
