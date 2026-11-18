// One airport's apt.dat rows into the layout the Map page draws (src/aptgeo.h), and the triangulator for its pavement.
// apt.dat 12.00: https://developer.x-plane.com/article/airport-data-apt-dat-12-00-file-format-specification/
#include "aptgeo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace xpt {
namespace {

using P2 = std::pair<double, double>;

double cross(const P2& o, const P2& a, const P2& b) { return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first); }
double ringArea(const std::vector<P2>& pts, const std::vector<int>& r) {
    double a = 0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) a += pts[r[j]].first * pts[r[i]].second - pts[r[i]].first * pts[r[j]].second;
    return a * 0.5;
}
bool inTri(const P2& a, const P2& b, const P2& c, const P2& p) {
    const double d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
    return d1 >= 0 && d2 >= 0 && d3 >= 0;   // a, b, c counter-clockwise; on an edge counts as inside
}

} // namespace

// Ear clipping, with each hole first joined to the outer ring by a bridge from its right-most point (the usual way: a ray to the
// right finds the outer edge it reaches first, and the bridge goes to the vertex of that edge, or to a vertex closer in angle).
std::vector<int> triangulate(const std::vector<std::vector<P2>>& rings) {
    std::vector<int> out;
    if (rings.empty()) return out;
    std::vector<P2> pts;
    std::vector<std::vector<int>> rr;
    for (const auto& ring : rings) {
        std::vector<int> idx;
        for (const P2& p : ring) {
            const int i = (int)pts.size();
            pts.push_back(p);
            if (!idx.empty() && std::fabs(pts[idx.back()].first - p.first) < 1e-6 && std::fabs(pts[idx.back()].second - p.second) < 1e-6) continue;
            idx.push_back(i);
        }
        while (idx.size() > 1 && std::fabs(pts[idx.front()].first - pts[idx.back()].first) < 1e-6 && std::fabs(pts[idx.front()].second - pts[idx.back()].second) < 1e-6) idx.pop_back();
        rr.push_back(idx);
    }
    std::vector<int> ring = rr[0];
    if (ring.size() < 3) return out;
    if (ringArea(pts, ring) < 0) std::reverse(ring.begin(), ring.end());   // the outer ring counter-clockwise
    // the holes, clockwise, right-most first
    std::vector<std::vector<int>> holes;
    for (size_t h = 1; h < rr.size(); ++h) {
        if (rr[h].size() < 3) continue;
        std::vector<int> hole = rr[h];
        if (ringArea(pts, hole) > 0) std::reverse(hole.begin(), hole.end());
        holes.push_back(hole);
    }
    auto maxX = [&](const std::vector<int>& r) { double m = -1e300; for (int i : r) m = std::max(m, pts[i].first); return m; };
    std::sort(holes.begin(), holes.end(), [&](const std::vector<int>& a, const std::vector<int>& b) { return maxX(a) > maxX(b); });
    for (const auto& hole : holes) {
        size_t mi = 0;
        for (size_t k = 1; k < hole.size(); ++k) if (pts[hole[k]].first > pts[hole[mi]].first) mi = k;
        const P2 M = pts[hole[mi]];
        double bestX = 1e300;
        int bestK = -1;
        const size_t n = ring.size();
        for (size_t k = 0; k < n; ++k) {
            const P2& a = pts[ring[k]];
            const P2& b = pts[ring[(k + 1) % n]];
            if ((a.second > M.second) == (b.second > M.second) && a.second != M.second) continue;
            if (a.second == b.second) continue;
            const double x = a.first + (M.second - a.second) * (b.first - a.first) / (b.second - a.second);
            if (x < M.first - 1e-9 || x >= bestX) continue;
            bestX = x;
            bestK = a.first > b.first ? (int)k : (int)((k + 1) % n);
        }
        if (bestK < 0) continue;   // not inside the outer ring: dropped
        // a vertex inside the triangle (M, the ray's hit, the edge's vertex) closer in angle is the one to bridge to
        {
            const P2 I(bestX, M.second), Pv = pts[ring[bestK]];
            double bestAng = 1e300;
            for (size_t k = 0; k < n; ++k) {
                const P2& v = pts[ring[k]];
                if ((int)k == bestK || v.first < M.first) continue;
                P2 a = M, b = I, c = Pv;
                if (cross(a, b, c) < 0) std::swap(b, c);
                if (!inTri(a, b, c, v)) continue;
                const double ang = std::fabs(v.second - M.second) / std::max(1e-9, v.first - M.first);
                if (ang < bestAng) { bestAng = ang; bestK = (int)k; }
            }
        }
        std::vector<int> joined;
        joined.reserve(n + hole.size() + 2);
        for (int k = 0; k <= bestK; ++k) joined.push_back(ring[k]);
        for (size_t k = 0; k < hole.size(); ++k) joined.push_back(hole[(mi + k) % hole.size()]);
        joined.push_back(hole[mi]);
        joined.push_back(ring[bestK]);
        for (size_t k = bestK + 1; k < n; ++k) joined.push_back(ring[k]);
        ring.swap(joined);
    }
    // ear clipping over a linked list
    const int n = (int)ring.size();
    std::vector<int> prev(n), next(n);
    for (int i = 0; i < n; ++i) { prev[i] = (i + n - 1) % n; next[i] = (i + 1) % n; }
    int left = n, cur = 0, fails = 0;
    auto same = [&](int a, int b) { return std::fabs(pts[a].first - pts[b].first) < 1e-6 && std::fabs(pts[a].second - pts[b].second) < 1e-6; };
    auto isEar = [&](int i) {
        const int a = ring[prev[i]], b = ring[i], c = ring[next[i]];
        if (cross(pts[a], pts[b], pts[c]) <= 0) return false;
        for (int j = next[next[i]]; j != prev[i]; j = next[j]) {
            const int v = ring[j];
            if (same(v, a) || same(v, b) || same(v, c)) continue;
            if (inTri(pts[a], pts[b], pts[c], pts[v])) return false;
        }
        return true;
    };
    out.reserve((size_t)(n - 2) * 3);
    while (left > 3) {
        const int a = ring[prev[cur]], b = ring[cur], c = ring[next[cur]];
        const double cr = cross(pts[a], pts[b], pts[c]);
        bool cut = false;
        if (std::fabs(cr) < 1e-9) cut = true;                       // a point on a straight line: dropped without a triangle
        else if (isEar(cur)) { out.push_back(a); out.push_back(b); out.push_back(c); cut = true; }
        else if (++fails > left) {                                   // no ear anywhere (a self-touching ring): cut here anyway
            if (cr > 0) { out.push_back(a); out.push_back(b); out.push_back(c); }
            cut = true;
        }
        if (cut) {
            next[prev[cur]] = next[cur];
            prev[next[cur]] = prev[cur];
            cur = next[cur];
            --left;
            fails = 0;
        } else {
            cur = next[cur];
        }
    }
    if (left == 3) {
        const int a = ring[prev[cur]], b = ring[cur], c = ring[next[cur]];
        if (cross(pts[a], pts[b], pts[c]) > 0) { out.push_back(a); out.push_back(b); out.push_back(c); }
    }
    return out;
}

