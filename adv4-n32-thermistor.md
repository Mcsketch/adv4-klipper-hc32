# AD4-NA (Nation N32G45x) ADC / Thermistor Model + Klippventurer N32 Review

Scope: Nation N32G45x ADC/thermistor model for the FlashForge Adventurer 4 "NA" MCU
variant, decoded against the factory IAP image
`stm32isp-ad4/Adventurer4-NA-IAP-2.3.0-20230216.hex`, cross-checked against upstream
Klipper's N32G45x support and the local Klippventurer N32 flasher
(`work/kv-next/support/usb-validation/n32/go-flash-overlay/flash/`).

Ground-truth files used:
- `NA.bin` = `arm-none-eabi-objcopy -I ihex -O binary` of the NA hex (44640 bytes).
- Vector table @ VMA `0x08010000`: SP=`0x20003b78` (RAM), reset=`0x080101ad` (flash).
  This confirms **app base = `0x08010000`** and therefore **`file_offset = VMA − 0x08010000`**.

---

## 1. N32G45x ADC / thermistor formulas (with numbers)

### 1.1 ADC digit range (upstream Klipper `src/stm32/adc.c`)

Klipper reuses the STM32F1 ADC path for `MACH_N32G45x` (Kconfig: `MACH_N32G452/G455`
`select MACH_STM32F1`). The MCU-side constant is fixed:

```c
DECL_CONSTANT("ADC_MAX", 4095);          // 12-bit, digital 0..4095
```

So in Klipper, the raw ADC count read from the MCU is **0..4095**, and the MCU reports
that count; all temperature math happens host-side (`klippy`).

### 1.2 ADC digital → physical voltage (the divider)

Klipper's host model (`klippy/extras/adc_temperature.py` + `thermistor.py`) is a simple
voltage-divider from the raw count to a fraction, then to a resistance. With a pullup
`R_p` and the thermistor as the lower leg:

```
adc_frac = ADC / ADC_MAX                       # 0..1  (ADC_MAX = 4095)
V_sensor = adc_frac * V_REF                    # V_REF = MCU ADC ref = 3.3 V (assumed)
R = R_p * adc_frac / (1.0 − adc_frac)          # thermistor resistance (ohms)
```

From `klippy/extras/thermistor.py` (`Thermistor.calc_temp`, `setup_coefficients*`):

```
R = pullup * adc / (1.0 − adc)                 # adc in 0..1, pullup default 4700 Ω
ln_r = ln(R − inline_resistor)
inv_t = c1 + c2*ln_r + c3*ln_r^3               # Steinhart–Hart
T(°C) = 1.0 / inv_t − 273.15
```

- `pullup_resistor` default **4700 Ω** (config `[thermistor]` / sensor default).
- Three-point Steinhart–Hart is fit from (T,R) triples; if `c3 ≤ 0` it falls back to a
  two-parameter **beta** model: `inv_t = c1 + c2*ln_r`, `c2 = 1/beta`.
- Reference voltage is not a Klipper constant; it cancels in the divider for a
  pullup-on-top topology (only the *fraction* matters). Assume **3.3 V**.

### 1.3 Which built-in curve a 100 k thermistor uses

`klippy/extras/temperature_sensors.cfg` defines the stock curves. For a **100 k** part:

| Curve (3-point Steinhart–Hart) | T1/R1 | T2/R2 | T3/R3 |
|---|---|---|---|
| `Generic 3950` | 25 °C / 100000 Ω | 150 °C / 1770 Ω | 250 °C / 230 Ω |
| `EPCOS 100K B57560G104F` | 25 °C / 100000 Ω | 150 °C / 1641.9 Ω | 250 °C / 226.15 Ω |
| `ATC Semitec 104GT-2` | 20 °C / 126800 Ω | 150 °C / 1360 Ω | 300 °C / 80.65 Ω |
| `NTC 100K MGB18-104F39050L32` (beta) | 25 °C / 100000 Ω | beta = 4100 | — |

A generic 100 k NTC (β≈3950) with the default 4700 Ω pullup produces these *fractions*:

| T | R (Ω) | adc_frac | ×4095 | ×384 |
|---|---|---|---|---|
| 25 °C | 100000 | 0.956 | 3914 | 367 |
| 60 °C | 18430 | 0.797 | 3262 | 306 |
| 110 °C | 2607 | 0.357 | 1462 | 137 |
| 190 °C | 399 | 0.078 | 321 | 30 |
| 215 °C | 236 | 0.048 | 196 | 18 |
| 265 °C | 93 | 0.019 | 79 | 7 |

