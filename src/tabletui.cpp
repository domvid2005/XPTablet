#include "tabletui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "version.h"

extern "C" {
extern const unsigned char xpt_asset_plex_regular[];
extern const unsigned int xpt_asset_plex_regular_size;
extern const unsigned char xpt_asset_plex_semibold[];
extern const unsigned int xpt_asset_plex_semibold_size;
extern const unsigned char xpt_asset_plex_bold[];
extern const unsigned int xpt_asset_plex_bold_size;
extern const unsigned char xpt_asset_mono[];
extern const unsigned int xpt_asset_mono_size;
}

namespace xpt {
namespace {

const float kStatusH = 28.0f, kHeaderH = 46.0f;
const float kPi = 3.14159265f;

struct AppDef { const char* id; const char* name; ImU32 color; const char* url; };   // id: its name in the settings file; url: a web app
const AppDef kApps[APP_COUNT] = {
    {"map", "Map", IM_COL32(52, 168, 110, 255), nullptr},                  {"airports", "Airports", IM_COL32(64, 132, 230, 255), nullptr},
    {"charts", "Charts", IM_COL32(236, 152, 50, 255), nullptr},            {"documents", "Documents", IM_COL32(120, 132, 156, 255), nullptr},
    {"routes", "Routes", IM_COL32(150, 96, 250, 255), nullptr},            {"weather", "Weather", IM_COL32(40, 180, 214, 255), nullptr},
    {"ground", "Ground", IM_COL32(246, 190, 48, 255), nullptr},            {"checklists", "Checklists", IM_COL32(60, 190, 130, 255), nullptr},
    {"performance", "Performance", IM_COL32(236, 92, 104, 255), nullptr},  {"traffic", "Traffic", IM_COL32(40, 190, 170, 255), nullptr},
    {"notes", "Notes", IM_COL32(244, 206, 80, 255), nullptr},
    {"simbrief", "SimBrief", IM_COL32(196, 58, 66, 255), "https://dispatch.simbrief.com"},
    {"livemap", "Live Map", IM_COL32(198, 92, 204, 255), nullptr},
    {"foreflight", "ForeFlight", IM_COL32(30, 140, 222, 255), "https://plan.foreflight.com"},
    {"browser", "Browser", IM_COL32(72, 110, 240, 255), nullptr},          {"settings", "Settings", IM_COL32(110, 120, 140, 255), nullptr},
};
const char* const kChartNames[2] = {"ChartFox", "Navigraph"};
const char* const kChartUrls[2] = {"https://chartfox.org", "https://charts.navigraph.com"};
const char* const kLiveNames[2] = {"Volanta", "VATSIM Map"};
const char* const kLiveUrls[2] = {"https://fly.volanta.app", "https://map.vatsim.net"};
const ImU32 kLiveColors[2] = {IM_COL32(198, 92, 204, 255), IM_COL32(44, 150, 96, 255)};
enum { ICON_VOLANTA = APP_COUNT, ICON_VATSIM };   // the Live Map icon's two pictures

Settings g_settings;
bool g_settingsChanged = false;
std::string g_url = "https://chartfox.org", g_site = "Browser";   // what the web page shows
int favSlots() { return std::max(0, std::min(kMaxFavorites, g_settings.favoriteSlots)); }
bool g_ownPerformance = false;   // the aircraft brings its own performance calculator (ToLiss): no Performance app
bool g_chrome = true;                                             // with the browser's own controls

void openWeb(const char* site, const char* url, bool chrome);

int chartsP() { return g_settings.chartsProvider == 1 ? 1 : 0; }
int liveP() { return g_settings.liveProvider == 1 ? 1 : 0; }
ImU32 appColor(int app) { return app == APP_LIVEMAP ? kLiveColors[liveP()] : kApps[app].color; }
int appIcon(int app) { return app == APP_LIVEMAP ? (liveP() ? (int)ICON_VATSIM : (int)ICON_VOLANTA) : app; }

// Every app is in exactly one of the two lists (grid order, favorites bar); whatever a settings file left out goes to the grid's end.
void normalizeLayout() {
    bool seen[APP_COUNT] = {};
    auto clean = [&](std::vector<int>& v) {
        std::vector<int> out;
        for (int a : v) if (a >= 0 && a < APP_COUNT && !seen[a]) { seen[a] = true; out.push_back(a); }
        v.swap(out);
    };
    clean(g_settings.favorites);
    std::vector<int> spill;   // favorites beyond the bar's size go back to the front of the grid
    while ((int)g_settings.favorites.size() > favSlots()) { spill.insert(spill.begin(), g_settings.favorites.back()); g_settings.favorites.pop_back(); }
    for (int a : spill) seen[a] = false;
    g_settings.order.insert(g_settings.order.begin(), spill.begin(), spill.end());
    clean(g_settings.order);
    for (int a = 0; a < APP_COUNT; ++a) if (!seen[a]) g_settings.order.push_back(a);
}

int g_app = APP_HOME;
float g_px = 1.0f;   // screen pixels per layout unit this frame

// ImGui anti-aliases with an edge one layout unit wide. The tablet is drawn at several pixels per unit, where that edge would be a
// blur several pixels wide: this makes it one screen pixel again, so lines, circles and rounded corners stay crisp.
void sharp(ImDrawList* dl) { dl->_FringeScale = 1.0f / g_px; }
float snap(float v) { return std::floor(v * g_px + 0.5f) / g_px; }   // to the nearest screen pixel
std::vector<ImVec2> g_touches;   // the touch test (Settings): where the screen was pressed

ImU32 shade(ImU32 c, float k) {   // brighter (k > 1) or darker
    auto ch = [&](int s) { return (ImU32)std::min(255.0f, ((c >> s) & 0xFF) * k) << s; };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

void textCentered(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* txt) {
    const float fs = f->FontSize * ImGui::GetIO().FontGlobalScale;
    const ImVec2 sz = f->CalcTextSizeA(fs, 1e9f, 0, txt);
    dl->AddText(f, fs, ImVec2(std::floor(cx - sz.x * 0.5f), y), col, txt);
}
float fontPx(ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

// The default wallpaper: a navy sky that lightens downwards, with a mountain range along the bottom edge.
void wallpaper(ImDrawList* dl, float w, float h) {
    const ImU32 top = IM_COL32(9, 22, 44, 255), bot = IM_COL32(24, 58, 104, 255);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), top, top, bot, bot);
    const float back[][2] = {{0.00f, 0.80f}, {0.12f, 0.62f}, {0.22f, 0.74f}, {0.36f, 0.50f}, {0.50f, 0.72f}, {0.62f, 0.56f}, {0.76f, 0.76f}, {0.88f, 0.60f}, {1.00f, 0.78f}};
    const float front[][2] = {{0.00f, 0.92f}, {0.10f, 0.80f}, {0.24f, 0.90f}, {0.42f, 0.70f}, {0.56f, 0.88f}, {0.70f, 0.76f}, {0.84f, 0.90f}, {1.00f, 0.80f}};
    auto range = [&](const float (*p)[2], int n, ImU32 col) {
        for (int i = 0; i + 1 < n; ++i) {
            const ImVec2 a(p[i][0] * w, p[i][1] * h), b(p[i + 1][0] * w, p[i + 1][1] * h);
            dl->AddQuadFilled(a, b, ImVec2(b.x, h), ImVec2(a.x, h), col);
        }
    };
    range(back, 9, IM_COL32(20, 46, 86, 255));
    range(front, 8, IM_COL32(14, 34, 66, 255));
}

// A small white pictogram for each app, drawn from lines and circles inside a circle of radius r around c.
void icon(ImDrawList* dl, int app, ImVec2 c, float r, ImU32 col) {
    c = ImVec2(snap(c.x), snap(c.y));
    const float t = snap(std::max(2.0f, r * 0.15f));   // a whole number of screen pixels: no half-covered edges
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (app) {
    case APP_MAP:   // a location pin
        dl->AddCircle(P(0, -0.25f), r * 0.50f, col, 24, t);
        dl->AddCircleFilled(P(0, -0.25f), r * 0.17f, col, 12);
        { const ImVec2 pts[3] = {P(-0.42f, 0.05f), P(0, 0.85f), P(0.42f, 0.05f)}; dl->AddPolyline(pts, 3, col, 0, t); }
        break;
    case APP_AIRPORTS:   // a runway seen from above
        dl->AddRect(P(-0.32f, -0.85f), P(0.32f, 0.85f), col, r * 0.08f, 0, t);
        for (int i = 0; i < 4; ++i) dl->AddLine(P(0, -0.60f + i * 0.36f), P(0, -0.42f + i * 0.36f), col, t);
        break;
    case APP_CHARTS:   // an approach plate: a sheet with a profile line
        dl->AddRect(P(-0.60f, -0.80f), P(0.60f, 0.80f), col, r * 0.10f, 0, t);
        dl->AddLine(P(-0.36f, -0.42f), P(0.36f, -0.42f), col, t);
        { const ImVec2 pts[3] = {P(-0.36f, 0.00f), P(0.00f, 0.00f), P(0.36f, 0.44f)}; dl->AddPolyline(pts, 3, col, 0, t); }
        break;
    case APP_DOCUMENTS:   // a folder
        dl->AddRect(P(-0.80f, -0.40f), P(0.80f, 0.64f), col, r * 0.10f, 0, t);
        { const ImVec2 pts[4] = {P(-0.80f, -0.40f), P(-0.62f, -0.66f), P(-0.14f, -0.66f), P(0.04f, -0.40f)}; dl->AddPolyline(pts, 4, col, 0, t); }
        break;
    case APP_ROUTES:   // waypoints joined by legs
        dl->AddLine(P(-0.66f, 0.56f), P(-0.10f, -0.20f), col, t);
        dl->AddLine(P(-0.10f, -0.20f), P(0.66f, -0.56f), col, t);
        dl->AddCircleFilled(P(-0.66f, 0.56f), r * 0.18f, col, 12);
        dl->AddCircleFilled(P(-0.10f, -0.20f), r * 0.18f, col, 12);
        dl->AddCircleFilled(P(0.66f, -0.56f), r * 0.18f, col, 12);
        break;
    case APP_WEATHER:   // a cloud
        dl->AddCircleFilled(P(-0.40f, 0.14f), r * 0.34f, col, 20);
        dl->AddCircleFilled(P(0.02f, -0.14f), r * 0.46f, col, 24);
        dl->AddCircleFilled(P(0.46f, 0.14f), r * 0.32f, col, 20);
        dl->AddRectFilled(P(-0.40f, 0.10f), P(0.46f, 0.47f), col);
        break;
    case APP_BROWSER:   // a globe
        dl->AddCircle(c, r * 0.80f, col, 28, t);
        dl->AddEllipse(c, ImVec2(r * 0.36f, r * 0.80f), col, 0, 24, t);
        dl->AddLine(P(-0.80f, 0), P(0.80f, 0), col, t);
        break;
    case APP_GROUND:   // an aircraft from above
        dl->AddLine(P(0, -0.86f), P(0, 0.80f), col, t * 1.7f);
        dl->AddTriangleFilled(P(0, -0.26f), P(-0.90f, 0.30f), P(0, 0.22f), col);
        dl->AddTriangleFilled(P(0, -0.26f), P(0.90f, 0.30f), P(0, 0.22f), col);
        dl->AddTriangleFilled(P(0, 0.50f), P(-0.38f, 0.86f), P(0.38f, 0.86f), col);
        break;
    case APP_CHECKLISTS:   // a tick
        { const ImVec2 pts[3] = {P(-0.66f, 0.04f), P(-0.20f, 0.52f), P(0.70f, -0.52f)}; dl->AddPolyline(pts, 3, col, 0, t * 1.5f); }
        break;
    case APP_PERFORMANCE:   // a gauge
        dl->PathArcTo(P(0, 0.30f), r * 0.80f, kPi, 2 * kPi, 24);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(0, 0.30f), P(0.42f, -0.26f), col, t * 1.3f);
        dl->AddCircleFilled(P(0, 0.30f), r * 0.13f, col, 12);
        break;
    case APP_TRAFFIC:   // a radar scope
        dl->AddCircle(c, r * 0.82f, col, 28, t);
        dl->AddCircle(c, r * 0.42f, col, 20, t);
        dl->AddLine(c, P(0.58f, -0.58f), col, t);
        dl->AddCircleFilled(P(-0.34f, -0.48f), r * 0.11f, col, 10);
        dl->AddCircleFilled(P(0.30f, 0.52f), r * 0.11f, col, 10);
        break;
    case APP_NOTES:   // a page with lines
        dl->AddRect(P(-0.62f, -0.80f), P(0.62f, 0.80f), col, r * 0.10f, 0, t);
        for (int i = 0; i < 3; ++i) dl->AddLine(P(-0.36f, -0.40f + i * 0.40f), P(0.36f, -0.40f + i * 0.40f), col, t);
        break;
    case APP_SETTINGS:   // a cog
        dl->AddCircle(c, r * 0.44f, col, 24, t * 1.3f);
        for (int i = 0; i < 8; ++i) {
            const float a = i * kPi / 4;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.54f, c.y + std::sin(a) * r * 0.54f), ImVec2(c.x + std::cos(a) * r * 0.86f, c.y + std::sin(a) * r * 0.86f), col, t * 1.5f);
        }
        break;
    case APP_SIMBRIEF:   // a briefing on a clipboard
        dl->AddRect(P(-0.60f, -0.66f), P(0.60f, 0.84f), col, r * 0.10f, 0, t);
        dl->AddRectFilled(P(-0.28f, -0.86f), P(0.28f, -0.52f), col, r * 0.08f);
        for (int i = 0; i < 3; ++i) dl->AddLine(P(-0.34f, -0.18f + i * 0.34f), P(0.34f, -0.18f + i * 0.34f), col, t);
        break;
    case ICON_VOLANTA:   // a flight's track from one place to another
        dl->PathLineTo(P(-0.72f, 0.48f));
        dl->PathBezierQuadraticCurveTo(P(0.0f, -1.10f), P(0.72f, 0.48f), 24);
        dl->PathStroke(col, 0, t);
        dl->AddCircleFilled(P(-0.72f, 0.48f), r * 0.19f, col, 12);
        dl->AddCircleFilled(P(0.72f, 0.48f), r * 0.19f, col, 12);
        break;
    case ICON_VATSIM:   // a station and its radio waves
        dl->AddCircleFilled(P(0, 0.34f), r * 0.20f, col, 12);
        for (int i = 1; i <= 2; ++i) {
            dl->PathArcTo(P(0, 0.34f), r * (0.20f + 0.34f * i), kPi * 1.20f, kPi * 1.80f, 16);
            dl->PathStroke(col, 0, t);
        }
        break;
    case APP_FOREFLIGHT:   // a compass
        dl->AddCircle(c, r * 0.84f, col, 28, t);
        dl->AddTriangleFilled(P(0.44f, -0.44f), P(-0.16f, -0.16f), P(0.16f, 0.16f), col);
        dl->AddTriangle(P(-0.44f, 0.44f), P(-0.16f, -0.16f), P(0.16f, 0.16f), col, t * 0.7f);
        break;
    default: break;
    }
}

