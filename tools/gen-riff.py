#!/usr/bin/env python3
"""Emit the buzzer riff table for new-usb-flash/ad4_buzzer.h.

    python3 -I tools/gen-riff.py indochine-l_aventurier.midi > /tmp/riff.h

The source MIDI is a multi-track file; the lead hook lives on one track
(default "Boris Jardel" -- the single-note line that opens the song).
Simultaneous notes on that track are collapsed to the top note, which is
where the melody is.

The file is ~18 s for these 17 notes, so the table is a jingle arrangement,
not a transcription: an eighth is fixed at EIGHTH ms, notes are capped at
CAP_MS and rests at GAP_MS. Pitch and rhythm are untouched.
"""
import sys

EIGHTH = 120      # ms per eighth note (the source tempo gives ~174 ms)
CAP_MS = 700      # sustained notes are shortened, they carry the vibrato
GAP_MS = 150      # inter-phrase rests
VIB_MS = 600      # notes at least this long get vibrato
WANT = 17
TRACK = "Boris Jardel"


def vli(b, i):
    n = 0
    for _ in range(4):
        if i >= len(b):
            return None, i
        c = b[i]
        i += 1
        n = (n << 7) | (c & 0x7F)
        if not c & 0x80:
            return n, i
    return None, i


def tracks(d):
    i, out = 14, []
    while i + 8 <= len(d):
        t, ln = d[i:i + 4], int.from_bytes(d[i + 4:i + 8], "big")
        if t == b"MTrk":
            out.append(d[i + 8:i + 8 + ln])
        i += 8 + ln
    return out


def parse(body):
    i, tick, run, name, ev = 0, 0, None, "", []
    while i < len(body):
        dl, i = vli(body, i)
        if dl is None or i >= len(body):
            break
        tick += dl
        s = body[i]
        if s < 0x80:
            if run is None:
                break
            s = run
        else:
            i += 1
            if s in (0xF0, 0xF7):
                ln, j = vli(body, i)
                if ln is None:
                    break
                i = j + ln
                continue
            if s == 0xFF:
                ty = body[i]
                ln, j = vli(body, i + 1)
                if ln is None:
                    break
                if ty == 0x03:
                    name = body[j:j + ln].decode("latin1")
                i = j + ln
                continue
            if 0x80 <= s < 0xF0:
                run = s
        st = run >> 4
        n = 2 if st in (0x8, 0x9, 0xA, 0xB, 0xE) else 1
        if i + n > len(body):
            break
        a = body[i:i + n]
        i += n
        if any(x > 127 for x in a):
            continue
        ev.append((tick, run, a[0], a[1] if n == 2 else 0))
    return name, ev


def main(path, want=WANT, track=TRACK):
    d = open(path, "rb").read()
    div = int.from_bytes(d[12:14], "big")
    src = None
    for body in tracks(d):
        name, ev = parse(body)
        if name.strip() == track:
            src = ev
            break
    if src is None:
        sys.exit("no track named %r" % track)

    ons = [e for e in src if (e[1] >> 4) == 9 and e[3] > 0]
    offs = [e for e in src if (e[1] >> 4) in (8, 9) and e[3] == 0]

    onsets = []
    for e in ons:
        if onsets and onsets[-1][0] == e[0]:
            if e[2] > onsets[-1][1]:          # keep the melody note
                onsets[-1][1] = e[2]
        else:
            onsets.append([e[0], e[2]])
        if len(onsets) >= want:
            break

    def ms(ticks):
        return round(ticks * EIGHTH / (div / 2))

    rows = []
    for k, (t, p) in enumerate(onsets):
        end = [o[0] for o in offs if o[0] > t and o[2] == p]
        dur = min(end) - t if end else div // 2
        n = min(ms(dur), CAP_MS)
        hz = round(440 * 2 ** ((p - 69) / 12))
        rows.append((hz, n, 1 if n >= VIB_MS else 0))
        if k + 1 < len(onsets):
            gap = ms(onsets[k + 1][0] - (t + dur))
            if gap > 0:
                rows.append((0, min(gap, GAP_MS), 0))

    print("/* %s: first %d notes of the %r track, eighth=%dms, notes capped at %dms. */"
          % (path.split("/")[-1], want, track, EIGHTH, CAP_MS))
    print("/* regenerate: python3 -I tools/gen-riff.py %s */" % path.split("/")[-1])
    print("static const ad4_note AD4_RIFF[] = {")
    for hz, n, v in rows:
        if hz == 0:
            print("    { 0, %d, 0 },%s" % (n, "  /* rest */"))
        else:
            print("    { %d, %d, %d },%s" % (hz, n, v, "  /* vibrato */" if v else ""))
    print("};")


if __name__ == "__main__":
    main(sys.argv[1])
