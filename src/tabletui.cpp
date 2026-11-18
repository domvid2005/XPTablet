#include "tabletui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "metar.h"
#include "suntime.h"
#include "tz.h"
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
// Volanta's violet (approximated: the brand's exact value can be dropped in here) and VATSIM's green
const ImU32 kLiveColors[2] = {IM_COL32(112, 72, 255, 255), IM_COL32(44, 150, 96, 255)};
const char* const kPlanNames[2] = {"ForeFlight", "SkyVector"};
const char* const kPlanUrls[2] = {"https://plan.foreflight.com", "https://skyvector.com"};
const ImU32 kPlanColors[2] = {IM_COL32(30, 140, 222, 255), IM_COL32(32, 132, 108, 255)};
enum { ICON_VOLANTA = APP_COUNT, ICON_VATSIM, ICON_SKYVECTOR };   // the Live Map icon's two pictures; the planning icon's second

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
int planP() { return g_settings.planProvider == 1 ? 1 : 0; }
ImU32 appColor(int app) {
    if (app == APP_LIVEMAP) return kLiveColors[liveP()];
    if (app == APP_FOREFLIGHT) return kPlanColors[planP()];
    return (app == APP_WEATHER && g_wx) ? IM_COL32(22, 24, 29, 255) : kApps[app].color;
}
int appIcon(int app) {
    if (app == APP_LIVEMAP) return liveP() ? (int)ICON_VATSIM : (int)ICON_VOLANTA;
    if (app == APP_FOREFLIGHT && planP()) return (int)ICON_SKYVECTOR;
    return app;
}

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
    case ICON_VOLANTA:   // a flight's great-circle track from one place to another, an aircraft on its way
        dl->PathLineTo(P(-0.74f, 0.52f));
        dl->PathBezierQuadraticCurveTo(P(0.0f, -1.06f), P(0.74f, 0.52f), 24);
        dl->PathStroke(col, 0, t * 0.8f);
        dl->AddCircleFilled(P(-0.74f, 0.52f), r * 0.15f, col, 12);
        dl->AddCircle(P(0.74f, 0.52f), r * 0.15f, col, 12, t * 0.8f);
        {   // the aircraft, at the top of the arc, heading right: a swept dart, cut out of the arc
            const ImVec2 a = P(0.02f, -0.27f);
            const float k = r * 0.40f;
            dl->AddCircleFilled(a, k * 0.78f, IM_COL32(112, 72, 255, 255), 16);   // the tile's own colour behind it
            dl->AddTriangleFilled(ImVec2(a.x + k, a.y), ImVec2(a.x - k * 0.75f, a.y - k * 0.70f), ImVec2(a.x - k * 0.30f, a.y), col);
            dl->AddTriangleFilled(ImVec2(a.x + k, a.y), ImVec2(a.x - k * 0.30f, a.y), ImVec2(a.x - k * 0.75f, a.y + k * 0.70f), col);
        }
        break;
    case ICON_SKYVECTOR:   // a folded aeronautical chart with a route drawn on it
        dl->PathLineTo(P(-0.80f, -0.56f)); dl->PathLineTo(P(-0.28f, -0.74f)); dl->PathLineTo(P(0.28f, -0.56f)); dl->PathLineTo(P(0.80f, -0.74f));
        dl->PathLineTo(P(0.80f, 0.62f)); dl->PathLineTo(P(0.28f, 0.80f)); dl->PathLineTo(P(-0.28f, 0.62f)); dl->PathLineTo(P(-0.80f, 0.80f));
        dl->PathStroke(col, ImDrawFlags_Closed, t * 0.85f);
        dl->AddLine(P(-0.28f, -0.74f), P(-0.28f, 0.62f), col, t * 0.6f);
        dl->AddLine(P(0.28f, -0.56f), P(0.28f, 0.80f), col, t * 0.6f);
        dl->AddLine(P(-0.56f, 0.40f), P(0.0f, -0.10f), col, t);
        dl->AddLine(P(0.0f, -0.10f), P(0.54f, -0.30f), col, t);
        dl->AddCircleFilled(P(-0.56f, 0.40f), r * 0.11f, col, 10);
        dl->AddCircleFilled(P(0.0f, -0.10f), r * 0.11f, col, 10);
        dl->AddCircleFilled(P(0.54f, -0.30f), r * 0.11f, col, 10);
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

// ---- the web browser ---------------------------------------------------------------------------------------------------------------
// The page itself is drawn by the browser program (the plugin passes its picture in s.browser); here are its controls, the
// pointer and keys going to it, and an on-screen keyboard (the cockpit tablet has no keyboard).
std::string g_navPending;            // an address to send to the browser
bool g_navForce = false;             // send it even when the page is already on that site
bool g_kbd = false;                  // the on-screen keyboard is up
int g_kbdTarget = 0;                 // what it types into: 0 the page, 1 the address
bool g_kbdShift = false, g_kbdSymbols = false;
bool g_wantedKbd = false;            // the page's own wish for a keyboard, last frame
std::string g_addrEdit;
bool g_webHover = false;
ImVec2 g_webLast(-1, -1);

std::string hostOf(const std::string& url) {
    size_t a = url.find("://");
    a = a == std::string::npos ? 0 : a + 3;
    size_t b = url.find_first_of("/?#", a);
    std::string h = url.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (h.compare(0, 4, "www.") == 0) h.erase(0, 4);
    return h;
}

// What was typed in the address bar, as an address: as it is, with https:// added, or a web search.
std::string addressFrom(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.empty()) return s;
    if (s.find("://") != std::string::npos) return s;
    if (s.find(' ') == std::string::npos && s.find('.') != std::string::npos) return "https://" + s;
    std::string q;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') q += (char)c;
        else if (c == ' ') q += '+';
        else { char hx[4]; std::snprintf(hx, sizeof hx, "%%%02X", c); q += hx; }
    }
    return "https://duckduckgo.com/?q=" + q;
}

int bookmarkIndex(const std::string& url) {
    for (size_t i = 0; i < g_settings.bookmarks.size(); ++i) if (g_settings.bookmarks[i].second == url) return (int)i;
    return -1;
}

// A small icon button drawn by hand: returns true when pressed.
bool iconButton(const char* id, ImVec2 a, ImVec2 b, bool enabled) {
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    ImGui::EndDisabled();
    return hit && enabled;
}
void arrow(ImDrawList* dl, ImVec2 c, bool left, ImU32 col) {
    const float d = left ? 1.0f : -1.0f;
    const ImVec2 p[3] = {ImVec2(c.x + 4 * d, c.y - 7), ImVec2(c.x - 3 * d, c.y), ImVec2(c.x + 4 * d, c.y + 7)};
    dl->AddPolyline(p, 3, col, 0, 2.0f);
}
void reloadIcon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->PathArcTo(c, 7.5f, kPi * 0.25f, kPi * 1.85f, 20);
    dl->PathStroke(col, 0, 2.0f);
    dl->AddTriangleFilled(ImVec2(c.x + 9.5f, c.y - 7.5f), ImVec2(c.x + 9.5f, c.y - 0.5f), ImVec2(c.x + 2.5f, c.y - 4.0f), col);
}
void keyboardIcon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect(ImVec2(c.x - 11, c.y - 7), ImVec2(c.x + 11, c.y + 7), col, 2.5f, 0, 1.6f);
    for (int r = 0; r < 2; ++r) for (int k = 0; k < 4; ++k) dl->AddRectFilled(ImVec2(c.x - 8 + k * 4.5f, c.y - 4 + r * 4), ImVec2(c.x - 6 + k * 4.5f, c.y - 2 + r * 4), col);
    dl->AddRectFilled(ImVec2(c.x - 5, c.y + 3.5f), ImVec2(c.x + 5, c.y + 5), col);
}
void star(ImDrawList* dl, ImVec2 c, bool filled, ImU32 col) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float ang = -kPi * 0.5f + i * kPi / 5, rad = (i & 1) ? 4.2f : 9.5f;
        pts[i] = ImVec2(c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad);
    }
    if (filled) dl->AddConcavePolyFilled(pts, 10, col); else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.6f);
}

void typeKey(Actions& act, int code, bool isChar) {
    if (g_kbdTarget == 1) {   // the address bar
        if (!isChar && code == 8) { if (!g_addrEdit.empty()) g_addrEdit.pop_back(); }
        else if (!isChar && code == 13) { const std::string u = addressFrom(g_addrEdit); if (!u.empty()) { g_navPending = u; g_navForce = true; } g_kbd = false; g_kbdTarget = 0; }
        else if (isChar && code >= 32 && code < 127) g_addrEdit += (char)code;
        return;
    }
    if (isChar) act.web.push_back({12 /*CMD_CHAR*/, 0, 0, code});
    else {   // a key down and up (x: 1 down); Enter also as a character, which is what submits a form
        act.web.push_back({13 /*CMD_KEY*/, 1, 0, code});
        if (code == 13) act.web.push_back({12, 0, 0, 13});
        act.web.push_back({13, 0, 0, code});
    }
}

} // namespace

