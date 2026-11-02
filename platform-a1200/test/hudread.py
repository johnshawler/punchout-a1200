#!/usr/bin/env python3
"""Read the speed HUD ("SPEED nnn%" + "Tnnnnnn Fnnnnnn" lines, drawn in colour index 1 = plane 0 only) from FS-UAE full-frame screenshots.
usage: hudread.py shot.png ...   prints T (ticks), F (game frames) from the bottom text line; the HUD font is the 8x8 demo font."""
import sys
from PIL import Image
# glyphs: build from amiga/font.c? simpler: template match against the digit glyphs found in the demo font file
import re, os
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', '..', 'amiga', 'font.c')).read()
rows = re.findall(r'\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}', src)
glyph = {}
for i, r in enumerate(rows):
    glyph[chr(32 + i)] = tuple(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', r))
def read_line(im, y0, x0, n):
    out = ''
    for c in range(n):
        g = []
        for r in range(8):
            b = 0
            for k in range(8):
                px = im.getpixel((x0 + (c * 8 + k) * 2, y0 + r * 2))
                b = (b << 1) | (1 if px[0] > 200 and px[1] < 120 else 0)   # red-ish = index 1
            g.append(b)
        best = min(glyph, key=lambda ch: sum(bin(a ^ b).count('1') for a, b in zip(glyph[ch], g)))
        out += best
    return out
for fn in sys.argv[1:]:
    im = Image.open(fn).convert('RGB')
    # display origin in the full-frame shot: first pixel of the 256x240 window = x 138, y 52 (PAL) / see platform README; search for text rows 28/29
    for ox, oy in ((138, 52), (138, 20)):
        l28 = read_line(im, oy + 28 * 16, ox, 32); l29 = read_line(im, oy + 29 * 16, ox, 32)
        if 'T' in l29 and 'F' in l29: break
    print(os.path.basename(fn), repr(l28.strip()), repr(l29.strip()))