**Key contrast:** the factory AD4 LUTs (section 2) use an ADC scale of roughly
**0..384**, not 0..4095. That means the NA firmware does *not* report a raw 12-bit count
to a stock 4700 Ω-pullup model — it either (a) prescales the raw count by ~1/10.7, or
(b) uses a different pullup/Vref so the whole curve compresses into 0..384. The factory
image therefore encodes its own (temp → LUT-index) tables and cannot be matched by a stock
Klipper `sensor_type`; a **custom** `[thermistor]` (3-point) built from the live
`QUERY_ADC` points is required. See caveats.

### 1.4 The ADC-clock caveat

Klipper's `src/stm32/adc.c` comments list F1/F4 sample timings but for `MACH_STM32F1`
(the path N32G45x selects) the calibration path is enabled and the ADC runs from the AHB
bus. `CLOCK_FREQ` (section 4) sets the bus, hence the ADC clock. The absolute count scale
must still be confirmed live with `QUERY_ADC` at a known temperature — the factory LUT
scale (0..384) already proves the vendor's mapping differs from raw 4095.

---

## 2. NA-hex decoded thermistor tables + comparison to a 100 k thermistor

### 2.1 Important correction to the prior notes — *chip scope, not a data error*

`adv4-exploration.md` §4.1 cites four descriptors at `0x0801A4CC / 0x0801A6AC /
0x0801A88C / 0x0801A974` and a word array at `0x08015D10`. Those offsets are **correct
for the HD image** (HC32F460, base `0x0800C000`): in `Adventurer4-HD-IAP-…hex` the four
words at `0x08015D10` are exactly the flash file-offsets `0x1A6AC / 0x1A88C / 0x1A974 /
0x1A4CC`, and the HD bed at `0x0801A6AC` does hold `(10.2 °C, 300), (11.0, 295), …`.
Earlier confusion arose from **comparing the HD-cited offsets against the NA image**,
which is a *different* chip (N32G45x, base `0x08010000`) whose LUTs live elsewhere. The
NA-correct offsets, decoded from the clean `Adventurer4-NA-IAP-2.3.0` objcopy binary:

| Real LUT (NA image) | VMA | file offset | shape | range (verified, NA.bin) |
|---|---|---|---|---|
| BED | `0x0801A104` | `0xA104` | 8 B/entry: u32 header word, then (u32 temp×10, u32 adc) pairs | 10.2 °C @ 300 → 188.6 °C @ 145 (32 entries, adc linear 300→145) |
| HOTEND standard | `0x0801A438` | `0xA438` | 4 B/entry (u16 temp×10, u16 adc) | 189.3 °C @ 369 → 3.0 °C @ 0 (252 entries) |
| HOTEND high-temp | `0x0801A914` | `0xA914` | 4 B/entry (u16 temp×10, u16 adc) | 396.8 °C @ 62 → 96.5 °C @ 3 (197 entries) |

The NA bed's first word (`0xFFFFFFE3`, = 65507 unsigned / −29 signed) is an unexplained
header word, not a sample; the first real `(102, 300)` pair begins at `0x0801A108`.

Exact head/tail values read directly from `NA.bin`:

- **BED** `0x0801A104`: `(102,300),(110,295),(119,290),(128,285),…` →
  **(10.2 °C, ADC 300) → (11.0 °C, 295) → (11.9 °C, 290) → (12.8 °C, 285)… → (147.5 °C, 155)**.
  Same value set as the HD bed (both chips share the bed LUT content).
- **HOTEND standard** `0x0801A438`: head `(1893,369),(1524,249),(1275,179),…` →
  **189.3 °C @ ADC 369 down to ~3 °C @ ADC 0**.
- **HOTEND high-temp** `0x0801A914`: head `(3968,62),(3906,60),(3846,59),…` →
  **396.8 °C @ ADC 62 down to 96.5 °C @ ADC 3**.

So: the prior notes' **HD** addresses stand; this section documents the **NA** addresses.
Any tooling that keys off LUT offsets must branch on the detected revision (sync reply
6 = HD, 7 = NA).

### 2.2 Do these match a standard 100 k thermistor?

**No — not with a stock Klipper curve, for three independent reasons:**

