#include "tz.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
extern const unsigned char xpt_asset_tzones[];
extern const unsigned int xpt_asset_tzones_size;
extern const unsigned char xpt_asset_basemap[];
extern const unsigned int xpt_asset_basemap_size;
}

namespace xpt {
namespace {

struct TzZone { int q; float lo0, la0, lo1, la1; std::vector<std::vector<ImVec2>> rings; };
struct TzData { std::vector<TzLine> lines; std::vector<TzZone> zones; };

const TzData& tzData() {
    static TzData t;
    static bool done = false;
    if (done) return t;
    done = true;
    const unsigned char* b = xpt_asset_tzones;
    const size_t n = xpt_asset_tzones_size;
    if (n < 12 || std::memcmp(b, "WXTZ", 4) != 0) return t;
    size_t at = 8;
    auto u32 = [&](uint32_t& v) { if (at + 4 > n) return false; v = (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24); at += 4; return true; };
    auto varint = [&](uint64_t& v) { v = 0; int sh = 0; while (at < n) { const unsigned char c = b[at++]; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) return true; sh += 7; if (sh > 63) return false; } return false; };
    auto points = [&](std::vector<ImVec2>& out, float& lo0, float& la0, float& lo1, float& la1) {
        uint64_t np = 0;
        if (!varint(np) || np > 1000000) return false;
        out.reserve((size_t)np);
        int64_t x = 0, y = 0;
        for (uint64_t k = 0; k < np; ++k) {
            uint64_t ux = 0, uy = 0;
            if (!varint(ux) || !varint(uy)) return false;
            x += (int64_t)(ux >> 1) ^ -(int64_t)(ux & 1);
            y += (int64_t)(uy >> 1) ^ -(int64_t)(uy & 1);
            const ImVec2 q((float)(x / 2000.0), (float)(y / 2000.0));
            out.push_back(q);
            lo0 = std::min(lo0, q.x); lo1 = std::max(lo1, q.x); la0 = std::min(la0, q.y); la1 = std::max(la1, q.y);
        }
        return true;
    };
    uint32_t nl = 0;
    if (!u32(nl)) return t;
    t.lines.reserve(nl);
    for (uint32_t i = 0; i < nl; ++i) {
        TzLine L; L.lo0 = L.la0 = 1e9f; L.lo1 = L.la1 = -1e9f;
        if (!points(L.pts, L.lo0, L.la0, L.lo1, L.la1)) { t.lines.clear(); return t; }
        t.lines.push_back(std::move(L));
    }
    uint32_t nz = 0;
    if (!u32(nz)) return t;
    for (uint32_t i = 0; i < nz && at < n; ++i) {
        TzZone z; z.q = (int)(signed char)b[at++]; z.lo0 = z.la0 = 1e9f; z.lo1 = z.la1 = -1e9f;
        uint64_t nr = 0;
        if (!varint(nr) || nr > 100000) break;
        z.rings.resize((size_t)nr);
        bool ok = true;
        for (auto& r : z.rings) if (!(ok = points(r, z.lo0, z.la0, z.lo1, z.la1))) break;
        if (!ok) break;
        t.zones.push_back(std::move(z));
    }
    return t;
}

} // namespace

const std::vector<TzLine>& tzLines() { return tzData().lines; }

const std::vector<GeoLine>& basemapLines() {
    static std::vector<GeoLine> lines;
    static bool done = false;
    if (done) return lines;
    done = true;
    const unsigned char* b = xpt_asset_basemap;
    const size_t n = xpt_asset_basemap_size;
    if (n < 12 || std::memcmp(b, "WXBM", 4) != 0) return lines;
    size_t at = 12;
    auto varint = [&](uint64_t& v) { v = 0; int sh = 0; while (at < n) { const unsigned char c = b[at++]; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) return true; sh += 7; if (sh > 63) return false; } return false; };
    const uint32_t count = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    lines.reserve(count);
    for (uint32_t i = 0; i < count && at < n; ++i) {
        GeoLine L;
        L.kind = b[at++];
        uint64_t np = 0;
        if (!varint(np) || np > 1000000) break;
        L.pts.reserve((size_t)np);
        int64_t x = 0, y = 0;
        L.lo0 = L.la0 = 1e9f; L.lo1 = L.la1 = -1e9f;
        for (uint64_t k = 0; k < np; ++k) {
            uint64_t ux = 0, uy = 0;
            if (!varint(ux) || !varint(uy)) break;
            x += (int64_t)(ux >> 1) ^ -(int64_t)(ux & 1);
            y += (int64_t)(uy >> 1) ^ -(int64_t)(uy & 1);
            const ImVec2 q((float)(x / 2000.0), (float)(y / 2000.0));
            L.pts.push_back(q);
            L.lo0 = std::min(L.lo0, q.x); L.lo1 = std::max(L.lo1, q.x); L.la0 = std::min(L.la0, q.y); L.la1 = std::max(L.la1, q.y);
        }
        lines.push_back(std::move(L));
    }
    return lines;
}