// The tablet's battery: an outline with a nub, filled to the charge, green while the aircraft feeds it and red when nearly empty.
void battery(ImDrawList* dl, ImVec2 p, int pct, bool charging) {
    const float w = 22, h = 11;
    p = ImVec2(snap(p.x), snap(p.y));
    const ImU32 line = IM_COL32(232, 238, 248, 255);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), line, 3.0f, 0, snap(1.2f));
    dl->AddRectFilled(ImVec2(p.x + w + 1, p.y + 3), ImVec2(p.x + w + 3, p.y + h - 3), line, 1.0f);
    const float k = std::max(0.0f, std::min(1.0f, pct / 100.0f));
    const ImU32 fill = charging ? IM_COL32(80, 214, 124, 255) : pct <= 15 ? IM_COL32(240, 84, 84, 255) : line;
    if (k > 0.02f) dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 2 + (w - 4) * k, p.y + h - 2), fill, 1.5f);
    if (charging) {   // a small bolt beside it
        const ImVec2 b(p.x - 9, p.y + h * 0.5f);
        dl->AddTriangleFilled(ImVec2(b.x + 2, b.y - 6), ImVec2(b.x - 3, b.y + 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(80, 214, 124, 255));
        dl->AddTriangleFilled(ImVec2(b.x - 2, b.y + 6), ImVec2(b.x + 3, b.y - 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(80, 214, 124, 255));
    }
}

void statusBar(ImDrawList* dl, const Fonts& f, const SimState& s, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), IM_COL32(0, 0, 0, 90));
    const float y = std::floor((kStatusH - fontPx(f.small)) * 0.5f);
    const ImU32 col = IM_COL32(232, 238, 248, 255);
    dl->AddText(f.small, fontPx(f.small), ImVec2(12, y), col, "XPTablet");
    char buf[32] = "--:--Z";
    if (s.zuluSec >= 0) std::snprintf(buf, sizeof buf, "%02d:%02dZ", (s.zuluSec / 3600) % 24, (s.zuluSec / 60) % 60);
    textCentered(dl, f.small, w * 0.5f, y, col, buf);
    // right: aircraft type, charge, battery
    float x = w - 12 - 25;
    battery(dl, ImVec2(x, std::floor((kStatusH - 11) * 0.5f)), s.batteryPct, s.charging);
    std::snprintf(buf, sizeof buf, "%d%%", std::max(0, std::min(100, s.batteryPct)));
    ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, buf);
    x -= sz.x + (s.charging ? 20 : 8);
    dl->AddText(f.small, fontPx(f.small), ImVec2(x, y), col, buf);
    if (!s.acfIcao.empty()) {
        sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, s.acfIcao.c_str());
        x -= sz.x + 16;
        dl->AddText(f.small, fontPx(f.small), ImVec2(x, y), col, s.acfIcao.c_str());
    }
}

