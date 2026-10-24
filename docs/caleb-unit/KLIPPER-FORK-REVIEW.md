# Code review — the `klipper-fork` branch (MCU firmware source)

_Reviewed 2026-10-24, before any flash on this unit. Nothing here has been
built or run; this is a read of the source against mainline Klipper._

Branch: `klipper-fork`, single commit `90cc71a`, "AD4 Klipper port snapshot".

## What the branch actually is

A **squashed full copy of Klipper** — 2 333 files, 2 973 450 insertions, one
commit. The message says it is a snapshot of a shallow working clone and that
the individual changes live in the author's local clone, which means **the
per-change history is not in the repo**. There is no way to see the order the
AD4 changes were made in, or what was tried and reverted.

Mainline Klipper **already has an HC32F460 port** (see
`config/printer-creality-ender2pro-hc32-2022.cfg`, and `src/hc32f460/` upstream,
Copyright 2022 Steven Gotthardt). The AD4 work is a small delta on top of it,
not a new port.

Diffed against `Klipper3d/klipper` master (`ee2c6fb`), the entire delta in
`src/hc32f460/` is **four files, ~99 inserted lines**:

| File | Change |
|---|---|
| `Kconfig` | +9 −4 |
| `adc.c` | +10 −4 |
| `main.c` | +34 |
| `serial.c` | +46 −2 |

`Makefile`, `gpio.c`, `gpio.h`, `hard_pwm.c`, `internal.h`, `interrupts.c` are
unchanged from mainline. No AD4-named files exist anywhere in the tree.

That small delta is good news for auditability: the whole firmware change is
reviewable in an afternoon.

---

## 1. SWD is disabled by Klipper itself — the most important finding

`src/hc32f460/main.c`, in `armcm_main()`:

```c
// disable JTAG/SWD on pins PA13, PA14, PA15, PB3, PB4
// SWD still works until the relevant pins are reconfigured. Proprietary
// flash program (XHSC ISP) must be used to reflash afterwards.
PORT_DebugPortSetting(ALL_DBG_PIN, Disable);
```

**This line is in mainline Klipper, not an AD4 addition.** Klipper's own startup
turns the debug port off on every boot.

Consequences:

- An SWD dump must happen **before** flashing Klipper. Afterwards the only
  window is the few milliseconds between reset release and `armcm_main()`
  reaching that call — i.e. connect-under-reset, which
  `adv4-swd-dump.md` lists as a fallback and which nobody has demonstrated on
  this board.
- `adv4-swd-dump.md` says SWD-enabled-on-shipped-board is **UNVERIFIED** and the
  pad locations are **UNKNOWN**. So "get an ST-Link later" is not a reliable
  plan; it is a plan that depends on two unknowns plus a timing race.
- The vendor restore path (`IAPCommand` + stock hex over the MCU UART) does
  **not** use SWD and is unaffected.

`printer.cfg` compounds this: it declares `[output_pin p_PA13]` and
`[output_pin p_PA14]` — SWDIO and SWCLK — as general-purpose outputs.

**Recommendation:** decide about SWD before Stage 2, not after. This is a
one-way door.

## 2. `adc.c` — both changes are thermal-safety fixes

```c
/* AD4 (2026-10-21): 30 clocks caused "ADC out of range" MCU shutdown +
 * a 147C bed spike the moment the hotend heater started switching —
 * too short for the high-impedance NTC dividers. 120 clocks ~10us. */
#define TIMEOUT_VAL     (120u)
```

```c
// Live AD4 data (2026-10-21): DR reads are LEFT-justified despite
// AdcDataAlign_Right (raw 0x9d7b..0xa17b -> >>4 = 2519..2583 stable;
// masking to 12 bits instead jumps 3963->123). Raw values >4095 also
// tripped adccmds.c "ADC out of range" and shut the MCU down.
return ADC_GetValue(M4_ADC1, g.chan) >> 4;
```

Read these as a **recorded thermal incident**: with mainline's values, a bed
temperature spike to 147 °C occurred when the hotend heater began switching.
Anyone building this firmware must carry both fixes. Building from mainline
`src/hc32f460` without them reproduces the conditions of that incident.

