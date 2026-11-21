// The tablet's user interface, drawn with Dear ImGui and nothing else: no X-Plane calls here, so the same code runs inside the
// plugin and in the off-line picture test. The host (src/plugin.cpp) gives it the sim's state and gets the user's requests back.
#pragma once

#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "aptgeo.h"
#include "imgui.h"

namespace xpt {

constexpr float kLogicalH = 480.0f;   // the tablet is always laid out 480 units tall; its width follows the screen's shape

// The tablet's apps. Charts, Live Map, SimBrief and ForeFlight are web apps: each opens its site as a page of its own.
// Each web site is an app of its own (APP_CHARTS is ChartFox, APP_LIVEMAP Volanta, APP_FOREFLIGHT ForeFlight); new apps go at the end,
// as the settings file keeps the home screen by these numbers.
enum App { APP_HOME = -1, APP_MAP, APP_AIRPORTS, APP_CHARTS, APP_DOCUMENTS, APP_ROUTES, APP_WEATHER, APP_GROUND, APP_CHECKLISTS,
           APP_PERFORMANCE, APP_TRAFFIC, APP_NOTES, APP_SIMBRIEF, APP_LIVEMAP, APP_FOREFLIGHT, APP_BROWSER, APP_SETTINGS,
           APP_NAVIGRAPH, APP_VATSIM, APP_SKYVECTOR, APP_MUSIC, APP_DISPATCH, APP_NAVSEARCH,
           // the App Store and what it offers (web apps, installed or not), and Games
           APP_APPSTORE, APP_GAMES, APP_DATIS, APP_SPOTIFY, APP_YTMUSIC, APP_APPLEMUSIC, APP_SOUNDCLOUD, APP_YOUTUBE, APP_VATSIMHUB, APP_NATTRAK, APP_IVAO,
           APP_TIMERS, APP_LOGBOOK, APP_ACARS, APP_XPILOT, APP_FR24,   // (App Store apps of XPTablet's own)
           APP_COUNT };

constexpr int kMaxFavorites = 7;   // the favorites bar holds up to 7 places (icons, or the 1 x 1 and 7 x 1 widgets); its size is chosen in Settings, 0 removes it

// What the user chose. Kept between sessions by the host (settingsToText / settingsFromText).
struct Settings {
    // the web apps on the home screen, each on or off: charts (ChartFox, Navigraph), live maps (Volanta, VATSIM), chart providers
    // (ForeFlight, SkyVector), flight planning (SimBrief)
    bool showCharts = true, showNavigraph = false, showLive = false, showVatsim = false, showForeflight = true, showSkyvector = false, showSimbrief = true;
    std::vector<int> order;       // the home screen's icons, in the order the user arranged them
    std::vector<std::string> installed = {"datis", "games"};   // the App Store's apps on the home screen (their ids)
    struct Folder { std::string name; std::vector<int> apps; };
    std::vector<Folder> folders;  // the home screen's folders (in order as kFolder0 + their index)
    int homePages = 1;            // the home screen's pages
    std::string volantaOverlay = "http://localhost:53011";   // Volanta's stream overlay (its widget)
    float brightness = 1.0f;      // the screen's brightness set in Settings, 0.1 to 1 (on top of what the aircraft sets)
    int favoriteSlots = 4;        // how many icons the favorites bar holds, 0 (no bar) to kMaxFavorites
    std::vector<int> favorites = {APP_MAP, APP_CHARTS, APP_WEATHER, APP_SIMBRIEF};   // the icons in the bar along the bottom (they leave the grid)
    std::vector<std::pair<std::string, std::string>> bookmarks;   // the browser's own bookmarks: title, address
    std::vector<std::pair<std::string, std::string>> history;     // the Browser's pages, newest last (not the web apps'): title, address
    float browserVolume = 1.0f;   // the Browser's videos and sounds, 0..1 (not the web apps')
    // the Map page
    int mapSource = 0;            // 0 OpenStreetMap, 1 satellite imagery
    bool mapDark = false;         // Display: the street map in night colours (light labels)
    bool mapDarkAuto = false;     // Display: the street map fades to its night colours with the daylight (as auto dim)
    bool mapAirports = true;      // X-Plane's airport layouts (apt.dat) over the map
    bool mapTimeZones = false;    // the time zone boundaries
    bool mapHeadingUp = false;    // Heading Lock (else North Lock)
    bool mapLocalTime = false;    // the time bubble shows local time (else Zulu)
    bool mapTraffic = true;       // AI and multiplayer aircraft
    bool mapOutlines = false;     // coastlines, borders, states and provinces
    bool mapNavaids = true;       // VORs, NDBs and DMEs
    int wallpaper = 0;            // Display: the theme (0 navy .. 4 amber, from the logo's colours; 5 Noir, 6 Cloud, 7-9 the Customs)
    struct CustomTheme { int bg = 0x2A3E5C, accent = 0x46A0FF; std::string picture; };   // colours 0xRRGGBB; a picture from plugins/XPTablet/backgrounds
    CustomTheme customs[3] = {{0x2A3E5C, 0x46A0FF, ""}, {0x2E5A3A, 0xF0A030, ""}, {0x4A2A52, 0xFF7AB0, ""}};   // the three Custom themes (7, 8, 9)
    int wallpaperBase = 0;        // the theme chosen before a seasonal one (it comes back when the season ends)
    std::string seasonalTold;     // the seasonal themes already announced ("index-year" items, space-separated)
    int accent = 0;               // Display: the colour of what is chosen (0 violet .. 4 gold), from the logo's letters
    bool localConnect = false;    // Local Connect: the tablet on a phone or tablet on the same network (off until switched on)
    bool localBattery = true;     // with a device connected that tells its battery: the tablet's battery is that device's
    int arcadeBest[6] = {0, 0, 0, 0, 0, 0};   // the best scores of Contrail, Taxi Rush, Sky Patrol, Hangar Climb, Cockpit 21 (most miles), Cargo Load
    bool physicalKeys = false;    // typing: the computer's own keyboard (a thin bar instead of the on-screen keys)
    bool mapRings = true;         // range rings round the aircraft (a quarter, half and all of the range), as on WXPlane's map
    bool mapIls = true;           // ILS and localizer courses (a layer of their own)
    bool mapWaypoints = false;    // fixes
    bool mapRoute = true;         // the SimBrief flight plan
    bool mapTrail = true;         // the trail flown since the plan was imported (the map's Features)
    bool mapStands = true;        // parking spaces (stands and gates) on the airport layouts
    bool mapBestRunway = true;    // the runway most into the wind outlined in green on the airport layouts
    bool mapDaylight = false;     // day and night across the map (the sun's height where it is, from the sim's time)
    bool cabinAuto = false;       // the cabin lights follow the daylight, as the tablet's auto dim: 100 % by day, down to 0 % at full night
    bool mapAirspaces = false;    // the Map shows VATSIM's airspaces (green where a controller is online)
    bool mapVatsimAtc = false;    // the Map shows VATSIM's controllers online round their airports
    bool vatsimAtcNotes = true;   // a notification when a controller comes online at the flight's airports
    int mapTrafficSource = 0;     // 0 nearby (X-Plane's TCAS: AI and multiplayer), 1 nearby and VATSIM worldwide
    bool mapTrafficTrails = false;   // each aircraft's trail behind it
    bool mapTrafficRoute = true;  // a VATSIM aircraft's filed route on the map while its card is open
    float mapTrafficRangeNm = 0;  // traffic shown within this distance of us (0: any)
    float mapTrafficAltFt = 0;    // and within this height of us (0: any)
    std::string gatesFor;         // the flight plan the gates are for (its SimBrief id)
    std::string startGate;        // the stand the flight left from (found when the plan came in on the ground; "N/A" in the air)
    std::string arrivalGate;      // the stand it is going to (typed; shown on the destination's layout)
    std::string simbriefUser;     // SimBrief user name or pilot ID (the Flight Plan page and the route on the map)
    std::string hoppieCode;       // the user's Hoppie ACARS logon code (Settings, Accounts): the ACARS app
    bool mapProcedures = true;    // the plan's SID, STAR and approach on the map
    bool autoDim = false;         // the screen dims itself as the daylight goes (WXPlane's curve); the brightness slider rests
    bool statusLocalTime = false; // the status bar's clock: local time where the aircraft is (else Zulu)
    bool mediaEnabled = true;     // the Music app and the music controls in the pull-down (not on macOS)
    int mediaDuck = 0;            // while the radios receive: 0 leave the music alone, 1 lower it (duckRx / duckTx), 2 pause it (and play it again after)
    float duckRx = 0.5f;          // lowered: the media's volume while receiving, 0..1
    float duckTx = 0.3f;          // and while transmitting
    float notifyVolume = 0.7f;    // the notification chime, 0 (silent) .. 1
    bool notifyEnabled = true;    // notifications: banners, and the list in the pull-down
    bool notifyTcas = true;       // a TCAS alert (traffic in resolution-advisory range)
    bool notifySimbrief = true;   // a new SimBrief flight plan (checked every 2 minutes with the SimBrief ID)
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
    ImTextureID logoMask = 0;     // the same logo in white (logoMaskPixels): drawn in the theme's colours
    ImTextureID icons = 0;        // the app icons' atlas (iconsPixels), uploaded by the host like the logo; 0: drawn icons instead
};

// XPTablet's logo (XP in white, TABLET in the gradient): w x h RGBA, followed by its halved copies down to 1 pixel high.
const unsigned char* logoPixels(int& w, int& h, int& levels);
// the logo, every pixel white with its own alpha (all its levels), and where TABLET starts across it (0..1)
const unsigned char* logoMaskPixels(int& w, int& h, int& levels, float& tabletFrom);
bool logoLetterBox(int n, float& u0, float& u1, float& v0, float& v1);   // TABLET's nth letter's box in the logo (fractions)
const unsigned char* iconsPixels(int& w, int& h, int& levels);   // the same layout as the logo's

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
    int jwReach = -1;   // openSAM: the jetways it gave the doors (1L first, then 2L: one only when a second would cross); -1 not known
    int jwDoorPax[3] = {0, 2, 4};
    bool jwDocked[3] = {false, false, false};
    int slides = -1;              // ToLiss: the cabin doors' slides, 1 armed, 0 disarmed, -1 unknown
    // what keeps a jetway from connecting (the Ground page's status)
    bool beacon = false, enginesRunning = false, parkBrake = false;
    // pushback: 1 BetterPushback is installed (its planner, tug and its state), 0 X-Plane's own (left, straight, right, stop)
    int pushSource = 0;
    bool bpStarted = false, bpConnected = false;
    bool landingLights = false, taxiLight = false;   // on: BetterPushback's tug will not push (it stops and calls out until they are off)   // BetterPushback: a pushback under way; its tug is connected
    float groundSpeedKt = 0;
    float cabinLight = -1;        // ToLiss: the cabin lights, 0..1, -1 unknown
    int cabinAuto = -1;           // ToLiss: the cabin lights' auto setting, 1 on, 0 off, -1 the aircraft has none (XPTablet's own Auto: Settings::cabinAuto)
    bool cabinReady = false;      // ToLiss: Cabin Ready was given (AirbusFBW/CheckCabin) for this phase; again after takeoff or landing
    bool jwMoving[3] = {false, false, false};   // a jetway is still moving to or from this door (not connected until it is there)
    bool slidesAuto = false;      // ToLiss: the slides follow the aircraft's own rule (armed once the beacon is on and the doors shut, disarmed parked)
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
    struct Tab { int id = 0; std::string title, url; bool loading = false; int group = 0; };   // group: 0 the Browser's, else a web app's
    std::vector<Tab> tabs;        // the open pages, in order
    int activeTab = 0;            // the shown one's id
    int zoomPct = 100;            // the shown page's zoom
    bool zoomAuto = true;         // it is the zoom fitting the page to the view (none chosen for the tab)
    bool fullscreen = false;      // the shown page is full screen (a video): it takes the whole tablet
};

