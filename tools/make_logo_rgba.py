#!/usr/bin/env python3
"""Builds assets/xptablet_logo.rgba, the status bar's logo, from assets/xptablet_logo.png: "XPTL", u32 width, height, levels,
then each level's RGBA (premultiplied-safe straight alpha), the first 96 pixels high and each next one half the last, every one
resampled from the original (Lanczos) so every size is as sharp as it can be. Usage: make_logo_rgba.py <in.png> <out.rgba>"""
import struct, sys
from PIL import Image

src = Image.open(sys.argv[1]).convert("RGBA")
bbox = src.getbbox()   # trim the empty margin
src = src.crop(bbox)
h0 = 96
w0 = round(src.width * h0 / src.height)
levels = []
w, h = w0, h0
while h >= 1 and w >= 1:
    # resample in premultiplied alpha, so the edges do not pick up the transparent pixels' colour
    pm = src.copy()
    r, g, b, a = pm.split()
    from PIL import ImageChops
    r = ImageChops.multiply(r, a); g = ImageChops.multiply(g, a); b = ImageChops.multiply(b, a)
    pm = Image.merge("RGBA", (r, g, b, a)).resize((w, h), Image.LANCZOS)
    r, g, b, a = pm.split()
    px = bytearray()
    for (rr, gg, bb, aa) in zip(r.getdata(), g.getdata(), b.getdata(), a.getdata()):
        if aa: px += bytes((min(255, rr * 255 // aa), min(255, gg * 255 // aa), min(255, bb * 255 // aa), aa))
        else: px += b"\0\0\0\0"
    levels.append(bytes(px))
    if h == 1: break
    w, h = max(1, w // 2), max(1, h // 2)
with open(sys.argv[2], "wb") as f:
    f.write(b"XPTL" + struct.pack("<III", w0, h0, len(levels)))
    for L in levels: f.write(L)
print(w0, h0, len(levels))
