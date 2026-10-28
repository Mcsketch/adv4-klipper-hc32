#!/usr/bin/env python3
"""Fetch a Klipper MCU's identify dictionary through ad4-bridge (TCP <-> ttyS1).

usage: klipper-identify.py [host[:port]] [out.json]
Prints version / MCU / clock / baud from the decompressed dictionary.
"""
import json, os, socket, sys, time, zlib
sys.path.insert(0, os.path.expanduser('~/klipper/klippy'))
import msgproto

COUNT = 40          # bytes per identify request (same as klippy serialhdl)
ROUND_TIMEOUT = 1.0
MAX_RETRIES = 5

def frame(seq, cmd):
    # msgproto.encode_msgblock nests the crc list; build the block flat
    out = [msgproto.MESSAGE_MIN + len(cmd),
           (seq & msgproto.MESSAGE_SEQ_MASK) | msgproto.MESSAGE_DEST] + list(cmd)
    return bytes(out + msgproto.crc16_ccitt(out) + [msgproto.MESSAGE_SYNC])

def main():
    target = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.219:5000'
    host, _, port = target.partition(':')
    port = int(port or 5000)
    out = sys.argv[2] if len(sys.argv) > 2 else None

    mp = msgproto.MessageParser()
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(0.2)
    seq, offset, retries = 1, 0, 0
    chunks, buf = [], bytearray()
    t0 = time.time()
    while True:
        cmd = mp.create_command('identify offset=%d count=%d' % (offset, COUNT))
        s.sendall(frame(seq, cmd))
        got, nak = None, None
        deadline = time.time() + ROUND_TIMEOUT
        while time.time() < deadline and got is None and nak is None:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                sys.exit('bridge closed the connection')
            buf += data
            while buf:
                n = mp.check_packet(buf)
                if n == 0:
                    break
                if n < 0:
                    del buf[0]          # garbage byte, resync
                    continue
                blk = bytes(buf[:n]); del buf[:n]
                rseq = blk[1] & msgproto.MESSAGE_SEQ_MASK
                if n == msgproto.MESSAGE_MIN:
                    # seq+1 = ack; seq = late ack of the previous round or
                    # "not received yet"; anything else = NAK -> resync
                    if rseq not in (seq, (seq + 1) & msgproto.MESSAGE_SEQ_MASK):
                        nak = rseq
                    continue
                p = mp.parse(blk)
                if p['#name'] == 'identify_response' and p['offset'] == offset:
                    got = bytes(p['data'])
                else:
                    print('  other msg:', mp.format_params(p), file=sys.stderr)
        if got is None:
            retries += 1
            if retries > MAX_RETRIES:
                sys.exit('no identify_response for offset %d after %d tries '
                         '(seq %d, nak %r, %d bytes so far)'
                         % (offset, MAX_RETRIES, seq, nak, sum(map(len, chunks))))
            if nak is not None:
                print('  resync: seq %d -> %d' % (seq, nak), file=sys.stderr)
                seq = nak
            continue
        retries = 0
        seq = (seq + 1) & msgproto.MESSAGE_SEQ_MASK
        chunks.append(got)
        offset += len(got)
        if len(got) < COUNT:
            break
    s.close()
    raw = b''.join(chunks)
    print('identify: %d compressed bytes in %.1f s' % (len(raw), time.time() - t0))
    d = json.loads(zlib.decompress(raw))
    cfg = d.get('config', {})
    print('version      :', d.get('version'))
    print('build_versions:', d.get('build_versions'))
    for k in ('MCU', 'CLOCK_FREQ', 'SERIAL_BAUD', 'STATS_SUMSQ_BASE'):
        if k in cfg:
            print('%-14s: %s' % (k, cfg[k]))
    print('commands     : %d   responses: %d' % (len(d.get('commands', {})),
                                                 len(d.get('responses', {}))))
    if out:
        with open(out, 'w') as f:
            json.dump(d, f, indent=1, sort_keys=True)
        print('wrote', out)

if __name__ == '__main__':
    main()