// A pilot on VATSIM (the Map's traffic card): who, where, and the filed flight plan.
// What is playing on the computer (Windows: the system's media controls; Linux: MPRIS players), from the host
struct MediaState {
    bool supported = false;       // this computer's media controls can be used (Windows, Linux)
    bool active = false;          // a player is there
    std::string app, title, artist, album;
    bool playing = false, canPrev = true, canNext = true;
    ImTextureID art = 0;          // the cover, artW x artH pixels; 0: none
    int artW = 0, artH = 0;
    ImTextureID artPrev = 0;      // the cover before it (it fades out under the new one), artPrevW x artPrevH
    int artPrevW = 0, artPrevH = 0;
    double artChangedAt = -100;   // when the cover changed (the plugin's clock: ImGui's time)
    ImU32 artColor = 0;           // the cover's own colour (its average, made deeper): the Media page's background; 0 none
    bool radioRx = false;         // the radios are receiving (xPilot's COM1/COM2, VATSIM voice apps)
    double posSec = -1, lenSec = -1;   // the track's point and length, in seconds (-1: the player does not say)
    bool canSeek = false;         // the player can be moved to another point
    bool radioTx = false;         // push-to-talk is held (xPilot's xpilot/ptt): the music is lowered or paused for it too
    bool ducked = false;          // the music is lowered or paused for the radio now
    std::string radioSource;      // what the radio is heard through ("xPilot", "TrackAudio"...), "" none
    float playerVolume = -1;      // the player's own volume, 0..1 (-1: it keeps it to itself)
    float masterVolume = -1;      // the computer's (Windows), 0..1 (-1: not here)
};