The left-justification evidence is internally consistent (stable 2519–2583 after
`>>4`; a 3963→123 discontinuity when masked instead), which is the signature of
a justification error rather than noise. It is a plausible silicon/DDL
discrepancy, but it rests on one unit's observation — worth confirming on a
second unit before proposing it upstream, since it would affect the Ender 2 Pro
users mainline serves.

## 3. `Kconfig` — correct for the AD4, not upstream-mergeable as written

Adds `HC32F460_SERIAL_PA9_PA10` (USART2, RX=PA10, TX=PA9), consistent with the
firmware-RE evidence in `adv4-swd-dump.md`. Good.

But it **removes** `HC32F460_SERIAL_PH2_PB10` rather than adding alongside it.
That silently breaks any existing user of the PH2/PB10 pinout. A PR would need
the option restored.

`RAM_START` is changed `0x1fff8000` → `0x20000000`, commented as the region
"PROVEN live on the AD4 by the bare-metal probe (2026-10-21)", with the old
value "never verified on this board".

**Flagging this as unresolved:** mainline uses `0x1fff8000` for the Ender 2 Pro's
HC32F460, so upstream has that address working on real hardware. Either the AD4
differs (plausible — different package or SRAM configuration), or the probe
misattributed a symptom caused by something else, such as the stale-clock bug
fixed separately in `main.c`. The two changes landed in the same session, so one
could mask the other. Not resolvable by reading; needs a build test at each
value.

## 4. `serial.c` — clean, and it documents a disproven path

Adds the USART2 PA9/PA10 block and an explicit **DO NOT USE** note on
`HC32F460_SERIAL_PA13_PA14`, recording that the original "vendor ISP runs on
JTMS/JTCK" claim was false and that hexes built that way were silent. Useful
negative result, properly labelled.

Also adds the `USART_NUM == 2` interrupt block, which is what makes USART2
usable. Straightforward.

## 5. `printer.cfg` — a draft, and the heater pins disagree with other docs

Its own header says hotend thermistor pin, LED pin, thermistor curves and nozzle
offsets are `TODO-PROBE`. Beyond that:

```
[extruder]   heater_pin: PC13   # PA1 parked here pending bed-heater test on PA1
[heater_bed] heater_pin: PA1    # PC4 disproven (M140 no heat); testing PA1
```

Both lines are mid-experiment, and the earlier research notes record the heater
pins differently (bed PA7, hotend PA1). `PA1` cannot be both. **Treat every
heater and sensor pin as unconfirmed until tested on this unit** — and test them
knowing a 147 °C bed excursion has already happened once on this hardware.

It also `[include]`s `adv3-macros.cfg`, `fluidd.cfg` and `KAMP_Settings.cfg`,
which are not in the branch; Klipper refuses to start on a missing include.

`[mcu] serial: socket://192.168.10.10:5000` hardcodes the author's SoC address.

## 6. `flasher/stm32isp-ad4.c` — two defects found while investigating read-back

Found 2026-10-24 while trying to establish whether the vendor bootloader
exposes any read command. Both are in the flasher, not the firmware.

### 6a. `st_getpid()` cannot succeed on this MCU (dead code, wrong ack)

```c
static int st_getpid(int fd, unsigned short *pid){
    unsigned char c = CMD_GET; ser_wr(fd,&c,1);
    if(!wait_ack(fd, ACK_OK)) return 0;        /* ACK_OK = 0x79 */
    ...
}
```

`wait_ack(fd, ACK_OK)` requires `0x79`. The file's own header documents at
length that this ROM is **not** a plain STM32 and answers with `0x06`/`0x07`,
never `0x79` — that HC32-specific handshake is the central point of the
protocol comment at lines 65-82. So `wait_ack` spins its ~300 ms window and
returns 0 unconditionally, and `st_getpid` returns failure **whether or not the
bootloader implements get-PID**.

Consequence for debugging: a get-PID probe is worthless as evidence. It cannot
distinguish "command unsupported" from "command supported, acks with 0x06".
This is the same defect class as the `-d` dump path, where `st_read` sends
STM32 `CMD_READ 0x11`, never receives `0x79`, and the caller silently counts a
chunk-fail and leaves the buffer zero-filled — which is why `-d` produces a
zero file rather than an error.

