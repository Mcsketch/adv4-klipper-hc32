#!/usr/bin/env python3
"""
make-positioned-ihex.py — emit a clean iHEX with type-05 (extended linear
address) records from a raw binary at a chosen base.

Why: GNU objcopy -O ihex --change-address=<base> does NOT emit type-05 records
for an image that straddles a 64K page — it wraps the 16-bit offset field, so
the second page lands at low addresses. Our flasher's loader then resolves the
whole image to base 0x00000000 and would clobber the ROM-ISP/reset region
(brick). The factory HD image carries proper type-04/05 records and resolves
correctly; we need the same for a custom Klipper image.

Usage:
  make-positioned-ihex.py <in.bin> <base_hex> <out.hex>
  e.g. make-positioned-ihex.py klipper.bin 0x000C0000 klipper.hex

The output uses one type-05 record per 64K page, followed by 16-byte type-00
data records, ending with a type-01 EOF. This is exactly what the
stm32isp-ad4 loader (and the factory IAPCommand) expect.
"""
import sys

def rec(cnt, off, typ, data=b""):
    body = bytes([cnt, (off >> 8) & 0xFF, off & 0xFF, typ]) + data
    chk = (~sum(body)) & 0xFF
    return ":%02X%04X%02X%s%02X" % (cnt, off, typ, data.hex().upper(), chk)

def main():
    if len(sys.argv) != 4:
        print(__doc__); sys.exit(2)
    src, base_s, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(base_s, 0)
    data = open(src, "rb").read()
    assert base % 0x10000 == 0, "base must be 64K-page aligned for type-05"

    out = []
    pos = 0
    while pos < len(data):
        page = base + pos                       # absolute page start
        upper = page >> 16                      # type-05 value
        page_len = min(0x10000, len(data) - pos)
        out.append(rec(2, 0, 0x05, bytes([upper >> 8, upper & 0xFF])))
        off = 0
        while off < page_len:
            n = min(16, page_len - off)
            chunk = data[pos + off: pos + off + n]
            out.append(rec(len(chunk), off & 0xFFFF, 0x00, chunk))
            off += 16
        pos += page_len
    out.append(rec(0, 0, 0x01))
    with open(dst, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {dst}: {len(data)} bytes @ 0x{base:08X} "
          f"(span 0x{base:08X}..0x{base+len(data):08X}), "
          f"{len(out)} records")

if __name__ == "__main__":
    main()
