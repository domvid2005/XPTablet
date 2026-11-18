#include "tabletui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "metar.h"
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
extern const unsigned char xpt_asset_wxlogo[];
extern const unsigned int xpt_asset_wxlogo_size;
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

bool g_wx = false;             // WXPlane is installed (told by the host each frame): the Weather icon and page are WXPlane's
ImTextureID g_wxLogo = 0;
Settings g_settings;
bool g_settingsChanged = false;
std::string g_url = "https://chartfox.org", g_site = "Browser";   // what the web page shows
int favSlots() { return std::max(0, std::min(kMaxFavorites, g_settings.favoriteSlots)); }
bool g_ownPerformance = false;   // the aircraft brings its own performance calculator (ToLiss): no Performance app
bool g_chrome = true;                                             // with the browser's own controls

void openWeb(const char* site, const char* url, bool chrome);

int chartsP() { return g_settings.chartsProvider == 1 ? 1 : 0; }
int liveP() { return g_settings.liveProvider == 1 ? 1 : 0; }
ImU32 appColor(int app) { return app == APP_LIVEMAP ? kLiveColors[liveP()] : (app == APP_WEATHER && g_wx) ? IM_COL32(22, 24, 29, 255) : kApps[app].color; }
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
EmbedArea g_embed;
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
    if (app == APP_WEATHER && g_wx && g_wxLogo) {   // WXPlane's own logo, WX above PLANE, on its dark tile
        const float r = tile * 0.46f * scale;
        dl->AddImage(g_wxLogo, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
    } else {
        icon(dl, appIcon(app), c, tile * 0.32f * scale, IM_COL32(255, 255, 255, 255));
    }
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

// ---- small pieces the pages share ---------------------------------------------------------------------------------------------------
const ImU32 kCard = IM_COL32(10, 22, 44, 210), kCardLine = IM_COL32(255, 255, 255, 30), kInk = IM_COL32(236, 241, 250, 255),
            kMuted = IM_COL32(150, 168, 198, 255), kGood = IM_COL32(80, 214, 124, 255), kWarn = IM_COL32(255, 190, 90, 255);

void card(ImDrawList* dl, ImVec2 a, ImVec2 b, const Fonts& f, const char* title) {
    dl->AddRectFilled(a, b, kCard, 12.0f);
    dl->AddRect(a, b, kCardLine, 12.0f);
    if (title) dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 14, a.y + 10), kMuted, title);
}
void textAt(ImDrawList* dl, ImFont* font, float x, float y, ImU32 col, const char* txt) { dl->AddText(font, fontPx(font), ImVec2(x, y), col, txt); }
void textRight(ImDrawList* dl, ImFont* font, float xRight, float y, ImU32 col, const char* txt) {
    const ImVec2 sz = font->CalcTextSizeA(fontPx(font), 1e9f, 0, txt);
    dl->AddText(font, fontPx(font), ImVec2(std::floor(xRight - sz.x), y), col, txt);
}
// A rounded button drawn by hand at a fixed place. on: shown lit. enabled: can be pressed.
bool pill(ImDrawList* dl, const Fonts& f, const char* id, const char* label, ImVec2 a, ImVec2 b, bool on, bool enabled) {
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const ImU32 bg = !enabled ? IM_COL32(255, 255, 255, 14) : on ? IM_COL32(70, 140, 255, down ? 255 : 220) : IM_COL32(255, 255, 255, down ? 70 : hot ? 50 : 30);
    dl->AddRectFilled(a, b, bg, (b.y - a.y) * 0.5f);
    textCentered(dl, f.small, (a.x + b.x) * 0.5f, std::floor((a.y + b.y - fontPx(f.small)) * 0.5f), enabled ? kInk : IM_COL32(236, 241, 250, 90), label);
    return hit && enabled;
}

// ---- Weather without WXPlane: X-Plane's own weather where the aircraft is ---------------------------------------------------------------
const char* coverWord(float c) { return c < 0.06f ? "CLR" : c < 0.30f ? "FEW" : c < 0.55f ? "SCT" : c < 0.90f ? "BKN" : "OVC"; }

// The METAR search: an airport code typed on the page's own keys (the cockpit tablet has no keyboard), the report as received
// and what it says.
int g_wxTab = 0;              // 0 the conditions at the aircraft, 1 the METAR search
std::string g_metarInput;

void metarPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH + 10, bot = h - 12, gap = 10;
    const float lw = std::floor(w * 0.52f);
    char b[160];
    // the code and the keys
    const ImVec2 a(12, top), e(12 + lw, bot);
    card(dl, a, e, f, "AIRPORT");
    const float ix = a.x + 14, iw = lw - 28;
    const ImVec2 i0(ix, a.y + 34), i1(ix + iw - 110, a.y + 34 + 40);
    dl->AddRectFilled(i0, i1, IM_COL32(255, 255, 255, 22), 10.0f);
    dl->AddRect(i0, i1, IM_COL32(110, 170, 255, 160), 10.0f);
    if (g_metarInput.empty()) textAt(dl, f.body, i0.x + 14, i0.y + 11, kMuted, "ICAO code, e.g. KBOS");
    else textAt(dl, f.title, i0.x + 14, i0.y + 9, kInk, g_metarInput.c_str());
    const bool can = g_metarInput.size() >= 3 && !s.metar.busy;
    if (pill(dl, f, "##msearch", s.metar.busy ? "Searching" : "Search", ImVec2(i1.x + 10, i0.y + 2), ImVec2(ix + iw, i1.y - 2), can, can)) act.metarSearch = g_metarInput;
    const char* rowsK[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    const float kg = 5, kw = std::floor((iw - 9 * kg) / 10), ky0 = i1.y + 14, kh = std::min(42.0f, std::floor((e.y - 14 - ky0 - 3 * kg) / 4));
    for (int r = 0; r < 4; ++r) {
        const int n = (int)std::strlen(rowsK[r]);
        float x = ix + (r == 2 ? (kw + kg) * 0.5f : r == 3 ? (kw + kg) * 1.0f : 0.0f);
        const float y = ky0 + r * (kh + kg);
        for (int k = 0; k < n; ++k, x += kw + kg) {
            char id[8], lbl[2] = {rowsK[r][k], 0};
            std::snprintf(id, sizeof id, "##k%c", rowsK[r][k]);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            const bool hit = ImGui::InvisibleButton(id, ImVec2(kw, kh));
            const bool down = ImGui::IsItemActive(), hot = ImGui::IsItemHovered();
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + kw, y + kh), IM_COL32(255, 255, 255, down ? 90 : hot ? 52 : 32), 8.0f);
            textCentered(dl, f.bold, x + kw * 0.5f, std::floor(y + (kh - fontPx(f.bold)) * 0.5f), kInk, lbl);
            if (hit && g_metarInput.size() < 4) g_metarInput += rowsK[r][k];
        }
        if (r == 3) {   // backspace, to the right of the last row
            const float bx = x, bw = ix + iw - bx;
            ImGui::SetCursorScreenPos(ImVec2(bx, y));
            const bool hit = ImGui::InvisibleButton("##kback", ImVec2(bw, kh));
            dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + kh), IM_COL32(255, 255, 255, ImGui::IsItemActive() ? 90 : 32), 8.0f);
            textCentered(dl, f.small, bx + bw * 0.5f, std::floor(y + (kh - fontPx(f.small)) * 0.5f), kInk, "Delete");
            if (hit && !g_metarInput.empty()) g_metarInput.pop_back();
        }
    }
    // the report
    const ImVec2 ra(e.x + gap, top), re(w - 12, bot);
    card(dl, ra, re, f, "REPORT");
    const float tx = ra.x + 14, tw = re.x - ra.x - 28;
    float y = ra.y + 36;
    const MetarLookup& m = s.metar;
    if (m.raw.empty()) {
        const char* msg = m.busy ? "Asking NOAA..." : !m.error.empty() ? m.error.c_str() : "Type an airport's ICAO code and press Search.";
        dl->AddText(f.body, fontPx(f.body), ImVec2(tx, y), m.error.empty() || m.busy ? kMuted : kWarn, msg, nullptr, tw);
        return;
    }
    wx::Report r;
    wx::parseReport(m.raw, r);
    // flight category from the ceiling and the visibility
    int ceil = -1;
    for (const wx::CloudGroup& c : r.clouds) if (c.cover >= wx::CloudGroup::BKN && (ceil < 0 || c.baseFtAgl < ceil)) ceil = c.baseFtAgl;
    const double visSm = r.cavok ? 10.0 : r.hasVis ? r.visM / wx::kSmToM : -1;
    const char* cat = ""; ImU32 catCol = kGood;
    if (visSm >= 0 || ceil >= 0) {
        const double v = visSm < 0 ? 99 : visSm; const int c = ceil < 0 ? 99999 : ceil;
        if (v < 1 || c < 500) { cat = "LIFR"; catCol = IM_COL32(230, 110, 230, 255); }
        else if (v < 3 || c < 1000) { cat = "IFR"; catCol = IM_COL32(240, 84, 84, 255); }
        else if (v <= 5 || c <= 3000) { cat = "MVFR"; catCol = IM_COL32(110, 170, 255, 255); }
        else { cat = "VFR"; catCol = kGood; }
    }
    textAt(dl, f.title, tx, y, kInk, m.icao.c_str());
    if (*cat) textRight(dl, f.bold, re.x - 14, y + 3, catCol, cat);
    y += 30;
    const ImVec2 rs = f.mono->CalcTextSizeA(fontPx(f.mono), 1e9f, tw - 20, m.raw.c_str());
    dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + tw, y + rs.y + 16), IM_COL32(0, 0, 0, 80), 8.0f);
    dl->AddText(f.mono, fontPx(f.mono), ImVec2(tx + 10, y + 8), IM_COL32(180, 230, 190, 255), m.raw.c_str(), nullptr, tw - 20);
    y += rs.y + 28;
    auto row = [&](const char* k, const char* v) { if (y + 20 > re.y - 8) return; textAt(dl, f.body, tx, y, kMuted, k); textRight(dl, f.bold, re.x - 14, y, kInk, v); y += 23; };
    if (r.hour >= 0) { std::snprintf(b, sizeof b, "%02d:%02dZ", r.hour, r.minute); row("Observed", b); }
    if (r.hasWind) {
        if (r.windKt < 1) std::snprintf(b, sizeof b, "calm");
        else if (r.windVariable) std::snprintf(b, sizeof b, "variable / %d kt", (int)std::lround(r.windKt));
        else if (r.gustKt > r.windKt) std::snprintf(b, sizeof b, "%03d\xC2\xB0 / %d G%d kt", r.windDir, (int)std::lround(r.windKt), (int)std::lround(r.gustKt));
        else std::snprintf(b, sizeof b, "%03d\xC2\xB0 / %d kt", r.windDir, (int)std::lround(r.windKt));
        row("Wind", b);
    }
    if (visSm >= 0) { if (visSm >= 10) std::snprintf(b, sizeof b, "10+ SM"); else std::snprintf(b, sizeof b, "%.1f SM  (%d m)", visSm, (int)(std::lround(r.visM / 100) * 100)); row("Visibility", b); }
    if (!r.wxText.empty()) row("Weather", r.wxText.c_str());
    {
        std::string c;
        static const char* cov[5] = {"FEW", "SCT", "BKN", "OVC", "VV"};
        for (const wx::CloudGroup& g : r.clouds) { std::snprintf(b, sizeof b, "%s%s%03d", c.empty() ? "" : "  ", cov[std::max(0, std::min(4, g.cover))], g.baseFtAgl / 100); c += b; }
        if (c.empty() && (r.clearSky || r.cavok)) c = "clear";
        if (!c.empty()) row("Clouds", c.c_str());
    }
    if (r.hasTemp) { if (r.hasDewp) std::snprintf(b, sizeof b, "%d / %d \xC2\xB0""C", (int)std::lround(r.tempC), (int)std::lround(r.dewpC)); else std::snprintf(b, sizeof b, "%d \xC2\xB0""C", (int)std::lround(r.tempC)); row("Temp / Dew", b); }
    if (r.hasAltim) { std::snprintf(b, sizeof b, "%d hPa  %.2f in", (int)std::lround(r.altimHpa), r.altimHpa / wx::kInHgToHpa); row("QNH", b); }
}

void simWeatherPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act, bool wxNotOnTablet) {
    // two views, switched at the right end of the title bar
    {
        const float y0 = kStatusH + 9, bw = 108;
        if (pill(dl, f, "##wxcond", "At the aircraft", ImVec2(w - 14 - 2 * bw - 6, y0), ImVec2(w - 14 - bw - 6, y0 + 28), g_wxTab == 0, true)) g_wxTab = 0;
        if (pill(dl, f, "##wxmetar", "METAR search", ImVec2(w - 14 - bw, y0), ImVec2(w - 14, y0 + 28), g_wxTab == 1, true)) g_wxTab = 1;
    }
    if (g_wxTab == 1) { metarPage(dl, f, s, w, h, act); return; }
    const SimWeather& x = s.wx;
    const float top = kStatusH + kHeaderH + 10, bot = h - 34, gap = 10;
    const float c0 = 12, cw = (w - 2 * c0 - 2 * gap) / 3;
    char b[96];
    // 1: at the aircraft
    {
        const ImVec2 a(c0, top), e(c0 + cw, bot);
        card(dl, a, e, f, "AT THE AIRCRAFT");
        const ImVec2 c(a.x + cw * 0.5f, a.y + 100);
        const float r = 52;
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 48, 2.0f);
        for (int i = 0; i < 4; ++i) {
            const float an = i * kPi * 0.5f - kPi * 0.5f;
            textCentered(dl, f.small, c.x + std::cos(an) * (r + 11), c.y + std::sin(an) * (r + 11) - fontPx(f.small) * 0.5f, kMuted, i == 0 ? "N" : i == 1 ? "E" : i == 2 ? "S" : "W");
        }
        if (x.valid && x.windKt >= 1) {   // an arrow flying with the wind: from where it blows, through the centre
            const float an = (x.windDir - 90) * kPi / 180;
            const ImVec2 from(c.x + std::cos(an) * (r - 6), c.y + std::sin(an) * (r - 6)), to(c.x - std::cos(an) * (r - 22), c.y - std::sin(an) * (r - 22));
            dl->AddLine(from, to, IM_COL32(110, 170, 255, 255), 3.0f);
            const ImVec2 n(std::cos(an), std::sin(an)), t(-n.y, n.x);
            dl->AddTriangleFilled(ImVec2(to.x - n.x * 4, to.y - n.y * 4), ImVec2(to.x + n.x * 10 + t.x * 7, to.y + n.y * 10 + t.y * 7), ImVec2(to.x + n.x * 10 - t.x * 7, to.y + n.y * 10 - t.y * 7), IM_COL32(110, 170, 255, 255));
        }
        float y = a.y + 176;
        auto row = [&](const char* k, const char* v) { textAt(dl, f.body, a.x + 14, y, kMuted, k); textRight(dl, f.bold, e.x - 14, y, kInk, v); y += 24; };
        if (!x.valid) { textCentered(dl, f.body, c.x, y, kMuted, "No weather data from the sim yet."); }
        else {
            if (x.windKt < 1) std::snprintf(b, sizeof b, "calm"); else std::snprintf(b, sizeof b, "%03d\xC2\xB0 / %d kt", ((int)std::lround(x.windDir) + 359) % 360 + 1, (int)std::lround(x.windKt));
            row("Wind", b);
            if (x.visSm >= 10) std::snprintf(b, sizeof b, "10+ SM"); else std::snprintf(b, sizeof b, "%.1f SM", x.visSm);
            row("Visibility", b);
            std::snprintf(b, sizeof b, "%d / %d \xC2\xB0""C", (int)std::lround(x.tempC), (int)std::lround(x.dewC));
            row("Temp / Dew", b);
            std::snprintf(b, sizeof b, "%d hPa  %.2f in", (int)std::lround(x.qnhHpa), x.qnhHpa / 33.8639f);
            row("QNH", b);
            row("Rain", x.rain < 0.02f ? "none" : x.rain < 0.3f ? "light" : x.rain < 0.7f ? "moderate" : "heavy");
        }
    }
    // 2: cloud layers, as a column of sky with the aircraft's height marked
    {
        const ImVec2 a(c0 + cw + gap, top), e(a.x + cw, bot);
        card(dl, a, e, f, "CLOUDS");
        const float y0 = a.y + 40, y1 = e.y - 16, colX = a.x + 16, colW = 46;
        float topFt = 20000;
        for (int i = 0; i < x.clouds; ++i) topFt = std::max(topFt, x.cloudTopFt[i] + 3000);
        topFt = std::max(topFt, x.altFt + 3000);
        auto yOf = [&](float ft) { return y1 - (y1 - y0) * std::max(0.0f, std::min(1.0f, ft / topFt)); };
        dl->AddRectFilledMultiColor(ImVec2(colX, y0), ImVec2(colX + colW, y1), IM_COL32(20, 50, 110, 255), IM_COL32(20, 50, 110, 255), IM_COL32(70, 130, 210, 255), IM_COL32(70, 130, 210, 255));
        float ty = y0;
        if (!x.valid || x.clouds == 0) textAt(dl, f.body, colX + colW + 14, y0, kInk, x.valid ? "Sky clear" : "No data");
        for (int i = x.clouds - 1; i >= 0; --i) {   // highest first, so the list reads top down like the sky
            const float yb = yOf(x.cloudBaseFt[i]), yt = yOf(x.cloudTopFt[i]);
            dl->AddRectFilled(ImVec2(colX, std::min(yt, yb - 3)), ImVec2(colX + colW, yb), IM_COL32(236, 241, 250, (int)(90 + 150 * std::min(1.0f, x.cloudCover[i]))), 3.0f);
            std::snprintf(b, sizeof b, "%s  %d ft", coverWord(x.cloudCover[i]), (int)(std::lround(x.cloudBaseFt[i] / 100) * 100));
            textAt(dl, f.bold, colX + colW + 14, ty, kInk, b);
            std::snprintf(b, sizeof b, "tops %d ft", (int)(std::lround(x.cloudTopFt[i] / 100) * 100));
            textAt(dl, f.small, colX + colW + 14, ty + 19, kMuted, b);
            ty += 46;
        }
        if (x.valid) {   // the aircraft
            const float ya = yOf(x.altFt);
            dl->AddTriangleFilled(ImVec2(colX - 9, ya - 5), ImVec2(colX - 9, ya + 5), ImVec2(colX - 1, ya), kWarn);
            dl->AddLine(ImVec2(colX, ya), ImVec2(colX + colW, ya), kWarn, 1.5f);
        }
    }
    // 3: winds aloft
    {
        const ImVec2 a(c0 + 2 * (cw + gap), top), e(a.x + cw, bot);
        card(dl, a, e, f, "WINDS ALOFT");
        float y = a.y + 38;
        textAt(dl, f.small, a.x + 14, y, kMuted, "Altitude"); textRight(dl, f.small, e.x - 14, y, kMuted, "From / kt");
        y += 22;
        const int rows = (int)((e.y - 12 - y) / 22);
        int step = 1;
        while (x.aloft > 0 && (x.aloft + step - 1) / step > rows) ++step;
        if (!x.valid || x.aloft == 0) textAt(dl, f.body, a.x + 14, y, kMuted, "No data");
        for (int i = x.aloft - 1; i >= 0; i -= step) {   // highest first
            if (x.aloftFt[i] >= 18000) std::snprintf(b, sizeof b, "FL%03d", (int)std::lround(x.aloftFt[i] / 100)); else std::snprintf(b, sizeof b, "%d ft", (int)(std::lround(x.aloftFt[i] / 100) * 100));
            const bool here = std::fabs(x.aloftFt[i] - x.altFt) < 1500;
            textAt(dl, here ? f.bold : f.body, a.x + 14, y, here ? kWarn : kInk, b);
            std::snprintf(b, sizeof b, "%03d\xC2\xB0 / %d", ((int)std::lround(x.aloftDir[i]) + 359) % 360 + 1, (int)std::lround(x.aloftKt[i]));
            textRight(dl, f.mono, e.x - 14, y + 1, kInk, b);
            y += 22;
        }
    }
    if (wxNotOnTablet)
        textCentered(dl, f.small, w * 0.5f, h - 24, kWarn, "The installed WXPlane does not show on the tablet yet: update WXPlane and it appears here. Until then, X-Plane's own weather.");
    else
        textCentered(dl, f.small, w * 0.5f, h - 24, kMuted, "X-Plane's own weather where you are. Airport reports: METAR search, top right. A radar map comes later.");
}

