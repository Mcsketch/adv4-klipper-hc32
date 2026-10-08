# AD4 ROM-ISP Protocol + Boot Chain — Verified Findings (strace ground truth)

Source: `strace -f -e trace=openat,write,read,nanosleep,ioctl,close` on the
factory `/opt/ad4-klipper/IAPCommand` while it flashed the stock HD image,
plus control-flow analysis of `work/hd.bin` (a 64 KB factory flash dump) and
the `stm32isp-ad4/` source. This supersedes the unverified protocol claims in
the older `.md` files.

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
  Confirmed live: **gpio34 = NRST** (value=1 after flash), **gpio35 = BOOT0**
  (value=0 after flash) — both exported, direction out.

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

**Recovery implication (t5):** because the ROM ISP lives at flash `0x0` and is
**never programmed**, *any* app state is recoverable by re-entering ISP
(BOOT0 HIGH + NRST pulse) and re-flashing the stock image. We do NOT need the
ROM bytes back — they are unerasable. This is the no-IAP-brick safety net.

## 3. MCU flash map (HD board)
| Region | Address | Content |
|---|---|---|
| ROM ISP | `0x00000000` (first ~0xC000?) | unerasable bootloader; owns USART pins |
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

> NOTE: `work/hd.bin` is a **64 KB** capture (only the app window 0x0–0xFFFF),
> not the full 256 KB flash. A full dump is required to map the ROM ISP region
> and the whole stub.

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
- The MCU side pin for that line: factory app MFP references **PA2** (the only
  MFP pin literal in the app: `0x40053808 = PA2`). On HC32F460, PA2/PA3 are a
  standard UART1 RX/TX pair. **User has confirmed PA2/PA3 is the correct
  mapping** → Klipper `HC32F460_SERIAL_PA3_PA2` is the right choice.

## 5. Klipper build status (Mac, cross-compiled)
- Toolchain: **xPack arm-none-eabi GCC 14.2.1** (native darwin-arm64, full
  newlib) at `~/.hermes/cache/scratch/xpack/...`. Rebuild with
  `CROSS_PREFIX=<xb>/arm-none-eabi-` and a `cpp` shim (`arm-none-eabi-gcc -E`)
  on PATH so the link script preprocesses.
- `.config`: `MACH_HC32F460`, `FLASH_APPLICATION_ADDRESS=0x00C000`,
  `SERIAL_PA3_PA2`, `SERIAL_BAUD=250000`, `CLOCK_SPEED_200M`.
- Output: `out/klipper.elf` (start `0x0000c000`), `out/klipper.bin`, and
  `out/klipper.hex` regenerated with
  `objcopy -I binary -O ihex --change-address=0xC000` (data @ `0xC000`).
- **Flashed to the MCU via `IAPCommand klipper-pa32.hex klipper-pa32.hex`** →
  `STM32Write: success. STM32iap finished HD.` But after release the MCU is
  **silent to Klipper identify** — see boot-stub theory (§3).

## 6. Open items / regressions to fix
- [ ] **Full 256 KB flash dump** (need a working HC32-protocol read — the
      STM32-style `stm32isp-ad4 -d` cannot; implement the HC32 read op).
- [ ] **Decode the boot stub at 0x0**: does it validate the app? What
      checksum/signature/size? This is the likely reason Klipper is silent.
- [ ] **Reconcile `stm32isp-ad4.c`**: its STM32 read/erase/write won't match
      the HC32 ROM (sync works, sub-commands don't). Port the 1030-byte-frame
      program + 0x06-ack protocol, and add a real read path.
- [ ] Confirm the app really boots: if the stub is the gate, we may need to
      also write the stub (or a Klipper that passes its check) — or the ROM
      "jump" path that bypasses the stub entirely.