// The home screen: the grid of icons and, along the bottom, the favorites bar. An icon is opened with a tap; pressed and moved,
// it comes away under the finger and can be dropped anywhere in the grid or in the bar, the others making room.
int g_pressApp = -1;          // the icon under the finger since the press, -1: none
ImVec2 g_pressPos;
bool g_dragging = false;

void drawTile(ImDrawList* dl, const Fonts& f, int app, ImVec2 c, float tile, bool label, float scale, float shadeK) {
    const ImVec2 a(c.x - tile * 0.5f * scale, c.y - tile * 0.5f * scale), b(c.x + tile * 0.5f * scale, c.y + tile * 0.5f * scale);
    dl->AddRectFilled(a, b, shade(appColor(app), shadeK), tile * 0.24f * scale);
    icon(dl, appIcon(app), c, tile * 0.32f * scale, IM_COL32(255, 255, 255, 255));
    if (label) textCentered(dl, f.small, c.x, c.y + tile * 0.5f + 6, IM_COL32(236, 241, 250, 255), appName(app));
}

void home(ImDrawList* dl, const Fonts& f, float w, float h) {
    normalizeLayout();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const float cellW = 104, cellH = 100, tile = 60, dockTile = 54, dockCell = 76, dockH = 74, dockPad = 12;
    const bool hasDock = favSlots() > 0;
    const float dockTop = hasDock ? h - dockH - 10 : h + 100;   // no favorites bar: the grid has the whole screen

    // the two lists as shown: visible apps, without the one being dragged
    const int drag = g_dragging ? g_pressApp : -1;
    std::vector<int> grid, dock;
    for (int a : g_settings.order) if (appVisible(a) && a != drag) grid.push_back(a);
    for (int a : g_settings.favorites) if (appVisible(a) && a != drag) dock.push_back(a);

    const int cols = std::max(1, (int)((w - 24) / cellW));
    const float gx0 = std::floor((w - cols * cellW) * 0.5f);
    // where the dragged icon would land: a place in the bar or in the grid
    bool toDock = false;
    int gap = -1;
    const int gridCount = (int)grid.size() + (drag >= 0 ? 1 : 0);
    const int rows = std::max(1, (gridCount + cols - 1) / cols);
    const float gy0 = kStatusH + std::max(8.0f, std::floor(((hasDock ? dockTop : h) - kStatusH - rows * cellH) * 0.5f));
    if (drag >= 0) {
        toDock = hasDock && m.y >= dockTop - 8 && (int)dock.size() < favSlots();
        if (toDock) {
            gap = std::max(0, std::min((int)dock.size(), (int)std::floor((m.x - ((w - favSlots() * dockCell) * 0.5f)) / dockCell)));
        } else {
            const int col = std::max(0, std::min(cols - 1, (int)std::floor((m.x - gx0) / cellW)));
            const int row = std::max(0, std::min(rows - 1, (int)std::floor((m.y - gy0) / cellH)));
            gap = std::max(0, std::min((int)grid.size(), row * cols + col));
        }
    }

    // the grid
    struct Hit { int app; ImVec2 a, b; };
    std::vector<Hit> hits;
    for (int k = 0; k < (int)grid.size(); ++k) {
        const int slot = (drag >= 0 && !toDock && k >= gap) ? k + 1 : k;
        const float cx = gx0 + (slot % cols) * cellW, cy = gy0 + (slot / cols) * cellH;
        const ImVec2 c(cx + cellW * 0.5f, cy + 6 + tile * 0.5f);
        const bool over = drag < 0 && m.x >= cx && m.x < cx + cellW && m.y >= cy && m.y < cy + cellH;
        const bool held = g_pressApp == grid[k] && io.MouseDown[0];
        drawTile(dl, f, grid[k], c, tile, true, held ? 0.94f : 1.0f, held ? 0.86f : over ? 1.12f : 1.0f);
        hits.push_back({grid[k], ImVec2(cx, cy), ImVec2(cx + cellW, cy + cellH)});
    }
    // the favorites bar: as many places as Settings gives it, the free ones shown as outlines
    const int dockSlots = favSlots();
    const float dw = dockSlots * dockCell + dockPad * 2;
    const float dx0 = std::floor((w - dw) * 0.5f);
    if (hasDock) dl->AddRectFilled(ImVec2(dx0, dockTop), ImVec2(dx0 + dw, dockTop + dockH), IM_COL32(255, 255, 255, drag >= 0 && toDock ? 52 : 30), 22.0f);
    for (int k = (int)dock.size() + ((drag >= 0 && toDock) ? 1 : 0); k < dockSlots; ++k) {
        const ImVec2 c(dx0 + dockPad + k * dockCell + dockCell * 0.5f, dockTop + dockH * 0.5f);
        dl->AddRect(ImVec2(c.x - dockTile * 0.5f, c.y - dockTile * 0.5f), ImVec2(c.x + dockTile * 0.5f, c.y + dockTile * 0.5f), IM_COL32(255, 255, 255, 46), dockTile * 0.24f, 0, snap(1.5f));
    }
    for (int k = 0; k < (int)dock.size(); ++k) {
        const int slot = (drag >= 0 && toDock && k >= gap) ? k + 1 : k;
        const float cx = dx0 + dockPad + slot * dockCell;
        const ImVec2 c(cx + dockCell * 0.5f, dockTop + dockH * 0.5f);
        const bool over = drag < 0 && m.x >= cx && m.x < cx + dockCell && m.y >= dockTop && m.y < dockTop + dockH;
        const bool held = g_pressApp == dock[k] && io.MouseDown[0];
        drawTile(dl, f, dock[k], c, dockTile, false, held ? 0.94f : 1.0f, held ? 0.86f : over ? 1.12f : 1.0f);
        hits.push_back({dock[k], ImVec2(cx, dockTop), ImVec2(cx + dockCell, dockTop + dockH)});
    }

    // the finger
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    ImGui::InvisibleButton("##home", ImVec2(w, h - kStatusH));
    if (ImGui::IsItemActivated()) {
        g_pressApp = -1; g_dragging = false; g_pressPos = m;
        for (const Hit& ht : hits) if (m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) g_pressApp = ht.app;
    }
    if (g_pressApp >= 0 && io.MouseDown[0] && !g_dragging && std::fabs(m.x - g_pressPos.x) + std::fabs(m.y - g_pressPos.y) > 12) g_dragging = true;
    if (g_dragging && g_pressApp >= 0) drawTile(dl, f, g_pressApp, m, tile, false, 1.12f, 1.08f);   // the lifted icon, under the finger
    if (ImGui::IsItemDeactivated() || (g_pressApp >= 0 && !io.MouseDown[0])) {
        const int app = g_pressApp;
        if (app >= 0 && g_dragging && gap >= 0) {   // dropped: into the bar or the grid, at the gap
            auto take = [&](std::vector<int>& v) { for (size_t i = 0; i < v.size(); ++i) if (v[i] == app) { v.erase(v.begin() + i); break; } };
            take(g_settings.order); take(g_settings.favorites);
            std::vector<int>& dst = toDock ? g_settings.favorites : g_settings.order;
            const std::vector<int>& shown = toDock ? dock : grid;
            // the gap counts shown icons; the stored list may also hold hidden ones: insert before the icon now at the gap
            size_t at = dst.size();
            if (gap < (int)shown.size()) for (size_t i = 0; i < dst.size(); ++i) if (dst[i] == shown[gap]) { at = i; break; }
            dst.insert(dst.begin() + at, app);
            g_settingsChanged = true;
        } else if (app >= 0 && !g_dragging) {
            for (const Hit& ht : hits) if (ht.app == app && m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) openApp(app);
        }
        g_pressApp = -1; g_dragging = false;
    }
}