// ---- Ground: jetway, ground power, chocks, service vehicles, pushback, and on a ToLiss its doors -------------------------------------------
// Aircraft are drawn from their real measurements, so every type comes out at its true shape and the fleet at one scale.
// Lengths in metres. eng: 2 or 4 under the wings, -2 two at the rear fuselage, -1 one jet at the rear, 1 one propeller on the
// nose, 12 two propellers on the wings. sweep: of the wing's leading edge, degrees.
struct PlaneSpec { const char* icao; float len, span, fus; int eng; float sweep; };
const PlaneSpec kPlanes[] = {
    {"A318", 31.44f, 34.10f, 3.95f, 2, 25}, {"A319", 33.84f, 35.80f, 3.95f, 2, 25}, {"A19N", 33.84f, 35.80f, 3.95f, 2, 25},
    {"A320", 37.57f, 35.80f, 3.95f, 2, 25}, {"A20N", 37.57f, 35.80f, 3.95f, 2, 25}, {"A321", 44.51f, 35.80f, 3.95f, 2, 25},
    {"A21N", 44.51f, 35.80f, 3.95f, 2, 25}, {"A332", 58.82f, 60.30f, 5.64f, 2, 30}, {"A333", 63.66f, 60.30f, 5.64f, 2, 30},
    {"A338", 58.82f, 64.00f, 5.64f, 2, 30}, {"A339", 63.66f, 64.00f, 5.64f, 2, 30}, {"A342", 59.40f, 60.30f, 5.64f, 4, 30},
    {"A343", 63.69f, 60.30f, 5.64f, 4, 30}, {"A345", 67.93f, 63.45f, 5.64f, 4, 31}, {"A346", 75.36f, 63.45f, 5.64f, 4, 31},
    {"A359", 66.80f, 64.75f, 5.96f, 2, 32}, {"A35K", 73.79f, 64.75f, 5.96f, 2, 32}, {"A388", 72.72f, 79.75f, 7.14f, 4, 33},
    {"B737", 33.60f, 35.80f, 3.76f, 2, 25}, {"B738", 39.47f, 35.80f, 3.76f, 2, 25}, {"B739", 42.10f, 35.80f, 3.76f, 2, 25},
    {"B38M", 39.52f, 35.92f, 3.76f, 2, 25}, {"B39M", 42.16f, 35.92f, 3.76f, 2, 25}, {"B752", 47.32f, 38.05f, 3.76f, 2, 25},
    {"B753", 54.47f, 38.05f, 3.76f, 2, 25}, {"B762", 48.51f, 47.57f, 5.03f, 2, 31}, {"B763", 54.94f, 47.57f, 5.03f, 2, 31},
    {"B772", 63.73f, 60.93f, 6.20f, 2, 31}, {"B77L", 63.73f, 64.80f, 6.20f, 2, 31}, {"B77W", 73.86f, 64.80f, 6.20f, 2, 31},
    {"B788", 56.72f, 60.12f, 5.77f, 2, 32}, {"B789", 62.81f, 60.12f, 5.77f, 2, 32}, {"B78X", 68.28f, 60.12f, 5.77f, 2, 32},
    {"B744", 70.66f, 64.44f, 6.50f, 4, 37}, {"B748", 76.25f, 68.40f, 6.50f, 4, 37},
    {"E170", 29.90f, 26.00f, 3.01f, 2, 23}, {"E175", 31.68f, 26.00f, 3.01f, 2, 23}, {"E190", 36.24f, 28.72f, 3.01f, 2, 23},
    {"E195", 38.65f, 28.72f, 3.01f, 2, 23}, {"BCS1", 35.00f, 35.10f, 3.70f, 2, 25}, {"BCS3", 38.70f, 35.10f, 3.70f, 2, 25},
    {"CRJ2", 26.77f, 21.21f, 2.69f, -2, 24}, {"CRJ7", 32.30f, 23.20f, 2.69f, -2, 25}, {"CRJ9", 36.20f, 24.85f, 2.69f, -2, 25},
    {"MD82", 45.06f, 32.87f, 3.34f, -2, 24}, {"MD83", 45.06f, 32.87f, 3.34f, -2, 24}, {"MD88", 45.06f, 32.87f, 3.34f, -2, 24},
    {"B712", 37.80f, 28.45f, 3.34f, -2, 24}, {"CL60", 20.85f, 19.61f, 2.69f, -2, 25}, {"GLF5", 29.40f, 28.50f, 2.40f, -2, 27},
    {"C25A", 14.53f, 15.19f, 1.60f, -2, 0}, {"C510", 12.37f, 13.16f, 1.50f, -2, 0}, {"E55P", 15.64f, 15.91f, 1.80f, -2, 20},
    {"SF50", 9.42f, 11.79f, 1.56f, -1, 0},
    {"C172", 8.28f, 11.00f, 1.07f, 1, 0}, {"C152", 7.34f, 10.16f, 1.02f, 1, 0}, {"SR22", 7.92f, 11.68f, 1.27f, 1, 0},
    {"P28A", 7.16f, 9.14f, 1.07f, 1, 0}, {"PC12", 14.40f, 16.28f, 1.53f, 1, 0}, {"TBM9", 10.74f, 12.83f, 1.30f, 1, 0},
    {"C208", 11.46f, 15.87f, 1.62f, 1, 0}, {"BE58", 9.09f, 11.53f, 1.07f, 12, 0}, {"BE9L", 10.82f, 15.32f, 1.45f, 12, 0},
    {"B350", 14.22f, 17.65f, 1.45f, 12, 0}, {"DA42", 8.56f, 13.42f, 1.20f, 12, 0},
};
const PlaneSpec& planeSpec(const std::string& icao) {
    for (const PlaneSpec& p : kPlanes) if (icao == p.icao) return p;
    // an unknown type: the nearest in the family by its first three letters, else a mid-size airliner
    for (const PlaneSpec& p : kPlanes) if (icao.size() >= 3 && icao.compare(0, 3, p.icao, 3) == 0) return p;
    static const PlaneSpec generic = {"", 37.57f, 35.80f, 3.95f, 2, 25};
    return generic;
}

