# AD4 ROM-ISP Protocol + Boot Chain — Verified Findings (strace ground truth)

Source: `strace -f -e trace=openat,write,read,nanosleep,ioctl,close` on the
factory `/opt/ad4-klipper/IAPCommand` while it flashed the stock HD image,
plus control-flow analysis of `work/hd.bin` (a 64 KB factory flash dump) and
the `stm32isp-ad4/` source. This supersedes the unverified protocol claims in
the older `.md` files.

> **Correction 2026-10-20 (session 5): "ROM-ISP" in this doc's title is a
> misnomer.** The HC32F460 has **no ROM-resident ISP** (unlike STM32F1). The
> thing answering `0x43` is a **flash-resident vendor bootloader**, inferred
> at `0x00000000..0x0000BFFF` (the `work/hd.bin` stub at offset `0x0` is its
> tail end or a jump stub — see §3). It is ordinary flash: erasable, and
> brickable if we ever write over it. Every "ROM" mention below should be
> read as "flash bootloader".

## 1. The IAPCommand tool = stock stm32isp fork (not custom)
`strings IAPCommand` (unstripped, GCC 4.9.4, `cfmakeraw/tcgetattr/...`):
```
Syntax:  stm32isp [Options] <file> <comport> <baudrate>
Example: stm32isp test.hex /dev/ttyS0 115200
file name NA:%s / fileName HD:%s / portName:%s / baudrate:%d
"touch /root/N32" ... "STM32iap finished NA."
"touch /root/H32" ... "STM32iap finished HD."
"STM32 reset" "STM-32 Wirting" "Connected to board." "Can not connect to board, %s timed-out."
The %dth frame data transmission is completed.
STM32Write: success. /  failed. /  timed-out.
```
- **Comport** = `/dev/ttyS1` (default). **Baud** = 115200 (default).
- **Two revision images**: NA (Nation N32) and HD (Hollystone HC32). This
  board is **HD** (`touch /root/H32` on success). The `NA`/`HD` in the strings
  are the *image filenames* passed as `argv[1] argv[2]`.
- GPIO (from `gpio_export/gpio_unexport` + `system()` strings):
  `/sys/class/gpio`, `echo %d > %s/export`, `echo %u > %s/unexport`,
  `echo %s > %s/gpio%u/direction`, `echo %c > %s/gpio%u/value`.
  Confirmed live: **gpio34 and gpio35 are both exported, direction out**
  (gpio34 value=1, gpio35 value=0 after flash). **gpio34 = MCU NRST is the
  working interpretation (proven by behavior). "gpio35 = BOOT0" is a LABEL,
  not a proven fact** — the HC32F460 has no BOOT0 pin; whether gpio35 even
  reaches the MCU has never been traced.

## 2. ROM-ISP serial protocol (the real one, from the strace)
The MCU is an HC32F460. Its **ROM ISP is NOT the STM32 ROM bootloader**
(STM32 `0x43` sync expects a `0x79 'Y'` ack; this ROM replies `0x07`). This
explains why the generic STM32-style `stm32isp-ad4 -d` dump path can sync at
`0x43` (the ROM answers the sync) but the read/erase/write sub-commands do not
match. The observed exchange (fd 3 = /dev/ttyS1):

```
write(3, "C", 1)            # 0x43 sync
read(3,  "\7", 1)           # MCU -> 0x07 (1-byte ROM revision/handshake)
write(3, <1030 bytes> =1030)  # frame 0
read(3,  "\6", 1)           # MCU -> 0x06 (ack)
write(3, <1030 bytes> =1030)  # frame 1
read(3,  "\6", 1)
... (one 1030-byte frame per flash 1024-byte page + 6-byte header)
```
- **Data frames are 1030 bytes**: 6-byte header + 1024 bytes of flash payload.
  The header carries frame index (LE) + an address word (LE). Frame 0 payload
  begins `d8 2a 00 20 95 c2 00 00 ...` = the HD vector table (SP=0x20002AD8,
  reset=0xC295) — i.e. the tool writes the image starting at the **app base**.
