// XPTablet emulator: the real plugin (plugin.cpp and its parts, built in) running against a stand-in for X-Plane: its datarefs,
// commands, windows, menus, flight loops and textures (xplm_emu.cpp), driven by a simulated aircraft (emu_sim.cpp), shown in a desktop
// window with a control panel (emu_panel.cpp, main_glfw.cpp) or run from a script without a screen (main_headless.cpp).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace emu {

// ---- datarefs ----------------------------------------------------------------------------------------------------------------------------
struct Dataref {
    std::string name;
    int types = 0;                  // xplmType_* bits it offers
    bool writable = true;
    bool fromPlugin = false;        // registered by the plugin (its accessors answer)
    bool touched = false;           // the plugin looked it up
    double value = 0;               // a plain number
    std::vector<float> vf;          // float arrays
    std::vector<int> vi;            // int arrays
    std::string bytes;              // byte arrays (strings)
    // the plugin's accessors, for its own datarefs
    void* geti = nullptr; void* seti = nullptr; void* getf = nullptr; void* setf = nullptr; void* getd = nullptr; void* setd = nullptr;
    void* getvi = nullptr; void* setvi = nullptr; void* getvf = nullptr; void* setvf = nullptr; void* getb = nullptr; void* setb = nullptr;
    void* readRefcon = nullptr; void* writeRefcon = nullptr;
};
// Finds (or makes) a dataref the emulated X-Plane has. Only names X-Plane or the loaded aircraft would have exist: sim/... always,
// others when the aircraft profile or a plugin provides them (see exists()).
Dataref* dataref(const std::string& name, bool create = true);
bool exists(const std::string& name);
void setNum(const std::string& name, double v);
double getNum(const std::string& name);
void setFloats(const std::string& name, const std::vector<float>& v);
void setInts(const std::string& name, const std::vector<int>& v);
void setBytes(const std::string& name, const std::string& b);
std::vector<Dataref*> allDatarefs();          // sorted by name
const std::vector<std::string>& missingLookups();   // names the plugin looked for that this X-Plane does not have
void setExtraPrefixes(const std::vector<std::string>& prefixes);   // the aircraft's own datarefs (AirbusFBW/, toliss_airbus/, ...)

// ---- commands ------------------------------------------------------------------------------------------------------------------------
struct Command { std::string name, description; bool fromPlugin = false; int sentByPlugin = 0; };
std::vector<Command> commands();
bool runCommand(const std::string& name);    // as if a key or button sent it once (begin, then end)
const std::vector<std::string>& commandLog(); // commands the plugin sent (sim/ground_ops/jetway, ...)

// ---- menus -----------------------------------------------------------------------------------------------------------------------------
struct MenuItem { std::string path; int menu = 0, index = 0; };
std::vector<MenuItem> menuItems();            // every item the plugin added, with its full path ("XPTablet > Show / hide tablet window")
void pickMenuItem(const MenuItem& m);

// ---- plugins (the fake ones that may be installed beside XPTablet) ------------------------------------------------------------------
void setFakePlugin(const std::string& signature, const std::string& name, const std::string& path, bool present);

// ---- the screen, windows and input --------------------------------------------------------------------------------------------------
// The emulated X-Plane screen is (0, 0)-(w, h) in boxels, y up. scale: pixels per boxel (for HiDPI).
void setScreen(int w, int h, float pixelsPerBoxel);
enum class View { Window, Cockpit };
void setView(View v);
View view();
// The cockpit view shows the aircraft's panel around the tablet's area (from AviTab.json); these say where.
bool cockpitArea(float& l, float& b, float& w, float& h);   // the panel rectangle shown (panel pixels)
// Pointer and keys, in screen boxels (y up). button: 0 left, 1 right.
void mouseMove(float x, float y);
void mouseButton(int button, bool down, float x, float y);
void mouseWheel(float x, float y, int clicks);
void keyChar(unsigned int c);                 // a typed character
void keySpecial(int vk, bool down, int flags);// XPLM_VK_* (backspace, arrows, return, ...)
bool tabletWindowRect(int& l, int& t, int& r, int& b);   // the plugin's floating window, if it is shown
void setTabletWindowRect(int l, int t, int r, int b);
// The tablet's layout point (lx, ly) on screen, for scripts: true when the tablet is on screen.
bool tabletPoint(float lx, float ly, float& sx, float& sy);

