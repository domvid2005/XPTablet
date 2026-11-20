// XPTablet's arcade games: Contrail, Taxi Rush, Sky Patrol and Hangar Climb. Every rule, map, sprite and name is XPTablet's own
// (drawn with lines and shapes, nothing taken from another game); the ideas are the old arcade kinds every such game shares.
#include "arcade.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace xpt {
namespace arcade {
namespace {

constexpr float kPi = 3.14159265f;
std::mt19937& rng() { static std::mt19937 r(12345u); return r; }
float frand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng()); }
int irand(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng()); }

// the field: a virtual W x H drawn into the box (kept to its aspect, centred)
struct View {
    ImVec2 o; float k = 1;
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
    float S(float v) const { return v * k; }
};
View fit(ImVec2 a, ImVec2 b, float W, float H) {
    View v;
    v.k = std::min((b.x - a.x) / W, (b.y - a.y) / H);
    v.o = ImVec2(std::floor((a.x + b.x - W * v.k) * 0.5f), std::floor((a.y + b.y - H * v.k) * 0.5f));
    return v;
}
ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// an aircraft from above, nose along `ang` (0: right, pi/2: down), about `len` long
void plane(ImDrawList* dl, ImVec2 c, float len, float ang, ImU32 col) {
    const float ca = std::cos(ang), sa = std::sin(ang);
    auto R = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * len, c.y + (x * sa + y * ca) * len); };
    const ImVec2 body[4] = {R(0.5f, 0), R(0.1f, 0.07f), R(-0.5f, 0.05f), R(-0.5f, -0.05f)};
    dl->AddConvexPolyFilled(body, 4, col);
    const ImVec2 body2[3] = {R(0.5f, 0), R(-0.5f, -0.05f), R(0.1f, -0.07f)};
    dl->AddConvexPolyFilled(body2, 3, col);
    const ImVec2 wing[4] = {R(0.12f, 0), R(-0.12f, 0.48f), R(-0.22f, 0.48f), R(-0.08f, 0)};
    dl->AddConvexPolyFilled(wing, 4, col);
    const ImVec2 wing2[4] = {R(0.12f, 0), R(-0.08f, 0), R(-0.22f, -0.48f), R(-0.12f, -0.48f)};
    dl->AddConvexPolyFilled(wing2, 4, col);
    const ImVec2 tail[4] = {R(-0.36f, 0), R(-0.48f, 0.2f), R(-0.54f, 0.2f), R(-0.5f, 0)};
    dl->AddConvexPolyFilled(tail, 4, col);
    const ImVec2 tail2[4] = {R(-0.36f, 0), R(-0.5f, 0), R(-0.54f, -0.2f), R(-0.48f, -0.2f)};
    dl->AddConvexPolyFilled(tail2, 4, col);
}
void centred(ImDrawList* dl, ImFont* f, float px, ImVec2 c, ImU32 col, const char* t) {
    const ImVec2 sz = f->CalcTextSizeA(px, 1e9f, 0, t);
    dl->AddText(f, px, ImVec2(std::floor(c.x - sz.x * 0.5f), std::floor(c.y - sz.y * 0.5f)), col, t);
}
// a dark band with a message, across the field
void banner(ImDrawList* dl, const View& v, float W, float H, ImFont* big, float bigPx, ImFont* small, float smallPx, const char* t, const char* sub) {
    const ImVec2 c = v.P(W * 0.5f, H * 0.5f);   // (the band in screen pixels, whatever the field's own units)
    dl->AddRectFilled(ImVec2(v.o.x, c.y - 34), ImVec2(v.o.x + W * v.k, c.y + 34), rgba(0, 0, 0, 150));
    centred(dl, big, bigPx, ImVec2(c.x, c.y - 10), rgba(255, 255, 255), t);
    if (sub) centred(dl, small, smallPx, ImVec2(c.x, c.y + 18), rgba(220, 228, 240), sub);
}
bool anyHit(const Input& in) { return in.leftHit || in.rightHit || in.upHit || in.downHit || in.actionHit; }