bool webTypingAddress() { return g_kbd && g_kbdTarget == 1; }
void webAddressKey(int code, bool isChar) { Actions none; typeKey(none, code, isChar); }

namespace {

// The on-screen keyboard across the bottom of the page, top at y. Returns its height.
float keyboard(ImDrawList* dl, const Fonts& f, float w, float y, Actions& act) {
    const float pad = 8, gap = 5, kh = 34, H = 5 * kh + 4 * gap + 2 * pad;
    dl->AddRectFilled(ImVec2(0, y), ImVec2(w, y + H), IM_COL32(20, 26, 38, 250));
    dl->AddLine(ImVec2(0, y), ImVec2(w, y), IM_COL32(255, 255, 255, 40));
    const char* letters[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const char* symbols[4] = {"1234567890", "!@#$%^&*()", "-_=+:;'\"?", "~`|\\<>,"};
    const char* const* rows = g_kbdSymbols ? symbols : letters;
    const float kw = std::floor((w - 2 * pad - 9 * gap) / 10);
    const float x00 = std::floor((w - (10 * kw + 9 * gap)) * 0.5f);
    auto key = [&](const char* id, const char* label, float x, float yy, float ww, bool lit) {
        ImGui::SetCursorScreenPos(ImVec2(x, yy));
        const bool hit = ImGui::InvisibleButton(id, ImVec2(ww, kh));
        const bool down = ImGui::IsItemActive();
        dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + ww, yy + kh), lit ? IM_COL32(70, 140, 255, 230) : IM_COL32(255, 255, 255, down ? 80 : 30), 7.0f);
        ImFont* fn = std::strlen(label) > 2 ? f.small : f.bold;
        textCentered(dl, fn, x + ww * 0.5f, std::floor(yy + (kh - fontPx(fn)) * 0.5f), kInk, label);
        return hit;
    };
    for (int r = 0; r < 4; ++r) {
        const int n = (int)std::strlen(rows[r]);
        const float yy = y + pad + r * (kh + gap);
        float x = x00 + (r == 2 ? (kw + gap) * 0.5f : r == 3 ? (kw + gap) * 1.5f : 0.0f);
        if (r == 3) {   // shift, at the left of the last letter row
            if (key("##kshift", g_kbdSymbols ? "" : "Shift", x00, yy, kw * 1.5f + gap * 0.5f, g_kbdShift) && !g_kbdSymbols) g_kbdShift = !g_kbdShift;
        }
        for (int k = 0; k < n; ++k, x += kw + gap) {
            char c = rows[r][k];
            if (g_kbdShift && c >= 'a' && c <= 'z') c = (char)(c - 32);
            char id[16], lbl[2] = {c, 0};
            std::snprintf(id, sizeof id, "##k%d_%d", r, k);
            if (key(id, lbl, x, yy, kw, false)) { typeKey(act, (unsigned char)c, true); g_kbdShift = false; }
        }
        if (r == 3 && key("##kback", "Delete", x, yy, x00 + 10 * kw + 9 * gap - x, false)) typeKey(act, 8, false);
    }
    // the last row: switch layers, @, space, full stop, slash, and Enter / Go
    const float yy = y + pad + 4 * (kh + gap);
    float x = x00;
    const float w15 = kw * 1.5f + gap * 0.5f;
    if (key("##klayer", g_kbdSymbols ? "ABC" : "?123", x, yy, w15, false)) { g_kbdSymbols = !g_kbdSymbols; g_kbdShift = false; }
    x += w15 + gap;
    if (key("##kat", "@", x, yy, kw, false)) typeKey(act, '@', true);
    x += kw + gap;
    const float spaceW = 10 * kw + 9 * gap - (w15 + gap) * 2 - (kw + gap) * 3;
    if (key("##kspace", "space", x, yy, spaceW, false)) typeKey(act, ' ', true);
    x += spaceW + gap;
    if (key("##kdot", ".", x, yy, kw, false)) typeKey(act, '.', true);
    x += kw + gap;
    if (key("##kslash", "/", x, yy, kw, false)) typeKey(act, '/', true);
    x += kw + gap;
    if (key("##kenter", g_kbdTarget == 1 ? "Go" : "Enter", x, yy, w15, true)) typeKey(act, 13, false);
    return H;
}

// The page: the browser's picture in the rectangle, with the pointer and the wheel going to it.
void webView(ImDrawList* dl, const Fonts& f, const SimState& s, float x0, float y0, float w, float h, Actions& act) {
    act.webShown = true;
    act.webX = x0; act.webY = y0; act.webW = w; act.webH = h;
    const BrowserState& b = s.browser;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(244, 246, 250, 255));
    const ImVec2 c(x0 + w * 0.5f, y0 + h * 0.42f);
    if (b.status != 1 || !b.tex) {
        const bool bad = b.status == -1 || b.status == -3;
        textCentered(dl, f.bold, c.x, c.y - 10, IM_COL32(30, 40, 60, 255), b.status == -3 ? "The browser is not available here" : b.status == -1 ? "The browser stopped" : b.status == 1 ? "Loading..." : "Starting the browser...");
        if (!b.message.empty()) dl->AddText(f.small, fontPx(f.small), ImVec2(x0 + 30, c.y + 16), IM_COL32(90, 100, 120, 255), b.message.c_str(), nullptr, w - 60);
        if (b.status == -1 && pill(dl, f, "##webretry", "Try again", ImVec2(c.x - 60, c.y + 64), ImVec2(c.x + 60, c.y + 92), true, true)) act.webRetry = true;
        (void)bad;
        return;
    }
    dl->AddImage(b.tex, ImVec2(x0, y0), ImVec2(x0 + w, y0 + h));
    // the pointer: moves, presses and the wheel, in the page's own units (one per layout unit)
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##web", ImVec2(w, h));
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 p(std::floor(io.MousePos.x - x0), std::floor(io.MousePos.y - y0));
    if ((hovered || active) && (p.x != g_webLast.x || p.y != g_webLast.y)) { act.web.push_back({7 /*MOUSE_MOVE*/, p.x, p.y, 0}); g_webLast = p; }
    if (ImGui::IsItemActivated()) act.web.push_back({8 /*MOUSE_DOWN*/, p.x, p.y, 0});
    if (ImGui::IsItemDeactivated()) act.web.push_back({9 /*MOUSE_UP*/, p.x, p.y, 0});
    if (hovered && io.MouseWheel != 0) act.web.push_back({11 /*WHEEL*/, p.x, p.y, (int)std::lround(io.MouseWheel * 100)});
    if (g_webHover && !hovered && !active) { act.web.push_back({10 /*MOUSE_LEAVE*/, g_webLast.x, g_webLast.y, 0}); g_webLast = ImVec2(-1, -1); }
    g_webHover = hovered || active;
}

void browserPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const BrowserState& b = s.browser;
    // the page's own wish for a keyboard: up when a text field takes the focus, down when it lets go
    if (b.wantsKeyboard != g_wantedKbd) { g_wantedKbd = b.wantsKeyboard; if (b.wantsKeyboard) { g_kbd = true; g_kbdTarget = 0; } else if (g_kbdTarget == 0) g_kbd = false; }
    // an address waiting. Typed or a bookmark: into the shown tab. A web app (Charts, Live Map...): its own tab, shown again as
    // it was left if it is open; else a blank tab takes it, else a new tab.
    if (!g_navPending.empty() && b.status == 1) {
        if (g_navForce || b.tabs.empty()) {
            act.webNavigate = g_navPending;
        } else {
            const std::string want = hostOf(g_navPending);
            int found = 0;
            for (const BrowserState::Tab& t : b.tabs) if (!t.url.empty() && hostOf(t.url) == want) { found = t.id; break; }
            const bool blank = b.url.empty() || b.url == "about:blank";
            if (found && found != b.activeTab) act.webTabSelect = found;
            else if (found) { /* already shown */ }
            else if (blank) act.webNavigate = g_navPending;
            else if ((int)b.tabs.size() < 8) { act.webTabNew = true; act.webTabUrl = g_navPending; }
            else act.webNavigate = g_navPending;
        }
        g_navPending.clear(); g_navForce = false;
    }
    float top = kStatusH + kHeaderH;
    const ImU32 ink = IM_COL32(210, 222, 242, 255), dim = IM_COL32(100, 118, 148, 255);
    float pageTop = top;
    if (!g_chrome) {
        // a web app: its site fills the page; back, reload and the keyboard sit at the right of the title bar
        const float my = kStatusH + kHeaderH * 0.5f;
        if (iconButton("##wback", ImVec2(w - 132, kStatusH + 6), ImVec2(w - 96, kStatusH + kHeaderH - 6), b.canBack)) act.webNav = 1;
        arrow(dl, ImVec2(w - 114, my), true, b.canBack ? ink : dim);
        if (iconButton("##wreload", ImVec2(w - 92, kStatusH + 6), ImVec2(w - 56, kStatusH + kHeaderH - 6), true)) act.webNav = b.loading ? 4 : 3;
        reloadIcon(dl, ImVec2(w - 74, my), ink);
        if (iconButton("##wkbd", ImVec2(w - 52, kStatusH + 6), ImVec2(w - 12, kStatusH + kHeaderH - 6), true)) { g_kbd = !g_kbd; g_kbdTarget = 0; }
        keyboardIcon(dl, ImVec2(w - 32, my), g_kbd ? IM_COL32(110, 170, 255, 255) : ink);
    } else {
        // the tabs: each with its page's title and a close button; + opens a blank one (and the address bar, to type where)
        const float tabsH = 32;
        dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + tabsH), IM_COL32(6, 16, 34, 245));
        {
            const int n = std::max(1, (int)b.tabs.size());
            const float plusW = 34, tw = std::min(190.0f, std::floor((w - 16 - plusW - 6) / n));
            float x = 8;
            int idx = 0;
            for (const BrowserState::Tab& t : b.tabs) {
                const bool on = t.id == b.activeTab;
                const ImVec2 a(x, top + 4), e(x + tw - 4, top + tabsH);
                char id[24];
                std::snprintf(id, sizeof id, "##tab%d", idx);
                ImGui::SetCursorScreenPos(a);
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x - 22, e.y - a.y)) && !on) act.webTabSelect = t.id;
                const bool hot = ImGui::IsItemHovered();
                dl->AddRectFilled(a, e, on ? IM_COL32(10, 24, 48, 255) : hot ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10), 8.0f, ImDrawFlags_RoundCornersTop);
                std::string title = !t.title.empty() ? t.title : (!t.url.empty() && t.url != "about:blank" ? hostOf(t.url) : std::string("New tab"));
                const float room = e.x - a.x - 34;
                while (title.size() > 3 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, title.c_str()).x > room) { title.resize(title.size() - 3); title += ".."; }
                dl->PushClipRect(a, ImVec2(e.x - 22, e.y), true);
                textAt(dl, f.small, a.x + 10, std::floor(a.y + (e.y - a.y - fontPx(f.small)) * 0.5f), on ? kInk : kMuted, title.c_str());
                dl->PopClipRect();
                if (t.loading) dl->AddCircleFilled(ImVec2(a.x + 4, a.y + (e.y - a.y) * 0.5f), 2.0f, IM_COL32(90, 160, 255, 255), 8);
                // close
                const ImVec2 cc(e.x - 12, a.y + (e.y - a.y) * 0.5f);
                std::snprintf(id, sizeof id, "##tabx%d", idx);
                ImGui::SetCursorScreenPos(ImVec2(e.x - 22, a.y));
                if (ImGui::InvisibleButton(id, ImVec2(22, e.y - a.y))) act.webTabClose = t.id;
                const ImU32 xc = ImGui::IsItemHovered() ? kInk : kMuted;
                dl->AddLine(ImVec2(cc.x - 4, cc.y - 4), ImVec2(cc.x + 4, cc.y + 4), xc, 1.5f);
                dl->AddLine(ImVec2(cc.x - 4, cc.y + 4), ImVec2(cc.x + 4, cc.y - 4), xc, 1.5f);
                x += tw;
                ++idx;
            }
            const ImVec2 pa(x + 2, top + 5), pe(x + 2 + plusW - 4, top + tabsH - 3);
            if (iconButton("##tabnew", pa, pe, (int)b.tabs.size() < 8 && b.status == 1)) { act.webTabNew = true; act.webTabUrl.clear(); g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); }
            const ImVec2 pc((pa.x + pe.x) * 0.5f, (pa.y + pe.y) * 0.5f);
            const ImU32 pcol = (int)b.tabs.size() < 8 ? kInk : kMuted;
            dl->AddLine(ImVec2(pc.x - 6, pc.y), ImVec2(pc.x + 6, pc.y), pcol, 1.8f);
            dl->AddLine(ImVec2(pc.x, pc.y - 6), ImVec2(pc.x, pc.y + 6), pcol, 1.8f);
        }
        top += tabsH;
        const float barH = 40, bmH = 34;
        dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + barH + bmH), IM_COL32(10, 24, 48, 235));
        const float my = top + barH * 0.5f;
        if (iconButton("##bback", ImVec2(8, top + 4), ImVec2(36, top + barH - 4), b.canBack)) act.webNav = 1;
        arrow(dl, ImVec2(22, my), true, b.canBack ? ink : dim);
        if (iconButton("##bfwd", ImVec2(38, top + 4), ImVec2(66, top + barH - 4), b.canForward)) act.webNav = 2;
        arrow(dl, ImVec2(52, my), false, b.canForward ? ink : dim);
        if (iconButton("##breload", ImVec2(70, top + 4), ImVec2(102, top + barH - 4), true)) act.webNav = b.loading ? 4 : 3;
        reloadIcon(dl, ImVec2(84, my), ink);
        // the address: tapped, it takes what is typed on the keyboard
        const ImVec2 a(110, top + 6), e(w - 92, top + barH - 6);
        const bool editing = g_kbd && g_kbdTarget == 1;
        if (iconButton("##baddr", a, e, true)) { g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); }
        dl->AddRectFilled(a, e, IM_COL32(255, 255, 255, editing ? 44 : 26), 14.0f);
        if (editing) dl->AddRect(a, e, IM_COL32(110, 170, 255, 200), 14.0f, 0, 1.5f);
        dl->PushClipRect(a, e, true);
        const std::string shown = editing ? g_addrEdit : (b.url.empty() ? g_url : b.url);
        const float ty = std::floor(my - fontPx(f.body) * 0.5f);
        if (editing && g_addrEdit.empty()) dl->AddText(f.body, fontPx(f.body), ImVec2(a.x + 14, ty), dim, "Type an address or a search");
        else dl->AddText(f.body, fontPx(f.body), ImVec2(a.x + 14, ty), ink, shown.c_str());
        if (editing) {   // the caret
            const float cx = a.x + 14 + f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, g_addrEdit.c_str()).x + 1;
            if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) dl->AddLine(ImVec2(cx, ty), ImVec2(cx, ty + fontPx(f.body)), ink, 1.5f);
        }
        dl->PopClipRect();
        // bookmark this page, and the keyboard
        const std::string cur = b.url;
        const int bmi = cur.empty() ? -1 : bookmarkIndex(cur);
        if (iconButton("##bstar", ImVec2(w - 86, top + 4), ImVec2(w - 52, top + barH - 4), !cur.empty() && b.status == 1)) {
            if (bmi >= 0) g_settings.bookmarks.erase(g_settings.bookmarks.begin() + bmi);
            else g_settings.bookmarks.push_back({b.title.empty() ? hostOf(cur) : b.title, cur});
            g_settingsChanged = true;
        }
        star(dl, ImVec2(w - 69, my), bmi >= 0, IM_COL32(255, 196, 80, 255));
        if (iconButton("##bkbd", ImVec2(w - 48, top + 4), ImVec2(w - 8, top + barH - 4), true)) { g_kbd = !(g_kbd && g_kbdTarget == 0); g_kbdTarget = 0; }
        keyboardIcon(dl, ImVec2(w - 28, my), g_kbd && g_kbdTarget == 0 ? IM_COL32(110, 170, 255, 255) : ink);
        // bookmarks: the starter set, then the user's own; pressing one loads it
        float x = 14;
        const float by = top + barH + 3;
        struct Bm { std::string name, url; };
        std::vector<Bm> bms = {{"ChartFox", kChartUrls[0]}, {"Navigraph", kChartUrls[1]}, {"SimBrief", kApps[APP_SIMBRIEF].url}, {"Volanta", kLiveUrls[0]},
                               {"VATSIM Map", kLiveUrls[1]}, {"ForeFlight", kPlanUrls[0]}, {"SkyVector", kPlanUrls[1]}};
        for (const auto& u : g_settings.bookmarks) bms.push_back({u.first.size() > 24 ? u.first.substr(0, 22) + ".." : u.first, u.second});
        int idx = 0;
        for (const Bm& bm : bms) {
            const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, bm.name.c_str());
            const float bw = sz.x + 22;
            if (x + bw > w - 10) break;
            char id[16];
            std::snprintf(id, sizeof id, "##bm%d", idx++);
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            const bool hit = ImGui::InvisibleButton(id, ImVec2(bw, bmH - 8));
            const bool hot = ImGui::IsItemHovered();
            const bool on = !cur.empty() && hostOf(cur) == hostOf(bm.url);
            dl->AddRectFilled(ImVec2(x, by), ImVec2(x + bw, by + bmH - 8), on ? IM_COL32(90, 150, 255, 90) : hot ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 20), 13.0f);
            dl->AddText(f.small, fontPx(f.small), ImVec2(x + 11, std::floor(by + (bmH - 8 - fontPx(f.small)) * 0.5f)), ink, bm.name.c_str());
            if (hit) { g_navPending = bm.url; g_navForce = true; g_url = bm.url; }
            x += bw + 8;
        }
        pageTop = top + barH + bmH;
        dl->AddLine(ImVec2(0, pageTop), ImVec2(w, pageTop), IM_COL32(255, 255, 255, 28));
    }
    if (b.loading && b.status == 1) {   // a thin moving bar while the page loads
        const float t = (float)std::fmod(ImGui::GetTime() * 0.8, 1.0);
        dl->AddRectFilled(ImVec2(w * t - w * 0.25f, pageTop - 2), ImVec2(w * t, pageTop + 1), IM_COL32(90, 160, 255, 255));
    }
    float kbH = 0;
    if (g_kbd) kbH = 5 * 34 + 4 * 5 + 16;
    webView(dl, f, s, 0, pageTop + 1, w, h - pageTop - 1 - kbH, act);
    if (g_kbd) keyboard(dl, f, w, h - kbH, act);
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