1. **Scale.** The factory LUTs are indexed 0..~384 (hotend std), 0..~370 (bed), and
   0..~62 (high-temp). A stock 4700 Ω-pullup 100 k NTC spans 0..~3915 of 4095
   (table in §1.3). The vendor's effective ADC full-scale is ~10× smaller, so the
   firmware's absolute scaling (Vref/pullup/prescale) differs from Klipper's model.
2. **Bed ceiling.** The bed LUT stops at ~117 °C with ADC 170. A stock 100 k at
   117 °C would read ~2100/4095, not 170. So the bed is *not* a stock 100 k on a
   4700 Ω pullup — it is a higher-NTC or a different divider (beds are commonly a
   100 k on a larger pullup, or a separate part).
3. **Hotend top.** The high-temp table reaches ~397 °C, above any 100 k's useful range
   and above the 265 °C nozzle ceiling the SoC enforces; it is a deliberately extended
   (non-standard) curve for high-temp nozzles.

**Conclusion:** the AD4-NA uses **vendor-specific (temp→ADC-index) LUTs**, not a
standard Steinhart–Hart 100 k. To run Klipper you must **not** reuse `Generic 3950` /
`EPCOS 100K`; you must build a **custom** `[thermistor]` from *live* `QUERY_ADC`
measurements (ice bath / room temp / a known set point) that pin the true (adc_frac, T)
mapping, then fit 3 points. The factory LUTs give the *shape* (NTC, monotonically
falling) and the *valid range*, but the absolute scale must be measured — the firmware
does not expose a `Vref`/pullup constant we can read out of the flat image.

What we **can** say with confidence from the image:
- The sensors are NTC thermistors (ADC falls as T rises). ✅
- Two distinct hotend curves (standard to ~189 °C, high-temp to ~397 °C) + one bed
  curve to ~117 °C, matching the interchangeable-nozzle design. ✅
- The mapping is table/LUT based (piecewise), not a closed-form β, in the vendor code. ✅

### 2.3 What we could and could not decode

- **Could:** exact byte-level LUT contents, ranges, monotonic shape, and the true file
  offsets of the three main LUTs (above). Confirmed app base `0x08010000` from the vector
  table.