// ================================================================================================================================
// Contrail: an aircraft that leaves a growing trail; it takes on fuel and must never cross its own contrail or leave the airspace
struct Contrail {
    static constexpr int W = 28, H = 20;
    std::deque<std::pair<int, int>> trail;   // the head first
    int dir = 0, want[2] = {-1, -1};         // 0 right, 1 down, 2 left, 3 up; turns waiting
    std::pair<int, int> fuel{0, 0};
    float acc = 0, t = 0;
    int score = 0; bool over = false, ready = true, started = false;
    void placeFuel() {
        for (;;) {
            const int x = irand(1, W - 2), y = irand(1, H - 2);
            bool on = false;
            for (const auto& c : trail) if (c.first == x && c.second == y) on = true;
            if (!on) { fuel = {x, y}; return; }
        }
    }
    void reset() {
        trail.clear();
        for (int i = 0; i < 4; ++i) trail.push_back({8 - i, H / 2});
        dir = 0; want[0] = want[1] = -1; acc = 0; score = 0; over = false; ready = true; started = true;
        placeFuel();
    }
    void turn(int d) {
        const int last = want[0] >= 0 ? (want[1] >= 0 ? want[1] : want[0]) : dir;
        if (d == last || (d + 2) % 4 == last) return;
        if (want[0] < 0) want[0] = d; else if (want[1] < 0) want[1] = d;
    }
    void step(const Input& in, float dt) {
        t += dt;
        if (over) return;
        if (in.rightHit) turn(0); if (in.downHit) turn(1); if (in.leftHit) turn(2); if (in.upHit) turn(3);
        if (ready) { if (anyHit(in)) ready = false; else return; }
        acc += dt;
        const float every = std::max(0.065f, 0.15f - trail.size() * 0.0025f);
        while (acc >= every && !over) {
            acc -= every;
            if (want[0] >= 0) { dir = want[0]; want[0] = want[1]; want[1] = -1; }
            static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
            const std::pair<int, int> head{trail.front().first + dx[dir], trail.front().second + dy[dir]};
            if (head.first < 0 || head.second < 0 || head.first >= W || head.second >= H) { over = true; break; }
            const bool eat = head == fuel;
            if (!eat) trail.pop_back();
            for (const auto& c : trail) if (c == head) over = true;
            if (over) break;
            trail.push_front(head);
            if (eat) { score += 10; placeFuel(); }
        }
    }
    void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx) {
        const View v = fit(a, b, W, H);
        dl->AddRectFilledMultiColor(v.P(0, 0), v.P(W, H), rgba(40, 92, 160), rgba(40, 92, 160), rgba(118, 170, 220), rgba(118, 170, 220));
        // the airspace's edge, dashed
        for (float x = 0; x < W; x += 1.0f) { dl->AddLine(v.P(x, 0.1f), v.P(x + 0.5f, 0.1f), rgba(255, 255, 255, 120), v.S(0.12f)); dl->AddLine(v.P(x, H - 0.1f), v.P(x + 0.5f, H - 0.1f), rgba(255, 255, 255, 120), v.S(0.12f)); }
        for (float y = 0; y < H; y += 1.0f) { dl->AddLine(v.P(0.1f, y), v.P(0.1f, y + 0.5f), rgba(255, 255, 255, 120), v.S(0.12f)); dl->AddLine(v.P(W - 0.1f, y), v.P(W - 0.1f, y + 0.5f), rgba(255, 255, 255, 120), v.S(0.12f)); }
        // the fuel: a drop, glowing
        const float pulse = 0.5f + 0.5f * std::sin(t * 6);
        const ImVec2 fc = v.P(fuel.first + 0.5f, fuel.second + 0.55f);
        dl->AddCircleFilled(fc, v.S(0.55f + 0.1f * pulse), rgba(255, 220, 90, 70), 0);
        dl->AddCircleFilled(fc, v.S(0.3f), rgba(255, 196, 40), 0);
        const ImVec2 tip[3] = {ImVec2(fc.x, fc.y - v.S(0.55f)), ImVec2(fc.x + v.S(0.27f), fc.y - v.S(0.1f)), ImVec2(fc.x - v.S(0.27f), fc.y - v.S(0.1f))};
        dl->AddConvexPolyFilled(tip, 3, rgba(255, 196, 40));
        // the contrail: puffs, older ones wider and fainter
        const int n = (int)trail.size();
        for (int i = n - 1; i >= 1; --i) {
            const float age = (float)i / std::max(1, n - 1);
            dl->AddCircleFilled(v.P(trail[i].first + 0.5f, trail[i].second + 0.5f), v.S(0.36f + 0.16f * age), rgba(255, 255, 255, (int)(230 - 130 * age)), 0);
        }
        static const float ang[4] = {0, kPi * 0.5f, kPi, -kPi * 0.5f};
        plane(dl, v.P(trail[0].first + 0.5f, trail[0].second + 0.5f), v.S(1.25f), ang[dir], over ? rgba(255, 120, 110) : rgba(250, 252, 255));
        if (over) banner(dl, v, W, H, big, bigPx, small, smallPx, "Airspace bust", "New game to fly again");
        else if (ready) banner(dl, v, W, H, big, bigPx, small, smallPx, "Contrail", "A direction to take off");
    }
};

// ================================================================================================================================
// Taxi Rush: an aircraft taxies the airport's taxiways for every centreline light; four ground vehicles patrol them. A takeoff
// clearance (the big green lights) makes them give way for a while: then they can be sent back to the apron.
struct TaxiRush {
    static constexpr int W = 21, H = 15;
    std::vector<std::string> map;   // '#' grass, '.' a light, 'o' a clearance, ' ' taxiway, 'H' the apron
    struct Mover { int cx = 0, cy = 0, dir = -1; float t = 0; };
    Mover me; int want = -1;
    struct Veh { Mover m; int kind = 0; float wait = 0; bool home = true; };
    Veh veh[4];
    float clearT = 0, modeT = 0, t = 0, deadT = 0; int combo = 0;
    bool scatter = true;
    int score = 0, livesLeft = 3, level = 1, lights = 0;
    bool over = false, ready = true, started = false;

