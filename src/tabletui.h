// The tablet's user interface, drawn with Dear ImGui and nothing else: no X-Plane calls here, so the same code runs inside the
// plugin and in the off-line picture test. The host (src/plugin.cpp) gives it the sim's state and gets the user's requests back.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aptgeo.h"
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
    int planProvider = 0;         // what the flight planning icon is: 0 ForeFlight, 1 SkyVector
    bool showCharts = true, showSimbrief = true, showLive = true, showForeflight = true;   // web app icons on the home screen
    std::vector<int> order;       // the home screen's icons, in the order the user arranged them
    float brightness = 1.0f;      // the screen's brightness set in Settings, 0.1 to 1 (on top of what the aircraft sets)
    int favoriteSlots = 4;        // how many icons the favorites bar holds, 0 (no bar) to kMaxFavorites
    std::vector<int> favorites = {APP_MAP, APP_CHARTS, APP_WEATHER, APP_SIMBRIEF};   // the icons in the bar along the bottom (they leave the grid)
    std::vector<std::pair<std::string, std::string>> bookmarks;   // the browser's own bookmarks: title, address
    // the Map page
    int mapSource = 0;            // 0 OpenStreetMap, 1 satellite imagery
    bool mapAirports = true;      // X-Plane's airport layouts (apt.dat) over the map
    bool mapTimeZones = false;    // the time zone boundaries
    bool mapHeadingUp = false;    // Heading Lock (else North Lock)
    bool mapLocalTime = false;    // the time bubble shows local time (else Zulu)
    bool mapTraffic = true;       // AI and multiplayer aircraft
    bool mapOutlines = false;     // coastlines, borders, states and provinces
    bool mapNavaids = true;       // VORs, NDBs, DMEs and ILS localizers
    bool mapWaypoints = false;    // fixes
    bool mapRoute = true;         // the SimBrief flight plan
    std::string simbriefUser;     // SimBrief user name or pilot ID (for the route on the map)
    bool statusLocalTime = false; // the status bar's clock: local time where the aircraft is (else Zulu)
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
    ImTextureID logo = 0;         // XPTablet's logo (logoPixels), uploaded by the host; 0: the name is written instead
};

// XPTablet's logo (XP in white, TABLET in the gradient): w x h RGBA, followed by its halved copies down to 1 pixel high.
const unsigned char* logoPixels(int& w, int& h, int& levels);

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
    // ToLiss: the doors a jetway can reach (1L; 2L too on an A321 with the classic door layout), each with the cabin door it is
    // (an index into paxDoor) and whether a jetway is docked there (openSAM, by door)
    int jwDoors = 0;
    int jwDoorPax[3] = {0, 2, 4};
    bool jwDocked[3] = {false, false, false};
    int slides = -1;              // ToLiss: the cabin doors' slides, 1 armed, 0 disarmed, -1 unknown
    // what keeps a jetway from connecting (the Ground page's status)
    bool beacon = false, enginesRunning = false, parkBrake = false;
    float groundSpeedKt = 0;
    float cabinLight = -1;        // ToLiss: the cabin lights, 0..1, -1 unknown
};

// The web browser (a program of its own, run by the plugin): what it shows.
struct BrowserState {
    int status = -2;              // -3 not on this computer (message), -2 not started, 0 starting, 1 running, -1 stopped (message)
    std::string message;
    ImTextureID tex = 0;          // the page's picture, texW x texH screen pixels
    int texW = 0, texH = 0;
    std::string url, title;
    bool loading = false, canBack = false, canForward = false;
    bool wantsKeyboard = false;   // a text field on the page has the focus
    struct Tab { int id = 0; std::string title, url; bool loading = false; };
    std::vector<Tab> tabs;        // the open pages, in order
    int activeTab = 0;            // the shown one's id
};