struct VatsimPilot {
    bool found = false, hasPlan = false;
    std::string callsign, name, cid, server, rating, squawk, logon;
    int altitudeFt = 0, groundspeedKt = 0, heading = 0, qnhMb = 0;
    double lat = 0, lon = 0;
    std::string rules, aircraft, aircraftFull, departure, arrival, alternate, cruiseAlt, cruiseTas, depTime, enroute, fuel, route, remarks, assignedSquawk;
};

// A VATSIM controller (or ATIS) online: its call sign ("KORD_TWR"), frequency ("120.750"), kind (facility: 1 FSS, 2 DEL, 3 GND,
// 4 TWR, 5 APP, 6 CTR; 7 the ATIS) and its text
struct VatsimController { std::string callsign, freq, name, text; int facility = 0; std::string logon; };   // logon: "14:02Z"

// VATSIM's airspaces (the VATSpy data): each area's outline (rings of lat, lon), its box and label point; and the FIRs, which say
// which area a center's call sign covers
struct AirspaceArea { std::string id; bool oceanic = false; double labelLat = 0, labelLon = 0, la0 = 0, lo0 = 0, la1 = 0, lo1 = 0;
                      std::vector<std::vector<std::pair<float, float>>> rings; };
struct AirspaceSet { std::vector<AirspaceArea> areas; struct Fir { std::string icao, name, prefix, boundary; }; std::vector<Fir> firs; };

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
    float vsFpm = 0, aglFt = 0;   // its vertical speed, its height above the ground
    bool onGround = false;
    // other aircraft (X-Plane's AI and multiplayer, as TCAS sees them)
    struct Traffic { double lat = 0, lon = 0; float altFt = 0, hdg = 0, gsKt = 0, vsFpm = 0; bool onGround = false; std::string callsign, type; };
    std::vector<Traffic> traffic;
    // radio navaids and fixes around the view (X-Plane's nav data: Custom Data first, else its own)
    struct Navaid { int type = 3; double lat = 0, lon = 0; float freq = 0, bearing = 0; std::string ident, name;
                    float magCourse = -1, gsAngle = 0; std::string airport, runway; bool dme = false; };   // type: 2 NDB, 3 VOR, 4/5 localizer, 12/13 DME
                    // (localizers: magCourse, the glide slope's angle (0: none), their airport and runway, a DME with them)
    std::vector<Navaid> navaids;
    struct Fix { double lat = 0, lon = 0; std::string ident; bool enroute = true; };
    std::vector<Fix> fixes;
    // the SimBrief flight plan
    struct RoutePoint { double lat = 0, lon = 0; std::string ident; };
    std::vector<RoutePoint> route;
    struct Procedure { std::string name; int kind = 0; std::vector<RoutePoint> pts; };   // kind 0 SID, 1 STAR, 2 approach
    std::vector<Procedure> procedures;   // the plan's departure, arrival and approach, from X-Plane's CIFP (drawn on the map)
    std::vector<RoutePoint> trafficRoute;   // the selected VATSIM aircraft's filed route (its fixes found in X-Plane's nav data)
    std::string routeStatus;      // "KSBN to KORD, 12 fixes", or why there is none
    // the traffic card's aircraft on VATSIM (for Actions::trafficSelected)
    VatsimPilot vatsim;
    std::string vatsimStatus;     // "" when the data is fresh; else what is happening
    std::shared_ptr<const AirspaceSet> airspaces;   // VATSIM's airspaces, once read (the Map's airspace layer)
    std::string airspaceStatus;   // how they stand ("Reading VATSIM's airspaces..."), "" when there
};

