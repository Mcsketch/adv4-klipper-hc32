# flashforge_init.sh — debug notes (no-python rewrite, 2026-10-06)

## "Nothing happens" — root cause
1. **Wrong filename**: the build script is `build-usb-stick.sh`, NOT
   `build-USB-disk.sh`. Running the latter is a no-op ("No such file").
2. **python3 is NOT on the SoC** (verified against the eMMC rootfs dump:
   0 occurrences of "python3"). The original `check_base()` called
   `python3` — on a machine with no python3 the image check failed and the
   script **exited before flashing**, writing only `flash-refused` to
   `/root/ad4-flash-status` (invisible without a console).
3. **No console** (no USB-serial adapter) meant there was nothing to see.
   → Workaround: **`telnet <soc-ip> 23` is a password-less root shell**
   (`/etc/init.d/telnetd` → `start-stop-daemon ... /usr/sbin/telnetd -F`,
   no `-l LOGIN`). SoC vendor net is 192.168.0.20 (not 192.168.1.x).

## Buzzer = PWM piezo, NOT a wav file
- There are **no .wav files** in the rootfs and `aplay` is only in busybox's
  builtin table (never invoked). The boot/print-end "sounds" are **beep
  tone sequences**: the vendor GUI's `buzzer/buzzer.cpp` → `buzzerPlay` →
  `PlayerThreadHandler voice`, driven by a **PWM sysfs node**
  (`/sys/class/pwm/pwmchip0/pwm0/{export,period,duty_cycle,enable}`), fired
  on events (`setPrinterReady`, `~BuildPrint`, button presses, page
  open/close). Note: the vendor itself logs `pwm_fd export error:` on some
  boots — so the PWM node may need the `export` write (which our `pwm_setup`
  does) before `period/duty/enable` are usable.
- Our `flashforge_init.sh` drives the SAME node directly (no vendor code
  touched): `pwm_setup` finds a usable pwm dir (exports pwm0 if needed),
  then `chime start|ok|fail` writes period/duty(=period/2)/enable:
  - **start** = 1 short 1.5 kHz blip
  - **ok**    = rising two-tone "ding" (2.5 kHz → 3.5 kHz)
  - **fail**  = 3 low 1 kHz buzzes
  If no PWM node is found it logs "sound skipped" and continues (flash is
  already done by then; sound is a bonus). `PWM_BASE` is overridable for
  testing. Verified against a fake sysfs tree: correct period/duty/enable.

## What changed
### flashforge_init.sh (fully rewritten)
- **No python3.** `check_base()` is now pure **awk** (portable mawk form —
  see pitfall below). The bridge step runs **only if python3 exists**,
  otherwise it logs "bridge SKIPPED" and continues (flash never needs python).
- **Full logging to the STICK.** Every step is timestamped and flushed to
  `/mnt/ad4-flash.log` (the stick is mounted at `/mnt` during the run).
  Pull the stick, read `ad4-flash.log`. Also mirrored to `/root/ad4-flash.log`
  and a one-line `/root/ad4-flash-status`.
- **Evidence first.** The log opens with `uname -m/-r`, `/proc/version`,
  `/proc/cpuinfo`, a **tool census** (python3/awk/ip/... present or MISSING),
  and the **stick inventory** (every expected file present + byte size).
  So even if it bails, the log shows it ran and what the SoC actually has.
- **Soft kernel check.** Arch (`armv7l`) is a hard stop (the flasher binary
  can't run otherwise). Kernel is a **warning**, not a hard stop — a
  near-mismatch (3.4.39 vs 3.4.39+) must not hide the real cause.
- **Tolerant process tools.** `pkill_safe`/`pid_alive` fall back to
  `killall` then a `/proc` scan (busybox may lack pkill/pgrep).
- **Open-source flasher preferred.** If `stm32isp-ad4` (our C tool) is on
  the stick it is used with `-v` (verbose: connect→PID→erase→write→verify→
  finished); otherwise it falls back to the factory `IAPCommand`. Both are
  shipped on the stick.

### build-usb-stick.sh
- Copies the **open-source** `stm32isp-ad4` (static ARM) onto the stick and
  verifies it is an ARM ELF; still verifies the factory `IAPCommand` md5.

### stm32isp-ad4/stm32isp-ad4.c (the open-source flasher)
- Added `#define _POSIX_C_SOURCE 200809L` + `_DEFAULT_SOURCE` + `#include
  <strings.h>` so it compiles under **musl** (zig) as well as glibc
  (`usleep`, `cfmakeraw`, `strcasecmp` were previously implicit-declared).
- Cross-compiled to a **static, stripped, soft-float ARMv7** binary
  (`zig cc -target arm-linux-musleabi -std=c11 -O2 -Wall -static`), 43 KB,
  **0 FPU instructions** → safe on the soft-float SoC. Shipped at
  `flashdisk/stm32isp-ad4`. sha256: c7876875d3c49bdb0f5eccf9dc2378041cb68ed8bde0768a8bb374c03d8fc14f.

## mawk pitfall (the subtle bug)
The SoC's awk is mawk (20200816 class). Two things break naive awk under it:
- **Comparison ops must be space-delimited.** `x!":"` and `x!=":"` are
  mis-tokenized and silently misbehave; `x != ":"` is required.
- **No `<<` bit-shift** (and mawk is picky about tight packing in general).
  Use `*65536` / `*16` instead. Type-04 and type-05 iHEX records are both
  `(value << 16)`.
Verified: `klipper.hex` → base `0x0000C000` ok=1; `factory-HD.hex` → base
`0x0000C000` ok=1 (it is the HD app-window image, not a full-chip dump).

## How to read the result (no console)
1. `./build-usb-stick.sh /Volumes/YOURSTICK`
2. Plug stick in, power the printer.
3. Wait ~30 s, unplug the stick.
4. Read `/Volumes/YOURSTICK/ad4-flash.log` — full timeline of what happened.
   `ad4-flash-status` is the one-line verdict.