bool pageHeader(ImDrawList* dl, const Fonts& f, float w, const char* title) {
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, kStatusH + kHeaderH), IM_COL32(10, 24, 48, 235));
    dl->AddLine(ImVec2(0, kStatusH + kHeaderH), ImVec2(w, kStatusH + kHeaderH), IM_COL32(255, 255, 255, 28));
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    const bool back = ImGui::InvisibleButton("##home", ImVec2(96, kHeaderH));
    const ImU32 col = ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 190, 255, 255);
    const float my = kStatusH + kHeaderH * 0.5f;
    { const ImVec2 pts[3] = {ImVec2(22, my - 8), ImVec2(14, my), ImVec2(22, my + 8)}; dl->AddPolyline(pts, 3, col, 0, 2.0f); }
    dl->AddText(f.body, fontPx(f.body), ImVec2(30, std::floor(my - fontPx(f.body) * 0.5f)), col, "Home");
    textCentered(dl, f.title, w * 0.5f, std::floor(my - fontPx(f.title) * 0.5f), IM_COL32(245, 248, 255, 255), title);
    return back;
}

void placeholderPage(ImDrawList* dl, const Fonts& f, int app, float w, float h) {
    const ImVec2 c(w * 0.5f, kStatusH + kHeaderH + (h - kStatusH - kHeaderH) * 0.42f);
    dl->AddRectFilled(ImVec2(c.x - 48, c.y - 48), ImVec2(c.x + 48, c.y + 48), appColor(app), 22.0f);
    icon(dl, appIcon(app), c, 30, IM_COL32(255, 255, 255, 255));
    textCentered(dl, f.body, c.x, c.y + 64, IM_COL32(210, 220, 236, 255), "This app arrives in a later test build.");
}

