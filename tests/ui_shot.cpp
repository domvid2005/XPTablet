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
#include "arcade.h"
#include "acars.h"
#include "simbrief.h"
#include "airspace_parse.h"
#include "airlines.h"
#include "tabrender.h"
#include "soft_raster.h"

using namespace xpt;

namespace {

using xpt_test::Canvas;
using xpt_test::raster;

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

static xpt::FlightPlan g_testPlan; static std::vector<xpt::MapState::Procedure> g_testProcs;
static int installedApps() { int n = 0; for (int a = 0; a < APP_COUNT; ++a) n += appInstalled(a) && appVisible(a) ? 1 : 0; return n; }
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
    fonts.logoMask = (ImTextureID)(intptr_t)5;
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
        if (!st.showNavigraph || st.showCharts || !st.showVatsim || st.showLive || st.showForeflight || !st.showSimbrief) { std::printf("FAIL: settings round trip\n"); ++failures; }
        int placed = 0;
        for (int a : st.order) placed += a >= 0 ? 1 : 0;
        if (st.favorites != std::vector<int>({APP_WEATHER, APP_MAP}) || st.order[0] != APP_SETTINGS || st.order[1] != -1 || st.order[2] != APP_NOTES || placed != installedApps() - 2) { std::printf("FAIL: layout round trip\n"); ++failures; }
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
        if (!settings().showNavigraph || settings().showCharts) { std::printf("FAIL: older settings\n"); ++failures; }
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
            int last = 20;   // the first page's last place, made free
            if (settings().order[last] >= 0) { settings().order.push_back(settings().order[last]); settings().order[last] = -1; }
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
            frame(56, H - 28, false); frame(56, H - 28, true); frame(56, H - 28, false);   // + : its menu
            { Canvas cv((int)(W * scale), (int)(H * scale)); batches.clear(); frame(-1000, -1000, false); raster(cv, batches, tex, tw, th, scale); writePng(cv, out + "/tablet_arrange_menu.png"); }
            frame(56, H - 28, false); frame(56, H - 28, true); frame(56, H - 28, false);   // and shut again
            {   // Compress, beside Done (above it where the favorites bar leaves no room): everything slides up, no place left open
                auto gaps = [&]() { int lastUsed = -1, n = 0; const auto& o = settings().order; for (int i = 0; i < (int)o.size(); ++i) if (o[i] >= 0 && appVisible(o[i])) lastUsed = i;
                                    for (int i = 0; i < lastUsed; ++i) if (o[i] < 0 || !appVisible(o[i])) ++n; return n; };
                std::vector<int> before;
                for (int e : settings().order) if (e >= 0 && appVisible(e)) before.push_back(e);
                if (gaps() == 0) { std::printf("FAIL: (test) Compress needs an open place to close\n"); ++failures; }
                const float slots = (float)std::max(0, std::min(kMaxFavorites, settings().favoriteSlots));
                const bool beside = W - 96 - 8 - 104 >= (slots > 0 ? (W + slots * 86) * 0.5f + 12 : 0) + 6;
                const float bx = beside ? W - 96 - 8 - 52 : W - 68, by = beside ? H - 28 : H - 68;
                frame(bx, by, false); frame(bx, by, true); frame(bx, by, false);
                frame(-1000, -1000, false); frame(-1000, -1000, false);
                { Canvas cv((int)(W * scale), (int)(H * scale)); batches.clear(); frame(-1000, -1000, false); raster(cv, batches, tex, tw, th, scale); writePng(cv, out + "/tablet_arrange_compressing.png"); }
                for (int k = 0; k < 40; ++k) frame(-1000, -1000, false);
                std::vector<int> after;
                for (int e : settings().order) if (e >= 0 && appVisible(e)) after.push_back(e);
                if (gaps() != 0) { std::printf("FAIL: Compress closes the open places (%d left)\n", gaps()); ++failures; }
                if (after != before) { std::printf("FAIL: Compress keeps everything, in its order\n"); ++failures; }
                { Canvas cv((int)(W * scale), (int)(H * scale)); batches.clear(); frame(-1000, -1000, false); raster(cv, batches, tex, tw, th, scale); writePng(cv, out + "/tablet_arrange_compressed.png"); }
            }
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
        if (!settings().favorites.empty() || std::count_if(settings().order.begin(), settings().order.end(), [](int a) { return a >= 0; }) != installedApps()) { std::printf("FAIL: favorites bar off\n"); ++failures; }
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
        // the App Store, Games, VATSIM's page; the home screen with widgets, a folder and a second page
        shot(t, APP_APPSTORE, "appstore");
        {   // its search: typed, the apps that match (from every category)
            auto tp = [&](float x, float y) { for (int k = 0; k < 4; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1 || k == 2);
                ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); } };
            tp(680, 99); for (char c : std::string("log")) tabletKey(c, true);
            if (!tabletTyping()) { std::printf("FAIL: the App Store's search takes typing (its keyboard comes up)\n"); ++failures; }
            {   // as it is while typing, the keyboard up (shot() would close it)
                std::vector<Batch> b;
                for (int i = 0; i < 12; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.05f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                    ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); b.clear(); clipDrawData(ImGui::GetDrawData(), b); }
                io.DeltaTime = 1.0f / 60.0f;
                Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_appstore_search_typing.png");
            }
            shot(t, APP_APPSTORE, "appstore_search");
            tabletKey(13, false); tp(774, 99);   // (put away, then cleared)
        }
        { const int keep = settings().wallpaper;
          settings().wallpaper = 5; shot(t, APP_HOME, "home_noir"); shot(t, APP_SETTINGS, "settings_noir");
          settings().wallpaper = 6; shot(t, APP_HOME, "home_cloud"); shot(t, APP_MAP, "map_cloud");
          settings().wallpaper = 8; shot(t, APP_HOME, "home_custom");
          settings().wallpaper = keep; }
        shot(t, APP_GAMES, "games");
        {   // a Custom background's colours from its picture: a sunset gives an orange accent, a green aurora a green one, grey the blue
            const int W = 200, H = 120;
            auto make = [&](int kind) {
                std::vector<unsigned char> px((size_t)W * H * 4);
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    unsigned char* q = &px[((size_t)y * W + x) * 4]; const float t = (float)y / H;
                    if (kind == 0) { q[0] = (unsigned char)(250 - 150 * t); q[1] = (unsigned char)(150 - 110 * t); q[2] = (unsigned char)(70 + 80 * t); }
                    else if (kind == 1) { const bool band = std::fabs(t - 0.4f) < 0.12f; q[0] = 8; q[1] = band ? 210 : 14; q[2] = band ? 110 : 34; }
                    else { q[0] = q[1] = q[2] = (unsigned char)(60 + 100 * t); }
                    q[3] = 255;
                }
                int avg, acc; pictureColors(px.data(), W, H, avg, acc); return std::make_pair(avg, acc);
            };
            auto hueOf = [](int c) { const float R = (c >> 16 & 255) / 255.f, G = (c >> 8 & 255) / 255.f, B = (c & 255) / 255.f, mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B)), d = mx - mn;
                if (d <= 0) return -1.0f; float h = mx == R ? std::fmod((G - B) / d, 6.0f) : mx == G ? (B - R) / d + 2 : (R - G) / d + 4; if (h < 0) h += 6; return h * 60; };
            const auto sun = make(0), aur = make(1), grey = make(2);
            std::printf("picture colours: sunset bg %06X accent %06X (hue %.0f), aurora bg %06X accent %06X (hue %.0f), grey accent %06X\n", sun.first, sun.second, hueOf(sun.second), aur.first, aur.second, hueOf(aur.second), grey.second);
            if (hueOf(sun.second) < 0 || hueOf(sun.second) > 50) { std::printf("FAIL: a sunset's accent is orange\n"); ++failures; }
            if (hueOf(aur.second) < 120 || hueOf(aur.second) > 175) { std::printf("FAIL: an aurora's accent is green\n"); ++failures; }
            if (grey.second != 0x46A0FF) { std::printf("FAIL: a grey picture's accent is the tablet's blue\n"); ++failures; }
            const int mxA = std::max(sun.second >> 16 & 255, std::max(sun.second >> 8 & 255, sun.second & 255));
            if (mxA < 220) { std::printf("FAIL: the accent is bright enough to read\n"); ++failures; }
        }
        {   // xPilot's commands as buttons on the VATSIM page (only those found)
            SimState u = t; u.xpilotCmds = {"xpilot/toggle_nearby_atc", "xpilot/toggle_text_message_console", "xpilot/toggle_notification_panel", "xpilot/toggle_aircraft_labels", "xpilot/toggle_tcas", "xpilot/toggle_default_atis", "xpilot/split_audio_channels"};
            u.xpilotLogin = 1; u.xpilotCallsign = "AAL1234"; u.xpilotCom1 = "SBN_TWR"; u.xpilotAircraft = 14;
            const auto keepInst = settings().installed; settings().installed.push_back("vatsimhub");
            shot(u, APP_VATSIMHUB, "vatsimhub_group");
            settings().installed.push_back("xpilot"); layoutHome();   // (installed in an earlier build)
            if (appVisible(APP_XPILOT)) { std::printf("FAIL: xPilot is not an app of its own\n"); ++failures; }
            auto tp0 = [&](float x, float y) { for (int k = 0; k < 4; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1 || k == 2);
                ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); } };
            tp0(-100, -100); tp0(670, 360);   // (the xPilot page, from the VATSIM app's bubble)
            if (currentApp() != APP_XPILOT) { std::printf("FAIL: the xPilot page from the VATSIM app (%d)\n", currentApp()); ++failures; }
            {   std::vector<Batch> b;
                for (int i = 0; i < 12; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.05f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                    ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); b.clear(); clipDrawData(ImGui::GetDrawData(), b); }
                io.DeltaTime = 1.0f / 60.0f;
                Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_xpilot_page.png"); }
            std::string pressed;
            for (int k = 0; k < 4; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(130, 200); io.AddMouseButtonEvent(0, k == 1 || k == 2);
                ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); if (!act.xpilotCmd.empty()) pressed = act.xpilotCmd; }
            if (pressed != "xpilot/toggle_text_message_console") { std::printf("FAIL: xPilot's Messages button (%s)\n", pressed.c_str()); ++failures; }
            {   // opened from the VATSIM app, Back goes back to it (and from there, Home)
                auto tp = [&](float x, float y) { for (int k = 0; k < 4; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1 || k == 2);
                    ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); } };
                openApp(APP_VATSIMHUB); tp(-100, -100);
                tp(670, 360);   // the xPilot bubble (bottom right)
                if (currentApp() != APP_XPILOT) { std::printf("FAIL: the VATSIM app's xPilot bubble (%d)\n", currentApp()); ++failures; }
                tp(40, 50);     // Back
                if (currentApp() != APP_VATSIMHUB) { std::printf("FAIL: Back from xPilot opened in VATSIM goes to VATSIM (%d)\n", currentApp()); ++failures; }
                tp(130, 150);   // Stats: a web page in the VATSIM group
                if (currentApp() != APP_BROWSER) { std::printf("FAIL: the VATSIM app's Stats bubble\n"); ++failures; }
                tp(40, 50);
                if (currentApp() != APP_VATSIMHUB) { std::printf("FAIL: Back from VATSIM's Stats goes to VATSIM (%d)\n", currentApp()); ++failures; }
                tp(40, 50);
                if (currentApp() != APP_HOME) { std::printf("FAIL: Back from VATSIM goes Home (%d)\n", currentApp()); ++failures; }
            }
            settings().installed = keepInst;
        }
        {   // ACARS: Hoppie's answers read, the URL made; the page logs on, asks for a PDC, answers a climb with WILCO
            std::vector<AcarsMsg> got; std::string err;
            if (!acarsParse("ok {KSBN telex {PDC AAV12 CLRD TO KDTW}} {KSBN cpdlc {/data2/7//WU/CLIMB TO @FL240@}}", got, err) || got.size() != 2 || got[0].text != "PDC AAV12 CLRD TO KDTW"
                || got[1].min != 7 || got[1].ra != "WU" || got[1].text != "CLIMB TO FL240") { std::printf("FAIL: ACARS poll answer read (%d)\n", (int)got.size()); ++failures; }
            got.clear(); if (acarsParse("error {illegal logon code}", got, err) || err != "illegal logon code") { std::printf("FAIL: ACARS error read: %s\n", err.c_str()); ++failures; }
            if (acarsUrl("ab c", "AAV12", "KSBN", "telex", "HI/THERE") != "https://www.hoppie.nl/acars/system/connect.html?logon=ab+c&from=AAV12&to=KSBN&type=telex&packet=HI%2FTHERE") { std::printf("FAIL: ACARS url\n"); ++failures; }
            if (cpdlcPacket(3, 7, "N", "WILCO") != "/data2/3/7/N/WILCO" || cpdlcPacket(4, 0, "Y", "REQUEST LOGON") != "/data2/4//Y/REQUEST LOGON") { std::printf("FAIL: CPDLC packet\n"); ++failures; }
            const auto keepInst = settings().installed; settings().installed.push_back("acars"); settings().hoppieCode = "secret";
            SimState u = t; u.plan.loaded = true; u.plan.callsign = "AAV12"; u.plan.origin = "KSBN"; u.plan.dest = "KDTW"; u.acfIcao = "A20N"; u.map.utc = 1763640000;
            std::vector<Actions::AcarsOut> sent; std::string from;
            auto fr = [&](float x, float y, bool d) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); for (auto& o : act.acarsOut) sent.push_back(o); from = act.acarsFrom; };
            auto tap = [&](float x, float y) { fr(x, y, false); fr(x, y, true); fr(x, y, true); fr(x, y, false); };
            openApp(APP_ACARS); fr(-100, -100, false);
            if (from != "AAV12") { std::printf("FAIL: ACARS polls as the plan's callsign (%s)\n", from.c_str()); ++failures; }
            tap(200, 154); for (char c : std::string("ksbn")) tabletKey(c, true); tabletKey(13, false);   // the station field, typed (Enter puts the keys away)
            fr(-100, -100, false);
            tap(330, 154);                                     // CPDLC Logon
            tap(700, 154);                                     // Request PDC
            if (sent.size() != 2 || sent[0].to != "KSBN" || sent[0].packet.find("/Y/REQUEST LOGON") == std::string::npos || sent[1].packet.find("REQUEST PREDEP CLEARANCE AAV12 A20N TO KDTW AT KSBN") != 0) {
                std::printf("FAIL: ACARS logon and PDC sent (%d)\n", (int)sent.size()); for (auto& o : sent) std::printf("  %s %s %s\n", o.to.c_str(), o.type.c_str(), o.packet.c_str()); ++failures; }
            AcarsMsg acc; acc.station = "KSBN"; acc.type = "cpdlc"; acc.text = "LOGON ACCEPTED"; acc.ra = "NE"; acc.min = 1; u.acars.inbox.push_back(acc);
            AcarsMsg pdc; pdc.station = "KSBN"; pdc.type = "telex"; pdc.text = "PDC AAV12 CLRD TO KDTW VIA GIJ THEN AS FILED. SQUAWK 4521"; u.acars.inbox.push_back(pdc);
            AcarsMsg clb; clb.station = "KSBN"; clb.type = "cpdlc"; clb.text = "CLIMB TO AND MAINTAIN FL240"; clb.ra = "WU"; clb.min = 9; u.acars.inbox.push_back(clb);
            u.acars.status = "Connected, checked every minute";
            fr(-100, -100, false); fr(-100, -100, false);
            shot(u, APP_ACARS, "acars");
            sent.clear();
            tap(550, 291);   // WILCO, on the climb (the newest message, at the top)
            if (sent.size() != 1 || sent[0].packet.find("/9/N/WILCO") == std::string::npos) { std::printf("FAIL: ACARS WILCO (%d %s)\n", (int)sent.size(), sent.empty() ? "" : sent[0].packet.c_str()); ++failures; }
            fr(-100, -100, false); shot(u, APP_ACARS, "acars_wilco");
            settings().installed = keepInst; settings().hoppieCode.clear();
        }
        {   // the plan's procedures from a CIFP file: KSBN's SID by its runway and the route's transition, KORD's STAR and ILS 28C to the runway
            FlightPlan p; p.loaded = true; p.origin = "KSBN"; p.originRunway = "27"; p.sid = "SBN2"; p.dest = "KORD"; p.destRunway = "28C"; p.star = "WATSN4";
            struct Fx { const char* id; double lat, lon; };
            static const Fx fixes[] = {{"KSBN", 41.7087, -86.3173}, {"KORD", 41.9786, -87.9048}, {"SBN", 41.62, -86.36}, {"ZEGEN", 41.74, -86.52}, {"GIJ", 41.7685, -86.3186},
                {"PMM", 42.4644, -86.1059}, {"OBK", 42.2206, -87.952}, {"WATSN", 42.05, -87.7}, {"VEECK", 42.03, -87.48}, {"FYTTE", 42.1, -87.6}, {"DEGEE", 41.9893, -87.602},
                {"KURKK", 41.9876, -87.713}, {"ROWLS", 41.9866, -87.772}};
            for (const char* id : {"KSBN", "GIJ", "PMM", "OBK", "KORD"}) for (const Fx& f : fixes) if (!std::strcmp(f.id, id)) { FlightPlan::Leg l; l.ident = id; l.lat = f.lat; l.lon = f.lon; p.legs.push_back(l); }
            auto cifp = [](const std::string& icao) -> std::string {
                if (icao == "KSBN") return "RWY:RW27,      ,      ,00799, ,ISBN,1,   ;N41421140,W086181290,0000;\nRWY:RW09,      ,      ,00799, ,     , ,   ;N41421140,W086200000,0000;\n"
                    "SID:010,1,SBN2,RW09,,,,,VA\nSID:015,1,SBN2,RW09,ROWLS,K5,P,C,TF\nSID:010,1,SBN2,RW27,,,,,VA\nSID:020,1,SBN2,RW27,ZEGEN,K5,P,C,TF\nSID:030,2,SBN2,ALL,SBN,K5,D,,TF\nSID:040,3,SBN2,GIJ,GIJ,K5,D,,TF\nSID:050,3,SBN2,PMM,PMM,K5,D,,TF\n";
                if (icao == "KORD") return "RWY:RW28C,      ,      ,00668, ,IORD,1,   ;N41591350,W087515750,0000;\n"
                    "STAR:010,4,WATSN4,OBK,OBK,K5,D,,IF\nSTAR:020,4,WATSN4,OBK,WATSN,K5,P,C,TF\nSTAR:030,5,WATSN4,ALL,FYTTE,K5,P,C,TF\nSTAR:040,6,WATSN4,RW28C,VEECK,K5,P,C,TF\n"
                    "APPCH:005,A,R28C,VEECK,VEECK,K5,P,C,IF\nAPPCH:006,R,R28C,,KURKK,K5,P,C,IF\nAPPCH:010,A,I28C,VEECK,VEECK,K5,P,C,IF\nAPPCH:020,I,I28C,,DEGEE,K5,P,C,IF\nAPPCH:030,I,I28C,,KURKK,K5,P,C,TF\nAPPCH:040,I,I28C,,RW28C,K5,P,G,TF\nAPPCH:050,I,I28C,,ROWLS,K5,P,C,CA\n";
                return "";
            };
            auto fix = [&](const std::string& id, double, double, double& la, double& lo) { for (const Fx& f : fixes) if (id == f.id) { la = f.lat; lo = f.lon; return true; } return false; };
            const auto pr = buildProcedures(p, cifp, fix);
            auto ids = [](const MapState::Procedure& q) { std::string r; for (const auto& x : q.pts) r += (r.empty() ? "" : " ") + x.ident; return r; };
            if (pr.size() != 3) { std::printf("FAIL: procedures: %d lines\n", (int)pr.size()); ++failures; }
            else {
                if (ids(pr[0]) != "RW27 ZEGEN SBN GIJ" || pr[0].name != "SBN2 RW27") { std::printf("FAIL: the SID: %s (%s)\n", ids(pr[0]).c_str(), pr[0].name.c_str()); ++failures; }
                if (ids(pr[1]) != "OBK WATSN FYTTE VEECK") { std::printf("FAIL: the STAR: %s\n", ids(pr[1]).c_str()); ++failures; }
                if (ids(pr[2]) != "DEGEE KURKK RW28C" || pr[2].name != "ILS 28C") { std::printf("FAIL: the approach: %s (%s)\n", ids(pr[2]).c_str(), pr[2].name.c_str()); ++failures; }
                if (std::fabs(pr[0].pts[0].lat - (41 + 42 / 60.0 + 11.40 / 3600)) > 1e-6 || std::fabs(pr[0].pts[0].lon + (86 + 18 / 60.0 + 12.90 / 3600)) > 1e-6) { std::printf("FAIL: the runway's threshold from its RWY record\n"); ++failures; }
            }
            g_testPlan = p; g_testProcs = pr;   // (pictured on the map with the map tests, below)
        }
        {   // the logbook: a flight found by itself (block out, takeoff, a landing, block in), saved and read back; the Timers page
            logbookFromText(std::string()); const auto keepInst = settings().installed; settings().installed.push_back("timers"); settings().installed.push_back("logbook");
            auto run = [&](SimState u, float secs) { for (float tt = 0; tt < secs; tt += 0.25f) { u.map.utc += 0; io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.25f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                ImGui::NewFrame(); Actions act; build(fonts, u, 800, kLogicalH, act); ImGui::Render(); } };
            SimState u = t; u.map.havePos = true; u.map.lat = 41.7; u.map.lon = -86.3; u.plan.loaded = true; u.plan.origin = "KSBN"; u.plan.dest = "KDTW"; u.plan.cruise = "FL340"; u.plan.callsign = "AAV12";
            { FlightPlan::Leg d; d.lat = 42.21; d.lon = -83.35; u.plan.legs.push_back(d); } u.map.utc = 1763640000; u.acfIcao = "A20N"; u.fuelKg = 6000;
            u.ground.enginesRunning = true; u.map.onGround = true; u.map.gsKt = 12; run(u, 3);
            u.map.utc += 600; u.map.onGround = false; u.map.gsKt = 160; u.map.vsFpm = 1800; run(u, 4);
            openApp(APP_TIMERS); u.map.utc += 3000; u.map.vsFpm = -180; u.map.altFt = 34000; shot(u, APP_TIMERS, "timers");
            u.map.utc += 1200; u.map.onGround = true; u.map.gsKt = 130; u.fuelKg = 3400; run(u, 3);
            shot(u, APP_LOGBOOK, "logbook_live");
            u.map.utc += 420; u.map.gsKt = 0; u.ground.enginesRunning = false; run(u, 6);
            const std::string lt = logbookToText();
            if (lt.find("flight\t") != 0 || lt.find("A20N") == std::string::npos || lt.find("\t-180\t2600\t") == std::string::npos) { std::printf("FAIL: the logbook's flight: %s\n", lt.c_str()); ++failures; }
            if (!takeLogbookChanged()) { std::printf("FAIL: a logged flight is saved\n"); ++failures; }
            logbookFromText(lt + lt); if (logbookToText() != lt + lt) { std::printf("FAIL: the logbook read back\n"); ++failures; }
            shot(u, APP_LOGBOOK, "logbook");
            logbookFromText(std::string()); settings().installed = keepInst;
        }
        {   // each game, opened from its card (nothing drawn over the board)
            auto tp = [&](float x, float y) { for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1);
                ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); } };
            tp(100, 200); shot(t, APP_GAMES, "games_2048");
            tp(60, 100); tp(300, 200); tp(200, 300); shot(t, APP_GAMES, "games_ttt");
            tp(60, 100);
            {   // the arcade games: each from its card, played a little (keys held and pressed), its page
                static const float cx[6] = {500, 690, 100, 300, 500, 690}, cy[6] = {200, 200, 380, 380, 380, 380};
                static const char* nm[6] = {"games_contrail", "games_taxirush", "games_skypatrol", "games_hangarclimb", "games_cockpit21", "games_cargoload"};
                for (int g = 0; g < 6; ++g) {
                    tp(cx[g], cy[g]);
                    tp(200, 250);   // (the field pressed: it has the keys)
                    if (!gameWantsKeys()) { std::printf("FAIL: %s's field takes the keys when pressed\n", nm[g]); ++failures; }
                    gameKeyEvent(g == 0 ? 3 : 2, true); gameKeyEvent(g == 0 ? 3 : 2, false);
                    gameKeyEvent(1, true);
                    for (int k = 0; k < 40; ++k) { if (k % 9 == 0) { gameKeyEvent(4, true); } if (k % 9 == 4) gameKeyEvent(4, false);
                        io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); io.AddMouseButtonEvent(0, false); ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); }
                    gameKeyEvent(1, false); gameKeyEvent(4, false);
                    if (g == 5) {   // Cargo Load: a container dragged from the first slot into the hold
                        const int before = xpt::arcade::score(5);
                        auto fr2 = [&](float x, float y, bool d) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d); ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); };
                        fr2(433, 202, false); fr2(433, 202, true);
                        for (int k = 1; k <= 12; ++k) fr2(433 + (169 - 433) * k / 12.0f, 202 + (290 - 202) * k / 12.0f, true);
                        fr2(169, 290, false); fr2(-1000, -1000, false);
                        if (xpt::arcade::score(5) <= before) { std::printf("FAIL: Cargo Load: a container dragged into the hold\n"); ++failures; }
                    }
                    shot(t, APP_GAMES, nm[g]);
                    tp(60, 100);
                }
            }
        }
        {   // the pad with one pointer (a mouse, a finger, a paired device's): a direction tapped keeps going, tapped again it stops; Sky Patrol fires by itself
            auto fr = [&](float x, float y, bool d) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d); ImGui::NewFrame(); Actions act; build(fonts, t, 800, kLogicalH, act); ImGui::Render(); };
            auto tapAt = [&](float x, float y) { fr(x, y, false); fr(x, y, true); fr(x, y, false); fr(-1000, -1000, false); };
            auto run = [&](int n) { for (int k = 0; k < n; ++k) fr(-1000, -1000, false); };
            auto at = []() { return xpt::arcade::playerX(xpt::arcade::SKY_PATROL); };
            openApp(APP_GAMES); run(4);
            tapAt(100, 380); run(3);                                     // Sky Patrol's card
            if (!arcadeOnScreen()) { std::printf("FAIL: (test) Sky Patrol is open\n"); ++failures; }
            tapAt(754, 414); run(6);                                     // Fire: the game begins
            tapAt(576, 396); run(1);                                     // Left tapped once: it keeps flying left
            const float l0 = at(); run(12);
            const float l1 = at(); run(12);
            const float l2 = at();
            if (!(l1 < l0 - 1) || !(l2 < l1 - 1 || l2 <= 12)) { std::printf("FAIL: a direction tapped on the pad keeps going (%g, %g, %g)\n", l0, l1, l2); ++failures; }
            {   // (pictured as it stands: the arrow that keeps going is lit)
                std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_games_pad_latched.png");
            }
            tapAt(576, 396);                                             // Left again: it stops
            const float s0 = at(); run(12);
            if (std::fabs(at() - s0) > 0.01f) { std::printf("FAIL: the direction tapped again stops\n"); ++failures; }
            tapAt(664, 396); run(1);                                     // Right: the other way; and at the games' eased pace
            const float r0 = at(); run(10);
            const float moved = at() - r0;                               // (Sky Patrol's ship was written to cross 150 units a second: 10 frames at 30 a second would be 50)
            if (!(moved > 5 && moved < 38)) { std::printf("FAIL: the games run at their eased pace (%g in a third of a second)\n", moved); ++failures; }
            tapAt(664, 396); run(2);
            tapAt(60, 100); run(2);                                      // All Games
            openApp(APP_HOME);
        }
        { SimState nv = t;
          nv.navHits = {{4, "GIJ", "GIPPER VORTAC", 41.77, -86.32, 11580}, {2, "SB", "SOUTH BEND NDB", 41.6, -86.2, 356}, {512, "SBNAP", "SBNAP", 41.5, -86.4, 0}};
          shot(nv, APP_NAVSEARCH, "navsearch"); }
        shot(t, APP_VATSIMHUB, "vatsimhub");
        {
            const Settings keep = settings();
            settings().installed = {"datis", "games", "spotify", "youtube", "nattrak"};
            settings().folders = {{"Ops", {APP_NOTES, APP_DOCUMENTS, APP_CHECKLISTS}}};
            settings().order = {kWidget0 + W_WEATHER, -1, kWidget0 + W_FLIGHT, -1, -1, -1, kWidget0 + W_FPS, -1, -1, -1, -1, -1, -1, kFolder0};
            SimState hw = t; hw.fps = 47; hw.media.supported = true; hw.media.active = true; hw.media.title = "Night Flight"; hw.media.artist = "The Cruise"; hw.media.playing = true;
            layoutHome();
            if (settings().homePages < 2) { std::printf("FAIL: widgets push the icons onto a second page (%d)\n", settings().homePages); ++failures; }
            shot(hw, APP_HOME, "home_widgets");
            {   // a tap on the folder opens it; a tap beside it closes it; a swipe shows the next page
                auto fr = [&](float x, float y, bool down) {
                    io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down);
                    ImGui::NewFrame(); Actions act; build(fonts, hw, 800, kLogicalH, act); ImGui::Render(); };
                fr(712, 175, false); fr(712, 175, true); fr(712, 175, false);
                shot(hw, APP_HOME, "home_folder");
                // its name: tapped, typed into
                for (int k = 0; k < 3; ++k) fr(400, 178, k == 1);
                if (!tabletTyping()) { std::printf("FAIL: a folder's name can be tapped to rename it\n"); ++failures; }
                else { tabletKey(8, false); tabletKey(8, false); tabletKey(8, false); for (char c : std::string("Docs")) tabletKey(c, true); }
                fr(-1000, -1000, false);
                if (settings().folders.empty() || settings().folders[0].name != "Docs") { std::printf("FAIL: the folder renamed (%s)\n", settings().folders.empty() ? "-" : settings().folders[0].name.c_str()); ++failures; }
                {   // an app dragged out of the open folder lands on the page; the folder dragged to the bin goes, its apps onto the page
                    fr(712, 175, false); fr(712, 175, true); fr(712, 175, false);   // open it again
                    for (int k = 0; k < 8; ++k) fr(-1000, -1000, false);
                    const int app = settings().folders[0].apps[0];
                    fr(250, 248, false); fr(250, 248, true);
                    for (int k = 1; k <= 12; ++k) fr(250 + k * 30.0f, 248 + k * 2.0f, true);
                    fr(610, 272, true); fr(610, 272, false); fr(-1000, -1000, false);
                    const bool out = std::find(settings().order.begin(), settings().order.end(), app) != settings().order.end();
                    if (settings().folders[0].apps.size() != 2 || !out) { std::printf("FAIL: an app dragged out of a folder (%zu left, placed %d)\n", settings().folders[0].apps.size(), out ? 1 : 0); ++failures; }
                    const std::vector<int> rest = settings().folders[0].apps;
                    fr(712, 175, false); fr(712, 175, true);
                    for (int k = 1; k <= 14; ++k) fr(712 - k * 47.0f, 175 + k * 20.0f, true);
                    fr(56, 455, true); fr(56, 455, true); fr(56, 455, false); fr(-1000, -1000, false);
                    bool placed = true;
                    for (int a2 : rest) placed = placed && std::find(settings().order.begin(), settings().order.end(), a2) != settings().order.end();
                    if (!settings().folders.empty() || !placed) { std::printf("FAIL: a folder in the bin unpacks onto the page (%zu folders, placed %d)\n", settings().folders.size(), placed ? 1 : 0); ++failures; }
                    settings().folders = {{"Docs", rest}};
                    for (int& x : settings().order) if (std::find(rest.begin(), rest.end(), x) != rest.end()) x = -1;
                    layoutHome();
                    fr(752, 452, false); fr(752, 452, true); fr(752, 452, false);   // Done
                }
                fr(40, 100, false); fr(40, 100, true); fr(40, 100, false);
                fr(600, 300, false); fr(600, 300, true);
                for (int k = 1; k <= 10; ++k) fr(600 - k * 40.0f, 300, true);
                fr(200, 300, false);
                shot(hw, APP_HOME, "home_page2");
                for (int k = 0; k <= 10; ++k) fr(200 + k * 40.0f, 300, k < 10);   // and back
            }
            {   // the folders and widgets round-trip
                const std::string txt = settingsToText();
                const Settings before = settings();
                settings() = Settings(); settingsFromText(txt);
                if (settings().folders.size() != 1 || settings().folders[0].name != "Docs" || settings().folders[0].apps.size() != before.folders[0].apps.size() || settings().order != before.order || settings().homePages != before.homePages)
                    { std::printf("FAIL: folders and widgets round trip\n"); ++failures; }
            }
            settings().order.push_back(kWidget0 + W_MEDIA);
            layoutHome();
            shot(hw, APP_HOME, "home_widgets2");
            {   // the favorites bar: seven places; Frame Rate takes one, Volanta's overlay all seven
                settings().favoriteSlots = 7;
                settings().favorites = {APP_MAP, APP_AIRPORTS, kWidget0 + W_FPS, APP_ROUTES, APP_GROUND, APP_CHECKLISTS, APP_NOTES};
                layoutHome();
                const auto& fv = settings().favorites;
                if (fv.size() != 7 || std::find(fv.begin(), fv.end(), kWidget0 + W_FPS) == fv.end()) { std::printf("FAIL: Frame Rate in a 7-place favorites bar (%zu)\n", fv.size()); ++failures; }
                for (int x : settings().order) if (x == kWidget0 + W_FPS) { std::printf("FAIL: Frame Rate both in the bar and on a page\n"); ++failures; }
                shot(hw, APP_HOME, "home_dock_fps");
                settings().favorites = {kWidget0 + W_VOLANTA};
                layoutHome();
                shot(hw, APP_HOME, "home_dock_volanta");
                settings().favorites = {kWidget0 + W_VOLANTA, APP_MAP};   // (no room for the Map beside it: to a page)
                layoutHome();
                if (settings().favorites.size() != 1) { std::printf("FAIL: the bar holds more than its seven places\n"); ++failures; }
                settings().favoriteSlots = 3; layoutHome();   // (too narrow for Volanta's overlay: it goes to a page)
                bool onPage = false; for (int x : settings().order) if (x == kWidget0 + W_VOLANTA) onPage = true;
                if (!settings().favorites.empty() || !onPage) { std::printf("FAIL: Volanta's overlay leaves a 3-place bar for a page\n"); ++failures; }
            }
            {   // Add Widget: at the top of the page, the icons there pushed down (and on), none lost
                settings() = keep; layoutHome();
                std::vector<int> before; for (int x : settings().order) if (x >= 0 && x < APP_COUNT) before.push_back(x);
                const int cap = (int)settings().order.size() / std::max(1, settings().homePages);
                for (int& x : settings().order) if (x == kWidget0 + W_WEATHER) x = -1;
                pushInWidget(kWidget0 + W_WEATHER, 0, cap); layoutHome();
                std::vector<int> after; for (int x : settings().order) if (x >= 0 && x < APP_COUNT) after.push_back(x);
                if (settings().order[0] != kWidget0 + W_WEATHER) { std::printf("FAIL: Add Widget puts it at the top of the page\n"); ++failures; }
                std::vector<int> b2 = before, a2 = after; std::sort(b2.begin(), b2.end()); std::sort(a2.begin(), a2.end());
                if (b2 != a2) { std::printf("FAIL: Add Widget lost or doubled an app (%zu, %zu)\n", before.size(), after.size()); ++failures; }
                if (!before.empty() && !after.empty() && before.front() != after.front()) { std::printf("FAIL: Add Widget keeps the icons in their order\n"); ++failures; }
                shot(hw, APP_HOME, "home_widget_pushed");
            }
            settings() = keep;
            layoutHome();
        }
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
            mp.map.airportPoints = {{"KXPT", 41.70, -86.32, 1, "", 9000}, {"KSBN", 41.7087, -86.3173, 1, "", 8412}, {"KGSH", 41.5264, -85.7922, 1, "", 5500}, {"KEKM", 41.7194, -86.0003, 1, "", 6500}};
            settings().mapAirports = true; settings().mapSource = 0; settings().mapLocalTime = true;
            shot(mp, APP_MAP, "map_airport");
            {   // as the emulator gives it: airports as dots only (no layouts), tapped for their card
                SimState em = mp; em.map.airports.clear(); em.map.lat = 41.75; em.map.lon = -86.40;
                em.map.airportPoints = {{"KSBN", 41.7087, -86.3173, 1, "South Bend Intl", 8500}, {"KORD", 41.9786, -87.9048, 1, "Chicago O'Hare Intl", 13000}};
                std::string got;
                auto tapAt = [&](float x, float y) {
                    const bool downs[5] = {false, true, true, false, false};
                    for (bool d : downs) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, d);
                        ImGui::NewFrame(); Actions act; build(fonts, em, 800, kLogicalH, act); ImGui::Render(); if (!act.airportOpen.empty()) got = act.airportOpen; }
                };
                openApp(APP_MAP); for (int i = 0; i < 9; ++i) mapCommand(1); shot(em, APP_MAP, "map_airport_dots");
                float fx = -1, fy = -1;
                for (float yy = 150; yy < 400 && got.empty(); yy += 5) for (float xx = 280; xx < 560 && got.empty(); xx += 5) { tapAt(xx, yy); if (!got.empty()) { fx = xx; fy = yy; } }
                if (got.empty()) { std::printf("FAIL: an airport's dot on the map cannot be tapped\n"); ++failures; }
                else {
                    shot(em, APP_MAP, "map_airport_dot_card");
                    // by finger: one tap on the dot opens nothing, a double tap opens its card
                    mapSelect(0, ""); setTouchInput(true); got.clear();
                    for (int k = 0; k < 10; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.1f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, em, 800, kLogicalH, act); ImGui::Render(); }   // (no double tap left over)
                    got.clear(); tapAt(fx, fy);
                    if (!got.empty()) { std::printf("FAIL: one finger tap on the map opened a card\n"); ++failures; }
                    tapAt(fx, fy);
                    if (got.empty()) { std::printf("FAIL: a finger double tap on the map opened no card\n"); ++failures; }
                    setTouchInput(false);
                }
                {   // its VATSIM ATC badge: tapped, the card (with the controllers)
                    mapSelect(0, ""); got.clear();
                    em.vatsimAtc = {{"SBN_TWR", "118.950", "South Bend Tower", "", 4}, {"KSBN_ATIS", "135.200", "", "INFO B", 7}}; em.vatsimAtcHave = true;
                    const bool was = settings().mapVatsimAtc; settings().mapVatsimAtc = true;
                    for (float yy = 150; yy < 400 && got.empty(); yy += 4) for (float xx = 280; xx < 600 && got.empty(); xx += 4) {
                        tapAt(xx, yy); if (!got.empty() && got != "KSBN") got.clear();
                    }
                    if (got.empty()) { std::printf("FAIL: no KSBN card from the map\n"); ++failures; }
                    else shot(em, APP_MAP, "map_airport_atc_card");
                    settings().mapVatsimAtc = was;
                }
                mapSelect(0, ""); for (int i = 0; i < 9; ++i) mapCommand(0);
                {   // the plan's SID, STAR and ILS (built above) on the map, at 60 nm
                    SimState u = em; u.plan = g_testPlan; planToRoute(u.plan, u.map); u.map.procedures = g_testProcs; u.map.lat = 41.95; u.map.lon = -87.2;
                    for (int k = 0; k < 30; ++k) mapCommand(1);   // (the farthest out, as the map was here) then in to 60 nm
                    for (int k = 0; k < 7; ++k) mapCommand(0);
                    mapCommand(2); shot(u, APP_MAP, "map_procedures");
                    for (int k = 0; k < 7; ++k) mapCommand(1);
                }
            }
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
            mapCommand(1); mapCommand(1); mapCommand(1); mapCommand(1);   // 5 nm (1, 1.5, 2, 3, 5)
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
                {   // a traffic advisory from 2 nm (1.8 nm out, level, not closing fast); none on the ground
                    SimState ta = ra;
                    ta.map.traffic = {{41.70 + 1.8 / 60.0, -86.30, 3300, 0, 140, 0, false, "N1TA", "C172"}};
                    if (trafficThreat(ta.map, ta.map.traffic[0]) != 1) { std::printf("FAIL: TCAS TA at 2 nm (%d)\n", trafficThreat(ta.map, ta.map.traffic[0])); ++failures; }
                    ta.map.onGround = true; ta.map.aglFt = 0;
                    if (trafficThreat(ta.map, ta.map.traffic[0]) != 0) { std::printf("FAIL: no TCAS on the ground\n"); ++failures; }
                }
                {   // a runway collision: we are on runway 27 and traffic is 2 nm out on final for it; then we are on short final and it is on the runway
                    SimState rc = mp;
                    rc.map.lat = 41.70; rc.map.lon = -86.32; rc.map.onGround = true; rc.map.aglFt = 0; rc.map.altFt = 800; rc.map.gsKt = 0; rc.map.hdgTrue = 270;
                    rc.map.traffic = {{41.70, -86.26, 1400, 270, 140, -700, false, "N2FIN", "B738"}};
                    { AptGeo g; AptGeo::Runway r; r.a = mercOf(41.70, -86.335); r.b = mercOf(41.70, -86.305); r.widthM = 45.72; r.idA = "09"; r.idB = "27"; g.runways.push_back(r);
                      rc.map.airports = {std::make_shared<AptGeo>(g)}; }
                    if (trafficThreat(rc.map, rc.map.traffic[0]) != 3) { std::printf("FAIL: runway collision, traffic landing on our runway\n"); ++failures; }
                    rc.map.traffic[0].hdg = 90;   // (going away from it: none)
                    if (trafficThreat(rc.map, rc.map.traffic[0]) == 3) { std::printf("FAIL: runway collision, traffic leaving\n"); ++failures; }
                    rc.map.lon = -86.30; rc.map.onGround = false; rc.map.aglFt = 150; rc.map.altFt = 950; rc.map.vsFpm = -700; rc.map.gsKt = 140;
                    rc.map.traffic = {{41.70, -86.32, 800, 90, 0, 0, true, "N3RWY", "C172"}};
                    if (trafficThreat(rc.map, rc.map.traffic[0]) != 3) { std::printf("FAIL: runway collision, landing on an occupied runway\n"); ++failures; }
                }
                shot(ra, APP_MAP, "map_traffic_ra");
                // the map's menus: Navigation, then Traffic (its source, range and height); the night side of the day; no map under it
                tap(mp, 60, 74 + 46 + 15); shot(mp, APP_MAP, "map_menu_navigation");
                tap(mp, 60, 74 + 46 + 36 + 15); shot(mp, APP_MAP, "map_menu_traffic");
                tap(mp, 700, 300);
                { SimState dn = mp; dn.map.utc = 1795060800LL; settings().mapDaylight = true; settings().mapSource = 2; for (int k = 0; k < 6; ++k) mapCommand(1);
                  shot(dn, APP_MAP, "map_daylight_nomap"); for (int k = 0; k < 6; ++k) mapCommand(0); settings().mapDaylight = false; settings().mapSource = 0; }
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
            {   // on the ground at the origin: "Welcome to" it, the figures at the left
                SimState wf = fp; wf.map.havePos = true; wf.map.onGround = true;
                wf.map.lat = wf.plan.legs.front().lat + 0.004; wf.map.lon = wf.plan.legs.front().lon; wf.map.hdgTrue = 270;
                shot(wf, APP_ROUTES, "flightplan_welcome");
                {   // the aircraft at either end of the progress line stays clear of the airports' names (the Christmas sleigh too)
                    const int keepWp = settings().wallpaper;
                    const double ll[6][2] = {{41.7087, -86.3173}, {41.7685, -86.3186}, {42.4644, -86.1059}, {42.2206, -87.9520}, {42.05, -87.70}, {41.9786, -87.9048}};
                    SimState wo = wf; for (int i = 0; i < 6; ++i) { wo.plan.legs[i].lat = ll[i][0]; wo.plan.legs[i].lon = ll[i][1]; }
                    wo.map.onGround = false; wo.map.lat = ll[0][0]; wo.map.lon = ll[0][1];
                    SimState we = wo; we.map.lat = ll[5][0]; we.map.lon = ll[5][1];
                    shot(wo, APP_ROUTES, "progress_origin"); shot(we, APP_ROUTES, "progress_dest");
                    setSeasonalPreview(true); settings().wallpaper = 10 + 13;
                    shot(wo, APP_ROUTES, "progress_christmas_origin"); shot(we, APP_ROUTES, "progress_christmas_dest");
                    setSeasonalPreview(false); settings().wallpaper = keepWp;
                }
            }
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
            {   // the seasonal themes: each on its own dates (2026: MLK Jan 19, Presidents' Feb 16, Easter Apr 5, Memorial May 25, Labor Sep 7,
                // Thanksgiving Nov 26); all of them offered in the emulator
                struct D { int k, y, m, d; bool on; };
                const D ds[] = {{0, 2026, 12, 31, true}, {0, 2027, 1, 2, true}, {0, 2027, 1, 3, false}, {1, 2026, 1, 19, true}, {1, 2026, 1, 20, false},
                    {2, 2026, 2, 14, true}, {2, 2026, 2, 15, false}, {3, 2026, 2, 16, true}, {4, 2026, 3, 17, true}, {5, 2026, 4, 5, true}, {5, 2026, 3, 29, true},
                    {5, 2026, 4, 7, false}, {6, 2026, 5, 25, true}, {6, 2026, 5, 22, false}, {7, 2026, 6, 19, true}, {8, 2026, 7, 4, true}, {9, 2026, 9, 7, true},
                    {9, 2026, 9, 8, false}, {10, 2026, 10, 31, true}, {10, 2026, 11, 1, false}, {11, 2026, 11, 11, true}, {12, 2026, 11, 26, true}, {12, 2026, 11, 20, false},
                    {13, 2026, 12, 1, false}, {13, 2026, 12, 23, false}, {13, 2026, 12, 24, true}, {13, 2026, 12, 25, true}, {13, 2026, 12, 26, false}, {14, 2026, 6, 1, true}, {14, 2026, 7, 1, false}, {15, 2026, 10, 15, true}, {15, 2026, 11, 1, false}};
                {   // the four seasons, by the calendar's months: one is always on, never two
                    static const D ss[] = {{16, 2027, 3, 1, true}, {16, 2027, 5, 31, true}, {16, 2027, 6, 1, false}, {17, 2027, 6, 1, true}, {17, 2027, 8, 31, true}, {17, 2027, 9, 1, false},
                                           {18, 2026, 11, 21, true}, {18, 2026, 12, 1, false}, {19, 2026, 12, 1, true}, {19, 2027, 1, 15, true}, {19, 2027, 2, 28, true}, {19, 2027, 3, 1, false}};
                    for (const D& d : ss) if (seasonalOnDate(d.k, d.y, d.m, d.d) != d.on) { std::printf("FAIL: %s on %d-%02d-%02d should be %s\n", seasonalName(d.k), d.y, d.m, d.d, d.on ? "on" : "off"); ++failures; }
                    for (int m = 1; m <= 12; ++m) { int on = 0; for (int k = 16; k < 20; ++k) on += seasonalOnDate(k, 2027, m, 15) ? 1 : 0; if (on != 1) { std::printf("FAIL: exactly one season in month %d (%d)\n", m, on); ++failures; } }
                }
                for (const D& d : ds) if (seasonalOnDate(d.k, d.y, d.m, d.d) != d.on) { std::printf("FAIL: %s on %d-%02d-%02d should be %s\n", seasonalName(d.k), d.y, d.m, d.d, d.on ? "on" : "off"); ++failures; }
                setSeasonalPreview(true);
                shot(st, APP_SETTINGS, "settings_display_seasonal");
                {   // more seasonal themes than a row holds: the round arrow under the row turns the page, and back
                    auto pic = [&](const char* nm) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.5f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render();
                        std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + nm); return b.size(); };
                    const int keepW = settings().wallpaper;
                    tap(st, 520, 310);   // (the first theme of the first page: New Year)
                    if (settings().wallpaper != 10 + 0) { std::printf("FAIL: the seasonal row's first page starts at New Year (%d)\n", settings().wallpaper); ++failures; }
                    tap(st, 696, 390); pic("/tablet_settings_seasonal_page2.png");
                    tap(st, 520, 310);
                    if (settings().wallpaper != 10 + 4) { std::printf("FAIL: the arrow under the seasonal row turns to its next page (%d)\n", settings().wallpaper); ++failures; }
                    tap(st, 592, 390); tap(st, 520, 310);
                    if (settings().wallpaper != 10 + 0) { std::printf("FAIL: the other arrow turns the seasonal row back (%d)\n", settings().wallpaper); ++failures; }
                    settings().wallpaper = keepW;
                }
                const int keepWp = settings().wallpaper;
                for (int k = 0; k < 20; ++k) { settings().wallpaper = 10 + k; char nm[32]; std::snprintf(nm, sizeof nm, "home_season%02d", k); shot(st, APP_HOME, nm); }
                settings().wallpaper = 10 + 10; shot(st, APP_HOME, "home_halloween");
                {   // Christmas: the user's aircraft is a sleigh (the Ground page, the map, the route bar); traffic stays as it is
                    settings().wallpaper = 10 + 13;
                    SimState xs = st; xs.map.havePos = true; xs.map.lat = 41.70; xs.map.lon = -86.32; xs.map.hdgTrue = 60; xs.acfIcao = "A20N";
                    shot(xs, APP_GROUND, "christmas_ground");
                    shot(xs, APP_MAP, "christmas_map");
                    settings().wallpaper = 10 + 10;   // Halloween: the witch on her broom
                    shot(xs, APP_GROUND, "halloween_ground");
                    settings().wallpaper = 10 + 12;   // Thanksgiving: a turkey
                    shot(xs, APP_GROUND, "thanksgiving_ground");
                    shot(xs, APP_MAP, "thanksgiving_map");
                    settings().wallpaper = 10 + 8;    // Independence Day: a firework rocket
                    shot(xs, APP_GROUND, "july4_ground");
                    shot(xs, APP_ROUTES, "july4_flight_plan");
                    auto run = [&](float secs) { for (float tt = 0; tt < secs; tt += 0.1f) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.1f; io.AddMousePosEvent(-1000, -1000);
                        ImGui::NewFrame(); Actions act; build(fonts, xs, 800, kLogicalH, act); ImGui::Render(); } };
                    celebrate(0); run(1.6f); shot(xs, APP_HOME, "celebrate_newyear");
                    celebrate(1); run(2.0f); shot(xs, APP_HOME, "celebrate_christmas");
                    // after the show: "from XPTablet" and a Dismiss button; it stays until that is pressed
                    auto frameAt = [&](float x, float y, bool down) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.05f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame(); Actions act; build(fonts, xs, 800, kLogicalH, act); ImGui::Render(); };
                    auto tapAt = [&](float x, float y) { frameAt(x, y, false); frameAt(x, y, true); frameAt(x, y, true); frameAt(x, y, false); frameAt(-1000, -1000, false); };
                    run(6.5f);
                    {   // pictured as it stands (shot() would reopen the page; the celebration is over everything anyway)
                        io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.016f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, xs, 800, kLogicalH, act); ImGui::Render();
                        std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_celebrate_from.png");
                    }
                    run(8.0f);
                    if (!celebrationShown()) { std::printf("FAIL: the celebration stays until it is dismissed\n"); ++failures; }
                    tapAt(400, 200);   // (a press elsewhere does nothing)
                    if (!celebrationShown()) { std::printf("FAIL: only the Dismiss button closes the celebration\n"); ++failures; }
                    tapAt(400, kLogicalH - 60.0f); run(0.3f);
                    if (!celebrationShown()) { std::printf("FAIL: the celebration fades out after Dismiss, not at once\n"); ++failures; }
                    run(1.0f);
                    if (celebrationShown()) { std::printf("FAIL: Dismiss closes the celebration\n"); ++failures; }
                    celebrate(0); run(0.5f); tapAt(400, kLogicalH - 60.0f); run(1.5f);
                    if (!celebrationShown()) { std::printf("FAIL: Dismiss is not there to press before the animation is done\n"); ++failures; }
                    run(7.0f); tapAt(400, kLogicalH - 60.0f); run(1.5f);
                    if (celebrationShown()) { std::printf("FAIL: Dismiss closes the New Year celebration\n"); ++failures; }
                }
                setSeasonalPreview(false);
                settings().wallpaper = 10 + 0; settings().wallpaperBase = 3;
                for (int k = 0; k < 2; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); }
                std::time_t nowT = std::time(nullptr); std::tm lt = *std::localtime(&nowT);
                if (!seasonalOnDate(0, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) && settings().wallpaper != 3) { std::printf("FAIL: a seasonal theme out of season gives way to the one before (%d)\n", settings().wallpaper); ++failures; }
                settings().wallpaper = keepWp;
                openApp(APP_SETTINGS);
            }
            {   // a Custom held: its editor, with the pictures from the backgrounds folder
                const int keepWp = settings().wallpaper;
                SimState sb = st; sb.backgroundFiles = {"aurora.jpg", "my-livery.png"}; sb.backgroundsFolder = "E:/X-Plane 12/Resources/plugins/XPTablet/backgrounds";
                for (int k = 0; k < 45; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(642, 205); io.AddMouseButtonEvent(0, true);
                    ImGui::NewFrame(); Actions act; build(fonts, sb, 800, kLogicalH, act); ImGui::Render(); }
                io.AddMouseButtonEvent(0, false);
                shot(sb, APP_SETTINGS, "settings_custom_theme");
                {   // its wheels can be dragged (the background's colour changes); a press beside the card does not close it
                    const int before = settings().customs[0].bg;
                    for (int k = 0; k < 6; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(472.0f + k * 8, 182); io.AddMouseButtonEvent(0, k < 5);
                        ImGui::NewFrame(); Actions act; build(fonts, sb, 800, kLogicalH, act); ImGui::Render(); }
                    if (settings().customs[0].bg == before) { std::printf("FAIL: the background wheel can be dragged\n"); ++failures; }
                    tap(sb, 20, 300);
                    tap(sb, 20, 300);
                    for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, sb, 800, kLogicalH, act); ImGui::Render(); }
                    shot(sb, APP_SETTINGS, "settings_custom_theme_dragged");
                }
                {   // the accent's hex box: tapped, a code typed sets the colour; a bright one makes the text on it dark
                    tap(sb, 660, 284);
                    for (char ch : std::string("ffe070")) tabletKey(ch, true);
                    for (int k = 0; k < 20; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, sb, 800, kLogicalH, act); ImGui::Render(); }
                    if (!tabletTyping()) { std::printf("FAIL: the hex box takes typing (its keyboard comes up)\n"); ++failures; }
                    {   // as it is while typing: the card lifted, the keyboard under it
                        std::vector<Batch> b;
                        for (int i = 0; i < 12; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.05f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddMouseButtonEvent(0, false);
                            ImGui::NewFrame(); Actions act; build(fonts, sb, 800, kLogicalH, act); ImGui::Render(); b.clear(); clipDrawData(ImGui::GetDrawData(), b); }
                        io.DeltaTime = 1.0f / 60.0f;
                        Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_settings_custom_hex_typing.png");
                    }
                    if (settings().customs[0].accent != 0xFFE070) { std::printf("FAIL: a hex code typed sets the accent (%06X)\n", settings().customs[0].accent); ++failures; }
                    shot(sb, APP_SETTINGS, "settings_custom_hex");
                    tabletKey(13, false);
                }
                if (settings().wallpaper != 7) { std::printf("FAIL: holding Custom 1 chooses it (%d)\n", settings().wallpaper); ++failures; }
                tap(sb, 690, 83);   // Done
                settings().wallpaper = keepWp;
            }
            settings().autoDim = false;
            tap(st, 80, 196);   // Data
            settings().simbriefUser = "alpineaviation";
            st.plan.status = "Imported KSBN to KORD at 05:12Z"; st.plan.loaded = true;
            shot(st, APP_SETTINGS, "settings_data");
            {   // Reset All: tapped twice, every setting back to its default, and the tablet boots
                const Settings keepAll = settings();
                setBattery(100);
                Settings fresh; settings() = fresh; layoutHome(); const std::string defaults = settingsToText();
                settings() = keepAll;
                settings().favoriteSlots = 2; settings().wallpaper = 3; settings().notifyTcas = false; settings().localConnect = true;
                tap(st, 80, 196);   // Data
                bool booted = false;
                for (float yy = 330; yy < 390 && !booted; yy += 4) {
                    tap(st, 713, yy); tap(st, 713, yy);
                    if (settings().favoriteSlots != 2) booted = true;
                }
                if (!booted) { std::printf("FAIL: Reset All (twice) did not reset\n"); ++failures; }
                else {
                    if (settingsToText() != defaults) { std::printf("FAIL: Reset All leaves settings that are not the defaults\n"); ++failures;
                        const std::string now2 = settingsToText(); size_t i = 0; while (i < now2.size() && i < defaults.size() && now2[i] == defaults[i]) ++i;
                        const size_t ls = defaults.rfind('\n', i) + 1; std::printf("  default: %s\n  now:     %s\n", defaults.substr(ls, defaults.find('\n', i) - ls).c_str(), now2.substr(ls, now2.find('\n', i) - ls).c_str()); }
                    if (tabletPoweredOn() && currentApp() != APP_HOME) { std::printf("FAIL: Reset All goes back to the home screen\n"); ++failures; }
                    shot(st, APP_HOME, "reset_all_boot");
                }
                settings() = keepAll; layoutHome();
                for (int k = 0; k < 120 && !(tabletPoweredOn() && currentApp() == APP_HOME && k > 80); ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 0.1f; io.AddMousePosEvent(-1000, -1000);
                    ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); }   // (the boot it started has finished)
                io.DeltaTime = 1.0f / 60.0f;
                openApp(APP_SETTINGS); tap(st, 80, 196);
            }
            settings().simbriefUser.clear();
            tap(st, 80, 280);   // Media
            settings().mediaDuck = 1; settings().duckRx = 0.35f; settings().duckTx = 0.15f;
            shot(st, APP_SETTINGS, "settings_media");
            if (settingsTabShown() != 5) { std::printf("FAIL: the Media tab of Settings was not opened (%d)\n", settingsTabShown()); ++failures; }
            settings().mediaDuck = 0;
            tap(st, 80, 238);   // Notifications
            shot(st, APP_SETTINGS, "settings_notifications");
            if (settingsTabShown() != 4) { std::printf("FAIL: the Notifications tab of Settings was not opened (%d)\n", settingsTabShown()); ++failures; }
            tap(st, 80, 364);   // Accounts
            shot(st, APP_SETTINGS, "settings_accounts");
            if (settingsTabShown() != 7) { std::printf("FAIL: the Accounts tab of Settings was not opened (%d)\n", settingsTabShown()); ++failures; }
            {   // where the host cannot serve Local Connect yet (the plugin today), Settings has no page for it: Accounts takes its place
                SimState nl = st; nl.local.status = -2;
                tap(nl, 80, 196); tap(nl, 80, 322);
                if (settingsTabShown() != 7) { std::printf("FAIL: without Local Connect, Accounts is in its place (%d)\n", settingsTabShown()); ++failures; }
                shot(nl, APP_SETTINGS, "settings_no_local_connect");
                tap(st, 80, 196);
            }
            tap(st, 80, 322);   // Local Connect
            {   // on, an Android phone connected (its battery the tablet's), an iPhone paired before, an iPad asking to pair
                SimState lc = st;
                settings().localConnect = true;
                lc.local.status = 2; lc.local.address = "https://192.168.1.20:8443"; lc.local.pairCode = "482913"; lc.local.pairName = "Dom's iPad";
                LocalDevice p; p.name = "Pixel 8"; p.kind = "Android phone"; p.connected = true; p.batteryPct = 64; p.charging = true;
                LocalDevice i; i.name = "Dom's iPhone"; i.kind = "iPhone"; i.lastSeen = "Today 14:32";
                lc.local.devices = {p, i};
                shot(lc, APP_SETTINGS, "settings_local_connect");
                // the address is dots until its eye is tapped; it stays shown (a toggle, not a hold); tapped again, dots again
                if (settingsReveal() != 0) { std::printf("FAIL: Settings' hidden fields start hidden (%u)\n", settingsReveal()); ++failures; }
                tap(lc, 459, 278);
                for (int k = 0; k < 12; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, lc, 800, kLogicalH, act); ImGui::Render(); }
                if (settingsReveal() != 8u) { std::printf("FAIL: the eye shows Local Connect's address and it stays shown (%u)\n", settingsReveal()); ++failures; }
                {   // (pictured as it stands: shot() would open Settings afresh)
                    std::vector<Batch> b; clipDrawData(ImGui::GetDrawData(), b); Canvas cv((int)(800 * scale), (int)(kLogicalH * scale)); raster(cv, b, tex, tw, th, scale); writePng(cv, out + "/tablet_settings_local_connect_shown.png");
                }
                tap(lc, 459, 278);
                if (settingsReveal() != 0) { std::printf("FAIL: the eye tapped again hides the address (%u)\n", settingsReveal()); ++failures; }
                // the status bar's battery: the phone's 64 %, and back to the tablet's own once it goes
                setBattery(37);
                for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, lc, 800, kLogicalH, act); ImGui::Render(); }
                const std::string t1 = settingsToText();
                if (t1.find("battery=37\n") == std::string::npos) { std::printf("FAIL: the tablet's own battery is kept while a device's shows\n"); ++failures; }
                lc.local.devices.clear();
                for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, lc, 800, kLogicalH, act); ImGui::Render(); }
                setBattery(100);
                settings().localConnect = false;
            }
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
            {   // VATSIM's airspaces: Chicago Center online (green, its card when tapped), Indianapolis quiet
                auto set = std::make_shared<AirspaceSet>();
                AirspaceArea zau; zau.id = "KZAU"; zau.labelLat = 41.9; zau.labelLon = -86.9; zau.rings = {{{43.0f, -89.0f}, {43.0f, -85.8f}, {41.2f, -85.8f}, {41.2f, -89.0f}, {43.0f, -89.0f}}};
                AirspaceArea zid; zid.id = "KZID"; zid.labelLat = 40.2; zid.labelLon = -86.0; zid.rings = {{{41.2f, -88.0f}, {41.2f, -84.0f}, {38.5f, -84.0f}, {38.5f, -88.0f}, {41.2f, -88.0f}}};
                for (AirspaceArea* ar : {&zau, &zid}) {
                    ar->la0 = ar->lo0 = 1e9; ar->la1 = ar->lo1 = -1e9;
                    for (const auto& q : ar->rings[0]) { ar->la0 = std::min(ar->la0, (double)q.first); ar->la1 = std::max(ar->la1, (double)q.first); ar->lo0 = std::min(ar->lo0, (double)q.second); ar->lo1 = std::max(ar->lo1, (double)q.second); }
                }
                set->areas = {zau, zid};
                set->firs = {{"KZAU", "Chicago", "CHI", "KZAU"}, {"KZID", "Indianapolis", "IND", "KZID"}};
                SimState as = mp; as.map.airspaces = set; as.vatsimAtcHave = true;
                as.vatsimAtc = {{"CHI_CTR", "133.200", "Chicago Center", "Chicago Center, radar services, monitor UNICOM in uncontrolled airspace", 6, "14:02Z"}};
                settings().mapAirspaces = true; settings().mapAirports = false; settings().mapTraffic = false;
                for (int k = 0; k < 3; ++k) mapCommand(1);   // out a little: both areas in view
                shot(as, APP_MAP, "map_airspaces");
                if (airspace::firFor(*set, "CHI_CTR") == nullptr || airspace::firFor(*set, "LON_S_CTR") != nullptr) { std::printf("FAIL: a center's call sign finds its FIR\n"); ++failures; }
                for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(330, 170); io.AddMouseButtonEvent(0, k == 1);
                    ImGui::NewFrame(); Actions act; build(fonts, as, 800, kLogicalH, act); ImGui::Render(); }
                shot(as, APP_MAP, "map_airspace_card");
                for (int k = 0; k < 3; ++k) mapCommand(2);
                settings().mapAirspaces = false;
            }
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
            {   // VATSIM's controllers online there (and their headsets)
                SimState av = ap; av.vatsimAtcHave = true;
                av.vatsimAtc = {{av.airport.icao + "_ATIS", "135.400", "", "INFORMATION KILO 1251Z WIND 270 AT 8 VIS 10 CEILING 2500 BROKEN ILS 27 IN USE", 7},
                                {av.airport.icao + "_TWR", "118.700", "Tower controller", "", 4}, {av.airport.icao + "_GND", "121.900", "Ground", "", 3}};
                shot(av, APP_AIRPORTS, "airports_vatsim");
            }
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
            // the abnormal and emergency procedures (an Airbus: its own set, de-icing too)
            c.acfIcao = "A321"; c.checklistText.clear(); c.checklistSource.clear(); c.engineType = 5;
            shot(c, APP_CHECKLISTS, "checklists_airliner_auto");
            auto tap = [&](float x, float y) {
                for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 60.0f; io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1);
                    ImGui::NewFrame(); Actions act; build(fonts, c, 800, kLogicalH, act); ImGui::Render(); } };
            tap(630, 97); shot(c, APP_CHECKLISTS, "checklists_abnormal");
            tap(735, 97); shot(c, APP_CHECKLISTS, "checklists_emergency");
            tap(533, 97);
            c.acfIcao = "AT76"; c.engineType = 2; shot(c, APP_CHECKLISTS, "checklists_turboprop");
        }
        {   // Documents: the list
            SimState d = t;
            d.docs.listed = true;
            d.docs.folders = {"E:/X-Plane 12/Output/XPTablet/Documents"};
            d.docs.files = {{"A320 FCOM.pdf", "/x/A320 FCOM.pdf", "XPTablet docs", true, 31457280}, {"Normal checklist.pdf", "/x/n.pdf", "XPTablet docs", true, 204800},
                            {"KSBN airport diagram.png", "/x/ksbn.png", "XPTablet docs", false, 880000}, {"KORD 10-9.jpg", "/x/kord.jpg", "AviTab charts", false, 1200000}};
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
            for (int i = 0; i < 24; ++i) { io.DeltaTime = 0.05f; io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); ImGui::NewFrame(); Actions act; build(fonts, s, 800, kLogicalH, act); ImGui::Render(); b.clear(); clipDrawData(ImGui::GetDrawData(), b); }   // (the highlight has settled)
            io.DeltaTime = 1.0f / 60.0f;
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
        // nothing connected yet on that A321: each door its own Connect (no menu), 1L's connects 1L
        { SimState g3 = g; g3.acfIcao = "A321"; g3.ground.paxDoors = 8; g3.ground.jwDoors = 2; g3.ground.jwDoorPax[0] = 0; g3.ground.jwDoorPax[1] = 2;
          float cy = -1;
          for (float yy = 90; yy < 460 && cy < 0; yy += 4) {   // 1L's Connect: pressed, its menu of jetways (no jetway action yet)
              const Actions q = press(g3, 78, yy);
              if (q.ground == GA_JETWAY_DOOR) { std::printf("FAIL: with two openSAM jetways, Connect must ask which\n"); ++failures; break; }
              for (int k = 0; k < 8; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, g3, 800, kLogicalH, act); ImGui::Render(); }
              const Actions m = press(g3, 220, yy + 18);
              if (m.ground == GA_JETWAY_DOOR) { cy = yy; if (m.groundIndex != 0) { std::printf("FAIL: 1L's menu connects 1L (%d)\n", m.groundIndex); ++failures; } }
              else press(g3, 700, 450);
          }
          if (cy < 0) { std::printf("FAIL: no jetway menu with two openSAM jetways\n"); ++failures; }
          else {
              press(g3, 78, cy);
              for (int k = 0; k < 8; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, g3, 800, kLogicalH, act); ImGui::Render(); }
              shot(g3, APP_GROUND, "ground_jetway_menu");
              press(g3, 700, 450);
          }
          // openSAM gave only 1L a jetway (2L's would cross): 2L's Connect is grey, 1L's connects straight away
          { SimState g5 = g3; g5.ground.jwReach = 1;
            bool two = false, one = false;
            for (float yy = 90; yy < 460; yy += 4) { const Actions q = press(g5, 78, yy); if (q.ground == GA_JETWAY_DOOR) { (q.groundIndex == 1 ? two : one) = true; } }
            if (two) { std::printf("FAIL: 2L's Connect works though its jetway would cross\n"); ++failures; }
            if (!one) { std::printf("FAIL: 1L's Connect with one openSAM jetway\n"); ++failures; }
            shot(g5, APP_GROUND, "ground_jetway_cross"); openApp(APP_GROUND); }
          // 2L docked: in 1L's menu, jetway 2 is grey (at 2L)
          if (cy >= 0) { SimState g6 = g3; g6.ground.jwReach = 2; g6.ground.jwDocked[1] = true;
            press(g6, 78, cy);
            for (int k = 0; k < 8; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, g6, 800, kLogicalH, act); ImGui::Render(); }
            shot(g6, APP_GROUND, "ground_jetway_menu_busy");
            const Actions m = press(g6, 220, cy + 18 + 34);
            if (m.ground == GA_JETWAY_DOOR) { std::printf("FAIL: the jetway at 2L is pressable in 1L's menu\n"); ++failures; }
            press(g6, 700, 450); openApp(APP_GROUND); }
          {   // Pushback: its bubble opens the panel; X-Plane's tug, then BetterPushback's
              SimState p1 = g3; p1.ground.chocks = true; p1.ground.parkBrake = true; p1.ground.extPower = true;
              auto settle = [&](const SimState& st2) { for (int k = 0; k < 10; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, st2, 800, kLogicalH, act); ImGui::Render(); } };
              press(p1, 735, 405); settle(p1);   // (Push Back: the square beside the jetway status)
              bool stop = false;
              for (float yy = 170; yy < 470 && !stop; yy += 6) { const Actions q = press(p1, 580, yy); if (q.ground == GA_PUSH_STOP) stop = true; }
              if (!stop) { std::printf("FAIL: the Pushback panel's Stop (X-Plane's tug)\n"); ++failures; }
              settle(p1);
              shot(p1, APP_GROUND, "ground_pushback_xp");
              SimState p2 = g3; p2.ground.pushSource = 1; p2.ground.bpConnected = true; p2.ground.parkBrake = true; p2.ground.taxiLight = true;
              settle(p2);   // (the panel stays open from X-Plane's tug above)
              bool start = false;
              for (float yy = 170; yy < 470 && !start; yy += 6) { const Actions q = press(p2, 480, yy); if (q.ground == GA_BP_START) start = true; }
              if (!start) { std::printf("FAIL: the Pushback panel's Start (BetterPushback)\n"); ++failures; }
              settle(p2);
              shot(p2, APP_GROUND, "ground_pushback_bp");
              {   // its tug: connected, its button disconnects it
                  bool disc = false;
                  for (float yy = 170; yy < 470 && !disc; yy += 6) { const Actions q = press(p2, 700, yy); if (q.ground == GA_BP_DISCONNECT) disc = true; }
                  if (!disc) { std::printf("FAIL: the Pushback panel's Disconnect the tug (BetterPushback)\n"); ++failures; }
                  settle(p2);
              }
              {   // Push Back again (red, "Close") puts the panel away (the cabin back)
                  press(p2, 735, 405); settle(p2);
                  bool cabin = false;
                  for (float yy = 100; yy < 340 && !cabin; yy += 6) { const Actions q = press(p2, 700, yy); if (q.ground == GA_SLIDES_AUTO) cabin = true; }
                  if (!cabin) { std::printf("FAIL: Push Back (Close) puts the panel away\n"); ++failures; }
              }
          }
          {   // a jetway docked: listed, and BetterPushback's Start and tug greyed (no action from them)
              SimState pj = g3; pj.ground.pushSource = 1; pj.ground.jwDocked[0] = true;
              press(pj, 735, 405);
              for (int k = 0; k < 10; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, pj, 800, kLogicalH, act); ImGui::Render(); }
              bool acted = false;
              for (float yy = 200; yy < 330; yy += 6) { const Actions q = press(pj, 480, yy); const Actions q2 = press(pj, 700, yy); if (q.ground == GA_BP_START || q2.ground == GA_BP_CONNECT) acted = true; }
              if (acted) { std::printf("FAIL: BetterPushback starts with a jetway connected\n"); ++failures; }
              shot(pj, APP_GROUND, "ground_pushback_jetway");
              press(pj, 735, 405);
          }
          {   // the tug on its way (a push started, not connected yet): yellow "Tug Connecting"
              SimState p3 = g3; p3.ground.pushSource = 1; p3.ground.bpStarted = true; p3.ground.bpConnected = false;
              press(p3, 735, 405);
              for (int k = 0; k < 10; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, p3, 800, kLogicalH, act); ImGui::Render(); }
              shot(p3, APP_GROUND, "ground_pushback_connecting");
              press(p3, 735, 405);
          }
          { SimState g4 = g3; g4.ground.slidesAuto = true; g4.ground.slides = 1; settings().cabinAuto = true; shot(g4, APP_GROUND, "ground_slides_auto"); settings().cabinAuto = false; }
          openApp(APP_GROUND); }
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
          { settings().showLive = true; openApp(APP_LIVEMAP);
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
          {   // the Media page: a long title (it slides), the media's and the computer's volumes; the pull-down from the logo
              SimState md = t;
              md.media.supported = true; md.media.active = true; md.media.playing = true; md.media.app = "Spotify";
              md.media.title = "A Very Long Song Title That Does Not Fit In Its Place At All"; md.media.artist = "The Approach Controllers"; md.media.album = "Cleared To Land";
              md.media.posSec = 83; md.media.lenSec = 241; md.media.canSeek = true; md.media.playerVolume = 0.6f; md.media.masterVolume = 0.8f;
              md.media.artColor = IM_COL32(150, 40, 90, 255);
              shot(md, APP_MUSIC, "media");
              {   // lowered for the radio: the media's bar is greyed and does not take a press; the computer's still does
                  SimState dk = md; dk.media.ducked = true; dk.media.radioRx = true; dk.media.radioSource = "xPilot"; dk.media.playerVolume = 0.15f;
                  shot(dk, APP_MUSIC, "media_ducked");
                  auto press = [&](const SimState& st, float x, float y) { Actions got; for (int k = 0; k < 4; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1 || k == 2);
                      ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); if (act.mediaPlayerVol >= 0) got.mediaPlayerVol = act.mediaPlayerVol; if (act.mediaMasterVol >= 0) got.mediaMasterVol = act.mediaMasterVol; }
                      io.AddMousePosEvent(-1000, -1000); io.AddMouseButtonEvent(0, false); ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); return got; };
                  const Actions g1 = press(dk, 640, 338);
                  if (g1.mediaPlayerVol >= 0) { std::printf("FAIL: the media's volume is not set while it is lowered for the radio\n"); ++failures; }
                  const Actions g2 = press(dk, 640, 376);
                  if (g2.mediaMasterVol < 0) { std::printf("FAIL: the computer's volume can still be set while the media is lowered\n"); ++failures; }
                  shot(md, APP_MUSIC, "media");
                  const Actions g3 = press(md, 640, 338);
                  if (g3.mediaPlayerVol < 0) { std::printf("FAIL: the media's volume slider sets the player's volume\n"); ++failures; }
              }
              for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(400, 14); io.AddMouseButtonEvent(0, k == 1);
                                            ImGui::NewFrame(); Actions act; build(fonts, md, 800, kLogicalH, act); ImGui::Render(); }
              for (int k = 0; k < 40; ++k) { io.AddMousePosEvent(-1000, -1000); ImGui::NewFrame(); Actions act; build(fonts, md, 800, kLogicalH, act); ImGui::Render(); }
              shot(md, APP_HOME, "pulldown_media");
              for (int k = 0; k < 3; ++k) { io.AddMousePosEvent(400, 14); io.AddMouseButtonEvent(0, k == 1); ImGui::NewFrame(); Actions act; build(fonts, md, 800, kLogicalH, act); ImGui::Render(); }
          }
          openApp(APP_BROWSER); shot(wb, APP_BROWSER, "browser_tabs");
          {   // the History over the page; the volume card; every tab closed: the New Tab page
              settings().history = {{"ChartFox", "https://chartfox.org/"}, {"SkyVector Aeronautical Charts", "https://skyvector.com/"}, {"VATSIM", "https://vatsim.net/"}};
              auto tapAt = [&](SimState& st, float x, float y) {
                  for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, k == 1);
                                                ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); } };
              tapAt(wb, 800 - 89, 162); shot(wb, APP_BROWSER, "browser_history"); tapAt(wb, 800 - 89, 162);
              tapAt(wb, 800 - 29, 126); shot(wb, APP_BROWSER, "browser_volume"); tapAt(wb, 400, 400);
              SimState nt = wb; tapAt(nt, 182, 92); nt.browser.tabs.clear(); nt.browser.activeTab = 0; shot(nt, APP_BROWSER, "browser_newtab");
              settings().history.clear();
          }
          { openApp(APP_SIMBRIEF); SimState w3 = wb; w3.browser.zoomPct = 125; shot(w3, APP_SIMBRIEF, "webapp_header"); openApp(APP_SIMBRIEF);
            // its Home button: back to SimBrief's own start page
            Actions got;
            for (int k = 0; k < 3; ++k) { io.DisplaySize = ImVec2(800, kLogicalH); io.AddMousePosEvent(800 - 114, 28.0f + 23.0f); io.AddMouseButtonEvent(0, k == 1);
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
    {   // a phone's tap: the finger was nowhere, then down and up at once (no hover before it); one tap opens an app
        ImGuiIO& io = ImGui::GetIO();
        SimState ph = s;
        auto frames = [&](int n) { for (int i = 0; i < n; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; ImGui::NewFrame(); Actions act; build(fonts, ph, 800, kLogicalH, act); ImGui::Render(); } };
        auto tap = [&](float x, float y, bool sameFrame) {   // as the emulator feeds a touch: through queuePointer / pumpPointer
            auto pf = [&](int n) { for (int i = 0; i < n; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; pumpPointer(); ImGui::NewFrame(); Actions act; build(fonts, ph, 800, kLogicalH, act); ImGui::Render(); } };
            queuePointer(-1000, -1000, -1); pf(3);
            queuePointer(x, y, -1); queuePointer(x, y, 1);
            if (!sameFrame) pf(1);
            queuePointer(x, y, -1); queuePointer(x, y, 0); pf(4);
            queuePointer(-1000, -1000, -1); pf(20);
        };
        {   // the same place with the finger there a frame first (as a mouse): does it open?
            openApp(APP_HOME); frames(30);
            io.AddMousePosEvent(88, 75); frames(2); io.AddMouseButtonEvent(0, true); frames(1); io.AddMouseButtonEvent(0, false); frames(1); io.AddMousePosEvent(-1000, -1000); frames(20);
            std::printf("mouse tap: app %d\n", currentApp());
        }
        {   // a finger drags the App Store's list up, begun on an app: it scrolls, and nothing is installed or opened
            openApp(APP_APPSTORE);
            auto pf = [&](int n) { for (int i = 0; i < n; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; pumpPointer(); ImGui::NewFrame(); Actions act; build(fonts, ph, 800, kLogicalH, act); ImGui::Render(); } };
            pf(20);
            const auto before = settings().installed;
            queuePointer(400, 300, -1); queuePointer(400, 300, 1); pf(2);
            for (int k = 1; k <= 20; ++k) { queuePointer(400, 300 - k * 8.0f, -1); pf(1); }
            queuePointer(400, 140, 0); pf(2); queuePointer(-1000, -1000, -1); pf(10);
            if (settings().installed != before || currentApp() != APP_APPSTORE) { std::printf("FAIL: a drag on the App Store pressed an app\n"); ++failures; }
            openApp(APP_HOME);
        }
        {   // typing in the Browser's address bar, then its last tab closed: the keys go too
            SimState bs = s; bs.browser.status = 1; bs.browser.tabs = {{1, "Example", "https://example.com", false, 0}}; bs.browser.activeTab = 1; bs.browser.url = "https://example.com";
            openApp(APP_BROWSER);
            auto bf = [&](const SimState& st, int n) { for (int i = 0; i < n; ++i) { io.DisplaySize = ImVec2(800, kLogicalH); io.DeltaTime = 1.0f / 30.0f; pumpPointer(); ImGui::NewFrame(); Actions act; build(fonts, st, 800, kLogicalH, act); ImGui::Render(); } };
            bf(bs, 10);
            for (float yy = 30; yy < 110 && !tabletTyping(); yy += 6) for (float xx = 200; xx < 600 && !tabletTyping(); xx += 40) {
                queuePointer(xx, yy, -1); queuePointer(xx, yy, 1); bf(bs, 2); queuePointer(xx, yy, 0); bf(bs, 3);
            }
            if (!tabletTyping()) { std::printf("FAIL: could not type in the Browser's address bar\n"); ++failures; }
            else {
                SimState none = bs; none.browser.tabs.clear(); none.browser.activeTab = 0;
                bf(none, 5);
                if (tabletTyping()) { std::printf("FAIL: the keyboard stays up with every tab closed\n"); ++failures; }
            }
            queuePointer(-1000, -1000, -1); bf(bs, 3);
            openApp(APP_HOME);
        }
        for (int same = 0; same < 2; ++same) {
            openApp(APP_HOME); frames(30);
            tap(88, 75, same != 0);
            std::printf("phone tap (%s): app %d\n", same ? "down and up in one frame" : "down, then up", currentApp());
            if (currentApp() == APP_HOME) { std::printf("FAIL: one phone tap does not open an app (%s)\n", same ? "down and up in one frame" : "down, then up"); ++failures; }
            if (currentApp() != APP_HOME) { tap(30, 50, same != 0); if (currentApp() != APP_HOME) { std::printf("FAIL: one phone tap on Home does not go home (%d)\n", currentApp()); ++failures; openApp(APP_HOME); } }
        }
    }
    {   // the arcade games played by random fingers for a while each: nothing goes out of its field, scores only grow
        for (int g = 0; g < xpt::arcade::COUNT; ++g) {
            xpt::arcade::reset(g);
            unsigned seed = 7u + g * 13u;
            int last = 0, games = 0;
            for (int i = 0; i < 20000; ++i) {
                seed = seed * 1103515245u + 12345u;
                xpt::arcade::Input in;
                const int k = (seed >> 16) % 7;
                in.left = k == 0; in.right = k == 1; in.up = k == 2; in.down = k == 3; in.action = (seed >> 20) % 3 == 0;
                in.leftHit = in.left && (seed >> 24) % 4 == 0; in.rightHit = in.right && (seed >> 24) % 4 == 0; in.upHit = in.up && (seed >> 24) % 4 == 0;
                in.downHit = in.down && (seed >> 24) % 4 == 0; in.actionHit = in.action && (seed >> 25) % 3 == 0;
                xpt::arcade::step(g, in, 1.0f / 60.0f);
                const int sc = xpt::arcade::score(g);
                if (sc < last && g != xpt::arcade::COCKPIT_21) { std::printf("FAIL: %s's score went down (%d to %d)\n", xpt::arcade::name(g), last, sc); ++failures; break; }   // (21: the miles go both ways)
                last = sc;
                if (xpt::arcade::over(g)) { ++games; xpt::arcade::reset(g); last = 0; }
            }
            std::printf("arcade %s: %d games over in 20000 steps\n", xpt::arcade::name(g), games);
        }
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