// ---- running ----------------------------------------------------------------------------------------------------------------------------
struct Paths {
    std::string xplaneRoot;    // the X-Plane folder the plugin sees (a real X-Plane 12 for its airports and navdata, or the emulator's own)
    std::string emuData;       // the emulator's own files: preferences, fake aircraft folders
    std::string pluginDir;     // where XPTablet's files are (shapes/, the browser helper): .../plugins/XPTablet
};
bool start(const Paths& p);          // starts and enables the plugin, then opens its window
void stop();
void restartPlugin();                // disable, stop, start, enable (tests a reload)
// One frame: the flight loops (and the simulated aircraft), then everything drawn into the current GL viewport (the screen area).
void frame(double dt);
void draw();                         // the X-Plane screen: its backdrop, then the plugin's windows (or the cockpit panel)
double simTime();
void log(const char* fmt, ...);
const std::vector<std::string>& logLines();
const Paths& paths();

// ---- the simulated aircraft and world (emu_sim.cpp) ------------------------------------------------------------------------------------
struct Profile { std::string name, icao, folder, acfFile; bool toliss = false; std::vector<std::string> prefixes; };
const std::vector<Profile>& profiles();
struct Sim {
    int profile = 0;
    std::string icaoOverride;
    double lat = 41.7087, lon = -86.3173;   // KSBN
    float hdg = 270, altFt = 799, aglFt = 0, gsKt = 0, vsFpm = 0;
    bool onGround = true, flying = false, engines = false, beacon = false, parkBrake = true, chocks = false, battery = true;
    bool cockpitTablet = false;       // the aircraft has an AviTab.json (the tablet on the cockpit panel)
    bool wxPlane = false, aviTab = false, openSam = false;
    float zuluHour = -1;              // < 0: now
    float sunPitch = 30;
    bool autoSun = true;
    // weather at the aircraft
    float windDir = 240, windKt = 12, visSm = 10, tempC = 15, dewC = 8, qnhHpa = 1013, rain = 0;
    int clouds = 1; float cloudBaseFt = 4000, cloudTopFt = 6000, cloudCover = 0.4f;
    // traffic
    int trafficCount = 6;
    float trafficRangeNm = 12;
};
Sim& sim();
void simApplyProfile();               // the aircraft changed: its folder, its datarefs, a PLANE_LOADED message
void simStep(double dt);              // moves things and writes the datarefs
void simAddIntruder();                // one aircraft head-on, 0.6 nm out, 200 ft above: TCAS RA range
void simResetTraffic();
struct TrafficAc { std::string callsign, type; double lat, lon; float altFt, hdg, gsKt, vsFpm; bool onGround; };
std::vector<TrafficAc>& traffic();

// ---- the control panel (emu_panel.cpp): drawn with its own ImGui context into the current viewport ------------------------------
void panelInit(float pixelScale);
void panelFrame(float w, float h, float pixelScale, double dt);   // builds and draws it
void panelMouse(float x, float y, int button, bool down, bool moved, float wheel);
void panelChar(unsigned int c);
void panelKey(int imguiKey, bool down);
bool panelWantsKeys();
extern float g_panelFbX;   // the panel's viewport's left edge in the framebuffer (pixels)
// requests from the panel to the window (main_glfw.cpp acts on them)
struct PanelRequests { bool screenshot = false; bool quit = false; };
PanelRequests& panelRequests();

// scripts (emu_script.cpp)
bool scriptLoad(const std::string& file);
bool scriptStep(double now, const std::function<bool(const std::string&)>& shot);   // false when it is over
int scriptFailures();
// the paths from the command line (--xplane, --data, --plugin) and emulator.ini, with the defaults beside the program
Paths pathsFromArgs(int argc, char** argv);
std::string argValue(int argc, char** argv, const char* name, const std::string& fallback);

// writes a PNG of the given RGBA pixels (bottom-up rows as GL reads them)
bool writePng(const std::string& path, const unsigned char* rgba, int w, int h);

} // namespace emu
