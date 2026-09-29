#!/usr/bin/env python3
"""Stream-decode Flashforge eMMC backup parts from FFAD4-BKUP.zip (no full-extract).
Part layout (from partitions.txt):
  part000 = LBA 0      .. 2097151   (1 GiB)   boot part0 + rootfs part1
  part001 = LBA 2097152 .. 4194303  (1 GiB)   data part2  [0..1G]
  part002 = LBA 4194304 .. 6291455  (1 GiB)   data part2  [1..2G]
  part003 = LBA 6291456 .. 8388607  (1 GiB)   data part2  [2..3G]
  part004 = LBA 8388608 .. 10485759 (1 GiB)   data part2  [3..4G]
  part005 = LBA 10485760.. 12582911 (1 GiB)   data part2  [4..5G]
  part006 = LBA 12582912.. 14680063 (1 GiB)   data part2  [5..6G]
  part007 = LBA 14680064.. 15269887 (302 MiB) data part2  [6G..end]
"""
import os, struct, sys, time, zipfile, zlib

ZIP = '/Users/admin/adv4-klip/FFAD4-BKUP.zip'
OUT = '/tmp/ad4bk'
os.makedirs(OUT, exist_ok=True)
SZ = 512  # LBA size

def decode_part(member, dest, want_offset=0, want_size=None, tag=''):
    """Stream-decode one member; if want_offset==0 and want_size is None, keep all.
    Otherwise write only the [want_offset, want_offset+want_size) LBA window."""
    z = zipfile.ZipFile(ZIP)
    info = z.getinfo(member)
    f = z.open(info, 'r')
    keep_all = want_size is None
    full = open(dest, 'wb')
    skip = want_offset
    to_write = want_size if not keep_all else None
    buf = b''
    pos = 0
    t0 = time.time()
    while True:
        chunk = f.read(1 << 20)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 1:
            if len(buf) < 2:
                break
            dlen = struct.unpack('<H', buf[:2])[0]
            if len(buf) < dlen + 4:
                break
            block = buf[2:2+dlen]
            buf = buf[dlen + 4:]  # +4 = dlen(2) + dlen16(2); trailing flag byte is part of dlen16 pair
            # NOTE: zlib decomp blocks in this tool are: u16 len + data + u16 len-check? verify below.
            data = zlib.decompressobj().decompress(block)
            if data:
                if keep_all:
                    full.write(data)
                else:
                    lba = pos // SZ
                    n = len(data)
                    if lba + (n + SZ - 1)//SZ <= skip:
                        pass
                    else:
                        # write overlap of [pos, pos+n) with [skip*SZ, (skip+to_write)*SZ)
                        s0, e0 = pos, pos + n
                        s1, e1 = skip * SZ, (skip + to_write) * SZ
                        a, b = max(s0, s1), min(e0, e1)
                        if a < b:
                            full.write(data[a - s0:b - s0])
            pos += len(data)
            if not keep_all and pos >= (skip + to_write) * SZ:
                f.close(); full.close()
                dt = time.time() - t0
                print(f"[{tag}] window {to_write} LBA written in {dt:.1f}s -> {dest}")
                return pos
            if time.time() - t0 > 5:
                t0 = time.time()
                done = pos if keep_all else min(pos, (skip+to_write)*SZ)
                tot = info.file_size if keep_all else (skip+to_write)*SZ
                print(f"[{tag}] {done/tot*100:5.1f}%  ({done//SZ} LBA)", flush=True)
    full.close()
    print(f"[{tag}] done {pos} bytes -> {dest}")
    return pos

if __name__ == '__main__':
    if '--all0' in sys.argv:
        decode_part('mmcblk0.img.part000', f'{OUT}/part000.bin', tag='part000')
    elif '--data' in sys.argv:
        # /data = LBA 26214400 (13.1G) .. 15269887 (7.3G total)
        # part001 covers 2097152..4194303 : want 26214400-4194303 = 1572863 LBA
        decode_part('mmcblk0.img.part001', f'{OUT}/data.bin', want_offset=26214400, want_size=1572863, tag='data')
        for i, part in enumerate(range(2, 8)):
            woff = 0
            wsz = 2097152 if part < 7 else 15269887 - (7*1048576 - 512000)  # part007 tail
            # part002..part006 full:
            wsz = 2097152
            if part == 7:
                wsz = 15269887 - ((2+5)*2097152) - 0  # leftover
            decode_part(f'mmcblk0.img.part{part}', f'{OUT}/data.bin',
                        want_offset=0, want_size=wsz, tag=f'data+{part}')
    else:
        # default: part000 full (boot + rootfs)
        decode_part('mmcblk0.img.part000', f'{OUT}/part000.bin', tag='part000')
