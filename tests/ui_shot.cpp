// Off-line pictures of the tablet: draws the real UI (src/tabletui.cpp) with Dear ImGui, clips it with the plugin's own clipper
// (src/tabrender.cpp) and rasterizes the triangles in software into PNG files, so the layout can be checked without X-Plane or a GPU.
// Also a smoke test: every app is opened and drawn at several screen shapes, and no triangle may leave the screen.
//   ui_shot <out-dir> [scale]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef XPT_UI_PNG
#include <zlib.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"   // (the error log: ImGui's own record of unbalanced calls, which it shows on screen in the sim)
#include "tabletui.h"
#include "airlines.h"
#include "tabrender.h"

using namespace xpt;

namespace {

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
                    if ((intptr_t)bt.tex == 3 || (intptr_t)bt.tex == 4) {   // XPTablet's logo or the icons' atlas, its first level, bilinear
                        int lw = 0, lh = 0, lv = 0;
                        const unsigned char* px = (intptr_t)bt.tex == 3 ? logoPixels(lw, lh, lv) : iconsPixels(lw, lh, lv);
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

#ifdef XPT_UI_PNG
void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back((char)((v >> (8 * i)) & 255)); }
void chunk(std::string& png, const char* type, const std::string& data) {
    be32(png, (uint32_t)data.size());
    const std::string td = std::string(type, 4) + data;
    png += td;
    be32(png, (uint32_t)crc32(0, (const Bytef*)td.data(), (uInt)td.size()));
}
bool writePng(const Canvas& cv, const std::string& path) {
    std::string raw;
    raw.reserve((size_t)cv.h * (cv.w * 3 + 1));
    for (int y = 0; y < cv.h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < cv.w; ++x)
            for (int k = 0; k < 3; ++k) raw.push_back((char)(unsigned char)std::lround(std::min(1.0f, std::max(0.0f, cv.rgb[((size_t)y * cv.w + x) * 3 + k])) * 255));
    }
    uLongf zn = compressBound(raw.size());
    std::string z(zn, '\0');
    if (compress2((Bytef*)&z[0], &zn, (const Bytef*)raw.data(), raw.size(), 6) != Z_OK) return false;
    z.resize(zn);
    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    be32(ihdr, (uint32_t)cv.w); be32(ihdr, (uint32_t)cv.h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}
#else
bool writePng(const Canvas&, const std::string&) { return true; }   // no zlib here: the frames are still drawn and rasterized
#endif

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const float scale = argc > 2 ? (float)std::atof(argv[2]) : 2.0f;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyStyle();
    Fonts fonts = loadFonts(scale);
    fonts.wxLogo = (ImTextureID)(intptr_t)2;
    fonts.logo = (ImTextureID)(intptr_t)3;
    fonts.icons = (ImTextureID)(intptr_t)4;
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex, &tw, &th);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);

    {   // the aircraft top views, as the plugin reads them from its shapes folder
        std::string text;
        FILE* sf = std::fopen(XPT_SOURCE_DIR "/assets/shapes/aircraft_shapes.txt", "rb");
        if (sf) { char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, sf)) > 0) text.append(buf, n); std::fclose(sf); }
        const int n = loadShapes(text);
        if (n < 150) { std::printf("FAIL: only %d aircraft shapes read\n", n); return 1; }
        for (const char* t : {"A319", "A20N", "A21N", "A339", "A346", "B736", "B738", "B39M", "MD82", "CRJ9", "CL60", "GL7T", "DH8D", "FA8X", "FA7X", "F2TH", "RFAL", "C172"})
            if (!hasShape(t)) { std::printf("FAIL: no shape for %s\n", t); return 1; }
    }
    SimState s;
    s.acfIcao = "A321"; s.zuluSec = 23 * 3600 + 7 * 60; s.onPanel = true;
    s.panel[0] = 1024; s.panel[1] = 0; s.panel[2] = 800; s.panel[3] = 480;
    s.panelEnabled = s.panelPowered = true; s.wxPlane = true; s.pixelScale = scale;
    s.batteryPct = 86; s.charging = true;

    int failures = 0;
    {   // typing, a letter at a time: the lines already written never change (only the last line's last word may move down a line)
        const std::string text = "CLEARED TO KORD VIA THE O'HARE FOUR DEPARTURE THEN AS FILED, MAINTAIN 5000 EXPECT FL350 10 MIN AFTER DEP, 124.35, SQUAWK 4521.  RWY 27L / 9R  ...";
        for (float wrapW : {180.0f, 333.0f, 520.0f}) {
            std::vector<std::string> prev;
            for (size_t k = 1; k <= text.size(); ++k) {
                const std::string v = text.substr(0, k);
                std::vector<std::string> cur;
                for (const auto& se : wrapLines(fonts.body, 15.0f * scale, v, wrapW * scale)) cur.push_back(v.substr(se.first, se.second - se.first));
                bool bad = cur.size() < prev.size();
                for (size_t i = 0; !bad && i + 1 < prev.size(); ++i) if (cur[i] != prev[i]) bad = true;
                if (bad) { std::printf("FAIL: typing rewrapped an earlier line at %zu chars (width %.0f): '%s'\n", k, wrapW, prev.empty() ? "" : prev[0].c_str()); ++failures; break; }
                prev = cur;
            }
        }
    }
    const float widths[] = {800, 640, 1024, 360};   // 5:3 (AviTab's shape), 4:3, wide, portrait
    for (float W : widths) {
        const float H = kLogicalH;
        for (int app = APP_HOME; app < APP_COUNT; ++app) {
            openApp(app);
            std::vector<Batch> batches;
            for (int frame = 0; frame < 3; ++frame) {
                io.DisplaySize = ImVec2(W, H);
                io.DeltaTime = 1.0f / 60.0f;
                if (app == APP_SETTINGS) {   // press inside the touch test on the second frame
                    io.AddMousePosEvent(W * 0.75f, H * 0.6f);
                    io.AddMouseButtonEvent(0, frame >= 1);
                } else {
                    io.AddMouseButtonEvent(0, false);
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                }
                ImGui::NewFrame();
                Actions act;
                build(fonts, s, W, H, act);
                ImGui::Render();
                clipDrawData(ImGui::GetDrawData(), batches);
            }
            size_t tris = 0;
            for (const Batch& b : batches) {
                tris += b.v.size() / 3;
                for (const ImDrawVert& v : b.v)
                    if (v.pos.x < -0.01f || v.pos.y < -0.01f || v.pos.x > W + 0.01f || v.pos.y > H + 0.01f) { std::printf("FAIL: drawn off the screen: %s at %gx%g (%.1f, %.1f)\n", appName(app), W, H, v.pos.x, v.pos.y); ++failures; break; }
            }
            if (tris == 0) { std::printf("FAIL: nothing drawn for %s at %gx%g\n", appName(app), W, H); ++failures; }
            if (app != APP_HOME && appVisible(app) && currentApp() == APP_HOME) { std::printf("FAIL: %s did not open\n", appName(app)); ++failures; }
            if (W == 800 && (app == APP_HOME || app == APP_SETTINGS || app == APP_MAP || app == APP_SIMBRIEF)) {
                Canvas cv((int)(W * scale), (int)(H * scale));
                raster(cv, batches, tex, tw, th, scale);
                const std::string path = out + "/tablet_" + (app == APP_HOME ? "home" : app == APP_SETTINGS ? "settings" : app == APP_SIMBRIEF ? "browser" : "placeholder") + ".png";
                if (!writePng(cv, path)) { std::printf("FAIL: cannot write %s\n", path.c_str()); ++failures; }
            }
        }
    }
    // brightness scaling keeps alpha and darkens colour
    {
        std::vector<Batch> b(1);
        ImDrawVert v; v.pos = ImVec2(0, 0); v.uv = ImVec2(0, 0); v.col = IM_COL32(200, 100, 50, 128);
        b[0].v.assign(3, v);
        scaleColors(b, 0.5f);
        const ImU32 c = b[0].v[0].col;
        if (((c >> IM_COL32_A_SHIFT) & 255) != 128 || ((c >> IM_COL32_R_SHIFT) & 255) != 100) { std::printf("FAIL: scaleColors\n"); ++failures; }
    }
    // settings: the text form round-trips, the two switchable icons follow their settings, a switched-off web app leaves the home screen
    {
        settings().showNavigraph = true; settings().showCharts = false; settings().showVatsim = true; settings().showForeflight = false;
        settings().favorites = {APP_WEATHER, APP_MAP}; settings().order = {APP_SETTINGS, -1, APP_NOTES};   // a free place between two icons
        const std::string txt = settingsToText();
        settings() = Settings();
        settingsFromText(txt);
        const Settings& st = settings();
        if (!st.showNavigraph || st.showCharts || !st.showVatsim || !st.showLive || st.showForeflight || !st.showSimbrief) { std::printf("FAIL: settings round trip\n"); ++failures; }
        int placed = 0;
        for (int a : st.order) placed += a >= 0 ? 1 : 0;
        if (st.favorites != std::vector<int>({APP_WEATHER, APP_MAP}) || st.order[0] != APP_SETTINGS || st.order[1] != -1 || st.order[2] != APP_NOTES || placed != APP_COUNT - 2) { std::printf("FAIL: layout round trip\n"); ++failures; }
        openApp(APP_NAVIGRAPH);
        if (currentApp() != APP_BROWSER || browserChrome() || std::string(browserUrl()).find("navigraph") == std::string::npos) { std::printf("FAIL: the Navigraph icon\n"); ++failures; }
        openApp(APP_HOME); openApp(APP_VATSIM);
        if (std::string(browserUrl()).find("vatsim") == std::string::npos || std::string(appName(APP_VATSIM)) != "VATSIM Radar") { std::printf("FAIL: the VATSIM icon\n"); ++failures; }
        openApp(APP_HOME);
        openApp(APP_BROWSER);
        if (!browserChrome()) { std::printf("FAIL: the Browser icon opens the full browser\n"); ++failures; }
        if (appVisible(APP_FOREFLIGHT) || !appVisible(APP_SIMBRIEF)) { std::printf("FAIL: web app switch\n"); ++failures; }
        // an older settings file (one charts icon set to Navigraph) shows the Navigraph icon
        settings() = Settings(); settingsFromText("charts_provider=1\nshow_charts=1\nlive_provider=0\n");
        if (!settings().showNavigraph || settings().showCharts || !settings().showLive) { std::printf("FAIL: older settings\n"); ++failures; }
        settings() = Settings();
    }
    // Performance is gone (everywhere); Checklists are not offered in a ToLiss (it has its own)
    {
        SimState t = s; t.toliss = true;
        openApp(APP_PERFORMANCE);
        io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render();
        if (appVisible(APP_PERFORMANCE) || currentApp() != APP_HOME) { std::printf("FAIL: Performance must go in a ToLiss\n"); ++failures; }
        { std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_toliss.png"); }
        openApp(APP_PERFORMANCE);
        if (currentApp() != APP_HOME) { std::printf("FAIL: a hidden app must not open\n"); ++failures; }
        ImGui::NewFrame(); build(fonts, s, 800, kLogicalH, act); ImGui::Render();
        if (appVisible(APP_PERFORMANCE) || appVisible(APP_TRAFFIC) || !appVisible(APP_ROUTES)) { std::printf("FAIL: Performance and Traffic are gone, Flight Plan is there\n"); ++failures; }
        if (appVisible(APP_CHECKLISTS) == false) { std::printf("FAIL: Checklists show outside a ToLiss\n"); ++failures; }
    }
    // dragging: press the first grid icon, carry it into the favorites bar, let go: it must now be a favorite
    {
        openApp(APP_HOME);
        const float W = 800, H = kLogicalH;
        const int first = APP_MAP;   // default order: Map is the first icon, top-left
        std::vector<Batch> batches;
        auto frame = [&](float x, float y, bool down) {
            io.DisplaySize = ImVec2(W, H); io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame(); Actions act; build(fonts, s, W, H, act); ImGui::Render();
            clipDrawData(ImGui::GetDrawData(), batches);
        };
        settings() = Settings(); settings().favorites = {APP_WEATHER, APP_CHARTS};
        frame(-1000, -1000, false); frame(-1000, -1000, false);
        const float cols = std::floor((W - 24) / 104), gx = std::floor((W - cols * 104) * 0.5f) + 52;
        float gy = 0;   // find the first row: probe downwards until a press picks the icon up
        frame(gx, 60, false); frame(gx, 60, false);
        // hold the icon (it wiggles after a moment), then carry it into the favorites bar
        for (gy = 40; gy < 300; gy += 10) { frame(gx, gy, false); frame(gx, gy, true); for (int k = 0; k < 55; ++k) frame(gx, gy, true);
            frame(gx + 40, gy + 40, true); frame(gx + 40, gy + 40, true);
            frame(W * 0.5f, H - 40, true); frame(W * 0.5f, H - 40, true);
            { Canvas cv((int)(W * scale), (int)(H * scale)); raster(cv, batches, tex, tw, th, scale); writePng(cv, out + "/tablet_drag.png"); }
            frame(W * 0.5f, H - 40, false); frame(W * 0.5f, H - 40, false);
            bool fav = false;
            for (int a : settings().favorites) fav = fav || a == first;
            if (fav) break;
            if (currentApp() != APP_HOME) openApp(APP_HOME);
        }
        bool fav = false;
        for (int a : settings().favorites) fav = fav || a == first;
        if (!fav || settings().favorites.size() != 3) { std::printf("FAIL: drag to favorites\n"); ++failures; }
        if (!takeSettingsChanged()) { std::printf("FAIL: a rearranged home screen must be saved\n"); ++failures; }
        // an icon dropped on a free place stays there (the places between are left free)
        {
            const int last = (int)settings().order.size() - 1;
            int app = -1, from = -1;
            for (int i = 0; i < (int)settings().order.size(); ++i) if (settings().order[i] >= 0 && appVisible(settings().order[i])) { app = settings().order[i]; from = i; break; }
            const float cx = std::floor((W - cols * 104) * 0.5f) + (last % (int)cols) * 104 + 52;
            const float rowsN = std::floor(((H - 90 - 10 - 6) - 28 - 10) / 100);
            const float gy0 = 28 + std::max(6.0f, std::floor(((H - 90 - 10 - 6) - 28 - rowsN * 100) * 0.5f));
            const float cy = gy0 + (last / (int)cols) * 100 + 36;
            const float fx = std::floor((W - cols * 104) * 0.5f) + (from % (int)cols) * 104 + 52, fy = gy0 + (from / (int)cols) * 100 + 36;
            frame(fx, fy, false); frame(fx, fy, true); frame(fx + 20, fy + 10, true); frame(cx, cy, true); frame(cx, cy, true); frame(cx, cy, false); frame(cx, cy, false);
            if (settings().order[last] != app || settings().order[from] != -1) { std::printf("FAIL: an icon put on a free place (%d at %d, %d at %d)\n", settings().order[last], last, settings().order[from], from); ++failures; }
            { Canvas cv((int)(W * scale), (int)(H * scale)); batches.clear(); frame(-1000, -1000, false); raster(cv, batches, tex, tw, th, scale); writePng(cv, out + "/tablet_arrange.png"); }
            // an icon dropped on another icon: the two change places, nothing else moves
            {
                std::vector<int> at;
                for (int i = 0; i < (int)settings().order.size() && at.size() < 3; ++i) if (settings().order[i] >= 0 && appVisible(settings().order[i])) at.push_back(i);
                if (at.size() >= 3) {
                    const int i = at[0], j = at[2], ai = settings().order[i], aj = settings().order[j], mid = settings().order[at[1]];
                    auto px = [&](int k) { return std::floor((W - cols * 104) * 0.5f) + (k % (int)cols) * 104 + 52; };
                    auto py = [&](int k) { return gy0 + (k / (int)cols) * 100 + 36; };
                    frame(px(i), py(i), false); frame(px(i), py(i), true); frame(px(i) + 20, py(i) + 10, true); frame(px(j), py(j), true); frame(px(j), py(j), true); frame(px(j), py(j), false); frame(px(j), py(j), false);
                    if (settings().order[i] != aj || settings().order[j] != ai || settings().order[at[1]] != mid) { std::printf("FAIL: icons dropped on each other must swap\n"); ++failures; }
                }
            }
            frame(W - 56, H - 28, false); frame(W - 56, H - 28, true); frame(W - 56, H - 28, false); frame(-1000, -1000, false);   // Done
        }
        settings() = Settings();
    }
    // the favorites bar's size: 4 by default; made smaller, the extra favorites go back to the grid and nothing is lost
    {
        settings() = Settings();
        if (settings().favoriteSlots != 4 || settings().favorites.size() != 4) { std::printf("FAIL: favorites default\n"); ++failures; }
        settings().favoriteSlots = 2;
        const std::string txt = settingsToText();
        settings() = Settings();
        settingsFromText(txt);
        if (settings().favoriteSlots != 2 || settings().favorites.size() != 2 || std::find(settings().order.begin(), settings().order.end(), (int)APP_WEATHER) == settings().order.end()) { std::printf("FAIL: favorites bar size\n"); ++failures; }
        settingsFromText("favorite_slots=9\n");
        if (settings().favoriteSlots != kMaxFavorites) { std::printf("FAIL: favorites bar size limit\n"); ++failures; }
        // 0 takes the bar away: every favorite returns to the grid, and the home screen still draws
        settings() = Settings(); settings().favoriteSlots = 0;
        settingsFromText(settingsToText());
        if (!settings().favorites.empty() || std::count_if(settings().order.begin(), settings().order.end(), [](int a) { return a >= 0; }) != APP_COUNT) { std::printf("FAIL: favorites bar off\n"); ++failures; }
        openApp(APP_HOME);
        std::vector<Batch> b;
        for (int i = 0; i < 2; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
            ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); clipDrawData(ImGui::GetDrawData(), b); }
        { Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_nobar.png"); }
        // Charts switched off: its icon goes, the setting is kept, and Settings still draws (with the source choice greyed out)
        settings() = Settings(); settings().showCharts = false;
        settingsFromText(settingsToText());
        if (settings().showCharts || appVisible(APP_CHARTS)) { std::printf("FAIL: charts switch\n"); ++failures; }
        openApp(APP_SETTINGS);
        for (int i = 0; i < 2; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); clipDrawData(ImGui::GetDrawData(), b); }
        { Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_settings_off.png"); }
        settings() = Settings();
    }
    // Weather: with WXPlane the page hands its area (below the title bar) to the host; without it, or with one too old, it does not
    {
        auto run = [&](SimState t) { openApp(APP_WEATHER); for (int i = 0; i < 2; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(400, 300); io.AddMouseButtonEvent(0, false);
            ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); } };
        SimState t = s; t.wxPlane = true; t.wxEmbed = 1;
        run(t);
        const EmbedArea e = embedArea();
        { std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_weather.png"); }
        if (!e.active || e.x != 0 || e.y < 60 || e.w != 800 || std::fabs(e.y + e.h - kLogicalH) > 0.01f) { std::printf("FAIL: weather embed area\n"); ++failures; }
        if (std::fabs(e.mx - 400) > 0.01f || std::fabs(e.my - (300 - e.y)) > 0.01f || e.down) { std::printf("FAIL: weather embed pointer\n"); ++failures; }
        t.wxEmbed = -1; run(t);
        if (!embedArea().active) { std::printf("FAIL: WXPlane is asked every frame, answered or not\n"); ++failures; }
        if (embedArea().mx > -1e8f || embedArea().down) { std::printf("FAIL: while WXPlane does not draw, the built-in page keeps the pointer\n"); ++failures; }
        { std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_weather_wx_old.png"); }
        t.wxPlane = false; t.wxEmbed = 0; run(t);
        if (embedArea().active) { std::printf("FAIL: no WXPlane, no area\n"); ++failures; }
        openApp(APP_HOME); run(s); openApp(APP_HOME);
        io.DisplaySize = ImVec2(800, kLogicalH); ImGui::NewFrame(); { Actions act; build(fonts, s, 800, kLogicalH, act); } ImGui::Render();
        if (embedArea().active) { std::printf("FAIL: the area is only for the Weather page\n"); ++failures; }
    }
    // Settings: the brightness slider moves and is kept; the touch test's Clear button clears (it lies inside the touch area)
    {
        settings() = Settings();
        openApp(APP_SETTINGS);
        const float W = 800, H = kLogicalH;
        auto frame = [&](float x, float y, bool down) {
            io.DisplaySize = ImVec2(W, H); io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame(); Actions act; build(fonts, s, W, H, act); ImGui::Render();
        };
        frame(-1000, -1000, false); frame(-1000, -1000, false);
        // Display (the sidebar's second entry), then drag the brightness slider from its right end to its left
        frame(80, 154, false); frame(80, 154, true); frame(80, 154, false); frame(80, 154, false);
        frame(362, 125, false); frame(362, 125, true); frame(215, 125, true); frame(215, 125, true); frame(215, 125, false); frame(215, 125, false);
        for (int k = 0; k < 30; ++k) frame(-1000, -1000, false);   // the knob glides to where it was let go
        if (settings().brightness > 0.6f || settings().brightness < 0.1f) { std::printf("FAIL: brightness slider (%.2f)\n", settings().brightness); ++failures; }
        const std::string txt = settingsToText();
        const float set = settings().brightness;
        settings() = Settings(); settingsFromText(txt);
        if (std::fabs(settings().brightness - set) > 0.011f) { std::printf("FAIL: brightness is not kept\n"); ++failures; }
        // back to General: marks in the touch test, then Clear (bottom right of the area)
        frame(80, 112, false); frame(80, 112, true); frame(80, 112, false); frame(80, 112, false);
        frame(600, 330, false); frame(600, 330, true); frame(640, 360, true); frame(680, 380, true); frame(680, 380, false); frame(680, 380, false);
        if (touchTestCount() < 2) { std::printf("FAIL: touch test marks (%d)\n", touchTestCount()); ++failures; }
        const float cx = W - 18 - 42, cy = H - 14 - 22;
        frame(cx, cy, false); frame(cx, cy, false); frame(cx, cy, true); frame(cx, cy, true); frame(cx, cy, false); frame(cx, cy, false);
        if (touchTestCount() != 0) { std::printf("FAIL: the touch test's Clear button (%d marks left)\n", touchTestCount()); ++failures; }
        settings() = Settings();
    }
    // The built-in Weather page (no WXPlane) and the Ground page, with data and without: drawn, pictured, and the Ground page's
    // buttons ask for the right things
    {
        auto shot = [&](const SimState& t, int app, const char* name) {
            openApp(app);
            std::vector<Batch> b;
            for (int i = 0; i < 6; ++i) {   // a few long frames: eased things (the map's zoom) settle
                io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.25f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); b.clear(); clipDrawData(ImGui::GetDrawData(), b); }
            io.DeltaTime = 1.0f / 60.0f;
            if (b.empty()) { std::printf("FAIL: %s not drawn\n", name); ++failures; }
            Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_" + name + ".png");
        };
        SimState t = s; t.wxPlane = false;
        shot(t, APP_WEATHER, "weather_nodata");
        if (std::string(appName(APP_WEATHER)) != "Weather") { std::printf("FAIL: without WXPlane the icon is Weather\n"); ++failures; }
        t.wx.valid = true; t.wx.windDir = 240; t.wx.windKt = 14; t.wx.visSm = 7; t.wx.tempC = 12; t.wx.dewC = 8; t.wx.qnhHpa = 1009; t.wx.rain = 0.2f; t.wx.altFt = 4200;
        t.wx.clouds = 3; const float cb[3] = {2800, 9000, 24000}, ct[3] = {5200, 11500, 26000}, cc[3] = {0.45f, 0.7f, 0.2f};
        for (int i = 0; i < 3; ++i) { t.wx.cloudBaseFt[i] = cb[i]; t.wx.cloudTopFt[i] = ct[i]; t.wx.cloudCover[i] = cc[i]; }
        t.wx.aloft = 13; for (int i = 0; i < 13; ++i) { t.wx.aloftFt[i] = 3000.0f + i * 3500.0f; t.wx.aloftDir[i] = 230.0f + i * 4; t.wx.aloftKt[i] = 12.0f + i * 7; }
        shot(t, APP_WEATHER, "weather_builtin");
        if (embedArea().active) { std::printf("FAIL: the built-in weather page hands nothing to WXPlane\n"); ++failures; }
        shot(s, APP_HOME, "home_wxplane");
        if (std::string(appName(APP_WEATHER)) != "WXPlane") { std::printf("FAIL: with WXPlane the icon is WXPlane\n"); ++failures; }
        shot(t, APP_GROUND, "ground_generic");
        {   // the Map page: a made-up airport (runway, apron with a hole and a curved edge, lines, stands, taxi routes)
            // the triangulator: a square with a square hole is 8 triangles
            const std::vector<std::vector<std::pair<double, double>>> sq = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{3, 3}, {3, 7}, {7, 7}, {7, 3}}};
            const size_t ntri = triangulate(sq).size() / 3;
            if (ntri != 8) { std::printf("FAIL: square with a hole: %zu triangles\n", ntri); ++failures; }
            const std::string rows =
                "1 774 0 0 KXPT XPTablet Test Field\n"
                "1302 datum_lat 41.70000000\n1302 datum_lon -86.32000000\n"
                "100 45.72 1 0 0.25 1 2 1 09 41.70000000 -86.33500000 0 0 3 0 0 0 27 41.70000000 -86.30500000 0 0 3 0 0 0\n"
                "110 1 0.25 0.00 Apron\n"
                "111 41.70120000 -86.32600000\n112 41.70120000 -86.31600000 41.70160000 -86.31500000\n111 41.70300000 -86.31400000\n"
                "111 41.70300000 -86.32600000\n113 41.70200000 -86.32600000\n"
                "111 41.70220000 -86.32200000\n111 41.70260000 -86.32200000\n111 41.70260000 -86.32000000\n113 41.70220000 -86.32000000\n"
                "110 1 0.25 0.00 Taxiway A\n"
                "111 41.70030000 -86.32500000\n111 41.70030000 -86.31700000\n111 41.70120000 -86.31700000\n113 41.70120000 -86.32500000\n"
                "120 Centreline\n111 41.70075000 -86.32480000 1\n111 41.70075000 -86.31750000 1\n115 41.70150000 -86.31750000\n"
                "120 Hold\n111 41.70040000 -86.32100000 4\n115 41.70040000 -86.31900000\n"
                "1201 41.70075000 -86.32480000 both 0 n0\n1201 41.70075000 -86.31750000 both 1 n1\n1202 0 1 twoway taxiway_D A\n"
                "1300 41.70280000 -86.32400000 180 gate heavy|jets A1\n1301 E airline DAL AAL zzz\n1300 41.70280000 -86.32300000 180 gate jets A2\n1300 41.70280000 -86.31800000 180 gate jets A3\n";
            AptGeo geo;
            if (!parseAirport(rows, geo) || geo.icao != "KXPT" || geo.runways.size() != 1 || geo.pavement.empty() || geo.stands.size() != 3 || geo.taxiways.size() != 1) {
                std::printf("FAIL: the test airport (%s, %zu runways, %zu pavement points, %zu stands)\n", geo.icao.c_str(), geo.runways.size(), geo.pavement.size(), geo.stands.size());
                ++failures;
            }
            SimState mp = t;
            mp.map.havePos = true; mp.map.lat = 41.7008; mp.map.lon = -86.3222; mp.map.hdgTrue = 92; mp.map.utc = 1795312200;   // 2026-11-23 13:10Z
            mp.map.haveLocalOffset = true; mp.map.localOffsetS = -5 * 3600;
            mp.map.airports.push_back(std::make_shared<AptGeo>(geo));
            mp.map.airportPoints = {{"KXPT", 41.70, -86.32, 1, ""}, {"KSBN", 41.7087, -86.3173, 1, ""}, {"KGSH", 41.5264, -85.7922, 1, ""}, {"KEKM", 41.7194, -86.0003, 1, ""}};
            settings().mapAirports = true; settings().mapSource = 0; settings().mapLocalTime = true;
            shot(mp, APP_MAP, "map_airport");
            {   // the frame's cost at every zoom step, from the farthest in to the closest, every layer on (a step costing
                // much more than the others is how "zoomed right in, the sim freezes" shows here)
                settings().mapTimeZones = true; settings().mapOutlines = true; settings().mapRings = true; settings().mapIls = true;
                settings().mapNavaids = true; settings().mapWaypoints = true;
                for (int i = 0; i < 40; ++i) mapCommand(1);   // all the way out
                double worst = 0; int worstStep = -1;
                for (int step = 0; step < 40; ++step) {
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < 8; ++i) {
                        io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.25f; io.AddMousePosEvent(420, 260);
                        ImGui::NewFrame(); Actions act; build(fonts, mp, 800, kLogicalH, act); ImGui::Render();
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 8;
                    const ImDrawData* dd = ImGui::GetDrawData();
                    std::printf("map step %2d: %6.2f ms a frame, %d vertices\n", step, ms, dd ? dd->TotalVtxCount : 0);
                    if (ms > worst) { worst = ms; worstStep = step; }
                    mapCommand(0);   // one step in
                }
                if (worst > 40) { std::printf("FAIL: the map takes %.0f ms a frame at zoom step %d\n", worst, worstStep); ++failures; }
                // a gate's card: the airlines that park there, decoded
                if (geo.stands[0].airlines != "DAL AAL zzz" || geo.stands[0].size != "E" || !airlineName("dal") || airlineName("ZZZ")) { std::printf("FAIL: stand airlines (%s)\n", geo.stands[0].airlines.c_str()); ++failures; }
                for (int i = 0; i < 6; ++i) mapCommand(1);
                { SimState g2 = mp; g2.map.lat = 41.7026; g2.map.lon = -86.3236; mapSelect(6, "KXPT|0"); shot(g2, APP_MAP, "map_gate"); mapSelect(0, ""); }
                for (int i = 0; i < 6; ++i) mapCommand(0);
                settings().mapOutlines = false; settings().mapWaypoints = false;
            }
            settings().mapSource = 1; settings().mapHeadingUp = true; settings().mapLocalTime = false;
            shot(mp, APP_MAP, "map_satellite_heading");
            settings().mapSource = 0; settings().mapHeadingUp = false; settings().mapTimeZones = true; settings().mapLocalTime = true;
            // zoomed out with the time zones: press - a few times
            openApp(APP_MAP);
            for (int k = 0; k < 7; ++k) {   // press - (its bubble at the bottom left)
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(26, kLogicalH - 25); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, mp, 800, kLogicalH, act); ImGui::Render(); }
            }
            shot(mp, APP_MAP, "map_timezones");
            settings().mapTimeZones = false; settings().mapLocalTime = false;
        }
        {   // the map's traffic and outlines
            SimState mp = t;
            mp.map.havePos = true; mp.map.lat = 41.70; mp.map.lon = -86.30; mp.map.hdgTrue = 270; mp.map.altFt = 3000; mp.map.utc = 1795312200;
            mp.map.traffic = {{41.76, -86.20, 3400, 250, 210, -800, false, "N172SP", "C172"}, {41.62, -86.45, 11000, 90, 420, 1500, false, "DAL1234", "A321"},
                              {41.705, -86.33, 774, 0, 0, 0, true, "SKW55", "CRJ9"}};
            settings().mapTraffic = true; settings().mapOutlines = true; settings().mapAirports = false;
            showOnMap(41.70, -86.30);
            for (int k = 0; k < 5; ++k) {   // press - five times: about 50 nm across
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(26, kLogicalH - 25); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, mp, 800, kLogicalH, act); ImGui::Render(); }
            }
            shot(mp, APP_MAP, "map_traffic_outlines");
            settings().mapAirports = true; settings().mapOutlines = false;
        }
        {   // tapping on the map: a traffic card (with VATSIM), a navaid card (with Morse)
            auto tap = [&](const SimState& st, float x, float y) {
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); }
            };
            SimState mp = t;
            mp.map.havePos = true; mp.map.lat = 41.70; mp.map.lon = -86.30; mp.map.hdgTrue = 90; mp.map.altFt = 3000; mp.map.utc = 1795312200;
            mp.map.traffic = {{41.775, -86.30, 11000, 270, 280, -1200, false, "DAL1234", "A321"}};
            mp.map.navaids = {{3, 41.70, -86.40, 111.70f, -3, "SBN", "SOUTH BEND"}};
            VatsimPilot& v = mp.map.vatsim;
            v.found = v.hasPlan = true; v.callsign = "DAL1234"; v.name = "Jane Pilot KSBN"; v.cid = "1234567"; v.server = "USA-EAST"; v.rating = "PPL"; v.squawk = "4521";
            v.logon = "04:55Z"; v.qnhMb = 1013; v.rules = "IFR"; v.aircraft = "A321"; v.aircraftFull = "A321/L"; v.departure = "KORD"; v.arrival = "KDTW"; v.alternate = "KCLE";
            v.cruiseAlt = "FL320"; v.cruiseTas = "450"; v.depTime = "0500"; v.enroute = "0105"; v.fuel = "0300"; v.route = "WYNDE2 ELX DORET KKISS"; v.remarks = "PBN/A1B1C1D1 /V/ SIMBRIEF";
            settings().mapTraffic = true; settings().mapNavaids = true; settings().mapAirports = false; settings().mapWaypoints = false;
            showOnMap(41.70, -86.30);
            mapCommand(1); mapCommand(1);   // 5 nm
            for (int k = 0; k < 20; ++k) tap(mp, -1000, -1000);   // the zoom settles
            const float cy = (28 + 46 + kLogicalH) * 0.5f, unitsPerNm = (kLogicalH - 28 - 46) * 0.5f / 5.0f;
            tap(mp, 400, cy - 4.5f * unitsPerNm);   // the traffic, 4.5 nm north
            shot(mp, APP_MAP, "map_card_traffic");
            tap(mp, 400 - 4.48f * unitsPerNm, cy);   // the VOR, 4.5 nm west
            shot(mp, APP_MAP, "map_card_vor");
            tap(mp, 700, 300);   // an empty place: the card closes
            {   // a crowd: labels find free places around their aircraft, never over each other
                SimState crowd = mp;
                crowd.map.traffic.clear();
                const char* cs[8] = {"UAL12", "AAL845", "SWA2231", "DAL77", "JBU402", "N123AB", "SKW5521", "ENY3310"};
                for (int i = 0; i < 8; ++i)
                    crowd.map.traffic.push_back({41.70 + 0.012 * (i % 3) - 0.01, -86.30 + 0.018 * i - 0.06, 3000.0f + i * 700, 90.0f + i * 30, 250, 0, false, cs[i], i % 2 ? "B738" : "A320"});
                shot(crowd, APP_MAP, "map_traffic_crowd");
                // one closing head-on, 200 ft above, 0.5 nm out: TCAS RA range (it pulses red)
                SimState ra = mp;
                ra.map.aglFt = 2400; ra.map.altFt = 3000; ra.map.gsKt = 140; ra.map.hdgTrue = 0;
                ra.map.traffic = {{41.70 + 0.5 / 60.0, -86.30, 3200, 180, 250, -300, false, "N555RA", "C172"}, {41.70 + 3.0 / 60.0, -86.33, 9000, 90, 250, 0, false, "FAR123", "B738"}};
                if (!trafficInRA(ra.map, ra.map.traffic[0]) || trafficInRA(ra.map, ra.map.traffic[1])) { std::printf("FAIL: TCAS RA range\n"); ++failures; }
                shot(ra, APP_MAP, "map_traffic_ra");
            }
            settings().mapAirports = true;
        }
        {   // power: the button by the clock fades the tablet to black; a tap anywhere starts it again (logo, loading bar, then home)
            auto tap = [&](float x, float y) {
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); }
            };
            openApp(APP_HOME);
            tap(16, 14);   // the power button
            shot(t, APP_HOME, "power_off");
            tap(400, 300);   // anywhere: it starts
            shot(t, APP_HOME, "power_boot");
            shot(t, APP_HOME, "power_wake");
            if (powerState() != 4) { std::printf("FAIL: waking (%d)\n", powerState()); ++failures; }
            shot(t, APP_HOME, "power_on");
            if (powerState() != 0) { std::printf("FAIL: on again (%d)\n", powerState()); ++failures; }
        }
        {   // every provider on the home screen: the services' own icons
            Settings keep = settings();
            settings().showNavigraph = settings().showVatsim = settings().showSkyvector = settings().showForeflight = settings().showLive = true;
            shot(t, APP_HOME, "home_all_apps");
            settings() = keep;
        }
        SimState g_previewState;
        {   // the Flight Plan page with a plan
            SimState fp = t;
            FlightPlan& p = fp.plan;
            p.loaded = true; p.callsign = "AAL1234"; p.aircraft = "A321"; p.registration = "N123AA"; p.units = "lbs"; p.importedAt = "05:12Z";
            p.origin = "KSBN"; p.originName = "South Bend Intl"; p.originRunway = "27"; p.dest = "KORD"; p.destName = "Chicago O'Hare Intl"; p.destRunway = "28C"; p.alternate = "KMDW";
            p.route = "GIJ DCT PMM DCT OBK"; p.sid = ""; p.star = "WATSN4"; p.cruise = "FL240"; p.distanceNm = "92"; p.ete = "38 min"; p.costIndex = "30"; p.avgWind = "head 22 kt";
            p.blockFuel = "9800"; p.tripFuel = "4200"; p.reserveFuel = "2900"; p.altnFuel = "1600"; p.taxiFuel = "600"; p.extraFuel = "500"; p.zfw = "141200"; p.tow = "150400"; p.ldw = "146200";
            p.pdfUrl = "https://www.simbrief.com/ofp/flightplans/KSBNKORD.pdf";
            const char* ids[] = {"KSBN", "GIJ", "PMM", "OBK", "WATSN", "KORD"};
            for (int i = 0; i < 6; ++i) { FlightPlan::Leg l; l.ident = ids[i]; l.airway = i == 4 ? "WATSN4" : ""; l.alt = i == 0 || i == 5 ? "" : "FL240"; l.legNm = std::to_string(10 + i * 7); l.totalTime = "0:" + std::to_string(10 + i * 5); fp.plan.legs.push_back(l); }
            settings().simbriefUser = "alpineaviation";
            for (auto& l : fp.plan.legs) if (l.ident == "WATSN") l.cross = "between 10000 and 12000, 250 kt";   // (a made-up one: how it shows)
            shot(fp, APP_ROUTES, "flightplan");
            openDispatchFromPlan(); shot(fp, APP_DISPATCH, "dispatch");
            { SimState np = t; shot(np, APP_DISPATCH, "dispatch_empty"); }
            {   // a newer plan from SimBrief, in its window before Import (the plan in use stays under it)
                SimState pv = fp;
                pv.planPreview = fp.plan; pv.planPreviewOpen = true; pv.planPreview.alternateName = "Chicago Midway Intl"; pv.planPreview.callsign = "AAL1235"; pv.planPreview.ofpId = "2/1";
                pv.planPreview.dest = "KORD"; pv.planPreview.sid = "SBN2";
                for (int k = 0; k < 25; ++k) shot(pv, APP_ROUTES, "flightplan_preview");   // (it grows in)
                g_previewState = pv;
            }
            SimState empty = t;
            shot(empty, APP_ROUTES, "flightplan_empty");
            settings().simbriefUser.clear();
        }
        {   // Settings: Display and Data
            auto tap = [&](const SimState& st, float x, float y) {
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); }
            };
            SimState st = t;
            st.autoDimLevel = 0.62f;
            st.dataInfo = {{"Airports", "38114 in X-Plane's scenery (custom first)"}, {"Navaids", "Custom Data (your AIRAC, e.g. Navigraph's)"}, {"Airport facts", "OurAirports, kept a month"},
                           {"Map tiles", "OpenStreetMap / Esri, kept a month in Output/XPTablet/tiles"}};
            openApp(APP_SETTINGS);
            tap(st, 80, 154);   // Display
            settings().autoDim = true;
            shot(st, APP_SETTINGS, "settings_display");
            settings().autoDim = false;
            tap(st, 80, 196);   // Data
            settings().simbriefUser = "alpineaviation";
            st.plan.status = "Imported KSBN to KORD at 05:12Z"; st.plan.loaded = true;
            shot(st, APP_SETTINGS, "settings_data");
            settings().simbriefUser.clear();
            tap(st, 80, 280);   // Media
            settings().mediaDuck = 1; settings().duckRx = 0.35f; settings().duckTx = 0.15f;
            shot(st, APP_SETTINGS, "settings_media");
            settings().mediaDuck = 0;
            tap(st, 80, 112);   // back to General
        }
        {   // the status bar's clock: a tap switches Zulu and local time where the aircraft is (EST here)
            SimState c = t;
            c.zuluSec = 13 * 3600 + 10 * 60; c.map.havePos = true; c.map.lat = 41.70; c.map.lon = -86.30; c.map.utc = 1795308600 + 0; c.map.haveLocalOffset = true; c.map.localOffsetS = -5 * 3600;
            settings().statusLocalTime = false;
            openApp(APP_HOME);
            const bool downs[4] = {false, true, false, false};
            for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(30, 14); io.AddMouseButtonEvent(0, d);
                ImGui::NewFrame(); Actions act; build(fonts, c, 800, kLogicalH, act); ImGui::Render(); }
            if (!settings().statusLocalTime) { std::printf("FAIL: tapping the clock switches it to local time\n"); ++failures; }
            shot(c, APP_HOME, "home_localtime");
            settings().statusLocalTime = false;
        }
        {   // navaids, waypoints and the SimBrief route; AviTab's map commands move the map
            SimState mp = t;
            mp.map.havePos = true; mp.map.lat = 41.70; mp.map.lon = -86.30; mp.map.hdgTrue = 300; mp.map.utc = 1795312200;
            mp.map.navaids = {{3, 41.705, -86.30, 111.70f, -3, "SBN", "SOUTH BEND"}, {2, 41.627, -86.312, 344, 0, "SB", "STAFFORD"},
                              {4, 41.70, -86.29, 109.50f, 271.4f, "ISBN", "27"}, {13, 41.80, -86.05, 113.0f, 0, "GIJ", "NILES DME"}};
            mp.map.fixes = {{41.80, -86.50, "NILES", true}, {41.62, -86.10, "HAGRT", true}, {41.71, -86.20, "RW27", false}};
            mp.map.route = {{41.7087, -86.3173, "KSBN"}, {41.80, -86.50, "NILES"}, {41.90, -87.20, "OBK"}, {41.9786, -87.9048, "KORD"}};
            mp.map.routeStatus = "KSBN to KORD (SimBrief)";
            settings().mapNavaids = true; settings().mapWaypoints = true; settings().mapRoute = true; settings().mapAirports = false; settings().mapTraffic = false;
            showOnMap(41.70, -86.30);
            for (int k = 0; k < 4; ++k) mapCommand(1);   // zoomed out four steps
            double la0, lo0, la1, lo1; int z0, z1; float rg0, rg1;
            mapView(la0, lo0, z0, rg0);
            mapCommand(4);   // pan right
            mapView(la1, lo1, z1, rg1);
            if (!(lo1 > lo0) || z1 != z0) { std::printf("FAIL: AviTab pan_right moves the map east (%.3f -> %.3f)\n", lo0, lo1); ++failures; }
            mapCommand(3);
            shot(mp, APP_MAP, "map_navaids_route");
            settings().mapAirports = true; settings().mapTraffic = true; settings().mapWaypoints = false;
        }
        {   // Airports: the list, and an airport with its radio, CTAF, runways and parking
            const std::string rows =
                "1 774 0 0 KXPT XPTablet Test Field\n1302 datum_lat 41.70000000\n1302 datum_lon -86.32000000\n"
                "100 45.72 1 0 0.25 1 2 1 09 41.70000000 -86.33500000 0 0 3 0 0 0 27 41.70000000 -86.30500000 0 0 3 0 0 0\n"
                "100 30.00 3 0 0.25 0 0 1 18 41.71000000 -86.32000000 0 0 1 0 0 0 36 41.69000000 -86.32000000 0 0 1 0 0 0\n"
                "1050 128650 ATIS\n1053 121900 GND\n1054 118950 TWR\n1055 125250 SOUTH BEND APP\n"
                "1300 41.70280000 -86.32400000 180 gate jets A1\n1300 41.70280000 -86.32300000 180 gate jets A2\n1300 41.70280000 -86.31800000 180 tie_down props GA1\n";
            auto geo = std::make_shared<AptGeo>();
            if (!parseAirport(rows, *geo) || geo->freqs.size() != 4 || geo->elevationFt != 774 || geo->runwayInfo.size() != 2) { std::printf("FAIL: airport radio and runways (%zu freqs)\n", geo->freqs.size()); ++failures; }
            SimState ap = t;
            ap.map.havePos = true; ap.map.lat = 41.65; ap.map.lon = -86.40;
            ap.airportList = {{"KXPT", 41.70, -86.32, 1, "XPTablet Test Field"}, {"KSBN", 41.7087, -86.3173, 1, "South Bend Intl"}, {"3IN6", 41.62, -86.28, 1, "Some Farm Strip"}};
            ap.airport.icao = "KXPT"; ap.airport.found = true; ap.airport.geo = geo;
            ap.airport.city = "South Bend"; ap.airport.region = "US-IN";
            ap.airport.freqs = {{"ATIS", 128.650, "ATIS"}, {"Ground", 121.900, "GND"}, {"Tower", 118.950, "TWR"}, {"Approach", 125.250, "SOUTH BEND APP"}};
            ap.airport.ctaf = {"CTAF", 118.950, "Tower frequency, the CTAF when the tower is closed"};
            ap.metar.icao = "KXPT"; ap.metar.raw = "KXPT 180054Z 24014G22KT 10SM FEW035 BKN250 08/02 A2998 RMK AO2";
            shot(ap, APP_AIRPORTS, "airports");
        }
        {   // Notes: a clearance typed in; the scratchpad with handwriting
            notesFromText("craft0=KORD\ncraft1=PORTAGE ONE, then as filed\ncraft2=5000, expect FL350 10 min after\ncraft3=125.25\ncraft4=4521\n"
                          "stroke=0 100,200 150,260 200,200 250,260 300,200\nstroke=1 400,500 420,520 460,560 520,600 600,620\nscratch=RWY 27 HOLD SHORT B\n");
            shot(t, APP_NOTES, "notes_clearance");
            // the Scratchpad tab (third pill)
            openApp(APP_NOTES);
            const bool downs[4] = {false, true, false, false};
            for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(12 + 2 * 128 + 60, 28 + 46 + 8 + 15); io.AddMouseButtonEvent(0, d);
                ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); }
            shot(t, APP_NOTES, "notes_scratchpad");
            // typing into a field on the Clearance pad
            const std::string saved = notesToText();
            if (saved.find("craft0=KORD") == std::string::npos || saved.find("stroke=1 400,500") == std::string::npos) { std::printf("FAIL: notes are kept\n"); ++failures; }
        }
        {   // Checklists (not on a ToLiss)
            SimState c = t; c.toliss = false; c.acfIcao = "C172";
            shot(c, APP_CHECKLISTS, "checklists_light");
            c.acfIcao = "B738";
            c.checklistText = "# Before start\nDoors | CLOSED\nBeacon | ON\n# Taxi\nFlaps | 5\n";
            c.checklistSource = "From Checklists/B738.txt";
            shot(c, APP_CHECKLISTS, "checklists_own");
        }
        {   // Documents: the list
            SimState d = t;
            d.docs.listed = true;
            d.docs.folders = {"E:/X-Plane 12/Output/XPTablet/Documents"};
            d.docs.files = {{"A320 FCOM.pdf", "/x/A320 FCOM.pdf", "This aircraft", true, 31457280}, {"Normal checklist.pdf", "/x/n.pdf", "This aircraft", true, 204800},
                            {"KSBN airport diagram.png", "/x/ksbn.png", "XPTablet's Documents", false, 880000}, {"KORD 10-9.jpg", "/x/kord.jpg", "AviTab charts", false, 1200000}};
            shot(d, APP_DOCUMENTS, "documents");
            // a PDF opens in the page itself (its own browser tab, in the Documents' group), not in the Browser
            d.browser.status = 1;
            { Actions a2; for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(100, 170); io.AddMouseButtonEvent(0, k == 1); ImGui::NewFrame(); Actions act; build(fonts, d, 800, kLogicalH, act); ImGui::Render(); if (act.webTabNew) a2 = act; }
              for (int k = 0; k < 2; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, d, 800, kLogicalH, act); ImGui::Render(); if (act.webTabNew) a2 = act; }
              if (!a2.webTabNew || a2.webTabGroup != 300 || a2.webTabUrl.find("FCOM.pdf#toolbar=0") == std::string::npos || currentApp() != APP_DOCUMENTS) {
                  std::printf("FAIL: a PDF in Documents opens in its own tab there (%d, group %d, %s, app %d)\n", a2.webTabNew ? 1 : 0, a2.webTabGroup, a2.webTabUrl.c_str(), currentApp()); ++failures; } }
            shot(d, APP_DOCUMENTS, "documents_pdf");
        }
        SimState g = s; g.toliss = true; g.acfIcao = "A20N";
        g.ground.toliss = true; g.ground.openSam = true; g.ground.jetway = 1; g.ground.paxDoors = 4; g.ground.cargoDoors = 2; g.ground.cargoDoors = 3; g.ground.deice = true; g.ground.jwDoors = 1; g.ground.slides = 1; g.ground.cabinLight = 0.4f; g.ground.cabinAuto = 0; g.ground.paxDoor[0] = 2; g.ground.paxDoor[1] = 1; g.ground.cargoDoor[0] = 2; g.ground.chocks = true; g.ground.extPower = true;
        shot(g, APP_GROUND, "ground_toliss");
        { SimState g2 = g; g2.acfIcao = "A21N"; g2.ground.paxDoors = 8; shot(g2, APP_GROUND, "ground_a321"); }
        { SimState g3 = t;
          for (const char* ty : {"A339", "C172", "MD82", "B744", "B738", "CL60", "FA8X", "CRJ9", "DH8D", "GL7T", "RFAL", "A346", "ZZZZ"}) { g3.acfIcao = ty; shot(g3, APP_GROUND, (std::string("ground_") + ty).c_str()); }
          SimState g4 = g; g4.acfIcao = "A319"; shot(g4, APP_GROUND, "ground_a319"); g4.acfIcao = "A346"; g4.ground.paxDoors = 8; shot(g4, APP_GROUND, "ground_a346_doors"); }
        {   // Settings, then its Credits view (the pill at the right of the title bar)
            shot(s, APP_SETTINGS, "settings");
            const bool downs[4] = {false, true, false, false};
            for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(80, kLogicalH - 12 - 30); io.AddMouseButtonEvent(0, d);
                ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); }
            std::vector<Batch> b;
            for (int i = 0; i < 2; ++i) { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); clipDrawData(ImGui::GetDrawData(), b); }
            Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_credits.png");
            for (bool d : downs) { io.AddMousePosEvent(80, 112); io.AddMouseButtonEvent(0, d);   // back to the settings
                ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); }
        }
        // the METAR search: type KBOS on the page's keys, press Search, and show an answer
        {
            SimState m = t;
            openApp(APP_WEATHER);
            auto tap = [&](float x, float y) {
                Actions got;
                const bool downs[4] = {false, true, false, false};
                for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                    ImGui::NewFrame(); Actions act; build(fonts, m, 800, kLogicalH, act); ImGui::Render(); if (!act.metarSearch.empty()) got = act; }
                return got;
            };
            tap(800 - 14 - 54, 28 + 9 + 14);   // the "METAR search" view
            // the keys: rows of 10 from x = 26, each (388 - 45) / 10 wide plus 5; the rows start under the code box
            const float kw = std::floor((416.0f - 28.0f - 45.0f) / 10.0f), ky0 = 84 + 34 + 40 + 14, kh = std::min(42.0f, std::floor((468.0f - 14.0f - ky0 - 15.0f) / 4.0f));
            auto key = [&](char c) {
                const char* rowsK[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
                for (int r = 0; r < 4; ++r) { const char* q = std::strchr(rowsK[r], c); if (!q) continue;
                    const float x0 = 26 + (r == 2 ? (kw + 5) * 0.5f : r == 3 ? (kw + 5) : 0.0f);
                    tap(x0 + (float)(q - rowsK[r]) * (kw + 5) + kw * 0.5f, ky0 + r * (kh + 5) + kh * 0.5f); }
            };
            key('K'); key('B'); key('O'); key('S');
            const Actions r2 = tap(26 + (416 - 28) - 55, 84 + 34 + 20);
            if (r2.metarSearch != "KBOS") { std::printf("FAIL: METAR search typed '%s'\n", r2.metarSearch.c_str()); ++failures; }
            m.metar.icao = "KBOS"; m.metar.raw = "KBOS 180054Z 24014G22KT 7SM -RA SCT028 BKN090 12/08 A2980 RMK AO2 SLP090";
            { std::vector<Batch> bb; for (int i = 0; i < 2; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                ImGui::NewFrame(); Actions act; build(fonts, m, 800, kLogicalH, act); ImGui::Render(); clipDrawData(ImGui::GetDrawData(), bb); }
              Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, bb, tex, tw, th, scale); writePng(cv, out + "/tablet_weather_metar.png"); }
            tap(800 - 14 - 108 - 6 - 54, 28 + 9 + 14);   // back to "At the aircraft" for the pictures that follow
        }
        // press Connect (jetway): the first row's button, right end of the Services card
        auto press = [&](const SimState& st, float x, float y) {
            Actions got;
            const bool downs[6] = {false, false, true, true, false, false};
            for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); if (act.ground != GA_NONE || act.planAccept || act.planDismiss || act.planImport || act.planClear) got = act; }
            return got;
        };
        openApp(APP_GROUND);
        // a ToLiss: the jetway is a bubble beside door 1L, at the card's left edge
        float jy = -1;
        for (float yy = 90; yy < 460 && jy < 0; yy += 4) { const Actions q = press(g, 78, yy); if (q.ground == GA_JETWAY_DOOR && q.groundIndex == 0) jy = yy; }
        if (jy < 0) { std::printf("FAIL: no Connect bubble at door 1L\n"); ++failures; }
        // an A321 with the classic doors: a second bubble, at 2L
        { SimState g2 = g; g2.acfIcao = "A321"; g2.ground.paxDoors = 8; g2.ground.jwDoors = 2; g2.ground.jwDocked[1] = true; g2.ground.paxDoor[2] = 2;
          bool second = false;
          for (float yy = 90; yy < 460 && !second; yy += 4) { const Actions q = press(g2, 78, yy); second = q.ground == GA_JETWAY_DOOR && q.groundIndex == 1; }
          if (!second) { std::printf("FAIL: no bubble at door 2L of an A321\n"); ++failures; }
          shot(g2, APP_GROUND, "ground_a321_jetways"); openApp(APP_GROUND); }
        // not a ToLiss: an airliner still gets the 1L bubble (X-Plane's own jetways, or openSAM's)
        { SimState t2 = t; t2.acfIcao = "B738"; t2.toliss = false; t2.ground.toliss = false; t2.ground.openSam = false; t2.ground.jetway = 1; t2.ground.jwDoors = 1;
          t2.ground.parkBrake = true;
          bool hit = false;
          for (float yy = 90; yy < 460 && !hit; yy += 4) { const Actions q = press(t2, 78, yy); hit = q.ground == GA_JETWAY_DOOR && q.groundIndex == 0; }
          if (!hit) { std::printf("FAIL: no 1L bubble on a B738\n"); ++failures; }
          t2.ground.beacon = true; t2.ground.enginesRunning = true; t2.ground.parkBrake = false;
          shot(t2, APP_GROUND, "ground_b738_limits"); openApp(APP_GROUND); }
        // the browser before its program is up, and with the on-screen keyboard (pressed from the address bar)
        { SimState wb = s; wb.browser.status = 0; shot(wb, APP_BROWSER, "browser_starting");
          wb.browser.status = 1; wb.browser.url = "https://chartfox.org/";
          wb.browser.tabs = {{1, "ChartFox - Free Flight Simulation Charts", "https://chartfox.org/", false, 0}, {2, "SimBrief Dispatch", "https://dispatch.simbrief.com/", true, 100 + APP_SIMBRIEF}, {3, "", "https://fly.volanta.app/", false, 100 + APP_LIVEMAP}};   // the Browser's, SimBrief's and Volanta's
          wb.browser.activeTab = 1;
          press(wb, 300, 28 + 46 + 32 + 20); shot(wb, APP_BROWSER, "browser_keyboard");
          // a web app already open in a tab is shown again (its tab selected), not loaded over the shown page
          { openApp(APP_LIVEMAP);
            Actions got;
            for (int k = 0; k < 2; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, wb, 800, kLogicalH, act); ImGui::Render(); if (act.webTabSelect || act.webTabNew || !act.webNavigate.empty()) got = act; }
            if (got.webTabSelect != 3) { std::printf("FAIL: Volanta's tab is shown again (%d, new %d, nav %s)\n", got.webTabSelect, got.webTabNew ? 1 : 0, got.webNavigate.c_str()); ++failures; } }
          // a web app not open gets a tab of its own
          { openApp(APP_SIMBRIEF); SimState w2 = wb; w2.browser.tabs.erase(w2.browser.tabs.begin() + 1);
            Actions got;
            for (int k = 0; k < 2; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, w2, 800, kLogicalH, act); ImGui::Render(); if (act.webTabSelect || act.webTabNew || !act.webNavigate.empty()) got = act; }
            if (!got.webTabNew || got.webTabUrl.find("simbrief") == std::string::npos || got.webTabGroup != 100 + APP_SIMBRIEF) { std::printf("FAIL: SimBrief opens in a new tab of its own group\n"); ++failures; } }
          // the Browser's page never shows a web app's tabs: its strip has the Browser's one tab only
          { openApp(APP_BROWSER); Actions got;
            for (int k = 0; k < 2; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, wb, 800, kLogicalH, act); ImGui::Render(); if (act.webTabSelect || act.webTabNew) got = act; }
            if (got.webTabNew || (got.webTabSelect && got.webTabSelect != 1)) { std::printf("FAIL: the Browser keeps to its own tab (select %d, new %d)\n", got.webTabSelect, got.webTabNew ? 1 : 0); ++failures; } }
          openApp(APP_BROWSER); shot(wb, APP_BROWSER, "browser_tabs");
          { openApp(APP_SIMBRIEF); SimState w3 = wb; w3.browser.zoomPct = 125; shot(w3, APP_SIMBRIEF, "webapp_header"); openApp(APP_SIMBRIEF);
            // its Home button: back to SimBrief's own start page
            Actions got;
            for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(800 - 154, 28.0f + 23.0f); io.AddMouseButtonEvent(0, k == 1);
                                          ImGui::NewFrame(); Actions act; build(fonts, w3, 800, kLogicalH, act); ImGui::Render(); if (!act.webNavigate.empty()) got = act; }
            if (got.webNavigate.find("dispatch.simbrief.com") == std::string::npos) { std::printf("FAIL: the web app Home button goes to its start page (%s)\n", got.webNavigate.c_str()); ++failures; } }
          openApp(APP_BROWSER);
          SimState wa = s; wa.browser.status = -3; wa.browser.message = "On macOS the web pages are not available yet."; shot(wa, APP_CHARTS, "browser_unavailable"); openApp(APP_GROUND); }
        {   // the plan window: Import and Dismiss take presses; the page's own buttons under it do not
            openApp(APP_ROUTES);
            bool took = false, dismissed = false;
            for (float yy = 300; yy < 470 && !(took && dismissed); yy += 4) {
                const Actions q1 = press(g_previewState, 560, yy); if (q1.planAccept) took = true;
                const Actions q2 = press(g_previewState, 240, yy); if (q2.planDismiss) dismissed = true;
            }
            if (!took || !dismissed) { std::printf("FAIL: the plan window's Import and Dismiss\n"); ++failures; }
            const Actions under = press(g_previewState, 760, 110);
            if (under.planImport || under.planClear) { std::printf("FAIL: the page under the plan window took a press\n"); ++failures; }
            openApp(APP_GROUND);
        }
        // the cabin lights: the slider sets the level in steps of 10 %; with them off, On brings back the level they last had
        { bool slid = false, on = false, arm = false;
          SimState off = g; off.ground.cabinLight = 0;
          for (float yy = 90; yy < 330 && !(slid && on && arm); yy += 3) {
              const Actions q = press(g, 660, yy);
              if (q.ground == GA_CABIN_LIGHT && q.groundIndex % 10 == 0 && q.groundIndex > 0 && q.groundIndex < 100) slid = true;
              const Actions o = press(off, 668, yy);
              if (o.ground == GA_CABIN_LIGHT && o.groundIndex > 0) on = true;
              const Actions o2 = press(g, 450, yy);
              if (o2.ground == GA_SLIDES_ARM) arm = true;
          }
          if (!slid) { std::printf("FAIL: the cabin light slider\n"); ++failures; }
          if (!on) { std::printf("FAIL: the cabin light On button\n"); ++failures; }
          if (!arm) { std::printf("FAIL: the slides Arm button\n"); ++failures; } }
        g.ground.onGround = false;
        const Actions r = press(g, 78, jy);
        if (r.ground != GA_NONE) { std::printf("FAIL: no jetway in the air\n"); ++failures; }
        openApp(APP_HOME);
    }
    {   // ImGui's recoverable errors (a Push without its Pop, an item before Begin...): in the sim each one is a "Dear ImGui" pop-up
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const char* log = g.DebugLogBuf.c_str();
        int errs = 0;
        for (const char* p = log; (p = std::strstr(p, "[imgui-error]")) != nullptr; ++p) {
            const char* e = std::strchr(p, '\n');
            std::printf("ImGui error: %.*s\n", (int)(e ? e - p : (long)std::strlen(p)), p);
            if (++errs > 20) break;
        }
        if (errs) { std::printf("ui_shot: %d ImGui error(s) in the frames above\n", errs); ++failures; }
    }
    ImGui::DestroyContext();
    std::printf(failures ? "ui_shot: %d failure(s)\n" : "ui_shot: ok\n", failures);
    return failures ? 1 : 0;
}