// The browser: a toolbar (back, forward, reload, the address, bookmark), the bookmarks, then the page itself.
void webContent(ImDrawList* dl, const Fonts& f, float w, float h, float py) {
    // the page: until the browser engine is in, a card saying what will load here
    dl->AddRectFilled(ImVec2(0, py), ImVec2(w, h), IM_COL32(244, 246, 250, 255));
    const ImVec2 c(w * 0.5f, py + (h - py) * 0.42f);
    dl->AddCircleFilled(c, 30, kApps[APP_BROWSER].color, 40);
    icon(dl, APP_BROWSER, c, 18, IM_COL32(255, 255, 255, 255));
    textCentered(dl, f.bold, c.x, c.y + 42, IM_COL32(30, 40, 60, 255), g_url.c_str());
    textCentered(dl, f.body, c.x, c.y + 64, IM_COL32(90, 100, 120, 255), "The page loads here once the browser engine is in (next build).");
}

void browserPage(ImDrawList* dl, const Fonts& f, float w, float h) {
    if (!g_chrome) { webContent(dl, f, w, h, kStatusH + kHeaderH); return; }   // a web app: the site fills the page, like any other app
    const float top = kStatusH + kHeaderH, barH = 40, bmH = 34;
    dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + barH + bmH), IM_COL32(10, 24, 48, 235));
    const ImU32 ink = IM_COL32(210, 222, 242, 255), dim = IM_COL32(120, 140, 172, 255);
    const float my = top + barH * 0.5f;
    // back, forward, reload
    { const ImVec2 p[3] = {ImVec2(26, my - 7), ImVec2(19, my), ImVec2(26, my + 7)}; dl->AddPolyline(p, 3, dim, 0, 2.0f); }
    { const ImVec2 p[3] = {ImVec2(50, my - 7), ImVec2(57, my), ImVec2(50, my + 7)}; dl->AddPolyline(p, 3, dim, 0, 2.0f); }
    dl->PathArcTo(ImVec2(86, my), 7.5f, kPi * 0.25f, kPi * 1.85f, 20);
    dl->PathStroke(ink, 0, 2.0f);
    dl->AddTriangleFilled(ImVec2(95.5f, my - 7.5f), ImVec2(95.5f, my - 0.5f), ImVec2(88.5f, my - 4.0f), ink);
    // the address
    const ImVec2 a(112, top + 6), b(w - 52, top + barH - 6);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 26), 14.0f);
    dl->PushClipRect(a, b, true);
    dl->AddText(f.body, fontPx(f.body), ImVec2(a.x + 14, std::floor(my - fontPx(f.body) * 0.5f)), ink, g_url.c_str());
    dl->PopClipRect();
    // bookmark star
    {
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i) {
            const float ang = -kPi * 0.5f + i * kPi / 5, rad = (i & 1) ? 4.2f : 9.5f;
            pts[i] = ImVec2(w - 28 + std::cos(ang) * rad, my + std::sin(ang) * rad);
        }
        dl->AddPolyline(pts, 10, IM_COL32(255, 196, 80, 255), ImDrawFlags_Closed, 1.6f);
    }
    // bookmarks: the starter set; pressing one loads it
    float x = 14;
    const float by = top + barH + 3;
    struct Bm { const char* name; const char* url; };
    const Bm bms[] = {{"ChartFox", kChartUrls[0]}, {"Navigraph", kChartUrls[1]}, {"SimBrief", kApps[APP_SIMBRIEF].url}, {"Volanta", kLiveUrls[0]},
                      {"VATSIM Map", kLiveUrls[1]}, {"ForeFlight", kApps[APP_FOREFLIGHT].url}};
    for (const Bm& bm : bms) {
        const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, bm.name);
        const float bw = sz.x + 22;
        if (x + bw > w - 10) break;
        ImGui::SetCursorScreenPos(ImVec2(x, by));
        ImGui::PushID(bm.name);
        const bool hit = ImGui::InvisibleButton("bm", ImVec2(bw, bmH - 8));
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool cur = g_url == bm.url;
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + bw, by + bmH - 8), cur ? IM_COL32(90, 150, 255, 90) : hot ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 20), 13.0f);
        dl->AddText(f.small, fontPx(f.small), ImVec2(x + 11, std::floor(by + (bmH - 8 - fontPx(f.small)) * 0.5f)), ink, bm.name);
        if (hit) openWeb("Browser", bm.url, true);
        x += bw + 8;
    }
    dl->AddLine(ImVec2(0, top + barH + bmH), ImVec2(w, top + barH + bmH), IM_COL32(255, 255, 255, 28));
    webContent(dl, f, w, h, top + barH + bmH);
}

