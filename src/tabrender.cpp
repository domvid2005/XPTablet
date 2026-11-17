#include "tabrender.h"

#include <algorithm>
#include <cmath>

namespace xpt {
namespace {

ImDrawVert lerpV(const ImDrawVert& a, const ImDrawVert& b, float t) {
    ImDrawVert o;
    o.pos = ImVec2(a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t);
    o.uv = ImVec2(a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t);
    ImU32 c = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = (float)((a.col >> s) & 0xFF), cb = (float)((b.col >> s) & 0xFF);
        c |= (ImU32)std::lround(ca + (cb - ca) * t) << s;
    }
    o.col = c;
    return o;
}

// Sutherland-Hodgman against one edge. axis 0: x, 1: y. keepGreater: keep the side where the coordinate >= limit.
int clipEdge(const ImDrawVert* in, int n, ImDrawVert* out, int axis, float limit, bool keepGreater) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ImDrawVert& a = in[i];
        const ImDrawVert& b = in[(i + 1) % n];
        const float ca = axis ? a.pos.y : a.pos.x, cb = axis ? b.pos.y : b.pos.x;
        const bool ia = keepGreater ? ca >= limit : ca <= limit, ib = keepGreater ? cb >= limit : cb <= limit;
        if (ia) out[m++] = a;
        if (ia != ib) out[m++] = lerpV(a, b, (limit - ca) / (cb - ca));
    }
    return m;
}

} // namespace

void clipDrawData(const ImDrawData* dd, std::vector<Batch>& out) {
    out.clear();
    if (!dd) return;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const float x0 = cmd.ClipRect.x, y0 = cmd.ClipRect.y, x1 = cmd.ClipRect.z, y1 = cmd.ClipRect.w;
            if (x1 <= x0 || y1 <= y0) continue;
            const ImTextureID tex = cmd.GetTexID();
            if (out.empty() || out.back().tex != tex) { out.emplace_back(); out.back().tex = tex; }
            std::vector<ImDrawVert>& dst = out.back().v;
            for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
                ImDrawVert tri[3];
                bool inside = true;
                for (int k = 0; k < 3; ++k) {
                    tri[k] = cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + e + k]];
                    inside = inside && tri[k].pos.x >= x0 && tri[k].pos.x <= x1 && tri[k].pos.y >= y0 && tri[k].pos.y <= y1;
                }
                if (inside) { dst.insert(dst.end(), tri, tri + 3); continue; }
                const float mnx = std::min({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x}), mxx = std::max({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x});
                const float mny = std::min({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y}), mxy = std::max({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y});
                if (mxx <= x0 || mnx >= x1 || mxy <= y0 || mny >= y1) continue;   // wholly outside
                ImDrawVert a[8], b[8];   // a triangle cut by four edges has at most 7 corners
                int m = clipEdge(tri, 3, a, 0, x0, true);
                m = clipEdge(a, m, b, 0, x1, false);
                m = clipEdge(b, m, a, 1, y0, true);
                m = clipEdge(a, m, b, 1, y1, false);
                for (int k = 1; k + 1 < m; ++k) { dst.push_back(b[0]); dst.push_back(b[k]); dst.push_back(b[k + 1]); }
            }
        }
    }
}

void scaleColors(std::vector<Batch>& batches, float k) {
    if (k >= 0.999f) return;
    const unsigned m = (unsigned)(std::max(0.0f, k) * 256.0f + 0.5f);
    for (Batch& b : batches)
        for (ImDrawVert& v : b.v) {
            const ImU32 c = v.col;
            const unsigned r = ((c >> IM_COL32_R_SHIFT) & 0xFF) * m >> 8, g = ((c >> IM_COL32_G_SHIFT) & 0xFF) * m >> 8, bl = ((c >> IM_COL32_B_SHIFT) & 0xFF) * m >> 8;
            v.col = (c & IM_COL32_A_MASK) | (r << IM_COL32_R_SHIFT) | (g << IM_COL32_G_SHIFT) | (bl << IM_COL32_B_SHIFT);
        }
}

} // namespace xpt
