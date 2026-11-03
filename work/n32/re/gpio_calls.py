import re, struct
d = open("na.bin", "rb").read(); B = 0x08010000
G = {0x40010800: "PA", 0x40010C00: "PB", 0x40011000: "PC", 0x40011400: "PD"}
MODE = {0x0: "AIN", 0x04: "IN_FLOAT", 0x28: "IPD", 0x48: "IPU", 0x14: "OUT_OD", 0x10: "OUT_PP", 0x1C: "AF_OD", 0x18: "AF_PP"}
FN = {0x8011c0c: "Init", 0x8011d22: "Read", 0x8011d34: "Reset", 0x8011d38: "Set"}
ins = []
for l in open("na.dis"):
    m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f ]+\s+\t(\S+)\s*(.*)", l)
    if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3)))

def lit(addr):
    o = addr - B
    return struct.unpack_from("<I", d, o)[0] if 0 <= o <= len(d) - 4 else None

def pins(mask):
    return ",".join(str(k) for k in range(16) if mask is not None and mask >> k & 1) if mask is not None else "?"

func = None; R = {}; S = {}
for a, op, arg in ins:
    if op.startswith("push") or op == "stmdb":
        func = a; R = {}; S = {}
    m = re.match(r"(r\d+|ip|lr), \[pc, #\d+\]\s+@ \(0x([0-9a-f]+)\)", arg)
    if op in ("ldr", "ldr.w") and m:
        R[m.group(1)] = lit(int(m.group(2), 16)); continue
    m = re.match(r"(r\d+), #(\d+)", arg)
    if op in ("movs", "mov", "mov.w", "movw") and m:
        R[m.group(1)] = int(m.group(2)); continue
    m = re.match(r"(r\d+), (r\d+)$", arg)
    if op in ("mov", "movs") and m:
        R[m.group(1)] = R.get(m.group(2)) if m.group(2) != "sp" else None; continue
    m = re.match(r"(r\d+), \[sp(?:, #(\d+))?\]", arg)
    if op in ("strh", "strh.w", "strb", "strb.w") and m:
        S[int(m.group(2) or 0)] = R.get(m.group(1)); continue
    if op == "bl":
        t = int(arg.split()[0], 16)
        if t in FN:
            port = G.get(R.get("r0"), hex(R["r0"]) if R.get("r0") is not None else "?")
            if FN[t] == "Init":
                mode = S.get(3); print(hex(a), "fn", hex(func), "Init", port, pins(S.get(0)), MODE.get(mode, mode))
            else:
                print(hex(a), "fn", hex(func), FN[t], port, pins(R.get("r1")))
        for r in ("r0", "r1", "r2", "r3", "ip"): R.pop(r, None)
        continue
    m = re.match(r"(r\d+)", arg)
    if m and op not in ("str", "str.w", "strh", "strb", "cmp", "tst", "cbz", "cbnz", "push", "pop") and not op.startswith("b"):
        R.pop(m.group(1), None)
