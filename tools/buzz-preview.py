#!/usr/bin/env python3
"""Render the buzzer cues in src/ad4_buzzer.h to WAV files, to hear a tune
without a printer. A 50 % square wave, like the piezo's PWM.

    python3 -I tools/buzz-preview.py src/ad4_buzzer.h [OUTDIR]

Writes OUTDIR/buzz-<name>.wav (default build/preview) for every ad4_note table.
"""
import os
import re
import sys
import wave

RATE = 44100
VOL = 6000


def tables(text):
    for m in re.finditer(r"static const ad4_note (AD4_\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        body = re.sub(r"/\*.*?\*/", "", m.group(2), flags=re.S)
        yield m.group(1), [(int(h), int(t)) for h, t in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", body)]


def render(notes):
    out = bytearray()
    for hz, ms in notes:
        n = RATE * ms // 1000
        for i in range(n):
            v = 0 if not hz else (VOL if (i * hz * 2 // RATE) % 2 == 0 else -VOL)
            out += v.to_bytes(2, "little", signed=True)
    return bytes(out)


def main(src, outdir="build/preview"):
    os.makedirs(outdir, exist_ok=True)
    for name, notes in tables(open(src).read()):
        path = os.path.join(outdir, "buzz-%s.wav" % name[4:].lower())
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(render(notes))
        print("%s  %d notes, %d ms" % (path, len(notes), sum(t for _, t in notes)))


if __name__ == "__main__":
    main(*sys.argv[1:])