    static std::string row(const char* half) { std::string s(half); std::string r = s; for (int i = 9; i >= 0; --i) r += s[i]; return r; }
    void buildMap() {
        static const char* halves[H] = {"###########", "#o........#", "#.###.###.#", "#..........", "#.###.#.###", "#.....#...#", "#####.###.#",
                                        "#...#.#HHHH", "#.#.#.#####", "#.#...#....", "#.#.###.###", "#o.........", "#.###.#.###", "#.........P", "###########"};
        map.clear();
        for (int y = 0; y < H; ++y) map.push_back(row(halves[y]));
        lights = 0;
        for (auto& r : map) for (char& c : r) { if (c == 'P') c = ' '; if (c == '.' || c == 'o') ++lights; }
    }
    bool open(int x, int y, bool vehicle) const {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        const char c = map[y][x];
        return c != '#' && (vehicle || c != 'H');
    }
    static void dxy(int d, int& dx, int& dy) { dx = d == 0 ? 1 : d == 2 ? -1 : 0; dy = d == 1 ? 1 : d == 3 ? -1 : 0; }
    ImVec2 pos(const Mover& m) const { int dx = 0, dy = 0; if (m.dir >= 0) dxy(m.dir, dx, dy); return ImVec2(m.cx + 0.5f + dx * m.t, m.cy + 0.5f + dy * m.t); }
    void placeAll() {
        me = Mover(); me.cx = 10; me.cy = 13; me.dir = -1; want = -1;
        static const int sx[4] = {8, 10, 12, 9};
        for (int i = 0; i < 4; ++i) { veh[i].m = Mover(); veh[i].m.cx = sx[i]; veh[i].m.cy = 7; veh[i].kind = i; veh[i].home = true; veh[i].wait = 1.0f + i * 2.5f; }
        clearT = 0; scatter = true; modeT = 0;
    }
    void reset() { buildMap(); score = 0; livesLeft = 3; level = 1; over = false; ready = true; started = true; deadT = 0; placeAll(); }
    void nextLevel() { ++level; buildMap(); placeAll(); ready = true; }
    // moves m by speed*dt; at each cell centre `choose` picks the next direction (or -1 to stop)
    template <class F> void advance(Mover& m, float dist, bool vehicle, F choose) {
        while (dist > 0) {
            if (m.dir < 0) { m.dir = choose(m); if (m.dir < 0) return; m.t = 0; int dx, dy; dxy(m.dir, dx, dy); if (!open(m.cx + dx, m.cy + dy, vehicle)) { m.dir = -1; return; } }
            const float leftToGo = 1.0f - m.t;
            if (dist < leftToGo) { m.t += dist; return; }
            dist -= leftToGo;
            int dx, dy; dxy(m.dir, dx, dy);
            m.cx += dx; m.cy += dy; m.t = 0;
            const int nd = choose(m);
            int ndx, ndy; if (nd >= 0) dxy(nd, ndx, ndy);
            if (nd < 0 || !open(m.cx + ndx, m.cy + ndy, vehicle)) { m.dir = -1; return; }
            m.dir = nd;
        }
    }
    void step(const Input& in, float dt) {
        t += dt;
        if (over) return;
        if (in.rightHit) want = 0; if (in.downHit) want = 1; if (in.leftHit) want = 2; if (in.upHit) want = 3;
        if (in.right) want = 0; else if (in.down) want = 1; else if (in.left) want = 2; else if (in.up) want = 3;
        if (ready) { if (anyHit(in)) ready = false; else return; }
        if (deadT > 0) { deadT -= dt; if (deadT <= 0) { if (livesLeft <= 0) over = true; else placeAll(); } return; }
        // the aircraft: a turn back at once, others at the next centre where open
        if (want >= 0 && me.dir >= 0 && want == (me.dir + 2) % 4) { int dx, dy; dxy(me.dir, dx, dy); me.cx += dx; me.cy += dy; me.t = 1.0f - me.t; me.dir = want; }
        const float spMe = 5.2f + 0.25f * level;
        advance(me, spMe * dt, false, [&](const Mover& m) {
            int dx, dy;
            if (want >= 0) { dxy(want, dx, dy); if (open(m.cx + dx, m.cy + dy, false)) return want; }
            if (m.dir >= 0) { dxy(m.dir, dx, dy); if (open(m.cx + dx, m.cy + dy, false)) return m.dir; }
            return -1;
        });
        char& c = map[me.cy][me.cx];
        if (c == '.') { c = ' '; score += 10; --lights; }
        else if (c == 'o') { c = ' '; score += 50; --lights; clearT = std::max(3.0f, 7.0f - level * 0.5f); combo = 0;
                             for (auto& v : veh) if (!v.home && v.m.dir >= 0) { v.m.dir = (v.m.dir + 2) % 4; v.m.t = 1 - v.m.t; int dx, dy; dxy((v.m.dir + 2) % 4, dx, dy); v.m.cx += dx; v.m.cy += dy; } }
        if (lights <= 0) { score += 500; nextLevel(); return; }
        if (clearT > 0) clearT -= dt;
        modeT += dt;
        if (modeT > (scatter ? 6.0f : 18.0f)) { modeT = 0; scatter = !scatter; }
        // the vehicles: each heads for its own target, never turning back at a junction
        const ImVec2 mp = pos(me);
        int mdx = 0, mdy = 0; if (me.dir >= 0) dxy(me.dir, mdx, mdy);
        for (int i = 0; i < 4; ++i) {
            Veh& v = veh[i];
            if (v.wait > 0) { v.wait -= dt; continue; }
            const bool fright = clearT > 0 && !v.home;
            const float sp = (fright ? 3.0f : 4.6f + 0.3f * level) * (map[v.m.cy][v.m.cx] == 'H' ? 0.7f : 1.0f);
            float tx = 0, ty = 0;
            static const float cornX[4] = {19, 1, 19, 1}, cornY[4] = {1, 1, 13, 13};
            if (v.home) { tx = 10; ty = 5; }   // out of the apron, through its two openings
            else if (scatter) { tx = cornX[i]; ty = cornY[i]; }
            else if (i == 0) { tx = mp.x; ty = mp.y; }                                      // the fuel truck: straight for it
            else if (i == 1) { tx = mp.x + mdx * 4; ty = mp.y + mdy * 4; }                  // the tug: where it is going
            else if (i == 2) { const ImVec2 p0 = pos(veh[0].m); tx = 2 * (mp.x + mdx * 2) - p0.x; ty = 2 * (mp.y + mdy * 2) - p0.y; }   // the follow-me car: with the truck
            else { const ImVec2 p = pos(v.m); const float d = std::hypot(p.x - mp.x, p.y - mp.y); if (d > 6) { tx = mp.x; ty = mp.y; } else { tx = cornX[3]; ty = cornY[3]; } }   // the de-icer: near, it shies off
            advance(v.m, sp * dt, true, [&](const Mover& m) {
                int best = -1; float bd = 1e9f;
                std::vector<int> opts;
                for (int d = 0; d < 4; ++d) {
                    if (m.dir >= 0 && d == (m.dir + 2) % 4) continue;
                    int dx, dy; dxy(d, dx, dy);
                    if (!open(m.cx + dx, m.cy + dy, true)) continue;
                    if (!v.home && map[m.cy + dy][m.cx + dx] == 'H' && map[m.cy][m.cx] != 'H') continue;   // (out, it stays out)
                    opts.push_back(d);
                    const float dd = std::hypot(m.cx + dx + 0.5f - tx, m.cy + dy + 0.5f - ty);
                    if (dd < bd) { bd = dd; best = d; }
                }
                if (opts.empty() && m.dir >= 0) return (m.dir + 2) % 4;
                if (fright && !opts.empty()) return opts[irand(0, (int)opts.size() - 1)];
                return best;
            });
            if (v.home && map[v.m.cy][v.m.cx] != 'H') v.home = false;
            const ImVec2 vp = pos(v.m);
            if (std::hypot(vp.x - mp.x, vp.y - mp.y) < 0.65f) {
                if (fright) { ++combo; score += 100 * (1 << std::min(combo, 4)); v.m = Mover(); v.m.cx = 9 + (i % 3); v.m.cy = 7; v.home = true; v.wait = 3.0f; }
                else { --livesLeft; deadT = 1.6f; return; }
            }
        }
    }
    void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx) {
        const View v = fit(a, b, W, H);
        dl->AddRectFilled(v.P(0, 0), v.P(W, H), rgba(54, 58, 66));   // the asphalt
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const char c = map[y][x];
            if (c == '#') dl->AddRectFilled(v.P(x + 0.04f, y + 0.04f), v.P(x + 0.96f, y + 0.96f), rgba(46, 104, 58), v.S(0.22f));
            else if (c == 'H') dl->AddRectFilled(v.P(x, y), v.P(x + 1, y + 1), rgba(70, 76, 88));
            else {   // the yellow centre line between open neighbours
                if (x + 1 < W && map[y][x + 1] != '#' && map[y][x + 1] != 'H' && c != 'H') dl->AddLine(v.P(x + 0.5f, y + 0.5f), v.P(x + 1.5f, y + 0.5f), rgba(214, 180, 40, 120), v.S(0.06f));
                if (y + 1 < H && map[y + 1][x] != '#' && map[y + 1][x] != 'H') dl->AddLine(v.P(x + 0.5f, y + 0.5f), v.P(x + 0.5f, y + 1.5f), rgba(214, 180, 40, 120), v.S(0.06f));
            }
        }
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const char c = map[y][x];
            if (c == '.') dl->AddCircleFilled(v.P(x + 0.5f, y + 0.5f), v.S(0.11f), rgba(90, 255, 140), 0);
            else if (c == 'o') {
                const float p = 0.5f + 0.5f * std::sin(t * 7);
                dl->AddCircleFilled(v.P(x + 0.5f, y + 0.5f), v.S(0.28f + 0.06f * p), rgba(60, 230, 110, 120), 0);
                dl->AddCircleFilled(v.P(x + 0.5f, y + 0.5f), v.S(0.2f), rgba(150, 255, 170), 0);
            }
        }
        // the vehicles: a body, a cab, a flashing amber beacon (blue and white while they give way)
        static const ImU32 vcol[4] = {IM_COL32(230, 70, 60, 255), IM_COL32(250, 170, 40, 255), IM_COL32(240, 220, 60, 255), IM_COL32(120, 200, 240, 255)};
        for (int i = 0; i < 4; ++i) {
            const ImVec2 p = pos(veh[i].m);
            const bool fright = clearT > 0 && !veh[i].home;
            const bool blink = fright && clearT < 1.5f && std::fmod(t, 0.3f) < 0.15f;
            const ImU32 col = fright ? (blink ? rgba(240, 240, 255) : rgba(60, 90, 220)) : vcol[i];
            const bool across = veh[i].m.dir == 0 || veh[i].m.dir == 2 || veh[i].m.dir < 0;
            const float hw = across ? 0.38f : 0.28f, hh = across ? 0.28f : 0.38f;
            dl->AddRectFilled(v.P(p.x - hw, p.y - hh), v.P(p.x + hw, p.y + hh), col, v.S(0.08f));
            dl->AddRectFilled(v.P(p.x - hw * 0.45f, p.y - hh * 0.45f), v.P(p.x + hw * 0.45f, p.y + hh * 0.45f), rgba(30, 36, 48, 200), v.S(0.05f));
            if (std::fmod(t + i * 0.2f, 0.6f) < 0.3f) dl->AddCircleFilled(v.P(p.x, p.y), v.S(0.09f), rgba(255, 190, 40), 0);
        }
        static const float ang[4] = {0, kPi * 0.5f, kPi, -kPi * 0.5f};
        const ImVec2 mp = pos(me);
        const bool hit = deadT > 0;
        plane(dl, v.P(mp.x, mp.y), v.S(0.95f), me.dir >= 0 ? ang[me.dir] : -kPi * 0.5f, hit ? rgba(255, 110, 100) : rgba(250, 252, 255));
        if (over) banner(dl, v, W, H, big, bigPx, small, smallPx, "Taxi Rush over", "New game to taxi again");
        else if (ready) { char t2[32]; std::snprintf(t2, sizeof t2, "Taxi Rush  -  level %d", level); banner(dl, v, W, H, big, bigPx, small, smallPx, t2, "A direction to start taxiing"); }
    }
};

