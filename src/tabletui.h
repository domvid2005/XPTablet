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
    float brightness = 1.0f;      // the screen's brightness set in Settings, 0.1 to 1 (on top of what the aircraft sets)
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
    ImTextureID wxLogo = 0;       // the WXPlane icon's picture (wxLogoPixels), uploaded by the host; 0: the icon is drawn without it
};

// The WXPlane icon's picture: 256 x 256 RGBA (WXPlane's logo, WX above PLANE), followed by its mip levels down to 1 x 1.
const unsigned char* wxLogoPixels(unsigned int& bytes);

// X-Plane's own weather where the aircraft is (the built-in Weather page, shown when WXPlane is not installed).
struct SimWeather {
    bool valid = false;
    float windDir = 0, windKt = 0;           // degrees true, knots
    float visSm = 0, tempC = 0, dewC = 0, qnhHpa = 0, rain = 0;   // rain: 0..1 on the aircraft
    float altFt = 0;                         // the aircraft's altitude
    int clouds = 0;                          // layers with any cover
    float cloudBaseFt[3] = {0, 0, 0}, cloudTopFt[3] = {0, 0, 0}, cloudCover[3] = {0, 0, 0};   // cover 0..1
    int aloft = 0;                           // wind layers
    float aloftFt[13] = {}, aloftDir[13] = {}, aloftKt[13] = {};
};

// The METAR search on the built-in Weather page: what the host last got back.
struct MetarLookup {
    bool busy = false;            // a request is on its way
    std::string icao, raw, error; // the answer: raw is the report, or error says why there is none
};

// The ground services the aircraft and the scenery offer right now (the Ground page).
struct GroundState {
    bool onGround = true;
    bool openSam = false;         // openSAM is installed: it drives the jetways and reports their state
    int jetway = 0;               // openSAM's state: 0 no jetway at this stand, 1 ready to dock, 2 docked, -1 moving or unavailable
    bool toliss = false;          // a ToLiss aircraft: its doors, chocks and ground power can be set
    int paxDoors = 0, cargoDoors = 0;        // how many of each this aircraft has
    int paxDoor[8] = {}, cargoDoor[4] = {};  // 0 closed, 1 automatic, 2 open (ToLiss's door modes); cargo: forward, aft, bulk
    bool deice = false;           // the aircraft offers de-icing
    bool chocks = false, extPower = false, hpAir = false, lpAir = false;
};

struct SimState {
    std::string acfIcao;          // "A321", "" when unknown
    int zuluSec = -1;             // seconds since 00:00Z, -1 when unknown
    bool onPanel = false;         // drawn on the aircraft's own tablet (else only the floating window)
    int panel[4] = {0, 0, 0, 0};  // left, bottom, width, height on the panel texture
    bool panelEnabled = false, panelPowered = false;
    bool aviTabRunning = false;   // AviTab is installed and active: the cockpit tablet stays AviTab's
    bool wxPlane = false;         // WXPlane is installed
    int wxEmbed = 0;              // WXPlane drew itself on the Weather page: 1 yes, -1 asked but it did not, 0 not asked yet
    bool toliss = false;          // a ToLiss aircraft: its own tablet does the performance calculations, so ours is left out
    float brightness = 1.0f;      // what the aircraft sets for its tablet (avitab/brightness)
    float pixelScale = 1.0f;      // screen pixels per layout unit
    int batteryPct = 100;         // the tablet's own battery
    bool charging = false;        // the aircraft's electrical system is feeding it
    SimWeather wx;
    MetarLookup metar;
    GroundState ground;
};

// What the user asked for on the Ground page; the host carries it out.
enum GroundAction { GA_NONE, GA_JETWAY, GA_GPU, GA_CHOCKS, GA_HP_AIR, GA_LP_AIR, GA_TRUCKS, GA_PUSH_LEFT, GA_PUSH_STRAIGHT, GA_PUSH_RIGHT, GA_PUSH_STOP,
                    GA_PAX_DOOR, GA_CARGO_DOOR, GA_DOORS_CLOSE, GA_DOORS_AUTO, GA_DEICE, GA_DEICE_QUICK };

struct Actions {
    int ground = GA_NONE;
    int groundIndex = 0;          // which door, for GA_PAX_DOOR / GA_CARGO_DOOR
    std::string metarSearch;      // look this airport's METAR up (the Weather page's search)
};

Fonts loadFonts(float pixelScale);   // (re)builds the font atlas for this many pixels per layout unit
void applyStyle();
void build(const Fonts& f, const SimState& s, float w, float h, Actions& act);   // one frame of the tablet, w x h layout units

// The part of the screen another plugin draws (WXPlane on the Weather page), and the pointer in it. In layout units.
struct EmbedArea {
    bool active = false;
    float x = 0, y = 0, w = 0, h = 0;
    float mx = -1e9f, my = -1e9f;   // the pointer, from the area's top-left corner; far outside when not over it
    bool down = false;
    float wheel = 0;
};
const EmbedArea& embedArea();   // as of the last build()

int currentApp();
void openApp(int app);   // APP_HOME: the home screen
const char* appName(int app);                // the icon's label (Charts and Live Map carry their provider's name)
bool appVisible(int app);                 // on the home screen (web app icons can be switched off)
int touchTestCount();                     // marks in the Settings touch test (for the tests)
const char* browserUrl();                 // the address the web page shows
bool browserChrome();                     // the full browser (address bar, bookmarks) rather than a web app's own page

} // namespace xpt
