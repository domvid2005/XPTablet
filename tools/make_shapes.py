#!/usr/bin/env python3
"""Turns the AircraftShapesSVG top views (third_party/AircraftShapesSVG, by RexKramer1 and amnesica, GPL-3.0) into
assets/shapes/aircraft_shapes.txt, the file XPTablet loads at run time to draw the aircraft on the Ground page.

The SVGs are drawn 1 unit = 1 metre, nose up. Each becomes, in metres with the nose at y = 0 and the centreline at x = 0:
    shape <ICAO>                 the file's name (an ICAO type designator)
    size <length> <span>         the outline's extent
    fill x y x y ...             a closed outline to fill (several for some types)
    line x y x y ...             an accent line drawn over it (panel lines, engines, ...)
Curves are flattened to straight pieces of at most 5 cm of error.
   tools/make_shapes.py [svg folder] [out file]"""
import glob, math, os, re, sys
import xml.etree.ElementTree as ET

NS = {"svg": "http://www.w3.org/2000/svg", "ink": "http://www.inkscape.org/namespaces/inkscape"}
INK_LABEL = "{http://www.inkscape.org/namespaces/inkscape}label"
TOL = 0.05


def translate_of(el):
    t = el.get("transform", "")
    m = re.match(r"\s*translate\(\s*([-\d.eE+]+)[ ,]*([-\d.eE+]*)\s*\)", t)
    if m:
        return float(m.group(1)), float(m.group(2) or 0)
    if t.strip():
        raise ValueError("transform not handled: " + t)
    return 0.0, 0.0


def bez(p0, p1, p2, p3):
    n = max(2, min(40, int(math.dist(p0, p1) + math.dist(p1, p2) + math.dist(p2, p3)) * 4 + 2))
    out = []
    for i in range(1, n + 1):
        t = i / n
        a = (1 - t) ** 3; b = 3 * (1 - t) ** 2 * t; c = 3 * (1 - t) * t * t; d = t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


def subpaths(d, ox, oy):
    """The path's pieces as (points, closed)."""
    toks = re.findall(r"[MmLlHhVvCcSsZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
    i = 0; cmd = None; cur = (0.0, 0.0); start = cur; pts = []; out = []; last_c2 = None

    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v

    def flush(closed):
        nonlocal pts
        if len(pts) >= 2:
            out.append(([(x + ox, y + oy) for x, y in pts], closed))
        pts = []

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]; i += 1
            if cmd in "Zz":
                flush(True); cur = start; continue
        rel = cmd.islower(); C = cmd.upper()
        if C == "M":
            flush(False)
            x, y = num(), num()
            cur = (cur[0] + x, cur[1] + y) if rel else (x, y)
            start = cur; pts = [cur]
            cmd = "l" if rel else "L"   # later pairs are lines
            last_c2 = None
        elif C == "L":
            x, y = num(), num(); cur = (cur[0] + x, cur[1] + y) if rel else (x, y); pts.append(cur); last_c2 = None
        elif C == "H":
            x = num(); cur = (cur[0] + x if rel else x, cur[1]); pts.append(cur); last_c2 = None
        elif C == "V":
            y = num(); cur = (cur[0], cur[1] + y if rel else y); pts.append(cur); last_c2 = None
        elif C in "CS":
            if C == "C":
                c1 = (num(), num())
                if rel: c1 = (cur[0] + c1[0], cur[1] + c1[1])
            else:
                c1 = (2 * cur[0] - last_c2[0], 2 * cur[1] - last_c2[1]) if last_c2 else cur
            c2 = (num(), num()); p = (num(), num())
            if rel:
                c2 = (cur[0] + c2[0], cur[1] + c2[1]); p = (cur[0] + p[0], cur[1] + p[1])
            pts.extend(bez(cur, c1, c2, p)); cur = p; last_c2 = c2
        else:
            raise ValueError("path command " + cmd)
    flush(False)
    return out


def simplify(pts):
    """Drops points that lie within TOL of the line through their neighbours (Douglas-Peucker)."""
    if len(pts) < 3:
        return pts
    def rec(a, b):
        (x0, y0), (x1, y1) = pts[a], pts[b]
        L = math.hypot(x1 - x0, y1 - y0) or 1e-9
        best, k = 0.0, -1
        for j in range(a + 1, b):
            dd = abs((x1 - x0) * (y0 - pts[j][1]) - (x0 - pts[j][0]) * (y1 - y0)) / L
            if dd > best: best, k = dd, j
        if best <= TOL: return [pts[a]]
        return rec(a, k) + rec(k, b)
    return rec(0, len(pts) - 1) + [pts[-1]]


def area(p):
    return 0.5 * sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p)))


