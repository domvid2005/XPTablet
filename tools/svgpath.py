"""A small SVG path reader: M L H V C S Q A Z (absolute and relative) into flattened subpaths, and a nonzero-rule rasterizer."""
import math, re
from PIL import Image

def subpaths(d, steps=24):
    toks = re.findall(r"[MmLlHhVvCcSsQqAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)
    i = 0; cmd = None; x = y = 0.0; sx = sy = 0.0; out = []; cur = []; lc = None
    def num():
        nonlocal i; v = float(toks[i]); i += 1; return v
    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]): cmd = toks[i]; i += 1
        rel = cmd.islower(); c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            if cur: out.append(cur)
            x, y = ox + num(), oy + num(); sx, sy = x, y; cur = [(x, y)]; cmd = "l" if rel else "L"; lc = None
        elif c == "L": x, y = ox + num(), oy + num(); cur.append((x, y)); lc = None
        elif c == "H": x = ox + num(); cur.append((x, y)); lc = None
        elif c == "V": y = (y if rel else 0) + num(); cur.append((x, y)); lc = None
        elif c in "CS":
            if c == "C": x1, y1 = ox + num(), oy + num()
            else: x1, y1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            x2, y2, x3, y3 = ox + num(), oy + num(), ox + num(), oy + num()
            for k in range(1, steps + 1):
                t = k / steps; u = 1 - t
                cur.append((u*u*u*x + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3, u*u*u*y + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3))
            lc = (x2, y2); x, y = x3, y3
        elif c == "Q":
            x1, y1, x2, y2 = ox + num(), oy + num(), ox + num(), oy + num()
            for k in range(1, steps + 1):
                t = k / steps; u = 1 - t
                cur.append((u*u*x + 2*u*t*x1 + t*t*x2, u*u*y + 2*u*t*y1 + t*t*y2))
            x, y = x2, y2; lc = None
        elif c == "A":
            rx, ry, phi, fa, fs = num(), num(), math.radians(num()), int(num()), int(num())
            x2, y2 = ox + num(), oy + num()
            cp, sp = math.cos(phi), math.sin(phi)
            dx, dy = (x - x2) / 2, (y - y2) / 2
            x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
            rx, ry = abs(rx), abs(ry)
            lam = x1p**2 / rx**2 + y1p**2 / ry**2
            if lam > 1: rx *= math.sqrt(lam); ry *= math.sqrt(lam)
            num_ = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
            co = math.sqrt(max(0, num_ / (rx*rx*y1p*y1p + ry*ry*x1p*x1p))) * (-1 if fa == fs else 1)
            cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
            cx, cy = cp * cxp - sp * cyp + (x + x2) / 2, sp * cxp + cp * cyp + (y + y2) / 2
            def ang(ux, uy, vx, vy):
                a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a
            t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
            dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
            if not fs and dt > 0: dt -= 2 * math.pi
            elif fs and dt < 0: dt += 2 * math.pi
            n = max(4, int(abs(dt) / (2 * math.pi) * steps * 4))
            for k in range(1, n + 1):
                t = t1 + dt * k / n
                cur.append((cx + rx * math.cos(t) * cp - ry * math.sin(t) * sp, cy + rx * math.cos(t) * sp + ry * math.sin(t) * cp))
            x, y = x2, y2; lc = None
        elif c == "Z":
            x, y = sx, sy
            if cur: out.append(cur); cur = []
            lc = None
    if cur: out.append(cur)
    return out

def raster(d, size, box=24.0, ss=4, margin=0.0):
    """The path filled (nonzero rule) as a size x size alpha image; box: the path's view size, margin: empty border in view units."""
    S = size * ss
    k = S / (box + 2 * margin)
    polys = [[((px + margin) * k, (py + margin) * k) for px, py in sp] for sp in subpaths(d)]
    import numpy as np
    ys = (np.arange(S) + 0.5)
    wind = np.zeros((S, S), dtype=np.int32)
    xs = (np.arange(S) + 0.5)
    for poly in polys:
        n = len(poly)
        for j in range(n):
            (x0, y0), (x1, y1) = poly[j], poly[(j + 1) % n]
            if y0 == y1: continue
            d_ = 1 if y1 > y0 else -1
            lo, hi = (y0, y1) if y0 < y1 else (y1, y0)
            rows = np.nonzero((ys >= lo) & (ys < hi))[0]
            for r in rows:
                xc = x0 + (ys[r] - y0) * (x1 - x0) / (y1 - y0)
                wind[r, xs > xc] += d_   # crossing to the left of these pixels
    m = (wind != 0).astype(np.uint8) * 255
    return Image.fromarray(m, "L").resize((size, size), Image.BOX)