namespace {

struct Node { double lat, lon; bool ctl; double clat, clon; int line; };

// The words of a row, without copying
struct Words {
    const char* w[64];
    int len[64];
    int n = 0;
    void split(const char* s, const char* e) {
        n = 0;
        while (s < e && n < 64) {
            while (s < e && (*s == ' ' || *s == '\t')) ++s;
            if (s >= e) break;
            const char* b = s;
            while (s < e && *s != ' ' && *s != '\t') ++s;
            w[n] = b; len[n] = (int)(s - b); ++n;
        }
    }
    double d(int i) const { return i < n ? std::strtod(w[i], nullptr) : 0.0; }
    int i(int k) const { return k < n ? std::atoi(w[k]) : 0; }
    std::string s(int k) const { return k < n ? std::string(w[k], len[k]) : std::string(); }
    std::string rest(int k, const char* end) const { return k < n ? std::string(w[k], end) : std::string(); }
};

// a node's segment to the next, as points (curved where either end has a control point), the last point left out
void segment(const Node& a, const Node& b, std::vector<P2>& out) {
    out.push_back(P2(a.lat, a.lon));
    if (!a.ctl && !b.ctl) return;
    const P2 p0(a.lat, a.lon), p3(b.lat, b.lon);
    const P2 p1 = a.ctl ? P2(a.clat, a.clon) : p0;
    const P2 p2 = b.ctl ? P2(2 * b.lat - b.clat, 2 * b.lon - b.clon) : p3;   // b's control point mirrored: the curve arriving
    const int steps = 8;
    for (int k = 1; k < steps; ++k) {
        const double t = (double)k / steps, u = 1 - t;
        const double c0 = u * u * u, c1 = 3 * u * u * t, c2 = 3 * u * t * t, c3 = t * t * t;
        out.push_back(P2(c0 * p0.first + c1 * p1.first + c2 * p2.first + c3 * p3.first, c0 * p0.second + c1 * p1.second + c2 * p2.second + c3 * p3.second));
    }
}

int lineKind(int code) {
    if (code > 50 && code < 100) code -= 50;   // the same lines with a black border
    switch (code) {
    case 1: return 0;                              // taxiway centreline
    case 4: return 1;                              // runway hold position
    case 5: case 6: return 2;                      // other hold position, ILS critical area
    case 3: case 7: case 8: case 9: case 10: return 3;   // taxiway edges
    default: return -1;
    }
}

} // namespace

