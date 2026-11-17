// The tablet's user interface, drawn with Dear ImGui and nothing else: no X-Plane calls here, so the same code runs inside the
// plugin and in the off-line picture test. The host (src/plugin.cpp) gives it the sim's state and gets the user's requests back.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

namespace xpt {

constexpr float kLogicalH = 480.0f;   // the tablet is always laid out 480 units tall; its width follows the screen's shape

// The tablet's apps. Charts, Live Map, SimBrief and ForeFlight are web apps: each opens its site as a page of its own.
enum App { APP_HOME = -1, APP_MAP, APP_AIRPORTS, APP_CHARTS, APP_DOCUMENTS, APP_ROUTES, APP_WEATHER, APP_GROUND, APP_CHECKLISTS,
           APP_PERFORMANCE, APP_TRAFFIC, APP_NOTES, APP_SIMBRIEF, APP_LIVEMAP, APP_FOREFLIGHT, APP_BROWSER, APP_SETTINGS, APP_COUNT };

constexpr int kMaxFavorites = 5;   // the favorites bar holds up to 5 icons; its size is chosen in Settings, 0 removes it

// What the user chose. Kept between sessions by the host (settingsToText / settingsFromText).
struct Settings {
    int chartsProvider = 0;       // what the Charts icon is: 0 ChartFox, 1 Navigraph
    int liveProvider = 0;         // what the Live Map icon is: 0 Volanta, 1 VATSIM Map
    bool showCharts = true, showSimbrief = true, showLive = true, showForeflight = true;   // web app icons on the home screen
    std::vector<int> order;       // the home screen's icons, in the order the user arranged them
    int favoriteSlots = 4;        // how many icons the favorites bar holds, 0 (no bar) to kMaxFavorites
    std::vector<int> favorites = {APP_MAP, APP_CHARTS, APP_WEATHER, APP_SIMBRIEF};   // the icons in the bar along the bottom (they leave the grid)
};
Settings& settings();
std::string settingsToText();
void settingsFromText(const std::string& text);
bool takeSettingsChanged();   // true once after the user changed a setting: time to save

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* mono = nullptr;
};

struct SimState {
    std::string acfIcao;          // "A321", "" when unknown
    int zuluSec = -1;             // seconds since 00:00Z, -1 when unknown
    bool onPanel = false;         // drawn on the aircraft's own tablet (else only the floating window)
    int panel[4] = {0, 0, 0, 0};  // left, bottom, width, height on the panel texture
    bool panelEnabled = false, panelPowered = false;
    bool aviTabRunning = false;   // AviTab is installed and active: the cockpit tablet stays AviTab's
    bool wxPlane = false;         // WXPlane is installed
    bool toliss = false;          // a ToLiss aircraft: its own tablet does the performance calculations, so ours is left out
    float brightness = 1.0f;
    float pixelScale = 1.0f;      // screen pixels per layout unit
    int batteryPct = 100;         // the tablet's own battery
    bool charging = false;        // the aircraft's electrical system is feeding it
};

struct Actions {
    bool setBrightness = false;
    float brightness = 1.0f;
};

Fonts loadFonts(float pixelScale);   // (re)builds the font atlas for this many pixels per layout unit
void applyStyle();
void build(const Fonts& f, const SimState& s, float w, float h, Actions& act);   // one frame of the tablet, w x h layout units

int currentApp();
void openApp(int app);   // APP_HOME: the home screen
const char* appName(int app);                // the icon's label (Charts and Live Map carry their provider's name)
bool appVisible(int app);                 // on the home screen (web app icons can be switched off)
const char* browserUrl();                 // the address the web page shows
bool browserChrome();                     // the full browser (address bar, bookmarks) rather than a web app's own page

} // namespace xpt
