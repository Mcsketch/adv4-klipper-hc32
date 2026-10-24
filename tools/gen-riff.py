#!/usr/bin/env python3
"""Emit the buzzer riff table for new-usb-flash/ad4_buzzer.h.

    python3 -I tools/gen-riff.py indochine-l_aventurier.midi > /tmp/riff.h

The source MIDI is a multi-track file; the lead hook lives on one track
(default "Boris Jardel" -- the single-note line that opens the song).
Simultaneous notes on that track are collapsed to the top note, which is
where the melody is.

Pitch and rhythm come straight from the track; the only arrangement choice is
the tempo (EIGHTH) and how many notes to take (WANT). Notes are legato, so the
table contains no rests.

The whole line is transposed up to FLOOR_HZ and above. The sunxi PWM divider
cannot represent a period that long: 370 Hz is the last frequency that fits,
and everything under it comes back as a click or as a wrapped, wrong pitch.
Measured on hardware - the same phrase at 208-370 Hz drops notes, at 416-740 Hz
it is clean.
"""
import sys

# The source is 172 bpm (348837 us/quarter, eighth = 174 ms). Play it 10 bpm
# under: the piezo needs the extra room to let each note speak.
EIGHTH = 185      # ms per eighth note
CAP_MS = 1000     # guard only; this phrase never reaches it
WANT = 32         # ~10.5 s at EIGHTH
TRACK = "Boris Jardel"
FLOOR_HZ = 400    # below this the PWM period overflows the divider

# The track opens with a four-note pickup (tick 86400) and repeats it; the
# figure that matches the sheet - four up, four down - starts at 107520, and
# from there the 'Boris Jardel' and 'Oli de Sat' tracks agree note for note.
START_TICK = 107520


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

    ons = [e for e in src if (e[1] >> 4) == 9 and e[3] > 0 and e[0] >= START_TICK]
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

    # Legato: every note is held to the next onset instead of to its own
    # note-off. The sheet is one unbroken line of eighths, and any air between
    # notes reads as a dropped note on a piezo.
    lo = min(p for _, p in onsets)
    oct_up = 0
    while 440 * 2 ** ((lo + 12 * oct_up - 69) / 12) < FLOOR_HZ:
        oct_up += 1

    rows = []
    for k, (t, p) in enumerate(onsets):
        end = [o[0] for o in offs if o[0] > t and o[2] == p]
        real = min(end) - t if end else div // 2
        span = onsets[k + 1][0] - t if k + 1 < len(onsets) else real
        n = min(ms(span), CAP_MS)
        hz = round(440 * 2 ** ((p + 12 * oct_up - 69) / 12))
        rows.append((hz, n))

    print("/* %s: first %d notes of the %r track, eighth=%dms, legato, +%d oct. */"
          % (path.split("/")[-1], want, track, EIGHTH, oct_up))
    print("/* regenerate: python3 -I tools/gen-riff.py %s */" % path.split("/")[-1])
    print("static const ad4_note AD4_RIFF[] = {")
    for hz, n in rows:
        print("    { %d, %d },%s" % (hz, n, "  /* rest */" if not hz else ""))
    print("};")


if __name__ == "__main__":
    main(sys.argv[1])