bool parseAirport(const std::string& rows, AptGeo& g) {
    Words w;
    enum { NONE, PAVE, LINEAR, OTHER } feat = NONE;
    std::vector<std::vector<Node>> rings;   // the feature's rings (pavement) or pieces (lines)
    std::vector<Node> nodes;
    bool closed = false;
    std::vector<std::vector<std::vector<P2>>> paves;   // every pavement's rings, lat/lon, for triangulating at the end
    struct RawLine { std::vector<Node> nodes; bool closed; };
    std::vector<RawLine> rawLines;
    std::map<int, P2> taxiNodes;
    struct Edge { int a, b; std::string name; bool taxiway; };
    std::vector<Edge> edges;
    bool haveRef = false;
    double dLat = 0, dLon = 0;
    bool haveDatumLat = false, haveDatumLon = false;

    auto endRing = [&](bool isClosed) {
        if (nodes.size() >= 2) {
            if (feat == PAVE) rings.push_back(nodes);
            else if (feat == LINEAR) rawLines.push_back({nodes, isClosed});
        }
        nodes.clear();
    };
    auto finish = [&]() {
        if (!nodes.empty()) endRing(closed);
        if (feat == PAVE && !rings.empty()) {
            std::vector<std::vector<P2>> rs;
            for (const auto& r : rings) {
                std::vector<P2> pts;
                for (size_t k = 0; k < r.size(); ++k) segment(r[k], r[(k + 1) % r.size()], pts);
                if (pts.size() >= 3) rs.push_back(std::move(pts));
            }
            if (!rs.empty()) paves.push_back(std::move(rs));
        }
        rings.clear();
        feat = NONE;
    };

    const char* s = rows.data();
    const char* end = s + rows.size();
    while (s < end) {
        const char* e = (const char*)std::memchr(s, '\n', (size_t)(end - s));
        if (!e) e = end;
        const char* le = e;
        if (le > s && le[-1] == '\r') --le;
        w.split(s, le);
        s = e + 1;
        if (w.n == 0) continue;
        const int code = w.i(0);
        if (code >= 111 && code <= 116) {
            if (feat == NONE || feat == OTHER) continue;
            Node nd;
            nd.lat = w.d(1); nd.lon = w.d(2);
            nd.ctl = code == 112 || code == 114 || code == 116;
            nd.clat = nd.ctl ? w.d(3) : 0; nd.clon = nd.ctl ? w.d(4) : 0;
            nd.line = nd.ctl ? w.i(5) : w.i(3);
            nodes.push_back(nd);
            if (code == 113 || code == 114) { closed = true; endRing(true); }
            else if (code == 115 || code == 116) { closed = false; endRing(false); }
            continue;
        }
        if (feat != NONE) finish();
        switch (code) {
        case 1: case 16: case 17:
            g.kind = code;
            g.elevationFt = (float)w.d(1);
            g.icao = w.s(4);
            g.name = w.rest(5, le);
            break;
        case 50: case 51: case 52: case 53: case 54: case 55: case 56:              // radio, in 10 kHz (older files)
        case 1050: case 1051: case 1052: case 1053: case 1054: case 1055: case 1056: {   // radio, in kHz (8.33 kHz spacing)
            AptGeo::Freq fq;
            fq.kind = code >= 1050 ? code - 1050 : code - 50;
            fq.mhz = code >= 1050 ? w.d(1) / 1000.0 : w.d(1) / 100.0;
            fq.name = w.rest(2, le);
            if (fq.mhz > 100 && fq.mhz < 140) g.freqs.push_back(fq);
            break;
        }
        case 110: feat = PAVE; closed = true; break;
        case 120: feat = LINEAR; closed = false; break;
        case 130: feat = OTHER; break;
        case 100: {
            AptGeo::Runway r;
            r.widthM = (float)w.d(1);
            r.idA = w.s(8); r.idB = w.s(17);
            r.a = mercOf(w.d(9), w.d(10));
            r.b = mercOf(w.d(18), w.d(19));
            g.runways.push_back(r);
            AptGeo::RunwayInfo ri;
            ri.surface = w.i(2);
            ri.lit = w.i(6) > 0;
            ri.displacedA = (float)w.d(11);
            ri.displacedB = (float)w.d(20);
            g.runwayInfo.push_back(ri);
            if (!haveRef) { haveRef = true; latLonOf(MercPt{(r.a.x + r.b.x) * 0.5, (r.a.y + r.b.y) * 0.5}, g.lat, g.lon); }
            break;
        }
        case 101: {
            AptGeo::Runway r;
            r.widthM = (float)w.d(1);
            r.water = true;
            r.idA = w.s(3); r.idB = w.s(6);
            r.a = mercOf(w.d(4), w.d(5));
            r.b = mercOf(w.d(7), w.d(8));
            g.runways.push_back(r);
            AptGeo::RunwayInfo ri;
            ri.surface = 13;   // water
            g.runwayInfo.push_back(ri);
            if (!haveRef) { haveRef = true; latLonOf(MercPt{(r.a.x + r.b.x) * 0.5, (r.a.y + r.b.y) * 0.5}, g.lat, g.lon); }
            break;
        }
        case 102: {
            AptGeo::Helipad h;
            h.id = w.s(1);
            h.p = mercOf(w.d(2), w.d(3));
            g.helipads.push_back(h);
            if (!haveRef) { haveRef = true; g.lat = w.d(2); g.lon = w.d(3); }
            break;
        }
        case 1201: taxiNodes[w.i(4)] = P2(w.d(1), w.d(2)); break;
        case 1202: {
            const std::string type = w.s(4);
            edges.push_back({w.i(1), w.i(2), w.s(5), type.compare(0, 7, "taxiway") == 0});
            break;
        }
        case 1300: {
            AptGeo::Stand st;
            st.p = mercOf(w.d(1), w.d(2));
            st.hdg = (float)w.d(3);
            st.type = w.s(4);
            st.uses = w.s(5);
            st.name = w.rest(6, le);
            g.stands.push_back(st);
            break;
        }
        case 1302: {
            const std::string k = w.s(1);
            if (k == "datum_lat" && w.n > 2) { dLat = w.d(2); haveDatumLat = true; }
            else if (k == "datum_lon" && w.n > 2) { dLon = w.d(2); haveDatumLon = true; }
            break;
        }
        default: break;
        }
    }
    if (feat != NONE) finish();
    if (haveDatumLat && haveDatumLon && (dLat != 0 || dLon != 0)) { g.lat = dLat; g.lon = dLon; haveRef = true; }
    if (!haveRef) {
        if (!paves.empty()) { g.lat = paves[0][0][0].first; g.lon = paves[0][0][0].second; haveRef = true; }
        else if (!g.stands.empty()) { latLonOf(g.stands[0].p, g.lat, g.lon); haveRef = true; }
    }
    if (!haveRef) return false;

    // the pavement, triangulated in metres around the reference point
    const double kD = 3.14159265358979323846 / 180.0;
    const double mx = std::cos(g.lat * kD) * 111320.0, my = 110540.0;
    for (const auto& rs : paves) {
        std::vector<std::vector<P2>> metres;
        std::vector<P2> flat;
        for (const auto& r : rs) {
            std::vector<P2> m;
            m.reserve(r.size());
            for (const P2& p : r) { m.push_back(P2((p.second - g.lon) * mx, (p.first - g.lat) * my)); flat.push_back(p); }
            metres.push_back(std::move(m));
        }
        // triangulate() numbers the points of all rings in order, but skips repeats; numbering is by position in `flat`
        const std::vector<int> tri = triangulate(metres);
        for (int i : tri) if (i >= 0 && i < (int)flat.size()) g.pavement.push_back(mercOf(flat[i].first, flat[i].second));
    }
    // the painted lines: each run of segments of one kind becomes a line
    bool anyCentre = false;
    for (const RawLine& rl : rawLines) {
        const size_t n = rl.nodes.size();
        const size_t segs = rl.closed ? n : n - 1;
        AptGeo::Line cur;
        cur.kind = -1;
        auto flush = [&]() { if (cur.kind >= 0 && cur.pts.size() >= 2) { if (cur.kind == 0) anyCentre = true; g.lines.push_back(cur); } cur.pts.clear(); cur.kind = -1; };
        for (size_t k = 0; k < segs; ++k) {
            const Node& a = rl.nodes[k];
            const Node& b = rl.nodes[(k + 1) % n];
            const int kind = lineKind(a.line);
            if (kind != cur.kind) { flush(); cur.kind = kind; }
            if (kind < 0) continue;
            std::vector<P2> pts;
            segment(a, b, pts);
            for (const P2& p : pts) cur.pts.push_back(mercOf(p.first, p.second));
            if (k + 1 == segs || lineKind(b.line) != kind) cur.pts.push_back(mercOf(b.lat, b.lon));
        }
        flush();
    }
    // the taxi routes: their names (once each, at the middle of the longest piece), and the centrelines where none are painted
    std::map<std::string, std::pair<double, MercPt>> best;
    for (const Edge& ed : edges) {
        const auto ia = taxiNodes.find(ed.a), ib = taxiNodes.find(ed.b);
        if (ia == taxiNodes.end() || ib == taxiNodes.end() || !ed.taxiway) continue;
        const MercPt a = mercOf(ia->second.first, ia->second.second), b = mercOf(ib->second.first, ib->second.second);
        if (!anyCentre) { AptGeo::Line L; L.kind = 0; L.pts = {a, b}; g.lines.push_back(L); }
        if (ed.name.empty()) continue;
        const double len = std::hypot(a.x - b.x, a.y - b.y);
        auto it = best.find(ed.name);
        if (it == best.end() || len > it->second.first) best[ed.name] = {len, MercPt{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}};
    }
    for (const auto& kv : best) g.taxiways.push_back({kv.second.second, kv.first});
    // the bounds
    g.lo = g.hi = mercOf(g.lat, g.lon);
    auto grow = [&](const MercPt& p) { g.lo.x = std::min(g.lo.x, p.x); g.lo.y = std::min(g.lo.y, p.y); g.hi.x = std::max(g.hi.x, p.x); g.hi.y = std::max(g.hi.y, p.y); };
    for (const MercPt& p : g.pavement) grow(p);
    for (const auto& r : g.runways) { grow(r.a); grow(r.b); }
    for (const auto& st : g.stands) grow(st.p);
    return !g.pavement.empty() || !g.runways.empty() || !g.helipads.empty();
}

} // namespace xpt