- **Ack byte = `0x06`** per frame (not STM32's `0x79`).
- **Sync reply = `0x07`** (1 byte, the ROM's revision id).
- No GPIO `write()` to `/sys` appears in the captured trace window for the
  *data* phase — the reset/BOOT0 dance happens at connect time (before the
  first `0x43`), consistent with `NRST 15.68 ms LOW then HIGH` + `BOOT0 HIGH`
  (see the existing `stm32isp-ad4.c` comment and the `nanosleep({0,2000000})`
  = 2 ms and `nanosleep({0,200000000})` = 200 ms spacing in the trace).
- The strace `write(3, "...\1\22\0\4...")`-style backslash-heavy lines are
  strace's octal escaping of *raw flash bytes* — the first such write at
  offset ~303 is frame 0 of the **NA** image (its payload contains the string
  `source/driver/src/hc32f460_gpio.c` from the compiled image), and the later
  `:10C00000`-aligned data is the **HD** image frames.

### Protocol table (HC32 ROM ISP, as observed)
| Direction | Bytes | Meaning |
|---|---|---|
| host→mcu | `0x43` | sync; ROM replies 1 byte |
| mcu→host | `0x07` | ROM alive / revision |
| host→mcu | 1030 B frame `[hdr 6][payload 1024]` | program one flash page |
| mcu→host | `0x06` | frame ack |
| host→mcu | (erase / read / jump — not yet decoded) | see "open items" |

**Recovery implication (t5) — weakened by the correction above:** the
bootloader lives at flash `0x0` and we **never program it**, so *expected*
behavior is that any app state is recoverable by re-entering the bootloader
(NRST pulse, with GPIO35 held HIGH as IAPCommand does — its role unproven)
and re-flashing the stock image. **It is NOT unerasable** — it is ordinary
flash; the safety net holds only while we keep honoring the
`base > 0x00008000` rule. And recovery has been observed to fail after
repeated flashing (STATUS.md §3 "no sync" state), so treat it as
*probable*, not *guaranteed*.

## 3. MCU flash map (HD board)
| Region | Address | Content |
|---|---|---|
| Vendor bootloader ("ISP") | `0x00000000` (first ~0xC000?) | flash-resident (inferred), **erasable** — we just never write it; owns USART pins while active |
| Boot stub | `0x00000000..0x0000C000` (in the *app window*) | factory "jump/validate" stub — see below |
| HD app | `0x0000C000..` | Klipper / vendor app (reset vector here) |
| NA app | `0x08010000..` | NA (N32) revision image base |

### The boot stub at 0x0 (from `work/hd.bin`)
`work/hd.bin` (64 KB factory flash dump) shows at offset `0x0` a short Thumb
**indirect-call dispatch stub** (NOT a vector table): a series of
`push{r4,lr}; ldr r0,[pc,#0x10]; ldr.w r0,[r0,#0x138..0x1xx]; cbz r0,skip;
ldr r1,[pc,#8]; ldr.w r0,[r1,#0x1xx]; blx r0; pop{r4,pc}` blocks — i.e.
call functions fetched from a struct at fixed offsets (`0x138,0x13C,0x140,...`).
This is the **factory jump vector / mini-bootloader** that runs at reset and
decides whether to execute the app at `0xC000` or drop to ISP. **This is why
Klipper can be "silent": if the stub validates the app (checksum/signature/
size) and Klipper fails validation, the stub never jumps to it** — the MCU
sits in the stub (or re-enters ISP), so the USART is never re-initialised by
Klipper and looks silent. The vector table the CPU actually uses at reset is
therefore the stub's, and the real app vector table (SP/reset) is at `0xC000`
(`SP=0x20002AD8, reset=0xC295`).

> NOTE: `work/hd.bin` is a **64 KB** capture (flash `0x0000..0xFFFF` only),
> not the full flash — the HC32F460 has **512 KB** (`0x0..0x7FFFF`), so ~448 KB
> is still unmapped, including most of the bootloader region. A full dump
> requires SWD (STM32-style ISP reads don't work — see `adv4-swd-dump.md`).

## 4. SoC ↔ MCU wiring (verified)
- SoC = Allwinner V3S (sun8i), kernel 3.4.39+, busybox, armv7l. **No
  `timeout`, no `sftp-server`** (use legacy `scp -O`). SSH root/`flashforge`.
- UART devices: `/dev/ttyS0`, `/dev/ttyS1`, `/dev/ttyS2`. `ttyS1` = uart0 @
  `0x1c28000` (the MCU line).
- **Both the ROM-ISP (`IAPCommand`) and the vendor app use `/dev/ttyS1`.**
  The vendor MCU app is `/opt/PROGRAM/software/2.2.4/adventurer4-arm` (Qt4 GUI,
  `-1 -D -qws`); its serial object string is **`/dev/ttyS1`**. So the app UART
  and the ISP UART are the *same physical SoC pin pair* — confirming the
  bridge is on the right line.
- **Correction 2026-10-20 (session 5): the MCU-side line is PROVEN to be
  USART2, TX=PA9 (FSEL 36), RX=PA10 (FSEL 37), 115200, DMA RX** — decoded
  from the factory app itself (descriptor builder `0x14f64`, link init
  `0x15068`, baud literal `0x1c200` consumed by `USART_SetBaudRate` at
  `0x15144`; FSEL map from HDSC HAL). The earlier claim here — "the only MFP
  pin literal is `0x40053808 = PA2`, so PA2/PA3 is the mapping" — is
  **wrong**: `0x40053800` is the PORT block base and `+0x8` is not a PA2
  function-select register. `HC32F460_SERIAL_PA3_PA2` is therefore NOT the
  vendor link; use `PA9_PA10` (added in our klipper-hc32 fork) to reuse the
  existing SoC wiring.

## 5. Klipper build status (Mac, cross-compiled)
- Toolchain: **xPack arm-none-eabi GCC 14.2.1** (native darwin-arm64, full
  newlib) at `~/.hermes/cache/scratch/xpack/...`. Rebuild with
  `CROSS_PREFIX=<xb>/arm-none-eabi-` and a `cpp` shim (`arm-none-eabi-gcc -E`)
  on PATH so the link script preprocesses.
- `.config` (this early build): `MACH_HC32F460`,
  `FLASH_APPLICATION_ADDRESS=0x00C000`, `SERIAL_PA3_PA2`,
  `SERIAL_BAUD=250000`, `CLOCK_SPEED_200M`. **Superseded:** a 250000 build is
  inaudible on the SoC (uart0 stuck at 115200) and PA3/PA2 is not the vendor
  link — the flashed image (`out/autoconf.h`) was
  `SERIAL_PA9_PA10`/USART2/115200, which matches the proven vendor wiring.
- Output: `out/klipper.elf` (start `0x0000c000`), `out/klipper.bin`, and
  `out/klipper.hex` regenerated with
  `objcopy -I binary -O ihex --change-address=0xC000` (data @ `0xC000`).
- **Flashed to the MCU via `IAPCommand klipper-pa32.hex klipper-pa32.hex`** →
  `STM32Write: success. STM32iap finished HD.` But after release the MCU is
  **silent to Klipper identify** — see boot-stub theory (§3).

## 6. Open items / regressions to fix
- [ ] **Full 512 KB flash dump** — no ISP read path exists (the vendor
      protocol has no decoded read op; `stm32isp-ad4 -d` sends STM32 `0x11`,
      gets nothing, and writes an all-zero file — `mcu-dump-full.bin` is
      junk). **Route: SWD via ST-Link clone — see `adv4-swd-dump.md`.**
      Dumping the bootloader is also the prerequisite for later decoding a
      read op over ISP.
- [ ] **Decode the boot stub at 0x0**: does it validate the app? What
      checksum/signature/size? This is the likely reason Klipper is silent.
- [ ] **Reconcile `stm32isp-ad4.c`**: its STM32 read/erase/write won't match
      the HC32 ROM (sync works, sub-commands don't). Port the 1030-byte-frame
      program + 0x06-ack protocol, and add a real read path.
- [ ] Confirm the app really boots: if the stub is the gate, we may need to
      also write the stub (or a Klipper that passes its check) — or the ROM
      "jump" path that bypasses the stub entirely.

## 2026-10-27 — NA/N32 unit: strap + sync verified live (nasync)

Run from the SoC over WiFi (`192.168.1.219`, dropbear) with the vendor GUI
stopped first (`ffstartup-arm` and `adventurer4-arm`; `wpa_supplicant` only
inherits the GUI's ttyS1 fd and can stay). Tool: `work/n32/nasync` (9dbcccc),
which pulses NRST (GPIO34, 15.68 ms) and sends a single `0x43`, nothing else.

| GPIO35 during reset | result (6 tries / 2 tries) |
|---|---|
| **1** | `0x43` answered with **`0x06`** every time, 72–90 ms after the sync byte; no boot banner in the 300 ms before it |
| **0** | silence (app mode; the app does not answer `0x43`) |

So on this NA board **GPIO35 is the IAP-mode strap** (high + reset = vendor
bootloader answers; low = it hands over). `0x06` = the NA revision byte,
exactly what `stm32isp-ad4.c` expects. (The stick never probed on 10-24: its
`-s` probe is skipped under NO_FLASH — fortunate, see below.)

**Observed afterwards (unresolved):** once the bootloader had been synced,
the MCU answered nothing in app mode — the restarted vendor GUI got no
bytes back (ttyS1 rx counter frozen, port 8899 never opened), and raw
`M119`/`M105`/`M115` on ttyS1 got no reply either; NRST reads high, strap
low. Working hypothesis: the sync alone made the bootloader invalidate the
application, so the next step is a **read-only SWD read-back** of
`0x08010000` (J16, `work/swd/swd-probe.sh`) before deciding what to write.
Transcript: `prodesk:~/ad4-backup/nasync-20261027.log`.

**Resolved 2026-10-27 (SWD read-back, J16):** `0x08010000` reads
`ffffffff ffffffff ffffffff ffffffff`; the core is halted in the bootloader's
HardFault handler (pc `0x080007f8`); bootloader vectors and option bytes
(`a5 5a ff ff`) untouched. **The vendor IAP erases the application as soon as
it is synced** — a bare `0x43` with GPIO35 high is already destructive on this
board. Rules that follow:
- never sync the IAP (nasync, `stm32isp-ad4 -s`, IAPCommand) unless a complete
  image write follows in the same run;
- the stick's boot-time revision probe (`stm32isp-ad4 -s`) is removed; the
  revision comes from an `ad4-rev` file on the stick instead.
Recovery = the vendor's own path: `IAPCommand <NA hex> <HD hex>` on the SoC
(kit staged in `/tmp/iap`, copies in `prodesk:~/ad4-backup`), then SWD compare
against `n32-flash-full-20261027-1942.bin`.

**Restored 2026-10-27 20:37 MDT.** `IAPCommand Adventurer4-NA-IAP-2.3.0-20230216.hex
Adventurer4-HD-IAP-2.3.0-20230216.hex` run from `/tmp/iap` on the SoC with the vendor
GUI stopped (driver: `work/n32/iap-flash.sh`; transcript
`prodesk:~/ad4-backup/iap-stock-restore-20261027.log`): `stm32 reset` → `Connected to
board.` → `Update :NA` → 44 frames (0–43, two `send error. again` auto-resends, frames
3 and 40) → `STM32iap finished NA.`, exit 0, 20 s wall; UART tx +47382 B, rx +47 B;
`/root/N32` touched; GPIO34 left exported out=1, GPIO35 out=0. SWD read-back
`n32-flash-full-20261027-2040.bin` is byte-identical to the pre-incident `…-1942.bin`
(sha256 `ad97e705…`), option bytes `a5 5a ff ff`. After the GUI restart the printer
answered `~M115` (v2.2.4), `~M105`, `~M119` over 8899 and showed temperatures on
screen; still healthy after a power cycle (fresh boot: no `fe`/`pe` on ttyS1).

Side effect worth a rule: **IAPCommand leaves `/dev/ttyS1` in even parity (8E1)** and
`adventurer4-arm` does not reset it when it opens the port, so every byte from the MCU
app (8N1) was counted as a framing or parity error (`/proc/tty/driver/uart`: `fe`+`pe`
grew exactly as fast as `rx`) and text came back corrupted (`Z-m�x:0`). `stty -F
/dev/ttyS1 -parenb` fixed it live; a reboot resets it too (which is why the vendor
update flow reboots). Anything that flashes through IAPCommand and then lets the GUI
or Klipper reopen the port must clear parity first.

Also observed: with the app pages blank the core did not wait in IAP mode — it ended
in the bootloader's own HardFault handler (pc `0x080007f8`, msp `0x200028d8`), i.e.
the bootloader tried to run the blank app. No app validation (header/CRC) was seen;
not proven absent. The IAP path (`iap-flash.sh <klipper hex>`, hex in the NA slot) is
therefore the flash route for this unit; SWD stays read-only verification.

**Klipper flashed 2026-10-27 21:03 MDT.** Same path, Klipper hex in the NA slot:
`sh /tmp/iap/iap-flash.sh klipper-n32g455-020d25e.hex` (mainline `020d25e`, defconfig
`work/n32/klipper-n32g455.defconfig`, app base `0x08010000`) with the vendor GUI stopped.
`Connected to board.` → `Update :NA` → 42 frames (0–41, no resends) → `STM32Write: success.`
→ `STM32iap finished NA.`, exit 0, 20 s wall; UART tx +45322 B, rx +64 B (all counted as
`fe`/`pe` because the port was 8E1, same as during the stock run). Then `stty -F /dev/ttyS1
-parenb`, `ad4-bridge 5000 /dev/ttyS1 115200` on the SoC, and
`work/n32/klipper-identify.py 192.168.1.219:5000` from prodesk: the MCU answers Klipper's
`identify` (first block NAKed back to seq 0, as a fresh MCU does), 3821 compressed bytes in
2.2 s with no retransmits → version `020d25e`, MCU `stm32f103xe` (Klipper's name for the
N32G455 target), `CLOCK_FREQ 144000000`, `SERIAL_BAUD 115200`, 116 commands / 25 responses
(`work/n32/klipper-n32g455-020d25e.identify.json`). Idle, the MCU emits a 13-byte
`stats count=… sum=… sumsq=…` block every 5 s — Klipper's normal heartbeat, so `rx` grows
with nobody connected.

The vendor kit (IAPCommand + both hexes, md5s identical to `~/ad4-backup`) also lives
permanently at `/opt/PROGRAM/control/2.3.0/` on the SoC, so a stock restore does not depend
on `/tmp` or the stick: kill `adventurer4-arm ffstartup-arm`, then
`cd /opt/PROGRAM/control/2.3.0 && ./IAPCommand Adventurer4-NA-IAP-2.3.0-20230216.hex
Adventurer4-HD-IAP-2.3.0-20230216.hex`, then `stty -F /dev/ttyS1 -parenb` (or reboot).

SWD read-back of the Klipper image is still owed: the first openocd run after the flash failed
at the USB layer (`reset device failed`, then `open failed`) — the usbipd attach had gone stale,
and `usbipd detach` dropped the share, which had never been persisted with `bind`. Re-run
`usbipd bind --busid 1-2` as administrator on the HP, then `swd-dump.sh` and
`cmp <(dd if=<dump> bs=1 skip=65536 count=42172) work/klipper-n32g455-020d25e.bin`.