void sectionTitle(const Fonts& f, const char* t) { ImGui::PushFont(f.bold); ImGui::TextUnformatted(t); ImGui::PopFont(); }

void settingsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH;
    const float colW = std::floor(w * 0.5f) - 24;
    ImGui::SetCursorScreenPos(ImVec2(18, top + 12));
    ImGui::BeginChild("##settings", ImVec2(colW, h - top - 24), 0, ImGuiWindowFlags_NoBackground);
    sharp(ImGui::GetWindowDrawList());
    sectionTitle(f, "Display");
    float br = s.brightness;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##brightness", &br, 0.1f, 1.0f, "Brightness %.2f")) { act.setBrightness = true; act.brightness = br; }
    int slots = favSlots();   // snaps to whole numbers; 0 takes the bar away
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##favorites", &slots, 0, kMaxFavorites, slots == 0 ? "Favorites bar off" : "Favorites bar %d") && slots != favSlots()) {
        g_settings.favoriteSlots = slots; g_settingsChanged = true;
    }
    ImGui::Dummy(ImVec2(0, 4));
    const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    auto choice = [&](const char* title, const char* const names[2], int& value, bool enabled) {   // one of two, side by side
        ImGui::BeginDisabled(!enabled);   // greyed out while its icon is switched off below
        sectionTitle(f, title);
        for (int i = 0; i < 2; ++i) {
            const bool on = (value == 1 ? 1 : 0) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 1.0f, 0.85f));
            if (ImGui::Button(names[i], ImVec2(half, 32)) && !on) { value = i; g_settingsChanged = true; }
            if (on) ImGui::PopStyleColor();
            if (i == 0) ImGui::SameLine();
        }
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 4));
    };
    choice("Charts icon", kChartNames, g_settings.chartsProvider, g_settings.showCharts);
    choice("Live map icon", kLiveNames, g_settings.liveProvider, g_settings.showLive);
    sectionTitle(f, "Show on the home screen");
    struct Tog { const char* name; bool* v; };
    const Tog togs[] = {{"Charts", &g_settings.showCharts}, {"Live map", &g_settings.showLive}, {"SimBrief", &g_settings.showSimbrief}, {"ForeFlight", &g_settings.showForeflight}};
    for (int i = 0; i < 4; ++i) {
        if (ImGui::Checkbox(togs[i].name, togs[i].v)) g_settingsChanged = true;
        if (i % 2 == 0) ImGui::SameLine(half + ImGui::GetStyle().ItemSpacing.x);
    }
    ImGui::EndChild();

    // right column: status, then the touch test
    const float rx = std::floor(w * 0.5f) + 6;
    ImGui::SetCursorScreenPos(ImVec2(rx, top + 12));
    ImGui::BeginChild("##status", ImVec2(w - 18 - rx, 150), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    sectionTitle(f, "Status");
    ImGui::PushFont(f.mono);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 3));
    ImGui::Text("Version     %s", XPT_VERSION);
    ImGui::Text("Aircraft    %s%s", s.acfIcao.empty() ? "-" : s.acfIcao.c_str(), s.toliss ? " (ToLiss)" : "");
    ImGui::Text("Shown on    %s", s.onPanel ? "cockpit tablet" : "window only");
    if (s.panel[2] > 0 && s.panel[3] > 0) ImGui::Text("Panel area  %d,%d  %dx%d", s.panel[0], s.panel[1], s.panel[2], s.panel[3]);
    else ImGui::TextUnformatted("Panel area  none (no AviTab slot)");
    ImGui::Text("WXPlane     %s", s.wxPlane ? "found" : "not installed");
    ImGui::Text("Battery     %d%%%s", s.batteryPct, s.charging ? ", charging" : "");
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (s.aviTabRunning) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 190, 90, 255));
        ImGui::TextWrapped("AviTab is installed and keeps the cockpit tablet. Remove it to let XPTablet take its place.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // touch test: press anywhere in the box, a mark must appear exactly under the pointer
    const ImVec2 a(rx, top + 12 + 158), b(w - 18, h - 14);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 80), 10.0f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 40), 10.0f);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##touch", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (g_touches.empty() || std::fabs(g_touches.back().x - m.x) + std::fabs(g_touches.back().y - m.y) > 3) g_touches.push_back(m);
        if (g_touches.size() > 400) g_touches.erase(g_touches.begin());
    }
    dl->PushClipRect(a, b, true);
    for (const ImVec2& p : g_touches) dl->AddCircleFilled(p, 4.0f, IM_COL32(255, 184, 64, 220), 10);
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        dl->AddLine(ImVec2(m.x - 12, m.y), ImVec2(m.x + 12, m.y), IM_COL32(255, 255, 255, 200));
        dl->AddLine(ImVec2(m.x, m.y - 12), ImVec2(m.x, m.y + 12), IM_COL32(255, 255, 255, 200));
    }
    dl->PopClipRect();
    textCentered(dl, f.small, (a.x + b.x) * 0.5f, a.y + 8, IM_COL32(200, 212, 232, 255), g_touches.empty() ? "Touch test: press and drag here" : "Touch test");
    ImGui::SetCursorScreenPos(ImVec2(b.x - 70, b.y - 34));
    if (!g_touches.empty() && ImGui::Button("Clear", ImVec2(60, 26))) g_touches.clear();
}

