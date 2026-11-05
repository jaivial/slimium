#!/usr/bin/env python3
"""pngdiff: compare two PNG screenshots pixel by pixel (stdlib only).

  pngdiff.py A.png B.png        -> {"w":..,"h":..,"diff_pct":..,"exact_pct":..,"same_size":..}
  pngdiff.py --pattern OUT.png  -> write a deterministic 256x256 RGB test image

diff_pct counts pixels whose largest channel delta exceeds 24 (anti-aliasing
noise tolerance); exact_pct counts every differing pixel. A size mismatch is
reported as 100.
"""
import json, struct, sys, zlib

def decode(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path + ': not a PNG'
    pos, idat, w = 8, [], None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and inter == 0 and ctype in (2, 6), 'unsupported PNG'
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        if f == 1:
            for x in range(bpp, stride): line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line); prev = line
    return w, h, bpp, rows

def compare(a, b):
    wa, ha, ba, ra = decode(a)
    wb, hb, bb, rb = decode(b)
    if (wa, ha) != (wb, hb):
        return {"w": wa, "h": ha, "w2": wb, "h2": hb, "same_size": False, "diff_pct": 100.0, "exact_pct": 100.0}
    diff = exact = 0
    for y in range(ha):
        la, lb = ra[y], rb[y]
        if la == lb and ba == bb:
            continue
        for x in range(wa):
            pa, pb = la[x * ba:x * ba + 3], lb[x * bb:x * bb + 3]
            if pa != pb:
                exact += 1
                if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > 24:
                    diff += 1
    n = wa * ha
    return {"w": wa, "h": ha, "same_size": True, "diff_pct": round(100.0 * diff / n, 3), "exact_pct": round(100.0 * exact / n, 3)}

def pattern(out):
    w = h = 256
    raw = b''.join(b'\x00' + bytes(v for x in range(w) for v in (x, y, (x * y) & 255)) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--pattern':
        pattern(sys.argv[2])
    elif len(sys.argv) == 3:
        print(json.dumps(compare(sys.argv[1], sys.argv[2])))
    else:
        sys.exit(__doc__)