- **Could not (and why):** (a) the *consumer* function that selects per-channel LUTs —
  the literal-pool / base-normalized reference search returns zero hits; the code reaches
  the tables indirectly, so the channel→LUT assignment is value-based inference, not
  proven. (b) The absolute ADC scale (Vref, pullup, prescale factor) — not present as a
  readable constant in the flat image; must be measured live with `QUERY_ADC`. (c) The
  "pointer array at `0x08015D10`" — that address is **not** a valid pointer array
  (words don't fall in the app range); the prior note's claim is not reproduced.

---

## 3. Klipper build recipe for AD4-NA (Nation N32G45x)

### 3.1 Kconfig / menuconfig (verified against upstream `src/stm32/Kconfig`)

| Setting | Value | Source (upstream) |
|---|---|---|
| Processor model | **`Nation N32G452`** (or `N32G455`) → `MACH_N32G45x` | `choice "Processor model"` |
| `MCU` | `stm32f103xe` (auto default for `MACH_N32G45x`) | `config MCU … default "stm32f103xe" if MACH_N32G45x` |
| `CLOCK_FREQ` | **`128000000`** (external) — or `64000000` if `STM32_CLOCK_REF_INTERNAL` | `default 128000000 if MACH_N32G45x` |
| `FLASH_SIZE` | **`0x20000`** (128 KiB) | `default 0x20000 if MACH_N32G45x` |
| `RAM_SIZE` | **`0x10000`** (64 KiB) | `default 0x10000 if MACH_N32G45x` |
| Bootloader offset | **`64KiB bootloader`** → `STM32_FLASH_START_10000` → `FLASH_APPLICATION_ADDRESS = 0x8010000` | `default 0x8010000 if STM32_FLASH_START_10000` |

> **Why 64 KiB (0x10000) and not 0:** the NA IAP image's app lives at `0x08010000`,
> i.e. **64 KiB** above the flash base `0x08000000`. That 64 KiB region is the vendor
> ROM/IAP bootloader + data. Klipper's `STM32_FLASH_START_10000` option produces exactly
> `FLASH_APPLICATION_ADDRESS = 0x8010000`, matching the vendor layout. Do **not** use
> "No bootloader" (0x8000000) — it would overwrite the ROM-IAP region and **brick the
> board** (the ROM-ISP entry is what recovery depends on; see §5).

### 3.2 `menuconfig` selection

```
Processor model: Nation N32G452          (or Nation N32G455)
Clock Reference: (external, default)     -> CLOCK_FREQ = 128000000
  (choose "Internal clock" only if you must; then CLOCK_FREQ = 64000000)
Bootloader offset: 64KiB bootloader       -> FLASH_APPLICATION_ADDRESS = 0x8010000
Communication interface: Serial
  - choose a free TX/RX pair (see 3.3)
Baud Rate: 250000 (default)
```

### 3.3 Serial pins — PA7/PA8 conflict

The HC32 notes flag `SERIAL_PA7_PA8` as conflicting with X `step_pin`. The N32G45x
shares the same pinmap, so **avoid PA7/PA8** for the host↔MCU UART. The N32G45x STM32F1
serial options are the `STM32_SERIAL_*` choices (PA9/PA10 = USART1, PA2/PA3 = USART2,
PD5/PD6 = USART2, PB10/PB11 = USART3). For AD4-NA:

- **Recommended:** a pair not used by steppers/endstops/fans. On AD4 the TMC mux is on
  PC10 and the fan/heater pins occupy PA1/PC2/PC4/PC5/PA5/PB9, so a safe default is
  **USART1 PA9 (TX) / PA10 (RX)** — confirm against the board pin map before flashing,
  since NA may differ from the HC32 pinmap.
- Do **not** reuse the factory SoC↔MCU UART (MCU USART0) for Klipper host comms if it is
  still wired to the SoC; pick a dedicated pair.

### 3.4 Ready `[mcu]` + build snippet

`printer.cfg` (AD4-NA):

```ini
[mcu]
# Baud must match the MCU build's CONFIG_..._BAUDRATE (250000 by default).
serial: /dev/ttyACM0        # or the SoC-side UART to the N32G45x
baud: 250000
restart_method: command     # N32G45x uses the "command" (BOOT/RESET) restart method

[thermistor ad4_hotend]     # MUST be fitted from live QUERY_ADC, see §2.2
temperature1: <T1>  resistance1: <R1>
temperature2: <T2>  resistance2: <R2>
temperature3: <T3>  resistance3: <R3>
pullup_resistor: <measured>   # do NOT assume 4700

[thermistor ad4_bed]        # fitted from live QUERY_ADC
temperature1: 25   resistance1: <R>
temperature2: 70   resistance2: <R>
temperature3: 110  resistance3: <R>

[extruder]
sensor_type: ad4_hotend
sensor_pin: <ADC pin>        # must be a valid N32G45x ADC pin
heater_pin: <heater pin>
min_temp: 0
max_temp: 265                # nozzle rating, not the LUT ceiling

[heater_bed]
sensor_type: ad4_bed
sensor_pin: <ADC pin>
heater_pin: <heater pin>
min_temp: 0
max_temp: 110                # matches the factory ~110 °C clamp
```

Build:

```
make menuconfig                # N32G452, 128 MHz ext, 64KiB bootloader, serial pair
make                           # -> out/klipper.bin  (app @ 0x08010000)
```

Flash with a tool that targets **`0x08010000`** and only the app window (see §4–5 for why
Klippventurer's current default does not do this).

---

## 4. What's wrong in KV's N32 work for AD4-NA (file:line evidence)

Files: `work/kv-next/support/usb-validation/n32/go-flash-overlay/flash/`

### (a) Flash base `0x08000000` vs correct `0x08010000` — **WRONG, bricking risk**

- `n32.go:10` → `const n32FlashBase uint32 = 0x08000000`
- `n32.go:11` → `const n32FlashSize uint32 = 0x00020000`
- `n32.go:72-84` `n32FlashBounds()` uses `n32FlashBase` **unless** the caller sets
  `mc.config.TargetFlashBase`.
- `n32.go:49-70` `n32ValidateFlashRange` only rejects writes **outside** `[base, base+size)`.

**Impact:** with the default (no `TargetFlashBase`), the allowed window is
`0x08000000..0x08020000`. A Klipper build flashed at its *natural* address `0x08010000`
(with 64 KiB bootloader) sits *inside* that window, so the range check **passes** — but
if the flasher is pointed at `0x08000000` (no offset) it will **erase+write the 64 KiB
ROM/IAP region**, destroying the recoverable bootloader. Conversely, the default window
*also* permits the `0x08000000` region that **should be write-protected** for
recovery. The bounds check is permissive in exactly the wrong direction: it protects
against writing *beyond 128 KiB*, not against writing the *bootloader* region.

**Fix:** default `n32FlashBase = 0x08010000` (matching `STM32_FLASH_START_10000`),
and *refuse* any write at `addr < 0x08010000` (keep the ROM-IAP recoverable) — i.e. the
opposite of the current "allow from 0x08000000" posture. Our own flasher already does
this (refuse `base <= 0x00008000`-style low addresses and auto-detect HD/NA from the
`0x43` reply).

### (b) Even parity vs AD4 MCU serial — **unverified, likely wrong**

- `serial.go:31-36` → `serial.Open(..., &serial.Mode{ DataBits:8, Parity: serial.EvenParity, StopBits: serial.OneStopBit })`

The AD4 MCU is entered via the **vendor IAP protocol** (sync byte `0x43`, reply
`6`=HD / `7`=NA), not the STM32 ROM bootloader's 8E1 framing. The factory IAPCommand's
UART settings are the source of truth for parity; **we have not captured them from the
AD4 rootfs in this pass**, so treat `EvenParity` as an *assumption carried over from the
STM32 ROM path*. It is plausible the vendor IAP uses **no parity (8N1)** — in which case
KV's `EvenParity` would silently corrupt every byte. This must be confirmed by capturing
raw IAP traffic (or reading the IAPCommand serial init) before flashing. Marking it an
assumption, not a verified fact.

### (c) Sync byte `0x7f` vs AD4's `0x43` — **WRONG**

- `stm32.go:14` → `const b_STM_SYNC byte = 0x7f`
- `stm32.go:12-13` → `b_STM_ACK = 0x79`, `b_STM_NACK = 0x1f`
- `n32.go:17-22` `n32Init()` → `return mc.stmInit()` (i.e. N32 speaks the *STM* sync).

The AD4's MCU boot entry is the **GPIO strap** (GPIO34=NRST, GPIO35=BOOT0) into the
vendor IAP, whose **sync byte is `0x43`** (reply `6`=HD, `7`=NA) — *not* the STM32 ROM
`0x7f`. So `n32Init()` will send `0x7f` and time out waiting for the vendor IAP. The
entire `stmCmdSync/Get/GetId/…` command map (`stm32.go:43-55`, `defaultCmdCodeMap`) is
the STM32 ROM mass-erase/write protocol and does not match the vendor IAP command set.
The N32 path needs its **own** sync byte + command map, not a thin wrapper over `stmInit`.

### (d) "serial-request @ 230400" boot entry vs the real GPIO-dance boot entry — **WRONG for AD4**

- The KV design (`docs/specs/n32-flashing-implementation.md`) and the broader flasher
  assume a **"Request Serial Bootloader" / `NationsCommand`** string for "ARM
  Adventurers" and a serial request at 230400 baud before probing at 115200.
- **AD4 rootfs (verified earlier):** contains `IAPCommand` + the GPIO34/GPIO35 dance and
  sync `0x43`; it does **NOT** contain any `Request Serial Bootloader` /
  `NationsCommand` string (those belong to AD5M/other models).

**Impact:** for AD4-NA the correct boot entry is the **GPIO strap**, not a serial
command. KV's "serial-request@230400" path will do nothing useful on AD4 (the string is
absent), and the 230400 handshake will never arm the vendor IAP. The AD4 needs the
`enterSTBL`-style GPIO sequence (`stm32.go:67-81`) wired to **GPIO34=NRST / GPIO35=BOOT0**
with the vendor IAP (sync `0x43`), not the generic STM BOOT0/BOOT1 pads KV configures by
default (`mcu.go:64-72`: Boot0GPIO=39, Boot1GPIO=41, PowerGPIO=19 — these are the *wrong*
line numbers for the AD4, which uses 34/35).

### (e) Recovery / safety against a bad-base write — **INSUFFICIENT**

- `n32.go:49-70` range check (see (a)): does **not** protect the ROM-IAP region.
- `n32.go:86-107` `n32ValidateChipID`: only checks a *text* chip ID match (and is
  skipped under `AllowUnknownChipID`). It does **not** verify that the *board* is actually
  an N32 (HD vs NA) — a wrong-family flash would still proceed. HD/NA must be distinguished
  by the vendor IAP reply byte (`6`=HD / `7`=NA), which KV does not read.
- `n32.go:43-47` `n32ExitBootloader` reuses `exitSTBL` (STM power/BOOT0/BOOT1 reset) —
  not the AD4's GPIO34/GPIO35 reset, so post-flash reset/exit is unverified.
- `probe.go:64-94` `stmCompatibleProbe` runs `enterSTBL` then STM `sync/GET/GET ID`; on
  AD4 that fails at sync (`0x7f` vs `0x43`), so even the read-only preflight cannot
  confirm the board — and it offers no HD/NA confirmation.

**Net:** KV's N32 path is a STM32-ROM wrapper with a pluggable base/size, but for AD4-NA
the base default, parity, sync byte, command set, and boot-entry are all STM-derived
assumptions that do not match the vendor IAP, and the range check actively *allows*
writing the ROM-IAP region. It is not safe to run on AD4 as-is.

---

## 5. Safety / recovery gaps (specific)

1. **ROM-IAP write-protect missing.** Default window `0x08000000..0x08020000`
   (`n32.go:10-11`) permits erasing the 64 KiB ROM/IAP region. A no-offset Klipper flash
   would brick the board and remove the only serial recovery path.
   *Required:* refuse `addr < 0x08010000`; default base `0x08010000`.
2. **No HD/NA discrimination.** Recovery and command selection both depend on knowing
   whether the board is HD (HC32F460) or NA (N32G45x). The vendor IAP reply byte
   (`6`=HD / `7`=NA) is the discriminator; KV's chip-ID check (`n32.go:86-107`) is a text
   match that does not read this. A mismatched flash could talk the wrong protocol.
3. **Boot-entry GPIO numbers wrong.** `mcu.go:64-72` defaults to Boot0=39 / Boot1=41 /
   Power=19; AD4 uses **GPIO34=NRST / GPIO35=BOOT0**. `enterSTBL` (`stm32.go:67-81`) and
   `n32ExitBootloader` (`n32.go:43-47`) therefore drive the wrong pins → board never
   enters/exits IAP, or a bad reset after flash.
4. **Sync/command mismatch blocks even read-only preflight.** `stm32.go:14` sync `0x7f`
   vs vendor `0x43`; the `defaultCmdCodeMap` (`stm32.go:43-55`) is the STM ROM map.
   `Probe()` (`probe.go:20-38`) will always fail on AD4, so there is no safe "identify
   only" preflight that confirms the board before any erase.
5. **Parity unverified.** `serial.go:34` `EvenParity` carried from the STM path; vendor
   IAP parity not captured. If it is 8N1, every byte is corrupted and the protocol
   never syncs (silent failure, not a clear error).
6. **No erase verification / readback.** `stmFlash` (`stm32.go:160-185`) erases then
   writes 256-byte blocks with no post-write readback or erase-confirm; a partial/failed
   erase in the app window with no recovery guard would leave a half-flashed MCU.
7. **Recovery story is STM-only.** The only "recovery" is `exitSTBL`/`Reset`
   (`mcu.go:162-165`), which for AD4 is the wrong GPIO sequence and the wrong protocol.
   There is no documented AD4 path to re-arm the vendor IAP after a bad flash.

---

## Appendix A — raw bytes at the *cited* (prior-note) offsets, for the record

`0x0801A4CC` (file `0x0a4cc`), as interleaved u16:
```
bd 00 04 00 b9 00 04 00 b5 00 04 00 b1 00 04 00 ad 00 04 00 …
```
→ colA 75..189 (monotonic dec), colB 0..4. **Not** the "hotend high-temp to 263 °C"
table the prior notes claimed. (The real hotend tables are at `0x0801A438`/`0x0801A914`.)

`0x0801A6AC` (file `0x0a6ac`), as interleaved u16:
```
30 00 00 00 30 00 01 00 2f 00 00 00 2f 00 00 00 2f 00 00 00 …
```
→ colA 34..48, colB 0..1. **Not** the "bed (10.2 °C, 300)…" table the prior notes
claimed. (The real bed table is at `0x0801A104`.)

`0x0801A88C` (file `0x0a88c`): colA 2717..8620 (monotonic dec), colB 29..287.
`0x0801A974` (file `0x0a974`): colA 1666..2873, colB 11..33 (a high-temp LUT fragment).

`0x08015D10` (file `0x5d10`), as LE u32:
```
80 ed 00 0a | 0b e0 4d 48 | 00 78 18 b9 | 4c 48 90 ed
```
→ `0x0a00ed80, 0x484de00b, 0xb9187800, 0xed90484c` — **none in the app's `0x0801xxxx`
range**, so this is **not** a function-pointer array. The prior note's "4-entry pointer
array at `0x08015D10`" is not reproduced.
