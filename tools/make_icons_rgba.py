#!/usr/bin/env python3
"""Builds assets/icons.rgba, the app icons' atlas: "XPTL", u32 width, height, levels, then each level's RGBA, every level resampled
from the full-size atlas. Cells of 256 x 256, 4 across: first the white pictograms (Material Design Icons, drawn from their vector
paths, tinted when drawn), then the services' own tiles made from their logos in assets/brands. Usage: make_icons_rgba.py <out.rgba> [preview.png]"""
import struct, sys, os
import numpy as np
from PIL import Image, ImageChops
sys.path.insert(0, os.path.dirname(__file__))
from mdi_paths import MDI
from svgpath import raster

CELL = 256
ORDER = ["cog", "list-status", "check-bold", "file-document", "star-four-points", "airplane", "airport", "web",   # pictograms
         "foreflight", "navigraph", "skyvector", "volanta"]                                                       # tiles
root = os.path.join(os.path.dirname(__file__), "..", "assets", "brands")

def gradient(c0, c1, radial=False):
    y, x = np.mgrid[0:CELL, 0:CELL] / (CELL - 1)
    t = np.clip(np.hypot(x - 0.5, y - 0.42) * 1.5, 0, 1) if radial else (x + y) / 2
    return (np.array(c0)[None, None, :] * (1 - t[..., None]) + np.array(c1)[None, None, :] * t[..., None])

def place(bg, rgb, alpha, box, size):
    """rgb/alpha cropped to box, fitted into size x size, centred on bg (float arrays); returns the tile."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    k = size / max(w, h)
    nw, nh = max(1, round(w * k)), max(1, round(h * k))
    pm = Image.fromarray(np.uint8(np.clip(rgb[y0:y1, x0:x1] * alpha[y0:y1, x0:x1, None], 0, 255))).resize((nw, nh), Image.LANCZOS)
    am = Image.fromarray(np.uint8(np.clip(alpha[y0:y1, x0:x1] * 255, 0, 255))).resize((nw, nh), Image.LANCZOS)
    pm, am = np.asarray(pm).astype(float), np.asarray(am).astype(float)[..., None] / 255
    ox, oy = (CELL - nw) // 2, (CELL - nh) // 2
    out = bg.copy()
    reg = out[oy:oy + nh, ox:ox + nw]
    out[oy:oy + nh, ox:ox + nw] = pm + reg * (1 - am)
    return out

def tile(name):
    if name == "volanta":   # its own app icon
        return np.asarray(Image.open(os.path.join(root, "volanta.png")).convert("RGB").resize((CELL, CELL), Image.LANCZOS)).astype(float)
    if name == "foreflight":   # the white sun-and-aircraft mark, lifted off its banner, on ForeFlight's blue
        f = np.asarray(Image.open(os.path.join(root, "foreflight.jpg")).convert("RGB")).astype(float)
        a = np.clip((f.min(axis=2) - 40) / 200, 0, 1)
        white = np.full_like(f, 255.0)
        return place(gradient((4, 38, 70), (12, 78, 114)), white, a, (368, 239, 518, 389), 150)
    if name == "navigraph":   # the compass star, lifted off its banner, on Navigraph's navy
        n = np.asarray(Image.open(os.path.join(root, "navigraph.png")).convert("RGB")).astype(float)
        bg = np.array([50, 66, 108.0])
        a = np.clip((n.max(axis=2) - 128) / 40, 0, 1)   # the banner's centre is up to ~110 bright: above it is the star
        col = np.where(a[..., None] > 0, (n - bg * (1 - a[..., None])) / np.maximum(a[..., None], 1e-3), 0)
        return place(gradient((52, 70, 116), (18, 26, 46), radial=True), col, a, (481, 153, 717, 389), 150)
    if name == "skyvector":   # the globe (with its shadow) on white
        s = np.asarray(Image.open(os.path.join(root, "skyvector.png")).convert("RGB")).astype(float)
        a = np.ones(s.shape[:2])
        return place(np.full((CELL, CELL, 3), 255.0), s, a, (0, 0, 400, 324), 214)

cols = 4
rows = (len(ORDER) + cols - 1) // cols
atlas = Image.new("RGBA", (cols * CELL, rows * CELL), (0, 0, 0, 0))
for i, name in enumerate(ORDER):
    x, y = (i % cols) * CELL, (i // cols) * CELL
    if name in MDI:
        a = raster(MDI[name], CELL, margin=0.0)   # the 24-unit icon fills its cell (each icon keeps its own designed margin)
        cell = Image.merge("RGBA", (Image.new("L", (CELL, CELL), 255),) * 3 + (a,))
    else:
        cell = Image.fromarray(np.uint8(np.clip(tile(name), 0, 255))).convert("RGBA")
    atlas.paste(cell, (x, y))
if len(sys.argv) > 2:
    prev = Image.new("RGBA", atlas.size, (40, 90, 200, 255)); prev.alpha_composite(atlas); prev.save(sys.argv[2])
levels = []
w, h = atlas.size
cur = atlas
while True:
    r, g, b, a = cur.split()
    levels.append(cur.tobytes())
    if w <= cols * 4: break
    w, h = w // 2, h // 2
    # resample from the full-size atlas in premultiplied alpha (no dark fringes)
    R, G, B, A = atlas.split()
    pm = Image.merge("RGBA", (ImageChops.multiply(R, A), ImageChops.multiply(G, A), ImageChops.multiply(B, A), A)).resize((w, h), Image.LANCZOS)
    p = np.asarray(pm).astype(float)
    al = p[..., 3:4]
    rgb = np.where(al > 0, p[..., :3] * 255 / np.maximum(al, 1), 0)
    cur = Image.fromarray(np.uint8(np.clip(np.concatenate([rgb, al], axis=2), 0, 255)), "RGBA")
with open(sys.argv[1], "wb") as f:
    f.write(b"XPTL" + struct.pack("<III", atlas.size[0], atlas.size[1], len(levels)))
    for L in levels: f.write(L)
print(atlas.size, len(levels), "levels;", ", ".join(ORDER))
