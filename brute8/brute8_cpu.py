#!/usr/bin/env python3
# brute8_cpu.py — CPU worker for the AD4 root-password DES sweep.
#
# Enumerates a slice of the 8-char [a-z0-9] "digit-start" space
# (first char in 0-9, rest in a-z0-9), checks each against the target
# traditional-DES (crypt) hash, and writes the plaintext to a shared
# result file if found.
#
# Usage: brute8_cpu.py <start> <count> <result_file> <log>
#   start  : first global index in the digit-start space (>= 0)
#   count  : number of candidates this worker covers
#
# The digit-start space: value v in [0, 10*36^7)
#   first_char = "0123456789"[ v // 36^7 ]      (0-9)
#   rest       = base36(v % 36^7) -> 7 chars    (a-z0-9)
#
# DES crypt uses only the first 8 chars, and 6-char + word+digit spaces
# are already fully exhausted (no match), so this 8-char digit-start slice
# (run 10/36 alongside hashcat GPU on the 26/36 letter-start slice) plus
# the GPU 7-char pass covers 100% of the remaining random-alphanumeric space.
import crypt, sys, time, os

CHARSET = "abcdefghijklmnopqrstuvwxyz0123456789"  # index 0-35
NP = 36
SEVEN = NP ** 7          # 78,364,164,096
FIRST_LO = 26            # digit-start: chars 26..35 of CHARSET = "0123456789"
FIRST_HI = 36            # exclusive
TARGET = "H9.zxVgLFpRG2"
SALT = TARGET[:2]

def base36_7(x):
    """x in [0, 36^7) -> 7-char string over CHARSET (little-endian digit order)."""
    out = []
    for _ in range(7):
        out.append(CHARSET[x % NP]); x //= NP
    return "".join(reversed(out))

def main():
    start = int(sys.argv[1]); count = int(sys.argv[2])
    result_file = sys.argv[3]; log = sys.argv[4]
    f = open(log, "a")
    t0 = time.time()
    n = 0; next_mark = 20_000_000
    for r in range(start, start + count):
        c0 = CHARSET[FIRST_LO + (r // SEVEN)]      # '0'..'9'
        cand = c0 + base36_7(r % SEVEN)
        n += 1
        if crypt.crypt(cand, SALT) == TARGET:
            with open(result_file, "w") as rf:
                rf.write(cand + "\n")
            f.write(f"[{time.strftime('%H:%M:%S')}] FOUND {cand} (r={r})\n")
            f.close(); return
        if n >= next_mark:
            pct = 100.0 * (r - start) / max(1, count)
            f.write(f"[{time.strftime('%H:%M:%S')}] {n:,}/{count:,} ({pct:.2f}%, {n/(time.time()-t0):,.0f}/s)\n")
            f.flush()
            next_mark += 20_000_000
    f.write(f"[{time.strftime('%H:%M:%S')}] DONE {n:,}\n"); f.close()

if __name__ == "__main__":
    main()
