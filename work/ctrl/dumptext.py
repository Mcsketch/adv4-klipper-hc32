#!/usr/bin/env python3
"""Dump IAPCommand .text fully; locate serial init (parity flags) and GPIO dance."""
import struct, re
from capstone import *

data = open('/Users/admin/adv4-klip/work/ctrl/IAPCommand','rb').read()
text_addr, text_off, text_size = 0x10a0c, 0xa0c, 3772
code = data[text_off:text_off+text_size]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True
insts = list(md.disasm(code, text_addr))
print(f"{len(insts)} instructions in .text")

# Resolve PLT names via .plt entries: plt entry = ldr pc,[pc,#imm]; value in pool = GOT slot;
# .rel.plt gives GOT slot -> symbol idx -> .dynstr
e_shoff = struct.unpack_from('<I', data, 0x20)[0]
e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2E)
e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]
secs={}
shstr_off = None
shstr_size = 0
for i in range(e_shnum):
    off = e_shoff + i*e_shentsize
    nm_off, stype, sf, sv, so, ss = struct.unpack_from('<IIIIII', data, off)
    if i == e_shstrndx:
        shstr_off = so
        shstr_size = ss
    secs[nm_off] = (stype, sf, sv, so, ss)
shstrtab = data[shstr_off:shstr_off+shstr_size]
def sname(nm):
    e = shstrtab.find(b'\x00', nm)
    return shstrtab[nm:e].decode('latin1')
def secbyname(n):
    for nm, v in secs.items():
        if sname(nm) == n:
            return v
    return None
dynstr = secbyname('.dynstr')
dynstr_off, dynstr_size = dynstr[3], dynstr[4]
def dynname(idx):
    e = data.index(b'\x00', dynstr_off+idx)
    return data[dynstr_off+idx:e].decode('latin1')
relplt = secbyname('.rel.plt')
plt_name_by_gotva = {}
for i in range(relplt[4]//8):
    off = relplt[3] + i*8
    r_offset, r_info = struct.unpack_from('<II', data, off)
    sym = (r_info >> 8) & 0xffffff
    plt_name_by_gotva[r_offset] = dynname(sym)

# GOT: map slot VA -> value (target). .got section
got = secbyname('.got')
got_off, got_size, got_va = got[3], got[4], got[2]

# Build call-name map: for each BL to .plt slot, resolve
plt = secbyname('.plt')
plt_va, plt_size = plt[2], plt[4]
call_name = {}
for i in range(0, plt_size-3, 4):
    ent = plt_va + i
    w = struct.unpack_from('<I', data, plt[3]+i)[0]
    # decode: E51FF000 form
    imm = (w & 0xfff)
    pool = ent + 8 + imm
    target = struct.unpack_from('<I', data, got_off + (pool - got_va))[0] if got_va <= pool < got_va+got_size else None
    name = plt_name_by_gotva.get(pool)
    if name:
        call_name[ent] = name

# Now walk .text: group into functions by 'mov pc, lr' terminators, label BL targets
out = []
cur_func_start = None
functions = []
for k, ins in enumerate(insts):
    if ins.mnemonic == 'bl':
        tgt = int(ins.op_str, 0) if ins.op_str.startswith('#') else None
        # ARM BL: op_str like '#0x10840'
        nm = call_name.get(tgt)
        ins._callname = nm or ''
    if ins.mnemonic == 'mov' and 'pc, lr' in ins.op_str:
        functions.append((ins.address, ins))

# print with call names and highlight parity-related immediates
for ins in insts:
    extra = ''
    if getattr(ins, '_callname', ''):
        extra = f"  ; {ins._callname}"
    o = ins.op_str
    # termios c_cflag masks: PARENB=0x100, PARODD=0x200; B115200 speed value; 15680 delay
    if re.search(r'0x100\b|0x200\b|0x300\b', o):
        extra += '   <== PARENB(0x100)/PARODD(0x200)?'
    if '15680' in o or '0x3d10' in o:
        extra += '   <== 15680us delay'
    if re.search(r'0x[0-9a-f]+', o):
        if 'push' in ins.mnemonic or 'pop' in ins.mnemonic:
            pass
    line = f"{ins.address:08x}  {ins.mnemonic:7s} {o}{extra}"
    print(line)