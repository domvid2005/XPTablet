#include "tabletui.h"
#include "airlines.h"

#include <algorithm>
#include <map>
#include <array>
#include <cfloat>
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
extern const unsigned char xpt_asset_logo[];
extern const unsigned int xpt_asset_logo_size;
extern const unsigned char xpt_asset_icons[];
extern const unsigned int xpt_asset_icons_size;
}

namespace xpt {
namespace {

const float kStatusH = 28.0f, kHeaderH = 46.0f;
const float kPi = 3.14159265f;
void normalizeLayout();   // the home screen's places
bool pill(ImDrawList* dl, const Fonts& f, const char* id, const char* label, ImVec2 a, ImVec2 b, bool on, bool enabled);
double distNmLL(double la1, double lo1, double la2, double lo2, double* brgOut);
const char* surfaceName(int s);
extern std::string g_aptSel;
extern float g_aptScroll;

struct AppDef { const char* id; const char* name; ImU32 color; const char* url; };   // id: its name in the settings file; url: a web app
const AppDef kApps[APP_COUNT] = {
    {"map", "Map", IM_COL32(52, 168, 110, 255), nullptr},                  {"airports", "Airports", IM_COL32(64, 132, 230, 255), nullptr},
    {"charts", "ChartFox", IM_COL32(236, 152, 50, 255), "https://chartfox.org"},            {"documents", "Documents", IM_COL32(120, 132, 156, 255), nullptr},
    {"routes", "Flight Plan", IM_COL32(150, 96, 250, 255), nullptr},            {"weather", "Weather", IM_COL32(40, 180, 214, 255), nullptr},
    {"ground", "Aircraft", IM_COL32(246, 190, 48, 255), nullptr},            {"checklists", "Checklists", IM_COL32(214, 64, 160, 255), nullptr},
    {"performance", "Performance", IM_COL32(236, 92, 104, 255), nullptr},  {"traffic", "Traffic", IM_COL32(40, 190, 170, 255), nullptr},
    {"notes", "Notes", IM_COL32(255, 138, 40, 255), nullptr},   // orange (Cabin keeps the yellow)
    {"simbrief", "SimBrief", IM_COL32(196, 58, 66, 255), "https://dispatch.simbrief.com"},
    {"livemap", "Volanta", IM_COL32(112, 72, 255, 255), "https://fly.volanta.app"},   // Volanta's violet (approximated)
    {"foreflight", "ForeFlight", IM_COL32(30, 140, 222, 255), "https://plan.foreflight.com"},
    {"browser", "Browser", IM_COL32(72, 110, 240, 255), nullptr},          {"settings", "Settings", IM_COL32(110, 120, 140, 255), nullptr},
    {"navigraph", "Navigraph", IM_COL32(28, 52, 92, 255), "https://charts.navigraph.com"},
    {"vatsim", "VATSIM Radar", IM_COL32(18, 28, 52, 255), "https://vatsim-radar.com"},
    {"skyvector", "SkyVector", IM_COL32(32, 132, 108, 255), "https://skyvector.com"},
    {"music", "Media", IM_COL32(250, 45, 85, 255), nullptr},   // (its id stays "music": saved home screens keep it)
    {"dispatch", "Dispatch", IM_COL32(58, 110, 200, 255), nullptr},   // the SimBrief briefing (its PDF)
};
enum { ICON_VOLANTA = APP_COUNT, ICON_VATSIM, ICON_SKYVECTOR };   // pictures of their own (the other web apps borrow one)

bool g_wx = false;             // WXPlane is installed (told by the host each frame): the Weather icon and page are WXPlane's
ImTextureID g_wxLogo = 0;
Settings g_settings;
bool g_settingsChanged = false;
std::string g_url = "https://chartfox.org", g_site = "Browser";   // what the web page shows
int favSlots() { return std::max(0, std::min(kMaxFavorites, g_settings.favoriteSlots)); }
bool g_mac = false;              // macOS (told by the host): no media controls
bool g_ownPerformance = false;   // the aircraft brings its own performance calculator (ToLiss): no Performance app
bool g_chrome = true;                                             // with the browser's own controls
int g_webGroup = 0;   // whose tabs are shown: 0 the Browser's (and documents'), else a web app's own (100 + its app)
bool g_webHold = false;   // the shown tab is not this group's yet (one moment): no picture from another group's page

void openWeb(const char* site, const char* url, bool chrome, int group = 0);
double localOffsetAt(const MapState& m, double lat, double lon, bool& estimate);
void fillConcave(ImDrawList* dl, std::vector<ImVec2> v, ImU32 col);
std::string zoneOf(double lat, double lon, double offS, long long utc);
ImTextureID g_logo = 0;   // XPTablet's logo texture (from the host)
ImTextureID g_icons = 0;  // the icons' atlas (tools/make_icons_rgba.py): 4 cells across; pictograms (white, tinted) then the services' tiles
// the atlas cell of an app's pictogram (Material Design Icons), or -1: drawn from lines instead
int glyphCell(int app) {
    switch (app) {
    case APP_SETTINGS: return 0;     // cog
    case APP_ROUTES: return 4;       // star-four-points
    case APP_CHECKLISTS: return 2;   // check-bold
    case APP_DOCUMENTS: return 3;    // file-document
    case APP_GROUND: return 5;       // airplane
    case APP_AIRPORTS: return 6;     // airport
    case APP_BROWSER: return 7;      // web
    case APP_WEATHER: return 15;     // weather-partly-snowy-rainy (the Weather page without WXPlane)
    case APP_MUSIC: return 17;       // music
    case APP_MAP: return 18;         // map (the folded map)
    case APP_NOTES: return 19;       // file-document-edit
    default: return -1;
    }
}
// the atlas cell of a service's own tile, made from its logo, or -1
int brandCell(int app) {
    switch (app) {
    case APP_FOREFLIGHT: return 8;
    case APP_NAVIGRAPH: return 9;
    case APP_SKYVECTOR: return 10;
    case APP_LIVEMAP: return 11;     // Volanta
    case APP_VATSIM: return 12;      // VATSIM Radar
    case APP_SIMBRIEF: return 13;
    case APP_CHARTS: return 16;      // ChartFox
    default: return -1;
    }
}
void cellUv(int cell, ImVec2& uv0, ImVec2& uv1) {
    const float in = 1.5f / 256.0f;   // a little inside the cell, so its neighbours never bleed in
    static int rows = 0;   // the atlas's rows of cells (4 across, 256 x 256 each)
    if (!rows) { int w = 0, h = 0, l = 0; iconsPixels(w, h, l); rows = std::max(1, h / 256); }
    uv0 = ImVec2((cell % 4 + in) / 4.0f, (cell / 4 + in) / (float)rows);
    uv1 = ImVec2((cell % 4 + 1 - in) / 4.0f, (cell / 4 + 1 - in) / (float)rows);
}

ImU32 appColor(int app) { return (app == APP_WEATHER && g_wx) ? IM_COL32(22, 24, 29, 255) : kApps[app].color; }
int appIcon(int app) {
    switch (app) {
    case APP_LIVEMAP: return ICON_VOLANTA;
    case APP_VATSIM: return ICON_VATSIM;
    case APP_SKYVECTOR: return ICON_SKYVECTOR;
    case APP_NAVIGRAPH: return APP_CHARTS;   // an approach plate too
    default: return app;
    }
}

// the home screen's places (below)

int g_app = APP_HOME;
int g_chimeSeq = 0;   // counted up for each notification's chime (and Test); the host plays it at the volume set in Settings, Notifications
// a web page full screen (a video's button): it takes the whole tablet, status bar and title bar too
bool videoFull(const SimState& s) { return g_app == APP_BROWSER && s.browser.status == 1 && s.browser.fullscreen && !g_webHold; }
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
// The looks to choose in Settings, Display, all from the XPTablet logo: its navy, and the colours of its letters (violet, magenta,
// coral, orange, gold). A background is a sky (top, bottom) and two mountain ranges; an accent is the colour of what is chosen.
struct Backdrop { const char* name; ImU32 top, bot, back, front; };
const Backdrop kBackdrops[5] = {
    {"Navy", IM_COL32(9, 22, 44, 255), IM_COL32(24, 58, 104, 255), IM_COL32(20, 46, 86, 255), IM_COL32(14, 34, 66, 255)},
    {"Violet", IM_COL32(16, 12, 42, 255), IM_COL32(60, 40, 118, 255), IM_COL32(46, 30, 96, 255), IM_COL32(32, 22, 72, 255)},
    {"Dusk", IM_COL32(22, 10, 34, 255), IM_COL32(100, 36, 96, 255), IM_COL32(76, 28, 78, 255), IM_COL32(52, 20, 58, 255)},
    {"Sunset", IM_COL32(20, 14, 40, 255), IM_COL32(150, 66, 72, 255), IM_COL32(104, 44, 64, 255), IM_COL32(64, 28, 52, 255)},
    {"Amber", IM_COL32(20, 16, 30, 255), IM_COL32(146, 96, 42, 255), IM_COL32(104, 66, 38, 255), IM_COL32(64, 40, 30, 255)},
};
struct Accent { const char* name; int r, g, b; };
// each background has its accent: Navy the blue XPTablet always had, the others the logo colour their sky is made of
const Accent kAccents[5] = {{"Blue", 70, 140, 255}, {"Violet", 150, 100, 235}, {"Magenta", 205, 80, 170}, {"Coral", 240, 96, 96}, {"Gold", 236, 176, 56}};
// the text colours follow the theme: moved by a share of how far the theme's accent is from Navy's blue (Navy: exactly as before)
ImU32 themeText(int r, int g, int b, float share) {
    const Accent& c = kAccents[std::max(0, std::min(4, g_settings.wallpaper))];
    const Accent& n = kAccents[0];
    auto ch = [&](int base, int t, int nv) { return std::max(0, std::min(255, base + (int)std::lround((t - nv) * share))); };
    return IM_COL32(ch(r, c.r, n.r), ch(g, c.g, n.g), ch(b, c.b, n.b), 255);
}
#define kInk themeText(236, 241, 250, 0.05f)     // the main text: white with the faintest tint
#define kMuted themeText(150, 168, 198, 0.32f)   // the quieter text: labels, hints, card titles
ImU32 accentCol(int a) { const Accent& c = kAccents[std::max(0, std::min(4, g_settings.wallpaper))]; return IM_COL32(c.r, c.g, c.b, std::max(0, std::min(255, a))); }
ImU32 accentLite(int a);
ImU32 mixCol(ImU32 a, ImU32 b, float t);
// the route on the map: the theme's accent, a little lighter so it shows on the street map and the imagery
ImU32 routeCol(int a) { const ImU32 c = mixCol(accentCol(255), IM_COL32(255, 255, 255, 255), 0.12f); return (c & 0x00FFFFFF) | ((ImU32)std::max(0, std::min(255, a)) << 24); }
ImU32 accentLite(int a) {   // the lighter one: rings, lines, focus
    const Accent& c = kAccents[std::max(0, std::min(4, g_settings.wallpaper))];
    return IM_COL32(c.r + (255 - c.r) * 2 / 5, c.g + (255 - c.g) * 2 / 5, c.b + (255 - c.b) * 2 / 5, std::max(0, std::min(255, a)));
}
// one background drawn into a box (the screen; or a little picture of it in Settings, the same drawing made small)
ImU32 mixCol(ImU32 a, ImU32 b, float t);
void wallpaperIn(ImDrawList* dl, ImVec2 o, float w, float h, int which, float round = 0) {
    const Backdrop& bd = kBackdrops[std::max(0, std::min(4, which))];
    const ImU32 top = bd.top, bot = bd.bot;
    if (round <= 0) dl->AddRectFilledMultiColor(o, ImVec2(o.x + w, o.y + h), top, top, bot, bot);
    else {   // rounded (the little pictures in Settings): a rounded box shaded top to bottom
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(255, 255, 255, 255), round);
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) {   // each point's colour by its height, top to bottom
            ImDrawVert& v = dl->VtxBuffer[i];
            const ImU32 c = mixCol(top, bot, std::max(0.0f, std::min(1.0f, (v.pos.y - o.y) / std::max(1.0f, h))));
            v.col = (c & 0x00FFFFFF) | (v.col & 0xFF000000);
        }
    }
    const int vMount = dl->VtxBuffer.Size;
    const float back[][2] = {{0.00f, 0.80f}, {0.12f, 0.62f}, {0.22f, 0.74f}, {0.36f, 0.50f}, {0.50f, 0.72f}, {0.62f, 0.56f}, {0.76f, 0.76f}, {0.88f, 0.60f}, {1.00f, 0.78f}};
    const float front[][2] = {{0.00f, 0.92f}, {0.10f, 0.80f}, {0.24f, 0.90f}, {0.42f, 0.70f}, {0.56f, 0.88f}, {0.70f, 0.76f}, {0.84f, 0.90f}, {1.00f, 0.80f}};
    auto range = [&](const float (*p)[2], int n, ImU32 col) {
        for (int i = 0; i + 1 < n; ++i) {
            const ImVec2 a(o.x + p[i][0] * w, o.y + p[i][1] * h), b(o.x + p[i + 1][0] * w, o.y + p[i + 1][1] * h);
            dl->AddQuadFilled(a, b, ImVec2(b.x, o.y + h), ImVec2(a.x, o.y + h), col);
        }
    };
    range(back, 9, bd.back);
    range(front, 8, bd.front);
    if (round > 0)   // the mountains kept inside the rounded corners: a point past a corner's curve goes onto it
        for (int i = vMount; i < dl->VtxBuffer.Size; ++i) {
            ImVec2& q = dl->VtxBuffer[i].pos;
            const float cx = q.x < o.x + round ? o.x + round : q.x > o.x + w - round ? o.x + w - round : q.x;
            const float cy = q.y < o.y + round ? o.y + round : q.y > o.y + h - round ? o.y + h - round : q.y;
            const float dx = q.x - cx, dy = q.y - cy, d = std::sqrt(dx * dx + dy * dy);
            if ((cx != q.x || cy != q.y) && d > round && (q.x < o.x + round || q.x > o.x + w - round) && (q.y < o.y + round || q.y > o.y + h - round))
                q = ImVec2(cx + dx * round / d, cy + dy * round / d);
        }
}
void wallpaper(ImDrawList* dl, float w, float h) { wallpaperIn(dl, ImVec2(0, 0), w, h, g_settings.wallpaper); }
// the theme's dark tone, for what lies over the page (the pull-down, glass cards, banners): navy turned towards the theme's sky
// a navy tone moved by a share of how far the theme's sky is from Navy's (so on Navy it is exactly the tone given)
ImU32 themeTone(int r, int g, int b, int a) {
    const Backdrop& bd = kBackdrops[std::max(0, std::min(4, g_settings.wallpaper))];
    const ImU32 n = kBackdrops[0].bot, t = bd.bot;
    auto ch = [&](int base, int sh) { const int d = (int)((t >> sh) & 255) - (int)((n >> sh) & 255); return std::max(0, std::min(255, base + (int)std::lround(d * 0.35f))); };
    return IM_COL32(ch(r, IM_COL32_R_SHIFT), ch(g, IM_COL32_G_SHIFT), ch(b, IM_COL32_B_SHIFT), std::max(0, std::min(255, a)));
}
ImU32 themeDark(int a) { return themeTone(12, 20, 40, a); }

// A small white pictogram for each app, drawn from lines and circles inside a circle of radius r around c.
void icon(ImDrawList* dl, int app, ImVec2 c, float r, ImU32 col) {
    c = ImVec2(snap(c.x), snap(c.y));
    const int cellNo = app == APP_COUNT + 100 ? 14 : (app >= 0 && app < APP_COUNT) ? glyphCell(app) : -1;   // APP_COUNT + 100: power
    if (g_icons && cellNo >= 0) {   // its pictogram (24 design units, ~20 of them drawn)
        ImVec2 uv0, uv1;
        cellUv(cellNo, uv0, uv1);
        // a little inside the cell: sampled right at its border, a small drawing blends in the cell beside it (the brand tiles
        // are solid squares, so a faint square showed round the power icon); the pictograms keep a wide margin, nothing is cut
        const float ix = (uv1.x - uv0.x) / 24.0f, iy = (uv1.y - uv0.y) / 24.0f;
        uv0.x += ix; uv0.y += iy; uv1.x -= ix; uv1.y -= iy;
        const float hs = r * 1.14f * 22.0f / 24.0f;
        dl->AddImage(g_icons, ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), uv0, uv1, col);
        return;
    }
    const float t = snap(std::max(2.0f, r * 0.15f));   // a whole number of screen pixels: no half-covered edges
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (app) {
    case APP_MAP:   // a location pin
        dl->AddCircle(P(0, -0.25f), r * 0.50f, col, 0, t);
        dl->AddCircleFilled(P(0, -0.25f), r * 0.17f, col, 0);
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
        dl->AddCircleFilled(P(-0.66f, 0.56f), r * 0.18f, col, 0);
        dl->AddCircleFilled(P(-0.10f, -0.20f), r * 0.18f, col, 0);
        dl->AddCircleFilled(P(0.66f, -0.56f), r * 0.18f, col, 0);
        break;
    case APP_WEATHER:   // a cloud
        dl->AddCircleFilled(P(-0.40f, 0.14f), r * 0.34f, col, 0);
        dl->AddCircleFilled(P(0.02f, -0.14f), r * 0.46f, col, 0);
        dl->AddCircleFilled(P(0.46f, 0.14f), r * 0.32f, col, 0);
        dl->AddRectFilled(P(-0.40f, 0.10f), P(0.46f, 0.47f), col);
        break;
    case APP_BROWSER:   // a globe
        dl->AddCircle(c, r * 0.80f, col, 0, t);
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
        dl->PathArcTo(P(0, 0.30f), r * 0.80f, kPi, 2 * kPi, 0);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(0, 0.30f), P(0.42f, -0.26f), col, t * 1.3f);
        dl->AddCircleFilled(P(0, 0.30f), r * 0.13f, col, 0);
        break;
    case APP_TRAFFIC:   // a radar scope
        dl->AddCircle(c, r * 0.82f, col, 0, t);
        dl->AddCircle(c, r * 0.42f, col, 0, t);
        dl->AddLine(c, P(0.58f, -0.58f), col, t);
        dl->AddCircleFilled(P(-0.34f, -0.48f), r * 0.11f, col, 0);
        dl->AddCircleFilled(P(0.30f, 0.52f), r * 0.11f, col, 0);
        break;
    case APP_DISPATCH:   // a page with lines (the briefing)
        dl->AddRect(P(-0.62f, -0.80f), P(0.62f, 0.80f), col, r * 0.10f, 0, t);
        for (int i = 0; i < 3; ++i) dl->AddLine(P(-0.36f, -0.40f + i * 0.40f), P(0.36f, -0.40f + i * 0.40f), col, t);
        break;
    case APP_SETTINGS:   // a cog
        dl->AddCircle(c, r * 0.44f, col, 0, t * 1.3f);
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
        dl->AddCircleFilled(P(-0.74f, 0.52f), r * 0.15f, col, 0);
        dl->AddCircle(P(0.74f, 0.52f), r * 0.15f, col, 0, t * 0.8f);
        {   // the aircraft, at the top of the arc, heading right: a swept dart, cut out of the arc
            const ImVec2 a = P(0.02f, -0.27f);
            const float k = r * 0.40f;
            dl->AddCircleFilled(a, k * 0.78f, IM_COL32(112, 72, 255, 255), 0);   // the tile's own colour behind it
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
        dl->AddCircleFilled(P(-0.56f, 0.40f), r * 0.11f, col, 0);
        dl->AddCircleFilled(P(0.0f, -0.10f), r * 0.11f, col, 0);
        dl->AddCircleFilled(P(0.54f, -0.30f), r * 0.11f, col, 0);
        break;
    case ICON_VATSIM:   // a station and its radio waves
        dl->AddCircleFilled(P(0, 0.34f), r * 0.20f, col, 0);
        for (int i = 1; i <= 2; ++i) {
            dl->PathArcTo(P(0, 0.34f), r * (0.20f + 0.34f * i), kPi * 1.20f, kPi * 1.80f, 0);
            dl->PathStroke(col, 0, t);
        }
        break;
    case APP_FOREFLIGHT:   // a compass
        dl->AddCircle(c, r * 0.84f, col, 0, t);
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
    // green, yellow at 20 % and under, red under 10 % (green while charging)
    const ImU32 fill = charging || pct > 20 ? IM_COL32(80, 214, 124, 255) : pct > 10 ? IM_COL32(250, 204, 60, 255) : IM_COL32(240, 84, 84, 255);
    if (k > 0.02f) dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 2 + (w - 4) * k, p.y + h - 2), fill, 1.5f);
    if (charging) {   // a small bolt beside it
        const ImVec2 b(p.x - 9, p.y + h * 0.5f);
        dl->AddTriangleFilled(ImVec2(b.x + 2, b.y - 6), ImVec2(b.x - 3, b.y + 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(80, 214, 124, 255));
        dl->AddTriangleFilled(ImVec2(b.x - 2, b.y + 6), ImVec2(b.x + 3, b.y - 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(80, 214, 124, 255));
    }
}

// ---- power: off (fading to black, then black until tapped), then starting (the logo and a loading bar), then home fading in as the
// logo rises into the status bar
enum { POWER_ON, POWER_FADING, POWER_OFF, POWER_BOOT, POWER_REVEAL,
       POWER_DEAD,          // the battery ran out: black at once, until tapped
       POWER_DEAD_PROMPT,   // tapped: an empty battery and a cable, as an iPhone shows; gone within 5 s unless the battery is tapped
       POWER_DEAD_CHARGE }; // the battery tapped: the charger is in, charging is shown 5 s, fades, and 5 s later the tablet starts
int g_power = POWER_ON;
double g_powerT0 = 0;
// the tablet's own battery (kept with the settings): 2 hours from full with the screen on (it rests while the screen is off),
// 30 minutes from empty to full on the charger, which is plugged in and out by tapping the battery in the status bar
float g_batt = 100.0f;
bool g_charger = false;
int g_battSaved = -1;
int g_lowAlert = 0;          // the low battery alert shown (20 or 10; 0: none)
double g_lowAlertT0 = 0;
bool g_lowWarned20 = false, g_lowWarned10 = false;   // given once on the way down (again after a charge above them)
constexpr double kDeadPromptS = 5.0, kDeadChargeShowS = 5.0, kDeadChargeFadeS = 0.6, kDeadChargeWaitS = 5.0;
float g_ccT = 0, g_ccTarget = 0;   // the pull-down: how far it is down (0..1), where it is going
bool g_ccDrag = false;             // a finger is pulling it (from the logo, or pushing it up by its bar)
float g_ccDragY0 = 0, g_ccDragT0 = 0;
float g_screenH = 480;
constexpr double kOffS = 0.7, kBootS = 2.2, kHoldS = 0.25, kRevealS = 0.8;
void powerOff() { g_power = POWER_FADING; g_powerT0 = ImGui::GetTime(); g_ccTarget = 0; g_ccT = 0; g_ccDrag = false; }
float smooth01(float t) { t = std::max(0.0f, std::min(1.0f, t)); return t * t * (3 - 2 * t); }
// the loading bar's (made up) progress at t seconds: a start, a few pauses, a quick finish
float bootProgress(float t) {
    static const float k[][2] = {{0, 0}, {0.35f, 0}, {0.70f, 0.18f}, {0.90f, 0.22f}, {1.30f, 0.55f}, {1.45f, 0.58f}, {1.90f, 0.92f}, {2.20f, 1.0f}};
    const int n = (int)(sizeof k / sizeof k[0]);
    if (t >= k[n - 1][0]) return 1.0f;
    for (int i = 1; i < n; ++i)
        if (t < k[i][0]) return k[i - 1][1] + (k[i][1] - k[i - 1][1]) * smooth01((t - k[i - 1][0]) / (k[i][0] - k[i - 1][0]));
    return 1.0f;
}
// the status bar's logo: where it sits (a: top left, size)
void statusLogoRect(float w, ImVec2& a, ImVec2& size) {
    int lw = 0, lh = 0, lv = 0;
    logoPixels(lw, lh, lv);
    const float hh = 15.0f, ww = hh * (float)lw / (float)std::max(1, lh);
    a = ImVec2(std::floor(w * 0.5f - ww * 0.5f), std::floor((kStatusH - hh) * 0.5f));
    size = ImVec2(ww, hh);
}

void statusBar(ImDrawList* dl, const Fonts& f, const SimState& s, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), IM_COL32(0, 0, 0, 90));
    {   // a touch of the theme: its sky's darkest tone and a faint glow of its accent (the logo and layout are unchanged)
        const Backdrop& bd = kBackdrops[std::max(0, std::min(4, g_settings.wallpaper))];
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), (bd.top & 0x00FFFFFF) | (70u << 24));
        // a clear edge under it: a dark line, then a hairline of the accent's light tone (no gradient)
        dl->AddRectFilled(ImVec2(0, kStatusH - 2), ImVec2(w, kStatusH - 1), IM_COL32(0, 0, 0, 110));
        dl->AddRectFilled(ImVec2(0, kStatusH - 1), ImVec2(w, kStatusH), accentLite(70));
    }
    const float y = std::floor((kStatusH - fontPx(f.small)) * 0.5f);
    const ImU32 col = IM_COL32(232, 238, 248, 255);
    // left: the clock, Zulu or (tapped) local time where the aircraft is, with its zone
    char buf[48] = "--:--Z";
    {
        const MapState& m = s.map;
        if (g_settings.statusLocalTime && s.zuluSec >= 0 && m.havePos) {
            bool est = false;
            const long long utc = m.utc ? m.utc : s.zuluSec;
            const double off = localOffsetAt(m, m.lat, m.lon, est);
            const long long t = ((((long long)s.zuluSec + (long long)off) % 86400) + 86400) % 86400;
            std::snprintf(buf, sizeof buf, "%02d:%02d %s%s", (int)(t / 3600), (int)(t / 60 % 60), est ? "~" : "", zoneOf(m.lat, m.lon, off, utc).c_str());
        } else if (s.zuluSec >= 0) {
            std::snprintf(buf, sizeof buf, "%02d:%02dZ", (s.zuluSec / 3600) % 24, (s.zuluSec / 60) % 60);
        }
        // the power button, in the corner
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        // (the power button and the clock each have their own place: their taps and highlights never overlap)
        if (ImGui::InvisibleButton("##power", ImVec2(29, kStatusH)) && g_power == POWER_ON) powerOff();
        const ImVec2 pc(15, kStatusH * 0.5f);
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(3, 3), ImVec2(27, kStatusH - 3), IM_COL32(255, 255, 255, 26), 6.0f);   // as the clock's
        icon(dl, APP_COUNT + 100, pc, 7.6f, ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : col);   // (no ring around it)   // (the power pictogram)
        const float cx = 37, tw = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, buf).x;
        ImGui::SetCursorScreenPos(ImVec2(30, 0));
        if (ImGui::InvisibleButton("##clock", ImVec2(cx - 30 + tw + 6, kStatusH))) { g_settings.statusLocalTime = !g_settings.statusLocalTime; g_settingsChanged = true; }
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(31, 3), ImVec2(cx + tw + 6, kStatusH - 3), IM_COL32(255, 255, 255, 26), 6.0f);
        dl->AddText(f.small, fontPx(f.small), ImVec2(cx, y), g_settings.statusLocalTime ? accentLite(255) : col, buf);   // local time: the theme's colour
    }
    // middle: the logo
    {   // the logo: tap it, or pull down from it, for the pull-down (music and notifications); tap again to close it
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetCursorScreenPos(ImVec2(w * 0.5f - 90, 0));
        ImGui::InvisibleButton("##logo", ImVec2(180, kStatusH));
        if (ImGui::IsItemActivated()) { g_ccDrag = true; g_ccDragY0 = io.MousePos.y; g_ccDragT0 = g_ccT; }
        if (ImGui::IsItemActive()) {
            const float dy = io.MousePos.y - g_ccDragY0;
            if (std::fabs(dy) > 6) g_ccT = std::max(0.0f, std::min(1.0f, g_ccDragT0 + dy / ((g_screenH - kStatusH) * 0.7f)));
        }
        if (ImGui::IsItemDeactivated()) {
            g_ccDrag = false;
            const bool tapped = std::fabs(io.MousePos.y - g_ccDragY0) < 6;
            if (tapped) g_ccTarget = g_ccTarget > 0.5f ? 0.0f : 1.0f;
            else g_ccTarget = g_ccT > (g_ccDragT0 > 0.5f ? 0.75f : 0.25f) ? 1.0f : 0.0f;
        }
        // hovered: a soft highlight, whether it will open the pull-down or close it (brighter while it is open)
        static float lh = 0;   // (the hover fades in and out)
        lh += ((ImGui::IsItemHovered() && !g_ccDrag ? 1.0f : 0.0f) - lh) * (1.0f - std::exp(-io.DeltaTime * 14.0f));
        if (lh > 0.01f) dl->AddRectFilled(ImVec2(w * 0.5f - 70, 3), ImVec2(w * 0.5f + 70, kStatusH - 3), IM_COL32(255, 255, 255, (int)(lh * (g_ccT > 0.5f ? 30 : 18))), 8.0f);
    }
    if (g_logo) {
        ImVec2 a, sz;
        statusLogoRect(w, a, sz);
        if (g_power != POWER_REVEAL) dl->AddImage(g_logo, a, ImVec2(a.x + sz.x, a.y + sz.y));   // (while waking, it is on its way up)
    } else {
        textCentered(dl, f.bold, w * 0.5f, y, col, "XPTablet");
    }
    // right: aircraft type, charge, battery
    float x = w - 12 - 25;
    const int pct = (int)std::ceil(g_batt - 0.001f);
    std::snprintf(buf, sizeof buf, "%d%%", std::max(0, std::min(100, pct)));
    ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, buf);
    {   // tap the battery (or its figure): the charger goes in, or comes out; highlighted on hover as the clock is
        const float bx0 = x - sz.x - (g_charger ? 20 : 8) - 6, bx1 = x + 25 + 6;
        ImGui::SetCursorScreenPos(ImVec2(bx0, 0));
        if (ImGui::InvisibleButton("##battery", ImVec2(bx1 - bx0, kStatusH))) { g_charger = !g_charger; g_settingsChanged = true; }
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(bx0, 3), ImVec2(bx1, kStatusH - 3), IM_COL32(255, 255, 255, 26), 6.0f);
    }
    battery(dl, ImVec2(x, std::floor((kStatusH - 11) * 0.5f)), pct, g_charger);
    x -= sz.x + (g_charger ? 20 : 8);
    dl->AddText(f.small, fontPx(f.small), ImVec2(x, y), col, buf);
    if (!s.acfIcao.empty()) {
        sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, s.acfIcao.c_str());
        x -= sz.x + 16;
        dl->AddText(f.small, fontPx(f.small), ImVec2(x, y), col, s.acfIcao.c_str());
    }
}

// The home screen, as on an iPad: tap an icon to open it; hold one (or an empty place) for a moment and the icons wiggle: they can
// then be dragged to any place of the grid (the free places show as faint squares) or into the favorites bar; Done (or a tap on
// an empty place) ends it. The grid is a set of places: an icon stays where it is put, gaps and all.
int g_pressApp = -1;          // the icon under the finger since the press, -1: none
ImVec2 g_pressPos;
double g_pressTime = -1;
bool g_dragging = false;
bool g_editMode = false;      // the icons wiggle and can be moved
bool g_editEnteredNow = false;   // this press began the wiggling (its release neither opens nor ends anything)
ImVec2 g_iconC[APP_COUNT];    // where each icon was last drawn (for the page opening from it)
float g_iconSize[APP_COUNT] = {};

void drawTile(ImDrawList* dl, const Fonts& f, int app, ImVec2 c, float tile, bool label, float scale, float shadeK) {
    const ImVec2 a(c.x - tile * 0.5f * scale, c.y - tile * 0.5f * scale), b(c.x + tile * 0.5f * scale, c.y + tile * 0.5f * scale);
    if (g_icons && brandCell(app) >= 0) {   // the service's own tile, from its logo
        ImVec2 uv0, uv1;
        cellUv(brandCell(app), uv0, uv1);
        const int k = (int)std::min(255.0f, 255.0f * shadeK);
        dl->AddImageRounded(g_icons, a, b, uv0, uv1, IM_COL32(k, k, k, 255), tile * 0.24f * scale);
        if (label) textCentered(dl, f.small, c.x, c.y + tile * 0.5f + 6, IM_COL32(236, 241, 250, 255), appName(app));
        return;
    }
    dl->AddRectFilled(a, b, shade(appColor(app), shadeK), tile * 0.24f * scale);
    if (app == APP_WEATHER && g_wx && g_wxLogo) {   // WXPlane's own logo, WX above PLANE, on its dark tile
        const float r = tile * 0.46f * scale;
        dl->AddImage(g_wxLogo, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
    } else {
        icon(dl, appIcon(app), c, tile * 0.32f * scale, IM_COL32(255, 255, 255, 255));
    }
    if (label) textCentered(dl, f.small, c.x, c.y + tile * 0.5f + 6, IM_COL32(236, 241, 250, 255), appName(app));
}
// the vertices drawn since vtx0, turned by angle about c (the wiggle)
void turnSince(ImDrawList* dl, int vtx0, ImVec2 c, float angle) {
    const float ca = std::cos(angle), sa = std::sin(angle);
    for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        const float x = p.x - c.x, y = p.y - c.y;
        p = ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca);
    }
}
float wiggle(int app) { return g_editMode ? 0.040f * (float)std::sin(ImGui::GetTime() * 2 * 3.14159 * 2.6 + app * 1.7) : 0.0f; }

struct Ease { float on, hot, down; };
Ease eased(const char* id, bool on, bool hot, bool down);
// A home icon with its hover: it eases brighter (a soft white glow, the logo tiles too) and lifts a little; pressed, it sinks
void hoverTile(ImDrawList* dl, const Fonts& f, int app, ImVec2 c, float tile, bool over, bool held) {
    char id[24];
    std::snprintf(id, sizeof id, "##hov%d", app);
    const Ease ez = eased(id, false, over && !held, held);
    const float scale = 1.0f + 0.05f * ez.hot - 0.06f * ez.down;
    drawTile(dl, f, app, c, tile, true, scale, 1.0f + 0.12f * ez.hot - 0.14f * ez.down);
    if (ez.hot > 0.01f) {
        const float hs = tile * 0.5f * scale;
        dl->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(255, 255, 255, (int)(38 * ez.hot)), tile * 0.24f * scale);
    }
}
// The grid as places: order[i] is the app at place i, or -1 for a free place. Favorites: the bar, in order, no gaps.
void normalizeLayout(int capacity) {
    bool seen[APP_COUNT] = {};
    std::vector<int> fav;
    for (int a : g_settings.favorites) if (a >= 0 && a < APP_COUNT && !seen[a]) { seen[a] = true; fav.push_back(a); }
    std::vector<int> spill;   // favorites beyond the bar's size go to the grid
    while ((int)fav.size() > favSlots()) { spill.insert(spill.begin(), fav.back()); seen[fav.back()] = false; fav.pop_back(); }
    g_settings.favorites.swap(fav);
    for (int& a : g_settings.order) { if (a < 0 || a >= APP_COUNT || seen[a]) a = -1; else seen[a] = true; }
    while ((int)g_settings.order.size() < capacity) g_settings.order.push_back(-1);
    auto place = [&](int app) {   // after the last icon (gaps the user left stay), else into the first free place, else at the end
        int last = -1;
        for (int i = 0; i < (int)g_settings.order.size(); ++i) if (g_settings.order[i] >= 0) last = i;
        for (int i = last + 1; i < capacity; ++i) if (g_settings.order[i] < 0) { g_settings.order[i] = app; return; }
        for (int i = 0; i < capacity; ++i) if (g_settings.order[i] < 0) { g_settings.order[i] = app; return; }
        g_settings.order.push_back(app);
    };
    for (int a : spill) { place(a); seen[a] = true; }
    for (int a = 0; a < APP_COUNT; ++a) if (!seen[a]) place(a);
    // icons past the grid's end (a narrower screen than before) come back into free places
    for (int i = capacity; i < (int)g_settings.order.size(); ++i) {
        const int a = g_settings.order[i];
        if (a < 0) continue;
        for (int k = 0; k < capacity; ++k) if (g_settings.order[k] < 0) { g_settings.order[k] = a; g_settings.order[i] = -1; break; }
    }
    while ((int)g_settings.order.size() > capacity && g_settings.order.back() < 0) g_settings.order.pop_back();
}
void normalizeLayout() { normalizeLayout(21); }
int g_gridCapacity = 21;

void home(ImDrawList* dl, const Fonts& f, float w, float h) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const double now = ImGui::GetTime();
    const float cellW = 104, cellH = 100, tile = 60, dockTile = tile, dockCell = 86, dockH = 98, dockPad = 12;   // the bar's icons as big as the grid's
    const bool hasDock = favSlots() > 0;
    const float dockTop = hasDock ? h - dockH - 10 : h + 100;   // no favorites bar: the grid has the whole screen
    const int cols = std::max(1, (int)((w - 24) / cellW));
    const float areaBot = hasDock ? dockTop - 6 : h - 8;
    const int rows = std::max(1, (int)((areaBot - kStatusH - 10) / cellH));
    g_gridCapacity = std::max(cols * rows, 1);
    normalizeLayout(g_gridCapacity);
    const float gx0 = std::floor((w - cols * cellW) * 0.5f);
    const float gy0 = kStatusH + std::max(6.0f, std::floor((areaBot - kStatusH - rows * cellH) * 0.5f));
    const int drag = g_dragging ? g_pressApp : -1;
    std::vector<int> dock;
    for (int a : g_settings.favorites) if (appVisible(a) && a != drag) dock.push_back(a);
    // where the dragged icon would land: a place in the grid, or a place in the bar
    bool toDock = false;
    int target = -1, gap = -1;
    if (drag >= 0) {
        toDock = hasDock && m.y >= dockTop - 8;
        if (toDock) {
            const bool fromDock = std::find(g_settings.favorites.begin(), g_settings.favorites.end(), drag) != g_settings.favorites.end();
            if ((int)dock.size() < favSlots() || fromDock) gap = std::max(0, std::min((int)dock.size(), (int)std::floor((m.x - ((w - favSlots() * dockCell) * 0.5f)) / dockCell)));
            else toDock = false;   // the bar is full
        }
        if (!toDock) {
            const int col = std::max(0, std::min(cols - 1, (int)std::floor((m.x - gx0) / cellW)));
            const int row = std::max(0, std::min(rows - 1, (int)std::floor((m.y - gy0) / cellH)));
            target = row * cols + col;
        }
    }

    // the grid: icons, and (while arranging) the free places, faintly
    struct Hit { int app; ImVec2 a, b; };
    std::vector<Hit> hits;
    const bool arranging = g_editMode || drag >= 0;
    for (int slot = 0; slot < g_gridCapacity; ++slot) {
        const float cx = gx0 + (slot % cols) * cellW, cy = gy0 + (slot / cols) * cellH;
        const ImVec2 c(cx + cellW * 0.5f, cy + 6 + tile * 0.5f);
        const int app = slot < (int)g_settings.order.size() ? g_settings.order[slot] : -1;
        const bool shown = app >= 0 && appVisible(app) && app != drag;
        if (slot == target && drag >= 0)
            dl->AddRectFilled(ImVec2(c.x - tile * 0.5f - 4, c.y - tile * 0.5f - 4), ImVec2(c.x + tile * 0.5f + 4, c.y + tile * 0.5f + 4), IM_COL32(255, 255, 255, 60), tile * 0.28f);
        if (!shown) {
            if (arranging) dl->AddRectFilled(ImVec2(c.x - tile * 0.5f, c.y - tile * 0.5f), ImVec2(c.x + tile * 0.5f, c.y + tile * 0.5f), IM_COL32(255, 255, 255, 22), tile * 0.24f);
            continue;
        }
        const bool over = drag < 0 && m.x >= cx && m.x < cx + cellW && m.y >= cy && m.y < cy + cellH;
        const bool held = g_pressApp == app && io.MouseDown[0];
        const int v0 = dl->VtxBuffer.Size;
        hoverTile(dl, f, app, c, tile, over, held);
        if (g_editMode) turnSince(dl, v0, c, wiggle(app));
        g_iconC[app] = c; g_iconSize[app] = tile;
        hits.push_back({app, ImVec2(cx, cy), ImVec2(cx + cellW, cy + cellH)});
    }
    // the favorites bar: as many places as Settings gives it, the free ones shown as outlines
    const int dockSlots = favSlots();
    const float dw = dockSlots * dockCell + dockPad * 2;
    const float dx0 = std::floor((w - dw) * 0.5f);
    if (hasDock) dl->AddRectFilled(ImVec2(dx0, dockTop), ImVec2(dx0 + dw, dockTop + dockH), IM_COL32(255, 255, 255, drag >= 0 && toDock ? 52 : 30), 22.0f);
    for (int k = (int)dock.size() + ((drag >= 0 && toDock) ? 1 : 0); k < dockSlots; ++k) {
        const ImVec2 c(dx0 + dockPad + k * dockCell + dockCell * 0.5f, dockTop + 8 + dockTile * 0.5f);
        dl->AddRect(ImVec2(c.x - dockTile * 0.5f, c.y - dockTile * 0.5f), ImVec2(c.x + dockTile * 0.5f, c.y + dockTile * 0.5f), IM_COL32(255, 255, 255, 46), dockTile * 0.24f, 0, snap(1.5f));
    }
    for (int k = 0; k < (int)dock.size(); ++k) {
        const int slot = (drag >= 0 && toDock && k >= gap) ? k + 1 : k;
        const float cx = dx0 + dockPad + slot * dockCell;
        const ImVec2 c(cx + dockCell * 0.5f, dockTop + 8 + dockTile * 0.5f);   // the icon, its name under it
        const bool over = drag < 0 && m.x >= cx && m.x < cx + dockCell && m.y >= dockTop && m.y < dockTop + dockH;
        const bool held = g_pressApp == dock[k] && io.MouseDown[0];
        const int v0 = dl->VtxBuffer.Size;
        hoverTile(dl, f, dock[k], c, dockTile, over, held);
        if (g_editMode) turnSince(dl, v0, c, wiggle(dock[k]));
        g_iconC[dock[k]] = c; g_iconSize[dock[k]] = dockTile;
        hits.push_back({dock[k], ImVec2(cx, dockTop), ImVec2(cx + dockCell, dockTop + dockH)});
    }

    // the finger (drawn first so Done, after it, takes its own presses)
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##home", ImVec2(w, h - kStatusH));
    if (ImGui::IsItemActivated()) {
        g_pressApp = -1; g_dragging = false; g_pressPos = m; g_pressTime = now; g_editEnteredNow = false;
        for (const Hit& ht : hits) if (m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) g_pressApp = ht.app;
    }
    const float moved = std::fabs(m.x - g_pressPos.x) + std::fabs(m.y - g_pressPos.y);
    if (ImGui::IsItemActive() && !g_editMode && g_pressTime >= 0 && now - g_pressTime >= 0.75 && moved < 10) { g_editMode = true; g_editEnteredNow = true; }   // held: wiggle
    if (ImGui::IsItemActive() && g_editMode && g_pressApp >= 0 && !g_dragging && moved > 6) g_dragging = true;
    if (g_dragging && g_pressApp >= 0) {   // the lifted icon, under the finger
        const int v0 = dl->VtxBuffer.Size;
        drawTile(dl, f, g_pressApp, m, tile, false, 1.12f, 1.08f);
        turnSince(dl, v0, m, 0.06f);
    }
    if (ImGui::IsItemDeactivated()) {
        const int app = g_pressApp;
        if (app >= 0 && g_dragging) {   // dropped
            auto& order = g_settings.order;
            auto& fav = g_settings.favorites;
            int from = -1;   // its place in the grid, if it was there
            for (int i = 0; i < (int)order.size(); ++i) if (order[i] == app) from = i;
            const bool fromDock = std::find(fav.begin(), fav.end(), app) != fav.end();
            if (toDock && gap >= 0) {
                if (fromDock) fav.erase(std::find(fav.begin(), fav.end(), app));
                if (from >= 0) order[from] = -1;
                // the gap counts shown icons; the stored list may also hold hidden ones: insert before the icon now at the gap
                size_t at = fav.size();
                if (gap < (int)dock.size()) for (size_t i = 0; i < fav.size(); ++i) if (fav[i] == dock[gap]) { at = i; break; }
                fav.insert(fav.begin() + at, app);
                g_settingsChanged = true;
            } else if (target >= 0) {
                while ((int)order.size() <= target) order.push_back(-1);
                const int there = order[target];
                if (there == app) {}
                else if (there < 0 || !appVisible(there)) {   // a free place (or one a hidden app keeps): it goes there
                    if (from >= 0) order[from] = there; else if (there >= 0) { /* a hidden app moves to the end */ order[target] = -1; order.push_back(there); }
                    if (fromDock) fav.erase(std::find(fav.begin(), fav.end(), app));
                    order[target] = app;
                    g_settingsChanged = true;
                } else {   // onto an icon: the two change places (from the bar, the other icon goes into the bar in its place)
                    if (fromDock) *std::find(fav.begin(), fav.end(), app) = there;
                    else if (from >= 0) order[from] = there;
                    order[target] = app;
                    if (fromDock && from >= 0) order[from] = -1;   // (it was in both: its old grid place is free now)
                    g_settingsChanged = true;
                }
            }
        } else if (!g_dragging && !g_editEnteredNow && moved < 10) {
            if (app >= 0 && !g_editMode) {
                for (const Hit& ht : hits) if (ht.app == app && m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) openApp(app);
            } else if (app < 0 && g_editMode) {
                g_editMode = false;   // a tap on an empty place: done
            }
        }
        g_pressApp = -1; g_dragging = false; g_pressTime = -1; g_editEnteredNow = false;
    }
    if (g_editMode && pill(dl, f, "##homedone", "Done", ImVec2(w - 96, h - 44), ImVec2(w - 16, h - 12), true, true)) g_editMode = false;
}

// ---- pages opening and closing, as on an iPad: a page grows out of its icon, and shrinks back into it ----------------------------------
double g_animT0 = -10;
bool g_animOpening = true;
int g_animPage = -1;          // the page closing
ImVec2 g_animIcon(0, 0);      // the icon it grows from and shrinks into
float g_animIconSize = 0;
constexpr double kOpenS = 0.34, kCloseS = 0.40;
void startPageAnim(bool opening, int iconApp) {
    g_animT0 = ImGui::GetTime();
    g_animOpening = opening;
    if (iconApp >= 0 && iconApp < APP_COUNT && g_iconSize[iconApp] > 0) { g_animIcon = g_iconC[iconApp]; g_animIconSize = g_iconSize[iconApp]; }
    else if (opening) { g_animIconSize = 0; }   // not from an icon (a command): from the middle
}
// The page drawn since vtx0 / cmd0, moved and scaled from the icon's square (e = 0) to the whole screen below the status bar (e = 1),
// its colours faded by alpha.
void placePage(ImDrawList* dl, int vtx0, int cmd0, float w, float h, float e, float alpha) {
    const ImVec2 F0(0, kStatusH), F1(w, h);
    const ImVec2 c = g_animIconSize > 0 ? g_animIcon : ImVec2(w * 0.5f, (kStatusH + h) * 0.5f);
    const float s0 = g_animIconSize > 0 ? g_animIconSize : 80.0f;
    const ImVec2 I0(c.x - s0 * 0.5f, c.y - s0 * 0.5f), I1(c.x + s0 * 0.5f, c.y + s0 * 0.5f);
    const ImVec2 R0(I0.x + (F0.x - I0.x) * e, I0.y + (F0.y - I0.y) * e), R1(I1.x + (F1.x - I1.x) * e, I1.y + (F1.y - I1.y) * e);
    const float sx = (R1.x - R0.x) / (F1.x - F0.x), sy = (R1.y - R0.y) / (F1.y - F0.y);
    auto map = [&](ImVec2 p) { return ImVec2(R0.x + (p.x - F0.x) * sx, R0.y + (p.y - F0.y) * sy); };
    const unsigned am = (unsigned)(std::max(0.0f, std::min(1.0f, alpha)) * 256.0f);
    for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos = map(v.pos);
        const unsigned a = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) * am >> 8;
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
    for (int i = cmd0; i < dl->CmdBuffer.Size; ++i) {
        ImVec4& r = dl->CmdBuffer[i].ClipRect;
        const ImVec2 a = map(ImVec2(r.x, r.y)), b = map(ImVec2(r.z, r.w));
        r = ImVec4(std::max(std::max(a.x, R0.x), 0.0f), std::max(std::max(a.y, R0.y), 0.0f), std::min(std::min(b.x, R1.x), w), std::min(std::min(b.y, R1.y), h));   // and never off the screen
    }
}
float easeOut(float t) { t = std::max(0.0f, std::min(1.0f, t)); const float u = 1 - t; return 1 - u * u * u; }


ImU32 mixCol(ImU32 a, ImU32 b, float t);
bool pageHeader(ImDrawList* dl, const Fonts& f, float w, const char* title) {
    // the bar under the status bar: dark, tinted with the theme's accent (the status bar itself never changes)
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, kStatusH + kHeaderH), mixCol(themeTone(10, 24, 48, 235), accentCol(235), 0.16f));
    dl->AddLine(ImVec2(0, kStatusH + kHeaderH), ImVec2(w, kStatusH + kHeaderH), accentCol(60));
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    const bool back = ImGui::InvisibleButton("##pagehome", ImVec2(96, kHeaderH));
    const ImU32 col = ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : accentLite(255);
    const float my = kStatusH + kHeaderH * 0.5f;
    { const ImVec2 pts[3] = {ImVec2(22, my - 8), ImVec2(14, my), ImVec2(22, my + 8)}; dl->AddPolyline(pts, 3, col, 0, 2.0f); }
    dl->AddText(f.body, fontPx(f.body), ImVec2(30, std::floor(my - fontPx(f.body) * 0.5f)), col, "Home");
    textCentered(dl, f.title, w * 0.5f, std::floor(my - fontPx(f.title) * 0.5f), IM_COL32(245, 248, 255, 255), title);
    return back;
}

void placeholderPage(ImDrawList* dl, const Fonts& f, int app, float w, float h) {
    const ImVec2 c(w * 0.5f, kStatusH + kHeaderH + (h - kStatusH - kHeaderH) * 0.42f);
    drawTile(dl, f, app, c, 96, false, 1.0f, 1.0f);
    textCentered(dl, f.body, c.x, c.y + 64, themeText(210, 220, 236, 0.15f), "This app arrives in a later test build.");
}

// The browser: a toolbar (back, forward, reload, the address, bookmark), the bookmarks, then the page itself.
// ---- small pieces the pages share ---------------------------------------------------------------------------------------------------
const ImU32 kCard = IM_COL32(10, 22, 44, 210), kCardLine = IM_COL32(255, 255, 255, 30), kGood = IM_COL32(80, 214, 124, 255), kWarn = IM_COL32(255, 190, 90, 255);

void card(ImDrawList* dl, ImVec2 a, ImVec2 b, const Fonts& f, const char* title) {
    dl->AddRectFilled(a, b, themeTone(10, 22, 44, 210), 12.0f);   // kCard, in the theme's tone
    dl->AddRect(a, b, kCardLine, 12.0f);
    if (title) dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 14, a.y + 10), kMuted, title);
}
void textAt(ImDrawList* dl, ImFont* font, float x, float y, ImU32 col, const char* txt) { dl->AddText(font, fontPx(font), ImVec2(x, y), col, txt); }
void textRight(ImDrawList* dl, ImFont* font, float xRight, float y, ImU32 col, const char* txt) {
    const ImVec2 sz = font->CalcTextSizeA(fontPx(font), 1e9f, 0, txt);
    dl->AddText(font, fontPx(font), ImVec2(std::floor(xRight - sz.x), y), col, txt);
}
// A rounded button drawn by hand at a fixed place. on: shown lit. enabled: can be pressed.
// How lit, hovered and pressed a control looks, eased over a few frames (by its id) so a change of state is a short fade
Ease eased(const char* id, bool on, bool hot, bool down) {
    static ImGuiStorage st;
    const ImGuiID k = ImGui::GetID(id);
    const float dt = ImGui::GetIO().DeltaTime;
    auto go = [&](ImGuiID key, float target, float rate) {
        float* v = st.GetFloatRef(key, target);
        *v += (target - *v) * (1.0f - std::exp(-dt * rate));
        if (std::fabs(*v - target) < 0.002f) *v = target;
        return *v;
    };
    return Ease{go(k, on ? 1.0f : 0.0f, 14.0f), go(k + 1, hot ? 1.0f : 0.0f, 18.0f), go(k + 2, down ? 1.0f : 0.0f, 28.0f)};
}
ImU32 mixCol(ImU32 a, ImU32 b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    auto ch = [&](int s) { const float x = ((a >> s) & 0xFF), y = ((b >> s) & 0xFF); return (ImU32)std::lround(x + (y - x) * t) << s; };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
bool pill(ImDrawList* dl, const Fonts& f, const char* id, const char* label, ImVec2 a, ImVec2 b, bool on, bool enabled) {
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const Ease e = eased(id, on && enabled, hot && enabled, down && enabled);
    // off: a light veil (brighter when hovered); on: blue; pressed: a touch smaller and brighter
    // hovered, plainly: off, the veil brightens and takes a tint and an outline of the accent; on, the accent lightens
    ImU32 off = mixCol(IM_COL32(255, 255, 255, 30), mixCol(IM_COL32(255, 255, 255, 64), accentCol(90), 0.35f), e.hot);
    ImU32 lit = mixCol(accentCol(225), accentLite(245), e.hot * 0.45f);
    ImU32 bg = mixCol(off, lit, e.on);
    bg = mixCol(bg, e.on > 0.5f ? accentLite(255) : IM_COL32(255, 255, 255, 84), e.down);
    if (!enabled) bg = IM_COL32(255, 255, 255, 14);
    const float inset = e.down * 1.5f;
    const ImVec2 a2(a.x + inset, a.y + inset), b2(b.x - inset, b.y - inset);
    dl->AddRectFilled(a2, b2, bg, (b2.y - a2.y) * 0.5f);
    if (enabled && e.hot * (1 - e.on) > 0.02f) dl->AddRect(a2, b2, accentLite((int)(150 * e.hot * (1 - e.on))), (b2.y - a2.y) * 0.5f, 0, 1.2f);
    textCentered(dl, f.small, (a.x + b.x) * 0.5f, std::floor((a.y + b.y - fontPx(f.small)) * 0.5f), enabled ? kInk : IM_COL32(236, 241, 250, 90), label);
    return hit && enabled;
}

// A slider whose knob glides: it follows the finger smoothly, settles on the nearest step when let go (steps > 0), and eases to a
// value set from elsewhere (auto dim). Greyed and inert when not enabled. Returns true when the user changed the value.
bool smoothSlider(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, ImVec2 b, float& v, float vmin, float vmax, int steps, bool enabled, const char* text) {
    static ImGuiStorage shownAt;   // where each slider's knob is drawn (0..1), by id
    // the value's room on the right fits its text (a short % leaves the track long), at least that of "100%"
    const float textW = (text && *text) ? std::max(f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, "100%").x, f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, text).x) + 6 : 0;
    const float x0 = a.x + 12, x1 = b.x - textW - 12, cy = (a.y + b.y) * 0.5f;
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    const bool active = enabled && ImGui::IsItemActive(), hot = enabled && ImGui::IsItemHovered();
    ImGui::EndDisabled();
    const ImGuiID key = ImGui::GetID(id);
    const float target = (v - vmin) / std::max(1e-6f, vmax - vmin);
    float shown = shownAt.GetFloat(key, target);
    bool changed = false;
    if (active) {
        const float t = std::max(0.0f, std::min(1.0f, (ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0)));
        float nv = vmin + t * (vmax - vmin);
        if (steps > 0) nv = vmin + std::round(t * steps) / steps * (vmax - vmin);
        if (std::fabs(nv - v) > 1e-6f) { v = nv; changed = true; }
        shown += (t - shown) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 30.0f));   // right under the finger, without a jump
    } else {
        shown += (target - shown) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f));   // glides to the value
        if (std::fabs(shown - target) < 0.001f) shown = target;
    }
    shownAt.SetFloat(key, shown);
    const float kx = x0 + (x1 - x0) * shown;
    const ImU32 fill = !enabled ? IM_COL32(150, 160, 180, 120) : accentCol(255);
    dl->AddRectFilled(ImVec2(x0, cy - 3), ImVec2(x1, cy + 3), IM_COL32(255, 255, 255, enabled ? 34 : 18), 3.0f);
    dl->AddRectFilled(ImVec2(x0, cy - 3), ImVec2(kx, cy + 3), fill, 3.0f);
    for (int i = 0; steps > 0 && steps <= 10 && i <= steps; ++i) {
        const float tx = x0 + (x1 - x0) * i / steps;
        dl->AddCircleFilled(ImVec2(tx, cy), 2.0f, IM_COL32(255, 255, 255, enabled ? 70 : 30), 0);
    }
    dl->AddCircleFilled(ImVec2(kx, cy + 1.5f), 11.0f, IM_COL32(0, 0, 0, 60), 0);   // a soft shadow
    dl->AddCircleFilled(ImVec2(kx, cy), active ? 12.0f : 11.0f, enabled ? (hot || active ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 241, 250, 255)) : IM_COL32(150, 160, 180, 255), 0);
    textRight(dl, f.bold, b.x, std::floor(cy - fontPx(f.bold) * 0.5f), enabled ? kInk : kMuted, text);
    return changed;
}

// ---- the web browser ---------------------------------------------------------------------------------------------------------------
// The page itself is drawn by the browser program (the plugin passes its picture in s.browser); here are its controls, the
// pointer and keys going to it, and an on-screen keyboard (the cockpit tablet has no keyboard).
std::string g_navPending;            // an address to send to the browser
bool g_navForce = false;             // send it even when the page is already on that site
bool g_kbd = false;                  // the on-screen keyboard is up
int g_kbdTarget = 0;                 // what it types into: 0 the page, 1 the address
bool g_kbdShift = false, g_kbdSymbols = false;
bool g_kbdCaps = false;      // shift locked (a quick double tap on Shift): capitals until Shift is tapped again
double g_kbdShiftT = -10;    // when Shift was last tapped
bool g_kbdKeysShown = false; // with the physical keyboard set: the on-screen keys asked for anyway
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

// a new tab's page (the helper's xptb::kStartPage): counted as blank, and called "New tab"
bool isStartPage(const std::string& u) {
    return u.empty() || u == "about:blank" || u == "https://duckduckgo.com/" || u == "https://duckduckgo.com";
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
    const bool hot = enabled && ImGui::IsItemHovered(), down = enabled && ImGui::IsItemActive();
    ImGui::EndDisabled();
    if (hot || down) {   // pressable: it lightens plainly, with the accent's edge (as the other buttons' hover)
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(255, 255, 255, down ? 48 : 32), 8.0f);
        ImGui::GetWindowDrawList()->AddRect(a, b, accentLite(down ? 170 : 120), 8.0f, 0, 1.2f);
    }
    return hit && enabled;
}
void arrow(ImDrawList* dl, ImVec2 c, bool left, ImU32 col) {
    const float d = left ? 1.0f : -1.0f;
    const ImVec2 p[3] = {ImVec2(c.x + 4 * d, c.y - 7), ImVec2(c.x - 3 * d, c.y), ImVec2(c.x + 4 * d, c.y + 7)};
    dl->AddPolyline(p, 3, col, 0, 2.0f);
}
void reloadIcon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->PathArcTo(c, 7.5f, kPi * 0.25f, kPi * 1.85f, 0);
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
    if (filled) fillConcave(dl, std::vector<ImVec2>(pts, pts + 10), col); else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.6f);
}

// A text field on a page (a note, a search) taking the keyboard: target 2
std::string* g_typeInto = nullptr;
bool g_typeMultiline = false;
bool g_typeChanged = false;
void popUtf8(std::string& s) {   // the last character, whole
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}
void typeKey(Actions& act, int code, bool isChar) {
    if (g_kbdTarget == 2) {
        if (!g_typeInto) { g_kbd = false; g_kbdTarget = 0; return; }
        if (!isChar && code == 8) popUtf8(*g_typeInto);
        else if (!isChar && code == 13) {
            if (g_typeMultiline) *g_typeInto += '\n';
            else { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; }
        } else if (isChar && code >= 32 && code < 127) *g_typeInto += (char)code;
        g_typeChanged = true;
        return;
    }
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
bool tabletTyping() { return g_kbd && (g_kbdTarget == 1 || (g_kbdTarget == 2 && g_typeInto)); }
void tabletKey(int code, bool isChar) { Actions none; typeKey(none, code, isChar); }

namespace {

// The on-screen keyboard across the bottom of the page, top at y. Returns its height.
float keyboardBar(ImDrawList* dl, const Fonts& f, float w, float y);
float keyboard(ImDrawList* dl, const Fonts& f, float w, float y, Actions& act) {
    if (g_settings.physicalKeys && !g_kbdKeysShown) return keyboardBar(dl, f, w, y);
    const float pad = 8, gap = 5, kh = 34, H = 5 * kh + 4 * gap + 2 * pad;
    dl->AddRectFilled(ImVec2(0, y), ImVec2(w, y + H), themeTone(20, 26, 38, 250));
    dl->AddLine(ImVec2(0, y), ImVec2(w, y), IM_COL32(255, 255, 255, 40));
    const char* letters[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const char* symbols[4] = {"1234567890", "!@#$%^&*()", "-_=+:;'\"?", "~`|\\<>,"};
    const char* const* rows = g_kbdSymbols ? symbols : letters;
    const float kw = std::floor((w - 2 * pad - 9 * gap) / 10);
    const float x00 = std::floor((w - (10 * kw + 9 * gap)) * 0.5f);
    auto key = [&](const char* id, const char* label, float x, float yy, float ww, bool lit) {
        ImGui::SetCursorScreenPos(ImVec2(x, yy));
        const bool hit = ImGui::InvisibleButton(id, ImVec2(ww, kh));
        const bool down = ImGui::IsItemActive(), hot = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + ww, yy + kh), lit ? accentCol(down ? 255 : hot ? 245 : 230) : IM_COL32(255, 255, 255, down ? 80 : hot ? 46 : 30), 7.0f);
        ImFont* fn = std::strlen(label) > 2 ? f.small : f.bold;
        textCentered(dl, fn, x + ww * 0.5f, std::floor(yy + (kh - fontPx(fn)) * 0.5f), kInk, label);
        return hit;
    };
    for (int r = 0; r < 4; ++r) {
        const int n = (int)std::strlen(rows[r]);
        const float yy = y + pad + r * (kh + gap);
        float x = x00 + (r == 2 ? (kw + gap) * 0.5f : r == 3 ? (kw + gap) * 1.5f : 0.0f);
        if (r == 3) {   // shift, at the left of the last letter row
            // Shift: once for one capital; tapped twice quickly, locked (Caps) until tapped again
            if (key("##kshift", g_kbdSymbols ? "" : g_kbdCaps ? "Caps" : "Shift", x00, yy, kw * 1.5f + gap * 0.5f, g_kbdShift || g_kbdCaps) && !g_kbdSymbols) {
                const double now = ImGui::GetTime();
                if (g_kbdCaps) { g_kbdCaps = false; g_kbdShift = false; }
                else if (g_kbdShift && now - g_kbdShiftT < 0.5) { g_kbdCaps = true; g_kbdShift = false; }
                else g_kbdShift = !g_kbdShift;
                g_kbdShiftT = now;
            }
            if (g_kbdCaps && !g_kbdSymbols) dl->AddLine(ImVec2(x00 + kw * 0.75f - 8, yy + kh - 7), ImVec2(x00 + kw * 0.75f + 8, yy + kh - 7), kInk, 2.0f);   // the lock's bar
        }
        for (int k = 0; k < n; ++k, x += kw + gap) {
            char c = rows[r][k];
            if ((g_kbdShift || g_kbdCaps) && c >= 'a' && c <= 'z') c = (char)(c - 32);
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
    // Done closes the keys on every keyboard (notes, where Enter makes a new line; a web page, where Enter goes to the page; the
    // address bar), except where Enter itself is Done (a one-line field)
    const bool doneKey = !(g_kbdTarget == 2 && !g_typeMultiline);
    const float spaceW = 10 * kw + 9 * gap - (w15 + gap) * 2 - (kw + gap) * 3 - (doneKey ? w15 + gap : 0);
    if (key("##kspace", "space", x, yy, spaceW, false)) typeKey(act, ' ', true);
    x += spaceW + gap;
    if (key("##kdot", ".", x, yy, kw, false)) typeKey(act, '.', true);
    x += kw + gap;
    if (key("##kslash", "/", x, yy, kw, false)) typeKey(act, '/', true);
    x += kw + gap;
    if (doneKey) {
        if (key("##kdone", "Done", x, yy, w15, false)) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; g_kbdShift = false; }
        x += w15 + gap;
    }
    if (key("##kenter", g_kbdTarget == 1 ? "Go" : (g_kbdTarget == 2 && !g_typeMultiline) ? "Done" : "Enter", x, yy, w15, true)) typeKey(act, 13, false);
    return H;
}
// With the physical keyboard set: a thin bar in the keyboard's place (type on the computer's keys), and the on-screen keys if wanted
constexpr float kKbdBarH = 40;
float keyboardBar(ImDrawList* dl, const Fonts& f, float w, float y) {
    const float yb = y + (5 * 34 + 4 * 5 + 16) - kKbdBarH;   // at the bottom, where the keyboard would end
    dl->AddRectFilled(ImVec2(0, yb), ImVec2(w, yb + kKbdBarH), themeTone(20, 26, 38, 250));
    dl->AddLine(ImVec2(0, yb), ImVec2(w, yb), IM_COL32(255, 255, 255, 40));
    keyboardIcon(dl, ImVec2(26, yb + kKbdBarH * 0.5f), accentLite(255));
    const bool notes = g_kbdTarget == 2 && g_typeMultiline;   // notes: Enter makes a new line, so Done closes it
    textAt(dl, f.small, 48, std::floor(yb + (kKbdBarH - fontPx(f.small)) * 0.5f), kInk,
           notes ? "Typing with your keyboard  \xC2\xB7  Enter for a new line" : "Typing with your keyboard  \xC2\xB7  Enter to finish");
    if (pill(dl, f, "##kbardone", "Done", ImVec2(w - 92, yb + 7), ImVec2(w - 12, yb + kKbdBarH - 7), true, true)) {   // (always: a way to close it)
        g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr;
    }
    return kKbdBarH;
}
// how tall the keyboard is now (the full keys, or the physical keyboard's bar)
float keyboardHeight() { return g_settings.physicalKeys && !g_kbdKeysShown ? kKbdBarH : 5 * 34 + 4 * 5 + 16; }

// The page: the browser's picture in the rectangle, with the pointer and the wheel going to it.
// dragScrolls (a document): a drag moves the page as a finger would (sent as the wheel), a tap clicks; else the pointer as a mouse
void webView(ImDrawList* dl, const Fonts& f, const SimState& s, float x0, float y0, float w, float h, Actions& act, bool dragScrolls = false) {
    act.webShown = true;
    act.webX = x0; act.webY = y0; act.webW = w; act.webH = h;
    const BrowserState& b = s.browser;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(244, 246, 250, 255));
    const ImVec2 c(x0 + w * 0.5f, y0 + h * 0.42f);
    if (b.status != 1 || !b.tex || g_webHold) {
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
    static bool dragging = false;
    if (dragScrolls) {
        if (ImGui::IsItemActivated()) dragging = false;
        if (active && !dragging && ImGui::IsMouseDragging(0, 5.0f)) dragging = true;
        if (active && dragging && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
            act.web.push_back({11 /*WHEEL*/, p.x, p.y, (int)std::lround(io.MouseDelta.y), (int)std::lround(io.MouseDelta.x)});
    }
    if ((hovered || active) && !(dragScrolls && dragging) && (p.x != g_webLast.x || p.y != g_webLast.y)) { act.web.push_back({7 /*MOUSE_MOVE*/, p.x, p.y, 0}); g_webLast = p; }
    if (!dragScrolls) {
        if (ImGui::IsItemActivated()) act.web.push_back({8 /*MOUSE_DOWN*/, p.x, p.y, 0});
        if (ImGui::IsItemDeactivated()) act.web.push_back({9 /*MOUSE_UP*/, p.x, p.y, 0});
    } else if (ImGui::IsItemDeactivated() && !dragging) {   // a tap: a click where it was
        act.web.push_back({8 /*MOUSE_DOWN*/, p.x, p.y, 0});
        act.web.push_back({9 /*MOUSE_UP*/, p.x, p.y, 0});
    }
    if (hovered && io.MouseWheel != 0) act.web.push_back({11 /*WHEEL*/, p.x, p.y, (int)std::lround(io.MouseWheel * 100)});
    if (g_webHover && !hovered && !active) { act.web.push_back({10 /*MOUSE_LEAVE*/, g_webLast.x, g_webLast.y, 0}); g_webLast = ImVec2(-1, -1); }
    g_webHover = hovered || active;
}

// The page's zoom: - , the zoom (muted while it is the one fitting the page to the view; tapped when chosen: back to fitting), +.
// The zoom chosen stays with that tab; a new tab fits its page.
void webZoomControls(ImDrawList* dl, const Fonts& f, const BrowserState& b, Actions& act, const char* tag, float x0, float y0, float y1, bool enabled) {
    auto zoomButton = [&](const char* id, ImVec2 a, ImVec2 e, int dir) {   // a magnifier with - or +
        if (iconButton(id, a, e, enabled)) act.webZoom = dir;
        const ImVec2 c((a.x + e.x) * 0.5f - 1, (a.y + e.y) * 0.5f - 1);
        const ImU32 col = enabled ? IM_COL32(210, 222, 242, 255) : IM_COL32(100, 118, 148, 255);
        dl->AddCircle(c, 6.5f, col, 0, 1.6f);
        dl->AddLine(ImVec2(c.x + 4.8f, c.y + 4.8f), ImVec2(c.x + 9, c.y + 9), col, 2.2f);
        dl->AddLine(ImVec2(c.x - 3, c.y), ImVec2(c.x + 3, c.y), col, 1.5f);
        if (dir > 0) dl->AddLine(ImVec2(c.x, c.y - 3), ImVec2(c.x, c.y + 3), col, 1.5f);
    };
    char id[24];
    std::snprintf(id, sizeof id, "##%szout", tag); zoomButton(id, ImVec2(x0, y0), ImVec2(x0 + 36, y1), -1);
    char zt[16];
    std::snprintf(zt, sizeof zt, "%d%%", enabled ? b.zoomPct : 100);
    std::snprintf(id, sizeof id, "##%szreset", tag);
    const ImVec2 za(x0 + 38, y0 + 2), ze(x0 + 88, y1 - 2);
    const bool chosen = enabled && !b.zoomAuto;
    if (iconButton(id, za, ze, chosen)) act.webZoom = 2;
    textCentered(dl, f.small, (za.x + ze.x) * 0.5f, std::floor((y0 + y1) * 0.5f - fontPx(f.small) * 0.5f), chosen ? kInk : kMuted, zt);
    std::snprintf(id, sizeof id, "##%szin", tag); zoomButton(id, ImVec2(x0 + 90, y0), ImVec2(x0 + 126, y1), 1);
}

void browserPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const BrowserState& b = s.browser;
    // the page's own wish for a keyboard: up when a text field takes the focus, down when it lets go
    if (b.wantsKeyboard != g_wantedKbd) { g_wantedKbd = b.wantsKeyboard; if (b.wantsKeyboard) { g_kbd = true; g_kbdTarget = 0; } else if (g_kbdTarget == 0) g_kbd = false; }
    // this page's own tabs: the Browser's, or this web app's (each keeps its pages apart: a chart PDF ForeFlight opens stays with it)
    std::vector<BrowserState::Tab> tabs;
    for (const BrowserState::Tab& t : b.tabs) if (t.group == g_webGroup) tabs.push_back(t);
    bool activeHere = false;
    for (const BrowserState::Tab& t : tabs) if (t.id == b.activeTab) activeHere = true;
    const bool room = (int)b.tabs.size() < 16;   // (all groups together)
    static std::map<int, int> lastShown;          // each group's tab shown last
    static int openingFor = -1; static double openingAt = -10;   // a tab asked for this group, not here yet
    if (!tabs.empty() || ImGui::GetTime() - openingAt > 3) openingFor = -1;
    auto openTabHere = [&](const std::string& url) {
        if (openingFor == g_webGroup || !room) return;
        act.webTabNew = true; act.webTabGroup = g_webGroup; act.webTabUrl = url;
        openingFor = g_webGroup; openingAt = ImGui::GetTime();
    };
    // an address waiting. Typed or a bookmark: into the shown tab. A web app: its own tab, shown again as it was left if it is open
    // (in the Browser: the tab with that site, else a blank tab takes it, else a new tab).
    if (!g_navPending.empty() && b.status == 1) {
        auto key = [](const std::string& u) { return u.compare(0, 5, "file:") == 0 ? u : hostOf(u); };
        if (tabs.empty()) openTabHere(g_navPending);
        else if (g_navForce) act.webNavigate = g_navPending;
        else if (g_webGroup != 0) {   // a web app: its tab shown last (it was left as it was)
            int want = lastShown.count(g_webGroup) ? lastShown[g_webGroup] : 0;
            bool ok = false;
            for (const BrowserState::Tab& t : tabs) if (t.id == want) ok = true;
            if (!ok) want = tabs.front().id;
            if (want != b.activeTab) act.webTabSelect = want;
        } else {
            const std::string want = key(g_navPending);
            int found = 0;
            for (const BrowserState::Tab& t : tabs) if (!t.url.empty() && key(t.url) == want) { found = t.id; break; }
            const bool blank = activeHere && isStartPage(b.url);
            if (found && found != b.activeTab) act.webTabSelect = found;
            else if (found) { /* already shown */ }
            else if (blank) act.webNavigate = g_navPending;
            else if (room) openTabHere(g_navPending);
            else act.webNavigate = g_navPending;
        }
        g_navPending.clear(); g_navForce = false;
    } else if (b.status == 1 && !act.webTabSelect && !act.webTabNew) {   // the shown tab always this page's own
        if (activeHere) lastShown[g_webGroup] = b.activeTab;
        else if (!tabs.empty()) {
            int want = lastShown.count(g_webGroup) ? lastShown[g_webGroup] : 0;
            bool ok = false;
            for (const BrowserState::Tab& t : tabs) if (t.id == want) ok = true;
            act.webTabSelect = ok ? want : tabs.front().id;
        } else openTabHere(g_webGroup == 0 ? std::string() : g_url);   // none left: the Browser's search page, or the app's site
    }
    g_webHold = !activeHere;
    if (videoFull(s)) {   // full screen: the page on the whole tablet; "Exit full screen" shows for a while when the pointer moves
        webView(dl, f, s, 0, 0, w, h, act);
        static double movedAt = -10;
        static ImVec2 lastMouse(-1, -1);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x != lastMouse.x || mp.y != lastMouse.y) { movedAt = ImGui::GetTime(); lastMouse = mp; }
        const float k = 1.0f - smooth01((float)(ImGui::GetTime() - movedAt - 2.5) / 0.4f);
        if (k > 0.01f) {
            const ImVec2 a(w - 150, 10), e(w - 10, 40);
            dl->AddRectFilled(a, e, IM_COL32(0, 0, 0, (int)(150 * k)), 15.0f);
            if (pill(dl, f, "##webfullx", "Exit full screen", a, e, false, true)) act.webExitFullscreen = true;
        }
        return;
    }
    // the tab strip: each tab with its page's title and a close button; + (the Browser's) opens a blank one and the address bar
    auto tabStrip = [&](float top, float tabsH, bool plus) {
        dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + tabsH), IM_COL32(6, 16, 34, 245));
        const int n = std::max(1, (int)tabs.size());
        const float plusW = plus ? 34.0f : 0.0f, tw = std::min(190.0f, std::floor((w - 16 - plusW - 6) / n));
        float x = 8;
        int idx = 0;
        for (const BrowserState::Tab& t : tabs) {
            const bool on = t.id == b.activeTab;
            const ImVec2 a(x, top + 4), e(x + tw - 4, top + tabsH);
            char id[24];
            std::snprintf(id, sizeof id, "##tab%d", idx);
            ImGui::SetCursorScreenPos(a);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x - 22, e.y - a.y)) && !on) act.webTabSelect = t.id;
            const bool hot = ImGui::IsItemHovered();
            dl->AddRectFilled(a, e, on ? themeTone(10, 24, 48, 255) : hot ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10), 8.0f, ImDrawFlags_RoundCornersTop);
            // the Browser's last tab's x clears it (a fresh search page takes its place): there is always one
            std::string title = isStartPage(t.url) ? std::string("New tab") : !t.title.empty() ? t.title : hostOf(t.url);
            const float roomW = e.x - a.x - 34;
            while (title.size() > 3 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, title.c_str()).x > roomW) { title.resize(title.size() - 3); title += ".."; }
            dl->PushClipRect(a, ImVec2(e.x - 22, e.y), true);
            textAt(dl, f.small, a.x + 10, std::floor(a.y + (e.y - a.y - fontPx(f.small)) * 0.5f), on ? kInk : kMuted, title.c_str());
            dl->PopClipRect();
            if (t.loading) dl->AddCircleFilled(ImVec2(a.x + 4, a.y + (e.y - a.y) * 0.5f), 2.0f, IM_COL32(90, 160, 255, 255), 0);
            const ImVec2 cc(e.x - 12, a.y + (e.y - a.y) * 0.5f);
            std::snprintf(id, sizeof id, "##tabx%d", idx);
            ImGui::SetCursorScreenPos(ImVec2(e.x - 22, a.y));
            if (ImGui::InvisibleButton(id, ImVec2(22, e.y - a.y))) act.webTabClose = t.id;
            const bool xh = ImGui::IsItemHovered();
            if (xh) dl->AddCircleFilled(cc, 8.0f, IM_COL32(255, 255, 255, 28), 0);
            const ImU32 xc = xh ? kInk : kMuted;
            dl->AddLine(ImVec2(cc.x - 4, cc.y - 4), ImVec2(cc.x + 4, cc.y + 4), xc, 1.5f);
            dl->AddLine(ImVec2(cc.x - 4, cc.y + 4), ImVec2(cc.x + 4, cc.y - 4), xc, 1.5f);
            x += tw;
            ++idx;
        }
        if (plus) {
            const ImVec2 pa(x + 2, top + 5), pe(x + 2 + plusW - 4, top + tabsH - 3);
            if (iconButton("##tabnew", pa, pe, room && b.status == 1)) { act.webTabNew = true; act.webTabGroup = 0; act.webTabUrl.clear(); g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); }
            const ImVec2 pc((pa.x + pe.x) * 0.5f, (pa.y + pe.y) * 0.5f);
            const ImU32 pcol = room ? kInk : kMuted;
            dl->AddLine(ImVec2(pc.x - 6, pc.y), ImVec2(pc.x + 6, pc.y), pcol, 1.8f);
            dl->AddLine(ImVec2(pc.x, pc.y - 6), ImVec2(pc.x, pc.y + 6), pcol, 1.8f);
        }
    };
    auto zoomGroup = [&](const char* tag, float x0, float y0, float y1) { webZoomControls(dl, f, b, act, tag, x0, y0, y1, b.status == 1 && !g_webHold); };
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
        keyboardIcon(dl, ImVec2(w - 32, my), g_kbd ? accentLite(255) : ink);
        // home: the app's own start page again
        const char* homeUrl = (g_webGroup >= 100 && g_webGroup - 100 < APP_COUNT) ? kApps[g_webGroup - 100].url : nullptr;
        if (iconButton("##whome", ImVec2(w - 172, kStatusH + 6), ImVec2(w - 136, kStatusH + kHeaderH - 6), homeUrl != nullptr) && homeUrl) act.webNavigate = homeUrl;
        {   // a house
            const ImVec2 c(w - 154, my);
            const ImU32 col = homeUrl ? ink : dim;
            dl->AddTriangleFilled(ImVec2(c.x - 9, c.y - 1), ImVec2(c.x, c.y - 9), ImVec2(c.x + 9, c.y - 1), col);
            dl->AddRectFilled(ImVec2(c.x - 6, c.y - 2), ImVec2(c.x + 6, c.y + 8), col, 1.5f);
            dl->AddRectFilled(ImVec2(c.x - 2, c.y + 2), ImVec2(c.x + 2, c.y + 8), themeTone(16, 34, 66, 255));
        }
        zoomGroup("w", w - 306, kStatusH + 6, kStatusH + kHeaderH - 6);
        if (tabs.size() > 1) {   // the app opened more (a chart, a PDF): its tabs, to switch and close them
            tabStrip(top, 30, false);
            pageTop = top + 30;
        }
    } else {
        const float tabsH = 32;
        tabStrip(top, tabsH, true);
        top += tabsH;
        const float barH = 40, bmH = 34;
        dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + barH + bmH), themeTone(10, 24, 48, 235));
        const float my = top + barH * 0.5f;
        if (iconButton("##bback", ImVec2(8, top + 4), ImVec2(36, top + barH - 4), b.canBack)) act.webNav = 1;
        arrow(dl, ImVec2(22, my), true, b.canBack ? ink : dim);
        if (iconButton("##bfwd", ImVec2(38, top + 4), ImVec2(66, top + barH - 4), b.canForward)) act.webNav = 2;
        arrow(dl, ImVec2(52, my), false, b.canForward ? ink : dim);
        if (iconButton("##breload", ImVec2(70, top + 4), ImVec2(102, top + barH - 4), true)) act.webNav = b.loading ? 4 : 3;
        reloadIcon(dl, ImVec2(84, my), ink);
        // the address: tapped, it takes what is typed on the keyboard
        // zoom, between the address and the star (the percentage inside the address, when it is not 100 %)
        zoomGroup("b", w - 216, top + 4, top + barH - 4);
        const ImVec2 a(110, top + 6), e(w - 222, top + barH - 6);
        const bool editing = g_kbd && g_kbdTarget == 1;
        if (iconButton("##baddr", a, e, true)) { g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); }
        dl->AddRectFilled(a, e, IM_COL32(255, 255, 255, editing ? 44 : 26), 14.0f);
        if (editing) dl->AddRect(a, e, accentLite(200), 14.0f, 0, 1.5f);
        dl->PushClipRect(a, e, true);
        // the shown tab's own address (a blank tab: none, and the hint; never another page's)
        const std::string shown = editing ? g_addrEdit : (g_webHold || isStartPage(b.url) ? std::string() : b.url);
        const float ty = std::floor(my - fontPx(f.body) * 0.5f);
        if (shown.empty()) dl->AddText(f.body, fontPx(f.body), ImVec2(a.x + 19, ty), dim, "Type an address or a search");   // (clear of the caret)
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
        keyboardIcon(dl, ImVec2(w - 28, my), g_kbd && g_kbdTarget == 0 ? accentLite(255) : ink);
        // bookmarks: only the user's own (the star adds and removes them); pressing one loads it
        float x = 14;
        const float by = top + barH + 3;
        struct Bm { std::string name, url; };
        std::vector<Bm> bms;
        if (g_settings.bookmarks.empty())
            dl->AddText(f.small, fontPx(f.small), ImVec2(x, std::floor(by + (bmH - 8 - fontPx(f.small)) * 0.5f)), dim, "No bookmarks yet: the star in the address bar adds this page");
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
    if (g_kbd) kbH = keyboardHeight();
    webView(dl, f, s, 0, pageTop + 1, w, h - pageTop - 1 - kbH, act);
    if (g_kbd) keyboard(dl, f, w, h - (5 * 34 + 4 * 5 + 16), act);   // (the bar puts itself at the bottom)
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
    dl->AddRect(i0, i1, accentLite(160), 10.0f);
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
            const bool hit = ImGui::InvisibleButton("##mkback", ImVec2(bw, kh));
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
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 0, 2.0f);
        for (int i = 0; i < 4; ++i) {
            const float an = i * kPi * 0.5f - kPi * 0.5f;
            textCentered(dl, f.small, c.x + std::cos(an) * (r + 11), c.y + std::sin(an) * (r + 11) - fontPx(f.small) * 0.5f, kMuted, i == 0 ? "N" : i == 1 ? "E" : i == 2 ? "S" : "W");
        }
        if (x.valid && x.windKt >= 1) {   // an arrow flying with the wind: from where it blows, through the centre
            const float an = (x.windDir - 90) * kPi / 180;
            const ImVec2 from(c.x + std::cos(an) * (r - 6), c.y + std::sin(an) * (r - 6)), to(c.x - std::cos(an) * (r - 22), c.y - std::sin(an) * (r - 22));
            dl->AddLine(from, to, accentLite(255), 3.0f);
            const ImVec2 n(std::cos(an), std::sin(an)), t(-n.y, n.x);
            dl->AddTriangleFilled(ImVec2(to.x - n.x * 4, to.y - n.y * 4), ImVec2(to.x + n.x * 10 + t.x * 7, to.y + n.y * 10 + t.y * 7), ImVec2(to.x + n.x * 10 - t.x * 7, to.y + n.y * 10 - t.y * 7), accentLite(255));
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
    // more of what flies about (sizes only matter for picking and sizing a close drawing)
    {"B733", 33.40f, 28.88f, 3.76f, 2, 25}, {"B734", 36.40f, 28.88f, 3.76f, 2, 25}, {"B735", 31.00f, 28.88f, 3.76f, 2, 25},
    {"B736", 31.24f, 35.80f, 3.76f, 2, 25}, {"B37M", 35.56f, 35.92f, 3.76f, 2, 25}, {"B3XM", 43.80f, 35.92f, 3.76f, 2, 25},
    {"B742", 70.66f, 59.64f, 6.50f, 4, 37}, {"B764", 61.37f, 51.92f, 5.03f, 2, 31}, {"B77F", 63.73f, 64.80f, 6.20f, 2, 31},
    {"B778", 70.87f, 71.75f, 6.20f, 2, 31}, {"B779", 76.72f, 71.75f, 6.20f, 2, 31}, {"A306", 54.08f, 44.84f, 5.64f, 2, 28},
    {"A310", 46.66f, 43.89f, 5.64f, 2, 28}, {"A3ST", 56.15f, 44.84f, 7.10f, 2, 28}, {"MD11", 61.62f, 51.97f, 6.02f, 3, 35},
    {"DC10", 55.50f, 50.40f, 6.02f, 3, 35}, {"MD90", 46.50f, 32.87f, 3.34f, -2, 24}, {"E145", 29.87f, 20.04f, 2.28f, -2, 23},
    {"E135", 26.33f, 20.04f, 2.28f, -2, 23}, {"E290", 36.20f, 33.72f, 3.01f, 2, 23}, {"E295", 41.50f, 35.12f, 3.01f, 2, 23},
    {"CRJ1", 26.77f, 21.21f, 2.69f, -2, 24}, {"CRJX", 39.10f, 26.18f, 2.69f, -2, 25}, {"AT72", 27.17f, 27.05f, 2.77f, 12, 0},
    {"AT75", 27.17f, 27.05f, 2.77f, 12, 0}, {"AT76", 27.17f, 27.05f, 2.77f, 12, 0}, {"AT43", 22.67f, 24.57f, 2.77f, 12, 0},
    {"AT45", 22.67f, 24.57f, 2.77f, 12, 0}, {"DH8A", 22.25f, 25.91f, 2.69f, 12, 0}, {"DH8C", 25.68f, 27.43f, 2.69f, 12, 0},
    {"DH8D", 32.83f, 28.42f, 2.69f, 12, 0}, {"SF34", 19.73f, 21.44f, 2.31f, 12, 0}, {"B190", 17.63f, 17.67f, 1.98f, 12, 0},
    {"JS41", 19.25f, 18.29f, 2.11f, 12, 0}, {"C130", 29.79f, 40.41f, 4.34f, 12, 0}, {"C17", 53.04f, 51.75f, 6.86f, 4, 25},
    {"C56X", 16.00f, 17.17f, 1.70f, -2, 0}, {"C68A", 19.37f, 22.04f, 1.95f, -2, 15}, {"C700", 22.30f, 22.04f, 2.20f, -2, 20},
    {"CL35", 20.92f, 21.00f, 2.20f, -2, 25}, {"GLEX", 30.30f, 28.65f, 2.69f, -2, 35}, {"GL7T", 33.80f, 31.70f, 2.69f, -2, 35},
    {"GLF4", 26.92f, 23.72f, 2.40f, -2, 27}, {"GLF6", 30.41f, 30.36f, 2.60f, -2, 33}, {"FA7X", 23.38f, 26.21f, 2.40f, -3, 30},
    {"LJ45", 17.68f, 14.58f, 1.80f, -2, 13}, {"PC24", 16.85f, 17.00f, 1.80f, -2, 10}, {"E50P", 12.82f, 12.25f, 1.60f, -2, 0},
    {"E75L", 31.68f, 26.00f, 3.01f, 2, 23}, {"E75S", 31.68f, 26.00f, 3.01f, 2, 23},
    {"C182", 8.84f, 11.00f, 1.07f, 1, 0}, {"C206", 8.61f, 10.92f, 1.10f, 1, 0}, {"C310", 9.74f, 11.25f, 1.20f, 12, 0},
    {"PA34", 8.72f, 11.85f, 1.10f, 12, 0}, {"P28B", 7.32f, 10.67f, 1.07f, 1, 0}, {"PA46", 8.81f, 13.11f, 1.27f, 1, 0},
    {"BE36", 8.38f, 10.21f, 1.10f, 1, 0}, {"BE20", 13.34f, 16.61f, 1.45f, 12, 0}, {"DHC6", 15.77f, 19.81f, 1.60f, 12, 0},
    {"DA40", 8.06f, 11.94f, 1.10f, 1, 0}, {"DA62", 9.19f, 14.55f, 1.20f, 12, 0}, {"C25B", 15.98f, 15.49f, 1.60f, -2, 0},
};
// a type's sizes when they are known (its own, else its family's by the first three letters); nullptr when not
const PlaneSpec* planeSpecKnown(const std::string& icao) {
    for (const PlaneSpec& p : kPlanes) if (icao == p.icao) return &p;
    for (const PlaneSpec& p : kPlanes) if (icao.size() >= 3 && icao.compare(0, 3, p.icao, 3) == 0) return &p;
    return nullptr;
}
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
const Shape* exactShape(const std::string& icao, float& len, float& span) {
    for (const ShapeAlias& a : g_aliases) if (a.icao == icao) { len = a.len; span = a.span; return &g_shapes[a.shape]; }
    for (const Shape& sh : g_shapes) if (sh.name == icao) { len = sh.len; span = sh.span; return &sh; }
    return nullptr;
}
// A type without a drawing of its own gets the closest there is (worked out once a type): one of its family (the same first three
// letters: B39M -> B738), else the one nearest in length and span, drawn at the type's own size when that is known.
struct ShapePick { int shape; float len, span; };
std::map<std::string, ShapePick> g_shapePicks;
size_t g_shapePicksFor = 0;   // the number of drawings the picks were made from (made again when the drawings load)
const Shape* closeShape(const std::string& icao, float& len, float& span) {
    if (g_shapes.empty() || icao.empty()) return nullptr;
    if (g_shapePicksFor != g_shapes.size()) { g_shapePicks.clear(); g_shapePicksFor = g_shapes.size(); }
    auto it = g_shapePicks.find(icao);
    if (it == g_shapePicks.end()) {
        const PlaneSpec* sp = planeSpecKnown(icao);
        const float wantL = sp ? sp->len : 37.57f, wantS = sp ? sp->span : 35.80f;   // unknown altogether: a mid-size airliner
        auto sizeOf = [&](int i, float& l, float& s) {
            for (const ShapeAlias& a : g_aliases) if (a.shape == i && a.icao == g_shapes[i].name) { l = a.len; s = a.span; return; }
            l = g_shapes[i].len; s = g_shapes[i].span;
        };
        int best = -1;
        float bestD = 1e9f;
        // the family first: drawings, and aliases, whose type shares the first three letters
        if (icao.size() >= 3) {
            for (int i = 0; i < (int)g_shapes.size(); ++i) {
                if (g_shapes[i].name.size() < 3 || g_shapes[i].name.compare(0, 3, icao, 0, 3) != 0) continue;
                float l, s; sizeOf(i, l, s);
                const float d = std::fabs(l - wantL) + std::fabs(s - wantS);
                if (d < bestD) { bestD = d; best = i; }
            }
            for (const ShapeAlias& a : g_aliases) {
                if (a.icao.size() < 3 || a.icao.compare(0, 3, icao, 0, 3) != 0) continue;
                const float d = std::fabs(a.len - wantL) + std::fabs(a.span - wantS);
                if (d < bestD) { bestD = d; best = a.shape; }
            }
        }
        if (best < 0) {   // else the nearest in size (relative, so a Cessna is not matched to a 737 for being "only" 30 m off)
            for (int i = 0; i < (int)g_shapes.size(); ++i) {
                float l, s; sizeOf(i, l, s);
                if (l <= 0 || s <= 0) continue;
                const float d = std::fabs(std::log(l / wantL)) + std::fabs(std::log(s / wantS));
                if (d < bestD) { bestD = d; best = i; }
            }
        }
        if (best < 0) return nullptr;
        float l, s; sizeOf(best, l, s);
        it = g_shapePicks.emplace(icao, ShapePick{best, sp ? sp->len : l, sp ? sp->span : s}).first;
    }
    len = it->second.len; span = it->second.span;
    return &g_shapes[it->second.shape];
}
const Shape* findShape(const std::string& icao, float& len, float& span) {
    if (const Shape* sh = exactShape(icao, len, span)) return sh;
    return closeShape(icao, len, span);
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
int g_mapMenu = 0;                   // the map's open menu: 0 none, 1 Navigation, 2 Traffic, 3 Features
double g_mapZoom = 14.0;             // Web Mercator zoom: the world is 256 * 2^zoom layout units across (from the range below)
// The range (the middle to the top or bottom edge): WXPlane's steps, with two closer ones for taxiing; the shown range eases to
// the chosen one in log space, as WXPlane's does.
// Close in, small steps (down to 0.05 nm, about 90 m to the edge: a stand and the aircraft on it); farther out, WXPlane's doubling steps.
const double kRangesNm[] = {0.05, 0.08, 0.1, 0.15, 0.2, 0.25, 0.35, 0.5, 0.75, 1, 1.5, 2, 3, 5, 7.5, 10, 15, 20, 30, 40, 60, 80, 120, 160, 320, 640, 1280, 2500};
constexpr int kRangeN = (int)(sizeof kRangesNm / sizeof kRangesNm[0]);
constexpr int kRangeTaxi = 9;   // 1 nm: where the map starts on the aircraft
int rangeIndexOf(double nm) {   // the step nearest a range (in log space)
    int best = 0;
    for (int i = 1; i < kRangeN; ++i) if (std::fabs(std::log(kRangesNm[i] / nm)) < std::fabs(std::log(kRangesNm[best] / nm))) best = i;
    return best;
}
int g_mapRange = kRangeTaxi;
double g_mapShownNm = 1.0;
bool g_mapFollow = true;             // the map stays centred on the aircraft
double g_mapGlideStart = -1;         // a glide to a new centre is under way since then (-1: none)
MercPt g_mapGlideFrom{0.5, 0.5}, g_mapGlideTo{0.5, 0.5};
bool g_mapGoto = false;              // centre on a place next time (Airports' Show on map)
// ---- the breadcrumbs: where the aircraft has flown since the SimBrief plan was imported (kept until it is cleared or another
// plan comes in). A jump of more than 8 nm between two samples (the aircraft moved in the sim) starts a new piece of trail.
struct Crumb { double lat = 0, lon = 0; bool start = false; };
std::vector<Crumb> g_crumbs;
std::string g_crumbsFor;   // the plan they belong to
double crumbNm(double la0, double lo0, double la1, double lo1) {
    const double dLa = (la1 - la0) * 60.0, dLo = (lo1 - lo0) * 60.0 * std::cos(la0 * kPi / 180.0);
    return std::sqrt(dLa * dLa + dLo * dLo);
}
void trackCrumbs(const SimState& s) {
    const FlightPlan& p = s.plan;
    if (!p.loaded) { g_crumbs.clear(); g_crumbsFor.clear(); return; }   // no plan, or cleared
    const std::string key = !p.ofpId.empty() ? p.ofpId : p.origin + ">" + p.dest + "@" + p.importedAt;
    if (key != g_crumbsFor) { g_crumbs.clear(); g_crumbsFor = key; }   // another plan
    const MapState& m = s.map;
    if (!m.havePos) return;
    if (g_crumbs.empty()) { g_crumbs.push_back({m.lat, m.lon, true}); return; }
    const Crumb& last = g_crumbs.back();
    const double d = crumbNm(last.lat, last.lon, m.lat, m.lon);
    if (d > 8.0) g_crumbs.push_back({m.lat, m.lon, true});          // moved in the sim: a new piece
    else if (d >= 0.12) g_crumbs.push_back({m.lat, m.lon, false});  // every eighth of a mile or so
    if (g_crumbs.size() > 60000) {   // a very long flight: every other point of the older half (pieces' starts kept)
        std::vector<Crumb> t;
        t.reserve(g_crumbs.size());
        for (size_t i = 0; i < g_crumbs.size(); ++i) if (i >= g_crumbs.size() / 2 || i % 2 == 0 || g_crumbs[i].start) t.push_back(g_crumbs[i]);
        g_crumbs.swap(t);
    }
}

float g_mapLastW = 800, g_mapLastH = 350;   // the map as last drawn (for AviTab's commands and datarefs)
double g_mapLastRot = 0, g_mapLastScale = 1, g_mapLastMPerMerc = 1;
double g_mapGotoLat = 0, g_mapGotoLon = 0;
MercPt g_mapCenter{0.5, 0.5};
bool g_mapSeen = false;              // the map has had the aircraft's position once
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
            const double acMap = qa * 900.0 + wx::dstShiftS(m.lat, m.lon, qa, now);
            // X-Plane's clock is used only where it agrees with the zone map at the aircraft (its own guess can be off by an hour,
            // e.g. South Bend on Central time by its longitude): then the time changes as the pointer crosses a zone's border
            const bool simAgrees = !acOnMap || std::fabs(m.localOffsetS - acMap) < 60.0;
            if (simAgrees && (!onMap || !acOnMap || q * 900.0 + wx::dstShiftS(lat, lon, q, now) == acMap)) return m.localOffsetS;
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
bool mapBubble(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, const char* label, const char* text, bool lit, float minW = 0, bool enabled = true) {
    const float lw = label ? f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, label).x + 7 : 0;
    const float tw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, text).x;
    const ImVec2 b(a.x + std::max(minW, lw + tw + 20), a.y + 30);
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y)) && enabled;
    const Ease e = eased(id, lit && enabled, ImGui::IsItemHovered() && enabled, ImGui::IsItemActive() && enabled);
    ImGui::EndDisabled();
    const float inset = e.down * 1.2f;
    const ImVec2 a2(a.x + inset, a.y + inset), b2(b.x - inset, b.y - inset);
    ImU32 bg = mixCol(themeTone(12, 22, 40, 225), themeTone(26, 40, 64, 238), e.hot);
    bg = mixCol(bg, mixCol(themeTone(12, 22, 40, 240), accentCol(240), 0.38f), e.on * 0.85f);   // lit: the theme's accent, darkened
    dl->AddRectFilled(a2, b2, bg, 9.0f);
    dl->AddRect(a2, b2, mixCol(IM_COL32(255, 255, 255, 46), accentLite(255), e.on), 9.0f, 0, 1.0f + 0.6f * e.on);
    const float tx = a.x + (b.x - a.x - lw - tw) * 0.5f;
    if (label) textAt(dl, f.small, tx, std::floor(a.y + (30 - fontPx(f.small)) * 0.5f + 1), kMuted, label);
    textAt(dl, f.body, tx + lw, std::floor(a.y + (30 - fontPx(f.body)) * 0.5f), enabled ? mixCol(kInk, accentLite(255), e.on) : IM_COL32(236, 241, 250, 80), text);
    return hit;
}
float bubbleW(const Fonts& f, const char* label, const char* text) {
    const float lw = label ? f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, label).x + 7 : 0;
    return lw + f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, text).x + 20;
}
// ---- the map's pop-out menus (Navigation, Traffic, Features): a row with a check box; a row with a value stepped by < >
bool menuCheck(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, float w, const char* label, bool on, bool enabled = true) {
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(w, 28)) && enabled;
    const Ease e = eased(id, on && enabled, ImGui::IsItemHovered() && enabled, ImGui::IsItemActive() && enabled);
    ImGui::EndDisabled();
    if (e.hot > 0.01f) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + 28), IM_COL32(255, 255, 255, (int)(16 * e.hot)), 7.0f);
    const ImVec2 b0(a.x + 8, a.y + 6), b1(a.x + 24, a.y + 22);   // the box: the accent with a tick when on
    dl->AddRectFilled(b0, b1, mixCol(themeTone(10, 18, 32, 255), accentCol(255), e.on), 4.0f);
    dl->AddRect(b0, b1, mixCol(IM_COL32(255, 255, 255, 110), accentLite(255), e.on), 4.0f, 0, 1.4f);
    if (e.on > 0.02f) {
        const ImU32 tc = IM_COL32(255, 255, 255, (int)(255 * e.on));
        dl->AddLine(ImVec2(b0.x + 3.5f, b0.y + 8.5f), ImVec2(b0.x + 6.5f, b0.y + 11.5f), tc, 2.0f);
        dl->AddLine(ImVec2(b0.x + 6.5f, b0.y + 11.5f), ImVec2(b0.x + 12.5f, b0.y + 4.5f), tc, 2.0f);
    }
    textAt(dl, f.body, a.x + 32, std::floor(a.y + (28 - fontPx(f.body)) * 0.5f), enabled ? kInk : kMuted, label);
    return hit;
}
// returns -1 or +1 when a side is tapped
int menuStepper(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, float w, const char* label, const char* value, bool canDown, bool canUp) {
    textAt(dl, f.body, a.x + 8, std::floor(a.y + (28 - fontPx(f.body)) * 0.5f), kInk, label);
    int r = 0;
    char bid[32];
    const float bw = 26, vx1 = a.x + w - 4, vx0 = vx1 - 2 * bw - 78;
    std::snprintf(bid, sizeof bid, "%s-", id);
    if (iconButton(bid, ImVec2(vx0, a.y + 1), ImVec2(vx0 + bw, a.y + 27), canDown)) r = -1;
    arrow(dl, ImVec2(vx0 + bw * 0.5f + 1, a.y + 14), true, canDown ? kInk : kMuted);
    textCentered(dl, f.small, vx0 + bw + 39, std::floor(a.y + (28 - fontPx(f.small)) * 0.5f), accentLite(255), value);
    std::snprintf(bid, sizeof bid, "%s+", id);
    if (iconButton(bid, ImVec2(vx1 - bw, a.y + 1), ImVec2(vx1, a.y + 27), canUp)) r = 1;
    arrow(dl, ImVec2(vx1 - bw * 0.5f - 1, a.y + 14), false, canUp ? kInk : kMuted);
    return r;
}

// A concave polygon filled whatever way its points go round (ImGui's filler wants them clockwise on the screen, y down)
void fillConcave(ImDrawList* dl, std::vector<ImVec2> v, ImU32 col) {
    if (v.size() < 3) return;
    double a = 0;
    for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) a += (double)v[j].x * v[i].y - (double)v[i].x * v[j].y;
    if (a < 0) std::reverse(v.begin(), v.end());
    dl->AddConcavePolyFilled(v.data(), (int)v.size(), col);
}

// A dashed polyline (screen points)
void dashed(ImDrawList* dl, const std::vector<ImVec2>& p, ImU32 col, float thick, float on, float off) {
    // Only the part of each piece that is on screen is cut into dashes, counted from where it comes on screen, in double precision:
    // zoomed right in, a line is millions of pixels long, and in single precision "a few pixels further along" was the same number,
    // so the dash loop stood still (the sim froze, 2026-11-18). The dash pattern still runs on from piece to piece.
    if (on <= 0.05f || off < 0) { if (p.size() >= 2) dl->AddPolyline(p.data(), (int)p.size(), col, 0, thick); return; }
    const ImVec2 c0 = dl->GetClipRectMin(), c1 = dl->GetClipRectMax();
    const double m = thick + 4, x0 = c0.x - m, y0 = c0.y - m, x1 = c1.x + m, y1 = c1.y + m;
    const double period = (double)on + off;
    double phase = 0;   // how far into the pattern the piece starts
    for (size_t i = 1; i < p.size(); ++i) {
        const double ax = p[i - 1].x, ay = p[i - 1].y, bx = p[i].x, by = p[i].y;
        const double dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 1e-3)) continue;   // (also a NaN)
        // the piece's visible stretch [u0, u1] of it (Liang-Barsky)
        double u0 = 0, u1 = 1;
        auto clipT = [&](double pq, double q) {
            if (std::fabs(pq) < 1e-12) return q >= 0;
            const double r = q / pq;
            if (pq < 0) { if (r > u1) return false; if (r > u0) u0 = r; } else { if (r < u0) return false; if (r < u1) u1 = r; }
            return true;
        };
        const bool shown = clipT(-dx, ax - x0) && clipT(dx, x1 - ax) && clipT(-dy, ay - y0) && clipT(dy, y1 - ay);
        if (shown) {
            const double tA = u0 * len, tB = u1 * len;
            const double ux = dx / len, uy = dy / len;
            // where the pattern is at tA; then dash by dash over the visible stretch only (a few thousand pixels at most)
            double t = tA, inP = std::fmod(phase + tA, period);
            int n = 0;
            while (t < tB && n++ < 4000) {
                if (inP < on) {
                    const double t1 = std::min(tB, t + (on - inP));
                    dl->AddLine(ImVec2((float)(ax + ux * t), (float)(ay + uy * t)), ImVec2((float)(ax + ux * t1), (float)(ay + uy * t1)), col, thick);
                    inP += t1 - t; t = t1;
                } else {
                    const double t1 = std::min(tB, t + (period - inP));
                    inP += t1 - t; t = t1;
                }
                if (inP >= period - 1e-9) inP = 0;
            }
        }
        phase = std::fmod(phase + len, period);
    }
}

// ---- TCAS: how close is this traffic? TCAS II v7.1's thresholds for our height above the ground: the time to the closest point (tau)
// and its distance (DMOD) across, and the height apart (ZTHR) or the time to the same height. A traffic advisory (TA) from 2 nm (or
// TCAS's TA tau) within 850 ft; a resolution advisory (RA) at v7.1's own. No TCAS on the ground or below 1000 ft above it, as TCAS
// itself: on the ground, the runway collision check instead (below).
struct Geom { double range, dz, closing, vClose; };
Geom geomOf(const MapState& m, const MapState::Traffic& t) {
    const double kLat = 60.0, kLon = 60.0 * std::cos(m.lat * kPi / 180.0);
    const double ex = (t.lon - m.lon) * kLon, ny = (t.lat - m.lat) * kLat;   // nm: east, north
    Geom g;
    g.range = std::sqrt(ex * ex + ny * ny);
    g.dz = t.altFt - m.altFt;
    const double ha = m.hdgTrue * kPi / 180.0, ta = t.hdg * kPi / 180.0;
    const double vx = t.gsKt * std::sin(ta) - m.gsKt * std::sin(ha), vy = t.gsKt * std::cos(ta) - m.gsKt * std::cos(ha);
    g.closing = -(ex * vx + ny * vy) / std::max(1e-6, g.range);   // kt, + when getting closer
    g.vClose = -(g.dz > 0 ? 1 : -1) * (t.vsFpm - m.vsFpm);          // ft/min, + when the heights come together
    return g;
}
int tcasLevel(const MapState& m, const MapState::Traffic& t) {   // 0 none, 1 TA, 2 RA
    if (!m.havePos || t.onGround || m.onGround || m.aglFt < 1000) return 0;
    const Geom g = geomOf(m, t);
    float tauS, dmod, zthr, taTau;
    if (m.aglFt < 2350) { tauS = 15; dmod = 0.20f; zthr = 600; taTau = 25; }
    else if (m.altFt < 5000) { tauS = 20; dmod = 0.35f; zthr = 600; taTau = 30; }
    else if (m.altFt < 10000) { tauS = 25; dmod = 0.55f; zthr = 600; taTau = 40; }
    else if (m.altFt < 20000) { tauS = 30; dmod = 0.80f; zthr = 600; taTau = 45; }
    else if (m.altFt < 42000) { tauS = 35; dmod = 1.10f; zthr = 700; taTau = 48; }
    else { tauS = 35; dmod = 1.10f; zthr = 800; taTau = 48; }
    const double tau = g.closing > 0 ? g.range / g.closing * 3600.0 : 1e9;
    const auto vertWithin = [&](double z, double tt) { return std::fabs(g.dz) < z || (g.vClose > 0 && std::fabs(g.dz) / g.vClose * 60.0 < tt); };
    if ((g.range < dmod || tau < tauS) && vertWithin(zthr, tauS)) return 2;
    if ((g.range < 2.0 || tau < taTau) && vertWithin(850, taTau)) return 1;
    return 0;
}
bool tcasRA(const MapState& m, const MapState::Traffic& t) { return tcasLevel(m, t) == 2; }

// ---- runway collision: on a runway with traffic coming in to land on it, or about to land (within 0.5 nm) on a runway with traffic on it.
// The runways are those of the airport layouts read round the aircraft (m.airports).
struct RwyFrame { double ux, uy, len, halfW; double ox, oy; };   // along (unit, nm east/north), length and half width (nm), its end A
bool rwyFrame(const MapState& m, const AptGeo::Runway& r, RwyFrame& f) {
    double la1, lo1, la2, lo2;
    latLonOf(r.a, la1, lo1); latLonOf(r.b, la2, lo2);
    const double kLat = 60.0, kLon = 60.0 * std::cos(m.lat * kPi / 180.0);
    f.ox = (lo1 - m.lon) * kLon; f.oy = (la1 - m.lat) * kLat;
    const double bx = (lo2 - m.lon) * kLon, by = (la2 - m.lat) * kLat;
    f.len = std::sqrt((bx - f.ox) * (bx - f.ox) + (by - f.oy) * (by - f.oy));
    if (f.len < 0.05) return false;
    f.ux = (bx - f.ox) / f.len; f.uy = (by - f.oy) / f.len;
    f.halfW = std::max(0.02, r.widthM / 1852.0 * 0.5 + 0.01);
    return true;
}
// where a point (nm east/north of us) is against the runway: along from end A, and across
void rwyPos(const RwyFrame& f, double x, double y, double& along, double& across) {
    along = (x - f.ox) * f.ux + (y - f.oy) * f.uy;
    across = -(x - f.ox) * f.uy + (y - f.oy) * f.ux;
}
// heading (degrees true) lined up with the runway, either way; +1 toward end B, -1 toward end A, 0 not lined up
int rwyAligned(const RwyFrame& f, float hdg) {
    const double rh = std::atan2(f.ux, f.uy) * 180.0 / kPi;
    double d = std::fmod(hdg - rh + 720.0, 360.0);
    if (d > 180) d -= 360;
    if (std::fabs(d) < 30) return 1;
    if (std::fabs(d) > 150) return -1;
    return 0;
}
bool runwayConflict(const MapState& m, const MapState::Traffic& t) {
    if (!m.havePos) return false;
    const double kLat = 60.0, kLon = 60.0 * std::cos(m.lat * kPi / 180.0);
    const double tx = (t.lon - m.lon) * kLon, ty = (t.lat - m.lat) * kLat;
    if (tx * tx + ty * ty > 36.0) return false;   // over 6 nm away
    for (const auto& ap : m.airports)
        for (const AptGeo::Runway& r : ap->runways) {
            if (r.water) continue;
            RwyFrame f;
            if (!rwyFrame(m, r, f)) continue;
            double oA, oX, tA, tX;
            rwyPos(f, 0, 0, oA, oX);
            rwyPos(f, tx, ty, tA, tX);
            const bool weOn = std::fabs(oX) < f.halfW && oA > -0.03 && oA < f.len + 0.03;
            const bool theyOn = std::fabs(tX) < f.halfW && tA > -0.03 && tA < f.len + 0.03;
            if (m.onGround && weOn && !t.onGround) {   // on it, and traffic coming in to it (lined up, low, within 4 nm of its end)
                const int dir = rwyAligned(f, t.hdg);
                const double toEnd = dir > 0 ? -tA : dir < 0 ? tA - f.len : -1;   // nm still to go before the runway
                if (dir && std::fabs(tX) < 0.35 && toEnd > -0.1 && toEnd < 4.0 && t.altFt - m.altFt < 1500 && t.vsFpm < 300) return true;
            }
            if (!m.onGround && t.onGround && theyOn && m.aglFt < 700 && m.vsFpm < 200) {   // about to land on it, with traffic on it
                const int dir = rwyAligned(f, m.hdgTrue);
                const double toEnd = dir > 0 ? -oA : dir < 0 ? oA - f.len : -1;
                if (dir && std::fabs(oX) < 0.25 && toEnd > -0.2 && toEnd < 0.5) return true;
            }
        }
    return false;
}

// ---- the map's labels: placed after everything is drawn, most important first, each where it touches no other label or symbol
struct MapLabel { int prio; ImVec2 q; float gap; std::string l1, l2; ImU32 c1, c2, bg; };
struct LRect { float x0, y0, x1, y1; bool hits(const LRect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; } };
std::vector<MapLabel> g_labels;
std::vector<LRect> g_symbols;   // the symbols drawn (labels keep off them)
void symbolAt(ImVec2 q, float r) { g_symbols.push_back({q.x - r, q.y - r, q.x + r, q.y + r}); }
void placeLabels(ImDrawList* dl, const Fonts& f, ImVec2 r0, ImVec2 r1) {
    std::stable_sort(g_labels.begin(), g_labels.end(), [](const MapLabel& a, const MapLabel& b) { return a.prio > b.prio; });
    std::vector<LRect> placed;
    const float fs = fontPx(f.small), lh = fs + 1;
    for (const MapLabel& L : g_labels) {
        const float pad = L.bg ? 3.0f : 1.0f;
        const float tw = std::max(f.small->CalcTextSizeA(fs, 1e9f, 0, L.l1.c_str()).x, L.l2.empty() ? 0.0f : f.small->CalcTextSizeA(fs, 1e9f, 0, L.l2.c_str()).x);
        const float w = tw + 2 * pad, h = (L.l2.empty() ? 1 : 2) * lh + 2 * pad - 1, g = L.gap, d = g * 0.72f;
        const ImVec2 q = L.q;
        const ImVec2 cand[8] = {{q.x + g, q.y - h * 0.5f}, {q.x - g - w, q.y - h * 0.5f}, {q.x + d, q.y - d - h}, {q.x + d, q.y + d},
                                {q.x - w * 0.5f, q.y - g - h}, {q.x - w * 0.5f, q.y + g}, {q.x - d - w, q.y - d - h}, {q.x - d - w, q.y + d}};
        for (const ImVec2& c : cand) {
            const LRect R{c.x, c.y, c.x + w, c.y + h};
            if (R.x0 < r0.x + 2 || R.x1 > r1.x - 2 || R.y0 < r0.y + 2 || R.y1 > r1.y - 2) continue;
            bool free = true;
            for (const LRect& o : placed) if (R.hits(o)) { free = false; break; }
            if (free) for (const LRect& o : g_symbols) {
                if (q.x >= o.x0 && q.x <= o.x1 && q.y >= o.y0 && q.y <= o.y1) continue;   // its own symbol
                if (R.hits(o)) { free = false; break; }
            }
            if (!free) continue;
            placed.push_back(R);
            if (L.bg) dl->AddRectFilled(ImVec2(R.x0, R.y0), ImVec2(R.x1, R.y1), L.bg, 3.0f);
            textAt(dl, f.small, R.x0 + pad, R.y0 + pad, L.c1, L.l1.c_str());
            if (!L.l2.empty()) textAt(dl, f.small, R.x0 + pad, R.y0 + pad + lh, L.c2, L.l2.c_str());
            break;
        }
    }
    g_labels.clear();
    g_symbols.clear();
}

// ---- the map's cards (a tapped traffic, navaid, fix, route point or airport) -------------------------------------------------------------
int g_selKind = 0;        // 0 none, 1 traffic, 2 navaid, 3 fix, 4 airport, 5 route point
std::string g_selKey;     // which: call sign, ident|type, ident, ICAO, index
ImVec2 g_mapPressPos;
ImVec2 g_mapFree0, g_mapFree1;   // the map's area clear of its buttons (cards stay inside it)
float g_cardScroll = 0;   // a card taller than the map scrolls
std::string g_cardFor;    // the card it is for (a new card starts at its top)
bool g_cardPinned = false; // the card was dragged by its top: cards open there (where it was left in the map's free area, as fractions,
float g_cardFx = 0, g_cardFy = 0;   // so it keeps its place when the map changes size)
bool g_cardMoving = false;
struct CardLine { int style; std::string a, b; };   // 0 title (a) and subtitle (b), 1 label and value, 2 text (b: its label), 3 Morse, 4 heading, 5 a button
const char* morseLetter(char c) {
    static const char* az[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
    static const char* d09[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return d09[c - '0'];
    return "";
}
std::string morseOf(const std::string& id) { std::string o; for (char c : id) { if (!o.empty()) o += ' '; o += morseLetter(c); } return o; }
std::string posText(double lat, double lon) {
    char b[48];
    std::snprintf(b, sizeof b, "%c%02d\xC2\xB0%04.1f'  %c%03d\xC2\xB0%04.1f'", lat >= 0 ? 'N' : 'S', (int)std::fabs(lat), std::fmod(std::fabs(lat), 1.0) * 60,
                  lon >= 0 ? 'E' : 'W', (int)std::fabs(lon), std::fmod(std::fabs(lon), 1.0) * 60);
    return b;
}
// Draws the card beside the item at `at`, kept inside the map; true when its button was pressed. Its close button closes it.
bool drawCard(ImDrawList* dl, const Fonts& f, const std::vector<CardLine>& L, ImVec2 at, ImVec2 r0, ImVec2 r1) {
    const float W = 300, pad = 14, inner = W - 2 * pad;
    float H = pad;
    for (const CardLine& l : L) {
        switch (l.style) {
        case 0: H += 44; break;
        case 1: H += 20; break;
        case 2: H += f.small->CalcTextSizeA(fontPx(f.small), 1e9f, inner - (l.b.empty() ? 0 : 70), l.a.c_str()).y + 8; break;
        case 3: H += 26; break;
        case 4: H += 28; break;
        case 5: H += 40; break;
        }
    }
    H += pad - 4;
    const float full = H;
    r0 = g_mapFree0; r1 = g_mapFree1;   // never over the map's buttons
    H = std::min(H, r1.y - r0.y);
    ImVec2 a(at.x + 18, at.y - 30);
    if (a.x + W > r1.x - 8) a.x = at.x - 18 - W;
    if (g_cardPinned) { a.x = r0.x + g_cardFx * std::max(0.0f, r1.x - r0.x - W); a.y = r0.y + g_cardFy * std::max(0.0f, r1.y - r0.y - H); }
    a.x = std::max(r0.x, std::min(a.x, r1.x - W));
    a.y = std::max(r0.y, std::min(a.y, r1.y - H));
    ImVec2 e(a.x + W, a.y + H);
    // the card takes its own presses (so they do not reach the map under it): dragged by its top it moves (and stays there,
    // for the next cards too); dragged lower down, a tall card scrolls
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##cardbody", ImVec2(W, H));
    const std::string me = std::to_string(g_selKind) + g_selKey;
    if (me != g_cardFor) { g_cardFor = me; g_cardScroll = 0; }
    if (ImGui::IsItemActivated()) g_cardMoving = ImGui::GetIO().MousePos.y < a.y + 40;
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_cardScroll -= ImGui::GetIO().MouseWheel * 36;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
        if (g_cardMoving) {
            a.x = std::max(r0.x, std::min(a.x + ImGui::GetIO().MouseDelta.x, r1.x - W));
            a.y = std::max(r0.y, std::min(a.y + ImGui::GetIO().MouseDelta.y, r1.y - H));
            e = ImVec2(a.x + W, a.y + H);
            g_cardPinned = true;
            g_cardFx = r1.x - r0.x - W > 1 ? (a.x - r0.x) / (r1.x - r0.x - W) : 0.0f;
            g_cardFy = r1.y - r0.y - H > 1 ? (a.y - r0.y) / (r1.y - r0.y - H) : 0.0f;
        } else g_cardScroll -= ImGui::GetIO().MouseDelta.y;
    }
    g_cardScroll = std::max(0.0f, std::min(g_cardScroll, full - H));
    dl->AddRectFilled(ImVec2(a.x + 2, a.y + 4), ImVec2(e.x + 2, e.y + 4), IM_COL32(0, 0, 0, 70), 14.0f);
    dl->AddRectFilled(a, e, themeTone(14, 26, 48, 246), 14.0f);
    dl->AddRect(a, e, IM_COL32(255, 255, 255, 40), 14.0f);
    dl->AddRectFilled(ImVec2((a.x + e.x) * 0.5f - 18, a.y + 5), ImVec2((a.x + e.x) * 0.5f + 18, a.y + 9), IM_COL32(255, 255, 255, 60), 2.0f);   // its grip
    bool pressed = false;
    dl->PushClipRect(ImVec2(a.x, a.y + 2), ImVec2(e.x, e.y - 2), true);
    float y = a.y + pad - g_cardScroll;
    const float x0 = a.x + pad, x1 = e.x - pad;
    for (const CardLine& l : L) {
        switch (l.style) {
        case 0:
            textAt(dl, f.title, x0, y, kInk, l.a.c_str());
            if (!l.b.empty()) { std::string sub = l.b; while (sub.size() > 3 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, sub.c_str()).x > inner - 10) { sub.resize(sub.size() - 3); sub += ".."; } textAt(dl, f.small, x0, y + 24, kMuted, sub.c_str()); }
            y += 44;
            break;
        case 1: {
            textAt(dl, f.small, x0, y + 1, kMuted, l.a.c_str());
            std::string v = l.b;
            while (v.size() > 3 && f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, v.c_str()).x > inner - 96) { v.resize(v.size() - 3); v += ".."; }
            textRight(dl, f.bold, x1, y, kInk, v.c_str());
            y += 20;
            break;
        }
        case 2: {
            float tx = x0;
            if (!l.b.empty()) { textAt(dl, f.small, x0, y, kMuted, l.b.c_str()); tx = x0 + 70; }
            const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, inner - (tx - x0), l.a.c_str());
            dl->AddText(f.small, fontPx(f.small), ImVec2(tx, y), l.b.empty() ? kMuted : IM_COL32(255, 214, 120, 255), l.a.c_str(), nullptr, inner - (tx - x0));
            y += sz.y + 8;
            break;
        }
        case 3: {   // the ident in Morse: dots and dashes drawn, letter by letter
            textAt(dl, f.small, x0, y + 4, kMuted, "Morse");
            float mx = x1;
            // measure first, then draw from the right
            float total = 0;
            for (size_t i = 0; i < l.a.size(); ++i) { for (const char* c = morseLetter(l.a[i]); *c; ++c) total += (*c == '-' ? 12.0f : 5.0f) + 3.0f; total += 10.0f; }
            mx = x1 - total + 10.0f;
            for (size_t i = 0; i < l.a.size(); ++i) {
                const char ch[2] = {l.a[i], 0};
                textCentered(dl, f.small, mx + 4, y - 7, kMuted, ch);
                for (const char* c = morseLetter(l.a[i]); *c; ++c) {
                    if (*c == '-') { dl->AddRectFilled(ImVec2(mx, y + 10), ImVec2(mx + 12, y + 14), kInk, 2.0f); mx += 15; }
                    else { dl->AddCircleFilled(ImVec2(mx + 2.5f, y + 12), 2.4f, kInk, 0); mx += 8; }
                }
                mx += 10;
            }
            y += 26;
            break;
        }
        case 4:
            y += 6;
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(255, 255, 255, 24));
            textAt(dl, f.small, x0, y + 6, accentLite(255), l.a.c_str());
            y += 22;
            break;
        case 5:
            if (pill(dl, f, "##cardbtn", l.a.c_str(), ImVec2(x0, y + 6), ImVec2(x1, y + 34), true, true)) pressed = true;
            y += 40;
            break;
        }
    }
    dl->PopClipRect();
    // close
    const ImVec2 xc(e.x - 20, a.y + 20);
    ImGui::SetCursorScreenPos(ImVec2(xc.x - 14, xc.y - 14));
    if (ImGui::InvisibleButton("##cardclose", ImVec2(28, 28))) g_selKind = 0;
    const bool xh = ImGui::IsItemHovered();
    dl->AddCircleFilled(xc, 13.0f, themeTone(14, 26, 48, 255), 0);   // over whatever scrolls under it
    if (xh) dl->AddCircleFilled(xc, 13.0f, IM_COL32(255, 255, 255, 30), 0);
    dl->AddLine(ImVec2(xc.x - 5, xc.y - 5), ImVec2(xc.x + 5, xc.y + 5), kInk, 1.6f);
    dl->AddLine(ImVec2(xc.x - 5, xc.y + 5), ImVec2(xc.x + 5, xc.y - 5), kInk, 1.6f);
    if (full > H) {   // a thin bar shows there is more
        const float bh = H * H / full, by = a.y + (H - bh) * (g_cardScroll / std::max(1.0f, full - H));
        dl->AddRectFilled(ImVec2(e.x - 6, by + 4), ImVec2(e.x - 3, by + bh - 4), IM_COL32(255, 255, 255, 70), 2.0f);
    }
    return pressed;
}

// the runway most into the wind (the most headwind; calm, under 3 kt: the longest), and which way: end 0 lands from A toward B
int bestRunway(const AptGeo& g, float windFrom, float windKt, int& end) {
    int best = -1;
    double bestScore = -1e9;
    end = 0;
    for (size_t i = 0; i < g.runways.size(); ++i) {
        const AptGeo::Runway& r = g.runways[i];
        if (r.water) continue;
        double la1, lo1, la2, lo2;
        latLonOf(r.a, la1, lo1); latLonOf(r.b, la2, lo2);
        const double dy = (la2 - la1) * 60.0, dx = (lo2 - lo1) * 60.0 * std::cos(la1 * kPi / 180.0);
        const double len = std::sqrt(dx * dx + dy * dy), hdg = std::atan2(dx, dy) * 180.0 / kPi;
        for (int e = 0; e < 2; ++e) {
            const double h = e == 0 ? hdg : hdg + 180.0;
            const double head = windKt < 3 ? 0.0 : windKt * std::cos((windFrom - h) * kPi / 180.0);
            const double score = head * 100.0 + len;   // the headwind first, the length to settle it
            if (score > bestScore) { bestScore = score; best = (int)i; end = e; }
        }
    }
    return best;
}
// the sun's height above the horizon (degrees) at a place and moment (seconds since 1970 UTC): the solar declination and hour angle
double sunElevation(double lat, double lon, long long utc) {
    const double days = utc / 86400.0 - 10957.5;   // from J2000
    const double g = std::fmod(357.529 + 0.98560028 * days, 360.0) * kPi / 180.0;
    const double q = std::fmod(280.459 + 0.98564736 * days, 360.0);
    const double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kPi / 180.0;
    const double e = (23.439 - 0.00000036 * days) * kPi / 180.0;
    const double dec = std::asin(std::sin(e) * std::sin(L));
    const double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L)) * 180.0 / kPi;
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * days, 360.0);
    const double ha = (gmst + lon - ra) * kPi / 180.0;
    const double la = lat * kPi / 180.0;
    return std::asin(std::sin(la) * std::sin(dec) + std::cos(la) * std::cos(dec) * std::cos(ha)) * 180.0 / kPi;
}
// traffic shown on the map: within the Traffic menu's range and height of us (any, when not set)
bool trafficShown(const MapState& m, const MapState::Traffic& t) {
    if (g_settings.mapTrafficRangeNm > 0 && m.havePos && distNmLL(m.lat, m.lon, t.lat, t.lon, nullptr) > g_settings.mapTrafficRangeNm) return false;
    if (g_settings.mapTrafficAltFt > 0 && m.havePos && std::fabs(t.altFt - m.altFt) > g_settings.mapTrafficAltFt) return false;
    return true;
}
// each aircraft's recent track (kept from the traffic the host gives, every 5 s, the last 10 minutes)
struct TrafficTrail { std::vector<std::pair<double, double>> pts; double seen = 0, added = -100; };
std::map<std::string, TrafficTrail> g_tfcTrails;
void trackTraffic(const MapState& m) {
    const double now = ImGui::GetTime();
    for (const MapState::Traffic& t : m.traffic) {
        TrafficTrail& tr = g_tfcTrails[t.callsign];
        tr.seen = now;
        if (now - tr.added >= 5.0) {
            tr.added = now;
            tr.pts.push_back({t.lat, t.lon});
            if (tr.pts.size() > 120) tr.pts.erase(tr.pts.begin());
        }
    }
    for (auto it = g_tfcTrails.begin(); it != g_tfcTrails.end();) { if (now - it->second.seen > 120) it = g_tfcTrails.erase(it); else ++it; }
}

void mapPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const MapState& m = s.map;
    const float top = kStatusH + kHeaderH;
    const ImVec2 r0(0, top), r1(w, h);
    const ImVec2 sc((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
    const MercPt ac = m.havePos ? mercOf(m.lat, m.lon) : MercPt{0.5, 0.5};
    // without a position the map starts on the whole world; the first position brings it in to the aircraft
    if (m.havePos && !g_mapSeen) { g_mapSeen = true; g_mapCenter = ac; g_mapFollow = true; g_mapRange = kRangeTaxi; g_mapShownNm = kRangesNm[kRangeTaxi]; }
    if (!m.havePos && !g_mapSeen) { g_mapRange = kRangeN - 1; g_mapShownNm = kRangesNm[kRangeN - 1]; }
    // a new centre (Center On Aircraft, Show on map, a command) is reached in a short glide, never a jump
    static bool wasFollowing = true;
    const double now = ImGui::GetTime();
    if (g_mapGoto) {
        g_mapGoto = false; g_mapFollow = false; g_mapSeen = true; g_mapRange = kRangeTaxi;
        g_mapGlideFrom = g_mapCenter; g_mapGlideTo = mercOf(g_mapGotoLat, g_mapGotoLon); g_mapGlideStart = now;
    }
    if (g_mapFollow && !wasFollowing && m.havePos) { g_mapGlideFrom = g_mapCenter; g_mapGlideStart = now; }
    wasFollowing = g_mapFollow;
    if (g_mapFollow && m.havePos) g_mapGlideTo = ac;
    if (g_mapGlideStart >= 0) {
        const float t = std::min(1.0f, (float)((now - g_mapGlideStart) / 0.55));
        const float e = 1 - (1 - t) * (1 - t) * (1 - t);
        g_mapCenter = MercPt{g_mapGlideFrom.x + (g_mapGlideTo.x - g_mapGlideFrom.x) * e, g_mapGlideFrom.y + (g_mapGlideTo.y - g_mapGlideFrom.y) * e};
        if (t >= 1) g_mapGlideStart = -1;
        g_mapCenter.x -= std::floor(g_mapCenter.x);
    } else if (g_mapFollow && m.havePos) {
        g_mapCenter = ac;
    }
    const bool headingUp = g_settings.mapHeadingUp && m.havePos;
    static double rotShown = 0;   // turns to North or to the heading smoothly (the shorter way round)
    {
        const double want = headingUp ? m.hdgTrue * kPi / 180.0 : 0.0;
        double d = std::remainder(want - rotShown, 2 * kPi);
        rotShown += d * (1.0 - std::exp(-ImGui::GetIO().DeltaTime * 10.0));
        if (std::fabs(d) < 1e-4) rotShown = want;
    }
    const double rot = rotShown;
    const double cr = std::cos(rot), sr = std::sin(rot);
    double cLat = 0, cLon = 0;
    latLonOf(g_mapCenter, cLat, cLon);
    {   // the range shown eases to the one chosen (in log space, WXPlane's rate)
        const double dt = std::max(0.0, std::min(0.25, (double)ImGui::GetIO().DeltaTime));
        g_mapRange = std::max(0, std::min(kRangeN - 1, g_mapRange));
        const double target = kRangesNm[g_mapRange], k = 1.0 - std::exp(-dt * 9.0);
        const double a = std::log(g_mapShownNm), b = std::log(target);
        g_mapShownNm = std::fabs(a - b) < 0.002 ? target : std::exp(a + (b - a) * k);
    }
    // layout units per Mercator unit: the range fills half the map's height. (Past 60 degrees north or south the zoom stops
    // following the latitude: Mercator stretches the map there, and a range true to the nautical mile zoomed the map in and out
    // as it was moved toward a pole. The rings and distances stay true: they use the latitude itself.)
    {
        const double yMax = mercOf(82.0, 0).y, yMin = mercOf(-82.0, 0).y;   // (and no further than 82 degrees: no tiles beyond)
        g_mapCenter.y = std::max(yMax, std::min(yMin, g_mapCenter.y));
        latLonOf(g_mapCenter, cLat, cLon);
    }
    const double mPerMercC = kEarthM * std::max(0.5, std::cos(cLat * kPi / 180.0));
    double scale = (r1.y - r0.y) * 0.5 / (g_mapShownNm * 1852.0 / mPerMercC);
    g_mapZoom = std::log2(scale / 256.0);
    // the symbols' size follows the zoom (as a chart's would): large close in, small far out, and the real size once that is bigger
    const float symLen = std::max(16.0f, std::min(46.0f, 46.0f - 6.0f * (float)std::log2(std::max(0.25, g_mapShownNm) / 0.25)));
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
    const bool satImg = g_settings.mapSource == 1;
    const bool noMap = g_settings.mapSource == 2;    // no map under the layers: plain white, or black with the night colours
    const bool sat = satImg || g_settings.mapDark;   // a dark map under everything: light ink, as on the imagery
    dl->PushClipRect(r0, r1, true);
    dl->AddRectFilled(r0, r1, noMap ? (g_settings.mapDark ? IM_COL32(8, 10, 14, 255) : IM_COL32(250, 250, 250, 255)) : sat ? IM_COL32(18, 24, 30, 255) : IM_COL32(232, 228, 220, 255));

    // ---- input: drag to move, the wheel to zoom (about the pointer); bubbles drawn later sit on top of it
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##map", ImVec2(r1.x - r0.x, r1.y - r0.y));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) g_mapPressPos = io.MousePos;
    const bool mapClick = ImGui::IsItemDeactivated() && std::fabs(io.MousePos.x - g_mapPressPos.x) + std::fabs(io.MousePos.y - g_mapPressPos.y) < 6;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && g_settings.mapRoute && m.route.size() >= 2) {   // the route, tapped twice: its Flight Plan
        const ImVec2 p = io.MousePos;
        float best = 1e9f;
        ImVec2 prev = toS(mercOf(m.route[0].lat, m.route[0].lon));
        for (size_t i = 1; i < m.route.size(); ++i) {
            const ImVec2 q = toS(mercOf(m.route[i].lat, m.route[i].lon));
            const float dx = q.x - prev.x, dy = q.y - prev.y, l2 = dx * dx + dy * dy;
            const float t = l2 > 0 ? std::max(0.0f, std::min(1.0f, ((p.x - prev.x) * dx + (p.y - prev.y) * dy) / l2)) : 0.0f;
            const float ex = prev.x + dx * t - p.x, ey = prev.y + dy * t - p.y;
            best = std::min(best, ex * ex + ey * ey);
            prev = q;
        }
        if (best < 12.0f * 12.0f) openApp(APP_ROUTES);   // (the page goes on drawing this frame)
    }
    if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && ImGui::IsMouseDragging(0, 2.0f)) {
        const double x = -io.MouseDelta.x, y = -io.MouseDelta.y;
        g_mapCenter.x += (x * cr - y * sr) / scale;
        g_mapCenter.y += (x * sr + y * cr) / scale;
        g_mapCenter.x -= std::floor(g_mapCenter.x);
        g_mapCenter.y = std::max(0.0, std::min(1.0, g_mapCenter.y));
        g_mapFollow = false;
        g_mapGlideStart = -1;   // the finger has it
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) g_mapRange = std::max(0, std::min(kRangeN - 1, g_mapRange + (io.MouseWheel > 0 ? -1 : 1)));   // a step a click
    latLonOf(g_mapCenter, cLat, cLon);

    // ---- the tiles: the zoom level that gives about one tile pixel per screen pixel, nearest the middle first
    const int src = satImg ? 1 : g_settings.mapDark ? 2 : 0;
    const double pxPer = std::max(1.0f, s.pixelScale);
    const int tz = std::max(1, std::min(19, (int)std::floor(g_mapZoom + std::log2(pxPer) + 0.3)));
    const int nT = 1 << tz;
    const ImVec2 corners[4] = {r0, ImVec2(r1.x, r0.y), r1, ImVec2(r0.x, r1.y)};
    double mx0 = 1e9, my0 = 1e9, mx1 = -1e9, my1 = -1e9;
    for (const ImVec2& c : corners) { const MercPt p = fromS(c); mx0 = std::min(mx0, p.x); mx1 = std::max(mx1, p.x); my0 = std::min(my0, p.y); my1 = std::max(my1, p.y); }
    const int tx0 = (int)std::floor(mx0 * nT), tx1 = (int)std::floor(mx1 * nT), ty0 = std::max(0, (int)std::floor(my0 * nT)), ty1 = std::min(nT - 1, (int)std::floor(my1 * nT));
    struct TileAt { int x, y; double d; };
    std::vector<TileAt> want;
    if (!noMap && tx1 - tx0 < 40 && ty1 - ty0 < 40)
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

    // ---- the outlines: coastlines, borders, states and provinces (lakes as coast)
    if (g_settings.mapOutlines) {
        double la0, lo0, la1, lo1;
        latLonOf(MercPt{mx0, my1}, la0, lo0);
        latLonOf(MercPt{mx1, my0}, la1, lo1);
        std::vector<ImVec2> pts;
        for (const GeoLine& L : basemapLines()) {
            if (L.lo1 < lo0 || L.lo0 > lo1 || L.la1 < la0 || L.la0 > la1) continue;
            if (L.kind == 2 && g_mapZoom < 4) continue;
            pts.clear();
            for (const ImVec2& q : L.pts) pts.push_back(toS(mercOf(q.y, q.x)));
            const ImU32 col = L.kind == 1 ? (sat ? IM_COL32(255, 255, 255, 220) : IM_COL32(60, 60, 80, 220))
                            : L.kind == 2 ? (sat ? IM_COL32(255, 255, 255, 150) : IM_COL32(90, 90, 110, 170))
                                          : (sat ? IM_COL32(150, 210, 255, 170) : IM_COL32(70, 120, 180, 170));
            dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, L.kind == 1 ? 1.6f : 1.0f);
        }
    }

    // ---- the time zones: dashed boundaries in the theme's accent
    if (g_settings.mapTimeZones) {
        double la0, lo0, la1, lo1;
        latLonOf(MercPt{mx0, my1}, la0, lo0);
        latLonOf(MercPt{mx1, my0}, la1, lo1);
        // the view's box in degrees, whichever corner is which, with a margin (else lines pop in and out at the edges as it zooms)
        if (la0 > la1) std::swap(la0, la1);
        if (lo0 > lo1) std::swap(lo0, lo1);
        const double padLa = (la1 - la0) * 0.1 + 0.5, padLo = (lo1 - lo0) * 0.1 + 0.5;
        la0 -= padLa; la1 += padLa; lo0 -= padLo; lo1 += padLo;
        if (lo1 - lo0 >= 360) { lo0 = -1e9; lo1 = 1e9; }   // the whole world across
        std::vector<ImVec2> pts;
        for (const TzLine& L : tzLines()) {
            if (L.lo1 < lo0 || L.lo0 > lo1 || L.la1 < la0 || L.la0 > la1) continue;
            // near the poles the map stretches the zones' meridian edges into long straight lines: none past 75 degrees (a line
            // that goes there is drawn in its pieces either side)
            auto flush = [&] {
                if (pts.size() >= 2) {
                    if (g_mapZoom < 6) dl->AddPolyline(pts.data(), (int)pts.size(), accentCol(210), 0, 1.4f);   // far out: plain (the theme's accent)
                    else dashed(dl, pts, accentCol(235), 1.7f, 7.0f, 5.0f);
                }
                pts.clear();
            };
            pts.clear();
            for (const ImVec2& q : L.pts) {
                if (std::fabs(q.y) > 75.0f) { flush(); continue; }
                pts.push_back(toS(mercOf(q.y, q.x)));
            }
            flush();
        }
    }

    // ---- day and night: the sun's height across the view at the sim's time, as a shade (none in daylight, deepest past
    // astronomical twilight, -12 degrees), on a grid of the screen so it follows the map however it is turned
    if (g_settings.mapDaylight && m.utc > 0) {
        const int nx = 24, ny = 16;
        float al[(nx + 1) * (ny + 1)];
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i <= nx; ++i) {
                const ImVec2 q(r0.x + (r1.x - r0.x) * i / nx, r0.y + (r1.y - r0.y) * j / ny);
                double la, lo;
                latLonOf(fromS(q), la, lo);
                const double el = sunElevation(la, lo, m.utc);
                al[j * (nx + 1) + i] = (float)std::max(0.0, std::min(1.0, -el / 12.0));
            }
        auto col = [&](float a) { return IM_COL32(6, 10, 34, (int)(165 * a)); };
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const float a00 = al[j * (nx + 1) + i], a10 = al[j * (nx + 1) + i + 1], a01 = al[(j + 1) * (nx + 1) + i], a11 = al[(j + 1) * (nx + 1) + i + 1];
                if (a00 + a10 + a01 + a11 < 0.001f) continue;
                const ImVec2 q0(r0.x + (r1.x - r0.x) * i / nx, r0.y + (r1.y - r0.y) * j / ny), q1(r0.x + (r1.x - r0.x) * (i + 1) / nx, r0.y + (r1.y - r0.y) * (j + 1) / ny);
                dl->AddRectFilledMultiColor(q0, q1, col(a00), col(a10), col(a11), col(a01));
            }
    }

    // ---- X-Plane's airports
    const double mPerMerc = kEarthM * std::cos(cLat * kPi / 180.0);
    const double halfDiagNm = std::sqrt((double)(w * w + (h - top) * (h - top))) * 0.5 / scale * mPerMerc / 1852.0;
    if (g_settings.mapAirports) {
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const bool layouts = g_mapZoom >= 11.5;
        const bool close = g_mapShownNm < 1.9;      // under 2 nm: the whole layout, taxiways and their names, stands
        const bool closest = g_mapShownNm < 0.75;   // 0.5 nm and in: the stands' names too
        std::vector<LRect> twy;                     // the taxiway names placed (none drawn over another)
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
                // the runways: dark, with a dashed white centreline and the numbers at each end; the one most into the wind outlined green
                int bestEnd = 0;
                const int best = g_settings.mapBestRunway && s.wx.valid ? bestRunway(g, s.wx.windDir, s.wx.windKt, bestEnd) : -1;
                for (size_t ri = 0; ri < g.runways.size(); ++ri) {
                    const AptGeo::Runway& rw = g.runways[ri];
                    const ImVec2 a = toS(rw.a), b = toS(rw.b);
                    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    if (len < 1) continue;
                    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len), nrm(-d.y, d.x);
                    const float hw = std::max(1.5f, rw.widthM * 0.5f * pxPerM);
                    const ImU32 rc = rw.water ? IM_COL32(60, 120, 200, 160) : IM_COL32(44, 48, 56, 255);
                    dl->AddQuadFilled(ImVec2(a.x + nrm.x * hw, a.y + nrm.y * hw), ImVec2(b.x + nrm.x * hw, b.y + nrm.y * hw),
                                      ImVec2(b.x - nrm.x * hw, b.y - nrm.y * hw), ImVec2(a.x - nrm.x * hw, a.y - nrm.y * hw), rc);
                    if (hw > 4 && !rw.water) dashed(dl, {a, b}, IM_COL32(255, 255, 255, 220), std::max(1.0f, hw * 0.08f), 30.0f * pxPerM, 20.0f * pxPerM);
                    if ((int)ri == best) {
                        const float ow = hw + 2.5f;
                        const ImVec2 q[4] = {ImVec2(a.x + nrm.x * ow - d.x * 3, a.y + nrm.y * ow - d.y * 3), ImVec2(b.x + nrm.x * ow + d.x * 3, b.y + nrm.y * ow + d.y * 3),
                                             ImVec2(b.x - nrm.x * ow + d.x * 3, b.y - nrm.y * ow + d.y * 3), ImVec2(a.x - nrm.x * ow - d.x * 3, a.y - nrm.y * ow - d.y * 3)};
                        dl->AddPolyline(q, 4, IM_COL32(16, 20, 30, 160), ImDrawFlags_Closed, 4.5f);
                        dl->AddPolyline(q, 4, IM_COL32(60, 220, 110, 255), ImDrawFlags_Closed, 2.5f);
                        if (g_mapZoom >= 12.5) {   // which way: a chip at the end it is landed from
                            const ImVec2 th = bestEnd == 0 ? a : b;
                            const ImVec2 out = bestEnd == 0 ? ImVec2(-d.x, -d.y) : d;
                            char bt[24];
                            std::snprintf(bt, sizeof bt, "Wind %s", (bestEnd == 0 ? rw.idA : rw.idB).c_str());
                            const ImVec2 ts = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, bt);
                            const ImVec2 c(th.x + out.x * 34, th.y + out.y * 34);
                            dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 5, c.y - ts.y * 0.5f - 2), ImVec2(c.x + ts.x * 0.5f + 5, c.y + ts.y * 0.5f + 2), IM_COL32(30, 140, 70, 235), 4.0f);
                            dl->AddText(f.small, fontPx(f.small), ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), bt);
                        }
                    }
                    if (g_mapZoom >= 13) {
                        textCentered(dl, f.bold, a.x - d.x * 14, a.y - d.y * 14 - fontPx(f.bold) * 0.5f, sat ? kInk : IM_COL32(20, 24, 32, 255), rw.idA.c_str());
                        textCentered(dl, f.bold, b.x + d.x * 14, b.y + d.y * 14 - fontPx(f.bold) * 0.5f, sat ? kInk : IM_COL32(20, 24, 32, 255), rw.idB.c_str());
                    }
                }
                // the painted lines
                std::vector<ImVec2> pts;
                for (const AptGeo::Line& L : g.lines) {
                    if (L.kind == 3 && !close) continue;
                    pts.clear();
                    for (const MercPt& p : L.pts) pts.push_back(toS(p));
                    const ImU32 col = L.kind == 1 ? IM_COL32(236, 64, 72, 255) : L.kind == 2 ? IM_COL32(255, 168, 60, 255)
                                    : L.kind == 3 ? IM_COL32(250, 214, 60, 150) : IM_COL32(250, 210, 40, 235);
                    const float th = L.kind == 1 ? 2.4f : L.kind == 2 ? 1.8f : L.kind == 3 ? 0.8f : (g_mapZoom >= 15 ? 1.4f : 1.0f);
                    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, th);
                }
                // the stands and the taxiway names, close in
                if (close && g_settings.mapStands)
                    for (size_t si = 0; si < g.stands.size(); ++si) {
                        const AptGeo::Stand& st = g.stands[si];
                        const ImVec2 q = toS(st.p);
                        if (q.x < r0.x - 40 || q.x > r1.x + 40 || q.y < r0.y - 20 || q.y > r1.y + 20) continue;
                        const bool picked = g_selKind == 6 && g_selKey == g.icao + "|" + std::to_string(si);
                        if (picked) dl->AddCircle(q, 8.0f, accentLite(255), 0, 2.2f);
                        dl->AddCircleFilled(q, picked ? 4.0f : 3.0f, picked ? accentLite(255) : IM_COL32(255, 255, 255, 235), 0);
                        dl->AddCircle(q, 3.0f, IM_COL32(20, 24, 32, 200), 0, 1.0f);
                        if (closest && !st.name.empty()) textAt(dl, f.small, q.x + 5, q.y - fontPx(f.small) * 0.5f, sat ? kInk : IM_COL32(30, 36, 48, 255), st.name.c_str());
                    }
                if (close)
                    for (const AptGeo::Label& lb : g.taxiways) {
                        const ImVec2 q = toS(lb.p);
                        if (q.x < r0.x || q.x > r1.x || q.y < r0.y || q.y > r1.y) continue;
                        const ImVec2 ts = f.bold->CalcTextSizeA(fontPx(f.small), 1e9f, 0, lb.text.c_str());
                        const LRect R{q.x - ts.x * 0.5f - 6, q.y - ts.y * 0.5f - 3, q.x + ts.x * 0.5f + 6, q.y + ts.y * 0.5f + 3};
                        bool clash = false;
                        for (const LRect& o : twy) if (R.hits(o)) { clash = true; break; }
                        if (clash) continue;
                        twy.push_back(R);
                        g_symbols.push_back(R);   // the map's other labels keep off it
                        dl->AddRectFilled(ImVec2(q.x - ts.x * 0.5f - 4, q.y - ts.y * 0.5f - 1), ImVec2(q.x + ts.x * 0.5f + 4, q.y + ts.y * 0.5f + 1), IM_COL32(250, 210, 40, 255), 3.0f);
                        dl->AddText(f.bold, fontPx(f.small), ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), IM_COL32(16, 16, 16, 255), lb.text.c_str());
                    }
            }
        }
        // further out: each airport as a dot with its code; the further out, only the bigger ones (by the longest runway):
        // 8000 ft and over from the start, 5000 ft from zoom 8, every airport (heliports and seaplane bases too) from 9
        const float minFt = g_mapZoom >= 9.0 ? 0.0f : g_mapZoom >= 8.0 ? 5000.0f : 8000.0f;
        act.mapPointMinFt = minFt;
        if (g_mapZoom >= 7.0) {
            for (const AptPoint& p : m.airportPoints) {
                if (layouts && std::find(drawn.begin(), drawn.end(), p.icao) != drawn.end()) continue;
                if (p.rwyFt < minFt && !(g_selKind == 4 && g_selKey == p.icao)) continue;
                const ImVec2 q = toS(mercOf(p.lat, p.lon));
                if (q.x < r0.x - 20 || q.x > r1.x + 20 || q.y < r0.y - 10 || q.y > r1.y + 10) continue;
                const ImU32 dot = p.kind == 17 ? IM_COL32(180, 90, 220, 255) : p.kind == 16 ? IM_COL32(40, 140, 220, 255) : IM_COL32(32, 92, 200, 255);
                const bool picked = g_selKind == 4 && g_selKey == p.icao;   // its card open: the theme's colour, a ring round it
                if (picked) dl->AddCircle(q, 7.5f, accentLite(255), 0, 2.0f);
                dl->AddCircleFilled(q, 4.0f, IM_COL32(255, 255, 255, 230), 0);
                dl->AddCircleFilled(q, 2.8f, picked ? accentCol(255) : dot, 0);
                symbolAt(q, 4.5f);
                if (g_mapZoom >= 8.5 || picked) g_labels.push_back({2, q, 6, p.icao, "", picked ? accentLite(255) : sat ? kInk : IM_COL32(24, 40, 80, 255), 0, picked ? IM_COL32(10, 16, 28, 190) : 0});
            }
        }
        act.mapLayouts = layouts;
        act.mapLayoutNm = std::min(25.0, std::max(3.0, halfDiagNm * 1.4));
        act.mapPointNm = g_mapZoom >= 7.0 ? std::min(400.0, halfDiagNm * 1.2) : 0.0;
    }
    act.mapShown = true;
    act.mapLat = cLat; act.mapLon = cLon;
    act.mapNavNm = ((g_settings.mapNavaids || g_settings.mapIls) && g_mapZoom >= 6.0) ? std::min(300.0, halfDiagNm * 1.3) : 0.0;
    act.mapFixNm = (g_settings.mapWaypoints && g_mapZoom >= 9.0) ? std::min(60.0, halfDiagNm * 1.3) : 0.0;
    // what AviTab's map datarefs and commands use
    g_mapLastW = r1.x - r0.x; g_mapLastH = r1.y - r0.y; g_mapLastRot = rot; g_mapLastScale = scale; g_mapLastMPerMerc = mPerMerc;

    // ---- the radio navaids: VOR (a hexagon, with a ring when close), NDB (a dotted ring), DME (a square), localizers (feathers)
    // the layers that are switched on and off (navaids, ILS, fixes, rings) are white on every map, with a thin dark edge
    // under them so they read on the light street map as on the imagery; their names white on dark chips
    const ImU32 navInk = IM_COL32(255, 255, 255, 255), ndbInk = IM_COL32(255, 255, 255, 255);
    const ImU32 haloInk = IM_COL32(16, 20, 30, 150), chipBg = IM_COL32(10, 16, 28, 190);
    if ((g_settings.mapNavaids || g_settings.mapIls) && g_mapZoom >= 6.0) {
        const double nmPx = scale / (mPerMerc / 1852.0);   // layout units per nautical mile
        for (const MapState::Navaid& n : m.navaids) {
            const ImVec2 q = toS(mercOf(n.lat, n.lon));
            if (n.type == 4 || n.type == 5) {   // the localizer's course, 10 nm out from the antenna past the runway
                if (!g_settings.mapIls || g_mapZoom < 9.5) continue;
                const double a = (n.bearing + 180.0) * kPi / 180.0 - rot;
                const float L = (float)(10.0 * nmPx), half = (float)(std::tan(2.5 * kPi / 180.0) * 10.0 * nmPx);
                const ImVec2 d((float)std::sin(a), (float)-std::cos(a)), nn(-d.y, d.x);
                const ImVec2 tip(q.x + d.x * L, q.y + d.y * L);
                const ImVec2 l(tip.x + nn.x * half, tip.y + nn.y * half), r(tip.x - nn.x * half, tip.y - nn.y * half);
                const bool picked = g_selKind == 2 && g_selKey == n.ident + "|" + std::to_string(n.type);   // its card open: the theme's colour
                if (picked) {   // the triangle filled faintly, so the chosen course stands out
                    dl->AddTriangleFilled(q, l, r, accentCol(45));
                }
                for (int pass = 0; pass < 2; ++pass) {   // the dark edge, then the white
                    const ImU32 c = picked ? (pass ? accentLite(255) : accentCol(140)) : pass ? IM_COL32(255, 255, 255, 230) : haloInk;
                    const float t = pass ? 0.0f : 2.0f;
                    dl->AddLine(q, l, c, 1.2f + t);
                    dl->AddLine(q, r, c, 1.2f + t);
                    dl->AddLine(q, tip, pass ? IM_COL32(255, 255, 255, 170) : haloInk, 1.0f + t);
                }
                if (g_mapZoom >= 11 || picked) g_labels.push_back({1, tip, 4, n.ident, "", picked ? accentLite(255) : IM_COL32(255, 255, 255, 255), 0, chipBg});
                continue;
            }
            if (!g_settings.mapNavaids) continue;   // (ILS only)
            if (q.x < r0.x - 40 || q.x > r1.x + 40 || q.y < r0.y - 40 || q.y > r1.y + 40) continue;
            // the one whose card is open: in the theme's colour (a wider edge of it under it)
            const bool picked = g_selKind == 2 && g_selKey == n.ident + "|" + std::to_string(n.type);
            for (int pass = 0; pass < 2; ++pass) {   // the dark edge (wider), then the white
            const ImU32 ink = picked ? (pass ? accentLite(255) : accentCol(150)) : pass ? (n.type == 2 ? ndbInk : navInk) : haloInk;
            const float tk = pass ? 0.0f : 2.0f;
            if (n.type == 3) {
                ImVec2 hx[6];
                for (int k = 0; k < 6; ++k) hx[k] = ImVec2(q.x + 7.0f * (float)std::cos(k * kPi / 3), q.y + 7.0f * (float)std::sin(k * kPi / 3));
                dl->AddPolyline(hx, 6, ink, ImDrawFlags_Closed, 1.6f + tk);
                dl->AddCircleFilled(q, 1.8f + tk * 0.5f, ink, 0);
                if (g_mapZoom >= 9) {   // a compass rose, turned to magnetic north (the VOR's own variation)
                    const ImU32 rc = IM_COL32((ink >> IM_COL32_R_SHIFT) & 0xFF, (ink >> IM_COL32_G_SHIFT) & 0xFF, (ink >> IM_COL32_B_SHIFT) & 0xFF, pass ? 150 : 90);
                    const float R = 42.0f;
                    dl->AddCircle(q, R, rc, 0, 1.0f + tk * 0.6f);
                    for (int k = 0; k < 36; ++k) {
                        const double a = (k * 10.0 + n.bearing) * kPi / 180.0 - rot;
                        const float len = k % 9 == 0 ? 9.0f : k % 3 == 0 ? 6.0f : 3.5f;
                        const ImVec2 d((float)std::sin(a), (float)-std::cos(a));
                        dl->AddLine(ImVec2(q.x + d.x * R, q.y + d.y * R), ImVec2(q.x + d.x * (R - len), q.y + d.y * (R - len)), rc, (k == 0 ? 2.0f : 1.0f) + tk * 0.6f);
                    }
                }
            } else if (n.type == 2) {
                for (int k = 0; k < 12; ++k) dl->AddCircleFilled(ImVec2(q.x + 7.0f * (float)std::cos(k * kPi / 6), q.y + 7.0f * (float)std::sin(k * kPi / 6)), 1.1f + tk * 0.5f, ink, 0);
                dl->AddCircleFilled(q, 2.2f + tk * 0.5f, ink, 0);
            } else {
                dl->AddRect(ImVec2(q.x - 5, q.y - 5), ImVec2(q.x + 5, q.y + 5), ink, 0.0f, 0, 1.5f + tk);
            }
            }
            if (g_mapZoom >= 7.5) {
                char b[48];
                if (n.type == 2) std::snprintf(b, sizeof b, "%s %.0f", n.ident.c_str(), n.freq);
                else if (n.type == 3) std::snprintf(b, sizeof b, "%s %.2f", n.ident.c_str(), n.freq);
                else std::snprintf(b, sizeof b, "%s", n.ident.c_str());
                g_labels.push_back({3, q, 10, b, "", picked ? accentLite(255) : navInk, 0, chipBg});
            }
            symbolAt(q, n.type == 3 && g_mapZoom >= 9 ? 9.0f : 7.0f);
        }
    }
    // ---- fixes: small triangles (terminal ones only close in)
    if (g_settings.mapWaypoints && g_mapZoom >= 9.0) {
        for (const MapState::Fix& fx : m.fixes) {
            if (!fx.enroute && g_mapZoom < 11) continue;
            const ImVec2 q = toS(mercOf(fx.lat, fx.lon));
            if (q.x < r0.x - 30 || q.x > r1.x + 30 || q.y < r0.y - 20 || q.y > r1.y + 20) continue;
            const bool picked = g_selKind == 3 && g_selKey == fx.ident;   // its card open: the theme's colour
            const ImU32 ink = picked ? accentLite(255) : IM_COL32(255, 255, 255, 240);   // white, with a dark edge (as the navaids)
            if (picked) dl->AddTriangleFilled(ImVec2(q.x, q.y - 4.5f), ImVec2(q.x + 4, q.y + 3), ImVec2(q.x - 4, q.y + 3), accentCol(110));
            dl->AddTriangle(ImVec2(q.x, q.y - 4.5f), ImVec2(q.x + 4, q.y + 3), ImVec2(q.x - 4, q.y + 3), picked ? accentCol(150) : IM_COL32(16, 20, 30, 150), 3.0f);
            dl->AddTriangle(ImVec2(q.x, q.y - 4.5f), ImVec2(q.x + 4, q.y + 3), ImVec2(q.x - 4, q.y + 3), ink, 1.2f);
            symbolAt(q, 4.5f);
            if (g_mapZoom >= 10.5) g_labels.push_back({0, q, 6, fx.ident, "", ink, 0, IM_COL32(10, 16, 28, 190)});
        }
    }
    // ---- the trail flown (Display settings): a solid line in a darker, quieter tone of the accent than the route, under it
    if (g_settings.mapTrail && g_crumbs.size() >= 2) {
        const ImU32 trail = mixCol(mixCol(accentCol(255), IM_COL32(120, 124, 134, 255), 0.5f), IM_COL32(0, 0, 0, 255), 0.25f);
        std::vector<ImVec2> pts;
        auto flushTrail = [&] {
            if (pts.size() >= 2) {   // solid: a faint light edge, then the line
                dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 80), 0, 4.4f);
                dl->AddPolyline(pts.data(), (int)pts.size(), trail, 0, 2.6f);
            }
            pts.clear();
        };
        for (const Crumb& c : g_crumbs) {
            if (c.start) flushTrail();
            pts.push_back(toS(mercOf(c.lat, c.lon)));
        }
        if (m.havePos) pts.push_back(toS(ac));   // up to the aircraft
        flushTrail();
    }
    // ---- the SimBrief flight plan: a line in the theme's accent through its fixes
    if (g_settings.mapRoute && m.route.size() >= 2) {
        std::vector<ImVec2> pts;
        for (const MapState::RoutePoint& rp : m.route) pts.push_back(toS(mercOf(rp.lat, rp.lon)));
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(20, 10, 30, 120), 0, 5.0f);
        dl->AddPolyline(pts.data(), (int)pts.size(), routeCol(255), 0, 2.6f);
        for (size_t i = 0; i < pts.size(); ++i) {
            const ImVec2 q = pts[i];
            if (q.x < r0.x - 20 || q.x > r1.x + 20 || q.y < r0.y - 20 || q.y > r1.y + 20) continue;
            const bool picked = g_selKind == 5 && g_selKey == std::to_string(i);   // its card open: filled, and a ring
            if (picked) dl->AddCircle(q, 7.0f, accentLite(255), 0, 2.0f);
            dl->AddCircleFilled(q, picked ? 4.0f : 3.2f, picked ? accentLite(255) : IM_COL32(255, 255, 255, 255), 0);
            dl->AddCircle(q, picked ? 4.0f : 3.2f, routeCol(255), 0, 1.5f);
            if (g_mapZoom >= 6.5 && (g_mapZoom >= 8.5 || i == 0 || i + 1 == pts.size())) {
                g_labels.push_back({4, q, 6, m.route[i].ident, "", IM_COL32(255, 255, 255, 255), 0, routeCol(230)});
            }
            symbolAt(q, 4.0f);
        }
    }

    // ---- other aircraft: a chevron along its heading, its call sign and its height against ours (hundreds of feet, TCAS style)
    // the selected VATSIM aircraft's filed route: a strong dashed line, its fixes as small rings and their names
    if (g_settings.mapTraffic && g_settings.mapTrafficRoute && g_selKind == 1 && m.trafficRoute.size() >= 2) {
        std::vector<ImVec2> tp;
        for (const MapState::RoutePoint& rp : m.trafficRoute) tp.push_back(toS(mercOf(rp.lat, rp.lon)));
        dl->AddPolyline(tp.data(), (int)tp.size(), IM_COL32(16, 20, 30, 170), 0, 4.5f);
        dashed(dl, tp, IM_COL32(120, 230, 255, 245), 2.4f, 12.0f, 6.0f);
        for (size_t i = 0; i < tp.size(); ++i) {
            dl->AddCircle(tp[i], 3.5f, IM_COL32(120, 230, 255, 255), 0, 1.6f);
            if (g_mapZoom >= 7.5 || i == 0 || i + 1 == tp.size()) g_labels.push_back({3, tp[i], 6, m.trafficRoute[i].ident, "", IM_COL32(255, 255, 255, 255), 0, IM_COL32(10, 60, 80, 220)});
        }
    }
    if (g_settings.mapTraffic) {
        if (g_settings.mapTrafficTrails)   // their trails first, under every aircraft: thin, in white over a dark edge
            for (const MapState::Traffic& t : m.traffic) {
                if (!trafficShown(m, t)) continue;
                auto it = g_tfcTrails.find(t.callsign);
                if (it == g_tfcTrails.end() || it->second.pts.size() < 2) continue;
                std::vector<ImVec2> tp;
                for (const auto& ll : it->second.pts) tp.push_back(toS(mercOf(ll.first, ll.second)));
                tp.push_back(toS(mercOf(t.lat, t.lon)));
                dl->AddPolyline(tp.data(), (int)tp.size(), IM_COL32(16, 20, 30, 110), 0, 3.0f);
                dl->AddPolyline(tp.data(), (int)tp.size(), IM_COL32(255, 255, 255, 150), 0, 1.3f);
            }
        for (const MapState::Traffic& t : m.traffic) {
            if (!trafficShown(m, t) && !(g_selKind == 1 && g_selKey == t.callsign)) continue;
            const ImVec2 q = toS(mercOf(t.lat, t.lon));
            if (q.x < r0.x - 30 || q.x > r1.x + 30 || q.y < r0.y - 30 || q.y > r1.y + 30) continue;
            if (g_selKind == 1 && g_selKey == t.callsign) {   // its card open: a ring in the theme's colour round it
                dl->AddCircleFilled(q, 15.0f, accentCol(40), 0);
                dl->AddCircle(q, 15.0f, accentLite(255), 0, 2.0f);
            }
            const double a = t.hdg * kPi / 180.0 - rot;
            const float sa = (float)std::sin(a), ca = (float)std::cos(a);
            auto P = [&](float x, float y) { return ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca); };
            const int rel = (int)std::lround((t.altFt - m.altFt) / 100.0f);
            // above: cyan, within 1000 ft: red, below: green; on the ground: grey
            ImU32 col = t.onGround ? IM_COL32(200, 206, 216, 255) : std::abs(rel) <= 10 ? IM_COL32(255, 70, 70, 255) : rel > 0 ? IM_COL32(60, 220, 255, 255) : IM_COL32(110, 230, 140, 255);
            const int level = tcasLevel(m, t);
            const bool ra = level == 2 || runwayConflict(m, t);
            if (level == 1 && !ra) {   // a traffic advisory: a red glow, breathing once a second
                const float k = 0.5f + 0.5f * (float)std::cos(ImGui::GetTime() * 2.0 * kPi);
                col = IM_COL32(255, 60, 60, 255);
                dl->AddCircleFilled(q, 13.0f + 4.0f * k, IM_COL32(255, 30, 30, (int)(30 + 40 * k)), 0);
            }
            if (ra) {   // a resolution advisory (or a runway collision): red, pulsing twice a second, with a ring spreading out
                const float ph = (float)std::fmod(ImGui::GetTime() * 2.0, 1.0);
                const float k = 0.5f + 0.5f * std::cos(ph * 2.0f * (float)kPi);
                col = IM_COL32(255, (int)(40 + 70 * (1 - k)), (int)(40 + 70 * (1 - k)), 255);
                dl->AddCircleFilled(q, 14.0f + 6.0f * k, IM_COL32(255, 30, 30, (int)(40 + 50 * k)), 0);
                dl->AddCircle(q, 12.0f + 18.0f * ph, IM_COL32(255, 40, 40, (int)(220 * (1 - ph))), 0, 2.0f);
            }
            float tl = 0, ts = 0;
            const Shape* tsh = t.type.empty() ? nullptr : findShape(t.type, tl, ts);
            if (tsh) {   // its type's outline, 24 units long (or its real size, close in)
                const float pm = std::max((float)(scale / mPerMerc), std::max(12.0f, symLen * 0.66f) / std::max(1.0f, tl));   // with the zoom too
                const float kx = ts / tsh->span * pm, ky = tl / tsh->len * pm;
                std::vector<ImVec2> pts;
                auto place = [&](const std::vector<ImVec2>& pp) {
                    pts.resize(pp.size());
                    for (size_t i = 0; i < pp.size(); ++i) { const float x = pp[i].x * kx, y = (pp[i].y - tsh->len * 0.5f) * ky; pts[i] = ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca); }
                };
                for (const auto& pp : tsh->fills) { place(pp); if (pts.size() >= 3) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(10, 16, 28, 200), ImDrawFlags_Closed, 2.0f); }
                for (const auto& pp : tsh->fills) { place(pp); fillConcave(dl, pts, col); }
            } else {
                fillConcave(dl, {P(0, -9), P(6, 7), P(0, 3), P(-6, 7)}, IM_COL32(10, 16, 28, 200));   // a dark edge under it
                fillConcave(dl, {P(0, -7.5f), P(4.6f, 5.4f), P(0, 2.2f), P(-4.6f, 5.4f)}, col);
            }
            if (g_selKind == 1 && g_selKey == t.callsign) dl->AddCircle(q, 16.0f, accentLite(255), 0, 2.0f);   // its card is open
            if (g_mapZoom >= 8) {
                char b[48];
                if (t.onGround) std::snprintf(b, sizeof b, "%s", t.callsign.c_str());
                else std::snprintf(b, sizeof b, "%s %c%02d%s", t.callsign.c_str(), rel >= 0 ? '+' : '-', std::min(99, std::abs(rel)), t.vsFpm > 500 ? "\xE2\x86\x91" : t.vsFpm < -500 ? "\xE2\x86\x93" : "");
                g_labels.push_back({ra ? 6 : 5, q, 13, b, g_mapZoom >= 10 ? t.type : std::string(), ra ? IM_COL32(220, 20, 30, 255) : sat ? kInk : IM_COL32(20, 30, 50, 255), sat ? kMuted : IM_COL32(70, 80, 100, 255),
                                    sat ? IM_COL32(10, 16, 28, 150) : IM_COL32(255, 255, 255, 170)});
            }
            symbolAt(q, 11.0f);
        }
    }

    // ---- range rings round the aircraft, as on WXPlane's map: a quarter, half and all of the range, labelled at the lower right
    if (m.havePos && g_settings.mapRings) {
        const ImVec2 q = toS(ac);
        const double nice = kRangesNm[std::max(0, std::min(kRangeN - 1, g_mapRange))];
        const float nmPx = (float)(scale / (mPerMerc / 1852.0));
        // white on every map, a faint dark edge under it; the figures white with a dark shadow
        for (float fr : {0.25f, 0.5f, 1.0f}) {
            const float rr = (float)(nice * fr) * nmPx;
            if (rr < 12) continue;
            dl->AddCircle(q, rr, IM_COL32(16, 20, 30, 90), 96, 3.0f);
            dl->AddCircle(q, rr, IM_COL32(255, 255, 255, 200), 96, 1.2f);
            char b[24];
            const double vnm = nice * fr;   // close in (under 0.1 nm), in feet to the nearest 10: "380 ft", not "0.0625"
            if (vnm < 0.1) std::snprintf(b, sizeof b, "%.0f ft", std::round(vnm * 6076.12 / 10.0) * 10.0);
            else std::snprintf(b, sizeof b, "%g", vnm);
            const ImVec2 tp(q.x + rr * 0.707f + 3, q.y + rr * 0.707f - 2);
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) if (dx || dy) dl->AddText(f.small, fontPx(f.small), ImVec2(tp.x + dx, tp.y + dy), IM_COL32(16, 20, 30, 170), b);
            dl->AddText(f.small, fontPx(f.small), tp, IM_COL32(255, 255, 255, 255), b);
        }
    }
    // ---- the aircraft: its own outline at its real size when that is bigger than the symbol, else the symbol
    if (m.havePos) {
        const ImVec2 q = toS(ac);
        const double a = (m.hdgTrue * kPi / 180.0) - rot;   // on screen, clockwise from up
        const float sa = (float)std::sin(a), ca = (float)std::cos(a);
        float sl = 0, ss = 0;
        const Shape* sh = findShape(s.acfIcao, sl, ss);
        const float pxPerM = (float)(scale / mPerMerc);
        if (sh) {
            // its own type's outline: at its real size when that is bigger than a symbol, else 36 units long (nose at y = 0, tail +y),
            // turned along the heading about its middle
            const float pm = std::max(pxPerM, symLen / std::max(1.0f, sl));
            const float kx = ss / sh->span * pm, ky = sl / sh->len * pm;
            std::vector<ImVec2> pts;
            auto place = [&](const std::vector<ImVec2>& p) {
                pts.resize(p.size());
                for (size_t i = 0; i < p.size(); ++i) {
                    const float x = p[i].x * kx, y = (p[i].y - sh->len * 0.5f) * ky;   // about the middle, +y towards the tail
                    pts[i] = ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca);
                }
            };
            // ours, in the theme's accent with a white rim over a dark one, so it never reads as one of the (yellow) traffic
            for (const auto& p : sh->fills) { place(p); if (pts.size() >= 3) { dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(20, 24, 32, 200), ImDrawFlags_Closed, 4.0f); } }   // a dark rim
            for (const auto& p : sh->fills) { place(p); if (pts.size() >= 3) { dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 2.0f); } }   // a white one
            for (const auto& p : sh->fills) { place(p); fillConcave(dl, pts, accentCol(255)); }
            for (const auto& p : sh->lines) { place(p); if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), mixCol(accentCol(255), IM_COL32(0, 0, 0, 255), 0.45f), 0, 1.0f); }
        } else {
            const float k = symLen / 26.0f;   // the stand-in is 26 units long
            auto P = [&](float x, float y) { x *= k; y *= k; return ImVec2(q.x + x * ca - y * sa, q.y + x * sa + y * ca); };   // y: forward is -y
            const ImVec2 body[] = {P(0, -13), P(2.2f, -9), P(2.2f, -3), P(12, 3), P(12, 5.5f), P(2.2f, 2.5f), P(1.8f, 9), P(5, 11.5f), P(5, 13),
                                   P(0, 11.5f), P(-5, 13), P(-5, 11.5f), P(-1.8f, 9), P(-2.2f, 2.5f), P(-12, 5.5f), P(-12, 3), P(-2.2f, -3), P(-2.2f, -9)};
            const std::vector<ImVec2> v(body, body + 18);
            dl->AddPolyline(v.data(), (int)v.size(), IM_COL32(20, 24, 32, 200), ImDrawFlags_Closed, 3.6f);
            dl->AddPolyline(v.data(), (int)v.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 1.8f);
            fillConcave(dl, v, accentCol(255));   // ours: the theme's accent, white-rimmed (never like the yellow traffic)
        }
    }

    symbolAt(sc, 18.0f);   // our aircraft (labels keep off it)
    placeLabels(dl, f, r0, r1);
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
            // the place, as WXPlane's map: the aircraft while the map is on it, else the + in the middle (the time changes as the +
            // crosses a zone's line)
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
        const double rnm = kRangesNm[g_mapRange];
        std::snprintf(rng, sizeof rng, "%g nm", rnm);   // (0.05, 0.75, 1.5, 7.5 ... as they are)
        float x = r0.x + 10;
        if (mapBubble(dl, f, "##mzout", ImVec2(x, y), nullptr, "-", false, 34)) g_mapRange = std::min(kRangeN - 1, g_mapRange + 1);
        x += 34 + 4;
        const float rw = bubbleW(f, nullptr, "2500 nm");
        mapBubble(dl, f, "##mrange", ImVec2(x, y), nullptr, rng, false, rw);
        x += rw + 4;
        if (mapBubble(dl, f, "##mzin", ImVec2(x, y), nullptr, "+", false, 34)) g_mapRange = std::max(0, g_mapRange - 1);
        x += 34 + 12;
        if (mapBubble(dl, f, "##mosm", ImVec2(x, y), nullptr, "Map", g_settings.mapSource == 0)) { g_settings.mapSource = 0; g_settingsChanged = true; }
        x += bubbleW(f, nullptr, "Map") + 4;
        if (mapBubble(dl, f, "##msat", ImVec2(x, y), nullptr, "Satellite", satImg)) { g_settings.mapSource = 1; g_settingsChanged = true; }
        x += bubbleW(f, nullptr, "Satellite") + 4;
        if (mapBubble(dl, f, "##mnone", ImVec2(x, y), nullptr, "No Map", noMap)) { g_settings.mapSource = 2; g_settingsChanged = true; }
        // the layers, down the left under the time: three bubbles, each opening its menu beside it (Navigation, Traffic, Features)
        struct Item { const char* id; const char* name; bool* v; bool enabled; };
        const Item nav[] = {{"##mapt", "Airports And Taxiways", &g_settings.mapAirports, true}, {"##mstand", "Parking Spaces", &g_settings.mapStands, g_settings.mapAirports},
                            {"##mbest", "Best Runway (Wind)", &g_settings.mapBestRunway, g_settings.mapAirports}, {"##mils", "ILS", &g_settings.mapIls, true},
                            {"##mnav", "Navaids", &g_settings.mapNavaids, true}, {"##mfix", "Waypoints (GPS)", &g_settings.mapWaypoints, true},
                            {"##mrte", "Flight Plan Route", &g_settings.mapRoute, !m.route.empty()}, {"##mring", "Range Rings", &g_settings.mapRings, true}};
        const Item tfc[] = {{"##mtraf", "Show Traffic", &g_settings.mapTraffic, true}, {"##mttrl", "Traffic Trails", &g_settings.mapTrafficTrails, g_settings.mapTraffic},
                            {"##mtrte", "Filed Route When Selected", &g_settings.mapTrafficRoute, g_settings.mapTraffic}};
        const Item fea[] = {{"##mtrail", "My Breadcrumbs", &g_settings.mapTrail, true}, {"##mout", "Coastlines And Borders", &g_settings.mapOutlines, true},
                            {"##mtz", "Time Zones", &g_settings.mapTimeZones, true}, {"##mday", "Daylight", &g_settings.mapDaylight, true}};
        const char* cats[3] = {"Navigation", "Traffic", "Features"};
        float cw = 0;
        for (const char* c : cats) cw = std::max(cw, bubbleW(f, nullptr, c));
        for (int i = 0; i < 3; ++i) {
            char id[16];
            std::snprintf(id, sizeof id, "##mcat%d", i);
            if (mapBubble(dl, f, id, ImVec2(r0.x + 10, r0.y + 46 + i * 36), nullptr, cats[i], g_mapMenu == i + 1, cw)) { g_mapMenu = g_mapMenu == i + 1 ? 0 : i + 1; if (g_mapMenu) { g_selKind = 0; g_selKey.clear(); } }   // (a menu and a card: one at a time)
        }
        if (g_mapMenu) {   // the open menu: All / Clear, then its rows
            const Item* items = g_mapMenu == 1 ? nav : g_mapMenu == 2 ? tfc : fea;
            const int n = g_mapMenu == 1 ? (int)(sizeof nav / sizeof nav[0]) : g_mapMenu == 2 ? (int)(sizeof tfc / sizeof tfc[0]) : (int)(sizeof fea / sizeof fea[0]);
            const float pw = 262, extra = g_mapMenu == 2 ? 3 * 30.0f + 6 : 0.0f;
            const float ph = 46 + n * 30 + extra + 8;
            const float px = r0.x + 10 + cw + 8;
            const float py = std::max(r0.y + 46, std::min(r0.y + 46 + (g_mapMenu - 1) * 36.0f, r1.y - 50 - ph));
            const ImVec2 pa(px, py), pb(px + pw, py + ph);
            ImGui::SetCursorScreenPos(pa);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##mmenubg", ImVec2(pw, ph));   // (taps on the menu stay off the map)
            dl->AddRectFilled(ImVec2(pa.x + 2, pa.y + 4), ImVec2(pb.x + 2, pb.y + 4), IM_COL32(0, 0, 0, 70), 12.0f);
            dl->AddRectFilled(pa, pb, themeTone(12, 22, 40, 242), 12.0f);
            dl->AddRect(pa, pb, IM_COL32(255, 255, 255, 46), 12.0f, 0, 1.0f);
            textAt(dl, f.bold, pa.x + 12, pa.y + 12, kInk, cats[g_mapMenu - 1]);
            if (pill(dl, f, "##mmall", "All", ImVec2(pb.x - 118, pa.y + 8), ImVec2(pb.x - 66, pa.y + 34), false, true))
                for (int i = 0; i < n; ++i) if (items[i].enabled || items[i].v == &g_settings.mapRoute) { *items[i].v = true; g_settingsChanged = true; }
            if (pill(dl, f, "##mmclr", "Clear", ImVec2(pb.x - 62, pa.y + 8), ImVec2(pb.x - 8, pa.y + 34), false, true))
                for (int i = 0; i < n; ++i) { *items[i].v = false; g_settingsChanged = true; }
            float ry = pa.y + 42;
            for (int i = 0; i < n; ++i, ry += 30)
                if (menuCheck(dl, f, items[i].id, ImVec2(pa.x + 4, ry), pw - 8, items[i].name, *items[i].v, items[i].enabled)) { *items[i].v = !*items[i].v; g_settingsChanged = true; }
            if (g_mapMenu == 2) {   // where the traffic comes from, and how far and how high it is shown
                ry += 4;
                const bool world = g_settings.mapTrafficSource == 1;
                const char* srcName = world ? "+ VATSIM World" : "Nearby";
                const int ds = menuStepper(dl, f, "##mtsrc", ImVec2(pa.x + 4, ry), pw - 8, "Source", srcName, world, !world);
                if (ds) { g_settings.mapTrafficSource = ds > 0 ? 1 : 0; g_settingsChanged = true; }
                ry += 30;
                static const float kRanges[] = {0, 5, 10, 25, 50, 100, 250, 500};
                static const float kAlts[] = {0, 1000, 2000, 5000, 10000, 20000};
                auto idxOf = [](const float* v, int nv, float x) { int k = 0; for (int i = 0; i < nv; ++i) if (std::fabs(v[i] - x) < 0.5f) k = i; return k; };
                char vb[24];
                int ri = idxOf(kRanges, 8, g_settings.mapTrafficRangeNm);
                if (ri == 0) std::snprintf(vb, sizeof vb, "Any"); else std::snprintf(vb, sizeof vb, "%.0f nm", kRanges[ri]);
                const int dr = menuStepper(dl, f, "##mtrng", ImVec2(pa.x + 4, ry), pw - 8, "Range", vb, ri > 0, ri < 7);
                if (dr) { g_settings.mapTrafficRangeNm = kRanges[std::max(0, std::min(7, ri + dr))]; g_settingsChanged = true; }
                ry += 30;
                int ai = idxOf(kAlts, 6, g_settings.mapTrafficAltFt);
                if (ai == 0) std::snprintf(vb, sizeof vb, "Any"); else std::snprintf(vb, sizeof vb, "\xC2\xB1%.0f ft", kAlts[ai]);
                const int da = menuStepper(dl, f, "##mtalt", ImVec2(pa.x + 4, ry), pw - 8, "Altitude", vb, ai > 0, ai < 5);
                if (da) { g_settings.mapTrafficAltFt = kAlts[std::max(0, std::min(5, ai + da))]; g_settingsChanged = true; }
            }
        }
        // where the map comes from, and whether the airports are still being found
        const char* attr = noMap ? "" : satImg ? "Imagery: Esri, Maxar, Earthstar Geographics" : "\xC2\xA9 OpenStreetMap contributors";
        if (*attr) {
            const ImVec2 as = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, attr);
            dl->AddRectFilled(ImVec2(r1.x - as.x - 12, r1.y - as.y - 6), r1, IM_COL32(255, 255, 255, 190), 4.0f, ImDrawFlags_RoundCornersTopLeft);
            dl->AddText(f.small, fontPx(f.small), ImVec2(r1.x - as.x - 6, r1.y - as.y - 3), IM_COL32(30, 36, 48, 255), attr);
        }
        if (g_settings.mapAirports && m.aptIndexing) textAt(dl, f.small, r0.x + 12, y - 22, sat ? kInk : IM_COL32(30, 36, 48, 255), "Finding X-Plane's airports...");
        (void)missing;
        // what is clear of the buttons: right of the layers, below the top row, above the bottom row
        g_mapFree0 = ImVec2(r0.x + 10 + cw + 10, r0.y + 10 + 30 + 8);
        g_mapFree1 = ImVec2(r1.x - 8, y - 8);
    }
    // ---- what was tapped: the nearest traffic, navaid, fix, route point or airport (as WXPlane's map), one card at a time
    if (mapClick && g_mapMenu) { g_mapMenu = 0; }   // a tap on the map closes the open menu (and only that)
    else if (mapClick) {
        const ImVec2 p = io.MousePos;
        float best = 16.0f;
        int kind = 0;
        std::string key;
        auto consider = [&](ImVec2 q, int k, const std::string& kk, float reach) {
            const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
            if (d <= reach && d < best + (k == 1 ? 4.0f : 0.0f)) { best = d; kind = k; key = kk; }   // traffic wins near-ties
        };
        if (g_settings.mapTraffic) for (const MapState::Traffic& t : m.traffic) if (trafficShown(m, t)) consider(toS(mercOf(t.lat, t.lon)), 1, t.callsign, 18);
        if ((g_settings.mapNavaids || g_settings.mapIls) && g_mapZoom >= 6.0)
            for (const MapState::Navaid& n : m.navaids) if ((n.type != 4 && n.type != 5) ? g_settings.mapNavaids : (g_settings.mapIls && g_mapZoom >= 9.5)) consider(toS(mercOf(n.lat, n.lon)), 2, n.ident + "|" + std::to_string(n.type), 16);
        if (g_settings.mapIls && g_mapZoom >= 9.5 && kind != 1) {   // a localizer's course: anywhere inside its triangle (as drawn)
            const double mPerMercT = kEarthM * std::cos(cLat * kPi / 180.0), nmPxT = scale / (mPerMercT / 1852.0);
            for (const MapState::Navaid& n : m.navaids) {
                if (n.type != 4 && n.type != 5) continue;
                const ImVec2 q = toS(mercOf(n.lat, n.lon));
                const double a = (n.bearing + 180.0) * kPi / 180.0 - rot;
                const float Lp = (float)(10.0 * nmPxT), half = (float)(std::tan(2.5 * kPi / 180.0) * 10.0 * nmPxT);
                const ImVec2 d((float)std::sin(a), (float)-std::cos(a)), nn(-d.y, d.x);
                // along the course and across it, from the antenna
                const float along = (p.x - q.x) * d.x + (p.y - q.y) * d.y, across = (p.x - q.x) * nn.x + (p.y - q.y) * nn.y;
                const float halfAt = along / std::max(1.0f, Lp) * half + 6.0f;   // (the triangle widens away from the antenna; a little slack)
                if (along > 0 && along < Lp && std::fabs(across) < halfAt && best > 1.0f) { best = 1.0f; kind = 2; key = n.ident + "|" + std::to_string(n.type); }
            }
        }
        if (g_settings.mapWaypoints && g_mapZoom >= 9.0) for (const MapState::Fix& fx : m.fixes) if (fx.enroute || g_mapZoom >= 11) consider(toS(mercOf(fx.lat, fx.lon)), 3, fx.ident, 12);
        if (g_settings.mapRoute) for (size_t i = 0; i < m.route.size(); ++i) consider(toS(mercOf(m.route[i].lat, m.route[i].lon)), 5, std::to_string(i), 12);
        // airports come before navaids, ILS courses, fixes and route points (only traffic is ahead of them): their dot, or close in
        // anywhere on their layout; and on a layout, a stand (its dot) before the airport
        if (g_settings.mapAirports && kind != 1) {
            float aBest = 16.0f;
            std::string aKey;
            if (g_mapZoom >= 7.0)
                for (const AptPoint& a : m.airportPoints) {
                    if (a.rwyFt < (g_mapZoom >= 9.0 ? 0.0f : g_mapZoom >= 8.0 ? 5000.0f : 8000.0f)) continue;   // (its dot is not drawn)
                    const ImVec2 q = toS(mercOf(a.lat, a.lon));
                    const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
                    if (d < aBest) { aBest = d; aKey = a.icao; }
                }
            bool stand = false;
            if (g_mapShownNm < 1.9 && g_settings.mapStands)   // the stands are drawn
                for (const auto& ap : m.airports) {
                    float sBest = 11.0f;
                    int si = -1;
                    for (size_t i = 0; i < ap->stands.size(); ++i) {
                        const ImVec2 q = toS(ap->stands[i].p);
                        const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
                        if (d < sBest) { sBest = d; si = (int)i; }
                    }
                    if (si >= 0) { kind = 6; key = ap->icao + "|" + std::to_string(si); stand = true; break; }
                }
            if (!stand && !aKey.empty()) { kind = 4; key = aKey; }
            else if (!stand && kind == 0 && g_mapZoom >= 11.5)   // close in: anywhere on an airport's layout (a navaid, fix or ILS tapped on it wins)
                for (const auto& ap : m.airports) {
                    const ImVec2 b0 = toS(ap->lo), b1 = toS(ap->hi);
                    if (p.x >= std::min(b0.x, b1.x) - 8 && p.x <= std::max(b0.x, b1.x) + 8 && p.y >= std::min(b0.y, b1.y) - 8 && p.y <= std::max(b0.y, b1.y) + 8) { kind = 4; key = ap->icao; break; }
                }
        }
        g_selKind = kind; g_selKey = key;   // nothing there: the card closes
    }
    if (g_selKind == 1) act.trafficSelected = g_selKey;
    if (g_selKind == 4) act.airportOpen = g_selKey;
    if (g_selKind) {
        std::vector<CardLine> L;
        ImVec2 at(-1, -1);
        char b[160];
        if (g_selKind == 1) {
            const MapState::Traffic* t = nullptr;
            for (const MapState::Traffic& x : m.traffic) if (x.callsign == g_selKey) t = &x;
            if (t) {
                at = toS(mercOf(t->lat, t->lon));
                const VatsimPilot& v = m.vatsim;
                const bool vat = v.found && v.callsign == g_selKey;
                L.push_back({0, t->callsign, t->type.empty() ? (vat ? v.aircraft : "") : t->type});
                std::snprintf(b, sizeof b, "%s", t->onGround ? "on the ground" : (t->altFt >= 18000 ? ("FL" + std::to_string((int)std::lround(t->altFt / 100))) : (std::to_string((int)std::lround(t->altFt / 10) * 10) + " ft")).c_str());
                L.push_back({1, "Altitude", b});
                if (m.havePos && !t->onGround) { std::snprintf(b, sizeof b, "%+.0f ft", (t->altFt - m.altFt) / 10.0f * 10.0f); L.push_back({1, "Relative", b}); }
                std::snprintf(b, sizeof b, "%.0f kt", t->gsKt); L.push_back({1, "Ground speed", b});
                std::snprintf(b, sizeof b, "%03d\xC2\xB0T", ((int)std::lround(t->hdg) % 360 + 360) % 360); L.push_back({1, "Heading", b});
                if (!t->onGround) { std::snprintf(b, sizeof b, "%+.0f fpm", std::round(t->vsFpm / 50) * 50); L.push_back({1, "Vertical speed", b}); }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, t->lat, t->lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0", d, brg); L.push_back({1, "From you", b}); }
                L.push_back({4, "VATSIM", ""});
                if (!vat) L.push_back({2, m.vatsimStatus.empty() ? "Asking VATSIM..." : m.vatsimStatus, ""});
                else {
                    {   // the name as VATSIM has it (often the pilot's number again, or the name and a home airport), the number on its own line
                        std::string nm = v.name;
                        while (!nm.empty() && nm.back() == ' ') nm.pop_back();
                        if (!v.cid.empty() && nm.size() > v.cid.size() && nm.compare(nm.size() - v.cid.size(), v.cid.size(), v.cid) == 0) { nm.resize(nm.size() - v.cid.size()); while (!nm.empty() && nm.back() == ' ') nm.pop_back(); }
                        if (!nm.empty() && nm != v.cid) L.push_back({1, "Pilot", nm});
                        if (!v.cid.empty()) L.push_back({1, "VATSIM ID", v.cid});
                    }
                    if (!v.rating.empty()) L.push_back({1, "Rating", v.rating});
                    L.push_back({1, "Server", v.server + (v.logon.empty() ? "" : ", since " + v.logon)});
                    L.push_back({1, "Squawk", v.squawk + (!v.assignedSquawk.empty() && v.assignedSquawk != "0000" && v.assignedSquawk != v.squawk ? "  (assigned " + v.assignedSquawk + ")" : "")});
                    if (v.qnhMb > 0) { std::snprintf(b, sizeof b, "%d hPa", v.qnhMb); L.push_back({1, "Altimeter", b}); }
                    if (v.hasPlan) {
                        L.push_back({1, "Flight plan", v.rules + "  " + v.departure + " \xE2\x86\x92 " + v.arrival + (v.alternate.empty() ? "" : "  (alt " + v.alternate + ")")});
                        L.push_back({1, "Aircraft", v.aircraftFull.empty() ? v.aircraft : v.aircraftFull});
                        L.push_back({1, "Cruise", v.cruiseAlt + (v.cruiseTas.empty() ? "" : "  at " + v.cruiseTas + " kt")});
                        auto hhmm = [](const std::string& x) { return x.size() == 4 ? x.substr(0, 2) + ":" + x.substr(2) : x; };
                        if (!v.depTime.empty()) L.push_back({1, "Departure", hhmm(v.depTime) + "Z" + (v.enroute.empty() ? "" : ", en route " + hhmm(v.enroute))});
                        if (!v.fuel.empty()) L.push_back({1, "Fuel on board", hhmm(v.fuel)});
                        if (!v.route.empty()) L.push_back({2, v.route, "route"});
                        if (!v.remarks.empty()) L.push_back({2, v.remarks.size() > 220 ? v.remarks.substr(0, 218) + ".." : v.remarks, "remarks"});
                    } else L.push_back({2, "No flight plan filed", ""});
                }
            }
        } else if (g_selKind == 2) {
            for (const MapState::Navaid& n : m.navaids) {
                if (n.ident + "|" + std::to_string(n.type) != g_selKey) continue;
                at = toS(mercOf(n.lat, n.lon));
                const char* kindName = n.type == 2 ? "NDB" : n.type == 3 ? "VOR" : n.type == 4 ? "ILS localizer" : n.type == 5 ? "Localizer" : "DME";
                L.push_back({0, n.ident, n.name});
                L.push_back({1, "Type", kindName});
                if (n.type == 2) std::snprintf(b, sizeof b, "%.0f kHz", n.freq); else std::snprintf(b, sizeof b, "%.2f MHz", n.freq);
                L.push_back({1, "Frequency", b});
                L.push_back({3, n.ident, morseOf(n.ident)});
                if (n.type == 4 || n.type == 5) {   // the approach: its runway, course (true and magnetic), glide slope and DME
                    if (!n.runway.empty()) L.push_back({1, "Runway", n.airport + " " + n.runway});
                    if (!n.name.empty()) L.push_back({1, "Approach", n.name});
                    if (n.magCourse >= 0) std::snprintf(b, sizeof b, "%03.0f\xC2\xB0T  /  %03.0f\xC2\xB0M", std::fmod(n.bearing + 360.0, 360.0), std::fmod(n.magCourse + 360.0, 360.0));
                    else std::snprintf(b, sizeof b, "%03.0f\xC2\xB0T", std::fmod(n.bearing + 360.0, 360.0));
                    L.push_back({1, "Course", b});
                    if (n.gsAngle > 0) { std::snprintf(b, sizeof b, "%.2f\xC2\xB0", n.gsAngle); L.push_back({1, "Glide slope", b}); }
                    else L.push_back({1, "Glide slope", "none (localizer only)"});
                    L.push_back({1, "DME", n.dme ? "yes" : "no"});
                }
                if (n.type == 3 && n.bearing != 0) { std::snprintf(b, sizeof b, "%.0f\xC2\xB0%s", std::fabs(n.bearing), n.bearing < 0 ? "W" : "E"); L.push_back({1, "Variation", b}); }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, n.lat, n.lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From you", b}); }
                L.push_back({1, "Position", posText(n.lat, n.lon)});
                break;
            }
        } else if (g_selKind == 3) {
            for (const MapState::Fix& fx : m.fixes) {
                if (fx.ident != g_selKey) continue;
                at = toS(mercOf(fx.lat, fx.lon));
                L.push_back({0, fx.ident, fx.enroute ? "Enroute fix" : "Terminal fix"});
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, fx.lat, fx.lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From you", b}); }
                L.push_back({1, "Position", posText(fx.lat, fx.lon)});
                break;
            }
        } else if (g_selKind == 5) {
            const size_t i = (size_t)std::atoi(g_selKey.c_str());
            if (i < m.route.size()) {
                const MapState::RoutePoint& rp = m.route[i];
                at = toS(mercOf(rp.lat, rp.lon));
                L.push_back({0, rp.ident, "Flight plan, point " + std::to_string(i + 1) + " of " + std::to_string(m.route.size())});
                if (i < s.plan.legs.size() && s.plan.legs[i].ident == rp.ident) {
                    const FlightPlan::Leg& lg = s.plan.legs[i];
                    if (!lg.cross.empty()) L.push_back({1, "Crossing", lg.cross});
                    if (!lg.airway.empty()) L.push_back({1, "Via", lg.airway});
                    if (!lg.alt.empty()) L.push_back({1, "Altitude", lg.alt});
                    if (!lg.legNm.empty()) L.push_back({1, "Leg", lg.legNm + " nm"});
                    if (!lg.totalTime.empty()) L.push_back({1, "Time from departure", lg.totalTime});
                }
                L.push_back({1, "Position", posText(rp.lat, rp.lon)});
            }
        } else if (g_selKind == 6) {   // a stand: its name, kind, size and the airlines that use it, decoded
            const size_t bar = g_selKey.find('|');
            const std::string icao = g_selKey.substr(0, bar);
            const size_t si = bar == std::string::npos ? (size_t)-1 : (size_t)std::atoi(g_selKey.c_str() + bar + 1);
            for (const auto& ap : m.airports) {
                if (ap->icao != icao || si >= ap->stands.size()) continue;
                const AptGeo::Stand& st = ap->stands[si];
                at = toS(st.p);
                std::string kindName = st.type == "gate" ? "Gate" : (st.type == "tie_down" || st.type == "tie-down") ? "Tie-down" : st.type == "hangar" ? "Hangar" : "Stand";
                L.push_back({0, st.name.empty() ? kindName : st.name, kindName + " at " + icao});
                if (!st.size.empty()) L.push_back({1, "Size", "ICAO class " + st.size});
                if (!st.uses.empty()) {   // "heavy|jets|turboprops": heavy, jets, turboprops
                    std::string u;
                    for (char c : st.uses) { if (c == '|') u += ", "; else u += c == '_' ? ' ' : c; }
                    L.push_back({1, "For", u});
                }
                if (!st.ops.empty()) { std::string o = st.ops; for (char& c : o) if (c == '_') c = ' '; if (!o.empty()) o[0] = (char)std::toupper((unsigned char)o[0]); L.push_back({1, "Use", o}); }
                std::snprintf(b, sizeof b, "%03.0f\xC2\xB0T", std::fmod(st.hdg + 360.0, 360.0)); L.push_back({1, "Heading", b});
                // the airlines (apt.dat: their ICAO codes), each with its name when known
                std::vector<std::string> codes;
                for (size_t q = 0; q < st.airlines.size();) {
                    while (q < st.airlines.size() && (st.airlines[q] == ' ' || st.airlines[q] == ',')) ++q;
                    size_t e = q;
                    while (e < st.airlines.size() && st.airlines[e] != ' ' && st.airlines[e] != ',') ++e;
                    if (e > q) codes.push_back(st.airlines.substr(q, e - q));
                    q = e;
                }
                if (!codes.empty()) {
                    L.push_back({4, "Airlines", ""});
                    for (size_t k = 0; k < codes.size() && k < 12; ++k) { std::string c = codes[k]; for (char& ch : c) ch = (char)std::toupper((unsigned char)ch); const char* n = airlineName(c); L.push_back({1, c, n ? n : "not in XPTablet's list"}); }
                    if (codes.size() > 12) L.push_back({2, "and " + std::to_string(codes.size() - 12) + " more", ""});
                } else L.push_back({2, "The scenery names no airlines for this stand.", ""});
                break;
            }
        } else if (g_selKind == 4) {
            double la = 0, lo = 0;
            bool placed = false;
            std::string nm;
            for (const AptPoint& a : m.airportPoints) if (a.icao == g_selKey) { la = a.lat; lo = a.lon; nm = a.name; placed = true; }
            for (const auto& ap : m.airports) if (ap->icao == g_selKey) { la = ap->lat; lo = ap->lon; nm = ap->name; placed = true; }
            if (placed) {
                at = toS(mercOf(la, lo));
                const AirportInfo& ai = s.airport;
                const bool have = ai.icao == g_selKey && ai.found && ai.geo;
                L.push_back({0, g_selKey, have && !ai.name.empty() ? ai.name : nm});
                if (!have) L.push_back({2, ai.icao == g_selKey && ai.busy ? "Reading the airport..." : "Reading the airport...", ""});
                else {
                    const AptGeo& g = *ai.geo;
                    std::snprintf(b, sizeof b, "%.0f ft", g.elevationFt);
                    L.push_back({1, "Elevation", b});
                    std::snprintf(b, sizeof b, "%.3f", ai.ctaf.mhz);
                    L.push_back({1, "CTAF", b});
                    int nf = 0;
                    for (const AirportInfo::Freq& fq : ai.freqs) {
                        if (fq.what == "CTAF" || nf >= 7) continue;
                        std::snprintf(b, sizeof b, "%.3f", fq.mhz);
                        L.push_back({1, fq.what, b});
                        ++nf;
                    }
                    L.push_back({4, "Runways", ""});
                    for (size_t i = 0; i < g.runways.size() && i < 6; ++i) {
                        double a1, o1, a2, o2;
                        latLonOf(g.runways[i].a, a1, o1);
                        latLonOf(g.runways[i].b, a2, o2);
                        const double ft = distNmLL(a1, o1, a2, o2, nullptr) * 6076.12;
                        const int surf = i < g.runwayInfo.size() ? g.runwayInfo[i].surface : 1;
                        std::snprintf(b, sizeof b, "%.0f x %.0f ft, %s", ft, g.runways[i].widthM * 3.28084, surfaceName(surf));
                        L.push_back({1, g.runways[i].idA + " / " + g.runways[i].idB, b});
                    }
                }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, la, lo, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From you", b}); }
                L.push_back({5, "Open in Airports", ""});
            }
        }
        if (L.empty() || at.x < -1e5f) g_selKind = 0;   // it has gone (out of range, landed, disconnected)
        else if (drawCard(dl, f, L, at, r0, r1)) {   // its button pressed
            if (g_selKind == 4) { g_aptSel = g_selKey; g_aptScroll = 0; openApp(APP_AIRPORTS); }
        }
    }

    dl->PopClipRect();
}

// ---- a text field (the on-screen keyboard types into it; so does the floating window's keyboard) ------------------------------------
} // namespace
// a typed text's lines, wrapped at wrapW (each: its first and past-its-last byte). A line's words stay on it as more are typed after
// them; the spaces a wrap falls on end the line they follow, and a space typed at the very end of a full line starts the next one
// (the caret goes there, as on an iPad, instead of hanging past the edge)
std::vector<std::pair<int, int>> wrapLines(ImFont* font, float px, const std::string& v, float wrapW) {
    std::vector<std::pair<int, int>> out;
    const char* base = v.c_str(), *end = base + v.size();
    const char* p0 = base;
    while (true) {
        const char* nl = std::find(p0, end, '\n');
        const char* s0 = p0;
        do {
            const char* cut = nl == s0 ? nl : font->CalcWordWrapPositionA(px / font->FontSize, s0, nl, wrapW);
            if (cut == s0 && s0 < nl) ++cut;
            out.push_back({(int)(s0 - base), (int)(cut - base)});
            s0 = cut;
            while (s0 < nl && *s0 == ' ') ++s0;
            if (s0 == nl && cut < nl) out.push_back({(int)(s0 - base), (int)(s0 - base)});   // only spaces after the wrap: a new, empty line
        } while (s0 < nl);
        if (nl == end) break;
        p0 = nl + 1;
    }
    return out;
}
namespace {
// ruled: the pad's line spacing (the scratchpad's lines): each line of text sits on one of them (0: plain)
bool textField(ImDrawList* dl, const Fonts& f, const char* id, std::string& v, ImVec2 a, ImVec2 b, const char* hint, bool multiline, float ruled = 0) {
    ImGui::SetCursorScreenPos(a);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    if (hit) { g_kbd = true; g_kbdTarget = 2; g_typeInto = &v; g_typeMultiline = multiline; }
    const bool focus = g_kbd && g_kbdTarget == 2 && g_typeInto == &v, hot = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, focus ? 30 : hot ? 22 : 14), 8.0f);
    dl->AddRect(a, b, focus ? accentLite(220) : IM_COL32(255, 255, 255, 30), 8.0f, 0, focus ? 1.5f : 1.0f);
    dl->PushClipRect(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), true);
    const float px = fontPx(f.body);
    const float ty = multiline ? a.y + 8 : std::floor(a.y + (b.y - a.y - px) * 0.5f);
    if (v.empty() && !focus) textAt(dl, f.body, a.x + 10, ty, kMuted, hint);
    else {
        // the caret is drawn on its own, at the text's end: added to the text, it moved a long line and rewrapped the last word
        // each time it blinked
        const bool caretOn = focus && std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        auto caret = [&](float x, float y) { if (caretOn) dl->AddLine(ImVec2(x + 1, y), ImVec2(x + 1, y + px), kInk, 1.5f); };
        if (multiline) {   // line by line (each on a rule when ruled), wrapped at the box's width
            const float wrapW = b.x - a.x - 24, step = ruled > 0 ? ruled : px + 5;
            struct Ln { const char* s; const char* e; };
            std::vector<Ln> ls;
            for (const auto& se : wrapLines(f.body, px, v, wrapW)) ls.push_back({v.c_str() + se.first, v.c_str() + se.second});
            // past the box, it moves up a whole line at a time (on the rules) so the last line shows
            // as many lines as there are rules (as the pad draws them: every `ruled` from its top, short of its bottom), so the line
            // being typed is never one drawn under the last rule, half cut off, until the text moved up a line
            const float y0 = ruled > 0 ? a.y + ruled - px - 6 : a.y + 8;
            int fits = 0;
            if (ruled > 0) { for (float ly = a.y + ruled; ly < b.y - 4; ly += ruled) ++fits; }
            else fits = (int)std::floor((b.y - 4 - y0 - px) / step) + 1;
            fits = std::max(1, fits);
            const int first = std::max(0, (int)ls.size() - fits);
            float y = y0;
            for (int k = first; k < (int)ls.size(); ++k, y += step) dl->AddText(f.body, px, ImVec2(a.x + 10, y), kInk, ls[k].s, ls[k].e);
            const Ln& last = ls.back();
            caret(a.x + 10 + f.body->CalcTextSizeA(px, 1e9f, 0, last.s, last.e).x, y0 + (int)(ls.size() - 1 - first) * step);
        } else {   // a long line shows its end (with room kept for the caret, so it does not move as it blinks)
            const float tw = f.body->CalcTextSizeA(px, 1e9f, 0, v.c_str()).x;
            const float room = b.x - a.x - 24;
            const float x = std::floor(a.x + 10 - std::max(0.0f, tw - room));   // (whole pixels: the letters do not shimmer as it moves)
            textAt(dl, f.body, x, ty, kInk, v.c_str());
            caret(x + tw, ty);
        }
    }
    dl->PopClipRect();
    return hit;
}
constexpr float kKbdFullH = 5 * 34 + 4 * 5 + 16;   // the on-screen keyboard's height
#define kKbdH keyboardHeight()                      // (the physical keyboard's bar is shorter)
// the keyboard at the bottom of a page while one of its fields is typed in; the page is drawn kbShift higher so the field shows
float pageKeyboard(ImDrawList* dl, const Fonts& f, float w, float h, Actions& act) {
    if (!(g_kbd && g_kbdTarget == 2 && g_typeInto)) return 0;
    keyboard(dl, f, w, h - kKbdFullH, act);
    return kKbdH;
}

// ---- Notes: a clearance (CRAFT), the ATIS, and a scratchpad to write on by hand or type ------------------------------------------------
const char* const kCraftLabels[6] = {"Clearance limit", "Route", "Altitude", "Frequency", "Transponder", "Remarks"};
const char* const kAtisLabels[9] = {"Information", "Time", "Wind", "Visibility", "Clouds", "Temp / dew", "Altimeter", "Runways / approach", "Remarks"};
std::string g_craft[6], g_atis[9], g_scratch;
struct Stroke { int color = 0; std::vector<ImVec2> pts; };   // points as fractions of the pad
std::vector<Stroke> g_strokes;
int g_notesTab = 0, g_penColor = 0;
bool g_eraser = false;                       // the scratchpad's eraser (else the pen)
std::vector<std::vector<Stroke>> g_strokeUndo;   // the drawing before each stroke or rub, for Undo
void strokesSaved() { g_strokeUndo.push_back(g_strokes); if (g_strokeUndo.size() > 40) g_strokeUndo.erase(g_strokeUndo.begin()); }
// rubs out the parts of the lines within r (pad fractions scaled by pw, ph) of q; a line cut in two becomes two
bool eraseIn(std::vector<Stroke>& strokes, ImVec2 q, float r, float pw, float ph) {
    bool changed = false;
    std::vector<Stroke> out;
    for (const Stroke& st : strokes) {
        Stroke cur{st.color, {}};
        for (const ImVec2& p : st.pts) {
            const float dx = (p.x - q.x) * pw, dy = (p.y - q.y) * ph;
            if (dx * dx + dy * dy <= r * r) { changed = true; if (!cur.pts.empty()) { out.push_back(cur); cur.pts.clear(); } }
            else cur.pts.push_back(p);
        }
        if (!cur.pts.empty()) out.push_back(cur);
    }
    if (changed) strokes.swap(out);
    return changed;
}
bool eraseAt(ImVec2 q, float r, float pw, float ph) { return eraseIn(g_strokes, q, r, pw, ph); }
// markup on a document: its own lines (kept as fractions of the picture, so they stay put when it is zoomed or moved), per file
std::map<std::string, std::vector<Stroke>> g_docMarks;
bool g_docMarkup = false, g_docEraser = false;
int g_docPen = 0;
std::vector<std::vector<Stroke>> g_docUndo;   // this document's drawing before each line or rub
std::string g_docUndoFor;
#define kDocPens kPens   // the same pens as the notes' scratchpad: white, yellow, blue, and the highlighter
bool g_penMode = true, g_notesChanged = false;
const ImU32 kPens[4] = {IM_COL32(240, 244, 250, 255), IM_COL32(255, 210, 70, 255), IM_COL32(110, 200, 255, 255),
                        IM_COL32(255, 214, 0, 150)};   // white, yellow, blue, and the highlighter (wide, see-through)
// a pen's dot in a tool row (the highlighter's is a flat marker tip); its ring when chosen, a faint one on hover
void penDot(ImDrawList* dl, ImVec2 c, ImU32 col, bool highlighter, bool sel, bool hot) {
    if (highlighter) dl->AddRectFilled(ImVec2(c.x - 9, c.y - 6), ImVec2(c.x + 9, c.y + 6), (col & 0x00FFFFFF) | 0xFF000000, 3.0f);
    else dl->AddCircleFilled(c, 10.0f, col, 0);
    if (sel) dl->AddCircle(c, 13.0f, accentLite(255), 0, 2.0f);
    else if (hot) dl->AddCircle(c, 13.0f, IM_COL32(255, 255, 255, 90), 0, 1.5f);
}

void notesPage(ImDrawList* dl, const Fonts& f, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH + 8;
    // the three pads
    const char* tabs[3] = {"Clearance", "ATIS", "Scratchpad"};
    for (int i = 0; i < 3; ++i) {
        char id[16];
        std::snprintf(id, sizeof id, "##ntab%d", i);
        if (pill(dl, f, id, tabs[i], ImVec2(12 + i * 128.0f, top), ImVec2(12 + i * 128.0f + 120, top + 30), g_notesTab == i, true) && g_notesTab != i) {
            g_notesTab = i; g_kbd = false; g_typeInto = nullptr;
        }
    }
    if (pill(dl, f, "##nclear", "Clear", ImVec2(w - 92, top), ImVec2(w - 12, top + 30), false, true)) {
        if (g_notesTab == 0) for (auto& x : g_craft) x.clear();
        else if (g_notesTab == 1) for (auto& x : g_atis) x.clear();
        else { if (!g_strokes.empty()) strokesSaved(); g_scratch.clear(); g_strokes.clear(); }
        g_notesChanged = true;
    }
    // the scratchpad's Undo, next to Clear
    if (g_notesTab == 2 && pill(dl, f, "##nundo", "Undo", ImVec2(w - 180, top), ImVec2(w - 100, top + 30), false, !g_strokeUndo.empty())) {
        g_strokes = g_strokeUndo.back(); g_strokeUndo.pop_back(); g_notesChanged = true;
    }
    const float y0 = top + 42;
    const bool kb = g_kbd && g_kbdTarget == 2 && g_typeInto;
    const float bottom = kb ? h - kKbdH : h;
    if (g_notesTab == 0 || g_notesTab == 1) {
        const int n = g_notesTab == 0 ? 6 : 9;
        std::string* vals = g_notesTab == 0 ? g_craft : g_atis;
        const char* const* labels = g_notesTab == 0 ? kCraftLabels : kAtisLabels;
        // rows: CRAFT one column (with its letters); ATIS two columns, remarks across
        struct Row { float x0, x1, y; };
        std::vector<Row> rows;
        const float rowH = 50;
        if (g_notesTab == 0) for (int i = 0; i < n; ++i) rows.push_back({60, w - 12, y0 + i * rowH});
        else {
            const float half = std::floor((w - 36) * 0.5f);
            for (int i = 0; i < 8; ++i) rows.push_back({i < 4 ? 12.0f : 24 + half, i < 4 ? 12 + half : w - 12, y0 + (i % 4) * rowH});
            rows.push_back({12, w - 12, y0 + 4 * rowH});
        }
        // keep the field being typed in above the keyboard
        // (it glides there, rather than jumping each time another field is tapped or the keyboard comes and goes)
        float want = 0;
        for (int i = 0; i < n; ++i) if (kb && g_typeInto == &vals[i]) want = std::max(0.0f, rows[i].y + rowH - bottom + 6);
        static float glide = 0;
        glide += (want - glide) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 14.0f));
        if (std::fabs(want - glide) < 0.4f) glide = want;
        const float shift = std::floor(glide + 0.5f);   // (drawn on whole pixels, so the text stays crisp while it moves)
        dl->PushClipRect(ImVec2(0, top + 36), ImVec2(w, bottom), true);
        for (int i = 0; i < n; ++i) {
            const Row& r = rows[i];
            const float y = r.y - shift;
            if (g_notesTab == 0 && i < 5) {   // the CRAFT letter
                const char L[2] = {"CRAFT"[i], 0};
                dl->AddCircleFilled(ImVec2(32, y + 30), 15.0f, accentCol(200), 0);
                textCentered(dl, f.bold, 32, y + 30 - fontPx(f.bold) * 0.5f, kInk, L);
            }
            textAt(dl, f.small, r.x0 + 2, y, kMuted, labels[i]);
            char id[16];
            std::snprintf(id, sizeof id, "##nf%d_%d", g_notesTab, i);
            textField(dl, f, id, vals[i], ImVec2(r.x0, y + 16), ImVec2(r.x1, y + 46), "", false);
        }
        dl->PopClipRect();
        if (g_typeChanged) { g_typeChanged = false; g_notesChanged = true; }
        pageKeyboard(dl, f, w, h, act);
        return;
    }
    // the scratchpad: pen or keyboard
    if (pill(dl, f, "##npen", "Pen", ImVec2(12, y0), ImVec2(92, y0 + 28), g_penMode, true)) { g_penMode = true; g_kbd = false; g_typeInto = nullptr; }
    if (pill(dl, f, "##ntype", "Type", ImVec2(98, y0), ImVec2(178, y0 + 28), !g_penMode, true)) g_penMode = false;
    for (int i = 0; i < 4; ++i) {   // the pen's colour, and the highlighter
        const ImVec2 c(206 + i * 32.0f, y0 + 14);
        ImGui::SetCursorScreenPos(ImVec2(c.x - 13, c.y - 13));
        char id[16];
        std::snprintf(id, sizeof id, "##npc%d", i);
        if (ImGui::InvisibleButton(id, ImVec2(26, 26))) { g_penColor = i; g_penMode = true; g_eraser = false; }
        penDot(dl, c, kPens[i], i == 3, g_penColor == i && !g_eraser && g_penMode, ImGui::IsItemHovered());
    }
    if (pill(dl, f, "##neraser", "Eraser", ImVec2(332, y0), ImVec2(418, y0 + 28), g_eraser && g_penMode, true)) { g_eraser = !(g_eraser && g_penMode); g_penMode = true; g_kbd = false; g_typeInto = nullptr; }
    const ImVec2 pa(12, y0 + 38), pb(w - 12, bottom - 10);
    dl->AddRectFilled(pa, pb, themeTone(14, 24, 40, 235), 10.0f);
    for (float ly = pa.y + 34; ly < pb.y - 4; ly += 34) dl->AddLine(ImVec2(pa.x + 10, ly), ImVec2(pb.x - 10, ly), IM_COL32(255, 255, 255, 16), 1.0f);
    const float pw = pb.x - pa.x, ph = std::max(1.0f, (h - 10) - pa.y);   // strokes are kept as fractions of the whole pad (keyboard or not)
    if (g_penMode) {
        ImGui::SetCursorScreenPos(pa);
        ImGui::InvisibleButton("##npad", ImVec2(pw, pb.y - pa.y));
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const ImVec2 q((mp.x - pa.x) / pw, (mp.y - pa.y) / ph);
        if (g_eraser) {   // rub out what the finger passes over (a ring shows where)
            static bool rubbed = false;
            if (ImGui::IsItemActivated()) { strokesSaved(); rubbed = false; }
            if (ImGui::IsItemActive() && eraseAt(q, 11.0f, pw, ph)) { rubbed = true; g_notesChanged = true; }
            if (ImGui::IsItemDeactivated() && !rubbed && !g_strokeUndo.empty()) g_strokeUndo.pop_back();   // nothing rubbed: nothing to undo
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) dl->AddCircle(mp, 11.0f, IM_COL32(255, 255, 255, 160), 0, 1.5f);
        } else if (ImGui::IsItemActivated()) { strokesSaved(); g_strokes.push_back(Stroke{g_penColor, {q}}); g_notesChanged = true; }
        else if (ImGui::IsItemActive() && !g_strokes.empty()) {
            const ImVec2 last = g_strokes.back().pts.back();
            if (std::fabs(last.x - q.x) * pw + std::fabs(last.y - q.y) * ph > 1.5f) g_strokes.back().pts.push_back(q);
        }
    } else {
        textField(dl, f, "##nscratch", g_scratch, pa, pb, "Tap to type", true, 34.0f);   // on the pad's lines
        if (g_typeChanged) { g_typeChanged = false; g_notesChanged = true; }
    }
    dl->PushClipRect(pa, pb, true);
    std::vector<ImVec2> pts;
    for (const Stroke& st : g_strokes) {
        pts.clear();
        for (const ImVec2& q : st.pts) pts.push_back(ImVec2(pa.x + q.x * pw, pa.y + q.y * ph));
        const bool hl = st.color % 4 == 3;
        if (pts.size() == 1) dl->AddCircleFilled(pts[0], hl ? 6.0f : 1.6f, kPens[st.color % 4], 0);
        else dl->AddPolyline(pts.data(), (int)pts.size(), kPens[st.color % 4], 0, hl ? 12.0f : 2.2f);
    }
    dl->PopClipRect();
    pageKeyboard(dl, f, w, h, act);
}

std::string escapeLine(const std::string& v) {
    std::string o;
    for (char c : v) { if (c == '\n') o += "\\n"; else if (c == '\\') o += "\\\\"; else o += c; }
    return o;
}
std::string unescapeLine(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) { o += v[i + 1] == 'n' ? '\n' : v[i + 1]; ++i; }
        else o += v[i];
    }
    return o;
}

// ---- Checklists: the aircraft's own file, or a general one for airliners or light aircraft; ticked off item by item ---------------------
struct CheckSection { std::string title; std::vector<std::pair<std::string, std::string>> items; };
const char* const kAirlinerChecklist =
    "# Cockpit preparation\nBatteries | ON\nExternal power / APU | ON\nFlight plan | LOADED\nFuel quantity | CHECKED\nAltimeters | SET\nTakeoff data | SET\n"
    "# Before start\nDoors | CLOSED\nSeat belt signs | ON\nParking brake | SET\nBeacon | ON\nTransponder | SET\nTakeoff briefing | COMPLETE\n"
    "# After start\nGenerators | ON\nAPU bleed | OFF\nAnti-ice | AS RQRD\nFlaps | SET\nFlight controls | CHECKED\nGround equipment | CLEAR\n"
    "# Before takeoff\nCabin | READY\nFlaps | SET\nTrim | SET\nTransponder | TA/RA\nLanding lights | ON\nRunway | CONFIRMED\n"
    "# After takeoff\nGear | UP\nFlaps | UP\nAutopilot | AS RQRD\nAltimeters | STD at transition\n"
    "# Approach\nArrival briefing | COMPLETE\nAltimeters | SET\nMinimums | SET\nSeat belt signs | ON\nAuto brake | SET\n"
    "# Landing\nGear | DOWN\nFlaps | LANDING\nSpeed brakes | ARMED\nLanding clearance | RECEIVED\n"
    "# After landing\nSpeed brakes | RETRACTED\nFlaps | UP\nTransponder | STBY\nLanding lights | OFF\nAPU | START\n"
    "# Parking\nParking brake | SET\nEngines | OFF\nSeat belt signs | OFF\nBeacon | OFF\nFuel pumps | OFF\n";
const char* const kLightChecklist =
    "# Preflight\nFuel quantity | CHECKED\nOil | CHECKED\nControl locks | REMOVED\nWeather and NOTAMs | CHECKED\n"
    "# Before start\nSeats and belts | SECURE\nFuel selector | BOTH\nCircuit breakers | IN\nAvionics | OFF\nBeacon | ON\n"
    "# Engine start\nMixture | RICH\nThrottle | 1/4 IN OPEN\nPropeller area | CLEAR\nMagnetos | START\nOil pressure | CHECKED\n"
    "# Before takeoff\nParking brake | SET\nFlight controls | FREE AND CORRECT\nInstruments | SET\nRun-up | COMPLETE\nFlaps | SET\nTrim | TAKEOFF\nTransponder | ALT\nLights | ON\n"
    "# Cruise\nPower | SET\nMixture | LEANED\nFuel | MONITORED\n"
    "# Before landing\nATIS / altimeter | SET\nFuel selector | BOTH\nMixture | RICH\nCarb heat | AS RQRD\nFlaps | AS RQRD\n"
    "# After landing\nFlaps | UP\nCarb heat | COLD\nTransponder | STBY\nLights | AS RQRD\n"
    "# Shutdown\nAvionics | OFF\nMixture | IDLE CUTOFF\nMagnetos | OFF\nMaster | OFF\nParking brake | SET\n";

std::vector<CheckSection> parseChecklist(const std::string& text) {
    std::vector<CheckSection> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && line[b] == ' ') ++b;
        line.erase(0, b);
        if (line.empty() || line.compare(0, 2, "//") == 0) continue;
        if (line[0] == '#') { size_t k = 1; while (k < line.size() && (line[k] == '#' || line[k] == ' ')) ++k; out.push_back({line.substr(k), {}}); continue; }
        if (out.empty()) out.push_back({"Checklist", {}});
        const size_t bar = line.find('|');
        std::string item = bar == std::string::npos ? line : line.substr(0, bar), val = bar == std::string::npos ? "" : line.substr(bar + 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        out.back().items.push_back({item, val});
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const CheckSection& c) { return c.items.empty(); }), out.end());
    return out;
}

int g_clSource = -1;    // -1 automatic: the aircraft's own file, else by its size; 0 own, 1 airliner, 2 light aircraft
int g_clSection = 0;
std::string g_clKey;    // what the ticks belong to (source and aircraft)
std::vector<std::vector<bool>> g_clDone;

void checklistsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h) {
    const float top = kStatusH + kHeaderH + 8;
    float len = 0, span = 0;
    const PlaneSpec& spec = planeSpec(s.acfIcao);
    if (!findShape(s.acfIcao, len, span)) len = spec.len;
    const bool own = !s.checklistText.empty();
    int src = g_clSource;
    if (src < 0 || (src == 0 && !own)) src = own ? 0 : (len >= 24 ? 1 : 2);
    const char* names[3] = {"This aircraft", "Airliner", "Light aircraft"};
    float x = 12;
    for (int i = 0; i < 3; ++i) {
        if (i == 0 && !own) continue;
        char id[16];
        std::snprintf(id, sizeof id, "##cls%d", i);
        const float bw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, names[i]).x + 32;
        if (pill(dl, f, id, names[i], ImVec2(x, top), ImVec2(x + bw, top + 30), src == i, true)) g_clSource = i;
        x += bw + 6;
    }
    if (own) textRight(dl, f.small, w - 14, top + 8, kMuted, s.checklistSource.c_str());
    else textRight(dl, f.small, w - 14, top + 8, kMuted, "Your own: Output/XPTablet/Checklists/<ICAO>.txt");
    const std::vector<CheckSection> list = parseChecklist(src == 0 ? s.checklistText : src == 1 ? kAirlinerChecklist : kLightChecklist);
    if (list.empty()) return;
    const std::string key = std::to_string(src) + "|" + s.acfIcao;
    if (key != g_clKey || g_clDone.size() != list.size()) {
        g_clKey = key; g_clSection = 0;
        g_clDone.assign(list.size(), {});
        for (size_t i = 0; i < list.size(); ++i) g_clDone[i].assign(list[i].items.size(), false);
    }
    g_clSection = std::max(0, std::min((int)list.size() - 1, g_clSection));
    // the sections, with how far each has got
    const float y0 = top + 42, leftW = 230;
    card(dl, ImVec2(12, y0), ImVec2(12 + leftW, h - 12), f, nullptr);
    const float rh = std::min(36.0f, std::floor((h - 12 - y0 - 12) / (float)list.size()));
    for (size_t i = 0; i < list.size(); ++i) {
        const float y = y0 + 6 + i * rh;
        int done = 0;
        for (bool b : g_clDone[i]) done += b ? 1 : 0;
        const bool all = done == (int)g_clDone[i].size();
        char id[32];
        std::snprintf(id, sizeof id, "##clsec%zu", i);
        ImGui::SetCursorScreenPos(ImVec2(18, y));
        if (ImGui::InvisibleButton(id, ImVec2(leftW - 12, rh - 2))) g_clSection = (int)i;
        if ((int)i == g_clSection) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + leftW, y + rh - 2), accentCol(80), 8.0f);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + leftW, y + rh - 2), IM_COL32(255, 255, 255, 18), 8.0f);
        textAt(dl, f.body, 28, std::floor(y + (rh - 2 - fontPx(f.body)) * 0.5f), all ? kGood : kInk, list[i].title.c_str());
        char c[16];
        std::snprintf(c, sizeof c, all ? "\xE2\x9C\x93" : "%d/%d", done, (int)g_clDone[i].size());
        if (all) dl->AddText(f.bold, fontPx(f.bold), ImVec2(leftW - 18, std::floor(y + (rh - 2 - fontPx(f.bold)) * 0.5f)), kGood, "OK");
        else textRight(dl, f.small, leftW - 2, std::floor(y + (rh - 2 - fontPx(f.small)) * 0.5f), kMuted, c);
    }
    // the items: tap one to tick it
    const CheckSection& sec = list[g_clSection];
    std::vector<bool>& done = g_clDone[g_clSection];
    const float rx = 24 + leftW, rw = w - 12 - rx;
    card(dl, ImVec2(rx, y0), ImVec2(rx + rw, h - 12), f, nullptr);
    textAt(dl, f.title, rx + 16, y0 + 10, kInk, sec.title.c_str());
    const float iy0 = y0 + 48, ih = std::min(38.0f, std::floor((h - 12 - 52 - iy0) / std::max(1.0f, (float)sec.items.size())));
    for (size_t i = 0; i < sec.items.size(); ++i) {
        const float y = iy0 + i * ih;
        char id[32];
        std::snprintf(id, sizeof id, "##cli%zu", i);
        ImGui::SetCursorScreenPos(ImVec2(rx + 10, y));
        if (ImGui::InvisibleButton(id, ImVec2(rw - 20, ih - 2))) done[i] = !done[i];
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(rx + 10, y), ImVec2(rx + rw - 10, y + ih - 2), IM_COL32(255, 255, 255, 14), 8.0f);
        const bool on = done[i];
        const ImVec2 c(rx + 28, y + (ih - 2) * 0.5f);
        dl->AddCircle(c, 9.0f, on ? kGood : IM_COL32(255, 255, 255, 90), 0, 1.6f);
        if (on) { dl->AddCircleFilled(c, 9.0f, kGood, 0); dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x - 1, c.y + 3.5f), IM_COL32(10, 30, 20, 255), 2.0f); dl->AddLine(ImVec2(c.x - 1, c.y + 3.5f), ImVec2(c.x + 5, c.y - 4), IM_COL32(10, 30, 20, 255), 2.0f); }
        const float ty = std::floor(y + (ih - 2 - fontPx(f.body)) * 0.5f);
        const ImU32 col = on ? kMuted : kInk;
        textAt(dl, f.body, rx + 48, ty, col, sec.items[i].first.c_str());
        if (!sec.items[i].second.empty()) {
            const float tw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, sec.items[i].first.c_str()).x;
            const float vw = f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, sec.items[i].second.c_str()).x;
            for (float dx = rx + 56 + tw; dx < rx + rw - 22 - vw; dx += 6) dl->AddCircleFilled(ImVec2(dx, ty + fontPx(f.body) * 0.75f), 0.9f, IM_COL32(255, 255, 255, 60), 0);
            textRight(dl, f.bold, rx + rw - 16, ty, on ? kMuted : IM_COL32(255, 214, 120, 255), sec.items[i].second.c_str());
        }
    }
    const bool all = std::all_of(done.begin(), done.end(), [](bool b) { return b; });
    if (pill(dl, f, "##clreset", "Reset", ImVec2(rx + 16, h - 56), ImVec2(rx + 116, h - 24), false, true)) std::fill(done.begin(), done.end(), false);
    if (pill(dl, f, "##clall", "Reset all", ImVec2(rx + 124, h - 56), ImVec2(rx + 244, h - 24), false, true)) for (auto& d : g_clDone) std::fill(d.begin(), d.end(), false);
    if (g_clSection + 1 < (int)list.size() && pill(dl, f, "##clnext", "Next section", ImVec2(rx + rw - 156, h - 56), ImVec2(rx + rw - 16, h - 24), all, true)) ++g_clSection;
}

// ---- Airports: X-Plane's airports (search, or the nearest), each with its radio (CTAF always), runways, parking and METAR -------------
std::string g_aptQuery, g_aptSel;
float g_aptScroll = 0;
const char* surfaceName(int s) {
    if (s == 1 || (s >= 20 && s <= 38)) return "Asphalt";
    if (s == 2 || (s >= 50 && s <= 57)) return "Concrete";
    switch (s) { case 3: return "Grass"; case 4: return "Dirt"; case 5: return "Gravel"; case 12: return "Dry lakebed"; case 13: return "Water"; case 14: return "Snow / ice"; default: return "Other"; }
}
double distNmLL(double la1, double lo1, double la2, double lo2, double* brgOut) {
    const double k = kPi / 180.0;
    const double dLat = (la2 - la1) * k, dLon = (lo2 - lo1) * k;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(la1 * k) * std::cos(la2 * k) * std::sin(dLon / 2) * std::sin(dLon / 2);
    if (brgOut) {
        const double y = std::sin(dLon) * std::cos(la2 * k), x = std::cos(la1 * k) * std::sin(la2 * k) - std::sin(la1 * k) * std::cos(la2 * k) * std::cos(dLon);
        *brgOut = std::fmod(std::atan2(y, x) / k + 360.0, 360.0);
    }
    return 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a)) * 3440.065;
}

void airportsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    act.airportsShown = true;
    act.airportSearch = g_aptQuery;
    const float top = kStatusH + kHeaderH + 8;
    const bool kb = g_kbd && g_kbdTarget == 2 && g_typeInto == &g_aptQuery;
    const float bottom = kb ? h - kKbdH : h;
    // the list
    const float lw = 270;
    textField(dl, f, "##aptq", g_aptQuery, ImVec2(12, top), ImVec2(12 + lw, top + 34), "Search: code or name", false);
    card(dl, ImVec2(12, top + 42), ImVec2(12 + lw, bottom - 10), f, g_aptQuery.empty() ? "NEAREST" : "FOUND");
    if (s.airportList.empty()) {
        dl->AddText(f.small, fontPx(f.small), ImVec2(26, top + 74), kMuted, s.airportStatus.empty() ? "No airport found" : s.airportStatus.c_str(), nullptr, lw - 28);
    }
    dl->PushClipRect(ImVec2(12, top + 64), ImVec2(12 + lw, bottom - 12), true);
    for (size_t i = 0; i < s.airportList.size(); ++i) {
        const AptPoint& p = s.airportList[i];
        const float y = top + 66 + i * 42;
        if (y > bottom - 20) break;
        char id[32];
        std::snprintf(id, sizeof id, "##aptl%zu", i);
        ImGui::SetCursorScreenPos(ImVec2(18, y));
        if (ImGui::InvisibleButton(id, ImVec2(lw - 12, 40))) { g_aptSel = p.icao; g_kbd = false; g_typeInto = nullptr; g_aptScroll = 0; }
        if (p.icao == g_aptSel) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + lw, y + 40), accentCol(80), 8.0f);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + lw, y + 40), IM_COL32(255, 255, 255, 18), 8.0f);
        textAt(dl, f.bold, 28, y + 3, kInk, p.icao.c_str());
        if (s.map.havePos) {
            double brg = 0;
            const double d = distNmLL(s.map.lat, s.map.lon, p.lat, p.lon, &brg);
            char b[32];
            std::snprintf(b, sizeof b, "%.0f nm %03.0f\xC2\xB0", d, brg);
            textRight(dl, f.small, 2 + lw, y + 5, kMuted, b);
        }
        std::string nm = p.name;
        while (nm.size() > 3 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, nm.c_str()).x > lw - 40) { nm.resize(nm.size() - 3); nm += ".."; }
        textAt(dl, f.small, 28, y + 22, kMuted, nm.c_str());
    }
    dl->PopClipRect();
    // the first time: the nearest airport, shown
    if (g_aptSel.empty() && g_aptQuery.empty() && !s.airportList.empty()) g_aptSel = s.airportList[0].icao;
    if (!g_aptSel.empty()) act.airportOpen = g_aptSel;
    // the airport
    const float rx = 24 + lw, rw = w - 12 - rx;
    const ImVec2 ra(rx, top), rb(rx + rw, bottom - 10);
    card(dl, ra, rb, f, nullptr);
    const AirportInfo& ap = s.airport;
    if (g_aptSel.empty()) { pageKeyboard(dl, f, w, h, act); return; }
    if (ap.icao != g_aptSel || ap.busy) { textCentered(dl, f.body, rx + rw * 0.5f, top + 60, kMuted, "Reading the airport..."); pageKeyboard(dl, f, w, h, act); return; }
    if (!ap.found) { textCentered(dl, f.body, rx + rw * 0.5f, top + 60, kMuted, "This airport is not in X-Plane's scenery."); pageKeyboard(dl, f, w, h, act); return; }
    const AptGeo& g = *ap.geo;
    if (ap.icao.size() >= 3 && ap.icao.size() <= 4 && s.metar.icao != ap.icao && !s.metar.busy) act.metarSearch = ap.icao;
    // scrolling: the wheel or a drag over the panel
    ImGui::SetCursorScreenPos(ra);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##aptpanel", ImVec2(rb.x - ra.x, rb.y - ra.y));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_aptScroll -= ImGui::GetIO().MouseWheel * 40;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) g_aptScroll -= ImGui::GetIO().MouseDelta.y;
    g_aptScroll = std::max(0.0f, g_aptScroll);
    dl->PushClipRect(ImVec2(ra.x + 2, ra.y + 2), ImVec2(rb.x - 2, rb.y - 2), true);
    const float x0 = rx + 18, x1 = rx + rw - 18;
    float y = top + 14 - g_aptScroll;
    // the name and place
    textAt(dl, f.title, x0, y, kInk, ap.icao.c_str());
    const float iw = f.title->CalcTextSizeA(fontPx(f.title), 1e9f, 0, ap.icao.c_str()).x;
    if (!ap.iata.empty()) textAt(dl, f.small, x0 + iw + 8, y + 8, kMuted, ap.iata.c_str());
    {
        ImGui::SetCursorScreenPos(ImVec2(x1 - 120, y));
        if (pill(dl, f, "##aptmap", "Show on map", ImVec2(x1 - 120, y), ImVec2(x1, y + 28), false, true)) {
            showOnMap(g.lat, g.lon);
            dl->PopClipRect();
            return;
        }
    }
    y += 32;
    textAt(dl, f.bold, x0, y, kInk, (ap.name.empty() ? g.name : ap.name).c_str());
    y += 22;
    std::string place = ap.city;
    if (!ap.region.empty()) place += (place.empty() ? "" : ", ") + ap.region;
    else if (!ap.country.empty()) place += (place.empty() ? "" : ", ") + ap.country;
    {
        char b[96];
        std::snprintf(b, sizeof b, "Elevation %.0f ft", g.elevationFt);
        std::string line = place.empty() ? std::string(b) : place + "  \xC2\xB7  " + b;
        if (s.map.havePos) {
            double brg = 0;
            const double d = distNmLL(s.map.lat, s.map.lon, g.lat, g.lon, &brg);
            char c[48];
            std::snprintf(c, sizeof c, "  \xC2\xB7  %.0f nm, %03.0f\xC2\xB0 from you", d, brg);
            line += c;
        }
        textAt(dl, f.small, x0, y, kMuted, line.c_str());
    }
    y += 26;
    // CTAF, always
    {
        const ImVec2 a(x0, y), b(x0 + 300, y + 52);
        dl->AddRectFilled(a, b, IM_COL32(255, 196, 80, 34), 10.0f);
        dl->AddRect(a, b, IM_COL32(255, 196, 80, 160), 10.0f, 0, 1.2f);
        textAt(dl, f.small, a.x + 12, a.y + 6, IM_COL32(255, 214, 120, 255), "CTAF");
        char fq[16];
        std::snprintf(fq, sizeof fq, "%.3f", ap.ctaf.mhz);
        textAt(dl, f.title, a.x + 12, a.y + 20, kInk, fq);
        dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 112, a.y + 9), kMuted, ap.ctaf.note.c_str(), nullptr, b.x - a.x - 120);
    }
    // the METAR beside it
    if (s.metar.icao == ap.icao && !s.metar.raw.empty()) dl->AddText(f.mono, fontPx(f.mono), ImVec2(x0 + 314, y + 2), kInk, s.metar.raw.c_str(), nullptr, x1 - x0 - 314);
    else textAt(dl, f.small, x0 + 314, y + 4, kMuted, s.metar.icao == ap.icao && s.metar.busy ? "METAR: asking NOAA..." : "No METAR");
    y += 64;
    // radio
    textAt(dl, f.small, x0, y, kMuted, "RADIO");
    y += 20;
    for (const AirportInfo::Freq& fq : ap.freqs) {
        textAt(dl, f.body, x0, y, kInk, fq.what.c_str());
        char b[16];
        std::snprintf(b, sizeof b, "%.3f", fq.mhz);
        textAt(dl, f.mono, x0 + 150, y + 1, kInk, b);
        if (!fq.note.empty()) {
            std::string n = fq.note;
            while (n.size() > 3 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, n.c_str()).x > x1 - x0 - 230) { n.resize(n.size() - 3); n += ".."; }
            textAt(dl, f.small, x0 + 230, y + 2, kMuted, n.c_str());
        }
        y += 22;
    }
    if (ap.freqs.empty()) { textAt(dl, f.small, x0, y, kMuted, "No frequencies published"); y += 22; }
    y += 10;
    // runways
    textAt(dl, f.small, x0, y, kMuted, "RUNWAYS");
    y += 20;
    for (size_t i = 0; i < g.runways.size(); ++i) {
        const AptGeo::Runway& r = g.runways[i];
        double la1, lo1, la2, lo2, brg = 0;
        latLonOf(r.a, la1, lo1);
        latLonOf(r.b, la2, lo2);
        const double lenFt = distNmLL(la1, lo1, la2, lo2, &brg) * 6076.12;
        const AptGeo::RunwayInfo ri = i < g.runwayInfo.size() ? g.runwayInfo[i] : AptGeo::RunwayInfo();
        char b[128];
        std::snprintf(b, sizeof b, "%s / %s", r.idA.c_str(), r.idB.c_str());
        textAt(dl, f.bold, x0, y, kInk, b);
        std::snprintf(b, sizeof b, "%s x %.0f ft   %s%s   %03.0f\xC2\xB0T", "", r.widthM * 3.28084, surfaceName(ri.surface), ri.lit ? ", lit" : "", brg);
        char len[32];
        std::snprintf(len, sizeof len, "%.0f", lenFt);
        std::string withComma = len;
        if (withComma.size() > 3) withComma.insert(withComma.size() - 3, ",");
        textAt(dl, f.body, x0 + 110, y, kInk, (withComma + b).c_str());
        y += 24;
    }
    y += 8;
    // parking
    if (!g.stands.empty()) {
        textAt(dl, f.small, x0, y, kMuted, "PARKING");
        y += 20;
        int gates = 0, ties = 0, hangars = 0, misc = 0;
        for (const auto& st : g.stands) { if (st.type == "gate") ++gates; else if (st.type == "tie_down" || st.type == "tie-down") ++ties; else if (st.type == "hangar") ++hangars; else ++misc; }
        char b[128];
        std::snprintf(b, sizeof b, "%d gates  \xC2\xB7  %d ramp / tie-down  \xC2\xB7  %d hangars%s", gates, ties, hangars, misc ? "  \xC2\xB7  others" : "");
        textAt(dl, f.body, x0, y, kInk, b);
        y += 24;
        std::string all;
        for (size_t i = 0; i < g.stands.size() && i < 120; ++i) { if (!all.empty()) all += ",  "; all += g.stands[i].name; }
        if (g.stands.size() > 120) all += ",  ...";
        const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, x1 - x0, all.c_str());
        dl->AddText(f.small, fontPx(f.small), ImVec2(x0, y), kMuted, all.c_str(), nullptr, x1 - x0);
        y += sz.y + 10;
    }
    const float contentH = y + g_aptScroll - (top + 14);
    g_aptScroll = std::min(g_aptScroll, std::max(0.0f, contentH - (rb.y - ra.y) + 30));
    dl->PopClipRect();
    pageKeyboard(dl, f, w, h, act);
}

// ---- Documents: PDFs (in the browser's PDF viewer) and pictures (here), from the tablet's folder, the aircraft's and AviTab's charts ----
float g_docZoom = 1.0f, g_docZoomShown = 1.0f;   // the zoom asked for, and the one shown (it glides there)
ImVec2 g_docPan(0, 0), g_docPanShown(0, 0);
std::string g_docLast;
std::string fileUrl(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    std::string out = "file://";
    if (path.empty() || path[0] != '/') out += "/";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == ':' || c == '~') out += (char)c;
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}
std::string sizeText(long long b) {
    char t[24];
    if (b >= 1 << 20) std::snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
    else std::snprintf(t, sizeof t, "%.0f KB", std::max(1.0, b / 1024.0));
    return t;
}
float scrollBar(ImDrawList* dl, const char* id, float x, float y0, float y1, float scroll, float content, float shown);   // (below)
float g_docListScroll = 0;
std::string g_docPdf;      // the PDF shown in the Documents page ("" : a picture, or none)

// ---- PDFs: one way for every page that shows one (Documents, Dispatch) -----------------------------------------------------------------
// Each page has a browser tab of its own (a group no other page shows) with Chromium's PDF viewer, its toolbar hidden and the pages
// fitted to the width; the page is shown in the page's own rectangle, a drag scrolls it and the page's zoom is + / -.
constexpr int kGroupDocuments = 300, kGroupDispatch = 301;
std::string pdfAddress(const std::string& url) { return url.find('#') == std::string::npos ? url + "#toolbar=0&navpanes=0&view=FitH" : url; }
void pdfView(ImDrawList* dl, const Fonts& f, const SimState& s, float x0, float y0, float w, float h, Actions& act, int group, const std::string& url) {
    const BrowserState& b = s.browser;
    static std::map<int, std::string> asked;   // the address each group's tab was last given
    static std::map<int, double> openedAt;
    const std::string want = pdfAddress(url);
    int tab = 0;
    for (const BrowserState::Tab& t : b.tabs) if (t.group == group) { tab = t.id; break; }
    if (b.status == 1) {
        if (!tab) {
            if ((!openedAt.count(group) || ImGui::GetTime() - openedAt[group] > 3) && b.tabs.size() < 16) {   // (once in 3 s: it takes a moment to come)
                act.webTabNew = true; act.webTabGroup = group; act.webTabUrl = want;
                asked[group] = want; openedAt[group] = ImGui::GetTime();
            }
        } else if (b.activeTab != tab) act.webTabSelect = tab;
        else if (asked[group] != want) { act.webNavigate = want; asked[group] = want; }
    }
    g_webHold = !(tab && b.activeTab == tab && asked[group] == want);
    webView(dl, f, s, x0, y0, w, h, act, true);
}
std::string g_docFilter;   // one folder ("This aircraft"...), or "" for all

void documentsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const DocsState& d = s.docs;
    if (!d.listed) act.docsList = true;
    const float top = kStatusH + kHeaderH + 8, lw = 290;
    if (iconButton("##docref", ImVec2(12, top), ImVec2(48, top + 30), true)) act.docsList = true;   // look again
    reloadIcon(dl, ImVec2(30, top + 15), kInk);
    {   // which folders: all of them, the aircraft's own, or XPTablet's Documents folder
        struct Flt { const char* id; const char* name; const char* folder; float x0, x1; };
        const Flt flts[] = {{"##docall", "All", "", 54, 104}, {"##docacf", "Aircraft", "This aircraft", 108, 198}, {"##docxpt", "XPTablet", "XPTablet's Documents", 202, 302}};
        for (const Flt& fl : flts)
            if (pill(dl, f, fl.id, fl.name, ImVec2(fl.x0, top), ImVec2(fl.x1, top + 30), g_docFilter == fl.folder, true) && g_docFilter != fl.folder) { g_docFilter = fl.folder; g_docListScroll = 0; }
        if (!g_docFilter.empty() && g_docFilter != "This aircraft" && g_docFilter != "XPTablet's Documents") g_docFilter.clear();
    }
    const ImVec2 la(12, top + 40), lb(12 + lw, h - 12);
    card(dl, la, lb, f, nullptr);
    if (d.files.empty()) {
        std::string t = "Put PDFs and pictures (PNG, JPG) in:\n";
        for (const std::string& fo : d.folders) t += "\n" + fo;
        dl->AddText(f.small, fontPx(f.small), ImVec2(la.x + 14, la.y + 14), kMuted, t.c_str(), nullptr, lw - 28);
    }
    // the list, by folder; it scrolls with the wheel or a drag
    ImGui::SetCursorScreenPos(la);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##doclist", ImVec2(lb.x - la.x, lb.y - la.y));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_docListScroll -= ImGui::GetIO().MouseWheel * 40;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) g_docListScroll -= ImGui::GetIO().MouseDelta.y;
    g_docListScroll = std::max(0.0f, g_docListScroll);
    // the list's height first (for the scroll bar, and the rows' width beside it)
    float contentH = 8;
    {
        std::string fo;
        int n = 0;
        for (const DocEntry& e : d.files) {
            if (!g_docFilter.empty() && e.folder != g_docFilter) continue;
            if (e.folder != fo) { fo = e.folder; contentH += 24; }
            contentH += 36; ++n;
        }
        contentH += 12;
        if (n == 0 && !d.files.empty()) textAt(dl, f.small, la.x + 14, la.y + 14, kMuted, g_docFilter == "This aircraft" ? "No documents in this aircraft's folder." : "No documents in XPTablet's Documents folder.");
    }
    const float shownH = lb.y - la.y;
    const bool scrolls = contentH > shownH + 1;
    g_docListScroll = std::min(g_docListScroll, std::max(0.0f, contentH - shownH));
    const float rowR = scrolls ? lb.x - 16 : lb.x - 6;
    dl->PushClipRect(ImVec2(la.x + 2, la.y + 2), ImVec2(lb.x - 2, lb.y - 2), true);
    float y = la.y + 8 - g_docListScroll;
    std::string folder;
    for (size_t i = 0; i < d.files.size(); ++i) {
        const DocEntry& e = d.files[i];
        if (!g_docFilter.empty() && e.folder != g_docFilter) continue;
        if (e.folder != folder) { folder = e.folder; textAt(dl, f.small, la.x + 12, y + 4, kMuted, folder.c_str()); y += 24; }
        char id[32];
        std::snprintf(id, sizeof id, "##doc%zu", i);
        ImGui::SetCursorScreenPos(ImVec2(la.x + 6, y));
        if (ImGui::InvisibleButton(id, ImVec2(rowR - la.x - 6, 34)) && y > la.y - 30) {
            if (e.pdf) g_docPdf = e.path;   // shown here, as a picture is
            else { g_docPdf.clear(); act.docOpen = e.path; g_docZoom = 1.0f; g_docPan = ImVec2(0, 0); }
        }
        if (e.path == (g_docPdf.empty() ? d.shownPath : g_docPdf)) dl->AddRectFilled(ImVec2(la.x + 6, y), ImVec2(rowR, y + 34), accentCol(80), 8.0f);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(la.x + 6, y), ImVec2(rowR, y + 34), IM_COL32(255, 255, 255, 18), 8.0f);
        // a small page: red for a PDF, blue for a picture
        const ImVec2 ic(la.x + 22, y + 17);
        dl->AddRectFilled(ImVec2(ic.x - 7, ic.y - 10), ImVec2(ic.x + 7, ic.y + 10), e.pdf ? IM_COL32(220, 70, 70, 255) : IM_COL32(70, 150, 230, 255), 2.0f);
        textCentered(dl, f.small, ic.x, ic.y - fontPx(f.small) * 0.5f, kInk, e.pdf ? "P" : "I");
        std::string nm = e.name;
        while (nm.size() > 3 && f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, nm.c_str()).x > lw - 110) { nm.resize(nm.size() - 3); nm += ".."; }
        textAt(dl, f.body, la.x + 38, std::floor(y + 17 - fontPx(f.body) * 0.5f), kInk, nm.c_str());
        textRight(dl, f.small, rowR - 6, std::floor(y + 17 - fontPx(f.small) * 0.5f), kMuted, sizeText(e.bytes).c_str());
        y += 36;
    }
    dl->PopClipRect();
    if (scrolls) g_docListScroll = scrollBar(dl, "##docbar", lb.x - 8, la.y + 8, lb.y - 8, g_docListScroll, contentH, shownH);
    // the picture: fitted, then the wheel or + / - to zoom and a drag to move
    const ImVec2 va(24 + lw, top), vb(w - 12, h - 12);
    if (!g_docPdf.empty()) {   // a PDF: its name and the zoom along the top, the pages under them
        dl->AddRectFilled(va, vb, themeTone(8, 14, 26, 240), 10.0f);
        std::string nm = g_docPdf.substr(g_docPdf.find_last_of("/\\") + 1);
        while (nm.size() > 3 && f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, nm.c_str()).x > vb.x - va.x - 170) { nm.resize(nm.size() - 3); nm += ".."; }
        textAt(dl, f.bold, va.x + 14, va.y + 13, kInk, nm.c_str());
        webZoomControls(dl, f, s.browser, act, "doc", vb.x - 136, va.y + 6, va.y + 38, s.browser.status == 1 && !g_webHold);
        pdfView(dl, f, s, va.x, va.y + 44, vb.x - va.x, vb.y - va.y - 44, act, kGroupDocuments, fileUrl(g_docPdf));
        return;
    }
    dl->AddRectFilled(va, vb, themeTone(8, 14, 26, 240), 10.0f);
    if (d.shownPath != g_docLast) { g_docLast = d.shownPath; g_docZoom = g_docZoomShown = 1.0f; g_docPan = g_docPanShown = ImVec2(0, 0); }
    if (d.busy) { textCentered(dl, f.body, (va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f - 8, kMuted, "Opening..."); return; }
    if (!d.error.empty()) { textCentered(dl, f.body, (va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f - 8, kWarn, d.error.c_str()); return; }
    if (!d.tex || d.texW <= 0 || d.texH <= 0) { textCentered(dl, f.body, (va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f - 8, kMuted, "Choose a document. PDFs open in the browser."); return; }
    ImGui::SetCursorScreenPos(va);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##docview", ImVec2(vb.x - va.x, vb.y - va.y));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) g_docZoom = std::max(0.5f, std::min(8.0f, g_docZoom * (io.MouseWheel > 0 ? 1.25f : 0.8f)));
    {   // zoom and Fit glide (as the map's moves do); a drag moves it at once
        const float k = 1.0f - std::exp(-io.DeltaTime * 14.0f);
        g_docZoomShown += (g_docZoom - g_docZoomShown) * k;
        if (std::fabs(g_docZoom - g_docZoomShown) < 0.001f) g_docZoomShown = g_docZoom;
        if (ImGui::IsItemActive() && !g_docMarkup) g_docPanShown = g_docPan;
        else { g_docPanShown.x += (g_docPan.x - g_docPanShown.x) * k; g_docPanShown.y += (g_docPan.y - g_docPanShown.y) * k; }
    }
    const float fit = std::min((vb.x - va.x - 16) / d.texW, (vb.y - va.y - 16) / d.texH) * g_docZoomShown;
    const ImVec2 c((va.x + vb.x) * 0.5f + g_docPanShown.x, (va.y + vb.y) * 0.5f + g_docPanShown.y);
    const ImVec2 i0(c.x - d.texW * fit * 0.5f, c.y - d.texH * fit * 0.5f), isz(d.texW * fit, d.texH * fit);   // the picture on screen
    std::vector<Stroke>& marks = g_docMarks[d.shownPath];
    if (g_docUndoFor != d.shownPath) { g_docUndoFor = d.shownPath; g_docUndo.clear(); }
    auto saveUndo = [&] { g_docUndo.push_back(marks); if (g_docUndo.size() > 40) g_docUndo.erase(g_docUndo.begin()); };
    const ImVec2 mp = io.MousePos;
    const ImVec2 q((mp.x - i0.x) / std::max(1.0f, isz.x), (mp.y - i0.y) / std::max(1.0f, isz.y));
    if (!g_docMarkup) {   // a drag moves the picture
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) { g_docPan.x += io.MouseDelta.x; g_docPan.y += io.MouseDelta.y; }
    } else if (g_docEraser) {   // rub out what the finger passes over
        static bool rubbed = false;
        if (ImGui::IsItemActivated()) { saveUndo(); rubbed = false; }
        if (ImGui::IsItemActive() && eraseIn(marks, q, 12.0f, isz.x, isz.y)) { rubbed = true; g_notesChanged = true; }
        if (ImGui::IsItemDeactivated() && !rubbed && !g_docUndo.empty()) g_docUndo.pop_back();
    } else if (ImGui::IsItemActivated()) { saveUndo(); marks.push_back(Stroke{g_docPen, {q}}); g_notesChanged = true; }
    else if (ImGui::IsItemActive() && !marks.empty()) {   // the line follows the finger
        const ImVec2 last = marks.back().pts.back();
        if (std::fabs(last.x - q.x) * isz.x + std::fabs(last.y - q.y) * isz.y > 1.5f) marks.back().pts.push_back(q);
    }
    dl->PushClipRect(va, vb, true);
    dl->AddImage(d.tex, i0, ImVec2(i0.x + isz.x, i0.y + isz.y));
    {   // the markup, over the picture (its lines grow with the zoom, as ink on paper would)
        std::vector<ImVec2> pts;
        const float z = isz.x / std::max(1.0f, vb.x - va.x - 16);
        for (const Stroke& st : marks) {
            pts.clear();
            for (const ImVec2& p : st.pts) pts.push_back(ImVec2(i0.x + p.x * isz.x, i0.y + p.y * isz.y));
            const ImU32 col = (st.color % 4 == 0 && d.light) ? IM_COL32(24, 26, 32, 255) : kDocPens[st.color % 4];   // white on paper: black
            const float th = (st.color % 4 == 3 ? 12.0f : 2.6f) * std::max(0.5f, z);
            if (pts.size() == 1) dl->AddCircleFilled(pts[0], th * 0.6f, col, 0);
            else dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, th);
        }
    }
    if (g_docMarkup && g_docEraser && (ImGui::IsItemHovered() || ImGui::IsItemActive())) dl->AddCircle(mp, 12.0f, IM_COL32(40, 40, 40, 200), 0, 1.5f);
    dl->PopClipRect();
    // the tools, as the notes' scratchpad has them, in one dark bar along the top: the pens and the highlighter, Eraser; Undo and Clear
    // together at the right, then Markup (which turns drawing on and off: off, a drag moves the page)
    dl->AddRectFilled(ImVec2(g_docMarkup ? va.x + 6 : vb.x - 96, va.y + 6), ImVec2(vb.x - 6, va.y + 44), themeTone(10, 18, 34, 225), 12.0f);
    dl->AddRectFilled(ImVec2(vb.x - 160, vb.y - 46), ImVec2(vb.x - 6, vb.y - 8), themeTone(10, 18, 34, 215), 12.0f);   // (under Fit, -, +)
    if (pill(dl, f, "##docmark", "Markup", ImVec2(vb.x - 90, va.y + 10), ImVec2(vb.x - 12, va.y + 40), g_docMarkup, true)) { g_docMarkup = !g_docMarkup; g_docEraser = false; }
    if (g_docMarkup) {
        for (int i = 0; i < 4; ++i) {
            const ImVec2 pc(va.x + 27 + i * 30.0f, va.y + 25);
            char id[16];
            std::snprintf(id, sizeof id, "##dpen%d", i);
            ImGui::SetCursorScreenPos(ImVec2(pc.x - 13, pc.y - 13));
            if (ImGui::InvisibleButton(id, ImVec2(26, 26))) { g_docPen = i; g_docEraser = false; }
            penDot(dl, pc, (i == 0 && d.light) ? IM_COL32(24, 26, 32, 255) : kDocPens[i], i == 3, g_docPen == i && !g_docEraser, ImGui::IsItemHovered());
            if (i == 0 && d.light) dl->AddCircle(pc, 10.0f, IM_COL32(255, 255, 255, 120), 0, 1.0f);   // (its edge, on the dark bar)
        }
        if (pill(dl, f, "##deraser", "Eraser", ImVec2(va.x + 138, va.y + 10), ImVec2(va.x + 210, va.y + 40), g_docEraser, true)) g_docEraser = !g_docEraser;
        if (pill(dl, f, "##dundo", "Undo", ImVec2(vb.x - 214, va.y + 10), ImVec2(vb.x - 158, va.y + 40), false, !g_docUndo.empty())) { marks = g_docUndo.back(); g_docUndo.pop_back(); g_notesChanged = true; }
        if (pill(dl, f, "##dclear", "Clear", ImVec2(vb.x - 152, va.y + 10), ImVec2(vb.x - 96, va.y + 40), false, !marks.empty())) { saveUndo(); marks.clear(); g_notesChanged = true; }
    }
    if (pill(dl, f, "##docfit", "Fit", ImVec2(vb.x - 154, vb.y - 42), ImVec2(vb.x - 104, vb.y - 12), false, true)) { g_docZoom = 1.0f; g_docPan = ImVec2(0, 0); }
    if (pill(dl, f, "##docout", "-", ImVec2(vb.x - 98, vb.y - 42), ImVec2(vb.x - 58, vb.y - 12), false, true)) g_docZoom = std::max(0.5f, g_docZoom * 0.8f);
    if (pill(dl, f, "##docin", "+", ImVec2(vb.x - 52, vb.y - 42), ImVec2(vb.x - 12, vb.y - 12), false, true)) g_docZoom = std::min(8.0f, g_docZoom * 1.25f);
}

// ---- Dispatch: the SimBrief briefing (its PDF), as the Flight Plan's "Briefing PDF" opens it, with the way back -------------------------
std::string fitText(ImFont* fnt, const std::string& t, float width);   // (below)
bool g_dispatchFromPlan = false;   // opened from the Flight Plan page: a button goes back to it
void openDispatch(bool fromPlan) { g_dispatchFromPlan = fromPlan; openApp(APP_DISPATCH); }
void dispatchPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const FlightPlan& p = s.plan;
    const float top = kStatusH + kHeaderH + 8;
    float x = 12;
    if (g_dispatchFromPlan) {
        if (pill(dl, f, "##dspback", "     Flight Plan", ImVec2(12, top), ImVec2(140, top + 30), false, true)) { g_dispatchFromPlan = false; openApp(APP_ROUTES); return; }
        arrow(dl, ImVec2(32, top + 15), true, kInk);   // back
        x = 152;
    }
    if (!p.loaded || p.pdfUrl.empty()) {
        textCentered(dl, f.bold, w * 0.5f, h * 0.45f - 10, kInk, "No briefing yet");
        textCentered(dl, f.small, w * 0.5f, h * 0.45f + 16, kMuted, p.loaded ? "SimBrief gave no PDF with this plan." : "Import your SimBrief plan on the Flight Plan page; its briefing shows here.");
        if (pill(dl, f, "##dspplan", "Open Flight Plan", ImVec2(w * 0.5f - 80, h * 0.45f + 44), ImVec2(w * 0.5f + 80, h * 0.45f + 74), true, true)) { g_dispatchFromPlan = false; openApp(APP_ROUTES); }
        return;
    }
    std::string title = p.origin + " \xE2\x86\x92 " + p.dest;
    if (!p.callsign.empty()) title = p.callsign + "   " + title;
    if (!p.importedAt.empty()) title += "   \xC2\xB7   imported " + p.importedAt;
    textAt(dl, f.bold, x + 4, top + 6, kInk, fitText(f.bold, title, w - 12 - 150 - x).c_str());
    webZoomControls(dl, f, s.browser, act, "dsp", w - 12 - 126, top - 1, top + 31, s.browser.status == 1 && !g_webHold);
    const float y0 = top + 40;
    pdfView(dl, f, s, 12, y0, w - 24, h - 12 - y0, act, kGroupDispatch, p.pdfUrl);
}

// ---- Flight Plan: the latest SimBrief plan (Import uses the SimBrief ID saved in Settings, Data); the briefing PDF in a tab -------------
float g_fplScroll = 0;

// ---- flight progress: along the imported route, from where the aircraft is (the nearest leg, then the fixes left)
struct Progress { bool ok = false; float frac = 0; double flownNm = 0, leftNm = 0; int etaMin = -1; };
double nmBetween(double la1, double lo1, double la2, double lo2) {
    const double r = kPi / 180.0, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * 3440.065 * std::asin(std::min(1.0, std::sqrt(a)));
}
Progress flightProgress(const SimState& s) {
    Progress pr;
    const FlightPlan& p = s.plan;
    const MapState& m = s.map;
    if (!p.loaded || p.legs.size() < 2 || !m.havePos) return pr;
    std::vector<double> cum(p.legs.size(), 0);   // the route's length up to each fix
    for (size_t i = 1; i < p.legs.size(); ++i) cum[i] = cum[i - 1] + nmBetween(p.legs[i - 1].lat, p.legs[i - 1].lon, p.legs[i].lat, p.legs[i].lon);
    const double total = cum.back();
    if (total < 1) return pr;
    // the leg the aircraft is on: the one whose ends it is closest to between (the least detour)
    size_t best = 1;
    double bestDetour = 1e18;
    for (size_t i = 1; i < p.legs.size(); ++i) {
        const double a = nmBetween(m.lat, m.lon, p.legs[i - 1].lat, p.legs[i - 1].lon), b = nmBetween(m.lat, m.lon, p.legs[i].lat, p.legs[i].lon);
        const double detour = a + b - (cum[i] - cum[i - 1]);
        if (detour < bestDetour) { bestDetour = detour; best = i; }
    }
    const double toNext = nmBetween(m.lat, m.lon, p.legs[best].lat, p.legs[best].lon);
    pr.leftNm = std::max(0.0, toNext + (total - cum[best]));
    pr.flownNm = std::max(0.0, total - pr.leftNm);
    pr.frac = (float)std::max(0.0, std::min(1.0, pr.flownNm / total));
    if (m.gsKt > 40) pr.etaMin = (int)std::lround(pr.leftNm / m.gsKt * 60.0);
    pr.ok = true;
    return pr;
}
// the bar: departure, the line flown (in the accent colour), the aircraft on it, destination; the figures over it
std::string fitText(ImFont* fnt, const std::string& t, float width);   // (below)
void progressStrip(ImDrawList* dl, const Fonts& f, const SimState& s, const Progress& pr, ImVec2 a, ImVec2 b, bool compact) {
    const FlightPlan& p = s.plan;
    const float cy = compact ? b.y - 12 : (a.y + b.y) * 0.5f + 4;
    const float lw = f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, p.origin.c_str()).x, rw = f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, p.dest.c_str()).x;
    const float x0 = a.x + lw + 12, x1 = b.x - rw - 12;
    textAt(dl, f.bold, a.x, std::floor(cy - fontPx(f.bold) * 0.5f), kInk, p.origin.c_str());
    textAt(dl, f.bold, b.x - rw, std::floor(cy - fontPx(f.bold) * 0.5f), kInk, p.dest.c_str());
    dl->AddRectFilled(ImVec2(x0, cy - 2.5f), ImVec2(x1, cy + 2.5f), IM_COL32(255, 255, 255, 40), 3.0f);
    const float px = x0 + (x1 - x0) * pr.frac;
    dl->AddRectFilled(ImVec2(x0, cy - 2.5f), ImVec2(px, cy + 2.5f), accentCol(255), 3.0f);
    dl->AddCircleFilled(ImVec2(x0, cy), 4.0f, accentCol(255), 0);
    dl->AddCircleFilled(ImVec2(x1, cy), 4.0f, IM_COL32(255, 255, 255, 120), 0);
    // the aircraft: its own outline (the drawing the map uses), small, nose along the bar; an arrowhead if there is no drawing
    float sl = 0, ss = 0;
    if (const Shape* sh = findShape(s.acfIcao, sl, ss)) {
        const float L = 24.0f, k = L / std::max(1.0f, std::max(sl, ss));   // fits 24 px whichever is longer, length or span
        const int v0 = dl->VtxBuffer.Size;
        drawShape(dl, *sh, ImVec2(px, cy - sl * k * 0.5f), k * ss / std::max(1.0f, sh->span), k * sl / std::max(1.0f, sh->len), IM_COL32(255, 255, 255, 255), IM_COL32(150, 162, 186, 255));
        turnSince(dl, v0, ImVec2(px, cy), 1.5707963f);   // nose up -> nose right
    } else dl->AddTriangleFilled(ImVec2(px + 9, cy), ImVec2(px - 6, cy - 7), ImVec2(px - 6, cy + 7), IM_COL32(255, 255, 255, 255));
    char t[96];
    if (pr.etaMin >= 0) std::snprintf(t, sizeof t, "%.0f nm flown  \xC2\xB7  %.0f nm to go  \xC2\xB7  %d:%02d left", pr.flownNm, pr.leftNm, pr.etaMin / 60, pr.etaMin % 60);
    else std::snprintf(t, sizeof t, "%.0f nm flown  \xC2\xB7  %.0f nm to go", pr.flownNm, pr.leftNm);
    // never over the aircraft: in the pull-down, on the "Flight" title's line (right of it); on the page, above the bar on the side
    // the aircraft is not (right while it is in the first half, left after)
    // (on the page: lifted clear of the aircraft's top, which reaches 13 px above the bar)
    const float ty = std::min(a.y - 2, std::floor(cy - 13 - fontPx(f.small) - 2));
    if (compact) {   // beside the box's "Flight" title (fitted to the room right of it, never over it or the aircraft below)
        const float room = b.x - (a.x + f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, "Flight").x + 14);
        textRight(dl, f.small, b.x, a.y - 21, kMuted, fitText(f.small, t, room).c_str());
    }
    else if (pr.frac < 0.5f) textRight(dl, f.small, b.x, ty, kMuted, t);
    else textAt(dl, f.small, a.x, ty, kMuted, t);
}
double g_previewT0 = 0;   // when the plan preview window appeared (it grows in)
float rowStep = 21;   // the Flight Plan rows' spacing (the fuel and weights card closes it up)
void flightPlanBody(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const FlightPlan& p = s.plan;
    const float top = kStatusH + kHeaderH + 8;
    const bool haveId = !g_settings.simbriefUser.empty();
    // the buttons, along the top right
    float bx = w - 12;
    auto button = [&](const char* id, const char* label, bool on, bool enabled) {
        const float bw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, label).x + 30;
        bx -= bw;
        const bool hit = pill(dl, f, id, label, ImVec2(bx, top), ImVec2(bx + bw, top + 32), on, enabled);
        bx -= 6;
        return hit;
    };
    // Import: the window at once on a plan dismissed earlier, and SimBrief asked for anything newer (its answer updates the window)
    const bool waiting = s.planPreview.loaded && !s.planPreviewOpen;
    if (button("##fpimport", p.busy ? "Importing..." : "Import from SimBrief", true, (haveId || waiting) && !p.busy)) { act.planReview = waiting; act.planImport = haveId; }
    if (button("##fpclear", "Clear", false, p.loaded)) act.planClear = true;
    if (button("##fppdf", "Briefing PDF", false, p.loaded && !p.pdfUrl.empty())) { openDispatch(true); return; }
    if (button("##fpmap", "Show on map", false, p.loaded && !p.legs.empty())) {
        double la = 0, lo = 0;
        for (const FlightPlan::Leg& l : p.legs) { la += l.lat; lo += l.lon; }
        showOnMap(la / p.legs.size(), lo / p.legs.size());
        g_mapRange = rangeIndexOf(160);
        return;
    }
    if (!p.loaded) {
        textAt(dl, f.title, 16, top + 2, kInk, "No flight plan");
        const char* why = !haveId ? "Enter your SimBrief ID in Settings, Data, then Import." : p.status.empty() ? "Import your latest SimBrief flight plan." : p.status.c_str();
        dl->AddText(f.body, fontPx(f.body), ImVec2(16, top + 50), kMuted, why, nullptr, w - 32);
        return;
    }
    // the flight: from, to, who
    {
        char b[160];
        std::snprintf(b, sizeof b, "%s \xE2\x86\x92 %s", p.origin.c_str(), p.dest.c_str());
        textAt(dl, f.title, 16, top, kInk, b);
        std::string who = p.callsign;
        if (!p.aircraft.empty()) who += (who.empty() ? "" : "  \xC2\xB7  ") + p.aircraft;
        if (!p.registration.empty()) who += "  \xC2\xB7  " + p.registration;
        if (!p.importedAt.empty()) who += "  \xC2\xB7  imported " + p.importedAt;
        textAt(dl, f.small, 16, top + 26, kMuted, who.c_str());
    }
    const Progress pr = flightProgress(s);
    float y0 = top + 48;
    const float bot = h - 12;
    if (pr.ok) {   // the flight's progress, across the page under the title
        progressStrip(dl, f, s, pr, ImVec2(16, top + 50), ImVec2(w - 16, top + 76), false);
        y0 += 34;
    }
    // the route
    const float c1 = 12, w1 = 300;
    card(dl, ImVec2(c1, y0), ImVec2(c1 + w1, bot), f, "ROUTE");
    float y = y0 + 32;
    auto row = [&](float x0, float x1, const char* k, const std::string& v) {
        if (v.empty()) return;
        textAt(dl, f.small, x0, y, kMuted, k);
        textRight(dl, f.bold, x1, y, kInk, v.c_str());
        y += rowStep;
    };
    {
        const std::string dep = p.originRunway.empty() ? p.originName : p.originName + "  rwy " + p.originRunway;
        const std::string arr = p.destRunway.empty() ? p.destName : p.destName + "  rwy " + p.destRunway;
        dl->AddText(f.small, fontPx(f.small), ImVec2(c1 + 14, y), kMuted, ("From " + dep).c_str(), nullptr, w1 - 28); y += 18;
        dl->AddText(f.small, fontPx(f.small), ImVec2(c1 + 14, y), kMuted, ("To " + arr).c_str(), nullptr, w1 - 28); y += 24;
        const std::string route = (p.sid.empty() ? "" : p.sid + " ") + p.route + (p.star.empty() ? "" : " " + p.star);
        const ImVec2 rs = f.mono->CalcTextSizeA(fontPx(f.mono), 1e9f, w1 - 28, route.c_str());
        const float rh = std::min(rs.y, 5 * fontPx(f.mono) * 1.2f);
        dl->PushClipRect(ImVec2(c1 + 10, y), ImVec2(c1 + w1 - 10, y + rh + 2), true);
        dl->AddText(f.mono, fontPx(f.mono), ImVec2(c1 + 14, y), IM_COL32(255, 214, 120, 255), route.c_str(), nullptr, w1 - 28);
        dl->PopClipRect();
        y += rh + 12;
    }
    row(c1 + 14, c1 + w1 - 14, "Cruise", p.cruise);
    row(c1 + 14, c1 + w1 - 14, "Distance", p.distanceNm.empty() ? "" : p.distanceNm + " nm");
    row(c1 + 14, c1 + w1 - 14, "Time en route", p.ete);
    row(c1 + 14, c1 + w1 - 14, "Cost index", p.costIndex);
    row(c1 + 14, c1 + w1 - 14, "Average wind", p.avgWind);
    row(c1 + 14, c1 + w1 - 14, "Alternate", p.alternate);
    // fuel and weights
    const float c2 = c1 + w1 + 10, w2 = 200;
    card(dl, ImVec2(c2, y0), ImVec2(c2 + w2, bot), f, "FUEL AND WEIGHTS");
    y = y0 + 32;
    const std::string u = p.units.empty() ? "" : " " + p.units;
    auto withUnit = [&](const std::string& v) { return v.empty() ? v : v + u; };
    auto ofMax = [&](const std::string& v, const std::string& mx) { return v.empty() ? v : mx.empty() ? v + u : v + " / " + mx + u; };
    {   // the rows close up to fit the card: fuel, then the load, then the weights against their limits
        int n = 0;
        for (const std::string* v : {&p.blockFuel, &p.tripFuel, &p.reserveFuel, &p.altnFuel, &p.taxiFuel, &p.extraFuel, &p.oew, &p.paxWeight, &p.bagWeight, &p.freight, &p.payload, &p.zfw, &p.tow, &p.ldw}) n += !v->empty();
        rowStep = std::max(15.0f, std::min(21.0f, (bot - y - 30) / std::max(1, n)));
    }
    row(c2 + 14, c2 + w2 - 14, "Block", withUnit(p.blockFuel));
    row(c2 + 14, c2 + w2 - 14, "Trip", withUnit(p.tripFuel));
    row(c2 + 14, c2 + w2 - 14, "Reserve", withUnit(p.reserveFuel));
    row(c2 + 14, c2 + w2 - 14, "Alternate", withUnit(p.altnFuel));
    row(c2 + 14, c2 + w2 - 14, "Taxi", withUnit(p.taxiFuel));
    row(c2 + 14, c2 + w2 - 14, "Extra", withUnit(p.extraFuel));
    y += 10;
    dl->AddLine(ImVec2(c2 + 14, y - 6), ImVec2(c2 + w2 - 14, y - 6), IM_COL32(255, 255, 255, 20));
    row(c2 + 14, c2 + w2 - 14, "Empty (OEW)", withUnit(p.oew));
    row(c2 + 14, c2 + w2 - 14, p.paxCount.empty() ? "Passengers" : ("Passengers (" + p.paxCount + ")").c_str(), withUnit(p.paxWeight));
    row(c2 + 14, c2 + w2 - 14, p.bags.empty() ? "Bags" : ("Bags (" + p.bags + ")").c_str(), withUnit(p.bagWeight));
    row(c2 + 14, c2 + w2 - 14, "Freight", withUnit(p.freight));
    row(c2 + 14, c2 + w2 - 14, "Payload", withUnit(p.payload));
    y += 10;
    dl->AddLine(ImVec2(c2 + 14, y - 6), ImVec2(c2 + w2 - 14, y - 6), IM_COL32(255, 255, 255, 20));
    row(c2 + 14, c2 + w2 - 14, "ZFW / max", ofMax(p.zfw, p.maxZfw));   // (short names: the figures and their limits are wide)
    row(c2 + 14, c2 + w2 - 14, "TOW / max", ofMax(p.tow, p.maxTow));
    row(c2 + 14, c2 + w2 - 14, "LW / max", ofMax(p.ldw, p.maxLdw));
    rowStep = 21;
    // the navigation log, scrolling
    const float c3 = c2 + w2 + 10, w3 = w - 12 - c3;
    const ImVec2 la(c3, y0), lb(c3 + w3, bot);
    card(dl, la, lb, f, "NAVLOG");
    ImGui::SetCursorScreenPos(la);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##fplog", ImVec2(lb.x - la.x, lb.y - la.y));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_fplScroll -= ImGui::GetIO().MouseWheel * 44;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) g_fplScroll -= ImGui::GetIO().MouseDelta.y;
    // a fix with a SID's or STAR's crossing restriction takes a second line (in the theme's colour) under it
    const float rowH = 22, crossH = 15, listTop = y0 + 50;
    float total = 0;
    for (const FlightPlan::Leg& l : p.legs) total += rowH + (l.cross.empty() ? 0 : crossH);
    g_fplScroll = std::max(0.0f, std::min(g_fplScroll, std::max(0.0f, total - (bot - 8 - listTop))));
    const float xs[4] = {c3 + 14, c3 + 14 + w3 * 0.30f, c3 + 14 + w3 * 0.56f, c3 + w3 - 14};
    textAt(dl, f.small, xs[0], y0 + 30, kMuted, "FIX");
    textAt(dl, f.small, xs[1], y0 + 30, kMuted, "VIA");
    textAt(dl, f.small, xs[2], y0 + 30, kMuted, "ALT");
    textRight(dl, f.small, xs[3], y0 + 30, kMuted, "NM  TIME");
    dl->PushClipRect(ImVec2(c3 + 2, listTop - 2), ImVec2(c3 + w3 - 2, bot - 4), true);
    float at = 0;
    for (size_t i = 0; i < p.legs.size(); ++i) {
        const FlightPlan::Leg& l = p.legs[i];
        const float hRow = rowH + (l.cross.empty() ? 0 : crossH);
        const float ry = listTop + at - g_fplScroll;
        at += hRow;
        if (ry < listTop - hRow || ry > bot) continue;
        if (i % 2) dl->AddRectFilled(ImVec2(c3 + 6, ry - 2), ImVec2(c3 + w3 - 6, ry + hRow - 4), IM_COL32(255, 255, 255, 8), 4.0f);
        if (!l.cross.empty()) textAt(dl, f.small, xs[0] + 10, ry + rowH - 4, accentLite(255), fitText(f.small, l.cross, xs[3] - xs[0] - 10).c_str());
        textAt(dl, f.bold, xs[0], ry, kInk, l.ident.c_str());
        textAt(dl, f.small, xs[1], ry + 1, kMuted, l.airway.c_str());
        textAt(dl, f.small, xs[2], ry + 1, kInk, l.alt.c_str());
        const std::string tail = l.legNm + "  " + l.totalTime;
        textRight(dl, f.small, xs[3], ry + 1, kInk, tail.c_str());
    }
    dl->PopClipRect();
}

void glass(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float alpha = 1.0f);
std::string fitText(ImFont* fnt, const std::string& t, float width);
// The plan SimBrief has, before it is taken: the route as a chain (departure, destination, the alternate to divert to) and what
// matters for the flight, then Dismiss (the flight plan stays as it was) or Import (this becomes the flight plan)
void planPreview(ImDrawList* dl, const Fonts& f, const FlightPlan& p, float w, float h, Actions& act) {
    const float t = smooth01((float)(ImGui::GetTime() - g_previewT0) / 0.25f);
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(120 * t)));
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##pvback", ImVec2(w, h - kStatusH));   // nothing under the window takes a press
    const float pw = std::min(640.0f, w - 40), ph = std::min(360.0f, h - kStatusH - 30);
    const float sc = 0.94f + 0.06f * t;
    const ImVec2 c(w * 0.5f, kStatusH + (h - kStatusH) * 0.5f);
    const ImVec2 a(std::floor(c.x - pw * 0.5f * sc), std::floor(c.y - ph * 0.5f * sc)), b(std::floor(c.x + pw * 0.5f * sc), std::floor(c.y + ph * 0.5f * sc));
    dl->AddRectFilled(a, b, themeDark((int)(235 * t)), 20.0f);   // solid enough that the page under it does not read through
    glass(dl, a, b, 20.0f, t);
    const int A = (int)(255 * t);
    auto col = [&](ImU32 c0) { return (c0 & 0x00FFFFFF) | ((ImU32)(((c0 >> 24) & 255) * t) << 24); };
    const float x0 = a.x + 22, x1 = b.x - 22;
    float y = a.y + 16;
    textAt(dl, f.bold, x0, y, col(kInk), "SimBrief flight plan");
    if (!p.importedAt.empty()) textRight(dl, f.small, x1, y + 3, col(kMuted), ("Fetched " + p.importedAt).c_str());
    y += 24;
    std::string who = p.callsign;
    for (const std::string& x : {p.aircraft, p.registration}) if (!x.empty()) who += (who.empty() ? "" : "  \xC2\xB7  ") + x;
    if (!who.empty()) textAt(dl, f.small, x0, y, col(kMuted), fitText(f.small, who, x1 - x0).c_str());
    y += 30;
    // the chain: departure -- destination - - alternate
    const bool alt = !p.alternate.empty();
    const float nx[3] = {x0 + 40, alt ? x0 + (x1 - x0) * 0.60f : x1 - 40, x1 - 40};
    const ImU32 nodeCol[3] = {accentCol(A), IM_COL32(80, 214, 124, A), IM_COL32(255, 170, 60, A)};
    const float cy = y + 12;
    {   // the flight: a solid line with the aircraft on it, the distance and time over it
        dl->AddLine(ImVec2(nx[0] + 12, cy), ImVec2(nx[1] - 12, cy), IM_COL32(200, 214, 240, (int)(170 * t)), 2.5f);
        const ImVec2 mid((nx[0] + nx[1]) * 0.5f, cy);
        // the plan's aircraft: its own outline (as on the route bar and the map), nose along the line; an arrowhead if none
        float sl = 0, ss = 0;
        if (const Shape* sh = findShape(p.aircraft, sl, ss)) {
            const float L = 26.0f, k = L / std::max(1.0f, std::max(sl, ss));
            const int v0 = dl->VtxBuffer.Size;
            drawShape(dl, *sh, ImVec2(mid.x, cy - sl * k * 0.5f), k * ss / std::max(1.0f, sh->span), k * sl / std::max(1.0f, sh->len), IM_COL32(255, 255, 255, A), IM_COL32(150, 162, 186, A));
            turnSince(dl, v0, mid, 1.5707963f);   // nose up -> nose right (towards the destination)
        } else dl->AddTriangleFilled(ImVec2(mid.x + 9, cy), ImVec2(mid.x - 6, cy - 6), ImVec2(mid.x - 6, cy + 6), IM_COL32(220, 230, 250, A));
        std::string over;
        if (!p.distanceNm.empty()) over = p.distanceNm + " nm";
        if (!p.ete.empty()) over += (over.empty() ? "" : "  \xC2\xB7  ") + p.ete;
        if (!over.empty()) textCentered(dl, f.small, mid.x, cy - 22, col(kMuted), over.c_str());
    }
    if (alt) {   // the diversion: dashed
        for (float x = nx[1] + 14; x < nx[2] - 14; x += 10) dl->AddLine(ImVec2(x, cy), ImVec2(std::min(x + 5, nx[2] - 14), cy), IM_COL32(255, 170, 60, (int)(200 * t)), 2.0f);
        textCentered(dl, f.small, (nx[1] + nx[2]) * 0.5f, cy - 22, col(IM_COL32(255, 190, 110, 255)), "diversion");
    }
    const std::string ids[3] = {p.origin, p.dest, p.alternate}, names[3] = {p.originName, p.destName, p.alternateName};
    const std::string rwy[3] = {p.originRunway.empty() ? "" : "RWY " + p.originRunway, p.destRunway.empty() ? "" : "RWY " + p.destRunway, ""};
    const char* role[3] = {"DEPARTURE", "DESTINATION", "ALTERNATE"};
    for (int i = 0; i < (alt ? 3 : 2); ++i) {
        dl->AddCircleFilled(ImVec2(nx[i], cy), 9.0f, nodeCol[i], 0);
        dl->AddCircle(ImVec2(nx[i], cy), 13.0f, (nodeCol[i] & 0x00FFFFFF) | ((ImU32)(90 * t) << 24), 0, 2.0f);
        const float colW = i == 0 ? (nx[1] - nx[0]) * 0.9f : 150.0f;
        textCentered(dl, f.small, nx[i], cy + 20, col(kMuted), role[i]);
        textCentered(dl, f.title, nx[i], cy + 36, col(kInk), ids[i].empty() ? "----" : ids[i].c_str());
        if (!names[i].empty()) textCentered(dl, f.small, nx[i], cy + 64, col(kMuted), fitText(f.small, names[i], colW).c_str());
        if (!rwy[i].empty()) textCentered(dl, f.small, nx[i], cy + 80, col(kInk), rwy[i].c_str());
    }
    y = cy + 104;
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(255, 255, 255, (int)(26 * t)));
    y += 10;
    // what matters for the flight, in two rows of four
    const std::string u = p.units.empty() ? "" : " " + p.units;
    const std::string cells[8][2] = {{"Cruise", p.cruise}, {"Cost index", p.costIndex}, {"Avg wind", p.avgWind}, {"SID / STAR", p.sid + (p.star.empty() ? "" : " / " + p.star)},
                                     {"Block fuel", p.blockFuel.empty() ? "" : p.blockFuel + u}, {"Trip fuel", p.tripFuel.empty() ? "" : p.tripFuel + u},
                                     {"ZFW", p.zfw.empty() ? "" : p.zfw + u}, {"TOW", p.tow.empty() ? "" : p.tow + u}};
    const float cw = (x1 - x0) / 4;
    for (int i = 0; i < 8; ++i) {
        const float cx = x0 + (i % 4) * cw, ry = y + (i / 4) * 40;
        textAt(dl, f.small, cx, ry, col(kMuted), cells[i][0].c_str());
        const std::string v = cells[i][1].empty() || cells[i][1] == " / " ? std::string("-") : cells[i][1];
        textAt(dl, f.bold, cx, ry + 15, col(kInk), fitText(f.bold, v, cw - 8).c_str());
    }
    y += 82;
    if (!p.route.empty()) {
        textAt(dl, f.small, x0, y, col(kMuted), "Route");
        textAt(dl, f.small, x0 + 46, y, col(kInk), fitText(f.small, p.route, x1 - x0 - 46).c_str());
    }
    // Dismiss or Import
    const float by = b.y - 50, bw = (x1 - x0 - 12) * 0.5f;
    if (pill(dl, f, "##pvdismiss", "Dismiss", ImVec2(x0, by), ImVec2(x0 + bw, by + 34), false, true)) act.planDismiss = true;
    if (pill(dl, f, "##pvimport", "Import", ImVec2(x1 - bw, by), ImVec2(x1, by + 34), true, true)) act.planAccept = true;
}
void flightPlanPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const bool preview = s.planPreview.loaded && s.planPreviewOpen;
    static bool shown = false;
    if (preview && !shown) g_previewT0 = ImGui::GetTime();
    shown = preview;
    // the page; while the window is up the pointer is hidden from it (nothing under the window is pressed)
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos; const bool down = io.MouseDown[0], clicked = io.MouseClicked[0], released = io.MouseReleased[0]; const float wh = io.MouseWheel;
    if (preview) { io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX); io.MouseDown[0] = io.MouseClicked[0] = io.MouseReleased[0] = false; io.MouseWheel = 0; }
    flightPlanBody(dl, f, s, w, h, act);
    if (preview) { io.MousePos = mp; io.MouseDown[0] = down; io.MouseClicked[0] = clicked; io.MouseReleased[0] = released; io.MouseWheel = wh; }
    if (preview) planPreview(dl, f, s.planPreview, w, h, act);
}

// Where a ToLiss's doors are along the fuselage, as fractions of its length from the nose (cabin doors front to back, then the
// cargo doors: forward, aft, bulk; all cargo doors are on the right).
// where a pair of cabin doors is, as a fraction of the length from the nose: the ToLiss types' own layouts (from the types' airport
// planning drawings, roughly: door centres over overall length), else a typical airliner's
float paxDoorAt(const std::string& icao, int pair, int pairs) {
    struct Lay { const char* icao; int n; float at[4]; };
    static const Lay lays[] = {
        {"A319", 2, {0.150f, 0.812f}}, {"A320", 2, {0.135f, 0.823f}}, {"A20N", 2, {0.135f, 0.823f}},
        {"A321", 4, {0.113f, 0.300f, 0.640f, 0.860f}}, {"A21N", 4, {0.113f, 0.300f, 0.640f, 0.860f}},   // 2: just ahead of the wing
        {"A339", 4, {0.120f, 0.272f, 0.633f, 0.857f}}, {"A346", 4, {0.108f, 0.250f, 0.590f, 0.850f}},
    };
    for (const Lay& l : lays) if (icao == l.icao && pairs == l.n) return l.at[std::max(0, std::min(pair, l.n - 1))];
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
    for (int k = 0; k < jwDoors && k < 3; ++k) anyDocked = anyDocked || g.jwDocked[k];   // each door's own (openSAM's or X-Plane's)
    const float boxTop = a.y + 34, boxBot = e.y - (doors && jwDoors > 1 ? 30 : 14);
    const float px = std::min((boxBot - boxTop) / acLen, (leftW - (doors ? 150 : 40)) / acSpan);   // room at the left for the bubbles
    const ImVec2 nose((a.x + e.x) * 0.5f + (doors ? 30.0f : 0.0f), std::floor((boxTop + boxBot - acLen * px) * 0.5f));
    // the drawing, resized to the type's length and span (kx, ky: from the drawing's metres to the type's)
    const float kx = shape ? acSpan / shape->span : 1.0f, ky = shape ? acLen / shape->len : 1.0f;
    // white in every theme, with neutral grey lines (no blue that reads as tinted next to a warm theme)
    if (shape) drawShape(dl, *shape, nose, px * kx, px * ky, IM_COL32(246, 247, 249, 255), IM_COL32(128, 132, 140, 255));
    else planeTopDown(dl, nose, px, spec, IM_COL32(246, 247, 249, 255), IM_COL32(128, 132, 140, 255));
    const float hw0 = shape ? fuselageHalf(*shape, 0.25f, 0) : -1;   // the fuselage's half-width forward of the wing, drawing metres
    const float hwPx = (shape && hw0 > 0 ? hw0 * kx : spec.fus * 0.5f) * px;
    if (doors) {
        const int pairs = (g.paxDoors + 1) / 2;
        float lastBy = -1e9f;   // the bubble above's place: bubbles keep apart (their door's block stays on the door)
        for (int k = 0; k < jwDoors && k < 3; ++k) {
            const int pax = g.jwDoorPax[k];
            const float frac = paxDoorAt(s.acfIcao, pax / 2, std::max(1, pairs));
            if (frac > 0.45f) continue;   // a jetway never goes to a door behind the wing
            const float y = nose.y + frac * acLen * px;
            float hw = hwPx;   // the fuselage's edge at this door (it narrows towards the tail)
            if (shape && hw0 > 0) { const float hh = fuselageHalf(*shape, frac, hw0); if (hh > 0) hw = hh * kx * px; }
            const float side = nose.x - hw;
            const bool docked = g.jwDocked[k];
            const bool open = g.toliss && pax >= 0 && pax < 8 && g.paxDoor[pax] == 2;
            // the block: the door, green while a jetway is on it, blue while the door is open without one
            const ImU32 blockCol = docked ? kGood : open ? accentLite(255) : IM_COL32(140, 156, 184, 255);
            dl->AddRectFilled(ImVec2(side - 9, y - 7), ImVec2(side + 1, y + 7), blockCol, 2.0f);
            // the bubble, at the card's left edge, with a leader to the block
            const float bw = 108, bx = a.x + 12;
            const float by = std::max(y, lastBy + 54);   // under the one above and its line of text (the leader angles to the door)
            lastBy = by;
            dl->AddLine(ImVec2(bx + bw, by), ImVec2(bx + bw + 14, by), IM_COL32(255, 255, 255, 46), 1.0f);
            dl->AddLine(ImVec2(bx + bw + 14, by), ImVec2(side - 9, y), IM_COL32(255, 255, 255, 46), 1.0f);
            char id[16], lbl[16];
            std::snprintf(id, sizeof id, "##jw%d", k);
            std::snprintf(lbl, sizeof lbl, "%s", docked ? "Disconnect" : "Connect");
            const bool moving = g.jwMoving[k];
            const bool canPress = gnd && !moving && (!g.openSam || g.jetway == 1 || g.jetway == 2);
            dl->AddRectFilled(ImVec2(bx, by - 14), ImVec2(bx + bw, by + 14), themeTone(16, 30, 56, 255), 14.0f);
            if (pill(dl, f, id, lbl, ImVec2(bx, by - 14), ImVec2(bx + bw, by + 14), docked, canPress)) { act.ground = GA_JETWAY_DOOR; act.groundIndex = k; }
            // under the bubble: the door's name and what the jetway is doing
            // as openSAM shows it: connected only once the jetway is there, "jetway moving" until then (both kinds of jetway)
            // (openSAM says "no jetway" while the aircraft is not ready for one: then the jetway status says why)
            const bool planeNotReady = !g.onGround || g.groundSpeedKt > 1.0f || g.beacon || g.enginesRunning || (!g.parkBrake && !g.chocks);
            const char* st = moving ? "Jetway moving"
                           : !g.openSam ? (docked ? (g.toliss ? "Connected, door open" : "Connected") : planeNotReady ? "Check status" : "X-Plane jetway")
                           : docked ? (g.toliss ? "Docked, door open" : "Docked") : planeNotReady ? "Check status" : g.jetway == 1 ? "Jetway ready" : g.jetway == 0 ? "No jetway here" : g.jetway == 2 ? "Jetway at the other door" : "Jetway moving";
            std::snprintf(b, sizeof b, "%d%c  \xC2\xB7  %s", pax / 2 + 1, 'L', st);
            textAt(dl, f.small, bx + 4, by + 17, docked ? kGood : moving ? kWarn : kMuted, b);
        }
        if (jwDoors > 1) textCentered(dl, f.small, (a.x + e.x) * 0.5f, e.y - 24, kMuted, "Each door connects on its own; the jetways leave with the last door.");
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
        // the lights: On and Off beside the name (On lit while they are on, Off while off; On comes back to the level they had),
        // and a slider in steps of 10 % under them
        textAt(dl, f.bold, cx0, y + 4, kInk, "Cabin lights");
        char lv[16];
        const int pct = g.cabinLight < 0 ? -1 : (int)std::lround(g.cabinLight * 100);
        static int lastOn = 100;   // the level they were last on at
        if (pct > 0) lastOn = pct;
        if (pct < 0) std::snprintf(lv, sizeof lv, "-");
        else if (pct == 0) std::snprintf(lv, sizeof lv, "Off");
        else std::snprintf(lv, sizeof lv, "%d %%", pct);
        textAt(dl, f.bold, cx0 + 112, y + 4, pct > 0 ? kInk : kMuted, lv);
        if (g.cabinLight >= 0) {
            const float bw = 64;
            const bool autoOn = g.cabinAuto == 1;
            if (pill(dl, f, "##clon", "On", ImVec2(cx1 - 2 * bw - 8, y), ImVec2(cx1 - bw - 8, y + 30), pct > 0 && !autoOn, true) && (pct <= 0 || autoOn)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = pct > 0 ? pct : lastOn; }
            if (pill(dl, f, "##cloff", "Off", ImVec2(cx1 - bw, y), ImVec2(cx1, y + 30), pct == 0 && !autoOn, true) && (pct != 0 || autoOn)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = 0; }
            if (g.cabinAuto >= 0 && pill(dl, f, "##clauto", "Auto", ImVec2(cx1 - 3 * bw - 16, y), ImVec2(cx1 - 2 * bw - 16, y + 30), autoOn, true)) act.ground = GA_CABIN_AUTO;
        }
        y += 36;
        if (g.cabinLight >= 0) {
            // the track, its fill, ticks at every 10 % and the handle; pressing or dragging on it sets the level, in 10 % clicks
            const float tx0 = cx0 + 10, tx1 = cx1 - 10, ty = y + 14;
            ImGui::SetCursorScreenPos(ImVec2(cx0, y));
            ImGui::InvisibleButton("##cablight", ImVec2(cx1 - cx0, 28));
            static float knob = -1;   // where the knob is drawn (0..1): it glides to the level, and follows the finger
            const float dt = ImGui::GetIO().DeltaTime;
            float target = std::max(0, pct) / 100.0f;
            if (ImGui::IsItemActive()) {
                const float t = std::max(0.0f, std::min(1.0f, (ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0)));
                const int want = (int)std::lround(t * 10) * 10;
                target = want / 100.0f;
                if (want != pct) { act.ground = GA_CABIN_LIGHT; act.groundIndex = want; }
                if (knob < 0) knob = t;
                knob += (t - knob) * (1.0f - std::exp(-dt * 30.0f));
            } else {
                if (knob < 0) knob = target;
                knob += (target - knob) * (1.0f - std::exp(-dt * 16.0f));
            }
            const float hx = tx0 + (tx1 - tx0) * knob;
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(tx1, ty + 3), IM_COL32(255, 255, 255, 40), 3.0f);
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(hx, ty + 3), IM_COL32(255, 214, 120, 255), 3.0f);
            for (int i = 0; i <= 10; ++i) {
                const float x = tx0 + (tx1 - tx0) * i / 10.0f;
                dl->AddLine(ImVec2(x, ty + 7), ImVec2(x, ty + (i % 5 == 0 ? 13 : 10)), IM_COL32(255, 255, 255, 90), 1.0f);
            }
            dl->AddCircleFilled(ImVec2(hx, ty), 10.0f, IM_COL32(245, 248, 255, 255), 0);
            dl->AddCircle(ImVec2(hx, ty), 10.0f, IM_COL32(255, 214, 120, 255), 0, 2.0f);
            y += 32;
            textAt(dl, f.small, cx0, y, kMuted, "Leave ToLiss Cabin Lights on Auto");
            y += 20;
        } else {
            textAt(dl, f.small, cx0, y, kMuted, "This aircraft does not offer its cabin lights.");
            y += 34;
        }
        dl->AddLine(ImVec2(cx0, y - 6), ImVec2(cx1, y - 6), IM_COL32(255, 255, 255, 18));
        // the slides
        textAt(dl, f.bold, cx0, y + 4, kInk, "Slides");
        if (g.slides >= 0) {
            const std::string st = std::string(g.slides == 1 ? "Armed" : "Disarmed") + (g.slidesAuto ? " (auto)" : "");
            textRight(dl, f.bold, cx1, y + 4, g.slides == 1 ? kWarn : kMuted, st.c_str());
        }
        y += 32;
        // Arm, Disarm, or Auto: the aircraft's own rule (ToLiss arms them once the beacon is on and the doors are shut, disarms parked)
        const float bw = std::floor((cx1 - cx0 - 16) / 3.0f);
        if (pill(dl, f, "##arm", "Arm", ImVec2(cx0, y), ImVec2(cx0 + bw, y + 30), !g.slidesAuto && g.slides == 1, true)) act.ground = GA_SLIDES_ARM;
        if (pill(dl, f, "##disarm", "Disarm", ImVec2(cx0 + bw + 8, y), ImVec2(cx0 + 2 * bw + 8, y + 30), !g.slidesAuto && g.slides == 0, true)) act.ground = GA_SLIDES_DISARM;
        if (pill(dl, f, "##sauto", "Auto", ImVec2(cx1 - bw, y), ImVec2(cx1, y + 30), g.slidesAuto, true)) act.ground = GA_SLIDES_AUTO;
        y += 42;
        // Cabin Ready: tells the aircraft the cabin is secure (the CABIN READY memo); greyed once given, until after takeoff or landing
        // always the same colour (only lighter while hovered): ToLiss keeps the memo once given, so there is nothing to show undone
        const ImVec2 ra(cx0, y), rb(cx1, std::min(split - 14, y + 44));
        {
            ImGui::SetCursorScreenPos(ra);
            const bool hit = ImGui::InvisibleButton("##cabready", ImVec2(rb.x - ra.x, rb.y - ra.y));
            const Ease ez = eased("##cabready", false, ImGui::IsItemHovered(), ImGui::IsItemActive());
            const float in = ez.down * 1.2f;
            dl->AddRectFilled(ImVec2(ra.x + in, ra.y + in), ImVec2(rb.x - in, rb.y - in), mixCol(accentCol(255), accentLite(255), ez.hot), 14.0f);
            textCentered(dl, f.bold, (ra.x + rb.x) * 0.5f, std::floor((ra.y + rb.y) * 0.5f - fontPx(f.bold) * 0.5f), IM_COL32(255, 255, 255, 255), "Cabin Ready");
            if (hit) act.ground = GA_CABIN_READY;
        }
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
                dl->AddCircleFilled(ImVec2(cx0 + 4, y + fontPx(f.body) * 0.5f + 1), 3.5f, kWarn, 0);
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
    if (s.wxEmbed != 1) textCentered(dl, f.body, c.x, c.y, themeText(190, 200, 220, 0.2f), "Opening WXPlane...");
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



// Settings' second view: who made what XPTablet is built from.
int g_settingsTab = 0;   // 0 General, 1 Display, 2 Data, 3 Credits, 4 Notifications, 5 Media (not on macOS)

float g_creditsScroll = 0;
void creditsPage(ImDrawList* dl, const Fonts& f, float left, float w, float h) {
    const float top = kStatusH + kHeaderH + 10;
    const ImVec2 ca(left, top), cb(w - 12, h - 12);
    card(dl, ca, cb, f, "CREDITS");
    // it scrolls: the wheel, or a drag
    ImGui::SetCursorScreenPos(ca);
    ImGui::SetNextItemAllowOverlap();   // (the scroll bar drawn over it takes its own presses)
    ImGui::InvisibleButton("##credits", ImVec2(cb.x - ca.x, cb.y - ca.y));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_creditsScroll -= ImGui::GetIO().MouseWheel * 40;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) g_creditsScroll -= ImGui::GetIO().MouseDelta.y;
    g_creditsScroll = std::max(0.0f, g_creditsScroll);
    dl->PushClipRect(ImVec2(ca.x, ca.y + 30), ImVec2(cb.x, cb.y - 2), true);
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
        {"Airport facts and CTAF", "OurAirports (ourairports.com) and its contributors", "Public domain"},
        {"Online traffic details", "VATSIM data feed (data.vatsim.net)", "Shown live, not stored"},
        {"Navaids and waypoints on the map", "X-Plane's navigation data (or the AIRAC in Custom Data, e.g. Navigraph's)", ""},
        {"Route on the map", "Your flight plan from SimBrief (simbrief.com)", ""},
        {"METAR reports", "NOAA / National Weather Service, Aviation Weather Center (aviationweather.gov)", "Public domain"},
        {"Weather page with WXPlane installed", "WXPlane by Alpine Aviation", ""},
        {"Airbus checklists", "FlightGear A320-family by Josh Davidson (Octal450), Jonathan Redpath and contributors (github.com/legoboyvdlp/A320-family), adapted", "GPL-2.0"},
        {"Icons and logos", "Provided through creatorsrepo.com", ""},
        {"App icons", "Material Design Icons by Pictogrammers (pictogrammers.com): cog, check-bold, file-document, star-four-points, airplane, airport, web, power, weather-partly-snowy-rainy", "Apache 2.0"},
        {"ForeFlight icon", "The ForeFlight logo is a trademark of ForeFlight LLC, used only to show their web app", "Trademark"},
        {"Navigraph icon", "The Navigraph logo is a trademark of Navigraph, used only to show their charts web app", "Trademark"},
        {"SkyVector icon", "The SkyVector logo is a trademark of SkyVector (Seattle Avionics), used only to show their web app", "Trademark"},
        {"Volanta icon", "The Volanta logo is a trademark of Orbx, used only to show their web map", "Trademark"},
        {"ChartFox icon", "The ChartFox logo is a trademark of ChartFox (chartfox.org), used only to show their charts web app", "Trademark"},
        {"SimBrief icon", "The SimBrief logo is a trademark of SimBrief (Navigraph), used only to show their web app", "Trademark"},
        {"VATSIM Radar icon", "The VATSIM Radar logo belongs to VATSIM Radar (vatsim-radar.com), used only to show their web map", "Trademark"},
        {"", "XPTablet is not made by or affiliated with ForeFlight, Navigraph, SkyVector, Volanta / Orbx, VATSIM Radar, ChartFox, SimBrief or VATSIM.", ""},
    };
    float y = top + 38 - g_creditsScroll;
    const float x0 = left + 18, xLic = w - 30, wrap = w - 30 - x0 - 150;
    for (const Credit& c : kCredits) {
        textAt(dl, f.bold, x0, y, kInk, c.what);
        if (*c.licence) textRight(dl, f.small, xLic, y + 2, kMuted, c.licence);
        const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, wrap, c.who);
        dl->AddText(f.small, fontPx(f.small), ImVec2(x0, y + 21), kMuted, c.who, nullptr, wrap);
        y += 21 + sz.y + 10;
    }
    textAt(dl, f.small, x0, y + 2, kMuted, "Full licence texts: LICENSES.txt and shapes/LICENSE-GPL-3.0.txt in the XPTablet folder.");
    dl->PopClipRect();
    const float content = y + 30 + g_creditsScroll - (top + 38);
    g_creditsScroll = std::min(g_creditsScroll, std::max(0.0f, content - (cb.y - ca.y - 40)));
    g_creditsScroll = scrollBar(dl, "##credbar", cb.x - 9, ca.y + 34, cb.y - 8, g_creditsScroll, content, cb.y - ca.y - 40);   // (draggable)
}

// ---- notifications, the pull-down (music and the notification list) and the Music app --------------------------------------------------
struct Note { int id; std::string title, body; int app; double t; bool banner = true; float dx = 0; bool dying = false; double dieT = 0;
              double awayT = -1; float awayFrom = 0; float yShown = -1e9f; };   // yShown: where its card is drawn (it glides up when one above goes)   // swiped up: sliding away since awayT (from awayFrom above its place)
std::vector<Note> g_notes;         // newest first
int g_noteId = 0;
double g_bannerDrag = -1;          // the banner being pushed up: where the press was (y)
float g_bannerDy = 0;
float g_ccScroll = 0;
const MediaState* g_media = nullptr;   // this frame's (for the status bar and the pull-down)
std::map<std::string, double> g_raSeen;   // traffic in TCAS RA range, and since when (a notification once per aircraft per minute)
int g_planSeqSeen = -1;

void postNote(const std::string& title, const std::string& body, int app) {
    if (!g_settings.notifyEnabled) return;
    Note n;
    n.id = ++g_noteId; n.title = title; n.body = body; n.app = app; n.t = ImGui::GetTime();
    // no pop-up while the page it leads to is already open (it is still in the list, in the pull-down)
    const bool here = app >= 0 && app < APP_COUNT && app != APP_HOME && app == g_app;
    n.banner = g_power == POWER_ON && !here;
    if (g_power == POWER_ON) ++g_chimeSeq;   // (a dark tablet stays quiet)
    if (n.banner) for (Note& o : g_notes) o.banner = false;   // one banner at a time: the newest
    g_notes.insert(g_notes.begin(), n);
    if (g_notes.size() > 40) g_notes.resize(40);
}
std::string agoText(double t) {
    const double s = ImGui::GetTime() - t;
    if (s < 60) return "now";
    char b[24];
    if (s < 3600) std::snprintf(b, sizeof b, "%dm ago", (int)(s / 60)); else std::snprintf(b, sizeof b, "%dh ago", (int)(s / 3600));
    return b;
}
// the pull-down's boxes, sized so the left column (Media, Brightness, Flight) ends clear of the bar at the bottom, as the list does
constexpr float kCcMediaH = 196, kCcBrightH = 88, kCcGap = 10, kCcBottomGap = 36;
// a thin scroll bar at x, from y0 to y1, for content taller than what shows (nothing when it all fits)
// It can be dragged (the thumb, or a press on the track moves it there) and returns the scroll, changed or not; it widens on hover.
float scrollBar(ImDrawList* dl, const char* id, float x, float y0, float y1, float scroll, float content, float shown) {
    if (content <= shown + 1 || y1 - y0 < 20) return scroll;
    const float track = y1 - y0, len = std::max(24.0f, track * shown / content), maxS = content - shown;
    ImGui::SetCursorScreenPos(ImVec2(x - 8, y0));
    ImGui::InvisibleButton(id, ImVec2(16, track));
    static float grabOff = 0;
    const float t0 = std::max(0.0f, std::min(1.0f, scroll / std::max(1.0f, maxS)));
    const float top0 = y0 + (track - len) * t0;
    const float my = ImGui::GetIO().MousePos.y;
    if (ImGui::IsItemActivated()) grabOff = (my >= top0 && my <= top0 + len) ? my - top0 : len * 0.5f;   // on the thumb: held where taken
    if (ImGui::IsItemActive()) scroll = std::max(0.0f, std::min(maxS, (my - grabOff - y0) / std::max(1.0f, track - len) * maxS));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    const float t = std::max(0.0f, std::min(1.0f, scroll / std::max(1.0f, maxS)));
    const float top = y0 + (track - len) * t, hw = hot ? 3.0f : 2.0f;
    dl->AddRectFilled(ImVec2(x - 1.5f, y0), ImVec2(x + 1.5f, y1), IM_COL32(255, 255, 255, hot ? 30 : 18), 2.0f);
    dl->AddRectFilled(ImVec2(x - hw, top), ImVec2(x + hw, top + len), hot ? accentLite(220) : IM_COL32(255, 255, 255, 120), hw);
    return scroll;
}
// glass, as iOS draws it: a light veil over what is behind, a hairline edge, a faint sheen at the top
void glass(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float alpha) {
    const int A = (int)(255 * alpha);
    dl->AddRectFilled(a, b, themeDark(215 * A / 255), r);   // a dark frosted base (theme's tone): what is behind barely shows
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 24 * A / 255), r);
    {   // the sheen at the top: a rounded band following the corners (a square one stuck out past them), fading downwards
        const float sh = std::min(std::max(r, 24.0f), (b.y - a.y) * 0.45f);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + sh), IM_COL32(255, 255, 255, 255), std::max(0.0f, r - 1), ImDrawFlags_RoundCornersTop);
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float t = std::max(0.0f, std::min(1.0f, (v.pos.y - a.y) / sh));
            const float edge = ((v.col >> IM_COL32_A_SHIFT) & 255) / 255.0f;   // (the anti-aliased rim keeps its fade)
            v.col = IM_COL32(255, 255, 255, (int)(16 * A / 255 * (1 - t) * edge));
        }
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 46 * A / 255), r, 0, 1.0f);
}
// the cover (or a stand-in), rounded
void cover(ImDrawList* dl, const MediaState& m, ImVec2 a, float size, float r) {
    const ImVec2 b(a.x + size, a.y + size);
    if (m.art && m.artW > 0 && m.artH > 0) {   // square-cropped
        const float aw = (float)m.artW, ah = (float)m.artH, side = std::min(aw, ah);
        const ImVec2 uv0((aw - side) * 0.5f / aw, (ah - side) * 0.5f / ah), uv1(1 - uv0.x, 1 - uv0.y);
        dl->AddImageRounded(m.art, a, b, uv0, uv1, IM_COL32(255, 255, 255, 255), r);
    } else {
        // a rounded gradient (the corners shaded vertex by vertex, as the wallpaper's are)
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255), r);
        const ImU32 tl = IM_COL32(250, 45, 85, 255), tr = IM_COL32(255, 120, 90, 255), br = IM_COL32(170, 40, 140, 255), bl = IM_COL32(90, 40, 160, 255);
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float u = std::max(0.0f, std::min(1.0f, (v.pos.x - a.x) / size)), t = std::max(0.0f, std::min(1.0f, (v.pos.y - a.y) / size));
            const ImU32 c = mixCol(mixCol(tl, tr, u), mixCol(bl, br, u), t);
            v.col = (c & 0x00FFFFFF) | (v.col & 0xFF000000);
        }
        // no cover: a disc (the user's disc icon, drawn as shapes so it is sharp at any size): a soft shadow under it, a faint
        // groove and sheen, and its hole showing the colours behind
        const ImVec2 c(a.x + size * 0.5f, a.y + size * 0.5f);
        const float R = size * 0.30f;
        for (int k = 3; k >= 1; --k) dl->AddCircleFilled(ImVec2(c.x, c.y + size * 0.015f * k), R + size * 0.012f * k, IM_COL32(40, 0, 40, 22), 96);
        dl->AddCircleFilled(c, R, IM_COL32(255, 255, 255, 238), 96);
        dl->AddCircle(c, R * 0.72f, IM_COL32(205, 55, 120, 40), 96, std::max(1.0f, size * 0.012f));   // the groove
        dl->AddCircle(c, R * 0.30f, IM_COL32(205, 55, 120, 70), 64, std::max(1.0f, size * 0.010f));   // the label's edge
        dl->AddCircleFilled(c, size * 0.055f, IM_COL32(205, 55, 120, 255), 32);
    }
}
// the track's progress: a bar with a dot to drag (when the player allows), the times beside it (inline) or under it
std::string clockText(double sec) {
    if (sec < 0) return "--:--";
    const int t = (int)sec;
    char b[16];
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
void progressBar(ImDrawList* dl, const Fonts& f, const MediaState& m, ImVec2 a, float x1, const char* id, bool inlineTimes, Actions& act) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(id);
    const bool known = m.active && m.lenSec > 0 && m.posSec >= 0;
    const float tw = inlineTimes ? 40.0f : 0.0f;
    const float bx0 = a.x + tw, bx1 = x1 - tw, cy = a.y + 8;
    float frac = known ? (float)std::min(1.0, m.posSec / m.lenSec) : 0.0f;
    // dragging: the dot follows the finger; let go, the track goes there
    ImGui::SetCursorScreenPos(ImVec2(bx0 - 8, a.y - 4));
    ImGui::InvisibleButton(id, ImVec2(bx1 - bx0 + 16, 24));
    const bool can = known && m.canSeek;
    const bool active = can && ImGui::IsItemActive();
    const float drag = std::max(0.0f, std::min(1.0f, (ImGui::GetIO().MousePos.x - bx0) / std::max(1.0f, bx1 - bx0)));
    if (active) { frac = drag; st->SetFloat(key, drag); }
    if (can && ImGui::IsItemDeactivated()) { act.mediaSeek = st->GetFloat(key, frac) * m.lenSec; st->SetFloat(key + 1, (float)ImGui::GetTime()); st->SetFloat(key + 2, st->GetFloat(key, frac)); }
    // just after a seek the player may not have caught up: the dot stays where it was let go for a moment
    if (!active && ImGui::GetTime() - st->GetFloat(key + 1, -10.0f) < 0.8) frac = st->GetFloat(key + 2, frac);
    const float hot = eased(id, active, can && ImGui::IsItemHovered(), active).hot;
    const float th = 4 + 2 * hot;
    dl->AddRectFilled(ImVec2(bx0, cy - th * 0.5f), ImVec2(bx1, cy + th * 0.5f), IM_COL32(255, 255, 255, 50), th);
    if (known) {
        const float fx = bx0 + (bx1 - bx0) * frac;
        dl->AddRectFilled(ImVec2(bx0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), IM_COL32(255, 255, 255, 220), th);
        dl->AddCircleFilled(ImVec2(fx, cy), 6.0f + 2.5f * hot, IM_COL32(255, 255, 255, 255), 24);
    }
    const double shownPos = known ? frac * m.lenSec : -1;
    const std::string l = clockText(shownPos), r = clockText(known ? m.lenSec : -1);
    if (inlineTimes) {
        textAt(dl, f.small, a.x, std::floor(cy - fontPx(f.small) * 0.5f), kMuted, l.c_str());
        textRight(dl, f.small, x1, std::floor(cy - fontPx(f.small) * 0.5f), kMuted, r.c_str());
    } else {
        textAt(dl, f.small, bx0, cy + 10, kMuted, l.c_str());
        textRight(dl, f.small, bx1, cy + 10, kMuted, r.c_str());
    }
}
// previous, play / pause, next: true when one was pressed (act.media says which)
void playerButtons(ImDrawList* dl, const MediaState& m, ImVec2 c, float r, const char* id, Actions& act) {
    const float gap = r * 2.9f;
    for (int k = 0; k < 3; ++k) {
        const ImVec2 p(c.x + (k - 1) * gap, c.y);
        const float rr = k == 1 ? r * 1.18f : r;
        char bid[40];
        std::snprintf(bid, sizeof bid, "%s%d", id, k);
        ImGui::SetCursorScreenPos(ImVec2(p.x - rr, p.y - rr));
        const bool enabled = m.active && (k == 1 || (k == 0 ? m.canPrev : m.canNext));
        if (ImGui::InvisibleButton(bid, ImVec2(rr * 2, rr * 2)) && enabled) act.media = k == 1 ? 1 : k == 0 ? 2 : 3;
        const bool hot = ImGui::IsItemHovered() && enabled, down = ImGui::IsItemActive() && enabled;
        if (hot || down) dl->AddCircleFilled(p, rr, IM_COL32(255, 255, 255, down ? 60 : 28), 0);
        const ImU32 col = enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 90);
        const float s = rr * 0.42f;
        if (k == 1) {
            if (m.playing) {   // pause: two bars
                dl->AddRectFilled(ImVec2(p.x - s * 0.75f, p.y - s), ImVec2(p.x - s * 0.22f, p.y + s), col, s * 0.15f);
                dl->AddRectFilled(ImVec2(p.x + s * 0.22f, p.y - s), ImVec2(p.x + s * 0.75f, p.y + s), col, s * 0.15f);
            } else {          // play: a triangle
                dl->AddTriangleFilled(ImVec2(p.x - s * 0.7f, p.y - s), ImVec2(p.x - s * 0.7f, p.y + s), ImVec2(p.x + s * 1.0f, p.y), col);
            }
        } else {             // two triangles and a bar, pointing back or on
            const float d = k == 0 ? -1.0f : 1.0f;
            for (int t = 0; t < 2; ++t) {
                const float x0 = p.x + d * (-s * 0.9f + t * s * 0.85f);
                dl->AddTriangleFilled(ImVec2(x0, p.y - s * 0.75f), ImVec2(x0, p.y + s * 0.75f), ImVec2(x0 + d * s * 0.85f, p.y), col);
            }
            dl->AddRectFilled(ImVec2(p.x + d * s * 0.85f - (d > 0 ? 0 : s * 0.2f), p.y - s * 0.75f), ImVec2(p.x + d * s * 0.85f + (d > 0 ? s * 0.2f : 0), p.y + s * 0.75f), col, 1.0f);
        }
    }
}
const char* duckName(int m) { return m == 1 ? "lowers the media (by the levels set)" : m == 2 ? "pauses the media" : "leaves the media alone"; }
std::string radioLine(const MediaState& m) {
    std::string r = m.radioTx ? std::string("Radio: transmitting") : m.radioRx ? "Radio: receiving" + (m.radioSource.empty() ? std::string() : " (" + m.radioSource + ")") : std::string("Radio: quiet");
    if (m.ducked) r += g_settings.mediaDuck == 2 ? ", media paused" : ", media lowered";
    return r;
}
std::string fitText(ImFont* fnt, const std::string& t, float width) {
    std::string s = t;
    while (s.size() > 2 && fnt->CalcTextSizeA(fontPx(fnt), 1e9f, 0, s.c_str()).x > width) { s.resize(s.size() - 1); while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back(); }
    if (s.size() != t.size() && s.size() > 1) { s.resize(s.size() - 1); s += "\xE2\x80\xA6"; }
    return s;
}

// The Music app: the cover, what is playing, the controls; and what the radio does to the music
void musicPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const MediaState& m = s.media;
    const float top = kStatusH + kHeaderH + 14, bot = h - 14;
    const float art = std::min(bot - top - 20, w * 0.42f);
    const ImVec2 a(std::floor(w * 0.06f + 10), top + std::floor((bot - top - art) * 0.5f));
    for (int k = 4; k >= 1; --k) {   // its shadow: soft, centred under it, a little lower (as a cover lifts off the page)
        const float g = 3.0f * k;
        dl->AddRectFilled(ImVec2(a.x - g, a.y + 6 - g), ImVec2(a.x + art + g, a.y + art + 6 + g), IM_COL32(0, 0, 0, 16), 20.0f + g);
    }
    cover(dl, m, a, art, 20.0f);
    const float x = a.x + art + 36, x1 = w - 30;
    float y = a.y + 6;
    if (!m.supported) {
        textAt(dl, f.title, x, y, kInk, "Media");
        dl->AddText(f.body, fontPx(f.body), ImVec2(x, y + 36), kMuted, "This computer's media controls could not be reached.", nullptr, x1 - x);
        return;
    }
    if (!m.active) {
        textAt(dl, f.title, x, y, kInk, "Nothing playing");
        dl->AddText(f.body, fontPx(f.body), ImVec2(x, y + 36), kMuted, "Play something in Spotify, Apple Music, a browser or any player: it shows here and in the pull-down from the logo.", nullptr, x1 - x);
        y += 110;
    } else {
        textAt(dl, f.small, x, y, IM_COL32(255, 120, 150, 255), fitText(f.small, m.app.empty() ? std::string("Now playing") : m.app, x1 - x).c_str());
        y += 24;
        textAt(dl, f.title, x, y, kInk, fitText(f.title, m.title.empty() ? std::string("Unknown title") : m.title, x1 - x).c_str());
        y += 30;
        textAt(dl, f.body, x, y, kMuted, fitText(f.body, m.artist, x1 - x).c_str());
        y += 22;
        if (!m.album.empty()) textAt(dl, f.small, x, y, kMuted, fitText(f.small, m.album, x1 - x).c_str());
        y += 30;
        progressBar(dl, f, m, ImVec2(x, y), x1, "##mpprog", false, act);
        y += 40;
    }
    playerButtons(dl, m, ImVec2(std::floor((x + x1) * 0.5f), y + 20), 22.0f, "##mp", act);
    y += 78;
    dl->AddLine(ImVec2(x, y), ImVec2(x1, y), IM_COL32(255, 255, 255, 24));
    textAt(dl, f.small, x, y + 12, m.radioRx ? kWarn : kMuted, radioLine(m).c_str());
    const std::string rule = std::string("When the radios receive or transmit, XPTablet ") + duckName(g_settings.mediaDuck) + " (Settings, Media).";
    dl->AddText(f.small, fontPx(f.small), ImVec2(x, y + 34), kMuted, rule.c_str(), nullptr, x1 - x);
}

// notifications from what the host tells: traffic in TCAS RA range, a new SimBrief plan
void watchForNotes(const SimState& s) {
    const double now = ImGui::GetTime();
    trackTraffic(s.map);
    for (const MapState::Traffic& t : s.map.traffic) {
        // the worst of: a runway collision (3), an RA (2), a TA (1); one notification each time it gets worse, and again after a minute
        const int level = runwayConflict(s.map, t) ? 3 : tcasLevel(s.map, t);
        if (!level) continue;
        const std::string key = t.callsign + "#" + std::to_string(level);
        auto it = g_raSeen.find(key);
        if (it != g_raSeen.end() && now - it->second < 60) { it->second = now; continue; }
        bool worse = false;   // (a TA after its RA: no second note)
        for (int l = level + 1; l <= 3; ++l) { auto w = g_raSeen.find(t.callsign + "#" + std::to_string(l)); if (w != g_raSeen.end() && now - w->second < 60) worse = true; }
        g_raSeen[key] = now;
        if (!g_settings.notifyTcas || worse) continue;
        double brg = 0;
        const double d = distNmLL(s.map.lat, s.map.lon, t.lat, t.lon, &brg);
        const int rel = (int)std::lround((t.altFt - s.map.altFt) / 100.0f) * 100;
        char b[160];
        if (level == 3) std::snprintf(b, sizeof b, "%s%s%s %s, %.1f nm", t.callsign.c_str(), t.type.empty() ? "" : " ", t.type.c_str(), t.onGround ? "is on the runway" : "is landing on your runway", d);
        else std::snprintf(b, sizeof b, "%s%s%s: %d ft %s, %.1f nm", t.callsign.c_str(), t.type.empty() ? "" : " ", t.type.c_str(), std::abs(rel), rel >= 0 ? "above" : "below", d);
        postNote(level == 3 ? "Runway collision alert" : level == 2 ? "TCAS resolution advisory" : "TCAS traffic advisory", b, APP_MAP);
    }
    for (auto it = g_raSeen.begin(); it != g_raSeen.end();) { if (now - it->second > 120) it = g_raSeen.erase(it); else ++it; }
    if (g_planSeqSeen < 0) g_planSeqSeen = s.newPlanSeq;
    if (s.newPlanSeq != g_planSeqSeen) {
        g_planSeqSeen = s.newPlanSeq;
        if (g_settings.notifySimbrief) postNote("New SimBrief flight plan detected", s.newPlanText, APP_ROUTES);
    }
}

void openFromNote(const Note& n) {
    g_ccTarget = 0;   // the pull-down closes
    if (n.app >= 0 && n.app < APP_COUNT && n.app != g_app) openApp(n.app);   // (already on that page: nothing more)
}

// the banner (newest notification, for 3 s): where it is now; false if none
bool bannerRect(float w, ImVec2& a, ImVec2& b, float& alpha, Note*& note) {
    note = nullptr;
    for (Note& n : g_notes) if (n.banner) { note = &n; break; }
    if (!note || g_ccT > 0.02f) return false;
    const double age = ImGui::GetTime() - note->t;
    if (age > 3.0 && g_bannerDrag < 0 && note->awayT < 0) { note->banner = false; note = nullptr; return false; }
    const float bw = std::min(440.0f, w - 40), bh = 62;
    const float in = std::min(1.0f, (float)age / 0.28f), e = 1 - (1 - in) * (1 - in) * (1 - in);
    alpha = age < 2.4 ? 1.0f : std::max(0.0f, 1.0f - (float)(age - 2.4) / 0.6f);   // gone by 3 s
    if (g_bannerDrag >= 0) alpha = 1.0f;
    float y = kStatusH + 8 - (1 - e) * (bh + 16) + std::min(0.0f, g_bannerDy);
    if (note->awayT >= 0) {   // swiped up: it slides on up and out, into the list
        const float p = std::min(1.0f, (float)(ImGui::GetTime() - note->awayT) / 0.22f);
        if (p >= 1.0f) { note->banner = false; note->awayT = -1; note = nullptr; return false; }
        y = kStatusH + 8 + note->awayFrom - p * p * (bh + 40);
        alpha = 1.0f - p;
    }
    a = ImVec2(std::floor((w - bw) * 0.5f), y);
    b = ImVec2(a.x + bw, y + bh);
    return true;
}

void noteCard(ImDrawList* dl, const Fonts& f, const Note& n, ImVec2 a, ImVec2 b, float alpha) {
    glass(dl, a, b, 16.0f, alpha);
    const int A = (int)(255 * alpha);
    const ImVec2 ic(a.x + 22, a.y + 20);
    if (n.app >= 0 && n.app < APP_COUNT) drawTile(dl, f, n.app, ic, 22, false, 1.0f, 1.0f);
    textAt(dl, f.small, a.x + 40, a.y + 12, IM_COL32(220, 228, 242, A), (n.app >= 0 && n.app < APP_COUNT ? appName(n.app) : "XPTablet"));
    textRight(dl, f.small, b.x - 12, a.y + 12, IM_COL32(170, 182, 204, A), agoText(n.t).c_str());
    textAt(dl, f.bold, a.x + 14, a.y + 32, IM_COL32(245, 248, 255, A), fitText(f.bold, n.title, b.x - a.x - 28).c_str());
    if (b.y - a.y > 66) textAt(dl, f.small, a.x + 14, a.y + 52, IM_COL32(200, 210, 228, A), fitText(f.small, n.body, b.x - a.x - 28).c_str());
    else { const std::string t = fitText(f.small, n.body, b.x - a.x - 28 - f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, n.title.c_str()).x - 10); (void)t; }
}

// the banner, drawn over everything; a tap opens its page, pushing it up puts it away (it stays in the pull-down's list)
void drawBanner(ImDrawList* dl, const Fonts& f, float w) {
    ImVec2 a, b;
    float alpha;
    Note* n;
    if (!bannerRect(w, a, b, alpha, n)) { g_bannerDrag = -1; g_bannerDy = 0; return; }
    // a banner shows the title and, under it, the text
    ImVec2 bb(b.x, b.y + 14);
    dl->AddRectFilled(ImVec2(a.x + 2, a.y + 6), ImVec2(bb.x + 2, bb.y + 8), IM_COL32(0, 0, 0, (int)(70 * alpha)), 18.0f);
    dl->AddRectFilled(a, bb, IM_COL32(16, 26, 46, (int)(200 * alpha)), 16.0f);
    noteCard(dl, f, *n, a, bb, alpha);
    if (n->awayT >= 0) return;   // on its way out: no more presses
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##banner", ImVec2(bb.x - a.x, bb.y - a.y));
    const ImGuiIO& io = ImGui::GetIO();
    static float pressX = 0, moved = 0;
    if (ImGui::IsItemActivated()) { g_bannerDrag = io.MousePos.y; g_bannerDy = 0; pressX = io.MousePos.x; moved = 0; }
    if (ImGui::IsItemActive()) {
        g_bannerDy = io.MousePos.y - (float)g_bannerDrag;
        moved = std::max(moved, std::fabs(g_bannerDy) + std::fabs(io.MousePos.x - pressX));   // the most it moved: a swipe, not a tap
    }
    if (ImGui::IsItemDeactivated()) {
        if (g_bannerDy < -10 || (moved >= 6 && g_bannerDy < 0)) { n->awayT = ImGui::GetTime(); n->awayFrom = std::min(0.0f, g_bannerDy); }   // swiped up: into the list
        else if (moved < 6) { const Note copy = *n; g_notes.erase(std::remove_if(g_notes.begin(), g_notes.end(), [&](const Note& x) { return x.id == copy.id; }), g_notes.end()); openFromNote(copy); }   // a tap: its page
        g_bannerDrag = -1; g_bannerDy = 0;
    }
}

// the pull-down, under the status bar: the music on the left, the notifications on the right
void drawPullDown(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float dt = ImGui::GetIO().DeltaTime;
    if (!g_ccDrag) g_ccT += (g_ccTarget - g_ccT) * (1.0f - std::exp(-dt * 14.0f));
    if (std::fabs(g_ccT - g_ccTarget) < 0.002f && !g_ccDrag) g_ccT = g_ccTarget;
    if (g_ccT <= 0.001f) { g_ccScroll = 0; return; }
    const float k = g_ccT, H = h - kStatusH, oy = -(1 - k) * H;
    dl->PushClipRect(ImVec2(0, kStatusH), ImVec2(w, h), true);   // never over the status bar
    // the veil: what is behind, dimmed and softened (a frosted look)
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(6, 12, 26, (int)(130 * k)));
    dl->AddRectFilled(ImVec2(0, kStatusH + oy), ImVec2(w, h + oy), themeDark(235));   // frosted, in the theme's tone: the page only just shows through
    dl->AddRectFilledMultiColor(ImVec2(0, kStatusH + oy), ImVec2(w, kStatusH + oy + H * 0.5f), accentCol(26), IM_COL32(255, 120, 200, 18), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH + oy));
    ImGui::SetNextItemAllowOverlap();   // the buttons, sliders and the bar drawn over it still get their presses
    ImGui::InvisibleButton("##ccback", ImVec2(w, H));   // the pull-down takes the presses on it (none reach the page under it)
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_ccScroll -= ImGui::GetIO().MouseWheel * 40;
    const bool music = !g_mac && g_settings.mediaEnabled;
    const float m = 16, top = kStatusH + oy + 14;
    const float colL = std::min(300.0f, std::floor(w * 0.38f));   // media (when on), then the screen's brightness
    // the music
    if (music) {
        const MediaState& md = s.media;
        textAt(dl, f.title, m + 4, top + 2, kInk, "Media");   // as Notifications beside it; the box level with Clear All
        const ImVec2 a(m, top + 34), b(m + colL, top + 34 + kCcMediaH);
        glass(dl, a, b, 24.0f);
        const float art = 74;
        for (int k = 3; k >= 1; --k) {   // a soft shadow, centred under the cover
            const float g = 2.0f * k;
            dl->AddRectFilled(ImVec2(a.x + 16 - g, a.y + 19 - g), ImVec2(a.x + 16 + art + g, a.y + 19 + art + g), IM_COL32(0, 0, 0, 18), 12.0f + g);
        }
        cover(dl, md, ImVec2(a.x + 16, a.y + 16), art, 12.0f);
        const float tx = a.x + 16 + art + 12, tw = b.x - tx - 14;
        if (!md.supported) dl->AddText(f.small, fontPx(f.small), ImVec2(tx, a.y + 20), kMuted, "Media controls are not available on this computer.", nullptr, tw);
        else if (!md.active) { textAt(dl, f.bold, tx, a.y + 22, kInk, "Not playing"); dl->AddText(f.small, fontPx(f.small), ImVec2(tx, a.y + 44), kMuted, "Play something in any player.", nullptr, tw); }
        else {
            textAt(dl, f.small, tx, a.y + 18, IM_COL32(255, 140, 165, 255), fitText(f.small, md.app.empty() ? std::string("Now playing") : md.app, tw).c_str());
            textAt(dl, f.bold, tx, a.y + 36, kInk, fitText(f.bold, md.title, tw).c_str());
            textAt(dl, f.small, tx, a.y + 58, kMuted, fitText(f.small, md.artist, tw).c_str());
        }
        if (md.active) progressBar(dl, f, md, ImVec2(a.x + 16, a.y + 104), b.x - 16, "##ccprog", true, act);
        playerButtons(dl, md, ImVec2(std::floor((a.x + b.x) * 0.5f), a.y + 146), 18.0f, "##ccmp", act);
        textCentered(dl, f.small, (a.x + b.x) * 0.5f, b.y - 24, md.radioRx || md.radioTx ? kWarn : kMuted, fitText(f.small, radioLine(md), colL - 24).c_str());
        // the Music app
        ImGui::SetCursorScreenPos(ImVec2(a.x + 16, a.y + 16));
        if (ImGui::InvisibleButton("##ccart", ImVec2(art, art))) { g_ccTarget = 0; openApp(APP_MUSIC); }
    }
    {   // the screen's brightness: the slider, and auto dim (as in Settings, Display)
        const float by = music ? top + 34 + kCcMediaH + kCcGap : top + 34;
        const ImVec2 a(m, by), b(m + colL, by + kCcBrightH);
        glass(dl, a, b, 24.0f);
        textAt(dl, f.bold, a.x + 16, a.y + 12, kInk, "Brightness");
        float br = g_settings.autoDim ? s.autoDimLevel : std::max(0.1f, std::min(1.0f, g_settings.brightness));
        char txt[16];
        std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(br * 100));
        if (smoothSlider(dl, f, "##ccbright", ImVec2(a.x + 4, a.y + 44), ImVec2(b.x - 12 - 96 - 8, a.y + 76), br, 0.1f, 1.0f, 0, !g_settings.autoDim, txt) && !g_settings.autoDim) {
            g_settings.brightness = br; g_settingsChanged = true;
        }
        // auto dim, right of the slider as in Settings, Display
        if (pill(dl, f, "##ccautodim", "Auto dim", ImVec2(b.x - 12 - 96, a.y + 45), ImVec2(b.x - 12, a.y + 75), g_settings.autoDim, true)) { g_settings.autoDim = !g_settings.autoDim; g_settingsChanged = true; }
    }
    {   // the flight's progress, under the brightness (only with a flight plan)
        const Progress pr = flightProgress(s);
        if (pr.ok) {
            const float py = (music ? top + 34 + kCcMediaH + kCcGap : top + 34) + kCcBrightH + kCcGap;
            const ImVec2 a(m, py), b(m + colL, py + 70);
            glass(dl, a, b, 24.0f);
            textAt(dl, f.bold, a.x + 16, a.y + 10, kInk, "Flight");
            progressStrip(dl, f, s, pr, ImVec2(a.x + 16, a.y + 32), ImVec2(b.x - 16, b.y - 4), true);
        }
    }
    // the notifications
    const float nx0 = m + colL + 14, nx1 = w - m;
    textAt(dl, f.title, nx0 + 4, top + 2, kInk, "Notifications");
    float y = top + 34;
    {
        const ImVec2 a(nx0, y), b(nx1, y + 40);
        ImGui::SetCursorScreenPos(a);
        const bool any = !g_notes.empty();
        if (ImGui::InvisibleButton("##ccclear", ImVec2(b.x - a.x, b.y - a.y)) && any) {   // they slide away one after another
            const double now = ImGui::GetTime();
            int k = 0;
            for (Note& n : g_notes) if (!n.dying) { n.dying = true; n.dieT = now + 0.045 * k++; n.dx = 0.001f; }
        }
        const bool hot = ImGui::IsItemHovered() && any;
        dl->AddRectFilled(a, b, any ? (hot ? IM_COL32(255, 255, 255, 60) : IM_COL32(255, 255, 255, 40)) : IM_COL32(255, 255, 255, 16), 20.0f);
        textCentered(dl, f.bold, (a.x + b.x) * 0.5f, std::floor(a.y + 20 - fontPx(f.bold) * 0.5f), any ? kInk : kMuted, "Clear All");
        y += 52;
    }
    const float listTop = y, listBot = h + oy - kCcBottomGap;   // (clear of the bar at the bottom, as the boxes at the left)
    if (!g_settings.notifyEnabled) textCentered(dl, f.small, (nx0 + nx1) * 0.5f, y + 20, kMuted, "Notifications are off (Settings, Notifications).");
    else if (g_notes.empty()) textCentered(dl, f.small, (nx0 + nx1) * 0.5f, y + 20, kMuted, "No notifications");
    const float cardH = 72, gapY = 10;
    const float content = g_notes.size() * (cardH + gapY);
    g_ccScroll = std::max(0.0f, std::min(g_ccScroll, std::max(0.0f, content - (listBot - listTop))));
    dl->PushClipRect(ImVec2(nx0 - 4, listTop), ImVec2(nx1 + 4, listBot), true);
    const ImGuiIO& io = ImGui::GetIO();
    y = listTop - g_ccScroll;
    for (size_t i = 0; i < g_notes.size(); ++i) {
        Note& n = g_notes[i];
        if (n.dying && ImGui::GetTime() >= n.dieT) n.dx += (n.dx >= 0 ? 1 : -1) * dt * (900 + 3 * std::fabs(n.dx));   // speeding up as it goes
        if (n.yShown < -1e8f) n.yShown = y;
        n.yShown += (y - n.yShown) * (1.0f - std::exp(-dt * 14.0f));   // closing up the gap smoothly
        if (std::fabs(n.yShown - y) < 0.3f) n.yShown = y;
        const float yy = n.yShown;
        const ImVec2 a(nx0 + n.dx, yy), b(nx1 + n.dx, yy + cardH);
        if (b.y > listTop && a.y < listBot) {
            const float al = n.dying ? std::max(0.0f, 1.0f - std::fabs(n.dx) / (w * 0.5f)) : 1.0f;
            noteCard(dl, f, n, a, b, al);
            if (!n.dying) {
                char id[24];
                std::snprintf(id, sizeof id, "##note%d", n.id);
                ImGui::SetCursorScreenPos(ImVec2(nx0, std::max(y, listTop)));
                ImGui::InvisibleButton(id, ImVec2(nx1 - nx0, std::max(1.0f, std::min(y + cardH, listBot) - std::max(y, listTop))));
                {   // hovered: the card brightens and takes the accent's edge (pressed, a little more)
                    const Ease ne = eased(id, false, ImGui::IsItemHovered(), ImGui::IsItemActive());
                    if (ne.hot + ne.down > 0.01f) {
                        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, (int)((22 * ne.hot + 14 * ne.down) * al)), 16.0f);
                        dl->AddRect(a, b, accentLite((int)(120 * ne.hot * al)), 16.0f, 0, 1.2f);
                    }
                }
                static float pressX = 0, pressY = 0, pressScroll = 0;
                static int dragWay = 0;   // 0 undecided, 1 sideways (swipe it away), 2 up or down (scroll the list)
                if (ImGui::IsItemActivated()) { pressX = io.MousePos.x; pressY = io.MousePos.y; pressScroll = g_ccScroll; dragWay = 0; }
                if (ImGui::IsItemActive()) {
                    const float ddx = io.MousePos.x - pressX, ddy = io.MousePos.y - pressY;
                    if (dragWay == 0 && std::fabs(ddx) + std::fabs(ddy) > 8) dragWay = std::fabs(ddx) >= std::fabs(ddy) ? 1 : 2;
                    if (dragWay == 1) n.dx = ddx;   // swiped sideways
                    if (dragWay == 2) g_ccScroll = std::max(0.0f, std::min(std::max(0.0f, content - (listBot - listTop)), pressScroll - ddy));
                }
                if (ImGui::IsItemDeactivated()) {
                    if (dragWay == 2) { n.dx = 0; }   // it was a scroll: nothing opens
                    else if (std::fabs(n.dx) > 90) { n.dying = true; n.dieT = ImGui::GetTime(); }   // gone
                    else if (std::fabs(n.dx) < 6) { const Note copy = n; g_notes.erase(g_notes.begin() + i); openFromNote(copy); break; }
                    else n.dx = 0;
                }
                if (!ImGui::IsItemActive() && !n.dying && n.dx != 0) n.dx *= std::exp(-dt * 18.0f);
            }
        }
        y += cardH + gapY;
    }
    g_notes.erase(std::remove_if(g_notes.begin(), g_notes.end(), [&](const Note& n) { return n.dying && std::fabs(n.dx) > w; }), g_notes.end());
    dl->PopClipRect();
    g_ccScroll = scrollBar(dl, "##ccbarscroll", nx1 + 8, listTop, listBot, g_ccScroll, content, listBot - listTop);   // (draggable)
    // the wheel anywhere over the list (the cards on it take the hover, so the pull-down's own catch-all does not see it there)
    if (ImGui::IsMouseHoveringRect(ImVec2(nx0, listTop), ImVec2(nx1 + 16, listBot), false) && io.MouseWheel != 0)
        g_ccScroll = std::max(0.0f, std::min(std::max(0.0f, content - (listBot - listTop)), g_ccScroll - io.MouseWheel * 40));
    // the bar to push it back up
    const float by = h + oy - 16;
    ImGui::SetCursorScreenPos(ImVec2(w * 0.5f - 110, by - 18));
    ImGui::InvisibleButton("##ccbar", ImVec2(220, 34));
    {   // hovered or held: it grows a little and brightens (in the accent's light tone), so it reads as something to drag
        const Ease be = eased("##ccbar", false, ImGui::IsItemHovered(), ImGui::IsItemActive());
        const float k = std::max(be.hot, be.down), hw = 60 + 12 * k, hh = 2.5f + 1.0f * k;
        dl->AddRectFilled(ImVec2(w * 0.5f - hw, by - hh - 0.5f), ImVec2(w * 0.5f + hw, by + hh - 0.5f), mixCol(IM_COL32(255, 255, 255, 150), accentLite(255), k * 0.7f), hh + 0.5f);
    }
    if (ImGui::IsItemActivated()) { g_ccDrag = true; g_ccDragY0 = io.MousePos.y; g_ccDragT0 = g_ccT; }
    if (ImGui::IsItemActive()) g_ccT = std::max(0.0f, std::min(1.0f, g_ccDragT0 + (io.MousePos.y - g_ccDragY0) / (H * 0.8f)));
    if (ImGui::IsItemDeactivated()) {
        g_ccDrag = false;
        const bool tapped = std::fabs(io.MousePos.y - g_ccDragY0) < 6;
        g_ccTarget = (tapped || g_ccT < 0.8f) ? 0.0f : 1.0f;
    }
    dl->PopClipRect();
}

void settingsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    {   // the pages, down a sidebar as on an iPad: General, Display, Data at the top, Credits at the bottom
        const float sTop = kStatusH + kHeaderH + 10, sBot = h - 12, sW = 168;
        card(dl, ImVec2(12, sTop), ImVec2(12 + sW, sBot), f, nullptr);
        // the pages: General, Display, Data, Notifications, Media (not on macOS) at the top; Credits at the bottom
        struct Tab { int id; const char* name; ImU32 tint; int glyph; };
        std::vector<Tab> tabs = {{0, "General", IM_COL32(110, 120, 140, 255), APP_SETTINGS}, {1, "Display", IM_COL32(64, 132, 230, 255), APP_WEATHER},
                                 {2, "Data", IM_COL32(60, 190, 130, 255), APP_DOCUMENTS}, {4, "Notifications", IM_COL32(240, 70, 70, 255), APP_NOTES}};
        if (!g_mac) tabs.push_back({5, "Media", IM_COL32(250, 45, 85, 255), APP_MUSIC});
        if (g_mac && g_settingsTab == 5) g_settingsTab = 0;
        tabs.push_back({3, "Credits", IM_COL32(236, 152, 50, 255), APP_NOTES});
        // the highlight glides to the chosen page (as the setting buttons ease), drawn under the names
        static float selY = -1;
        float wantY = -1;
        for (size_t t = 0; t < tabs.size(); ++t) if (tabs[t].id == g_settingsTab) wantY = t + 1 == tabs.size() ? sBot - 50 : sTop + 8 + t * 42;
        if (wantY >= 0) {
            if (selY < 0 || std::fabs(selY - wantY) > 400) selY = wantY;
            selY += (wantY - selY) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f));
            if (std::fabs(selY - wantY) < 0.3f) selY = wantY;
            dl->AddRectFilled(ImVec2(18, selY), ImVec2(6 + sW, selY + 40), accentCol(110), 10.0f);
        }
        for (size_t t = 0; t < tabs.size(); ++t) {
            const int i = tabs[t].id;
            const bool last = t + 1 == tabs.size();
            const float ry = !last ? sTop + 8 + t * 42 : sBot - 50;
            const ImVec2 a(18, ry), e(6 + sW, ry + 40);
            char id[16];
            std::snprintf(id, sizeof id, "##set%d", i);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x, e.y - a.y)) && g_settingsTab != i) { g_settingsTab = i; g_kbd = false; g_typeInto = nullptr; }
            const bool on = g_settingsTab == i, hot = ImGui::IsItemHovered();
            const Ease ez = eased(id, on, hot && !on, ImGui::IsItemActive());
            if (ez.hot > 0.01f) dl->AddRectFilled(a, e, IM_COL32(255, 255, 255, (int)(18 * ez.hot + 14 * ez.down)), 10.0f);   // hover fades in and out
            const ImVec2 ic(a.x + 20, a.y + 20);
            dl->AddRectFilled(ImVec2(ic.x - 12, ic.y - 12), ImVec2(ic.x + 12, ic.y + 12), tabs[t].tint, 6.0f);
            icon(dl, tabs[t].glyph, ic, 7.0f, IM_COL32(255, 255, 255, 255));
            textAt(dl, f.body, a.x + 42, std::floor(a.y + 20 - fontPx(f.body) * 0.5f), kInk, tabs[t].name);
        }
        dl->AddLine(ImVec2(24, sBot - 58), ImVec2(sW, sBot - 58), IM_COL32(255, 255, 255, 24));
    }
    const float x0 = 12 + 168 + 14;
    if (g_settingsTab == 3) { creditsPage(dl, f, x0, w, h); return; }
    const float top = kStatusH + kHeaderH + 16;
    const float colW = std::floor((w - 12 - x0 - 16) * 0.5f), lx = x0, rx = x0 + colW + 16;
    // a choice of two, side by side; greyed out (and inert) when not enabled
    auto choice = [&](const char* id, float x, float& y, float width, const char* title, const char* const names[2], int& value, bool enabled) {
        textAt(dl, f.bold, x, y, enabled ? kInk : kMuted, title);
        y += 22;
        const float half = std::floor((width - 8) * 0.5f);
        for (int i = 0; i < 2; ++i) {
            char bid[24];
            std::snprintf(bid, sizeof bid, "%s%d", id, i);
            if (pill(dl, f, bid, names[i], ImVec2(x + i * (half + 8), y), ImVec2(x + i * (half + 8) + half, y + 30), (value == 1 ? 1 : 0) == i, enabled) && enabled) { value = i; g_settingsChanged = true; }
        }
        y += 42;
    };
    auto toggle = [&](const char* id, float x, float y, float width, const char* label, bool& v, bool enabled) {   // a pill that is lit when on
        if (pill(dl, f, id, label, ImVec2(x, y), ImVec2(x + width, y + 30), v, enabled) && enabled) { v = !v; g_settingsChanged = true; }
    };

    if (g_settingsTab == 0) {   // ---- General: the home screen's icons; status and the touch test
        float y = top;
        // the web apps on the home screen: each lit one is there; any number of them, none needed
        textAt(dl, f.title, lx, y, kInk, "On the home screen");
        y += 32;
        const float q = std::floor((colW - 8) * 0.5f);
        textAt(dl, f.bold, lx, y, kInk, "Chart providers");
        y += 22;
        toggle("##tgff", lx, y, q, "ForeFlight", g_settings.showForeflight, true);
        toggle("##tgcf", lx + q + 8, y, q, "ChartFox", g_settings.showCharts, true);
        toggle("##tgng", lx, y + 36, q, "Navigraph", g_settings.showNavigraph, true);
        toggle("##tgsv", lx + q + 8, y + 36, q, "SkyVector", g_settings.showSkyvector, true);
        y += 78;
        textAt(dl, f.bold, lx, y, kInk, "Live map");
        y += 22;
        toggle("##tgvo", lx, y, q, "Volanta", g_settings.showLive, true);
        toggle("##tgva", lx + q + 8, y, q, "VATSIM Radar", g_settings.showVatsim, true);
        y += 42;
        textAt(dl, f.bold, lx, y, kInk, "Flight planning");
        y += 22;
        toggle("##tgsb", lx, y, colW, "SimBrief", g_settings.showSimbrief, true);
        // status
        float sy = top;
        textAt(dl, f.bold, rx, sy, kInk, "Status");
        sy += 24;
        char b[96];
        auto line = [&](const char* k, const char* v) { textAt(dl, f.mono, rx, sy, kMuted, k); textAt(dl, f.mono, rx + 100, sy, kInk, v); sy += 18; };
        line("Version", XPT_VERSION);
        std::snprintf(b, sizeof b, "%s%s", s.acfIcao.empty() ? "-" : s.acfIcao.c_str(), s.toliss ? " (ToLiss)" : "");
        line("Aircraft", b);
        line("Shown on", s.onPanel ? "cockpit tablet" : "window only");
        if (s.panel[2] > 0 && s.panel[3] > 0) { std::snprintf(b, sizeof b, "%d,%d  %dx%d", s.panel[0], s.panel[1], s.panel[2], s.panel[3]); line("Panel area", b); }
        else line("Panel area", "none (no AviTab slot)");
        line("WXPlane", s.wxPlane ? "found" : "not installed");
        std::snprintf(b, sizeof b, "%d%%%s", (int)std::ceil(g_batt - 0.001f), g_charger ? ", charging" : "");
        line("Battery", b);
        if (s.aviTabRunning) { dl->AddText(f.small, fontPx(f.small), ImVec2(rx, sy + 2), kWarn, "AviTab is installed and keeps the cockpit tablet. Remove it to let XPTablet take its place.", nullptr, w - 18 - rx); sy += 34; }
        // the touch test: press anywhere in the box, a mark must appear exactly under the pointer
        const ImVec2 a(rx, sy + 10), e(w - 18, h - 14);
        dl->AddRectFilled(a, e, IM_COL32(0, 0, 0, 80), 10.0f);
        dl->AddRect(a, e, IM_COL32(255, 255, 255, 40), 10.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();   // the Clear button lies inside this area: without this the area took its presses
        ImGui::InvisibleButton("##touch", ImVec2(e.x - a.x, e.y - a.y));
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            if (g_touches.empty() || std::fabs(g_touches.back().x - m.x) + std::fabs(g_touches.back().y - m.y) > 3) g_touches.push_back(m);
            if (g_touches.size() > 400) g_touches.erase(g_touches.begin());
        }
        dl->PushClipRect(a, e, true);
        for (const ImVec2& p : g_touches) dl->AddCircleFilled(p, 4.0f, IM_COL32(255, 184, 64, 220), 0);
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            dl->AddLine(ImVec2(m.x - 12, m.y), ImVec2(m.x + 12, m.y), IM_COL32(255, 255, 255, 200));
            dl->AddLine(ImVec2(m.x, m.y - 12), ImVec2(m.x, m.y + 12), IM_COL32(255, 255, 255, 200));
        }
        dl->PopClipRect();
        textCentered(dl, f.small, (a.x + e.x) * 0.5f, a.y + 8, themeText(200, 212, 232, 0.15f), g_touches.empty() ? "Touch test: press and drag here" : "Touch test");
        if (!g_touches.empty() && pill(dl, f, "##touchclr", "Clear", ImVec2(e.x - 74, e.y - 36), ImVec2(e.x - 10, e.y - 8), false, true)) g_touches.clear();
        return;
    }

    if (g_settingsTab == 4) {   // ---- Notifications
        float y = top;
        toggle("##ntfon", lx, y, colW, g_settings.notifyEnabled ? "Notifications: on" : "Notifications: off", g_settings.notifyEnabled, true);
        y += 42;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "A banner drops in under the status bar and fades after 3 seconds: tap it to open its page, push it up to put it away. Pull down from the logo for the list.", nullptr, colW);
        y += 56;
        textAt(dl, f.bold, lx, y, g_settings.notifyEnabled ? kInk : kMuted, "Alerts");
        y += 24;
        toggle("##ntftcas", lx, y, colW, "TCAS alert", g_settings.notifyTcas, g_settings.notifyEnabled);
        y += 36;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Traffic in TCAS resolution-advisory range. Opens the Map.", nullptr, colW);
        y += 30;
        toggle("##ntfsb", lx, y, colW, "New SimBrief flight plan", g_settings.notifySimbrief, g_settings.notifyEnabled);
        y += 36;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "SimBrief is checked every 2 minutes with your SimBrief ID; a new plan is loaded. Opens the Flight Plan.", nullptr, colW);
        y += 34;
        if (g_settings.simbriefUser.empty()) textAt(dl, f.small, lx, y, kWarn, "Enter your SimBrief ID under Data first.");
        // the right: try one
        float ry = top;
        textAt(dl, f.bold, rx, ry, kInk, "Try it");
        ry += 26;
        if (pill(dl, f, "##ntftest", "Send a test notification", ImVec2(rx, ry), ImVec2(w - 18, ry + 30), false, g_settings.notifyEnabled))
            postNote("XPTablet", "Notifications are working. Tap to open Settings.", APP_SETTINGS);
        ry += 44;
        char b[64];
        std::snprintf(b, sizeof b, "%d in the list", (int)g_notes.size());
        textAt(dl, f.small, rx, ry, kMuted, b);
        ry += 40;
        textAt(dl, f.bold, rx, ry, kInk, "Sound");
        ry += 26;
        {   // the chime's volume (0: silent), and a test
            char txt[16];
            std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(g_settings.notifyVolume * 100));
            float v = g_settings.notifyVolume;
            if (smoothSlider(dl, f, "##notifyvol", ImVec2(rx - 4, ry), ImVec2(w - 18 - 76, ry + 30), v, 0.0f, 1.0f, 20, true, txt)) { g_settings.notifyVolume = v; g_settingsChanged = true; }
            if (pill(dl, f, "##notifytest", "Test", ImVec2(w - 18 - 68, ry + 1), ImVec2(w - 18, ry + 29), false, g_settings.notifyVolume > 0)) ++g_chimeSeq;
            ry += 40;
        }
        dl->AddText(f.small, fontPx(f.small), ImVec2(rx, ry), kMuted, "The chime with each notification, played through X-Plane's sound. At 0% it is silent.", nullptr, w - 18 - rx);
        return;
    }
    if (g_settingsTab == 5 && !g_mac) {   // ---- Media: the Music app and the pull-down's player; what the radio does to the music
        float y = top;
        toggle("##medon", lx, y, colW, g_settings.mediaEnabled ? "Media controls: on" : "Media controls: off", g_settings.mediaEnabled, true);
        y += 42;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "The Media app and the player in the pull-down: what is playing on this computer (Spotify, a browser, any player), with its cover.", nullptr, colW);
        y += 56;
        textAt(dl, f.bold, lx, y, g_settings.mediaEnabled ? kInk : kMuted, "When the radios receive or transmit");
        y += 24;
        const char* modes[3] = {"No change", "Lower media", "Pause media"};
        for (int i = 0; i < 3; ++i) {
            char id[16];
            std::snprintf(id, sizeof id, "##duck%d", i);
            if (pill(dl, f, id, modes[i], ImVec2(lx, y), ImVec2(lx + colW, y + 30), g_settings.mediaDuck == i, g_settings.mediaEnabled) && g_settings.mediaEnabled) { g_settings.mediaDuck = i; g_settingsChanged = true; }
            y += 36;
        }
        // lowered: to how much, receiving and transmitting each (greyed unless "Lower media")
        const bool lowering = g_settings.mediaEnabled && g_settings.mediaDuck == 1;
        auto level = [&](const char* id, const char* name, float& v) {
            textAt(dl, f.small, lx, y + 7, lowering ? kInk : kMuted, name);
            char txt[16];
            std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(v * 100));
            float tv = v;
            if (smoothSlider(dl, f, id, ImVec2(lx + 96, y), ImVec2(lx + colW, y + 30), tv, 0.0f, 1.0f, 20, lowering, txt) && lowering) { v = tv; g_settingsChanged = true; }
            y += 36;
        };
        level("##duckrx", "Receiving", g_settings.duckRx);
        level("##ducktx", "Transmitting", g_settings.duckTx);
        // the right: what is going on now
        float ry = top;
        const MediaState& md = s.media;
        textAt(dl, f.bold, rx, ry, kInk, "Now");
        ry += 26;
        const float rw = w - 18 - rx;
        if (!md.supported) { dl->AddText(f.small, fontPx(f.small), ImVec2(rx, ry), kWarn, "This computer's media controls could not be reached.", nullptr, rw); ry += 22; }
        else {
            textAt(dl, f.small, rx, ry, kMuted, "Player"); textRight(dl, f.bold, w - 18, ry - 1, kInk, fitText(f.bold, md.active ? (md.app.empty() ? std::string("playing") : md.app) : std::string("none"), rw - 80).c_str());
            ry += 22;
            if (md.active) { textAt(dl, f.small, rx, ry, kMuted, "Track"); textRight(dl, f.bold, w - 18, ry - 1, kInk, fitText(f.bold, md.title, rw - 80).c_str()); ry += 22; }
            textAt(dl, f.small, rx, ry, kMuted, "Radio"); textRight(dl, f.bold, w - 18, ry - 1, md.radioRx || md.radioTx ? kWarn : kInk, md.radioTx ? "transmitting" : md.radioRx ? (md.radioSource.empty() ? "receiving" : ("receiving (" + md.radioSource + ")").c_str()) : "quiet");
            ry += 22;
            textAt(dl, f.small, rx, ry, kMuted, "Media"); textRight(dl, f.bold, w - 18, ry - 1, md.ducked ? kWarn : kInk, md.ducked ? (g_settings.mediaDuck == 2 ? "paused for the radio" : "lowered for the radio") : (md.playing ? "playing" : "not playing"));
            ry += 22;
        }
        ry += 16;
        dl->AddText(f.small, fontPx(f.small), ImVec2(rx, ry), kMuted, "Paused media plays again when the radio is quiet. Receiving: X-Plane's own ATC and ATIS, xPilot's COM1 and COM2, and on Windows VATSIM voice apps (TrackAudio, swift...). Transmitting: X-Plane's and xPilot's push-to-talk.", nullptr, w - 18 - rx);
        return;
    }
    if (g_settingsTab == 1) {   // ---- Display: brightness (or auto dim), the favorites bar, the clock (the map's options are on the map)
        float y = top;
        textAt(dl, f.bold, lx, y, kInk, "Brightness");
        y += 22;
        // the slider rests (greyed) while auto dim sets the brightness, as WXPlane's dimming does
        const float autoW = 104;
        float br = g_settings.autoDim ? s.autoDimLevel : std::max(0.1f, std::min(1.0f, g_settings.brightness));
        char txt[32];
        std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(br * 100));
        if (smoothSlider(dl, f, "##brightness", ImVec2(lx, y - 2), ImVec2(lx + colW - autoW - 8, y + 30), br, 0.1f, 1.0f, 0, !g_settings.autoDim, txt) && !g_settings.autoDim) {
            g_settings.brightness = br; g_settingsChanged = true;
        }
        toggle("##autodim", lx + colW - autoW, y, autoW, "Auto dim", g_settings.autoDim, true);
        y += 34;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, g_settings.autoDim ? "Dims with the daylight: full until sunset, down to 45% once the sun is 8\xC2\xB0 below the horizon." : "Multiplied with the aircraft's own tablet brightness knob. Brighter uses the battery faster.", nullptr, colW);
        y += 40;
        textAt(dl, f.bold, lx, y, kInk, "Favorites bar");
        y += 22;
        float slots = (float)favSlots();   // steps of one icon; 0 takes the bar away
        char st2[16];
        if (favSlots() == 0) std::snprintf(st2, sizeof st2, "Off"); else std::snprintf(st2, sizeof st2, "%d icons", favSlots());
        if (smoothSlider(dl, f, "##favorites", ImVec2(lx, y - 2), ImVec2(lx + colW, y + 30), slots, 0, (float)kMaxFavorites, kMaxFavorites, true, st2)) {
            g_settings.favoriteSlots = (int)std::lround(slots); g_settingsChanged = true;
        }
        const char* clockNames[2] = {"Zulu", "Local"};
        int clk = g_settings.statusLocalTime ? 1 : 0;
        y += 46;   // under the favorites bar
        choice("##chclk", lx, y, colW, "Clock zone (tap the clock in the status bar too)", clockNames, clk, true);
        if (clk != (g_settings.statusLocalTime ? 1 : 0)) g_settings.statusLocalTime = clk == 1;
        const char* mapNames[2] = {"Light", "Dark"};
        int md = g_settings.mapDark ? 1 : 0;
        choice("##chmapd", lx, y, colW, "Map display (the street map; Satellite is set on the map)", mapNames, md, true);
        g_settings.mapDark = md == 1;
        const char* keyNames[2] = {"On-screen keys", "Physical keyboard"};
        int pk = g_settings.physicalKeys ? 1 : 0;
        choice("##chkeys", lx, y, colW, "Typing in a text box", keyNames, pk, true);
        if ((pk == 1) != g_settings.physicalKeys) { g_settings.physicalKeys = pk == 1; g_kbdKeysShown = false; }
        {   // the right column: the look, a background with its accent (all from the XPTablet logo's colours)
            float ry = top;
            textAt(dl, f.bold, rx, ry, kInk, "Color theme");
            ry += 24;
            const float tw = std::floor((colW - 4 * 8) / 5), th = std::floor(tw * 480.0f / 800.0f);   // the screen's shape
            for (int i = 0; i < 5; ++i) {   // the wallpaper itself, made small, with its accent under it
                const Backdrop& bd = kBackdrops[i];
                const Accent& ac = kAccents[i];
                const ImVec2 a(std::floor(rx + i * (tw + 8)), std::floor(ry)), b(a.x + tw, a.y + th);   // (on whole pixels: even edges)
                char id[16];
                std::snprintf(id, sizeof id, "##wp%d", i);
                ImGui::SetCursorScreenPos(a);
                if (ImGui::InvisibleButton(id, ImVec2(tw, th + 34))) { g_settings.wallpaper = i; g_settings.accent = i; g_settingsChanged = true; }
                const bool sel = g_settings.wallpaper == i, hot = ImGui::IsItemHovered();
                dl->PushClipRect(a, b, true);
                wallpaperIn(dl, a, tw, th, i, 8.0f);
                dl->PopClipRect();
                // the frame on the picture's own rounded edge; the chosen one ringed in the theme's accent
                // chosen: a ring that hugs the picture's edge all round (no dark gap between them, the same on every side)
                if (sel) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), accentLite(255), 9.0f, 0, 2.5f);
                else dl->AddRect(a, b, hot ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 40), 8.0f, 0, 1.0f);
                dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, b.y + 10), 5.0f, IM_COL32(ac.r, ac.g, ac.b, 255), 0);
                textCentered(dl, f.small, (a.x + b.x) * 0.5f, b.y + 18, sel ? kInk : kMuted, bd.name);
            }
            ry += th + 34 + 22;   // under the themes: the flight's trail on the map
            const char* trailNames[2] = {"Shown", "Hidden"};
            int tr = g_settings.mapTrail ? 0 : 1;
            choice("##chtrail", rx, ry, colW, "Flight trail on the map (since the SimBrief plan)", trailNames, tr, true);
            if ((tr == 0) != g_settings.mapTrail) { g_settings.mapTrail = tr == 0; g_settingsChanged = true; }
        }
        return;
    }

    // ---- Data: SimBrief, and where the tablet's data comes from
    const bool typing = g_kbd && g_kbdTarget == 2 && g_typeInto == &g_settings.simbriefUser;
    float y = top;
    textAt(dl, f.bold, lx, y, kInk, "SimBrief ID");
    y += 22;
    dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Your SimBrief user name or pilot ID (Account settings on simbrief.com). The Flight Plan page imports your latest plan with it.", nullptr, w - 12 - lx);
    y += 36;
    textField(dl, f, "##sbuser", g_settings.simbriefUser, ImVec2(lx, y), ImVec2(w - 12 - 200, y + 36), "Tap to enter your SimBrief ID", false);
    if (g_typeChanged) { g_typeChanged = false; g_settingsChanged = true; }
    if (pill(dl, f, "##sbimport", s.plan.busy ? "Importing..." : "Import flight plan", ImVec2(w - 12 - 192, y + 2), ImVec2(w - 12, y + 34), true, !g_settings.simbriefUser.empty() && !s.plan.busy)) {
        act.planImport = true; g_kbd = false; g_typeInto = nullptr;
    }
    y += 44;
    if (!s.plan.status.empty()) { textAt(dl, f.small, lx, y, s.plan.loaded ? kGood : kMuted, s.plan.status.c_str()); }
    y += 26;
    if (typing) { pageKeyboard(dl, f, w, h, act); return; }
    textAt(dl, f.bold, lx, y, kInk, "Data");
    y += 24;
    for (const auto& kv : s.dataInfo) {
        textAt(dl, f.small, lx, y, kMuted, kv.first.c_str());
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx + 130, y), kInk, kv.second.c_str(), nullptr, w - 12 - lx - 130 - 170);
        y += 21;
    }
    if (pill(dl, f, "##clrtiles", "Clear map cache", ImVec2(w - 12 - 150, top + 150), ImVec2(w - 12, top + 182), false, true)) act.clearTileCache = true;
    // the browser's cookies, logins, history and cache, all of it: asked twice, as it signs out of every site
    static double askedAt = -100;
    const double now = ImGui::GetTime();
    const bool asking = now - askedAt < 4.0;
    if (pill(dl, f, "##clrweb", asking ? "Tap again: erase" : "Reset browser data", ImVec2(w - 12 - 150, top + 190), ImVec2(w - 12, top + 222), asking, true)) {
        if (asking) { act.resetBrowserData = true; askedAt = -100; postNote("Browser data erased", "Signed out of every site; cookies, history and cache cleared.", APP_SETTINGS); }
        else askedAt = now;
    }
    if (asking) textRight(dl, f.small, w - 12, top + 228, kWarn, "Signs out of every site");
}

void openWeb(const char* site, const char* url, bool chrome, int group) {
    g_site = site; g_url = url; g_chrome = chrome; g_webGroup = group;
    g_navPending = url; g_navForce = false;
    if (g_kbdTarget == 1 || g_kbdTarget == 2) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; }
    g_app = APP_BROWSER;
}

} // namespace
bool trafficInRA(const MapState& m, const MapState::Traffic& t) { return tcasRA(m, t); }
int trafficThreat(const MapState& m, const MapState::Traffic& t) { return runwayConflict(m, t) ? 3 : tcasLevel(m, t); }
void mapSelect(int kind, const std::string& key) { g_selKind = kind; g_selKey = key; }
void openDispatchFromPlan() { openDispatch(true); }
int powerState() { return g_power; }   // 0 on, 1 fading off, 2 off, 3 starting, 4 waking

Fonts loadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the data is static
    cfg.OversampleH = 3; cfg.OversampleV = 2;   // finer glyph placement, so text stays crisp at any position
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

void drawPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act, int app);
void build(const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    trackCrumbs(s);   // (on every page: the trail is kept whether or not the map is open)
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
    g_logo = f.logo;
    g_icons = f.icons;
    g_mac = s.mac;
    g_media = &s.media;
    g_screenH = h;
    if (!g_settings.notifyEnabled) for (Note& n : g_notes) n.banner = false;
    watchForNotes(s);
    ImGuiStyle& st = ImGui::GetStyle();
    st.CircleTessellationMaxError = 0.30f / g_px;   // curves are as smooth on a dense screen as on a plain one
    st.CurveTessellationTol = 1.25f / g_px;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    sharp(dl);
    const double now = ImGui::GetTime();
    if (g_power == POWER_FADING && now - g_powerT0 >= kOffS) g_power = POWER_OFF;
    if (g_power == POWER_BOOT && now - g_powerT0 >= kBootS + kHoldS) { g_power = POWER_REVEAL; g_powerT0 = now; }   // (the wake starts now)
    if (g_power == POWER_REVEAL && now - g_powerT0 >= kRevealS) g_power = POWER_ON;
    {   // the battery: down while the screen is on and the charger out, up on the charger (screen on or not)
        const float dt = std::max(0.0f, std::min(1.0f, ImGui::GetIO().DeltaTime));
        const bool screenOn = g_power == POWER_ON || g_power == POWER_FADING || g_power == POWER_REVEAL || g_power == POWER_BOOT;
        if (g_charger) g_batt = std::min(100.0f, g_batt + dt * 100.0f / 1800.0f);
        else if (screenOn) {   // the brighter the screen, the faster it goes: 2 hours at full, about 5 at the dimmest (as a screen's draw does)
            const float bright = std::max(0.1f, std::min(1.0f, g_settings.autoDim ? s.autoDimLevel : g_settings.brightness));
            g_batt = std::max(0.0f, g_batt - dt * 100.0f / 7200.0f * (0.35f + 0.65f * bright));
        }
        const int p = (int)std::ceil(g_batt - 0.001f);
        if (p != g_battSaved) { g_battSaved = p; g_settingsChanged = true; }   // kept a percent at a time
        // low battery: an alert at 20 % and at 10 % on the way down (not while charging); charged above them, they can come again
        if (p > 21) g_lowWarned20 = false;
        if (p > 11) g_lowWarned10 = false;
        if (g_charger) g_lowAlert = 0;
        else if (g_power == POWER_ON) {
            if (p <= 10 && !g_lowWarned10) { g_lowWarned10 = g_lowWarned20 = true; g_lowAlert = 10; g_lowAlertT0 = now; g_ccTarget = 0; }
            else if (p <= 20 && !g_lowWarned20) { g_lowWarned20 = true; g_lowAlert = 20; g_lowAlertT0 = now; g_ccTarget = 0; }
        }
        if (g_batt <= 0.0f && !g_charger && screenOn) {   // empty: black at once
            g_power = POWER_DEAD; g_powerT0 = now; g_ccTarget = 0; g_ccT = 0; g_ccDrag = false; g_kbd = false; g_typeInto = nullptr;
        }
    }
    if (g_power == POWER_DEAD_PROMPT && now - g_powerT0 >= kDeadPromptS) g_power = POWER_DEAD;   // not tapped: it went away
    if (g_power == POWER_DEAD_CHARGE && now - g_powerT0 >= kDeadChargeShowS + kDeadChargeFadeS + kDeadChargeWaitS) {   // it starts by itself
        g_power = POWER_BOOT; g_powerT0 = now;
        g_app = APP_HOME; g_animPage = -1; g_editMode = false;
    }
    if (g_power == POWER_DEAD || g_power == POWER_DEAD_PROMPT || g_power == POWER_DEAD_CHARGE) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
        const float t = (float)(now - g_powerT0);
        // the large battery in the middle, as an iPhone's
        const float bw = 150, bh = 66, cx = w * 0.5f, cy = h * 0.44f;
        const ImVec2 ba(cx - bw * 0.5f, cy - bh * 0.5f), bb(cx + bw * 0.5f, cy + bh * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        const bool tapped = ImGui::InvisibleButton("##deadtap", ImVec2(w, h));
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const bool onBattery = mp.x >= ba.x - 24 && mp.x <= bb.x + 30 && mp.y >= ba.y - 24 && mp.y <= bb.y + 24;
        if (tapped && g_power == POWER_DEAD) { g_power = POWER_DEAD_PROMPT; g_powerT0 = now; }
        else if (tapped && g_power == POWER_DEAD_PROMPT && onBattery) { g_power = POWER_DEAD_CHARGE; g_powerT0 = now; g_charger = true; g_settingsChanged = true; }
        float al = 0;
        if (g_power == POWER_DEAD_PROMPT) al = smooth01(t / 0.3f) * (1.0f - smooth01((t - (float)kDeadPromptS + 0.8f) / 0.8f));
        if (g_power == POWER_DEAD_CHARGE) al = smooth01(t / 0.3f) * (1.0f - smooth01((t - (float)kDeadChargeShowS) / (float)kDeadChargeFadeS));
        if (al > 0.002f) {
            const int A = (int)(255 * al);
            const bool charging = g_power == POWER_DEAD_CHARGE;
            const ImU32 rim = IM_COL32(120, 124, 132, A);
            dl->AddRect(ba, bb, rim, 14.0f, 0, 3.0f);
            dl->AddRectFilled(ImVec2(bb.x + 4, cy - 12), ImVec2(bb.x + 10, cy + 12), rim, 3.0f);   // the cap
            const float lvl = std::max(0.06f, g_batt / 100.0f);
            const ImU32 fill = charging ? IM_COL32(76, 217, 100, A) : IM_COL32(255, 59, 48, A);
            dl->AddRectFilled(ImVec2(ba.x + 6, ba.y + 6), ImVec2(ba.x + 6 + (bw - 12) * lvl, bb.y - 6), fill, 9.0f);
            if (charging) {   // a bolt across it, the level under it
                const ImU32 bolt = IM_COL32(255, 255, 255, A);
                dl->AddTriangleFilled(ImVec2(cx + 9, cy - 28), ImVec2(cx - 14, cy + 4), ImVec2(cx + 2, cy + 4), bolt);
                dl->AddTriangleFilled(ImVec2(cx - 9, cy + 28), ImVec2(cx + 14, cy - 4), ImVec2(cx - 2, cy - 4), bolt);
                char pb[16];
                std::snprintf(pb, sizeof pb, "%d%%", (int)std::ceil(g_batt - 0.001f));
                textCentered(dl, f.bold, cx, bb.y + 26, IM_COL32(220, 224, 232, A), pb);
            } else {   // the cable to plug in, up towards it
                const ImU32 c2 = IM_COL32(150, 154, 162, A);
                const float ty = bb.y + 34;
                dl->AddRectFilled(ImVec2(cx - 9, ty), ImVec2(cx + 9, ty + 26), c2, 4.0f);     // the plug
                dl->AddRectFilled(ImVec2(cx - 5, ty - 12), ImVec2(cx + 5, ty), c2, 2.0f);     // its tip
                dl->AddRectFilled(ImVec2(cx - 3, ty + 26), ImVec2(cx + 3, h), c2);              // the cable, down off the screen
                textCentered(dl, f.small, cx, ba.y - 34, IM_COL32(180, 184, 192, A), "Tap the battery to plug in the charger");
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    if (g_power == POWER_OFF || g_power == POWER_BOOT) {   // a black screen: nothing else runs; a tap anywhere starts it
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        if (ImGui::InvisibleButton("##wake", ImVec2(w, h)) && g_power == POWER_OFF) {
            g_power = POWER_BOOT; g_powerT0 = ImGui::GetTime();
            g_app = APP_HOME; g_animPage = -1; g_editMode = false; g_kbd = false; g_typeInto = nullptr;
        }
        if (g_power == POWER_BOOT) {   // the logo, and the bar filling under it
            const float t = (float)(ImGui::GetTime() - g_powerT0), al = smooth01(t / 0.35f);
            const int A = (int)(255 * al);
            if (g_logo) {
                int lw = 0, lh = 0, lv = 0;
                logoPixels(lw, lh, lv);
                const float hh = 44.0f, ww = hh * (float)lw / (float)std::max(1, lh);
                dl->AddImage(g_logo, ImVec2(w * 0.5f - ww * 0.5f, h * 0.42f - hh * 0.5f), ImVec2(w * 0.5f + ww * 0.5f, h * 0.42f + hh * 0.5f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, A));
            }
            const float bw = 180, by = h * 0.42f + 50;
            dl->AddRectFilled(ImVec2(w * 0.5f - bw * 0.5f, by), ImVec2(w * 0.5f + bw * 0.5f, by + 4), IM_COL32(255, 255, 255, (int)(46 * al)), 2.0f);
            const float p = bootProgress(t);
            if (p > 0.01f) dl->AddRectFilled(ImVec2(w * 0.5f - bw * 0.5f, by), ImVec2(w * 0.5f - bw * 0.5f + bw * p, by + 4), IM_COL32(255, 255, 255, A), 2.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    const bool powerAnim = g_power != POWER_ON;
    if (powerAnim) ImGui::BeginDisabled();   // fading out, or fading in: no taps yet
    wallpaper(dl, w, h);
    if (!videoFull(s)) statusBar(dl, f, s, w);   // (a full-screen video covers it, and its buttons are not there to be hit)
    // the pull-down or a banner on top: what is under them takes no presses (and stays as it is)
    bool covered = g_ccT > 0.001f || g_bannerDrag >= 0 || g_lowAlert != 0;
    {
        ImVec2 ba, bb; float bal; Note* bn;
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (bannerRect(w, ba, bb, bal, bn) && mp.x >= ba.x && mp.x <= bb.x && mp.y >= ba.y && mp.y <= bb.y + 14) covered = true;
    }
    const float daCover = st.DisabledAlpha;
    if (covered) { st.DisabledAlpha = 1.0f; ImGui::BeginDisabled(); }
    // and the pointer is hidden from what is under it: some pages read presses themselves (home's icons, the map, the scratchpad)
    ImGuiIO& cio = ImGui::GetIO();
    const ImVec2 heldPos = cio.MousePos, heldDelta = cio.MouseDelta;
    const bool heldDown = cio.MouseDown[0], heldClicked = cio.MouseClicked[0], heldReleased = cio.MouseReleased[0];
    const float heldWheel = cio.MouseWheel;
    if (covered) {
        cio.MousePos = ImVec2(-FLT_MAX, -FLT_MAX); cio.MouseDelta = ImVec2(0, 0);
        cio.MouseDown[0] = false; cio.MouseClicked[0] = false; cio.MouseReleased[0] = false; cio.MouseWheel = 0;
    }
    const double since = ImGui::GetTime() - g_animT0;
    const bool onHome = g_app < 0 || g_app >= APP_COUNT;
    const bool opening = !onHome && g_animOpening && since < kOpenS;
    const bool closing = onHome && !g_animOpening && since < kCloseS && g_animPage >= 0 && g_animPage < APP_COUNT;
    if (onHome || opening) {   // the home screen (still, under a page growing out of it)
        if (opening) ImGui::BeginDisabled();
        home(dl, f, w, h);
        if (opening) ImGui::EndDisabled();
    }
    if (!onHome || closing) {
        const int app = onHome ? g_animPage : g_app;
        Actions closingAct;   // a closing page asks for nothing
        Actions& pa = closing ? closingAct : act;
        const float da = st.DisabledAlpha;
        if (closing) { st.DisabledAlpha = 1.0f; ImGui::BeginDisabled(); }
        dl->AddDrawCmd();   // the page's own commands, so it can be moved and scaled as a whole
        const int vtx0 = dl->VtxBuffer.Size, cmd0 = dl->CmdBuffer.Size - 1;
        drawPage(dl, f, s, w, h, pa, app);
        if (closing) g_embed = EmbedArea();   // a page on its way out asks WXPlane for nothing
        // the page grows out of its icon (and back into it) solid, as on an iPad: no fading
        if (opening) placePage(dl, vtx0, cmd0, w, h, easeOut((float)(since / kOpenS)), 1.0f);
        if (closing) {   // shrinks back gently (easing in and out), and melts into its icon over the last part instead of vanishing
            const float t = (float)(since / kCloseS);
            const float e = t * t * t * (t * (t * 6 - 15) + 10);
            placePage(dl, vtx0, cmd0, w, h, 1.0f - e, t < 0.55f ? 1.0f : 1.0f - (t - 0.55f) / 0.45f);
        }
        if (closing) { ImGui::EndDisabled(); st.DisabledAlpha = da; }
    }
    if (covered) {
        ImGui::EndDisabled(); st.DisabledAlpha = daCover;
        cio.MousePos = heldPos; cio.MouseDelta = heldDelta;   // the pointer back, for what is on top
        cio.MouseDown[0] = heldDown; cio.MouseClicked[0] = heldClicked; cio.MouseReleased[0] = heldReleased; cio.MouseWheel = heldWheel;
    }
    if (g_ccT > 0.001f) g_embed = EmbedArea();   // web pages and WXPlane stay under the pull-down
    if (!powerAnim) { drawPullDown(dl, f, s, w, h, act); drawBanner(dl, f, w); }
    if (!powerAnim && g_lowAlert) {   // the low battery alert, as an iPhone's: over everything, Close or Plug In
        const float t = (float)(ImGui::GetTime() - g_lowAlertT0), e = smooth01(t / 0.25f);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(110 * e)));
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##lowback", ImVec2(w, h));   // nothing under it takes a press
        const float aw = 300, ah = 150, sc = 0.92f + 0.08f * e;
        const ImVec2 c(w * 0.5f, h * 0.5f);
        const ImVec2 a(c.x - aw * 0.5f * sc, c.y - ah * 0.5f * sc), b(c.x + aw * 0.5f * sc, c.y + ah * 0.5f * sc);
        glass(dl, a, b, 18.0f, e);
        char t1[48], t2[64];
        std::snprintf(t1, sizeof t1, "Low Battery");
        std::snprintf(t2, sizeof t2, "%d%% of battery remaining", g_lowAlert);
        const int A = (int)(255 * e);
        textCentered(dl, f.bold, c.x, a.y + 22, IM_COL32(240, 244, 250, A), t1);
        textCentered(dl, f.small, c.x, a.y + 48, IM_COL32(190, 198, 214, A), t2);
        const float by = b.y - 46;
        dl->AddLine(ImVec2(a.x, by), ImVec2(b.x, by), IM_COL32(255, 255, 255, (int)(40 * e)));
        dl->AddLine(ImVec2(c.x, by), ImVec2(c.x, b.y), IM_COL32(255, 255, 255, (int)(40 * e)));
        auto button = [&](const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool bold, ImDrawFlags corner) {
            ImGui::SetCursorScreenPos(p0);
            const bool hit = ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
            // the highlight fits the button: square on the inside, the alert's own rounded corner on the outside
            if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), IM_COL32(255, 255, 255, ImGui::IsItemActive() ? 30 : 18), 17.0f * sc, corner);
            textCentered(dl, bold ? f.bold : f.body, (p0.x + p1.x) * 0.5f, std::floor((p0.y + p1.y) * 0.5f - fontPx(f.body) * 0.5f), accentLite(A), label);
            return hit;
        };
        if (button("##lowclose", "Close", ImVec2(a.x, by), ImVec2(c.x, b.y), false, ImDrawFlags_RoundCornersBottomLeft)) g_lowAlert = 0;
        if (button("##lowplug", "Plug In", ImVec2(c.x, by), ImVec2(b.x, b.y), true, ImDrawFlags_RoundCornersBottomRight)) { g_lowAlert = 0; g_charger = true; g_settingsChanged = true; }
    }
    if (powerAnim) {
        ImGui::EndDisabled();
        g_embed = EmbedArea();   // web pages and WXPlane stay hidden while the screen fades
        const float t = (float)(ImGui::GetTime() - g_powerT0);
        if (g_power == POWER_FADING) {   // powering off: a fade to black
            const float k = t / (float)kOffS;
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(255 * std::min(1.0f, k * k * (3 - 2 * k)))));
        } else {   // waking: the black lifts off home while the logo rises and shrinks into the status bar
            const float e = smooth01(t / (float)kRevealS);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(255 * (1 - e))));
            const float bar = 1.0f - smooth01(t / 0.25f);   // the loading bar fades first
            if (bar > 0) dl->AddRectFilled(ImVec2(w * 0.5f - 90, h * 0.42f + 50), ImVec2(w * 0.5f + 90, h * 0.42f + 54), IM_COL32(255, 255, 255, (int)(255 * bar)), 2.0f);
            if (g_logo) {
                ImVec2 a1, s1;
                statusLogoRect(w, a1, s1);
                const float hh = 44.0f + (s1.y - 44.0f) * e, ww = hh * s1.x / s1.y;
                const float cy = h * 0.42f + (a1.y + s1.y * 0.5f - h * 0.42f) * e;
                dl->AddImage(g_logo, ImVec2(w * 0.5f - ww * 0.5f, cy - hh * 0.5f), ImVec2(w * 0.5f + ww * 0.5f, cy + hh * 0.5f));
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// One page, with its title bar
void drawPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act, int app) {
    {
        // pages sit on a darkened wallpaper of their own (solid, so nothing behind shows through while one grows out of its icon)
        dl->PushClipRect(ImVec2(0, kStatusH), ImVec2(w, h), true);
        wallpaper(dl, w, h);
        dl->PopClipRect();
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), themeTone(8, 18, 36, 170));   // the veil, in the theme's tone
        if (!(app == APP_BROWSER && videoFull(s)) && pageHeader(dl, f, w, app == APP_BROWSER ? g_site.c_str() : appName(app))) openApp(APP_HOME);
        if (app == APP_SETTINGS) settingsPage(dl, f, s, w, h, act);
        else if (app == APP_BROWSER) browserPage(dl, f, s, w, h, act);
        else if (app == APP_MAP) mapPage(dl, f, s, w, h, act);
        else if (app == APP_NOTES) notesPage(dl, f, w, h, act);
        else if (app == APP_ROUTES) flightPlanPage(dl, f, s, w, h, act);
        else if (app == APP_CHECKLISTS) checklistsPage(dl, f, s, w, h);
        else if (app == APP_AIRPORTS) airportsPage(dl, f, s, w, h, act);
        else if (app == APP_DOCUMENTS) documentsPage(dl, f, s, w, h, act);
        else if (app == APP_WEATHER) weatherPage(dl, f, s, w, h, act);
        else if (app == APP_GROUND) groundPage(dl, f, s, w, h, act);
        else if (app == APP_MUSIC) musicPage(dl, f, s, w, h, act);
        else if (app == APP_DISPATCH) dispatchPage(dl, f, s, w, h, act);
        else placeholderPage(dl, f, app, w, h);
    }
}

int currentApp() { return g_app; }
void notify(const std::string& title, const std::string& body, int app) { postNote(title, body, app); }
int notificationCount() { return (int)g_notes.size(); }
bool pullDownOpen() { return g_ccT > 0.5f; }

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

std::string checklistFor(const std::string& text, const std::string& icao, const std::string& engine) {
    std::string out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) continue;
            const std::string tags = "," + line.substr(1, close - 1) + ",";
            if (tags.find("," + icao + ",") == std::string::npos && (engine.empty() || tags.find("," + engine + ",") == std::string::npos)) continue;
            line = line.substr(close + 1);
            while (!line.empty() && line[0] == ' ') line.erase(0, 1);
        }
        out += line + "\n";
    }
    return out;
}
void setBattery(float pct) { g_batt = std::max(0.0f, std::min(100.0f, pct)); }
bool hasShape(const std::string& icao) { float l, sp; return exactShape(icao, l, sp) != nullptr; }   // a drawing of its own
const unsigned char* wxLogoPixels(unsigned int& bytes) { bytes = xpt_asset_wxlogo_size; return xpt_asset_wxlogo; }
const unsigned char* logoPixels(int& w, int& h, int& levels) {   // "XPTL", width, height, levels (u32), then the pixels
    const unsigned char* b = xpt_asset_logo;
    auto u32 = [&](int at) { return (int)((unsigned)b[at] | ((unsigned)b[at + 1] << 8) | ((unsigned)b[at + 2] << 16) | ((unsigned)b[at + 3] << 24)); };
    if (xpt_asset_logo_size < 16 || std::memcmp(b, "XPTL", 4) != 0) { w = h = levels = 0; return nullptr; }
    w = u32(4); h = u32(8); levels = u32(12);
    return b + 16;
}
const unsigned char* iconsPixels(int& w, int& h, int& levels) {
    const unsigned char* b = xpt_asset_icons;
    auto u32 = [&](int at) { return (int)((unsigned)b[at] | ((unsigned)b[at + 1] << 8) | ((unsigned)b[at + 2] << 16) | ((unsigned)b[at + 3] << 24)); };
    if (xpt_asset_icons_size < 16 || std::memcmp(b, "XPTL", 4) != 0) { w = h = levels = 0; return nullptr; }
    w = u32(4); h = u32(8); levels = u32(12);
    return b + 16;
}
int touchTestCount() { return (int)g_touches.size(); }
const EmbedArea& embedArea() { return g_embed; }
const char* browserUrl() { return g_url.c_str(); }
bool browserChrome() { return g_chrome; }
int chimeSeq() { return g_chimeSeq; }
bool webTyping() { return g_app == APP_BROWSER && g_kbd && g_kbdTarget == 0; }   // a web page's text field has the keyboard

const char* appName(int app) {
    if (app == APP_WEATHER && g_wx) return "WXPlane";
    return (app >= 0 && app < APP_COUNT) ? kApps[app].name : "Home";
}

bool appVisible(int app) {
    switch (app) {
    case APP_CHARTS: return g_settings.showCharts;
    case APP_NAVIGRAPH: return g_settings.showNavigraph;
    case APP_SIMBRIEF: return g_settings.showSimbrief;
    case APP_LIVEMAP: return g_settings.showLive;
    case APP_VATSIM: return g_settings.showVatsim;
    case APP_FOREFLIGHT: return g_settings.showForeflight;
    case APP_SKYVECTOR: return g_settings.showSkyvector;
    case APP_PERFORMANCE: return false;                // removed: accurate figures need the aircraft maker's data
    case APP_CHECKLISTS: return true;   // on every aircraft, ToLiss too (its own checklists, else the airliner set)
    case APP_TRAFFIC: return false;                    // traffic is a layer of the Map
    case APP_MUSIC: return !g_mac && g_settings.mediaEnabled;   // macOS gives no media controls
    default: return app >= 0 && app < APP_COUNT;
    }
}

// Charts, Live Map, SimBrief and ForeFlight are sites shown as pages of their own. The Browser icon opens the full browser.
void openApp(int app) {
    if (g_kbdTarget == 2) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; }   // a page's keyboard closes with the page
    const bool fromHome = g_app < 0 || g_app >= APP_COUNT;
    if (app < 0 || app >= APP_COUNT || !appVisible(app)) {
        if (!fromHome) { g_animPage = g_app; startPageAnim(false, -1); }   // the page shrinks back into its icon
        g_app = APP_HOME;
        return;
    }
    if (fromHome) { g_editMode = false; startPageAnim(true, app); }   // it grows out of its icon
    if (kApps[app].url) { openWeb(kApps[app].name, kApps[app].url, false, 100 + app); return; }   // its own tabs
    if (app == APP_BROWSER) { g_site = "Browser"; g_chrome = true; g_webGroup = 0; }
    g_app = app;
}

std::string notesToText() {
    std::string out;
    for (int i = 0; i < 6; ++i) out += "craft" + std::to_string(i) + "=" + escapeLine(g_craft[i]) + "\n";
    for (int i = 0; i < 9; ++i) out += "atis" + std::to_string(i) + "=" + escapeLine(g_atis[i]) + "\n";
    out += "scratch=" + escapeLine(g_scratch) + "\n";
    for (const Stroke& st : g_strokes) {   // pen colour, then the points (thousandths of the pad)
        out += "stroke=" + std::to_string(st.color);
        for (const ImVec2& q : st.pts) out += " " + std::to_string((int)std::lround(q.x * 1000)) + "," + std::to_string((int)std::lround(q.y * 1000));
        out += "\n";
    }
    for (const auto& dm : g_docMarks)   // a document's markup: its file, a tab, then each line as the scratchpad's (ten-thousandths)
        for (const Stroke& st : dm.second) {
            out += "docmark=" + escapeLine(dm.first) + "\t" + std::to_string(st.color);
            for (const ImVec2& q : st.pts) out += " " + std::to_string((int)std::lround(q.x * 10000)) + "," + std::to_string((int)std::lround(q.y * 10000));
            out += "\n";
        }
    return out;
}
void notesFromText(const std::string& text) {
    size_t p = 0;
    g_strokes.clear();
    g_docMarks.clear();
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        p = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k.compare(0, 5, "craft") == 0) { const int i = std::atoi(k.c_str() + 5); if (i >= 0 && i < 6) g_craft[i] = unescapeLine(v); }
        else if (k.compare(0, 4, "atis") == 0) { const int i = std::atoi(k.c_str() + 4); if (i >= 0 && i < 9) g_atis[i] = unescapeLine(v); }
        else if (k == "scratch") g_scratch = unescapeLine(v);
        else if (k == "stroke") {
            Stroke st;
            st.color = std::atoi(v.c_str());
            size_t q = v.find(' ');
            while (q != std::string::npos && q < v.size()) {
                int x = 0, y = 0;
                if (std::sscanf(v.c_str() + q + 1, "%d,%d", &x, &y) == 2) st.pts.push_back(ImVec2(x / 1000.0f, y / 1000.0f));
                q = v.find(' ', q + 1);
            }
            if (!st.pts.empty()) g_strokes.push_back(st);
        } else if (k == "docmark") {
            const size_t tab = v.find('\t');
            if (tab == std::string::npos) continue;
            Stroke st;
            const std::string rest = v.substr(tab + 1);
            st.color = std::atoi(rest.c_str());
            size_t q = rest.find(' ');
            while (q != std::string::npos && q < rest.size()) {
                int x = 0, y = 0;
                if (std::sscanf(rest.c_str() + q + 1, "%d,%d", &x, &y) == 2) st.pts.push_back(ImVec2(x / 10000.0f, y / 10000.0f));
                q = rest.find(' ', q + 1);
            }
            if (!st.pts.empty()) g_docMarks[unescapeLine(v.substr(0, tab))].push_back(st);
        }
    }
}
bool takeNotesChanged() { const bool c = g_notesChanged; g_notesChanged = false; return c; }
void mapCommand(int what) {
    if (what == 0) g_mapRange = std::max(0, g_mapRange - 1);
    else if (what == 1) g_mapRange = std::min(kRangeN - 1, g_mapRange + 1);
    else if (what == 2) g_mapFollow = true;
    else if (what >= 3 && what <= 6) {   // a quarter of the map's width or height, in the direction on screen
        const double x = what == 3 ? -g_mapLastW * 0.25 : what == 4 ? g_mapLastW * 0.25 : 0.0;
        const double y = what == 5 ? -g_mapLastH * 0.25 : what == 6 ? g_mapLastH * 0.25 : 0.0;
        const double cr = std::cos(g_mapLastRot), sr = std::sin(g_mapLastRot), sc = g_mapLastScale;
        const MercPt from = g_mapGlideStart >= 0 ? g_mapGlideTo : g_mapCenter;
        MercPt to{from.x + (x * cr - y * sr) / sc, from.y + (x * sr + y * cr) / sc};
        to.y = std::max(0.0, std::min(1.0, to.y));
        g_mapGlideFrom = g_mapCenter; g_mapGlideTo = to; g_mapGlideStart = ImGui::GetTime();   // a pan glides there
        g_mapFollow = false;
    }
}
void mapView(double& lat, double& lon, int& zoom, float& verticalRangeNm) {
    latLonOf(g_mapGlideStart >= 0 ? g_mapGlideTo : g_mapCenter, lat, lon);   // gliding: where it is going
    zoom = (int)std::lround(g_mapZoom);
    verticalRangeNm = (float)(g_mapLastH / std::max(1e-9, g_mapLastScale) * g_mapLastMPerMerc / 1852.0);
}
void openDocuments(const std::string& folder) { g_docFilter = folder; g_docListScroll = 0; openApp(APP_DOCUMENTS); }

void showOnMap(double lat, double lon) {
    g_mapGoto = false; g_mapFollow = false; g_mapSeen = true;
    g_mapCenter = mercOf(lat, lon);
    g_mapRange = kRangeTaxi; g_mapShownNm = kRangesNm[kRangeTaxi];
    openApp(APP_MAP);
}

Settings& settings() { return g_settings; }
bool takeSettingsChanged() { const bool c = g_settingsChanged; g_settingsChanged = false; return c; }

std::string settingsToText() {
    normalizeLayout();
    char buf[512];   // (room for the longest line of settings)
    std::snprintf(buf, sizeof buf, "favorite_slots=%d\nbrightness=%.2f\nshow_charts=%d\nshow_navigraph=%d\nshow_simbrief=%d\nshow_live=%d\nshow_vatsim=%d\nshow_foreflight=%d\nshow_skyvector=%d\n",
                  favSlots(), (double)std::max(0.1f, std::min(1.0f, g_settings.brightness)), g_settings.showCharts ? 1 : 0, g_settings.showNavigraph ? 1 : 0,
                  g_settings.showSimbrief ? 1 : 0, g_settings.showLive ? 1 : 0, g_settings.showVatsim ? 1 : 0, g_settings.showForeflight ? 1 : 0, g_settings.showSkyvector ? 1 : 0);
    std::string out = buf;
    std::snprintf(buf, sizeof buf, "media_enabled=%d\nmedia_duck=%d\nnotify_enabled=%d\nnotify_tcas=%d\nnotify_simbrief=%d\n", g_settings.mediaEnabled ? 1 : 0,
                  std::max(0, std::min(2, g_settings.mediaDuck)), g_settings.notifyEnabled ? 1 : 0, g_settings.notifyTcas ? 1 : 0, g_settings.notifySimbrief ? 1 : 0);
    out += buf;
    std::snprintf(buf, sizeof buf, "map_navaids=%d\nmap_waypoints=%d\nmap_route=%d\nstatus_local_time=%d\nauto_dim=%d\n", g_settings.mapNavaids ? 1 : 0, g_settings.mapWaypoints ? 1 : 0,
                  g_settings.mapRoute ? 1 : 0, g_settings.statusLocalTime ? 1 : 0, g_settings.autoDim ? 1 : 0);
    out += buf;
    std::snprintf(buf, sizeof buf, "duck_rx=%.2f\nduck_tx=%.2f\nnotify_volume=%.2f\n", (double)g_settings.duckRx, (double)g_settings.duckTx, (double)g_settings.notifyVolume);
    out += buf;
    std::snprintf(buf, sizeof buf, "map_stands=%d\nmap_best_runway=%d\nmap_daylight=%d\nmap_traffic_source=%d\nmap_traffic_trails=%d\nmap_traffic_route=%d\nmap_traffic_range=%.0f\nmap_traffic_alt=%.0f\n",
                  g_settings.mapStands ? 1 : 0, g_settings.mapBestRunway ? 1 : 0, g_settings.mapDaylight ? 1 : 0, g_settings.mapTrafficSource, g_settings.mapTrafficTrails ? 1 : 0,
                  g_settings.mapTrafficRoute ? 1 : 0, (double)g_settings.mapTrafficRangeNm, (double)g_settings.mapTrafficAltFt);
    out += buf;
    {   // the SimBrief name, without line breaks
        std::string u = g_settings.simbriefUser;
        for (char& c : u) if (c == '\n' || c == '\r') c = ' ';
        out += "simbrief_user=" + u + "\n";
    }
    std::snprintf(buf, sizeof buf, "map_source=%d\nmap_airports=%d\nmap_timezones=%d\nmap_heading_up=%d\nmap_local_time=%d\nmap_traffic=%d\nmap_outlines=%d\nmap_dark=%d\nmap_ils=%d\nmap_rings=%d\nmap_trail=%d\nphysical_keys=%d\nwallpaper=%d\naccent=%d\nbattery=%d\ncharger=%d\n",
                  g_settings.mapSource, g_settings.mapAirports ? 1 : 0, g_settings.mapTimeZones ? 1 : 0, g_settings.mapHeadingUp ? 1 : 0, g_settings.mapLocalTime ? 1 : 0,
                  g_settings.mapTraffic ? 1 : 0, g_settings.mapOutlines ? 1 : 0, g_settings.mapDark ? 1 : 0, g_settings.mapIls ? 1 : 0, g_settings.mapRings ? 1 : 0, g_settings.mapTrail ? 1 : 0, g_settings.physicalKeys ? 1 : 0, g_settings.wallpaper, g_settings.accent, (int)std::ceil(g_batt - 0.001f), g_charger ? 1 : 0);
    out += buf;
    auto list = [&](const char* key, const std::vector<int>& v) {
        out += key; out += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ","; out += v[i] >= 0 && v[i] < APP_COUNT ? kApps[v[i]].id : "-"; }   // "-": a free place
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
    int oldChart = 0, oldLive = 0, oldPlan = 0;
    bool newStyle = false;
    auto parseList = [](const std::string& val, std::vector<int>& v) {
        v.clear();
        size_t p = 0;
        while (p <= val.size()) {
            size_t e = val.find(',', p);
            if (e == std::string::npos) e = val.size();
            const std::string id = val.substr(p, e - p);
            if (id == "-") v.push_back(-1);
            else for (int a = 0; a < APP_COUNT; ++a) if (id == kApps[a].id) v.push_back(a);
            p = e + 1;
        }
    };
    g_settings.bookmarks.clear();   // (read again, the file's bookmarks replace these rather than adding a second set)
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
        // before 0.9, each kind was one icon set to one site: that site's icon is the one shown
        if (key == "charts_provider") oldChart = v;
        else if (key == "live_provider") oldLive = v;
        else if (key == "plan_provider") oldPlan = v;
        else if (key == "show_navigraph") { g_settings.showNavigraph = v != 0; newStyle = true; }
        else if (key == "show_vatsim") g_settings.showVatsim = v != 0;
        else if (key == "show_skyvector") g_settings.showSkyvector = v != 0;
        else if (key == "map_source") g_settings.mapSource = v == 1 || v == 2 ? v : 0;
        else if (key == "map_airports") g_settings.mapAirports = v != 0;
        else if (key == "map_timezones") g_settings.mapTimeZones = v != 0;
        else if (key == "map_heading_up") g_settings.mapHeadingUp = v != 0;
        else if (key == "map_dark") g_settings.mapDark = v != 0;
        else if (key == "map_local_time") g_settings.mapLocalTime = v != 0;
        else if (key == "map_traffic") g_settings.mapTraffic = v != 0;
        else if (key == "map_outlines") g_settings.mapOutlines = v != 0;
        else if (key == "map_navaids") g_settings.mapNavaids = v != 0;
        else if (key == "map_ils") g_settings.mapIls = v != 0;
        else if (key == "map_rings") g_settings.mapRings = v != 0;
        else if (key == "map_trail") g_settings.mapTrail = v != 0;
        else if (key == "map_stands") g_settings.mapStands = v != 0;
        else if (key == "map_best_runway") g_settings.mapBestRunway = v != 0;
        else if (key == "map_daylight") g_settings.mapDaylight = v != 0;
        else if (key == "map_traffic_source") g_settings.mapTrafficSource = v == 1 ? 1 : 0;
        else if (key == "map_traffic_trails") g_settings.mapTrafficTrails = v != 0;
        else if (key == "map_traffic_route") g_settings.mapTrafficRoute = v != 0;
        else if (key == "map_traffic_range") g_settings.mapTrafficRangeNm = std::max(0.0f, std::min(2000.0f, (float)std::atof(val.c_str())));
        else if (key == "map_traffic_alt") g_settings.mapTrafficAltFt = std::max(0.0f, std::min(60000.0f, (float)std::atof(val.c_str())));
        else if (key == "physical_keys") g_settings.physicalKeys = v != 0;
        else if (key == "wallpaper") { g_settings.wallpaper = std::max(0, std::min(4, v)); g_settings.accent = g_settings.wallpaper; }
        else if (key == "accent") g_settings.accent = std::max(0, std::min(4, v));
        else if (key == "battery") g_batt = (float)std::max(0, std::min(100, v));
        else if (key == "charger") g_charger = v != 0;
        else if (key == "media_enabled") g_settings.mediaEnabled = v != 0;
        else if (key == "media_duck") g_settings.mediaDuck = std::max(0, std::min(2, v));
        else if (key == "duck_rx") g_settings.duckRx = std::max(0.0f, std::min(1.0f, (float)std::atof(val.c_str())));
        else if (key == "duck_tx") g_settings.duckTx = std::max(0.0f, std::min(1.0f, (float)std::atof(val.c_str())));
        else if (key == "notify_volume") g_settings.notifyVolume = std::max(0.0f, std::min(1.0f, (float)std::atof(val.c_str())));
        else if (key == "notify_enabled") g_settings.notifyEnabled = v != 0;
        else if (key == "notify_tcas") g_settings.notifyTcas = v != 0;
        else if (key == "notify_simbrief") g_settings.notifySimbrief = v != 0;
        else if (key == "map_waypoints") g_settings.mapWaypoints = v != 0;
        else if (key == "map_route") g_settings.mapRoute = v != 0;
        else if (key == "simbrief_user") g_settings.simbriefUser = val;
        else if (key == "status_local_time") g_settings.statusLocalTime = v != 0;
        else if (key == "auto_dim") g_settings.autoDim = v != 0;
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
    if (!newStyle) {   // an older settings file: the site its one icon was set to
        if (oldChart == 1) { g_settings.showNavigraph = g_settings.showCharts; g_settings.showCharts = false; }
        if (oldLive == 1) { g_settings.showVatsim = g_settings.showLive; g_settings.showLive = false; }
        if (oldPlan == 1) { g_settings.showSkyvector = g_settings.showForeflight; g_settings.showForeflight = false; }
    }
    normalizeLayout();
}

} // namespace xpt
