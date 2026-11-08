#!/usr/bin/env python3
"""Compose preview PNGs into contact sheets: sheet.py <shots dir> <out.png> [first] [count] [scale]"""
import os
import sys

from PIL import Image, ImageDraw

d, out = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 999
scale = int(sys.argv[5]) if len(sys.argv) > 5 else 2
files = sorted(f for f in os.listdir(d) if f.endswith('.png'))[first:first + count]
cols, tw, th, cap = 2, 240 * scale, 135 * scale, 22
rows = (len(files) + cols - 1) // cols
img = Image.new('RGB', (cols * tw + (cols + 1) * 12, rows * (th + cap + 12) + 12), (250, 250, 250))
dr = ImageDraw.Draw(img)
for i, f in enumerate(files):
    x, y = 12 + (i % cols) * (tw + 12), 12 + (i // cols) * (th + cap + 12)
    img.paste(Image.open(os.path.join(d, f)).convert('RGB').resize((tw, th), Image.NEAREST), (x, y + cap))
    dr.text((x, y + 4), f, fill=(60, 60, 60))
img.save(out)
