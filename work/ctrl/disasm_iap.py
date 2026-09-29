#!/usr/bin/env python3
"""Disassemble IAPCommand's serial-setup path to settle UART parity (PARENB?) and confirm the GPIO dance constants."""
import struct, sys
from capstone import *

data = open('/Users/admin/adv4-klip/work/ctrl/IAPCommand','rb').read()
e_phoff, e_shoff = struct.unpack_from('<II', data, 0x1C)
e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x2A)
e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2E)
e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]

secs=[]
for i in range(e_shnum):
    off = e_shoff + i*e_shentsize
    sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<IIIIII', data, off)
    secs.append(dict(name=sh_name, type=sh_type, addr=sh_addr, offset=sh_offset, size=sh_size))
shstr_off = secs[e_shstrndx]['offset']
shstrtab = data[shstr_off:shstr_off+secs[e_shstrndx]['size']]
for s in secs:
    e = shstrtab.find(b'\x00', s['name'])
    s['name'] = shstrtab[s['name']:e].decode('latin1')

def secbyname(name):
    for s in secs:
        if s['name']==name: return s
    return None

text = secbyname('.text')
rodata = secbyname('.rodata')

# VA of /dev/ttyS1 string
s1 = data.find(b'/dev/ttyS1\x00')
va_s1 = rodata['addr'] + (s1 - rodata['offset']) if rodata['offset'] <= s1 < rodata['offset']+rodata['size'] else None
print("VA(/dev/ttyS1) =", hex(va_s1) if va_s1 else None, " fileoff", hex(s1))

# find ldr-from-literal-pool instructions referencing va_s1 in .text
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.detail = True
code = data[text['offset']:text['offset']+text['size']]
refs = []
for i in range(0, len(code)-3, 4):
    w = struct.unpack_from('<I', code, i)[0]
    # literal pool value could be anywhere; simpler: scan for the 4-byte value equal to va_s1
    if w == va_s1:
        refs.append(text['addr'] + i)
print("literal-pool hits for va_s1:", [hex(r) for r in refs])

# for each hit, look back up to 64 bytes for 'ldr rX, [pc, #imm]' with matching offset
import re
h = ''
for r in refs:
    rel = r - text['addr']
    back = code[max(0,rel-96):rel]
    # disassemble the preceding region
    found=None
    for inst in md.disasm(back, text['addr']+max(0,rel-96)):
        m = re.match(r'ldr\s+r\d+,\s*\[pc(?:,\s*#0x[0-9a-f]+)?\]', inst.mnemonic)
        if m:
            ldr_va = inst.address
            pool = ldr_va + 8 + int(re.search(r'#0x([0-9a-f]+)', inst.mnemonic or '').group(1) if '#' in inst.mnemonic else 0)
            if pool == r:
                found = (ldr_va, inst.mnemonic)
    if found:
        print("ldr ref at", hex(found[0]))

# Now: find the serial init function. Strategy: disassemble whole .text, find calls to
# open/cfsetispeed clusters. Simpler: find function that contains the ldr ref and disassemble 0x200 bytes before it.
def disasm_region(va, size, label):
    off = va - text['addr']
    if off < 0 or off+size > text['size']:
        print(f"[{label}] region out of .text"); return []
    insts = list(md.disasm(code[off:off+size], va))
    print(f"===== {label} @ {hex(va)} ({len(insts)} insns) =====")
    for ins in insts:
        line = f"  {ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}"
        print(line)
    return insts

# Find the function start: walk back from an ldr ref to 'mov pc, lr'
if refs:
    target = refs[0] - 4  # instruction just before literal
    rel = target - text['addr']
    window = code[max(0, rel-0x300):rel+0x100]
    wbase = text['addr'] + max(0, rel-0x300)
    insts = list(md.disasm(window, wbase))
    # find the last 'mov pc, lr' before target
    func_start = None
    for ins in insts:
        if ins.address <= target and ins.mnemonic=='mov' and 'pc, lr' in ins.op_str:
            func_start = ins.address
    if func_start is None:
        func_start = wbase
    print("guess function start:", hex(func_start))
    # disassemble from func start for 0x400 bytes
    off = func_start - text['addr']
    insts = list(md.disasm(code[off:off+0x400], func_start))
    for ins in insts:
        line = f"  {ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}"
        # flag parity-relevant ops
        flag = ''
        if '0x100' in ins.op_str or '0x300' in ins.op_str:
            flag = '   <== PARENB/PARODD mask!'
        if 'cfset' in ins.mnemonic or ins.mnemonic=='bl':
            flag = flag or ('   (call)')
        print(line+flag)
        if ins.mnemonic=='mov' and 'pc, lr' in ins.op_str and ins.address > func_start:
            break