def collect(el, ox, oy, layer, out):
    tx, ty = translate_of(el); ox += tx; oy += ty
    lab = el.get(INK_LABEL)
    if el.tag.endswith("}g") and lab is not None:
        layer = "accent" if lab.lower().startswith("accent") else ("skip" if "background" in lab.lower() else "outline")
    if el.tag.endswith("}path") and layer != "skip":
        style = el.get("style", "")
        if "display:none" in style: return
        for pts, closed in subpaths(el.get("d", ""), ox, oy):
            out.append((layer, pts, closed))
    for ch in el:
        if ch.tag.endswith("}g") and "display:none" in ch.get("style", ""): continue
        collect(ch, ox, oy, layer, out)


def convert(path):
    root = ET.parse(path).getroot()
    items = []
    collect(root, 0.0, 0.0, "outline", items)
    fills = [p for lay, p, c in items if lay == "outline" and len(p) >= 3]
    if not fills:
        return None
    # the outline layer: its closed pieces are filled; a piece drawn open is closed for filling too (a few files leave the
    # last segment out). Accent pieces are lines.
    lines = [p for lay, p, c in items if lay == "accent"]
    xs = [x for p in fills for x, _ in p]; ys = [y for p in fills for _, y in p]
    cx = (min(xs) + max(xs)) / 2; y0 = min(ys)
    norm = lambda p: [(round(x - cx, 3), round(y - y0, 3)) for x, y in simplify(p)]
    fills = [norm(p) for p in fills]
    fills = [p for p in fills if abs(area(p)) > 0.05]
    fills.sort(key=lambda p: -abs(area(p)))
    lines = [norm(p) for p in lines]
    return (max(ys) - y0, max(xs) - min(xs)), fills, lines


# Real length and span (metres) of the types flown most in X-Plane: the drawing is resized to these, so each comes out exact
# (most drawings are within 2%; a few are a variant, e.g. E195 is drawn as the E2, CRJX 16% long).
REAL = {
    "A318": (31.44, 34.10), "A19N": (33.84, 35.80), "A320": (37.57, 35.80), "A20N": (37.57, 35.80), "A321": (44.51, 35.80), "A21N": (44.51, 35.80),
    "A332": (58.82, 60.30), "A333": (63.66, 60.30), "A338": (58.82, 64.00), "A339": (63.66, 64.00), "A342": (59.40, 60.30), "A343": (63.69, 60.30),
    "A345": (67.93, 63.45), "A346": (75.36, 63.45), "A359": (66.80, 64.75), "A35K": (73.79, 64.75), "A388": (72.72, 79.75), "A306": (54.08, 44.84),
    "A310": (46.66, 43.89), "B733": (33.40, 28.88), "B734": (36.40, 28.88), "B735": (31.01, 28.88), "B737": (33.63, 35.79), "B738": (39.47, 35.79),
    "B739": (42.11, 35.79), "B38M": (39.52, 35.92), "B39M": (42.16, 35.92), "B752": (47.32, 38.05), "B753": (54.47, 38.05), "B762": (48.51, 47.57),
    "B763": (54.94, 47.57), "B764": (61.37, 51.92), "B772": (63.73, 60.93), "B77L": (63.73, 64.80), "B77W": (73.86, 64.80), "B788": (56.72, 60.12),
    "B789": (62.81, 60.12), "B78X": (68.28, 60.12), "B742": (70.66, 59.64), "B744": (70.66, 64.44), "B748": (76.25, 68.40), "B712": (37.81, 28.45),
    "MD11": (61.62, 51.97), "E170": (29.90, 26.00), "BCS1": (35.00, 35.10), "BCS3": (38.70, 35.10), "CRJ2": (26.77, 21.21), "CRJ7": (32.30, 23.24),
    "CRJ9": (36.20, 24.85), "CRJX": (39.13, 26.18), "DH8C": (25.68, 27.43), "DH8D": (32.83, 28.42), "GL5T": (29.50, 28.65), "LJ35": (14.83, 12.04),
    "FA7X": (23.19, 26.21), "C750": (22.04, 19.38), "C172": (8.28, 11.00), "SR22": (7.92, 11.68), "P28A": (7.16, 9.14), "PC12": (14.40, 16.28),
    "C208": (11.46, 15.87), "B350": (14.22, 17.65), "DA42": (8.56, 13.42), "MRF1": (15.30, 8.40), "RFAL": (15.27, 10.90), "MIRA": (15.03, 8.22),
    "CL2T": (19.82, 28.63), "C25B": (15.59, 16.26), "GLF6": (30.41, 30.36), "AT75": (27.17, 27.05), "AT45": (22.67, 24.57), "B190": (17.63, 17.67),
    "SF34": (19.73, 21.44), "D328": (21.11, 20.98), "J328": (21.11, 20.98), "F50": (25.25, 29.00), "RJ85": (28.60, 26.34), "SU95": (29.94, 27.80),
    "PC6T": (11.00, 15.87), "PA46": (8.81, 13.11), "BN2P": (10.86, 14.94),
}
# Types without a drawing of their own: the nearest drawing of the family, resized to the type's own length and span.
ALIASES = {
    # ToLiss, Zibo / LevelUp, Laminar's own and other common X-Plane airliners
    "A319": ("A318", 33.84, 35.80), "B736": ("B737", 31.24, 34.32), "B732": ("B733", 30.53, 28.35),
    "MD81": ("B712", 45.06, 32.87), "MD82": ("B712", 45.06, 32.87), "MD83": ("B712", 45.06, 32.87), "MD88": ("B712", 45.06, 32.87),
    "MD87": ("B712", 39.75, 32.87), "MD90": ("B712", 46.50, 32.87), "DC93": ("B712", 36.37, 28.47),
    "E175": ("E170", 31.68, 26.00), "E190": ("E170", 36.24, 28.72), "E195": ("E170", 38.65, 28.72),
    "E290": ("E195", 36.25, 33.72), "E295": ("E195", 41.50, 35.12),
    "B461": ("RJ85", 26.16, 26.34), "B462": ("RJ85", 28.60, 26.34), "B463": ("RJ85", 30.99, 26.34), "RJ70": ("RJ85", 26.16, 26.34), "RJ1H": ("RJ85", 30.99, 26.34),
    # Bombardier
    "CRJ1": ("CRJ2", 26.77, 21.21), "CL60": ("GL5T", 20.85, 19.61), "CL30": ("GL5T", 20.92, 19.46), "CL35": ("GL5T", 20.92, 21.00),
    "GLEX": ("GL5T", 30.30, 28.65), "GL7T": ("GL5T", 33.88, 31.70), "GL8T": ("GL5T", 33.88, 31.70),
    "LJ31": ("LJ35", 14.83, 13.36), "LJ40": ("LJ35", 16.69, 14.58), "LJ45": ("LJ35", 17.68, 14.58), "LJ60": ("LJ35", 17.88, 13.34),
    "LJ70": ("LJ35", 17.68, 15.51), "LJ75": ("LJ35", 17.68, 15.51), "DH8A": ("DH8C", 22.25, 25.89), "DH8B": ("DH8C", 22.25, 25.89), "CL4G": ("CL2T", 19.82, 28.63),
    # Dassault
    "FA8X": ("FA7X", 24.46, 26.29), "FA6X": ("FA7X", 25.68, 25.91), "F900": ("FA7X", 20.21, 19.33), "F2TH": ("FA7X", 20.23, 19.33),
    "FA50": ("FA7X", 18.52, 18.86), "FA20": ("FA7X", 17.15, 16.32), "FA10": ("FA7X", 13.86, 13.08), "MIR2": ("MIRA", 14.36, 9.13),
    # others in Laminar's own fleet and common add-ons
    "BE9L": ("B350", 10.82, 15.32), "BE20": ("B350", 13.36, 16.61), "C25A": ("C25B", 14.53, 15.19), "C25C": ("C25B", 16.26, 15.49),
    "C510": ("C25B", 12.37, 13.16), "C68A": ("C750", 18.97, 22.05), "C700": ("C750", 22.30, 22.04), "B37M": ("B38M", 35.56, 35.92),
}


