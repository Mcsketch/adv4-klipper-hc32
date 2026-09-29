#!/usr/bin/env python3
"""Dump IAPCommand .text and flag termios-relevant constants (parity, baud, CLOCAL|CREAD) and GPIO/sysfs paths."""
import struct, re
from capstone import *

data = open('/Users/admin/adv4-klip/work/ctrl/IAPCommand','rb').read()
e_shoff = struct.unpack_from('<I', data, 0x20)[0]
e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2E)
e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]
secs=[]
for i in range(e_shnum):
    off = e_shoff + i*e_shentsize
    vals = struct.unpack_from('<IIIIIIIIII', data, off)
    secs.append(dict(zip(['name','type','flags','addr','offset','size','link','info','addralign','entsize'], vals)))
shstr_off = secs[e_shstrndx]['offset']
def byname(n):
    for s in secs:
        nm_off = shstr_off + s['name']
        e = data.find(b'\x00', nm_off)
        if data[nm_off:e].decode('latin1') == n: return s
    return None

# resolve symbol table for call targets
sym = byname('.symtab'); strtab = byname('.strtab')
syms = {}
for i in range(sym['size']//16):
    off = sym['offset'] + i*16
    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', data, off)
    e2 = strtab['offset'] + data.find(b'\x00', strtab['offset'] + st_name)
    name = data[strtab['offset']+st_name:e2].decode('latin1')
    if name and st_value:
        syms[st_value] = name

text = byname('.text')
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB) if False else Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.detail = True
code = data[text['offset']:text['offset']+text['size']]
insts = list(md.disasm(code, text['addr']))

# Interesting constants:
# ARM termios: CLOCAL=0x40, CREAD=0x80 (Linux); CS8=0x30 (CSIZE 0x30 | CS8 0x30);
# PARENB=0x100, PARODD=0x200; B115200=0x22 (ARM), B250000 nonstandard.
# cflag build: CLOCAL|CREAD = 0xC0 ; CLOCAL|CREAD|CS8 = 0xC0|0x30 = 0xF0 ; |PARENB = 0x1F0 ; |PARODD=0x2F0
FLAGS = {
 0x40:'CLOCAL', 0x80:'CREAD', 0x40|0x80:'CLOCAL|CREAD',
 0x30:'CSIZE&CS8', 0x100:'PARENB', 0x200:'PARODD', 0x100|0x200:'PARENB|PARODD',
 0xC0:'CLOCAL|CREAD', 0xF0:'CLOCAL|CREAD|CS8', 0x1F0:'CLOCAL|CREAD|CS8|PARENB', 0x2F0:'CLOCAL|CREAD|CS8|PARENB|PARODD',
 0x120:'PARENB|CREAD', 0x320:'PARENB|PARODD|CREAD',
 0x22:'B115200?', 0x20:'B9600?', 0x33:'B230400?', 0x32:'B57600?',
 0x1C000:'CFON|CRTSCTS?',
}
print(f"== .text {len(insts)} instructions ==")
interesting = []
for ins in insts:
    m = ins.mnemonic
    op = ins.op_str
    tag = ''
    # literal movs / orrs / movw that build termios words
    mm = re.match(r'0x([0-9a-f]+)', op)
    if mm:
        v = int(mm.group(1),16)
        if v in FLAGS:
            tag = f"  <== {FLAGS[v]}"
        elif 0x80 <= v <= 0x400 and v in (0x80,0x100,0x180,0x280,0x380,0xC0,0x1C0,0x3C0,0x440):
            tag = f"  <== possible cflag 0x{v:x}"
    if m in ('bl','bx') or m=='ldr' and ', #0x' in op:
        pass
    if tag or m=='bl':
        tgt = syms.get(ins.address + 4, '') if m=='bl' else ''
        print(f"{ins.address:08x}  {m:6s} {op:28s} {tgt:20s}{tag}")
