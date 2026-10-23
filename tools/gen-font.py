#!/usr/bin/env python3
"""Turn the upstream font8x8_basic.h table into new-usb-flash/ad4_font8x8.h.

Upstream: https://github.com/dhepper/font8x8 (public domain / CC0). Download
the header into its own directory first, then:

    python3 -I tools/gen-font.py /path/to/font8x8_basic.h new-usb-flash/ad4_font8x8.h

The generated header keeps only ASCII 32..126 and stores each row as one byte
with bit 0 = the leftmost pixel, which is how ad4-screen.c renders it.
"""
import re
import sys

USAGE = "usage: gen-font.py FONT8X8_BASIC_H OUT_H"


def parse(path):
    src = open(path, encoding="utf-8").read()
    body = src.split("font8x8_basic[128][8] = {", 1)[1]
    glyphs = []
    for row in re.findall(r"\{([^{}]*)\}", body):
        vals = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", row)]
        if len(vals) != 8:
            raise SystemExit("bad row: %d bytes" % len(vals))
        glyphs.append(vals)
    if len(glyphs) != 128:
        raise SystemExit("expected 128 glyphs, got %d" % len(glyphs))
    return glyphs


def art(g):
    return ["".join("#" if (g[r] >> b) & 1 else "." for b in range(8)) for r in range(8)]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(USAGE)
    glyphs = parse(sys.argv[1])
    out = [
        "/* 8x8 public-domain bitmap font (Daniel Hepper, font8x8_basic, CC0).",
        "   Generated from the upstream header by tools/gen-font.py - do not edit by hand.",
        "   glyph[c][r] = one 8-pixel row, bit 0 = leftmost pixel. */",
        "#ifndef AD4_FONT8X8_H",
        "#define AD4_FONT8X8_H",
        "",
        "static const unsigned char font8x8[95][8] = {",
    ]
    for c in range(32, 127):
        out.append("  { %s }," % ", ".join("0x%02x" % b for b in glyphs[c]) +
                   "  /* %s */" % chr(c))
    out += ["};", "", "#endif"]
    open(sys.argv[2], "w").write("\n".join(out) + "\n")

    # a text preview, so the bit order can be eyeballed before the first render
    for c in (65, 77, 48, 57, 46, 58, 47, 45, 61, 40, 41):
        print(chr(c))
        for line in art(glyphs[c]):
            print("   " + line)


if __name__ == "__main__":
    main()
