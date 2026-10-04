# AD4 root-password DES brute — GPU + all CPU cores

Cracks the AD4 SoC root password `H9.zxVgLFpRG2` (traditional DES / `crypt(3)`,
8-byte max) using **the M1 Ultra GPU (hashcat mode 1500) + all 20 CPU cores**,
in parallel, with a **safe pause** so you can still use the Mac.

> Why this works: hashcat 7's homebrew bottle ships the DES module as **mode
> 1500** (NOT 1200 — that's missing from the bottle). Verified by cracking a
> known DES hash. GPU measured **~12.3M/s**; CPU ~216K/s/core → ~4.3M/s at 20.

## Coverage — 100% of the remaining random `[a-z0-9]` space
Already exhausted with no match: 1–5 char, 6-char exhaustive (2.18B), and
170K-words × digits (17B). What's left is 7–8 char random alphanumeric:

| Engine | Space | Size | Rate | Time |
|---|---|---|---|---|
| GPU | 7-char all `[a-z0-9]` | 78,364,164,096 | ~12.3M/s | **~1.8 h** |
| GPU | 8-char LETTER-start (`?1?2..?2`, pos1=a–z) | 2,037,468,266,496 | ~12.3M/s | ~2.0 d |
| 20 CPU | 8-char DIGIT-start (pos1=0–9) | 783,641,640,960 | ~4.3M/s | ~2.1 d |

GPU runs 7-char then 8-char-letter **sequentially** (full rate each); the CPU
workers run the 8-char-digit slice in parallel. The two 8-char slices are
disjoint and together with 7-char cover 100% of the 7–8 char `[a-z0-9]` space.
**Both engines finish together in ~2.1 days** (50% shot ≈ ~1 day).

## Launch it yourself (Hermes + LM Studio closed, machine idle)
```sh
cd ~/adv4-klip/brute8
./launch-brute8.sh            # starts GPU(7→8 char) + 20 CPU workers
```

## Control (safe pause — state stays in RAM, zero progress lost)
```sh
./launch-brute8.sh status     # live progress + CPU/GPU
./launch-brute8.sh pause      # SIGSTOP everything (GPU+CPU -> ~0%, use your Mac)
./launch-brute8.sh resume     # SIGCONT (exactly where it stopped)
./launch-brute8.sh stop       # hard stop
```
`pause` is the emergency lever: freeze, do whatever, `resume` later. The
processes just stop; nothing is recomputed.

## Result
A found password is written to **`FOUND.txt`** (by GPU and CPU in parallel) and
the run self-stops via the monitor. Watch it with:
```sh
./launch-brute8.sh status
```

## Files
- `launch-brute8.sh` — orchestrator (start/pause/resume/stop/status)
- `brute8_cpu.py` — CPU worker (enumerates a slice of the 8-char digit-start
  space; verified bijection — no gaps, no overlaps)
- `pids`, `FOUND.txt`, `gpu7.log`, `gpu8.log`, `cpu_w*.log` — runtime artifacts

## Note on an 8-char password that is mixed-case or has symbols
DES `crypt` truncates to 8 bytes, but if the vendor password were
`[a-zA-Z0-9]` 8-char (218 T candidates) it is **not CPU-feasible** on this
machine (~2+ years). This kit covers the realistic random `[a-z0-9]` case. If
it fully exhausts with no match, the practical fallback is the one-stick
`chpasswd` (root-by-construction) — see `../ALL-FROM-PI.md`.
