#!/usr/bin/env python3
"""Makes the WXPlane icon's picture for the home screen from WXPlane's own logo (WX and PLANE side by side): the same letters,
WX stacked on top of PLANE. Writes assets/wxplane_stacked.png (to look at) and assets/wxplane_stacked.rgba, which the plugin embeds:
256 x 256 RGBA followed by its smaller copies down to 1 x 1 (mip levels), so the icon stays clean at any size.
   tools/make_wx_icon.py <path to WXPlane's assets/wxplane_logo.png>"""
import sys
import numpy as np
from PIL import Image

def main(src):
    logo = Image.open(src).convert("RGBA")
    a = np.asarray(logo)
    alpha = a[..., 3] > 40
    white = alpha & (a[..., 0] > 235) & (a[..., 1] > 235) & (a[..., 2] > 235)   # "WX" is white, "PLANE" is coloured
    colour = alpha & ~white
    def box(mask):
        ys, xs = np.where(mask)
        return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    wx_box, pl_box = box(white), box(colour & (np.arange(a.shape[1])[None, :] > box(white)[2] - 4))
    wx, pl = logo.crop(wx_box), logo.crop(pl_box)
    S = 1024                                    # drawn large, then reduced
    width = int(S * 0.80)
    wx = wx.resize((width, round(wx.height * width / wx.width)), Image.LANCZOS)
    pl = pl.resize((width, round(pl.height * width / pl.width)), Image.LANCZOS)
    gap = int(S * 0.06)
    total = wx.height + gap + pl.height
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    y = (S - total) // 2
    img.alpha_composite(wx, ((S - width) // 2, y))
    img.alpha_composite(pl, ((S - width) // 2, y + wx.height + gap))
    img.resize((512, 512), Image.LANCZOS).save("assets/wxplane_stacked.png")
    out = bytearray()
    n = 256
    while n >= 1:
        out += img.resize((n, n), Image.LANCZOS).tobytes()
        n //= 2
    open("assets/wxplane_stacked.rgba", "wb").write(out)
    print("assets/wxplane_stacked.rgba", len(out), "bytes")

if __name__ == "__main__":
    main(sys.argv[1])