// The Flight Plan page: the latest SimBrief plan (the operational flight plan's summary and its navigation log).
struct FlightPlan {
    bool loaded = false, busy = false;
    std::string status;           // what happened ("Imported 18:42Z", "No plan for that name"...)
    std::string callsign, aircraft, registration, units;   // units: lbs or kgs
    std::string origin, originName, originRunway, dest, destName, destRunway, alternate, alternateName;
    std::string route, cruise, distanceNm, ete, costIndex, avgWind, sid, star;
    std::string blockFuel, tripFuel, reserveFuel, altnFuel, taxiFuel, extraFuel, zfw, tow, ldw;
    std::string oew, paxCount, paxWeight, bags, bagWeight, freight, payload, maxZfw, maxTow, maxLdw;   // the load, from SimBrief's weights
    std::string pdfUrl;           // the briefing (the OFP as SimBrief's PDF)
    std::string importedAt;       // when (Zulu)
    std::string ofpId;            // SimBrief's request id and generation time: a new plan has a new one
    struct Leg { std::string ident, airway, alt, legNm, totalTime, type; double lat = 0, lon = 0;
                 std::string cross;   // the SID's or STAR's crossing restriction there ("at or above FL220", "... 280 kt"), from X-Plane's CIFP
    };
    std::vector<Leg> legs;
    std::string divertKey;        // a diversion: the plan it replaced (its breadcrumbs go on), "" when none
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
    bool light = false;           // the picture is mostly light (paper): the white pen draws black on it
    bool busy = false;
    std::string error;
};

