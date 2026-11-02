#!/usr/bin/env python3
"""Read 'Tnnnnnn Fnnnnnn' from the speed HUD of full-frame screenshots (HUD text on display rows 28/29, 8x8 font from amiga/font.c, drawn in plane 0 = colour index 1 only).
usage: hudtf.py shot.png ...  -> prints T and F per shot and the overall F/T between first and last"""
import sys, re, os
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', '..', 'amiga', 'font.c')).read()
glyph = {}
for i, m in enumerate(re.finditer(r'\{((?:0x[0-9A-Fa-f]{2},?){8})\}', src)):
    glyph[chr(32 + i)] = tuple(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
def line(im, ox, oy, row, ncol):
    res = ''
    # colour index 1: the most common non-black colour in the HUD rows is the text colour
    for c in range(ncol):
        g = []
        for r in range(8):
            b = 0
            for k in range(8):
                px = im.getpixel((ox + (c * 8 + k) * 2 + 1, oy + (row * 8 + r) * 2 + 1))
                b = (b << 1) | (1 if max(px) > 100 else 0)
            g.append(b)
        res += min(glyph, key=lambda ch: sum(bin(a ^ b).count('1') for a, b in zip(glyph[ch], g)))
    return res
vals = []
for fn in sys.argv[1:]:
    im = Image.open(fn).convert('RGB')
    for oy in (52, 0):
        t = line(im, 138, oy, 29, 32)
        m = re.search(r'(\d{6})\s+F(\d+)', t)
        if m: vals.append((int(m.group(1)), int(m.group(2)))); print(os.path.basename(fn), m.group(1), m.group(2)); break
    else: print(os.path.basename(fn), 'unreadable', repr(t))
if len(vals) > 1 and vals[-1][0] > vals[0][0]:
    print('overall F/T = %.1f %%' % (100.0 * (vals[-1][1] - vals[0][1]) / (vals[-1][0] - vals[0][0])))
