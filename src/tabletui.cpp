#include "tabletui.h"
#include "airspace_parse.h"
#include "airlines.h"
#include "imgui_internal.h"
#include "arcade.h"
#include "acars.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <array>
#include <sstream>
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
float itemHover();                                          // the last item's hover, eased
float openness(const char* id, bool open, float rate = 14.0f);   // a menu opening and closing, eased
void resetTablet();   // Reset All (below)
void seasonalStep();   // the seasonal themes (below)
void resetLogAndTimers();   // Reset All: the logbook and the timers (below)
void acarsStep(const SimState& s, Actions& act);   // ACARS: what came up, into its log (below)
void logStep(const SimState& s);   // the logbook's watch on the flight (below)
// a finger's drag up or down over a list scrolls it, even begun on one of its rows (whose tap is then let go); the mouse's too
bool g_touchScrolling = false;
bool g_touchInput = false;   // the last press came from a finger (a phone or tablet browser): the map opens its cards on a double tap
void touchScroll(float& scroll, ImVec2 a, ImVec2 b) {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.MouseDown[0]) return;
    const ImVec2 c = io.MouseClickedPos[0];
    if (c.x < a.x || c.x > b.x || c.y < a.y || c.y > b.y) return;
    const float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
    if (!g_touchScrolling && std::fabs(dy) > 8 && std::fabs(dy) > std::fabs(dx) * 1.2f) g_touchScrolling = true;
    if (!g_touchScrolling) return;
    scroll -= io.MouseDelta.y;
    if (ImGui::GetActiveID() != 0) ImGui::ClearActiveID();   // (the row pressed: no tap when the finger lifts)
}
void popIn(ImDrawList* dl, int vtx0, ImVec2 origin, float t);  // drawn since vtx0: grown and faded in with t
bool pill(ImDrawList* dl, const Fonts& f, const char* id, const char* label, ImVec2 a, ImVec2 b, bool on, bool enabled);
double distNmLL(double la1, double lo1, double la2, double lo2, double* brgOut);
const char* surfaceName(int s);
extern std::string g_aptSel;
extern float g_aptScroll;

// XPTablet's own icons each have a colour of their own: no two a shade of the same one (a light and a deep tone far apart, as sky
// blue and navy, may both be used; the brands' tiles keep their brands' colours)
struct AppDef { const char* id; const char* name; ImU32 color; const char* url; };   // id: its name in the settings file; url: a web app
const AppDef kApps[APP_COUNT] = {
    {"map", "Map", IM_COL32(46, 170, 96, 255), nullptr},                  {"airports", "Airports", IM_COL32(86, 180, 250, 255), nullptr},
    {"charts", "ChartFox", IM_COL32(236, 152, 50, 255), "https://chartfox.org"},            {"documents", "Documents", IM_COL32(186, 152, 108, 255), nullptr},
    {"routes", "Flight Plan", IM_COL32(150, 96, 250, 255), nullptr},            {"weather", "Weather", IM_COL32(0, 196, 200, 255), nullptr},
    {"ground", "Aircraft", IM_COL32(246, 190, 48, 255), nullptr},            {"checklists", "Checklists", IM_COL32(214, 64, 160, 255), nullptr},
    {"performance", "Performance", IM_COL32(22, 100, 66, 255), nullptr},  {"traffic", "Traffic", IM_COL32(255, 124, 104, 255), nullptr},
    {"notes", "Notes", IM_COL32(255, 138, 40, 255), nullptr},
    {"simbrief", "SimBrief", IM_COL32(196, 58, 66, 255), "https://dispatch.simbrief.com"},
    {"livemap", "Volanta", IM_COL32(112, 72, 255, 255), "https://fly.volanta.app"},   // Volanta's violet (approximated)
    {"foreflight", "ForeFlight", IM_COL32(30, 140, 222, 255), "https://plan.foreflight.com"},
    {"browser", "Browser", IM_COL32(0, 112, 124, 255), nullptr},          {"settings", "Settings", IM_COL32(122, 130, 146, 255), nullptr},
    {"navigraph", "Navigraph", IM_COL32(28, 52, 92, 255), "https://charts.navigraph.com"},
    {"vatsim", "VATSIM Radar", IM_COL32(18, 28, 52, 255), "https://vatsim-radar.com"},
    {"skyvector", "SkyVector", IM_COL32(32, 132, 108, 255), "https://skyvector.com"},
    {"music", "Media", IM_COL32(236, 48, 56, 255), nullptr},   // (its id stays "music": saved home screens keep it)
    {"dispatch", "Dispatch", IM_COL32(128, 86, 58, 255), nullptr},   // the SimBrief briefing (its PDF)
    {"navsearch", "Navaids", IM_COL32(132, 190, 40, 255), nullptr},   // X-Plane's navaids, fixes and airports, searched
    {"appstore", "App Store", IM_COL32(70, 56, 160, 255), nullptr},   // deep indigo, a white bag (as the other icons: one white glyph on its tile)
    {"games", "Games", IM_COL32(255, 128, 176, 255), nullptr},
    {"datis", "D-ATIS", IM_COL32(64, 68, 76, 255), "https://datis.clowd.io"},
    {"spotify", "Spotify", IM_COL32(30, 185, 84, 255), "https://open.spotify.com"},
    {"ytmusic", "YouTube Music", IM_COL32(230, 33, 23, 255), "https://music.youtube.com"},
    {"applemusic", "Apple Music", IM_COL32(250, 60, 90, 255), "https://music.apple.com"},
    {"soundcloud", "SoundCloud", IM_COL32(255, 85, 0, 255), "https://soundcloud.com"},
    {"youtube", "YouTube", IM_COL32(220, 30, 30, 255), "https://www.youtube.com"},
    {"vatsimhub", "VATSIM", IM_COL32(40, 70, 160, 255), nullptr},   // with AIP / Stats bubbles
    {"nattrak", "natTrak", IM_COL32(20, 90, 150, 255), "https://nattrak.vatsim.net"},
    {"ivao", "IVAO Webeye", IM_COL32(20, 80, 200, 255), "https://webeye.ivao.aero"},
    {"timers", "Timers", IM_COL32(30, 50, 110, 255), nullptr},
    {"logbook", "Logbook", IM_COL32(132, 32, 52, 255), nullptr},
    {"acars", "ACARS", IM_COL32(124, 130, 50, 255), nullptr},
    {"xpilot", "xPilot", IM_COL32(30, 44, 70, 255), nullptr},   // xPilot's own windows: a page of the VATSIM app (not in the App Store, never on the home screen)
    {"fr24", "Flightradar24", IM_COL32(16, 16, 18, 255), "https://www.flightradar24.com"},   // real-world traffic, live
};
enum { ICON_VOLANTA = APP_COUNT, ICON_VATSIM, ICON_SKYVECTOR };   // pictures of their own (the other web apps borrow one)

bool g_wx = false;             // WXPlane is installed (told by the host each frame): the Weather icon and page are WXPlane's
ImTextureID g_wxLogo = 0;
Settings g_settings;
// the App Store's apps: on the home screen only once installed (the others were always there, their Settings switches too)
bool g_installedRead = false;   // the settings file had its list (else D-ATIS comes installed)
bool storeApp(int app) { return app == APP_GAMES || (app >= APP_DATIS && app < APP_COUNT); }
bool installed(int app) {
    if (app == APP_XPILOT) return false;   // (a page of the VATSIM app now: never an icon, even where it was installed before)
    if (!storeApp(app)) return true;
    for (const std::string& id : g_settings.installed) if (id == kApps[app].id) return true;
    return false;
}
bool g_settingsChanged = false;
std::string g_url = "https://chartfox.org", g_site = "Browser";   // what the web page shows
int favSlots() { return std::max(0, std::min(kMaxFavorites, g_settings.favoriteSlots)); }
bool g_mac = false;              // macOS (told by the host): no media controls
bool g_ownPerformance = false;   // the aircraft brings its own performance calculator (ToLiss): no Performance app
bool g_chrome = true;                                             // with the browser's own controls
double g_subT0 = -10;   // when an app's own page (VATSIM's Stats, xPilot...) was opened or left: the page fades in
int g_backTo = -1;    // a page opened from inside another app (VATSIM's Stats, natTrak, xPilot): Back returns there, not Home
int g_webGroup = 0;   // whose tabs are shown: 0 the Browser's (and documents'), else a web app's own (100 + its app)
bool g_webHold = false;   // the shown tab is not this group's yet (one moment): no picture from another group's page

void openWeb(const char* site, const char* url, bool chrome, int group = 0);
double localOffsetAt(const MapState& m, double lat, double lon, bool& estimate);
void fillConcave(ImDrawList* dl, std::vector<ImVec2> v, ImU32 col);
std::string zoneOf(double lat, double lon, double offS, long long utc);
ImTextureID g_logo = 0;   // XPTablet's logo texture (from the host)
ImTextureID g_logoMask = 0;   // the logo in white, drawn in the theme's colours (Noir, Cloud, the Customs and the seasonal themes)
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
    case APP_SOUNDCLOUD: return 20;
    case APP_YOUTUBE: return 21;
    case APP_YTMUSIC: return 22;
    case APP_APPLEMUSIC: return 23;
    case APP_VATSIMHUB: return 24;   // VATSIM
    case APP_NATTRAK: return 25;
    case APP_IVAO: return 26;        // IVAO Webeye
    case APP_SPOTIFY: return 27;
    case APP_FR24: return 28;        // Flightradar24
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
    {"Ocean", IM_COL32(9, 22, 44, 255), IM_COL32(24, 58, 104, 255), IM_COL32(20, 46, 86, 255), IM_COL32(14, 34, 66, 255)},
    {"Lavender", IM_COL32(16, 12, 42, 255), IM_COL32(60, 40, 118, 255), IM_COL32(46, 30, 96, 255), IM_COL32(32, 22, 72, 255)},
    {"Dusk", IM_COL32(22, 10, 34, 255), IM_COL32(100, 36, 96, 255), IM_COL32(76, 28, 78, 255), IM_COL32(52, 20, 58, 255)},
    {"Sunset", IM_COL32(20, 14, 40, 255), IM_COL32(150, 66, 72, 255), IM_COL32(104, 44, 64, 255), IM_COL32(64, 28, 52, 255)},
    {"Amber", IM_COL32(20, 16, 30, 255), IM_COL32(146, 96, 42, 255), IM_COL32(104, 66, 38, 255), IM_COL32(64, 40, 30, 255)},
};
struct Accent { const char* name; int r, g, b; };
// each background has its accent: Navy the blue XPTablet always had, the others the logo colour their sky is made of
const Accent kAccents[5] = {{"Blue", 70, 140, 255}, {"Violet", 150, 100, 235}, {"Magenta", 205, 80, 170}, {"Coral", 240, 96, 96}, {"Gold", 236, 176, 56}};
// ---- the seasonal themes: one for each major US holiday, two awareness months and the four seasons; each offered only while it is on (by the
// computer's own date), and the theme chosen before it comes back when it ends
struct Seasonal { const char* name; Backdrop bd; Accent ac; };
const Seasonal kSeasonal[] = {
    {"New Year", {"New Year", IM_COL32(6, 8, 20, 255), IM_COL32(38, 36, 70, 255), IM_COL32(28, 26, 52, 255), IM_COL32(16, 16, 34, 255)}, {"Gold", 236, 196, 92}},
    {"", {"", IM_COL32(9, 22, 44, 255), IM_COL32(24, 58, 104, 255), IM_COL32(20, 46, 86, 255), IM_COL32(14, 34, 66, 255)}, {"Blue", 70, 140, 255}},   // (1: a theme taken out; its place kept, so the others keep their numbers in saved settings. Never offered)
    {"Valentine's", {"Valentine's", IM_COL32(30, 6, 18, 255), IM_COL32(150, 40, 80, 255), IM_COL32(112, 28, 62, 255), IM_COL32(70, 16, 40, 255)}, {"Rose", 255, 110, 150}},
    {"Presidents' Day", {"Presidents' Day", IM_COL32(8, 14, 36, 255), IM_COL32(40, 60, 118, 255), IM_COL32(30, 44, 92, 255), IM_COL32(18, 28, 62, 255)}, {"Red", 226, 72, 72}},
    {"St. Patrick's", {"St. Patrick's", IM_COL32(4, 22, 12, 255), IM_COL32(22, 110, 58, 255), IM_COL32(16, 82, 44, 255), IM_COL32(10, 54, 30, 255)}, {"Shamrock", 96, 214, 112}},
    {"Easter", {"Easter", IM_COL32(40, 46, 86, 255), IM_COL32(150, 170, 214, 255), IM_COL32(120, 160, 150, 255), IM_COL32(86, 130, 110, 255)}, {"Lilac", 190, 150, 240}},
    {"Memorial Day", {"Memorial Day", IM_COL32(8, 12, 28, 255), IM_COL32(44, 54, 92, 255), IM_COL32(32, 40, 70, 255), IM_COL32(20, 26, 46, 255)}, {"Poppy", 214, 56, 56}},
    {"", {"", IM_COL32(9, 22, 44, 255), IM_COL32(24, 58, 104, 255), IM_COL32(20, 46, 86, 255), IM_COL32(14, 34, 66, 255)}, {"Blue", 70, 140, 255}},   // (7: likewise taken out, its place kept)
    {"Independence Day", {"Independence Day", IM_COL32(6, 10, 32, 255), IM_COL32(36, 52, 120, 255), IM_COL32(120, 30, 44, 255), IM_COL32(70, 20, 34, 255)}, {"Red", 236, 64, 72}},
    {"Labor Day", {"Labor Day", IM_COL32(10, 14, 22, 255), IM_COL32(58, 74, 96, 255), IM_COL32(44, 56, 74, 255), IM_COL32(28, 36, 50, 255)}, {"Hi-vis", 255, 140, 40}},
    {"Halloween", {"Halloween", IM_COL32(10, 4, 18, 255), IM_COL32(70, 28, 96, 255), IM_COL32(40, 16, 56, 255), IM_COL32(14, 8, 16, 255)}, {"Pumpkin", 255, 128, 24}},
    {"Veterans Day", {"Veterans Day", IM_COL32(10, 14, 12, 255), IM_COL32(64, 76, 56, 255), IM_COL32(46, 56, 42, 255), IM_COL32(28, 34, 26, 255)}, {"Gold", 222, 180, 80}},
    {"Thanksgiving", {"Thanksgiving", IM_COL32(26, 12, 22, 255), IM_COL32(150, 70, 44, 255), IM_COL32(104, 48, 30, 255), IM_COL32(60, 28, 22, 255)}, {"Harvest", 244, 160, 52}},
    {"Christmas", {"Christmas", IM_COL32(4, 16, 12, 255), IM_COL32(24, 80, 52, 255), IM_COL32(18, 60, 40, 255), IM_COL32(10, 36, 24, 255)}, {"Holly", 222, 52, 60}},
    {"Men's Mental Health", {"Men's Mental Health", IM_COL32(6, 14, 34, 255), IM_COL32(30, 74, 150, 255), IM_COL32(22, 56, 116, 255), IM_COL32(14, 36, 78, 255)}, {"Blue", 70, 160, 255}},
    {"Breast Cancer Awareness", {"Breast Cancer Awareness", IM_COL32(30, 10, 24, 255), IM_COL32(170, 70, 120, 255), IM_COL32(130, 50, 96, 255), IM_COL32(80, 30, 60, 255)}, {"Pink", 255, 130, 180}},
    // the four seasons (by the calendar's months, as the weather services count them: spring March to May, and so on)
    {"Spring", {"Spring", IM_COL32(34, 72, 110, 255), IM_COL32(128, 190, 190, 255), IM_COL32(72, 140, 100, 255), IM_COL32(46, 104, 76, 255)}, {"Blossom", 255, 150, 190}},
    {"Summer", {"Summer", IM_COL32(14, 58, 120, 255), IM_COL32(84, 170, 224, 255), IM_COL32(40, 132, 104, 255), IM_COL32(24, 96, 80, 255)}, {"Sun", 255, 196, 60}},
    {"Fall", {"Fall", IM_COL32(30, 16, 12, 255), IM_COL32(170, 88, 34, 255), IM_COL32(122, 58, 26, 255), IM_COL32(74, 36, 20, 255)}, {"Maple", 236, 120, 40}},
    {"Winter", {"Winter", IM_COL32(8, 18, 42, 255), IM_COL32(74, 104, 152, 255), IM_COL32(120, 142, 176, 255), IM_COL32(84, 104, 140, 255)}, {"Ice", 130, 200, 255}},
};
constexpr int kSeasonalN = (int)(sizeof kSeasonal / sizeof kSeasonal[0]);
constexpr int kSeasonal0 = 10;   // the first seasonal theme's number
constexpr int kThemes = kSeasonal0 + kSeasonalN;   // 0-4 from the logo, 5 Noir, 6 Cloud, 7-9 the three Customs, then the seasonal ones
bool g_seasonalAll = false;   // every seasonal theme offered whatever the date (the browser emulator, to look at them)
// the nth weekday (0 Sunday) of a month (n 1-5; -1 the last), as a day of the month
int nthWeekday(int year, int month, int wday, int n) {
    std::tm t{}; t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = 1; t.tm_hour = 12; std::mktime(&t);
    const int first = 1 + (wday - t.tm_wday + 7) % 7;
    if (n > 0) return first + (n - 1) * 7;
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = dim[month - 1]; if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days = 29;
    int d = first; while (d + 7 <= days) d += 7; return d;
}
int dayOfYear(int year, int month, int day) { std::tm t{}; t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day; t.tm_hour = 12; std::mktime(&t); return t.tm_yday; }
int easterDoy(int y) {   // Easter Sunday (the Gregorian computus), as a day of the year
    const int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7, m = (a + 11 * h + 22 * l) / 451;
    const int month = (h + l - 7 * m + 114) / 31, day = (h + l - 7 * m + 114) % 31 + 1;
    return dayOfYear(y, month, day);
}
// is seasonal theme s on, on this date: each holiday a few days before it until it is over; the awareness months all month
bool seasonalOn(int s, int year, int month, int day) {
    const int doy = dayOfYear(year, month, day);
    auto within = [&](int m, int d, int before, int after) { const int h = dayOfYear(year, m, d); return doy >= h - before && doy <= h + after; };
    switch (s) {
    case 0: return (month == 12 && day == 31) || (month == 1 && day <= 2);                       // New Year: Dec 31 to Jan 2
    case 2: return month == 2 && day >= 7 && day <= 14;                                         // Valentine's Day: Feb 7-14
    case 3: return within(2, nthWeekday(year, 2, 1, 3), 2, 0);                                   // Presidents' Day: 3rd Monday of February
    case 4: return month == 3 && day >= 14 && day <= 17;                                        // St. Patrick's Day: Mar 14-17
    case 5: { const int e = easterDoy(year); return doy >= e - 7 && doy <= e + 1; }             // Easter: Palm Sunday to Easter Monday
    case 6: return within(5, nthWeekday(year, 5, 1, -1), 2, 0);                                  // Memorial Day: last Monday of May
    case 8: return (month == 6 && day == 30) || (month == 7 && day <= 5);                        // Independence Day: Jun 30 to Jul 5
    case 9: return within(9, nthWeekday(year, 9, 1, 1), 2, 0);                                   // Labor Day: 1st Monday of September
    case 10: return month == 10 && day >= 17;                                                   // Halloween: Oct 17-31
    case 11: return month == 11 && day >= 9 && day <= 11;                                       // Veterans Day: Nov 9-11
    case 12: return within(11, nthWeekday(year, 11, 4, 4), 3, 3);                                // Thanksgiving: 4th Thursday of November, its week
    case 13: return month == 12 && (day == 24 || day == 25);                                    // Christmas: Christmas Eve and the day itself
    case 14: return month == 6;                                                                 // Men's Mental Health Month (US Men's Health Month): June
    case 15: return month == 10;                                                                // Breast Cancer Awareness Month: October
    case 16: return month >= 3 && month <= 5;                                                   // Spring: March to May
    case 17: return month >= 6 && month <= 8;                                                   // Summer: June to August
    case 18: return month >= 9 && month <= 11;                                                  // Fall: September to November
    case 19: return month == 12 || month <= 2;                                                  // Winter: December to February
    default: return false;
    }
}
bool christmasTheme() { return std::max(0, std::min(kThemes - 1, g_settings.wallpaper)) == kSeasonal0 + 13; }
int specialCraft() { const int t = std::max(0, std::min(kThemes - 1, g_settings.wallpaper)); return t == kSeasonal0 + 13 ? 1 : t == kSeasonal0 + 10 ? 2 : t == kSeasonal0 + 12 ? 3 : t == kSeasonal0 + 8 ? 4 : 0; }
bool seasonalAvailable(int s) {
    if (s == 1 || s == 7) return false;   // (the two places of themes taken out: never offered, and one saved there goes back to the theme before)
    if (g_seasonalAll) return true;
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    return seasonalOn(s, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}
// the Custom theme's picture, as the host has loaded it (set each frame from the SimState)
ImTextureID g_bgTex = 0;
int g_bgW = 0, g_bgH = 0;
std::string g_bgPic;
int themeIndex() { return std::max(0, std::min(kThemes - 1, g_settings.wallpaper)); }
ImU32 rgbCol(int rgb, float k = 1.0f) {   // 0xRRGGBB, darkened by k
    auto c = [&](int sh) { return (int)std::lround(((rgb >> sh) & 255) * k); };
    return IM_COL32(c(16), c(8), c(0), 255);
}
// a theme's sky: the five from the logo; Noir, dark greys; Cloud, a pale blue day; Custom, from the colour chosen
Backdrop themeBackdrop(int which) {
    which = std::max(0, std::min(kThemes - 1, which));
    if (which < 5) return kBackdrops[which];
    if (which == 5) return {"Noir", IM_COL32(8, 8, 10, 255), IM_COL32(44, 46, 50, 255), IM_COL32(32, 33, 36, 255), IM_COL32(21, 22, 25, 255)};
    if (which == 6) return {"Cloud", IM_COL32(58, 116, 190, 255), IM_COL32(176, 210, 238, 255), IM_COL32(124, 170, 214, 255), IM_COL32(96, 146, 198, 255)};
    if (which >= kSeasonal0) return kSeasonal[which - kSeasonal0].bd;
    static const char* names[3] = {"Custom 1", "Custom 2", "Custom 3"};
    const int bg = g_settings.customs[which - 7].bg;
    return {names[which - 7], rgbCol(bg, 0.30f), rgbCol(bg, 1.0f), rgbCol(bg, 0.78f), rgbCol(bg, 0.56f)};
}
Accent themeAccent() {
    const int i = themeIndex();
    if (i < 5) return kAccents[i];
    if (i == 5) return {"Silver", 132, 142, 160};
    if (i == 6) return {"Sky", 36, 128, 232};
    if (i >= kSeasonal0) return kSeasonal[i - kSeasonal0].ac;
    const int a = g_settings.customs[i - 7].accent;
    return {"Custom", (a >> 16) & 255, (a >> 8) & 255, a & 255};
}
// the text colours follow the theme: moved by a share of how far the theme's accent is from Navy's blue (Navy: exactly as before)
ImU32 themeText(int r, int g, int b, float share) {
    const Accent c = themeAccent();
    const Accent& n = kAccents[0];
    auto ch = [&](int base, int t, int nv) { return std::max(0, std::min(255, base + (int)std::lround((t - nv) * share))); };
    return IM_COL32(ch(r, c.r, n.r), ch(g, c.g, n.g), ch(b, c.b, n.b), 255);
}
#define kInk themeText(236, 241, 250, 0.05f)     // the main text: white with the faintest tint
#define kMuted themeText(150, 168, 198, 0.32f)   // the quieter text: labels, hints, card titles
ImU32 accentCol(int a) { const Accent c = themeAccent(); return IM_COL32(c.r, c.g, c.b, std::max(0, std::min(255, a))); }
ImU32 accentLite(int a);
ImU32 mixCol(ImU32 a, ImU32 b, float t);
// text on the accent: white, unless the accent is so bright white would not read on it (under 3:1); then a deep shade of the accent
bool accentIsLight() {
    const Accent c = themeAccent();
    auto lin = [](int v) { const float x = v / 255.0f; return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); };
    const float L = 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
    return 1.05f / (L + 0.05f) < 2.5f;   // (the tablet's own blues and violets keep white text)
}
ImU32 onAccent() { return accentIsLight() ? mixCol(accentCol(255), IM_COL32(0, 0, 0, 255), 0.8f) : kInk; }
// the route on the map: the theme's accent, a little lighter so it shows on the street map and the imagery
ImU32 routeCol(int a) { const ImU32 c = mixCol(accentCol(255), IM_COL32(255, 255, 255, 255), 0.12f); return (c & 0x00FFFFFF) | ((ImU32)std::max(0, std::min(255, a)) << 24); }
ImU32 accentLite(int a) {   // the lighter one: rings, lines, focus
    const Accent c = themeAccent();
    return IM_COL32(c.r + (255 - c.r) * 2 / 5, c.g + (255 - c.g) * 2 / 5, c.b + (255 - c.b) * 2 / 5, std::max(0, std::min(255, a)));
}
// ---- the seasonal themes' pictures: drawn over the sky (stage 0, behind the mountains) and on the hills (stage 1, in front),
// scaled to the box (the screen, or a little picture of it in Settings). Simple shapes of our own: nothing copied.
float decoRnd(int i, int k) { unsigned x = (unsigned)(i * 7919 + k * 104729 + 12345); x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (x % 10000) / 10000.0f; }
void decoStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float rot = 0) {
    ImVec2 p[10];
    for (int i = 0; i < 10; ++i) { const float a = rot - 1.5708f + i * 0.6283f, rr = i % 2 ? r * 0.42f : r; p[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr); }
    for (int i = 0; i < 10; i += 2) dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], col), dl->AddTriangleFilled(c, p[i], p[(i + 9) % 10], col);
}
void decoHeart(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.2f), r * 0.55f, col, 16);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y - r * 0.2f), r * 0.55f, col, 16);
    dl->AddTriangleFilled(ImVec2(c.x - r * 1.02f, c.y), ImVec2(c.x + r * 1.02f, c.y), ImVec2(c.x, c.y + r * 1.15f), col);
}
void decoBurst(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int rays) {   // a firework: rays out from its middle, a dot at each end
    for (int i = 0; i < rays; ++i) {
        const float a = i * 6.2832f / rays, ca = std::cos(a), sa = std::sin(a);
        dl->AddLine(ImVec2(c.x + ca * r * 0.25f, c.y + sa * r * 0.25f), ImVec2(c.x + ca * r * 0.9f, c.y + sa * r * 0.9f), (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * 176 / 255) << 24), std::max(1.0f, r * 0.06f));
        dl->AddCircleFilled(ImVec2(c.x + ca * r, c.y + sa * r), std::max(1.0f, r * 0.07f), col, 8);
    }
}
void decoClover(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    for (int i = 0; i < 3; ++i) { const float a = -1.5708f + i * 2.0944f; dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * r * 0.5f, c.y + std::sin(a) * r * 0.5f), r * 0.48f, col, 14); }
    dl->AddLine(c, ImVec2(c.x + r * 0.3f, c.y + r * 1.1f), col, std::max(1.0f, r * 0.14f));
}
void decoRibbon(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {   // an awareness ribbon: a loop at the top, its two tails crossing down
    const float t = std::max(2.0f, r * 0.22f);
    dl->PathArcTo(ImVec2(c.x, c.y - r * 0.45f), r * 0.42f, 3.4f, 6.0f + 0.0f, 20); dl->PathStroke(col, 0, t);
    dl->AddBezierCubic(ImVec2(c.x - r * 0.40f, c.y - r * 0.30f), ImVec2(c.x - r * 0.30f, c.y + r * 0.1f), ImVec2(c.x + r * 0.2f, c.y + r * 0.5f), ImVec2(c.x + r * 0.55f, c.y + r * 1.0f), col, t);
    dl->AddBezierCubic(ImVec2(c.x + r * 0.40f, c.y - r * 0.30f), ImVec2(c.x + r * 0.30f, c.y + r * 0.1f), ImVec2(c.x - r * 0.2f, c.y + r * 0.5f), ImVec2(c.x - r * 0.55f, c.y + r * 1.0f), col, t);
}
void decoTree(ImDrawList* dl, ImVec2 base, float r, ImU32 col) {   // a fir: three tiers and a trunk
    dl->AddRectFilled(ImVec2(base.x - r * 0.08f, base.y - r * 0.25f), ImVec2(base.x + r * 0.08f, base.y), IM_COL32(40, 26, 18, 255));
    for (int i = 0; i < 3; ++i) {
        const float y0 = base.y - r * 0.2f - i * r * 0.45f, ww = r * (0.55f - i * 0.12f);
        dl->AddTriangleFilled(ImVec2(base.x - ww, y0), ImVec2(base.x + ww, y0), ImVec2(base.x, y0 - r * 0.7f), col);
    }
}
void decoPumpkin(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 o = IM_COL32(232, 110, 24, 255), d = IM_COL32(190, 80, 16, 255);
    dl->AddEllipseFilled(ImVec2(c.x - r * 0.42f, c.y), ImVec2(r * 0.55f, r * 0.75f), d);
    dl->AddEllipseFilled(ImVec2(c.x + r * 0.42f, c.y), ImVec2(r * 0.55f, r * 0.75f), d);
    dl->AddEllipseFilled(c, ImVec2(r * 0.6f, r * 0.8f), o);
    dl->AddRectFilled(ImVec2(c.x - r * 0.08f, c.y - r * 1.05f), ImVec2(c.x + r * 0.1f, c.y - r * 0.7f), IM_COL32(70, 90, 30, 255), r * 0.05f);
    const ImU32 g = IM_COL32(255, 220, 90, 255);   // its face, lit
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.1f), ImVec2(c.x - r * 0.15f, c.y - r * 0.1f), ImVec2(c.x - r * 0.3f, c.y - r * 0.35f), g);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.15f, c.y - r * 0.1f), ImVec2(c.x + r * 0.45f, c.y - r * 0.1f), ImVec2(c.x + r * 0.3f, c.y - r * 0.35f), g);
    dl->AddRectFilled(ImVec2(c.x - r * 0.4f, c.y + r * 0.15f), ImVec2(c.x + r * 0.4f, c.y + r * 0.3f), g, r * 0.06f);
}
void decoBat(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float lift = 0) {   // lift: its wings raised (above 0) or lowered, as it beats them
    dl->AddEllipseFilled(c, ImVec2(r * 0.18f, r * 0.26f), col);
    for (int s = -1; s <= 1; s += 2) {
        const ImVec2 p[5] = {ImVec2(c.x + s * r * 0.12f, c.y - r * 0.1f), ImVec2(c.x + s * r * 0.6f, c.y - r * (0.35f + lift * 0.5f)), ImVec2(c.x + s * r * 1.0f, c.y - r * (0.05f + lift)),
                             ImVec2(c.x + s * r * 0.7f, c.y + r * (0.02f - lift * 0.7f)), ImVec2(c.x + s * r * 0.45f, c.y + r * 0.15f)};
        dl->AddTriangleFilled(p[0], p[1], p[4], col); dl->AddTriangleFilled(p[1], p[2], p[3], col); dl->AddTriangleFilled(p[1], p[3], p[4], col);
    }
}
void decoLeaf(ImDrawList* dl, ImVec2 c, float r, float rot, ImU32 col) {
    const float ca = std::cos(rot), sa = std::sin(rot);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
    dl->AddQuadFilled(P(0, -r), P(r * 0.55f, 0), P(0, r), P(-r * 0.55f, 0), col);
    dl->AddLine(P(0, -r * 0.8f), P(0, r * 1.25f), (col & 0x00FFFFFF) | 0x90000000, std::max(1.0f, r * 0.08f));
}
void decoEgg(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 band) {
    dl->AddEllipseFilled(c, ImVec2(r * 0.72f, r), col);
    dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.08f), ImVec2(c.x + r * 0.7f, c.y + r * 0.12f), band);
}
void decoFlower(ImDrawList* dl, ImVec2 c, float r, ImU32 petal, ImU32 mid) {
    for (int i = 0; i < 5; ++i) { const float a = i * 1.2566f; dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * r * 0.5f, c.y + std::sin(a) * r * 0.5f), r * 0.45f, petal, 12); }
    dl->AddCircleFilled(c, r * 0.3f, mid, 12);
}
void decoGourd(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 rib) {   // a pumpkin as it grows: its lobes, a stem, no face
    dl->AddEllipseFilled(ImVec2(c.x - r * 0.46f, c.y), ImVec2(r * 0.52f, r * 0.72f), rib);
    dl->AddEllipseFilled(ImVec2(c.x + r * 0.46f, c.y), ImVec2(r * 0.52f, r * 0.72f), rib);
    dl->AddEllipseFilled(c, ImVec2(r * 0.58f, r * 0.78f), col);
    dl->AddEllipseFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.2f), ImVec2(r * 0.16f, r * 0.42f), IM_COL32(255, 255, 255, 34));
    dl->AddRectFilled(ImVec2(c.x - r * 0.08f, c.y - r * 1.04f), ImVec2(c.x + r * 0.10f, c.y - r * 0.68f), IM_COL32(86, 100, 40, 255), r * 0.05f);
}
void decoWheat(ImDrawList* dl, ImVec2 base, float hgt, float lean, ImU32 col) {   // a stalk of wheat: its ear of grains at the top
    const ImVec2 top(base.x + lean * hgt, base.y - hgt);
    dl->AddLine(base, top, col, std::max(1.0f, hgt * 0.022f));
    for (int i = 0; i < 6; ++i) {
        const float t = 1.0f - i * 0.055f;
        const ImVec2 p(base.x + lean * hgt * t, base.y - hgt * t);
        for (int sd = -1; sd <= 1; sd += 2) dl->AddEllipseFilled(ImVec2(p.x + sd * hgt * 0.03f, p.y), ImVec2(hgt * 0.022f, hgt * 0.045f), col, sd * 0.5f + lean, 8);
    }
}
// a horn of plenty on its side, its mouth to the left: wicker rings narrowing to a curled tail, and the harvest spilling out
void decoCornucopia(ImDrawList* dl, ImVec2 c, float s) {
    const int n = 16;
    for (int j = 0; j <= n; ++j) {   // from the tail to the mouth, each ring over the one before
        const float t = (float)j / n, e = 1 - t;
        const ImVec2 p(c.x + s * (1.02f * e - 0.10f * e * e * e), c.y - s * (0.62f * e * e + 0.20f * e * e * e * e));
        const float r = s * (0.07f + 0.35f * t);
        dl->AddCircleFilled(p, r, j % 2 ? IM_COL32(176, 118, 60, 255) : IM_COL32(150, 96, 46, 255), 24);
        dl->AddCircle(p, r, IM_COL32(104, 62, 28, 150), 24, std::max(1.0f, s * 0.012f));
    }
    dl->AddEllipseFilled(ImVec2(c.x - s * 0.02f, c.y), ImVec2(s * 0.20f, s * 0.40f), IM_COL32(58, 32, 16, 255), 0, 28);   // its mouth
    dl->AddEllipse(ImVec2(c.x - s * 0.02f, c.y), ImVec2(s * 0.20f, s * 0.40f), IM_COL32(204, 150, 84, 255), 0, 28, std::max(1.0f, s * 0.03f));
    // the harvest: a pumpkin, apples, a pear, grapes, an ear of corn
    decoGourd(dl, ImVec2(c.x - s * 0.52f, c.y + s * 0.12f), s * 0.30f, IM_COL32(232, 120, 30, 255), IM_COL32(196, 90, 20, 255));
    dl->AddEllipseFilled(ImVec2(c.x - s * 0.30f, c.y + s * 0.02f), ImVec2(s * 0.09f, s * 0.26f), IM_COL32(240, 200, 70, 255), 1.05f, 16);   // corn
    dl->AddEllipseFilled(ImVec2(c.x - s * 0.20f, c.y + s * 0.08f), ImVec2(s * 0.07f, s * 0.20f), IM_COL32(120, 150, 60, 255), 0.75f, 14);   // (its husk)
    dl->AddCircleFilled(ImVec2(c.x - s * 0.14f, c.y + s * 0.24f), s * 0.14f, IM_COL32(200, 44, 44, 255), 20);                               // apples
    dl->AddCircleFilled(ImVec2(c.x - s * 0.18f, c.y + s * 0.20f), s * 0.04f, IM_COL32(255, 255, 255, 60), 10);
    dl->AddCircleFilled(ImVec2(c.x + s * 0.02f, c.y + s * 0.30f), s * 0.12f, IM_COL32(170, 196, 70, 255), 20);                              // a pear
    dl->AddCircleFilled(ImVec2(c.x + s * 0.02f, c.y + s * 0.17f), s * 0.07f, IM_COL32(170, 196, 70, 255), 16);
    for (int g = 0; g < 7; ++g) dl->AddCircleFilled(ImVec2(c.x - s * (0.44f - (g % 3) * 0.085f + (g / 3) * 0.04f), c.y + s * (0.36f + (g / 3) * 0.075f)), s * 0.05f, IM_COL32(120, 60, 150, 255), 12);   // grapes
}
void decoFlake(ImDrawList* dl, ImVec2 c, float r, float rot, ImU32 col) {   // a snowflake: six arms, a pair of barbs on each
    const float th = std::max(1.0f, r * 0.10f);
    for (int i = 0; i < 6; ++i) {
        const float a = rot + i * 1.0472f, ca = std::cos(a), sa = std::sin(a);
        dl->AddLine(c, ImVec2(c.x + ca * r, c.y + sa * r), col, th);
        const ImVec2 m(c.x + ca * r * 0.62f, c.y + sa * r * 0.62f);
        for (int sd = -1; sd <= 1; sd += 2) { const float b = a + sd * 0.9f; dl->AddLine(m, ImVec2(m.x + std::cos(b) * r * 0.30f, m.y + std::sin(b) * r * 0.30f), col, th); }
    }
}
void decoCloud(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(ImVec2(c.x - r * 0.9f, c.y + r * 0.15f), r * 0.55f, col, 20); dl->AddCircleFilled(ImVec2(c.x - r * 0.25f, c.y - r * 0.2f), r * 0.75f, col, 24);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y), r * 0.62f, col, 20); dl->AddCircleFilled(ImVec2(c.x + r * 1.1f, c.y + r * 0.2f), r * 0.42f, col, 18);
    dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y + r * 0.1f), ImVec2(c.x + r * 1.1f, c.y + r * 0.62f), col, r * 0.3f);
}
void decoRoundTree(ImDrawList* dl, ImVec2 base, float r, ImU32 col) {   // a broadleaf tree: a trunk and a round crown
    dl->AddRectFilled(ImVec2(base.x - r * 0.10f, base.y - r * 0.9f), ImVec2(base.x + r * 0.10f, base.y), IM_COL32(70, 42, 26, 255));
    dl->AddCircleFilled(ImVec2(base.x - r * 0.38f, base.y - r * 1.0f), r * 0.50f, col, 20); dl->AddCircleFilled(ImVec2(base.x + r * 0.38f, base.y - r * 1.0f), r * 0.50f, col, 20);
    dl->AddCircleFilled(ImVec2(base.x, base.y - r * 1.38f), r * 0.58f, col, 22);
}
void decoBalloon(ImDrawList* dl, ImVec2 c, float r, ImU32 a, ImU32 b) {   // a hot-air balloon: its striped envelope, the lines, the basket
    dl->AddCircleFilled(c, r, a, 28);
    dl->AddEllipseFilled(c, ImVec2(r * 0.62f, r), b, 0, 24); dl->AddEllipseFilled(c, ImVec2(r * 0.26f, r), a, 0, 20);
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.72f, c.y + r * 0.68f), ImVec2(c.x + r * 0.72f, c.y + r * 0.68f), ImVec2(c.x, c.y + r * 1.5f), a);
    dl->AddLine(ImVec2(c.x - r * 0.3f, c.y + r * 1.2f), ImVec2(c.x - r * 0.2f, c.y + r * 1.62f), IM_COL32(60, 40, 30, 255), std::max(1.0f, r * 0.04f));
    dl->AddLine(ImVec2(c.x + r * 0.3f, c.y + r * 1.2f), ImVec2(c.x + r * 0.2f, c.y + r * 1.62f), IM_COL32(60, 40, 30, 255), std::max(1.0f, r * 0.04f));
    dl->AddRectFilled(ImVec2(c.x - r * 0.24f, c.y + r * 1.6f), ImVec2(c.x + r * 0.24f, c.y + r * 1.9f), IM_COL32(120, 80, 44, 255), r * 0.05f);
}
void seasonalDecor(ImDrawList* dl, ImVec2 o, float w, float h, int k, int stage) {
    const float u = h / 480.0f;   // (the screen is 480 tall: everything in proportion)
    auto at = [&](float fx, float fy) { return ImVec2(o.x + fx * w, o.y + fy * h); };
    const float tm = (float)ImGui::GetTime();
    auto fade = [](ImU32 col, float k2) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * std::max(0.0f, std::min(1.0f, k2))) << 24); };
    // something breathing, slowly: 1 - depth .. 1, over secs, each (which) at its own point in it
    auto breathe = [&](float secs, float depth, float which) { return 1.0f - depth * (0.5f + 0.5f * std::sin(tm * 6.2832f / secs + which * 2.4f)); };
    // the stars: each twinkling in its own slow time (some seconds dim, some bright)
    auto stars = [&](int n, float maxY, ImU32 col) { for (int i = 0; i < n; ++i) dl->AddCircleFilled(at(decoRnd(i, k + 1), decoRnd(i, k + 2) * maxY), std::max(0.6f, (0.8f + decoRnd(i, 3) * 1.4f) * u),
                                                                                                     fade(col, breathe(7.0f + decoRnd(i, 5) * 9.0f, 0.6f, i * 1.7f)), 6); };
    // Things adrift in the air (snow, leaves, petals), slowly: each on its own lane, swaying a little, turning, and coming round
    // again. secs: how long one takes to cross the screen; up: rising instead of falling. draw(where, a size 0..1, its turn, which,
    // how much of it shows: it fades in and out at the edges).
    auto adrift = [&](int n, int salt, float secs, bool up, const std::function<void(ImVec2, float, float, int, float)>& draw) {
        for (int i = 0; i < n; ++i) {
            const float ph = decoRnd(i, salt), sp = 0.7f + 0.6f * decoRnd(i, salt + 1);
            float t = ph + tm * sp / secs;
            t -= std::floor(t);
            const float y = (up ? 1 - t : t) * 1.14f - 0.07f;
            const float x = decoRnd(i, salt + 2) + std::sin(tm * (0.18f + 0.2f * decoRnd(i, salt + 3)) + ph * 6.2832f) * 0.035f + (t - 0.5f) * 0.06f * (decoRnd(i, salt + 4) - 0.5f);
            const float show = std::min(1.0f, std::min(t, 1 - t) * 10.0f);
            draw(at(x, y), decoRnd(i, salt + 5), ph * 6.2832f + tm * (0.15f + 0.3f * decoRnd(i, salt + 6)) * (i % 2 ? 1.0f : -1.0f), i, show);
        }
    };
    // a cloud crossing the sky, very slowly, and round again
    auto cloudAt = [&](int i, float fy, float r, float secs, ImU32 col) {
        float t = decoRnd(i, 71) + tm / secs;
        t -= std::floor(t);
        decoCloud(dl, at(t * 1.4f - 0.2f, fy), r * u, fade(col, std::min(1.0f, std::min(t, 1 - t) * 8.0f)));
    };
    // a firework, slowly: it opens out, hangs, sinks a little as it fades, and after a pause opens again (each in its own time)
    auto bloom = [&](int i, ImVec2 c, float r, ImU32 col, int rays, float secs) {
        float t = i * 0.27f + tm / secs;
        t -= std::floor(t);
        if (t > 0.94f) return;
        const float g = 1 - std::pow(1 - std::min(1.0f, t / 0.5f), 3.0f);
        const float a = t < 0.08f ? t / 0.08f : t < 0.52f ? 1.0f : std::max(0.0f, 1 - (t - 0.52f) / 0.42f);
        decoBurst(dl, ImVec2(c.x, c.y + g * g * 6 * u * (t > 0.5f ? 1 + (t - 0.5f) * 2 : 1.0f)), r * (0.12f + 0.88f * g), fade(col, a), rays);
    };
    // motes of light rising, each glowing and dimming
    auto motes = [&](int n, int salt, float secs, ImU32 col) {
        adrift(n, salt, secs, true, [&](ImVec2 p, float sz, float, int i, float show) { dl->AddCircleFilled(p, (1.2f + sz * 1.8f) * u, fade(col, show * breathe(6.0f + decoRnd(i, salt + 9) * 6.0f, 0.7f, (float)i)), 8); });
    };
    switch (k) {
    case 0:   // New Year: stars and fireworks
        if (stage == 0) { stars(60, 0.55f, IM_COL32(255, 255, 255, 140));
            bloom(0, at(0.22f, 0.22f), 46 * u, IM_COL32(255, 214, 110, 255), 16, 11.0f); bloom(1, at(0.62f, 0.14f), 38 * u, IM_COL32(200, 160, 255, 255), 14, 13.0f);
            bloom(2, at(0.84f, 0.32f), 30 * u, IM_COL32(120, 220, 255, 255), 12, 12.0f); bloom(3, at(0.42f, 0.38f), 24 * u, IM_COL32(255, 140, 180, 255), 12, 14.0f); }
        break;
    case 2:   // Valentine's: hearts floating up
        if (stage == 0) adrift(14, 9, 46.0f, true, [&](ImVec2 p, float sz, float, int i, float show) { decoHeart(dl, p, (8 + sz * 16) * u, fade(i % 3 ? IM_COL32(255, 120, 160, 110) : IM_COL32(255, 200, 215, 90), show)); });
        break;
    case 3: case 11:   // Presidents' Day, Veterans Day: an arc of stars, and a flag's stripes waving faintly across the sky
        if (stage == 0) {
            // the stripes as one smooth waving cloth: each a band between two sine curves, in fine slices that share their edges
            auto edge = [&](int line, float fx) { return 0.12f + line * 0.045f + 0.025f * std::sin(fx * 7.0f + 0.6f - tm * 0.22f) + 0.006f * std::sin(fx * 17.0f - tm * 0.35f); };   // (the wave running along it, slowly)
            const int seg = std::max(24, (int)(w / 6));
            for (int st = 0; st < 7; ++st) {
                const ImU32 col = st % 2 ? IM_COL32(255, 255, 255, 16) : IM_COL32(220, 60, 70, 34);
                for (int x = 0; x < seg; ++x) {
                    const float f0 = (float)x / seg, f1 = (float)(x + 1) / seg;
                    dl->AddQuadFilled(at(f0, edge(st, f0)), at(f1, edge(st, f1)), at(f1, edge(st + 1, f1)), at(f0, edge(st + 1, f0)), col);
                }
            }
            for (int i = 0; i < 13; ++i) { const float a = 3.4f + i * 2.6f / 12; decoStar(dl, ImVec2(o.x + w * 0.5f + std::cos(a) * w * 0.32f, o.y + h * 0.56f + std::sin(a) * h * 0.32f), 9 * u, fade(k == 3 ? IM_COL32(255, 255, 255, 96) : IM_COL32(240, 210, 120, 110), breathe(9.0f, 0.45f, (float)i))); }
        }
        break;
    case 4:   // St. Patrick's: clovers drifting, a rainbow's arc
        if (stage == 0) {
            static const ImU32 bow[6] = {IM_COL32(230, 70, 70, 50), IM_COL32(240, 150, 60, 50), IM_COL32(240, 220, 80, 50), IM_COL32(90, 200, 100, 50), IM_COL32(70, 130, 230, 50), IM_COL32(140, 90, 220, 50)};
            for (int i = 0; i < 6; ++i) { dl->PathArcTo(at(0.7f, 0.78f), (230 - i * 12) * u, 3.1416f, 6.2832f, 48); dl->PathStroke(bow[i], 0, 12 * u); }
            adrift(12, 9, 52.0f, false, [&](ImVec2 p, float sz, float, int, float show) { decoClover(dl, p, (9 + sz * 10) * u, fade(IM_COL32(110, 220, 120, 100), show)); });
        }
        break;
    case 5:   // Easter: flowers in the sky's light, eggs on the hills
        if (stage == 0) adrift(10, 9, 60.0f, false, [&](ImVec2 p, float sz, float, int, float show) { decoFlower(dl, p, (8 + sz * 8) * u, fade(IM_COL32(255, 255, 255, 56), show), fade(IM_COL32(255, 220, 120, 90), show)); });
        else { static const ImU32 eggs[5] = {IM_COL32(250, 180, 200, 255), IM_COL32(180, 220, 250, 255), IM_COL32(255, 236, 150, 255), IM_COL32(200, 180, 250, 255), IM_COL32(180, 240, 200, 255)};
            for (int i = 0; i < 6; ++i) decoEgg(dl, at(i < 3 ? 0.03f + i * 0.045f : 0.88f + (i - 3) * 0.045f, 0.9f + decoRnd(i, 4) * 0.05f), (12 + decoRnd(i, 5) * 5) * u, eggs[i % 5], IM_COL32(255, 255, 255, 170)); }
        break;
    case 6:   // Memorial Day: stars over the dusk, poppies on the hills
        if (stage == 0) stars(40, 0.5f, IM_COL32(255, 255, 255, 120));
        else for (int i = 0; i < 14; ++i) { const ImVec2 c = at(0.03f + decoRnd(i, 6) * 0.94f + std::sin(tm * 0.5f + i * 1.3f) * 0.003f, 0.88f + decoRnd(i, 7) * 0.09f); const float r = (8 + decoRnd(i, 8) * 6) * u;   // (the poppies in a light wind)
            decoFlower(dl, c, r, IM_COL32(210, 40, 40, 255), IM_COL32(20, 14, 14, 255)); }
        break;
    case 8:   // Independence Day: fireworks in red, white and blue, and 1776 in a quill's script, its foot tucked behind the mountains
        if (stage == 0) { stars(40, 0.5f, IM_COL32(255, 255, 255, 110));
            {   // 1776, as a broad quill writes it: the nib held at one angle, so each stroke is thick or thin by the way it runs
                const ImU32 ink = IM_COL32(244, 246, 252, 255);
                const ImVec2 c = at(0.504f, 0.577f);                         // (centered, in the middle valley: the ridge covers its ends' feet)
                const float q = 1.70f * u;                                   // (a unit of the script)
                const ImVec2 nib(std::cos(0.663f) * 2.3f, -std::sin(0.663f) * 2.3f);
                auto P = [&](float x, float y) { return ImVec2(c.x + (x - 0.24f * y) * q, c.y + y * q); };   // (slanted)
                auto quill = [&](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float k) {
                    // the stroke as a band of pieces sharing their edges (filled plain, so no seam shows between them), then its
                    // two sides and ends drawn over as smooth lines
                    const int n = 22;
                    ImVec2 sa[n + 1], sb[n + 1];
                    for (int i = 0; i <= n; ++i) {
                        const float t = (float)i / n, v = 1 - t;
                        const float x = v * v * v * p0.x + 3 * v * v * t * p1.x + 3 * v * t * t * p2.x + t * t * t * p3.x, y = v * v * v * p0.y + 3 * v * v * t * p1.y + 3 * v * t * t * p2.y + t * t * t * p3.y;
                        sa[i] = P(x - nib.x * k, y - nib.y * k); sb[i] = P(x + nib.x * k, y + nib.y * k);
                    }
                    const ImDrawListFlags was = dl->Flags;
                    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
                    for (int i = 1; i <= n; ++i) dl->AddQuadFilled(sa[i - 1], sb[i - 1], sb[i], sa[i], ink);
                    dl->Flags = was;
                    dl->AddPolyline(sa, n + 1, ink, 0, 1.0f); dl->AddPolyline(sb, n + 1, ink, 0, 1.0f);
                    dl->AddLine(sa[0], sb[0], ink, 1.0f); dl->AddLine(sa[n], sb[n], ink, 1.0f);
                    dl->AddBezierCubic(P(p0.x, p0.y), P(p1.x, p1.y), P(p2.x, p2.y), P(p3.x, p3.y), ink, std::max(1.0f, 0.9f * q), n);   // (never thinner than a hairline)
                };
                quill({-42, -9}, {-39, -11}, {-36, -14}, {-34, -17}, 0.5f);      // 1: the pen coming in, the stem, its foot
                quill({-34, -17}, {-34, -6}, {-35, 6}, {-36, 16}, 1.0f);
                quill({-41, 16}, {-38, 15}, {-34, 15}, {-30, 16}, 0.6f);
                for (int k7 = 0; k7 < 2; ++k7) {                                  // 7, 7: a waved bar, a long stem swept down
                    const float x = k7 * 21.0f;
                    quill({x - 25, -13}, {x - 21, -18}, {x - 16, -11}, {x - 8, -16}, 1.0f);
                    quill({x - 8, -16}, {x - 13, -6}, {x - 17, 6}, {x - 19, 19}, 1.0f);
                }
                quill({36, -16}, {26, -14}, {19, -2}, {20, 8}, 1.0f);            // 6: a tall hook into its bowl
                quill({20, 8}, {21, 18}, {33, 18}, {34, 9}, 1.0f);
                quill({34, 9}, {35, 1}, {24, 0}, {20, 8}, 1.0f);
                quill({-46, 24}, {-20, 30}, {10, 20}, {44, 25}, 0.45f);          // and a flourish under it
            }
            bloom(0, at(0.2f, 0.24f), 50 * u, IM_COL32(240, 70, 80, 255), 18, 12.0f); bloom(1, at(0.5f, 0.14f), 42 * u, IM_COL32(255, 255, 255, 255), 16, 14.0f);
            bloom(2, at(0.78f, 0.26f), 48 * u, IM_COL32(90, 140, 255, 255), 18, 13.0f); bloom(3, at(0.36f, 0.42f), 26 * u, IM_COL32(255, 255, 255, 230), 12, 11.0f);
            bloom(4, at(0.66f, 0.44f), 28 * u, IM_COL32(240, 70, 80, 230), 12, 15.0f); }
        break;
    case 9:   // Labor Day: a city's skyline at work, cranes over it
        if (stage == 1) {
            for (int i = 0; i < 18; ++i) { const float x0 = 0.02f + i * 0.054f, hh = 0.1f + decoRnd(i, 3) * 0.16f; dl->AddRectFilled(at(x0, 1.0f - hh), at(x0 + 0.046f, 1.0f), IM_COL32(14, 18, 26, 255));
                for (int wdw = 0; wdw < 4; ++wdw) if (decoRnd(i, wdw + 20) > 0.45f) dl->AddRectFilled(at(x0 + 0.01f + (wdw % 2) * 0.018f, 1.0f - hh + 0.02f + (wdw / 2) * 0.04f), at(x0 + 0.018f + (wdw % 2) * 0.018f, 1.0f - hh + 0.035f + (wdw / 2) * 0.04f), fade(IM_COL32(255, 200, 110, 170), breathe(24.0f + decoRnd(i, wdw + 30) * 30.0f, 0.75f, i * 4.0f + wdw)));   // (its lights going down and up, each in its own time)
            }
            for (int c = 0; c < 2; ++c) { const float x = c ? 0.74f : 0.28f; dl->AddLine(at(x, 1.0f), at(x, 0.52f), IM_COL32(255, 150, 50, 220), 4 * u);
                dl->AddLine(at(x - 0.06f, 0.54f), at(x + 0.16f, 0.54f), IM_COL32(255, 150, 50, 220), 4 * u);
                const float sway = std::sin(tm * 0.45f + c * 1.7f) * 0.004f;   // (its hook swinging a little, its light glowing)
                dl->AddLine(at(x + 0.13f, 0.54f), at(x + 0.13f + sway, 0.66f), IM_COL32(255, 150, 50, 160), 1.5f * u); dl->AddRectFilled(ImVec2(at(x + 0.13f + sway, 0).x - 3 * u, at(0, 0.66f).y), ImVec2(at(x + 0.13f + sway, 0).x + 3 * u, at(0, 0.66f).y + 5 * u), IM_COL32(255, 150, 50, 200));
                dl->AddCircleFilled(at(x, 0.515f), 3 * u, fade(IM_COL32(255, 70, 60, 255), breathe(4.0f, 0.8f, (float)c)), 10); }
        }
        break;
    case 10:   // Halloween: a big moon, bats, pumpkins on the hills
        if (stage == 0) { dl->AddCircleFilled(at(0.78f, 0.58f), 62 * u, IM_COL32(255, 236, 190, 230), 48); dl->AddCircleFilled(at(0.78f, 0.58f), 80 * u * (1.08f - 0.08f * breathe(11.0f, 1.0f, 0)), IM_COL32(255, 220, 160, 30), 48);
            for (int i = 0; i < 6; ++i) {   // the bats: each crossing the sky in a minute or two, one way or the other, beating its wings unhurriedly
                float t = decoRnd(i, 9) + tm / (70.0f + decoRnd(i, 12) * 60.0f);
                t -= std::floor(t);
                const float x = (i % 2 ? 1 - t : t) * 1.2f - 0.1f, y = 0.08f + decoRnd(i, 10) * 0.4f + std::sin(tm * 0.4f + i * 2.1f) * 0.012f;
                decoBat(dl, at(x, y), (14 + decoRnd(i, 11) * 10) * u, fade(IM_COL32(12, 6, 16, 240), std::min(1.0f, std::min(t, 1 - t) * 10.0f)), std::sin(tm * 2.4f + i * 1.9f) * 0.16f);
            } }
        break;
    case 12:   // Thanksgiving: a harvest moon behind the hills; before them a horn of plenty, pumpkins and a sheaf of wheat; a few leaves coming down
        if (stage == 0) {
            const ImVec2 m = at(0.83f, 0.61f);   // (low, its foot behind the hills)
            dl->AddCircleFilled(m, 92 * u, IM_COL32(255, 196, 120, 22), 48); dl->AddCircleFilled(m, 70 * u, IM_COL32(255, 204, 130, 34), 48);
            dl->AddCircleFilled(m, 52 * u, IM_COL32(255, 222, 164, 235), 48);
            dl->AddCircleFilled(ImVec2(m.x - 14 * u, m.y - 10 * u), 9 * u, IM_COL32(236, 190, 130, 120), 20); dl->AddCircleFilled(ImVec2(m.x + 16 * u, m.y + 12 * u), 6 * u, IM_COL32(236, 190, 130, 110), 16);
            static const ImU32 lv[4] = {IM_COL32(230, 120, 40, 210), IM_COL32(200, 70, 30, 210), IM_COL32(240, 180, 60, 210), IM_COL32(170, 90, 40, 210)};
            adrift(9, 9, 34.0f, false, [&](ImVec2 p, float sz, float rot, int i, float show) { decoLeaf(dl, p, (8 + sz * 7) * u, rot, fade(lv[i % 4], show)); });
        } else {
            // at the left: the sheaf of wheat and the pumpkins beside it
            for (int i = 0; i < 9; ++i) decoWheat(dl, at(0.085f + i * 0.004f, 0.985f), (92 + decoRnd(i, 5) * 26) * u, (i - 4) * 0.075f, IM_COL32(232, 188, 96, 255));
            dl->AddRectFilled(ImVec2(at(0.085f, 0).x - 4 * u, at(0, 0.90f).y), ImVec2(at(0.085f, 0).x + 20 * u, at(0, 0.918f).y), IM_COL32(170, 60, 40, 255), 2 * u);   // its band
            decoGourd(dl, at(0.185f, 0.925f), 34 * u, IM_COL32(232, 120, 30, 255), IM_COL32(196, 90, 20, 255));
            decoGourd(dl, at(0.245f, 0.945f), 22 * u, IM_COL32(240, 226, 196, 255), IM_COL32(214, 196, 160, 255));   // (a white one)
            decoGourd(dl, at(0.035f, 0.955f), 20 * u, IM_COL32(214, 96, 30, 255), IM_COL32(176, 72, 20, 255));
            // at the right: the horn of plenty
            decoCornucopia(dl, at(0.85f, 0.885f), 74 * u);
        }
        break;
    case 13:   // Christmas: snow falling, firs on the hills, a bright star
        if (stage == 0) { decoStar(dl, at(0.5f, 0.12f), 18 * u, IM_COL32(255, 230, 150, 240)); dl->AddCircleFilled(at(0.5f, 0.12f), 30 * u * (1.15f - 0.15f * breathe(6.0f, 1.0f, 0)), IM_COL32(255, 230, 150, 30), 32); }   // (its glow coming and going)
        else { for (int i = 0; i < 9; ++i) decoTree(dl, at(0.04f + i * 0.115f + decoRnd(i, 3) * 0.03f, 1.0f), (60 + decoRnd(i, 4) * 40) * u, IM_COL32(12, 52, 30, 255));
            adrift(80, 30, 40.0f, false, [&](ImVec2 p, float sz, float, int, float show) { dl->AddCircleFilled(p, (1 + sz * 2.2f) * u, fade(IM_COL32(255, 255, 255, 190), show), 8); }); }
        break;
    case 14: case 15:   // Men's Mental Health (a blue ribbon), Breast Cancer Awareness (a pink one): in a soft light that comes and goes, motes rising round it
        if (stage == 0) {
            const ImU32 glow = k == 14 ? IM_COL32(120, 180, 255, 255) : IM_COL32(255, 160, 200, 255);
            const float br = 1.07f - 0.07f * breathe(10.0f, 1.0f, 0);
            dl->AddCircleFilled(at(0.72f, 0.58f), 110 * u * br, fade(glow, 26 / 255.0f), 48); dl->AddCircleFilled(at(0.72f, 0.58f), 70 * u * br, fade(glow, 30 / 255.0f), 48);
            decoRibbon(dl, at(0.72f, 0.58f), 70 * u, k == 14 ? IM_COL32(80, 160, 255, 255) : IM_COL32(255, 130, 180, 255));
            stars(30, 0.6f, IM_COL32(255, 255, 255, 90));
            motes(10, 40, 90.0f, fade(glow, 90 / 255.0f));
        }
        break;
    case 16:   // Spring: a soft sun and slow clouds; blossom trees and flowers on the hills; petals adrift
        if (stage == 0) {
            const ImVec2 sn = at(0.76f, 0.70f);   // (rising in the valley between two hills)
            dl->AddCircleFilled(sn, 84 * u, IM_COL32(255, 244, 200, 30), 48); dl->AddCircleFilled(sn, 40 * u, IM_COL32(255, 244, 206, 235), 40);
            cloudAt(0, 0.53f, 20, 260, IM_COL32(255, 255, 255, 120)); cloudAt(1, 0.57f, 14, 340, IM_COL32(255, 255, 255, 100));
        } else {
            for (int i = 0; i < 4; ++i) decoRoundTree(dl, at(0.06f + i * 0.30f + decoRnd(i, 6) * 0.04f, 0.90f + decoRnd(i, 7) * 0.06f), (30 + decoRnd(i, 8) * 12) * u, i % 2 ? IM_COL32(255, 176, 204, 255) : IM_COL32(250, 214, 226, 255));
            for (int i = 0; i < 16; ++i) decoFlower(dl, at(0.03f + decoRnd(i, 21) * 0.94f, 0.90f + decoRnd(i, 22) * 0.09f), (5 + decoRnd(i, 23) * 4) * u, i % 3 == 0 ? IM_COL32(255, 230, 120, 255) : i % 3 == 1 ? IM_COL32(255, 160, 196, 255) : IM_COL32(236, 236, 255, 255), IM_COL32(250, 190, 60, 255));
            adrift(22, 40, 38.0f, false, [&](ImVec2 p, float sz, float rot, int i, float show) { dl->AddEllipseFilled(p, ImVec2((3 + sz * 2.5f) * u, (1.8f + sz * 1.4f) * u), fade(i % 2 ? IM_COL32(255, 196, 216, 220) : IM_COL32(255, 232, 240, 220), show), rot, 10); });
        }
        break;
    case 17:   // Summer: the sun, its rays turning slowly; clouds; two hot-air balloons adrift
        if (stage == 0) {
            const ImVec2 sn = at(0.76f, 0.63f);   // (in the valley between two hills, clear of the icons' rows)
            for (int i = 0; i < 12; ++i) { const float a = tm * 0.05f + i * 0.5236f; dl->AddLine(ImVec2(sn.x + std::cos(a) * 40 * u, sn.y + std::sin(a) * 40 * u), ImVec2(sn.x + std::cos(a) * (56 + 8 * (i % 2)) * u, sn.y + std::sin(a) * (56 + 8 * (i % 2)) * u), IM_COL32(255, 226, 120, 150), 3.5f * u); }
            dl->AddCircleFilled(sn, 48 * u, IM_COL32(255, 236, 150, 36), 48); dl->AddCircleFilled(sn, 32 * u, IM_COL32(255, 220, 96, 245), 44);
            cloudAt(0, 0.52f, 20, 280, IM_COL32(255, 255, 255, 140)); cloudAt(1, 0.58f, 14, 360, IM_COL32(255, 255, 255, 110));
            for (int i = 0; i < 2; ++i) {   // the balloons: across the sky in some minutes, bobbing a little
                float t = decoRnd(i, 81) + tm / (300.0f + i * 80);
                t -= std::floor(t);
                const float show = std::min(1.0f, std::min(t, 1 - t) * 8.0f);
                decoBalloon(dl, at(t * 1.3f - 0.15f, 0.50f + i * 0.05f + std::sin(tm * 0.2f + i * 2.0f) * 0.010f), (19 - i * 5) * u, fade(i ? IM_COL32(240, 96, 80, 255) : IM_COL32(255, 196, 60, 255), show), fade(i ? IM_COL32(255, 236, 200, 255) : IM_COL32(60, 150, 220, 255), show));
            }
        } else for (int i = 0; i < 5; ++i) decoRoundTree(dl, at(0.05f + i * 0.23f + decoRnd(i, 6) * 0.05f, 0.92f + decoRnd(i, 7) * 0.05f), (26 + decoRnd(i, 8) * 12) * u, i % 2 ? IM_COL32(30, 110, 70, 255) : IM_COL32(44, 132, 84, 255));
        break;
    case 18:   // Fall: a low sun's glow; trees turned on the hills; leaves coming down
        if (stage == 0) { dl->AddCircleFilled(at(0.5f, 0.66f), 190 * u, IM_COL32(255, 170, 70, 26), 48); dl->AddCircleFilled(at(0.5f, 0.66f), 120 * u, IM_COL32(255, 180, 80, 34), 48); }
        else {
            static const ImU32 tr[4] = {IM_COL32(214, 96, 30, 255), IM_COL32(196, 60, 34, 255), IM_COL32(232, 164, 50, 255), IM_COL32(170, 84, 36, 255)};
            for (int i = 0; i < 6; ++i) decoRoundTree(dl, at(0.04f + i * 0.19f + decoRnd(i, 6) * 0.04f, 0.91f + decoRnd(i, 7) * 0.06f), (26 + decoRnd(i, 8) * 14) * u, tr[i % 4]);
            static const ImU32 lv[4] = {IM_COL32(236, 124, 40, 225), IM_COL32(206, 72, 30, 225), IM_COL32(244, 186, 60, 225), IM_COL32(176, 92, 40, 225)};
            adrift(24, 9, 30.0f, false, [&](ImVec2 p, float sz, float rot, int i, float show) { decoLeaf(dl, p, (8 + sz * 9) * u, rot, fade(lv[i % 4], show)); });
        }
        break;
    case 19:   // Winter: a pale moon and stars; firs under snow; snow coming down, some of it flakes turning
        if (stage == 0) { stars(40, 0.5f, IM_COL32(255, 255, 255, 120));
            dl->AddCircleFilled(at(0.29f, 0.555f), 46 * u, IM_COL32(220, 232, 255, 28), 40); dl->AddCircleFilled(at(0.29f, 0.555f), 24 * u, IM_COL32(232, 240, 255, 235), 36); }   // (the moon, by the highest hill)
        else {
            for (int i = 0; i < 8; ++i) {
                const ImVec2 b = at(0.05f + i * 0.13f + decoRnd(i, 3) * 0.03f, 1.0f);
                const float r = (56 + decoRnd(i, 4) * 40) * u;
                decoTree(dl, b, r, IM_COL32(22, 58, 66, 255));
                dl->AddTriangleFilled(ImVec2(b.x, b.y - r * 1.80f), ImVec2(b.x - r * 0.125f, b.y - r * 1.52f), ImVec2(b.x + r * 0.125f, b.y - r * 1.52f), IM_COL32(236, 244, 255, 255));   // snow on its top
                for (int tI = 0; tI < 2; ++tI) { const float y0 = b.y - r * 0.2f - tI * r * 0.45f, ww = r * (0.55f - tI * 0.12f);   // and along its lower boughs
                    dl->AddLine(ImVec2(b.x - ww * 0.92f, y0 - r * 0.02f), ImVec2(b.x - ww * 0.45f, y0 - r * 0.35f), IM_COL32(236, 244, 255, 200), std::max(1.0f, r * 0.05f));
                    dl->AddLine(ImVec2(b.x + ww * 0.92f, y0 - r * 0.02f), ImVec2(b.x + ww * 0.45f, y0 - r * 0.35f), IM_COL32(236, 244, 255, 200), std::max(1.0f, r * 0.05f)); }
            }
            adrift(70, 30, 44.0f, false, [&](ImVec2 p, float sz, float, int, float show) { dl->AddCircleFilled(p, (1 + sz * 2.0f) * u, fade(IM_COL32(255, 255, 255, 200), show), 8); });
            adrift(9, 50, 60.0f, false, [&](ImVec2 p, float sz, float rot, int, float show) { decoFlake(dl, p, (6 + sz * 6) * u, rot, fade(IM_COL32(255, 255, 255, 170), show)); });
        }
        break;
    default: break;
    }
}
// one background drawn into a box (the screen; or a little picture of it in Settings, the same drawing made small)
ImU32 mixCol(ImU32 a, ImU32 b, float t);
void wallpaperIn(ImDrawList* dl, ImVec2 o, float w, float h, int which, float round = 0) {
    const Backdrop bd = themeBackdrop(which);
    {   // a Custom theme with its picture loaded: the picture, filling the box (cropped to its shape)
        const int wi = std::max(0, std::min(kThemes - 1, which));
        if (wi >= 7 && wi <= 9 && g_bgTex && g_bgW > 0 && g_bgH > 0 && !g_settings.customs[wi - 7].picture.empty() && g_settings.customs[wi - 7].picture == g_bgPic) {
            const float ia = (float)g_bgW / g_bgH, ba = w / std::max(1.0f, h);
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (ia > ba) { const float k = ba / ia; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; } else { const float k = ia / ba; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
            if (round > 0) dl->AddImageRounded(g_bgTex, o, ImVec2(o.x + w, o.y + h), uv0, uv1, IM_COL32(255, 255, 255, 255), round);
            else dl->AddImage(g_bgTex, o, ImVec2(o.x + w, o.y + h), uv0, uv1);
            return;
        }
    }
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
    const int seasonK = std::max(0, std::min(kThemes - 1, which)) - kSeasonal0;
    if (seasonK >= 0) seasonalDecor(dl, o, w, h, seasonK, 0);   // (a seasonal theme's sky: its picture; the everyday themes stay still)
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
    if (seasonK >= 0) seasonalDecor(dl, o, w, h, seasonK, 1);   // (and on its hills)
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
    const Backdrop bd = themeBackdrop(g_settings.wallpaper);
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
    case APP_NAVSEARCH:   // a compass rose with a magnifier
        dl->AddCircle(P(-0.14f, -0.14f), r * 0.56f, col, 0, t);
        dl->AddTriangleFilled(P(-0.14f, -0.58f), P(-0.28f, -0.14f), P(0.0f, -0.14f), col);
        dl->AddTriangleFilled(P(-0.14f, 0.30f), P(-0.28f, -0.14f), P(0.0f, -0.14f), IM_COL32(255, 255, 255, 120));
        dl->AddLine(P(0.28f, 0.28f), P(0.80f, 0.80f), col, t * 1.6f);
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
bool g_battFromDevice = false;   // Local Connect: the battery shown is the connected device's (the tablet's own kept below)
float g_ownBatt = 100.0f; bool g_ownCharger = false;
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

// the logo: as made (the five themes drawn from its colours), else XP in white and TABLET in the theme's accent, light to bright
void drawLogo(ImDrawList* dl, ImVec2 a, ImVec2 b, int alpha) {
    if (!g_logoMask || themeIndex() < 5) { dl->AddImage(g_logo, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, alpha)); return; }
    int lw, lh, lv; float from = 0.5f;
    logoMaskPixels(lw, lh, lv, from);
    const float sx = a.x + (b.x - a.x) * from;
    dl->AddImage(g_logoMask, a, ImVec2(sx, b.y), ImVec2(0, 0), ImVec2(from, 1), IM_COL32(255, 255, 255, alpha));
    // Halloween: TABLET's A is a jack-o'-lantern (its letter left out of the slices, the pumpkin in its place)
    float au0 = 2, au1 = 2, av0 = 0, av1 = 1;
    const bool pumpkinA = themeIndex() == kSeasonal0 + 10 && logoLetterBox(1, au0, au1, av0, av1);
    const int n = 6;   // TABLET in slices, its colour running from the accent's light tone to the accent
    for (int i = 0; i < n; ++i) {
        const float u0 = from + (1 - from) * i / n, u1 = from + (1 - from) * (i + 1) / n;
        const ImU32 c = (mixCol(accentLite(255), accentCol(255), (i + 0.5f) / n) & 0x00FFFFFF) | ((ImU32)alpha << 24);
        auto slice = [&](float s0, float s1) { if (s1 > s0) dl->AddImage(g_logoMask, ImVec2(a.x + (b.x - a.x) * s0, a.y), ImVec2(a.x + (b.x - a.x) * s1, b.y), ImVec2(s0, 0), ImVec2(s1, 1), c); };
        if (pumpkinA && u1 > au0 && u0 < au1) { slice(u0, std::min(u1, au0)); slice(std::max(u0, au1), u1); }
        else slice(u0, u1);
    }
    if (pumpkinA && alpha > 8) {
        const float lh = (av1 - av0) * (b.y - a.y), cx = a.x + (b.x - a.x) * (au0 + au1) * 0.5f, by = a.y + (b.y - a.y) * av1;
        const float r = lh / 1.85f;
        decoPumpkin(dl, ImVec2(cx, by - r * 0.8f), r);
    }
}
void statusBar(ImDrawList* dl, const Fonts& f, const SimState& s, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), IM_COL32(0, 0, 0, 90));
    {   // a touch of the theme: its sky's darkest tone and a faint glow of its accent (the logo and layout are unchanged)
        const Backdrop bd = themeBackdrop(g_settings.wallpaper);
        if (themeIndex() < 5) dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), (bd.top & 0x00FFFFFF) | (70u << 24));
        else {   // Noir, Cloud, the Customs and the seasonal themes: the bar in the theme's own colours (deep enough for white text)
            const ImU32 c0 = mixCol(mixCol(bd.top, bd.bot, 0.35f), IM_COL32(0, 0, 0, 255), 0.45f);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), (c0 & 0x00FFFFFF) | (215u << 24));
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, kStatusH), accentCol(0), accentCol(34), accentCol(34), accentCol(0));
        }
        // a clear edge under it: a dark line, then a hairline of the accent's light tone (no gradient)
        dl->AddRectFilled(ImVec2(0, kStatusH - 2), ImVec2(w, kStatusH - 1), IM_COL32(0, 0, 0, 110));
        dl->AddRectFilled(ImVec2(0, kStatusH - 1), ImVec2(w, kStatusH), accentLite(themeIndex() < 5 ? 70 : 140));
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
        if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(3, 3), ImVec2(27, kStatusH - 3), IM_COL32(255, 255, 255, (int)(26 * hk)), 6.0f);   // as the clock's
        icon(dl, APP_COUNT + 100, pc, 7.6f, ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : col);   // (no ring around it)   // (the power pictogram)
        const float cx = 37, tw = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, buf).x;
        ImGui::SetCursorScreenPos(ImVec2(30, 0));
        if (ImGui::InvisibleButton("##clock", ImVec2(cx - 30 + tw + 6, kStatusH))) { g_settings.statusLocalTime = !g_settings.statusLocalTime; g_settingsChanged = true; }
        if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(31, 3), ImVec2(cx + tw + 6, kStatusH - 3), IM_COL32(255, 255, 255, (int)(26 * hk)), 6.0f);
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
        if (g_power != POWER_REVEAL) drawLogo(dl, a, ImVec2(a.x + sz.x, a.y + sz.y), 255);   // (while waking, it is on its way up)
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
        if (ImGui::InvisibleButton("##battery", ImVec2(bx1 - bx0, kStatusH)) && !g_battFromDevice) { g_charger = !g_charger; g_settingsChanged = true; }   // (a device's battery: its own charger decides)
        if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(bx0, 3), ImVec2(bx1, kStatusH - 3), IM_COL32(255, 255, 255, (int)(26 * hk)), 6.0f);
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
    } else if (storeApp(app) || app == APP_APPSTORE || app == APP_GAMES) {   // (until their logos come: a mark of their own)
        const float r = tile * 0.32f * scale;
        const ImU32 wcol = IM_COL32(255, 255, 255, 255);
        if (app == APP_APPSTORE) {   // a storefront (Material's "store" pictogram, 24 units: its sign, the awning, the shop with its window and door)
            const float k = r * 2.0f / 18.0f;
            auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12) * k, c.y + (y - 12) * k); };
            const ImU32 cut = shade(appColor(app), shadeK);
            dl->AddRectFilled(P(4, 4), P(20, 6), wcol);   // the sign
            const ImVec2 aw[6] = {P(4, 7), P(20, 7), P(21, 12), P(21, 14), P(3, 14), P(3, 12)};   // the awning
            dl->AddConvexPolyFilled(aw, 6, wcol);
            dl->AddRectFilled(P(4, 14), P(14, 20), wcol);   // the shop front, its window cut out
            dl->AddRectFilled(P(6, 14), P(12, 18), cut);
            dl->AddRectFilled(P(18, 14), P(20, 20), wcol);   // the door's side
        } else if (app == APP_TIMERS) {   // a stopwatch: its face, its crown, a hand
            dl->AddCircle(ImVec2(c.x, c.y + r * 0.12f), r * 0.78f, wcol, 0, std::max(2.0f, r * 0.16f));
            dl->AddRectFilled(ImVec2(c.x - r * 0.18f, c.y - r * 0.92f), ImVec2(c.x + r * 0.18f, c.y - r * 0.72f), wcol, r * 0.05f);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.12f), ImVec2(c.x + r * 0.34f, c.y - r * 0.26f), wcol, std::max(2.0f, r * 0.14f));
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.12f), r * 0.1f, wcol, 0);
        } else if (app == APP_LOGBOOK) {   // a log book: its cover, its spine, ruled lines
            const ImU32 k = shade(appColor(app), shadeK);
            dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.86f), ImVec2(c.x + r * 0.7f, c.y + r * 0.86f), wcol, r * 0.12f);
            dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.86f), ImVec2(c.x - r * 0.46f, c.y + r * 0.86f), IM_COL32(220, 210, 196, 255), r * 0.12f, ImDrawFlags_RoundCornersLeft);
            for (int i = 0; i < 4; ++i) dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r * 0.45f + i * r * 0.32f), ImVec2(c.x + r * 0.52f, c.y - r * 0.45f + i * r * 0.32f), k, std::max(1.5f, r * 0.09f));
        } else if (app == APP_ACARS) {   // a message on its way up: a speech bubble with text lines, a small signal arc
            const ImU32 k = shade(appColor(app), shadeK);
            dl->AddRectFilled(ImVec2(c.x - r * 0.86f, c.y - r * 0.62f), ImVec2(c.x + r * 0.86f, c.y + r * 0.42f), wcol, r * 0.22f);
            const ImVec2 tl[3] = {ImVec2(c.x - r * 0.42f, c.y + r * 0.38f), ImVec2(c.x - r * 0.1f, c.y + r * 0.38f), ImVec2(c.x - r * 0.5f, c.y + r * 0.8f)};
            dl->AddConvexPolyFilled(tl, 3, wcol);
            for (int i = 0; i < 3; ++i) dl->AddLine(ImVec2(c.x - r * 0.58f, c.y - r * 0.34f + i * r * 0.26f), ImVec2(c.x + (i == 2 ? r * 0.12f : r * 0.58f), c.y - r * 0.34f + i * r * 0.26f), k, std::max(1.5f, r * 0.1f));
        } else if (app == APP_XPILOT) {   // a headset: its band, two ear cups and the microphone
            const float t = std::max(2.0f, r * 0.16f);
            dl->PathArcTo(ImVec2(c.x, c.y + r * 0.06f), r * 0.66f, 3.14159f * 1.05f, 3.14159f * 1.95f, 24); dl->PathStroke(wcol, 0, t);
            dl->AddRectFilled(ImVec2(c.x - r * 0.82f, c.y - r * 0.02f), ImVec2(c.x - r * 0.46f, c.y + r * 0.58f), wcol, r * 0.14f);
            dl->AddRectFilled(ImVec2(c.x + r * 0.46f, c.y - r * 0.02f), ImVec2(c.x + r * 0.82f, c.y + r * 0.58f), wcol, r * 0.14f);
            dl->PathArcTo(ImVec2(c.x - r * 0.2f, c.y + r * 0.46f), r * 0.44f, 3.14159f * 0.15f, 3.14159f * 0.9f, 16); dl->PathStroke(wcol, 0, std::max(1.5f, r * 0.11f));
            dl->AddCircleFilled(ImVec2(c.x + r * 0.18f, c.y + r * 0.86f), r * 0.13f, wcol, 0);
        } else if (app == APP_GAMES) {   // a game pad
            dl->AddRectFilled(ImVec2(c.x - r * 0.86f, c.y - r * 0.40f), ImVec2(c.x + r * 0.86f, c.y + r * 0.46f), wcol, r * 0.40f);
            const ImU32 k = shade(appColor(app), shadeK);
            dl->AddRectFilled(ImVec2(c.x - r * 0.58f, c.y - r * 0.06f), ImVec2(c.x - r * 0.22f, c.y + r * 0.10f), k);
            dl->AddRectFilled(ImVec2(c.x - r * 0.48f, c.y - r * 0.16f), ImVec2(c.x - r * 0.32f, c.y + r * 0.20f), k);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.34f, c.y - r * 0.04f), r * 0.10f, k, 0);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.56f, c.y + r * 0.14f), r * 0.10f, k, 0);
        } else {   // its name's first letters
            std::string mono;
            const char* nm = appName(app);
            mono += nm[0];
            for (const char* q = nm; *q; ++q) if (*q == ' ' && q[1] && mono.size() < 2) mono += q[1];
            if (app == APP_DATIS) mono = "ATIS";
            ImFont* fn = mono.size() > 2 ? f.bold : f.title;
            textCentered(dl, fn, c.x, std::floor(c.y - fontPx(fn) * 0.5f * scale), wcol, mono.c_str());
        }
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
void textAt(ImDrawList* dl, ImFont* font, float x, float y, ImU32 col, const char* txt);
float pageKeyboard(ImDrawList* dl, const Fonts& f, float w, float h, Actions& act);
extern bool g_kbd;
extern int g_kbdTarget;
extern std::string* g_typeInto;
void glass(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float alpha);
bool textField(ImDrawList* dl, const Fonts& f, const char* id, std::string& v, ImVec2 a, ImVec2 b, const char* hint, bool multiline, float ruled);
// VATSIM's controllers for an airport: their call signs start with its ICAO code (or, in the US, the code without its K)
std::vector<VatsimController> atcFor(const SimState& s, const std::string& icao) {
    std::vector<VatsimController> out;
    if (icao.size() < 3) return out;
    const std::string p1 = icao + "_", p2 = icao.size() == 4 && icao[0] == 'K' ? icao.substr(1) + "_" : std::string("\x01");
    for (const VatsimController& c : s.vatsimAtc)
        if (c.facility >= 2 && c.facility != 6 && (c.callsign.compare(0, p1.size(), p1) == 0 || c.callsign.compare(0, p2.size(), p2) == 0)) out.push_back(c);
    auto rank = [](const VatsimController& c) { return c.facility == 7 ? 0 : c.facility; };   // the ATIS first, then DEL, GND, TWR, APP
    std::sort(out.begin(), out.end(), [&](const VatsimController& a, const VatsimController& b) { return rank(a) != rank(b) ? rank(a) < rank(b) : a.callsign < b.callsign; });
    return out;
}

// VATSIM's airspaces with a center (or FSS) online: each area's id with its controllers
std::map<std::string, std::vector<VatsimController>> airspacesOnline(const SimState& s) {
    std::map<std::string, std::vector<VatsimController>> on;
    if (!s.map.airspaces) return on;
    for (const VatsimController& c : s.vatsimAtc) {
        if (c.facility != 6 && c.facility != 1) continue;
        if (const AirspaceSet::Fir* fir = airspace::firFor(*s.map.airspaces, c.callsign)) on[fir->boundary].push_back(c);
    }
    return on;
}
bool insideRings(const AirspaceArea& a, double lat, double lon) {   // even-odd over every ring
    if (lat < a.la0 || lat > a.la1 || lon < a.lo0 || lon > a.lo1) return false;
    bool in = false;
    for (const auto& r : a.rings)
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
            const double yi = r[i].first, xi = r[i].second, yj = r[j].first, xj = r[j].second;
            if (((yi > lat) != (yj > lat)) && lon < (xj - xi) * (lat - yi) / (yj - yi + 1e-12) + xi) in = !in;
        }
    return in;
}
double g_airspaceTapLat = 0, g_airspaceTapLon = 0;   // where the airspace card's area was tapped

// The home screen's places: pages of cols x rows; a widget takes a block of them from its corner (its place in the list).
int g_gridCols = 7, g_gridRows = 3;
bool isApp(int e) { return e >= 0 && e < APP_COUNT; }
bool isFolder(int e) { return e >= kFolder0 && e < kFolder0 + (int)g_settings.folders.size(); }
bool isWidget(int e) { return e >= kWidget0 && e < kWidget0 + W_COUNT; }
void widgetCells(int kind, int& wc, int& wr) {
    static const int sz[W_COUNT][2] = {{2, 2}, {4, 2}, {2, 2}, {1, 1}, {7, 1}};   // (Volanta's overlay: a strip across the page)
    wc = std::min(sz[kind][0], g_gridCols); wr = std::min(sz[kind][1], g_gridRows);
}
const char* widgetName(int kind) {
    static const char* n[W_COUNT] = {"Weather", "Flight", "Now Playing", "Frame Rate", "Volanta Overlay"};
    return n[kind];
}
// the places a widget covers (cap per page): marks cover[] with the widget's own place in the list
bool widgetFits(int anchor, int kind, int cap, const std::vector<int>& cover, int self) {
    int wc, wr; widgetCells(kind, wc, wr);
    const int slot = anchor % cap, col = slot % g_gridCols, row = slot / g_gridCols;
    if (col + wc > g_gridCols || row + wr > g_gridRows) return false;
    for (int r = 0; r < wr; ++r) for (int c = 0; c < wc; ++c) {
        const int i = anchor - slot + (row + r) * g_gridCols + col + c;
        if (i < (int)cover.size() && cover[i] >= 0 && cover[i] != self) return false;
    }
    return true;
}
void markWidget(int anchor, int kind, int cap, std::vector<int>& cover) {
    int wc, wr; widgetCells(kind, wc, wr);
    const int slot = anchor % cap, col = slot % g_gridCols, row = slot / g_gridCols;
    for (int r = 0; r < wr; ++r) for (int c = 0; c < wc; ++c) {
        const int i = anchor - slot + (row + r) * g_gridCols + col + c;
        if (i < (int)cover.size()) cover[i] = anchor;
    }
}
// which widget covers each place (its corner's index), -1 none
std::vector<int> coverOf(int cap) {
    std::vector<int> cover(g_settings.order.size(), -1);
    for (int i = 0; i < (int)g_settings.order.size(); ++i) if (isWidget(g_settings.order[i])) markWidget(i, g_settings.order[i] - kWidget0, cap, cover);
    return cover;
}
// the widgets the favorites bar takes (one row high): Frame Rate (one place) and Volanta's overlay (all seven)
void pushInWidgetAt(int widget, int start, int cap);   // (below)
bool dockWidget(int e) { return isWidget(e) && (e - kWidget0 == W_FPS || e - kWidget0 == W_VOLANTA); }
int favSpan(int e) { return dockWidget(e) && e - kWidget0 == W_VOLANTA ? 7 : 1; }
void normalizeLayout(int capacity) {
    auto& order = g_settings.order;
    auto& folders = g_settings.folders;
    bool seen[APP_COUNT] = {};
    std::vector<int> fav;
    bool favWidget[W_COUNT] = {};
    for (int a : g_settings.favorites) {   // apps, and the bar's two widgets (no folders)
        if (isApp(a) && !seen[a]) { seen[a] = true; fav.push_back(a); }
        else if (dockWidget(a) && !favWidget[a - kWidget0]) { favWidget[a - kWidget0] = true; fav.push_back(a); }
    }
    std::vector<int> spill;   // favorites beyond the bar's size go to the grid (by the places they take)
    auto spanOf = [&]() { int n = 0; for (int a : fav) n += favSpan(a); return n; };
    while (!fav.empty() && spanOf() > favSlots()) {
        const int b = fav.back(); fav.pop_back();
        if (isApp(b)) { spill.insert(spill.begin(), b); seen[b] = false; }
        else { favWidget[b - kWidget0] = false; spill.insert(spill.begin(), b); }
    }
    g_settings.favorites.swap(fav);
    for (auto& fo : folders) {   // a folder's apps: each once, and only apps
        std::vector<int> keep;
        for (int a : fo.apps) if (isApp(a) && !seen[a] && installed(a)) { seen[a] = true; keep.push_back(a); }
        fo.apps.swap(keep);
    }
    std::vector<char> folderSeen(folders.size(), 0);
    bool widgetSeen[W_COUNT] = {};
    for (int k = 0; k < W_COUNT; ++k) widgetSeen[k] = favWidget[k];   // (one in the bar is not on a page too)
    for (int& a : order) {
        if (isWidget(a)) { if (widgetSeen[a - kWidget0]) a = -1; else widgetSeen[a - kWidget0] = true; continue; }
        if (isFolder(a)) { if (folderSeen[a - kFolder0]) a = -1; else folderSeen[a - kFolder0] = 1; continue; }
        if (!isApp(a) || seen[a] || !installed(a)) a = -1; else seen[a] = true;
    }
    g_settings.homePages = std::max(1, std::max(g_settings.homePages, ((int)order.size() + capacity - 1) / capacity));
    order.resize((size_t)(g_settings.homePages * capacity), -1);
    // widgets: each where it fits (one that no longer does, or overlaps another, moves to a place where it does)
    std::vector<int> cover(order.size(), -1);
    std::vector<int> movedWidgets;
    for (int i = 0; i < (int)order.size(); ++i) if (isWidget(order[i])) {
        const int k = order[i] - kWidget0;
        if (widgetFits(i, k, capacity, cover, -1)) markWidget(i, k, capacity, cover);
        else { movedWidgets.push_back(order[i]); order[i] = -1; }
    }
    std::vector<int> bumped;   // icons a widget now covers
    for (int i = 0; i < (int)order.size(); ++i) if (order[i] >= 0 && !isWidget(order[i]) && cover[i] >= 0) { bumped.push_back(order[i]); order[i] = -1; }
    auto addPage = [&]() { g_settings.homePages++; order.resize((size_t)(g_settings.homePages * capacity), -1); cover.resize(order.size(), -1); };
    auto freeAt = [&](int i) { return order[i] < 0 && cover[i] < 0; };
    auto place = [&](int e) {   // after the last icon (gaps the user left stay), else into the first free place, else on a new page
        if (isWidget(e)) {
            for (;;) {
                for (int i = 0; i < (int)order.size(); ++i) {
                    if (!freeAt(i) || !widgetFits(i, e - kWidget0, capacity, cover, -1)) continue;
                    bool clear = true;   // (and no icon in its block)
                    int wc, wr; widgetCells(e - kWidget0, wc, wr);
                    const int slot = i % capacity, col = slot % g_gridCols, row = slot / g_gridCols;
                    for (int r = 0; r < wr && clear; ++r) for (int c = 0; c < wc; ++c) if (!freeAt(i - slot + (row + r) * g_gridCols + col + c)) { clear = false; break; }
                    if (!clear) continue;
                    order[i] = e; markWidget(i, e - kWidget0, capacity, cover);
                    return;
                }
                addPage();
            }
        }
        int last = -1;
        for (int i = 0; i < (int)order.size(); ++i) if (order[i] >= 0 && !isWidget(order[i])) last = i;
        for (int i = last + 1; i < (int)order.size(); ++i) if (freeAt(i)) { order[i] = e; return; }
        for (int i = 0; i < (int)order.size(); ++i) if (freeAt(i)) { order[i] = e; return; }
        addPage();
        for (int i = 0; i < (int)order.size(); ++i) if (freeAt(i)) { order[i] = e; return; }
    };
    for (int e : movedWidgets) place(e);
    for (int e : bumped) place(e);
    for (int a : spill) { place(a); if (isApp(a)) seen[a] = true; }
    for (int a = 0; a < APP_COUNT; ++a) if (!seen[a] && installed(a) && appVisible(a)) place(a);   // (an app hidden now gets a place when shown)
    for (int k = 0; k < (int)folders.size(); ++k) if (!folderSeen[k]) place(kFolder0 + k);
}
void normalizeLayout() { normalizeLayout(g_gridCols * g_gridRows); }
// a widget put at the start of a page (start: that page's first place): what was there from then on, in its order, is laid
// again after it, each in the next place free (a widget in the next block free), pages added as needed
void pushInWidgetAt(int widget, int start, int cap) {
    auto& order = g_settings.order;
    std::vector<int> rest;
    for (int i = start; i < (int)order.size(); ++i) if (order[i] >= 0) { rest.push_back(order[i]); order[i] = -1; }
    std::vector<int> cv(order.size(), -1);
    for (int i = 0; i < start && i < (int)order.size(); ++i) if (isWidget(order[i])) markWidget(i, order[i] - kWidget0, cap, cv);
    auto put = [&](int e) {
        for (;;) {
            for (int i = start; i < (int)order.size(); ++i) {
                if (order[i] >= 0 || cv[i] >= 0) continue;
                if (isWidget(e)) {
                    if (!widgetFits(i, e - kWidget0, cap, cv, -1)) continue;
                    int wc, wr; widgetCells(e - kWidget0, wc, wr);
                    const int slot = i % cap, col = slot % g_gridCols, row = slot / g_gridCols;
                    bool clear = true;
                    for (int r = 0; r < wr && clear; ++r) for (int c = 0; c < wc; ++c) { const int j = i - slot + (row + r) * g_gridCols + col + c; if (j >= (int)order.size() || order[j] >= 0) { clear = false; break; } }
                    if (!clear) continue;
                    order[i] = e; markWidget(i, e - kWidget0, cap, cv);
                } else order[i] = e;
                return;
            }
            g_settings.homePages++; order.resize((size_t)(g_settings.homePages * cap), -1); cv.resize(order.size(), -1);
        }
    };
    put(widget);
    for (int e : rest) put(e);
    g_settingsChanged = true;
}
int g_gridCapacity = 21;
void trimPages();
// Compress (arranging): every icon, folder and widget laid again from the first place on, in its order, no place left open between
// them; the pages emptied go. Apps not shown now give their places up (they get one again when shown). Each thing then slides from
// where it was.
std::map<int, int> g_slideFrom;   // an entry -> the place it was in
double g_slideT0 = -10;
constexpr float kSlideTime = 0.38f;
void compressLayout(int cap) {
    auto& order = g_settings.order;
    g_slideFrom.clear();
    std::vector<int> all;
    for (int i = 0; i < (int)order.size(); ++i) {
        const int e = order[i];
        if (e < 0) continue;
        g_slideFrom[e] = i;
        if (isApp(e) && !appVisible(e)) continue;
        all.push_back(e);
    }
    std::fill(order.begin(), order.end(), -1);
    std::vector<int> cv(order.size(), -1);
    for (int e : all) {
        bool placed = false;
        while (!placed) {
            for (int i = 0; i < (int)order.size() && !placed; ++i) {
                if (order[i] >= 0 || cv[i] >= 0) continue;
                if (isWidget(e)) {
                    if (!widgetFits(i, e - kWidget0, cap, cv, -1)) continue;
                    int wc, wr; widgetCells(e - kWidget0, wc, wr);
                    const int slot = i % cap, col = slot % g_gridCols, row = slot / g_gridCols;
                    bool clear = true;
                    for (int r = 0; r < wr && clear; ++r) for (int c = 0; c < wc; ++c) { const int j = i - slot + (row + r) * g_gridCols + col + c; if (j >= (int)order.size() || order[j] >= 0) { clear = false; break; } }
                    if (!clear) continue;
                    order[i] = e; markWidget(i, e - kWidget0, cap, cv);
                } else order[i] = e;
                placed = true;
            }
            if (!placed) { g_settings.homePages++; order.resize((size_t)(g_settings.homePages * cap), -1); cv.resize(order.size(), -1); }
        }
    }
    trimPages();
    g_slideT0 = ImGui::GetTime();
    g_settingsChanged = true;
}
// the pages after the last one that holds anything go (one page always stays)
void trimPages() {
    const int cap = g_gridCapacity;
    int used = 1;
    for (int i = 0; i < (int)g_settings.order.size(); ++i) if (g_settings.order[i] >= 0) used = std::max(used, i / cap + 1);
    g_settings.homePages = used;
    g_settings.order.resize((size_t)(used * cap), -1);
}
bool* storeFlag(int app);   // (below)
void installApp(int app, bool on) {
    if (bool* fl = storeFlag(app)) *fl = on;
    else if (storeApp(app)) {
        auto& v = g_settings.installed;
        v.erase(std::remove(v.begin(), v.end(), std::string(kApps[app].id)), v.end());
        if (on) v.push_back(kApps[app].id);
    }
    if (!on) {   // out of the grid, the bar and its folder
        for (int& e : g_settings.order) if (e == app) e = -1;
        auto& fv = g_settings.favorites;
        fv.erase(std::remove(fv.begin(), fv.end(), app), fv.end());
        for (auto& fo : g_settings.folders) fo.apps.erase(std::remove(fo.apps.begin(), fo.apps.end(), app), fo.apps.end());
    }
    normalizeLayout(g_gridCapacity);
    g_settingsChanged = true;
}

int g_homePage = 0;            // the page shown
float g_homeScroll = 0;        // where the pages are now (a page's number; eases to g_homePage, follows a swipe)
bool g_swiping = false;
double g_edgeSince = -1;       // an icon held at the screen's edge since then (the page turns)
int g_openFolder = -1;         // the folder shown open, -1 none
double g_folderT0 = -10;
bool g_widgetPicker = false;   // the list of widgets to add (while arranging)
bool g_addMenu = false;        // the + button's menu: New Folder, Add Widget
bool g_dragFromFolder = false; // an app being carried out of an open folder (its press began on the folder's card)
int g_grabCol = 0, g_grabRow = 0;   // where in a widget it was taken hold of (in places)
void drawWidget(ImDrawList* dl, const Fonts& f, const SimState& s, int kind, ImVec2 a, ImVec2 b, Actions& act, bool live);   // (below)

// a folder: a frosted tile with its first apps in small, its name under it
void folderTile(ImDrawList* dl, const Fonts& f, int k, ImVec2 c, float tile, bool label, float scale) {
    const ImVec2 a(c.x - tile * 0.5f * scale, c.y - tile * 0.5f * scale), b(c.x + tile * 0.5f * scale, c.y + tile * 0.5f * scale);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 52), tile * 0.24f * scale);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 40), tile * 0.24f * scale);
    const auto& apps = g_settings.folders[k].apps;
    const int n = std::min(9, (int)apps.size()), g = n <= 4 ? 2 : 3;   // the apps fill the tile: 2 x 2 up to four, else 3 x 3
    const float side = tile * scale, pad = side * (g == 2 ? 0.11f : 0.09f), gap = side * (g == 2 ? 0.08f : 0.06f);
    const float mini = (side - 2 * pad - (g - 1) * gap) / g;
    for (int i = 0; i < n; ++i) {
        const ImVec2 mc(a.x + pad + mini * 0.5f + (i % g) * (mini + gap), a.y + pad + mini * 0.5f + (i / g) * (mini + gap));
        drawTile(dl, f, apps[i], mc, mini, false, 1.0f, 1.0f);
    }
    if (label) textCentered(dl, f.small, c.x, c.y + tile * 0.5f + 6, IM_COL32(236, 241, 250, 255), g_settings.folders[k].name.c_str());
}

void home(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const double now = ImGui::GetTime();
    const float dt = io.DeltaTime;
    const float cellW = 104, cellH = 100, tile = 60, dockTile = tile, dockCell = 86, dockH = 98, dockPad = 12;   // the bar's icons as big as the grid's
    const bool hasDock = favSlots() > 0;
    const float dockTop = hasDock ? h - dockH - 10 : h + 100;   // no favorites bar: the grid has the whole screen
    const int cols = std::max(1, (int)((w - 24) / cellW));
    const float areaBot = (hasDock ? dockTop - 6 : h - 8) - 16;   // (the page dots under the grid)
    const int rows = std::max(1, (int)((areaBot - kStatusH - 10) / cellH));
    g_gridCols = cols; g_gridRows = rows;
    const int cap = std::max(cols * rows, 1);
    if (cap != g_gridCapacity) { g_gridCapacity = cap; }
    normalizeLayout(cap);
    auto& order = g_settings.order;
    const bool arranging = g_editMode || g_dragging;
    if (!arranging && (int)order.size() > cap) {   // (empty pages at the end go when the arranging is done)
        bool empty = true;
        for (int i = (g_settings.homePages - 1) * cap; i < (int)order.size(); ++i) if (order[i] >= 0) empty = false;
        if (empty && g_settings.homePages > 1) trimPages();
    }
    int pages = g_settings.homePages;
    if (g_dragging) {   // carrying something: a page more to put it on
        bool lastEmpty = true;
        for (int i = (pages - 1) * cap; i < pages * cap; ++i) if (order[i] >= 0) lastEmpty = false;
        if (!lastEmpty) { ++pages; order.resize((size_t)(pages * cap), -1); g_settings.homePages = pages; }
    }
    g_homePage = std::max(0, std::min(pages - 1, g_homePage));
    if (!g_swiping) {
        g_homeScroll += (g_homePage - g_homeScroll) * (1.0f - std::exp(-dt * 12.0f));
        if (std::fabs(g_homeScroll - g_homePage) < 0.001f) g_homeScroll = (float)g_homePage;
    }
    const std::vector<int> cover = coverOf(cap);
    const float gx0 = std::floor((w - cols * cellW) * 0.5f);
    const float gy0 = kStatusH + std::max(6.0f, std::floor((areaBot - kStatusH - rows * cellH) * 0.5f));
    const int drag = g_dragging ? g_pressApp : -1;
    std::vector<int> dock;
    for (int a : g_settings.favorites) if (((isApp(a) && appVisible(a)) || dockWidget(a)) && a != drag) dock.push_back(a);
    int dockUsed = 0;   // the places the bar's entries take
    for (int a : dock) dockUsed += favSpan(a);
    const ImVec2 bubA(16, h - 44), bubB(g_dragging ? 150.0f : 96.0f, h - 12);   // (as wide as Done; wider with a word while carrying)        // arranging: + (New Folder, Add Widget); a bin for an empty folder or a widget carried
    const bool overBubble = drag >= 0 && m.x >= bubA.x && m.x < bubB.x && m.y >= bubA.y - 6 && m.y < bubB.y + 6;
    // where the dragged thing would land: a place in the grid (a widget: its corner), or a place in the bar
    bool toDock = false;
    int target = -1, gap = -1;
    if (drag >= 0 && !overBubble) {
        toDock = hasDock && (isApp(drag) || dockWidget(drag)) && m.y >= dockTop - 8;
        if (toDock) {
            if (dockUsed + favSpan(drag) <= favSlots()) {   // room for it (the bar's places: an icon one, Frame Rate one, Volanta's overlay all seven)
                const int place = (int)std::floor((m.x - ((w - favSlots() * dockCell) * 0.5f)) / dockCell);
                gap = 0;   // the entry it goes before: the first one starting at or past that place
                for (int k = 0, at = 0; k < (int)dock.size(); ++k) { if (at + favSpan(dock[k]) * 0.5f <= place) gap = k + 1; at += favSpan(dock[k]); }
            } else toDock = false;   // the bar is full
        }
        if (!toDock && m.y < dockTop - 8) {
            int col = std::max(0, std::min(cols - 1, (int)std::floor((m.x - gx0) / cellW)));
            int row = std::max(0, std::min(rows - 1, (int)std::floor((m.y - gy0) / cellH)));
            if (isWidget(drag)) {
                int wc, wr; widgetCells(drag - kWidget0, wc, wr);
                col = std::max(0, std::min(cols - wc, col - g_grabCol)); row = std::max(0, std::min(rows - wr, row - g_grabRow));
            }
            target = g_homePage * cap + row * cols + col;
        }
        // held at the screen's edge: the page turns
        const bool edge = m.x < 18 || m.x > w - 18;
        if (edge && g_edgeSince < 0) g_edgeSince = now;
        if (!edge) g_edgeSince = -1;
        if (edge && now - g_edgeSince > 0.6) {
            const int to = g_homePage + (m.x < 18 ? -1 : 1);
            if (to >= 0 && to < pages) g_homePage = to;
            g_edgeSince = now;
        }
    } else g_edgeSince = -1;
    // a widget can go where its block is free (of others, and of icons)
    auto widgetCanGo = [&](int anchor, int kind, int self) {
        if (!widgetFits(anchor, kind, cap, cover, self)) return false;
        int wc, wr; widgetCells(kind, wc, wr);
        const int slot = anchor % cap, col = slot % cols, row = slot / cols;
        for (int r = 0; r < wr; ++r) for (int c = 0; c < wc; ++c) {
            const int i = anchor - slot + (row + r) * cols + col + c, e = i < (int)order.size() ? order[i] : -1;
            if (e >= 0 && e != self && !(isApp(e) && !appVisible(e))) return false;
        }
        return true;
    };

    // the pages: icons, folders, widgets; while arranging, the free places faintly
    struct Hit { int e; ImVec2 a, b; int slot; };
    std::vector<Hit> hits;
    const bool liveWidgets = !g_editMode && drag < 0 && g_openFolder < 0;
    for (int p = 0; p < pages; ++p) {
        const float ox = std::floor((p - g_homeScroll) * w);
        if (std::fabs(ox) >= w) continue;
        const bool thisPage = std::fabs(p - g_homeScroll) < 0.02f;
        dl->PushClipRect(ImVec2(0, kStatusH), ImVec2(w, dockTop - 4), true);
        for (int slot = 0; slot < cap; ++slot) {
            const int gi = p * cap + slot;
            const int e = gi < (int)order.size() ? order[gi] : -1;
            float slideX = 0, slideY = 0;   // (Compress: on its way from the place it was in)
            if (e >= 0 && now - g_slideT0 < kSlideTime) {
                const auto was = g_slideFrom.find(e);
                if (was != g_slideFrom.end() && was->second != gi) {
                    const int p0 = was->second / cap, s0 = was->second % cap;
                    const float k = 1 - smooth01((float)(now - g_slideT0) / kSlideTime);
                    slideX = (std::floor((p0 - g_homeScroll) * w) + (s0 % cols) * cellW - ox - (slot % cols) * cellW) * k;
                    slideY = ((s0 / cols) - (slot / cols)) * cellH * k;
                }
            }
            const float cx = ox + gx0 + (slot % cols) * cellW + slideX, cy = gy0 + (slot / cols) * cellH + slideY;
            const ImVec2 c(cx + cellW * 0.5f, cy + 6 + tile * 0.5f);
            if (isWidget(drag) && target >= 0 && p == g_homePage) {   // the block it would take
                int wc, wr; widgetCells(drag - kWidget0, wc, wr);
                if (gi == target) {
                    const bool ok = widgetCanGo(target, drag - kWidget0, drag);
                    dl->AddRectFilled(ImVec2(cx + (cellW - tile) * 0.5f - 4, cy + 2), ImVec2(cx + (wc - 1) * cellW + (cellW + tile) * 0.5f + 4, cy + (wr - 1) * cellH + 10 + tile), ok ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 80, 80, 50), tile * 0.28f);
                }
            } else if (gi == target && drag >= 0)
                dl->AddRectFilled(ImVec2(c.x - tile * 0.5f - 4, c.y - tile * 0.5f - 4), ImVec2(c.x + tile * 0.5f + 4, c.y + tile * 0.5f + 4), isFolder(e) && isApp(drag) ? accentCol(110) : IM_COL32(255, 255, 255, 60), tile * 0.28f);
            if (isWidget(e) && e != drag) {
                int wc, wr; widgetCells(e - kWidget0, wc, wr);
                // (the icons' own edges: from the first one's corner to the last one's, the icons' rounding)
                const ImVec2 wa(cx + (cellW - tile) * 0.5f, cy + 6), wb(cx + (wc - 1) * cellW + (cellW + tile) * 0.5f, cy + (wr - 1) * cellH + 6 + tile);
                const int v0 = dl->VtxBuffer.Size;
                drawWidget(dl, f, s, e - kWidget0, wa, wb, act, liveWidgets && thisPage);
                if (g_editMode) {
                    turnSince(dl, v0, ImVec2((wa.x + wb.x) * 0.5f, (wa.y + wb.y) * 0.5f), wiggle(e) * 0.25f);
                    // its remove badge
                    const ImVec2 bc(wa.x + 4, wa.y + 4);
                    dl->AddCircleFilled(bc, 11, IM_COL32(60, 64, 72, 240), 0);
                    dl->AddLine(ImVec2(bc.x - 4.5f, bc.y), ImVec2(bc.x + 4.5f, bc.y), IM_COL32(255, 255, 255, 255), 2.0f);
                    ImGui::SetCursorScreenPos(ImVec2(bc.x - 13, bc.y - 13));
                    char bid[24]; std::snprintf(bid, sizeof bid, "##wdel%d", e);
                    if (ImGui::InvisibleButton(bid, ImVec2(26, 26)) && !g_addMenu && !g_widgetPicker && g_openFolder < 0) { order[gi] = -1; g_settingsChanged = true; }
                }
                hits.push_back({e, wa, wb, gi});
                continue;
            }
            const bool shown = (isApp(e) && appVisible(e) && e != drag) || (isFolder(e) && e != drag);
            if (!shown) {
                if (arranging && cover[gi] < 0) dl->AddRectFilled(ImVec2(c.x - tile * 0.5f, c.y - tile * 0.5f), ImVec2(c.x + tile * 0.5f, c.y + tile * 0.5f), IM_COL32(255, 255, 255, 22), tile * 0.24f);
                continue;
            }
            const bool over = drag < 0 && m.x >= cx && m.x < cx + cellW && m.y >= cy && m.y < cy + cellH;
            const bool held = g_pressApp == e && io.MouseDown[0];
            const int v0 = dl->VtxBuffer.Size;
            if (isFolder(e)) {
                const Ease ez = eased(("##fold" + std::to_string(e)).c_str(), false, over, held);
                folderTile(dl, f, e - kFolder0, c, tile, true, 1.0f + 0.06f * ez.hot - 0.06f * ez.down);
            } else {
                hoverTile(dl, f, e, c, tile, over, held);
                g_iconC[e] = c; g_iconSize[e] = tile;
            }
            if (g_editMode) turnSince(dl, v0, c, wiggle(e));
            hits.push_back({e, ImVec2(cx, cy), ImVec2(cx + cellW, cy + cellH), gi});
        }
        dl->PopClipRect();
    }
    // the page dots, over the bar
    if (pages > 1) {
        const float dy = (hasDock ? dockTop : h) - 12, dw0 = pages * 14.0f;
        for (int p = 0; p < pages; ++p) {
            const float nearK = std::max(0.0f, 1.0f - std::fabs(p - g_homeScroll));
            dl->AddCircleFilled(ImVec2(std::floor(w * 0.5f - dw0 * 0.5f + 7 + p * 14), dy), 3.2f + nearK * 0.8f, IM_COL32(255, 255, 255, (int)(90 + 165 * nearK)), 0);
        }
    }
    // the favorites bar: as many places as Settings gives it, the free ones shown as outlines
    const int dockSlots = favSlots();
    const float dw = dockSlots * dockCell + dockPad * 2;
    const float dx0 = std::floor((w - dw) * 0.5f);
    if (hasDock) dl->AddRectFilled(ImVec2(dx0, dockTop), ImVec2(dx0 + dw, dockTop + dockH), IM_COL32(255, 255, 255, drag >= 0 && toDock ? 52 : 30), 22.0f);
    for (int k = dockUsed + ((drag >= 0 && toDock) ? favSpan(drag) : 0); k < dockSlots; ++k) {
        const ImVec2 c(dx0 + dockPad + k * dockCell + dockCell * 0.5f, dockTop + 8 + dockTile * 0.5f);
        dl->AddRect(ImVec2(c.x - dockTile * 0.5f, c.y - dockTile * 0.5f), ImVec2(c.x + dockTile * 0.5f, c.y + dockTile * 0.5f), IM_COL32(255, 255, 255, 46), dockTile * 0.24f, 0, snap(1.5f));
    }
    for (int k = 0, at = 0; k < (int)dock.size(); at += favSpan(dock[k]), ++k) {
        const int slot = (drag >= 0 && toDock && k >= gap) ? at + favSpan(drag) : at;
        const float cx = dx0 + dockPad + slot * dockCell;
        if (isWidget(dock[k])) {   // a widget in the bar: the icons' height, its places wide
            const int sp = favSpan(dock[k]);
            const ImVec2 wa(cx + (dockCell - dockTile) * 0.5f, dockTop + 8), wb(cx + (sp - 1) * dockCell + (dockCell + dockTile) * 0.5f, dockTop + 8 + dockTile);
            const int v0 = dl->VtxBuffer.Size;
            drawWidget(dl, f, s, dock[k] - kWidget0, wa, wb, act, liveWidgets);
            if (g_editMode) turnSince(dl, v0, ImVec2((wa.x + wb.x) * 0.5f, (wa.y + wb.y) * 0.5f), wiggle(dock[k]) * 0.25f);
            hits.push_back({dock[k], ImVec2(cx, dockTop), ImVec2(cx + sp * dockCell, dockTop + dockH), -1});
            continue;
        }
        const ImVec2 c(cx + dockCell * 0.5f, dockTop + 8 + dockTile * 0.5f);   // the icon, its name under it
        const bool over = drag < 0 && m.x >= cx && m.x < cx + dockCell && m.y >= dockTop && m.y < dockTop + dockH;
        const bool held = g_pressApp == dock[k] && io.MouseDown[0];
        const int v0 = dl->VtxBuffer.Size;
        hoverTile(dl, f, dock[k], c, dockTile, over, held);
        if (g_editMode) turnSince(dl, v0, c, wiggle(dock[k]));
        g_iconC[dock[k]] = c; g_iconSize[dock[k]] = dockTile;
        hits.push_back({dock[k], ImVec2(cx, dockTop), ImVec2(cx + dockCell, dockTop + dockH), -1});
    }
    // arranging: the + button (its menu: New Folder, Add Widget). Something carried onto it: an app makes a new folder; an empty
    // folder or a widget goes (it is a bin then)
    const bool carryEmptyFolder = isFolder(drag) && g_settings.folders[drag - kFolder0].apps.empty();
    const bool carryBin = isFolder(drag) || isWidget(drag);   // (a folder with apps: they go onto the page, the folder goes)
    if (g_editMode || drag >= 0) {
        const Ease eb0 = eased("##addbtn", false, ImGui::IsMouseHoveringRect(bubA, bubB) && drag < 0, ImGui::IsMouseHoveringRect(bubA, bubB) && ImGui::IsMouseDown(0) && drag < 0);
        const Ease eb = eased("##newfolder", overBubble, false, false);
        const ImU32 bg = carryBin ? mixCol(IM_COL32(200, 50, 50, 200), IM_COL32(240, 70, 70, 255), eb.on) : mixCol(IM_COL32(255, 255, 255, 40), accentCol(220), std::max(eb.on, g_addMenu ? 1.0f : 0.0f));
        const float grow = eb.on * 3 - eb0.down * 1.5f;
        const ImU32 bg2 = carryBin ? bg : mixCol(bg, mixCol(IM_COL32(255, 255, 255, 64), accentCol(160), 0.35f), eb0.hot * 0.6f);
        dl->AddRectFilled(ImVec2(bubA.x - grow, bubA.y - grow), ImVec2(bubB.x + grow, bubB.y + grow), bg2, 16.0f + grow);
        if (eb0.hot > 0.02f && !carryBin) dl->AddRect(ImVec2(bubA.x - grow, bubA.y - grow), ImVec2(bubB.x + grow, bubB.y + grow), accentLite((int)(150 * eb0.hot)), 16.0f + grow, 0, 1.2f);
        const float mx = (bubA.x + bubB.x) * 0.5f, my = (bubA.y + bubB.y) * 0.5f;
        // the glyph and its word, centred together on the button
        const char* word = drag < 0 ? "" : carryBin ? (isWidget(drag) ? "Remove" : carryEmptyFolder ? "Delete" : "Unpack") : "New Folder";
        const float tw = *word ? f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, word).x : 0.0f;
        const bool glyph = true;
        const float gw = glyph ? 16.0f : 0.0f, gap = (glyph && *word) ? 8.0f : 0.0f;
        const float x0 = std::floor(mx - (gw + gap + tw) * 0.5f);
        const float gx = x0 + gw * 0.5f;
        if (glyph && carryBin) {   // a bin
            dl->AddRect(ImVec2(gx - 6, my - 5), ImVec2(gx + 6, my + 8), kInk, 2.0f, 0, 1.6f);
            dl->AddLine(ImVec2(gx - 8, my - 7), ImVec2(gx + 8, my - 7), kInk, 1.8f);
            dl->AddLine(ImVec2(gx - 2.5f, my - 10), ImVec2(gx + 2.5f, my - 10), kInk, 1.8f);
        } else if (glyph) {   // a plus, on its own (no box)
            const float r = *word ? 6.0f : 8.0f;
            dl->AddLine(ImVec2(gx, my - r), ImVec2(gx, my + r), kInk, 2.2f);
            dl->AddLine(ImVec2(gx - r, my), ImVec2(gx + r, my), kInk, 2.2f);
        }
        if (*word) textAt(dl, f.small, x0 + gw + gap, std::floor(my - fontPx(f.small) * 0.5f), kInk, word);
    }

    // the finger (drawn first so Done, after it, takes its own presses)
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##home", ImVec2(w, h - kStatusH));
    if (ImGui::IsItemActivated()) {
        g_pressApp = -1; g_dragging = false; g_swiping = false; g_pressPos = m; g_pressTime = now; g_editEnteredNow = false;
        for (const Hit& ht : hits) if (m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) {
            g_pressApp = ht.e;
            if (isWidget(ht.e)) { g_grabCol = (int)((m.x - ht.a.x) / cellW); g_grabRow = (int)((m.y - ht.a.y) / cellH); }
        }
    }
    const float dxp = m.x - g_pressPos.x, dyp = m.y - g_pressPos.y;
    const float moved = std::fabs(dxp) + std::fabs(dyp);
    if (ImGui::IsItemActive() && !g_editMode && g_pressTime >= 0 && now - g_pressTime >= 0.75 && moved < 10) { g_editMode = true; g_editEnteredNow = true; }   // held: wiggle
    if (ImGui::IsItemActive() && g_editMode && g_pressApp >= 0 && !g_dragging && !g_swiping && moved > 6) g_dragging = true;
    // a swipe across (not carrying anything): the pages follow the finger
    if (ImGui::IsItemActive() && !g_dragging && !g_swiping && std::fabs(dxp) > 12 && std::fabs(dxp) > std::fabs(dyp) * 1.3f && !(g_editMode && g_pressApp >= 0)) g_swiping = true;
    if (g_swiping && ImGui::IsItemActive()) {
        float sc = g_homePage - dxp / w;
        if (sc < 0) sc *= 0.3f;   // (a little give past either end)
        if (sc > pages - 1) sc = (pages - 1) + (sc - (pages - 1)) * 0.3f;
        g_homeScroll = sc;
    }
    if (g_dragging && g_pressApp >= 0) {   // the lifted icon, under the finger
        const int v0 = dl->VtxBuffer.Size;
        if (isApp(g_pressApp)) drawTile(dl, f, g_pressApp, m, tile, false, 1.12f, 1.08f);
        else if (isFolder(g_pressApp)) folderTile(dl, f, g_pressApp - kFolder0, m, tile, false, 1.12f);
        else {
            int wc, wr; widgetCells(g_pressApp - kWidget0, wc, wr);
            const ImVec2 a(m.x - (g_grabCol + 0.5f) * cellW + 8, m.y - (g_grabRow + 0.5f) * cellH + 4);
            dl->AddRectFilled(a, ImVec2(a.x + wc * cellW - 16, a.y + wr * cellH - 14), themeTone(10, 22, 44, 200), 18.0f);
            dl->AddRect(a, ImVec2(a.x + wc * cellW - 16, a.y + wr * cellH - 14), accentLite(160), 18.0f, 0, 1.5f);
            textCentered(dl, f.bold, a.x + (wc * cellW - 16) * 0.5f, a.y + (wr * cellH - 14) * 0.5f - 8, kInk, widgetName(g_pressApp - kWidget0));
        }
        turnSince(dl, v0, m, 0.06f);
    }
    if (ImGui::IsItemDeactivated() || (g_dragFromFolder && !io.MouseDown[0])) {   // (let go: on the home screen's finger, or an app carried out of a folder)
        const int e = g_pressApp;
        auto& fav = g_settings.favorites;
        if (g_swiping) {
            if (dxp < -w * 0.15f) g_homePage = std::min(pages - 1, g_homePage + 1);
            else if (dxp > w * 0.15f) g_homePage = std::max(0, g_homePage - 1);
            g_swiping = false;
        } else if (e >= 0 && g_dragging) {   // dropped
            int from = -1;   // its place in the grid, if it was there
            for (int i = 0; i < (int)order.size(); ++i) if (order[i] == e) from = i;
            const bool fromDock = std::find(fav.begin(), fav.end(), e) != fav.end();
            auto takeOut = [&]() { if (fromDock) fav.erase(std::find(fav.begin(), fav.end(), e)); if (from >= 0) order[from] = -1; };
            if (overBubble) {
                if (isApp(e)) {   // a new folder, where the app was, with the app in it; open, its name to type
                    const int at = from >= 0 ? from : -1;
                    takeOut();
                    g_settings.folders.push_back({"Folder", {e}});
                    const int fe = kFolder0 + (int)g_settings.folders.size() - 1;
                    if (at >= 0) order[at] = fe;
                    normalizeLayout(cap);
                    g_openFolder = fe - kFolder0; g_folderT0 = now;
                    g_settingsChanged = true;
                } else if (isWidget(e)) {   // the bin: the widget goes
                    takeOut();
                    g_settingsChanged = true;
                } else if (isFolder(e)) {   // the bin: the folder goes (the ones after it move up a number); its apps into the page's free places
                    const int k = e - kFolder0;
                    const std::vector<int> apps = g_settings.folders[k].apps;
                    if (from >= 0) order[from] = -1;
                    g_settings.folders.erase(g_settings.folders.begin() + k);
                    for (int& x : order) if (x > e && x < kWidget0) --x;
                    const std::vector<int> cv = coverOf(cap);
                    const int start = from >= 0 ? from : g_homePage * cap;
                    for (int app : apps) {
                        int at = -1;
                        for (int i = start; i < (int)order.size() && at < 0; ++i) if (order[i] < 0 && (i >= (int)cv.size() || cv[i] < 0)) at = i;
                        for (int i = 0; i < start && at < 0; ++i) if (order[i] < 0 && (i >= (int)cv.size() || cv[i] < 0)) at = i;
                        if (at >= 0) order[at] = app; else order.push_back(app);
                    }
                    g_settingsChanged = true;
                }
            } else if (toDock && gap >= 0) {
                takeOut();
                // the gap counts shown icons; the stored list may also hold hidden ones: insert before the icon now at the gap
                size_t at = fav.size();
                if (gap < (int)dock.size()) for (size_t i = 0; i < fav.size(); ++i) if (fav[i] == dock[gap]) { at = i; break; }
                fav.insert(fav.begin() + at, e);
                g_settingsChanged = true;
            } else if (target >= 0 && isWidget(e)) {
                if (widgetCanGo(target, e - kWidget0, e)) {
                    takeOut();
                    // (a hidden app in its block moves to the end)
                    int wc, wr; widgetCells(e - kWidget0, wc, wr);
                    const int slot = target % cap, col = slot % cols, row = slot / cols;
                    std::vector<int> moveOut;
                    for (int r = 0; r < wr; ++r) for (int c = 0; c < wc; ++c) {
                        int& x = order[target - slot + (row + r) * cols + col + c];
                        if (x >= 0) { moveOut.push_back(x); x = -1; }
                    }
                    order[target] = e;
                    for (int x : moveOut) order.push_back(x);
                    g_settingsChanged = true;
                }
            } else if (target >= 0 && target < (int)order.size() && cover[target] < 0) {
                const int there = order[target];
                if (there == e) {}
                else if (isFolder(there) && isApp(e)) {   // into the folder
                    takeOut();
                    g_settings.folders[there - kFolder0].apps.push_back(e);
                    g_settingsChanged = true;
                } else if (there < 0 || (isApp(there) && !appVisible(there))) {   // a free place (or one a hidden app keeps): it goes there
                    if (from >= 0) order[from] = there; else if (there >= 0) { order[target] = -1; order.push_back(there); }
                    if (fromDock) fav.erase(std::find(fav.begin(), fav.end(), e));
                    order[target] = e;
                    g_settingsChanged = true;
                } else if (!isWidget(there)) {   // onto an icon: the two change places (from the bar, the other icon goes into the bar in its place)
                    if (fromDock && !isApp(there)) {}   // (a folder never goes into the bar)
                    else {
                        if (fromDock) *std::find(fav.begin(), fav.end(), e) = there;
                        else if (from >= 0) order[from] = there;
                        order[target] = e;
                        if (fromDock && from >= 0) order[from] = -1;   // (it was in both: its old grid place is free now)
                        g_settingsChanged = true;
                    }
                }
            }
        } else if (!g_dragging && !g_editEnteredNow && moved < 10) {
            if (e >= 0 && !g_editMode) {
                for (const Hit& ht : hits) if (ht.e == e && m.x >= ht.a.x && m.x < ht.b.x && m.y >= ht.a.y && m.y < ht.b.y) {
                    if (isApp(e)) openApp(e);
                    else if (isFolder(e)) { g_openFolder = e - kFolder0; g_folderT0 = now; }
                    else if (isWidget(e)) {   // a widget opens what it shows
                        static const int to[W_COUNT] = {APP_WEATHER, APP_ROUTES, APP_MUSIC, -1, APP_LIVEMAP};
                        const int app = to[e - kWidget0];
                        if (app >= 0 && appVisible(app)) openApp(app);
                    }
                }
            } else if (e >= 0 && g_editMode && isFolder(e)) {
                g_openFolder = e - kFolder0; g_folderT0 = now;   // (arranging: open to rename it, or take apps out)
            } else if (e < 0 && g_editMode) {
                g_editMode = false;   // a tap on an empty place: done
            }
        }
        g_pressApp = -1; g_dragging = false; g_pressTime = -1; g_editEnteredNow = false; g_dragFromFolder = false;
        normalizeLayout(cap);
    }
    if (g_editMode && drag < 0) {
        if (pill(dl, f, "##homedone", "Done", ImVec2(w - 96, h - 44), ImVec2(w - 16, h - 12), true, true)) { g_editMode = false; g_widgetPicker = false; g_addMenu = false; }
        {   // Compress, beside Done (above it where the favorites bar leaves no room beside): everything slides up to fill the open places
            const float barRight = hasDock ? (w + favSlots() * dockCell) * 0.5f + 12 : 0;
            const bool beside = w - 96 - 8 - 104 >= barRight + 6;
            const ImVec2 ca = beside ? ImVec2(w - 96 - 8 - 104, h - 44) : ImVec2(w - 120, h - 84), cb = beside ? ImVec2(w - 96 - 8, h - 12) : ImVec2(w - 16, h - 52);
            if (pill(dl, f, "##homecompress", "Compress", ca, cb, false, true)) { g_addMenu = false; g_widgetPicker = false; compressLayout(cap); }
        }
        // + tapped: its menu, above it
        ImGui::SetCursorScreenPos(bubA);
        if (ImGui::InvisibleButton("##addbtn", ImVec2(bubB.x - bubA.x, bubB.y - bubA.y))) { g_addMenu = !g_addMenu; g_widgetPicker = false; }
        const float mt = openness("##addmenuopen", g_addMenu);
        if (g_addMenu) {   // open: a press anywhere else only closes it (nothing under it is pressed; no tint)
            ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##addmenucatch", ImVec2(w, h - kStatusH))) g_addMenu = false;
        }
        if (mt > 0.01f) {   // (it grows out of the + and shrinks back into it)
            const int v0 = dl->VtxBuffer.Size;
            ImGui::BeginDisabled(!g_addMenu);
            const ImVec2 ma(bubA.x, bubA.y - 86), mb(bubA.x + 150, bubA.y - 8);
            ImGui::SetCursorScreenPos(ma);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##addmenubg", ImVec2(mb.x - ma.x, mb.y - ma.y));
            dl->AddRectFilled(ImVec2(ma.x + 2, ma.y + 4), ImVec2(mb.x + 2, mb.y + 4), IM_COL32(0, 0, 0, 70), 14.0f);
            dl->AddRectFilled(ma, mb, themeTone(12, 22, 40, 242), 14.0f);
            dl->AddRect(ma, mb, IM_COL32(255, 255, 255, 46), 14.0f, 0, 1.0f);
            if (pill(dl, f, "##addfolder", "New Folder", ImVec2(ma.x + 8, ma.y + 8), ImVec2(mb.x - 8, ma.y + 38), false, true)) {   // an empty one, at the first free place
                g_addMenu = false;
                g_settings.folders.push_back({"Folder", {}});
                normalizeLayout(cap);
                g_openFolder = (int)g_settings.folders.size() - 1; g_folderT0 = now;
                g_settingsChanged = true;
            }
            if (pill(dl, f, "##addwidget", "Add Widget", ImVec2(ma.x + 8, ma.y + 42), ImVec2(mb.x - 8, ma.y + 72), false, true)) { g_addMenu = false; g_widgetPicker = true; }
            ImGui::EndDisabled();
            popIn(dl, v0, ImVec2(bubA.x + 20, bubA.y), mt);
        }
    } else if (!g_editMode) g_addMenu = false;
    // the widgets to add: a card over the grid (a widget is never a favorite)
    if (!g_editMode) g_widgetPicker = false;
    const float wpt = openness("##wpickopen", g_widgetPicker);
    if (wpt > 0.01f) {
        const int wv0 = dl->VtxBuffer.Size;
        ImGui::BeginDisabled(!g_widgetPicker);
        const ImVec2 a(170, kStatusH + 30), b(w - 170, h - 120);
        if (g_widgetPicker) {   // a press beside the card only closes it (nothing under it is pressed; no tint)
            ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##wpickcatch", ImVec2(w, h - kStatusH))) g_widgetPicker = false;
            ImGui::SetCursorScreenPos(a);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##wpickcard", ImVec2(b.x - a.x, b.y - a.y));
        }
        dl->AddRectFilled(a, b, themeTone(14, 26, 50, 235), 18.0f);
        glass(dl, a, b, 18.0f, 1.0f);
        textAt(dl, f.bold, a.x + 18, a.y + 14, kInk, "Add A Widget");
        float y = a.y + 46;
        for (int k = 0; k < W_COUNT; ++k) {
            bool there = false;
            for (int x : order) if (x == kWidget0 + k) there = true;
            for (int x : g_settings.favorites) if (x == kWidget0 + k) there = true;
            int wc, wr; widgetCells(k, wc, wr);
            char lab[64]; std::snprintf(lab, sizeof lab, "%s   %d x %d", widgetName(k), wc, wr);
            char id[24]; std::snprintf(id, sizeof id, "##addw%d", k);
            if (pill(dl, f, id, there ? (std::string(widgetName(k)) + "  (on the home screen)").c_str() : lab, ImVec2(a.x + 18, y), ImVec2(b.x - 18, y + 32), false, !there)) {
                // at the top of the page shown, and everything on it (and after it) moves down as many rows as it takes, as on an
                // iPad: what no longer fits flows onto the next page (a new one if need be)
                pushInWidgetAt(kWidget0 + k, g_homePage * cap, cap);
                normalizeLayout(cap);
                for (int i = 0; i < (int)order.size(); ++i) if (order[i] == kWidget0 + k) g_homePage = i / cap;
                g_widgetPicker = false;
                g_settingsChanged = true;
            }
            y += 38;
        }
        ImGui::EndDisabled();
        popIn(dl, wv0, ImVec2(w * 0.5f, (kStatusH + h) * 0.5f), wpt);
    }
    // an open folder: its apps on a frosted card; its name can be typed over; arranging, each app has a badge that takes it out
    static int closingFolder = -1; static double closingT0 = -10;
    if (g_openFolder < 0 && closingFolder >= 0 && closingFolder < (int)g_settings.folders.size() && now - closingT0 < 0.18) {   // shrinking away
        const int vc = dl->VtxBuffer.Size;
        const float e = 1.0f - (float)(now - closingT0) / 0.18f;
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(150 * e)));
        const auto& fo = g_settings.folders[closingFolder];
        const float pw = 4 * 100 + 40, ph = std::max(1, ((int)fo.apps.size() + 3) / 4) * 96 + 96;
        const ImVec2 c(w * 0.5f, (kStatusH + h) * 0.5f - 10), a(c.x - pw * 0.5f, c.y - ph * 0.5f), b(c.x + pw * 0.5f, c.y + ph * 0.5f);
        dl->AddRectFilled(a, b, themeTone(14, 26, 50, 230), 24.0f);
        textCentered(dl, f.body, c.x, a.y + 22, kInk, fo.name.c_str());
        for (size_t i = 0; i < fo.apps.size(); ++i) drawTile(dl, f, fo.apps[i], ImVec2(a.x + 20 + (i % 4) * 100 + 50, a.y + 70 + (i / 4) * 96 + tile * 0.5f), tile, true, 1.0f, 1.0f);
        popIn(dl, vc + 4, c, e);   // (the veil fades on its own)
    } else if (g_openFolder < 0) closingFolder = -1;
    if (g_openFolder >= 0 && g_openFolder < (int)g_settings.folders.size()) {
        closingFolder = g_openFolder; closingT0 = now;
        auto& fo = g_settings.folders[g_openFolder];
        const float e0 = std::min(1.0f, (float)(now - g_folderT0) / 0.22f), e = 1 - (1 - e0) * (1 - e0) * (1 - e0);
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(150 * e)));
        ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
        ImGui::SetNextItemAllowOverlap();   // (the card and its name field, after it, take their own presses)
        const bool outside = ImGui::InvisibleButton("##folderveil", ImVec2(w, h - kStatusH));
        const int n = (int)fo.apps.size(), fc = 4, fr = std::max(1, (n + fc - 1) / fc);
        const float cw = 100, ch = 96, pw = fc * cw + 40, ph = fr * ch + 96;
        const bool naming = g_kbd && g_kbdTarget == 2 && g_typeInto == &fo.name;   // its name being typed: the card moves up over the keys
        const ImVec2 c(w * 0.5f, naming ? kStatusH + 12 + ph * 0.5f : (kStatusH + h) * 0.5f - 10);
        const float sc = 0.85f + 0.15f * e;
        const ImVec2 a(c.x - pw * 0.5f * sc, c.y - ph * 0.5f * sc), b(c.x + pw * 0.5f * sc, c.y + ph * 0.5f * sc);
        dl->AddRectFilled(a, b, themeTone(14, 26, 50, (int)(230 * e)), 24.0f);   // (solid under the frost: the icons behind stay out of it)
        glass(dl, a, b, 24.0f, e);
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##folderpanel", ImVec2(b.x - a.x, b.y - a.y));   // (a press on the card keeps it open)
        textField(dl, f, "##foldername", fo.name, ImVec2(c.x - 120, a.y + 14), ImVec2(c.x + 120, a.y + 48), "Folder Name", false, 0);
        if (fo.name.size() > 24) fo.name.resize(24);
        int takeOut = -1;
        for (int i = 0; i < n; ++i) {
            const ImVec2 ic(a.x + 20 + (i % fc) * cw + cw * 0.5f, a.y + 70 + (i / fc) * ch + tile * 0.5f);
            char id[24]; std::snprintf(id, sizeof id, "##fapp%d", i);
            ImGui::SetCursorScreenPos(ImVec2(ic.x - cw * 0.5f, ic.y - tile * 0.5f - 4));
            ImGui::SetNextItemAllowOverlap();
            const bool hit = ImGui::InvisibleButton(id, ImVec2(cw, ch));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 8.0f) && takeOut < 0 && !g_dragFromFolder) {   // dragged: out of the folder, onto the page
                const int app = fo.apps[i];
                fo.apps.erase(fo.apps.begin() + i);
                g_openFolder = -1;
                g_editMode = true; g_dragging = true; g_dragFromFolder = true; g_pressApp = app; g_pressTime = -1;
                g_settingsChanged = true;
                break;
            }
            const int v0 = dl->VtxBuffer.Size;
            hoverTile(dl, f, fo.apps[i], ic, tile, ImGui::IsItemHovered(), ImGui::IsItemActive());
            g_iconC[fo.apps[i]] = ic; g_iconSize[fo.apps[i]] = tile;
            if (g_editMode) {
                turnSince(dl, v0, ic, wiggle(fo.apps[i]));
                const ImVec2 bc(ic.x - tile * 0.5f + 2, ic.y - tile * 0.5f + 2);
                dl->AddCircleFilled(bc, 11, IM_COL32(60, 64, 72, 240), 0);
                dl->AddLine(ImVec2(bc.x - 4.5f, bc.y), ImVec2(bc.x + 4.5f, bc.y), IM_COL32(255, 255, 255, 255), 2.0f);
                ImGui::SetCursorScreenPos(ImVec2(bc.x - 13, bc.y - 13));
                char bid[24]; std::snprintf(bid, sizeof bid, "##fout%d", i);
                if (ImGui::InvisibleButton(bid, ImVec2(26, 26))) takeOut = i;
            } else if (hit) { const int app = fo.apps[i]; g_openFolder = -1; openApp(app); break; }
        }
        if (n == 0) textCentered(dl, f.small, c.x, a.y + 80, kMuted, g_editMode ? "Drag apps onto the folder to add them" : "Empty: hold an icon, then drag apps onto the folder");
        if (takeOut >= 0) {   // back onto the home screen
            fo.apps.erase(fo.apps.begin() + takeOut);
            normalizeLayout(cap);
            g_settingsChanged = true;
        }
        if (naming) pageKeyboard(dl, f, w, h, act);   // the on-screen keys (with the physical keyboard set, its bar)
        if (outside && !(g_kbd && g_kbdTarget == 2 && g_typeInto == &fo.name)) { g_openFolder = -1; g_settingsChanged = true; }
        else if (outside) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; g_settingsChanged = true; }
    } else g_openFolder = -1;
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
    dl->AddText(f.body, fontPx(f.body), ImVec2(30, std::floor(my - fontPx(f.body) * 0.5f)), col, g_backTo >= 0 && g_backTo < APP_COUNT ? kApps[g_backTo].name : "Home");
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
// the last item's hover, eased (a short fade in and out, as the buttons'); pressed, a little brighter (up to 1.5)
float itemHover() {
    static ImGuiStorage st;
    float* v = st.GetFloatRef(ImGui::GetItemID(), 0.0f);
    const float target = ImGui::IsItemHovered() ? (ImGui::IsItemActive() ? 1.5f : 1.0f) : 0.0f;
    *v += (target - *v) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 18.0f));
    if (std::fabs(*v - target) < 0.004f) *v = target;
    return *v;
}
// a menu, card or bubble opening (towards 1) and closing (towards 0), by its id: drawn while above 0, so it fades and grows in and out
float openness(const char* id, bool open, float rate) {
    static ImGuiStorage st;
    float* v = st.GetFloatRef(ImGui::GetID(id), 0.0f);
    const float target = open ? 1.0f : 0.0f;
    *v += (target - *v) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * rate));
    if (std::fabs(*v - target) < 0.004f) *v = target;
    return *v;
}
// what was drawn since vtx0, grown from origin (92 % to full) and faded in with t (0..1, eased out)
void popIn(ImDrawList* dl, int vtx0, ImVec2 origin, float t) {
    if (t >= 0.999f) return;
    const float e = 1 - (1 - t) * (1 - t) * (1 - t), k = 0.92f + 0.08f * e;
    const unsigned am = (unsigned)(std::max(0.0f, std::min(1.0f, e)) * 256.0f);
    for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos = ImVec2(origin.x + (v.pos.x - origin.x) * k, origin.y + (v.pos.y - origin.y) * k);
        const unsigned a = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) * am >> 8;
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}
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
    textCentered(dl, f.small, (a.x + b.x) * 0.5f, std::floor((a.y + b.y - fontPx(f.small)) * 0.5f), enabled ? mixCol(kInk, onAccent(), e.on) : IM_COL32(236, 241, 250, 90), label);
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

// a new tab's page (the helper's xptb::kStartPage): counted as blank, and called "New Tab"
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
    const Ease e = eased(id, false, hot, down);
    if (e.hot > 0.01f || e.down > 0.01f) {   // pressable: it lightens, with the accent's edge (faded in and out, as the other buttons')
        const float in = e.down * 1.5f;
        const ImVec2 a2(a.x + in, a.y + in), b2(b.x - in, b.y - in);
        ImGui::GetWindowDrawList()->AddRectFilled(a2, b2, IM_COL32(255, 255, 255, (int)(32 * e.hot + 16 * e.down)), 8.0f);
        ImGui::GetWindowDrawList()->AddRect(a2, b2, accentLite((int)(120 * e.hot + 50 * e.down)), 8.0f, 0, 1.2f);
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
    {   // capitals where a sentence starts (notes, a folder's name): Shift comes on by itself, as on an iPad (tapped, it goes off)
        static const std::string* lastInto = nullptr;
        static size_t lastLen = (size_t)-1;
        bool prose = g_kbdTarget == 2 && g_typeInto && g_typeMultiline;
        for (const auto& fo : g_settings.folders) if (g_typeInto == &fo.name) prose = true;
        if (prose && (g_typeInto != lastInto || g_typeInto->size() != lastLen) && !g_kbdCaps && !g_kbdSymbols) {
            const std::string& t = *g_typeInto;
            size_t i = t.size();
            while (i > 0 && t[i - 1] == ' ') --i;
            g_kbdShift = i == 0 || t[i - 1] == '\n' || ((t[i - 1] == '.' || t[i - 1] == '!' || t[i - 1] == '?') && i < t.size());
        }
        lastInto = prose ? g_typeInto : nullptr;
        lastLen = prose ? g_typeInto->size() : (size_t)-1;
    }
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
        textCentered(dl, fn, x + ww * 0.5f, std::floor(yy + (kh - fontPx(fn)) * 0.5f), lit ? onAccent() : kInk, label);
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
        if (r == 3) {   // Delete: held, it goes on deleting (after half a second, 16 a second)
            static double heldAt = -1, lastRep = 0;
            static bool repeated = false;
            const bool hit = key("##kback", "Delete", x, yy, x00 + 10 * kw + 9 * gap - x, false);
            const double now = ImGui::GetTime();
            if (ImGui::IsItemActivated()) { typeKey(act, 8, false); repeated = false; }   // (on the press, as a real key)
            if (ImGui::IsItemActive()) {
                if (heldAt < 0) heldAt = now;
                else if (now - heldAt > 0.45 && now - lastRep > 0.06) { typeKey(act, 8, false); lastRep = now; repeated = true; }
            } else heldAt = -1;
            (void)hit; (void)repeated;
        }
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
// inputH: how much of it, from the top, takes the pointer (the rest is under the keys), -1 all
void webView(ImDrawList* dl, const Fonts& f, const SimState& s, float x0, float y0, float w, float h, Actions& act, bool dragScrolls = false, float inputH = -1) {
    act.webShown = true;
    act.webX = x0; act.webY = y0; act.webW = w; act.webH = h;
    const BrowserState& b = s.browser;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(244, 246, 250, 255));
    const ImVec2 c(x0 + w * 0.5f, y0 + h * 0.42f);
    if (b.status != 1 || !b.tex || g_webHold) {
        const bool bad = b.status == -1 || b.status == -3;
        textCentered(dl, f.bold, c.x, c.y - 10, IM_COL32(30, 40, 60, 255), b.status == -3 ? "The browser is not available here" : b.status == -1 ? "The browser stopped" : b.status == 1 ? "Loading..." : "Starting the browser...");
        if (!b.message.empty()) dl->AddText(f.small, fontPx(f.small), ImVec2(x0 + 30, c.y + 16), IM_COL32(90, 100, 120, 255), b.message.c_str(), nullptr, w - 60);
        if (b.status == -1 && pill(dl, f, "##webretry", "Try Again", ImVec2(c.x - 60, c.y + 64), ImVec2(c.x + 60, c.y + 92), true, true)) act.webRetry = true;
        (void)bad;
        return;
    }
    dl->AddImage(b.tex, ImVec2(x0, y0), ImVec2(x0 + w, y0 + h));
    // the pointer: moves, presses and the wheel, in the page's own units (one per layout unit)
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##web", ImVec2(w, inputH >= 0 ? std::max(1.0f, inputH) : h));
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 p(std::floor(io.MousePos.x - x0), std::floor(io.MousePos.y - y0));
    static bool dragging = false;
    static bool asMouse = false;   // a document's press at its right edge: its own scroll bar, dragged as a mouse would
    if (ImGui::IsItemActivated()) asMouse = dragScrolls && p.x > w - 20;
    const bool docMode = dragScrolls && !asMouse;
    dragScrolls = docMode;
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
    if (hovered && io.MouseWheel != 0) act.web.push_back({11 /*WHEEL*/, p.x, p.y, (int)std::lround(io.MouseWheel * 100), 0, io.KeyCtrl ? 1 : 0});   // (Ctrl and the wheel: a PDF zooms)
    if (g_webHover && !hovered && !active) { act.web.push_back({10 /*MOUSE_LEAVE*/, g_webLast.x, g_webLast.y, 0}); g_webLast = ImVec2(-1, -1); }
    g_webHover = hovered || active;
}

// The page's zoom: - , the zoom (muted while it is the one fitting the page to the view; tapped when chosen: back to fitting), +.
// The zoom chosen stays with that tab; a new tab fits its page.
float scrollBar(ImDrawList* dl, const char* id, float x, float y0, float y1, float scroll, float content, float shown);   // (below)
bool g_browserNoTabs = false;   // the Browser's last tab was closed: its New Tab page (no tab opened by itself)
bool g_barsHidden = false;      // the Browser's tabs, address and bookmarks folded away (the arrow brings them back)
bool g_historyOpen = false, g_volumeOpen = false;
float g_historyScroll = 0;
int g_appHomeClosing = 0;       // a web app's Home: its other tabs being closed (one a frame), then its first goes home
bool g_zoomTyping = false;   // a zoom percentage being typed (the % tapped twice)
std::string g_zoomEdit;
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
    // tapped (a zoom chosen): back to fitting; tapped twice: a percentage typed on the keys (Enter sets it)
    ImGui::SetCursorScreenPos(za);
    ImGui::BeginDisabled(!enabled);
    const bool zhit = ImGui::InvisibleButton(id, ImVec2(ze.x - za.x, ze.y - za.y)) && enabled;
    const bool zhot = ImGui::IsItemHovered() && enabled;
    const bool zdouble = zhot && ImGui::IsMouseDoubleClicked(0);
    ImGui::EndDisabled();
    if (g_zoomTyping && g_typeInto != &g_zoomEdit) {   // typed and finished (Enter or Done): that zoom, if a number came
        g_zoomTyping = false;
        const int pct = std::atoi(g_zoomEdit.c_str());
        if (pct > 0) { act.webZoom = 3; act.webZoomPct = std::max(25, std::min(300, pct)); }
    }
    const bool typing = g_zoomTyping && g_typeInto == &g_zoomEdit;
    if (zhot || typing) dl->AddRectFilled(za, ze, IM_COL32(255, 255, 255, typing ? 34 : 20), 8.0f);
    if (typing) dl->AddRect(za, ze, accentLite(220), 8.0f, 0, 1.4f);
    if (zdouble) { g_zoomTyping = true; g_zoomEdit.clear(); g_typeInto = &g_zoomEdit; g_typeMultiline = false; g_kbd = true; g_kbdTarget = 2; }
    else if (zhit && chosen && !typing) act.webZoom = 2;
    if (typing) {
        const std::string tt = g_zoomEdit + "%";
        textCentered(dl, f.small, (za.x + ze.x) * 0.5f, std::floor((y0 + y1) * 0.5f - fontPx(f.small) * 0.5f), kInk, tt.c_str());
    } else textCentered(dl, f.small, (za.x + ze.x) * 0.5f, std::floor((y0 + y1) * 0.5f - fontPx(f.small) * 0.5f), chosen ? kInk : kMuted, zt);
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
        } else if (g_webGroup != 0 || !g_browserNoTabs) openTabHere(g_webGroup == 0 ? std::string() : g_url);   // none left: the Browser's search page, or the app's site
    }
    g_webHold = !activeHere;
    if (g_webGroup == 0 && activeHere && b.status == 1 && !b.loading && !b.url.empty() && !isStartPage(b.url) && b.url.compare(0, 5, "file:") != 0) {
        static std::string lastUrl;
        if (b.url != lastUrl) {   // (a page seen again moves to the end, newest last; 200 at most)
            lastUrl = b.url;
            auto& hist = g_settings.history;
            for (size_t i = 0; i < hist.size(); ++i) if (hist[i].second == b.url) { hist.erase(hist.begin() + (long)i); break; }
            hist.push_back({b.title.empty() ? hostOf(b.url) : b.title, b.url});
            if (hist.size() > 200) hist.erase(hist.begin());
            g_settingsChanged = true;
        }
    }
    // the Browser's media volume: a speaker, and its slider under it
    auto volumeButton = [&](const char* id, ImVec2 a, ImVec2 e) {
        if (iconButton(id, a, e, b.status == 1)) g_volumeOpen = !g_volumeOpen;
        const ImVec2 c((a.x + e.x) * 0.5f - 2, (a.y + e.y) * 0.5f);
        const ImU32 col = g_volumeOpen ? accentLite(255) : IM_COL32(210, 222, 242, 255);
        dl->AddRectFilled(ImVec2(c.x - 7, c.y - 3), ImVec2(c.x - 3, c.y + 3), col, 1.0f);
        dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 3), ImVec2(c.x + 2, c.y - 8), ImVec2(c.x + 2, c.y + 8), col);
        dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 3), ImVec2(c.x + 2, c.y + 8), ImVec2(c.x - 4, c.y + 3), col);
        const float v = g_settings.browserVolume;
        if (v <= 0.001f) { dl->AddLine(ImVec2(c.x + 5, c.y - 4), ImVec2(c.x + 11, c.y + 4), col, 1.6f); dl->AddLine(ImVec2(c.x + 5, c.y + 4), ImVec2(c.x + 11, c.y - 4), col, 1.6f); }
        else {
            dl->PathArcTo(c, 6.5f, -0.8f, 0.8f, 8); dl->PathStroke(col, 0, 1.6f);
            if (v > 0.5f) { dl->PathArcTo(c, 10.5f, -0.8f, 0.8f, 10); dl->PathStroke(col, 0, 1.6f); }
        }
    };
    auto volumeBody = [&](ImVec2 a) {   // a card with the slider; a tap anywhere else closes it
        const ImVec2 e(a.x + 240, a.y + 52);
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##volbg", ImVec2(e.x - a.x, e.y - a.y));
        const bool inside = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(a.x + 2, a.y + 4), ImVec2(e.x + 2, e.y + 4), IM_COL32(0, 0, 0, 70), 12.0f);
        dl->AddRectFilled(a, e, themeTone(12, 22, 40, 246), 12.0f);
        dl->AddRect(a, e, IM_COL32(255, 255, 255, 46), 12.0f);
        char txt[16];
        std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(g_settings.browserVolume * 100));
        float v = g_settings.browserVolume;
        if (smoothSlider(dl, f, "##bvol", ImVec2(a.x + 10, a.y + 11), ImVec2(e.x - 10, a.y + 41), v, 0.0f, 1.0f, 20, true, txt)) {
            g_settings.browserVolume = v; g_settingsChanged = true; act.webVolume = (int)std::lround(v * 100);
        }
        if (ImGui::IsMouseClicked(0) && !inside && !ImGui::IsItemHovered()) {
            static int skip = 0;   // (the click that opened it is not the one that closes it)
            if (++skip > 1) { g_volumeOpen = false; skip = 0; }
        }
    };
    auto volumePanel = [&](ImVec2 a) {   // (it drops in from its button, and fades away)
        const float vt = openness("##volopen", g_volumeOpen);
        if (vt < 0.01f) return;
        const int v0 = dl->VtxBuffer.Size;
        ImGui::BeginDisabled(!g_volumeOpen);
        volumeBody(a);
        ImGui::EndDisabled();
        popIn(dl, v0, ImVec2(a.x + 220, a.y), vt);
    };
    if (videoFull(s)) {   // full screen: the page on the whole tablet; "Exit Full Screen" shows for a while when the pointer moves
        webView(dl, f, s, 0, 0, w, h, act);
        static double movedAt = -10;
        static ImVec2 lastMouse(-1, -1);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x != lastMouse.x || mp.y != lastMouse.y) { movedAt = ImGui::GetTime(); lastMouse = mp; }
        const float k = 1.0f - smooth01((float)(ImGui::GetTime() - movedAt - 2.5) / 0.4f);
        if (k > 0.01f) {
            const ImVec2 a(w - 150, 10), e(w - 10, 40);
            dl->AddRectFilled(a, e, IM_COL32(0, 0, 0, (int)(150 * k)), 15.0f);
            if (pill(dl, f, "##webfullx", "Exit Full Screen", a, e, false, true)) act.webExitFullscreen = true;
            if (g_webGroup == 0) {
                dl->AddRectFilled(ImVec2(w - 196, 10), ImVec2(w - 158, 40), IM_COL32(0, 0, 0, (int)(150 * k)), 15.0f);
                volumeButton("##fvol", ImVec2(w - 196, 10), ImVec2(w - 158, 40));
            }
        }
        if (g_webGroup == 0) { if (g_volumeOpen) movedAt = ImGui::GetTime(); volumePanel(ImVec2(w - 250, 46)); }
        return;
    }
    // the tab strip: each tab with its page's title and a close button; + (the Browser's) opens a blank one and the address bar
    auto tabStrip = [&](float top, float tabsH, bool plus) {
        dl->AddRectFilled(ImVec2(0, top), ImVec2(w, top + tabsH), themeTone(6, 16, 34, 245));
        const int n = std::max(1, (int)tabs.size());
        const float plusW = plus ? 34.0f : 0.0f, tw = std::min(190.0f, std::floor((w - 16 - plusW - 6) / n));
        float x = 8;
        int idx = 0;
        // each tab glides to its place and width: a new one grows in, the others slide over when one closes
        static std::map<int, std::pair<float, float>> placed;   // tab id: its x and width as drawn
        struct Ghost { float x, w; std::string title; double t0; };
        static std::vector<Ghost> ghosts;                       // tabs just closed: they shrink and fade where they were
        static std::set<int> closing;                           // (and are not drawn again while the browser closes them)
        for (auto it = closing.begin(); it != closing.end();) { bool still = false; for (const BrowserState::Tab& t : tabs) still = still || t.id == *it; if (still) ++it; else it = closing.erase(it); }
        for (size_t gi = 0; gi < ghosts.size();) {
            const float e = (float)(ImGui::GetTime() - ghosts[gi].t0) / 0.18f;
            if (e >= 1) { ghosts.erase(ghosts.begin() + (long)gi); continue; }
            const Ghost& gh = ghosts[gi];
            const float gw = gh.w * (1 - e);
            if (gw > 6) {
                const ImVec2 ga(gh.x, top + 4), ge(gh.x + gw - 4, top + tabsH);
                dl->AddRectFilled(ga, ge, IM_COL32(255, 255, 255, (int)(14 * (1 - e))), 8.0f, ImDrawFlags_RoundCornersTop);
                dl->PushClipRect(ga, ge, true);
                textAt(dl, f.small, ga.x + 10, std::floor(ga.y + (ge.y - ga.y - fontPx(f.small)) * 0.5f), IM_COL32(150, 168, 198, (int)(255 * (1 - e))), gh.title.c_str());
                dl->PopClipRect();
            }
            ++gi;
        }
        const float dtT = ImGui::GetIO().DeltaTime, kT = 1.0f - std::exp(-dtT * 16.0f);
        {
            std::map<int, std::pair<float, float>> keep;
            float tx = 8;
            for (const BrowserState::Tab& t : tabs) {
                if (closing.count(t.id)) continue;   // (the others close up at once)
                auto it = placed.find(t.id);
                std::pair<float, float> p = it != placed.end() ? it->second : std::make_pair(tx, 0.0f);
                p.first += (tx - p.first) * kT; p.second += (tw - p.second) * kT;
                if (std::fabs(p.first - tx) < 0.3f) p.first = tx;
                if (std::fabs(p.second - tw) < 0.3f) p.second = tw;
                keep[t.id] = p;
                tx += tw;
            }
            placed.swap(keep);
        }
        float plusX = 8;
        for (const BrowserState::Tab& t : tabs) { const auto& p = placed[t.id]; plusX = std::max(plusX, p.first + p.second); }
        for (const BrowserState::Tab& t : tabs) {
            const bool on = t.id == b.activeTab;
            x = placed[t.id].first;
            const float twNow = placed[t.id].second;
            if (twNow < 26 || closing.count(t.id)) { ++idx; continue; }   // (still growing in: drawn once there is room for it; or closing)
            const ImVec2 a(x, top + 4), e(x + twNow - 4, top + tabsH);
            char id[24];
            std::snprintf(id, sizeof id, "##tab%d", idx);
            ImGui::SetCursorScreenPos(a);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x - 22, e.y - a.y)) && !on) act.webTabSelect = t.id;
            const bool hot = ImGui::IsItemHovered();
            dl->AddRectFilled(a, e, on ? themeTone(10, 24, 48, 255) : hot ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10), 8.0f, ImDrawFlags_RoundCornersTop);
            // the Browser's last tab's x clears it (a fresh search page takes its place): there is always one
            std::string title = isStartPage(t.url) ? std::string("New Tab") : !t.title.empty() ? t.title : hostOf(t.url);
            const float roomW = e.x - a.x - 34;
            while (title.size() > 4 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, title.c_str()).x > roomW) { title.resize(title.size() - 4); title += "..."; }
            dl->PushClipRect(a, ImVec2(e.x - 22, e.y), true);
            textAt(dl, f.small, a.x + 10, std::floor(a.y + (e.y - a.y - fontPx(f.small)) * 0.5f), on ? kInk : kMuted, title.c_str());
            dl->PopClipRect();
            if (t.loading) dl->AddCircleFilled(ImVec2(a.x + 4, a.y + (e.y - a.y) * 0.5f), 2.0f, IM_COL32(90, 160, 255, 255), 0);
            const ImVec2 cc(e.x - 12, a.y + (e.y - a.y) * 0.5f);
            std::snprintf(id, sizeof id, "##tabx%d", idx);
            ImGui::SetCursorScreenPos(ImVec2(e.x - 22, a.y));
            if (ImGui::InvisibleButton(id, ImVec2(22, e.y - a.y))) {
                act.webTabClose = t.id; if (g_webGroup == 0 && tabs.size() == 1) g_browserNoTabs = true;
                ghosts.push_back({x, twNow, title, ImGui::GetTime()});
                closing.insert(t.id);
            }
            const bool xh = ImGui::IsItemHovered();
            if (xh) dl->AddCircleFilled(cc, 8.0f, IM_COL32(255, 255, 255, 28), 0);
            const ImU32 xc = xh ? kInk : kMuted;
            dl->AddLine(ImVec2(cc.x - 4, cc.y - 4), ImVec2(cc.x + 4, cc.y + 4), xc, 1.5f);
            dl->AddLine(ImVec2(cc.x - 4, cc.y + 4), ImVec2(cc.x + 4, cc.y - 4), xc, 1.5f);
            ++idx;
        }
        x = plusX;
        if (plus) {
            const ImVec2 pa(x + 2, top + 5), pe(x + 2 + plusW - 4, top + tabsH - 3);
            if (iconButton("##tabnew", pa, pe, room && b.status == 1)) { act.webTabNew = true; act.webTabGroup = 0; act.webTabUrl.clear(); g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); g_browserNoTabs = false; }
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
        // (no keyboard button: the keys come up by themselves when a text field on the page is tapped)
        // a page the app opened (a chart, a PDF, VATSIM's prefiling) shows over it, without tabs: Back leaves it (closed) when it
        // has nowhere else to go back to, and Home closes every one and goes to the app's start
        const bool canBack = b.canBack || tabs.size() > 1;
        if (iconButton("##wback", ImVec2(w - 92, kStatusH + 6), ImVec2(w - 56, kStatusH + kHeaderH - 6), canBack && !g_webHold)) {
            if (b.canBack) act.webNav = 1;
            else if (tabs.size() > 1) act.webTabClose = b.activeTab;
        }
        arrow(dl, ImVec2(w - 74, my), true, canBack ? ink : dim);
        if (iconButton("##wreload", ImVec2(w - 52, kStatusH + 6), ImVec2(w - 16, kStatusH + kHeaderH - 6), true)) act.webNav = b.loading ? 4 : 3;
        reloadIcon(dl, ImVec2(w - 34, my), ink);
        // home: the app's own start page again
        const char* homeUrl = (g_webGroup >= 100 && g_webGroup - 100 < APP_COUNT) ? kApps[g_webGroup - 100].url : nullptr;
        if (iconButton("##whome", ImVec2(w - 132, kStatusH + 6), ImVec2(w - 96, kStatusH + kHeaderH - 6), homeUrl != nullptr) && homeUrl) g_appHomeClosing = 1;
        if (g_appHomeClosing && homeUrl && b.status == 1 && !act.webTabClose) {
            if (tabs.size() > 1) act.webTabClose = tabs.back().id;   // (the app's own first tab stays)
            else { g_appHomeClosing = 0; if (!tabs.empty() && tabs.front().id != b.activeTab) act.webTabSelect = tabs.front().id; act.webNavigate = homeUrl; }
        }
        {   // a house
            const ImVec2 c(w - 114, my);
            const ImU32 col = homeUrl ? ink : dim;
            dl->AddTriangleFilled(ImVec2(c.x - 9, c.y - 1), ImVec2(c.x, c.y - 9), ImVec2(c.x + 9, c.y - 1), col);
            dl->AddRectFilled(ImVec2(c.x - 6, c.y - 2), ImVec2(c.x + 6, c.y + 8), col, 1.5f);
            dl->AddRectFilled(ImVec2(c.x - 2, c.y + 2), ImVec2(c.x + 2, c.y + 8), themeTone(16, 34, 66, 255));
        }
        zoomGroup("w", w - 266, kStatusH + 6, kStatusH + kHeaderH - 6);
    } else if (g_barsHidden) {   // folded away: a small tab at the top brings them back
        const ImVec2 a(w * 0.5f - 26, top), e(w * 0.5f + 26, top + 18);
        dl->AddRectFilled(a, e, themeTone(10, 24, 48, 235), 9.0f, ImDrawFlags_RoundCornersBottom);
        if (iconButton("##bexpand", a, e, true)) g_barsHidden = false;
        const ImVec2 c(w * 0.5f, top + 8);
        dl->AddLine(ImVec2(c.x - 7, c.y - 3), ImVec2(c.x, c.y + 3), kInk, 2.0f);
        dl->AddLine(ImVec2(c.x, c.y + 3), ImVec2(c.x + 7, c.y - 3), kInk, 2.0f);
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
        if (iconButton("##baddr", a, e, true)) {
            g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear();
            if (g_webGroup == 0 && tabs.empty() && b.status == 1 && room) { g_browserNoTabs = false; act.webTabNew = true; act.webTabGroup = 0; act.webTabUrl.clear(); }   // (no tab: as the +)
        }
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
        // (no keyboard button: the keys come up by themselves for a text field on the page, and go with Done or a tap elsewhere)
        volumeButton("##bvolb", ImVec2(w - 48, top + 4), ImVec2(w - 10, top + barH - 4));
        // bookmarks: only the user's own (the star adds and removes them); pressing one loads it
        float x = 14;
        const float by = top + barH + 3;
        struct Bm { std::string name, url; };
        std::vector<Bm> bms;
        if (g_settings.bookmarks.empty())
            dl->AddText(f.small, fontPx(f.small), ImVec2(x, std::floor(by + (bmH - 8 - fontPx(f.small)) * 0.5f)), dim, "No bookmarks yet: the star in the address bar adds this page");
        for (const auto& u : g_settings.bookmarks) bms.push_back({u.first.size() > 24 ? u.first.substr(0, 22) + "..." : u.first, u.second});
        {   // at the right: the History, and the arrow folding the bars away
            if (pill(dl, f, "##bhist", "History", ImVec2(w - 132, by), ImVec2(w - 46, by + bmH - 8), g_historyOpen, true)) { g_historyOpen = !g_historyOpen; g_historyScroll = 0; }
            if (iconButton("##bfold", ImVec2(w - 40, by), ImVec2(w - 8, by + bmH - 8), true)) { g_barsHidden = true; g_historyOpen = false; g_volumeOpen = false; }
            const ImVec2 c(w - 24, by + (bmH - 8) * 0.5f);
            dl->AddLine(ImVec2(c.x - 7, c.y + 3), ImVec2(c.x, c.y - 3), kInk, 2.0f);
            dl->AddLine(ImVec2(c.x, c.y - 3), ImVec2(c.x + 7, c.y + 3), kInk, 2.0f);
        }
        int idx = 0;
        for (const Bm& bm : bms) {
            const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, bm.name.c_str());
            const float bw = sz.x + 22;
            if (x + bw > w - 140) break;
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
    // the keys sit over the page (the page keeps its size: it is not laid out again for them); taps on them stay theirs
    {   // the last tab closed: its keys go too (the on-screen keyboard, or the physical keyboard's bar)
        static size_t hadTabs = 0;
        if (g_webGroup == 0 && tabs.empty() && hadTabs > 0 && g_kbd && (g_kbdTarget == 0 || g_kbdTarget == 1)) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; }
        if (g_webGroup == 0) hadTabs = tabs.size();
    }
    float kbH = 0;
    if (g_kbd) kbH = keyboardHeight();
    if (g_webGroup == 0 && tabs.empty() && g_browserNoTabs) {   // every tab closed: a big + to start a new one
        dl->AddRectFilled(ImVec2(0, pageTop + 1), ImVec2(w, h), themeTone(8, 16, 30, 255));
        const ImVec2 c(w * 0.5f, pageTop + (h - pageTop) * 0.44f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - 56, c.y - 56));
        const bool hit = ImGui::InvisibleButton("##newtabbig", ImVec2(112, 112)) && room && b.status == 1;
        const Ease e = eased("##newtabbig", false, ImGui::IsItemHovered(), ImGui::IsItemActive());
        const float r = 50.0f - 2.0f * e.down + 3.0f * e.hot;
        dl->AddCircleFilled(ImVec2(c.x + 2, c.y + 4), r, IM_COL32(0, 0, 0, 70), 0);
        dl->AddCircleFilled(c, r, mixCol(themeTone(22, 40, 70, 255), accentCol(255), 0.25f + 0.35f * e.hot), 0);
        dl->AddCircle(c, r, mixCol(IM_COL32(255, 255, 255, 60), accentLite(255), e.hot), 0, 1.6f);
        dl->AddLine(ImVec2(c.x - 18, c.y), ImVec2(c.x + 18, c.y), kInk, 3.5f);
        dl->AddLine(ImVec2(c.x, c.y - 18), ImVec2(c.x, c.y + 18), kInk, 3.5f);
        textCentered(dl, f.bold, c.x, c.y + 64, kInk, "New Tab");
        if (hit) { g_browserNoTabs = false; act.webTabNew = true; act.webTabGroup = 0; act.webTabUrl.clear(); g_kbd = true; g_kbdTarget = 1; g_addrEdit.clear(); }
    } else webView(dl, f, s, 0, pageTop + 1, w, h - pageTop - 1, act, false, (g_historyOpen || g_volumeOpen) ? 0.0f : h - pageTop - 1 - kbH);   // (a card open: the page waits)
    const float histT = openness("##histopen", g_chrome && g_historyOpen && !g_barsHidden);
    if (g_chrome && !g_barsHidden && histT > 0.01f) {   // the Browser's pages, newest first; a tap goes there; Clear History empties it (it drops in)
        const int histV0 = dl->VtxBuffer.Size;
        ImGui::BeginDisabled(!g_historyOpen);
        const ImVec2 a(w - 380, pageTop + 6), e(w - 10, std::min(h - 10 - kbH, pageTop + 340));
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##histbg", ImVec2(e.x - a.x, e.y - a.y));
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_historyScroll -= ImGui::GetIO().MouseWheel * 40;
        touchScroll(g_historyScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        {   // a tap on the page closes it (not the tap that opened it)
            static int frames = 0;
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool inPanel = mp.x >= a.x && mp.x <= e.x && mp.y >= a.y && mp.y <= e.y;
            if (++frames > 2 && ImGui::IsMouseClicked(0) && !inPanel && mp.y > pageTop) { g_historyOpen = false; frames = 0; }
        }
        dl->AddRectFilled(ImVec2(a.x + 2, a.y + 4), ImVec2(e.x + 2, e.y + 4), IM_COL32(0, 0, 0, 80), 12.0f);
        dl->AddRectFilled(a, e, themeTone(12, 22, 40, 248), 12.0f);
        dl->AddRect(a, e, IM_COL32(255, 255, 255, 46), 12.0f);
        textAt(dl, f.bold, a.x + 14, a.y + 12, kInk, "History");
        if (pill(dl, f, "##histclr", "Clear History", ImVec2(e.x - 176, a.y + 8), ImVec2(e.x - 46, a.y + 36), false, !g_settings.history.empty())) { g_settings.history.clear(); g_settingsChanged = true; }
        if (iconButton("##histx", ImVec2(e.x - 40, a.y + 8), ImVec2(e.x - 10, a.y + 36), true)) g_historyOpen = false;
        const ImVec2 xc(e.x - 25, a.y + 22);
        dl->AddLine(ImVec2(xc.x - 5, xc.y - 5), ImVec2(xc.x + 5, xc.y + 5), kInk, 1.6f);
        dl->AddLine(ImVec2(xc.x - 5, xc.y + 5), ImVec2(xc.x + 5, xc.y - 5), kInk, 1.6f);
        const float ly0 = a.y + 44, ly1 = e.y - 8, rowH = 40;
        const auto& hist = g_settings.history;
        const float content = hist.size() * rowH;
        g_historyScroll = std::max(0.0f, std::min(g_historyScroll, std::max(0.0f, content - (ly1 - ly0))));
        if (hist.empty()) textAt(dl, f.small, a.x + 14, ly0 + 6, kMuted, "No pages yet. The web apps' pages are not kept here.");
        dl->PushClipRect(ImVec2(a.x, ly0), ImVec2(e.x, ly1), true);
        for (size_t k = 0; k < hist.size(); ++k) {
            const auto& hp = hist[hist.size() - 1 - k];
            const float y = ly0 + k * rowH - g_historyScroll;
            if (y + rowH < ly0 || y > ly1) continue;
            char id[24];
            std::snprintf(id, sizeof id, "##hist%zu", k);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 6, y));
            if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x - 24, rowH - 2)) && y >= ly0 - 4) { g_navPending = hp.second; g_navForce = true; g_url = hp.second; g_historyOpen = false; }
            if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(a.x + 6, y), ImVec2(e.x - 18, y + rowH - 2), IM_COL32(255, 255, 255, (int)(18 * hk)), 7.0f);
            std::string t = hp.first, u = hp.second;
            while (t.size() > 4 && f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, t.c_str()).x > e.x - a.x - 40) { t.resize(t.size() - 4); t += "..."; }
            while (u.size() > 4 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, u.c_str()).x > e.x - a.x - 40) { u.resize(u.size() - 4); u += "..."; }
            textAt(dl, f.body, a.x + 14, y + 2, kInk, t.c_str());
            textAt(dl, f.small, a.x + 14, y + 21, kMuted, u.c_str());
        }
        dl->PopClipRect();
        if (content > ly1 - ly0) g_historyScroll = scrollBar(dl, "##histbar", e.x - 8, ly0, ly1, g_historyScroll, content, ly1 - ly0);
        ImGui::EndDisabled();
        popIn(dl, histV0, ImVec2(w - 90, pageTop), histT);
    }
    if (g_chrome && !g_barsHidden) volumePanel(ImVec2(w - 250, pageTop + 6));   // (under the bars: nothing of theirs under it)
    if (g_kbd) {
        const float ky = h - kbH;
        dl->AddRectFilled(ImVec2(0, ky - 6), ImVec2(w, ky), IM_COL32(0, 0, 0, 40));   // (a soft edge over the page)
        keyboard(dl, f, w, h - (5 * 34 + 4 * 5 + 16), act);   // (the bar puts itself at the bottom)
    }
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
        if (pill(dl, f, "##wxcond", "At The Aircraft", ImVec2(w - 14 - 2 * bw - 6, y0), ImVec2(w - 14 - bw - 6, y0 + 28), g_wxTab == 0, true)) g_wxTab = 0;
        if (pill(dl, f, "##wxmetar", "METAR Search", ImVec2(w - 14 - bw, y0), ImVec2(w - 14, y0 + 28), g_wxTab == 1, true)) g_wxTab = 1;
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
        if (!x.valid || x.clouds == 0) textAt(dl, f.body, colX + colW + 14, y0, kInk, x.valid ? "Sky Clear" : "No Data");
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
        if (!x.valid || x.aloft == 0) textAt(dl, f.body, a.x + 14, y, kMuted, "No Data");
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
        textCentered(dl, f.small, w * 0.5f, h - 24, kMuted, "X-Plane's own weather where you are. Airport reports: METAR Search, top right.");
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
// ---- Christmas: the user's aircraft is a sleigh drawn from above, pulled by four reindeer (our own simple drawing). c: its middle,
// len: its whole length on screen, ang: its heading on screen (0 up, clockwise, radians)
bool christmasTheme();
void drawSleigh(ImDrawList* dl, ImVec2 c, float len, float ang, int alpha = 255) {
    const float k = len / 108.0f, ca = std::cos(ang), sa = std::sin(ang);
    auto P = [&](float x, float y) { x *= k; y = (y + 6) * k; return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };   // y: forward is -y
    auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * alpha / 255) << 24); };
    auto ell = [&](float x, float y, float rx, float ry, ImU32 col) {
        ImVec2 p[20];
        for (int i = 0; i < 20; ++i) { const float t = i * 6.2832f / 20; p[i] = P(x + std::cos(t) * rx, y + std::sin(t) * ry); }
        dl->AddConvexPolyFilled(p, 20, A(col));
    };
    const float lw = std::max(1.0f, 1.6f * k);
    const ImU32 brown = IM_COL32(140, 92, 52, 255), dark = IM_COL32(96, 60, 34, 255), antler = IM_COL32(222, 200, 160, 255), gold = IM_COL32(236, 196, 92, 255);
    // the reins, from the sleigh's front to each pair
    for (int sd = -1; sd <= 1; sd += 2) dl->AddLine(P(sd * 9.0f, 8), P(sd * 7.0f, -52), A(gold), std::max(1.0f, k * 0.9f));
    // the reindeer: two pairs, each a body, a head with antlers
    for (int pr = 0; pr < 2; ++pr)
        for (int sd = -1; sd <= 1; sd += 2) {
            const float x = sd * 8.0f, y = -12.0f - pr * 26.0f;
            ell(x, y, 4.2f, 9.0f, brown);
            ell(x, y - 10.5f, 2.8f, 3.4f, dark);
            dl->AddLine(P(x - 1.2f, y - 12), P(x - 5.0f, y - 17.5f), A(antler), lw); dl->AddLine(P(x - 3.5f, y - 15.5f), P(x - 6.5f, y - 14.5f), A(antler), lw);
            dl->AddLine(P(x + 1.2f, y - 12), P(x + 5.0f, y - 17.5f), A(antler), lw); dl->AddLine(P(x + 3.5f, y - 15.5f), P(x + 6.5f, y - 14.5f), A(antler), lw);
        }
    // the sleigh: gold runners with curled fronts, a red body, a darker seat back, the sack of presents and its driver
    for (int sd = -1; sd <= 1; sd += 2) {
        dl->AddLine(P(sd * 15.5f, 6), P(sd * 15.5f, 48), A(gold), std::max(1.0f, 2.2f * k));
        dl->AddBezierCubic(P(sd * 15.5f, 6), P(sd * 15.5f, -2), P(sd * 9.0f, -2), P(sd * 10.0f, 4), A(gold), std::max(1.0f, 2.2f * k));
    }
    {
        const ImVec2 body[8] = {P(-13, 10), P(-8, 6), P(8, 6), P(13, 10), P(14, 44), P(10, 47), P(-10, 47), P(-14, 44)};
        dl->AddConvexPolyFilled(body, 8, A(IM_COL32(200, 30, 40, 255)));
        dl->AddPolyline(body, 8, A(gold), ImDrawFlags_Closed, std::max(1.0f, 1.4f * k));
    }
    ell(0, 40, 10.0f, 5.0f, IM_COL32(150, 18, 28, 255));   // the seat's back
    ell(0, 34, 8.0f, 7.5f, IM_COL32(136, 96, 60, 255));    // the sack
    dl->AddLine(P(-5, 31), P(5, 37), A(IM_COL32(110, 76, 46, 255)), lw);
    ell(0, 22, 7.5f, 6.0f, IM_COL32(214, 34, 44, 255));    // the driver: a red coat, its white fur collar, his hat (its bobble forward)
    ell(0, 17.5f, 5.5f, 2.2f, IM_COL32(250, 250, 250, 255));
    ell(0, 15, 3.4f, 3.4f, IM_COL32(200, 24, 36, 255));
    ell(0, 12.5f, 1.4f, 1.4f, IM_COL32(255, 255, 255, 255));
    for (int i = 0; i < 3; ++i) ell(-6.0f + i * 6.0f, 28.0f, 2.0f, 2.0f, i == 1 ? IM_COL32(70, 170, 90, 255) : IM_COL32(240, 200, 80, 255));   // presents
}

// ---- Halloween: the user's aircraft is a witch on her broom, from above (our own simple drawing): the broom along the heading,
// its bristles fanned at the back, her cloak spread behind her, her hat's wide brim and point, and her cat riding at the back
void drawWitch(ImDrawList* dl, ImVec2 c, float len, float ang, int alpha = 255) {
    const float k = len / 100.0f, ca = std::cos(ang), sa = std::sin(ang);
    auto P = [&](float x, float y) { x *= k; y *= k; return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };   // y: forward is -y
    auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * alpha / 255) << 24); };
    auto ell = [&](float x, float y, float rx, float ry, ImU32 col) {
        ImVec2 p[20];
        for (int i = 0; i < 20; ++i) { const float t = i * 6.2832f / 20; p[i] = P(x + std::cos(t) * rx, y + std::sin(t) * ry); }
        dl->AddConvexPolyFilled(p, 20, A(col));
    };
    // the bristles: a fan of straw at the tail, then its binding
    for (int i = -5; i <= 5; ++i) dl->AddLine(P(i * 0.6f, 30), P(i * 2.4f, 50), A(IM_COL32(214, 170, 90, 255)), std::max(1.0f, 1.6f * k));
    dl->AddRectFilled(P(-3, 28), P(3, 31), A(IM_COL32(120, 60, 40, 255)));
    {   // the binding's band, as a little quad along the stick
        const ImVec2 q[4] = {P(-3.2f, 27.5f), P(3.2f, 27.5f), P(3.2f, 31.5f), P(-3.2f, 31.5f)};
        dl->AddConvexPolyFilled(q, 4, A(IM_COL32(150, 40, 60, 255)));
    }
    dl->AddLine(P(0, -48), P(0, 30), A(IM_COL32(110, 72, 40, 255)), std::max(1.5f, 2.6f * k));   // the stick
    // her cloak, spread out behind her in the wind
    {
        const ImVec2 cl[6] = {P(0, -6), P(10, 2), P(15, 20), P(0, 16), P(-15, 20), P(-10, 2)};
        dl->AddConvexPolyFilled(cl, 6, A(IM_COL32(62, 30, 92, 255)));
    }
    ell(0, -4, 7.0f, 8.0f, IM_COL32(44, 22, 64, 255));     // her shoulders
    for (int sd = -1; sd <= 1; sd += 2) dl->AddLine(P(sd * 5.5f, -6), P(sd * 1.5f, -18), A(IM_COL32(44, 22, 64, 255)), std::max(1.0f, 3.0f * k));   // her arms, to the stick
    ell(0, -16.5f, 2.2f, 2.2f, IM_COL32(120, 190, 100, 255));   // her hands on the stick
    ell(0, -10, 11.0f, 11.0f, IM_COL32(70, 40, 96, 255));   // her hat's brim, from above
    ell(0, -10, 5.0f, 5.0f, IM_COL32(52, 28, 70, 255));     // its cone
    ell(0, -10, 1.6f, 1.6f, IM_COL32(16, 8, 22, 255));      // its point
    dl->AddCircle(P(0, -10), 7.2f * k, A(IM_COL32(240, 150, 40, 255)), 20, std::max(1.0f, 1.4f * k));   // the hat's band
    // the cat at the back of the broom, its tail curling out
    ell(0, 22, 3.6f, 4.4f, IM_COL32(18, 14, 20, 255));
    ell(0, 17.5f, 2.6f, 2.6f, IM_COL32(18, 14, 20, 255));
    dl->AddBezierCubic(P(0, 26), P(6, 30), P(8, 24), P(5, 20), A(IM_COL32(18, 14, 20, 255)), std::max(1.0f, 1.4f * k));
    ell(-1.0f, 16.6f, 0.6f, 0.6f, IM_COL32(250, 220, 80, 255)); ell(1.0f, 16.6f, 0.6f, 0.6f, IM_COL32(250, 220, 80, 255));
}
// ---- Thanksgiving: the user's aircraft is a turkey, from above (our own simple drawing): its tail fanned wide behind it in bands
// of brown, orange and cream, its wings at its sides, its head forward with the red wattle and the beak
void drawTurkey(ImDrawList* dl, ImVec2 c, float len, float ang, int alpha = 255) {
    const float k = len / 100.0f, ca = std::cos(ang), sa = std::sin(ang);
    auto P = [&](float x, float y) { x *= k; y *= k; return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };   // y: forward is -y
    auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * alpha / 255) << 24); };
    auto ell = [&](float x, float y, float rx, float ry, ImU32 col) {
        ImVec2 p[20];
        for (int i = 0; i < 20; ++i) { const float t = i * 6.2832f / 20; p[i] = P(x + std::cos(t) * rx, y + std::sin(t) * ry); }
        dl->AddConvexPolyFilled(p, 20, A(col));
    };
    // the tail: eleven feathers fanned out from behind the body, each in three bands (cream tip, orange, dark brown root)
    const float fx = 0, fy = 10;
    for (int i = -5; i <= 5; ++i) {
        const float a0 = 1.5708f + (i - 0.46f) * 0.27f, a1 = 1.5708f + (i + 0.46f) * 0.27f;   // (around straight back: +y)
        auto at = [&](float a, float r) { return P(fx + std::cos(a) * r, fy + std::sin(a) * r); };
        static const float rr[4] = {8, 24, 33, 39};
        static const ImU32 band[3] = {IM_COL32(96, 56, 30, 255), IM_COL32(214, 120, 44, 255), IM_COL32(244, 226, 190, 255)};
        for (int b = 0; b < 3; ++b) { const ImVec2 q[4] = {at(a0, rr[b]), at(a1, rr[b]), at(a1, rr[b + 1]), at(a0, rr[b + 1])}; dl->AddConvexPolyFilled(q, 4, A(i % 2 ? band[b] : mixCol(band[b], IM_COL32(60, 34, 18, 255), 0.18f))); }
    }
    for (int sd = -1; sd <= 1; sd += 2) ell(sd * 11.0f, 3, 6.0f, 12.5f, IM_COL32(104, 62, 34, 255));   // its wings
    for (int sd = -1; sd <= 1; sd += 2) for (int j = 0; j < 3; ++j) dl->AddLine(P(sd * (9.0f + j * 1.8f), -2 + j * 2.0f), P(sd * (10.5f + j * 1.8f), 12), A(IM_COL32(70, 40, 22, 255)), std::max(1.0f, 1.1f * k));
    ell(0, 2, 12.0f, 17.0f, IM_COL32(134, 82, 44, 255));    // its body
    ell(0, -2, 8.0f, 10.0f, IM_COL32(150, 96, 54, 255));    // (the lighter breast)
    ell(0, -17, 4.2f, 6.0f, IM_COL32(120, 74, 42, 255));    // its neck
    ell(0, -24, 4.6f, 5.0f, IM_COL32(86, 120, 170, 255));   // its head (a tom's is blue and red)
    { const ImVec2 bk[3] = {P(-2.2f, -27.5f), P(2.2f, -27.5f), P(0, -34)}; dl->AddConvexPolyFilled(bk, 3, A(IM_COL32(236, 170, 60, 255))); }   // the beak
    ell(1.9f, -28.5f, 1.5f, 3.0f, IM_COL32(210, 44, 48, 255));   // the wattle, hanging beside it
    ell(-2.0f, -25, 0.8f, 0.8f, IM_COL32(20, 16, 14, 255)); ell(2.0f, -25, 0.8f, 0.8f, IM_COL32(20, 16, 14, 255));   // its eyes
}

// ---- Independence Day: the user's aircraft is a firework rocket, from above (our own simple drawing): a red cone, a tube in red,
// white and blue with a star, its stick trailing, and the fuse's flame and sparks behind it
void drawFirework(ImDrawList* dl, ImVec2 c, float len, float ang, int alpha = 255) {
    const float k = len / 100.0f, ca = std::cos(ang), sa = std::sin(ang);
    auto P = [&](float x, float y) { x *= k; y *= k; return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };   // y: forward is -y
    auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 255) * alpha / 255) << 24); };
    auto quad = [&](float x0, float y0, float x1, float y1, ImU32 col) { const ImVec2 q[4] = {P(x0, y0), P(x1, y0), P(x1, y1), P(x0, y1)}; dl->AddConvexPolyFilled(q, 4, A(col)); };
    dl->AddLine(P(3.5f, 4), P(3.5f, 50), A(IM_COL32(196, 160, 110, 255)), std::max(1.5f, 2.4f * k));   // the stick, tied to its side
    // the flame at the tail and its sparks
    { const ImVec2 fl[3] = {P(-5, 12), P(5, 12), P(0, 34)}; dl->AddConvexPolyFilled(fl, 3, A(IM_COL32(255, 150, 40, 255))); }
    { const ImVec2 fl[3] = {P(-2.8f, 12), P(2.8f, 12), P(0, 25)}; dl->AddConvexPolyFilled(fl, 3, A(IM_COL32(255, 236, 130, 255))); }
    static const float sp[7][3] = {{-6, 30, 1.5f}, {5.5f, 34, 1.3f}, {-2, 40, 1.6f}, {7, 43, 1.0f}, {-7.5f, 45, 1.1f}, {1, 48, 1.3f}, {-4, 36, 0.9f}};
    for (const auto& q : sp) dl->AddCircleFilled(P(q[0], q[1]), std::max(0.8f, q[2] * k), A(IM_COL32(255, 220, 110, 255)), 8);
    // the tube: bands of red, white and blue
    quad(-6.5f, -30, 6.5f, 12, IM_COL32(244, 244, 248, 255));
    quad(-6.5f, -30, 6.5f, -18, IM_COL32(36, 64, 150, 255));
    quad(-6.5f, -10, 6.5f, -3, IM_COL32(206, 44, 54, 255));
    quad(-6.5f, 4, 6.5f, 12, IM_COL32(206, 44, 54, 255));
    {   // a white star on the blue
        ImVec2 st[10];
        for (int i = 0; i < 10; ++i) { const float a = -1.5708f + i * 0.62832f, r = i % 2 ? 1.7f : 4.2f; st[i] = P(std::cos(a) * r, -24 + std::sin(a) * r); }
        for (int i = 0; i < 10; ++i) { const ImVec2 tri[3] = {P(0, -24), st[i], st[(i + 1) % 10]}; dl->AddConvexPolyFilled(tri, 3, A(IM_COL32(255, 255, 255, 255))); }
    }
    { const ImVec2 cone[3] = {P(-7.5f, -30), P(7.5f, -30), P(0, -49)}; dl->AddConvexPolyFilled(cone, 3, A(IM_COL32(206, 44, 54, 255))); }   // its cone
    dl->AddLine(P(-6.5f, -30), P(6.5f, -30), A(IM_COL32(250, 214, 110, 255)), std::max(1.0f, 1.4f * k));   // a gold band under it
}

// the seasonal stand-ins for the user's aircraft: 1 Christmas's sleigh, 2 Halloween's witch, 3 Thanksgiving's turkey, 4 Independence
// Day's firework, 0 none (the aircraft itself)
int specialCraft();
void drawCraft(ImDrawList* dl, ImVec2 c, float len, float ang, int alpha = 255) {
    const int sc = specialCraft();
    if (sc == 2) drawWitch(dl, c, len, ang, alpha); else if (sc == 3) drawTurkey(dl, c, len, ang, alpha); else if (sc == 4) drawFirework(dl, c, len, ang, alpha); else drawSleigh(dl, c, len, ang, alpha);
}

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
    const std::string key = !p.divertKey.empty() ? p.divertKey : !p.ofpId.empty() ? p.ofpId : p.origin + ">" + p.dest + "@" + p.importedAt;   // (a diversion: the original's)
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
bool drawCardBody(ImDrawList* dl, const Fonts& f, const std::vector<CardLine>& L, ImVec2 at, ImVec2 r0, ImVec2 r1);
// (a new card grows out of what was tapped: faded and grown in over a fifth of a second)
bool drawCard(ImDrawList* dl, const Fonts& f, const std::vector<CardLine>& L, ImVec2 at, ImVec2 r0, ImVec2 r1) {
    static std::string shown; static double t0 = -10;
    const std::string key = L.empty() ? std::string() : L[0].a + "|" + L[0].b;
    if (key != shown) { shown = key; t0 = ImGui::GetTime(); }
    const int v0 = dl->VtxBuffer.Size;
    const bool hit = drawCardBody(dl, f, L, at, r0, r1);
    popIn(dl, v0, at, std::min(1.0f, (float)(ImGui::GetTime() - t0) / 0.2f));
    return hit;
}
bool drawCardBody(ImDrawList* dl, const Fonts& f, const std::vector<CardLine>& L, ImVec2 at, ImVec2 r0, ImVec2 r1) {
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
        } else if (!g_touchScrolling) g_cardScroll -= ImGui::GetIO().MouseDelta.y;
    }
    if (!(ImGui::IsItemActive() && g_cardMoving)) touchScroll(g_cardScroll, ImVec2(a.x, a.y + 40), e);   // (begun on its rows too)
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
            if (!l.b.empty()) { std::string sub = l.b; while (sub.size() > 4 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, sub.c_str()).x > inner - 10) { sub.resize(sub.size() - 4); sub += "..."; } textAt(dl, f.small, x0, y + 24, kMuted, sub.c_str()); }
            y += 44;
            break;
        case 1: {
            textAt(dl, f.small, x0, y + 1, kMuted, l.a.c_str());
            std::string v = l.b;
            while (v.size() > 4 && f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, v.c_str()).x > inner - 96) { v.resize(v.size() - 4); v += "..."; }
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
    // how dark the street map is: Dark 1, Light 0; Auto with the daylight (0 with the sun up, 1 once it is 8 degrees below, as auto dim)
    const float night = g_settings.mapDarkAuto ? std::max(0.0f, std::min(1.0f, (1.0f - s.autoDimLevel) / 0.55f)) : g_settings.mapDark ? 1.0f : 0.0f;
    const bool darkMap = night >= 0.5f;
    const bool sat = satImg || darkMap;   // a dark map under everything: light ink, as on the imagery
    dl->PushClipRect(r0, r1, true);
    dl->AddRectFilled(r0, r1, noMap ? mixCol(IM_COL32(250, 250, 250, 255), IM_COL32(8, 10, 14, 255), night) : satImg ? IM_COL32(18, 24, 30, 255) : mixCol(IM_COL32(232, 228, 220, 255), IM_COL32(18, 24, 30, 255), night));

    // ---- input: drag to move, the wheel to zoom (about the pointer); bubbles drawn later sit on top of it
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##map", ImVec2(r1.x - r0.x, r1.y - r0.y));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) g_mapPressPos = io.MousePos;
    const bool mapTap = ImGui::IsItemDeactivated() && std::fabs(io.MousePos.x - g_mapPressPos.x) + std::fabs(io.MousePos.y - g_mapPressPos.y) < 6;
    // by finger, a single tap is often the start of a drag or a pinch: a card opens (or closes) on a double tap there; one tap
    // still closes an open menu. With a mouse one click does it all.
    bool mapClick = mapTap;
    if (mapTap && g_touchInput && !g_mapMenu) {
        static double lastTapT = -10; static ImVec2 lastTapP(-1000, -1000);
        const double now = ImGui::GetTime();
        const bool second = now - lastTapT < 0.40 && std::fabs(io.MousePos.x - lastTapP.x) + std::fabs(io.MousePos.y - lastTapP.y) < 30;
        mapClick = second;
        lastTapT = second ? -10 : now; lastTapP = io.MousePos;
    }
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
    // the street map's two sets: the light one, and the dark one over it as the night comes (both while it fades)
    int srcs[2] = {satImg ? 1 : night < 1.0f ? 0 : 2, (!satImg && night > 0.0f && night < 1.0f) ? 2 : -1};
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
    for (int layer = 0; layer < 2; ++layer) {
    const int src = srcs[layer];
    if (src < 0) break;
    const ImU32 tint = layer == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, (int)std::lround(255 * night));
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
        if (tex) dl->AddImageQuad(tex, p00, p10, p11, p01, u0, ImVec2(u1.x, u0.y), u1, ImVec2(u0.x, u1.y), tint);
        else if (layer == 0) {
            ++missing;
            if (!m.tile) {   // the off-line test: a quiet checkerboard where the tiles would be
                const ImU32 c = ((t.x + t.y) & 1) ? (sat ? IM_COL32(40, 52, 44, 255) : IM_COL32(222, 218, 208, 255)) : (sat ? IM_COL32(34, 46, 38, 255) : IM_COL32(236, 232, 224, 255));
                dl->AddQuadFilled(p00, p10, p11, p01, c);
            }
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
                        if (!g_settings.arrivalGate.empty() && s.plan.loaded && g.icao == s.plan.dest) {   // the flight's arrival gate: green, named
                            std::string nm = st.name;
                            for (char& c : nm) c = (char)std::toupper((unsigned char)c);
                            if (nm == g_settings.arrivalGate) {
                                dl->AddCircleFilled(q, 9.0f, IM_COL32(60, 220, 110, 70), 0);
                                dl->AddCircle(q, 9.0f, IM_COL32(60, 220, 110, 255), 0, 2.4f);
                                g_labels.push_back({6, q, 10, "Gate " + st.name, "", IM_COL32(255, 255, 255, 255), 0, IM_COL32(30, 140, 70, 235)});
                            }
                        }
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
        // VATSIM's controllers online: an approach's 30 nm ring, and a badge of letters by the airport (D G T A, I for its ATIS)
        if (g_settings.mapVatsimAtc && g_mapZoom >= 6.0 && !s.vatsimAtc.empty()) {
            for (const AptPoint& p : m.airportPoints) {
                const std::vector<VatsimController> atc = atcFor(s, p.icao);
                if (atc.empty()) continue;
                const ImVec2 q = toS(mercOf(p.lat, p.lon));
                if (q.x < r0.x - 400 || q.x > r1.x + 400 || q.y < r0.y - 400 || q.y > r1.y + 400) continue;
                std::string letters;
                bool app = false;
                for (const VatsimController& c : atc) {
                    const char L = c.facility == 2 ? 'D' : c.facility == 3 ? 'G' : c.facility == 4 ? 'T' : c.facility == 5 ? 'A' : c.facility == 7 ? 'I' : 0;
                    if (L && letters.find(L) == std::string::npos) letters += L;
                    app = app || c.facility == 5;
                }
                if (app) {
                    const ImVec2 e = toS(mercOf(p.lat + 0.5, p.lon));   // 30 nm north
                    const float rr = std::fabs(q.y - e.y);
                    dl->AddCircleFilled(q, rr, IM_COL32(70, 160, 255, 26), 64);
                    dl->AddCircle(q, rr, IM_COL32(90, 170, 255, 170), 64, 1.5f);
                }
                if (q.x < r0.x || q.x > r1.x || q.y < r0.y || q.y > r1.y) continue;
                const float bw = letters.size() * 11.0f + 8;
                const ImVec2 ba(q.x + 8, q.y - 20), bb(ba.x + bw, ba.y + 16);
                dl->AddRectFilled(ba, bb, IM_COL32(10, 20, 36, 225), 5.0f);
                for (size_t i = 0; i < letters.size(); ++i) {
                    const char L = letters[i];
                    const ImU32 col = L == 'D' ? IM_COL32(120, 200, 255, 255) : L == 'G' ? IM_COL32(110, 220, 130, 255) : L == 'T' ? IM_COL32(255, 110, 110, 255) : L == 'A' ? IM_COL32(110, 160, 255, 255) : IM_COL32(255, 210, 100, 255);
                    const char t[2] = {L, 0};
                    textAt(dl, f.bold, ba.x + 4 + i * 11.0f, ba.y + 1, col, t);
                }
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
    // ---- the plan's procedures (SID green, STAR amber, approach magenta), under the route: a thin line, its fixes, its name
    if (g_settings.mapProcedures && !m.procedures.empty()) {
        static const ImU32 pc[3] = {IM_COL32(70, 205, 140, 255), IM_COL32(255, 170, 60, 255), IM_COL32(230, 100, 210, 255)};
        for (const MapState::Procedure& pr : m.procedures) {
            if (pr.pts.size() < 2) continue;
            const ImU32 col = pc[std::max(0, std::min(2, pr.kind))];
            std::vector<ImVec2> pts;
            for (const MapState::RoutePoint& rp : pr.pts) pts.push_back(toS(mercOf(rp.lat, rp.lon)));
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(10, 10, 20, 110), 0, 4.0f);
            dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 2.0f);
            for (size_t i = 0; i < pts.size(); ++i) {
                const ImVec2 q = pts[i];
                if (q.x < r0.x - 20 || q.x > r1.x + 20 || q.y < r0.y - 20 || q.y > r1.y + 20) continue;
                dl->AddCircleFilled(q, 2.6f, col, 0);
                if (g_mapZoom >= 9.5) g_labels.push_back({4, q, 5, pr.pts[i].ident, "", IM_COL32(255, 255, 255, 230), 0, (col & 0x00FFFFFF) | 0xC8000000});
            }
            if (g_mapZoom >= 7.0) {   // its name, by its middle fix
                const ImVec2 q = pts[pts.size() / 2];
                if (q.x > r0.x && q.x < r1.x && q.y > r0.y && q.y < r1.y) g_labels.push_back({4, ImVec2(q.x, q.y - 14), 6, pr.name, "", IM_COL32(20, 20, 30, 255), 0, col});
            }
        }
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
    // VATSIM's airspaces: every outline faintly; green with a controller online (its code on it); the one tapped in the accent
    if (g_settings.mapAirspaces && m.airspaces) {
        const auto online = airspacesOnline(s);
        double vla0 = 90, vlo0 = 180, vla1 = -90, vlo1 = -180;
        for (const ImVec2& c : {r0, ImVec2(r1.x, r0.y), r1, ImVec2(r0.x, r1.y)}) { double la, lo; latLonOf(fromS(c), la, lo); vla0 = std::min(vla0, la); vla1 = std::max(vla1, la); vlo0 = std::min(vlo0, lo); vlo1 = std::max(vlo1, lo); }
        const bool wrapView = vlo1 - vlo0 > 300;   // (the whole world, or across the date line: no culling by longitude)
        std::vector<ImVec2> pts;
        for (int pass = 0; pass < 2; ++pass)   // the quiet ones first, the online and chosen ones over them
            for (const AirspaceArea& ar : m.airspaces->areas) {
                const bool on = online.count(ar.id) > 0, picked = g_selKind == 7 && g_selKey == ar.id;
                if ((on || picked) != (pass == 1)) continue;
                if (ar.la1 < vla0 || ar.la0 > vla1 || (!wrapView && (ar.lo1 < vlo0 || ar.lo0 > vlo1))) continue;
                const ImU32 col = picked ? accentLite(255) : on ? IM_COL32(80, 214, 124, 235) : sat ? IM_COL32(255, 255, 255, 60) : IM_COL32(50, 70, 110, 80);
                const float th = picked ? 2.6f : on ? 2.0f : 1.0f;
                for (const auto& ring : ar.rings) {
                    pts.clear();
                    float lastLon = ring.empty() ? 0 : ring[0].second;
                    for (const auto& q : ring) {
                        if (std::fabs(q.second - lastLon) > 180) { if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, th); pts.clear(); }   // (across the date line)
                        lastLon = q.second;
                        const ImVec2 sp = toS(mercOf(q.first, q.second));
                        if (!pts.empty() && std::fabs(sp.x - pts.back().x) + std::fabs(sp.y - pts.back().y) < 1.5f) continue;
                        pts.push_back(sp);
                    }
                    if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, th);
                }
                if ((on || picked) && g_mapZoom >= 3.0) {   // its code, on a small dark label
                    const ImVec2 lp = toS(mercOf(ar.labelLat, ar.labelLon));
                    const ImVec2 ts = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, ar.id.c_str());
                    dl->AddRectFilled(ImVec2(lp.x - ts.x * 0.5f - 5, lp.y - ts.y * 0.5f - 2), ImVec2(lp.x + ts.x * 0.5f + 5, lp.y + ts.y * 0.5f + 2), IM_COL32(10, 30, 20, 210), 4.0f);
                    textAt(dl, f.small, std::floor(lp.x - ts.x * 0.5f), std::floor(lp.y - ts.y * 0.5f), picked ? accentLite(255) : IM_COL32(120, 235, 150, 255), ar.id.c_str());
                }
            }
    }
    if (g_settings.mapTraffic) {
        if (g_settings.mapTrafficTrails && g_selKind == 1)   // the selected aircraft's trail only, under every aircraft: thin, white over a dark edge
            for (const MapState::Traffic& t : m.traffic) {
                if (t.callsign != g_selKey) continue;
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
        if (specialCraft()) drawCraft(dl, q, std::max(symLen * 1.7f, sh ? sl * pxPerM : 0.0f), (float)a);   // (Christmas's sleigh, Halloween's witch: not the aircraft)
        else if (sh) {
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
                            {"##mrte", "Flight Plan Route", &g_settings.mapRoute, !m.route.empty()}, {"##mproc", "SID, STAR And Approach", &g_settings.mapProcedures, !m.procedures.empty()}, {"##mring", "Range Rings", &g_settings.mapRings, true}};
        const Item tfc[] = {{"##mtraf", "Show Traffic", &g_settings.mapTraffic, true}, {"##mttrl", "Trail When Selected", &g_settings.mapTrafficTrails, g_settings.mapTraffic},
                            {"##mtrte", "Filed Route When Selected", &g_settings.mapTrafficRoute, g_settings.mapTraffic}};
        const Item fea[] = {{"##mtrail", "My Breadcrumbs", &g_settings.mapTrail, true}, {"##mout", "Coastlines And Borders", &g_settings.mapOutlines, true},
                            {"##mtz", "Time Zones", &g_settings.mapTimeZones, true}, {"##mday", "Daylight", &g_settings.mapDaylight, true},
                            {"##mvatc", "VATSIM ATC Online", &g_settings.mapVatsimAtc, true}, {"##mvasp", "VATSIM Airspaces", &g_settings.mapAirspaces, true}};
        const char* cats[3] = {"Navigation", "Traffic", "Features"};
        float cw = 0;
        for (const char* c : cats) cw = std::max(cw, bubbleW(f, nullptr, c));
        for (int i = 0; i < 3; ++i) {
            char id[16];
            std::snprintf(id, sizeof id, "##mcat%d", i);
            if (mapBubble(dl, f, id, ImVec2(r0.x + 10, r0.y + 46 + i * 36), nullptr, cats[i], g_mapMenu == i + 1, cw)) { g_mapMenu = g_mapMenu == i + 1 ? 0 : i + 1; if (g_mapMenu) { g_selKind = 0; g_selKey.clear(); } }   // (a menu and a card: one at a time)
        }
        static int menuShown = 0;   // the menu drawn (it stays while it shrinks away after closing)
        if (g_mapMenu) menuShown = g_mapMenu;
        const float menuT = openness("##mapmenuopen", g_mapMenu != 0 && g_mapMenu == menuShown);
        if (menuT < 0.01f && !g_mapMenu) menuShown = 0;
        if (menuShown && menuT > 0.01f) {   // the open menu: All / Clear, then its rows (growing out of its bubble)
            const int menuV0 = dl->VtxBuffer.Size;
            ImGui::BeginDisabled(!g_mapMenu);
            const int g_mapMenuNow = menuShown;
            const Item* items = g_mapMenuNow == 1 ? nav : g_mapMenuNow == 2 ? tfc : fea;
            const int n = g_mapMenuNow == 1 ? (int)(sizeof nav / sizeof nav[0]) : g_mapMenuNow == 2 ? (int)(sizeof tfc / sizeof tfc[0]) : (int)(sizeof fea / sizeof fea[0]);
            const float pw = 262, extra = g_mapMenuNow == 2 ? 3 * 30.0f + 6 : 0.0f;
            const float ph = 46 + n * 30 + extra + 8;
            const float px = r0.x + 10 + cw + 8;
            const float py = std::max(r0.y + 46, std::min(r0.y + 46 + (g_mapMenuNow - 1) * 36.0f, r1.y - 50 - ph));
            const ImVec2 pa(px, py), pb(px + pw, py + ph);
            ImGui::SetCursorScreenPos(pa);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##mmenubg", ImVec2(pw, ph));   // (taps on the menu stay off the map)
            dl->AddRectFilled(ImVec2(pa.x + 2, pa.y + 4), ImVec2(pb.x + 2, pb.y + 4), IM_COL32(0, 0, 0, 70), 12.0f);
            dl->AddRectFilled(pa, pb, themeTone(12, 22, 40, 242), 12.0f);
            dl->AddRect(pa, pb, IM_COL32(255, 255, 255, 46), 12.0f, 0, 1.0f);
            textAt(dl, f.bold, pa.x + 12, pa.y + 12, kInk, cats[g_mapMenuNow - 1]);
            if (pill(dl, f, "##mmall", "All", ImVec2(pb.x - 118, pa.y + 8), ImVec2(pb.x - 66, pa.y + 34), false, true))
                for (int i = 0; i < n; ++i) if (items[i].enabled || items[i].v == &g_settings.mapRoute) { *items[i].v = true; g_settingsChanged = true; }
            if (pill(dl, f, "##mmclr", "Clear", ImVec2(pb.x - 62, pa.y + 8), ImVec2(pb.x - 8, pa.y + 34), false, true))
                for (int i = 0; i < n; ++i) { *items[i].v = false; g_settingsChanged = true; }
            float ry = pa.y + 42;
            for (int i = 0; i < n; ++i, ry += 30)
                if (menuCheck(dl, f, items[i].id, ImVec2(pa.x + 4, ry), pw - 8, items[i].name, *items[i].v, items[i].enabled)) { *items[i].v = !*items[i].v; g_settingsChanged = true; }
            if (g_mapMenuNow == 2) {   // where the traffic comes from, and how far and how high it is shown
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
            ImGui::EndDisabled();
            popIn(dl, menuV0, ImVec2(r0.x + 10 + cw, r0.y + 46 + (g_mapMenuNow - 1) * 36.0f + 15), menuT);
        }
        // where the map comes from, and whether the airports are still being found
        const char* attr = noMap ? "" : satImg ? "Imagery: Esri, Maxar, Earthstar Geographics" : "\xC2\xA9 OpenStreetMap contributors";
        if (*attr) {
            const ImVec2 as = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, attr);
            dl->AddRectFilled(ImVec2(r1.x - as.x - 12, r1.y - as.y - 6), r1, IM_COL32(255, 255, 255, 190), 4.0f, ImDrawFlags_RoundCornersTopLeft);
            dl->AddText(f.small, fontPx(f.small), ImVec2(r1.x - as.x - 6, r1.y - as.y - 3), IM_COL32(30, 36, 48, 255), attr);
        }
        if (g_settings.mapAirports && m.aptIndexing) textAt(dl, f.small, r0.x + 12, y - 22, sat ? kInk : IM_COL32(30, 36, 48, 255), "Finding X-Plane's airports...");
        else if (g_settings.mapAirspaces && !m.airspaces && !m.airspaceStatus.empty()) textAt(dl, f.small, r0.x + 12, y - 22, sat ? kInk : IM_COL32(30, 36, 48, 255), m.airspaceStatus.c_str());
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
            if (aKey.empty() && g_settings.mapVatsimAtc && g_mapZoom >= 6.0 && !s.vatsimAtc.empty())   // its VATSIM ATC badge (beside the dot) opens it too
                for (const AptPoint& a : m.airportPoints) {
                    const ImVec2 q = toS(mercOf(a.lat, a.lon));
                    if (p.x < q.x - 4 || p.x > q.x + 70 || p.y < q.y - 26 || p.y > q.y + 4) continue;
                    const std::vector<VatsimController> atc = atcFor(s, a.icao);
                    if (atc.empty()) continue;
                    std::string letters;
                    for (const VatsimController& c : atc) { const char L = c.facility == 2 ? 'D' : c.facility == 3 ? 'G' : c.facility == 4 ? 'T' : c.facility == 5 ? 'A' : c.facility == 7 ? 'I' : 0; if (L && letters.find(L) == std::string::npos) letters += L; }
                    if (p.x <= q.x + 8 + letters.size() * 11.0f + 8 + 4) { aKey = a.icao; break; }
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
        if (kind == 0 && g_settings.mapAirspaces && m.airspaces) {   // nothing else there: the airspace it is in
            double la, lo;
            latLonOf(fromS(p), la, lo);
            const auto online = airspacesOnline(s);
            double bestArea = 1e18;
            for (const AirspaceArea& ar : m.airspaces->areas) {
                if (!insideRings(ar, la, lo)) continue;
                const double area = (ar.la1 - ar.la0) * (ar.lo1 - ar.lo0) * (online.count(ar.id) ? 0.01 : 1.0);
                if (area < bestArea) { bestArea = area; kind = 7; key = ar.id; g_airspaceTapLat = la; g_airspaceTapLon = lo; }
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
                std::snprintf(b, sizeof b, "%.0f kt", t->gsKt); L.push_back({1, "Ground Speed", b});
                std::snprintf(b, sizeof b, "%03d\xC2\xB0T", ((int)std::lround(t->hdg) % 360 + 360) % 360); L.push_back({1, "Heading", b});
                if (!t->onGround) { std::snprintf(b, sizeof b, "%+.0f fpm", std::round(t->vsFpm / 50) * 50); L.push_back({1, "Vertical Speed", b}); }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, t->lat, t->lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From You", b}); }
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
                        L.push_back({1, "Flight Plan", v.rules + "  " + v.departure + " \xE2\x86\x92 " + v.arrival + (v.alternate.empty() ? "" : "  (alt " + v.alternate + ")")});
                        L.push_back({1, "Aircraft", v.aircraftFull.empty() ? v.aircraft : v.aircraftFull});
                        L.push_back({1, "Cruise", v.cruiseAlt + (v.cruiseTas.empty() ? "" : "  at " + v.cruiseTas + " kt")});
                        auto hhmm = [](const std::string& x) { return x.size() == 4 ? x.substr(0, 2) + ":" + x.substr(2) : x; };
                        if (!v.depTime.empty()) L.push_back({1, "Departure", hhmm(v.depTime) + "Z" + (v.enroute.empty() ? "" : ", en route " + hhmm(v.enroute))});
                        if (!v.fuel.empty()) L.push_back({1, "Fuel On Board", hhmm(v.fuel)});
                        if (!v.route.empty()) L.push_back({2, v.route, "route"});
                        if (!v.remarks.empty()) L.push_back({2, v.remarks.size() > 220 ? v.remarks.substr(0, 218) + "..." : v.remarks, "Remarks"});
                    } else L.push_back({2, "No flight plan filed", ""});
                }
            }
        } else if (g_selKind == 2) {
            for (const MapState::Navaid& n : m.navaids) {
                if (n.ident + "|" + std::to_string(n.type) != g_selKey) continue;
                at = toS(mercOf(n.lat, n.lon));
                const char* kindName = n.type == 2 ? "NDB" : n.type == 3 ? "VOR" : n.type == 4 ? "ILS Localizer" : n.type == 5 ? "Localizer" : "DME";
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
                    if (n.gsAngle > 0) { std::snprintf(b, sizeof b, "%.2f\xC2\xB0", n.gsAngle); L.push_back({1, "Glide Slope", b}); }
                    else L.push_back({1, "Glide Slope", "none (localizer only)"});
                    L.push_back({1, "DME", n.dme ? "yes" : "no"});
                }
                if (n.type == 3 && n.bearing != 0) { std::snprintf(b, sizeof b, "%.0f\xC2\xB0%s", std::fabs(n.bearing), n.bearing < 0 ? "W" : "E"); L.push_back({1, "Variation", b}); }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, n.lat, n.lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From You", b}); }
                L.push_back({1, "Position", posText(n.lat, n.lon)});
                break;
            }
        } else if (g_selKind == 3) {
            for (const MapState::Fix& fx : m.fixes) {
                if (fx.ident != g_selKey) continue;
                at = toS(mercOf(fx.lat, fx.lon));
                L.push_back({0, fx.ident, fx.enroute ? "Enroute Fix" : "Terminal Fix"});
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, fx.lat, fx.lon, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From You", b}); }
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
                    if (!lg.totalTime.empty()) L.push_back({1, "Time From Departure", lg.totalTime});
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
                if (!st.size.empty()) L.push_back({1, "Size", "ICAO Class " + st.size});
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
        } else if (g_selKind == 7 && m.airspaces) {   // an airspace: its name, and who is controlling it (frequency, name, message)
            at = toS(mercOf(g_airspaceTapLat, g_airspaceTapLon));
            const AirspaceArea* ar = nullptr;
            for (const AirspaceArea& x : m.airspaces->areas) if (x.id == g_selKey) ar = &x;
            std::string name;
            for (const auto& fr : m.airspaces->firs) if (fr.boundary == g_selKey) { name = fr.name; break; }
            L.push_back({0, name.empty() ? g_selKey : name, g_selKey + (ar && ar->oceanic ? "  \xC2\xB7  oceanic" : "  \xC2\xB7  FIR")});
            const auto online = airspacesOnline(s);
            const auto it = online.find(g_selKey);
            if (it == online.end()) {
                L.push_back({2, s.vatsimAtcHave ? "No controller online: UNICOM 122.800" : "Asking VATSIM who is online...", ""});
            } else {
                for (const VatsimController& c : it->second) {
                    L.push_back({4, c.callsign, ""});
                    L.push_back({1, "Frequency", c.freq});
                    if (!c.name.empty()) L.push_back({1, "Controller", c.name});
                    if (!c.logon.empty()) L.push_back({1, "Online Since", c.logon});
                    if (!c.text.empty()) L.push_back({2, c.text.size() > 300 ? c.text.substr(0, 298) + "..." : c.text, "Message"});
                }
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
                if (g_settings.mapVatsimAtc && s.vatsimAtcHave) {   // VATSIM's controllers there (the badge's letters, in full)
                    const std::vector<VatsimController> atc = atcFor(s, g_selKey);
                    L.push_back({4, "VATSIM ATC", ""});
                    if (atc.empty()) L.push_back({2, "No controller online: UNICOM 122.800", ""});
                    for (const VatsimController& c : atc) L.push_back({1, c.facility == 7 ? c.callsign + " (ATIS)" : c.callsign, c.freq});
                }
                if (m.havePos) { double brg = 0; const double d = distNmLL(m.lat, m.lon, la, lo, &brg); std::snprintf(b, sizeof b, "%.1f nm, %03.0f\xC2\xB0T", d, brg); L.push_back({1, "From You", b}); }
                L.push_back({5, "Open In Airports", ""});
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
const char* const kCraftLabels[6] = {"Clearance Limit", "Route", "Altitude", "Frequency", "Transponder", "Remarks"};
const char* const kAtisLabels[9] = {"Information", "Time", "Wind", "Visibility", "Clouds", "Temp / Dew", "Altimeter", "Runways / Approach", "Remarks"};
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
    "# Cockpit Preparation\nBatteries | ON\nExternal power / APU | ON\nFlight plan | LOADED\nFuel quantity | CHECKED\nAltimeters | SET\nTakeoff data | SET\n"
    "# Before Start\nDoors | CLOSED\nSeat belt signs | ON\nParking brake | SET\nBeacon | ON\nTransponder | SET\nTakeoff briefing | COMPLETE\n"
    "# After Start\nGenerators | ON\nAPU bleed | OFF\nAnti-ice | AS RQRD\nFlaps | SET\nFlight controls | CHECKED\nGround equipment | CLEAR\n"
    "# Before Takeoff\nCabin | READY\nFlaps | SET\nTrim | SET\nTransponder | TA/RA\nLanding lights | ON\nRunway | CONFIRMED\n"
    "# After Takeoff\nGear | UP\nFlaps | UP\nAutopilot | AS RQRD\nAltimeters | STD at transition\n"
    "# Approach\nArrival briefing | COMPLETE\nAltimeters | SET\nMinimums | SET\nSeat belt signs | ON\nAuto brake | SET\n"
    "# Landing\nGear | DOWN\nFlaps | LANDING\nSpeed brakes | ARMED\nLanding clearance | RECEIVED\n"
    "# After Landing\nSpeed brakes | RETRACTED\nFlaps | UP\nTransponder | STBY\nLanding lights | OFF\nAPU | START\n"
    "# Parking\nParking brake | SET\nEngines | OFF\nSeat belt signs | OFF\nBeacon | OFF\nFuel pumps | OFF\n";
const char* const kLightChecklist =
    "# Preflight\nFuel quantity | CHECKED\nOil | CHECKED\nControl locks | REMOVED\nWeather and NOTAMs | CHECKED\n"
    "# Before Start\nSeats and belts | SECURE\nFuel selector | BOTH\nCircuit breakers | IN\nAvionics | OFF\nBeacon | ON\n"
    "# Engine Start\nMixture | RICH\nThrottle | 1/4 IN OPEN\nPropeller area | CLEAR\nMagnetos | START\nOil pressure | CHECKED\n"
    "# Before Takeoff\nParking brake | SET\nFlight controls | FREE AND CORRECT\nInstruments | SET\nRun-up | COMPLETE\nFlaps | SET\nTrim | TAKEOFF\nTransponder | ALT\nLights | ON\n"
    "# Cruise\nPower | SET\nMixture | LEANED\nFuel | MONITORED\n"
    "# Before Landing\nATIS / altimeter | SET\nFuel selector | BOTH\nMixture | RICH\nCarb heat | AS RQRD\nFlaps | AS RQRD\n"
    "# After Landing\nFlaps | UP\nCarb heat | COLD\nTransponder | STBY\nLights | AS RQRD\n"
    "# Shutdown\nAvionics | OFF\nMixture | IDLE CUTOFF\nMagnetos | OFF\nMaster | OFF\nParking brake | SET\n";

// the turboprops' normal list, and the abnormal and emergency procedures (general ones; the Airbus set for the Airbus types).
// Simulation use only, not for real-world flight.
const char* const kTurbopropChecklist =
    "# Before Start\nDoors | CLOSED\nParking brake | SET\nBatteries | ON\nFuel quantity | CHECKED\nPropellers | FEATHER / AREA CLEAR\nBeacon | ON\n"
    "# Engine Start\nCondition levers | FUEL OFF\nStart | ENGAGE\nIgnition / fuel at 12-15% NH | ON\nITT | MONITORED\nGenerators | ON\n"
    "# Before Taxi\nCondition levers | MIN RPM / TAXI\nAnti-ice | AS RQRD\nFlaps | SET\nFlight controls | CHECKED\nTakeoff briefing | COMPLETE\n"
    "# Before Takeoff\nPropellers | MAX RPM\nAutofeather | ARMED\nTrim | SET\nTransponder | ALT\nLights | ON\n"
    "# After Takeoff\nGear | UP\nFlaps | UP\nPower | CLIMB\nAutofeather | OFF\n"
    "# Cruise\nPower | CRUISE\nPropellers | CRUISE RPM\nFuel | MONITORED\nIce protection | AS RQRD\n"
    "# Approach\nBriefing | COMPLETE\nAltimeters | SET\nPropellers | MAX RPM (by the threshold)\nFlaps | APPROACH\n"
    "# Landing\nGear | DOWN, 3 GREEN\nFlaps | LANDING\nLanding clearance | RECEIVED\n"
    "# Shutdown\nParking brake | SET\nCondition levers | FUEL OFF / FEATHER\nGenerators | OFF\nBeacon | OFF\nBatteries | OFF\n";
const char* const kAirbusAbnormal =
    "# Icing Conditions\nIcing: OAT/TAT at or below 10\xC2\xB0""C with visible moisture | CHECK\nENG ANTI ICE | ON\nWING ANTI ICE | AS RQRD (not on the ground before takeoff)\nPROBE / WINDOW HEAT | AUTO (ON when needed)\nIce on the wipers or airframe | MONITOR\n"
    "# Ground De-Icing / Anti-Icing\nHoldover time and fluid type | NOTED\nFLAPS / SLATS | RETRACTED\nSPEED BRAKES | RETRACTED\nPACK 1 + 2 | OFF\nAPU BLEED | OFF\nENG BLEED | AS RQRD (OFF if engines running and asked)\nDITCHING pb | ON (outflow valve and vents shut)\nDoors and windows | CLOSED\nAfter spraying: wait 1 minute | DONE\nDITCHING pb | OFF\nPACKS / APU BLEED | ON\nFlight controls | CHECKED\nHoldover time | MONITORED\n"
    "# Engine Anti-Ice Fault\nAffected ENG ANTI ICE | OFF THEN ON\nIcing conditions | AVOID\nThrust | AS RQRD\n"
    "# Gear Not Up / Gear Disagree\nGear lever | DOWN THEN UP\nSpeed | BELOW VLE\nGear | LEAVE DOWN if no retraction\nFuel burn (gear down) | CONSIDER\n"
    "# Gear Gravity Extension\nSpeed | BELOW VLE\nGear lever | DOWN\nGravity extension handle | TURN 3 TIMES CLOCKWISE\nGear indications | 3 GREEN\nLanding distance | CHECKED\n"
    "# Hydraulic System Low Pressure\nAffected ENG PUMP | OFF\nPTU | AS RQRD\nLanding configuration | PLAN (longer distance)\nApproach | STABLE, EARLY\n"
    "# Generator Fault\nAffected GEN | OFF THEN ON\nIf not restored: APU GEN | ON\nGalley | AS RQRD\n"
    "# Bleed Fault\nAffected ENG BLEED | OFF\nX BLEED | AS RQRD\nWing anti-ice | AVOID icing\nAltitude | FL 230 MAX if one bleed\n"
    "# Fuel Imbalance\nFuel used and quantities | CHECKED (leak?)\nCROSS FEED | ON\nPumps of the light side | OFF\nWhen balanced: CROSS FEED | OFF, PUMPS ON\n"
    "# Unreliable Airspeed\nAutopilot / FD / A/THR | OFF\nPitch and thrust | AS RQRD (memory items)\nProbe / window heat | ON\nCompare speeds (ADR 1, 2, 3) | CHECK\nFaulty ADR | OFF\n"
    "# Overweight Landing\nLanding configuration | FULL (as allowed)\nApproach speed | VLS\nAuto brake | AS RQRD\nTouchdown | GENTLE (less than 360 ft/min)\nInspection after landing | REQUEST\n"
    "# Go-Around\nThrust levers | TOGA\nRotation | 15\xC2\xB0 nose up\nFlaps | ONE STEP UP\nPositive climb: gear | UP\nSRS / GA TRK | CHECKED\nAutopilot | AS RQRD\n";
const char* const kAirbusEmergency =
    "# Engine Fire\nThrust lever (affected) | IDLE\nENG MASTER (affected) | OFF\nENG FIRE pb | PUSH\nAGENT 1 (after 10 s) | DISCH\nATC | NOTIFY (MAYDAY)\nIf fire after 30 s: AGENT 2 | DISCH\nLand | AT THE NEAREST AIRPORT\n"
    "# Engine Failure / Severe Damage\nThrust lever (affected) | IDLE\nENG MASTER (affected) | OFF\nENG FIRE pb (damage) | PUSH\nAGENT 1 | DISCH\nSingle-engine procedures | APPLY\nATC | NOTIFY\n"
    "# Engine Failure After V1\nRudder | CONTROL\nV2 to V2+10 | FLY\nPositive climb: gear | UP\nAt acceleration altitude: speed | GREEN DOT\nECAM actions | COMPLETE\n"
    "# Emergency Descent\nCrew oxygen masks | ON\nSigns | ON\nEmergency descent | INITIATE\nThrust levers | IDLE\nSpeed brakes | FULL\nSpeed | MAX / APPROPRIATE\nATC | NOTIFY\nSquawk | 7700\nPax oxygen masks | ON (if cabin above 14000 ft)\n"
    "# Smoke / Fumes\nCrew oxygen masks | ON, 100%\nCrew communication | ESTABLISH\nGalley | OFF\nCabin fans | OFF\nSmoke source | IDENTIFY, ISOLATE\nDiversion | CONSIDER\n"
    "# All Engines Failure\nSpeed | OPTIMUM (green dot)\nEmergency electrical power | CHECKED (RAT)\nATC | NOTIFY (MAYDAY), 7700\nEngine relight | ATTEMPT\nAPU | START (below FL 250)\nForced landing / ditching | PREPARE\n"
    "# Stall Recovery\nNose down pitch | APPLY\nBank | WINGS LEVEL\nThrust | AS RQRD\nSpeed brakes | RETRACTED\nWhen out of the stall | RECOVER SMOOTHLY\n"
    "# Evacuation\nAircraft / parking brake | STOP / ON\nATC | NOTIFY\nCabin crew (PA) | AT STATIONS\nENG MASTERS | OFF\nFire pbs (all) | PUSH\nAgents | AS RQRD\nEvacuation | INITIATE\n";
const char* const kAirlinerAbnormal =
    "# Icing Conditions\nEngine anti-ice | ON\nWing anti-ice | AS RQRD\nProbe heat | ON\nIce on the airframe | MONITOR\n"
    "# Ground De-Icing / Anti-Icing\nHoldover time and fluid type | NOTED\nFlaps | UP\nPacks / APU bleed | OFF\nAfter spraying: packs and bleeds | ON\nFlight controls | CHECKED\nHoldover time | MONITORED\n"
    "# Gear Will Not Retract\nGear lever | DOWN\nSpeed | BELOW GEAR LIMIT\nFuel burn (gear down) | CONSIDER\n"
    "# Manual Gear Extension\nSpeed | BELOW GEAR LIMIT\nManual extension | PERFORM\nGear | 3 GREEN\n"
    "# Hydraulic Failure\nAffected pump | OFF\nLanding distance | CHECKED\nApproach | EARLY, STABLE\n"
    "# Generator Failure\nAffected generator | RESET\nAPU generator | ON\nLoad shedding | AS RQRD\n"
    "# Unreliable Airspeed\nAutopilot / autothrottle | OFF\nPitch and thrust | AS RQRD\nProbe heat | ON\nSpeeds | COMPARE\n"
    "# Go-Around\nThrust | TOGA\nPitch | 15\xC2\xB0 nose up\nFlaps | ONE STEP UP\nPositive climb: gear | UP\n";
const char* const kAirlinerEmergency =
    "# Engine Fire\nThrust lever (affected) | IDLE\nFuel control (affected) | CUTOFF\nFire handle | PULL\nAgent | DISCHARGE\nATC | MAYDAY\nLand | NEAREST AIRPORT\n"
    "# Engine Failure\nThrust lever (affected) | IDLE\nFuel control (affected) | CUTOFF\nSingle-engine procedures | APPLY\nATC | NOTIFY\n"
    "# Emergency Descent\nOxygen masks | ON\nThrust | IDLE\nSpeed brakes | EXTENDED\nTransponder | 7700\nATC | NOTIFY\n"
    "# Smoke / Fumes\nOxygen masks | ON, 100%\nSource | IDENTIFY, ISOLATE\nDiversion | CONSIDER\n"
    "# Evacuation\nParking brake | SET\nEngines | SHUT DOWN\nFire handles | PULL\nATC | NOTIFY\nEvacuation | INITIATE\n";
const char* const kTurbopropAbnormal =
    "# Icing Conditions\nEngine intake anti-ice | ON\nPropeller de-ice | ON\nAirframe de-ice boots | AS RQRD\nMinimum icing speed | OBSERVE\n"
    "# Ground De-Icing / Anti-Icing\nHoldover time and fluid type | NOTED\nFlaps | UP\nBleeds | OFF\nAfter spraying: flight controls | CHECKED\n"
    "# Generator Failure\nAffected generator | RESET\nLoads | SHED AS RQRD\n"
    "# Propeller Overspeed\nPower lever | REDUCE\nCondition lever | REDUCE RPM\nIf it persists: feather | CONSIDER\n";
const char* const kTurbopropEmergency =
    "# Engine Fire\nPower lever (affected) | IDLE\nCondition lever (affected) | FUEL OFF / FEATHER\nFire handle | PULL\nAgent | DISCHARGE\nATC | MAYDAY\n"
    "# Engine Failure After Takeoff\nDirectional control | MAINTAIN\nAutofeather | CHECKED (else feather)\nSpeed | V2\nPositive climb: gear | UP\nCondition lever (affected) | FUEL OFF\n"
    "# Emergency Descent\nOxygen masks | ON\nPower levers | IDLE\nSpeed | VMO\nTransponder | 7700\n";
const char* const kLightAbnormal =
    "# Carburetor Icing\nCarb heat | ON (HOT)\nThrottle | FULL\nMixture | ADJUST for smooth running\nAfter the ice clears: carb heat | AS RQRD\n"
    "# Low Oil Pressure\nOil temperature | CHECK\nPower | REDUCE\nLand | AS SOON AS PRACTICAL\n"
    "# Alternator Failure\nAmmeter | CHECK\nMaster (ALT) | OFF THEN ON\nElectrical load | REDUCE\nLand | AS SOON AS PRACTICAL\n"
    "# Rough Running Engine\nCarb heat | ON\nMixture | ADJUST\nFuel selector | BOTH\nMagnetos | CHECK L, R, BOTH\n";
const char* const kLightEmergency =
    "# Engine Failure After Takeoff\nAirspeed | BEST GLIDE\nMixture | IDLE CUTOFF\nFuel selector | OFF\nMagnetos | OFF\nFlaps | AS RQRD\nMaster | OFF\nLand | STRAIGHT AHEAD\n"
    "# Engine Failure In Flight\nAirspeed | BEST GLIDE\nLanding site | CHOOSE\nCarb heat | ON\nFuel selector | BOTH\nMixture | RICH\nMagnetos | BOTH (or START)\nIf no restart: forced landing | PREPARE\n"
    "# Engine Fire In Flight\nMixture | IDLE CUTOFF\nFuel selector | OFF\nMaster | OFF\nCabin heat and air | OFF\nForced landing | EXECUTE\n"
    "# Electrical Fire\nMaster | OFF\nAvionics | OFF\nVents | CLOSED\nFire extinguisher | ACTIVATE\nLand | AS SOON AS POSSIBLE\n"
    "# Forced Landing\nAirspeed | BEST GLIDE\nATC | MAYDAY, 7700\nSeats and belts | SECURE\nFlaps | AS RQRD\nDoors | UNLATCHED\nMaster | OFF before touchdown\n";

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

int g_clSource = -1;    // -1 automatic: the aircraft's own file, else by its engines (or size); 0 own, 1 airliner, 2 light aircraft, 3 turboprop
int g_clKind = 0;       // 0 normal, 1 abnormal, 2 emergency
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
    // automatic: the aircraft's own file, else by its engines (jets: airliner; turboprops; pistons and electric: light), else its size
    const int et = s.engineType;
    const int byEngine = (et == 4 || et == 5) ? 1 : (et == 2 || et == 8) ? 3 : (et == 0 || et == 1 || et == 3) ? 2 : (len >= 24 ? 1 : 2);
    if (src < 0 || (src == 0 && !own)) src = own ? 0 : byEngine;
    const char* names[4] = {"This Aircraft", "Airliner", "Light Aircraft", "Turboprop"};
    float x = 12;
    for (int i = 0; i < 4; ++i) {
        if (i == 0 && !own) continue;
        char id[16];
        std::snprintf(id, sizeof id, "##cls%d", i);
        const float bw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, names[i]).x + 32;
        if (pill(dl, f, id, names[i], ImVec2(x, top), ImVec2(x + bw, top + 30), src == i, true)) g_clSource = i;
        x += bw + 6;
    }
    {   // which procedures: normal, abnormal, emergency (the emergency one in red)
        static const char* kinds[3] = {"Normal", "Abnormal", "Emergency"};
        float kx = w - 12;
        for (int k = 2; k >= 0; --k) {
            const float bw = f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, kinds[k]).x + 28;
            kx -= bw;
            char id[16]; std::snprintf(id, sizeof id, "##clk%d", k);
            if (pill(dl, f, id, kinds[k], ImVec2(kx, top), ImVec2(kx + bw, top + 30), g_clKind == k, true)) g_clKind = k;
            if (k == 2 && g_clKind == 2) dl->AddRect(ImVec2(kx, top), ImVec2(kx + bw, top + 30), IM_COL32(255, 96, 96, 255), 15.0f, 0, 2.0f);
            kx -= 6;
        }
    }
    // the aircraft's own file may hold abnormal and emergency sections too ("# Abnormal: ...", "# Emergency: ..."); else the general ones
    const bool airbus = s.acfIcao.size() == 4 && s.acfIcao[0] == 'A' && (s.acfIcao[1] == '3' || s.acfIcao[1] == '2') && len >= 24;
    std::vector<CheckSection> list;
    {
        const std::vector<CheckSection> all = parseChecklist(src == 0 ? s.checklistText : src == 1 ? kAirlinerChecklist : src == 3 ? kTurbopropChecklist : kLightChecklist);
        auto kindOf = [](const std::string& t) { return t.compare(0, 9, "Abnormal:") == 0 ? 1 : t.compare(0, 10, "Emergency:") == 0 ? 2 : 0; };
        bool ownKind = false;
        for (const CheckSection& c : all) if (kindOf(c.title) == g_clKind) {
            CheckSection c2 = c;
            if (g_clKind) { const size_t colon = c2.title.find(':'); c2.title = c2.title.substr(colon + 1); while (!c2.title.empty() && c2.title[0] == ' ') c2.title.erase(0, 1); }
            list.push_back(c2); ownKind = true;
        }
        if (g_clKind > 0 && !ownKind) {
            const int fam = (src == 0 || src == 1) ? (airbus ? 0 : 1) : src == 3 ? 2 : 3;   // Airbus, airliner, turboprop, light
            const char* const ab[4] = {kAirbusAbnormal, kAirlinerAbnormal, kTurbopropAbnormal, kLightAbnormal};
            const char* const em[4] = {kAirbusEmergency, kAirlinerEmergency, kTurbopropEmergency, kLightEmergency};
            list = parseChecklist(g_clKind == 1 ? ab[fam] : em[fam]);
        }
    }
    {
        const char* note = own ? s.checklistSource.c_str() : "Your own: Output/XPTablet/Checklists/<ICAO>.txt";
        if (g_clKind > 0) note = "General procedures, for the simulator only";
        textAt(dl, f.small, 12, top + 34, kMuted, note);
    }
    if (list.empty()) return;
    const std::string key = std::to_string(src) + "|" + std::to_string(g_clKind) + "|" + s.acfIcao;
    if (key != g_clKey || g_clDone.size() != list.size()) {
        g_clKey = key; g_clSection = 0;
        g_clDone.assign(list.size(), {});
        for (size_t i = 0; i < list.size(); ++i) g_clDone[i].assign(list[i].items.size(), false);
    }
    g_clSection = std::max(0, std::min((int)list.size() - 1, g_clSection));
    // the sections, with how far each has got
    const float y0 = top + 56, leftW = 230;
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
        else if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + leftW, y + rh - 2), IM_COL32(255, 255, 255, (int)(18 * hk)), 8.0f);
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
        if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(rx + 10, y), ImVec2(rx + rw - 10, y + ih - 2), IM_COL32(255, 255, 255, (int)(14 * hk)), 8.0f);
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
    if (pill(dl, f, "##clall", "Reset All", ImVec2(rx + 124, h - 56), ImVec2(rx + 244, h - 24), false, true)) for (auto& d : g_clDone) std::fill(d.begin(), d.end(), false);
    if (g_clSection + 1 < (int)list.size() && pill(dl, f, "##clnext", "Next Section", ImVec2(rx + rw - 156, h - 56), ImVec2(rx + rw - 16, h - 24), all, true)) ++g_clSection;
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

// a headset: tapped, its frequency goes into COM1's standby (lit a moment after)
bool headsetButton(ImDrawList* dl, const char* id, ImVec2 c, double mhz, Actions& act) {
    static std::string litId; static double litAt = -10;
    const ImVec2 a(c.x - 13, c.y - 12), b(c.x + 13, c.y + 12);
    const bool hit = iconButton(id, a, b, mhz > 0);
    if (hit) { act.com1Standby = mhz; litId = id; litAt = ImGui::GetTime(); }
    const bool lit = litId == id && ImGui::GetTime() - litAt < 1.5;
    const ImU32 col = lit ? kGood : IM_COL32(200, 214, 238, 255);
    dl->PathArcTo(ImVec2(c.x, c.y + 1), 7.5f, 3.14159f, 2 * 3.14159f, 12); dl->PathStroke(col, 0, 1.8f);
    dl->AddRectFilled(ImVec2(c.x - 9, c.y), ImVec2(c.x - 5, c.y + 7), col, 1.5f);
    dl->AddRectFilled(ImVec2(c.x + 5, c.y), ImVec2(c.x + 9, c.y + 7), col, 1.5f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lit ? "Set in COM1 standby" : "Tune COM1 standby");
    return hit;
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
        else if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(18, y), ImVec2(6 + lw, y + 40), IM_COL32(255, 255, 255, (int)(18 * hk)), 8.0f);
        textAt(dl, f.bold, 28, y + 3, kInk, p.icao.c_str());
        if (s.map.havePos) {
            double brg = 0;
            const double d = distNmLL(s.map.lat, s.map.lon, p.lat, p.lon, &brg);
            char b[32];
            std::snprintf(b, sizeof b, "%.0f nm %03.0f\xC2\xB0", d, brg);
            textRight(dl, f.small, 2 + lw, y + 5, kMuted, b);
        }
        std::string nm = p.name;
        while (nm.size() > 4 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, nm.c_str()).x > lw - 40) { nm.resize(nm.size() - 4); nm += "..."; }
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
    touchScroll(g_aptScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    static float aptMax = 1e9f;   // (last frame's end: the list is never drawn past it)
    g_aptScroll = std::max(0.0f, std::min(g_aptScroll, aptMax));
    dl->PushClipRect(ImVec2(ra.x + 2, ra.y + 2), ImVec2(rb.x - 2, rb.y - 2), true);
    const float x0 = rx + 18, x1 = rx + rw - 18;
    float y = top + 14 - g_aptScroll;
    // the name and place
    textAt(dl, f.title, x0, y, kInk, ap.icao.c_str());
    const float iw = f.title->CalcTextSizeA(fontPx(f.title), 1e9f, 0, ap.icao.c_str()).x;
    if (!ap.iata.empty()) textAt(dl, f.small, x0 + iw + 8, y + 8, kMuted, ap.iata.c_str());
    {
        ImGui::SetCursorScreenPos(ImVec2(x1 - 120, y));
        if (pill(dl, f, "##aptmap", "Show On Map", ImVec2(x1 - 120, y), ImVec2(x1, y + 28), false, true)) {
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
        dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 112, a.y + 9), kMuted, ap.ctaf.note.c_str(), nullptr, b.x - a.x - 150);
        headsetButton(dl, "##hsctaf", ImVec2(b.x - 20, (a.y + b.y) * 0.5f), ap.ctaf.mhz, act);
    }
    // the METAR beside it
    if (s.metar.icao == ap.icao && !s.metar.raw.empty()) dl->AddText(f.mono, fontPx(f.mono), ImVec2(x0 + 314, y + 2), kInk, s.metar.raw.c_str(), nullptr, x1 - x0 - 314);
    else textAt(dl, f.small, x0 + 314, y + 4, kMuted, s.metar.icao == ap.icao && s.metar.busy ? "METAR: asking NOAA..." : "No METAR");
    y += 64;
    // VATSIM: who is online here (their frequencies), else UNICOM 122.800
    {
        textAt(dl, f.small, x0, y, kMuted, "VATSIM");
        y += 20;
        const std::vector<VatsimController> atc = atcFor(s, ap.icao);
        int k = 0;
        for (const VatsimController& c : atc) {
            textAt(dl, f.body, x0, y, c.facility == 7 ? IM_COL32(120, 200, 255, 255) : kGood, c.callsign.c_str());
            textAt(dl, f.mono, x0 + 150, y + 1, kInk, c.freq.c_str());
            char id[24]; std::snprintf(id, sizeof id, "##hsv%d", k++);
            headsetButton(dl, id, ImVec2(x0 + 226, y + 9), std::atof(c.freq.c_str()), act);
            if (c.facility == 7 && !c.text.empty()) {   // the ATIS: its text, wrapped
                const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, x1 - x0 - 250, c.text.c_str());
                dl->AddText(f.small, fontPx(f.small), ImVec2(x0 + 250, y + 2), kMuted, c.text.c_str(), nullptr, x1 - x0 - 250);
                y += std::max(22.0f, sz.y + 6);
            } else {
                textAt(dl, f.small, x0 + 250, y + 2, kMuted, c.name.c_str());
                y += 22;
            }
        }
        if (atc.empty()) {
            textAt(dl, f.body, x0, y, kInk, "UNICOM");
            textAt(dl, f.mono, x0 + 150, y + 1, kInk, "122.800");
            headsetButton(dl, "##hsunicom", ImVec2(x0 + 226, y + 9), 122.8, act);
            textAt(dl, f.small, x0 + 250, y + 2, kMuted, s.vatsimAtcHave ? "No ATC online here: VATSIM's CTAF" : "VATSIM's CTAF (checking who is online...)");
            y += 22;
        }
        y += 10;
    }
    // radio
    textAt(dl, f.small, x0, y, kMuted, "RADIO");
    y += 20;
    int fqi = 0;
    for (const AirportInfo::Freq& fq : ap.freqs) {
        textAt(dl, f.body, x0, y, kInk, fq.what.c_str());
        char b[16];
        std::snprintf(b, sizeof b, "%.3f", fq.mhz);
        textAt(dl, f.mono, x0 + 150, y + 1, kInk, b);
        char id[24]; std::snprintf(id, sizeof id, "##hsf%d", fqi++);
        headsetButton(dl, id, ImVec2(x0 + 226, y + 9), fq.mhz, act);
        if (!fq.note.empty()) {
            std::string n = fq.note;
            while (n.size() > 4 && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, n.c_str()).x > x1 - x0 - 250) { n.resize(n.size() - 4); n += "..."; }
            textAt(dl, f.small, x0 + 250, y + 2, kMuted, n.c_str());
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
    aptMax = std::max(0.0f, contentH - (rb.y - ra.y) + 30);
    g_aptScroll = std::min(g_aptScroll, aptMax);
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
float g_docListScroll = 0;
std::string g_docPdf;      // the PDF shown in the Documents page ("" : a picture, or none)

// ---- PDFs: one way for every page that shows one (Documents, Dispatch) -----------------------------------------------------------------
// The PDF's zoom: - and + are Chromium's PDF viewer's own (Ctrl and the wheel, at the middle of the view), Fit lays it out again
// fitted to the width. (The page zoom does not reach the PDF viewer's pages.)
void pdfZoomControls(ImDrawList* dl, const Fonts& f, const SimState& s, Actions& act, const char* tag, float x0, float y0, float y1, ImVec2 viewMid, bool enabled) {
    char id[24];
    auto mag = [&](ImVec2 a, ImVec2 e, int dir) {
        const ImVec2 c((a.x + e.x) * 0.5f - 1, (a.y + e.y) * 0.5f - 1);
        const ImU32 col = enabled ? IM_COL32(210, 222, 242, 255) : IM_COL32(100, 118, 148, 255);
        dl->AddCircle(c, 6.5f, col, 0, 1.6f);
        dl->AddLine(ImVec2(c.x + 4.8f, c.y + 4.8f), ImVec2(c.x + 9, c.y + 9), col, 2.2f);
        dl->AddLine(ImVec2(c.x - 3, c.y), ImVec2(c.x + 3, c.y), col, 1.5f);
        if (dir > 0) dl->AddLine(ImVec2(c.x, c.y - 3), ImVec2(c.x, c.y + 3), col, 1.5f);
    };
    std::snprintf(id, sizeof id, "##%spzout", tag);
    if (iconButton(id, ImVec2(x0, y0), ImVec2(x0 + 36, y1), enabled)) act.web.push_back({11, viewMid.x, viewMid.y, -100, 0, 1});
    mag(ImVec2(x0, y0), ImVec2(x0 + 36, y1), -1);
    std::snprintf(id, sizeof id, "##%spzfit", tag);
    if (pill(dl, f, id, "Fit", ImVec2(x0 + 40, y0 + 1), ImVec2(x0 + 86, y1 - 1), false, enabled)) act.webNav = 3;   // (laid out again: fitted)
    std::snprintf(id, sizeof id, "##%spzin", tag);
    if (iconButton(id, ImVec2(x0 + 90, y0), ImVec2(x0 + 126, y1), enabled)) act.web.push_back({11, viewMid.x, viewMid.y, 100, 0, 1});
    mag(ImVec2(x0 + 90, y0), ImVec2(x0 + 126, y1), 1);
    (void)s;
}
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
std::string g_docFilter;   // one folder ("XPTablet Docs", "AviTab Charts"), or "" for all

void documentsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const DocsState& d = s.docs;
    if (!d.listed) act.docsList = true;
    const float top = kStatusH + kHeaderH + 8, lw = 290;
    if (iconButton("##docref", ImVec2(12, top), ImVec2(48, top + 30), true)) act.docsList = true;   // look again
    reloadIcon(dl, ImVec2(30, top + 15), kInk);
    {   // which folders: all of them, XPTablet's docs folder, or AviTab's charts
        struct Flt { const char* id; const char* name; const char* folder; float x0, x1; };
        const Flt flts[] = {{"##docall", "All", "", 54, 104}, {"##docxpt", "XPTablet", "XPTablet Docs", 108, 198}, {"##docavi", "AviTab", "AviTab Charts", 202, 302}};
        for (const Flt& fl : flts)
            if (pill(dl, f, fl.id, fl.name, ImVec2(fl.x0, top), ImVec2(fl.x1, top + 30), g_docFilter == fl.folder, true) && g_docFilter != fl.folder) { g_docFilter = fl.folder; g_docListScroll = 0; }
        if (!g_docFilter.empty() && g_docFilter != "XPTablet Docs" && g_docFilter != "AviTab Charts") g_docFilter.clear();
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
    touchScroll(g_docListScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
        if (n == 0 && !d.files.empty()) textAt(dl, f.small, la.x + 14, la.y + 14, kMuted, g_docFilter == "AviTab Charts" ? "No charts in AviTab's charts folder." : "No documents in XPTablet's docs folder.");
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
        else if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(la.x + 6, y), ImVec2(rowR, y + 34), IM_COL32(255, 255, 255, (int)(18 * hk)), 8.0f);
        // a small page: red for a PDF, blue for a picture
        const ImVec2 ic(la.x + 22, y + 17);
        dl->AddRectFilled(ImVec2(ic.x - 7, ic.y - 10), ImVec2(ic.x + 7, ic.y + 10), e.pdf ? IM_COL32(220, 70, 70, 255) : IM_COL32(70, 150, 230, 255), 2.0f);
        textCentered(dl, f.small, ic.x, ic.y - fontPx(f.small) * 0.5f, kInk, e.pdf ? "P" : "I");
        std::string nm = e.name;
        while (nm.size() > 4 && f.body->CalcTextSizeA(fontPx(f.body), 1e9f, 0, nm.c_str()).x > lw - 110) { nm.resize(nm.size() - 4); nm += "..."; }
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
        while (nm.size() > 4 && f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, nm.c_str()).x > vb.x - va.x - 170) { nm.resize(nm.size() - 4); nm += "..."; }
        textAt(dl, f.bold, va.x + 14, va.y + 13, kInk, nm.c_str());
        pdfZoomControls(dl, f, s, act, "doc", vb.x - 136, va.y + 6, va.y + 38, ImVec2((vb.x - va.x) * 0.5f, (vb.y - va.y - 44) * 0.5f), s.browser.status == 1 && !g_webHold);
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
    const float y0 = top + 40;
    pdfZoomControls(dl, f, s, act, "dsp", w - 12 - 126, top - 1, top + 31, ImVec2((w - 24) * 0.5f, (h - 12 - y0) * 0.5f), s.browser.status == 1 && !g_webHold);
    pdfView(dl, f, s, 12, y0, w - 24, h - 12 - y0, act, kGroupDispatch, p.pdfUrl);
}

// ---- Flight Plan: the latest SimBrief plan (Import uses the SimBrief ID saved in Settings, Accounts); the briefing PDF in a tab -------------
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
std::string welcomeAirport(const SimState& s, const Progress& pr);   // (below)
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
    // the aircraft: its own outline (the drawing the map uses), small, nose along the bar; an arrowhead if there is no drawing.
    // Kept clear of the airports' names at the ends: its middle stays half its length (and a little room) inside them
    float sl = 0, ss = 0;
    const Shape* sh = specialCraft() ? nullptr : findShape(s.acfIcao, sl, ss);
    const float shapeK = sh ? 24.0f / std::max(1.0f, std::max(sl, ss)) : 0.0f;   // (fits 24 px whichever is longer, length or span)
    const float half = specialCraft() ? 17.0f : sh ? sl * shapeK * 0.5f : 9.0f;
    const float lo = x0 + half, hi = x1 - half;   // (a clear gap, the width of the line's own margin, between it and each name)
    const float ax = lo < hi ? std::max(lo, std::min(hi, px)) : (x0 + x1) * 0.5f;
    if (specialCraft()) drawCraft(dl, ImVec2(ax, cy), 34.0f, 1.5707963f);   // (the season's sleigh or witch, heading along the bar)
    else if (sh) {
        const int v0 = dl->VtxBuffer.Size;
        drawShape(dl, *sh, ImVec2(ax, cy - sl * shapeK * 0.5f), shapeK * ss / std::max(1.0f, sh->span), shapeK * sl / std::max(1.0f, sh->len), IM_COL32(255, 255, 255, 255), IM_COL32(150, 162, 186, 255));
        turnSince(dl, v0, ImVec2(ax, cy), 1.5707963f);   // nose up -> nose right
    } else dl->AddTriangleFilled(ImVec2(ax + 9, cy), ImVec2(ax - 6, cy - 7), ImVec2(ax - 6, cy + 7), IM_COL32(255, 255, 255, 255));
    char t[96];
    if (pr.etaMin >= 0) std::snprintf(t, sizeof t, "%.0f nm flown  \xC2\xB7  %.0f nm to go  \xC2\xB7  %d:%02d left", pr.flownNm, pr.leftNm, pr.etaMin / 60, pr.etaMin % 60);
    else std::snprintf(t, sizeof t, "%.0f nm flown  \xC2\xB7  %.0f nm to go", pr.flownNm, pr.leftNm);
    // never over the aircraft: in the pull-down, on the "Flight" title's line (right of it); on the page, above the bar on the side
    // the aircraft is not (right while it is in the first half, left after)
    // (on the page: lifted clear of the aircraft's top, which reaches 13 px above the bar)
    const float ty = std::min(a.y - 2, std::floor(cy - 13 - fontPx(f.small) - 2));
    const std::string welcome = welcomeAirport(s, pr);
    if (compact) {   // beside the box's title (fitted to the room right of it, never over it or the aircraft below)
        const char* title = welcome.empty() ? "Flight" : "Welcome to XXXX";
        const float room = b.x - (a.x + f.bold->CalcTextSizeA(fontPx(f.bold), 1e9f, 0, title).x + 14);
        if (welcome.empty()) textRight(dl, f.small, b.x, a.y - 21, kMuted, fitText(f.small, t, room).c_str());
    } else {   // on the page: always at the left, under the call sign; "Welcome to" the airport over the bar's right
        textAt(dl, f.small, a.x, ty, kMuted, t);
        if (!welcome.empty()) textRight(dl, f.bold, b.x, ty - 2, accentLite(255), ("Welcome to " + welcome).c_str());
    }
}
// the airport the aircraft is at, on the ground, when it is the plan's origin (before it has gone far) or its destination: the
// flight's progress says "Welcome to" it
std::string welcomeAirport(const SimState& s, const Progress& pr) {
    const FlightPlan& p = s.plan;
    const MapState& m = s.map;
    if (!p.loaded || p.legs.size() < 2 || !m.havePos || !m.onGround) return std::string();
    if (nmBetween(m.lat, m.lon, p.legs.back().lat, p.legs.back().lon) < 3.0) return p.dest;
    if (nmBetween(m.lat, m.lon, p.legs.front().lat, p.legs.front().lon) < 3.0 && (!pr.ok || pr.flownNm < 5)) return p.origin;
    return std::string();
}
// the stand nearest the aircraft (within 80 m) on the airport layouts read round it, "" none
std::string nearestStand(const SimState& s) {
    const MapState& m = s.map;
    if (!m.havePos) return std::string();
    std::string best;
    double bestM = 80;
    for (const auto& ap : m.airports)
        for (const AptGeo::Stand& st : ap->stands) {
            double la, lo;
            latLonOf(st.p, la, lo);
            const double d = nmBetween(m.lat, m.lon, la, lo) * 1852.0;
            if (d < bestM && !st.name.empty()) { bestM = d; best = st.name; }
        }
    return best;
}
// the gates follow the plan: a new plan finds the stand the aircraft is at (on the ground; for half a minute while the layouts are
// read), or N/A when it came in in the air; the arrival gate is typed
void trackGates(const SimState& s) {
    const FlightPlan& p = s.plan;
    if (!p.loaded) return;
    const std::string key = !p.divertKey.empty() ? p.divertKey : !p.ofpId.empty() ? p.ofpId : p.origin + ">" + p.dest + "@" + p.importedAt;
    static double since = -100;
    if (key != g_settings.gatesFor) {
        g_settings.gatesFor = key;
        g_settings.startGate = s.map.havePos && !s.map.onGround ? "N/A" : "";
        g_settings.arrivalGate.clear();
        g_settingsChanged = true;
        since = ImGui::GetTime();
    }
    if (g_settings.startGate.empty() && s.map.onGround && ImGui::GetTime() - since < 30) {
        const std::string st = nearestStand(s);
        if (!st.empty()) { g_settings.startGate = st; g_settingsChanged = true; }
    }
}
std::string g_gateEdit;   // a gate being typed (the departure's, or the arrival's)
int g_gateEditing = 0;    // 1 departure, 2 arrival
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
    if (button("##fpimport", p.busy ? "Importing..." : "Import From SimBrief", true, (haveId || waiting) && !p.busy)) { act.planReview = waiting; act.planImport = haveId; }
    if (button("##fpclear", "Clear", false, p.loaded)) act.planClear = true;
    // a diversion: SimBrief's latest plan (made for the new destination), from where the aircraft is; Undo goes back to the plan before
    if (s.planOriginal.loaded) { if (button("##fpundo", "Undo Divert", true, true)) act.planUndoDivert = true; }
    else if (button("##fpdivert", "Divert", false, p.loaded && haveId && !p.busy)) act.planDivert = true;
    if (button("##fppdf", "Briefing PDF", false, p.loaded && !p.pdfUrl.empty())) { openDispatch(true); return; }
    if (button("##fpmap", "Show On Map", false, p.loaded && !p.legs.empty())) {
        double la = 0, lo = 0;
        for (const FlightPlan::Leg& l : p.legs) { la += l.lat; lo += l.lon; }
        showOnMap(la / p.legs.size(), lo / p.legs.size());
        g_mapRange = rangeIndexOf(160);
        return;
    }
    if (!p.loaded) {
        textAt(dl, f.title, 16, top + 2, kInk, "No flight plan");
        const char* why = !haveId ? "Enter your SimBrief ID in Settings, Accounts, then Import." : p.status.empty() ? "Import your latest SimBrief flight plan." : p.status.c_str();
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
    row(c1 + 14, c1 + w1 - 14, "Time En Route", p.ete);
    row(c1 + 14, c1 + w1 - 14, "Cost Index", p.costIndex);
    row(c1 + 14, c1 + w1 - 14, "Average Wind", p.avgWind);
    row(c1 + 14, c1 + w1 - 14, "Alternate", p.alternate);
    {   // the gates: the one left from (found, or typed), the one going to (typed; its dot is marked on the destination's layout)
        trackGates(s);
        y += 6;
        auto gateRow = [&](int which, const char* label, std::string& v) {
            textAt(dl, f.small, c1 + 14, y + 7, kMuted, label);
            const ImVec2 ga(c1 + w1 - 14 - 110, y), gb(c1 + w1 - 14, y + 28);
            char id[16];
            std::snprintf(id, sizeof id, "##gate%d", which);
            if (g_gateEditing == which) {
                textField(dl, f, id, g_gateEdit, ga, gb, "Gate", false);
                if (g_typeInto != &g_gateEdit) {   // typed and finished
                    std::string t = g_gateEdit;
                    for (char& c : t) c = (char)std::toupper((unsigned char)c);
                    v = t; g_gateEditing = 0; g_settingsChanged = true;
                }
            } else if (pill(dl, f, id, v.empty() ? "Set" : v.c_str(), ga, gb, false, true)) {
                g_gateEditing = which; g_gateEdit = v == "N/A" ? std::string() : v;
                g_typeInto = &g_gateEdit; g_typeMultiline = false; g_kbd = true; g_kbdTarget = 2;
            }
            y += 34;
        };
        gateRow(1, "Departure Gate", g_settings.startGate);
        gateRow(2, "Arrival Gate", g_settings.arrivalGate);
        // the destination's stands, when its layout has been read (the map near it): the ones that start with what is typed
        if (g_gateEditing == 2 && !g_gateEdit.empty()) {
            std::string up = g_gateEdit;
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            float gx = c1 + 14;
            int shown = 0;
            for (const auto& ap : s.map.airports) {
                if (ap->icao != p.dest) continue;
                for (const AptGeo::Stand& st : ap->stands) {
                    std::string nm = st.name;
                    for (char& c : nm) c = (char)std::toupper((unsigned char)c);
                    if (nm.compare(0, up.size(), up) != 0 || shown >= 4) continue;
                    const float bw = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, st.name.c_str()).x + 20;
                    char id[24];
                    std::snprintf(id, sizeof id, "##gsug%d", shown++);
                    if (pill(dl, f, id, st.name.c_str(), ImVec2(gx, y), ImVec2(gx + bw, y + 24), false, true)) {
                        g_settings.arrivalGate = nm; g_gateEditing = 0; g_typeInto = nullptr; g_kbd = false; g_kbdTarget = 0; g_settingsChanged = true;
                    }
                    gx += bw + 6;
                }
            }
            if (shown) y += 30;
        }
    }
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
    row(c2 + 14, c2 + w2 - 14, "ZFW / Max", ofMax(p.zfw, p.maxZfw));   // (short names: the figures and their limits are wide)
    row(c2 + 14, c2 + w2 - 14, "TOW / Max", ofMax(p.tow, p.maxTow));
    row(c2 + 14, c2 + w2 - 14, "LW / Max", ofMax(p.ldw, p.maxLdw));
    rowStep = 21;
    // the navigation log, scrolling
    const float c3 = c2 + w2 + 10, w3 = w - 12 - c3;
    const ImVec2 la(c3, y0), lb(c3 + w3, bot);
    card(dl, la, lb, f, "NAVLOG");
    ImGui::SetCursorScreenPos(la);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##fplog", ImVec2(lb.x - la.x, lb.y - la.y));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_fplScroll -= ImGui::GetIO().MouseWheel * 44;
    touchScroll(g_fplScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
    textAt(dl, f.bold, x0, y, col(kInk), "SimBrief Flight Plan");
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
        if (specialCraft()) drawCraft(dl, mid, 36.0f, 1.5707963f, A);   // (the season's sleigh or witch)
        else if (const Shape* sh = findShape(p.aircraft, sl, ss)) {
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
    const std::string cells[8][2] = {{"Cruise", p.cruise}, {"Cost Index", p.costIndex}, {"Avg Wind", p.avgWind}, {"SID / STAR", p.sid + (p.star.empty() ? "" : " / " + p.star)},
                                     {"Block Fuel", p.blockFuel.empty() ? "" : p.blockFuel + u}, {"Trip Fuel", p.tripFuel.empty() ? "" : p.tripFuel + u},
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
    if (specialCraft()) drawCraft(dl, ImVec2(nose.x, nose.y + acLen * px * 0.5f), acLen * px, 0.0f);   // (the season's sleigh or witch, at the aircraft's size)
    else if (shape) drawShape(dl, *shape, nose, px * kx, px * ky, IM_COL32(246, 247, 249, 255), IM_COL32(128, 132, 140, 255));
    else planeTopDown(dl, nose, px, spec, IM_COL32(246, 247, 249, 255), IM_COL32(128, 132, 140, 255));
    const float hw0 = shape ? fuselageHalf(*shape, 0.25f, 0) : -1;   // the fuselage's half-width forward of the wing, drawing metres
    const float hwPx = (shape && hw0 > 0 ? hw0 * kx : spec.fus * 0.5f) * px;
    // each door keeps its bubble; with two jetways or more at the stand, a door's Connect opens a menu of the jetways that can serve it
    static int jwMenuDoor = -1;   // the door whose menu is open, -1 none
    const int reachN = g.openSam && g.jwReach >= 0 ? std::min(g.jwReach, jwDoors) : jwDoors;   // the doors openSAM gave a jetway (in door order)
    const bool chooser = doors && g.openSam && reachN > 1;   // (openSAM finds two jetways or more: Connect lists them)
    if (!chooser) jwMenuDoor = -1;
    float menuBy = -1;
    if (doors) {
        const int pairs = (g.paxDoors + 1) / 2;
        float lastBy = -1e9f;   // the bubble above's place: bubbles keep apart (their door's block stays on the door)
        float firstBy = -1;
        for (int k = 0; k < jwDoors && k < 3; ++k) {
            const int pax = g.jwDoorPax[k];
            const float frac = paxDoorAt(s.acfIcao, pax / 2, std::max(1, pairs));
            if (frac > 0.45f) continue;   // a jetway never goes to a door behind the wing
            float y = nose.y + frac * acLen * px;
            float hw = hwPx;   // the fuselage's edge at this door (it narrows towards the tail)
            if (shape && hw0 > 0) { const float hh = fuselageHalf(*shape, frac, hw0); if (hh > 0) hw = hh * kx * px; }
            if (const int sc = specialCraft()) {   // the season's sleigh, witch, turkey or firework: the door on its side (not on a reindeer, the broom's stick, the tail)
                static const float fr[4][3] = {{0.685f, 0.889f, 0.79f}, {0.47f, 0.66f, 0.57f}, {0.42f, 0.62f, 0.52f}, {0.28f, 0.58f, 0.43f}},
                                   hwU[4][3] = {{14.5f, 15.0f, 14.5f}, {8.5f, 13.5f, 11.0f}, {10.5f, 16.0f, 17.0f}, {6.5f, 6.5f, 6.5f}};
                y = nose.y + fr[sc - 1][k] * acLen * px;
                hw = hwU[sc - 1][k] * acLen * px / (sc == 1 ? 108.0f : 100.0f);
            }
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
            if (firstBy < 0) firstBy = by;
            if (jwMenuDoor == k) menuBy = by;
            dl->AddLine(ImVec2(bx + bw, by), ImVec2(bx + bw + 14, by), IM_COL32(255, 255, 255, 46), 1.0f);
            dl->AddLine(ImVec2(bx + bw + 14, by), ImVec2(side - 9, y), IM_COL32(255, 255, 255, 46), 1.0f);
            char id[16], lbl[16];
            std::snprintf(id, sizeof id, "##jw%d", k);
            std::snprintf(lbl, sizeof lbl, "%s", docked ? "Disconnect" : "Connect");
            const bool moving = g.jwMoving[k];
            const bool planeNotReady = !g.onGround || g.groundSpeedKt > 1.0f || g.beacon || g.enginesRunning || (!g.parkBrake && !g.chocks);
            // openSAM gives the doors their jetways in order and leaves a door out when its jetway would cross another: that door's Connect is grey
            const bool crosses = g.openSam && g.jwReach >= 0 && k >= reachN && !docked && !moving && !planeNotReady && (g.jetway == 1 || g.jetway == 2);
            const bool canPress = gnd && !moving && !crosses && (!g.openSam || g.jetway == 1 || g.jetway == 2);
            dl->AddRectFilled(ImVec2(bx, by - 14), ImVec2(bx + bw, by + 14), themeTone(16, 30, 56, 255), 14.0f);
            if (pill(dl, f, id, lbl, ImVec2(bx, by - 14), ImVec2(bx + bw, by + 14), docked || jwMenuDoor == k, canPress)) {
                if (chooser && !docked) jwMenuDoor = jwMenuDoor == k ? -1 : k;   // (several jetways: which one, from its menu)
                else { act.ground = GA_JETWAY_DOOR; act.groundIndex = k; jwMenuDoor = -1; }
            }
            // under the bubble: the door's name and what the jetway is doing
            // as openSAM shows it: connected only once the jetway is there, "jetway moving" until then (both kinds of jetway)
            // (openSAM says "no jetway" while the aircraft is not ready for one: then the jetway status says why)
            const char* st = crosses ? "Jetways Would Cross" : moving ? "Jetway Moving"
                           : !g.openSam ? (docked ? (g.toliss ? "Connected, Door Open" : "Connected") : planeNotReady ? "Check Status" : "X-Plane Jetway")
                           : docked ? (g.toliss ? "Docked, Door Open" : "Docked") : planeNotReady ? "Check Status" : g.jetway == 1 ? "Jetway Ready" : g.jetway == 0 ? "No Jetway Here" : g.jetway == 2 ? "Jetway At The Other Door" : "Jetway Moving";
            std::snprintf(b, sizeof b, "%d%c  \xC2\xB7  %s", pax / 2 + 1, 'L', st);
            textAt(dl, f.small, bx + 4, by + 17, docked ? kGood : moving || crosses ? kWarn : kMuted, b);
        }
        // the menu: a bubble beside Connect, one row a door (its jetway), then both; it grows out of Connect
        static int menuShownFor = -1;
        if (jwMenuDoor >= 0) menuShownFor = jwMenuDoor;
        const float mT = openness("##jwmenuopen", jwMenuDoor >= 0);
        if (mT < 0.01f && jwMenuDoor < 0) menuShownFor = -1;
        if (mT > 0.01f && menuShownFor >= 0 && (menuBy >= 0 || jwMenuDoor < 0)) {
            static float lastMenuBy = 0;
            if (menuBy >= 0) lastMenuBy = menuBy;
            const float firstBy = lastMenuBy;   // (the menu beside its door's bubble)
            const int v0 = dl->VtxBuffer.Size;
            ImGui::BeginDisabled(jwMenuDoor < 0);
            const int door = menuShownFor;
            struct Row { std::string label; int door; bool busy; };   // busy: on the other door already (grey)
            std::vector<Row> rows;
            for (int j = 0; j < reachN && j < 3; ++j) {   // openSAM's jetways, nearest the nose first (jetway j is the one it gave door j)
                const bool busy = j != door && g.jwDocked[j];
                char lb[48];
                if (busy) std::snprintf(lb, sizeof lb, "Jetway %d  \xC2\xB7  At %dL", j + 1, g.jwDoorPax[j] / 2 + 1);
                else std::snprintf(lb, sizeof lb, "Jetway %d%s", j + 1, j == door ? "  (This Door's)" : "");
                rows.push_back({lb, door, busy});
            }
            const float mx = a.x + 128, mw = 200, rh = 34, mh = rows.size() * rh + 16;
            const float my0 = std::min(firstBy - 14, e.y - 8 - mh);
            ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##jwmenucatch", ImVec2(w, h - kStatusH))) jwMenuDoor = -1;   // (a press beside it: closed)
            ImGui::SetCursorScreenPos(ImVec2(mx, my0));
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##jwmenubg", ImVec2(mw, mh));
            dl->AddRectFilled(ImVec2(mx + 2, my0 + 4), ImVec2(mx + mw + 2, my0 + mh + 4), IM_COL32(0, 0, 0, 70), 14.0f);
            dl->AddRectFilled(ImVec2(mx, my0), ImVec2(mx + mw, my0 + mh), themeTone(12, 22, 40, 246), 14.0f);
            dl->AddRect(ImVec2(mx, my0), ImVec2(mx + mw, my0 + mh), IM_COL32(255, 255, 255, 46), 14.0f, 0, 1.0f);
            for (size_t i = 0; i < rows.size(); ++i) {
                char id[16]; std::snprintf(id, sizeof id, "##jwm%zu", i);
                const float ry = my0 + 8 + i * rh;
                if (pill(dl, f, id, rows[i].label.c_str(), ImVec2(mx + 8, ry), ImVec2(mx + mw - 8, ry + rh - 6), false, !rows[i].busy)) {
                    act.ground = GA_JETWAY_DOOR; act.groundIndex = rows[i].door; jwMenuDoor = -1;
                }
            }
            ImGui::EndDisabled();
            popIn(dl, v0, ImVec2(mx, firstBy), mT);
        }
        if (jwDoors > 1) textCentered(dl, f.small, (a.x + e.x) * 0.5f, e.y - 24, kMuted, chooser ? "Connect: choose the door's jetway. Each door opens on its own." : g.openSam && g.jwReach >= 1 && reachN < jwDoors ? "openSAM leaves 2L without a jetway: it would cross 1L's." : "Each door connects on its own; the jetways leave with the last door.");
    }

    // ---- the right side: the cabin (top two thirds) and whether a jetway can connect (bottom third); the Pushback panel over both
    static bool pbOpen = false;
    // (the cabin's controls are left out while the panel covers them: a greyed control would still keep the panel's from the finger)
    const float rx = e.x + 10, rw = w - 12 - rx;
    const float split = top + std::floor((bot - top) * 2.0f / 3.0f);
    card(dl, ImVec2(rx, top), ImVec2(rx + rw, split - 5), f, "CABIN");
    const float pushSq = bot - (split + 5);   // the Pushback card beside it: a square
    card(dl, ImVec2(rx, split + 5), ImVec2(rx + rw - pushSq - 10, bot), f, "JETWAY STATUS");
    card(dl, ImVec2(rx + rw - pushSq, split + 5), ImVec2(rx + rw, bot), f, nullptr);
    const float cx0 = rx + 16, cx1 = rx + rw - 16;
    if (pbOpen) {}
    else if (!g.toliss) {
        // not a ToLiss: the same rows, each with a greyed "Not Available" bubble
        float y = top + 38;
        const char* rowsNA[3] = {"Cabin Lights", "Slides", "Cabin Ready"};
        for (int i = 0; i < 3; ++i) {
            textAt(dl, f.bold, cx0, y + 4, kMuted, rowsNA[i]);
            char id[16]; std::snprintf(id, sizeof id, "##cabna%d", i);
            pill(dl, f, id, "Not Available", ImVec2(cx1 - 180, y), ImVec2(cx1, y + 30), false, false);
            y += 48;
        }
    } else {
        float y = top + 38;
        // the lights: On and Off beside the name (On lit while they are on, Off while off; On comes back to the level they had),
        // and a slider in steps of 10 % under them
        textAt(dl, f.bold, cx0, y + 4, kInk, "Cabin Lights");
        char lv[16];
        const int pct = g.cabinLight < 0 ? -1 : (int)std::lround(g.cabinLight * 100);
        static int lastOn = 100;   // the level they were last on at
        if (pct > 0) lastOn = pct;
        if (pct < 0) std::snprintf(lv, sizeof lv, "-");
        else if (pct == 0) std::snprintf(lv, sizeof lv, "Off");
        else std::snprintf(lv, sizeof lv, "%d%%", pct);
        textAt(dl, f.bold, cx0 + 112, y + 4, pct > 0 ? kInk : kMuted, lv);
        textAt(dl, f.small, cx0, y + 26, kMuted, "Keep ToLiss Cabin Light Controls To Auto");
        if (g.cabinLight >= 0) {
            const float bw = 64;
            // Auto: XPTablet dims and brightens them with the daylight, as the tablet's own auto dim (100 % by day, 0 % at full night);
            // On, Off or the slider take it off Auto
            const bool autoOn = g_settings.cabinAuto;
            if (pill(dl, f, "##clon", "On", ImVec2(cx1 - 2 * bw - 8, y), ImVec2(cx1 - bw - 8, y + 30), pct > 0 && !autoOn, true) && (pct <= 0 || autoOn)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = pct > 0 ? pct : lastOn; g_settings.cabinAuto = false; g_settingsChanged = true; }
            if (pill(dl, f, "##cloff", "Off", ImVec2(cx1 - bw, y), ImVec2(cx1, y + 30), pct == 0 && !autoOn, true) && (pct != 0 || autoOn)) { act.ground = GA_CABIN_LIGHT; act.groundIndex = 0; g_settings.cabinAuto = false; g_settingsChanged = true; }
            if (pill(dl, f, "##clauto", "Auto", ImVec2(cx1 - 3 * bw - 16, y), ImVec2(cx1 - 2 * bw - 16, y + 30), autoOn, true)) { g_settings.cabinAuto = !g_settings.cabinAuto; g_settingsChanged = true; }
        }
        y += 50;
        if (g.cabinLight >= 0) {
            // the track, its fill, ticks at every 10 % and the handle; pressing or dragging on it sets the level, in 10 % clicks
            const float tx0 = cx0 + 10, tx1 = cx1 - 10, ty = y + 14;
            ImGui::SetCursorScreenPos(ImVec2(cx0, y));
            ImGui::BeginDisabled(g_settings.cabinAuto);   // (on Auto the daylight sets them: the slider is greyed)
            ImGui::InvisibleButton("##cablight", ImVec2(cx1 - cx0, 28));
            ImGui::EndDisabled();
            const float greyK = openness("##cabgrey", g_settings.cabinAuto);
            static float knob = -1;   // where the knob is drawn (0..1): it glides to the level, and follows the finger
            const float dt = ImGui::GetIO().DeltaTime;
            float target = std::max(0, pct) / 100.0f;
            if (ImGui::IsItemActive()) {
                const float t = std::max(0.0f, std::min(1.0f, (ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0)));
                const int want = (int)std::lround(t * 10) * 10;
                target = want / 100.0f;
                if (want != pct) { act.ground = GA_CABIN_LIGHT; act.groundIndex = want; g_settings.cabinAuto = false; g_settingsChanged = true; }
                if (knob < 0) knob = t;
                knob += (t - knob) * (1.0f - std::exp(-dt * 30.0f));
            } else {
                if (knob < 0) knob = target;
                knob += (target - knob) * (1.0f - std::exp(-dt * 16.0f));
            }
            const float hx = tx0 + (tx1 - tx0) * knob;
            // on Auto only the bar and the knob go grey (eased), not the area round them
            auto greyed = [&](int r, int g2, int b2, int al) { const float k = greyK; const int gr = 120;
                return IM_COL32((int)(r + (gr - r) * k), (int)(g2 + (gr + 6 - g2) * k), (int)(b2 + (gr + 18 - b2) * k), al); };
            const ImU32 fillCol = greyed(255, 214, 120, 255), knobCol = greyed(245, 248, 255, 255);
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(tx1, ty + 3), IM_COL32(255, 255, 255, (int)(40 - 14 * greyK)), 3.0f);
            dl->AddRectFilled(ImVec2(tx0, ty - 3), ImVec2(hx, ty + 3), fillCol, 3.0f);
            for (int i = 0; i <= 10; ++i) {
                const float x = tx0 + (tx1 - tx0) * i / 10.0f;
                dl->AddLine(ImVec2(x, ty + 7), ImVec2(x, ty + (i % 5 == 0 ? 13 : 10)), IM_COL32(255, 255, 255, 90), 1.0f);
            }
            dl->AddCircleFilled(ImVec2(hx, ty), 10.0f, knobCol, 0);
            dl->AddCircle(ImVec2(hx, ty), 10.0f, fillCol, 0, 2.0f);
            y += 32;
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
        // Arm and Disarm hold them so; with Auto, ToLiss arms and disarms them itself: Arm and Disarm are greyed then, the one that
        // matches what ToLiss has done still lit (a press on either holds it so again, out of Auto)
        const ImVec2 armA(cx0, y), armB(cx0 + bw, y + 30), disA(cx0 + bw + 8, y), disB(cx0 + 2 * bw + 8, y + 30);
        if (pill(dl, f, "##arm", "Arm", armA, armB, g.slides == 1, true)) act.ground = GA_SLIDES_ARM;
        if (pill(dl, f, "##disarm", "Disarm", disA, disB, g.slides == 0, true)) act.ground = GA_SLIDES_DISARM;
        {
            const float gk = openness("##slgrey", g.slidesAuto);   // (greyed in and out smoothly)
            if (gk > 0.01f) {
                dl->AddRectFilled(armA, armB, IM_COL32(14, 18, 28, (int)(150 * gk)), 15.0f);
                dl->AddRectFilled(disA, disB, IM_COL32(14, 18, 28, (int)(150 * gk)), 15.0f);
            }
        }
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
            textCentered(dl, f.bold, (ra.x + rb.x) * 0.5f, std::floor((ra.y + rb.y) * 0.5f - fontPx(f.bold) * 0.5f), onAccent(), "Cabin Ready");
            if (hit) act.ground = GA_CABIN_READY;
        }
    }
    {   // the jetway status: Good, or what keeps a jetway from connecting
        std::vector<std::string> why;
        if (!g.onGround) why.push_back("Not On The Ground");
        else if (g.groundSpeedKt > 1.0f) why.push_back("Aircraft Moving");
        if (g.beacon) why.push_back("Beacon Light On");
        if (g.enginesRunning) why.push_back("Engines Running");
        if (!g.parkBrake && !g.chocks) why.push_back("Chocks Or Parking Brake Not Set");
        if (g.openSam && why.empty()) {
            if (g.jetway == 0) why.push_back("No Jetway At This Stand");
            else if (g.jetway == -1 && !anyDocked) why.push_back("Jetway Moving, Or None Within Reach");
        }
        float y = split + 5 + 32;
        if (!doors) {
            textAt(dl, f.title, cx0, y, kMuted, "Not Available");
            textAt(dl, f.small, cx0, y + 30, kMuted, "This Aircraft Does Not Use Jetways");
        } else if (why.empty()) {
            textAt(dl, f.title, cx0, y, kGood, anyDocked ? "Good  \xC2\xB7  Connected" : "Good");
            textAt(dl, f.small, cx0, y + 30, kMuted, g.openSam ? "openSAM Jetways" : "X-Plane's Own Jetways");
        } else {
            for (const std::string& t : why) {
                if (y > bot - 22) break;
                dl->AddCircleFilled(ImVec2(cx0 + 4, y + fontPx(f.body) * 0.5f + 1), 3.5f, kWarn, 0);
                textAt(dl, f.body, cx0 + 16, y, kInk, t.c_str());
                y += 22;
            }
        }
    }
    // ---- pushback: a bubble under the aircraft's size, its panel growing out of it: BetterPushback's planner, tug, Start and Stop
    // when it is installed, X-Plane's own tug (left, straight, right, stop) otherwise; and what is still in the way of pushing
    {
        const bool bp = g.pushSource == 1;
        // the button: a square in its own card beside the jetway status, "Push" over "Back" (lit while it is open or pushing)
        const float sq = pushSq, ins = 14;
        const ImVec2 pa(rx + rw - sq + ins, split + 5 + ins), pb(rx + rw - ins, bot - ins);
        {
            ImGui::SetCursorScreenPos(pa);
            if (ImGui::InvisibleButton("##pushbtn", ImVec2(pb.x - pa.x, pb.y - pa.y))) pbOpen = !pbOpen;   // (open: it closes the panel)
            const bool lit = pbOpen || g.bpStarted, hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
            const Ease ez = eased("##pushbtn", lit, ImGui::IsItemHovered(), ImGui::IsItemActive());
            const float redK = openness("##pushred", pbOpen && hov, 18.0f);   // open and hovered: red, "Close"
            const float in = ez.down * 1.5f;
            const ImU32 off = mixCol(IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 40), ez.hot);
            const ImU32 base = mixCol(off, mixCol(accentCol(255), accentLite(255), ez.hot), ez.on);
            dl->AddRectFilled(ImVec2(pa.x + in, pa.y + in), ImVec2(pb.x - in, pb.y - in), mixCol(base, IM_COL32(220, 60, 60, 255), redK), 16.0f);
            const float cx = (pa.x + pb.x) * 0.5f, cy = (pa.y + pb.y) * 0.5f, lh = fontPx(f.title);
            const ImU32 tc = lit ? IM_COL32(255, 255, 255, 255) : kInk;
            if (redK > 0.5f) textCentered(dl, f.title, cx, std::floor(cy - lh * 0.5f), IM_COL32(255, 255, 255, 255), "Close");
            else { textCentered(dl, f.title, cx, std::floor(cy - lh), tc, "Push"); textCentered(dl, f.title, cx, std::floor(cy), tc, "Back"); }
        }
        const float t = openness("##pushopen", pbOpen);
        if (t > 0.01f) {   // the panel: over the cabin's card; Push Back (red, "Close", under the finger) puts it away
            std::vector<std::string> checks;   // what is in the way
            if (!gnd) checks.push_back("Not On The Ground");
            // a jetway still on the aircraft (openSAM's or X-Plane's): no tug is called and no push starts until it is off
            const bool jwOn = anyDocked || (g.openSam && g.jetway == 2);
            if (jwOn) checks.push_back(bp ? "Jetway connected: disconnect it before the tug" : "Jetway connected: disconnect it first");
            if (g.chocks) checks.push_back("Remove the chocks");
            if (g.toliss && g.extPower) checks.push_back("Disconnect ground power");
            bool doorOpen = false;
            if (g.toliss) { for (int i = 0; i < g.paxDoors && i < 8; ++i) if (g.paxDoor[i] == 2) doorOpen = true; for (int i = 0; i < g.cargoDoors && i < 4; ++i) if (g.cargoDoor[i] == 2) doorOpen = true; }
            if (doorOpen) checks.push_back("Close the doors");
            if (g.parkBrake && (!bp || g.bpConnected)) checks.push_back(bp ? "Release the parking brake: the tug is on" : "Release the parking brake");
            // BetterPushback's tug stops (and calls out) while the landing lights or the taxi light are on
            if (g.landingLights) checks.push_back(bp ? "Landing lights off: the tug will not push" : "Turn the landing lights off");
            if (g.taxiLight) checks.push_back(bp ? "Taxi light off: the tug will not push" : "Turn the taxi light off");
            const ImVec2 qa(rx, top), qb(rx + rw, split - 5);   // over the cabin's card (the jetway status and Push Back stay)
            const int v0 = dl->VtxBuffer.Size;
            ImGui::BeginDisabled(!pbOpen);
            ImGui::SetCursorScreenPos(qa);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##pushbg", ImVec2(qb.x - qa.x, qb.y - qa.y));   // (the cabin under it takes no presses)
            dl->AddRectFilled(qa, qb, themeTone(12, 22, 40, 255), 16.0f);
            dl->AddRect(qa, qb, IM_COL32(255, 255, 255, 40), 16.0f, 0, 1.0f);
            const float x0 = qa.x + 16, x1 = qb.x - 16;
            textAt(dl, f.small, x0, qa.y + 12, kMuted, "PUSHBACK");
            textRight(dl, f.small, x1, qa.y + 12, kMuted, bp ? "BetterPushback" : "X-Plane's Tug");
            float y = qa.y + 36;
            // the tug's way there and back: yellow "Tug Connecting" from the press (or the push's start) until it is on, yellow
            // "Tug Disconnecting" from Disconnect or Stop until it is off (BetterPushback only says connected or not)
            static double connAsked = -1, discAsked = -1;
            static bool handSignals = false;   // disconnected on purpose during a push: it waits, not connecting
            const double nowT = ImGui::GetTime();
            if (g.bpConnected || nowT - connAsked > 180) connAsked = -1;
            if (!g.bpConnected || nowT - discAsked > 180) discAsked = -1;
            if (!g.bpStarted && !g.bpConnected) handSignals = false;
            const bool lightsStop = bp && g.bpStarted && g.bpConnected && (g.landingLights || g.taxiLight);
            const bool disconnecting = bp && discAsked >= 0 && g.bpConnected;
            const bool connecting = bp && !g.bpConnected && !handSignals && (connAsked >= 0 || g.bpStarted);
            const char* state = !bp ? "Steer With The Rudder While It Pushes"
                              : lightsStop ? "Stopped: The Lights Blind The Driver"
                              : disconnecting ? "Tug Disconnecting"
                              : connecting ? "Tug Connecting"
                              : g.bpStarted && g.bpConnected ? "Pushing Back"
                              : g.bpConnected ? "Tug Connected"
                              : handSignals ? "Tug Disconnected: Hand Signals" : "Tug Not Connected";
            const ImU32 stateCol = lightsStop || disconnecting || connecting ? kWarn : g.bpConnected ? kGood : kInk;
            textAt(dl, f.bold, x0, y, stateCol, state);
            y += 28;
            // the buttons, at the bottom (the list above them, as much as fits: the rest counted)
            const float rowH = 36, by1 = qb.y - 12 - rowH, by0 = by1 - rowH - 4;
            if (checks.empty()) { dl->AddCircleFilled(ImVec2(x0 + 4, y + 9), 4.0f, kGood, 0); textAt(dl, f.body, x0 + 16, y, kGood, "Ready To Push"); y += 24; }
            for (size_t i = 0; i < checks.size(); ++i) {
                if (y + 24 > by0 - 4 && i + 1 < checks.size()) {
                    char more[32]; std::snprintf(more, sizeof more, "and %d more", (int)(checks.size() - i));
                    textAt(dl, f.small, x0 + 16, y + 2, kWarn, more);
                    break;
                }
                dl->AddCircleFilled(ImVec2(x0 + 4, y + 9), 4.0f, kWarn, 0);
                textAt(dl, f.body, x0 + 16, y, kInk, checks[i].c_str());
                y += 24;
            }
            const float hwB = std::floor((x1 - x0 - 8) * 0.5f);
            if (bp) {
                // the tug: connected (or on its way, or waiting at the aircraft disconnected) it can be disconnected; else connected
                const bool tugOn = g.bpConnected;
                const char* tugLbl = tugOn ? "Disconnect The Tug" : g.bpStarted ? "Reconnect The Tug" : "Connect The Tug";
                const bool tugBusy = connecting || disconnecting;   // (on its way, or leaving: wait for it)
                if (pill(dl, f, "##bpplan", "Plan Route", ImVec2(x0, by0), ImVec2(x0 + hwB, by0 + rowH - 6), false, gnd && !g.bpStarted)) act.ground = GA_BP_PLAN;
                if (pill(dl, f, "##bpconn", tugLbl, ImVec2(x1 - hwB, by0), ImVec2(x1, by0 + rowH - 6), tugOn && !tugBusy, gnd && !tugBusy && (tugOn || !jwOn))) {
                    act.ground = tugOn ? GA_BP_DISCONNECT : g.bpStarted ? GA_BP_RECONNECT : GA_BP_CONNECT;
                    if (tugOn) { discAsked = nowT; if (g.bpStarted) handSignals = true; } else { connAsked = nowT; handSignals = false; }
                }
                if (pill(dl, f, "##bpstart", "Start", ImVec2(x0, by1), ImVec2(x0 + hwB, by1 + rowH - 6), g.bpStarted, gnd && !g.bpStarted && !jwOn)) { act.ground = GA_BP_START; if (!g.bpConnected) connAsked = nowT; handSignals = false; }
                if (pill(dl, f, "##bpstop", "Stop", ImVec2(x1 - hwB, by1), ImVec2(x1, by1 + rowH - 6), false, g.bpStarted)) { act.ground = GA_BP_STOP; if (g.bpConnected) discAsked = nowT; }
            } else {
                const float tw = std::floor((x1 - x0 - 16) / 3.0f);
                if (pill(dl, f, "##pbl", "Left", ImVec2(x0, by0), ImVec2(x0 + tw, by0 + rowH - 6), false, gnd && !jwOn)) act.ground = GA_PUSH_LEFT;
                if (pill(dl, f, "##pbs", "Straight", ImVec2(x0 + tw + 8, by0), ImVec2(x0 + 2 * tw + 8, by0 + rowH - 6), false, gnd && !jwOn)) act.ground = GA_PUSH_STRAIGHT;
                if (pill(dl, f, "##pbr", "Right", ImVec2(x1 - tw, by0), ImVec2(x1, by0 + rowH - 6), false, gnd && !jwOn)) act.ground = GA_PUSH_RIGHT;
                if (pill(dl, f, "##pbstop", "Stop", ImVec2(x0, by1), ImVec2(x1, by1 + rowH - 6), false, gnd)) act.ground = GA_PUSH_STOP;
            }
            ImGui::EndDisabled();
            popIn(dl, v0, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), t);
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
    touchScroll(g_creditsScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    static float credMax = 1e9f;
    g_creditsScroll = std::max(0.0f, std::min(g_creditsScroll, credMax));
    dl->PushClipRect(ImVec2(ca.x, ca.y + 30), ImVec2(cb.x, cb.y - 2), true);
    struct Credit { const char* what; const char* who; const char* licence; };
    static const Credit kCredits[] = {
        {"XPTablet", "\xC2\xA9 2026 Alpine Aviation. Free to use; not for resale or re-upload (LICENSE.txt)", "Freeware"},
        {"Aircraft Top Views (Ground Page)", "AircraftShapesSVG by RexKramer1 and amnesica (BelugaProject)\ngithub.com/RexKramer1/AircraftShapesSVG", "GNU GPL v3.0"},
        {"User Interface", "Dear ImGui by Omar Cornut and contributors", "MIT"},
        {"Text", "IBM Plex Sans (IBM Corp.), JetBrains Mono (The JetBrains Mono Project Authors)", "SIL Open Font License 1.1"},
        {"Downloads", "libcurl (Daniel Stenberg and contributors), zlib (Jean-loup Gailly, Mark Adler), OpenSSL on Linux", "curl, zlib, Apache 2.0"},
        {"Web Pages", "Chromium Embedded Framework (Marshall A. Greenblatt and contributors), X-Plane's own copy of Chromium", "BSD 3-clause"},
        {"Map", "\xC2\xA9 OpenStreetMap contributors (openstreetmap.org/copyright); satellite imagery: Esri, Maxar, Earthstar Geographics", "ODbL; Esri terms"},
        {"Airport Layouts On The Map", "X-Plane's airport data (the Scenery Gateway and its contributors) and your installed scenery", "GNU GPL"},
        {"Time Zones", "Natural Earth (via WXPlane)", "Public domain"},
        {"Map Pictures", "stb_image by Sean Barrett", "MIT / public domain"},
        {"Airport Facts And CTAF", "OurAirports (ourairports.com) and its contributors", "Public domain"},
        {"Online Traffic Details", "VATSIM data feed (data.vatsim.net)", "Shown live, not stored"},
        {"Navaids And Waypoints On The Map", "X-Plane's navigation data (or the AIRAC in Custom Data, e.g. Navigraph's)", ""},
        {"Route On The Map", "Your flight plan from SimBrief (simbrief.com)", ""},
        {"METAR Reports", "NOAA / National Weather Service, Aviation Weather Center (aviationweather.gov)", "Public domain"},
        {"Weather Page With WXPlane Installed", "WXPlane by Alpine Aviation", ""},
        {"Airbus Checklists", "FlightGear A320-family by Josh Davidson (Octal450), Jonathan Redpath and contributors (github.com/legoboyvdlp/A320-family), adapted", "GPL-2.0"},
        {"Icons And Logos", "Provided through creatorsrepo.com", ""},
        {"App Icons", "Material Design Icons by Pictogrammers (pictogrammers.com): cog, check-bold, file-document, star-four-points, airplane, airport, web, power, weather-partly-snowy-rainy; and Google's Material Icons \"store\" (the App Store)", "Apache 2.0"},
        {"ForeFlight Icon", "The ForeFlight logo is a trademark of ForeFlight LLC, used only to show their web app", "Trademark"},
        {"Navigraph Icon", "The Navigraph logo is a trademark of Navigraph, used only to show their charts web app", "Trademark"},
        {"SkyVector Icon", "The SkyVector logo is a trademark of SkyVector (Seattle Avionics), used only to show their web app", "Trademark"},
        {"Volanta Icon", "The Volanta logo is a trademark of Orbx, used only to show their web map", "Trademark"},
        {"ChartFox Icon", "The ChartFox logo is a trademark of ChartFox (chartfox.org), used only to show their charts web app", "Trademark"},
        {"SimBrief Icon", "The SimBrief logo is a trademark of SimBrief (Navigraph), used only to show their web app", "Trademark"},
        {"VATSIM Radar Icon", "The VATSIM Radar logo belongs to VATSIM Radar (vatsim-radar.com), used only to show their web map", "Trademark"},
        {"VATSIM Icon", "The VATSIM logo is a trademark of VATSIM, Inc. (vatsim.net), used only to show their pages", "Trademark"},
        {"Local Connect", "Mbed TLS (trustedfirmware.org/projects/mbed-tls): the encryption between the computer and paired devices. stb_image_write by Sean Barrett: the tablet's picture for them", "Apache-2.0; MIT / public domain"},
        {"Flightradar24 Icon", "The Flightradar24 logo belongs to Flightradar24 AB (flightradar24.com), used only to show their web app", "Trademark"},
        {"natTrak Icon", "The natTrak logo belongs to VATSIM's natTrak (nattrak.vatsim.net), used only to show their web app", "Trademark"},
        {"IVAO Icon", "The IVAO logo is a trademark of the International Virtual Aviation Organisation (ivao.aero), used only to show their Webeye map", "Trademark"},
        {"Spotify, YouTube, YouTube Music, Apple Music and SoundCloud icons", "Their logos are trademarks of Spotify AB, Google LLC, Apple Inc. and SoundCloud Global Limited, used only to show their web players", "Trademark"},
        {"ACARS And CPDLC", "Hoppie's ACARS network (hoppie.nl): telex, pre-departure clearances and CPDLC, with your own logon code; for flight simulation only", "Free service"},
        {"VATSIM ATC And Airspaces", "VATSIM data feed (data.vatsim.net); FIR and airspace boundaries from the VATSpy Data Project (github.com/vatsimnetwork/vatspy-data-project), fetched and shown as published", "CC BY-SA 4.0"},
        {"Jetways", "openSAM by Holger Teutsch (hotbso) and contributors (github.com/hotbso/openSAM), its commands and datarefs (no openSAM code is in XPTablet)", "LGPL-2.1"},
        {"Games", "Knots and Crosses, 2048 (after Gabriele Cirulli's game), and Contrail, Taxi Rush, Sky Patrol, Hangar Climb, Cockpit 21 and Cargo Load: all made for XPTablet, every map, rule and drawing its own", ""},
        {"", "XPTablet is not made by or affiliated with ForeFlight, Navigraph, SkyVector, Volanta / Orbx, VATSIM Radar, ChartFox, SimBrief, VATSIM, IVAO, Spotify, Google / YouTube, Apple, SoundCloud or openSAM.", ""},
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
    textAt(dl, f.small, x0, y + 2, kMuted, "Full license texts: LICENSES.txt and shapes/LICENSE-GPL-3.0.txt in the XPTablet folder.");
    dl->PopClipRect();
    const float content = y + 30 + g_creditsScroll - (top + 38);
    credMax = std::max(0.0f, content - (cb.y - ca.y - 40));
    g_creditsScroll = std::min(g_creditsScroll, credMax);
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
    if (g_power != POWER_ON || (g_batt <= 0.0f && !g_charger)) return;   // a tablet that is off or flat gets none (and makes no sound)
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
constexpr float kCcMediaBase = 196, kCcBrightH = 88, kCcGap = 10, kCcBottomGap = 36;
float g_ccMediaH = kCcMediaBase;   // the media card's height: a row more when the computer's volume is there under the media's
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
    // a new cover fades in over the one before (0.45 s)
    const float t = std::max(0.0f, std::min(1.0f, (float)(ImGui::GetTime() - m.artChangedAt) / 0.45f));
    const float e = t * t * (3 - 2 * t);
    auto image = [&](ImTextureID tex, int w, int h, float alpha) {   // square-cropped
        const float aw = (float)w, ah = (float)h, side = std::min(aw, ah);
        const ImVec2 uv0((aw - side) * 0.5f / aw, (ah - side) * 0.5f / ah), uv1(1 - uv0.x, 1 - uv0.y);
        dl->AddImageRounded(tex, a, b, uv0, uv1, IM_COL32(255, 255, 255, (int)(255 * alpha)), r);
    };
    if (m.artPrev && m.artPrevW > 0 && e < 1.0f && m.art && m.artW > 0) {
        image(m.artPrev, m.artPrevW, m.artPrevH, 1.0f);
        image(m.art, m.artW, m.artH, e);
    } else if (m.art && m.artW > 0 && m.artH > 0) {
        image(m.art, m.artW, m.artH, 1.0f);
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
        dl->AddRectFilled(ImVec2(bx0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), mixCol(IM_COL32(255, 255, 255, 220), accentLite(255), hot), th);
        if (hot > 0.02f) dl->AddCircleFilled(ImVec2(fx, cy), 9.0f + 5.0f * hot, IM_COL32(255, 255, 255, (int)(40 * hot)), 24);   // a glow under the finger
        dl->AddCircleFilled(ImVec2(fx, cy), 5.0f + 3.5f * hot, IM_COL32(255, 255, 255, 255), 24);
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
        const bool hit = ImGui::InvisibleButton(bid, ImVec2(rr * 2, rr * 2)) && enabled;
        if (hit) act.media = k == 1 ? 1 : k == 0 ? 2 : 3;
        // alive: a glass disc that swells on hover and gives under the finger; pressed, a ring spreads out from it; the play button
        // in the theme's accent
        const Ease e = eased(bid, k == 1 && m.playing, ImGui::IsItemHovered() && enabled, ImGui::IsItemActive() && enabled);
        ImGuiStorage* stg = ImGui::GetStateStorage();
        const ImGuiID pk = ImGui::GetID(bid);
        if (hit) stg->SetFloat(pk + 7, (float)ImGui::GetTime());
        const float since = (float)ImGui::GetTime() - stg->GetFloat(pk + 7, -10.0f);
        const float scale = 1.0f + 0.06f * e.hot - 0.08f * e.down;
        const float rd = rr * scale;
        if (k == 1) {
            dl->AddCircleFilled(ImVec2(p.x, p.y + 2), rd + 1, IM_COL32(0, 0, 0, 60), 0);
            dl->AddCircleFilled(p, rd, mixCol(accentCol(enabled ? 230 : 90), accentLite(enabled ? 255 : 90), e.hot * 0.6f), 0);
        } else if (e.hot > 0.01f || e.down > 0.01f) dl->AddCircleFilled(p, rd, IM_COL32(255, 255, 255, (int)(26 * e.hot + 34 * e.down)), 0);
        if (since < 0.45f) {   // the ring
            const float q = since / 0.45f;
            dl->AddCircle(p, rr * (1.0f + 0.45f * q), IM_COL32(255, 255, 255, (int)(150 * (1 - q))), 0, 2.0f);
        }
        const ImU32 col = enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 90);
        const float s = rr * 0.42f * scale;
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
    std::string r = m.radioTx ? std::string("Radio: Transmitting") : m.radioRx ? "Radio: Receiving" + (m.radioSource.empty() ? std::string() : " (" + m.radioSource + ")") : std::string("Radio: Quiet");
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
// A line too long for its room: 5 s after the track came, and then every 30 s, it slides slowly to its end and back (25 units a
// second, a moment's rest at each end); one that fits is drawn as it is
void marquee(ImDrawList* dl, ImFont* fnt, float x, float y, float room, ImU32 col, const std::string& text, double since) {
    const float tw = fnt->CalcTextSizeA(fontPx(fnt), 1e9f, 0, text.c_str()).x;
    if (tw <= room) { textAt(dl, fnt, x, y, col, text.c_str()); return; }
    const float over = tw - room, run = over / 25.0f;   // seconds to the end
    const double t = std::fmod(std::max(0.0, ImGui::GetTime() - since - 5.0), 30.0);
    float off = 0;
    if (ImGui::GetTime() - since >= 5.0) {
        if (t < run) off = over * (float)(t / run);
        else if (t < run + 1.5) off = over;
        else if (t < 2 * run + 1.5) off = over * (1.0f - (float)((t - run - 1.5) / run));
    }
    const float e = off / std::max(1.0f, over), eased = e * e * (3 - 2 * e);
    dl->PushClipRect(ImVec2(x, y - 2), ImVec2(x + room, y + fontPx(fnt) + 4), true);
    textAt(dl, fnt, std::floor(x - over * eased), y, col, text.c_str());
    dl->PopClipRect();
}
double trackSince(const MediaState& m) {   // when this track began to be shown
    static std::string key;
    static double at = 0;
    const std::string k = m.title + "\x01" + m.artist;
    if (k != key) { key = k; at = ImGui::GetTime(); }
    return at;
}
// a volume slider, a speaker and its name at its left: "Media" (the player's own) or "PC" (the computer's). The media's is greyed
// and inert while XPTablet holds the music down for the radio (it would only fight the lowering; it comes back with the volume)
constexpr float kVolLabelW = 62;
void volumeSlider(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 a, float x1, float vol, float& out, bool computer, bool enabled = true) {
    const ImVec2 c(a.x + 10, a.y + 15);
    const ImU32 col = enabled ? IM_COL32(210, 222, 242, 255) : IM_COL32(210, 222, 242, 90);
    dl->AddRectFilled(ImVec2(c.x - 6, c.y - 3), ImVec2(c.x - 2, c.y + 3), col, 1.0f);
    dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y - 8), ImVec2(c.x + 3, c.y + 8), col);
    dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 8), ImVec2(c.x - 3, c.y + 3), col);
    textAt(dl, f.small, a.x + 22, std::floor(c.y - fontPx(f.small) * 0.5f), enabled ? kInk : IM_COL32(236, 241, 250, 90), computer ? "PC" : "Media");
    char txt[16];
    std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(vol * 100));
    float v = vol;
    if (smoothSlider(dl, f, id, ImVec2(a.x + kVolLabelW, a.y), ImVec2(x1, a.y + 30), v, 0.0f, 1.0f, 0, enabled, txt)) out = v;
}

void musicPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const MediaState& m = s.media;
    {   // the background: the cover's colour, from the top left into the theme's, changing slowly with the track
        static ImU32 shown = 0;
        const ImU32 want = m.active && m.artColor ? m.artColor : 0;
        const float k = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 3.0f);
        shown = mixCol(shown ? shown : (want ? want : themeTone(10, 20, 40, 255)), want ? want : themeTone(10, 20, 40, 255), k);
        if (want || shown) {
            const ImU32 c0 = (shown & 0x00FFFFFF) | (200u << 24), c1 = (shown & 0x00FFFFFF) | (40u << 24);
            dl->AddRectFilledMultiColor(ImVec2(0, kStatusH + kHeaderH), ImVec2(w, h), c0, c1, IM_COL32(0, 0, 0, 0), c1);
        }
    }
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
        textAt(dl, f.title, x, y, kInk, "Nothing Playing");
        dl->AddText(f.body, fontPx(f.body), ImVec2(x, y + 36), kMuted, "Play something in Spotify, Apple Music, a browser or any player: it shows here and in the pull-down from the logo.", nullptr, x1 - x);
        y += 110;
    } else {
        textAt(dl, f.small, x, y, IM_COL32(255, 120, 150, 255), fitText(f.small, m.app.empty() ? std::string("Now Playing") : m.app, x1 - x).c_str());
        y += 24;
        const double since = trackSince(m);
        marquee(dl, f.title, x, y, x1 - x, kInk, m.title.empty() ? std::string("Unknown Title") : m.title, since);
        y += 30;
        marquee(dl, f.body, x, y, x1 - x, kMuted, m.artist, since);
        y += 22;
        if (!m.album.empty()) marquee(dl, f.small, x, y, x1 - x, kMuted, m.album, since);
        y += 30;
        progressBar(dl, f, m, ImVec2(x, y), x1, "##mpprog", false, act);
        y += 40;
    }
    playerButtons(dl, m, ImVec2(std::floor((x + x1) * 0.5f), y + 20), 22.0f, "##mp", act);
    y += 62;
    // the volumes: the player's own, and (Windows) the computer's
    if (m.playerVolume >= 0) { float v = -1; volumeSlider(dl, f, "##mpvol", ImVec2(x, y), x1, m.playerVolume, v, false, !m.ducked); if (v >= 0) act.mediaPlayerVol = v; y += 38; }
    if (m.masterVolume >= 0) { float v = -1; volumeSlider(dl, f, "##mpmaster", ImVec2(x, y), x1, m.masterVolume, v, true); if (v >= 0) act.mediaMasterVol = v; y += 38; }
    y += 4;
    dl->AddLine(ImVec2(x, y), ImVec2(x1, y), IM_COL32(255, 255, 255, 24));
    textAt(dl, f.small, x, y + 10, m.radioRx ? kWarn : kMuted, radioLine(m).c_str());
    if (y + 56 < h - 14) {
        const std::string rule = std::string("When the radios receive or transmit, XPTablet ") + duckName(g_settings.mediaDuck) + " (Settings, Media).";
        dl->AddText(f.small, fontPx(f.small), ImVec2(x, y + 32), kMuted, rule.c_str(), nullptr, x1 - x);
    }
}

// notifications from what the host tells: traffic in TCAS RA range, a new SimBrief plan
void watchForNotes(const SimState& s) {
    const double now = ImGui::GetTime();
    trackTraffic(s.map);
    {   // VATSIM: a controller coming online at the flight's departure or destination (not those already there when it was first seen)
        static std::set<std::string> seen;
        static bool primed = false;
        static std::string forPlan;
        const std::string planKey = s.plan.loaded ? s.plan.origin + ">" + s.plan.dest : std::string();
        if (planKey != forPlan) { forPlan = planKey; seen.clear(); primed = false; }
        if (g_settings.vatsimAtcNotes && s.plan.loaded && s.vatsimAtcHave) {
            for (const std::string& icao : {s.plan.origin, s.plan.dest}) {
                for (const VatsimController& c : atcFor(s, icao)) {
                    if (!seen.insert(c.callsign).second || !primed) continue;
                    postNote(c.callsign + " online", (c.facility == 7 ? std::string("ATIS ") : std::string()) + c.freq + (c.name.empty() || c.facility == 7 ? std::string() : "  \xC2\xB7  " + c.name), APP_AIRPORTS);
                }
            }
            primed = true;
        }
    }
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
        postNote(level == 3 ? "Runway Collision Alert" : level == 2 ? "TCAS Resolution Advisory" : "TCAS Traffic Advisory", b, APP_MAP);
    }
    for (auto it = g_raSeen.begin(); it != g_raSeen.end();) { if (now - it->second > 120) it = g_raSeen.erase(it); else ++it; }
    if (g_planSeqSeen < 0) g_planSeqSeen = s.newPlanSeq;
    if (s.newPlanSeq != g_planSeqSeen) {
        g_planSeqSeen = s.newPlanSeq;
        if (g_settings.notifySimbrief) postNote("New SimBrief Flight Plan Detected", s.newPlanText, APP_ROUTES);
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
        g_ccMediaH = kCcMediaBase + (md.masterVolume >= 0 ? 34.0f : 0.0f);
        const float kCcMediaH = g_ccMediaH;
        const ImVec2 a(m, top + 34), b(m + colL, top + 34 + kCcMediaH);
        glass(dl, a, b, 24.0f);
        ImGui::SetCursorScreenPos(a);   // the card (where nothing else is): the Media page
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##ccmediacard", ImVec2(b.x - a.x, b.y - a.y))) { g_ccTarget = 0; openApp(APP_MUSIC); }
        const float art = 74;
        for (int k = 3; k >= 1; --k) {   // a soft shadow, centred under the cover
            const float g = 2.0f * k;
            dl->AddRectFilled(ImVec2(a.x + 16 - g, a.y + 19 - g), ImVec2(a.x + 16 + art + g, a.y + 19 + art + g), IM_COL32(0, 0, 0, 18), 12.0f + g);
        }
        cover(dl, md, ImVec2(a.x + 16, a.y + 16), art, 12.0f);
        const float tx = a.x + 16 + art + 12, tw = b.x - tx - 14;
        if (!md.supported) dl->AddText(f.small, fontPx(f.small), ImVec2(tx, a.y + 20), kMuted, "Media controls are not available on this computer.", nullptr, tw);
        else if (!md.active) { textAt(dl, f.bold, tx, a.y + 22, kInk, "Not Playing"); dl->AddText(f.small, fontPx(f.small), ImVec2(tx, a.y + 44), kMuted, "Play something in any player.", nullptr, tw); }
        else {
            textAt(dl, f.small, tx, a.y + 18, IM_COL32(255, 140, 165, 255), fitText(f.small, md.app.empty() ? std::string("Now Playing") : md.app, tw).c_str());
            const double since = trackSince(md);
            marquee(dl, f.bold, tx, a.y + 36, tw, kInk, md.title, since);
            marquee(dl, f.small, tx, a.y + 58, tw, kMuted, md.artist, since);
        }
        if (md.active) progressBar(dl, f, md, ImVec2(a.x + 16, a.y + 104), b.x - 16, "##ccprog", true, act);
        playerButtons(dl, md, ImVec2(std::floor((a.x + b.x) * 0.5f), a.y + 140), 18.0f, "##ccmp", act);
        const float volY = a.y + kCcMediaBase - 38;   // the media's volume (or, when the player keeps it to itself, the radio's line), then the computer's
        if (md.active && md.playerVolume >= 0) { float v = -1; volumeSlider(dl, f, "##ccvol", ImVec2(a.x + 10, volY), b.x - 12, md.playerVolume, v, false, !md.ducked); if (v >= 0) act.mediaPlayerVol = v; }
        else textCentered(dl, f.small, (a.x + b.x) * 0.5f, volY + 14, md.radioRx || md.radioTx ? kWarn : kMuted, fitText(f.small, radioLine(md), colL - 24).c_str());
        if (md.masterVolume >= 0) { float v = -1; volumeSlider(dl, f, "##ccmaster", ImVec2(a.x + 10, volY + 34), b.x - 12, md.masterVolume, v, true); if (v >= 0) act.mediaMasterVol = v; }
        // the Music app
        ImGui::SetCursorScreenPos(ImVec2(a.x + 16, a.y + 16));
        if (ImGui::InvisibleButton("##ccart", ImVec2(art, art))) { g_ccTarget = 0; openApp(APP_MUSIC); }
    }
    {   // the screen's brightness: the slider, and auto dim (as in Settings, Display)
        const float by = music ? top + 34 + g_ccMediaH + kCcGap : top + 34;
        const ImVec2 a(m, by), b(m + colL, by + kCcBrightH);
        glass(dl, a, b, 24.0f);
        ImGui::SetCursorScreenPos(a);   // the card: Settings, Display
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##ccbrightcard", ImVec2(b.x - a.x, b.y - a.y))) { g_ccTarget = 0; g_settingsTab = 1; openApp(APP_SETTINGS); }
        textAt(dl, f.bold, a.x + 16, a.y + 12, kInk, "Brightness");
        float br = g_settings.autoDim ? s.autoDimLevel : std::max(0.1f, std::min(1.0f, g_settings.brightness));
        char txt[16];
        std::snprintf(txt, sizeof txt, "%d%%", (int)std::lround(br * 100));
        if (smoothSlider(dl, f, "##ccbright", ImVec2(a.x + 4, a.y + 44), ImVec2(b.x - 12 - 96 - 8, a.y + 76), br, 0.1f, 1.0f, 0, !g_settings.autoDim, txt) && !g_settings.autoDim) {
            g_settings.brightness = br; g_settingsChanged = true;
        }
        // auto dim, right of the slider as in Settings, Display
        if (pill(dl, f, "##ccautodim", "Auto Dim", ImVec2(b.x - 12 - 96, a.y + 45), ImVec2(b.x - 12, a.y + 75), g_settings.autoDim, true)) { g_settings.autoDim = !g_settings.autoDim; g_settingsChanged = true; }
    }
    {   // the flight's progress, under the brightness (only with a flight plan)
        const Progress pr = flightProgress(s);
        if (pr.ok) {
            const float py = (music ? top + 34 + g_ccMediaH + kCcGap : top + 34) + kCcBrightH + kCcGap;
            const ImVec2 a(m, py), b(m + colL, py + 70);
            glass(dl, a, b, 24.0f);
            ImGui::SetCursorScreenPos(a);   // the card: the Flight Plan
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##ccflightcard", ImVec2(b.x - a.x, b.y - a.y))) { g_ccTarget = 0; openApp(APP_ROUTES); }
            const std::string welcome = welcomeAirport(s, pr);
            textAt(dl, f.bold, a.x + 16, a.y + 10, welcome.empty() ? kInk : accentLite(255), welcome.empty() ? "Flight" : ("Welcome to " + welcome).c_str());
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
    else if (g_notes.empty()) textCentered(dl, f.small, (nx0 + nx1) * 0.5f, y + 20, kMuted, "No Notifications");
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
    { float sc = g_ccScroll; touchScroll(sc, ImVec2(nx0, listTop), ImVec2(nx1 + 16, listBot)); g_ccScroll = std::max(0.0f, std::min(std::max(0.0f, content - (listBot - listTop)), sc)); }
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


// ---- a Custom theme's editor: its background and accent on colour wheels, a picture (from plugins/XPTablet/backgrounds), and a preview
int g_customEdit = -1;   // the Custom being edited (0-2), -1 none
double g_customEditT0 = -10;
bool colorWheel(ImDrawList* dl, const Fonts& f, const char* id, ImVec2 c, float r, int& rgb, float value, const char* label) {
    const int v0 = dl->VtxBuffer.Size;
    dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 255), 64);
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) {   // each point: the hue round it, the colour's strength out from the middle
        ImDrawVert& vx = dl->VtxBuffer[i];
        const float dx = vx.pos.x - c.x, dy = vx.pos.y - c.y;
        const float hue = std::fmod(std::atan2(dy, dx) / (2 * 3.14159265f) + 1.0f, 1.0f), sat = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / r);
        float cr, cg, cbl; ImGui::ColorConvertHSVtoRGB(hue, sat, value, cr, cg, cbl);
        vx.col = (vx.col & IM_COL32_A_MASK) | IM_COL32((int)(cr * 255), (int)(cg * 255), (int)(cbl * 255), 0);
    }
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 70), 64, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    ImGui::InvisibleButton(id, ImVec2(r * 2, r * 2));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float dx = mp.x - c.x, dy = mp.y - c.y;
        const float hue = std::fmod(std::atan2(dy, dx) / (2 * 3.14159265f) + 1.0f, 1.0f), sat = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / r);
        float cr, cg, cbl; ImGui::ColorConvertHSVtoRGB(hue, sat, value, cr, cg, cbl);
        const int nv = ((int)(cr * 255) << 16) | ((int)(cg * 255) << 8) | (int)(cbl * 255);
        if (nv != rgb) { rgb = nv; changed = true; }
    }
    float hh, ss, vv;
    ImGui::ColorConvertRGBtoHSV(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, hh, ss, vv);
    const ImVec2 k(c.x + std::cos(hh * 2 * 3.14159265f) * ss * r, c.y + std::sin(hh * 2 * 3.14159265f) * ss * r);
    dl->AddCircleFilled(k, 6.0f, rgbCol(rgb), 16);
    dl->AddCircle(k, 6.0f, IM_COL32(0, 0, 0, 200), 16, 2.5f);
    dl->AddCircle(k, 6.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
    textCentered(dl, f.small, c.x, c.y + r + 6, kMuted, label);
    return changed;
}
std::string g_hexBuf[2];        // the Custom editor's hex boxes (background, accent), as typed
int g_hexShown[2] = {-1, -1};   // the colour each box last showed
void customThemeEditor(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    Settings::CustomTheme& ct = g_settings.customs[g_customEdit];
    const bool hexTyping = g_kbd && g_kbdTarget == 2 && (g_typeInto == &g_hexBuf[0] || g_typeInto == &g_hexBuf[1]);
    // typing a hex code: the card moves up just far enough for the colour bubbles to clear the keys (its top stays on the screen)
    const float liftNeed = (kStatusH + h) * 0.5f - 200.0f + 62 + 124 + 74 + 8 - (h - kKbdH);
    const float lift = std::max(0.0f, std::min(liftNeed, (kStatusH + h) * 0.5f - 200.0f - 6.0f)) * openness("##ctlift", hexTyping);
    const float e0 = std::min(1.0f, (float)(ImGui::GetTime() - g_customEditT0) / 0.22f), e = 1 - (1 - e0) * (1 - e0) * (1 - e0);
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(150 * e)));
    const float pw = 700, ph = 400, sc = 0.9f + 0.1f * e;
    const ImVec2 c(w * 0.5f, (kStatusH + h) * 0.5f - lift);
    const ImVec2 a(c.x - pw * 0.5f * sc, c.y - ph * 0.5f * sc), b(c.x + pw * 0.5f * sc, c.y + ph * 0.5f * sc);
    dl->AddRectFilled(a, b, themeTone(14, 26, 50, (int)(240 * e)), 22.0f);
    glass(dl, a, b, 22.0f, e);
    // (its press catcher, over the whole screen, is the last thing submitted: everything on the card takes its presses first,
    // and nothing under the card gets any; only Done closes it)
    struct Catch { float w, h; ~Catch() { ImGui::SetCursorScreenPos(ImVec2(0, kStatusH)); ImGui::InvisibleButton("##ctveil", ImVec2(w, h - kStatusH)); } } catcher{w, h};
    char title[32]; std::snprintf(title, sizeof title, "Custom %d", g_customEdit + 1);
    textAt(dl, f.title, a.x + 20, a.y + 16, kInk, title);
    if (pill(dl, f, "##ctdone", "Done", ImVec2(b.x - 100, a.y + 14), ImVec2(b.x - 18, a.y + 44), true, true)) { g_customEdit = -1; g_settingsChanged = true; return; }
    // the preview: the home screen in small, with this theme (its picture too), its accent on a button and a switch
    const ImVec2 pa(a.x + 20, a.y + 62);
    const float pvw = 300, pvh = 180;
    const int keepTheme = g_settings.wallpaper;
    g_settings.wallpaper = 7 + g_customEdit;
    dl->PushClipRect(pa, ImVec2(pa.x + pvw, pa.y + pvh), true);
    wallpaperIn(dl, pa, pvw, pvh, 7 + g_customEdit, 10.0f);
    dl->AddRectFilled(pa, ImVec2(pa.x + pvw, pa.y + 12), IM_COL32(0, 0, 0, 90), 10.0f, ImDrawFlags_RoundCornersTop);
    for (int i = 0; i < 6; ++i) {
        static const int apps[6] = {APP_MAP, APP_AIRPORTS, APP_ROUTES, APP_GROUND, APP_CHECKLISTS, APP_NOTES};
        drawTile(dl, f, apps[i], ImVec2(pa.x + 30 + i * 48, pa.y + 46), 30, false, 1.0f, 1.0f);
    }
    dl->AddRectFilled(ImVec2(pa.x + 60, pa.y + pvh - 46), ImVec2(pa.x + pvw - 60, pa.y + pvh - 10), IM_COL32(255, 255, 255, 30), 12.0f);
    for (int i = 0; i < 4; ++i) {
        static const int dock[4] = {APP_WEATHER, APP_SIMBRIEF, APP_SETTINGS, APP_MUSIC};
        drawTile(dl, f, dock[i], ImVec2(pa.x + 90 + i * 40, pa.y + pvh - 28), 26, false, 1.0f, 1.0f);
    }
    dl->AddRectFilled(ImVec2(pa.x + 24, pa.y + 82), ImVec2(pa.x + 124, pa.y + 104), accentCol(230), 11.0f);
    textCentered(dl, f.small, pa.x + 74, pa.y + 85, onAccent(), "Button");
    dl->AddRectFilled(ImVec2(pa.x + 140, pa.y + 84), ImVec2(pa.x + 176, pa.y + 102), accentCol(230), 9.0f);
    dl->AddCircleFilled(ImVec2(pa.x + 167, pa.y + 93), 7, IM_COL32(255, 255, 255, 255), 0);
    dl->AddRectFilled(ImVec2(pa.x + 190, pa.y + 82), ImVec2(pa.x + pvw - 24, pa.y + 104), themeTone(10, 22, 44, 210), 8.0f);
    textAt(dl, f.small, pa.x + 198, pa.y + 85, kMuted, "A Card");
    dl->PopClipRect();
    dl->AddRect(pa, ImVec2(pa.x + pvw, pa.y + pvh), IM_COL32(255, 255, 255, 60), 10.0f, 0, 1.0f);
    g_settings.wallpaper = keepTheme;
    textCentered(dl, f.small, pa.x + pvw * 0.5f, pa.y + pvh + 8, kMuted, "Preview");
    // the wheels
    const float wr = 62, bgCx = pa.x + pvw + 40 + wr, acCx = pa.x + pvw + 82 + wr * 3;
    for (float cx : {bgCx, acCx}) {   // each in a bubble of its own: the wheel, its name and its hex code
        const ImVec2 ba(cx - 78, pa.y - 6), bb(cx + 78, pa.y + wr * 2 + 74);
        dl->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 12), 18.0f);
        dl->AddRect(ba, bb, IM_COL32(255, 255, 255, 34), 18.0f, 0, 1.0f);
    }
    if (colorWheel(dl, f, "##ctbg", ImVec2(bgCx, pa.y + wr + 4), wr, ct.bg, 0.45f, "Background")) g_settingsChanged = true;
    if (colorWheel(dl, f, "##ctac", ImVec2(acCx, pa.y + wr + 4), wr, ct.accent, 1.0f, "Accent")) g_settingsChanged = true;
    // under each wheel, its colour as a hex code: it follows the wheel, and a code typed in (6 digits) sets the colour
    auto hexBox = [&](int k, const char* id, int& rgb, float cx) {
        std::string& v = g_hexBuf[k];
        const bool typing = g_kbd && g_kbdTarget == 2 && g_typeInto == &v;
        if (!typing && g_hexShown[k] != rgb) { char t[8]; std::snprintf(t, sizeof t, "%06X", rgb & 0xFFFFFF); v = t; g_hexShown[k] = rgb; }
        static bool wasTyping[2] = {false, false};
        if (typing && !wasTyping[k]) v.clear();   // (tapped: a fresh code, as a selected field would be typed over)
        wasTyping[k] = typing;
        if (typing) {   // only hex digits, upper case, at most six
            std::string clean;
            for (char ch : v) if (std::isxdigit((unsigned char)ch) && clean.size() < 6) clean += (char)std::toupper((unsigned char)ch);
            v = clean;
            if (v.size() == 6) { const int nv = (int)std::strtol(v.c_str(), nullptr, 16); if (nv != rgb) { rgb = nv; g_settingsChanged = true; } g_hexShown[k] = rgb; }
        }
        const ImVec2 a0(cx - 68, pa.y + wr * 2 + 30), a1(cx + 68, pa.y + wr * 2 + 60);
        dl->AddRectFilled(ImVec2(a0.x, a0.y + 6), ImVec2(a0.x + 18, a0.y + 24), rgbCol(rgb), 4.0f);   // (a swatch of it)
        dl->AddRect(ImVec2(a0.x, a0.y + 6), ImVec2(a0.x + 18, a0.y + 24), IM_COL32(255, 255, 255, 80), 4.0f);
        textAt(dl, f.body, a0.x + 24, std::floor((a0.y + a1.y) * 0.5f - fontPx(f.body) * 0.5f), kMuted, "#");
        textField(dl, f, id, v, ImVec2(a0.x + 38, a0.y), a1, "RRGGBB", false);
        if (typing && v.size() < 6) textCentered(dl, f.small, cx, a1.y + 1, kWarn, "6 digits");
    };
    hexBox(0, "##cthexbg", ct.bg, bgCx);
    hexBox(1, "##cthexac", ct.accent, acCx);
    // the picture: none, or one from the backgrounds folder (put there from your computer)
    float py = pa.y + pvh + 34;
    textAt(dl, f.bold, a.x + 20, py, kInk, "Picture");
    float px = a.x + 122;
    auto pick = [&](const char* id, const std::string& label, const std::string& file) {
        const float bw = std::min(160.0f, f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, label.c_str()).x + 28);
        if (px + bw > b.x - 20) { px = a.x + 122; py += 36; }
        if (py > b.y - 40) return;
        if (pill(dl, f, id, fitText(f.small, label, bw - 16).c_str(), ImVec2(px, py - 4), ImVec2(px + bw, py + 24), ct.picture == file, true) && ct.picture != file) { ct.picture = file; g_settingsChanged = true; }
        px += bw + 6;
    };
    {   // the refresh: the folder looked at again (pictures added since show), beside the word Picture
        const ImVec2 ra(a.x + 20 + 64, py - 4), rb(ra.x + 28, py + 24);
        if (iconButton("##ctlook", ra, rb, true)) act.backgroundsList = true;
        reloadIcon(dl, ImVec2((ra.x + rb.x) * 0.5f, (ra.y + rb.y) * 0.5f), kInk);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look in the backgrounds folder again");
    }
    pick("##ctpnone", "None", "");
    for (size_t i = 0; i < s.backgroundFiles.size() && i < 12; ++i) { char id[24]; std::snprintf(id, sizeof id, "##ctp%zu", i); pick(id, s.backgroundFiles[i], s.backgroundFiles[i]); }
    const std::string where = "Import a picture (PNG or JPG): put it in the backgrounds folder in XPTablet's plugin folder, " + (s.backgroundsFolder.empty() ? std::string("plugins/XPTablet/backgrounds") : s.backgroundsFolder);
    dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 20, b.y - 44), kMuted, where.c_str(), nullptr, b.x - a.x - 40);
    if (hexTyping) { dl->AddRectFilled(ImVec2(0, h - kKbdH), ImVec2(w, h), themeTone(8, 16, 30, 255)); pageKeyboard(dl, f, w, h, act); }   // (a solid backing: the card does not show through the keys; before the card's press catcher, so the keys take their presses)
}

void settingsBody(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act);
// Settings; with a Custom theme's editor open, the editor first (it takes the presses before anything under it) but drawn on top
// what was drawn since vtx0 faded in (t 0..1): a page's content changing (a Settings tab, an App Store category)
void fadeSince(ImDrawList* dl, int vtx0, float t) {
    if (t >= 0.999f) return;
    const unsigned am = (unsigned)(std::max(0.0f, std::min(1.0f, 1 - (1 - t) * (1 - t))) * 256.0f);
    for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const unsigned a = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) * am >> 8;
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}
// ---- things kept out of sight in Settings (the SimBrief ID, Hoppie's code, Volanta's address, Local Connect's address): dots until
// their eye is tapped, dots again when it is tapped once more (a toggle), and hidden again whenever another tab is opened
unsigned g_reveal = 0;   // a bit for each: 0 SimBrief, 1 Hoppie, 2 Volanta, 3 Local Connect's address
bool eyeToggle(ImDrawList* dl, const char* id, ImVec2 ea, ImVec2 eb, bool shown) {
    ImGui::SetCursorScreenPos(ea);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(eb.x - ea.x, eb.y - ea.y));
    const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    const Ease e = eased(id, shown, hot, down);
    dl->AddRectFilled(ea, eb, mixCol(IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 34), std::max(e.hot, e.on * 0.6f)), 8.0f);
    const ImVec2 ec(std::floor((ea.x + eb.x) * 0.5f), std::floor((ea.y + eb.y) * 0.5f));
    const ImU32 col = mixCol(kMuted, accentLite(255), e.on);
    dl->PathArcTo(ImVec2(ec.x, ec.y + 6), 11, 3.14159f * 1.22f, 3.14159f * 1.78f, 12); dl->PathStroke(col, 0, 1.6f);
    dl->PathArcTo(ImVec2(ec.x, ec.y - 6), 11, 3.14159f * 0.22f, 3.14159f * 0.78f, 12); dl->PathStroke(col, 0, 1.6f);
    dl->AddCircleFilled(ec, 3.2f, col, 0);
    if (e.on < 0.98f) {   // hidden: a stroke through the eye (drawn away as it opens)
        const float k = 1 - e.on;
        dl->AddLine(ImVec2(ec.x - 9 * k, ec.y + 8 * k), ImVec2(ec.x + 9 * k, ec.y - 8 * k), (col & 0x00FFFFFF) | ((ImU32)(255 * k) << 24), 1.8f);
    }
    return hit;
}
// a text field hidden as dots, its eye at its right; shown as typed while it is typed into. True while it is being typed into.
bool hiddenField(ImDrawList* dl, const Fonts& f, const char* id, std::string& v, ImVec2 a, ImVec2 b, const char* hint, int bit) {
    static std::string dots[8];
    const bool typing = g_kbd && g_kbdTarget == 2 && g_typeInto == &v;
    const bool shown = (g_reveal >> bit) & 1u;
    const ImVec2 fb(b.x - 44, b.y);
    if (typing || shown) textField(dl, f, id, v, a, fb, hint, false, 0);
    else {
        std::string& d = dots[bit & 7];
        d.clear();
        for (size_t i = 0; i < v.size() && i < 40; ++i) d += "\xE2\x80\xA2";
        textField(dl, f, id, d, a, fb, hint, false, 0);
        if (g_typeInto == &d) g_typeInto = &v;   // (tapped: it is typed into, shown as typed)
    }
    char eid[48];
    std::snprintf(eid, sizeof eid, "%s_eye", id);
    if (eyeToggle(dl, eid, ImVec2(b.x - 40, a.y + 2), ImVec2(b.x - 2, b.y - 2), shown)) g_reveal ^= 1u << bit;
    return typing;
}

void settingsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    static int shownTab = -1; static double tabT0 = -10;
    if (g_settingsTab != shownTab) { shownTab = g_settingsTab; tabT0 = ImGui::GetTime(); }
    if (g_customEdit < 0) {   // (a tab chosen: its content fades in)
        const int v0 = dl->VtxBuffer.Size;
        settingsBody(dl, f, s, w, h, act);
        fadeSince(dl, v0, std::min(1.0f, (float)(ImGui::GetTime() - tabT0) / 0.18f) * 0.65f + 0.35f);
        return;
    }
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    customThemeEditor(dl, f, s, w, h, act);
    dl->ChannelsSetCurrent(0);
    settingsBody(dl, f, s, w, h, act);
    dl->ChannelsMerge();
}
void settingsBody(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    {   // the pages, down a sidebar as on an iPad: General, Display, Data at the top, Credits at the bottom
        const float sTop = kStatusH + kHeaderH + 10, sBot = h - 12, sW = 168;
        card(dl, ImVec2(12, sTop), ImVec2(12 + sW, sBot), f, nullptr);
        // the pages: General, Display, Data, Notifications, Media (not on macOS) at the top; Credits at the bottom
        struct Tab { int id; const char* name; ImU32 tint; int glyph; };
        std::vector<Tab> tabs = {{0, "General", IM_COL32(110, 120, 140, 255), APP_SETTINGS}, {1, "Display", IM_COL32(64, 132, 230, 255), APP_WEATHER},
                                 {2, "Data", IM_COL32(60, 190, 130, 255), APP_DOCUMENTS}, {4, "Notifications", IM_COL32(240, 70, 70, 255), APP_NOTES}};
        if (!g_mac) tabs.push_back({5, "Media", IM_COL32(250, 45, 85, 255), APP_MUSIC});
        if (s.local.status != -2) tabs.push_back({6, "Local Connect", IM_COL32(24, 150, 170, 255), APP_BROWSER});   // (not shown until the host can serve it)
        else if (g_settingsTab == 6) g_settingsTab = 0;
        tabs.push_back({7, "Accounts", IM_COL32(150, 96, 250, 255), APP_SETTINGS});
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
            if (ImGui::InvisibleButton(id, ImVec2(e.x - a.x, e.y - a.y)) && g_settingsTab != i) { g_settingsTab = i; g_kbd = false; g_typeInto = nullptr; g_reveal = 0; }
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
        // the web apps: got or removed in the App Store now
        textAt(dl, f.title, lx, y, kInk, "On The Home Screen");
        y += 34;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Charts, flight planning, live maps, networks, music and video: get them, or remove them, in the App Store. Hold an icon on the home screen to arrange them.", nullptr, colW);
        y += 58;
        if (pill(dl, f, "##genstore", "Open The App Store", ImVec2(lx, y), ImVec2(lx + colW, y + 32), true, true)) openApp(APP_APPSTORE);
        // status
        float sy = top;
        textAt(dl, f.bold, rx, sy, kInk, "Status");
        sy += 24;
        char b[96];
        auto line = [&](const char* k, const char* v) { textAt(dl, f.mono, rx, sy, kMuted, k); textAt(dl, f.mono, rx + 100, sy, kInk, v); sy += 18; };
        line("Version", XPT_VERSION);
        std::snprintf(b, sizeof b, "%s%s", s.acfIcao.empty() ? "-" : s.acfIcao.c_str(), s.toliss ? " (ToLiss)" : "");
        line("Aircraft", b);
        line("Shown On", s.onPanel ? "cockpit tablet" : "window only");
        if (s.panel[2] > 0 && s.panel[3] > 0) { std::snprintf(b, sizeof b, "%d,%d  %dx%d", s.panel[0], s.panel[1], s.panel[2], s.panel[3]); line("Panel Area", b); }
        else line("Panel Area", "none (no AviTab slot)");
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
        textCentered(dl, f.small, (a.x + e.x) * 0.5f, a.y + 8, themeText(200, 212, 232, 0.15f), g_touches.empty() ? "Touch Test: press and drag here" : "Touch Test");
        if (!g_touches.empty() && pill(dl, f, "##touchclr", "Clear", ImVec2(e.x - 74, e.y - 36), ImVec2(e.x - 10, e.y - 8), false, true)) g_touches.clear();
        return;
    }

    if (g_settingsTab == 4) {   // ---- Notifications
        float y = top;
        toggle("##ntfon", lx, y, colW, g_settings.notifyEnabled ? "Notifications: On" : "Notifications: Off", g_settings.notifyEnabled, true);
        y += 42;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "A banner drops in under the status bar and fades after 3 seconds: tap it to open its page, push it up to put it away. Pull down from the logo for the list.", nullptr, colW);
        y += 56;
        textAt(dl, f.bold, lx, y, g_settings.notifyEnabled ? kInk : kMuted, "Alerts");
        y += 24;
        toggle("##ntftcas", lx, y, colW, "TCAS Alert", g_settings.notifyTcas, g_settings.notifyEnabled);
        y += 36;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Traffic in TCAS resolution-advisory range. Opens the Map.", nullptr, colW);
        y += 30;
        toggle("##ntfsb", lx, y, colW, "New SimBrief Flight Plan", g_settings.notifySimbrief, g_settings.notifyEnabled);
        y += 36;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "SimBrief is checked every 2 minutes with your SimBrief ID; a new plan is loaded. Opens the Flight Plan.", nullptr, colW);
        y += 34;
        if (g_settings.simbriefUser.empty()) textAt(dl, f.small, lx, y, kWarn, "Enter your SimBrief ID under Accounts first.");
        y += 26;
        toggle("##ntfatc", lx, y, colW, "VATSIM ATC Online", g_settings.vatsimAtcNotes, g_settings.notifyEnabled);
        y += 36;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "A controller comes online at your departure or destination. Opens Airports.", nullptr, colW);
        // the right: try one
        float ry = top;
        textAt(dl, f.bold, rx, ry, kInk, "Try It");
        ry += 26;
        if (pill(dl, f, "##ntftest", "Send A Test Notification", ImVec2(rx, ry), ImVec2(w - 18, ry + 30), false, g_settings.notifyEnabled))
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
    if (g_settingsTab == 7) {   // ---- Accounts: the keys and codes other services give you (kept on this computer, shown as dots)
        float y = top;
        auto secret = [&](const char* id, const char* title, const char* what, std::string& v, const char* hint) {
            textAt(dl, f.bold, lx, y, kInk, title);
            y += 22;
            dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, what, nullptr, w - 12 - lx);
            y += 38;
            const bool typing = g_kbd && g_kbdTarget == 2 && g_typeInto == &v;
            hiddenField(dl, f, id, v, ImVec2(lx, y), ImVec2(w - 12, y + 34), hint, 1);
            y += 46;
            return typing;
        };
        const bool sbTyping = g_kbd && g_kbdTarget == 2 && g_typeInto == &g_settings.simbriefUser;
        {   // SimBrief: the ID the Flight Plan page imports with (hidden as dots unless typed or its eye is on), and an import now
        textAt(dl, f.bold, lx, y, kInk, "SimBrief ID");
        y += 22;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Your SimBrief user name or pilot ID (Account settings on simbrief.com). The Flight Plan page imports your latest plan with it.", nullptr, w - 12 - lx);
        y += 36;
        hiddenField(dl, f, "##sbuser", g_settings.simbriefUser, ImVec2(lx, y), ImVec2(w - 12 - 200, y + 36), "Tap to enter your SimBrief ID", 0);
        if (g_typeChanged) { g_typeChanged = false; g_settingsChanged = true; }
        if (pill(dl, f, "##sbimport", s.plan.busy ? "Importing..." : "Import Flight Plan", ImVec2(w - 12 - 192, y + 2), ImVec2(w - 12, y + 34), true, !g_settings.simbriefUser.empty() && !s.plan.busy)) {
            act.planImport = true; g_kbd = false; g_typeInto = nullptr;
        }
        y += 44;
        if (!s.plan.status.empty()) { textAt(dl, f.small, lx, y, s.plan.loaded ? kGood : kMuted, s.plan.status.c_str()); }
        y += 26;
        if (sbTyping) { pageKeyboard(dl, f, w, h, act); return; }
        y += 4;
        }
        const bool typing = secret("##achop", "Hoppie ACARS Logon Code", "For the ACARS app (CPDLC and telex on VATSIM and IVAO): register at hoppie.nl/acars, and the logon code comes by e-mail.", g_settings.hoppieCode, "Tap to enter your Hoppie logon code");
        if (g_typeChanged) { g_typeChanged = false; g_settingsChanged = true; }
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Kept only in XPTablet's settings on this computer, and sent only to the service each is for.", nullptr, w - 12 - lx);
        if (typing) pageKeyboard(dl, f, w, h, act);
        return;
    }
    if (g_settingsTab == 6) {   // ---- Local Connect: the tablet on a phone or tablet on the same network
        const LocalConnectState& L = s.local;
        float y = top;
        toggle("##lcon", lx, y, colW, g_settings.localConnect ? "Local Connect: On" : "Local Connect: Off", g_settings.localConnect, true);
        y += 42;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "Use this tablet on your phone or iPad: on the same Wi-Fi, open the address below in Safari or Chrome. The browser warns once that the connection is not private (the certificate is your own computer's): choose Advanced or Show Details, then continue. The device shows this tablet and works it by touch. Only your home network can reach it, and each device is paired once with a code shown here.", nullptr, colW);
        y += 100;
        textAt(dl, f.bold, lx, y, kInk, "Address");
        y += 24;
        {   // the address, large, in a field of its own
            const ImVec2 a(lx, y), e(lx + colW, y + 44);
            dl->AddRectFilled(a, e, themeTone(10, 20, 38, 255), 12.0f);
            dl->AddRect(a, e, IM_COL32(255, 255, 255, 30), 12.0f, 0, 1.0f);
            const bool on = g_settings.localConnect && L.status == 2 && !L.address.empty();
            const char* txt = !g_settings.localConnect ? "Switch Local Connect on" : on ? L.address.c_str() : L.status == 1 ? "Starting..." : L.address.empty() ? "No network found" : L.address.c_str();
            const bool shown = (g_reveal >> 3) & 1u;   // (hidden as dots until its eye is on: a screen being shared need not show it)
            static std::string dots;
            if (on && !shown) { dots.clear(); for (size_t i = 0; i < L.address.size() && i < 28; ++i) dots += "\xE2\x80\xA2"; txt = dots.c_str(); }
            textAt(dl, on ? f.title : f.body, a.x + 14, std::floor((a.y + e.y) * 0.5f - fontPx(on ? f.title : f.body) * 0.5f), on ? kInk : kMuted, txt);
            if (on && eyeToggle(dl, "##lcaddr_eye", ImVec2(e.x - 44, a.y + 4), ImVec2(e.x - 4, e.y - 4), shown)) g_reveal ^= 1u << 3;
            y += 54;
        }
        // the status: a dot and a line
        textAt(dl, f.bold, lx, y, kInk, "Status");
        y += 26;
        int connected = 0;
        const LocalDevice* first = nullptr;
        for (const LocalDevice& d : L.devices) if (d.connected) { ++connected; if (!first) first = &d; }
        std::string st;
        ImU32 dot = kMuted;
        if (!g_settings.localConnect) st = "Off";
        else if (L.status == -2) { st = "Not available in this version yet"; dot = kWarn; }
        else if (L.status == -1) { st = "Cannot Start" + (L.error.empty() ? std::string() : ": " + L.error); dot = IM_COL32(240, 90, 90, 255); }
        else if (L.status == 1) { st = "Starting..."; dot = kWarn; }
        else if (connected == 1) { st = "Connected: " + first->name; dot = kGood; }
        else if (connected > 1) { st = "Connected: " + first->name + " and " + std::to_string(connected - 1) + " more"; dot = kGood; }
        else { st = "Waiting For A Device"; dot = accentLite(255); }
        dl->AddCircleFilled(ImVec2(lx + 6, y + 9), 5.0f, dot, 0);
        dl->AddText(f.body, fontPx(f.body), ImVec2(lx + 20, y), kInk, st.c_str(), nullptr, colW - 20);
        y += 34;
        if (g_settings.localConnect && !L.pairCode.empty()) {   // a device asking to pair: its code, to type on it
            const ImVec2 a(lx, y), e(lx + colW, y + 92);
            dl->AddRectFilled(a, e, accentCol(40), 12.0f);
            dl->AddRect(a, e, accentLite(120), 12.0f, 0, 1.0f);
            const std::string who = (L.pairName.empty() ? std::string("A device") : L.pairName) + " wants to connect. Enter on it:";
            textAt(dl, f.small, a.x + 12, a.y + 10, kInk, who.c_str());
            std::string code = L.pairCode;
            if (code.size() == 6) code = code.substr(0, 3) + " " + code.substr(3);
            textAt(dl, f.title, a.x + 12, a.y + 34, kInk, code.c_str());
            if (pill(dl, f, "##lcdeny", "Not Me", ImVec2(e.x - 96, a.y + 50), ImVec2(e.x - 10, a.y + 80), false, true)) act.localPairDeny = true;
            y += 102;
        }
        // the right: the devices, and the battery
        float ry = top;
        textAt(dl, f.title, rx, ry, kInk, "Devices");
        ry += 34;
        if (L.devices.empty()) { dl->AddText(f.small, fontPx(f.small), ImVec2(rx, ry), kMuted, "None paired yet.", nullptr, colW); ry += 30; }
        for (size_t i = 0; i < L.devices.size() && i < 4; ++i) {
            const LocalDevice& d = L.devices[i];
            const ImVec2 a(rx, ry), e(rx + colW, ry + 58);
            dl->AddRectFilled(a, e, themeTone(10, 20, 38, 255), 12.0f);
            dl->AddCircleFilled(ImVec2(a.x + 14, a.y + 18), 4.5f, d.connected ? kGood : IM_COL32(120, 130, 150, 255), 0);
            textAt(dl, f.bold, a.x + 26, a.y + 8, kInk, d.name.c_str());
            std::string sub = d.kind.empty() ? std::string() : d.kind;
            if (d.connected) sub += sub.empty() ? "Connected" : "  \xC2\xB7  Connected";
            else if (!d.lastSeen.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + std::string("Last seen ") + d.lastSeen;
            textAt(dl, f.small, a.x + 26, a.y + 32, kMuted, sub.c_str());
            if (d.connected && d.batteryPct >= 0) {   // its battery, as its own status bar shows it
                char bb[24]; std::snprintf(bb, sizeof bb, "%d%%%s", d.batteryPct, d.charging ? ", charging" : "");
                textRight(dl, f.small, e.x - 96, a.y + 10, d.batteryPct <= 20 && !d.charging ? kWarn : kInk, bb);
            }
            char id[24]; std::snprintf(id, sizeof id, "##lcforget%zu", i);
            if (pill(dl, f, id, "Remove", ImVec2(e.x - 86, a.y + 14), ImVec2(e.x - 10, a.y + 44), false, true)) act.localForget = d.name;
            ry += 66;
        }
        ry += 8;
        textAt(dl, f.bold, rx, ry, kInk, "Battery");
        ry += 24;
        toggle("##lcbatt", rx, ry, colW, "Use The Device's Battery", g_settings.localBattery, true);
        ry += 38;
        dl->AddText(f.small, fontPx(f.small), ImVec2(rx, ry), kMuted, "With a device connected, the tablet shows that device's charge and whether it is charging, instead of its own. Android's Chrome and other Chromium browsers share these; iPhone and iPad browsers do not, so there the tablet keeps its own battery.", nullptr, colW);
        return;
    }
    if (g_settingsTab == 5 && !g_mac) {   // ---- Media: the Music app and the pull-down's player; what the radio does to the music
        float y = top;
        toggle("##medon", lx, y, colW, g_settings.mediaEnabled ? "Media Controls: On" : "Media Controls: Off", g_settings.mediaEnabled, true);
        y += 42;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, "The Media app and the player in the pull-down: what is playing on this computer (Spotify, a browser, any player), with its cover.", nullptr, colW);
        y += 56;
        textAt(dl, f.bold, lx, y, g_settings.mediaEnabled ? kInk : kMuted, "When The Radios Receive Or Transmit");
        y += 24;
        const char* modes[3] = {"No Change", "Lower Media", "Pause Media"};
        for (int i = 0; i < 3; ++i) {
            char id[16];
            std::snprintf(id, sizeof id, "##duck%d", i);
            if (pill(dl, f, id, modes[i], ImVec2(lx, y), ImVec2(lx + colW, y + 30), g_settings.mediaDuck == i, g_settings.mediaEnabled) && g_settings.mediaEnabled) { g_settings.mediaDuck = i; g_settingsChanged = true; }
            y += 36;
        }
        // lowered: to how much, receiving and transmitting each (greyed unless "Lower Media")
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
        toggle("##autodim", lx + colW - autoW, y, autoW, "Auto Dim", g_settings.autoDim, true);
        y += 34;
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx, y), kMuted, g_settings.autoDim ? "Dims with the daylight: full until sunset, down to 45% once the sun is 8\xC2\xB0 below the horizon." : "Multiplied with the aircraft's own tablet brightness knob. Brighter uses the battery faster.", nullptr, colW);
        y += 40;
        textAt(dl, f.bold, lx, y, kInk, "Favorites Bar");
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
        choice("##chclk", lx, y, colW, "Clock Zone (tap the clock in the status bar too)", clockNames, clk, true);
        if (clk != (g_settings.statusLocalTime ? 1 : 0)) g_settings.statusLocalTime = clk == 1;
        {   // the street map: Light, Dark, or Auto (it fades to Dark with the daylight, as auto dim does)
            textAt(dl, f.bold, lx, y, kInk, "Map Display (the street map; Satellite is set on the map)");
            y += 24;
            static const char* names[3] = {"Light", "Dark", "Auto"};
            const int cur = g_settings.mapDarkAuto ? 2 : g_settings.mapDark ? 1 : 0;
            const float third = std::floor((colW - 16) / 3);
            for (int i = 0; i < 3; ++i) {
                char bid[16]; std::snprintf(bid, sizeof bid, "##chmapd%d", i);
                if (pill(dl, f, bid, names[i], ImVec2(lx + i * (third + 8), y), ImVec2(lx + i * (third + 8) + third, y + 30), cur == i, true) && cur != i) {
                    g_settings.mapDarkAuto = i == 2; if (i < 2) g_settings.mapDark = i == 1; g_settingsChanged = true;
                }
            }
            y += 42;
        }
        const char* keyNames[2] = {"On-Screen Keys", "Physical Keyboard"};
        int pk = g_settings.physicalKeys ? 1 : 0;
        choice("##chkeys", lx, y, colW, "Typing In A Text Box", keyNames, pk, true);
        if ((pk == 1) != g_settings.physicalKeys) { g_settings.physicalKeys = pk == 1; g_kbdKeysShown = false; }
        {   // the right column: the look, a background with its accent (all from the XPTablet logo's colours)
            float ry = top;
            textAt(dl, f.bold, rx, ry, kInk, "Color Theme");
            ry += 24;
            const float tw = std::floor((colW - 4 * 8) / 5), th = std::floor(tw * 480.0f / 800.0f);   // the screen's shape
            // the seasonal themes on now (all of them in the emulator): a row of their own under the ten, five at a time
            std::vector<int> seas;
            for (int k = 0; k < kSeasonalN; ++k) if (seasonalAvailable(k)) seas.push_back(kSeasonal0 + k);
            static int seasPage = 0;
            const int seasPer = 4;   // (four to a page: wider places, so their longer names fit under them)
            const int seasPages = std::max(1, ((int)seas.size() + seasPer - 1) / seasPer);
            seasPage = std::max(0, std::min(seasPage, seasPages - 1));
            std::vector<int> order;
            for (int i = 0; i < 10; ++i) order.push_back(i);
            for (int k = seasPage * seasPer; k < (int)seas.size() && k < seasPage * seasPer + seasPer; ++k) order.push_back(seas[k]);
            static int seasPageShown = 0; static double seasT0 = -10;   // (a page turned: its themes fade in)
            if (seasPageShown != seasPage) { seasPageShown = seasPage; seasT0 = ImGui::GetTime(); }
            int seasV0 = -1;
            for (int slot = 0; slot < (int)order.size(); ++slot) {   // the wallpaper itself, made small, with its accent under it (rows of five)
                const int i = order[slot];
                const Backdrop bd = themeBackdrop(i);
                const int keep = g_settings.wallpaper;
                g_settings.wallpaper = i; const Accent ac = themeAccent(); g_settings.wallpaper = keep;
                const float seasGap = slot >= 10 ? 30.0f : 0.0f;   // (the seasonal row under its own heading)
                const float cellW = std::floor((colW - (seasPer - 1) * 8) / seasPer);
                if (slot == 10) seasV0 = dl->VtxBuffer.Size;
                const ImVec2 a = slot < 10 ? ImVec2(std::floor(rx + (slot % 5) * (tw + 8)), std::floor(ry + (slot / 5) * (th + 40)))   // (on whole pixels: even edges)
                                           : ImVec2(std::floor(rx + (slot - 10) * (cellW + 8) + (cellW - tw) * 0.5f), std::floor(ry + 2 * (th + 40) + seasGap));
                const ImVec2 b(a.x + tw, a.y + th);
                char id[16];
                std::snprintf(id, sizeof id, "##wp%d", i);
                ImGui::SetCursorScreenPos(a);
                const bool tapped = ImGui::InvisibleButton(id, ImVec2(tw, th + 34));
                // a Custom held: its editor (colour wheels, a picture, and how it will look)
                static double heldAt = -1; static int heldOn = -1;
                if (ImGui::IsItemActivated()) { heldAt = ImGui::GetTime(); heldOn = i; }
                bool opened = false;
                if (i >= 7 && i <= 9 && ImGui::IsItemActive() && heldOn == i && heldAt >= 0 && ImGui::GetTime() - heldAt > 0.5) {
                    g_customEdit = i - 7; g_customEditT0 = ImGui::GetTime(); heldAt = -1; opened = true;
                    g_settings.wallpaper = i; g_settings.accent = 4; g_settingsChanged = true; act.backgroundsList = true;
                }
                if (tapped && heldAt >= 0 && !opened) { g_settings.wallpaper = i; g_settings.accent = std::min(i, 4); if (i < kSeasonal0) g_settings.wallpaperBase = i; g_settingsChanged = true; }
                if (ImGui::IsItemDeactivated()) heldAt = -1;
                const bool sel = g_settings.wallpaper == i, hot = ImGui::IsItemHovered();
                dl->PushClipRect(a, b, true);
                wallpaperIn(dl, a, tw, th, i, 8.0f);
                dl->PopClipRect();
                // the frame on the picture's own rounded edge; the chosen one ringed in the theme's accent
                // chosen: a ring that hugs the picture's edge all round (no dark gap between them, the same on every side)
                if (sel) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), accentLite(255), 9.0f, 0, 2.5f);
                else dl->AddRect(a, b, hot ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 40), 8.0f, 0, 1.0f);
                if (i >= 7 && i <= 9) {   // (a pencil: hold to edit)
                    const ImVec2 pc(b.x - 9, a.y + 9);
                    dl->AddCircleFilled(pc, 7, IM_COL32(0, 0, 0, 140), 0);
                    dl->AddLine(ImVec2(pc.x - 3, pc.y + 3), ImVec2(pc.x + 3, pc.y - 3), IM_COL32(255, 255, 255, 230), 1.6f);
                }
                dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, b.y + 10), 5.0f, IM_COL32(ac.r, ac.g, ac.b, 255), 0);
                if (slot < 10) textCentered(dl, f.small, (a.x + b.x) * 0.5f, b.y + 18, sel ? kInk : kMuted, fitText(f.small, bd.name, tw + 6).c_str());
                else {   // a seasonal theme's name: on as many lines as it needs, each centered under its picture
                    std::vector<std::string> lines; std::string cur;
                    std::istringstream in(bd.name); std::string wd;
                    while (in >> wd) {
                        const std::string tryL = cur.empty() ? wd : cur + " " + wd;
                        if (!cur.empty() && f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, tryL.c_str()).x > cellW + 4) { lines.push_back(cur); cur = wd; } else cur = tryL;
                    }
                    if (!cur.empty()) lines.push_back(cur);
                    for (size_t li = 0; li < lines.size() && li < 3; ++li) textCentered(dl, f.small, (a.x + b.x) * 0.5f, b.y + 18 + li * 13.0f, sel ? kInk : kMuted, fitText(f.small, lines[li], cellW + 6).c_str());
                }
            }
            if (seasV0 >= 0) fadeSince(dl, seasV0, std::min(1.0f, (float)(ImGui::GetTime() - seasT0) / 0.2f));
            ry += 2 * (th + 40);
            textAt(dl, f.small, rx, ry - 8, kMuted, "Hold a Custom theme to choose its colors and picture.");
            if (!seas.empty()) {   // the seasonal row's heading, and its pages
                textAt(dl, f.bold, rx, ry + 12, kInk, "Seasonal");
                textAt(dl, f.small, rx + 78, ry + 15, kMuted, "While It Lasts");
                if (seasPages > 1) {   // more than a row of them: under the row, in its middle, a dot for each page between two round arrows
                    const float cx = rx + colW * 0.5f, cy = ry + 30 + th + 76, gap = 12.0f, half = (seasPages - 1) * gap * 0.5f;
                    for (int p = 0; p < seasPages; ++p) dl->AddCircleFilled(ImVec2(cx - half + p * gap, cy), p == seasPage ? 3.5f : 2.5f, p == seasPage ? accentLite(255) : IM_COL32(255, 255, 255, 70), 12);
                    auto arrow = [&](const char* id, float x, int dir, bool enabled) {
                        ImGui::SetCursorScreenPos(ImVec2(x - 17, cy - 17));
                        const bool tapped = ImGui::InvisibleButton(id, ImVec2(34, 34)) && enabled;
                        const bool hot = enabled && ImGui::IsItemHovered();
                        dl->AddCircleFilled(ImVec2(x, cy), 13.0f, IM_COL32(255, 255, 255, !enabled ? 10 : hot ? 46 : 24), 28);
                        const ImU32 ink = enabled ? kInk : IM_COL32(255, 255, 255, 50);
                        const ImVec2 pts[3] = {ImVec2(x - dir * 2.5f, cy - 5.5f), ImVec2(x + dir * 3.0f, cy), ImVec2(x - dir * 2.5f, cy + 5.5f)};
                        dl->AddPolyline(pts, 3, ink, 0, 1.8f);
                        return tapped;
                    };
                    if (arrow("##seasprev", cx - half - 28, -1, seasPage > 0)) --seasPage;
                    if (arrow("##seasnext", cx + half + 28, 1, seasPage + 1 < seasPages)) ++seasPage;
                }
            }
        }
        return;
    }

    // ---- Data: SimBrief, and where the tablet's data comes from
    float y = top;
    // Volanta's stream overlay (its widget on the home screen): the address the Volanta app serves it at
    textAt(dl, f.bold, lx, y, kInk, "Volanta Overlay");
    textAt(dl, f.small, lx + 150, y + 2, kMuted, "For the home screen's Volanta widget (Volanta, Settings, Stream overlay)");
    y += 22;
    hiddenField(dl, f, "##volov", g_settings.volantaOverlay, ImVec2(lx, y), ImVec2(w - 12 - 200, y + 32), "http://localhost:53011", 2);
    if (g_kbd && g_kbdTarget == 2 && g_typeInto == &g_settings.volantaOverlay) { if (g_typeChanged) { g_typeChanged = false; g_settingsChanged = true; } pageKeyboard(dl, f, w, h, act); return; }
    y += 42;
    textAt(dl, f.bold, lx, y, kInk, "Data");
    y += 24;
    for (const auto& kv : s.dataInfo) {
        textAt(dl, f.small, lx, y, kMuted, kv.first.c_str());
        dl->AddText(f.small, fontPx(f.small), ImVec2(lx + 130, y), kInk, kv.second.c_str(), nullptr, w - 12 - lx - 130 - 170);
        y += 21;
    }
    if (pill(dl, f, "##clrtiles", "Clear Map Cache", ImVec2(w - 12 - 150, top + 150), ImVec2(w - 12, top + 182), false, true)) act.clearTileCache = true;
    // the browser's cookies, logins, history and cache, all of it: asked twice, as it signs out of every site
    static double askedAt = -100;
    const double now = ImGui::GetTime();
    const bool asking = now - askedAt < 4.0;
    if (pill(dl, f, "##clrweb", asking ? "Tap Again: Erase" : "Reset Browser Data", ImVec2(w - 12 - 150, top + 190), ImVec2(w - 12, top + 222), asking, true)) {
        if (asking) { act.resetBrowserData = true; askedAt = -100; postNote("Browser Data Erased", "Signed out of every site; cookies, history and cache cleared.", APP_SETTINGS); }
        else askedAt = now;
    }
    if (asking) textRight(dl, f.small, w - 12, top + 228, kWarn, "Signs out of every site");
    // the whole tablet back as it came: every setting, the home screen, themes, notes and drawings, notifications, the battery,
    // the browser's data and the map cache; asked twice, then it starts again (the boot) as new
    static double resetAskedAt = -100;
    const bool resetAsking = now - resetAskedAt < 4.0;
    if (pill(dl, f, "##resetall", resetAsking ? "Tap Again: Reset All" : "Reset All", ImVec2(w - 12 - 150, top + 250), ImVec2(w - 12, top + 282), resetAsking, true)) {
        if (resetAsking) { resetAskedAt = -100; act.resetBrowserData = true; act.clearTileCache = true; resetTablet(); return; }
        resetAskedAt = now;
    }
    textRight(dl, f.small, w - 12, top + 288, resetAsking ? kWarn : kMuted, resetAsking ? "Everything goes back to its defaults" : "Every setting to its default");
}
void resetTablet() {   // Reset All (Settings, Data): defaults everywhere, then the boot as from off
    g_settings = Settings();
    notesFromText(std::string());
    resetLogAndTimers();
    for (int i = 0; i < 6; ++i) g_craft[i].clear();
    for (int i = 0; i < 9; ++i) g_atis[i].clear();
    g_scratch.clear();
    g_notes.clear();
    g_batt = 100.0f; g_charger = false; g_battFromDevice = false; g_ownBatt = 100.0f; g_ownCharger = false;
    g_lowWarned20 = g_lowWarned10 = false; g_lowAlert = 0;
    g_homePage = 0; g_homeScroll = 0; g_openFolder = -1; g_editMode = false; g_dragging = false; g_widgetPicker = false; g_addMenu = false;
    g_customEdit = -1; g_settingsTab = 0;
    g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr;
    normalizeLayout();
    g_settingsChanged = true; g_notesChanged = true;
    g_power = POWER_BOOT; g_powerT0 = ImGui::GetTime();
    g_app = APP_HOME; g_animPage = -1;
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
bool tabletPoweredOn() { return g_power == POWER_ON && !(g_batt <= 0.0f && !g_charger); }
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
// ---- at midnight of New Year's Day and Christmas Day (the computer's clock), while the tablet is on: a fade to black, five seconds of
// celebration (fireworks; snow and the sleigh crossing the sky), then "from" and XPTablet's logo in the holiday theme's colours, and a
// Dismiss button at the bottom. It stays until Dismiss is pressed, and then fades back to what was there
int g_celebrate = -1;        // 0 New Year, 1 Christmas, -1 none
double g_celebT0 = 0;
double g_celebOutT0 = -1;    // when Dismiss was pressed (-1: not yet)
bool g_celebHot = false, g_celebDown = false;   // the pointer over / pressing Dismiss
constexpr float kCelebIn = 0.6f, kCelebShow = 5.0f, kCelebFrom = 1.5f, kCelebOut = 0.6f;   // fade in, the show, "from XPTablet" arriving, fade out
bool celebrating() { return g_celebrate >= 0; }
bool celebDismissReady() { return g_celebrate >= 0 && g_celebOutT0 < 0 && ImGui::GetTime() - g_celebT0 >= kCelebIn + kCelebShow + kCelebFrom; }
void celebDismissRect(float w, float h, ImVec2& a, ImVec2& b) { a = ImVec2(std::floor(w * 0.5f) - 70, h - 78); b = ImVec2(std::floor(w * 0.5f) + 70, h - 42); }
void celebrationStep() {
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    static int lastDay = -1;
    const int day = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
    if (lastDay >= 0 && day != lastDay && g_power == POWER_ON) {   // midnight passed while the tablet was on
        if (lt.tm_mon == 0 && lt.tm_mday == 1) celebrate(0);
        else if (lt.tm_mon == 11 && lt.tm_mday == 25) celebrate(1);
    }
    lastDay = day;
}
void drawCelebration(ImDrawList* dl, const Fonts& f, float w, float h) {
    if (!celebrating()) return;
    const float t = (float)(ImGui::GetTime() - g_celebT0);
    const float out = g_celebOutT0 < 0 ? 1.0f : 1 - smooth01((float)(ImGui::GetTime() - g_celebOutT0) / kCelebOut);   // (Dismiss pressed: all of it fades away)
    if (g_celebOutT0 >= 0 && out <= 0.0f) { g_celebrate = -1; g_celebOutT0 = -1; return; }
    const float black = smooth01(t / kCelebIn) * out;
    const float show = std::min(smooth01((t - kCelebIn * 0.6f) / 0.4f), 1 - smooth01((t - kCelebIn - kCelebShow + 0.3f) / 0.5f)) * out;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(255 * black)));
    const float ft = t - kCelebIn - kCelebShow;   // after the show
    if (ft > 0) {
        // "from" and XPTablet's logo, in the holiday theme's colours (its accent and that accent's light tone), on the black
        const float fk = smooth01((ft - 0.2f) / 0.7f) * out;
        if (fk > 0.01f) {
            const Accent& ac = kSeasonal[g_celebrate == 0 ? 0 : 13].ac;
            const ImU32 main = IM_COL32(ac.r, ac.g, ac.b, 255), lite = mixCol(main, IM_COL32(255, 255, 255, 255), 0.5f);
            int lw = 4, lh = 1, lv = 0; float from = 0.5f;
            logoMaskPixels(lw, lh, lv, from);
            const float hh = 52, ww = std::floor(hh * lw / std::max(1, lh));
            const ImVec2 a(std::floor(w * 0.5f - ww * 0.5f), std::floor(h * 0.47f - hh * 0.5f + 8 * (1 - smooth01((ft - 0.2f) / 0.7f)))), b(a.x + ww, a.y + hh);   // (it rises a little into place)
            const int A8 = (int)(255 * fk);
            const char* fromTxt = "from";
            const float px = fontPx(f.title), fw = f.title->CalcTextSizeA(px, 1e9f, 0, fromTxt).x;
            dl->AddText(f.title, px, ImVec2(std::floor(w * 0.5f - fw * 0.5f), a.y - px - 12), IM_COL32(255, 255, 255, (int)(190 * fk)), fromTxt);
            if (g_logoMask) {
                dl->AddImage(g_logoMask, a, ImVec2(a.x + ww * from, b.y), ImVec2(0, 0), ImVec2(from, 1), IM_COL32(255, 255, 255, A8));   // XP in white
                const int n = 6;   // TABLET in slices, from the light tone to the accent (as the logo is drawn in a theme's colours)
                for (int i = 0; i < n; ++i) {
                    const float u0 = from + (1 - from) * i / n, u1 = from + (1 - from) * (i + 1) / n;
                    dl->AddImage(g_logoMask, ImVec2(a.x + ww * u0, a.y), ImVec2(a.x + ww * u1, b.y), ImVec2(u0, 0), ImVec2(u1, 1), (mixCol(lite, main, (i + 0.5f) / n) & 0x00FFFFFF) | ((ImU32)A8 << 24));
                }
            } else dl->AddImage(g_logo, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, A8));
            const float lk = smooth01((ft - 0.6f) / 0.7f);   // a line drawn out under it, from the middle
            dl->AddRectFilled(ImVec2(w * 0.5f - ww * 0.5f * lk, b.y + 14), ImVec2(w * 0.5f + ww * 0.5f * lk, b.y + 15.5f), (main & 0x00FFFFFF) | ((ImU32)(170 * fk) << 24));
        }
        // Dismiss, at the bottom: it pops up once the logo is there; pressing it fades everything away
        const float pk = smooth01((ft - kCelebFrom) / 0.35f);
        if (pk > 0.01f) {
            ImVec2 pa, pb;
            celebDismissRect(w, h, pa, pb);
            const Ease e = eased("##celebDismiss", false, g_celebHot, g_celebDown);
            const float grow = (1 - pk) * 10 + 1.2f * std::sin(pk * 3.14159f) * -3.0f + e.down * 1.5f;   // (from a little smaller, a touch past its size, and settled)
            const ImVec2 a2(pa.x + grow, pa.y + grow * 0.3f + 10 * (1 - pk)), b2(pb.x - grow, pb.y - grow * 0.3f + 10 * (1 - pk));
            const int v0 = dl->VtxBuffer.Size;
            ImU32 bg = mixCol(IM_COL32(255, 255, 255, 40), mixCol(IM_COL32(255, 255, 255, 72), accentCol(90), 0.35f), e.hot);
            bg = mixCol(bg, IM_COL32(255, 255, 255, 96), e.down);
            dl->AddRectFilled(a2, b2, bg, (b2.y - a2.y) * 0.5f);
            dl->AddRect(a2, b2, IM_COL32(255, 255, 255, 70 + (int)(80 * e.hot)), (b2.y - a2.y) * 0.5f, 0, 1.2f);
            textCentered(dl, f.small, (a2.x + b2.x) * 0.5f, std::floor((a2.y + b2.y - fontPx(f.small)) * 0.5f), kInk, "Dismiss");
            fadeSince(dl, v0, std::min(pk, out));
        }
    }
    if (show <= 0.01f) return;
    const float st = t - kCelebIn * 0.6f;   // the show's own clock
    auto A = [&](ImU32 c, float k = 1.0f) { return (c & 0x00FFFFFF) | ((ImU32)std::lround(((c >> 24) & 255) * show * std::max(0.0f, std::min(1.0f, k))) << 24); };
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    if (g_celebrate == 0) {   // New Year: a night sky, fireworks going up and bursting one after another, the new year in gold
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), A(IM_COL32(4, 6, 20, 255)), A(IM_COL32(4, 6, 20, 255)), A(IM_COL32(30, 24, 60, 255)), A(IM_COL32(30, 24, 60, 255)));
        for (int i = 0; i < 70; ++i) dl->AddCircleFilled(ImVec2(decoRnd(i, 1) * w, decoRnd(i, 2) * h * 0.7f), 0.8f + decoRnd(i, 3) * 1.2f, A(IM_COL32(255, 255, 255, 150), 0.5f + 0.5f * std::sin(st * 3 + i)), 6);
        static const ImU32 cols[6] = {IM_COL32(255, 210, 100, 255), IM_COL32(255, 110, 150, 255), IM_COL32(120, 210, 255, 255), IM_COL32(180, 140, 255, 255), IM_COL32(140, 255, 160, 255), IM_COL32(255, 255, 255, 255)};
        for (int n = 0; n < 12; ++n) {
            const float launch = n * 0.38f, up = 0.7f;
            const float lt0 = st - launch;
            if (lt0 < 0) continue;
            const ImVec2 base(w * (0.12f + decoRnd(n, 7) * 0.76f), h), top(base.x + (decoRnd(n, 8) - 0.5f) * 80, h * (0.10f + decoRnd(n, 9) * 0.20f));   // (bursting above the words, never over them)
            const ImU32 col = cols[n % 6];
            if (lt0 < up) {   // its trail going up
                const float k = smooth01(lt0 / up);
                const ImVec2 p(base.x + (top.x - base.x) * k, base.y + (top.y - base.y) * k);
                dl->AddLine(ImVec2(p.x, p.y + 18), p, A(IM_COL32(255, 230, 180, 200)), 2.0f);
                dl->AddCircleFilled(p, 2.5f, A(IM_COL32(255, 240, 200, 255)), 8);
            } else {   // the burst: sparks flying out, falling a little, fading
                const float bt = lt0 - up, life = 1.6f;
                if (bt > life) continue;
                const float fade = 1 - bt / life, rr = (40 + decoRnd(n, 10) * 40) * smooth01(std::min(1.0f, bt / 0.5f) * 0.7f + bt / life * 0.3f) * 1.4f;
                for (int sp = 0; sp < 28; ++sp) {
                    const float a = sp * 6.2832f / 28 + decoRnd(n, 11);
                    const ImVec2 q(top.x + std::cos(a) * rr, top.y + std::sin(a) * rr + 30 * bt * bt);
                    dl->AddCircleFilled(q, 2.2f, A(col, fade), 6);
                    dl->AddLine(ImVec2(top.x + std::cos(a) * rr * 0.7f, top.y + std::sin(a) * rr * 0.7f + 30 * bt * bt), q, A(col, fade * 0.5f), 1.2f);
                }
            }
        }
        char yr[16]; std::snprintf(yr, sizeof yr, "%d", lt.tm_year + 1900);
        const float tk = smooth01((st - 0.5f) / 0.8f);
        const float px1 = fontPx(f.title) * 2.0f, px2 = fontPx(f.title) * 3.2f;
        const char* l1 = "Happy New Year";
        const float w1 = f.title->CalcTextSizeA(px1, 1e9f, 0, l1).x, w2 = f.title->CalcTextSizeA(px2, 1e9f, 0, yr).x;
        dl->AddText(f.title, px1, ImVec2(w * 0.5f - w1 * 0.5f, h * 0.5f - 30), A(IM_COL32(255, 255, 255, 255), tk), l1);
        dl->AddText(f.title, px2, ImVec2(w * 0.5f - w2 * 0.5f, h * 0.5f + 14), A(IM_COL32(255, 206, 96, 255), tk), yr);
    } else {   // Christmas: snow falling over a night sky and hills of firs, the sleigh crossing under the moon, "Merry Christmas"
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), A(IM_COL32(6, 14, 30, 255)), A(IM_COL32(6, 14, 30, 255)), A(IM_COL32(20, 52, 70, 255)), A(IM_COL32(20, 52, 70, 255)));
        dl->AddCircleFilled(ImVec2(w * 0.8f, h * 0.2f), 46, A(IM_COL32(255, 248, 220, 230)), 40);
        dl->AddCircleFilled(ImVec2(w * 0.8f, h * 0.2f), 64, A(IM_COL32(255, 248, 220, 30)), 40);
        for (int i = 0; i < 9; ++i) decoTree(dl, ImVec2(w * (0.04f + i * 0.115f), h), 70 + decoRnd(i, 4) * 50, A(IM_COL32(10, 40, 26, 255)));
        dl->AddRectFilled(ImVec2(0, h - 10), ImVec2(w, h), A(IM_COL32(230, 240, 250, 255)));
        // the sleigh: from the left to the right, rising a little, across the sky
        const float sk = std::max(0.0f, std::min(1.0f, (st - 0.3f) / 4.4f));
        drawSleigh(dl, ImVec2(-120 + (w + 240) * sk, h * 0.42f - sk * 90), 150, 1.5707963f - 0.2f, (int)(255 * show));
        for (int i = 0; i < 140; ++i) {   // the snow, drifting down
            const float sp = 30 + decoRnd(i, 5) * 50, x = decoRnd(i, 6) * w + std::sin(st * 1.3f + i) * 12;
            const float y = std::fmod(decoRnd(i, 7) * h + st * sp, h + 20) - 10;
            dl->AddCircleFilled(ImVec2(x, y), 1.2f + decoRnd(i, 8) * 2.2f, A(IM_COL32(255, 255, 255, 220)), 8);
        }
        const float tk = smooth01((st - 0.6f) / 0.8f);
        const float px1 = fontPx(f.title) * 2.6f;
        const char* l1 = "Merry Christmas";
        const float w1 = f.title->CalcTextSizeA(px1, 1e9f, 0, l1).x;
        dl->AddText(f.title, px1, ImVec2(w * 0.5f - w1 * 0.5f + 2, h * 0.68f + 2), A(IM_COL32(0, 0, 0, 140), tk), l1);
        dl->AddText(f.title, px1, ImVec2(w * 0.5f - w1 * 0.5f, h * 0.68f), A(IM_COL32(255, 90, 90, 255), tk), l1);
    }
}
void build(const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    if (!ImGui::GetIO().MouseDown[0]) g_touchScrolling = false;
    logStep(s);   // (the logbook keeps watching, whatever page is shown)
    seasonalStep();   // (a seasonal theme that has ended gives way; one that has begun is announced once)
    acarsStep(s, act);   // (and ACARS keeps listening)
    {   // a Custom theme's picture: the one the theme shown (or being edited) wants; its average colour becomes its background once
        g_bgTex = s.bgTex; g_bgW = s.bgW; g_bgH = s.bgH; g_bgPic = s.bgPicture;
        const int slot = g_customEdit >= 0 ? g_customEdit : (g_settings.wallpaper >= 7 && g_settings.wallpaper <= 9 ? g_settings.wallpaper - 7 : -1);
        act.backgroundWanted = slot >= 0 ? g_settings.customs[slot].picture : std::string();
        static std::string matched[3];
        if (slot >= 0 && !act.backgroundWanted.empty() && s.bgPicture == act.backgroundWanted && s.bgAverage >= 0 && matched[slot] != act.backgroundWanted) {
            if (!matched[slot].empty() || g_customEdit >= 0) {   // (chosen now, not on loading): its background and accent from the picture
                g_settings.customs[slot].bg = s.bgAverage;
                if (s.bgAccent >= 0) g_settings.customs[slot].accent = s.bgAccent;
                g_settingsChanged = true;
            }
            matched[slot] = act.backgroundWanted;
        }
    }
    trackCrumbs(s);   // (on every page: the trail is kept whether or not the map is open)
    // big draw lists (the map's lines) go past 65536 vertices: the renderer follows each command's vertex offset
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##tablet", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    celebrationStep();
    g_celebHot = g_celebDown = false;
    if (celebrating()) {   // (nothing under it takes a press meanwhile; its own Dismiss button is read here, drawn with the rest at the end)
        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        const bool hit = ImGui::InvisibleButton("##celebrating", ImVec2(w, h));
        if (celebDismissReady()) {
            ImVec2 pa, pb;
            celebDismissRect(w, h, pa, pb);
            const ImGuiIO& cio = ImGui::GetIO();
            auto in = [&](ImVec2 p) { return p.x >= pa.x && p.x <= pb.x && p.y >= pa.y && p.y <= pb.y; };
            const bool began = in(cio.MouseClickedPos[0]);   // (the press must begin and end on the button)
            g_celebHot = ImGui::IsItemHovered() && in(cio.MousePos);
            g_celebDown = ImGui::IsItemActive() && began && in(cio.MousePos);
            if (hit && began && in(cio.MousePos)) g_celebOutT0 = ImGui::GetTime();
        }
    }
    g_ownPerformance = s.toliss;
    if (g_app == APP_PERFORMANCE && g_ownPerformance) g_app = APP_HOME;
    g_px = std::max(1.0f, s.pixelScale);
    g_embed = EmbedArea();
    g_wx = s.wxPlane;
    g_wxLogo = f.wxLogo;
    g_logo = f.logo; g_logoMask = f.logoMask;
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
        // Local Connect: a connected device that tells its battery is the tablet's battery (its charge, and its charger); the
        // tablet's own comes back, as it was, once none is
        const LocalDevice* dev = nullptr;
        if (g_settings.localConnect && g_settings.localBattery) for (const LocalDevice& d : s.local.devices) if (d.connected && d.batteryPct >= 0) { dev = &d; break; }
        if (dev) {
            if (!g_battFromDevice) { g_ownBatt = g_batt; g_ownCharger = g_charger; g_battFromDevice = true; }
            g_batt = (float)std::max(0, std::min(100, dev->batteryPct)); g_charger = dev->charging;
        } else if (g_battFromDevice) { g_batt = g_ownBatt; g_charger = g_ownCharger; g_battFromDevice = false; }
        if (g_battFromDevice) {}   // (the device's own: nothing to run down or charge here)
        else if (g_charger) g_batt = std::min(100.0f, g_batt + dt * 100.0f / 1800.0f);
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
                drawLogo(dl, ImVec2(w * 0.5f - ww * 0.5f, h * 0.42f - hh * 0.5f), ImVec2(w * 0.5f + ww * 0.5f, h * 0.42f + hh * 0.5f), A);
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
        home(dl, f, s, w, h, act);
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
                drawLogo(dl, ImVec2(w * 0.5f - ww * 0.5f, cy - hh * 0.5f), ImVec2(w * 0.5f + ww * 0.5f, cy + hh * 0.5f), 255);
            }
        }
    }
    drawCelebration(ImGui::GetWindowDrawList(), f, w, h);   // (over everything, the status bar too)
    ImGui::End();
    ImGui::PopStyleVar();
}

namespace {

// ---- Navigation: X-Plane's navaids and fixes (airports: the Airports app), found by their code or name; tapped, shown on the map -------------------------
std::string g_navQuery;
float g_navScroll = 0;
void navSearchPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH + 10;
    textField(dl, f, "##navq", g_navQuery, ImVec2(12, top), ImVec2(w - 12, top + 40), "Search: a VOR, NDB, ILS, DME or fix (code or name)", false, 0);
    for (char& c : g_navQuery) c = (char)std::toupper((unsigned char)c);
    act.navSearch = g_navQuery;
    const float ly0 = top + 52, ly1 = (g_kbd && g_kbdTarget == 2) ? h - 230 : h - 12;
    card(dl, ImVec2(12, ly0), ImVec2(w - 12, ly1), f, nullptr);
    ImGui::SetCursorScreenPos(ImVec2(12, ly0));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##navlist", ImVec2(w - 24, ly1 - ly0));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_navScroll -= ImGui::GetIO().MouseWheel * 40;
    touchScroll(g_navScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float rowH = 44, content = s.navHits.size() * rowH + 12;
    g_navScroll = std::max(0.0f, std::min(g_navScroll, std::max(0.0f, content - (ly1 - ly0))));
    if (s.navHits.empty() && g_navQuery.size() < 2) textAt(dl, f.small, 28, ly0 + 16, kMuted, "Type two letters or more.");
    else if (s.navHits.empty()) textAt(dl, f.small, 28, ly0 + 16, kMuted, "Nothing found in X-Plane's navigation data.");
    dl->PushClipRect(ImVec2(14, ly0 + 2), ImVec2(w - 14, ly1 - 2), true);
    for (size_t i = 0; i < s.navHits.size(); ++i) {
        const auto& n = s.navHits[i];
        const float y = ly0 + 6 + i * rowH - g_navScroll;
        if (y + rowH < ly0 || y > ly1) continue;
        char id[24]; std::snprintf(id, sizeof id, "##navh%zu", i);
        ImGui::SetCursorScreenPos(ImVec2(18, y));
        if (ImGui::InvisibleButton(id, ImVec2(w - 36 - 10, rowH - 4)) && !ImGui::IsMouseDragging(0, 3.0f)) { g_kbd = false; g_typeInto = nullptr; showOnMap(n.lat, n.lon); }
        if (const float hk = itemHover(); hk > 0.01f) dl->AddRectFilled(ImVec2(18, y), ImVec2(w - 28, y + rowH - 4), IM_COL32(255, 255, 255, (int)(16 * hk)), 8.0f);
        const char* kind = n.type == 1 ? "AIRPORT" : n.type == 2 ? "NDB" : n.type == 4 ? "VOR" : n.type == 8 ? "ILS" : n.type == 16 ? "LOC" : n.type == 512 ? "FIX" : n.type == 1024 ? "DME" : "NAV";
        const ImU32 kc = n.type == 1 ? IM_COL32(80, 150, 255, 255) : n.type == 2 ? IM_COL32(200, 120, 220, 255) : n.type == 4 || n.type == 1024 ? IM_COL32(80, 200, 170, 255) : n.type == 512 ? IM_COL32(200, 200, 210, 255) : IM_COL32(255, 190, 90, 255);
        dl->AddRectFilled(ImVec2(28, y + 11), ImVec2(96, y + 31), (kc & 0x00FFFFFF) | (60u << 24), 6.0f);
        textCentered(dl, f.small, 62, y + 12, kc, kind);
        textAt(dl, f.bold, 110, y + 11, kInk, n.ident.c_str());
        textAt(dl, f.small, 190, y + 13, kMuted, fitText(f.small, n.name, w - 190 - 260).c_str());
        char rt[64] = "";
        if (n.freq > 0) std::snprintf(rt, sizeof rt, n.type == 2 ? "%.0f kHz" : "%.2f", n.type == 2 ? n.freq : n.freq / 100.0f);
        textAt(dl, f.mono, w - 250, y + 13, kInk, rt);
        if (s.map.havePos) {
            const double d = nmBetween(s.map.lat, s.map.lon, n.lat, n.lon);
            const double brg = std::fmod(std::atan2(std::sin((n.lon - s.map.lon) * 0.0174533) * std::cos(n.lat * 0.0174533),
                std::cos(s.map.lat * 0.0174533) * std::sin(n.lat * 0.0174533) - std::sin(s.map.lat * 0.0174533) * std::cos(n.lat * 0.0174533) * std::cos((n.lon - s.map.lon) * 0.0174533)) * 57.2958 + 360.0, 360.0);
            char dt[32]; std::snprintf(dt, sizeof dt, "%.0f nm %03.0f\xC2\xB0", d, brg);
            textRight(dl, f.small, w - 34, y + 13, kMuted, dt);
        }
    }
    dl->PopClipRect();
    pageKeyboard(dl, f, w, h, act);
}

// ---- the home screen's widgets ------------------------------------------------------------------------------------------------------
const float kWidgetR = 60 * 0.24f;   // a widget's corners: the icons' own rounding
void drawWidget(ImDrawList* dl, const Fonts& f, const SimState& s, int kind, ImVec2 a, ImVec2 b, Actions& act, bool live) {
    const float W = b.x - a.x, H = b.y - a.y;
    if (kind != W_VOLANTA || !live) {
        glass(dl, a, b, kWidgetR, 1.0f);
    }
    const float px = a.x + 14;
    switch (kind) {
    case W_WEATHER: {   // the weather where the aircraft is (WXPlane's name on it when it is there)
        textAt(dl, f.small, px, a.y + 10, kMuted, g_wx ? "WXPlane" : "Weather");
        const SimWeather& x = s.wx;
        if (!x.valid) { textAt(dl, f.small, px, a.y + 40, kMuted, "No weather yet"); break; }
        char t[48];
        std::snprintf(t, sizeof t, "%.0f\xC2\xB0", x.tempC);
        dl->AddText(f.title, fontPx(f.title) * 1.6f, ImVec2(px, a.y + 28), kInk, t);
        std::snprintf(t, sizeof t, "%03.0f\xC2\xB0 / %.0f kt", x.windDir, x.windKt);
        textAt(dl, f.small, px, b.y - 62, kInk, t);
        std::snprintf(t, sizeof t, "QNH %.0f", x.qnhHpa);
        textAt(dl, f.small, px, b.y - 42, kMuted, t);
        if (x.visSm >= 10) std::snprintf(t, sizeof t, "Vis 10+ sm"); else std::snprintf(t, sizeof t, "Vis %.1f sm", x.visSm);
        textAt(dl, f.small, px, b.y - 24, kMuted, t);
        // a sun, or a cloud when it is overcast
        const ImVec2 ic(b.x - 34, a.y + 34);
        const bool cloudy = x.clouds > 0 && x.cloudCover[0] > 0.6f;
        if (!cloudy) {
            dl->AddCircleFilled(ic, 11, IM_COL32(255, 204, 80, 255), 0);
            for (int i = 0; i < 8; ++i) { const float an = i * 3.14159f / 4; dl->AddLine(ImVec2(ic.x + std::cos(an) * 15, ic.y + std::sin(an) * 15), ImVec2(ic.x + std::cos(an) * 20, ic.y + std::sin(an) * 20), IM_COL32(255, 204, 80, 255), 2.0f); }
        } else {
            dl->AddCircleFilled(ImVec2(ic.x - 7, ic.y + 3), 9, IM_COL32(220, 228, 240, 255), 0);
            dl->AddCircleFilled(ImVec2(ic.x + 3, ic.y - 3), 11, IM_COL32(220, 228, 240, 255), 0);
            dl->AddRectFilled(ImVec2(ic.x - 16, ic.y + 2), ImVec2(ic.x + 15, ic.y + 12), IM_COL32(220, 228, 240, 255), 5.0f);
        }
        break;
    }
    case W_FLIGHT: {   // the flight: its callsign, from and to, and how far along it is
        textAt(dl, f.small, px, a.y + 10, kMuted, "Flight");
        const FlightPlan& p = s.plan;
        if (!p.loaded) { textAt(dl, f.small, px, a.y + 40, kMuted, "No flight plan: import one from SimBrief"); break; }
        std::string top = p.callsign.empty() ? (p.origin + "  \xE2\x80\x93  " + p.dest) : (p.callsign + "   " + p.origin + "  \xE2\x80\x93  " + p.dest);
        textAt(dl, f.bold, px, a.y + 30, kInk, fitText(f.bold, top, W - 28).c_str());
        const Progress pr = flightProgress(s);
        progressStrip(dl, f, s, pr, ImVec2(a.x + 10, a.y + 60), ImVec2(b.x - 10, b.y - 10), true);
        break;
    }
    case W_MEDIA: {   // what is playing: its cover, title, artist, and the buttons
        const MediaState& m = s.media;
        if (!m.active) {
            textAt(dl, f.small, px, a.y + 10, kMuted, "Now Playing");
            textCentered(dl, f.small, (a.x + b.x) * 0.5f, a.y + H * 0.5f, kMuted, m.supported ? "Nothing Playing" : "Media controls are not available");
            break;
        }
        const float cs = std::min(64.0f, H * 0.4f);
        cover(dl, m, ImVec2(px, a.y + 12), cs, 10.0f);
        const float tx = px + cs + 10, room = b.x - 12 - tx;
        marquee(dl, f.bold, tx, a.y + 16, room, kInk, m.title.empty() ? std::string("Unknown") : m.title, trackSince(m));
        textAt(dl, f.small, tx, a.y + 40, kMuted, fitText(f.small, m.artist, room).c_str());
        if (!live) ImGui::BeginDisabled();
        playerButtons(dl, m, ImVec2((a.x + b.x) * 0.5f, b.y - 36), 16, "##wmedia", act);
        if (!live) ImGui::EndDisabled();
        break;
    }
    case W_FPS: {   // X-Plane's frame rate: green from 30, amber from 20, else red
        const float fps = s.fps;
        const ImU32 col = fps >= 30 ? kGood : fps >= 20 ? kWarn : IM_COL32(255, 96, 96, 255);
        char t[16];
        std::snprintf(t, sizeof t, "%d", (int)std::lround(fps));
        const float big = fontPx(f.title) * (H < 90 ? 1.05f : 1.5f);
        const ImVec2 sz = f.title->CalcTextSizeA(big, 1e9f, 0, t);
        dl->AddText(f.title, big, ImVec2(std::floor((a.x + b.x - sz.x) * 0.5f), std::floor(a.y + (H < 90 ? 4 : H * 0.5f - sz.y * 0.65f))), fps > 0 ? col : kMuted, fps > 0 ? t : "--");
        textCentered(dl, f.small, (a.x + b.x) * 0.5f, b.y - (H < 90 ? 18 : 22), kMuted, "FPS");
        break;
    }
    case W_VOLANTA: {   // Volanta's stream overlay (its page, from the Volanta app on this computer), across the screen
        const BrowserState& br = s.browser;
        if (!live) {
            textAt(dl, f.small, px, a.y + 10, kMuted, "Volanta Overlay");
            textCentered(dl, f.small, (a.x + b.x) * 0.5f, a.y + H * 0.5f - 6, kMuted, g_settings.volantaOverlay.c_str());
            break;
        }
        // shown here: the browser's tab for it (its own group), opened when it is not there yet
        act.webShown = true;
        act.webX = a.x; act.webY = a.y; act.webW = W; act.webH = H;
        int tab = 0;
        for (const BrowserState::Tab& t : br.tabs) if (t.group == 400) tab = t.id;
        static double askedAt = -10;
        if (br.status == 1 && !tab && ImGui::GetTime() - askedAt > 3 && (int)br.tabs.size() < 16) {
            act.webTabNew = true; act.webTabGroup = 400; act.webTabUrl = g_settings.volantaOverlay; askedAt = ImGui::GetTime();
        } else if (br.status == 1 && tab && br.activeTab != tab && !act.webTabSelect) act.webTabSelect = tab;
        if (br.status == 1 && tab && br.activeTab == tab && br.tex) {
            dl->AddImageRounded(br.tex, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), kWidgetR);
        } else {
            glass(dl, a, b, kWidgetR, 1.0f);
            textAt(dl, f.small, px, a.y + 10, kMuted, "Volanta Overlay");
            textCentered(dl, f.small, (a.x + b.x) * 0.5f, a.y + H * 0.5f - 6, kMuted, br.status == -3 ? "The browser is not available here" : "Opening the overlay...");
        }
        break;
    }
    default: break;
    }
}

// ---- the App Store: the extra web apps, each installed (onto the home screen) or removed with a tap ---------------------------------------
// the apps the App Store offers: the web apps that were switched on and off in Settings (each its own setting) and the store's own
bool* storeFlag(int app) {
    switch (app) {
    case APP_CHARTS: return &g_settings.showCharts;
    case APP_NAVIGRAPH: return &g_settings.showNavigraph;
    case APP_LIVEMAP: return &g_settings.showLive;
    case APP_VATSIM: return &g_settings.showVatsim;
    case APP_FOREFLIGHT: return &g_settings.showForeflight;
    case APP_SKYVECTOR: return &g_settings.showSkyvector;
    case APP_SIMBRIEF: return &g_settings.showSimbrief;
    default: return nullptr;
    }
}
bool storeInstalled(int app) { bool* fl = storeFlag(app); return fl ? *fl : installed(app); }
int g_storeCat = 0;   // the category shown: 0 all
float g_storeScroll = 0;
double g_storeFadeT0 = -10;   // the search changed: what it finds fades in
std::string g_storeSearch;   // the App Store's search (an app's name or what it does, in every category)
void storePage(ImDrawList* dl, const Fonts& f, float w, float h, Actions& act) {
    struct Item { int app; int cat; const char* what; };   // cat: 1 charts, 2 planning, 3 networks, 4 music and video, 5 games
    static const Item items[] = {
        {APP_FOREFLIGHT, 1, "ForeFlight's charts and maps on the web"},
        {APP_CHARTS, 1, "ChartFox: free charts for most of the world"},
        {APP_NAVIGRAPH, 1, "Navigraph Charts (a Navigraph account)"},
        {APP_SKYVECTOR, 1, "SkyVector's aeronautical charts"},
        {APP_SIMBRIEF, 2, "SimBrief: plan and dispatch your flights"},
        {APP_DATIS, 2, "Digital ATIS for the airports that publish it"},
        {APP_TIMERS, 2, "A stopwatch, a hold timer, the flight's block and air times, ETA and top of descent"},
        {APP_LOGBOOK, 2, "Every flight logged by itself: times, route, landing rate and fuel"},
        {APP_ACARS, 3, "Hoppie ACARS: CPDLC, pre-departure clearances and telex on VATSIM and IVAO"},
        {APP_VATSIM, 3, "VATSIM Radar: the network's traffic and ATC live"},
        {APP_LIVEMAP, 3, "Volanta's live map of your flights"},
        {APP_VATSIMHUB, 3, "VATSIM: network stats, events, the AIP and pilot resources"},
        {APP_IVAO, 3, "IVAO's live map of pilots and controllers"},
        {APP_FR24, 3, "Flightradar24: real-world air traffic, live"},
        {APP_SPOTIFY, 4, "Spotify's web player"},
        {APP_YTMUSIC, 4, "YouTube Music's web player"},
        {APP_APPLEMUSIC, 4, "Apple Music on the web"},
        {APP_SOUNDCLOUD, 4, "SoundCloud"},
        {APP_YOUTUBE, 4, "YouTube"},
        {APP_GAMES, 5, "2048, Knots and Crosses, and six aviation games of its own"},
    };
    const float top = kStatusH + kHeaderH + 10;
    {   // the categories
        static const char* cats[6] = {"All", "Charts", "Flight Planning", "Networks", "Music & Video", "Games"};
        float x = 16;
        for (int c = 0; c < 6; ++c) {
            const float bw = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, cats[c]).x + 32;
            char id[16]; std::snprintf(id, sizeof id, "##scat%d", c);
            if (pill(dl, f, id, cats[c], ImVec2(x, top), ImVec2(x + bw, top + 30), g_storeCat == c, true) && g_storeCat != c) { g_storeCat = c; g_storeScroll = 0; }
            x += bw + 8;
        }
        static std::string was;
        textField(dl, f, "##storesearch", g_storeSearch, ImVec2(std::max(x + 8, w - 230), top), ImVec2(w - 16, top + 30), "Search Apps", false);
        if (!g_storeSearch.empty() && pill(dl, f, "##storeclr", "x", ImVec2(w - 46, top + 3), ImVec2(w - 20, top + 27), false, true)) g_storeSearch.clear();
        if (was != g_storeSearch) { was = g_storeSearch; g_storeScroll = 0; g_storeFadeT0 = ImGui::GetTime(); }
    }
    auto lower = [](std::string v) { for (char& c : v) c = (char)std::tolower((unsigned char)c); return v; };
    const std::string q = lower(g_storeSearch);
    std::vector<const Item*> shown;
    for (const Item& it : items) {
        if (g_storeCat != 0 && it.cat != g_storeCat) continue;   // (the category chosen, with or without a search)
        if (q.empty() || lower(appName(it.app)).find(q) != std::string::npos || lower(it.what).find(q) != std::string::npos) shown.push_back(&it);
    }
    static int catShown = -1; static double catT0 = -10;   // (a category chosen: its apps fade in)
    if (catShown != g_storeCat) { catShown = g_storeCat; catT0 = ImGui::GetTime(); }
    const int storeV0 = dl->VtxBuffer.Size;
    const float ly0 = top + 42, ly1 = h - 8, gapX = 10, gapY = 10;
    const float cw = std::floor((w - 32 - gapX * 2) / 3), ch = 84;
    const float content = ((shown.size() + 2) / 3) * (ch + gapY);
    ImGui::SetCursorScreenPos(ImVec2(0, ly0));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##storelist", ImVec2(w, ly1 - ly0));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_storeScroll -= ImGui::GetIO().MouseWheel * 40;
    touchScroll(g_storeScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    g_storeScroll = std::max(0.0f, std::min(g_storeScroll, std::max(0.0f, content - (ly1 - ly0))));
    dl->PushClipRect(ImVec2(0, ly0), ImVec2(w, ly1), true);
    for (size_t i = 0; i < shown.size(); ++i) {
        const int app = shown[i]->app;
        const ImVec2 a(16 + (i % 3) * (cw + gapX), ly0 + (i / 3) * (ch + gapY) - g_storeScroll), b(a.x + cw, a.y + ch);
        if (b.y < ly0 || a.y > ly1) continue;
        card(dl, a, b, f, nullptr);
        drawTile(dl, f, app, ImVec2(a.x + 38, a.y + ch * 0.5f), 52, false, 1.0f, 1.0f);
        const float tx = a.x + 74, room = b.x - 10 - tx;
        textAt(dl, f.bold, tx, a.y + 10, kInk, fitText(f.bold, appName(app), room - 60).c_str());
        dl->AddText(f.small, fontPx(f.small), ImVec2(tx, a.y + 34), kMuted, shown[i]->what, nullptr, room);
        const bool on = storeInstalled(app);
        char id[24]; std::snprintf(id, sizeof id, "##store%d", app);
        if (pill(dl, f, id, on ? "Remove" : "Get", ImVec2(b.x - 66, a.y + 8), ImVec2(b.x - 10, a.y + 30), !on, a.y + 8 >= ly0 && a.y + 30 <= ly1)) installApp(app, !on);
    }
    dl->PopClipRect();
    fadeSince(dl, storeV0, std::min(1.0f, (float)(ImGui::GetTime() - std::max(catT0, g_storeFadeT0)) / 0.2f));
    if (content > ly1 - ly0) g_storeScroll = scrollBar(dl, "##storebar", w - 8, ly0 + 4, ly1 - 4, g_storeScroll, content, ly1 - ly0);
    if (shown.empty()) textCentered(dl, f.body, w * 0.5f, ly0 + 40, kMuted, ("No apps match \"" + g_storeSearch + "\"" + (g_storeCat ? " in this category" : "")).c_str());
    pageKeyboard(dl, f, w, h, act);
}

// ---- VATSIM: a page of bubbles, each a part of VATSIM's own sites (they open as the app's tabs) ----------------------------------------
void vatsimHubPage(ImDrawList* dl, const Fonts& f, float w, float h) {
    struct Bubble { const char* name; const char* what; const char* url; ImU32 col; };
    static const Bubble bubbles[] = {
        {"Stats", "Your hours, flights and the network's numbers", "https://stats.vatsim.net", IM_COL32(40, 120, 220, 255)},
        {"AIP", "Airports, their controllers and procedures (myVATSIM)", "https://my.vatsim.net", IM_COL32(40, 160, 120, 255)},
        {"Events", "What is on across the network", "https://my.vatsim.net/events", IM_COL32(220, 120, 40, 255)},
        {"Pilot Resources", "Guides, charts, maps and tools", "https://vatsim.net/docs/pilots/pilots", IM_COL32(140, 90, 220, 255)},
        {"natTrak", "North Atlantic oceanic clearances", "https://nattrak.vatsim.net", IM_COL32(20, 90, 150, 255)},
        {"xPilot", "Its messages, nearby ATC and more, opened in the sim", nullptr, IM_COL32(30, 44, 70, 255)},
    };
    const float top = kStatusH + kHeaderH + 16, gap = 14, bw = std::floor((w - 32 - 2 * gap) / 3), bh = std::floor((h - top - 14 - gap) / 2);
    for (int i = 0; i < 6; ++i) {
        const ImVec2 a(16 + (i % 3) * (bw + gap), top + (i / 3) * (bh + gap)), b(a.x + bw, a.y + bh);
        char id[24]; std::snprintf(id, sizeof id, "##vbub%d", i);
        ImGui::SetCursorScreenPos(a);
        const bool hit = ImGui::InvisibleButton(id, ImVec2(bw, bh));
        const Ease e = eased(id, false, ImGui::IsItemHovered(), ImGui::IsItemActive());
        const float in = e.down * 3;
        const ImVec2 a2(a.x + in, a.y + in), b2(b.x - in, b.y - in);
        dl->AddRectFilled(a2, b2, mixCol(mixCol(bubbles[i].col, IM_COL32(0, 0, 0, 255), 0.35f), bubbles[i].col, 0.5f + 0.4f * e.hot), 28.0f);
        dl->AddRect(a2, b2, IM_COL32(255, 255, 255, (int)(30 + 60 * e.hot)), 28.0f, 0, 1.2f);
        dl->AddText(f.title, fontPx(f.title), ImVec2(a2.x + 22, a2.y + 22), IM_COL32(255, 255, 255, 255), bubbles[i].name);
        dl->AddText(f.small, fontPx(f.small), ImVec2(a2.x + 22, a2.y + 62), IM_COL32(235, 240, 250, 220), bubbles[i].what, nullptr, bw - 44);
        if (hit) { if (bubbles[i].url) openWeb("VATSIM", bubbles[i].url, false, 100 + APP_VATSIMHUB); else { g_app = APP_XPILOT; g_animPage = -1; } g_backTo = APP_VATSIMHUB; g_subT0 = ImGui::GetTime(); }
    }
}

// ---- xPilot: its own windows opened from the tablet (its commands, when found in the sim), and what its radios are doing
struct XpCmd { const char* cmd; const char* label; const char* what; };
const XpCmd kXpCmds[] = {   // xPilot's own commands, as its plugin names them (github.com/xpilot-project/xpilot, plugin/src/plugin.cpp)
    {"xpilot/toggle_text_message_console", "Messages", "Its text window: ATC, private and frequency messages"},
    {"xpilot/toggle_nearby_atc", "Nearby ATC", "The controllers in range, their frequencies"},
    {"xpilot/toggle_notification_panel", "Notifications", "Its notification panel"},
    {"xpilot/toggle_aircraft_labels", "Aircraft Labels", "Other aircraft's labels, on or off"},
    {"xpilot/toggle_tcas", "TCAS Control", "xPilot's traffic on the TCAS, on or off"},
    {"xpilot/toggle_default_atis", "X-Plane ATIS", "X-Plane's own ATIS, on or off"},
    {"xpilot/split_audio_channels", "Split Audio", "COM1 in one ear, COM2 in the other"},
};
void xpilotPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH + 10;
    std::vector<const XpCmd*> xp;
    for (const XpCmd& c : kXpCmds) for (const std::string& n : s.xpilotCmds) if (n == c.cmd) xp.push_back(&c);
    card(dl, ImVec2(12, top), ImVec2(w - 12, top + 76), f, "XPILOT");
    const bool loaded = !s.xpilotCmds.empty();
    // its own status: connected (as whom, how many aircraft it shows) or not, and the stations on the radios
    std::string head = !loaded ? "xPilot is not loaded (its plugin is not running)"
                     : s.xpilotLogin > 0 ? "Connected" + (s.xpilotCallsign.empty() ? std::string() : " As " + s.xpilotCallsign) + (s.xpilotLogin == 2 ? " (Observer)" : "")
                     : s.xpilotLogin == 0 ? "Not Connected" : "xPilot Is Loaded";
    textAt(dl, f.bold, 28, top + 30, !loaded ? kWarn : s.xpilotLogin > 0 ? kGood : kInk, head.c_str());
    std::string line = s.media.radioTx ? "Transmitting" : s.media.radioRx && s.media.radioSource == "xPilot" ? "Receiving" : "Radio quiet";
    if (s.xpilotLogin > 0) {
        if (!s.xpilotCom1.empty()) line += "  \xC2\xB7  COM1 " + s.xpilotCom1;
        if (!s.xpilotCom2.empty()) line += "  \xC2\xB7  COM2 " + s.xpilotCom2;
        line += "  \xC2\xB7  " + std::to_string(s.xpilotAircraft) + " aircraft";
    } else line += "  \xC2\xB7  connect in the xPilot app; typing stays in xPilot's own windows";
    textAt(dl, f.small, 28, top + 52, kMuted, line.c_str());
    if (!loaded) return;
    const float y0 = top + 90, gap = 12, cols = 3, tw = std::floor((w - 24 - gap * (cols - 1)) / cols);
    const int rows = ((int)xp.size() + 2) / 3;
    const float th = std::min(150.0f, std::floor((h - 12 - y0 - gap * (rows - 1)) / std::max(1, rows)));
    for (size_t i = 0; i < xp.size(); ++i) {
        const ImVec2 a(12 + (i % 3) * (tw + gap), y0 + (i / 3) * (th + gap)), b(a.x + tw, a.y + th);
        char id[64]; std::snprintf(id, sizeof id, "##xp_%s", xp[i]->cmd);
        ImGui::SetCursorScreenPos(a);
        const bool hit = ImGui::InvisibleButton(id, ImVec2(tw, th));
        const Ease e = eased(id, false, ImGui::IsItemHovered(), ImGui::IsItemActive());
        const float in = e.down * 3;
        const ImVec2 a2(a.x + in, a.y + in), b2(b.x - in, b.y - in);
        dl->AddRectFilled(a2, b2, IM_COL32(255, 255, 255, (int)(14 + 16 * e.hot)), 22.0f);
        dl->AddRect(a2, b2, IM_COL32(255, 255, 255, (int)(30 + 50 * e.hot)), 22.0f, 0, 1.2f);
        dl->AddText(f.title, fontPx(f.title), ImVec2(a2.x + 18, a2.y + 18), kInk, xp[i]->label);
        dl->AddText(f.small, fontPx(f.small), ImVec2(a2.x + 18, a2.y + 54), kMuted, xp[i]->what, nullptr, tw - 36);
        if (hit) act.xpilotCmd = xp[i]->cmd;
    }
}

// ---- Games: 2048 and Knots and Crosses against the tablet ---------------------------------------------------------------------------
int g_game = -1;   // the game shown, -1 the list (0 2048, 1 Knots and Crosses, 2.. the arcade games)
bool g_arcKeys = false;            // an arcade game's field was pressed: the computer's keys play it
// The pad is worked by one pointer (the mouse, a finger, a paired device's finger over Local Connect), so a direction tapped on it
// keeps going until it is tapped again: Jump can then be tapped while walking. Sky Patrol fires by itself for the same reason. It
// is the same wherever the tablet is played from.
int g_arcLatch = -1;               // the direction that keeps going (0 left, 1 right, 2 up, 3 down), -1 none
bool g_arcHeld[5] = {}, g_arcHit[5] = {};   // its keys: held, and pressed since the last frame (left, right, up, down, action)
bool g_2048Keys = false;   // its board was pressed: the computer's W A S D and arrows play it
int g_gameKeyDir = -1;     // a move from those keys, waiting for the page
int g_board[16] = {}, g_score = 0, g_best = 0;
float g_pop[16] = {};   // a tile's grow-in (0 to 1)
bool g_2048Over = false;
void addTile() {
    int freeN = 0;
    for (int v : g_board) freeN += v == 0;
    if (!freeN) return;
    int k = std::rand() % freeN;
    for (int i = 0; i < 16; ++i) if (!g_board[i] && k-- == 0) { g_board[i] = (std::rand() % 10) ? 2 : 4; g_pop[i] = 0; return; }
}
void new2048() { for (int& v : g_board) v = 0; g_score = 0; g_2048Over = false; addTile(); addTile(); }
bool slide(int dir) {   // 0 left, 1 right, 2 up, 3 down
    bool movedAny = false;
    for (int line = 0; line < 4; ++line) {
        int idx[4];
        for (int k = 0; k < 4; ++k) {
            const int kk = (dir == 1 || dir == 3) ? 3 - k : k;
            idx[k] = dir <= 1 ? line * 4 + kk : kk * 4 + line;
        }
        int vals[4], n = 0;
        for (int k = 0; k < 4; ++k) if (g_board[idx[k]]) vals[n++] = g_board[idx[k]];
        int out[4] = {0, 0, 0, 0}, o = 0;
        bool merged[4] = {};
        for (int k = 0; k < n; ++k) {
            if (o > 0 && out[o - 1] == vals[k] && !merged[o - 1]) { out[o - 1] *= 2; g_score += out[o - 1]; merged[o - 1] = true; }
            else out[o++] = vals[k];
        }
        for (int k = 0; k < 4; ++k) {
            if (g_board[idx[k]] != out[k]) { movedAny = true; if (merged[k]) g_pop[idx[k]] = 0.4f; }
            g_board[idx[k]] = out[k];
        }
    }
    return movedAny;
}
bool canMove() {
    for (int i = 0; i < 16; ++i) {
        if (!g_board[i]) return true;
        if (i % 4 < 3 && g_board[i] == g_board[i + 1]) return true;
        if (i < 12 && g_board[i] == g_board[i + 4]) return true;
    }
    return false;
}
int g_ttt[9] = {}, g_tttTurn = 1, g_tttResult = 0;   // cells: 0 empty, 1 you (X), 2 the tablet (O); result: 0 on, 1 you, 2 tablet, 3 draw
int tttWinner(const int* c) {
    static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for (auto& l : L) if (c[l[0]] && c[l[0]] == c[l[1]] && c[l[1]] == c[l[2]]) return c[l[0]];
    for (int i = 0; i < 9; ++i) if (!c[i]) return 0;
    return 3;
}
int tttScore(int* c, int who) {   // minimax: +10 the tablet wins, -10 you do
    const int r = tttWinner(c);
    if (r == 2) return 10; if (r == 1) return -10; if (r == 3) return 0;
    int best = who == 2 ? -100 : 100;
    for (int i = 0; i < 9; ++i) if (!c[i]) {
        c[i] = who;
        const int v = tttScore(c, 3 - who);
        c[i] = 0;
        best = who == 2 ? std::max(best, v) : std::min(best, v);
    }
    return best;
}
void tttTabletMove() {
    int bestI = -1, best = -100;
    for (int i = 0; i < 9; ++i) if (!g_ttt[i]) {
        g_ttt[i] = 2;
        int v = tttScore(g_ttt, 1);
        g_ttt[i] = 0;
        if (v > best) { best = v; bestI = i; }
    }
    // the tablet is beatable: about half its moves are a free cell picked at random (it still takes a win it sees half the time)
    if (std::rand() % 100 < 45) {
        int freeN = 0;
        for (int c : g_ttt) freeN += c == 0;
        if (freeN) { int k = std::rand() % freeN; for (int i = 0; i < 9; ++i) if (!g_ttt[i] && k-- == 0) { bestI = i; break; } }
    }
    if (bestI >= 0) g_ttt[bestI] = 2;
}
// ---- the logbook: each flight found by itself (block out, takeoff, landing, block in), kept a line a flight ----------------------------
struct LogEntry {
    std::string date, aircraft, callsign, from, to, route, out, off, on, in;   // date YYYY-MM-DD, times HH:MM (UTC)
    int blockMin = 0, airMin = 0, landings = 0, rateFpm = 0, fuelKg = -1; float nm = 0;
};
std::vector<LogEntry> g_log;          // oldest first
bool g_logChanged = false;
struct LogLive {                       // the flight under way
    int phase = 0;                     // 0 parked, 1 taxiing out, 2 airborne, 3 landed (taxiing in)
    long long outT = 0, offT = 0, onT = 0; double sinceT = 0;   // sim UTC seconds; sinceT: a condition held since (tablet seconds)
    LogEntry e; float fuelOut = -1, lastVs = 0, minVs = 0; double lastLat = 0, lastLon = 0;
} g_live;
std::string hhmm(long long t) { if (t <= 0) return "--:--"; const long long d = ((t % 86400) + 86400) % 86400; char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", (int)(d / 3600), (int)(d / 60 % 60)); return b; }
std::string ymd(long long t) {
    if (t <= 0) return "";
    long long z = t / 86400 + 719468; const long long era = (z >= 0 ? z : z - 146096) / 146097; const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long long y = yoe + era * 400; const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) ++y;
    char b[16]; std::snprintf(b, sizeof b, "%04lld-%02lld-%02lld", y, m, d); return b;
}
// where the aircraft is, as an airport: the plan's (origin before takeoff, destination after landing) when close, else the nearest
std::string whereAirport(const SimState& s, bool departing) {
    const MapState& m = s.map;
    std::string best; double bd = 6.0;   // nm
    for (const AptPoint& a : m.airportPoints) { const double d = distNmLL(m.lat, m.lon, a.lat, a.lon, nullptr); if (d < bd) { bd = d; best = a.icao; } }
    if (s.plan.loaded) { const std::string& p = departing ? s.plan.origin : s.plan.dest; if (best.empty() || best == p) return p.empty() ? best : p; }
    if (best.empty()) { char b[32]; std::snprintf(b, sizeof b, "%.2f%c %.2f%c", std::fabs(m.lat), m.lat < 0 ? 'S' : 'N', std::fabs(m.lon), m.lon < 0 ? 'W' : 'E'); return b; }
    return best;
}
void logStep(const SimState& s) {
    const MapState& m = s.map;
    const GroundState& g = s.ground;
    if (!m.havePos || m.utc <= 0) return;
    LogLive& L = g_live;
    const double now = ImGui::GetTime();
    auto held = [&](bool cond, double secs) { if (!cond) { L.sinceT = 0; return false; } if (L.sinceT <= 0) L.sinceT = now; return now - L.sinceT >= secs; };
    if (L.phase >= 2 && !m.onGround) {   // the distance flown
        if (L.lastLat != 0 || L.lastLon != 0) { const double d = distNmLL(L.lastLat, L.lastLon, m.lat, m.lon, nullptr); if (d < 20) L.e.nm += (float)d; }
        L.lastLat = m.lat; L.lastLon = m.lon;
    }
    if (L.phase == 0) {
        if (held(g.enginesRunning && m.onGround && m.gsKt > 3.0f, 2.0)) {   // block out
            L = LogLive(); L.phase = 1; L.outT = m.utc;
            L.e.date = ymd(m.utc); L.e.aircraft = s.acfIcao; L.e.from = whereAirport(s, true); L.e.out = hhmm(m.utc);
            if (s.plan.loaded) { L.e.callsign = s.plan.callsign; L.e.route = s.plan.route.substr(0, 120); }
            L.fuelOut = s.fuelKg;
        }
    } else if (L.phase == 1) {
        if (held(!m.onGround, 3.0)) { L.phase = 2; L.offT = m.utc; L.e.off = hhmm(m.utc); L.lastLat = m.lat; L.lastLon = m.lon; L.sinceT = 0; }
        else if (m.onGround && !g.enginesRunning && m.gsKt < 1.0f) { L = LogLive(); }   // (back on the stand without flying: no flight)
    } else if (L.phase == 2) {
        if (!m.onGround) L.minVs = std::min(L.minVs, m.vsFpm), L.lastVs = m.vsFpm;
        if (m.onGround) {
            if (L.sinceT <= 0) { L.e.rateFpm = (int)std::lround(std::min(L.lastVs, 0.0f)); ++L.e.landings; }   // (the touchdown: the last rate in the air)
            if (held(true, 2.0)) { L.phase = 3; L.onT = m.utc; L.e.on = hhmm(m.utc); L.sinceT = 0; }
        } else L.sinceT = 0;
    } else if (L.phase == 3) {
        if (!m.onGround) { if (held(true, 3.0)) { L.phase = 2; L.sinceT = 0; } return; }   // (a touch and go: airborne again)
        if (held(m.gsKt < 1.0f && (!g.enginesRunning || g.parkBrake), 5.0)) {   // block in: the flight is logged
            L.e.in = hhmm(m.utc); L.e.to = whereAirport(s, false);
            L.e.blockMin = (int)((m.utc - L.outT) / 60); L.e.airMin = (int)((L.onT - L.offT) / 60);
            if (L.fuelOut >= 0 && s.fuelKg >= 0) L.e.fuelKg = (int)std::lround(std::max(0.0f, L.fuelOut - s.fuelKg));
            g_log.push_back(L.e); g_logChanged = true;
            postNote("Logbook", L.e.from + " to " + L.e.to + " logged: " + std::to_string(L.e.blockMin / 60) + "h " + std::to_string(L.e.blockMin % 60) + "m block", APP_LOGBOOK);
            L = LogLive();
        }
    }
}
float g_logScroll = 0;
void logbookPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h) {
    const float top = kStatusH + kHeaderH + 10;
    // this flight, as it goes
    card(dl, ImVec2(12, top), ImVec2(w - 12, top + 78), f, "THIS FLIGHT");
    const LogLive& L = g_live;
    static const char* phases[4] = {"Parked: the log starts at block out (engines running, moving)", "Taxiing Out", "Airborne", "Landed: Taxiing In"};
    textAt(dl, f.bold, 28, top + 32, L.phase ? kGood : kMuted, phases[L.phase]);
    if (L.phase) {
        char b[160]; std::snprintf(b, sizeof b, "%s  %s   Out %s   Off %s   On %s   %.0f nm", L.e.aircraft.c_str(), L.e.from.c_str(), L.e.out.c_str(), L.e.off.empty() ? "--:--" : L.e.off.c_str(), L.e.on.empty() ? "--:--" : L.e.on.c_str(), L.e.nm);
        textAt(dl, f.small, 28, top + 54, kInk, b);
    }
    // the flights, newest first
    const float la = top + 90, lb = h - 12;
    int totalMin = 0; for (const LogEntry& e : g_log) totalMin += e.blockMin;
    char head[96]; std::snprintf(head, sizeof head, "FLIGHTS  \xC2\xB7  %d  \xC2\xB7  %d:%02d block", (int)g_log.size(), totalMin / 60, totalMin % 60);
    card(dl, ImVec2(12, la), ImVec2(w - 12, lb), f, head);
    const float x0 = 28, rowH = 46, listTop = la + 34;
    static const char* cols[7] = {"Date", "Aircraft", "Route", "Block", "Air", "Landing", "Fuel"};
    static const float cx[7] = {0, 96, 170, 410, 470, 530, 620};
    for (int c = 0; c < 7; ++c) textAt(dl, f.small, x0 + cx[c], listTop - 4, kMuted, cols[c]);
    ImGui::SetCursorScreenPos(ImVec2(12, listTop + 16));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##logbg", ImVec2(w - 24, lb - listTop - 18));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_logScroll -= ImGui::GetIO().MouseWheel * 40;
    touchScroll(g_logScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float shown = lb - listTop - 22, content = g_log.size() * rowH;
    g_logScroll = std::max(0.0f, std::min(g_logScroll, std::max(0.0f, content - shown)));
    if (g_log.empty()) textAt(dl, f.small, x0, listTop + 26, kMuted, "No flights yet: fly one, and it is logged when you are back on a stand.");
    dl->PushClipRect(ImVec2(12, listTop + 16), ImVec2(w - 12, lb - 6), true);
    int del = -1;
    for (int i = (int)g_log.size() - 1, r = 0; i >= 0; --i, ++r) {
        const LogEntry& e = g_log[i];
        const float y = listTop + 20 + r * rowH - g_logScroll;
        if (y > lb || y + rowH < listTop) continue;
        if (r % 2 == 0) dl->AddRectFilled(ImVec2(20, y - 4), ImVec2(w - 20, y + rowH - 8), IM_COL32(255, 255, 255, 10), 8.0f);
        char b[96];
        textAt(dl, f.small, x0 + cx[0], y + 2, kInk, e.date.c_str());
        textAt(dl, f.small, x0 + cx[1], y + 2, kInk, e.aircraft.c_str());
        std::snprintf(b, sizeof b, "%s \xE2\x86\x92 %s", e.from.c_str(), e.to.c_str()); textAt(dl, f.bold, x0 + cx[2], y, kInk, b);
        std::snprintf(b, sizeof b, "%s %s  %s %s", e.out.c_str(), e.off.c_str(), e.on.c_str(), e.in.c_str()); textAt(dl, f.small, x0 + cx[2], y + 20, kMuted, b);
        std::snprintf(b, sizeof b, "%d:%02d", e.blockMin / 60, e.blockMin % 60); textAt(dl, f.small, x0 + cx[3], y + 2, kInk, b);
        std::snprintf(b, sizeof b, "%d:%02d", e.airMin / 60, e.airMin % 60); textAt(dl, f.small, x0 + cx[4], y + 2, kInk, b);
        std::snprintf(b, sizeof b, "%d fpm", e.rateFpm); textAt(dl, f.small, x0 + cx[5], y + 2, e.rateFpm < -600 ? kWarn : kInk, b);
        if (e.landings > 1) { std::snprintf(b, sizeof b, "%d landings", e.landings); textAt(dl, f.small, x0 + cx[5], y + 20, kMuted, b); }
        if (e.fuelKg >= 0) {   // in the flight plan's units (lbs), else kg
            if (s.plan.loaded && s.plan.units == "lbs") std::snprintf(b, sizeof b, "%d lbs", (int)std::lround(e.fuelKg * 2.20462)); else std::snprintf(b, sizeof b, "%d kg", e.fuelKg);
            textAt(dl, f.small, x0 + cx[6], y + 2, kInk, b);
        }
        char id[24]; std::snprintf(id, sizeof id, "##logdel%d", i);
        if (pill(dl, f, id, "Delete", ImVec2(w - 96, y), ImVec2(w - 28, y + 26), false, true)) del = i;
    }
    dl->PopClipRect();
    if (del >= 0) { g_log.erase(g_log.begin() + del); g_logChanged = true; }
}

// ---- Timers: a stopwatch, a hold timer that counts down (a chime at zero), the flight's block and air times, ETA and top of descent
double g_swStart = -1, g_swAcc = 0;      // the stopwatch: running since (tablet seconds), the time before
double g_cdStart = -1; float g_cdSet = 60, g_cdLeft = 60;   // the countdown: running since, its setting, what is left when stopped
bool g_cdRang = false;
void timersPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h) {
    const float top = kStatusH + kHeaderH + 10, colW = std::floor((w - 36) * 0.5f);
    const double now = ImGui::GetTime();
    auto mmss = [](double t, bool tenths) { char b[24]; t = std::max(0.0, t); const int m = (int)(t / 60), sec = (int)t % 60;
        if (tenths) std::snprintf(b, sizeof b, "%02d:%02d.%d", m, sec, (int)(std::fmod(t, 1.0) * 10)); else std::snprintf(b, sizeof b, "%d:%02d", m, sec); return std::string(b); };
    // the stopwatch
    const ImVec2 a1(12, top), b1(12 + colW, top + 170);
    card(dl, a1, b1, f, "STOPWATCH");
    const double sw = g_swAcc + (g_swStart >= 0 ? now - g_swStart : 0);
    textCentered(dl, f.title, (a1.x + b1.x) * 0.5f, a1.y + 56, kInk, mmss(sw, true).c_str());
    const float bw = std::floor((colW - 48) * 0.5f);
    if (pill(dl, f, "##swgo", g_swStart >= 0 ? "Stop" : "Start", ImVec2(a1.x + 16, b1.y - 50), ImVec2(a1.x + 16 + bw, b1.y - 18), g_swStart >= 0, true)) {
        if (g_swStart >= 0) { g_swAcc += now - g_swStart; g_swStart = -1; } else g_swStart = now;
    }
    if (pill(dl, f, "##swreset", "Reset", ImVec2(b1.x - 16 - bw, b1.y - 50), ImVec2(b1.x - 16, b1.y - 18), false, sw > 0)) { g_swAcc = 0; g_swStart = g_swStart >= 0 ? now : -1; }
    // the hold timer
    const ImVec2 a2(b1.x + 12, top), b2(w - 12, top + 170);
    card(dl, a2, b2, f, "HOLD TIMER");
    const double left = g_cdStart >= 0 ? g_cdSet - (now - g_cdStart) : g_cdLeft;
    if (g_cdStart >= 0 && left <= 0 && !g_cdRang) { g_cdRang = true; g_cdStart = -1; g_cdLeft = 0; ++g_chimeSeq; postNote("Timer", "The hold timer is up", APP_TIMERS); }
    textCentered(dl, f.title, (a2.x + b2.x) * 0.5f, a2.y + 56, left <= 0 ? kWarn : kInk, mmss(left, false).c_str());
    static const float presets[4] = {60, 90, 120, 240};
    static const char* pl[4] = {"1:00", "1:30", "2:00", "4:00"};
    const float pw = std::floor((b2.x - a2.x - 32 - 18) / 4);
    for (int i = 0; i < 4; ++i) {
        char id[16]; std::snprintf(id, sizeof id, "##cdp%d", i);
        if (pill(dl, f, id, pl[i], ImVec2(a2.x + 16 + i * (pw + 6), a2.y + 86), ImVec2(a2.x + 16 + i * (pw + 6) + pw, a2.y + 112), g_cdSet == presets[i], g_cdStart < 0)) { g_cdSet = presets[i]; g_cdLeft = presets[i]; g_cdRang = false; }
    }
    if (pill(dl, f, "##cdgo", g_cdStart >= 0 ? "Stop" : "Start", ImVec2(a2.x + 16, b2.y - 50), ImVec2(a2.x + 16 + bw, b2.y - 18), g_cdStart >= 0, left > 0 || g_cdStart >= 0)) {
        if (g_cdStart >= 0) { g_cdLeft = (float)std::max(0.0, left); g_cdStart = -1; } else { g_cdSet = g_cdLeft; g_cdStart = now; g_cdRang = false; }
    }
    if (pill(dl, f, "##cdreset", "Reset", ImVec2(b2.x - 16 - bw, b2.y - 50), ImVec2(b2.x - 16, b2.y - 18), false, true)) { g_cdStart = -1; g_cdLeft = g_cdSet; g_cdRang = false; }
    // the flight: block and air times (from the logbook), and towards the destination: distance, ETA, top of descent
    const ImVec2 a3(12, top + 182), b3(w - 12, h - 12);
    card(dl, a3, b3, f, "THIS FLIGHT");
    const MapState& m = s.map;
    const LogLive& L = g_live;
    auto dur = [](long long secs) { if (secs <= 0) return std::string("--"); char b[16]; std::snprintf(b, sizeof b, "%d:%02d", (int)(secs / 3600), (int)(secs / 60 % 60)); return std::string(b); };
    const long long block = L.phase && m.utc > 0 ? m.utc - L.outT : 0, air = L.phase >= 2 && m.utc > 0 ? (L.phase == 2 ? m.utc : L.onT) - L.offT : 0;
    struct Row { const char* k; std::string v; };
    std::vector<Row> rows = {{"Block Time", dur(block)}, {"Air Time", dur(air)}};
    const FlightPlan& p = s.plan;
    if (p.loaded && !p.legs.empty() && m.havePos) {
        const FlightPlan::Leg& d = p.legs.back();
        const double dist = distNmLL(m.lat, m.lon, d.lat, d.lon, nullptr);
        static std::string toLabel; toLabel = "To " + (p.dest.empty() ? std::string("destination") : p.dest);
        char b[64]; std::snprintf(b, sizeof b, "%.0f nm", dist); rows.push_back({toLabel.c_str(), b});
        if (m.gsKt > 40 && m.utc > 0) rows.push_back({"ETA", hhmm(m.utc + (long long)(dist / m.gsKt * 3600)) + "Z"});
        // top of descent: 3 nm for every 1000 ft to lose (to the destination's field, near sea level if unknown)
        float cruise = 0; { const std::string& c = p.cruise; if (c.size() > 2 && (c[0] == 'F' || c[0] == 'f')) cruise = (float)std::atof(c.c_str() + 2) * 100; else cruise = (float)std::atof(c.c_str()); }
        const float alt = std::max(m.altFt, cruise > 0 ? cruise : m.altFt);
        if (alt > 3000 && !m.onGround) {
            const double todNm = (alt - 1500) / 1000.0 * 3.0, toTod = dist - todNm;
            if (toTod > 0) std::snprintf(b, sizeof b, "in %.0f nm", toTod); else std::snprintf(b, sizeof b, "passed");
            std::string v = b;
            if (toTod > 0 && m.gsKt > 40) v += "  (" + std::to_string((int)(toTod / m.gsKt * 60)) + " min)";
            rows.push_back({"Top Of Descent", v});
        }
    } else rows.push_back({"Destination", "Import a SimBrief plan for the distance, ETA and top of descent"});
    float y = a3.y + 38;
    for (const Row& r : rows) { textAt(dl, f.body, 28, y, kMuted, r.k ? r.k : ""); textAt(dl, f.bold, 220, y, kInk, r.v.c_str()); y += 30; }
}

// ---- ACARS: Hoppie's network. A CPDLC logon to a station, a pre-departure clearance asked for, telex either way; uplinks answered
std::vector<AcarsMsg> g_acLog;          // up and down, oldest first
size_t g_acSeen = 0;                    // how much of the host's inbox is in the log
std::string g_acStation, g_acTo, g_acText, g_acAtis, g_acCall;
int g_acLogon = 0, g_acMin = 0;         // CPDLC: 0 not logged on, 1 asked, 2 logged on (to g_acLogonTo); the next message number
std::string g_acLogonTo;
float g_acScroll = 0;
std::string acCallsign(const SimState& s) { return !g_acCall.empty() ? g_acCall : s.plan.loaded ? s.plan.callsign : std::string(); }
void acSend(const SimState& s, Actions& act, const std::string& to, const std::string& type, const std::string& packet, const std::string& shown) {
    act.acarsOut.push_back({to, type, packet});
    AcarsMsg m; m.up = false; m.station = to; m.type = type; m.text = shown; m.utc = s.map.utc;
    g_acLog.push_back(m);
}
void acarsStep(const SimState& s, Actions& act) {
    const bool on = installed(APP_ACARS) && !g_settings.hoppieCode.empty() && !acCallsign(s).empty();
    act.acarsFrom = on ? acCallsign(s) : std::string();
    while (g_acSeen < s.acars.inbox.size()) {   // the new uplinks into the log: a logon's answer moves the CPDLC state on
        AcarsMsg m = s.acars.inbox[g_acSeen++];
        if (m.utc <= 0) m.utc = s.map.utc;
        if (m.type == "cpdlc") {
            if (m.text.find("LOGON ACCEPTED") != std::string::npos) { g_acLogon = 2; g_acLogonTo = m.station; }
            else if (m.text.find("LOGOFF") != std::string::npos || m.text.find("LOGON REJECTED") != std::string::npos || m.text.find("UNABLE") == 0) { if (m.station == g_acLogonTo) g_acLogon = 0; }
            else if (m.text.compare(0, 9, "HANDOVER ") == 0) { g_acLogon = 0; g_acStation = m.text.substr(9); while (!g_acStation.empty() && g_acStation[0] == '@') g_acStation.erase(0, 1); }
        }
        g_acLog.push_back(m);
        postNote("ACARS", m.station + ": " + m.text.substr(0, 90), APP_ACARS);
        ++g_chimeSeq;
    }
}
void acarsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH + 10;
    const std::string cs = acCallsign(s);
    const bool code = !g_settings.hoppieCode.empty(), ready = code && !cs.empty();
    card(dl, ImVec2(12, top), ImVec2(w - 12, top + 140), f, "DATALINK");
    std::string st;
    if (!code) st = "Add your Hoppie logon code in Settings, Accounts (hoppie.nl, free)";
    else if (cs.empty()) st = "Type a callsign, or import a SimBrief plan";
    else st = cs + "  \xC2\xB7  " + (s.acars.status.empty() ? std::string("Connecting to Hoppie...") : s.acars.status)
              + (g_acLogon == 2 ? "  \xC2\xB7  CPDLC: " + g_acLogonTo : g_acLogon == 1 ? "  \xC2\xB7  CPDLC logon to " + g_acLogonTo + "..." : std::string());
    textAt(dl, f.small, 28, top + 30, ready ? (g_acLogon == 2 ? kGood : kInk) : kWarn, st.c_str());
    auto up = [](std::string& v) { for (char& c : v) c = (char)std::toupper((unsigned char)c); };
    up(g_acStation); up(g_acTo); up(g_acCall); up(g_acAtis); up(g_acText);
    float y = top + 54, x = 28;
    textField(dl, f, "##accall", g_acCall, ImVec2(x, y), ImVec2(x + 112, y + 32), s.plan.loaded && !s.plan.callsign.empty() ? s.plan.callsign.c_str() : "Callsign", false); x += 120;
    textField(dl, f, "##acstn", g_acStation, ImVec2(x, y), ImVec2(x + 100, y + 32), "Station", false); x += 108;
    const bool logged = g_acLogon != 0;
    if (pill(dl, f, "##aclogon", logged ? "CPDLC Logoff" : "CPDLC Logon", ImVec2(x, y), ImVec2(x + 132, y + 32), g_acLogon == 2, ready && (logged || g_acStation.size() >= 3))) {
        if (logged) { acSend(s, act, g_acLogonTo, "cpdlc", cpdlcPacket(++g_acMin, 0, "N", "LOGOFF"), "LOGOFF"); g_acLogon = 0; }
        else { g_acLogonTo = g_acStation; g_acLogon = 1; acSend(s, act, g_acStation, "cpdlc", cpdlcPacket(++g_acMin, 0, "Y", "REQUEST LOGON"), "REQUEST LOGON"); }
    }
    x += 140;
    textField(dl, f, "##acatis", g_acAtis, ImVec2(x, y), ImVec2(x + 64, y + 32), "ATIS", false); x += 72;
    if (g_acAtis.size() > 1) g_acAtis.resize(1);
    if (pill(dl, f, "##acpdc", "Request PDC", ImVec2(x, y), ImVec2(w - 28, y + 32), false, ready && g_acStation.size() >= 3 && s.plan.loaded)) {
        std::string t = "REQUEST PREDEP CLEARANCE " + cs + " " + (s.acfIcao.empty() ? std::string("ZZZZ") : s.acfIcao) + " TO " + s.plan.dest + " AT " + s.plan.origin;
        if (!g_acAtis.empty()) t += " ATIS " + g_acAtis;
        acSend(s, act, g_acStation, "telex", t, t);
    }
    y += 44; x = 28;
    textField(dl, f, "##acto", g_acTo, ImVec2(x, y), ImVec2(x + 112, y + 32), "To", false); x += 120;
    textField(dl, f, "##actext", g_acText, ImVec2(x, y), ImVec2(w - 132, y + 32), "Telex Message", false);
    if (pill(dl, f, "##acsend", "Send", ImVec2(w - 124, y), ImVec2(w - 28, y + 32), false, ready && g_acTo.size() >= 3 && !g_acText.empty())) {
        acSend(s, act, g_acTo, "telex", g_acText, g_acText); g_acText.clear();
        if (g_typeInto == &g_acText) { g_kbd = false; g_typeInto = nullptr; }
    }
    // the messages, newest first; an uplink that wants an answer has its buttons
    const float la = top + 152, lb = h - 12;
    card(dl, ImVec2(12, la), ImVec2(w - 12, lb), f, "MESSAGES");
    const float listTop = la + 30, rowH = 52;
    ImGui::SetCursorScreenPos(ImVec2(12, listTop));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##acbg", ImVec2(w - 24, std::max(1.0f, lb - listTop - 4)));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) g_acScroll -= ImGui::GetIO().MouseWheel * 40;
    touchScroll(g_acScroll, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // each row as tall as its text (a clearance runs to a few lines)
    auto wants = [](const AcarsMsg& m) { return m.up && m.type == "cpdlc" && (m.ra == "WU" || m.ra == "AN" || m.ra == "R" || m.ra == "Y") && (m.answer.empty() || m.answer == "STANDBY"); };
    auto shown = [](const AcarsMsg& m) { return m.answer.empty() ? m.text : m.text + "   [" + m.answer + "]"; };
    std::vector<float> rh(g_acLog.size());
    float content = 0;
    for (size_t i = 0; i < g_acLog.size(); ++i) {
        const float tr = wants(g_acLog[i]) ? w - 300 : w - 36;
        rh[i] = std::max(rowH, 26 + f.body->CalcTextSizeA(fontPx(f.body), FLT_MAX, tr - 28, shown(g_acLog[i]).c_str()).y + 10);
        content += rh[i];
    }
    g_acScroll = std::max(0.0f, std::min(g_acScroll, std::max(0.0f, content - (lb - listTop - 8))));
    if (g_acLog.empty()) textAt(dl, f.small, 28, listTop + 6, kMuted, ready ? "No messages yet. Log on to a station, ask for a clearance, or send a telex." : "Messages from Hoppie's network show here.");
    dl->PushClipRect(ImVec2(12, listTop), ImVec2(w - 12, lb - 4), true);
    float ryAt = listTop + 4 - g_acScroll;
    for (int i = (int)g_acLog.size() - 1, r = 0; i >= 0; --i, ++r) {
        AcarsMsg& m = g_acLog[i];
        const float ry = ryAt, rowHi = rh[i];
        ryAt += rowHi;
        if (ry > lb || ry + rowHi < listTop) continue;
        if (r % 2 == 0) dl->AddRectFilled(ImVec2(20, ry - 4), ImVec2(w - 20, ry + rowHi - 8), IM_COL32(255, 255, 255, 10), 8.0f);
        char head[96]; std::snprintf(head, sizeof head, "%s  %s %s  %s", hhmm(m.utc).c_str(), m.up ? "\xE2\x86\x93 from" : "\xE2\x86\x91 to", m.station.c_str(), m.type == "cpdlc" ? "CPDLC" : "telex");
        textAt(dl, f.small, 28, ry, m.up ? accentLite(255) : kMuted, head);
        const bool wantsIt = wants(m);
        const float textR = wantsIt ? w - 300 : w - 36;
        const std::string t = shown(m);
        dl->AddText(f.body, fontPx(f.body), ImVec2(28, ry + 18), kInk, t.c_str(), nullptr, textR - 28);
        if (wantsIt) {
            const char* a[3]; int n = 0;
            if (m.ra == "WU") { a[0] = "WILCO"; a[1] = "UNABLE"; a[2] = "STANDBY"; n = 3; }
            else if (m.ra == "AN") { a[0] = "AFFIRM"; a[1] = "NEGATIVE"; a[2] = "STANDBY"; n = 3; }
            else { a[0] = "ROGER"; n = 1; }
            if (m.answer == "STANDBY") n = std::min(n, 2);
            float bx = w - 28;
            for (int k = n - 1; k >= 0; --k) {
                const float bw = 84; bx -= bw;
                char id[32]; std::snprintf(id, sizeof id, "##acr%d_%d", i, k);
                if (pill(dl, f, id, a[k], ImVec2(bx, ry + 6), ImVec2(bx + bw, ry + 36), k == 0, ready)) {
                    m.answer = a[k];
                    acSend(s, act, m.station, "cpdlc", cpdlcPacket(++g_acMin, m.min, "N", a[k]), a[k]);
                }
                bx -= 6;
            }
        }
    }
    dl->PopClipRect();
    pageKeyboard(dl, f, w, h, act);
}

void seasonalStep() {
    if (g_settings.wallpaper >= kSeasonal0 && !seasonalAvailable(g_settings.wallpaper - kSeasonal0)) {   // its season over: the theme before
        g_settings.wallpaper = std::max(0, std::min(kSeasonal0 - 1, g_settings.wallpaperBase));
        g_settings.accent = std::min(g_settings.wallpaper, 4); g_settingsChanged = true;
    }
    static bool told = false;   // once each start, when the tablet is on: a theme whose season it is, not announced yet this year
    if (told || g_power != POWER_ON) return;
    told = true;
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    const int year = lt.tm_year + 1900;
    for (int k = 0; k < kSeasonalN; ++k) {
        if (!seasonalOn(k, year, lt.tm_mon + 1, lt.tm_mday)) continue;   // (the real date, even where all are offered)
        const std::string key = std::to_string(k) + "-" + std::to_string((k == 0 || k == 19) && lt.tm_mon == 11 ? year + 1 : year);   // (New Year's Eve, and a winter's December, count with the new year)
        const std::string& t = g_settings.seasonalTold;
        if ((" " + t + " ").find(" " + key + " ") != std::string::npos) continue;
        g_settings.seasonalTold = t.empty() ? key : t + " " + key;
        if (g_settings.seasonalTold.size() > 400) g_settings.seasonalTold.erase(0, g_settings.seasonalTold.find(' ', g_settings.seasonalTold.size() - 400) + 1);
        g_settingsChanged = true;
        postNote("Color Theme", std::string("The ") + kSeasonal[k].name + (k >= 16 ? " theme is here for the season" : k >= 14 ? " theme is here for the month" : " theme is here for the holiday") + ": Settings, Display, Seasonal", APP_SETTINGS);
    }
}
void resetLogAndTimers() { g_acLog.clear(); g_acStation.clear(); g_acTo.clear(); g_acText.clear(); g_acAtis.clear(); g_acCall.clear(); g_acLogon = 0; g_acLogonTo.clear(); g_log.clear(); g_logChanged = true; g_live = LogLive(); g_swStart = -1; g_swAcc = 0; g_cdStart = -1; g_cdSet = g_cdLeft = 60; g_cdRang = false; }
void gamesPage(ImDrawList* dl, const Fonts& f, float w, float h) {
    const float top = kStatusH + kHeaderH;
    const float dt = ImGui::GetIO().DeltaTime;
    if (g_game < 0) {   // the list: four across, two rows (2048, Knots and Crosses, then the arcade games)
        struct G { const char* name; const char* what; ImU32 col; };
        const G games[8] = {{"2048", "Slide the tiles; equal ones join. Reach 2048.", IM_COL32(237, 194, 46, 255)},
                            {"Knots and Crosses", "Three in a row against the tablet.", IM_COL32(80, 170, 240, 255)},
                            {arcade::name(0), arcade::blurb(0), IM_COL32(70, 150, 230, 255)},
                            {arcade::name(1), arcade::blurb(1), IM_COL32(60, 170, 100, 255)},
                            {arcade::name(2), arcade::blurb(2), IM_COL32(120, 90, 230, 255)},
                            {arcade::name(3), arcade::blurb(3), IM_COL32(230, 120, 50, 255)},
                            {arcade::name(4), arcade::blurb(4), IM_COL32(30, 120, 90, 255)},
                            {arcade::name(5), arcade::blurb(5), IM_COL32(90, 100, 130, 255)}};
        const float gap = 12, bw = std::floor((w - 32 - 3 * gap) / 4), bh = std::floor((h - top - 40 - gap) * 0.5f);
        for (int i = 0; i < 8; ++i) {
            const ImVec2 a(16 + (i % 4) * (bw + gap), top + 26 + (i / 4) * (bh + gap)), b(a.x + bw, a.y + bh);
            char id[16]; std::snprintf(id, sizeof id, "##game%d", i);
            ImGui::SetCursorScreenPos(a);
            const bool hit = ImGui::InvisibleButton(id, ImVec2(bw, bh));
            const Ease e = eased(id, false, ImGui::IsItemHovered(), ImGui::IsItemActive());
            const float in = e.down * 3;
            dl->AddRectFilled(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), mixCol(mixCol(games[i].col, IM_COL32(0, 0, 0, 255), 0.45f), games[i].col, 0.45f + 0.35f * e.hot), 24.0f);
            dl->AddText(f.bold, fontPx(f.bold), ImVec2(a.x + 16, a.y + 14), IM_COL32(255, 255, 255, 255), games[i].name);
            dl->AddText(f.small, fontPx(f.small), ImVec2(a.x + 16, a.y + 40), IM_COL32(240, 244, 250, 220), games[i].what, nullptr, bw - 32);
            // a little picture of it, at the bottom right
            const ImVec2 pc(b.x - 56, b.y - 46);
            if (i == 0) {
                static const char* v[4] = {"2", "4", "8", "16"};
                for (int k = 0; k < 4; ++k) {
                    const ImVec2 t0(pc.x - 30 + (k % 2) * 31, pc.y - 30 + (k / 2) * 31);
                    dl->AddRectFilled(t0, ImVec2(t0.x + 28, t0.y + 28), IM_COL32(255, 255, 255, 60 + k * 30), 6.0f);
                    textCentered(dl, f.small, t0.x + 14, t0.y + 14 - fontPx(f.small) * 0.5f, IM_COL32(255, 255, 255, 255), v[k]);
                }
            } else if (i == 1) {
                for (int k = 1; k < 3; ++k) {
                    dl->AddLine(ImVec2(pc.x - 30 + k * 20, pc.y - 30), ImVec2(pc.x - 30 + k * 20, pc.y + 30), IM_COL32(255, 255, 255, 150), 2.5f);
                    dl->AddLine(ImVec2(pc.x - 30, pc.y - 30 + k * 20), ImVec2(pc.x + 30, pc.y - 30 + k * 20), IM_COL32(255, 255, 255, 150), 2.5f);
                }
                dl->AddLine(ImVec2(pc.x - 26, pc.y - 26), ImVec2(pc.x - 14, pc.y - 14), IM_COL32(255, 255, 255, 255), 3.0f);
                dl->AddLine(ImVec2(pc.x - 14, pc.y - 26), ImVec2(pc.x - 26, pc.y - 14), IM_COL32(255, 255, 255, 255), 3.0f);
                dl->AddCircle(ImVec2(pc.x, pc.y), 6, IM_COL32(255, 220, 140, 255), 0, 3.0f);
                dl->AddCircle(ImVec2(pc.x + 20, pc.y + 20), 6, IM_COL32(255, 220, 140, 255), 0, 3.0f);
            } else arcade::icon(i - 2, dl, pc, 42.0f);
            if (hit) {
                g_game = i;
                if (i == 0) { bool empty = true; for (int v : g_board) if (v) empty = false; if (empty) new2048(); }
                if (i >= 2 && !arcade::started(i - 2)) arcade::reset(i - 2);
                for (int k = 0; k < 5; ++k) g_arcHeld[k] = g_arcHit[k] = false;
                g_arcLatch = -1;
            }
        }
        return;
    }
    if (pill(dl, f, "##gamesback", "All Games", ImVec2(16, top + 12), ImVec2(116, top + 40), false, true)) { g_game = -1; return; }
    if (g_game >= 2) {   // an arcade game: its field at the left, the score and its pad at the right
        const int ag = g_game - 2;
        const float panelW = 220;
        const ImVec2 fa(16, top + 52), fb(w - 16 - panelW - 16, h - 12);
        // the field (a press on it gives it the computer's keys)
        ImGui::SetCursorScreenPos(fa);
        ImGui::InvisibleButton("##arcfield", ImVec2(fb.x - fa.x, fb.y - fa.y));
        const bool fieldJustPressed = ImGui::IsItemActivated(), fieldReleased = ImGui::IsItemDeactivated();
        if (fieldJustPressed) g_arcKeys = true;
        const bool fieldHeld = ImGui::IsItemActive();
        const bool pad = arcade::usesPad(ag);
        // the pad: four directions and the action, held as long as they are pressed (the tap games have none)
        const float px0 = fb.x + 16, px1 = w - 16;
        const float bs = 44, padCx = px0 + 56, padCy = h - 12 - bs * 1.5f - 6;
        bool padHeld[5] = {}, padHit[5] = {};
        auto padBtn = [&](int k, ImVec2 c, const char* id, float r) {
            ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
            ImGui::InvisibleButton(id, ImVec2(r * 2, r * 2));
            padHeld[k] = ImGui::IsItemActive(); padHit[k] = ImGui::IsItemActivated();
            const Ease e = eased(id, padHeld[k] || (k < 4 && g_arcLatch == k), ImGui::IsItemHovered(), padHeld[k]);
            dl->AddRectFilled(ImVec2(c.x - r + 2, c.y - r + 2), ImVec2(c.x + r - 2, c.y + r - 2), mixCol(IM_COL32(255, 255, 255, 26 + (int)(20 * e.hot)), accentCol(230), e.on), k == 4 ? r : 12.0f);
            return c;
        };
        if (pad) {
        const ImVec2 cL = padBtn(0, ImVec2(padCx - bs, padCy), "##arcL", bs * 0.5f), cR = padBtn(1, ImVec2(padCx + bs, padCy), "##arcR", bs * 0.5f);
        const ImVec2 cU = padBtn(2, ImVec2(padCx, padCy - bs), "##arcU", bs * 0.5f), cD = padBtn(3, ImVec2(padCx, padCy + bs), "##arcD", bs * 0.5f);
        const ImVec2 cA = padBtn(4, ImVec2(px1 - 30, padCy + bs * 0.4f), "##arcA", 28.0f);
        auto tri = [&](ImVec2 c, int d) {   // an arrow in a pad button
            const float r = 9;
            ImVec2 p[3];
            if (d == 0) { p[0] = ImVec2(c.x - r, c.y); p[1] = ImVec2(c.x + r * 0.6f, c.y - r); p[2] = ImVec2(c.x + r * 0.6f, c.y + r); }
            else if (d == 1) { p[0] = ImVec2(c.x + r, c.y); p[1] = ImVec2(c.x - r * 0.6f, c.y + r); p[2] = ImVec2(c.x - r * 0.6f, c.y - r); }
            else if (d == 2) { p[0] = ImVec2(c.x, c.y - r); p[1] = ImVec2(c.x + r, c.y + r * 0.6f); p[2] = ImVec2(c.x - r, c.y + r * 0.6f); }
            else { p[0] = ImVec2(c.x, c.y + r); p[1] = ImVec2(c.x - r, c.y - r * 0.6f); p[2] = ImVec2(c.x + r, c.y - r * 0.6f); }
            dl->AddTriangleFilled(p[0], p[1], p[2], kInk);
        };
        tri(cL, 0); tri(cR, 1); tri(cU, 2); tri(cD, 3);
        textCentered(dl, f.small, cA.x, cA.y - fontPx(f.small) * 0.5f, kInk, ag == arcade::SKY_PATROL ? "Fire" : ag == arcade::HANGAR_CLIMB ? "Jump" : "Go");
        }
        // a press beside the field and the pad gives the keys back to X-Plane
        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) g_arcKeys = false;
        // a swipe on the field steers too (a finger's quick drag: a direction); the tap games get the finger itself
        arcade::Input in;
        in.px = ImGui::GetIO().MousePos.x; in.py = ImGui::GetIO().MousePos.y;
        in.press = fieldJustPressed; in.held = fieldHeld; in.release = fieldReleased;
        if (pad) {
            static ImVec2 from; static bool swiped = false;
            if (ImGui::IsItemActivated()) {}
            if (fieldHeld && ImGui::IsMouseClicked(0)) { from = ImGui::GetIO().MousePos; swiped = false; }
            if (fieldHeld && !swiped) {
                const ImVec2 m = ImGui::GetIO().MousePos; const float dx = m.x - from.x, dy = m.y - from.y;
                if (std::fabs(dx) > 24 || std::fabs(dy) > 24) {
                    swiped = true;
                    const int d = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
                    if (d == 0) in.leftHit = true; else if (d == 1) in.rightHit = true; else if (d == 2) in.upHit = true; else in.downHit = true;
                }
            }
        }
        if (pad) {   // a direction tapped on the pad (or swiped on the field) keeps going, tapped again it stops; Sky Patrol fires by itself
            int tapped = in.leftHit ? 0 : in.rightHit ? 1 : in.upHit ? 2 : in.downHit ? 3 : -1;   // (a swipe on the field)
            bool again = false;
            for (int k = 0; k < 4; ++k) if (padHit[k]) { tapped = k; again = g_arcLatch == k; }
            if (tapped >= 0) g_arcLatch = again ? -1 : tapped;
            if (g_arcLatch >= 0) padHeld[g_arcLatch] = true;
            if (ag == arcade::SKY_PATROL) padHeld[4] = true;
        } else g_arcLatch = -1;
        in.left = g_arcHeld[0] || padHeld[0]; in.right = g_arcHeld[1] || padHeld[1]; in.up = g_arcHeld[2] || padHeld[2]; in.down = g_arcHeld[3] || padHeld[3]; in.action = g_arcHeld[4] || padHeld[4];
        in.leftHit = in.leftHit || g_arcHit[0] || padHit[0]; in.rightHit = in.rightHit || g_arcHit[1] || padHit[1]; in.upHit = in.upHit || g_arcHit[2] || padHit[2];
        in.downHit = in.downHit || g_arcHit[3] || padHit[3]; in.actionHit = g_arcHit[4] || padHit[4];
        for (bool& b : g_arcHit) b = false;
        if (!g_arcKeys) for (bool& b : g_arcHeld) b = false;
        arcade::step(ag, in, dt);
        const int sc = arcade::score(ag);
        if (sc > g_settings.arcadeBest[ag]) { g_settings.arcadeBest[ag] = sc; g_settingsChanged = true; }
        dl->PushClipRect(fa, fb, true);
        arcade::draw(ag, dl, fa, fb, f.title, fontPx(f.title), f.small, fontPx(f.small));
        dl->PopClipRect();
        if (g_arcKeys) dl->AddRect(ImVec2(fa.x - 2, fa.y - 2), ImVec2(fb.x + 2, fb.y + 2), accentLite(200), 6.0f, 0, 2.0f);   // (it has the keys)
        // the panel: name, score, best, lives, New game, how to play
        float y = top + 52;
        textAt(dl, f.title, px0, y, kInk, arcade::name(ag)); y += 36;
        char t[48];
        std::snprintf(t, sizeof t, ag == arcade::COCKPIT_21 ? "Miles %d" : "Score %d", sc); textAt(dl, f.bold, px0, y, kInk, t); y += 24;
        std::snprintf(t, sizeof t, "Best %d", g_settings.arcadeBest[ag]); textAt(dl, f.body, px0, y, kMuted, t);
        if (arcade::lives(ag) >= 0) { std::snprintf(t, sizeof t, "Lives %d", std::max(0, arcade::lives(ag))); textRight(dl, f.body, px1, y, kMuted, t); }
        y += 30;
        if (pill(dl, f, "##arcnew", "New Game", ImVec2(px0, y), ImVec2(px1, y + 30), false, true)) { arcade::reset(ag); g_arcKeys = true; g_arcLatch = -1; }
        y += 40;
        dl->AddText(f.small, fontPx(f.small), ImVec2(px0, y), kMuted, arcade::help(ag), nullptr, px1 - px0);
        return;
    }
    if (g_game == 0) {   // 2048: swipe on the board, or the arrow keys
        // the board at the left under All games; the score, the best and New game in a column to its right (nothing over the board)
        const float side = std::floor(std::min(h - top - 62, w * 0.5f)), cell = side / 4;
        const ImVec2 o(16, top + 52);
        const float px = o.x + side + 32;
        char t[48];
        std::snprintf(t, sizeof t, "Score %d", g_score); textAt(dl, f.title, px, o.y + 4, kInk, t);
        std::snprintf(t, sizeof t, "Best %d", g_best); textAt(dl, f.body, px, o.y + 42, kMuted, t);
        if (pill(dl, f, "##2048new", "New Game", ImVec2(px, o.y + 78), ImVec2(px + 140, o.y + 108), false, true)) new2048();
        ImGui::SetCursorScreenPos(o);
        ImGui::InvisibleButton("##2048board", ImVec2(side, side));
        static ImVec2 from;
        if (ImGui::IsItemActivated()) { from = ImGui::GetIO().MousePos; g_2048Keys = true; }   // (pressed: it takes W A S D)
        int dir = g_gameKeyDir;
        g_gameKeyDir = -1;
        if (ImGui::IsItemDeactivated()) {
            const ImVec2 to = ImGui::GetIO().MousePos;
            const float dx = to.x - from.x, dy = to.y - from.y;
            if (std::fabs(dx) > 30 || std::fabs(dy) > 30) dir = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
        }
        // the keypad, under the help: four arrows in a cross
        {
            const float kx = px + 54, ky = o.y + 190, kb = 46;
            struct K { float x, y; int d; };
            const K keys[4] = {{kx, ky, 2}, {kx - kb - 6, ky + kb + 6, 0}, {kx, ky + kb + 6, 3}, {kx + kb + 6, ky + kb + 6, 1}};
            for (const K& k : keys) {
                char id[16]; std::snprintf(id, sizeof id, "##2048k%d", k.d);
                const ImVec2 ka(k.x, k.y), kb2(k.x + kb, k.y + kb);
                ImGui::SetCursorScreenPos(ka);
                const bool hit = ImGui::InvisibleButton(id, ImVec2(kb, kb));
                const Ease e = eased(id, false, ImGui::IsItemHovered(), ImGui::IsItemActive());
                const float in = e.down * 2;
                dl->AddRectFilled(ImVec2(ka.x + in, ka.y + in), ImVec2(kb2.x - in, kb2.y - in), mixCol(IM_COL32(255, 255, 255, 30), mixCol(IM_COL32(255, 255, 255, 64), accentCol(120), 0.4f), e.hot + e.down), 12.0f);
                if (e.hot > 0.02f) dl->AddRect(ImVec2(ka.x + in, ka.y + in), ImVec2(kb2.x - in, kb2.y - in), accentLite((int)(150 * e.hot)), 12.0f, 0, 1.2f);
                const ImVec2 c((ka.x + kb2.x) * 0.5f, (ka.y + kb2.y) * 0.5f);
                const float r = 9;
                ImVec2 p0, p1, p2;   // a triangle pointing its way
                if (k.d == 0) { p0 = ImVec2(c.x - r, c.y); p1 = ImVec2(c.x + r * 0.6f, c.y - r); p2 = ImVec2(c.x + r * 0.6f, c.y + r); }
                else if (k.d == 1) { p0 = ImVec2(c.x + r, c.y); p1 = ImVec2(c.x - r * 0.6f, c.y + r); p2 = ImVec2(c.x - r * 0.6f, c.y - r); }
                else if (k.d == 2) { p0 = ImVec2(c.x, c.y - r); p1 = ImVec2(c.x + r, c.y + r * 0.6f); p2 = ImVec2(c.x - r, c.y + r * 0.6f); }
                else { p0 = ImVec2(c.x, c.y + r); p1 = ImVec2(c.x - r, c.y - r * 0.6f); p2 = ImVec2(c.x + r, c.y - r * 0.6f); }
                dl->AddTriangleFilled(p0, p1, p2, kInk);
                if (hit) dir = k.d;
            }
        }
        // a press anywhere but the board and the keypad: the keys go back to X-Plane
        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) g_2048Keys = false;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dir = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dir = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dir = 3;
        if (dir >= 0 && !g_2048Over && slide(dir)) { addTile(); g_best = std::max(g_best, g_score); g_settingsChanged = true; if (!canMove()) g_2048Over = true; }
        dl->AddRectFilled(o, ImVec2(o.x + side, o.y + side), IM_COL32(187, 173, 160, 255), 14.0f);
        for (int i = 0; i < 16; ++i) {
            g_pop[i] = std::min(1.0f, g_pop[i] + dt * 6.0f);
            const ImVec2 c0(o.x + (i % 4) * cell + 6, o.y + (i / 4) * cell + 6), c1(c0.x + cell - 12, c0.y + cell - 12);
            dl->AddRectFilled(c0, c1, IM_COL32(205, 193, 180, 255), 8.0f);
            const int v = g_board[i];
            if (!v) continue;
            int lv = 0; for (int x = v; x > 2; x >>= 1) ++lv;
            static const ImU32 cols[] = {IM_COL32(238, 228, 218, 255), IM_COL32(237, 224, 200, 255), IM_COL32(242, 177, 121, 255), IM_COL32(245, 149, 99, 255), IM_COL32(246, 124, 95, 255), IM_COL32(246, 94, 59, 255), IM_COL32(237, 207, 114, 255), IM_COL32(237, 204, 97, 255), IM_COL32(237, 200, 80, 255), IM_COL32(237, 197, 63, 255), IM_COL32(237, 194, 46, 255), IM_COL32(60, 58, 50, 255)};
            const float pe = g_pop[i], sc = pe < 1 ? 0.6f + 0.4f * (1 - (1 - pe) * (1 - pe)) : 1.0f;
            const ImVec2 mc((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), hs((c1.x - c0.x) * 0.5f * sc, (c1.y - c0.y) * 0.5f * sc);
            dl->AddRectFilled(ImVec2(mc.x - hs.x, mc.y - hs.y), ImVec2(mc.x + hs.x, mc.y + hs.y), cols[std::min(11, lv)], 8.0f);
            char num[12]; std::snprintf(num, sizeof num, "%d", v);
            ImFont* fn = v < 1000 ? f.title : f.bold;
            textCentered(dl, fn, mc.x, std::floor(mc.y - fontPx(fn) * 0.5f), lv < 2 ? IM_COL32(119, 110, 101, 255) : IM_COL32(249, 246, 242, 255), num);
        }
        if (g_2048Over) {
            dl->AddRectFilled(o, ImVec2(o.x + side, o.y + side), IM_COL32(238, 228, 218, 170), 14.0f);
            textCentered(dl, f.title, o.x + side * 0.5f, o.y + side * 0.5f - 14, IM_COL32(119, 110, 101, 255), "No Moves Left");
        }
        dl->AddText(f.small, fontPx(f.small), ImVec2(px, o.y + 126), kMuted, g_2048Keys ? "Swipe on the board, or play with W A S D or the arrow keys (press beside the board to give them back to X-Plane)." : "Swipe on the board, or press it to play with W A S D or the arrow keys.", nullptr, w - 16 - px);
        if (g_2048Keys) dl->AddRect(ImVec2(o.x - 3, o.y - 3), ImVec2(o.x + side + 3, o.y + side + 3), accentLite(220), 16.0f, 0, 2.0f);   // (it has the keys)
        return;
    }
    // Knots and Crosses: you are X and go first; the tablet answers
    const float side = std::floor(std::min(h - top - 62, 340.0f)), cell = side / 3;
    const ImVec2 o(std::floor((w - side) * 0.5f) - 120, top + 52);
    const float px = o.x + side + 40;
    if (pill(dl, f, "##tttnew", "New Game", ImVec2(px, o.y + 52), ImVec2(px + 140, o.y + 82), false, true)) { for (int& c : g_ttt) c = 0; g_tttResult = 0; }
    for (int i = 0; i < 9; ++i) {
        const ImVec2 c0(o.x + (i % 3) * cell, o.y + (i / 3) * cell), c1(c0.x + cell, c0.y + cell);
        char id[16]; std::snprintf(id, sizeof id, "##ttt%d", i);
        ImGui::SetCursorScreenPos(c0);
        if (ImGui::InvisibleButton(id, ImVec2(cell, cell)) && !g_ttt[i] && !g_tttResult) {
            g_ttt[i] = 1;
            g_tttResult = tttWinner(g_ttt);
            if (!g_tttResult) { tttTabletMove(); g_tttResult = tttWinner(g_ttt); }
        }
        if (ImGui::IsItemHovered() && !g_ttt[i] && !g_tttResult) dl->AddRectFilled(ImVec2(c0.x + 6, c0.y + 6), ImVec2(c1.x - 6, c1.y - 6), IM_COL32(255, 255, 255, 18), 12.0f);
        const ImVec2 mc((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f);
        const float r = cell * 0.28f;
        if (g_ttt[i] == 1) {
            dl->AddLine(ImVec2(mc.x - r, mc.y - r), ImVec2(mc.x + r, mc.y + r), accentLite(255), 7.0f);
            dl->AddLine(ImVec2(mc.x + r, mc.y - r), ImVec2(mc.x - r, mc.y + r), accentLite(255), 7.0f);
        } else if (g_ttt[i] == 2) dl->AddCircle(mc, r, IM_COL32(255, 190, 90, 255), 0, 7.0f);
    }
    for (int k = 1; k < 3; ++k) {
        dl->AddLine(ImVec2(o.x + k * cell, o.y + 8), ImVec2(o.x + k * cell, o.y + side - 8), IM_COL32(255, 255, 255, 90), 3.0f);
        dl->AddLine(ImVec2(o.x + 8, o.y + k * cell), ImVec2(o.x + side - 8, o.y + k * cell), IM_COL32(255, 255, 255, 90), 3.0f);
    }
    const char* msg = g_tttResult == 1 ? "You Win!" : g_tttResult == 2 ? "The Tablet Wins" : g_tttResult == 3 ? "A Draw" : "Your Move (X)";
    textAt(dl, f.title, px, o.y + 4, g_tttResult == 1 ? kGood : kInk, msg);
}

} // namespace
// One page, with its title bar
void drawPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act, int app) {
    {
        // pages sit on a darkened wallpaper of their own (solid, so nothing behind shows through while one grows out of its icon)
        dl->PushClipRect(ImVec2(0, kStatusH), ImVec2(w, h), true);
        wallpaper(dl, w, h);
        dl->PopClipRect();
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), themeTone(8, 18, 36, 170));   // the veil, in the theme's tone
        const int pageV0 = dl->VtxBuffer.Size;
        struct SubFade { ImDrawList* dl; int v0; ~SubFade() { const float t = (float)(ImGui::GetTime() - g_subT0) / 0.18f; if (t < 1.0f) fadeSince(dl, v0, std::max(0.0f, t)); } } subFade{dl, pageV0};   // (into or out of an app's own page: a quick fade)
        if (!(app == APP_BROWSER && videoFull(s)) && pageHeader(dl, f, w, app == APP_BROWSER ? g_site.c_str() : appName(app))) {
            if (g_backTo >= 0) { const int to = g_backTo; g_backTo = -1; if (g_kbdTarget == 2 || g_kbdTarget == 1) { g_kbd = false; g_kbdTarget = 0; g_typeInto = nullptr; } g_app = to; g_subT0 = ImGui::GetTime(); g_animPage = -1; }   // (back into the app it came from)
            else openApp(APP_HOME);
        }
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
        else if (app == APP_APPSTORE) storePage(dl, f, w, h, act);
        else if (app == APP_NAVSEARCH) navSearchPage(dl, f, s, w, h, act);
        else if (app == APP_GAMES) gamesPage(dl, f, w, h);
        else if (app == APP_TIMERS) timersPage(dl, f, s, w, h);
        else if (app == APP_LOGBOOK) logbookPage(dl, f, s, w, h);
        else if (app == APP_ACARS) acarsPage(dl, f, s, w, h, act);
        else if (app == APP_VATSIMHUB) vatsimHubPage(dl, f, w, h);
        else if (app == APP_XPILOT) xpilotPage(dl, f, s, w, h, act);
        else placeholderPage(dl, f, app, w, h);
    }
}

int currentApp() { return g_app; }
bool gameWantsKeys() { return g_app == APP_GAMES && ((g_2048Keys && g_game == 0) || (g_arcKeys && g_game >= 2)); }
void gameKey(int dir) { g_gameKeyDir = dir; }
bool arcadeOnScreen() { return g_app == APP_GAMES && g_game >= 2; }
void gameKeyEvent(int key, bool down) {
    if (key < 0 || key > 4) return;
    if (g_game == 0) { if (down && key < 4) g_gameKeyDir = key; return; }
    if (down && !g_arcHeld[key]) g_arcHit[key] = true;
    g_arcHeld[key] = down;
}
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
void setBattery(float pct) { (g_battFromDevice ? g_ownBatt : g_batt) = std::max(0.0f, std::min(100.0f, pct)); }   // (a device's battery showing: the tablet's own)
bool hasShape(const std::string& icao) { float l, sp; return exactShape(icao, l, sp) != nullptr; }   // a drawing of its own
const unsigned char* wxLogoPixels(unsigned int& bytes) { bytes = xpt_asset_wxlogo_size; return xpt_asset_wxlogo; }
const unsigned char* logoPixels(int& w, int& h, int& levels) {   // "XPTL", width, height, levels (u32), then the pixels
    const unsigned char* b = xpt_asset_logo;
    auto u32 = [&](int at) { return (int)((unsigned)b[at] | ((unsigned)b[at + 1] << 8) | ((unsigned)b[at + 2] << 16) | ((unsigned)b[at + 3] << 24)); };
    if (xpt_asset_logo_size < 16 || std::memcmp(b, "XPTL", 4) != 0) { w = h = levels = 0; return nullptr; }
    w = u32(4); h = u32(8); levels = u32(12);
    return b + 16;
}
const unsigned char* logoMaskPixels(int& w, int& h, int& levels, float& tabletFrom) {
    static std::vector<unsigned char> mask;
    static float from = 0.5f;
    const unsigned char* p = logoPixels(w, h, levels);
    if (!p) return nullptr;
    if (mask.empty()) {
        size_t n = 0; int lw = w, lh = h;
        for (int l = 0; l < levels; ++l) { n += (size_t)lw * lh * 4; lw = std::max(1, lw / 2); lh = std::max(1, lh / 2); }
        mask.assign(p, p + n);
        for (size_t i = 0; i < n; i += 4) mask[i] = mask[i + 1] = mask[i + 2] = 255;
        int first = w;   // TABLET: the first column with a coloured (not white) letter in it
        for (int x = 0; x < w && first == w; ++x)
            for (int y = 0; y < h; ++y) {
                const unsigned char* q = p + ((size_t)y * w + x) * 4;
                const int mx = std::max(q[0], std::max(q[1], q[2])), mn = std::min(q[0], std::min(q[1], q[2]));
                if (q[3] > 128 && mx - mn > 60) { first = x; break; }
            }
        from = first < w ? (float)first / w : 0.5f;
    }
    tabletFrom = from;
    return mask.data();
}
// the nth letter of TABLET (0 T, 1 A...) in the logo: its box, as fractions of the picture (false: not found)
bool logoLetterBox(int n, float& u0, float& u1, float& v0, float& v1) {
    static std::vector<std::array<float, 4>> boxes;
    static bool done = false;
    if (!done) {
        done = true;
        int w, h, l; float from = 0.5f;
        const unsigned char* p = logoPixels(w, h, l);
        logoMaskPixels(w, h, l, from);
        if (!p) return false;
        int x = (int)(from * w), start = -1, top = h, bot = -1;
        for (; x <= w; ++x) {
            bool any = false;
            if (x < w) for (int y = 0; y < h; ++y) if (p[((size_t)y * w + x) * 4 + 3] > 64) { any = true; top = std::min(top, y); bot = std::max(bot, y); }
            if (any && start < 0) start = x;
            if (!any && start >= 0) { boxes.push_back({(float)start / w, (float)x / w, (float)top / h, (float)(bot + 1) / h}); start = -1; top = h; bot = -1; }
        }
    }
    if (n < 0 || n >= (int)boxes.size()) return false;
    u0 = boxes[n][0]; u1 = boxes[n][1]; v0 = boxes[n][2]; v1 = boxes[n][3];
    return true;
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
    default: return app >= 0 && app < APP_COUNT && installed(app);
    }
}
bool appInstalled(int app) { return installed(app); }
void layoutHome() { normalizeLayout(); }
void pushInWidget(int widget, int start, int cap) { pushInWidgetAt(widget, start, cap); }

// Charts, Live Map, SimBrief and ForeFlight are sites shown as pages of their own. The Browser icon opens the full browser.
void openApp(int app) {
    g_backTo = -1;   // (opened from Home: its Back goes Home)
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

std::string logbookToText() {
    std::string out;
    auto clean = [](std::string v) { for (char& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return v; };
    for (const LogEntry& e : g_log) {
        char nums[96]; std::snprintf(nums, sizeof nums, "\t%d\t%d\t%d\t%d\t%d\t%.1f", e.blockMin, e.airMin, e.landings, e.rateFpm, e.fuelKg, (double)e.nm);
        out += "flight\t" + clean(e.date) + "\t" + clean(e.aircraft) + "\t" + clean(e.callsign) + "\t" + clean(e.from) + "\t" + clean(e.to) + "\t" + clean(e.out) + "\t" + clean(e.off) + "\t" + clean(e.on) + "\t" + clean(e.in) + nums + "\t" + clean(e.route) + "\n";
    }
    return out;
}
void logbookFromText(const std::string& text) {
    g_log.clear();
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p); p = e + 1;
        std::vector<std::string> f; size_t a = 0;
        while (a <= line.size()) { size_t t = line.find('\t', a); if (t == std::string::npos) t = line.size(); f.push_back(line.substr(a, t - a)); a = t + 1; }
        if (f.size() < 17 || f[0] != "flight") continue;
        LogEntry L; L.date = f[1]; L.aircraft = f[2]; L.callsign = f[3]; L.from = f[4]; L.to = f[5]; L.out = f[6]; L.off = f[7]; L.on = f[8]; L.in = f[9];
        L.blockMin = std::atoi(f[10].c_str()); L.airMin = std::atoi(f[11].c_str()); L.landings = std::atoi(f[12].c_str()); L.rateFpm = std::atoi(f[13].c_str());
        L.fuelKg = std::atoi(f[14].c_str()); L.nm = (float)std::atof(f[15].c_str()); L.route = f[16];
        g_log.push_back(L);
    }
}
void pictureColors(const unsigned char* rgba, int w, int h, int& average, int& accent) {
    average = accent = -1;
    if (!rgba || w <= 0 || h <= 0) return;
    const size_t n = (size_t)w * h;
    const size_t step = std::max<size_t>(1, n / 40000);   // (about 40 000 pixels looked at)
    double r = 0, g = 0, b = 0; size_t cnt = 0;
    double hw[36] = {}, hr[36] = {}, hg[36] = {}, hb[36] = {};   // the hues, weighted by how vivid
    for (size_t i = 0; i < n; i += step) {
        const unsigned char* p = rgba + i * 4;
        r += p[0]; g += p[1]; b += p[2]; ++cnt;
        const float R = p[0] / 255.0f, G = p[1] / 255.0f, B = p[2] / 255.0f;
        const float mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B)), d = mx - mn;
        if (mx < 0.2f || d < 0.12f) continue;   // (dark or grey: no colour of its own)
        const float s = d / mx;
        float hue = mx == R ? std::fmod((G - B) / d, 6.0f) : mx == G ? (B - R) / d + 2 : (R - G) / d + 4;
        if (hue < 0) hue += 6;
        const int bin = std::min(35, (int)(hue * 6));
        const double wt = s * s * mx;
        hw[bin] += wt; hr[bin] += wt * R; hg[bin] += wt * G; hb[bin] += wt * B;
    }
    if (!cnt) return;
    average = ((int)(r / cnt * 0.8) << 16) | ((int)(g / cnt * 0.8) << 8) | (int)(b / cnt * 0.8);
    int best = -1; double bw = 0;
    for (int i = 0; i < 36; ++i) { const double s3 = hw[i] + 0.5 * (hw[(i + 35) % 36] + hw[(i + 1) % 36]); if (s3 > bw) { bw = s3; best = i; } }
    if (best < 0 || bw < cnt * 0.002) { accent = 0x46A0FF; return; }   // (no real colour in it: the tablet's blue)
    float R = (float)(hr[best] / hw[best]), G = (float)(hg[best] / hw[best]), B = (float)(hb[best] / hw[best]);
    // made clear: its hue kept, saturation at least 0.55 and brightness about 0.95
    const float mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B));
    float s = mx > 0 ? (mx - mn) / mx : 0; s = std::max(0.55f, std::min(0.9f, s));
    auto chan = [&](float c) { const float t = mx > mn ? (c - mn) / (mx - mn) : 0; return 0.95f * (1 - s + s * t); };
    accent = ((int)std::lround(chan(R) * 255) << 16) | ((int)std::lround(chan(G) * 255) << 8) | (int)std::lround(chan(B) * 255);
}
void setSeasonalPreview(bool all) { g_seasonalAll = all; }
int settingsTabShown() { return g_settingsTab; }
void celebrate(int which) { g_celebrate = which; g_celebT0 = ImGui::GetTime(); g_celebOutT0 = -1; }
bool celebrationShown() { return g_celebrate >= 0; }
unsigned settingsReveal() { return g_reveal; }
bool seasonalOnDate(int k, int year, int month, int day) { return seasonalOn(k, year, month, day); }
const char* seasonalName(int k) { return k >= 0 && k < kSeasonalN ? kSeasonal[k].name : ""; }
bool takeLogbookChanged() { const bool c = g_logChanged; g_logChanged = false; return c; }
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
    std::snprintf(buf, sizeof buf, "map_navaids=%d\nmap_waypoints=%d\nmap_route=%d\nmap_procedures=%d\nstatus_local_time=%d\nauto_dim=%d\n", g_settings.mapNavaids ? 1 : 0, g_settings.mapWaypoints ? 1 : 0,
                  g_settings.mapRoute ? 1 : 0, g_settings.mapProcedures ? 1 : 0, g_settings.statusLocalTime ? 1 : 0, g_settings.autoDim ? 1 : 0);
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
        out += "wallpaper_base=" + std::to_string(g_settings.wallpaperBase) + "\nseasonal_told=" + g_settings.seasonalTold + "\n";
        { std::string v = g_settings.hoppieCode; for (char& c : v) if (c == '\n' || c == '\r') c = ' '; out += "hoppie_code=" + v + "\n"; }
    }
    out += std::string("map_airspaces=") + (g_settings.mapAirspaces ? "1" : "0") + "\n";
    out += std::string("map_dark_auto=") + (g_settings.mapDarkAuto ? "1" : "0") + "\n";
    out += std::string("cabin_auto=") + (g_settings.cabinAuto ? "1" : "0") + "\n";
    { char ab[128]; std::snprintf(ab, sizeof ab, "arcade_best=%d,%d,%d,%d,%d,%d\n", g_settings.arcadeBest[0], g_settings.arcadeBest[1], g_settings.arcadeBest[2], g_settings.arcadeBest[3], g_settings.arcadeBest[4], g_settings.arcadeBest[5]); out += ab; }
    out += std::string("local_connect=") + (g_settings.localConnect ? "1" : "0") + "\nlocal_battery=" + (g_settings.localBattery ? "1" : "0") + "\n";
    out += std::string("map_vatsim_atc=") + (g_settings.mapVatsimAtc ? "1" : "0") + "\nvatsim_atc_notes=" + (g_settings.vatsimAtcNotes ? "1" : "0") + "\n";
    for (int i = 0; i < 3; ++i) {   // the Customs: colours, and the picture's file name
        char cb[96]; std::snprintf(cb, sizeof cb, "custom%d_bg=%06X\ncustom%d_accent=%06X\n", i, g_settings.customs[i].bg & 0xFFFFFF, i, g_settings.customs[i].accent & 0xFFFFFF); out += cb;
        std::string pic = g_settings.customs[i].picture;
        for (char& ch : pic) if (ch == '\n' || ch == '\r') ch = ' ';
        out += "custom" + std::to_string(i) + "_picture=" + pic + "\n";
    }
    {   // the home screen: its pages, folders (name, a tab, then the apps) and Volanta's overlay address
        out += "home_pages=" + std::to_string(g_settings.homePages) + "\n";
        for (const auto& fo : g_settings.folders) {
            std::string n = fo.name;
            for (char& c : n) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            out += "folder=" + n + "\t";
            for (size_t i = 0; i < fo.apps.size(); ++i) { if (i) out += ","; if (fo.apps[i] >= 0 && fo.apps[i] < APP_COUNT) out += kApps[fo.apps[i]].id; }
            out += "\n";
        }
        std::string v = g_settings.volantaOverlay;
        for (char& c : v) if (c == '\n' || c == '\r') c = ' ';
        out += "volanta_overlay=" + v + "\n";
    }
    std::snprintf(buf, sizeof buf, "map_source=%d\nmap_airports=%d\nmap_timezones=%d\nmap_heading_up=%d\nmap_local_time=%d\nmap_traffic=%d\nmap_outlines=%d\nmap_dark=%d\nmap_ils=%d\nmap_rings=%d\nmap_trail=%d\nphysical_keys=%d\nwallpaper=%d\naccent=%d\nbattery=%d\ncharger=%d\n",
                  g_settings.mapSource, g_settings.mapAirports ? 1 : 0, g_settings.mapTimeZones ? 1 : 0, g_settings.mapHeadingUp ? 1 : 0, g_settings.mapLocalTime ? 1 : 0,
                  g_settings.mapTraffic ? 1 : 0, g_settings.mapOutlines ? 1 : 0, g_settings.mapDark ? 1 : 0, g_settings.mapIls ? 1 : 0, g_settings.mapRings ? 1 : 0, g_settings.mapTrail ? 1 : 0, g_settings.physicalKeys ? 1 : 0, g_settings.wallpaper, g_settings.accent, (int)std::ceil((g_battFromDevice ? g_ownBatt : g_batt) - 0.001f), (g_battFromDevice ? g_ownCharger : g_charger) ? 1 : 0);   // (the tablet's own, not a device's)
    out += buf;
    auto list = [&](const char* key, const std::vector<int>& v) {
        out += key; out += "=";
        for (size_t i = 0; i < v.size(); ++i) {   // "-": a free place; "@2": folder 2; "#1": widget 1
            if (i) out += ",";
            if (v[i] >= 0 && v[i] < APP_COUNT) out += kApps[v[i]].id;
            else if (v[i] >= kWidget0 && v[i] < kWidget0 + W_COUNT) out += "#" + std::to_string(v[i] - kWidget0);
            else if (v[i] >= kFolder0 && v[i] < kWidget0) out += "@" + std::to_string(v[i] - kFolder0);
            else out += "-";
        }
        out += "\n";
    };
    list("order", g_settings.order);
    {
        out += "installed=";
        for (size_t i = 0; i < g_settings.installed.size(); ++i) { if (i) out += ","; out += g_settings.installed[i]; }
        out += "\n";
    }
    list("favorites", g_settings.favorites);
    for (const auto& bm : g_settings.bookmarks) {   // title, a tab, the address (tabs and line ends taken out of the title)
        std::string t = bm.first;
        for (char& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out += "bookmark=" + t + "\t" + bm.second + "\n";
    }
    for (const auto& hp : g_settings.history) {
        std::string t = hp.first;
        for (char& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        std::string u = hp.second;
        for (char& c : u) if (c == '\n' || c == '\r') c = ' ';
        out += "history=" + t + "\t" + u + "\n";
    }
    std::snprintf(buf, sizeof buf, "browser_volume=%.2f\n", (double)g_settings.browserVolume);
    out += buf;
    auto oneLine = [](std::string v) { for (char& c : v) if (c == '\n' || c == '\r') c = ' '; return v; };
    out += "gates_for=" + oneLine(g_settings.gatesFor) + "\nstart_gate=" + oneLine(g_settings.startGate) + "\narrival_gate=" + oneLine(g_settings.arrivalGate) + "\n";
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
            else if (id.size() > 1 && id[0] == '#') v.push_back(kWidget0 + std::max(0, std::min(W_COUNT - 1, std::atoi(id.c_str() + 1))));
            else if (id.size() > 1 && id[0] == '@') v.push_back(kFolder0 + std::max(0, std::min(999, std::atoi(id.c_str() + 1))));
            else for (int a = 0; a < APP_COUNT; ++a) if (id == kApps[a].id) v.push_back(a);
            p = e + 1;
        }
    };
    bool foldersRead = false;
    g_settings.bookmarks.clear();   // (read again, the file's bookmarks replace these rather than adding a second set)
    g_settings.history.clear();
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
        else if (key == "map_dark_auto") g_settings.mapDarkAuto = v != 0;
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
        else if (key == "map_vatsim_atc") g_settings.mapVatsimAtc = v != 0;
        else if (key == "map_airspaces") g_settings.mapAirspaces = v != 0;
        else if (key == "cabin_auto") g_settings.cabinAuto = v != 0;
        else if (key == "vatsim_atc_notes") g_settings.vatsimAtcNotes = v != 0;
        else if (key == "local_connect") g_settings.localConnect = v != 0;
        else if (key == "arcade_best") { int a6[6] = {0, 0, 0, 0, 0, 0}; std::sscanf(val.c_str(), "%d,%d,%d,%d,%d,%d", &a6[0], &a6[1], &a6[2], &a6[3], &a6[4], &a6[5]); for (int i = 0; i < 6; ++i) g_settings.arcadeBest[i] = std::max(0, a6[i]); }
        else if (key == "local_battery") g_settings.localBattery = v != 0;
        else if (key == "map_traffic_range") g_settings.mapTrafficRangeNm = std::max(0.0f, std::min(2000.0f, (float)std::atof(val.c_str())));
        else if (key == "map_traffic_alt") g_settings.mapTrafficAltFt = std::max(0.0f, std::min(60000.0f, (float)std::atof(val.c_str())));
        else if (key == "physical_keys") g_settings.physicalKeys = v != 0;
        else if (key == "custom_bg") g_settings.customs[0].bg = (int)std::strtol(val.c_str(), nullptr, 16) & 0xFFFFFF;   // (one Custom, before)
        else if (key == "custom_accent") g_settings.customs[0].accent = (int)std::strtol(val.c_str(), nullptr, 16) & 0xFFFFFF;
        else if (key.size() > 8 && key.compare(0, 6, "custom") == 0 && key[6] >= '0' && key[6] <= '2' && key[7] == '_') {
            Settings::CustomTheme& ct = g_settings.customs[key[6] - '0'];
            const std::string what = key.substr(8);
            if (what == "bg") ct.bg = (int)std::strtol(val.c_str(), nullptr, 16) & 0xFFFFFF;
            else if (what == "accent") ct.accent = (int)std::strtol(val.c_str(), nullptr, 16) & 0xFFFFFF;
            else if (what == "picture") ct.picture = val;
        }
        else if (key == "wallpaper") { g_settings.wallpaper = std::max(0, std::min(kThemes - 1, v)); g_settings.accent = g_settings.wallpaper; }
        else if (key == "wallpaper_base") g_settings.wallpaperBase = std::max(0, std::min(kSeasonal0 - 1, v));
        else if (key == "seasonal_told") g_settings.seasonalTold = val;
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
        else if (key == "map_procedures") g_settings.mapProcedures = v != 0;
        else if (key == "map_route") g_settings.mapRoute = v != 0;
        else if (key == "simbrief_user") g_settings.simbriefUser = val;
        else if (key == "hoppie_code") g_settings.hoppieCode = val;
        else if (key == "status_local_time") g_settings.statusLocalTime = v != 0;
        else if (key == "auto_dim") g_settings.autoDim = v != 0;
        else if (key == "favorite_slots") g_settings.favoriteSlots = std::max(0, std::min(kMaxFavorites, v));
        else if (key == "brightness") g_settings.brightness = std::max(0.1f, std::min(1.0f, (float)std::atof(val.c_str())));
        else if (key == "show_charts") g_settings.showCharts = v != 0;
        else if (key == "show_simbrief") g_settings.showSimbrief = v != 0;
        else if (key == "show_live") g_settings.showLive = v != 0;
        else if (key == "show_foreflight") g_settings.showForeflight = v != 0;
        else if (key == "order") parseList(val, g_settings.order);
        else if (key == "home_pages") g_settings.homePages = std::max(1, std::min(20, std::atoi(val.c_str())));
        else if (key == "volanta_overlay") g_settings.volantaOverlay = val;
        else if (key == "folder") {
            if (!foldersRead) { g_settings.folders.clear(); foldersRead = true; }
            Settings::Folder fo;
            const size_t tab = val.find('\t');
            fo.name = val.substr(0, tab);
            if (tab != std::string::npos) parseList(val.substr(tab + 1), fo.apps);
            g_settings.folders.push_back(fo);
        }
        else if (key == "favorites") parseList(val, g_settings.favorites);
        else if (key == "bookmark") {
            const size_t tab = val.find('\t');
            if (tab != std::string::npos && tab + 1 < val.size()) g_settings.bookmarks.push_back({val.substr(0, tab), val.substr(tab + 1)});
        }
        else if (key == "history") {
            const size_t tab = val.find('\t');
            if (tab != std::string::npos && tab + 1 < val.size() && g_settings.history.size() < 200) g_settings.history.push_back({val.substr(0, tab), val.substr(tab + 1)});
        }
        else if (key == "installed") {
            g_settings.installed.clear();
            size_t p0 = 0;
            while (p0 <= val.size()) { size_t e = val.find(',', p0); if (e == std::string::npos) e = val.size(); if (e > p0) g_settings.installed.push_back(val.substr(p0, e - p0)); p0 = e + 1; }
            g_installedRead = true;
        }
        else if (key == "gates_for") g_settings.gatesFor = val;
        else if (key == "start_gate") g_settings.startGate = val;
        else if (key == "arrival_gate") g_settings.arrivalGate = val;
        else if (key == "browser_volume") g_settings.browserVolume = std::max(0.0f, std::min(1.0f, (float)std::atof(val.c_str())));
    }
    if (!newStyle) {   // an older settings file: the site its one icon was set to
        if (oldChart == 1) { g_settings.showNavigraph = g_settings.showCharts; g_settings.showCharts = false; }
        if (oldLive == 1) { g_settings.showVatsim = g_settings.showLive; g_settings.showLive = false; }
        if (oldPlan == 1) { g_settings.showSkyvector = g_settings.showForeflight; g_settings.showForeflight = false; }
    }
    normalizeLayout();
}

void setTouchInput(bool on) { g_touchInput = on; }
// A press must find the pointer already on its spot: the tablet's buttons that let others overlap them (the home screen's icons,
// the pages' rows) only take a press where the pointer was the frame before (Dear ImGui's rule). A finger lands without hovering
// first, and a mouse in the sim can too: its press arrives together with its position whenever the sim did not report the
// pointer the frame before (a slow frame rate, a click straight after a move), and such presses were lost (2026-11-21: apps
// opened only after many clicks). So every pointer, the sim's mouse, a finger in the browser and a paired device's, is queued
// here and fed a frame at a time: a press or release whose spot the pointer is not on yet moves it there this frame and lands on
// the next, and a press and its release (a quick tap) never fall in one frame.
namespace { struct MouseEv { float x, y; int down; }; std::vector<MouseEv> g_mouseQ; }
void queuePointer(float x, float y, int down) { g_mouseQ.push_back({x, y, down}); }
bool pointerPending() { for (const MouseEv& e : g_mouseQ) if (e.down >= 0) return true; return false; }
void pumpPointer() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    auto whole = [](float v) { return v > -FLT_MAX ? ImFloor(v) : v; };   // (as Dear ImGui keeps positions)
    size_t i = 0;
    for (; i < g_mouseQ.size(); ++i) {
        const MouseEv e = g_mouseQ[i];
        if (e.down < 0) { io.AddMousePosEvent(e.x, e.y); continue; }   // moves: all of them now
        // is the pointer on the spot already, by the frame just drawn, with nothing waiting to move it?
        bool moving = false;
        for (const ImGuiInputEvent& ev : g.InputEventsQueue) if (ev.Type == ImGuiInputEventType_MousePos) { moving = true; break; }
        if (moving || io.MousePos.x != whole(e.x) || io.MousePos.y != whole(e.y)) { io.AddMousePosEvent(e.x, e.y); break; }   // there first; the press or release waits for the next frame
        io.AddMouseButtonEvent(0, e.down != 0);   // one press or release a frame
        ++i;
        break;
    }
    if (i > 0) g_mouseQ.erase(g_mouseQ.begin(), g_mouseQ.begin() + i);
}

} // namespace xpt