// ================================================================================================================================
// Sky Patrol: rogue drones over the airport in formation, swooping down in turns; your jet fires up at them, and dodges their hail
struct SkyPatrol {
    static constexpr float W = 320, H = 240;
    struct Shot { float x, y; };
    struct Drone { float sx, sy, x, y; int kind, hp; int state; float t, vx; bool alive; };   // state 0 in formation, 1 diving, 2 returning
    float px = 160, fireCd = 0, sway = 0, diveCd = 2, t = 0, hitT = 0, waveT = 0;
    std::vector<Shot> shots, hail;
    std::vector<Drone> drones;
    struct Cloud { float x, y, r, s; };
    std::vector<Cloud> clouds;
    int score = 0, livesLeft = 3, wave = 1;
    bool over = false, ready = true, started = false;
    void newWave() {
        drones.clear(); shots.clear(); hail.clear();
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) {
            Drone d; d.sx = 160 + (c - 3.5f) * 28; d.sy = 34 + r * 22; d.x = d.sx; d.y = d.sy - 120 - r * 20; d.kind = r == 0 ? 0 : r == 1 ? 1 : 2;
            d.hp = r == 0 ? 2 : 1; d.state = 2; d.t = 0; d.vx = 0; d.alive = true; drones.push_back(d);
        }
        diveCd = 2.5f; waveT = 0;
    }
    void reset() {
        px = 160; score = 0; livesLeft = 3; wave = 1; over = false; ready = true; started = true; hitT = 0;
        clouds.clear(); for (int i = 0; i < 9; ++i) clouds.push_back({frand(0, W), frand(0, H), frand(10, 26), frand(14, 30)});
        newWave();
    }
    void step(const Input& in, float dt) {
        t += dt;
        for (auto& c : clouds) { c.y += c.s * dt; if (c.y - c.r > H) { c.y = -c.r; c.x = frand(0, W); } }
        if (over) return;
        if (ready) { if (anyHit(in)) ready = false; else return; }
        // the jet
        const float sp = 150;
        if (in.left) px -= sp * dt;
        if (in.right) px += sp * dt;
        px = std::max(14.0f, std::min(W - 14, px));
        fireCd -= dt;
        if ((in.action || in.up) && fireCd <= 0 && shots.size() < 3) { shots.push_back({px, H - 30}); fireCd = 0.24f; }
        for (auto& s : shots) s.y -= 300 * dt;
        shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot& s) { return s.y < -10; }), shots.end());
        for (auto& h : hail) h.y += (110 + wave * 6) * dt;
        hail.erase(std::remove_if(hail.begin(), hail.end(), [&](const Shot& s) { return s.y > H + 10; }), hail.end());
        if (hitT > 0) hitT -= dt;
        // the formation sways; drones dive in turns, then come back round from the top
        sway = std::sin(t * 0.9f) * 22;
        waveT += dt;
        diveCd -= dt;
        int alive = 0;
        for (auto& d : drones) if (d.alive) ++alive;
        if (diveCd <= 0 && waveT > 3) {
            diveCd = std::max(0.7f, 2.4f - wave * 0.2f);
            std::vector<int> idle;
            for (int i = 0; i < (int)drones.size(); ++i) if (drones[i].alive && drones[i].state == 0) idle.push_back(i);
            if (!idle.empty()) { Drone& d = drones[idle[irand(0, (int)idle.size() - 1)]]; d.state = 1; d.t = 0; d.vx = (d.x < px ? 1 : -1) * 40; }
        }
        for (auto& d : drones) {
            if (!d.alive) continue;
            const float hx = d.sx + sway, hy = d.sy;
            if (d.state == 0) { d.x = hx; d.y = hy; if (frand(0, 1) < dt * 0.04f * wave && hail.size() < 6) hail.push_back({d.x, d.y + 6}); }
            else if (d.state == 1) {   // the swoop: a loop out, then down at the jet
                d.t += dt;
                const float vy = 70 + wave * 8;
                d.vx += ((px - d.x) * 1.6f - d.vx) * dt * 1.2f;
                d.x += (d.vx + std::sin(d.t * 4) * 60) * dt;
                d.y += vy * dt;
                if (frand(0, 1) < dt * 0.8f && hail.size() < 6 && d.y < H - 60) hail.push_back({d.x, d.y + 6});
                if (d.y > H + 12) { d.y = -14; d.state = 2; }
            } else {   // back to its place
                const float dx = hx - d.x, dy = hy - d.y, l = std::hypot(dx, dy);
                if (l < 3) { d.state = 0; d.x = hx; d.y = hy; }
                else { const float v = std::min(l, 150 * dt); d.x += dx / l * v; d.y += dy / l * v; }
            }
            // hit by a shot
            for (auto& s : shots) if (s.y > -5 && std::fabs(s.x - d.x) < 10 && std::fabs(s.y - d.y) < 9) {
                s.y = -100;
                if (--d.hp <= 0) { d.alive = false; score += (d.kind == 0 ? 150 : d.kind == 1 ? 80 : 50) * (d.state == 1 ? 2 : 1); }
                break;
            }
            // into the jet
            if (d.alive && hitT <= 0 && std::fabs(d.x - px) < 13 && std::fabs(d.y - (H - 20)) < 11) { d.alive = false; hitT = 2.0f; --livesLeft; hail.clear(); }
        }
        if (hitT <= 0) for (auto& h : hail) if (std::fabs(h.x - px) < 9 && std::fabs(h.y - (H - 20)) < 9) { hitT = 2.0f; --livesLeft; hail.clear(); break; }
        if (livesLeft <= 0) { over = true; return; }
        if (alive == 0) { ++wave; score += 300; newWave(); }
    }
    static void drone(ImDrawList* dl, const View& v, float x, float y, int kind, float t) {
        static const ImU32 col[3] = {IM_COL32(230, 90, 80, 255), IM_COL32(250, 190, 60, 255), IM_COL32(150, 210, 250, 255)};
        const float s = kind == 0 ? 1.15f : 1.0f;
        for (int i = 0; i < 4; ++i) {
            const float ox = (i % 2 ? 6.5f : -6.5f) * s, oy = (i / 2 ? 5.0f : -5.0f) * s;
            dl->AddLine(v.P(x, y), v.P(x + ox, y + oy), rgba(200, 206, 216), v.S(1.3f));
            const float spin = 0.4f + 0.6f * std::fabs(std::sin(t * 30 + i));
            dl->AddEllipseFilled(v.P(x + ox, y + oy), ImVec2(v.S(4.2f * s * spin), v.S(1.4f * s)), rgba(220, 226, 236, 150));
        }
        dl->AddRectFilled(v.P(x - 4 * s, y - 3 * s), v.P(x + 4 * s, y + 3 * s), col[kind], v.S(1.5f));
        dl->AddCircleFilled(v.P(x, y + 1), v.S(1.3f), rgba(255, 60, 60), 0);
    }
    void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx) {
        const View v = fit(a, b, W, H);
        dl->AddRectFilledMultiColor(v.P(0, 0), v.P(W, H), rgba(18, 36, 78), rgba(18, 36, 78), rgba(56, 104, 170), rgba(56, 104, 170));
        dl->PushClipRect(v.P(0, 0), v.P(W, H), true);
        for (const auto& c : clouds) {
            dl->AddCircleFilled(v.P(c.x, c.y), v.S(c.r * 0.6f), rgba(255, 255, 255, 34), 0);
            dl->AddCircleFilled(v.P(c.x + c.r * 0.5f, c.y + 2), v.S(c.r * 0.45f), rgba(255, 255, 255, 34), 0);
            dl->AddCircleFilled(v.P(c.x - c.r * 0.5f, c.y + 3), v.S(c.r * 0.4f), rgba(255, 255, 255, 34), 0);
        }
        for (const auto& d : drones) if (d.alive) drone(dl, v, d.x, d.y, d.kind, t);
        for (const auto& s : shots) dl->AddLine(v.P(s.x, s.y), v.P(s.x, s.y - 7), rgba(255, 240, 150), v.S(2.0f));
        for (const auto& h : hail) dl->AddCircleFilled(v.P(h.x, h.y), v.S(2.4f), rgba(220, 240, 255), 0);
        if (!(hitT > 0 && std::fmod(t, 0.2f) < 0.1f)) plane(dl, v.P(px, H - 20), v.S(24), -kPi * 0.5f, rgba(250, 252, 255));
        dl->PopClipRect();
        if (over) banner(dl, v, W, H, big, bigPx, small, smallPx, "Patrol over", "New game to scramble again");
        else if (ready) { char t2[32]; std::snprintf(t2, sizeof t2, "Sky Patrol  -  wave %d", wave); banner(dl, v, W, H, big, bigPx, small, smallPx, t2, "Left and right to fly, the action button to fire"); }
    }
};

