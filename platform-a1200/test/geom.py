#!/usr/bin/env python3
"""Geometry check of a ptest screenshot (FS-UAE full-frame shot, 1 lores pixel = 2 x 2 screenshot pixels).
prints the bounding box of the red frame (palette index 255), of the blue fill and the colours just outside the box (border blanking)."""
import sys
from PIL import Image
for fn in sys.argv[1:]:
    im = Image.open(fn).convert('RGB'); W, H = im.size; px = im.load()
    def bbox(col, tol=40):
        xs = []; ys = []
        for y in range(H):
            for x in range(W):
                p = px[x, y]
                if all(abs(p[i] - col[i]) <= tol for i in range(3)): xs.append(x); ys.append(y)
        return (min(xs), min(ys), max(xs), max(ys), len(xs)) if xs else None
    red = bbox((255, 0, 0)); blue = bbox((0, 0, 128)); green = bbox((0, 255, 255))
    print(fn.split('/')[-1], 'size', im.size, 'red', red, 'blue', blue, 'cyan-marker', green)
    if red:
        x0, y0, x1, y1, _ = red
        print('  expect width 512 height 480: got', x1 - x0 + 1, y1 - y0 + 1, ' left border px', px[max(x0 - 6, 0), (y0 + y1) // 2], ' right', px[min(x1 + 6, W - 1), (y0 + y1) // 2],
              ' above', px[(x0 + x1) // 2, max(y0 - 6, 0)], ' below', px[(x0 + x1) // 2, min(y1 + 6, H - 1)])
