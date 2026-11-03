#!/usr/bin/env python3
"""
make-positioned-ihex.py — emit a clean iHEX with type-04 (extended address)
records from a raw binary at a chosen base, matching the format the factory
Adventurer-4 HD image and the vendor IAPCommand loader are proven to consume.

Record format (matches Adventurer4-HD-IAP-2.3.0-20230216.hex):
  :020000040000FA   <- type-04 "extended address": data = upper 16 bits of the
                        LINEAR address (loader computes abs = (data<<16)|offset,
                        the same as a type-05 record, it just uses type 0x04).
  :10C00000<16B>c   <- type-00 data records, 16-bit offset field.

A new type-04 record is emitted every time the upper-16-bit segment changes,
so an image that straddles a 64K linear page (e.g. base 0xC000, ~27 KB ->
0xC000..0x12C60) is emitted as:
  seg 0x0000 : offsets 0xC000..0xFFFF
  seg 0x0001 : offsets 0x0000..0x2C60
...which is exactly how the full-chip factory dump is laid out.

Why not `objcopy -O ihex --change-address`?
  GNU objcopy does NOT emit extended-address records for an image that
  straddles a 64K page — it wraps the 16-bit offset field, so the second
  page lands at low addresses and the loader resolves the whole image to
  base 0x00000000, clobbering the ROM-ISP/reset region (BRICK). This tool
  emits one type-04 record per 64K page, so the base is always resolved
  correctly by IAPCommand.

Safety note: the MCU ROM-ISP at 0x00000000 is never written by a correctly
  positioned image (base 0xC000 is > 0x8000). flashforge_init.sh additionally
  runs a base-address guard before flashing.

Usage:
  make-positioned-ihex.py <in.bin> <base_hex> <out.hex>
  e.g. make-positioned-ihex.py klipper.bin 0xC000 klipper-ad4.hex
"""
import sys

def rec(cnt, off, typ, data=b""):
    body = bytes([cnt, (off >> 8) & 0xFF, off & 0xFF, typ]) + data
    chk = (0x100 - sum(body)) & 0xFF   # two's complement (GNU/factory ihex standard)
    return ":%02X%04X%02X%s%02X" % (cnt, off, typ, data.hex().upper(), chk)

def main():
    if len(sys.argv) != 4:
        print(__doc__); sys.exit(2)
    src, base_s, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(base_s, 0)
    data = open(src, "rb").read()

    out = []
    cur_seg = None            # current upper-16-bit segment
    i = 0
    while i < len(data):
        addr = base + i
        seg = (addr >> 16) & 0xFFFF
        if seg != cur_seg:
            out.append(rec(2, 0, 0x04, bytes([seg >> 8, seg & 0xFF])))
            cur_seg = seg
        off16 = addr & 0xFFFF
        # clamp chunk so a 16-byte record NEVER crosses a 64K page boundary
        page_room = 0x10000 - off16
        n = min(16, page_room, len(data) - i)
        chunk = data[i:i+n]
        out.append(rec(n, off16, 0x00, chunk))
        i += n
    out.append(rec(0, 0, 0x01))
    with open(dst, "w") as f:
        f.write("\n".join(out) + "\n")
    end = base + len(data)
    print("wrote %s: %d bytes @ 0x%08X..0x%08X (%d records)"
          % (dst, len(data), base, end, len(out)))

if __name__ == "__main__":
    main()
