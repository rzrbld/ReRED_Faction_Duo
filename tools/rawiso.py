#!/usr/bin/env python3
"""Read ISO9660 directly from a raw MODE1/2352 .bin (no ISO conversion).
usage: rawiso.py BIN ls            -> list all files
       rawiso.py BIN x OUTDIR [glob...] -> extract (case-insensitive glob)"""
import sys, os, struct, fnmatch
SECT, OFF = 2352, 16
class Raw:
    def __init__(s, p):
        s.f = open(p, 'rb'); sz = os.path.getsize(p)
        s.raw = (sz % 2352 == 0) and s._probe()
        if not s.raw: s.sect, s.off = 2048, 0
        else: s.sect, s.off = SECT, OFF
    def _probe(s):
        s.f.seek(16*2352); h = s.f.read(16)
        return h[:12] == b'\x00' + b'\xff'*10 + b'\x00'
    def read(s, lba, n):
        out = bytearray()
        while n > 0:
            s.f.seek(lba*s.sect + s.off); c = s.f.read(2048)
            out += c[:min(n, 2048)]; n -= 2048; lba += 1
        return bytes(out)
def walk(r, lba, size, path=''):
    data = r.read(lba, size); i = 0
    while i < len(data):
        ln = data[i]
        if ln == 0:
            i = (i // 2048 + 1) * 2048; continue
        rec = data[i:i+ln]; i += ln
        elba, esz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        flags, nl = rec[25], rec[32]; name = rec[33:33+nl]
        if name in (b'\x00', b'\x01'): continue
        name = (name.decode('utf-16-be') if JOLIET else name.decode('latin1')).split(';')[0]
        p = path + '/' + name
        if flags & 2: yield from walk(r, elba, esz, p)
        else: yield p, elba, esz
JOLIET = False
def main():
    global JOLIET
    r = Raw(sys.argv[1]); pvd = r.read(16, 2048)
    for l in range(17, 32):
        d = r.read(l, 2048)
        if d[0] == 255: break
        if d[0] == 2 and d[88:90] == b'%/' : pvd, JOLIET = d, True; break
    assert pvd[1:6] == b'CD001', 'not iso9660'
    root = pvd[156:156+34]
    files = list(walk(r, struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]))
    if sys.argv[2] == 'ls':
        for p, l, s in files: print(f'{s:12d} {p}')
    else:
        out, pats = sys.argv[3], [x.lower() for x in sys.argv[4:]] or ['*']
        for p, l, s in files:
            if not any(fnmatch.fnmatch(p.lower(), q) for q in pats): continue
            dst = os.path.join(out, p.lstrip('/')); os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as o:
                lba, left = l, s
                while left > 0:
                    n = min(left, 2048*512); o.write(r.read(lba, n)); lba += 512; left -= n
            print('x', p, s)
main()
