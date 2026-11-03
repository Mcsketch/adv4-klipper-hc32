#!/usr/bin/env python3
"""ppm2png.py FILE.ppm... -> FILE.png next to each (binary P6 only, stdlib only)."""
import struct, sys, zlib

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in sys.argv[1:]:
    raw = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while raw[pos:pos + 1].isspace():
            pos += 1
        if raw[pos:pos + 1] == b"#":
            pos = raw.index(b"\n", pos)
            continue
        end = pos
        while not raw[end:end + 1].isspace():
            end += 1
        fields.append(raw[pos:end])
        pos = end
    assert fields[0] == b"P6", path
    w, h = int(fields[1]), int(fields[2])
    pix = raw[pos + 1:pos + 1 + w * h * 3]
    rows = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    open(path[:-4] + ".png", "wb").write(png)
