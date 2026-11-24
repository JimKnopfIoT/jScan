#!/usr/bin/env python3
"""Compare two PNM scans: best (dx, dy) offset and per-channel brightness."""
import re, sys
import numpy as np

def rd(p):
    d = open(p, 'rb').read()
    toks, pos = [], 0
    while len(toks) < 4:
        m = re.match(rb'\s*(?:#[^\n]*\n\s*)*(\S+)', d[pos:])
        toks.append(m.group(1)); pos += m.end()
    w, h, ch = int(toks[1]), int(toks[2]), 3 if toks[0] == b'P6' else 1
    return np.frombuffer(d[pos + 1:pos + 1 + w * h * ch], np.uint8).reshape(h, w, ch).astype(float)

a, b = rd(sys.argv[1]), rd(sys.argv[2])
ga, gb = a.mean(2), b.mean(2)
h, w = min(ga.shape[0], gb.shape[0]) - 40, min(ga.shape[1], gb.shape[1]) - 40
best = None
for dy in range(-20, 21):
    for dx in range(-20, 21):
        A = ga[20:20 + h, 20:20 + w]
        B = gb[20 + dy:20 + dy + h, 20 + dx:20 + dx + w]
        if B.shape != A.shape:
            continue
        e = float(np.mean((A - A.mean()) * (B - B.mean())) / (A.std() * B.std() + 1e-9))
        if best is None or e > best[0]:
            best = (e, dx, dy)
print('sizes', a.shape, b.shape, 'best offset of 2nd vs 1st: dx=%d dy=%d corr=%.3f' % (best[1], best[2], best[0]))
print('mean 1st', a.mean((0, 1)).round(1), '2nd', b.mean((0, 1)).round(1))
