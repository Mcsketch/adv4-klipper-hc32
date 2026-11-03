import re, struct, sys
from collections import Counter, defaultdict
d = open("na.bin", "rb").read(); B = 0x08010000
G = {0x40010800: "PA", 0x40010C00: "PB", 0x40011000: "PC", 0x40011400: "PD"}
ins = []
for l in open("na.dis"):
    m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f ]+\s+\t(\S+)\s*(.*)", l)
    if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3)))

def lit(addr):
    o = addr - B
    return struct.unpack_from("<I", d, o)[0] if 0 <= o <= len(d) - 4 else None

def func_of(i):
    # nearest preceding push = function start
    for j in range(i, max(i - 400, 0), -1):
        if ins[j][1].startswith("push") or ins[j][1] == "stmdb": return hex(ins[j][0])
    return "?"

calls = Counter(); rows = []
for i, (a, op, arg) in enumerate(ins):
    m = re.match(r"r0, \[pc, #\d+\]\s+@ \(0x([0-9a-f]+)\)", arg)
    if not (op == "ldr" and m): continue
    v = lit(int(m.group(1), 16))
    if v not in G: continue
    r1 = None; tgt = "-"
    for j in range(i + 1, min(i + 8, len(ins))):
        a2, op2, arg2 = ins[j]
        mm = re.match(r"r1, #(\d+)", arg2)
        if op2.startswith("mov") and mm: r1 = int(mm.group(1))
        if op2 in ("bl", "b.w", "b"): tgt = arg2.split()[0]; break
    pins = [k for k in range(16) if r1 is not None and r1 >> k & 1]
    rows.append((hex(a), func_of(i), G[v] + ",".join(map(str, pins)), r1, tgt)); calls[tgt] += 1
for r in rows: print(*r)
print(calls)