// The Map page: the aircraft, the time, and what the host has loaded for the view.
struct MapState {
    bool havePos = false;
    double lat = 0, lon = 0;
    float hdgTrue = 0, gsKt = 0;
    long long utc = 0;            // the sim's date and time, seconds since 1970 UTC; 0 unknown
    bool haveLocalOffset = false; // X-Plane's local time at the aircraft minus UTC
    double localOffsetS = 0;
    // a tile's texture, or 0 while it is not loaded (with request, the host fetches it); null in the off-line test
    ImTextureID (*tile)(int source, int z, int x, int y, bool request) = nullptr;
    std::vector<std::shared_ptr<const AptGeo>> airports;   // layouts around the view
    std::vector<AptPoint> airportPoints;                    // positions around the view
    bool aptIndexing = false;     // the airport index is still being built
    int aptCount = 0;
    float altFt = 0;              // the aircraft's altitude (MSL)
    // other aircraft (X-Plane's AI and multiplayer, as TCAS sees them)
    struct Traffic { double lat = 0, lon = 0; float altFt = 0, hdg = 0, gsKt = 0, vsFpm = 0; bool onGround = false; std::string callsign, type; };
    std::vector<Traffic> traffic;
    // radio navaids and fixes around the view (X-Plane's nav data: Custom Data first, else its own)
    struct Navaid { int type = 3; double lat = 0, lon = 0; float freq = 0, bearing = 0; std::string ident, name; };   // type: 2 NDB, 3 VOR, 4/5 localizer, 12/13 DME
    std::vector<Navaid> navaids;
    struct Fix { double lat = 0, lon = 0; std::string ident; bool enroute = true; };
    std::vector<Fix> fixes;
    // the SimBrief flight plan
    struct RoutePoint { double lat = 0, lon = 0; std::string ident; };
    std::vector<RoutePoint> route;
    std::string routeStatus;      // "KSBN to KORD, 12 fixes", or why there is none
};

// The Airports page: one airport, read from X-Plane's scenery (apt.dat) and OurAirports.
struct AirportInfo {
    std::string icao;
    bool busy = false;            // being read
    bool found = false;
    std::shared_ptr<const AptGeo> geo;   // runways, radio, stands, layout
    std::string name, city, country, region, iata;   // from OurAirports when it knows the airport
    struct Freq { std::string what; double mhz = 0; std::string note; };
    std::vector<Freq> freqs;      // every frequency, X-Plane's and OurAirports' merged
    Freq ctaf;                    // always set: published CTAF, else UNICOM, else the tower's (when closed), else 122.900 MULTICOM
    std::string metar;            // the latest report, when fetched
};

// The Documents page: what is in the folders, and the picture being looked at.
struct DocEntry { std::string name, path, folder; bool pdf = false; long long bytes = 0; };
struct DocsState {
    bool listed = false;
    std::vector<DocEntry> files;
    std::vector<std::string> folders;   // the folders looked in (to tell the user where to put files)
    std::string shownPath;        // the picture shown
    ImTextureID tex = 0;
    int texW = 0, texH = 0;
    bool busy = false;
    std::string error;
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
    BrowserState browser;
    MetarLookup metar;
    GroundState ground;
    MapState map;
    AirportInfo airport;
    std::vector<AptPoint> airportList;   // the search's results, or the nearest airports
    std::string airportStatus;    // why the list is empty ("Finding X-Plane's airports...")
    DocsState docs;
    std::string checklistText;    // the aircraft's own checklist file (see Checklists), "" for the built-in ones
    std::string checklistSource;  // where it came from
};

// What the user asked for on the Ground page; the host carries it out.
enum GroundAction { GA_NONE, GA_JETWAY, GA_GPU, GA_CHOCKS, GA_HP_AIR, GA_LP_AIR, GA_TRUCKS, GA_PUSH_LEFT, GA_PUSH_STRAIGHT, GA_PUSH_RIGHT, GA_PUSH_STOP,
                    GA_PAX_DOOR, GA_CARGO_DOOR, GA_DOORS_CLOSE, GA_DOORS_AUTO, GA_DEICE, GA_DEICE_QUICK,
                    GA_JETWAY_DOOR, GA_SLIDES_ARM, GA_SLIDES_DISARM, GA_CABIN_LIGHT };

