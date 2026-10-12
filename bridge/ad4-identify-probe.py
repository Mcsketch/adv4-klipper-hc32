#!/usr/bin/env python3
"""Send a Klipper 'identify offset=0 count=64' request to the MCU over the
SoC TCP bridge and report the raw reply + decoded identify dump.
Protocol frame (from klippy/msgproto.py):
    [msglen, seq|0x10, <cmd...>, crc16_hi, crc16_lo, 0x7e]
identify = msgid 1, format 'identify offset=%u count=%c'
"""
import socket, struct, sys, time

def crc16_ccitt(buf):
    crc = 0xffff
    for data in buf:
        data ^= crc & 0xff
        data ^= (data & 0x0f) << 4
        crc = ((data << 8) | (crc >> 8)) ^ (data >> 4) ^ (data << 3)
    return (crc >> 8) & 0xff, crc & 0xff

def build_identify(offset=0, count=64, seq=0):
    msgid = [1]                      # identify
    offset_e = [offset]              # %u -> uint32: 0 -> 1 byte
    count_e = [count]                # %c -> byte: 64 -> 1 byte
    cmd = msgid + offset_e + count_e
    msglen = 5 + len(cmd)            # MESSAGE_MIN(5) + cmd
    seqb = (seq & 0x0f) | 0x10
    body = [msglen, seqb] + cmd
    hi, lo = crc16_ccitt(body)
    return bytes(body + [hi, lo, 0x7e])

def parse_frames(buf):
    """Yield (msglen, payload_bytes) for each complete frame in buf."""
    frames = []
    i = 0
    while i + 5 <= len(buf):
        msglen = buf[i]
        if msglen < 5 or msglen > 64:
            i += 1
            continue
        if i + msglen > len(buf):
            break
        if buf[i + msglen - 1] != 0x7e:
            i += 1
            continue
        frames.append((msglen, buf[i:i+msglen]))
        i += msglen
    return frames

def decode_identify_payload(payload):
    """Identify_response offset=%u data=%.*s ; msgid 0."""
    out = []
    pos = 0
    # msgid (uint32) at 2..? -> just skip to parse offset
    # layout: [msglen, seq, msgid(0)->0x00, offset(%u), len(%.*s), data...]
    # msgid 0 encodes as single byte 0x00
    p = 2  # after msglen,seq
    # msgid
    c = payload[p]; p += 1
    # offset %u
    off = c = payload[p]; p += 1  # offset=0 -> 1 byte
    # len %.*s
    ln = payload[p]; p += 1
    data = payload[p:p+ln]
    out.append((off, data))
    return out

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.10"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    send_reset = "--reset" in sys.argv

    req = build_identify(0, 64)
    print("Sending identify frame (%d bytes): %s" % (len(req), req.hex()))
    s = socket.create_connection((host, port), timeout=6)
    s.settimeout(4)
    if send_reset:
        # also send an MCU reset-like poke: just resend identify
        pass
    s.sendall(req)
    buf = bytearray()
    try:
        end = time.time() + 4
        while time.time() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if any(f[0] is not None for f in parse_frames(buf)):
                # got at least one frame; give a short grace for more
                time.sleep(0.3)
                try:
                    more = s.recv(4096)
                    if more:
                        buf += more
                    else:
                        break
                except socket.timeout:
                    break
    except Exception as e:
        print("recv error:", e)
    s.close()
    if not buf:
        print("NO DATA received (UART silent at this baud).")
        return
    print("RAW (%d bytes): %s" % (len(buf), buf.hex()))
    frames = parse_frames(buf)
    print("Parsed %d frame(s)" % len(frames))
    for idx, (ml, fr) in enumerate(frames):
        print("  frame[%d] len=%d crc_ok=%s payload=%s" % (
            idx, ml,
            (crc16_ccitt(bytes(fr[:ml-3])) == tuple(fr[ml-3:ml-1])),
            fr.hex()))
        # try identify_response (msgid 0)
        if ml >= 7 and fr[2] == 0x00:
            off, data = None, b''
            p = 3
            # offset %u (may be multibyte)
            off = fr[p]; p += 1
            ln = fr[p]; p += 1
            data = fr[p:p+ln]
            print("    identify_response offset=%d len=%d" % (off, ln))
            print("    text: %s" % data.decode('latin1', 'replace'))

if __name__ == "__main__":
    main()