void openWeb(const char* site, const char* url, bool chrome) {
    g_site = site; g_url = url; g_chrome = chrome;
    g_app = APP_BROWSER;
}

} // namespace

Fonts loadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the data is static
    cfg.OversampleH = 2; cfg.OversampleV = 1;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0};
    const float s = std::max(1.0f, pixelScale);
    Fonts f;
    f.body = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_regular, (int)xpt_asset_plex_regular_size, 15.0f * s, &cfg, ranges);
    f.bold = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_semibold, (int)xpt_asset_plex_semibold_size, 15.0f * s, &cfg, ranges);
    f.small = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_semibold, (int)xpt_asset_plex_semibold_size, 12.5f * s, &cfg, ranges);
    f.title = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_bold, (int)xpt_asset_plex_bold_size, 19.0f * s, &cfg, ranges);
    f.mono = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_mono, (int)xpt_asset_mono_size, 12.5f * s, &cfg, ranges);
    io.FontDefault = f.body;
    io.FontGlobalScale = 1.0f / s;
    return f;
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.WindowBorderSize = 0; st.FrameRounding = 8; st.GrabRounding = 8; st.ChildRounding = 8;
    st.FramePadding = ImVec2(10, 8);       // finger-sized controls
    st.ItemSpacing = ImVec2(8, 7);
    st.GrabMinSize = 26;
    st.ScrollbarSize = 16;
    st.AntiAliasedLinesUseTex = false;   // the baked line textures are made for one pixel per unit and blur when scaled
    st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.16f);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.22f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.66f, 1.0f, 1);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.78f, 1.0f, 1);
    st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.12f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.20f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.28f);
}

