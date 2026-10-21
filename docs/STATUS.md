# AD4 → Klipper (HC32F460) — STATUS (session 2026-10-12)

Working dir: `/Users/admin/adv4-klip` (git repo, **no remote configured**)
Klipper src : `/Users/admin/klipper-hc32`
Toolchain   : `arm-none-eabi-gcc 14.2.1` @ `/Users/admin/toolchains/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin`

## ✅ What is DONE and verified this session

### 1. Root cause of the 3-day "no serial reply" loop — FOUND and fixed
Every "115200 build" since 2026-10-08 was actually **250000**. Setting
`CONFIG_SERIAL_BAUD=115200` in `.config` was silently ignored by the vendored kconfiglib:
`SERIAL_BAUD`'s prompt is gated behind `LOW_LEVEL_OPTIONS` (default off) → symbol resolves
invisible → `str_value` falls back to `default 250000` even though `user_value='115200'` is
stored (and `set_value()` returns True). Klipper's `kconfiglib.patch` forces the symbol to be
*written* to `autoconf.h` but does **not** fix the value, and `genconfig.py` even *skips the
write* (no content change) so `out/autoconf.h` keeps a stale mtime — invisible to any freshness
check. Full write-up + repro in `KLIPPER-AD4-FLASH-GUIDE.md` (§0, the 🚨 PITFALL box).

**Fix:** enable `LOW_LEVEL_OPTIONS=y` in `.config` (makes the expert option visible so its value
is honored). Both `LOW_LEVEL_OPTIONS=y` and `SERIAL_BAUD=115200` now survive `make olddefconfig`.

### 2. Real 115200 Klipper build — COMPILED and verified
- `out/autoconf.h`: `#define CONFIG_SERIAL_BAUD 115200`, `HC32F460_SERIAL_PA9_PA15 1`,
  `CLOCK_FREQ 200000000`, `FLASH_APPLICATION_ADDRESS 0x00C000` ✅
- `make clean && make -j8 CPP=arm-none-eabi-cpp` → **RC=0**, `out/klipper.bin` = 27832 B
- **Build note (macOS):** host `/usr/bin/cpp` is a clang wrapper that fails with
  `cc: error: no input files` on the `.lds.S` linker-script step. Override `CPP=arm-none-eabi-cpp`.
- `check-gcc.sh` readelf warnings are harmless (that check only; .bin/.elf still produced).

### 3. Ready-to-flash positioned hex (grounded to 0xC000, checksums valid)
| artifact | bytes | sha256 |
|---|---|---|
| `flash-images/klipper-115200-real.hex` (project) | 76588 (1743 records) | `a90f39ce5e20d0bbef35ba88b9489eeb16e89c909965be85e73164ed6c905427` |
| staged on SoC: `/root/klipper-115200-real.hex` | 76588 | `a90f39ce5e20d0bbef35ba88b9489eeb16e89c909965be85e73164ed6c905427` |
| `out/klipper.bin` (raw) | 27832 | (source of the hex) |

Validator `scratch/validate_ihex.py`: **ALL CHECKSUMS VALID** (two-complement / factory standard),
type-04 `:020000040000` base, 1742 data records + 1 end record.

### 4. Flasher (open-source fork) — READY
- `stm32isp-ad4/stm32isp-ad4-fork` (Mac) sha256 `47d03eaf51da21244e9c5af8ddbac56d0920aa95c601603787a446b7c4672999`
- Same binary deployed on the SoC at `/root/stm32isp-ad4-fork` (sha256 matches).
- Correct sync-wait fix present (does **not** require the reply this HC32 MCU never sends).
- Also present on SoC: vendor `/root/IAPCommand` (the factory tool, proven to flash this MCU).

## ⏸ BLOCKED — needs you (a physical action I cannot do)

**The MCU is in the "fatigued state" and will not answer ROM-ISP sync.**
`/root/ad4_diag` (GPIO strapping + 0x43 sync probe) **hung 90 s** → the MCU did not enter ISP.
Per the established notes, this happens after many NRST cycles and is cleared **only by a
physical power cycle**. Repeated NRST pulses make it *worse*, so I stopped probing.

**To unblock:** fully power-cycle the AD4 (unplug + hold power button until the capacitors drain,
replug). Then:

```sh
# on the SoC (or via SSH root@192.168.10.10), as root:
# stop the vendor daemon holding /dev/ttyS1 first:
systemctl stop <vendor-daemon>      # identify: fuser /dev/ttyS1
# flash the REAL 115200 Klipper (open-source fork):
/root/stm32isp-ad4-fork -f /root/klipper-115200-real.hex /dev/ttyS1
# (or the proven vendor tool:)  /root/IAPCommand /root/klipper-115200-real.hex /dev/ttyS1 115200
```

Then run the **whole chain** (this is the actual acceptance test):
1. `ssh ... 'stty -F /dev/ttyS1 115200; dd bs=1 count=1 if=/dev/zero > /dev/ttyS1'`  # trigger identify
2. read `/dev/ttyS1` → expect the **~1535-byte Klipper `identify`** (0xD0 0x00 0x00 0x06 …).
   If it's 0 B / garbage → baud mismatch (it's not now) or MCU not running Klipper.
3. If the bridge is up: Mac pyserial → socat PTY → (SoC bridge) → /dev/ttyS1 → same identify.

## 📚 Doc correction job — DONE (this file + flash guide)
- `KLIPPER-AD4-FLASH-GUIDE.md`: added the 🚨 PITFALL box (baud silent-fallback root cause + fix + verify).
- This STATUS.md: state, hashes, and the exact next step.

## ⚠ "Update github" — CANNOT PUSH
`git remote -v` → **empty**. The repo has local history (latest commit
`c3478df stm32isp-ad4: port to HC32 vendor 1030B IAP protocol…`) but no remote to push to.
The open-source flasher fork lives at `/Users/admin/adv4-klip/stm32isp-ad4/` (see
`stm32isp-ad4/flash-mcu.sh` for the SoC unattended flash path). Give me a GitHub URL and I will
`git remote add origin …` + push.