// ================================================================================================================================
// Hangar Climb: a runaway cargo loader at the top of the hangar sends tyres rolling down its ramps; climb the ladders, jump the
// tyres, and reach the emergency stop at the top
struct HangarClimb {
    static constexpr float W = 320, H = 240;
    struct Ramp { float x0, x1, y0, y1; };   // from x0 (at y0) to x1 (at y1)
    Ramp ramps[7];
    struct Ladder { float x; int lo; };      // from ramp lo up to lo + 1
    std::vector<Ladder> ladders;
    struct Tyre { float x, y, vx, vy; int level; int state; bool scored; float rot; };   // state 0 rolling, 1 falling, 2 down a ladder
    std::vector<Tyre> tyres;
    float x = 30, y = 0, vy = 0, t = 0, spawnCd = 1, deadT = 0, winT = 0, faceX = 1;
    int level = 0, state = 0;   // 0 walking, 1 climbing, 2 in the air
    int score = 0, livesLeft = 3, round = 1;
    bool over = false, ready = true, started = false;
    float gy(int lv, float px) const { const Ramp& r = ramps[lv]; const float k = (px - r.x0) / (r.x1 - r.x0); return r.y0 + (r.y1 - r.y0) * std::max(0.0f, std::min(1.0f, k)); }
    void build() {
        ramps[0] = {0, W, 228, 228};
        for (int k = 1; k <= 5; ++k) {
            const float base = 228 - k * 35;
            if (k % 2) ramps[k] = {14, 290, base - 5, base + 5};   // down to the right: tyres off its right end
            else ramps[k] = {30, 306, base + 5, base - 5};         // down to the left: off its left end
        }
        ramps[6] = {118, 202, 228 - 6 * 35 + 10, 228 - 6 * 35 + 10};   // the top deck, the stop button on it
        ladders = {{250, 0}, {70, 1}, {180, 1}, {236, 2}, {96, 3}, {206, 3}, {266, 4}, {160, 5}};
    }
    void placeMe() { x = 30; level = 0; state = 0; y = gy(0, x); vy = 0; tyres.clear(); spawnCd = 1.0f; }
    void reset() { build(); score = 0; livesLeft = 3; round = 1; over = false; ready = true; started = true; deadT = 0; winT = 0; placeMe(); }
    const Ladder* ladderAt(int lo, float px, float tol) const { for (const auto& l : ladders) if (l.lo == lo && std::fabs(l.x - px) < tol) return &l; return nullptr; }
    void step(const Input& in, float dt) {
        t += dt;
        if (over) return;
        if (ready) { if (anyHit(in)) ready = false; else return; }
        if (deadT > 0) { deadT -= dt; if (deadT <= 0) { if (livesLeft <= 0) over = true; else placeMe(); } return; }
        if (winT > 0) { winT -= dt; if (winT <= 0) { ++round; placeMe(); ready = true; } return; }
        // the climber
        const float walk = 62;
        if (state == 0) {
            if (in.left) { x -= walk * dt; faceX = -1; }
            if (in.right) { x += walk * dt; faceX = 1; }
            x = std::max(ramps[level].x0 + 4, std::min(ramps[level].x1 - 4, x));
            y = gy(level, x);
            if (in.up) if (const Ladder* l = ladderAt(level, x, 6)) { x = l->x; state = 1; }
            if (in.down && level > 0) if (const Ladder* l = ladderAt(level - 1, x, 6)) { x = l->x; state = 1; --level; y = gy(level + 1, x); }
            if (in.actionHit) { state = 2; vy = -118; }
        } else if (state == 1) {   // on a ladder: from ramp `level` up to `level + 1`
            const float top = gy(level + 1, x), bottom = gy(level, x);
            if (in.up) y -= 40 * dt;
            if (in.down) y += 40 * dt;
            if (y <= top) { y = top; ++level; state = 0; }
            else if (y >= bottom) { y = bottom; state = 0; }
        } else {   // jumping: back on the same ramp
            vy += 420 * dt; y += vy * dt;
            if (in.left) { x -= walk * 0.8f * dt; faceX = -1; }
            if (in.right) { x += walk * 0.8f * dt; faceX = 1; }
            x = std::max(ramps[level].x0 + 4, std::min(ramps[level].x1 - 4, x));
            if (vy > 0 && y >= gy(level, x)) { y = gy(level, x); state = 0; }
        }
        if (level == 6) { score += 1500; winT = 2.0f; return; }
        // the tyres: from the loader at the top left, down every ramp, sometimes down a ladder
        spawnCd -= dt;
        if (spawnCd <= 0) { spawnCd = std::max(0.9f, 2.6f - round * 0.25f) * frand(0.8f, 1.3f); tyres.push_back({48, gy(5, 48) - 6, 0, 0, 5, 0, false, 0}); }
        const float roll = 64 + round * 8;
        for (auto& ty : tyres) {
            if (ty.state == 0) {
                const Ramp& r = ramps[ty.level];
                const float dirx = ty.level == 0 ? -1.0f : (r.y1 > r.y0 ? 1.0f : -1.0f);
                const float oldX = ty.x;
                ty.x += dirx * roll * dt; ty.rot += dirx * roll * dt / 6;
                ty.y = gy(ty.level, ty.x) - 6;
                if (ty.level > 0 && (ty.x > r.x1 || ty.x < r.x0)) { ty.state = 1; ty.vy = 0; ty.vx = dirx * roll * 0.6f; }
                else if (ty.level > 0) for (const auto& l : ladders) if (l.lo == ty.level - 1 && ((oldX < l.x && ty.x >= l.x) || (oldX > l.x && ty.x <= l.x)) && irand(0, 99) < 22) { ty.x = l.x; ty.state = 2; break; }
            } else if (ty.state == 1) {
                ty.vy += 400 * dt; ty.x += ty.vx * dt; ty.y += ty.vy * dt;
                if (ty.y >= gy(ty.level - 1, ty.x) - 6) { --ty.level; ty.state = 0; ty.y = gy(ty.level, ty.x) - 6; }
            } else {
                ty.y += 60 * dt;
                if (ty.y >= gy(ty.level - 1, ty.x) - 6) { --ty.level; ty.state = 0; }
            }
            // the climber: a jump over it scores, touching it costs a life
            const float dx = std::fabs(ty.x - x), feet = y, head = y - 16;
            if (dx < 8 && feet > ty.y - 6 && head < ty.y + 6) { --livesLeft; deadT = 1.6f; return; }
            if (!ty.scored && state == 2 && dx < 8 && feet < ty.y - 4) { ty.scored = true; score += 100; }
        }
        tyres.erase(std::remove_if(tyres.begin(), tyres.end(), [](const Tyre& ty) { return ty.x < -12 || ty.x > W + 12; }), tyres.end());
    }
    void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx) {
        const View v = fit(a, b, W, H);
        dl->AddRectFilledMultiColor(v.P(0, 0), v.P(W, H), rgba(28, 32, 44), rgba(28, 32, 44), rgba(48, 54, 70), rgba(48, 54, 70));
        dl->PushClipRect(v.P(0, 0), v.P(W, H), true);
        // the hangar's roof trusses
        for (float rx = 0; rx < W; rx += 40) { dl->AddLine(v.P(rx, 0), v.P(rx + 20, 12), rgba(70, 78, 96), v.S(1.2f)); dl->AddLine(v.P(rx + 20, 12), v.P(rx + 40, 0), rgba(70, 78, 96), v.S(1.2f)); }
        // the ladders
        for (const auto& l : ladders) {
            const float y0 = gy(l.lo, l.x), y1 = gy(l.lo + 1, l.x);
            dl->AddLine(v.P(l.x - 4, y0), v.P(l.x - 4, y1), rgba(200, 206, 216), v.S(1.2f));
            dl->AddLine(v.P(l.x + 4, y0), v.P(l.x + 4, y1), rgba(200, 206, 216), v.S(1.2f));
            for (float ry = y1 + 3; ry < y0; ry += 5) dl->AddLine(v.P(l.x - 4, ry), v.P(l.x + 4, ry), rgba(200, 206, 216), v.S(1.0f));
        }
        // the ramps: an orange deck with its bracing
        for (int k = 0; k < 7; ++k) {
            const Ramp& r = ramps[k];
            const ImVec2 p0 = v.P(r.x0, r.y0), p1 = v.P(r.x1, r.y1), q0 = v.P(r.x0, r.y0 + 5), q1 = v.P(r.x1, r.y1 + 5);
            const ImVec2 deck[4] = {p0, p1, q1, q0};
            dl->AddConvexPolyFilled(deck, 4, k == 6 ? rgba(240, 200, 60) : rgba(226, 110, 50));
            for (float bx = r.x0; bx < r.x1 - 4; bx += 8) {
                const float ya = gy(k, bx), yb = gy(k, bx + 8);
                dl->AddLine(v.P(bx, ya + 5), v.P(bx + 4, (ya + yb) * 0.5f), rgba(150, 64, 30), v.S(0.9f));
                dl->AddLine(v.P(bx + 4, (ya + yb) * 0.5f), v.P(bx + 8, yb + 5), rgba(150, 64, 30), v.S(0.9f));
            }
        }
        // the emergency stop on the top deck, and the runaway loader beside the top ramp
        const float sx = 160, sy = ramps[6].y0;
        dl->AddRectFilled(v.P(sx - 7, sy - 12), v.P(sx + 7, sy), rgba(240, 200, 40), v.S(1.5f));
        dl->AddCircleFilled(v.P(sx, sy - 13), v.S(4.5f + 0.6f * std::sin(t * 6)), rgba(220, 40, 40), 0);
        const float ly = gy(5, 30);
        dl->AddRectFilled(v.P(14, ly - 26), v.P(44, ly - 8), rgba(244, 196, 40), v.S(2));
        dl->AddRectFilled(v.P(18, ly - 34), v.P(32, ly - 26), rgba(244, 196, 40), v.S(1.5f));
        dl->AddRectFilled(v.P(20, ly - 32), v.P(30, ly - 27), rgba(40, 60, 90), v.S(1));
        dl->AddCircleFilled(v.P(20, ly - 6), v.S(3.5f), rgba(30, 30, 34), 0);
        dl->AddCircleFilled(v.P(38, ly - 6), v.S(3.5f), rgba(30, 30, 34), 0);
        if (std::fmod(t, 0.5f) < 0.25f) dl->AddCircleFilled(v.P(25, ly - 36), v.S(2), rgba(255, 150, 30), 0);
        // the tyres
        for (const auto& ty : tyres) {
            const ImVec2 c = v.P(ty.x, ty.y);
            dl->AddCircleFilled(c, v.S(6), rgba(24, 24, 28), 0);
            dl->AddCircleFilled(c, v.S(2.6f), rgba(160, 166, 176), 0);
            for (int s = 0; s < 3; ++s) { const float an = ty.rot + s * 2.094f; dl->AddLine(c, ImVec2(c.x + std::cos(an) * v.S(4.6f), c.y + std::sin(an) * v.S(4.6f)), rgba(90, 94, 104), v.S(1)); }
        }
        // the climber: helmet, hi-vis vest, legs that walk
        if (!(deadT > 0 && std::fmod(t, 0.2f) < 0.1f)) {
            const float step2 = state == 0 ? std::sin(x * 0.5f) * 3 : 0;
            dl->AddLine(v.P(x, y - 7), v.P(x - 3 + step2, y), rgba(40, 60, 110), v.S(2));
            dl->AddLine(v.P(x, y - 7), v.P(x + 3 - step2, y), rgba(40, 60, 110), v.S(2));
            dl->AddRectFilled(v.P(x - 3.5f, y - 15), v.P(x + 3.5f, y - 7), rgba(250, 140, 30), v.S(1.2f));
            dl->AddLine(v.P(x - 3.5f, y - 11), v.P(x + 3.5f, y - 11), rgba(230, 240, 120), v.S(1));
            dl->AddCircleFilled(v.P(x, y - 18), v.S(2.8f), rgba(240, 200, 170), 0);
            dl->AddRectFilled(v.P(x - 3.2f, y - 21.5f), v.P(x + 3.2f, y - 18.5f), rgba(250, 250, 250), v.S(1.4f));
            dl->AddLine(v.P(x + 3.5f * faceX, y - 13), v.P(x + 6 * faceX, y - 10), rgba(250, 140, 30), v.S(1.6f));
        }
        dl->PopClipRect();
        if (over) banner(dl, v, W, H, big, bigPx, small, smallPx, "Hangar closed", "New game to climb again");
        else if (winT > 0) banner(dl, v, W, H, big, bigPx, small, smallPx, "Loader stopped!", "On to the next hangar");
        else if (ready) { char t2[40]; std::snprintf(t2, sizeof t2, "Hangar Climb  -  hangar %d", round); banner(dl, v, W, H, big, bigPx, small, smallPx, t2, "Reach the red stop at the top; the action button jumps"); }
    }
};