void build(const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##tablet", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    g_ownPerformance = s.toliss;
    if (g_app == APP_PERFORMANCE && g_ownPerformance) g_app = APP_HOME;
    g_px = std::max(1.0f, s.pixelScale);
    ImGuiStyle& st = ImGui::GetStyle();
    st.CircleTessellationMaxError = 0.30f / g_px;   // curves are as smooth on a dense screen as on a plain one
    st.CurveTessellationTol = 1.25f / g_px;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    sharp(dl);
    wallpaper(dl, w, h);
    statusBar(dl, f, s, w);
    if (g_app < 0 || g_app >= APP_COUNT) {
        home(dl, f, w, h);
    } else {
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(8, 18, 36, 170));   // pages sit on a darkened wallpaper
        const int app = g_app;
        if (pageHeader(dl, f, w, app == APP_BROWSER ? g_site.c_str() : appName(app))) g_app = APP_HOME;
        if (app == APP_SETTINGS) settingsPage(dl, f, s, w, h, act);
        else if (app == APP_BROWSER) browserPage(dl, f, w, h);
        else placeholderPage(dl, f, app, w, h);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

int currentApp() { return g_app; }
const char* browserUrl() { return g_url.c_str(); }
bool browserChrome() { return g_chrome; }

const char* appName(int app) {
    if (app == APP_CHARTS) return kChartNames[chartsP()];
    if (app == APP_LIVEMAP) return kLiveNames[liveP()];
    return (app >= 0 && app < APP_COUNT) ? kApps[app].name : "Home";
}

bool appVisible(int app) {
    switch (app) {
    case APP_CHARTS: return g_settings.showCharts;
    case APP_SIMBRIEF: return g_settings.showSimbrief;
    case APP_LIVEMAP: return g_settings.showLive;
    case APP_FOREFLIGHT: return g_settings.showForeflight;
    case APP_PERFORMANCE: return !g_ownPerformance;
    default: return app >= 0 && app < APP_COUNT;
    }
}

// Charts, Live Map, SimBrief and ForeFlight are sites shown as pages of their own. The Browser icon opens the full browser.
void openApp(int app) {
    if (app < 0 || app >= APP_COUNT || !appVisible(app)) { g_app = APP_HOME; return; }
    if (app == APP_CHARTS) { openWeb(kChartNames[chartsP()], kChartUrls[chartsP()], false); return; }
    if (app == APP_LIVEMAP) { openWeb(kLiveNames[liveP()], kLiveUrls[liveP()], false); return; }
    if (kApps[app].url) { openWeb(kApps[app].name, kApps[app].url, false); return; }
    if (app == APP_BROWSER) { g_site = "Browser"; g_chrome = true; }
    g_app = app;
}

Settings& settings() { return g_settings; }
bool takeSettingsChanged() { const bool c = g_settingsChanged; g_settingsChanged = false; return c; }

std::string settingsToText() {
    normalizeLayout();
    char buf[256];
    std::snprintf(buf, sizeof buf, "charts_provider=%d\nlive_provider=%d\nfavorite_slots=%d\nshow_charts=%d\nshow_simbrief=%d\nshow_live=%d\nshow_foreflight=%d\n", chartsP(), liveP(), favSlots(), g_settings.showCharts ? 1 : 0,
                  g_settings.showSimbrief ? 1 : 0, g_settings.showLive ? 1 : 0, g_settings.showForeflight ? 1 : 0);
    std::string out = buf;
    auto list = [&](const char* key, const std::vector<int>& v) {
        out += key; out += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ","; out += kApps[v[i]].id; }
        out += "\n";
    };
    list("order", g_settings.order);
    list("favorites", g_settings.favorites);
    return out;
}

void settingsFromText(const std::string& text) {
    auto parseList = [](const std::string& val, std::vector<int>& v) {
        v.clear();
        size_t p = 0;
        while (p <= val.size()) {
            size_t e = val.find(',', p);
            if (e == std::string::npos) e = val.size();
            const std::string id = val.substr(p, e - p);
            for (int a = 0; a < APP_COUNT; ++a) if (id == kApps[a].id) v.push_back(a);
            p = e + 1;
        }
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        const int v = std::atoi(val.c_str());
        if (key == "charts_provider") g_settings.chartsProvider = v == 1 ? 1 : 0;
        else if (key == "live_provider") g_settings.liveProvider = v == 1 ? 1 : 0;
        else if (key == "favorite_slots") g_settings.favoriteSlots = std::max(0, std::min(kMaxFavorites, v));
        else if (key == "show_charts") g_settings.showCharts = v != 0;
        else if (key == "show_simbrief") g_settings.showSimbrief = v != 0;
        else if (key == "show_live") g_settings.showLive = v != 0;
        else if (key == "show_foreflight") g_settings.showForeflight = v != 0;
        else if (key == "order") parseList(val, g_settings.order);
        else if (key == "favorites") parseList(val, g_settings.favorites);
    }
    normalizeLayout();
}

} // namespace xpt