// The aircraft from above, nose up, its nose at `nose` and `px` screen units per metre.
void planeTopDown(ImDrawList* dl, ImVec2 nose, float px, const PlaneSpec& p, ImU32 col, ImU32 shade2) {
    const float L = p.len, hw = p.fus * 0.5f, semi = p.span * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(nose.x + x * px, nose.y + y * px); };   // metres: x right, y aft of the nose
    const bool prop = p.eng == 1 || p.eng == 12, rear = p.eng < 0;
    // mirrored shapes are given in the same turning direction on both sides (ImGui's smooth edges depend on it)
    auto quad = [&](float s, ImVec2 q0, ImVec2 q1, ImVec2 q2, ImVec2 q3) { if (s > 0) dl->AddQuadFilled(q0, q1, q2, q3, col); else dl->AddQuadFilled(q3, q2, q1, q0, col); };
    const float sw = std::tan(p.sweep * kPi / 180.0f);
    // wing: root chord and where it sits along the fuselage
    const float cr = prop ? p.span * 0.15f : p.span * 0.17f, ct = prop ? p.span * 0.09f : p.span * 0.045f;
    const float mid = prop ? 0.34f : rear ? 0.56f : 0.46f;                      // the root's mid-chord along the fuselage
    const float yr = L * mid - cr * 0.5f;                                       // root leading edge
    const float kink = prop ? 0.0f : 0.34f;                                     // inboard part with a straight trailing edge
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float s = (float)sgn;
        const float xk = hw + (semi - hw) * kink, yk = yr + (xk - hw * 0.5f) * sw, ytip = yr + (semi - hw * 0.5f) * sw;
        if (kink > 0) {
            quad(s, P(s * hw * 0.5f, yr), P(s * xk, yk), P(s * xk, yr + cr * 0.98f), P(s * hw * 0.5f, yr + cr));
            quad(s, P(s * xk, yk), P(s * semi, ytip), P(s * semi, ytip + ct), P(s * xk, yr + cr * 0.98f));
        } else {
            quad(s, P(s * hw * 0.5f, yr), P(s * semi, ytip), P(s * semi, ytip + ct), P(s * hw * 0.5f, yr + cr));
        }
        // tailplane
        const float ts = p.span * (prop ? 0.17f : 0.175f), tc = cr * (prop ? 0.50f : 0.60f), ty = L - tc * 1.25f, tsw = prop ? 0.1f : 0.60f;
        quad(s, P(s * hw * 0.3f, ty), P(s * ts, ty + ts * tsw), P(s * ts, ty + ts * tsw + tc * 0.40f), P(s * hw * 0.3f, ty + tc));
        // engines
        const float nl = std::min(p.fus * 1.15f, p.span * 0.11f), nd = nl * 0.52f;
        auto nacelle = [&](float x, float y) { dl->AddRectFilled(P(s * x - nd * 0.5f, y), P(s * x + nd * 0.5f, y + nl), col, nd * 0.5f * px); };
        if (p.eng == 2 || p.eng == 4) {
            const float x1 = hw + (semi - hw) * (p.eng == 4 ? 0.30f : 0.31f);
            nacelle(x1, yr + (x1 - hw * 0.5f) * sw - nl * 0.62f);
            if (p.eng == 4) { const float x2 = hw + (semi - hw) * 0.60f; nacelle(x2, yr + (x2 - hw * 0.5f) * sw - nl * 0.62f); }
        } else if (p.eng == -2) {
            nacelle(hw + nd * 0.62f, L * 0.74f);
        } else if (p.eng == 12) {
            const float x1 = hw + (semi - hw) * 0.26f;
            nacelle(x1, yr - nl * 0.55f);
            dl->AddLine(P(s * x1 - nd * 1.5f, yr - nl * 0.55f), P(s * x1 + nd * 1.5f, yr - nl * 0.55f), shade2, 2.0f);   // propeller disc
        }
    }
    if (p.eng == -1) dl->AddRectFilled(P(-p.fus * 0.22f, L * 0.62f), P(p.fus * 0.22f, L * 0.80f), shade2, p.fus * 0.22f * px);   // one engine on top
    // fuselage: a rounded nose, a straight body and a tail cone
    const float noseLen = p.fus * (prop ? 1.4f : 1.7f), tailLen = p.fus * (prop ? 3.4f : 3.0f);
    const int N = 12;
    dl->PathClear();
    for (int i = 0; i <= N; ++i) { const float a = kPi * 0.5f * i / N; dl->PathLineTo(P(-hw * std::cos(a), noseLen * (1 - std::sin(a)))); }   // left side: body shoulder up to the tip
    for (int i = N - 1; i >= 0; --i) { const float a = kPi * 0.5f * i / N; dl->PathLineTo(P(hw * std::cos(a), noseLen * (1 - std::sin(a)))); }  // tip down the right side
    dl->PathLineTo(P(hw, L - tailLen));
    dl->PathLineTo(P(hw * 0.22f, L));
    dl->PathLineTo(P(-hw * 0.22f, L));
    dl->PathLineTo(P(-hw, L - tailLen));
    dl->PathFillConvex(col);
    if (p.eng == 1) dl->AddLine(P(-p.span * 0.085f, 0.05f), P(p.span * 0.085f, 0.05f), shade2, 2.0f);   // the propeller
    dl->AddLine(P(0, L - tailLen * 0.95f), P(0, L - 0.2f), shade2, std::max(1.5f, p.fus * 0.07f * px));   // the fin, seen edge-on
    dl->AddLine(P(-hw * 0.55f, noseLen * 0.62f), P(hw * 0.55f, noseLen * 0.62f), shade2, 1.5f);          // the windscreen
}

// ---- the aircraft top views (AircraftShapesSVG by RexKramer1 and amnesica, GPL-3.0), read from the plugin's shapes folder --------
struct Shape {
    std::string name;
    float len = 0, span = 0;                       // metres, as stored
    std::vector<std::vector<ImVec2>> fills, lines; // metres, nose at y = 0, centreline at x = 0
};
struct ShapeAlias { std::string icao; int shape; float len, span; };
std::vector<Shape> g_shapes;
std::vector<ShapeAlias> g_aliases;

// The drawing for a type and the size to draw it at: its own, or (for a type without one) its family's resized to the type.
const Shape* findShape(const std::string& icao, float& len, float& span) {
    for (const ShapeAlias& a : g_aliases) if (a.icao == icao) { len = a.len; span = a.span; return &g_shapes[a.shape]; }
    for (const Shape& sh : g_shapes) if (sh.name == icao) { len = sh.len; span = sh.span; return &sh; }
    return nullptr;
}

// Half the fuselage's width (metres) at a point frac of the way back: of the outline's crossings at that height, the one nearest
// near the width at a quarter of the way back (where nothing but the fuselage is), so wings and engines are passed over.
float fuselageHalf(const Shape& sh, float frac, float ref) {
    const float y = frac * sh.len;
    float best = -1;
    for (const std::vector<ImVec2>& p : sh.fills)
        for (size_t i = 0, n = p.size(); i < n; ++i) {
            const ImVec2 a = p[i], b = p[(i + 1) % n];
            if ((a.y > y) == (b.y > y) || a.y == b.y) continue;
            const float x = std::fabs(a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y));
            if (ref <= 0 ? (best < 0 || x < best) : (best < 0 || std::fabs(x - ref) < std::fabs(best - ref))) best = x;
        }
    return best;
}

void drawShape(ImDrawList* dl, const Shape& sh, ImVec2 nose, float pxX, float pxY, ImU32 fill, ImU32 line) {
    std::vector<ImVec2> pts;
    auto map = [&](const std::vector<ImVec2>& p) {
        pts.resize(p.size());
        for (size_t i = 0; i < p.size(); ++i) pts[i] = ImVec2(nose.x + p[i].x * pxX, nose.y + p[i].y * pxY);
    };
    for (const std::vector<ImVec2>& p : sh.fills) { map(p); if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), fill); }
    for (const std::vector<ImVec2>& p : sh.lines) { map(p); if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), line, 0, 1.0f); }
}

// Where a ToLiss's doors are along the fuselage, as fractions of its length from the nose (cabin doors front to back, then the
// cargo doors: forward, aft, bulk; all cargo doors are on the right).
float paxDoorAt(int pair, int pairs) {
    static const float two[2] = {0.135f, 0.845f}, four[4] = {0.115f, 0.355f, 0.600f, 0.865f}, three[3] = {0.125f, 0.50f, 0.855f};
    if (pairs <= 2) return two[std::min(pair, 1)];
    if (pairs == 3) return three[std::min(pair, 2)];
    return four[std::min(pair, 3)];
}
const float kCargoAt[3] = {0.245f, 0.665f, 0.745f};

void groundPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const GroundState& g = s.ground;
    const float top = kStatusH + kHeaderH + 10, bot = h - 12;
    const float leftW = std::floor(w * 0.44f);
    const bool gnd = g.onGround;
    const PlaneSpec& spec = planeSpec(s.acfIcao);
    float shLen = 0, shSpan = 0;
    const Shape* shape = findShape(s.acfIcao, shLen, shSpan);
    const float acLen = shape ? shLen : spec.len, acSpan = shape ? shSpan : spec.span;

    // ---- the aircraft and its doors
    const ImVec2 a(12, top), e(12 + leftW, bot);
    card(dl, a, e, f, nullptr);
    char b[96];
    std::snprintf(b, sizeof b, "%s", s.acfIcao.empty() ? "AIRCRAFT" : s.acfIcao.c_str());
    textAt(dl, f.small, a.x + 14, a.y + 10, kMuted, b);
    std::snprintf(b, sizeof b, "%.1f m long  \xC2\xB7  %.1f m span", acLen, acSpan);
    textRight(dl, f.small, e.x - 14, a.y + 10, kMuted, b);
    // ToLiss: the doors a jetway can reach, each with its block on the fuselage and a Connect / Disconnect bubble beside it.
    // When the jetway is docked, the door opens; when it leaves, the door closes (the plugin does that, see doGround).
    const bool doors = g.toliss && g.jwDoors > 0;
    const float boxTop = a.y + 34, boxBot = e.y - (doors && g.jwDoors > 1 && g.openSam ? 30 : 14);
    const float px = std::min((boxBot - boxTop) / acLen, (leftW - (doors ? 150 : 40)) / acSpan);   // room at the left for the bubbles
    const ImVec2 nose((a.x + e.x) * 0.5f + (doors ? 30.0f : 0.0f), std::floor((boxTop + boxBot - acLen * px) * 0.5f));
    // the drawing, resized to the type's length and span (kx, ky: from the drawing's metres to the type's)
    const float kx = shape ? acSpan / shape->span : 1.0f, ky = shape ? acLen / shape->len : 1.0f;
    if (shape) drawShape(dl, *shape, nose, px * kx, px * ky, IM_COL32(200, 211, 230, 255), IM_COL32(104, 120, 150, 255));
    else planeTopDown(dl, nose, px, spec, IM_COL32(200, 211, 230, 255), IM_COL32(120, 136, 164, 255));
    const float hw0 = shape ? fuselageHalf(*shape, 0.25f, 0) : -1;   // the fuselage's half-width forward of the wing, drawing metres
    const float hwPx = (shape && hw0 > 0 ? hw0 * kx : spec.fus * 0.5f) * px;
    if (doors) {
        const int pairs = (g.paxDoors + 1) / 2;
        for (int k = 0; k < g.jwDoors && k < 3; ++k) {
            const int pax = g.jwDoorPax[k];
            const float frac = paxDoorAt(pax / 2, std::max(1, pairs));
            const float y = nose.y + frac * acLen * px;
            float hw = hwPx;   // the fuselage's edge at this door (it narrows towards the tail)
            if (shape && hw0 > 0) { const float hh = fuselageHalf(*shape, frac, hw0); if (hh > 0) hw = hh * kx * px; }
            const float side = nose.x - hw;
            const bool docked = g.openSam ? g.jwDocked[k] : g.jetway == 2;
            const bool open = pax >= 0 && pax < 8 && g.paxDoor[pax] == 2;
            // the block: the door, green while a jetway is on it, blue while the door is open without one
            const ImU32 blockCol = docked ? kGood : open ? IM_COL32(110, 170, 255, 255) : IM_COL32(140, 156, 184, 255);
            dl->AddRectFilled(ImVec2(side - 9, y - 7), ImVec2(side + 1, y + 7), blockCol, 2.0f);
            // the bubble, at the card's left edge, with a leader to the block
            const float bw = 108, bx = a.x + 12;
            dl->AddLine(ImVec2(bx + bw, y), ImVec2(side - 9, y), IM_COL32(255, 255, 255, 46), 1.0f);
            char id[16], lbl[16];
            std::snprintf(id, sizeof id, "##jw%d", k);
            std::snprintf(lbl, sizeof lbl, "%s", docked ? "Disconnect" : "Connect");
            const bool canPress = gnd && (!g.openSam || g.jetway == 1 || g.jetway == 2);
            dl->AddRectFilled(ImVec2(bx, y - 14), ImVec2(bx + bw, y + 14), IM_COL32(16, 30, 56, 255), 14.0f);
            if (pill(dl, f, id, lbl, ImVec2(bx, y - 14), ImVec2(bx + bw, y + 14), docked, canPress)) { act.ground = GA_JETWAY_DOOR; act.groundIndex = k; }
            // under the bubble: the door's name and what the jetway is doing
            const char* st = !g.openSam ? (docked ? "jetway on" : "X-Plane jetway")
                           : docked ? "docked, door open" : g.jetway == 1 ? "jetway ready" : g.jetway == 0 ? "no jetway here" : g.jetway == 2 ? "jetway at the other door" : "jetway moving";
            std::snprintf(b, sizeof b, "%d%c  \xC2\xB7  %s", pax / 2 + 1, 'L', st);
            textAt(dl, f.small, bx + 4, y + 17, docked ? kGood : kMuted, b);
        }
        if (g.jwDoors > 1 && g.openSam) textCentered(dl, f.small, (a.x + e.x) * 0.5f, e.y - 24, kMuted, "openSAM moves every jetway at the stand together.");
    }

    // ---- the services
    const float rx = e.x + 10, rw = w - 12 - rx;
    card(dl, ImVec2(rx, top), ImVec2(rx + rw, bot), f, "SERVICES");
    float y = top + 32;
    const int rows = (doors ? 0 : 1) + 1 + (g.toliss ? 2 + (g.deice ? 1 : 0) + (g.slides >= 0 ? 1 : 0) + (g.cabinLight >= 0 ? 1 : 0) : 0) + 2;
    const float rowH = std::min(50.0f, std::floor((bot - 12 - y) / rows)), bx1 = rx + rw - 14, bh = std::min(28.0f, rowH - 14);
    auto rowLabel = [&](const char* name, const char* state, ImU32 stateCol, bool lit) {
        dl->AddCircleFilled(ImVec2(rx + 20, y + rowH * 0.5f - 2), 4.0f, lit ? kGood : IM_COL32(255, 255, 255, 40), 12);   // a lamp: lit when connected
        textAt(dl, f.bold, rx + 34, y + rowH * 0.5f - 19, kInk, name);
        if (state && *state) textAt(dl, f.small, rx + 34, y + rowH * 0.5f, stateCol, state);
    };
    auto btn = [&](const char* id, const char* label, float right, float width, bool on, bool enabled) {
        const float by = y + (rowH - 4 - bh) * 0.5f;
        return pill(dl, f, id, label, ImVec2(right - width, by), ImVec2(right, by + bh), on, enabled);
    };
    auto rowEnd = [&]() { y += rowH; dl->AddLine(ImVec2(rx + 14, y - 3), ImVec2(rx + rw - 14, y - 3), IM_COL32(255, 255, 255, 18)); };
    if (!doors) {   // jetway (on a ToLiss it is at the doors, on the left)
        const bool docked = g.openSam && g.jetway == 2;
        const char* st = !g.openSam ? "X-Plane's own jetways" : docked ? "Connected" : g.jetway == 1 ? "Ready to connect" : g.jetway == 0 ? "No jetway at this stand" : "Moving";
        rowLabel("Jetway", st, docked ? kGood : kMuted, docked);
        if (btn("##jw", docked ? "Disconnect" : "Connect", bx1, 104, docked, gnd && (!g.openSam || g.jetway == 1 || g.jetway == 2))) act.ground = GA_JETWAY;
        rowEnd();
    }
    {   // ground power
        rowLabel("Ground power", g.toliss ? (g.extPower ? "Connected" : "Not connected") : "Ask the ground crew", g.toliss && g.extPower ? kGood : kMuted, g.toliss && g.extPower);
        if (btn("##gpu", g.toliss ? (g.extPower ? "Remove" : "Connect") : "Request", bx1, 104, g.toliss && g.extPower, gnd)) act.ground = GA_GPU;
        rowEnd();
    }
    if (g.toliss) {   // chocks, ground air and de-icing: where the aircraft lets them be set
        rowLabel("Chocks", g.chocks ? "In place" : "Removed", g.chocks ? kGood : kMuted, g.chocks);
        if (btn("##chk", g.chocks ? "Remove" : "Set", bx1, 104, g.chocks, gnd)) act.ground = GA_CHOCKS;
        rowEnd();
        rowLabel("Ground air", g.hpAir || g.lpAir ? "Connected" : "Not connected", g.hpAir || g.lpAir ? kGood : kMuted, g.hpAir || g.lpAir);
        if (btn("##hp", "HP start", bx1 - 84, 78, g.hpAir, gnd)) act.ground = GA_HP_AIR;
        if (btn("##lp", "LP cond", bx1, 78, g.lpAir, gnd)) act.ground = GA_LP_AIR;
        rowEnd();
        if (g.deice) {
            rowLabel("De-icing", "The trucks spray, or done at once", kMuted, false);
            if (btn("##di", "Trucks", bx1 - 84, 78, false, gnd)) act.ground = GA_DEICE;
            if (btn("##diq", "Instant", bx1, 78, false, gnd)) act.ground = GA_DEICE_QUICK;
            rowEnd();
        }
    }
    if (g.toliss && g.slides >= 0) {   // the cabin doors' slides
        rowLabel("Cabin doors", g.slides == 1 ? "Slides armed" : "Slides disarmed", g.slides == 1 ? kWarn : kMuted, g.slides == 1);
        if (btn("##arm", "Arm", bx1 - 84, 78, g.slides == 1, true)) act.ground = GA_SLIDES_ARM;
        if (btn("##disarm", "Disarm", bx1, 78, g.slides == 0, true)) act.ground = GA_SLIDES_DISARM;
        rowEnd();
    }
    if (g.toliss && g.cabinLight >= 0) {   // the cabin lights
        char lv[32];
        std::snprintf(lv, sizeof lv, g.cabinLight < 0.02f ? "Off" : "%d %%", (int)std::lround(g.cabinLight * 100));
        rowLabel("Cabin lights", lv, kMuted, g.cabinLight >= 0.02f);
        const float bw = 64;
        const char* names[3] = {"Off", "Dim", "Bright"};
        const int lvl = g.cabinLight < 0.02f ? 0 : g.cabinLight < 0.75f ? 1 : 2;
        for (int i = 0; i < 3; ++i) {
            char id[8];
            std::snprintf(id, sizeof id, "##cl%d", i);
            if (btn(id, names[i], bx1 - (2 - i) * (bw + 5), bw, lvl == i, true)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = i; }
        }
        rowEnd();
    }
    {   // service vehicles
        rowLabel("Service vehicles", g.toliss ? "Stairs, catering and loaders come with the doors" : "Fuel, catering, baggage (X-Plane's trucks)", kMuted, false);
        if (btn("##trk", g.toliss ? "X-Plane's" : "Call", bx1, 104, false, gnd)) act.ground = GA_TRUCKS;
        rowEnd();
    }
    {   // pushback
        rowLabel("Pushback", "X-Plane's tug", kMuted, false);
        const float bw = 54;
        const char* names[4] = {"Left", "Back", "Right", "Stop"};
        const int acts[4] = {GA_PUSH_LEFT, GA_PUSH_STRAIGHT, GA_PUSH_RIGHT, GA_PUSH_STOP};
        for (int i = 0; i < 4; ++i) {
            char id[8];
            std::snprintf(id, sizeof id, "##pb%d", i);
            if (btn(id, names[i], bx1 - (3 - i) * (bw + 5), bw, false, gnd)) act.ground = acts[i];
        }
        rowEnd();
    }
    if (!gnd) textCentered(dl, f.small, rx + rw * 0.5f, top + 10, kWarn, "Ground services are available on the ground.");
}