struct Actions {
    int ground = GA_NONE;
    int groundIndex = 0;          // which door (GA_PAX_DOOR, GA_CARGO_DOOR, GA_JETWAY_DOOR); GA_CABIN_LIGHT: the level, 0-100 %
    std::string metarSearch;      // look this airport's METAR up (the Weather page's search)
    // the browser: shown this frame (start it if it is not running), where the page is (layout units) and what to send it
    struct WebEvent { int type; float x, y; int code; };   // type: an xptb::CmdType (mouse, wheel, char, key)
    bool webShown = false, webRetry = false;
    float webX = 0, webY = 0, webW = 0, webH = 0;
    std::vector<WebEvent> web;
    std::string webNavigate;
    int webNav = 0;               // 1 back, 2 forward, 3 reload, 4 stop
    bool webTabNew = false;       // open a tab (at webTabUrl, or blank) and show it
    std::string webTabUrl;
    int webTabSelect = 0, webTabClose = 0;   // a tab's id: show it, close it (0: none)
    // the Map page: shown this frame, its centre, and the airports to have ready around it
    bool mapShown = false, mapLayouts = false;
    // the Airports page: list airports matching this text ("" with airportsShown: the nearest), or read one in full
    bool airportsShown = false;
    std::string airportSearch;    // sent when it changes
    std::string airportOpen;      // the airport to show
    // the Documents page: list the folders (again), show a picture
    bool docsList = false;
    bool routeReload = false;     // fetch the SimBrief plan again
    std::string docOpen;
    double mapLat = 0, mapLon = 0, mapLayoutNm = 0, mapPointNm = 0;
    double mapNavNm = 0, mapFixNm = 0;   // navaids and fixes to have ready around the centre (0: none)
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

// The aircraft top views (the plugin's shapes/aircraft_shapes.txt, from AircraftShapesSVG): the file's text. Returns how many
// shapes were read; types without one are drawn from their measurements instead.
int loadShapes(const std::string& text);
bool hasShape(const std::string& icao);   // a drawing for this type (its own, or its family's)

int currentApp();
// The browser's address bar is taking typed keys (the floating window's keyboard goes there instead of to the page).
bool webTypingAddress();
// Something on the tablet is taking typed keys (the address bar, a note, a search): the floating window's keyboard goes there.
bool tabletTyping();
void tabletKey(int code, bool isChar);   // code: a character, or 8 (back) / 13 (enter)
// The Notes page's contents, kept between sessions by the host
std::string notesToText();
void notesFromText(const std::string& text);
bool takeNotesChanged();
// Open the Map page centred on a place (the Airports page's "Show on map")
void showOnMap(double lat, double lon);
// AviTab's map commands and datarefs: 0 zoom in, 1 zoom out, 2 recentre, 3 pan left, 4 right, 5 up, 6 down
void mapCommand(int what);
void mapView(double& lat, double& lon, int& zoom, float& verticalRangeNm);
// The Documents page, showing one folder ("This aircraft" for AviTab/app_plane_manual), or all ("")
void openDocuments(const std::string& folder);
void webAddressKey(int code, bool isChar);   // code: a character, or 8 (back) / 13 (enter)
void openApp(int app);   // APP_HOME: the home screen
const char* appName(int app);                // the icon's label (Charts and Live Map carry their provider's name)
bool appVisible(int app);                 // on the home screen (web app icons can be switched off)
int touchTestCount();                     // marks in the Settings touch test (for the tests)
const char* browserUrl();                 // the address the web page shows
bool browserChrome();                     // the full browser (address bar, bookmarks) rather than a web app's own page

} // namespace xpt
