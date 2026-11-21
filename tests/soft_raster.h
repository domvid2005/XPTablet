// The tests' own renderer: the tablet's frame (ImGui triangles) drawn into memory, pixel by pixel, without a graphics card. Slow and
// plain (no smaller copies of the pictures), but it shows what the tablet shows: the screenshots the tests write, and the picture
// the stand-in host (tests/local_host.cpp) gives Local Connect's mirror.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "imgui.h"
#include "tabletui.h"
#include "tabrender.h"

namespace xpt_test {
using namespace xpt;

struct Canvas {
    int w, h;
    std::vector<float> rgb;
    Canvas(int W, int H) : w(W), h(H), rgb((size_t)W * H * 3, 0.0f) {}
};

void raster(Canvas& cv, const std::vector<Batch>& batches, const unsigned char* tex, int tw, int th, float scale) {
    for (const Batch& bt : batches)
        for (size_t e = 0; e + 2 < bt.v.size(); e += 3) {
            const ImDrawVert* v = &bt.v[e];
            float X[3], Y[3];
            for (int k = 0; k < 3; ++k) { X[k] = v[k].pos.x * scale; Y[k] = v[k].pos.y * scale; }
            const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({X[0], X[1], X[2]}))), x1 = std::min(cv.w - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
            const int y0 = std::max(0, (int)std::floor(std::min({Y[0], Y[1], Y[2]}))), y1 = std::min(cv.h - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float b0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
                    const float b1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
                    const float b2 = 1.0f - b0 - b1;
                    if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f) continue;
                    float col[4];
                    for (int k = 0; k < 4; ++k)
                        col[k] = (b0 * ((v[0].col >> (8 * k)) & 255) + b1 * ((v[1].col >> (8 * k)) & 255) + b2 * ((v[2].col >> (8 * k)) & 255)) / 255.0f;
                    const float u = b0 * v[0].uv.x + b1 * v[1].uv.x + b2 * v[2].uv.x, w = b0 * v[0].uv.y + b1 * v[1].uv.y + b2 * v[2].uv.y;
                    const float fx = u * tw - 0.5f, fy = w * th - 0.5f;   // bilinear sample of the alpha atlas
                    const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
                    const float ax = fx - ix, ay = fy - iy;
                    auto T = [&](int a, int b) { a = std::max(0, std::min(tw - 1, a)); b = std::max(0, std::min(th - 1, b)); return tex[(size_t)b * tw + a] / 255.0f; };
                    const float ta = (T(ix, iy) * (1 - ax) + T(ix + 1, iy) * ax) * (1 - ay) + (T(ix, iy + 1) * (1 - ax) + T(ix + 1, iy + 1) * ax) * ay;
                    float a = col[3] * ta;
                    float tc[3] = {1, 1, 1};
                    if ((intptr_t)bt.tex == 2) {   // the WXPlane icon's picture: 256 x 256 RGBA
                        unsigned int nb = 0;
                        const unsigned char* px = wxLogoPixels(nb);
                        const int lx = std::max(0, std::min(255, (int)(u * 256))), ly = std::max(0, std::min(255, (int)(w * 256)));
                        const unsigned char* q = px + ((size_t)ly * 256 + lx) * 4;
                        for (int k = 0; k < 3; ++k) tc[k] = q[k] / 255.0f;
                        a = col[3] * q[3] / 255.0f;
                    }
                    if ((intptr_t)bt.tex == 3 || (intptr_t)bt.tex == 4 || (intptr_t)bt.tex == 5) {   // XPTablet's logo or the icons' atlas, its first level, bilinear
                        int lw = 0, lh = 0, lv = 0;
                        float lfrom = 0; const unsigned char* px = (intptr_t)bt.tex == 3 ? logoPixels(lw, lh, lv) : (intptr_t)bt.tex == 5 ? logoMaskPixels(lw, lh, lv, lfrom) : iconsPixels(lw, lh, lv);
                        const float fx2 = u * lw - 0.5f, fy2 = w * lh - 0.5f;
                        const int jx = (int)std::floor(fx2), jy = (int)std::floor(fy2);
                        const float bx = fx2 - jx, by = fy2 - jy;
                        float acc[4] = {0, 0, 0, 0};
                        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::max(0, std::min(lw - 1, jx + dx)), sy = std::max(0, std::min(lh - 1, jy + dy));
                            const float wgt = (dx ? bx : 1 - bx) * (dy ? by : 1 - by);
                            const unsigned char* q = px + ((size_t)sy * lw + sx) * 4;
                            for (int k = 0; k < 3; ++k) acc[k] += wgt * q[k] * q[3] / 255.0f;
                            acc[3] += wgt * q[3];
                        }
                        for (int k = 0; k < 3; ++k) tc[k] = acc[3] > 0 ? acc[k] / acc[3] : 0;
                        a = col[3] * acc[3] / 255.0f;
                    }
                    float* o = &cv.rgb[((size_t)y * cv.w + x) * 3];
                    for (int k = 0; k < 3; ++k) o[k] = o[k] * (1 - a) + col[k] * tc[k] * a;
                }
        }
}

}   // namespace xpt_test