// Local Connect: the tablet in a phone's or tablet's browser on the home network (the plugin serves it)
struct LocalDevice {
    std::string name;             // as the device calls itself ("Dom's iPhone"), or its kind
    std::string kind;             // "iPhone", "iPad", "Android phone", "Android tablet", "Computer"
    bool connected = false;       // open now (paired devices that are not open are listed too)
    int batteryPct = -1;          // its battery, -1 not told (iPhone and iPad browsers do not share it)
    bool charging = false;
    std::string lastSeen;         // when it was last open ("Today 14:32"), for one not connected
};
struct LocalConnectState {
    int status = 0;               // 0 off, 1 starting, 2 on (waiting or connected), -1 cannot start (error), -2 not in this build yet
    std::string address;          // what to open on the device: https://192.168.1.20:8443
    std::string error;            // why it could not start
    std::string pairCode;         // a device asking to pair: the code it must enter ("482913"), empty none
    std::string pairName;         // and its name
    std::vector<LocalDevice> devices;
};
// Hoppie's ACARS: a message up (from a station) or down (sent); CPDLC's numbers and the reply it wants
struct AcarsMsg {
    bool up = true;
    std::string station, type, text;   // type: telex, cpdlc, ...
    long long utc = 0;
    int min = 0, mrn = 0; std::string ra;   // CPDLC: its number, the one it answers, the reply it wants (WU, AN, R, Y, N, NE)
    std::string answer;                // the reply given (WILCO...), or "" none yet
};
struct AcarsState {
    std::vector<AcarsMsg> inbox;       // what came up, in order (only added to)
    std::string status;                // "Polling every minute", an error from Hoppie
    long long lastPoll = 0;            // sim UTC of the last poll
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
    LocalConnectState local;      // Local Connect (Settings)
    AcarsState acars;             // Hoppie's ACARS (the ACARS app)
    int xpilotLogin = -1;         // xPilot's connection (its xpilot/login/status): 0 not connected, 1 a pilot, 2 an observer; -1 not known
    std::string xpilotCallsign, xpilotCom1, xpilotCom2;   // its callsign on the network, the stations tuned on COM1 and COM2
    int xpilotAircraft = 0;       // the aircraft it is drawing
    std::vector<std::string> xpilotCmds;   // xPilot's own commands found in the sim (its message window, nearby ATC...): buttons for them
    float fuelKg = -1;            // the fuel on board, kg (-1 unknown): the logbook's fuel used
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
    FlightPlan plan;
    FlightPlan planOriginal;      // the plan a diversion replaced (Undo Divert brings it back); loaded false when none
    float autoDimLevel = 1.0f;    // what auto dim makes of the brightness now (from the sun)
    std::vector<std::pair<std::string, std::string>> dataInfo;   // Settings, Data: where the tablet's data comes from, and how it is
    bool mac = false;             // macOS: no media controls (no Music app, no Media settings)
    MediaState media;
    float fps = 0;                // X-Plane's frame rate
    std::vector<std::string> backgroundFiles;   // the pictures in plugins/XPTablet/backgrounds (for a Custom theme)
    std::string backgroundsFolder;              // that folder (to tell the user)
    ImTextureID bgTex = 0;        // the Custom theme's picture, loaded (bgPicture: which), 0 none
    int bgW = 0, bgH = 0;
    std::string bgPicture;
    int bgAverage = -1;           // its average colour (0xRRGGBB, made deeper): the theme's tone with it, -1 not known
    int bgAccent = -1;            // its strongest colour, made bright enough to read (0xRRGGBB): the theme's accent with it, -1 not known
    struct NavHit { int type = 0; std::string ident, name; double lat = 0, lon = 0; float freq = 0; };   // type: XPLMNavType
    std::vector<NavHit> navHits;  // what the Navigation page's search found
    std::vector<VatsimController> vatsimAtc;   // VATSIM's controllers and ATIS online (when fetched; empty otherwise)
    bool vatsimAtcHave = false;   // the list is from a fetch (else it is not known)
    int engineType = -1;          // the first engine's kind (X-Plane's acf_en_type: 0/1 piston, 2/8 turboprop, 3 electric, 4/5 jet), -1 unknown
    FlightPlan planPreview;       // a plan fetched from SimBrief, waiting for Import or Dismiss (loaded: the window shows it)
    bool planPreviewOpen = false; // its window is up (Dismiss closes it; the plan stays, to review again)
    std::string planDismissed;    // the last plan dismissed (its ofpId): the 2-minute check does not offer it again
    int newPlanSeq = 0;           // goes up when the 2-minute check finds a new SimBrief plan (then newPlanText says which)
    std::string newPlanText;
};