Contrail g_contrail;
TaxiRush g_taxi;
SkyPatrol g_sky;
HangarClimb g_hangar;

}  // namespace

const char* name(int g) {
    static const char* n[COUNT] = {"Contrail", "Taxi Rush", "Sky Patrol", "Hangar Climb"};
    return g >= 0 && g < COUNT ? n[g] : "";
}
const char* blurb(int g) {
    static const char* n[COUNT] = {"Take on fuel; never cross your own contrail.", "Taxi for every light; the ground vehicles patrol.",
                                   "Rogue drones swoop in formation: clear the sky.", "Jump the rolling tyres; hit the loader's stop."};
    return g >= 0 && g < COUNT ? n[g] : "";
}
const char* help(int g) {
    static const char* n[COUNT] = {"Steer with the arrows (W A S D). Each fuel drop lengthens the contrail and speeds you up; the airspace's edge and your own trail end the flight.",
                                   "Taxi with the arrows (W A S D) over every green light. A big green light is a takeoff clearance: for a few seconds the vehicles give way, and you can send them back to the apron.",
                                   "Left and right fly, the action button (Space) fires. Drones that swoop at you score double; dodge their hail.",
                                   "Left and right walk, up and down climb ladders, the action button (Space) jumps. Jump a tyre for points; reach the red stop on the top deck."};
    return g >= 0 && g < COUNT ? n[g] : "";
}
float aspect(int g) { return g == CONTRAIL ? 28.0f / 20.0f : g == TAXI_RUSH ? 21.0f / 15.0f : 320.0f / 240.0f; }
void reset(int g) {
    if (g == CONTRAIL) g_contrail.reset(); else if (g == TAXI_RUSH) g_taxi.reset(); else if (g == SKY_PATROL) g_sky.reset(); else if (g == HANGAR_CLIMB) g_hangar.reset();
}
bool started(int g) { return g == CONTRAIL ? g_contrail.started : g == TAXI_RUSH ? g_taxi.started : g == SKY_PATROL ? g_sky.started : g == HANGAR_CLIMB ? g_hangar.started : false; }
void step(int g, const Input& in, float dt) {
    if (!started(g)) reset(g);
    dt = std::max(0.0f, std::min(dt, 0.1f));
    Input first = in, rest = in;   // (a long frame in small steps; the presses only in the first)
    rest.leftHit = rest.rightHit = rest.upHit = rest.downHit = rest.actionHit = false;
    const int n = std::max(1, (int)std::ceil(dt / 0.02f));
    for (int i = 0; i < n; ++i) {
        const Input& u = i == 0 ? first : rest;
        const float d = dt / n;
        if (g == CONTRAIL) g_contrail.step(u, d); else if (g == TAXI_RUSH) g_taxi.step(u, d); else if (g == SKY_PATROL) g_sky.step(u, d); else if (g == HANGAR_CLIMB) g_hangar.step(u, d);
    }
}
void draw(int g, ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx) {
    if (!started(g)) reset(g);
    if (g == CONTRAIL) g_contrail.draw(dl, a, b, big, bigPx, small, smallPx);
    else if (g == TAXI_RUSH) g_taxi.draw(dl, a, b, big, bigPx, small, smallPx);
    else if (g == SKY_PATROL) g_sky.draw(dl, a, b, big, bigPx, small, smallPx);
    else if (g == HANGAR_CLIMB) g_hangar.draw(dl, a, b, big, bigPx, small, smallPx);
}
int score(int g) { return g == CONTRAIL ? g_contrail.score : g == TAXI_RUSH ? g_taxi.score : g == SKY_PATROL ? g_sky.score : g == HANGAR_CLIMB ? g_hangar.score : 0; }
int lives(int g) { return g == TAXI_RUSH ? g_taxi.livesLeft : g == SKY_PATROL ? g_sky.livesLeft : g == HANGAR_CLIMB ? g_hangar.livesLeft : -1; }
bool over(int g) { return g == CONTRAIL ? g_contrail.over : g == TAXI_RUSH ? g_taxi.over : g == SKY_PATROL ? g_sky.over : g == HANGAR_CLIMB ? g_hangar.over : false; }

