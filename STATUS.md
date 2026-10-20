# AD4 → Klipper (HC32F460 HD) — STATUS

_Last updated: 2026-10-20 (session 5 annotations over session 4 report)._

## 🔬 Session 5 (2026-10-20): factory firmware RE — link PROVEN, dump diagnosis

Reverse-engineered the factory HD app
(`FlashForge-Hacks-main/Firmware/Adventurer4-2.2.4-2.3-20230216/control-2.3.0`,
app at flash `0xC000`, disassembled force-Thumb).

- **SoC↔MCU link, PROVEN from firmware**: **USART2** (base `0x4001d400`),
  **TX = PA9 (FSEL 36), RX = PA10 (FSEL 37), baud 115200**, DMA-driven RX.
  Descriptor builder at `0x14f64` (baud literal `0x1c200` at `0x14f74`),
  link init at `0x15068` calls `USART_Init` (`0x13b80`) then
  `USART_SetBaudRate` (`0x13b18`) with `115200`. FSEL numbers confirmed against
  HDSC HAL (`klipper-hc32/lib/hc32f460/driver/inc/hc32f460_gpio.h`:
  `Func_Usart2_Tx=36`, `Func_Usart2_Rx=37`). Matches the live
  `/dev/ttyS1 @ 115200` observation — **the pin/baud hypothesis for the silent
  klipper is ruled out** (`out/autoconf.h` shows the flashed hex was built
  `PA9_PA10`/USART2/115200; the "`PA9_PA15`" mention below is a typo).
- **`HC32F460_SERIAL_PA13_PA14` klipper variant = hallucination**: PA13/PA14
  are SWDIO/SWCLK with no USART alternate function; the vendor link is not on
  them. That variant's hex was silent — expected.
- **`-d` dump can never work over ISP**: `st_read` sends STM32 ROM-ISP
  `CMD_READ 0x11`, which the vendor 1030 B bootloader does not speak → all
  reads time out → the tool writes the zero buffer anyway.
  `mcu-dump-full.bin` (256 KB of `00`) is worthless. Full dump requires SWD →
  see new doc **`adv4-swd-dump.md`** (ST-Link clone on PA13/PA14 + SoC GPIO34
  NRST; HC32F460 has no ROM ISP — the "ISP" is a flash bootloader, inferred at
  `0x0..0xBFFF`).
- **GPIO35 = "BOOT0" remains NOT PROVEN** (see corrections below).

## ✅ Verified this session (real output, no guesses) — session 4

### 1. Open-source flasher — flashes + full acks (tested twice). **No signs of
life from the flashed app yet — "works" = write path only, execution unproven**
Binary `stm32isp-ad4-fork` (Mac + SoC, sha256 **`47d03eaf…2999`**,
source `stm32isp-ad4/stm32isp-ad4.c`, Makefile in same dir).

Command (on the SoC, as root):
```
/root/stm32isp-ad4 -r hd -p /dev/ttyS1 -v /opt/ad4-klipper/NA.hex /root/klipper-115200-real.hex
```
Real output (both runs):
```
Connected to board. (sync reply 0x07 = HD/H32)
loaded /root/klipper-115200-real.hex: 27832 bytes @ 0x0000c000 .. 0x00012cb7
programming 27832 bytes via vendor 1030B frames (auto-erase, header carries addr)
all frames acked 0x06 -> write complete
STM32iap finished HD.
```
Second run with `/opt/ad4-klipper/klipper-pa915-debugoff.hex` (27864 B):
`all frames acked 0x06 -> write complete`.

**Answer to "can our stm32isp fork flash the printer now?" → it writes and
gets full acks; whether the written app ever runs is UNPROVEN** (every probe
of a flashed custom app has returned 0 bytes — see §3). It straps
GPIO35 (claimed "BOOT0" — **not proven**; only GPIO34=NRST is confirmed from
IAPCommand) + NRST (GPIO34) itself, syncs the bootloader at 115200, erases via
the vendor 1030-byte packets, and programs. No USB stick, no vendor GUI, no
`IAPCommand` needed (that tool remains as an independent cross-check).
Note: "ROM-ISP" is a misnomer here — the HC32F460 has no ROM ISP; the
bootloader answering the sync is flash-resident (inferred at `0x0..0xBFFF`).

### 2. Bridge — WORKS, and the stale-250k bug is FIXED
- **Bug found:** the deployed `/opt/ad4-klipper/ad4-bridge` (old sha
  `be87332e…`) hardcoded 250000; the current source
  (`soc-hack/ad4-bridge.c`) defaults to 115200 and is correct.
- Rebuilt + redeployed: new sha256 **`2326a814…b5dbd`**, now running:
  `ad4-bridge: TCP :5000 <-> /dev/ttyS1 @115200 (static, no-python)`,
  listening on 0.0.0.0:5000 (netstat-confirmed).
- **Whole chain tested from the Mac:** `python3` socket →
  `TCP 192.168.10.10:5000` → bridge → `/dev/ttyS1` — the IDENTIFY frame was
  delivered (0 B back, see §3). socat PTY path (`ad4-mac-start.sh`) unchanged.
- `flashdisk/ad4-daemon.sh` was still launching the bridge with an explicit
  `250000` arg — **fixed to 115200** in both the C and python fallback branches
  (repo + synced to `/opt/ad4-klipper/ad4-daemon.sh`, sha `850e88e9…`).
  Note: `/opt/ad4-klipper/enable` is **absent**, so the daemon is not
  auto-starting at boot right now; the bridge was started manually this session.