// What the user asked for on the Ground page; the host carries it out.
enum GroundAction { GA_NONE, GA_JETWAY, GA_GPU, GA_CHOCKS, GA_HP_AIR, GA_LP_AIR, GA_TRUCKS, GA_PUSH_LEFT, GA_PUSH_STRAIGHT, GA_PUSH_RIGHT, GA_PUSH_STOP,
                    GA_PAX_DOOR, GA_CARGO_DOOR, GA_DOORS_CLOSE, GA_DOORS_AUTO, GA_DEICE, GA_DEICE_QUICK,
                    GA_JETWAY_DOOR, GA_SLIDES_ARM, GA_SLIDES_DISARM, GA_CABIN_LIGHT, GA_SLIDES_AUTO, GA_CABIN_READY, GA_CABIN_AUTO, GA_OPENSAM_UI, GA_JETWAY_SET,
                    GA_BP_PLAN, GA_BP_CONNECT, GA_BP_START, GA_BP_STOP, GA_BP_DISCONNECT, GA_BP_RECONNECT };   // GA_BP_*: BetterPushback's planner, tug, start and stop   // GA_JETWAY_SET: groundIndex a mask of the doors to connect

struct Actions {
    std::string xpilotCmd;           // one of xPilot's commands, pressed once (from SimState::xpilotCmds)
    struct AcarsOut { std::string to, type, packet; };
    std::vector<AcarsOut> acarsOut;   // ACARS messages to send now (from acarsFrom, with the logon code in Settings, Accounts)
    std::string acarsFrom;           // the callsign ACARS uses; empty: no polling (no logon code, no callsign, or the app not installed)
    std::string localForget;      // Local Connect: a paired device removed (its name)
    bool localPairDeny = false;   // Local Connect: the device asking to pair is turned away
    int ground = GA_NONE;
    int groundIndex = 0;          // which door (GA_PAX_DOOR, GA_CARGO_DOOR, GA_JETWAY_DOOR); GA_CABIN_LIGHT: the level, 0-100 %
    std::string metarSearch;      // look this airport's METAR up (the Weather page's search)
    // the browser: shown this frame (start it if it is not running), where the page is (layout units) and what to send it
    struct WebEvent { int type; float x, y; int code; int code2 = 0; int ctrl = 0; };   // type: an xptb::CmdType (mouse, wheel, char, key); a wheel's code2: across, ctrl: Ctrl held (a PDF zooms)
    bool webShown = false, webRetry = false;
    float webX = 0, webY = 0, webW = 0, webH = 0;
    std::vector<WebEvent> web;
    std::string webNavigate;
    int webNav = 0;               // 1 back, 2 forward, 3 reload, 4 stop
    bool webTabNew = false;       // open a tab (at webTabUrl, or blank) in group webTabGroup, and show it
    int webTabGroup = 0;
    int webZoom = 0;              // the shown page: +1 closer, -1 farther, 2 back to 100 %, 3 to webZoomPct
    int webZoomPct = 100;
    int webVolume = -1;           // the Browser's media volume, 0..100, when changed (-1 none)
    float mediaPlayerVol = -1;    // the player's own volume set (0..1), -1 none
    float mediaMasterVol = -1;    // the computer's volume set (0..1), -1 none
    bool webExitFullscreen = false;
    std::string webTabUrl;
    int webTabSelect = 0, webTabClose = 0;   // a tab's id: show it, close it (0: none)
    // the Map page: shown this frame, its centre, and the airports to have ready around it
    bool mapShown = false, mapLayouts = false;
    float mapPointMinFt = 0;      // the map's airport dots: only airports with a runway at least this long (the bigger ones further out)
    // the Airports page: list airports matching this text ("" with airportsShown: the nearest), or read one in full
    bool airportsShown = false;
    bool backgroundsList = false; // list the backgrounds folder again
    std::string backgroundWanted; // the picture the theme shown wants ("" none)
    double com1Standby = 0;       // tune COM1's standby to this (MHz), 0 none
    std::string airportSearch;    // sent when it changes
    std::string airportOpen;      // the airport to show
    std::string navSearch;        // the Navigation page's search (sent each frame while it is shown; the host looks when it changes)
    // the Documents page: list the folders (again), show a picture
    bool docsList = false;
    std::string trafficSelected;  // the Map's traffic card is open on this call sign (look it up on VATSIM)
    bool planImport = false;      // fetch the SimBrief plan (with the saved name / ID) into the preview
    bool planAccept = false;      // the preview's Import: it becomes the flight plan
    bool planDismiss = false;
    bool planReview = false;      // open the window again on the plan dismissed     // the preview's Dismiss: it goes, the flight plan stays as it was
    bool planClear = false;       // forget it
    bool planDivert = false;      // a diversion: SimBrief's latest plan, taken from where the aircraft is (Undo goes back)
    bool planUndoDivert = false;
    bool clearTileCache = false;  // delete the map tiles kept on disk
    bool resetBrowserData = false;   // the browser's profile (logins, cookies, history, cache): stopped, deleted, started afresh
    std::string docOpen;
    int media = 0;                // the media player: 1 play / pause, 2 previous, 3 next
    double mediaSeek = -1;        // move the track to this point (seconds); -1: no
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
bool hasShape(const std::string& icao);
void setBattery(float pct);
// The bundled checklists' lines for one aircraft: a line "[TAGS] item | value" is kept when a tag is its ICAO type or its engine
std::string checklistFor(const std::string& text, const std::string& icao, const std::string& engine);   // the tablet's battery level (the emulators' test controls)   // a drawing for this type (its own, or its family's)

int currentApp();
// A touch screen's pointer (no hover before a press): queued, and fed to Dear ImGui a step a frame (call pumpPointer before NewFrame)
void queuePointer(float x, float y, int down);   // down: -1 a move, 1 pressed, 0 released
void pumpPointer();
void setTouchInput(bool on);   // the pointer is a finger (the map's cards then open on a double tap)
// The browser's address bar is taking typed keys (the floating window's keyboard goes there instead of to the page).
bool webTypingAddress();
bool webTyping();
int chimeSeq();               // counted up for each notification chime: the host plays it (at settings().notifyVolume) when it changes            // a web page's text field has the keyboard (its keys go to the page)
// Something on the tablet is taking typed keys (the address bar, a note, a search): the floating window's keyboard goes there.
bool tabletTyping();
bool gameWantsKeys();                    // 2048's board was pressed: W A S D and the arrows play it
void gameKey(int dir);                   // a move for it: 0 left, 1 right, 2 up, 3 down
void gameKeyEvent(int key, bool down);  // the arcade games: 0 left, 1 right, 2 up, 3 down, 4 the action (Space), pressed or let go
void tabletKey(int code, bool isChar);   // code: a character, or 8 (back) / 13 (enter)
// The Notes page's contents, kept between sessions by the host
std::string logbookToText();   // the logbook's flights, one a line (saved by the host: Output/XPTablet/logbook.txt)
void logbookFromText(const std::string& text);
bool takeLogbookChanged();
void setSeasonalPreview(bool all);
int settingsTabShown();   // the Settings page's tab (for the tests)
void celebrate(int which);   // the midnight celebration now: 0 New Year, 1 Christmas (it starts by itself at midnight; this is for a preview)
bool celebrationShown();     // it is up (it stays until its Dismiss button is pressed)
unsigned settingsReveal();   // which of Settings' hidden fields are shown now (a bit each: SimBrief, Hoppie, Volanta, Local Connect's address)

// ---- Local Connect, on the paired device (src/localclient) ----
void setRemoteClient(bool on);   // this tablet is a device's copy of the computer's: it keeps no logbook of its own
void setMapCredit(const std::string& credit);   // the street map's credit line (the default: OpenStreetMap's)
// The screen before the tablet is live. phase: 0 connecting, 1 enter the pairing code (typed into code with its own keys), 2 turned
// away (message says why). Returns 1 when the sixth digit was entered this frame, 2 when Try Again was pressed, else 0.
int localClientScreen(const Fonts& f, float w, float h, int phase, const std::string& message, std::string& code);
const char* currentWebUrl();     // the page shown is a web app: its address ("" otherwise) and its name
const char* currentWebName();
bool seasonalOnDate(int theme, int year, int month, int day);   // seasonal theme n (0 New Year .. 15 Breast Cancer Awareness) on that date
const char* seasonalName(int theme);   // every seasonal colour theme offered whatever the date (the browser emulator)
// A Custom background's colours, from its pixels (RGBA): its average made deeper (the background), and its most striking colour made
// bright and clear enough for buttons and switches (the accent). Shared by the plugin and the emulator.
void pictureColors(const unsigned char* rgba, int w, int h, int& average, int& accent);
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
// the home screen's places hold an app, a folder (kFolder0 + its index) or a widget (kWidget0 + its kind), or -1 (free)
constexpr int kFolder0 = 1000, kWidget0 = 2000;
enum WidgetKind { W_WEATHER, W_FLIGHT, W_MEDIA, W_FPS, W_VOLANTA, W_COUNT };
bool appInstalled(int app);
void layoutHome();
void pushInWidget(int widget, int start, int cap);   // a widget at the start of a page, what was there moved on after it (the + menu's Add Widget)                       // the home screen's places put in order (as it is drawn)               // an App Store app is on the home screen only once installed
bool appVisible(int app);                 // on the home screen (web app icons can be switched off)
std::vector<std::pair<int, int>> wrapLines(struct ImFont* font, float px, const std::string& v, float wrapW);   // a typed text's lines (the tests)
void mapSelect(int kind, const std::string& key);
void openDispatchFromPlan();
bool tabletPoweredOn();   // the tablet is on (not switched off, not flat): only then does it lower or pause media, or make sounds   // the Dispatch page, as the Flight Plan's Briefing PDF opens it (for the tests)   // opens the map's card for an item (for the tests)
int touchTestCount();                     // marks in the Settings touch test (for the tests)
const char* browserUrl();                 // the address the web page shows
bool browserChrome();                     // the full browser (address bar, bookmarks) rather than a web app's own page

bool trafficInRA(const MapState& m, const MapState::Traffic& t);
int trafficThreat(const MapState& m, const MapState::Traffic& t);   // 0 none, 1 TCAS TA, 2 TCAS RA, 3 runway collision (the tests)
int powerState();
void notify(const std::string& title, const std::string& body, int app);   // a notification (a banner, then the pull-down's list); tapping opens app
int notificationCount();
bool pullDownOpen();   // 0 on, 1 fading off, 2 off, 3 starting, 4 waking   // TCAS RA range (or about to collide): drawn pulsing red

} // namespace xpt
