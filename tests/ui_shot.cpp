// Off-line pictures of the tablet: draws the real UI (src/tabletui.cpp) with Dear ImGui, clips it with the plugin's own clipper
// (src/tabrender.cpp) and rasterizes the triangles in software into PNG files, so the layout can be checked without X-Plane or a GPU.
// Also a smoke test: every app is opened and drawn at several screen shapes, and no triangle may leave the screen.
//   ui_shot <out-dir> [scale]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef XPT_UI_PNG
#include <zlib.h>
#endif

#include "imgui.h"
#include "tabletui.h"
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
                    const float a = col[3] * ta;
                    float* o = &cv.rgb[((size_t)y * cv.w + x) * 3];
                    for (int k = 0; k < 3; ++k) o[k] = o[k] * (1 - a) + col[k] * a;
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
    const Fonts fonts = loadFonts(scale);
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex, &tw, &th);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);

    SimState s;
    s.acfIcao = "A321"; s.zuluSec = 23 * 3600 + 7 * 60; s.onPanel = true;
    s.panel[0] = 1024; s.panel[1] = 0; s.panel[2] = 800; s.panel[3] = 480;
    s.panelEnabled = s.panelPowered = true; s.wxPlane = true; s.pixelScale = scale;
    s.batteryPct = 86; s.charging = true;

    int failures = 0;
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
                    if (v.pos.x < -0.01f || v.pos.y < -0.01f || v.pos.x > W + 0.01f || v.pos.y > H + 0.01f) { ++failures; break; }
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
        settings().chartsProvider = 1; settings().liveProvider = 1; settings().showForeflight = false;
        settings().favorites = {APP_WEATHER, APP_MAP}; settings().order = {APP_SETTINGS, APP_NOTES};
        const std::string txt = settingsToText();
        settings() = Settings();
        settingsFromText(txt);
        const Settings& st = settings();
        if (st.chartsProvider != 1 || st.liveProvider != 1 || st.showForeflight || !st.showSimbrief) { std::printf("FAIL: settings round trip\n"); ++failures; }
        if (st.favorites != std::vector<int>({APP_WEATHER, APP_MAP}) || st.order.size() != (size_t)APP_COUNT - 2 || st.order[0] != APP_SETTINGS || st.order[1] != APP_NOTES) { std::printf("FAIL: layout round trip\n"); ++failures; }
        openApp(APP_CHARTS);
        if (currentApp() != APP_BROWSER || browserChrome() || std::string(browserUrl()).find("navigraph") == std::string::npos) { std::printf("FAIL: charts provider\n"); ++failures; }
        openApp(APP_LIVEMAP);
        if (std::string(browserUrl()).find("vatsim") == std::string::npos || std::string(appName(APP_LIVEMAP)) != "VATSIM Map") { std::printf("FAIL: live map provider\n"); ++failures; }
        openApp(APP_BROWSER);
        if (!browserChrome()) { std::printf("FAIL: the Browser icon opens the full browser\n"); ++failures; }
        if (appVisible(APP_FOREFLIGHT) || !appVisible(APP_SIMBRIEF)) { std::printf("FAIL: web app switch\n"); ++failures; }
        settings() = Settings();
    }
    // a ToLiss aircraft has its own performance calculator: the Performance app is not offered there
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
        if (!appVisible(APP_PERFORMANCE)) { std::printf("FAIL: Performance must return in other aircraft\n"); ++failures; }
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
        for (gy = 40; gy < 300; gy += 10) { frame(gx, gy, false); frame(gx, gy, true); frame(gx + 40, gy + 40, true); frame(gx + 40, gy + 40, true);
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
        if (settings().favoriteSlots != 2 || settings().favorites.size() != 2 || settings().order.size() != (size_t)APP_COUNT - 2 || settings().order[0] != APP_WEATHER) { std::printf("FAIL: favorites bar size\n"); ++failures; }
        settingsFromText("favorite_slots=9\n");
        if (settings().favoriteSlots != kMaxFavorites) { std::printf("FAIL: favorites bar size limit\n"); ++failures; }
        settings() = Settings();
    }
    ImGui::DestroyContext();
    std::printf(failures ? "ui_shot: %d failure(s)\n" : "ui_shot: ok\n", failures);
    return failures ? 1 : 0;
}