void icon(int g, ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 w = IM_COL32(255, 255, 255, 255);
    if (g == CONTRAIL) {
        for (int i = 0; i < 6; ++i) dl->AddCircleFilled(ImVec2(c.x - r * 0.55f + i * r * 0.14f, c.y + r * 0.3f - (i > 3 ? (i - 3) * r * 0.16f : 0)), r * (0.1f + 0.012f * (5 - i)), IM_COL32(255, 255, 255, 120 + i * 20), 0);
        plane(dl, ImVec2(c.x + r * 0.42f, c.y - r * 0.12f), r * 0.6f, -0.6f, w);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y + r * 0.45f), r * 0.1f, IM_COL32(255, 200, 50, 255), 0);
    } else if (g == TAXI_RUSH) {
        dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.5f), ImVec2(c.x + r * 0.6f, c.y + r * 0.5f), IM_COL32(255, 255, 255, 140), r * 0.12f, 0, r * 0.08f);
        for (int i = 0; i < 4; ++i) dl->AddCircleFilled(ImVec2(c.x - r * 0.1f + i * r * 0.18f, c.y), r * 0.05f, IM_COL32(120, 255, 160, 255), 0);
        plane(dl, ImVec2(c.x - r * 0.34f, c.y), r * 0.42f, 0, w);
        dl->AddRectFilled(ImVec2(c.x + r * 0.38f, c.y - r * 0.38f), ImVec2(c.x + r * 0.58f, c.y - r * 0.22f), IM_COL32(250, 170, 40, 255), r * 0.03f);
    } else if (g == SKY_PATROL) {
        for (int i = 0; i < 3; ++i) {
            const ImVec2 d(c.x - r * 0.4f + i * r * 0.4f, c.y - r * 0.4f);
            dl->AddRectFilled(ImVec2(d.x - r * 0.08f, d.y - r * 0.06f), ImVec2(d.x + r * 0.08f, d.y + r * 0.06f), IM_COL32(250, 190, 60, 255), r * 0.02f);
            dl->AddLine(ImVec2(d.x - r * 0.15f, d.y - r * 0.1f), ImVec2(d.x + r * 0.15f, d.y + r * 0.1f), IM_COL32(255, 255, 255, 180), r * 0.03f);
            dl->AddLine(ImVec2(d.x + r * 0.15f, d.y - r * 0.1f), ImVec2(d.x - r * 0.15f, d.y + r * 0.1f), IM_COL32(255, 255, 255, 180), r * 0.03f);
        }
        dl->AddLine(ImVec2(c.x, c.y + r * 0.05f), ImVec2(c.x, c.y - r * 0.15f), IM_COL32(255, 240, 150, 255), r * 0.05f);
        plane(dl, ImVec2(c.x, c.y + r * 0.42f), r * 0.55f, -kPi * 0.5f, w);
    } else {
        for (int k = 0; k < 3; ++k) {
            const float yy = c.y + r * 0.5f - k * r * 0.4f;
            dl->AddLine(ImVec2(c.x - r * 0.6f, yy - (k % 2 ? r * 0.06f : -r * 0.06f)), ImVec2(c.x + r * 0.6f, yy + (k % 2 ? r * 0.06f : -r * 0.06f)), IM_COL32(255, 150, 80, 255), r * 0.08f);
        }
        dl->AddLine(ImVec2(c.x + r * 0.3f, c.y + r * 0.5f), ImVec2(c.x + r * 0.3f, c.y + r * 0.1f), IM_COL32(255, 255, 255, 200), r * 0.04f);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y + r * 0.02f), r * 0.11f, IM_COL32(20, 20, 24, 255), 0);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.62f), r * 0.09f, IM_COL32(230, 50, 50, 255), 0);
    }
}

}  // namespace arcade
}  // namespace xpt