// ---- the map ---------------------------------------------------------------------------------------------------------------------
// OpenStreetMap or satellite tiles (the host fetches them), X-Plane's airport layouts over them, the aircraft, and WXPlane's map
// bubbles: the time (Zulu or local, with the zone) at the top left; Center On Aircraft and North / Heading Lock at the top right.
double g_mapZoom = 14.0;             // Web Mercator zoom: the world is 256 * 2^zoom layout units across
bool g_mapFollow = true;             // the map stays centred on the aircraft
MercPt g_mapCenter{0.5, 0.5};
bool g_mapSeen = false;              // the map has had the aircraft's position once
constexpr double kMapMinZoom = 2.0, kMapMaxZoom = 19.5;
constexpr double kEarthM = 40075016.686;   // the equator's length

// Local time at a point (WXPlane's rule): X-Plane's own time where the point shares the aircraft's clock (within 250 nm), else the
// time zone map's standard offset plus daylight time where it is kept; else, off the map, an estimate from the longitude (~).
double localOffsetAt(const MapState& m, double lat, double lon, bool& estimate) {
    estimate = false;
    int q = 0;
    const bool onMap = tzStdQuarterHours(lat, lon, q);
    const std::time_t now = (std::time_t)m.utc;
    if (m.haveLocalOffset && m.havePos) {
        const double dLat = (lat - m.lat) * 60.0, dLon = (lon - m.lon) * 60.0 * std::cos(m.lat * kPi / 180.0);
        if (std::sqrt(dLat * dLat + dLon * dLon) < 250.0) {
            int qa = 0;
            const bool acOnMap = tzStdQuarterHours(m.lat, m.lon, qa);
            if (!onMap || !acOnMap || q * 900.0 + wx::dstShiftS(lat, lon, q, now) == qa * 900.0 + wx::dstShiftS(m.lat, m.lon, qa, now)) return m.localOffsetS;
        }
    }
    if (onMap) return q * 900.0 + wx::dstShiftS(lat, lon, q, now);
    estimate = true;
    return std::round(lon / 15.0) * 3600.0;
}
std::string clockOf(long long t) {
    t = ((t % 86400) + 86400) % 86400;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60));
    return b;
}
std::string zoneOf(double lat, double lon, double offS, long long utc) {
    const std::string n = wx::zoneName(lat, lon, offS, (std::time_t)utc);
    if (!n.empty()) return n;
    const int mins = (int)std::lround(std::fabs(offS) / 60.0);
    char b[24];
    if (mins % 60) std::snprintf(b, sizeof b, "UTC%s%d:%02d", offS < 0 ? "-" : "+", mins / 60, mins % 60);
    else std::snprintf(b, sizeof b, "UTC%s%d", offS < 0 ? "-" : "+", mins / 60);
    return b;
}

// A bubble on the map: a dark rounded box with a small label and a value; lit (blue) when on. Returns true when pressed.
bool mapBubble(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, const char* label, const char* text, bool lit, float minW = 0) {
    const float lw = label ? f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, label).x + 7 : 0;
    const float tw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, text).x;
    const ImVec2 b(a.x + std::max(minW, lw + tw + 20), a.y + 30);
    ImGui::SetCursorScreenPos(a);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, hov ? IM_COL32(26, 40, 64, 238) : IM_COL32(12, 22, 40, 225), 9.0f);
    dl->AddRect(a, b, lit ? IM_COL32(110, 170, 255, 255) : IM_COL32(255, 255, 255, 46), 9.0f, 0, lit ? 1.6f : 1.0f);
    const float tx = a.x + (b.x - a.x - lw - tw) * 0.5f;
    if (label) textAt(dl, f.small, tx, std::floor(a.y + (30 - fontPx(f.small)) * 0.5f + 1), kMuted, label);
    textAt(dl, f.body, tx + lw, std::floor(a.y + (30 - fontPx(f.body)) * 0.5f), lit ? IM_COL32(150, 196, 255, 255) : kInk, text);
    return hit;
}
float bubbleW(const Fonts& f, const char* label, const char* text) {
    const float lw = label ? f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, label).x + 7 : 0;
    return lw + f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, text).x + 20;
}

// A dashed polyline (screen points)
void dashed(ImDrawList* dl, const std::vector<ImVec2>& p, ImU32 col, float thick, float on, float off) {
    float phase = 0;
    for (size_t i = 1; i < p.size(); ++i) {
        const ImVec2 a = p[i - 1], b = p[i];
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        if (len < 1e-3f) continue;
        float t = 0;
        while (t < len) {
            const float period = on + off, inP = std::fmod(phase + t, period);
            if (inP < on) {
                const float t1 = std::min(len, t + (on - inP));
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len), ImVec2(a.x + (b.x - a.x) * t1 / len, a.y + (b.y - a.y) * t1 / len), col, thick);
                t = t1;
            } else {
                t += period - inP;
            }
        }
        phase = std::fmod(phase + len, on + off);
    }
}

void mapPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const MapState& m = s.map;
    const float top = kStatusH + kHeaderH;
    const ImVec2 r0(0, top), r1(w, h);
    const ImVec2 sc((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
    const MercPt ac = m.havePos ? mercOf(m.lat, m.lon) : MercPt{0.5, 0.5};
    // without a position the map starts on the whole world; the first position brings it in to the aircraft
    if (m.havePos && !g_mapSeen) { g_mapSeen = true; g_mapCenter = ac; g_mapFollow = true; if (g_mapZoom < 8) g_mapZoom = 14.0; }
    if (!m.havePos && !g_mapSeen) g_mapZoom = std::min(g_mapZoom, 3.0);
    if (g_mapFollow && m.havePos) g_mapCenter = ac;
    const bool headingUp = g_settings.mapHeadingUp && m.havePos;
    const double rot = headingUp ? m.hdgTrue * kPi / 180.0 : 0.0;
    const double cr = std::cos(rot), sr = std::sin(rot);
    double scale = 256.0 * std::pow(2.0, g_mapZoom);          // layout units per Mercator unit
    double cLat = 0, cLon = 0;
    latLonOf(g_mapCenter, cLat, cLon);
    // the world to the screen and back (heading up: the world turned by minus the heading about the middle)
    auto toS = [&](const MercPt& p) {
        double dx = (p.x - g_mapCenter.x), dy = (p.y - g_mapCenter.y);
        if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1;   // across the date line
        dx *= scale; dy *= scale;
        return ImVec2((float)(sc.x + dx * cr + dy * sr), (float)(sc.y - dx * sr + dy * cr));
    };
    auto fromS = [&](ImVec2 q) {
        const double x = q.x - sc.x, y = q.y - sc.y;
        MercPt p;
        p.x = g_mapCenter.x + (x * cr - y * sr) / scale;
        p.y = g_mapCenter.y + (x * sr + y * cr) / scale;
        return p;
    };
    const bool sat = g_settings.mapSource == 1;
    dl->PushClipRect(r0, r1, true);
    dl->AddRectFilled(r0, r1, sat ? IM_COL32(18, 24, 30, 255) : IM_COL32(232, 228, 220, 255));

    // ---- input: drag to move, the wheel to zoom (about the pointer); bubbles drawn later sit on top of it
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##map", ImVec2(r1.x - r0.x, r1.y - r0.y));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && ImGui::IsMouseDragging(0, 2.0f)) {
        const double x = -io.MouseDelta.x, y = -io.MouseDelta.y;
        g_mapCenter.x += (x * cr - y * sr) / scale;
        g_mapCenter.y += (x * sr + y * cr) / scale;
        g_mapCenter.x -= std::floor(g_mapCenter.x);
        g_mapCenter.y = std::max(0.0, std::min(1.0, g_mapCenter.y));
        g_mapFollow = false;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        const double z0 = g_mapZoom;
        g_mapZoom = std::max(kMapMinZoom, std::min(kMapMaxZoom, g_mapZoom + io.MouseWheel * 0.5));
        if (!g_mapFollow) {   // the place under the pointer stays under it
            const MercPt under = fromS(io.MousePos);
            const double k = std::pow(2.0, z0 - g_mapZoom);
            g_mapCenter.x = under.x + (g_mapCenter.x - under.x) * k;
            g_mapCenter.y = under.y + (g_mapCenter.y - under.y) * k;
        }
        scale = 256.0 * std::pow(2.0, g_mapZoom);
    }
    latLonOf(g_mapCenter, cLat, cLon);

    // ---- the tiles: the zoom level that gives about one tile pixel per screen pixel, nearest the middle first
    const int src = sat ? 1 : 0;
    const double pxPer = std::max(1.0f, s.pixelScale);
    const int tz = std::max(1, std::min(19, (int)std::floor(g_mapZoom + std::log2(pxPer) + 0.3)));
    const int nT = 1 << tz;
    const ImVec2 corners[4] = {r0, ImVec2(r1.x, r0.y), r1, ImVec2(r0.x, r1.y)};
    double mx0 = 1e9, my0 = 1e9, mx1 = -1e9, my1 = -1e9;
    for (const ImVec2& c : corners) { const MercPt p = fromS(c); mx0 = std::min(mx0, p.x); mx1 = std::max(mx1, p.x); my0 = std::min(my0, p.y); my1 = std::max(my1, p.y); }
    const int tx0 = (int)std::floor(mx0 * nT), tx1 = (int)std::floor(mx1 * nT), ty0 = std::max(0, (int)std::floor(my0 * nT)), ty1 = std::min(nT - 1, (int)std::floor(my1 * nT));
    struct TileAt { int x, y; double d; };
    std::vector<TileAt> want;
    if (tx1 - tx0 < 40 && ty1 - ty0 < 40)
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const double cx = (tx + 0.5) / nT - g_mapCenter.x, cy = (ty + 0.5) / nT - g_mapCenter.y;
                want.push_back({tx, ty, cx * cx + cy * cy});
            }
    std::sort(want.begin(), want.end(), [](const TileAt& a, const TileAt& b) { return a.d < b.d; });
    int missing = 0;
    for (const TileAt& t : want) {
        const int wx = ((t.x % nT) + nT) % nT;
        const MercPt q0{(double)t.x / nT, (double)t.y / nT}, q1{(double)(t.x + 1) / nT, (double)(t.y + 1) / nT};
        const ImVec2 p00 = toS(q0), p10 = toS(MercPt{q1.x, q0.y}), p11 = toS(q1), p01 = toS(MercPt{q0.x, q1.y});
        ImTextureID tex = m.tile ? m.tile(src, tz, wx, t.y, true) : 0;
        ImVec2 u0(0, 0), u1(1, 1);
        for (int dz = 1; !tex && m.tile && dz <= 6 && tz - dz >= 0; ++dz) {   // a coarser tile, stretched, until this one comes
            const int px = wx >> dz, py = t.y >> dz;
            tex = m.tile(src, tz - dz, px, py, false);
            if (tex) {
                const float k = 1.0f / (float)(1 << dz);
                u0 = ImVec2((wx - (px << dz)) * k, (t.y - (py << dz)) * k);
                u1 = ImVec2(u0.x + k, u0.y + k);
            }
        }
        if (tex) dl->AddImageQuad(tex, p00, p10, p11, p01, u0, ImVec2(u1.x, u0.y), u1, ImVec2(u0.x, u1.y));
        else {
            ++missing;
            if (!m.tile) {   // the off-line test: a quiet checkerboard where the tiles would be
                const ImU32 c = ((t.x + t.y) & 1) ? (sat ? IM_COL32(40, 52, 44, 255) : IM_COL32(222, 218, 208, 255)) : (sat ? IM_COL32(34, 46, 38, 255) : IM_COL32(236, 232, 224, 255));
                dl->AddQuadFilled(p00, p10, p11, p01, c);
            }
        }
    }

    // ---- the time zones: dashed violet boundaries
    if (g_settings.mapTimeZones) {
        double la0, lo0, la1, lo1;
        latLonOf(MercPt{mx0, my1}, la0, lo0);
        latLonOf(MercPt{mx1, my0}, la1, lo1);
        std::vector<ImVec2> pts;
        for (const TzLine& L : tzLines()) {
            if (L.lo1 < lo0 || L.lo0 > lo1 || L.la1 < la0 || L.la0 > la1) continue;
            pts.clear();
            for (const ImVec2& q : L.pts) pts.push_back(toS(mercOf(q.y, q.x)));
            if (g_mapZoom < 6) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(182, 120, 255, 200), 0, 1.2f);   // far out: plain (fewer vertices)
            else dashed(dl, pts, IM_COL32(182, 120, 255, 230), 1.6f, 7.0f, 5.0f);
        }
    }

    // ---- X-Plane's airports
    const double mPerMerc = kEarthM * std::cos(cLat * kPi / 180.0);
    const double halfDiagNm = std::sqrt((double)(w * w + (h - top) * (h - top))) * 0.5 / scale * mPerMerc / 1852.0;
    if (g_settings.mapAirports) {
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const bool layouts = g_mapZoom >= 11.5;
        std::vector<std::string> drawn;
        if (layouts) {
            for (const auto& ap : m.airports) {
                const AptGeo& g = *ap;
                // off screen: skipped
                const ImVec2 b0 = toS(g.lo), b1 = toS(g.hi), b2 = toS(MercPt{g.lo.x, g.hi.y}), b3 = toS(MercPt{g.hi.x, g.lo.y});
                const float bx0 = std::min(std::min(b0.x, b1.x), std::min(b2.x, b3.x)), bx1 = std::max(std::max(b0.x, b1.x), std::max(b2.x, b3.x));
                const float by0 = std::min(std::min(b0.y, b1.y), std::min(b2.y, b3.y)), by1 = std::max(std::max(b0.y, b1.y), std::max(b2.y, b3.y));
                if (bx1 < r0.x || bx0 > r1.x || by1 < r0.y || by0 > r1.y) continue;
                drawn.push_back(g.icao);
                const double mpm = kEarthM * std::cos(g.lat * kPi / 180.0);   // metres per Mercator unit here
                const float pxPerM = (float)(scale / mpm);
                // the pavement
                const ImU32 pave = sat ? IM_COL32(70, 76, 88, 130) : IM_COL32(146, 150, 160, 255);
                const int nv = (int)g.pavement.size() / 3 * 3;
                for (int i = 0; i < nv; i += 3 * 2000) {   // in batches (16-bit indices)
                    const int cnt = std::min(nv - i, 3 * 2000);
                    dl->PrimReserve(cnt, cnt);
                    for (int k = 0; k < cnt; ++k) {
                        const ImVec2 q = toS(g.pavement[i + k]);
                        dl->PrimWriteVtx(q, uv, pave);
                        dl->PrimWriteIdx((ImDrawIdx)(dl->_VtxCurrentIdx - 1));
                    }
                }
                // the runways: dark, with a dashed white centreline and the numbers at each end
                for (const AptGeo::Runway& rw : g.runways) {
                    const ImVec2 a = toS(rw.a), b = toS(rw.b);
                    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    if (len < 1) continue;
                    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len), nrm(-d.y, d.x);
                    const float hw = std::max(1.5f, rw.widthM * 0.5f * pxPerM);
                    const ImU32 rc = rw.water ? IM_COL32(60, 120, 200, 160) : IM_COL32(44, 48, 56, 255);
                    dl->AddQuadFilled(ImVec2(a.x + nrm.x * hw, a.y + nrm.y * hw), ImVec2(b.x + nrm.x * hw, b.y + nrm.y * hw),
                                      ImVec2(b.x - nrm.x * hw, b.y - nrm.y * hw), ImVec2(a.x - nrm.x * hw, a.y - nrm.y * hw), rc);
                    if (hw > 4 && !rw.water) dashed(dl, {a, b}, IM_COL32(255, 255, 255, 220), std::max(1.0f, hw * 0.08f), 30.0f * pxPerM, 20.0f * pxPerM);
                    if (g_mapZoom >= 13) {
                        textCentered(dl, f.bold, a.x - d.x * 14, a.y - d.y * 14 - fontPx(f.bold) * 0.5f, sat ? kInk : IM_COL32(20, 24, 32, 255), rw.idA.c_str());
                        textCentered(dl, f.bold, b.x + d.x * 14, b.y + d.y * 14 - fontPx(f.bold) * 0.5f, sat ? kInk : IM_COL32(20, 24, 32, 255), rw.idB.c_str());
                    }
                }
                // the painted lines
                std::vector<ImVec2> pts;
                for (const AptGeo::Line& L : g.lines) {
                    if (L.kind == 3 && g_mapZoom < 15) continue;
                    pts.clear();
                    for (const MercPt& p : L.pts) pts.push_back(toS(p));
                    const ImU32 col = L.kind == 1 ? IM_COL32(236, 64, 72, 255) : L.kind == 2 ? IM_COL32(255, 168, 60, 255)
                                    : L.kind == 3 ? IM_COL32(250, 214, 60, 150) : IM_COL32(250, 210, 40, 235);
                    const float th = L.kind == 1 ? 2.4f : L.kind == 2 ? 1.8f : L.kind == 3 ? 0.8f : (g_mapZoom >= 15 ? 1.4f : 1.0f);
                    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, th);
                }
                // the stands and the taxiway names, close in
                if (g_mapZoom >= 15.5)
                    for (const AptGeo::Stand& st : g.stands) {
                        const ImVec2 q = toS(st.p);
                        if (q.x < r0.x - 40 || q.x > r1.x + 40 || q.y < r0.y - 20 || q.y > r1.y + 20) continue;
                        dl->AddCircleFilled(q, 3.0f, IM_COL32(255, 255, 255, 235), 10);
                        dl->AddCircle(q, 3.0f, IM_COL32(20, 24, 32, 200), 10, 1.0f);
                        if (g_mapZoom >= 16.5 && !st.name.empty()) textAt(dl, f.small, q.x + 5, q.y - fontPx(f.small) * 0.5f, sat ? kInk : IM_COL32(30, 36, 48, 255), st.name.c_str());
                    }
                if (g_mapZoom >= 14.5)
                    for (const AptGeo::Label& lb : g.taxiways) {
                        const ImVec2 q = toS(lb.p);
                        if (q.x < r0.x || q.x > r1.x || q.y < r0.y || q.y > r1.y) continue;
                        const ImVec2 ts = f.bold->CalcTextSizeA(fontPx(f.small), 1e9f, 0, lb.text.c_str());
                        dl->AddRectFilled(ImVec2(q.x - ts.x * 0.5f - 4, q.y - ts.y * 0.5f - 1), ImVec2(q.x + ts.x * 0.5f + 4, q.y + ts.y * 0.5f + 1), IM_COL32(250, 210, 40, 255), 3.0f);
                        dl->AddText(f.bold, fontPx(f.small), ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), IM_COL32(16, 16, 16, 255), lb.text.c_str());
                    }
            }
        }
        // further out: each airport as a dot with its code
        if (g_mapZoom >= 7.0) {
            for (const AptPoint& p : m.airportPoints) {
                if (layouts && std::find(drawn.begin(), drawn.end(), p.icao) != drawn.end()) continue;
                const ImVec2 q = toS(mercOf(p.lat, p.lon));
                if (q.x < r0.x - 20 || q.x > r1.x + 20 || q.y < r0.y - 10 || q.y > r1.y + 10) continue;
                const ImU32 dot = p.kind == 17 ? IM_COL32(180, 90, 220, 255) : p.kind == 16 ? IM_COL32(40, 140, 220, 255) : IM_COL32(32, 92, 200, 255);
                dl->AddCircleFilled(q, 4.0f, IM_COL32(255, 255, 255, 230), 12);
                dl->AddCircleFilled(q, 2.8f, dot, 12);
                if (g_mapZoom >= 8.5) {
                    const ImU32 tc = sat ? kInk : IM_COL32(24, 40, 80, 255);
                    textAt(dl, f.small, q.x + 6, q.y - fontPx(f.small) * 0.5f, tc, p.icao.c_str());
                }
            }
        }
        act.mapLayouts = layouts;
        act.mapLayoutNm = std::min(25.0, std::max(3.0, halfDiagNm * 1.4));
        act.mapPointNm = g_mapZoom >= 7.0 ? std::min(400.0, halfDiagNm * 1.2) : 0.0;
    }
    act.mapShown = true;
    act.mapLat = cLat; act.mapLon = cLon;

    // ---- the aircraft: its own outline at its real size when that is bigger than the symbol, else the symbol
    if (m.havePos) {
        const ImVec2 q = toS(ac);
        const double a = (m.hdgTrue * kPi / 180.0) - rot;   // on screen, clockwise from up
        const float sa = (float)std::sin(a), ca = (float)std::cos(a);
        float sl = 0, ss = 0;
        const Shape* sh = findShape(s.acfIcao, sl, ss);
        const float pxPerM = (float)(scale / mPerMerc);
        if (sh && sl * pxPerM > 34) {
            // the drawing: nose at y = 0, going back with +y; turned so the nose points along the heading, centred on its middle
            const float kx = ss / sh->span * pxPerM, ky = sl / sh->len * pxPerM;
            std::vector<ImVec2> pts;
            auto place = [&](const std::vector<ImVec2>& p) {
                pts.resize(p.size());
                for (size_t i = 0; i < p.size(); ++i) {
                    const float x = p[i].x * kx, y = (p[i].y - sh->len * 0.5f) * ky;   // about the middle, +y towards the tail
                    pts[i] = ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca);
                }
            };
            for (const auto& p : sh->fills) { place(p); if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 245)); }
            for (const auto& p : sh->lines) { place(p); if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(40, 70, 130, 255), 0, 1.0f); }
        } else {
            auto P = [&](float x, float y) { return ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca); };   // y: forward is -y
            const ImVec2 body[] = {P(0, -13), P(2.2f, -9), P(2.2f, -3), P(12, 3), P(12, 5.5f), P(2.2f, 2.5f), P(1.8f, 9), P(5, 11.5f), P(5, 13),
                                   P(0, 11.5f), P(-5, 13), P(-5, 11.5f), P(-1.8f, 9), P(-2.2f, 2.5f), P(-12, 5.5f), P(-12, 3), P(-2.2f, -3), P(-2.2f, -9)};
            std::vector<ImVec2> v(body, body + 18);
            if (ImGui::GetIO().DisplaySize.x > 0) {
                // clockwise on screen for the concave fill
                float area = 0;
                for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) area += (v[j].x - v[i].x) * (v[j].y + v[i].y);
                if (area > 0) std::reverse(v.begin(), v.end());
            }
            dl->AddConcavePolyFilled(v.data(), (int)v.size(), IM_COL32(255, 214, 60, 255));
            dl->AddPolyline(v.data(), (int)v.size(), IM_COL32(20, 24, 32, 255), ImDrawFlags_Closed, 1.3f);
        }
    }

    // ---- the bubbles
    {   // the time: Zulu, or local (with its zone) at the aircraft, or at the + in the middle while the map is moved off it
        const bool movedOff = !g_mapFollow || !m.havePos;
        if (movedOff) {
            const float a = 6.0f, t = 1.5f;
            dl->AddLine(ImVec2(sc.x - a, sc.y), ImVec2(sc.x + a, sc.y), IM_COL32(0, 0, 0, 120), t + 1.5f);
            dl->AddLine(ImVec2(sc.x, sc.y - a), ImVec2(sc.x, sc.y + a), IM_COL32(0, 0, 0, 120), t + 1.5f);
            dl->AddLine(ImVec2(sc.x - a, sc.y), ImVec2(sc.x + a, sc.y), IM_COL32(255, 255, 255, 230), t);
            dl->AddLine(ImVec2(sc.x, sc.y - a), ImVec2(sc.x, sc.y + a), IM_COL32(255, 255, 255, 230), t);
        }
        const long long utc = m.utc ? m.utc : (s.zuluSec >= 0 ? s.zuluSec : 0);
        std::string t;
        if (g_settings.mapLocalTime) {
            bool est = false;
            const double tLat = movedOff ? cLat : m.lat, tLon = movedOff ? cLon : m.lon;
            const double off = localOffsetAt(m, tLat, tLon, est);
            t = clockOf(utc + (long long)off) + " " + (est ? "~" : "") + zoneOf(tLat, tLon, off, utc);
        } else {
            t = clockOf(utc) + "Z";
        }
        if (mapBubble(dl, f, "##mtime", ImVec2(r0.x + 10, r0.y + 10), g_settings.mapLocalTime ? "LOCAL" : "TIME", t.c_str(), g_settings.mapLocalTime)) {
            g_settings.mapLocalTime = !g_settings.mapLocalTime; g_settingsChanged = true;
        }
    }
    {   // North / Heading Lock, and Center On Aircraft
        char lk[40];
        if (headingUp) { const int hd = ((int)std::lround(m.hdgTrue) % 360 + 360) % 360; std::snprintf(lk, sizeof lk, "Heading Lock %03d\xC2\xB0T", hd == 0 ? 360 : hd); }
        else std::snprintf(lk, sizeof lk, "%s", g_settings.mapHeadingUp ? "Heading Lock" : "North Lock");
        const float wl = bubbleW(f, nullptr, lk), wc = bubbleW(f, nullptr, "Center On Aircraft");
        if (mapBubble(dl, f, "##mlock", ImVec2(r1.x - 10 - wl, r0.y + 10), nullptr, lk, g_settings.mapHeadingUp)) { g_settings.mapHeadingUp = !g_settings.mapHeadingUp; g_settingsChanged = true; }
        if (mapBubble(dl, f, "##mcenter", ImVec2(r1.x - 10 - wl - 6 - wc, r0.y + 10), nullptr, "Center On Aircraft", g_mapFollow && m.havePos) && m.havePos) g_mapFollow = true;
    }
    {   // along the bottom: the zoom, then the map's kind and its layers
        const float y = r1.y - 40;
        char rng[24];
        const double widthNm = (r1.x - r0.x) / scale * mPerMerc / 1852.0;
        if (widthNm >= 100) std::snprintf(rng, sizeof rng, "%.0f nm", widthNm);
        else if (widthNm >= 10) std::snprintf(rng, sizeof rng, "%.0f nm", widthNm);
        else std::snprintf(rng, sizeof rng, "%.1f nm", widthNm);
        float x = r0.x + 10;
        if (mapBubble(dl, f, "##mzout", ImVec2(x, y), nullptr, "-", false, 34)) g_mapZoom = std::max(kMapMinZoom, std::floor(g_mapZoom - 0.5));
        x += 34 + 4;
        const float rw = bubbleW(f, nullptr, "2500 nm");
        mapBubble(dl, f, "##mrange", ImVec2(x, y), nullptr, rng, false, rw);
        x += rw + 4;
        if (mapBubble(dl, f, "##mzin", ImVec2(x, y), nullptr, "+", false, 34)) g_mapZoom = std::min(kMapMaxZoom, std::floor(g_mapZoom + 1.0));
        x += 34 + 12;
        if (mapBubble(dl, f, "##mosm", ImVec2(x, y), nullptr, "Map", !sat)) { g_settings.mapSource = 0; g_settingsChanged = true; }
        x += bubbleW(f, nullptr, "Map") + 4;
        if (mapBubble(dl, f, "##msat", ImVec2(x, y), nullptr, "Satellite", sat)) { g_settings.mapSource = 1; g_settingsChanged = true; }
        x += bubbleW(f, nullptr, "Satellite") + 12;
        if (mapBubble(dl, f, "##mapt", ImVec2(x, y), nullptr, "Airports", g_settings.mapAirports)) { g_settings.mapAirports = !g_settings.mapAirports; g_settingsChanged = true; }
        x += bubbleW(f, nullptr, "Airports") + 4;
        if (mapBubble(dl, f, "##mtz", ImVec2(x, y), nullptr, "Time Zones", g_settings.mapTimeZones)) { g_settings.mapTimeZones = !g_settings.mapTimeZones; g_settingsChanged = true; }
        // where the map comes from, and whether the airports are still being found
        const char* attr = sat ? "Imagery: Esri, Maxar, Earthstar Geographics" : "\xC2\xA9 OpenStreetMap contributors";
        const ImVec2 as = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, attr);
        dl->AddRectFilled(ImVec2(r1.x - as.x - 12, r1.y - as.y - 6), r1, IM_COL32(255, 255, 255, 190), 4.0f, ImDrawFlags_RoundCornersTopLeft);
        dl->AddText(f.small, fontPx(f.small), ImVec2(r1.x - as.x - 6, r1.y - as.y - 3), IM_COL32(30, 36, 48, 255), attr);
        if (g_settings.mapAirports && m.aptIndexing) textAt(dl, f.small, r0.x + 12, y - 22, sat ? kInk : IM_COL32(30, 36, 48, 255), "Finding X-Plane's airports...");
        (void)missing;
    }
    dl->PopClipRect();
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
    // the doors with a jetway bubble, as the plugin found them (a ToLiss's; openSAM's count otherwise); only airliners, 24 m or longer
    const int jwDoors = g.toliss ? g.jwDoors : (acLen >= 24.0f ? std::max(1, g.jwDoors) : 0);
    const bool doors = jwDoors > 0;
    bool anyDocked = false;
    for (int k = 0; k < jwDoors && k < 3; ++k) anyDocked = anyDocked || (g.openSam ? g.jwDocked[k] : g.jetway == 2);
    const float boxTop = a.y + 34, boxBot = e.y - (doors && jwDoors > 1 ? 30 : 14);
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
        for (int k = 0; k < jwDoors && k < 3; ++k) {
            const int pax = g.jwDoorPax[k];
            const float frac = paxDoorAt(pax / 2, std::max(1, pairs));
            const float y = nose.y + frac * acLen * px;
            float hw = hwPx;   // the fuselage's edge at this door (it narrows towards the tail)
            if (shape && hw0 > 0) { const float hh = fuselageHalf(*shape, frac, hw0); if (hh > 0) hw = hh * kx * px; }
            const float side = nose.x - hw;
            const bool docked = g.openSam ? g.jwDocked[k] : g.jetway == 2;
            const bool open = g.toliss && pax >= 0 && pax < 8 && g.paxDoor[pax] == 2;
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
            const char* st = !g.openSam ? (docked ? (g.toliss ? "connected, door open" : "connected") : "X-Plane jetway")
                           : docked ? (g.toliss ? "docked, door open" : "docked") : g.jetway == 1 ? "jetway ready" : g.jetway == 0 ? "no jetway here" : g.jetway == 2 ? "jetway at the other door" : "jetway moving";
            std::snprintf(b, sizeof b, "%d%c  \xC2\xB7  %s", pax / 2 + 1, 'L', st);
            textAt(dl, f.small, bx + 4, y + 17, docked ? kGood : kMuted, b);
        }
        if (jwDoors > 1) textCentered(dl, f.small, (a.x + e.x) * 0.5f, e.y - 24, kMuted, g.openSam ? "openSAM moves every jetway at the stand together." : "X-Plane connects every door's jetway together.");
    }

    // ---- the right side: the cabin (top two thirds) and whether a jetway can connect (bottom third)
    const float rx = e.x + 10, rw = w - 12 - rx;
    const float split = top + std::floor((bot - top) * 2.0f / 3.0f);
    card(dl, ImVec2(rx, top), ImVec2(rx + rw, split - 5), f, "CABIN");
    card(dl, ImVec2(rx, split + 5), ImVec2(rx + rw, bot), f, "JETWAY STATUS");
    const float cx0 = rx + 16, cx1 = rx + rw - 16;
    if (!g.toliss) {
        dl->AddText(f.body, fontPx(f.body), ImVec2(cx0, top + 40), kMuted, "Cabin lights and slide arming are for ToLiss aircraft.", nullptr, cx1 - cx0);
    } else {
        float y = top + 38;
        // the lights: a slider in steps of 10 %, with On (full) and Off under it
        textAt(dl, f.bold, cx0, y, kInk, "Cabin lights");
        char lv[16];
        const int pct = g.cabinLight < 0 ? -1 : (int)std::lround(g.cabinLight * 100);
        if (pct < 0) std::snprintf(lv, sizeof lv, "-");
        else if (pct == 0) std::snprintf(lv, sizeof lv, "Off");
        else std::snprintf(lv, sizeof lv, "%d %%", pct);
        textRight(dl, f.bold, cx1, y, pct > 0 ? kInk : kMuted, lv);
        y += 30;
        if (g.cabinLight >= 0) {
            // the track, its fill, ticks at every 10 % and the handle; pressing or dragging on it sets the level, in 10 % clicks
            const float tx0 = cx0 + 10, tx1 = cx1 - 10, ty = y + 14;
            ImGui::SetCursorScreenPos(ImVec2(cx0, y));
            ImGui::InvisibleButton("##cablight", ImVec2(cx1 - cx0, 28));
            int shown = pct;
            if (ImGui::IsItemActive()) {
                const float t = std::max(0.0f, std::min(1.0f, (ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0)));
                const int want = (int)std::lround(t * 10) * 10;
                shown = want;
                if (want != pct) { act.ground = GA_CABIN_LIGHT; act.groundIndex = want; }
            }
            const float hx = tx0 + (tx1 - tx0) * std::max(0, shown) / 100.0f;
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(tx1, ty + 3), IM_COL32(255, 255, 255, 40), 3.0f);
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(hx, ty + 3), IM_COL32(255, 214, 120, 255), 3.0f);
            for (int i = 0; i <= 10; ++i) {
                const float x = tx0 + (tx1 - tx0) * i / 10.0f;
                dl->AddLine(ImVec2(x, ty + 7), ImVec2(x, ty + (i % 5 == 0 ? 13 : 10)), IM_COL32(255, 255, 255, 90), 1.0f);
            }
            dl->AddCircleFilled(ImVec2(hx, ty), 10.0f, IM_COL32(245, 248, 255, 255), 24);
            dl->AddCircle(ImVec2(hx, ty), 10.0f, IM_COL32(255, 214, 120, 255), 24, 2.0f);
            y += 40;
            const float bw = (cx1 - cx0 - 10) * 0.5f;
            if (pill(dl, f, "##clon", "On", ImVec2(cx0, y), ImVec2(cx0 + bw, y + 30), pct >= 100, true)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = 100; }
            if (pill(dl, f, "##cloff", "Off", ImVec2(cx1 - bw, y), ImVec2(cx1, y + 30), pct == 0, true)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = 0; }
            y += 46;
        } else {
            textAt(dl, f.small, cx0, y, kMuted, "This aircraft does not offer its cabin lights.");
            y += 34;
        }
        dl->AddLine(ImVec2(cx0, y - 6), ImVec2(cx1, y - 6), IM_COL32(255, 255, 255, 18));
        // the slides
        textAt(dl, f.bold, cx0, y + 4, kInk, "Slides");
        if (g.slides >= 0) textRight(dl, f.bold, cx1, y + 4, g.slides == 1 ? kWarn : kMuted, g.slides == 1 ? "Armed" : "Disarmed");
        y += 32;
        const float bw = (cx1 - cx0 - 10) * 0.5f;
        if (pill(dl, f, "##arm", "Arm", ImVec2(cx0, y), ImVec2(cx0 + bw, y + 30), g.slides == 1, true)) act.ground = GA_SLIDES_ARM;
        if (pill(dl, f, "##disarm", "Disarm", ImVec2(cx1 - bw, y), ImVec2(cx1, y + 30), g.slides == 0, true)) act.ground = GA_SLIDES_DISARM;
    }
    {   // the jetway status: Good, or what keeps a jetway from connecting
        std::vector<std::string> why;
        if (!g.onGround) why.push_back("Not on the ground");
        else if (g.groundSpeedKt > 1.0f) why.push_back("Aircraft moving");
        if (g.beacon) why.push_back("Beacon light on");
        if (g.enginesRunning) why.push_back("Engines running");
        if (!g.parkBrake && !g.chocks) why.push_back("Chocks or parking brake not set");
        if (g.openSam && why.empty()) {
            if (g.jetway == 0) why.push_back("No jetway at this stand");
            else if (g.jetway == -1 && !anyDocked) why.push_back("Jetway moving, or none within reach");
        }
        float y = split + 5 + 32;
        if (why.empty()) {
            textAt(dl, f.title, cx0, y, kGood, anyDocked ? "Good  \xC2\xB7  connected" : "Good");
            textAt(dl, f.small, cx0, y + 30, kMuted, g.openSam ? "openSAM jetways" : "X-Plane's own jetways");
        } else {
            for (const std::string& t : why) {
                if (y > bot - 22) break;
                dl->AddCircleFilled(ImVec2(cx0 + 4, y + fontPx(f.body) * 0.5f + 1), 3.5f, kWarn, 10);
                textAt(dl, f.body, cx0 + 16, y, kInk, t.c_str());
                y += 22;
            }
        }
    }
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
        {"Web pages", "Chromium Embedded Framework (Marshall A. Greenblatt and contributors), X-Plane's own copy of Chromium", "BSD 3-clause"},
        {"Map", "\xC2\xA9 OpenStreetMap contributors (openstreetmap.org/copyright); satellite imagery: Esri, Maxar, Earthstar Geographics", "ODbL; Esri terms"},
        {"Airport layouts on the map", "X-Plane's airport data (the Scenery Gateway and its contributors) and your installed scenery", "GNU GPL"},
        {"Time zones", "Natural Earth (via WXPlane)", "Public domain"},
        {"Map pictures", "stb_image by Sean Barrett", "MIT / public domain"},
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
    choice("Flight planning icon", kPlanNames, g_settings.planProvider, g_settings.showForeflight);
    sectionTitle(f, "Show on the home screen");
    struct Tog { const char* name; bool* v; };
    const Tog togs[] = {{"Charts", &g_settings.showCharts}, {"Live map", &g_settings.showLive}, {"SimBrief", &g_settings.showSimbrief}, {"Planning", &g_settings.showForeflight}};
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
    g_navPending = url; g_navForce = false;
    if (g_kbdTarget == 1) { g_kbd = false; g_kbdTarget = 0; }
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
    // big draw lists (the map's lines) go past 65536 vertices: the renderer follows each command's vertex offset
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
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
        else if (app == APP_BROWSER) browserPage(dl, f, s, w, h, act);
        else if (app == APP_MAP) mapPage(dl, f, s, w, h, act);
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
    if (app == APP_FOREFLIGHT) return kPlanNames[planP()];
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
    if (app == APP_FOREFLIGHT) { openWeb(kPlanNames[planP()], kPlanUrls[planP()], false); return; }
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
    std::snprintf(buf, sizeof buf, "plan_provider=%d\nmap_source=%d\nmap_airports=%d\nmap_timezones=%d\nmap_heading_up=%d\nmap_local_time=%d\n", planP(),
                  g_settings.mapSource == 1 ? 1 : 0, g_settings.mapAirports ? 1 : 0, g_settings.mapTimeZones ? 1 : 0, g_settings.mapHeadingUp ? 1 : 0, g_settings.mapLocalTime ? 1 : 0);
    out += buf;
    auto list = [&](const char* key, const std::vector<int>& v) {
        out += key; out += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ","; out += kApps[v[i]].id; }
        out += "\n";
    };
    list("order", g_settings.order);
    list("favorites", g_settings.favorites);
    for (const auto& bm : g_settings.bookmarks) {   // title, a tab, the address (tabs and line ends taken out of the title)
        std::string t = bm.first;
        for (char& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out += "bookmark=" + t + "\t" + bm.second + "\n";
    }
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
        else if (key == "plan_provider") g_settings.planProvider = v == 1 ? 1 : 0;
        else if (key == "map_source") g_settings.mapSource = v == 1 ? 1 : 0;
        else if (key == "map_airports") g_settings.mapAirports = v != 0;
        else if (key == "map_timezones") g_settings.mapTimeZones = v != 0;
        else if (key == "map_heading_up") g_settings.mapHeadingUp = v != 0;
        else if (key == "map_local_time") g_settings.mapLocalTime = v != 0;
        else if (key == "favorite_slots") g_settings.favoriteSlots = std::max(0, std::min(kMaxFavorites, v));
        else if (key == "brightness") g_settings.brightness = std::max(0.1f, std::min(1.0f, (float)std::atof(val.c_str())));
        else if (key == "show_charts") g_settings.showCharts = v != 0;
        else if (key == "show_simbrief") g_settings.showSimbrief = v != 0;
        else if (key == "show_live") g_settings.showLive = v != 0;
        else if (key == "show_foreflight") g_settings.showForeflight = v != 0;
        else if (key == "order") parseList(val, g_settings.order);
        else if (key == "favorites") parseList(val, g_settings.favorites);
        else if (key == "bookmark") {
            const size_t tab = val.find('\t');
            if (tab != std::string::npos && tab + 1 < val.size()) g_settings.bookmarks.push_back({val.substr(0, tab), val.substr(tab + 1)});
        }
    }
    normalizeLayout();
}

} // namespace xpt