### 3. Klipper on the MCU — FLASHED, currently SILENT (MCU fatigue)
- Flash contents: `klipper-115200-real.hex` (27832 B app @0xC000, built
  2026-10-12, `CONFIG_SERIAL_BAUD 115200`,
  `HC32F460_SERIAL_PA9_PA10`/USART2 (session-5 note: `out/autoconf.h` says
  `PA9_PA10`; the "PA9_PA15" written here earlier was a typo),
  app base 0x0000C000 — all confirmed in `out/autoconf.h`).
  Session-5 RE confirms PA9/PA10/USART2/115200 is **exactly** the vendor
  link, so pin/baud mismatch is ruled out as the silence cause.
- Probes this session (all real):
  - IDENTIFY @115200: 20× storm → **0/20 replies**
  - IDENTIFY @250000: 6× + 30× → **0 replies**
  - ROM sync @115200: answered **0x07** earlier (that's what the flasher used)
  - ROM sync @250000 (`ad4_diag` phase C): answered **0x07** as well
  - After ~20 reset cycles (diags + flashes): **no ROM sync, no app, nothing**
- Verdict (session 4): "fatigue state". **Session-5 correction: this is a
  HYPOTHESIS, not a documented state** — "MCU stops answering ISP after many
  NRST cycles, only a power cycle clears it" was never independently
  reproduced; an equally consistent reading is that the bootloader itself
  needs the SoC's exact reset sequence (or a GPIO35 level) to answer again,
  or that repeated partial flashes left it in a non-syncing state. Treat
  "fatigue" as unconfirmed until a power cycle demonstrably restores sync.

## ⏸ BLOCKED on you (physical action)
**Power-cycle the AD4:** unplug, hold the power button ~10 s to drain caps,
replug. Then from the Mac:

```bash
# 1. make sure the bridge is up on the SoC (daemon is not enabled right now):
sshpass -p flashforge123 ssh root@192.168.10.10 \
  'nohup /opt/ad4-klipper/ad4-bridge 5000 /dev/ttyS1 >/opt/ad4-klipper/ad4-bridge.log 2>&1 & sleep 1; netstat -ltn | grep :5000'

# 2. probe straight over the bridge (expect the ~1500 B Klipper identify):
python3 - <<'EOF'
import socket,time
s=socket.create_connection(("192.168.10.10",5000),timeout=5); s.settimeout(8)
s.sendall(b'IDENTIFY\x00\xb3\x9f'); time.sleep(0.3); s.sendall(b'IDENTIFY\x00\xb3\x9f')
end=time.time()+8; got=b""
while time.time()<end:
    try: d=s.recv(4096)
    except socket.timeout: break
    if not d: break
    got+=d
print(f"{len(got)} bytes:", got[:48].hex(' '))
EOF

# 3. if bytes came back, run the full Mac chain:
/Users/admin/adv4-klip/soc-hack/ad4-mac-start.sh   # socat PTY + klippy
tail -f /Users/admin/klipper-hc32/klippy.log
```
If it's still silent after a clean power cycle, the next suspect is the
app image itself (vector table / clock init), and we verify by re-flashing
`/root/factory-HD.hex` (factory app) and checking it answers — that isolates
image vs hardware.

## 📚 Doc corrections — DONE
- `flashdisk/ad4-daemon.sh`: bridge baud 250000 → **115200** (both branches).
- This file: rewritten from verified outputs; stale "250000 default" removed.
- `KLIPPER-AD4-FLASH-GUIDE.md`: baud facts were correct (115200, PITFALL box,
  base 0x0000C000), but session 5 corrected its protocol/boot claims:
  "ROM-ISP/BOOT0" framing → flash-resident bootloader + unproven GPIO35;
  "standard STM32F1 ISP protocol" → vendor 1030 B frames; sync mapping
  swapped to `0x06`=NA / `0x07`=HD; flash size 256→512 KB; `PA9_PA15`
  typo → `PA9_PA10`; `IAPCommand` two-hex-arg usage noted.
- `klipper-hc32/src/hc32f460/serial.c`: `PA13_PA14` variant comments
  rewritten as **DISPROVEN / DO NOT USE**; `PA9_PA10` comment marked
  PROVEN (firmware RE).
- `soc-hack/ad4-bridge.c` header: already documents the 24 MHz uart0 /
  115200 constraint.

## 📦 GitHub — needs your call
`git remote -v` is **empty** and `gh auth status` = **not logged in**, so
"update github" cannot be done by me right now. Local repo is clean except
the in-flight edits (all committed once you pick one):
1. Give me a repo URL + I'll `git remote add origin …` and push after you
   `gh auth login` (or add a token), or
2. Point me at an existing fork you want this pushed to.

Pending commits ready to make: bridge redeploy note, daemon baud fix,
`soc-hack/ispfresh.c` + `app_probe.c` (diagnostic tools), STATUS.md.

## Key artifacts
| Item | Location | sha256 (first 8) |
|---|---|---|
| Flasher (Mac + SoC) | `stm32isp-ad4/stm32isp-ad4-fork`, `/root/stm32isp-ad4` | 47d03eaf |
| Klipper hex on SoC | `/root/klipper-115200-real.hex` | 27832 B @0xC000 |
| Bridge binary (new) | `/opt/ad4-klipper/ad4-bridge` | 2326a814 |
| Daemon (fixed) | `/opt/ad4-klipper/ad4-daemon.sh` | 850e88e9 |
| Fresh-pass diag tool | `/root/ispfresh` (src `soc-hack/ispfresh.c`) | — |