// Weather: WXPlane's own screen when WXPlane is installed (it draws itself into the page: the host passes the area on), otherwise
// a note until the built-in weather map exists.
void weatherPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH;
    const ImVec2 c(w * 0.5f, top + (h - top) * 0.42f);
    if (!s.wxPlane) { simWeatherPage(dl, f, s, w, h, act, false); return; }
    // WXPlane is asked to draw here every frame, whatever its version: nothing is checked beforehand. While the installed WXPlane
    // does not draw on the tablet (one from before it could), the page is the built-in weather, and WXPlane is still asked every
    // frame: the moment it draws, it covers the page and takes it over.
    if (s.wxEmbed == -1) {
        simWeatherPage(dl, f, s, w, h, act, true);
        g_embed.active = true;   // asked, but given no pointer: the built-in page keeps it
        g_embed.x = 0; g_embed.y = top; g_embed.w = w; g_embed.h = h - top;
        return;
    }
    dl->AddRectFilled(ImVec2(0, top), ImVec2(w, h), IM_COL32(18, 20, 26, 255));
    if (s.wxEmbed != 1) textCentered(dl, f.body, c.x, c.y, IM_COL32(190, 200, 220, 255), "Opening WXPlane...");
    ImGui::SetCursorScreenPos(ImVec2(0, top));
    ImGui::InvisibleButton("##wxplane", ImVec2(w, h - top));   // the pointer here belongs to WXPlane
    const ImGuiIO& io = ImGui::GetIO();
    g_embed.active = true;
    g_embed.x = 0; g_embed.y = top; g_embed.w = w; g_embed.h = h - top;
    const bool over = ImGui::IsItemHovered() || ImGui::IsItemActive();
    g_embed.mx = over ? io.MousePos.x : -1e9f;
    g_embed.my = over ? io.MousePos.y - top : -1e9f;
    g_embed.down = ImGui::IsItemActive();
    g_embed.wheel = ImGui::IsItemHovered() ? io.MouseWheel : 0.0f;
}

void sectionTitle(const Fonts& f, const char* t) { ImGui::PushFont(f.bold); ImGui::TextUnformatted(t); ImGui::PopFont(); }

// Settings' second view: who made what XPTablet is built from.
int g_settingsTab = 0;   // 0 the settings, 1 credits