// The zone shapes for the lookup were simplified more than the drawn lines (0.02 against 0.01 degrees), so on their own they
// put a border up to a mile or two from where the map draws it. inPolys is the plain lookup; tzStdQuarterHours below makes the drawn
// line the border: near one, the zones a little way out on its two sides are asked, and the point takes the zone of its side.
static bool inPolys(double lat, double lon, int& q) {
    if (lon > 180) lon -= 360; else if (lon < -180) lon += 360;
    const float x = (float)lon, y = (float)lat;
    for (const TzZone& z : tzData().zones) {
        if (x < z.lo0 || x > z.lo1 || y < z.la0 || y > z.la1) continue;
        bool in = false;
        for (const auto& r : z.rings)
            for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
                if ((r[i].y > y) != (r[j].y > y) && x < (r[j].x - r[i].x) * (y - r[i].y) / (r[j].y - r[i].y) + r[i].x) in = !in;
        if (in) { q = z.q; return true; }
    }
    return false;
}
bool tzStdQuarterHours(double lat, double lon, int& q) {
    // the same point as a moment ago (the aircraft and the pointer are asked every frame): its answer again (exact match only, so a
    // crossing is never missed however close the map is zoomed)
    struct Hit { double lat = 1e9, lon = 1e9; int q = 0; bool ok = false; };
    static Hit cache[4]; static int next = 0;
    for (const Hit& h : cache) if (lat == h.lat && lon == h.lon) { q = h.q; return h.ok; }
    if (lon > 180) lon -= 360; else if (lon < -180) lon += 360;
    int base = 0;
    bool ok = inPolys(lat, lon, base);
    // the nearest drawn border within reach (lengths in degrees of latitude: longitude scaled by cos(lat))
    const double reach = 0.06, kx = std::max(0.05, std::cos(lat * 3.14159265358979 / 180.0));
    double best = reach * reach, fx = 0, fy = 0, nx = 0, ny = 0;
    bool found = false;
    for (const TzLine& L : tzLines()) {
        if (lon < L.lo0 - reach / kx || lon > L.lo1 + reach / kx || lat < L.la0 - reach || lat > L.la1 + reach) continue;
        for (size_t i = 1; i < L.pts.size(); ++i) {
            const double ax = (L.pts[i - 1].x - lon) * kx, ay = L.pts[i - 1].y - lat, bx = (L.pts[i].x - lon) * kx, by = L.pts[i].y - lat;
            const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            if (l2 < 1e-14) continue;
            const double t = std::max(0.0, std::min(1.0, -(ax * dx + ay * dy) / l2));
            const double px = ax + dx * t, py = ay + dy * t, d2 = px * px + py * py;
            if (d2 < best) { best = d2; fx = px; fy = py; const double l = std::sqrt(l2); nx = -dy / l; ny = dx / l; found = true; }
        }
    }
    if (found) {   // the zones just beyond the line on each side; if they differ, the line decides
        const double off = 0.07;
        int qa = 0, qb = 0;
        const bool okA = inPolys(lat + fy + ny * off, lon + (fx + nx * off) / kx, qa);
        const bool okB = inPolys(lat + fy - ny * off, lon + (fx - nx * off) / kx, qb);
        if (okA && okB && qa != qb) {
            const double side = (0 - fx) * nx + (0 - fy) * ny;   // the point (the origin here) against the line's normal
            base = side >= 0 ? qa : qb; ok = true;
        }
    }
    Hit hit; hit.lat = lat; hit.lon = lon; hit.q = base; hit.ok = ok;
    cache[next] = hit; next = (next + 1) % 4;
    q = base;
    return ok;
}

} // namespace xpt