def resize(fills, lines, L0, S0, L1, S1):
    kx, ky = S1 / S0, L1 / L0
    f = lambda p: [(round(x * kx, 3), round(y * ky, 3)) for x, y in p]
    return [f(p) for p in fills], [f(p) for p in lines]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "third_party/AircraftShapesSVG"
    dst = sys.argv[2] if len(sys.argv) > 2 else "assets/shapes/aircraft_shapes.txt"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    out = ["# Aircraft top views for XPTablet, made by tools/make_shapes.py from AircraftShapesSVG",
           "# (https://github.com/RexKramer1/AircraftShapesSVG, by RexKramer1 and amnesica, GNU GPL v3.0: see LICENSE-GPL-3.0.txt).",
           "# Metres; nose at y = 0, centreline at x = 0."]
    n = 0
    for f in sorted(glob.glob(os.path.join(src, "*.svg"))):
        name = os.path.splitext(os.path.basename(f))[0]
        if " " in name or name in ("Unidentified", "BALL", "CVN-65", "GYRO"):   # alternate poses, the carrier, the balloon
            continue
        try:
            r = convert(f)
        except Exception as e:
            print("skip", name, e); continue
        if not r: print("skip", name, "(no outline)"); continue
        (L, S), fills, lines = r
        if name in REAL:
            fills, lines = resize(fills, lines, L, S, *REAL[name])
            L, S = REAL[name]
        out.append("shape " + name)
        out.append("size %.2f %.2f" % (L, S))
        for p in fills: out.append("fill " + " ".join("%g %g" % q for q in p))
        for p in lines: out.append("line " + " ".join("%g %g" % q for q in p))
        n += 1
    out.append("# type, drawing, its length and span")
    for k in sorted(ALIASES):
        shp, L, S = ALIASES[k]
        out.append("alias %s %s %.2f %.2f" % (k, shp, L, S))
    open(dst, "w").write("\n".join(out) + "\n")
    print(dst, n, "shapes,", len(ALIASES), "aliases,", os.path.getsize(dst), "bytes")


if __name__ == "__main__":
    main()