void creditsPage(ImDrawList* dl, const Fonts& f, float w, float h) {
    const float top = kStatusH + kHeaderH + 10;
    card(dl, ImVec2(12, top), ImVec2(w - 12, h - 12), f, "CREDITS");
    struct Credit { const char* what; const char* who; const char* licence; };
    static const Credit kCredits[] = {
        {"XPTablet", "Alpine Aviation", "Freeware"},
        {"Aircraft top views (Ground page)", "AircraftShapesSVG by RexKramer1 and amnesica (BelugaProject), github.com/RexKramer1/AircraftShapesSVG", "GNU GPL v3.0"},
        {"User interface", "Dear ImGui by Omar Cornut and contributors", "MIT"},
        {"Text", "IBM Plex Sans (IBM Corp.), JetBrains Mono (The JetBrains Mono Project Authors)", "SIL Open Font License 1.1"},
        {"Downloads", "libcurl (Daniel Stenberg and contributors), zlib (Jean-loup Gailly, Mark Adler), OpenSSL on Linux", "curl, zlib, Apache 2.0"},
        {"METAR reports", "NOAA / National Weather Service, Aviation Weather Center (aviationweather.gov)", "Public domain"},
        {"Weather page with WXPlane installed", "WXPlane by Alpine Aviation", ""},
    };
    float y = top + 38;
    const float x0 = 30, xLic = w - 30, wrap = w - 60 - 170;
    for (const Credit& c : kCredits) {
        textAt(dl, f.bold, x0, y, kInk, c.what);
        if (*c.licence) textRight(dl, f.small, xLic, y + 2, kMuted, c.licence);
        const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, wrap, c.who);
        dl->AddText(f.small, fontPx(f.small), ImVec2(x0, y + 21), kMuted, c.who, nullptr, wrap);
        y += 21 + sz.y + 10;
    }
    textAt(dl, f.small, x0, y + 2, kMuted, "Full licence texts: LICENSES.txt and shapes/LICENSE-GPL-3.0.txt in the XPTablet folder.");
}

void settingsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    (void)act;
    {   // two views, switched at the right end of the title bar
        const float y0 = kStatusH + 9, bw = 92;
        if (pill(dl, f, "##setmain", "Settings", ImVec2(w - 14 - 2 * bw - 6, y0), ImVec2(w - 14 - bw - 6, y0 + 28), g_settingsTab == 0, true)) g_settingsTab = 0;
        if (pill(dl, f, "##setcred", "Credits", ImVec2(w - 14 - bw, y0), ImVec2(w - 14, y0 + 28), g_settingsTab == 1, true)) g_settingsTab = 1;
    }
    if (g_settingsTab == 1) { creditsPage(dl, f, w, h); return; }
    const float top = kStatusH + kHeaderH;
    const float colW = std::floor(w * 0.5f) - 24;
    ImGui::SetCursorScreenPos(ImVec2(18, top + 12));
    ImGui::BeginChild("##settings", ImVec2(colW, h - top - 24), 0, ImGuiWindowFlags_NoBackground);
    sharp(ImGui::GetWindowDrawList());
    sectionTitle(f, "Display");
    // The tablet's own brightness, kept in the settings. The aircraft has a say too (its tablet's brightness knob writes
    // avitab/brightness every frame, which is why a slider on that value alone would not move): the screen shows the two multiplied.
    float br = std::max(0.1f, std::min(1.0f, g_settings.brightness));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##brightness", &br, 0.1f, 1.0f, "Brightness %.2f")) { g_settings.brightness = br; g_settingsChanged = true; }
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
    ImGui::SetNextItemAllowOverlap();   // the Clear button lies inside this area: without this the area took its presses
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
    g_embed = EmbedArea();
    g_wx = s.wxPlane;
    g_wxLogo = f.wxLogo;
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
        else if (app == APP_WEATHER) weatherPage(dl, f, s, w, h, act);
        else if (app == APP_GROUND) groundPage(dl, f, s, w, h, act);
        else placeholderPage(dl, f, app, w, h);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

int currentApp() { return g_app; }

int loadShapes(const std::string& text) {
    g_shapes.clear(); g_aliases.clear();
    std::vector<std::string> pendingAliases;
    size_t pos = 0;
    std::vector<float> v;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const char* p = text.c_str() + pos;
        const char* q = text.c_str() + end;
        pos = end + 1;
        while (p < q && (*p == ' ' || *p == '\r')) ++p;
        if (p >= q || *p == '#') continue;
        const char* sp = p;
        while (sp < q && *sp != ' ') ++sp;
        const std::string key(p, sp);
        if (key == "shape") {
            g_shapes.emplace_back();
            g_shapes.back().name = std::string(sp + 1, q);
            while (!g_shapes.back().name.empty() && (g_shapes.back().name.back() == '\r' || g_shapes.back().name.back() == ' ')) g_shapes.back().name.pop_back();
            continue;
        }
        if (key == "alias") { pendingAliases.emplace_back(sp + 1, q); continue; }
        if (g_shapes.empty()) continue;
        v.clear();
        for (const char* c = sp; c < q;) {
            char* e2 = nullptr;
            const float x = std::strtof(c, &e2);
            if (e2 == c) { ++c; continue; }
            v.push_back(x);
            c = e2;
        }
        Shape& sh = g_shapes.back();
        if (key == "size" && v.size() >= 2) { sh.len = v[0]; sh.span = v[1]; }
        else if ((key == "fill" || key == "line") && v.size() >= 4) {
            std::vector<ImVec2> pts;
            for (size_t i = 0; i + 1 < v.size(); i += 2) pts.emplace_back(v[i], v[i + 1]);
            if (key == "fill") {
                // ImGui's concave fill needs the outline clockwise on screen (y down): the drawings come either way round
                double area = 0;
                for (size_t i = 0, n = pts.size(); i < n; ++i) area += (double)pts[i].x * pts[(i + 1) % n].y - (double)pts[(i + 1) % n].x * pts[i].y;
                if (area < 0) std::reverse(pts.begin(), pts.end());
            }
            (key == "fill" ? sh.fills : sh.lines).push_back(std::move(pts));
        }
    }
    // a shape is usable with a size and something to fill
    std::vector<Shape> ok;
    for (Shape& sh : g_shapes) if (sh.len > 0.5f && sh.span > 0.5f && !sh.fills.empty()) ok.push_back(std::move(sh));
    g_shapes.swap(ok);
    for (const std::string& line : pendingAliases) {   // "<type> <drawing> <length> <span>"
        char icao[16] = "", shape[16] = "";
        float len = 0, span = 0;
        if (std::sscanf(line.c_str(), "%15s %15s %f %f", icao, shape, &len, &span) != 4 || len <= 0 || span <= 0) continue;
        for (size_t i = 0; i < g_shapes.size(); ++i)
            if (g_shapes[i].name == shape) { g_aliases.push_back({icao, (int)i, len, span}); break; }
    }
    return (int)g_shapes.size();
}

bool hasShape(const std::string& icao) { float l, sp; return findShape(icao, l, sp) != nullptr; }
const unsigned char* wxLogoPixels(unsigned int& bytes) { bytes = xpt_asset_wxlogo_size; return xpt_asset_wxlogo; }
int touchTestCount() { return (int)g_touches.size(); }
const EmbedArea& embedArea() { return g_embed; }
const char* browserUrl() { return g_url.c_str(); }
bool browserChrome() { return g_chrome; }

const char* appName(int app) {
    if (app == APP_CHARTS) return kChartNames[chartsP()];
    if (app == APP_LIVEMAP) return kLiveNames[liveP()];
    if (app == APP_WEATHER && g_wx) return "WXPlane";
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
    std::snprintf(buf, sizeof buf, "charts_provider=%d\nlive_provider=%d\nfavorite_slots=%d\nbrightness=%.2f\nshow_charts=%d\nshow_simbrief=%d\nshow_live=%d\nshow_foreflight=%d\n", chartsP(), liveP(), favSlots(), (double)std::max(0.1f, std::min(1.0f, g_settings.brightness)), g_settings.showCharts ? 1 : 0,
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
        else if (key == "brightness") g_settings.brightness = std::max(0.1f, std::min(1.0f, (float)std::atof(val.c_str())));
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