`st_getpid` currently has **no callers**; it is inherited from upstream
stm32isp. Line 467 separately records a deliberate decision not to send `0x02`,
because stray reply bytes can desync the per-frame `0x06` acks. That decision
looks correct and should stand — the function should be deleted or fixed to
expect `IAP_FRAME_OK`, not left as a trap for the next person debugging
read-back.

### 6b. HD/NA revision bytes are documented backwards vs. the code

The usage block and the `#define`s disagree:

| Location | Claim |
|---|---|
| Line 36 (usage comment) | `0x06 (HD/H32) or 0x07 (NA/N32)` |
| Line 76 (protocol comment) | `0x06 (HD revision) or 0x07 (NA revision)` |
| `#define SYNC_REV_HD` | `0x07` |
| `#define SYNC_REV_NA` | `0x06` |

The comments say `0x06`=HD; the code says `0x07`=HD. They cannot both hold.

This is not cosmetic: the sync reply is the **revision detector**, and revision
selects the flash base — `HD_BASE 0x0800C000` vs `NA_BASE 0x08010000`. The
`-r` override exists, so a correct manual invocation is unaffected, but
autodetect is ambiguous and the verbose line
(`"sync reply 0x%02x = %s"`) will label the revision according to whichever of
the two is wrong.

Compounding it: `IAP_FRAME_OK` is also `0x06`, which the file acknowledges
("Same value as the HD-sync byte; context disambiguates"). With the HD/NA
mapping itself in doubt, that collision is harder to reason about than the
comment implies.

**Unresolved:** which byte this unit's HD chip actually returns. Settling it
requires one sync with `-v` and reading the reply — not yet done, because it
means resetting the MCU into ISP state.

### 6c. Read-back may be address-range-limited rather than absent

The dump-path comment records an empirical result:

> reading at `0x08000000` is out of range for the HD ROM and every chunk times
> out (hangs)

...and prescribes `0x00000000..0x40000` (relative addressing) instead. That
distinction implies out-of-range reads behave differently from in-range ones.
But given 6a — `st_read` can never see its expected `0x79` ack — **no read at
any address can report success**, so the zero-filled output is equally
consistent with every read failing for the ack reason alone. The relative-vs-
absolute addressing conclusion may be sound, but the evidence as recorded does
not establish it.

**Practical upshot:** the flasher cannot presently demonstrate that read-back
works or that it does not. Read-back over UART is therefore not a dependable
backup path, which is what makes an SWD dump before Stage 2 the only verified
route to a byte-exact image of the stock firmware — and Klipper's startup
`PORT_DebugPortSetting(ALL_DBG_PIN, Disable)` (§2) closes that window at first
flash.

## Summary

| Item | Status |
|---|---|
| Firmware delta is small and reviewable (4 files, ~99 lines) | **VERIFIED** by diff against mainline `ee2c6fb` |
| Klipper disables SWD at startup | **VERIFIED** in source (mainline code) |
| ADC fixes are required to avoid a known thermal event | **VERIFIED** in source + author's recorded incident |
| ADC left-justification is a real silicon/DDL discrepancy | **PLAUSIBLE** — one unit, one session |
| `RAM_START` must be `0x20000000` on the AD4 | **UNRESOLVED** — conflicts with mainline's working value |
| Serial on USART2 PA9/PA10 | **CONSISTENT** with independent firmware RE |
| Heater / sensor / stepper / endstop pins | **UNCONFIRMED**, internally contradictory |
| `printer.cfg` runs as-is | **NO** — missing includes, hardcoded IP, TODO-PROBE items |
| `st_getpid()` can ever succeed on this MCU | **NO** — expects 0x79, ROM sends 0x06/0x07; dead code |
| UART read-back works (or provably does not) | **UNDEMONSTRABLE** with current flasher (§6a, §6c) |
| HD vs NA sync byte | **CONTRADICTORY** — comments say 0x06=HD, `#define`s say 0x07=HD |

Nothing in this branch has been built or flashed on this unit.
