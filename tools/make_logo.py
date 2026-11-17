#!/usr/bin/env python3
"""Draws the XPTablet logo (assets/xptablet_logo.png): a heavy white "XP" and a tall, thin, letter-spaced "TABLET" in a colour gradient.
   tools/make_logo.py [out.png]   Needs Pillow, numpy and the Inter fonts (Inter-Black, Inter-Medium)."""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT_DIR = "/usr/share/fonts/opentype/inter/"
H = 448                      # logo height in pixels (4x the 112 px the WXPlane logo uses)
GRAD = [(255, 184, 64), (255, 94, 108), (150, 96, 250)]   # amber -> coral -> violet

def text_mask(txt, font_file, cap_h):
    """One line of text as an 'L' mask cropped to its ink, with capitals cap_h pixels tall."""
    f = ImageFont.truetype(FONT_DIR + font_file, 1000)
    b = f.getbbox("H")
    f = ImageFont.truetype(FONT_DIR + font_file, int(1000 * cap_h / (b[3] - b[1])))
    box = f.getbbox(txt)
    im = Image.new("L", (box[2] - box[0] + 8, box[3] - box[1] + 8), 0)
    ImageDraw.Draw(im).text((4 - box[0], 4 - box[1]), txt, font=f, fill=255)
    return im.crop(im.getbbox())

def grad(t):
    t = max(0.0, min(1.0, t)) * (len(GRAD) - 1)
    i = min(int(t), len(GRAD) - 2)
    k = t - i
    return tuple(round(GRAD[i][c] * (1 - k) + GRAD[i + 1][c] * k) for c in range(3))

def main(out):
    # "XP": Inter Black, a little wider than drawn, the full height
    xp = text_mask("XP", "Inter-Black.otf", H)
    xp = xp.resize((int(xp.width * 1.12), H), Image.LANCZOS)
    # "TABLET": each letter squeezed to a tall narrow shape, the vertical strokes thickened back to the horizontal ones' weight
    cap = int(H * 0.62)
    letters = []
    for ch in "TABLET":
        m = text_mask(ch, "Inter-Medium.otf", cap)
        m = m.resize((max(1, int(m.width * 0.50)), cap), Image.LANCZOS)
        pad = Image.new("L", (m.width + 12, cap), 0)
        pad.paste(m, (6, 0))
        # horizontal-only thickening: the brightest of the mask shifted a few pixels left and right
        acc = np.asarray(pad)
        for dx in range(-4, 5):
            sh = Image.new("L", pad.size, 0)
            sh.paste(pad, (dx, 0))
            acc = np.maximum(acc, np.asarray(sh))
        acc = Image.fromarray(acc)
        letters.append(acc.crop(acc.getbbox()))
    gap0 = int(H * 0.07)                # between the P and the T
    gap = int(H * 0.30)                 # letter spacing
    wt = sum(l.width for l in letters) + gap * (len(letters) - 1)
    W = xp.width + gap0 + wt
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    img.paste(Image.new("RGBA", xp.size, (255, 255, 255, 255)), (0, 0), xp)
    x = xp.width + gap0
    x0 = x
    for l in letters:
        col = Image.new("RGBA", l.size)
        px = col.load()
        for i in range(l.width):
            c = grad((x + i - x0) / max(1, wt - 1))
            for j in range(l.height):
                px[i, j] = c + (255,)
        img.paste(col, (x, H - l.height), l)
        x += l.width + gap
    img.save(out)
    bg = Image.new("RGBA", (W + 160, H + 160), (14, 34, 64, 255))
    bg.alpha_composite(img, (80, 80))
    bg.resize((bg.width // 2, bg.height // 2), Image.LANCZOS).save(out.replace(".png", "_on_navy.png"))
    print(out, img.size)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "assets/xptablet_logo.png")
