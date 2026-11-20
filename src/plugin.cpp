// XPTablet - the plugin. Puts the tablet (src/tabletui.cpp) in two places:
//   * on the aircraft's own cockpit tablet, in the area of the 3-D panel texture the aircraft reserved for AviTab. XPTablet speaks
//     the interface aircraft already use for that: the AviTab.json file in the aircraft folder, the avitab/panel_* datarefs and the
//     AviTab/* commands. So it appears where AviTab would, with no change to the aircraft.
//   * in a floating window (also a VR window), for aircraft without a tablet.
// If AviTab itself is installed and running, XPTablet leaves that interface to it and only offers the window.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <sstream>
#include <unordered_set>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "XPLMSound.h"
#include "XPLMNavigation.h"

#include "aptdat.h"
#include "navdata.h"
#include "vatsim.h"
#include "simbrief.h"
#include "media.h"
#include "ourairports.h"
#include "stb_image.h"
#include "browser.h"
#include "tiles.h"

#include "imgui.h"
#include "json_mini.h"
#include "http.h"
#include "metarfetch.h"
#include "tabletui.h"
#include "tabrender.h"
#include "version.h"
#include "wxplane_embed.h"

#if defined(_WIN32)
#include <windows.h>
#define XPT_GL __stdcall
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define XPT_GL
#endif

namespace {

void logMsg(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    XPLMDebugString((std::string("[XPTablet] ") + buf + "\n").c_str());
}

// ---- OpenGL 1.x, looked up at run time from the library X-Plane has loaded (no GL headers or libraries needed to build) --------------
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
const GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
             GL_LINEAR = 0x2601, GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078, GL_FLOAT = 0x1406,
             GL_TRIANGLES = 0x0004, GL_MODELVIEW = 0x1700, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_UNPACK_ALIGNMENT = 0x0CF5,
             GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_CULL_FACE = 0x0B44, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F;
const unsigned int GL_CLIENT_VERTEX_ARRAY_BIT = 0x00000002;

struct GL {
    void(XPT_GL* Enable)(GLenum) = nullptr;
    void(XPT_GL* Disable)(GLenum) = nullptr;
    GLboolean(XPT_GL* IsEnabled)(GLenum) = nullptr;
    void(XPT_GL* BlendFunc)(GLenum, GLenum) = nullptr;
    void(XPT_GL* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void(XPT_GL* TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(XPT_GL* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;   // the browser's pictures (optional)
    void(XPT_GL* PixelStorei)(GLenum, GLint) = nullptr;
    void(XPT_GL* EnableClientState)(GLenum) = nullptr;
    void(XPT_GL* VertexPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(XPT_GL* TexCoordPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(XPT_GL* ColorPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(XPT_GL* DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void(XPT_GL* MatrixMode)(GLenum) = nullptr;
    void(XPT_GL* PushMatrix)(void) = nullptr;
    void(XPT_GL* PopMatrix)(void) = nullptr;
    void(XPT_GL* Translatef)(float, float, float) = nullptr;
    void(XPT_GL* Scalef)(float, float, float) = nullptr;
    void(XPT_GL* PushClientAttrib)(unsigned int) = nullptr;
    void(XPT_GL* PopClientAttrib)(void) = nullptr;
    void(XPT_GL* Color4f)(float, float, float, float) = nullptr;
    int state = 0;
} gl;

void* glSym(const char* n) {
#if defined(_WIN32)
    HMODULE m = GetModuleHandleA("opengl32.dll");
    if (!m) m = LoadLibraryA("opengl32.dll");
    return m ? (void*)GetProcAddress(m, n) : nullptr;
#elif defined(__APPLE__)
    static void* h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    void* p = dlsym(RTLD_DEFAULT, n);
    return p ? p : (h ? dlsym(h, n) : nullptr);
#else
    void* p = dlsym(RTLD_DEFAULT, n);
    if (p) return p;
    static void* h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    return h ? dlsym(h, n) : nullptr;
#endif
}
template <class F> bool glGet(F& f, const char* n) { f = (F)glSym(n); return f != nullptr; }

bool loadGl() {
    if (gl.state == 0) {
        bool ok = true;
        ok &= glGet(gl.Enable, "glEnable"); ok &= glGet(gl.Disable, "glDisable"); ok &= glGet(gl.IsEnabled, "glIsEnabled");
        ok &= glGet(gl.BlendFunc, "glBlendFunc"); ok &= glGet(gl.TexImage2D, "glTexImage2D"); ok &= glGet(gl.TexParameteri, "glTexParameteri");
        ok &= glGet(gl.PixelStorei, "glPixelStorei"); ok &= glGet(gl.EnableClientState, "glEnableClientState");
        ok &= glGet(gl.VertexPointer, "glVertexPointer"); ok &= glGet(gl.TexCoordPointer, "glTexCoordPointer"); ok &= glGet(gl.ColorPointer, "glColorPointer");
        ok &= glGet(gl.DrawArrays, "glDrawArrays"); ok &= glGet(gl.MatrixMode, "glMatrixMode"); ok &= glGet(gl.PushMatrix, "glPushMatrix");
        ok &= glGet(gl.PopMatrix, "glPopMatrix"); ok &= glGet(gl.Translatef, "glTranslatef"); ok &= glGet(gl.Scalef, "glScalef");
        ok &= glGet(gl.PushClientAttrib, "glPushClientAttrib"); ok &= glGet(gl.PopClientAttrib, "glPopClientAttrib"); ok &= glGet(gl.Color4f, "glColor4f");
        glGet(gl.TexSubImage2D, "glTexSubImage2D");
        gl.state = ok ? 1 : -1;
        if (!ok) logMsg("OpenGL functions not found, the tablet cannot be drawn");
    }
    return gl.state == 1;
}

// ---- state ---------------------------------------------------------------------------------------------------------------------
const char* kAviTabSignature = "org.solhost.folko.avitab";
const char* kWxPlaneSignature = WXPLANE_SIGNATURE;

// The cockpit tablet's area on the panel texture, and its switches. These are what the avitab/panel_* datarefs read and write.
int g_panelLeft = 0, g_panelBottom = 0, g_panelWidth = 0, g_panelHeight = 0;
int g_panelEnabled = 0, g_panelPowered = 0;
float g_brightness = 1.0f;
bool g_captureClicks = true;   // AviTab.json's panel/disable_capture_window turns this off (the aircraft sends AviTab/click_left itself)
bool g_ownsAviTab = false;     // we registered the avitab/* datarefs and AviTab/* commands (AviTab itself is not running)
bool g_aviTabRunning = false;

ImGuiContext* g_ctx = nullptr;
xpt::Fonts g_fonts;
int g_fontTex = 0;
float g_fontScale = 0;
std::vector<xpt::Batch> g_batches;   // the current frame, clipped, in layout units
int g_frameCycle = -1;
float g_logW = 800.0f;               // the layout's width this frame (its height is always xpt::kLogicalH)
std::chrono::steady_clock::time_point g_lastFrame{};
xpt::SimState g_sim;

XPLMWindowID g_win = nullptr;        // the floating window
XPLMWindowID g_capture = nullptr;    // invisible, over the whole screen: catches clicks that land on the cockpit tablet
bool g_captureDown = false;          // a press on the cockpit tablet is being held
int g_winHoverCycle = -10;           // the last sim frame the mouse was over the floating window
float g_winScale = 1.0f;             // screen units per layout unit in the floating window, and where the layout starts in it
float g_winOx = 0, g_winOy = 0;

XPLMDataRef g_drClickX = nullptr, g_drClickY = nullptr, g_drZulu = nullptr, g_drIcao = nullptr, g_drVr = nullptr, g_drUiScale = nullptr, g_drBus = nullptr;
float g_battery = 100.0f;            // the tablet's own battery, percent
int g_wxLogoTex = 0;                 // the WXPlane icon's picture
int g_iconsTex = 0;
int g_logoTex = 0;                   // XPTablet's logo
xpt::Actions g_pendingGround;        // a press on the Ground page, waiting for the flight loop

// X-Plane's weather at the aircraft, the ground services and the ToLiss doors: looked up once, read in the flight loop
struct SimRefs {
    XPLMDataRef windDir = nullptr, windMs = nullptr, vis = nullptr, temp = nullptr, dew = nullptr, qnh = nullptr, rain = nullptr, alt = nullptr;
    XPLMDataRef cBase = nullptr, cTop = nullptr, cCover = nullptr, aAlt = nullptr, aDir = nullptr, aKt = nullptr, onGround = nullptr;
    XPLMDataRef jwStatus = nullptr, jwDoorStatus = nullptr;            // openSAM: the stand's jetway, and docked by door
    XPLMDataRef slides = nullptr, cabinPerc = nullptr, cabinLevel = nullptr, exitConfig = nullptr;   // ToLiss
    XPLMDataRef cabinAuto = nullptr;   // ToLiss's cabin light auto setting, when its plugin has one (found by name, probeToLiss)
    bool wasDocked[3] = {false, false, false};                         // for opening and closing the doors with the jetways
    bool xpJetway = false;                                             // without openSAM: X-Plane's own jetways, as last pressed
    bool wantDoor[3] = {false, false, false};                          // the doors the user connected (only those open and show docked)
    double xpJetwayT0 = -100;                                          // when X-Plane's own jetways were last sent for, or away (they take a while)
    double jwPressT[3] = {-100, -100, -100};                           // when each door's bubble was last pressed
    bool cabinReady = false, cabinReadyGround = true;                  // ToLiss: Cabin Ready given; on the ground when it was (cleared on takeoff, landing)
    XPLMDataRef jwNumber = nullptr;                                    // openSAM: how many jetways serve the plane
    XPLMDataRef beacon = nullptr, engRun = nullptr, parkBrake = nullptr, isChocked = nullptr, gs = nullptr, tlParkBrake = nullptr;
    XPLMDataRef paxDoor = nullptr, cargoDoor = nullptr, chocks = nullptr, extPower = nullptr, hpAir = nullptr, lpAir = nullptr;   // ToLiss
    bool probed = false;                                               // what the ToLiss offers has been written to Log.txt
    bool found = false;
} g_refs;
std::vector<XPLMDataRef> g_ownedRefs;
struct Cmd { XPLMCommandRef ref; int id; };
std::vector<Cmd> g_cmds;
XPLMMenuID g_menu = nullptr;
int g_menuItem = -1;
XPLMFlightLoopID g_loop = nullptr;

bool panelActive() { return g_ownsAviTab && g_panelEnabled != 0 && g_panelWidth > 0 && g_panelHeight > 0; }

// Where the tablet's picture goes inside the aircraft's rectangle. AviTab's picture is 800 x 480 and is centred in the rectangle at
// that shape, and aircraft are drawn around exactly that (the ToLiss A320 reserves 1024 x 640 and AviTab fills 1024 x 614 of it; its
// own menu strip meets that picture's top edge). Filling the whole rectangle put our top rows under the aircraft's own artwork.
constexpr float kPanelAspect = 800.0f / 480.0f;
void panelPicture(float& l, float& b, float& w, float& h) {
    const float rw = (float)g_panelWidth, rh = (float)g_panelHeight;
    if (rw / rh > kPanelAspect) { h = rh; w = std::floor(rh * kPanelAspect); }
    else { w = rw; h = std::floor(rw / kPanelAspect); }
    l = (float)g_panelLeft + std::floor((rw - w) * 0.5f);
    b = (float)g_panelBottom + std::floor((rh - h) * 0.5f);
}

// ---- the cockpit tablet's place: AviTab.json in the aircraft folder ------------------------------------------------------------------
std::string aircraftDir() {
    char file[512] = "", path[2048] = "";
    XPLMGetNthAircraftModel(0, file, path);
    std::string p = path;
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : p.substr(0, cut + 1);
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// A ToLiss aircraft is known by its systems plugin: one with "toliss" in its signature (XP11.ToLiss.A320.systems), loaded from the
// aircraft's own folder. (Plugins in Resources/plugins with ToLiss in their name are add-ons, loaded with every aircraft.)
bool isToLissAircraft() {
    auto lower = [](std::string s) { for (char& c : s) { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); else if (c == '\\') c = '/'; } return s; };
    const std::string dir = lower(aircraftDir());
    if (dir.empty()) return false;
    const int n = XPLMCountPlugins();
    for (int i = 0; i < n; ++i) {
        char name[256] = "", path[1024] = "", sig[256] = "", desc[256] = "";
        const XPLMPluginID id = XPLMGetNthPlugin(i);
        XPLMGetPluginInfo(id, name, path, sig, desc);
        if (lower(sig).find("toliss") != std::string::npos && lower(path).compare(0, dir.size(), dir) == 0) return true;
    }
    return false;
}

// ---- settings, kept in Output/preferences/XPTablet.prf ---------------------------------------------------------------------------------
std::string settingsPath() {
    char p[2048] = "";
    XPLMGetPrefsPath(p);   // .../Output/preferences/Set X-Plane.prf
    std::string s = p;
    const size_t cut = s.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : s.substr(0, cut + 1) + "XPTablet.prf";
}
std::string g_savedSettings;   // the settings as last read or written, so a change of any kind is seen and saved
void loadSettings() {
    std::string text;
    if (!settingsPath().empty() && readFile(settingsPath(), text)) xpt::settingsFromText(text);
    g_savedSettings = xpt::settingsToText();   // (what is in force now: a later difference is a change to save)
}
void saveSettings() {
    const std::string path = settingsPath();
    if (path.empty()) return;
    const std::string text = xpt::settingsToText();
    // written beside it, then put in its place: a crash or a full disk mid-write never leaves the settings empty
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { logMsg("cannot write %s", tmp.c_str()); return; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) { std::remove(tmp.c_str()); logMsg("cannot write %s", tmp.c_str()); return; }
    std::remove(path.c_str());   // (Windows will not rename over a file)
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { logMsg("cannot replace %s", path.c_str()); return; }
    g_savedSettings = text;
}
// ---- the update check: once a session, the version on the SkunkCrafts Updater's release (the cfg the updater app reads); newer
// than this one, a notification says so (once, after start-up), as WXPlane does
std::thread g_updTh;
std::atomic<bool> g_updCancel{false};
std::mutex g_updM;
std::string g_updVersion;   // the newer version, "" when none (guarded by g_updM)
bool g_updChecked = false, g_updShown = false;
bool versionNewer(const std::string& a, const std::string& b) {   // a > b, compared number by number ("0.10" > "0.9.2")
    auto parts = [](const std::string& v) {
        std::vector<long> p; long n = -1;
        for (char c : v) { if (std::isdigit((unsigned char)c)) n = (n < 0 ? 0 : n) * 10 + (c - '0'); else if (c == '.') { p.push_back(n < 0 ? 0 : n); n = -1; } else break; }
        if (n >= 0) p.push_back(n);
        return p;
    };
    const std::vector<long> pa = parts(a), pb = parts(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}
void updateCheckStart() {
    if (g_updChecked) return;
    g_updChecked = true;
    g_updCancel = false;
    g_updTh = std::thread([]() {
        std::string body, err;
        long status = 0;
        const std::string url = "https://raw.githubusercontent.com/domvid2005/XPTablet/refs/heads/release/XPTablet/skunkcrafts_updater.cfg";
        if (!wx::httpGetCancellable(url, "", body, status, err, 15, 64 * 1024, &g_updCancel) || status != 200) {
            if (!g_updCancel) logMsg("update check: could not read the SkunkCrafts release (%s)", err.empty() ? ("HTTP " + std::to_string(status)).c_str() : err.c_str());
            return;
        }
        std::string ver;
        size_t at = body.find("version|");
        if (at != std::string::npos) {
            at += 8;
            const size_t e = body.find_first_of("\r\n", at);
            ver = body.substr(at, e == std::string::npos ? std::string::npos : e - at);
            while (!ver.empty() && std::isspace((unsigned char)ver.back())) ver.pop_back();
        }
        if (ver.empty()) { logMsg("update check: no version in the SkunkCrafts release"); return; }
        if (versionNewer(ver, XPT_VERSION)) {
            { std::lock_guard<std::mutex> lk(g_updM); g_updVersion = ver; }
            logMsg("update check: XPTablet %s is available on the SkunkCrafts Updater (this is %s)", ver.c_str(), XPT_VERSION);
        } else logMsg("update check: up to date (%s; the SkunkCrafts release is %s)", XPT_VERSION, ver.c_str());
    });
}
void updateCheckStop() {
    g_updCancel = true;
    if (g_updTh.joinable()) g_updTh.join();
}
// (each frame) the notification, once: after the tablet has started up
void updateNotify() {
    if (g_updShown) return;
    std::string v;
    { std::lock_guard<std::mutex> lk(g_updM); v = g_updVersion; }
    if (v.empty()) return;
    g_updShown = true;
    xpt::notify("Update available", "XPTablet " + v + " is ready in the SkunkCrafts Updater (this is " XPT_VERSION ")", xpt::APP_SETTINGS);
}

extern "C" const unsigned char xpt_asset_notify[];   // the notification chime: 16-bit mono PCM at 48 kHz (assets/notify_48k_mono.pcm)
extern "C" const unsigned int xpt_asset_notify_size;
// The chime, through X-Plane's own sound (its interface bus), at Settings, Media's volume: once for each new notification (and Test)
void chimeStep() {
    static int last = -1;
    const int seq = xpt::chimeSeq();
    if (last < 0) { last = seq; return; }
    if (seq == last) return;
    last = seq;
    if (!xpt::tabletPoweredOn()) return;   // (off or flat: silent)
    const float vol = xpt::settings().notifyVolume;
    if (vol <= 0.001f) return;
    FMOD_CHANNEL* ch = XPLMPlayPCMOnBus((void*)xpt_asset_notify, xpt_asset_notify_size, FMOD_SOUND_FORMAT_PCM16, 48000, 1, 0, xplm_AudioUI, nullptr, nullptr);
    if (ch) XPLMSetAudioVolume(ch, vol);
}
// once a second: saved whenever the settings differ from the file, whether or not the change was flagged
void saveSettingsIfChanged(float dt) {
    static float since = 0;
    since += dt;
    if (since < 1.0f) return;
    since = 0;
    if (xpt::settingsToText() != g_savedSettings) saveSettings();
}

// The aircraft top views: shapes/aircraft_shapes.txt next to the platform folders (XPTablet/<platform>/XPTablet.xpl).
void loadShapesFile() {
    char path[2048] = "";
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    std::string p = path;
    for (int up = 0; up < 2; ++up) { const size_t cut = p.find_last_of("/\\"); if (cut != std::string::npos) p.resize(cut); }
    const std::string file = p + "/shapes/aircraft_shapes.txt";
    std::string text;
    if (!readFile(file, text)) { logMsg("%s not found: aircraft are drawn from their measurements", file.c_str()); return; }
    logMsg("%d aircraft top views (AircraftShapesSVG) read", xpt::loadShapes(text));
}

void loadAircraftConfig() {
    g_panelLeft = g_panelBottom = g_panelWidth = g_panelHeight = 0;
    g_panelEnabled = g_panelPowered = 0;
    g_captureClicks = true;
    const std::string dir = aircraftDir();
    if (dir.empty()) return;
    // XPTablet.json (same layout) wins, so an aircraft or a user can give XPTablet its own area; then AviTab's file
    for (const char* name : {"XPTablet.json", "AviTab.json", "avitab.json", "Avitab.json"}) {
        std::string text;
        if (!readFile(dir + name, text)) continue;
        wx::Json j;
        const wx::Json* p = wx::parseJson(text, j) ? j.get("panel") : nullptr;
        if (!p) { logMsg("%s%s: no \"panel\" section, ignored", dir.c_str(), name); continue; }
        auto num = [&](const char* k) { const wx::Json* v = p->get(k); return v ? (int)std::lround(v->asNum(0)) : 0; };
        auto flag = [&](const char* k) { const wx::Json* v = p->get(k); return v && ((v->type == wx::Json::Bool && v->b) || (v->type == wx::Json::Num && v->num != 0)); };
        g_panelLeft = num("left"); g_panelBottom = num("bottom"); g_panelWidth = num("width"); g_panelHeight = num("height");
        if (flag("enabled")) g_panelEnabled = g_panelPowered = 1;
        g_captureClicks = !flag("disable_capture_window");
        logMsg("%s: cockpit tablet at %d,%d size %dx%d%s%s", name, g_panelLeft, g_panelBottom, g_panelWidth, g_panelHeight,
             g_panelEnabled ? ", enabled" : ", waits for the aircraft to enable it", g_captureClicks ? "" : ", clicks come from the aircraft");
        return;
    }
    logMsg("no AviTab.json in %s: window only for this aircraft", dir.c_str());
}

// Where on the cockpit tablet the mouse points, in layout units. False when it is not on the tablet.
bool panelPointer(float& x, float& y) {
    if (!panelActive() || !g_panelPowered || !g_drClickX || !g_drClickY) return false;
    const float cx = XPLMGetDataf(g_drClickX), cy = XPLMGetDataf(g_drClickY);
    float l, b, w, h;
    panelPicture(l, b, w, h);
    if (cx < l || cx > l + w || cy < b || cy > b + h) return false;
    x = (cx - l) / w * g_logW;
    y = (b + h - cy) / h * xpt::kLogicalH;
    return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------
void buildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    g_fonts = xpt::loadFonts(scale);
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!g_fontTex) XPLMGenerateTextureNumbers(&g_fontTex, 1);
    XPLMBindTexture2d(g_fontTex, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
    io.Fonts->ClearTexData();
    if (!g_wxLogoTex) {   // the WXPlane icon's picture, with its smaller copies (trilinear: clean at any icon size)
        unsigned int bytes = 0;
        const unsigned char* px = xpt::wxLogoPixels(bytes);
        XPLMGenerateTextureNumbers(&g_wxLogoTex, 1);
        XPLMBindTexture2d(g_wxLogoTex, 0);
        int level = 0, last = 0;
        size_t off = 0;
        for (int n = 256; n >= 1 && off + (size_t)n * n * 4 <= bytes; n /= 2, ++level) {
            gl.TexImage2D(GL_TEXTURE_2D, level, (GLint)GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + off);
            off += (size_t)n * n * 4;
            last = level;
        }
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, last);
    }
    g_fonts.wxLogo = (ImTextureID)(intptr_t)g_wxLogoTex;
    if (!g_logoTex) {   // XPTablet's logo, with its smaller copies (each made from the original at full quality)
        int lw = 0, lh = 0, lv = 0;
        const unsigned char* px = xpt::logoPixels(lw, lh, lv);
        if (px && lw > 0 && lh > 0) {
            XPLMGenerateTextureNumbers(&g_logoTex, 1);
            XPLMBindTexture2d(g_logoTex, 0);
            gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            size_t off = 0;
            int w = lw, h = lh;
            for (int level = 0; level < lv; ++level) {
                gl.TexImage2D(GL_TEXTURE_2D, level, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + off);
                off += (size_t)w * h * 4;
                w = std::max(1, w / 2); h = std::max(1, h / 2);
            }
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, lv - 1);
            XPLMBindTexture2d(0, 0);
        }
    }
    if (!g_iconsTex) {   // the app icons' atlas, the same way
        int lw = 0, lh = 0, lv = 0;
        const unsigned char* px = xpt::iconsPixels(lw, lh, lv);
        if (px && lw > 0 && lh > 0) {
            XPLMGenerateTextureNumbers(&g_iconsTex, 1);
            XPLMBindTexture2d(g_iconsTex, 0);
            gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            size_t off = 0;
            int w = lw, h = lh;
            for (int level = 0; level < lv; ++level) {
                gl.TexImage2D(GL_TEXTURE_2D, level, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + off);
                off += (size_t)w * h * 4;
                w = std::max(1, w / 2); h = std::max(1, h / 2);
            }
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, lv - 1);
            XPLMBindTexture2d(0, 0);
        }
    }
    g_fonts.icons = (ImTextureID)(intptr_t)g_iconsTex;
    g_fonts.logo = (ImTextureID)(intptr_t)g_logoTex;
    g_fontScale = scale;
}

// The floating window's geometry and how the layout sits in it. With a cockpit tablet active the window shows the same picture, so
// it keeps the tablet's shape (bars at the sides if the window is another shape); alone, the layout fills the window.
void windowLayout(float& ww, float& wh) {
    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(g_win, &l, &t, &r, &b);
    ww = (float)std::max(1, r - l); wh = (float)std::max(1, t - b);
}

// ---- the web browser ---------------------------------------------------------------------------------------------------------
// xptbrowser runs X-Plane's own Chromium (Resources/dlls/64/cef/<platform>) outside X-Plane, started the first time a web page
// is shown and kept running (logins and open pages stay) until the plugin is disabled. Its pictures become one texture.
int g_webTex = 0, g_webTexW = 0, g_webTexH = 0;
int g_webViewW = 0, g_webViewH = 0;
float g_webViewScale = 0;
bool g_webTried = false;
bool g_webShown = false;
bool g_wantKeys = false;   // the floating window takes the keyboard (a web page or a text field is shown)

bool fileExists(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; }
void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/' && path[i] != '\\') continue;
        const std::string d = path.substr(0, i);
#if defined(_WIN32)
        CreateDirectoryA(d.c_str(), nullptr);
#else
        mkdir(d.c_str(), 0755);
#endif
    }
}
std::string pluginFolder() {   // .../plugins/XPTablet (the platform folder's parent)
    char path[2048] = "";
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    std::string p = path;
    for (int up = 0; up < 2; ++up) { const size_t cut = p.find_last_of("/\\"); if (cut != std::string::npos) p.resize(cut); }
    return p;
}

void browserStart() {
    g_webTried = true;
    xpt::BrowserState& b = g_sim.browser;
#if defined(__APPLE__)
    b.status = -3;
    b.message = "On macOS the web pages are not available yet: X-Plane's Chromium there can only run inside X-Plane itself.";
#else
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    const std::string root = sys;
  #if defined(_WIN32)
    const char* cands[] = {"Resources/dlls/64/cef/win", "Resources/dlls/64/cef"};
    const char* lib = "/libcef.dll";
    const std::string helper = pluginFolder() + "/win_x64/xptbrowser.exe";
  #else
    const char* cands[] = {"Resources/dlls/64/cef/lin", "Resources/dlls/64/cef/linux", "Resources/dlls/64/cef", "Resources/dlls/64"};
    const char* lib = "/libcef.so";
    const std::string helper = pluginFolder() + "/lin_x64/xptbrowser";
  #endif
    std::string cef;
    for (const char* c : cands) if (fileExists(root + c + lib)) { cef = root + c; break; }
    if (cef.empty()) { b.status = -3; b.message = "X-Plane's Chromium (libcef) was not found under Resources/dlls/64/cef."; logMsg("browser: %s", b.message.c_str()); return; }
    if (!fileExists(helper)) { b.status = -1; b.message = "xptbrowser is missing from the plugin folder; reinstall XPTablet."; logMsg("browser: %s not found", helper.c_str()); return; }
    const std::string out = root + "Output/XPTablet/browser";
    makeDirs(out);
    std::string err;
    if (!xpt::browser::start(helper, cef, out, err)) { b.status = -1; b.message = err; logMsg("browser: %s", err.c_str()); return; }
    b.status = 0; b.message.clear();
    g_webViewW = g_webViewH = 0; g_webViewScale = 0;
    logMsg("browser: started with CEF from %s, profile %s", cef.c_str(), out.c_str());
#endif
}

void browserStop() {
    if (xpt::browser::started()) xpt::browser::stop();
    g_sim.browser = xpt::BrowserState();
    g_webTexW = g_webTexH = 0;
    g_webTried = false;
}

// After each UI build: start the browser when a page wants it, send it what happened, and take its newest picture.
void browserStep(const xpt::Actions& act) {
    namespace br = xpt::browser;
    xpt::BrowserState& b = g_sim.browser;
    if (act.webRetry) browserStop();
    if (act.webShown && !br::started() && !g_webTried) browserStart();
    if (!br::started()) return;
    br::heartbeat();
    const int st = br::state();
    if (st == -1) { if (b.status != -1) logMsg("browser: %s", br::error().c_str()); b.status = -1; b.message = br::error(); b.tex = 0; return; }
    b.status = st == 1 ? 1 : 0;
    if (act.webShown) {   // the view's size, in layout units; the picture is drawn at the tablet's pixels per unit
        const int w = (int)std::lround(act.webW), h = (int)std::lround(act.webH);
        const float sc = std::max(1.0f, std::min(2.0f, g_fontScale));
        if (w > 0 && h > 0 && (w != g_webViewW || h != g_webViewH || std::fabs(sc - g_webViewScale) > 0.01f)) {
            g_webViewW = w; g_webViewH = h; g_webViewScale = sc;
            br::push(xptb::CMD_RESIZE, w, h, 0, 0, sc);
        }
    }
    for (const xpt::Actions::WebEvent& e : act.web) {
        if (e.type == xptb::CMD_KEY) br::push(e.type, 0, 0, e.code, (int)e.x);
        else if (e.type == xptb::CMD_CHAR) br::push(e.type, 0, 0, e.code);
        else br::push(e.type, (int)e.x, (int)e.y, e.code, e.code2, e.ctrl ? 1.0f : 0.0f);
    }
    if (!act.webNavigate.empty()) br::push(xptb::CMD_NAVIGATE, 0, 0, 0, 0, 0, act.webNavigate);
    switch (act.webNav) {
    case 1: br::push(xptb::CMD_BACK); break;
    case 2: br::push(xptb::CMD_FORWARD); break;
    case 3: br::push(xptb::CMD_RELOAD); break;
    case 4: br::push(xptb::CMD_STOP); break;
    default: break;
    }
    if (act.webTabNew) br::push(xptb::CMD_TAB_NEW, 0, act.webTabGroup, 0, 0, 0, act.webTabUrl);
    if (act.webZoom == 3) br::push(xptb::CMD_ZOOM, 3, act.webZoomPct);
    else if (act.webZoom) br::push(xptb::CMD_ZOOM, act.webZoom == 2 ? 0 : act.webZoom);
    {   // the Browser's media volume: as set (silent while the tablet is off or flat), sent when it changes and when the browser starts
        static int sent = -1;
        if (g_sim.browser.status != 1) sent = -1;
        else {
            const int want = xpt::tabletPoweredOn() ? (act.webVolume >= 0 ? act.webVolume : (int)std::lround(xpt::settings().browserVolume * 100)) : 0;
            if (want != sent) { sent = want; br::push(xptb::CMD_MEDIA_VOLUME, want); }
        }
    }
    if (act.webExitFullscreen) br::push(xptb::CMD_EXIT_FULLSCREEN);
    if (act.webTabSelect) br::push(xptb::CMD_TAB_SELECT, act.webTabSelect);
    if (act.webTabClose) br::push(xptb::CMD_TAB_CLOSE, act.webTabClose);
    bool loading = false, back = false, fwd = false, kbd = false;
    std::vector<br::TabInfo> tabs;
    int active = 0, zoom = 100;
    bool full = false;
    bool zAuto = true;
    if (br::info(b.url, b.title, loading, back, fwd, kbd, tabs, active, zoom, full, &zAuto)) {
        b.zoomPct = zoom; b.fullscreen = full; b.zoomAuto = zAuto;
        b.loading = loading; b.canBack = back; b.canForward = fwd; b.wantsKeyboard = kbd;
        b.tabs.clear();
        for (const br::TabInfo& t : tabs) b.tabs.push_back({t.id, t.title, t.url, t.loading, t.group});
        b.activeTab = active;
    }
    const uint8_t* px = nullptr;
    int w = 0, h = 0;
    if (br::frame(px, w, h)) {
        if (!g_webTex) XPLMGenerateTextureNumbers(&g_webTex, 1);
        XPLMBindTexture2d(g_webTex, 0);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (w != g_webTexW || h != g_webTexH || !gl.TexSubImage2D) {
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, px);
            g_webTexW = w; g_webTexH = h;
        } else {
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, 0x80E1, GL_UNSIGNED_BYTE, px);
        }
        XPLMBindTexture2d(0, 0);
        br::done();
        b.tex = (ImTextureID)(intptr_t)g_webTex;
        b.texW = w; b.texH = h;
    }
}

// ---- the map: tiles as textures, and X-Plane's airports ---------------------------------------------------------------------
// Tiles are fetched and decoded on threads of their own (src/tiles.cpp); here they become textures, at most kMaxTileTex of them,
// the least recently drawn given to new tiles. The airport index is built the first time the Map page is shown.
constexpr size_t kMaxTileTex = 320;
struct TileTex { int tex = 0; int used = 0; };
std::map<xpt::tiles::Key, TileTex> g_tileTex;
std::vector<xpt::tiles::Key> g_tileWants;
bool g_mapStarted = false;
bool g_ourAirportsStarted = false;
std::thread g_docThread;   // reads a picture for the Documents page
std::thread g_sbThread;    // fetches the SimBrief plan

ImTextureID tileTexture(int src, int z, int x, int y, bool request) {
    const xpt::tiles::Key k{src, z, x, y};
    auto it = g_tileTex.find(k);
    if (it != g_tileTex.end()) { it->second.used = XPLMGetCycleNumber(); return (ImTextureID)(intptr_t)it->second.tex; }
    if (request && g_tileWants.size() < 64) g_tileWants.push_back(k);
    return 0;
}

void mapStart() {
    if (g_mapStarted) return;
    g_mapStarted = true;
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    const std::string root = sys;
    makeDirs(root + "Output/XPTablet/tiles/");
    xpt::tiles::start(root + "Output/XPTablet/tiles");
    xpt::aptdat::start(root);
    xpt::navdata::start(root);
    logMsg("map: tiles cached in Output/XPTablet/tiles; reading X-Plane's airports");
}
void mapStop() {
    if (!g_mapStarted) return;
    xpt::tiles::stop();
    xpt::aptdat::stop();
    xpt::navdata::stop();
    if (g_sbThread.joinable()) g_sbThread.join();
    g_mapStarted = false;
    xpt::ourairports::stop();
    g_ourAirportsStarted = false;
    xpt::vatsim::stop();
    if (g_docThread.joinable()) g_docThread.join();
    g_sim.map.airports.clear();
    g_sim.map.airportPoints.clear();
}

// After each UI build: what the map asked for, and the tiles that arrived (a few per frame, so no frame takes long)
void mapStep(const xpt::Actions& act) {
    // the airports' runways round the aircraft are wanted for the runway collision alert, map or no map: on the ground or
    // low (under 3000 ft above it), the airport index starts, and when the map is not on screen its view is the aircraft
    const xpt::MapState& ms = g_sim.map;
    const bool nearGround = ms.havePos && (ms.onGround || ms.aglFt < 3000) && xpt::settings().notifyTcas;
    if (act.mapShown || (nearGround && XPLMGetElapsedTime() > 20)) mapStart();
    if (!g_mapStarted) return;
    if (!act.mapShown && nearGround) {
        xpt::aptdat::setView(ms.lat, ms.lon, 4.0, 0.0, true);
        if ((XPLMGetCycleNumber() & 31) == 0) {
            int n = 0;
            std::vector<xpt::AptPoint> pts;
            xpt::aptdat::get(g_sim.map.airports, pts, g_sim.map.aptIndexing, n);
        }
    }
    if (act.mapShown) {
        xpt::tiles::want(g_tileWants);
        xpt::aptdat::setView(act.mapLat, act.mapLon, act.mapLayoutNm, act.mapPointNm, act.mapLayouts, act.mapPointMinFt);
        xpt::navdata::setView(act.mapLat, act.mapLon, act.mapNavNm, act.mapFixNm);
        if ((XPLMGetCycleNumber() & 7) == 0) {
            int n = 0;
            xpt::aptdat::get(g_sim.map.airports, g_sim.map.airportPoints, g_sim.map.aptIndexing, n);
            g_sim.map.aptCount = n;
            xpt::navdata::get(g_sim.map.navaids, g_sim.map.fixes);
        }
    }
    g_tileWants.clear();
    xpt::tiles::Tile t;
    for (int k = 0; k < 6 && xpt::tiles::take(t); ++k) {
        if (t.failed || t.w <= 0 || t.h <= 0 || g_tileTex.count(t.key)) continue;
        int tex = 0;
        if (g_tileTex.size() < kMaxTileTex) {
            XPLMGenerateTextureNumbers(&tex, 1);
        } else {   // the texture of the tile drawn longest ago
            auto old = g_tileTex.begin();
            for (auto it = g_tileTex.begin(); it != g_tileTex.end(); ++it) if (it->second.used < old->second.used) old = it;
            tex = old->second.tex;
            g_tileTex.erase(old);
        }
        XPLMBindTexture2d(tex, 0);
        // shown smaller than they are (a zoom between levels, a big picture fitted): mipmapped, made by the driver as they upload
        gl.TexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP */, 1);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
        XPLMBindTexture2d(0, 0);
        g_tileTex[t.key] = {tex, XPLMGetCycleNumber()};
    }
}

// ---- the SimBrief flight plan (the Flight Plan page, and the route on the map) ---------------------------------------------------
// The user's latest plan, from SimBrief's public fetcher with the saved user name or pilot ID, on a thread, when Import is pressed.
std::mutex g_sbMx;
bool g_sbDone = false;
xpt::FlightPlan g_sbPlan;

bool g_sbQuiet = false;   // the 2-minute check (not Import): nothing is shown unless the plan is new
void simbriefFetch(const std::string& user, bool quiet = false) {
    if (g_sbThread.joinable()) g_sbThread.join();
    g_sbQuiet = quiet;
    if (!quiet) { g_sim.plan.busy = true; g_sim.plan.status = "Asking SimBrief..."; }
    g_sbThread = std::thread([user] {
        const std::string url = xpt::simbriefUrl(user);
        std::string body, err;
        long code = 0;
        xpt::FlightPlan p;
        if (!wx::httpGet(url, body, code, err, 25, 32u << 20)) {
            p.status = code == 400 ? "SimBrief has no plan for that ID" : "SimBrief could not be reached";
        } else {
            p = xpt::parseSimbrief(body);
        }
        std::lock_guard<std::mutex> lk(g_sbMx);
        g_sbPlan = p; g_sbDone = true;
    });
}

// The plan's route for the map: the departure, its fixes, the destination
void planToRoute() {
    xpt::planToRoute(g_sim.plan, g_sim.map);
}

void planStep(const xpt::Actions& act) {
    if (act.planImport && !xpt::settings().simbriefUser.empty() && !g_sim.plan.busy) simbriefFetch(xpt::settings().simbriefUser);
    {   // every 2 minutes with the SimBrief ID: a new plan is loaded and announced
        static double next = 30;
        const double now = XPLMGetElapsedTime();
        const xpt::Settings& st = xpt::settings();
        bool idle;
        { std::lock_guard<std::mutex> lk(g_sbMx); idle = !g_sbDone; }
        if (st.notifySimbrief && !st.simbriefUser.empty() && !g_sim.plan.busy && idle && now >= next) {
            next = now + 120;
            simbriefFetch(st.simbriefUser, true);
        }
    }
    if (act.planAccept || act.planDismiss || act.planReview) { const bool took = act.planAccept; xpt::planDecide(g_sim, act); if (took) planToRoute(); }
    if (act.planClear) {
        if (g_sbThread.joinable()) g_sbThread.join();
        g_sim.plan = xpt::FlightPlan();
        g_sim.plan.status = "Cleared";
        planToRoute();
    }
    std::lock_guard<std::mutex> lk(g_sbMx);
    if (g_sbDone) {
        g_sbDone = false;
        if (g_sbThread.joinable()) g_sbThread.join();
        const int seq = g_sim.newPlanSeq;
        if (g_sbPlan.loaded) {   // the SID's and STAR's crossing restrictions, from X-Plane's procedure files (your AIRAC's first)
            char sys[2048] = "";
            XPLMGetSystemPath(sys);
            const std::string root = sys;
            const int n = xpt::addCrossings(g_sbPlan, [&](const std::string& icao) {
                std::string t;
                if (!readFile(root + "Custom Data/CIFP/" + icao + ".dat", t)) readFile(root + "Resources/default data/CIFP/" + icao + ".dat", t);
                return t;
            });
            logMsg("SimBrief: %d crossing restriction(s) from the %s and %s procedures", n, g_sbPlan.sid.empty() ? "-" : g_sbPlan.sid.c_str(), g_sbPlan.star.empty() ? "-" : g_sbPlan.star.c_str());
        }
        xpt::planFetched(g_sim, g_sbPlan, g_sbQuiet);   // into the preview: the flight plan stays until its Import
        if (g_sim.newPlanSeq != seq) logMsg("SimBrief: new plan %s offered", g_sbPlan.ofpId.c_str());
        else if (!g_sbQuiet) logMsg("SimBrief: %s", g_sbPlan.loaded ? "plan fetched for review" : g_sbPlan.status.c_str());
    }
}

// Settings, Data: where the tablet's data comes from (a few times a second is plenty)
void dataInfoStep() {
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    const std::string root = sys;
    auto& d = g_sim.dataInfo;
    d.clear();
    std::vector<std::shared_ptr<const xpt::AptGeo>> l; std::vector<xpt::AptPoint> pts; bool indexing = false; int n = 0;
    if (g_mapStarted) xpt::aptdat::get(l, pts, indexing, n);
    d.push_back({"Airports", !g_mapStarted ? "read when the Map or Airports page first opens" : indexing ? "reading X-Plane's scenery..." : std::to_string(n) + " in X-Plane's scenery (custom first)"});
    std::FILE* cf = std::fopen((root + "Custom Data/earth_nav.dat").c_str(), "rb");
    if (cf) std::fclose(cf);
    d.push_back({"Navaids", cf ? "Custom Data (your AIRAC, e.g. Navigraph's)" : "X-Plane's own (Resources/default data)"});
    const std::string oa = xpt::ourairports::status();
    d.push_back({"Airport facts", xpt::ourairports::ready() ? "OurAirports, kept a month" : !g_ourAirportsStarted ? "OurAirports: when the Airports page opens" : oa});
    d.push_back({"Map tiles", "OpenStreetMap / Esri, kept a month in Output/XPTablet/tiles"});
    d.push_back({"Weather", "X-Plane's own; METARs from NOAA"});
    d.push_back({"Documents", "Output/XPTablet/Documents, the aircraft's folder, AviTab's charts"});
}

void browserStop();
void resetBrowserData() {   // the helper quits first (it holds the files open); the next page shown starts it on an empty profile
    browserStop();
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    std::error_code ec;
    const auto n = std::filesystem::remove_all(std::filesystem::u8path(std::string(sys) + "Output/XPTablet/browser"), ec);
    logMsg("browser: profile erased (%d files)%s%s", (int)n, ec ? ", " : "", ec ? ec.message().c_str() : "");
}

void clearTileCache() {
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    std::error_code ec;
    const auto n = std::filesystem::remove_all(std::filesystem::u8path(std::string(sys) + "Output/XPTablet/tiles"), ec);
    makeDirs(std::string(sys) + "Output/XPTablet/tiles/");
    logMsg("map: %d cached tiles removed", (int)n);
}


std::vector<xpt::MapState::Traffic> g_vatsimTraffic;   // VATSIM's pilots as traffic (refreshed every few seconds while wanted)

// A VATSIM filed route's fixes, found in X-Plane's nav data one after another (each the nearest of its name to the one before):
// airways, speed and level groups, DCT and procedure names are left out (they are not fixes, or not on their own)
std::vector<xpt::MapState::RoutePoint> resolveRoute(const xpt::VatsimPilot& v) {
    std::vector<xpt::MapState::RoutePoint> out;
    auto find = [&](const std::string& id, XPLMNavType types, float nearLat, float nearLon, bool nearOk, xpt::MapState::RoutePoint& rp) {
        float la = nearLat, lo = nearLon;
        const XPLMNavRef r = XPLMFindNavAid(nullptr, id.c_str(), nearOk ? &la : nullptr, nearOk ? &lo : nullptr, nullptr, types);
        if (r == XPLM_NAV_NOT_FOUND) return false;
        float rla = 0, rlo = 0;
        char outId[32] = {};
        XPLMGetNavAidInfo(r, nullptr, &rla, &rlo, nullptr, nullptr, nullptr, outId, nullptr, nullptr);
        if (id != outId) return false;   // (a partial match)
        rp.lat = rla; rp.lon = rlo; rp.ident = id;
        return true;
    };
    xpt::MapState::RoutePoint rp;
    float la = (float)v.lat, lo = (float)v.lon;
    if (!v.departure.empty() && find(v.departure, xplm_Nav_Airport, la, lo, false, rp)) { out.push_back(rp); la = (float)rp.lat; lo = (float)rp.lon; }
    std::string tok;
    std::istringstream in(v.route);
    while (in >> tok && out.size() < 200) {
        const size_t slash = tok.find('/');
        if (slash != std::string::npos) tok = tok.substr(0, slash);   // "KORD/09L", "WAYPT/N0450F350"
        if (tok.empty() || tok == "DCT" || tok == v.departure || tok == v.arrival) continue;
        // fixes and navaids are 2 to 5 letters; anything with a figure is an airway (J146), a procedure (FLMGO2) or a group
        bool allLetters = tok.size() >= 2 && tok.size() <= 5;
        for (char c : tok) if (c < 'A' || c > 'Z') allLetters = false;
        if (!allLetters) continue;
        if (find(tok, (XPLMNavType)(xplm_Nav_Fix | xplm_Nav_VOR | xplm_Nav_NDB), la, lo, true, rp)) {
            const double d = std::hypot((rp.lat - la) * 60.0, (rp.lon - lo) * 60.0 * std::cos(la * 3.14159265 / 180.0));
            if (d > 1500) continue;   // (the same name somewhere else in the world)
            out.push_back(rp); la = (float)rp.lat; lo = (float)rp.lon;
        }
    }
    if (!v.arrival.empty() && find(v.arrival, xplm_Nav_Airport, la, lo, false, rp)) out.push_back(rp);
    return out;
}

// The aircraft and the time, for the map (a few times a second)
void readMapState() {
    static XPLMDataRef lat = XPLMFindDataRef("sim/flightmodel/position/latitude"), lon = XPLMFindDataRef("sim/flightmodel/position/longitude");
    static XPLMDataRef psi = XPLMFindDataRef("sim/flightmodel/position/true_psi"), gs = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    static XPLMDataRef loc = XPLMFindDataRef("sim/time/local_time_sec"), zulu = XPLMFindDataRef("sim/time/zulu_time_sec");
    static XPLMDataRef days = XPLMFindDataRef("sim/time/local_date_days");
    xpt::MapState& m = g_sim.map;
    m.havePos = lat && lon;
    if (m.havePos) { m.lat = XPLMGetDatad(lat); m.lon = XPLMGetDatad(lon); }
    m.hdgTrue = psi ? XPLMGetDataf(psi) : 0.0f;
    m.gsKt = gs ? XPLMGetDataf(gs) * 1.943844f : 0.0f;
    m.tile = tileTexture;
    static XPLMDataRef elev = XPLMFindDataRef("sim/flightmodel/position/elevation");
    m.altFt = elev ? (float)(XPLMGetDatad(elev) * 3.28084) : 0.0f;
    static XPLMDataRef vvi = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm"), agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    static XPLMDataRef gnd = XPLMFindDataRef("sim/flightmodel/failures/onground_any");
    m.vsFpm = vvi ? XPLMGetDataf(vvi) : 0.0f;
    m.aglFt = agl ? XPLMGetDataf(agl) * 3.28084f : 0.0f;
    m.onGround = gnd && XPLMGetDatai(gnd) != 0;
    // other aircraft, as X-Plane's TCAS sees them (AI and multiplayer: plugins such as xPilot and LiveTraffic feed it); 0 is us
    static XPLMDataRef tNum = XPLMFindDataRef("sim/cockpit2/tcas/indicators/tcas_num_acf");
    static XPLMDataRef tLat = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/lat"), tLon = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/lon");
    static XPLMDataRef tEle = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/ele"), tPsi = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/psi");
    static XPLMDataRef tVs = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/vertical_speed"), tV = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/V_msc");
    static XPLMDataRef tWow = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/weight_on_wheels");
    static XPLMDataRef tId = XPLMFindDataRef("sim/cockpit2/tcas/targets/flight_id"), tType = XPLMFindDataRef("sim/cockpit2/tcas/targets/icao_type");
    m.traffic.clear();
    if (tLat && tLon && tEle) {
        const int n = std::max(0, std::min(64, tNum ? XPLMGetDatai(tNum) : 64));
        float la[64] = {}, lo[64] = {}, el[64] = {}, ps[64] = {}, vs[64] = {}, v[64] = {};
        int wow[64] = {};
        char ids[512] = {}, types[512] = {};
        XPLMGetDatavf(tLat, la, 0, 64); XPLMGetDatavf(tLon, lo, 0, 64); XPLMGetDatavf(tEle, el, 0, 64);
        if (tPsi) XPLMGetDatavf(tPsi, ps, 0, 64);
        if (tVs) XPLMGetDatavf(tVs, vs, 0, 64);
        if (tV) XPLMGetDatavf(tV, v, 0, 64);
        if (tWow) XPLMGetDatavi(tWow, wow, 0, 64);
        if (tId) XPLMGetDatab(tId, ids, 0, 512);
        if (tType) XPLMGetDatab(tType, types, 0, 512);
        for (int i = 1; i < n; ++i) {
            if (la[i] == 0 && lo[i] == 0) continue;
            xpt::MapState::Traffic t;
            t.lat = la[i]; t.lon = lo[i]; t.altFt = el[i] * 3.28084f; t.hdg = ps[i]; t.vsFpm = vs[i]; t.gsKt = v[i] * 1.943844f; t.onGround = wow[i] != 0;
            t.callsign.assign(ids + i * 8, strnlen(ids + i * 8, 8));
            t.type.assign(types + i * 8, strnlen(types + i * 8, 8));
            if (t.callsign.empty()) t.callsign = t.type.empty() ? "TFC" : t.type;
            m.traffic.push_back(t);
        }
    }
    // and VATSIM's world (the Map's Traffic: Nearby + VATSIM World): every pilot not already in X-Plane's own list
    if (xpt::settings().mapTrafficSource == 1 && !g_vatsimTraffic.empty()) {
        std::unordered_set<std::string> have;
        for (const auto& t : m.traffic) have.insert(t.callsign);
        for (const auto& t : g_vatsimTraffic) if (!have.count(t.callsign)) m.traffic.push_back(t);
    }
    // the sim's moment: this year (X-Plane keeps no year), its day of the year and its UTC time of day
    if (zulu) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        const int year = tm.tm_year + 1900;
        long long jan1 = 0;   // days from 1970 to 1 January of the year
        for (int yy = 1970; yy < year; ++yy) jan1 += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
        const double z = XPLMGetDataf(zulu);
        int d = days ? XPLMGetDatai(days) : tm.tm_yday;
        double off = loc ? (double)XPLMGetDataf(loc) - z : 0.0;
        while (off > 14 * 3600.0) off -= 86400.0;
        while (off <= -12 * 3600.0) off += 86400.0;
        // the day is the local one: UTC is a day on where local time is behind and has gone past midnight (or the other way)
        const double localTod = z + off;
        if (localTod < 0) d += 1; else if (localTod >= 86400) d -= 1;
        m.utc = (jan1 + d) * 86400LL + (long long)z;
        m.haveLocalOffset = loc != nullptr;
        m.localOffsetS = std::round(off / 900.0) * 900.0;
    }
}

// ---- the Airports page: X-Plane's airport and OurAirports' facts, with a CTAF always ---------------------------------------------
std::string g_aptSearchSent = "\x01";
std::chrono::steady_clock::time_point g_aptNearestAt;
std::string g_aptWanted;                       // the airport the page shows
std::shared_ptr<const xpt::AptGeo> g_aptGeo;   // its layout, once read
bool g_aptMergedWithOA = false;                // OurAirports' facts are in (they may come later than the layout)

const char* freqName(int kind) {
    static const char* n[7] = {"ATIS", "UNICOM", "Clearance", "Ground", "Tower", "Approach", "Departure"};
    return kind >= 0 && kind < 7 ? n[kind] : "Radio";
}
std::string oaName(const std::string& t) {
    if (t == "CTAF") return "CTAF";
    if (t == "UNIC" || t == "UNICOM") return "UNICOM";
    if (t == "TWR") return "Tower";
    if (t == "GND") return "Ground";
    if (t == "ATIS" || t == "AWOS" || t == "ASOS" || t == "AFIS") return t;
    if (t == "CLD" || t == "DEL" || t == "CD") return "Clearance";
    if (t == "APP" || t == "A/D") return "Approach";
    if (t == "DEP") return "Departure";
    if (t == "MULTICOM" || t == "MULT") return "MULTICOM";
    if (t == "RDO" || t == "FSS") return "Radio";
    return t;
}

// The airport's facts: its radio from the scenery and from OurAirports (merged, the same frequency once), and the CTAF by these rules:
// the published CTAF; else UNICOM (the CTAF at most uncontrolled US fields); else the tower's frequency, used as the CTAF when the
// tower is closed; else 122.900 (MULTICOM), the frequency for a field with none published.
void buildAirportInfo() {
    xpt::AirportInfo& a = g_sim.airport;
    a.icao = g_aptWanted;
    a.busy = false;
    a.found = g_aptGeo != nullptr;
    a.geo = g_aptGeo;
    a.freqs.clear();
    a.name.clear(); a.city.clear(); a.country.clear(); a.region.clear(); a.iata.clear();
    if (!a.found) return;
    const xpt::ourairports::Info oa = xpt::ourairports::lookup(g_aptWanted);
    g_aptMergedWithOA = xpt::ourairports::ready();
    if (oa.found) { a.name = oa.name; a.city = oa.city; a.country = oa.country; a.region = oa.region; a.iata = oa.iata; }
    auto add = [&](const std::string& what, double mhz, const std::string& note) {
        for (const auto& f : a.freqs) if (std::fabs(f.mhz - mhz) < 0.003 && f.what == what) return;
        a.freqs.push_back({what, mhz, note});
    };
    for (const auto& f : g_aptGeo->freqs) add(freqName(f.kind), f.mhz, f.name);
    for (const auto& f : oa.freqs) add(oaName(f.type), f.mhz, f.desc);
    std::stable_sort(a.freqs.begin(), a.freqs.end(), [](const xpt::AirportInfo::Freq& x, const xpt::AirportInfo::Freq& y) {
        static const char* order[] = {"ATIS", "AWOS", "ASOS", "AFIS", "CTAF", "UNICOM", "MULTICOM", "Clearance", "Ground", "Tower", "Approach", "Departure"};
        auto rank = [&](const std::string& s) { for (int i = 0; i < 12; ++i) if (s == order[i]) return i; return 12; };
        return rank(x.what) < rank(y.what);
    });
    auto find = [&](const char* what) -> const xpt::AirportInfo::Freq* { for (const auto& f : a.freqs) if (f.what == what) return &f; return nullptr; };
    if (const auto* f = find("CTAF")) a.ctaf = {"CTAF", f->mhz, "Published CTAF"};
    else if (const auto* u = find("UNICOM")) a.ctaf = {"CTAF", u->mhz, "UNICOM, used as the CTAF"};
    else if (const auto* t = find("Tower")) a.ctaf = {"CTAF", t->mhz, "Tower frequency, the CTAF when the tower is closed"};
    else a.ctaf = {"CTAF", 122.900, "None published: MULTICOM 122.900"};
}

void airportsStep(const xpt::Actions& act) {
    if (!act.airportsShown && !(act.mapShown && !act.airportOpen.empty())) return;   // the Airports page, or an airport's card on the map
    if (!g_mapStarted) mapStart();   // the index lives with the map's
    if (!g_ourAirportsStarted) {
        g_ourAirportsStarted = true;
        char sys[2048] = "";
        XPLMGetSystemPath(sys);
        makeDirs(std::string(sys) + "Output/XPTablet/");
        xpt::ourairports::start(std::string(sys) + "Output/XPTablet/ourairports");
    }
    // the list: the search's results, or the nearest airports (every few seconds, as the aircraft moves)
    bool indexing = false;
    { std::vector<std::shared_ptr<const xpt::AptGeo>> l; std::vector<xpt::AptPoint> p; int n = 0; xpt::aptdat::get(l, p, indexing, n); }
    const auto now = std::chrono::steady_clock::now();
    if (indexing) { g_sim.airportStatus = "Finding X-Plane's airports..."; g_aptSearchSent = "\x01"; }
    else if (act.airportSearch != g_aptSearchSent || (act.airportSearch.empty() && now - g_aptNearestAt > std::chrono::seconds(5))) {
        g_aptSearchSent = act.airportSearch;
        g_aptNearestAt = now;
        g_sim.airportList = act.airportSearch.empty() ? (g_sim.map.havePos ? xpt::aptdat::nearest(g_sim.map.lat, g_sim.map.lon, 12) : std::vector<xpt::AptPoint>())
                                                      : xpt::aptdat::search(act.airportSearch, 12);
        g_sim.airportStatus.clear();
    }
    // the airport shown
    if (!act.airportOpen.empty() && act.airportOpen != g_aptWanted && !indexing) {
        g_aptWanted = act.airportOpen;
        g_aptGeo.reset();
        g_sim.airport = xpt::AirportInfo();
        g_sim.airport.icao = g_aptWanted;
        g_sim.airport.busy = true;
        xpt::aptdat::requestAirport(g_aptWanted);
    }
    std::shared_ptr<const xpt::AptGeo> g;
    std::string icao;
    if (xpt::aptdat::takeAirport(g, icao) && icao == g_aptWanted) { g_aptGeo = g; buildAirportInfo(); }
    else if (!g_sim.airport.busy && g_sim.airport.found && !g_aptMergedWithOA && xpt::ourairports::ready()) buildAirportInfo();
}

// ---- the Documents page ------------------------------------------------------------------------------------------------------------
// PDFs and pictures from Output/XPTablet/Documents, the aircraft's folder and AviTab's charts folder. Pictures are read on a thread.
std::mutex g_docMx;
bool g_docReady = false;
std::string g_docPath, g_docError;
std::vector<uint8_t> g_docPixels;
int g_docW = 0, g_docH = 0;
int g_docTex = 0;

void listDocuments() {
    namespace fs = std::filesystem;
    xpt::DocsState& d = g_sim.docs;
    d.files.clear(); d.folders.clear(); d.listed = true;
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    const std::string root = sys;
    const std::string mine = root + "Output/XPTablet/Documents";
    makeDirs(mine + "/");
    struct Where { std::string path, label; int depth; };
    std::vector<Where> where = {{mine, "XPTablet's Documents", 4}};
    const std::string acf = aircraftDir();
    if (!acf.empty()) where.push_back({acf, "This aircraft", 3});
    if (fs::exists(fs::u8path(root + "Resources/plugins/AviTab/charts"))) where.push_back({root + "Resources/plugins/AviTab/charts", "AviTab charts", 4});
    for (const Where& w : where) {
        d.folders.push_back(w.path);
        std::error_code ec;
        fs::recursive_directory_iterator it(fs::u8path(w.path), fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && d.files.size() < 500; it.increment(ec)) {
            if (it.depth() > w.depth) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().u8string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            const bool pdf = ext == ".pdf";
            if (!pdf && ext != ".png" && ext != ".jpg" && ext != ".jpeg") continue;
            xpt::DocEntry e;
            e.name = it->path().filename().u8string();
            e.path = it->path().u8string();
            e.folder = w.label;
            e.pdf = pdf;
            e.bytes = (long long)it->file_size(ec);
            d.files.push_back(e);
        }
    }
    logMsg("documents: %d files", (int)d.files.size());
}

void docsStep(const xpt::Actions& act) {
    if (act.docsList) listDocuments();
    if (!act.docOpen.empty() && act.docOpen != g_sim.docs.shownPath) {
        if (g_docThread.joinable()) g_docThread.join();
        g_sim.docs.shownPath = act.docOpen;
        g_sim.docs.busy = true;
        g_sim.docs.error.clear();
        const std::string path = act.docOpen;
        g_docThread = std::thread([path] {
            std::string bytes, err;
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!readFile(path, bytes)) err = "The file could not be read";
            else {
                int n = 0;
                unsigned char* p = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
                if (!p) err = "Not a picture XPTablet can show";
                else {
                    // at most 4096 across: halved until it fits
                    std::vector<uint8_t> img(p, p + (size_t)w * h * 4);
                    stbi_image_free(p);
                    while (w > 4096 || h > 4096) {
                        const int nw = w / 2, nh = h / 2;
                        std::vector<uint8_t> half((size_t)nw * nh * 4);
                        for (int y = 0; y < nh; ++y)
                            for (int x = 0; x < nw; ++x)
                                for (int c = 0; c < 4; ++c)
                                    half[((size_t)y * nw + x) * 4 + c] = (uint8_t)((img[((size_t)(2 * y) * w + 2 * x) * 4 + c] + img[((size_t)(2 * y) * w + 2 * x + 1) * 4 + c] +
                                                                                   img[((size_t)(2 * y + 1) * w + 2 * x) * 4 + c] + img[((size_t)(2 * y + 1) * w + 2 * x + 1) * 4 + c]) / 4);
                        img.swap(half); w = nw; h = nh;
                    }
                    px.swap(img);
                }
            }
            std::lock_guard<std::mutex> lk(g_docMx);
            g_docPath = path; g_docError = err; g_docPixels.swap(px); g_docW = w; g_docH = h; g_docReady = true;
        });
    }
    std::lock_guard<std::mutex> lk(g_docMx);
    if (!g_docReady) return;
    g_docReady = false;
    xpt::DocsState& d = g_sim.docs;
    if (g_docPath != d.shownPath) return;
    d.busy = false;
    d.error = g_docError;
    d.tex = 0;
    if (!g_docError.empty() || g_docPixels.empty()) return;
    if (!g_docTex) XPLMGenerateTextureNumbers(&g_docTex, 1);
    XPLMBindTexture2d(g_docTex, 0);
    // shown smaller than they are (a zoom between levels, a big picture fitted): mipmapped, made by the driver as they upload
        gl.TexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP */, 1);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, g_docW, g_docH, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_docPixels.data());
    XPLMBindTexture2d(0, 0);
    d.tex = (ImTextureID)(intptr_t)g_docTex;
    d.texW = g_docW; d.texH = g_docH;
    {   // light or dark: the average brightness over a sample of its pixels
        double sum = 0; size_t n = 0;
        for (size_t i = 0; i + 3 < g_docPixels.size(); i += 4 * 97) { sum += 0.30 * g_docPixels[i] + 0.59 * g_docPixels[i + 1] + 0.11 * g_docPixels[i + 2]; ++n; }
        d.light = n > 0 && sum / n > 150;
    }
    std::vector<uint8_t>().swap(g_docPixels);
}

// ---- the Checklists page: the aircraft's own checklist, if it has one ------------------------------------------------------------
// Output/XPTablet/Checklists/<ICAO>.txt, or xptablet_checklist.txt in the aircraft's folder ("# Section", then "Item | VALUE" lines).
void loadChecklist() {
    g_sim.checklistText.clear();
    g_sim.checklistSource.clear();
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    makeDirs(std::string(sys) + "Output/XPTablet/Checklists/");
    std::vector<std::pair<std::string, std::string>> cands;
    if (!g_sim.acfIcao.empty()) cands.push_back({std::string(sys) + "Output/XPTablet/Checklists/" + g_sim.acfIcao + ".txt", "Checklists/" + g_sim.acfIcao + ".txt"});
    const std::string acf = aircraftDir();
    if (!acf.empty()) cands.push_back({acf + "xptablet_checklist.txt", "the aircraft's xptablet_checklist.txt"});
    for (const auto& c : cands) if (readFile(c.first, g_sim.checklistText)) { g_sim.checklistSource = "From " + c.second; return; }
    // the Airbus types: the bundled checklists (FlightGear A320-family, GPL-2), the lines for this type and its engine
    static const char* const kAirbus[] = {"A319", "A320", "A20N", "A321", "A21N", "A339", "A333", "A346"};
    bool airbus = false;
    for (const char* t : kAirbus) airbus = airbus || g_sim.acfIcao == t;
    if (!airbus) return;
    char path[2048] = "";
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    std::string dir = path;
    for (int up = 0; up < 2; ++up) { const size_t cut = dir.find_last_of("/\\"); if (cut != std::string::npos) dir.resize(cut); }
    std::string text;
    if (!readFile(dir + "/checklists/airbus.txt", text)) return;
    // the engine: ToLiss's single-aisles say which (AirbusFBW/EngineTypeIndex: 0 IAE V2500, 3 LEAP-1A seen; 1 CFM56, 2 PW1100G taken
    // as the others); the A330-900 has Trent 7000s, the A340-600 Trent 500s
    std::string eng, engName;
    const std::string& ic = g_sim.acfIcao;
    if (ic == "A339") { eng = "TRENT7000"; engName = "Trent 7000"; }
    else if (ic == "A346") { eng = "TRENT500"; engName = "Trent 500"; }
    else if (XPLMDataRef r = XPLMFindDataRef("AirbusFBW/EngineTypeIndex")) {
        const int k = (XPLMGetDataRefTypes(r) & xplmType_Int) ? XPLMGetDatai(r) : (int)XPLMGetDataf(r);
        static const char* const ids[4] = {"V2500", "CFM56", "PW1100G", "LEAP"}, *names[4] = {"IAE V2500", "CFM56", "PW1100G", "LEAP-1A"};
        if (k >= 0 && k < 4) { eng = ids[k]; engName = names[k]; }
    }
    const std::string out = xpt::checklistFor(text, ic, eng);
    g_sim.checklistText = out;
    g_sim.checklistSource = "Airbus checklists (FlightGear A320-family, GPL-2)" + (engName.empty() ? std::string() : "  \xC2\xB7  " + engName);
}

// ---- Notes: kept in Output/preferences/XPTablet_notes.txt --------------------------------------------------------------------------
std::string notesPath() { const std::string s = settingsPath(); return s.empty() ? s : s.substr(0, s.size() - 4) + "_notes.txt"; }
void loadNotes() { std::string t; if (readFile(notesPath(), t)) xpt::notesFromText(t); }
void saveNotes() {
    FILE* f = std::fopen(notesPath().c_str(), "wb");
    if (!f) return;
    const std::string t = xpt::notesToText();
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
}

// One UI frame per sim frame, built by whichever of the two surfaces is drawn first and shared by the other.
// ---- media: what plays on the computer, its cover, and the radio's effect on it (Windows, Linux; not macOS) ------------------------------
int g_artTex = 0;
bool g_pttCmd = false;   // xPilot's push-to-talk command is held (or toggled on)
void mediaStep(const xpt::Actions& act) {
#if defined(__APPLE__)
    g_sim.mac = true;
    g_sim.media = xpt::MediaState();
    (void)act;
    return;
#else
    const xpt::Settings& st = xpt::settings();
    if (!st.mediaEnabled) { xpt::media::stop(); g_sim.media = xpt::MediaState(); g_sim.media.supported = true; return; }
    xpt::media::start();
    if (act.media) xpt::media::command(act.media);
    if (act.mediaSeek >= 0) xpt::media::seek(act.mediaSeek);
    if (act.mediaPlayerVol >= 0) xpt::media::setPlayerVolume(act.mediaPlayerVol);
    if (act.mediaMasterVol >= 0) xpt::media::setMasterVolume(act.mediaMasterVol);
    // the radios: xPilot says when COM1 or COM2 is receiving (looked for now and then: it may load after us)
    static XPLMDataRef com1 = nullptr, com2 = nullptr, rxOld = nullptr, ptt = nullptr;
    // X-Plane's own radios too: its ATC, ATIS and plugins' voices received on COM1 / COM2, and the pilot's voice sent
    static XPLMDataRef xpRx1 = XPLMFindDataRef("sim/atc/com1_rx"), xpRx2 = XPLMFindDataRef("sim/atc/com2_rx");
    static XPLMDataRef xpTx1 = XPLMFindDataRef("sim/atc/com1_tx"), xpTx2 = XPLMFindDataRef("sim/atc/com2_tx");
    if ((XPLMGetCycleNumber() & 255) == 1 && ((!com1 && !rxOld) || !ptt)) {
        com1 = XPLMFindDataRef("xpilot/audio/com1_rx");
        com2 = XPLMFindDataRef("xpilot/audio/com2_rx");
        rxOld = XPLMFindDataRef("xpilot/audio/rx");
        ptt = XPLMFindDataRef("xpilot/ptt");   // 1 while push-to-talk is held
    }
    auto on = [](XPLMDataRef r) {
        if (!r) return false;
        return (XPLMGetDataRefTypes(r) & xplmType_Int) ? XPLMGetDatai(r) != 0 : XPLMGetDataf(r) > 0.5f;
    };
    const bool pilotRx = on(com1) || on(com2) || on(rxOld), xpRx = on(xpRx1) || on(xpRx2);
    // xPilot's push-to-talk: its dataref, and also its command itself (a key or button bound to it: some versions of xPilot do not
    // set the dataref then)
    static bool pttWatched = false;
    if (!pttWatched && (XPLMGetCycleNumber() & 255) == 1) {
        for (const char* name : {"xpilot/ptt", "xpilot/toggle_ptt"}) {
            if (XPLMCommandRef c = XPLMFindCommand(name)) {
                XPLMRegisterCommandHandler(c, [](XPLMCommandRef, XPLMCommandPhase phase, void* toggle) -> int {
                    if (toggle) { if (phase == xplm_CommandBegin) g_pttCmd = !g_pttCmd; }
                    else g_pttCmd = phase != xplm_CommandEnd;
                    return 1;   // (xPilot still gets it)
                }, 1, (void*)(intptr_t)(std::strcmp(name, "xpilot/toggle_ptt") == 0));
                pttWatched = true;
            }
        }
    }
    const bool pilotTx = on(ptt) || g_pttCmd;
    const bool rx = pilotRx || xpRx, tx = pilotTx || on(xpTx1) || on(xpTx2);
    const char* who = pilotRx || pilotTx ? "xPilot" : "X-Plane ATC";
    // (a tablet that is off or flat leaves the media alone)
    xpt::media::setDuck(xpt::tabletPoweredOn() ? st.mediaDuck : 0, rx || tx, rx || tx ? who : "", tx ? st.duckTx : st.duckRx);   // transmitting: its own level
    const xpt::media::Now n = xpt::media::now();
    xpt::MediaState& m = g_sim.media;
    m.supported = n.supported; m.active = n.active; m.app = n.app; m.title = n.title; m.artist = n.artist; m.album = n.album;
    m.playing = n.playing; m.canPrev = n.canPrev; m.canNext = n.canNext;
    m.posSec = n.posSec; m.lenSec = n.lenSec; m.canSeek = n.canSeek;
    m.radioRx = rx || n.radioApp;
    m.radioTx = tx;
    m.radioSource = rx ? who : n.radioApp ? n.radioAppName : "";
    m.ducked = n.ducked;
    m.playerVolume = n.playerVolume; m.masterVolume = n.masterVolume;
    // the cover, into a texture of its own (mipmapped: it is shown small and large)
    static int version = 0;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (xpt::media::takeArt(version, px, w, h)) {
        // the new cover goes into the other of two textures, so the old one can fade out under it (Media page and pull-down)
        static int artTex2 = 0;
        if (m.art && m.artW > 0) { m.artPrev = m.art; m.artPrevW = m.artW; m.artPrevH = m.artH; }
        else { m.artPrev = 0; m.artPrevW = m.artPrevH = 0; }
        m.artChangedAt = ImGui::GetTime();
        if (g_artTex && loadGl()) { if (!artTex2) XPLMGenerateTextureNumbers(&artTex2, 1); std::swap(g_artTex, artTex2); }
        m.artColor = 0;
        if (w > 0 && h > 0) {   // its colour: the average of its more colourful pixels, made deeper (white text stays clear on it)
            double r = 0, g = 0, b = 0, wsum = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4 * 7) {
                const double pr = px[i], pg = px[i + 1], pb = px[i + 2];
                const double mx = std::max(pr, std::max(pg, pb)), mn = std::min(pr, std::min(pg, pb));
                const double wgt = 0.15 + (mx - mn) / 255.0;   // (colourful pixels count more than greys)
                r += pr * wgt; g += pg * wgt; b += pb * wgt; wsum += wgt;
            }
            if (wsum > 0) {
                r /= wsum; g /= wsum; b /= wsum;
                const double mx = std::max(1.0, std::max(r, std::max(g, b))), k = std::min(1.0, 150.0 / mx);
                m.artColor = IM_COL32((int)(r * k), (int)(g * k), (int)(b * k), 255);
            }
        }
        if (w > 0 && h > 0 && loadGl()) {
            if (!g_artTex) XPLMGenerateTextureNumbers(&g_artTex, 1);
            XPLMBindTexture2d(g_artTex, 0);
            gl.TexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP */, 1);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
            gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            XPLMBindTexture2d(0, 0);
            m.artW = w; m.artH = h;
        } else {
            m.artW = m.artH = 0;
        }
    }
    m.art = (n.active && g_artTex && m.artW > 0) ? (ImTextureID)(intptr_t)g_artTex : 0;
#endif
}

void ensureFrame() {
    const int cycle = XPLMGetCycleNumber();
    if (cycle == g_frameCycle) return;
    g_frameCycle = cycle;
    ImGui::SetCurrentContext(g_ctx);
    const bool winShown = g_win && XPLMGetWindowIsVisible(g_win);
    float ww = 800, wh = 480;
    if (winShown) windowLayout(ww, wh);
    float aspect;
    if (panelActive()) aspect = kPanelAspect;   // AviTab's shape, whatever the rectangle's
    else aspect = ww / wh;
    g_logW = xpt::kLogicalH * std::max(0.6f, std::min(2.6f, aspect));
    if (winShown) {
        g_winScale = std::min(ww / g_logW, wh / xpt::kLogicalH);
        g_winOx = (ww - g_logW * g_winScale) * 0.5f;
        g_winOy = (wh - xpt::kLogicalH * g_winScale) * 0.5f;
    }
    // pixels per layout unit, for crisp text: the larger of the two surfaces
    float need = 1.0f;
    if (panelActive()) { float pl, pb, pw, ph; panelPicture(pl, pb, pw, ph); need = std::max(need, ph / xpt::kLogicalH); }
    if (winShown) need = std::max(need, g_winScale * (g_drUiScale ? std::max(1.0f, XPLMGetDataf(g_drUiScale)) : 1.0f));
    need = std::max(1.0f, std::min(3.0f, need));
    if (g_fontScale <= 0 || std::fabs(need - g_fontScale) > 0.08f * g_fontScale) buildFonts(need);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_logW, xpt::kLogicalH);
    const auto now = std::chrono::steady_clock::now();
    const float dt = g_lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - g_lastFrame).count() : 1.0f / 60.0f;
    io.DeltaTime = std::max(1e-4f, std::min(0.5f, dt));
    g_lastFrame = now;

    // the pointer: over the floating window (its cursor callback reports that), else on the cockpit tablet, else nowhere
    if (!g_captureDown && cycle - g_winHoverCycle > 1) {
        float px = 0, py = 0;
        const bool onPanel = panelPointer(px, py);
        if (onPanel) io.AddMousePosEvent(px, py);
        if (!onPanel && !ImGui::IsMouseDown(0)) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    g_sim.onPanel = panelActive();
    g_sim.panel[0] = g_panelLeft; g_sim.panel[1] = g_panelBottom; g_sim.panel[2] = g_panelWidth; g_sim.panel[3] = g_panelHeight;
    g_sim.panelEnabled = g_panelEnabled != 0; g_sim.panelPowered = g_panelPowered != 0;
    g_sim.brightness = g_brightness;
    g_sim.aviTabRunning = g_aviTabRunning;
    g_sim.pixelScale = g_fontScale;

    ImGui::NewFrame();
    xpt::Actions act;
    xpt::build(g_fonts, g_sim, g_logW, xpt::kLogicalH, act);
    ImGui::Render();
    xpt::clipDrawData(ImGui::GetDrawData(), g_batches);
    if (!act.metarSearch.empty()) { xpt::metarRequest(act.metarSearch); g_sim.metar = xpt::MetarLookup(); g_sim.metar.icao = act.metarSearch; g_sim.metar.busy = true; }
    if (act.ground != xpt::GA_NONE) g_pendingGround = act;   // carried out from the flight loop, not from inside drawing
    browserStep(act);
    mapStep(act);
    planStep(act);
    mediaStep(act);
    {   // the Map's traffic card: its aircraft on VATSIM (and its filed route); VATSIM's world as traffic when chosen
        const bool world = act.mapShown && xpt::settings().mapTraffic && xpt::settings().mapTrafficSource == 1;
        xpt::vatsim::want((act.mapShown && !act.trafficSelected.empty()) || world);
        if ((act.mapShown && !act.trafficSelected.empty()) || world) xpt::vatsim::start();
        if (act.mapShown && !act.trafficSelected.empty()) {
            g_sim.map.vatsim = xpt::vatsim::lookup(act.trafficSelected, g_sim.map.vatsimStatus);
            static std::string routeKey;
            const xpt::VatsimPilot& v = g_sim.map.vatsim;
            const std::string key = v.found && v.hasPlan && xpt::settings().mapTrafficRoute ? v.callsign + "|" + v.departure + "|" + v.route + "|" + v.arrival : std::string();
            if (key != routeKey) { routeKey = key; g_sim.map.trafficRoute = key.empty() ? std::vector<xpt::MapState::RoutePoint>() : resolveRoute(v); }
        } else g_sim.map.trafficRoute.clear();
        static double lastWorld = -100;
        if (!world) g_vatsimTraffic.clear();
        else if (XPLMGetElapsedTime() - lastWorld > 3.0) {
            lastWorld = XPLMGetElapsedTime();
            g_vatsimTraffic.clear();
            for (const xpt::VatsimPilot& p : xpt::vatsim::allPilots()) {
                if (p.lat == 0 && p.lon == 0) continue;
                xpt::MapState::Traffic t;
                t.lat = p.lat; t.lon = p.lon; t.altFt = (float)p.altitudeFt; t.hdg = (float)p.heading; t.gsKt = (float)p.groundspeedKt;
                t.onGround = p.groundspeedKt < 40 && p.altitudeFt < 15000;   // (VATSIM gives no ground flag: slow is taken as on the ground)
                t.callsign = p.callsign; t.type = p.aircraft;
                g_vatsimTraffic.push_back(t);
            }
        }
    }
    if (act.clearTileCache) clearTileCache();
    if (act.resetBrowserData) resetBrowserData();
    if ((XPLMGetCycleNumber() & 15) == 0) dataInfoStep();
    airportsStep(act);
    docsStep(act);
    g_webShown = act.webShown;
    // the floating window keeps the keyboard only while a web page or a text field is on it
    g_wantKeys = g_webShown || xpt::tabletTyping();
    if (g_wantKeys && g_win && XPLMGetWindowIsVisible(g_win) && cycle - g_winHoverCycle < 2 && !XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(g_win);
    if (!g_wantKeys && g_win && XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(nullptr);
}

// Draws the frame with its top-left corner at (left, top) and sx/sy screen units per layout unit (y up on screen).
void drawBatches(const std::vector<xpt::Batch>& batches, float left, float top, float sx, float sy) {
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const bool cull = gl.IsEnabled(GL_CULL_FACE) != 0;
    gl.Disable(GL_CULL_FACE);
    gl.PushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.EnableClientState(GL_COLOR_ARRAY);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.Translatef(left, top, 0.0f);
    gl.Scalef(sx, -sy, 1.0f);   // ImGui's y runs down
    gl.Color4f(1, 1, 1, 1);
    for (const xpt::Batch& b : batches) {
        if (b.v.empty()) continue;
        XPLMBindTexture2d((int)(intptr_t)b.tex, 0);
        const ImDrawVert* v = b.v.data();
        gl.VertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, pos)));
        gl.TexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, uv)));
        gl.ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, col)));
        gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)b.v.size());
    }
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.Color4f(1, 1, 1, 1);
    XPLMBindTexture2d(0, 0);
    if (cull) gl.Enable(GL_CULL_FACE);
}

// The Weather page with WXPlane installed: WXPlane draws its own screen into the page's area. It is asked from inside our drawing,
// with the matrix set so the area's top-left corner is the origin, y runs down and one unit is one pixel of this surface
// (wxplane_embed.h). left/top: where the tablet's own top-left corner is; sx/sy: surface units per layout unit.
void drawEmbedded(float left, float top, float sx, float sy, float pixelsPerUnit, float brightness) {
    const xpt::EmbedArea& e = xpt::embedArea();
    if (!e.active) return;
    const XPLMPluginID wx = XPLMFindPluginBySignature(kWxPlaneSignature);
    if (wx == XPLM_NO_PLUGIN_ID || !XPLMIsPluginEnabled(wx)) { g_sim.wxPlane = false; return; }
    WxEmbedRequest rq;
    std::memset(&rq, 0, sizeof rq);
    rq.structSize = (int)sizeof rq;
    rq.version = 1;
    rq.widthPx = e.w * sx; rq.heightPx = e.h * sy;
    rq.pixelsPerUnit = pixelsPerUnit;
    rq.mouseX = e.mx < -1e8f ? -1e9f : e.mx * sx;
    rq.mouseY = e.my < -1e8f ? -1e9f : e.my * sy;
    rq.mouseDown = e.down ? 1 : 0;
    rq.wheel = e.wheel;
    rq.brightness = brightness;
    rq.frame = XPLMGetCycleNumber();
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.Translatef(left + e.x * sx, top - e.y * sy, 0.0f);
    gl.Scalef(1.0f, -1.0f, 1.0f);
    XPLMSendMessageToPlugin(wx, WXPLANE_MSG_EMBED, &rq);
    gl.PopMatrix();
    g_sim.wxEmbed = rq.handled ? 1 : -1;   // an older WXPlane does not know the message
}

void fillRect(float l, float b, float r, float t, float red, float green, float blue) {
    XPLMSetGraphicsState(0, 0, 0, 0, 0, 0, 0);
    const float v[12] = {l, b, r, b, r, t, l, b, r, t, l, t};
    gl.PushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.Color4f(red, green, blue, 1);
    gl.VertexPointer(2, GL_FLOAT, 0, v);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    gl.PopClientAttrib();
    gl.Color4f(1, 1, 1, 1);
}

// The Settings brightness, or with auto dim WXPlane's curve: full with the sun up, down to 45% once it is 8 degrees below the horizon
float autoDimLevel() {
    static XPLMDataRef sun = XPLMFindDataRef("sim/graphics/scenery/sun_pitch_degrees");
    if (!sun) return 1.0f;
    const float t = std::max(0.0f, std::min(1.0f, -XPLMGetDataf(sun) / 8.0f));
    return 1.0f - 0.55f * t;
}
float userBrightness() {
    g_sim.autoDimLevel = autoDimLevel();
    if (xpt::settings().autoDim) return g_sim.autoDimLevel;
    return std::max(0.1f, std::min(1.0f, xpt::settings().brightness));
}

// ---- the cockpit tablet ---------------------------------------------------------------------------------------------------------------
// Runs while X-Plane draws the 3-D cockpit's panel texture: the coordinates are that texture's pixels, origin bottom-left.
int drawPanel(XPLMDrawingPhase, int, void*) {
    if (!panelActive() || !g_ctx || !loadGl()) return 1;
    float l, b, w, h;
    panelPicture(l, b, w, h);
    if (!g_panelPowered) { fillRect(l, b, l + w, b + h, 0, 0, 0); return 1; }
    ensureFrame();
    const float bright = g_brightness * userBrightness();   // the aircraft's knob times the Settings slider
    if (bright < 0.999f) {
        std::vector<xpt::Batch> dim = g_batches;
        xpt::scaleColors(dim, bright);
        drawBatches(dim, l, b + h, w / g_logW, h / xpt::kLogicalH);
    } else {
        drawBatches(g_batches, l, b + h, w / g_logW, h / xpt::kLogicalH);
    }
    drawEmbedded(l, b + h, w / g_logW, h / xpt::kLogicalH, 1.0f, bright);
    return 1;
}

// A press, drag or release on the cockpit tablet. Returns true when the tablet took it.
bool panelClick(XPLMMouseStatus status) {
    if (!g_ctx) return false;
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    float x = 0, y = 0;
    const bool on = panelPointer(x, y);
    if (status == xplm_MouseDown) {
        if (!on) return false;
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
        g_captureDown = true;
        return true;
    }
    if (!g_captureDown) return false;
    if (on) io.AddMousePosEvent(x, y);   // dragged off the tablet: the pointer stays where it left
    if (status == xplm_MouseUp) { io.AddMouseButtonEvent(0, false); g_captureDown = false; }
    return true;
}

void captureDraw(XPLMWindowID, void*) {}
int captureClick(XPLMWindowID, int, int, XPLMMouseStatus status, void*) { return (g_captureClicks && panelClick(status)) ? 1 : 0; }
int capturePass(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; }
XPLMCursorStatus captureCursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
int captureWheel(XPLMWindowID, int, int, int wheel, int clicks, void*) {
    float x, y;
    if (!g_captureClicks || wheel != 0 || !g_ctx || !panelPointer(x, y)) return 0;
    ImGui::SetCurrentContext(g_ctx);
    ImGui::GetIO().AddMousePosEvent(x, y);
    ImGui::GetIO().AddMouseWheelEvent(0, (float)clicks);
    return 1;
}
void noKey(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
// The floating window's keyboard, while a web page is shown: into the address bar while it is being typed in, else to the page.
// XPLM's virtual key codes for the editing keys are Windows' (back 8, tab 9, enter 13, escape 27, page/home/end/arrows 33-40,
// delete 46).
// Settings, Display, "Physical keyboard": while a text box takes typing, the computer's keys go to it wherever X-Plane's focus is
// (the cockpit tablet too); everything else (Escape, the arrows, X-Plane's own keys when nothing is typed in) goes on as usual
int keySniffer(char key, XPLMKeyFlags flags, char vkey, void*) {
    if (!xpt::settings().physicalKeys) return 1;
    // a web page's text field (a site's search box) with the computer's keyboard: the keys go to the page, not to X-Plane
    if (xpt::webTyping() && g_webShown && xpt::browser::started() && !xpt::tabletTyping()) {
        namespace br = xpt::browser;
        const unsigned char vk = (unsigned char)vkey, ch = (unsigned char)key;
        const bool down = (flags & xplm_DownFlag) != 0, up = (flags & xplm_UpFlag) != 0;
        if (flags & (xplm_ControlFlag | xplm_OptionAltFlag)) return 1;   // (X-Plane's own shortcuts stay X-Plane's)
        const bool editKey = vk == 8 || vk == 9 || vk == 13 || vk == 27 || (vk >= 33 && vk <= 40) || vk == 46;
        if (editKey) {
            if (down || up) br::push(xptb::CMD_KEY, 0, 0, vk, down ? 1 : 0);
            if (down && vk == 13) br::push(xptb::CMD_CHAR, 0, 0, 13);
            return 0;
        }
        if (ch >= 32 && ch < 127) { if (down) br::push(xptb::CMD_CHAR, 0, 0, ch); return 0; }
        return 1;
    }
    if (!xpt::tabletTyping()) return 1;
    const unsigned char vk = (unsigned char)vkey, ch = (unsigned char)key;
    const bool down = (flags & xplm_DownFlag) != 0;
    if (vk == 8 || vk == 13) { if (down) xpt::tabletKey(vk, false); return 0; }
    if (ch >= 32 && ch < 127 && !(flags & (xplm_ControlFlag | xplm_OptionAltFlag))) { if (down) xpt::tabletKey(ch, true); return 0; }
    return 1;
}
void winKey(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey, void*, int losingFocus) {
    if (losingFocus) return;
    const unsigned char vk = (unsigned char)vkey, ch = (unsigned char)key;
    const bool down = (flags & xplm_DownFlag) != 0, up = (flags & xplm_UpFlag) != 0;
    const bool editKey = vk == 8 || vk == 9 || vk == 13 || vk == 27 || (vk >= 33 && vk <= 40) || vk == 46;
    if (xpt::tabletTyping()) {   // the address bar, a note, a search
        if (!down) return;
        if (vk == 8 || vk == 13) xpt::tabletKey(vk, false);
        else if (ch >= 32 && ch < 127) xpt::tabletKey(ch, true);
        return;
    }
    if (!g_webShown) return;
    namespace br = xpt::browser;
    if (!br::started()) return;
    if (editKey) {
        if (down || up) br::push(xptb::CMD_KEY, 0, 0, vk, down ? 1 : 0);
        if (down && vk == 13) br::push(xptb::CMD_CHAR, 0, 0, 13);
    } else if (down && ch >= 32) {
        br::push(xptb::CMD_CHAR, 0, 0, ch);
    }
}

// The click-catching window covers the whole screen, so it is only there while the mouse points at the cockpit tablet (or a press
// that began on it is still held). Left up all the time it sat over X-Plane's own pop-up messages, whose buttons then could not
// be pressed (2026-11-17: the "Understood" button of a scenery warning).
void updateCaptureWindow() {
    float px = 0, py = 0;
    const bool want = panelActive() && g_captureClicks && (g_captureDown || panelPointer(px, py));
    if (want && !g_capture) {
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.visible = 1;
        p.drawWindowFunc = captureDraw;
        p.handleMouseClickFunc = captureClick;
        p.handleRightClickFunc = capturePass;
        p.handleMouseWheelFunc = captureWheel;
        p.handleKeyFunc = noKey;
        p.handleCursorFunc = captureCursor;
        p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
        p.layer = xplm_WindowLayerFlightOverlay;
        g_capture = XPLMCreateWindowEx(&p);
    }
    if (g_capture) {
        XPLMSetWindowIsVisible(g_capture, want ? 1 : 0);
        if (want) {
            int l = 0, t = 0, r = 0, b = 0;
            XPLMGetScreenBoundsGlobal(&l, &t, &r, &b);
            XPLMSetWindowGeometry(g_capture, l, t, r, b);
        }
    }
}

// ---- the floating window -------------------------------------------------------------------------------------------------------------
void winToLayout(int x, int y, float& lx, float& ly) {
    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(g_win, &l, &t, &r, &b);
    lx = ((float)(x - l) - g_winOx) / g_winScale;
    ly = ((float)(t - y) - g_winOy) / g_winScale;
}

void winDraw(XPLMWindowID w, void*) {
    if (!g_ctx || !loadGl()) return;
    ensureFrame();
    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(w, &l, &t, &r, &b);
    fillRect((float)l, (float)b, (float)r, (float)t, 0, 0, 0);
    const float bright = userBrightness();   // the window has no aircraft knob: the Settings slider alone
    if (bright < 0.999f) {
        std::vector<xpt::Batch> dim = g_batches;
        xpt::scaleColors(dim, bright);
        drawBatches(dim, (float)l + g_winOx, (float)t - g_winOy, g_winScale, g_winScale);
    } else {
        drawBatches(g_batches, (float)l + g_winOx, (float)t - g_winOy, g_winScale, g_winScale);
    }
    drawEmbedded((float)l + g_winOx, (float)t - g_winOy, g_winScale, g_winScale, g_drUiScale ? std::max(1.0f, XPLMGetDataf(g_drUiScale)) : 1.0f, bright);
}
int winClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void*) {
    if (!g_ctx) return 1;
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    float lx, ly;
    winToLayout(x, y, lx, ly);
    io.AddMousePosEvent(lx, ly);
    if (status == xplm_MouseDown) io.AddMouseButtonEvent(0, true);
    else if (status == xplm_MouseUp) io.AddMouseButtonEvent(0, false);
    if (status == xplm_MouseDown && g_wantKeys && g_win && !XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(g_win);
    g_winHoverCycle = XPLMGetCycleNumber();
    return 1;
}
int winRightClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }
int winWheel(XPLMWindowID, int x, int y, int wheel, int clicks, void*) {
    if (!g_ctx) return 1;
    ImGui::SetCurrentContext(g_ctx);
    float lx, ly;
    winToLayout(x, y, lx, ly);
    ImGui::GetIO().AddMousePosEvent(lx, ly);
    if (wheel == 0) ImGui::GetIO().AddMouseWheelEvent(0, (float)clicks); else ImGui::GetIO().AddMouseWheelEvent((float)clicks, 0);
    return 1;
}
XPLMCursorStatus winCursor(XPLMWindowID, int x, int y, void*) {
    if (g_ctx && !g_captureDown) {
        ImGui::SetCurrentContext(g_ctx);
        float lx, ly;
        winToLayout(x, y, lx, ly);
        ImGui::GetIO().AddMousePosEvent(lx, ly);
        g_winHoverCycle = XPLMGetCycleNumber();
    }
    return xplm_CursorDefault;
}

void placeWindowForVr() {
    if (!g_win) return;
    const bool vr = g_drVr && XPLMGetDatai(g_drVr) != 0;
    XPLMSetWindowPositioningMode(g_win, vr ? xplm_WindowVR : xplm_WindowPositionFree, -1);
}

void toggleWindow() {
    if (!g_win) {
        int sl = 0, st = 0, sr = 0, sb = 0;
        XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.left = sl + 80; p.bottom = sb + 120; p.right = p.left + 800; p.top = p.bottom + 480;
        p.visible = 1;
        p.drawWindowFunc = winDraw;
        p.handleMouseClickFunc = winClick;
        p.handleRightClickFunc = winRightClick;
        p.handleMouseWheelFunc = winWheel;
        p.handleKeyFunc = winKey;
        p.handleCursorFunc = winCursor;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        p.layer = xplm_WindowLayerFloatingWindows;
        g_win = XPLMCreateWindowEx(&p);
        XPLMSetWindowTitle(g_win, "XPTablet");
        XPLMSetWindowResizingLimits(g_win, 400, 240, 3200, 1920);
        placeWindowForVr();
        return;
    }
    const int show = XPLMGetWindowIsVisible(g_win) ? 0 : 1;
    XPLMSetWindowIsVisible(g_win, show);
    if (show) { placeWindowForVr(); XPLMBringWindowToFront(g_win); }
}

// ---- datarefs and commands -------------------------------------------------------------------------------------------------------------
int getInt(void* r) { return *(int*)r; }
void setInt(void* r, int v) { *(int*)r = v; }
float getFloat(void* r) { return *(float*)r; }
void setBrightness(void*, float v) { g_brightness = std::max(0.0f, std::min(1.0f, v)); }
int getOwnsAviTab(void*) { return g_ownsAviTab ? 1 : 0; }
int getInMenu(void*) { return xpt::currentApp() == xpt::APP_HOME ? 1 : 0; }
// AviTab's map datarefs: where the Map page looks
float getMapLat(void*) { double la, lo; int z; float r; xpt::mapView(la, lo, z, r); return (float)la; }
float getMapLon(void*) { double la, lo; int z; float r; xpt::mapView(la, lo, z, r); return (float)lo; }
int getMapZoom(void*) { double la, lo; int z; float r; xpt::mapView(la, lo, z, r); return z; }
float getMapRange(void*) { double la, lo; int z; float r; xpt::mapView(la, lo, z, r); return r; }

void regInt(const std::string& name, int* v, bool writable) {
    g_ownedRefs.push_back(XPLMRegisterDataAccessor(name.c_str(), xplmType_Int, writable ? 1 : 0, getInt, writable ? setInt : nullptr, nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, v, v));
}
void regIntFn(const std::string& name, int (*fn)(void*)) {
    g_ownedRefs.push_back(XPLMRegisterDataAccessor(name.c_str(), xplmType_Int, 0, fn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, nullptr));
}
void regFloat(const std::string& name, float (*rd)(void*), void (*wr)(void*, float), void* ref) {
    g_ownedRefs.push_back(XPLMRegisterDataAccessor(name.c_str(), xplmType_Float, wr ? 1 : 0, nullptr, nullptr, rd, wr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, ref, ref));
}

// The interface aircraft use to place and switch the tablet. prefix "avitab" (when AviTab is not there) and always "xptablet".
void registerDatarefs(const std::string& prefix) {
    regInt(prefix + "/panel_left", &g_panelLeft, true);
    regInt(prefix + "/panel_bottom", &g_panelBottom, true);
    regInt(prefix + "/panel_width", &g_panelWidth, true);
    regInt(prefix + "/panel_height", &g_panelHeight, true);
    regInt(prefix + "/panel_enabled", &g_panelEnabled, true);
    regInt(prefix + "/panel_powered", &g_panelPowered, true);
    regFloat(prefix + "/brightness", getFloat, setBrightness, &g_brightness);
    regIntFn(prefix + "/is_in_menu", getInMenu);
    // 1 when XPTablet provides AviTab's interface (AviTab is not installed). WXPlane reads it: it then leaves the tablet's screen
    // alone and shows on the Weather page, instead of drawing over the screen as it does on AviTab.
    if (prefix == "xptablet") regIntFn(prefix + "/owns_avitab", getOwnsAviTab);
    // the map app's position: filled in when the map exists
    regFloat(prefix + "/map/latitude", getMapLat, nullptr, nullptr);
    regFloat(prefix + "/map/longitude", getMapLon, nullptr, nullptr);
    regIntFn(prefix + "/map/zoom", getMapZoom);
    regFloat(prefix + "/map/vertical_range", getMapRange, nullptr, nullptr);
}

enum { CMD_TOGGLE, CMD_HOME, CMD_CLICK, CMD_WHEEL_UP, CMD_WHEEL_DOWN, CMD_NOP, CMD_PLANE_DOCS, CMD_MAP0 = 50, CMD_APP0 = 100 };

int onCommand(XPLMCommandRef, XPLMCommandPhase phase, void* ref) {
    const int id = (int)(intptr_t)ref;
    if (id == CMD_CLICK) {   // the aircraft's own tablet hotspot: a press while held, at the place the mouse points on the panel
        if (phase == xplm_CommandBegin) panelClick(xplm_MouseDown);
        else if (phase == xplm_CommandContinue) panelClick(xplm_MouseDrag);
        else panelClick(xplm_MouseUp);
        return 1;
    }
    if (phase != xplm_CommandBegin) return 1;
    if (id == CMD_TOGGLE) toggleWindow();
    else if (id == CMD_HOME) xpt::openApp(xpt::APP_HOME);
    else if (id == CMD_WHEEL_UP || id == CMD_WHEEL_DOWN) {
        float x, y;
        if (g_ctx && panelPointer(x, y)) {
            ImGui::SetCurrentContext(g_ctx);
            ImGui::GetIO().AddMousePosEvent(x, y);
            ImGui::GetIO().AddMouseWheelEvent(0, id == CMD_WHEEL_UP ? 1.0f : -1.0f);
        }
    } else if (id >= CMD_APP0) xpt::openApp(id - CMD_APP0);
    else if (id >= CMD_MAP0) xpt::mapCommand(id - CMD_MAP0);
    else if (id == CMD_PLANE_DOCS) { xpt::openDocuments("This aircraft"); g_sim.docs.listed = false; }
    return 1;
}

void regCmd(const std::string& name, const char* desc, int id) {
    XPLMCommandRef c = XPLMCreateCommand(name.c_str(), desc);
    XPLMRegisterCommandHandler(c, onCommand, 1, (void*)(intptr_t)id);
    g_cmds.push_back({c, id});
}

void registerCommands(const std::string& p) {
    regCmd(p + "/toggle_tablet", "Show or hide the tablet window", CMD_TOGGLE);
    regCmd(p + "/Home", "Tablet: home screen", CMD_HOME);
    regCmd(p + "/click_left", "Tablet: press at the mouse position on the cockpit tablet", CMD_CLICK);
    regCmd(p + "/wheel_up", "Tablet: scroll up", CMD_WHEEL_UP);
    regCmd(p + "/wheel_down", "Tablet: scroll down", CMD_WHEEL_DOWN);
    regCmd(p + "/app_charts", "Tablet: charts", CMD_APP0 + xpt::APP_CHARTS);
    regCmd(p + "/app_airports", "Tablet: airports", CMD_APP0 + xpt::APP_AIRPORTS);
    regCmd(p + "/app_routes", "Tablet: Flight Plan (routes)", CMD_APP0 + xpt::APP_ROUTES);
    regCmd(p + "/app_maps", "Tablet: map", CMD_APP0 + xpt::APP_MAP);
    regCmd(p + "/app_plane_manual", "Tablet: aircraft documents", CMD_PLANE_DOCS);
    regCmd(p + "/app_notes", "Tablet: notes", CMD_APP0 + xpt::APP_NOTES);
    regCmd(p + "/app_navigraph", "Tablet: Navigraph Charts", CMD_APP0 + xpt::APP_NAVIGRAPH);
    regCmd(p + "/app_about", "Tablet: settings", CMD_APP0 + xpt::APP_SETTINGS);
    // the map's own controls: accepted now so aircraft buttons bound to them do nothing rather than fail, wired up with the map
    {   // the Map page, as AviTab's map: zoom in, out, back on the aircraft, and moved a quarter of the way
        const char* names[7] = {"zoom_in", "zoom_out", "recentre", "pan_left", "pan_right", "pan_up", "pan_down"};
        for (int i = 0; i < 7; ++i) regCmd(p + "/" + names[i], "Tablet: map control", CMD_MAP0 + i);
    }
}

void menuHandler(void*, void*) { toggleWindow(); }

void findSimRefs() {
    SimRefs& r = g_refs;
    if (!r.found) {
        r.found = true;
        r.windDir = XPLMFindDataRef("sim/weather/aircraft/wind_now_direction_degt");
        r.windMs = XPLMFindDataRef("sim/weather/aircraft/wind_now_speed_msc");
        r.vis = XPLMFindDataRef("sim/weather/aircraft/visibility_reported_sm");
        r.temp = XPLMFindDataRef("sim/weather/aircraft/temperature_ambient_deg_c");
        r.dew = XPLMFindDataRef("sim/weather/aircraft/dewpoint_deg_c");
        r.qnh = XPLMFindDataRef("sim/weather/aircraft/qnh_pas");
        r.rain = XPLMFindDataRef("sim/weather/aircraft/precipitation_on_aircraft_ratio");
        r.alt = XPLMFindDataRef("sim/flightmodel/position/elevation");
        r.cBase = XPLMFindDataRef("sim/weather/aircraft/cloud_base_msl_m");
        r.cTop = XPLMFindDataRef("sim/weather/aircraft/cloud_tops_msl_m");
        r.cCover = XPLMFindDataRef("sim/weather/aircraft/cloud_coverage_percent");
        r.aAlt = XPLMFindDataRef("sim/weather/aircraft/wind_altitude_msl_m");
        r.aDir = XPLMFindDataRef("sim/weather/aircraft/wind_direction_degt");
        r.aKt = XPLMFindDataRef("sim/weather/aircraft/wind_speed_kts");
        r.onGround = XPLMFindDataRef("sim/flightmodel/failures/onground_any");
        r.beacon = XPLMFindDataRef("sim/cockpit2/switches/beacon_on");
        r.engRun = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");
        r.parkBrake = XPLMFindDataRef("sim/cockpit2/controls/parking_brake_ratio");
        r.isChocked = XPLMFindDataRef("sim/flightmodel2/gear/is_chocked");
        r.gs = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    }
    // other plugins' datarefs appear when they load (an aircraft's own plugin after the aircraft): looked for until found
    if (!r.jwStatus) r.jwStatus = XPLMFindDataRef("opensam/jetway/status");
    if (!r.jwDoorStatus) r.jwDoorStatus = XPLMFindDataRef("opensam/jetway/door/status");
    if (!r.jwNumber) r.jwNumber = XPLMFindDataRef("opensam/jetway/number");
    if (!r.tlParkBrake) r.tlParkBrake = XPLMFindDataRef("AirbusFBW/ParkBrake");
    if (!r.slides) r.slides = XPLMFindDataRef("AirbusFBW/SlideArmedArray");
    if (!r.cabinPerc) r.cabinPerc = XPLMFindDataRef("AirbusFBW/cabinLightLevel_perc");
    if (!r.cabinLevel) r.cabinLevel = XPLMFindDataRef("AirbusFBW/CabinLightLevel");
    if (!r.exitConfig) r.exitConfig = XPLMFindDataRef("AirbusFBW/A321ExitConfig");
    if (!r.paxDoor) r.paxDoor = XPLMFindDataRef("AirbusFBW/PaxDoorModeArray");
    if (!r.cargoDoor) r.cargoDoor = XPLMFindDataRef("AirbusFBW/CargoDoorModeArray");
    if (!r.chocks) r.chocks = XPLMFindDataRef("AirbusFBW/Chocks");
    if (!r.extPower) r.extPower = XPLMFindDataRef("AirbusFBW/EnableExternalPower");
    if (!r.hpAir) r.hpAir = XPLMFindDataRef("AirbusFBW/GroundHPAir");
    if (!r.lpAir) r.lpAir = XPLMFindDataRef("AirbusFBW/GroundLPAir");
}

// A number from a dataref of whichever kind it is (int, float or double; the first value of an array).
float readNum(XPLMDataRef d) {
    const int t = XPLMGetDataRefTypes(d);
    if (t & xplmType_Int) return (float)XPLMGetDatai(d);
    if (t & xplmType_Float) return XPLMGetDataf(d);
    if (t & xplmType_Double) return (float)XPLMGetDatad(d);
    if (t & xplmType_IntArray) { int v = 0; XPLMGetDatavi(d, &v, 0, 1); return (float)v; }
    if (t & xplmType_FloatArray) { float v = 0; XPLMGetDatavf(d, &v, 0, 1); return v; }
    return 0.0f;
}
void writeNum(XPLMDataRef d, float v) {
    const int t = XPLMGetDataRefTypes(d);
    if (t & xplmType_Int) XPLMSetDatai(d, (int)std::lround(v));
    else if (t & xplmType_Float) XPLMSetDataf(d, v);
    else if (t & xplmType_Double) XPLMSetDatad(d, v);
    else if (t & xplmType_IntArray) { int i = (int)std::lround(v); XPLMSetDatavi(d, &i, 0, 1); }
    else if (t & xplmType_FloatArray) XPLMSetDatavf(d, &v, 0, 1);
}

// Something another plugin offers under this name: a command, or a dataref to set to 1. True when it was there.
bool trigger(const char* name) {
    if (const XPLMCommandRef c = XPLMFindCommand(name)) { XPLMCommandOnce(c); return true; }
    if (const XPLMDataRef d = XPLMFindDataRef(name)) {
        if (!XPLMCanWriteDataRef(d)) { logMsg("%s is a read-only dataref", name); return false; }
        if (XPLMGetDataRefTypes(d) & xplmType_Int) XPLMSetDatai(d, 1); else XPLMSetDataf(d, 1.0f);
        return true;
    }
    logMsg("%s: neither a command nor a dataref", name);
    return false;
}
bool offered(const char* name) { return XPLMFindCommand(name) != nullptr || XPLMFindDataRef(name) != nullptr; }

// Once per ToLiss aircraft: what its plugin offers for the Ground page, in Log.txt, so a wrong guess shows up there.
void probeToLiss() {
    // every cabin light dataref the aircraft's plugin has (X-Plane 12 lists them all): in Log.txt, and an "auto" one used for the
    // Aircraft page's Auto button. (The level is AirbusFBW/cabinLightLevel_perc; the selector's position AirbusFBW/CabinLightLevel.)
    {
        const int n = XPLMCountDataRefs();
        std::vector<XPLMDataRef> all((size_t)std::max(0, n));
        if (n > 0) XPLMGetDataRefsByIndex(0, n, all.data());
        int shown = 0;
        for (XPLMDataRef d : all) {
            if (!d) continue;
            XPLMDataRefInfo_t info;
            std::memset(&info, 0, sizeof info);
            info.structSize = sizeof info;
            XPLMGetDataRefInfo(d, &info);
            if (!info.name) continue;
            std::string lo;
            for (const char* c = info.name; *c; ++c) lo += (char)std::tolower((unsigned char)*c);
            if (lo.find("cabin") == std::string::npos || (lo.find("light") == std::string::npos && lo.find("lt") == std::string::npos)) continue;
            const int t = info.type;
            float v = 0;
            if (t & xplmType_Int) v = (float)XPLMGetDatai(d); else if (t & xplmType_Float) v = XPLMGetDataf(d);
            logMsg("ToLiss: cabin light dataref %s (%s%s, %s) = %g", info.name, (t & xplmType_Int) ? "int" : (t & xplmType_Float) ? "float" : "array",
                   (t & (xplmType_IntArray | xplmType_FloatArray)) ? " array" : "", info.writable ? "writable" : "read-only", v);
            if (!g_refs.cabinAuto && info.writable && lo.find("auto") != std::string::npos && (t & (xplmType_Int | xplmType_Float))) {
                g_refs.cabinAuto = d;
                logMsg("ToLiss: %s is used for the Aircraft page's cabin light Auto", info.name);
            }
            if (++shown > 40) break;
        }
        if (!shown) logMsg("ToLiss: no cabin light datarefs listed");
    }
    auto describe = [](const char* name) {
        const XPLMDataRef d = XPLMFindDataRef(name);
        if (XPLMFindCommand(name)) { logMsg("ToLiss: %s is a command", name); return; }
        if (!d) { logMsg("ToLiss: %s not found", name); return; }
        const int t = XPLMGetDataRefTypes(d);
        const int n = (t & xplmType_IntArray) ? XPLMGetDatavi(d, nullptr, 0, 0) : (t & xplmType_FloatArray) ? XPLMGetDatavf(d, nullptr, 0, 0) : 1;
        logMsg("ToLiss: %s is %s%s%s%s, %d value(s), %s", name, (t & xplmType_Int) ? "int " : "", (t & xplmType_Float) ? "float " : "", (t & xplmType_IntArray) ? "int[] " : "",
               (t & xplmType_FloatArray) ? "float[] " : "", n, XPLMCanWriteDataRef(d) ? "writable" : "read-only");
    };
    for (const char* n : {"AirbusFBW/PaxDoorModeArray", "AirbusFBW/CargoDoorModeArray", "AirbusFBW/BulkDoor", "AirbusFBW/Chocks", "AirbusFBW/EnableExternalPower",
                          "toliss_airbus/iscsinterface/animatedDeice", "toliss_airbus/iscsinterface/quickDeice", "toliss_airbus/door_commands/all_to_mode_close",
                          "toliss_airbus/door_commands/all_to_mode_auto", "toliss_airbus/door_commands/arm_slides", "toliss_airbus/door_commands/disarm_slides",
                          "AirbusFBW/SlideArmedArray", "AirbusFBW/cabinLightLevel_perc", "AirbusFBW/CabinLightLevel", "AirbusFBW/A321ExitConfig",
                          "opensam/jetway/status", "opensam/jetway/door/status"})
        describe(n);
}

// The built-in Weather page's numbers and the Ground page's state. A few times a second is plenty.
// Jetways docked some other way (openSAM's own window, its automatic mode) count as connected at their doors
void adoptDocked(const xpt::GroundState& g, const int docked[3]) {
    SimRefs& r = g_refs;
    if (!g.openSam) return;
    bool any = false;
    for (bool w : r.wantDoor) any = any || w;
    bool anyDocked = false;
    for (int k = 0; k < 3; ++k) anyDocked = anyDocked || docked[k] != 0;
    if (!any && anyDocked) for (int k = 0; k < 3; ++k) r.wantDoor[k] = docked[k] != 0;
    if (!anyDocked && g.jetway != 2 && g.jetway != -1) { static int quiet = 0; if (any && ++quiet > 600) { for (bool& w : r.wantDoor) w = false; quiet = 0; } }
}

// Each door's jetway, as openSAM shows it: connected only once the jetway is there, moving until then. openSAM says by door; X-Plane's
// own jetways (all of a stand's at once) take about 12 s to come or go, from the press that sent for them or sent them away.
constexpr double kXpJetwayS = 12.0;
void jetwayDoors(xpt::GroundState& g, const int docked[3]) {
    SimRefs& r = g_refs;
    const double now = XPLMGetElapsedTime();
    const bool xpThere = r.xpJetway && now - r.xpJetwayT0 >= kXpJetwayS;
    for (int k = 0; k < 3; ++k) {
        const bool here = k < g.jwDoors;
        if (g.openSam) {
            g.jwDocked[k] = here && r.wantDoor[k] && docked[k] != 0;
            g.jwMoving[k] = here && r.wantDoor[k] != (docked[k] != 0) && now - r.jwPressT[k] < 60;
        } else {
            g.jwDocked[k] = here && r.wantDoor[k] && xpThere;
            g.jwMoving[k] = here && now - r.xpJetwayT0 < kXpJetwayS && (r.wantDoor[k] ? r.xpJetway : now - r.jwPressT[k] < kXpJetwayS);
        }
    }
}
void readSimState() {
    SimRefs& r = g_refs;
    const float mToFt = 3.28084f;
    xpt::SimWeather& x = g_sim.wx;
    x.valid = r.windDir && r.windMs && r.qnh && r.temp;
    if (x.valid) {
        x.windDir = XPLMGetDataf(r.windDir);
        x.windKt = XPLMGetDataf(r.windMs) * 1.943844f;
        x.visSm = r.vis ? XPLMGetDataf(r.vis) : 10.0f;
        x.tempC = XPLMGetDataf(r.temp);
        x.qnhHpa = XPLMGetDataf(r.qnh) / 100.0f;
        x.rain = r.rain ? XPLMGetDataf(r.rain) : 0.0f;
        x.altFt = r.alt ? (float)XPLMGetDatad(r.alt) * mToFt : 0.0f;
        float base[3] = {0, 0, 0}, topm[3] = {0, 0, 0}, cov[3] = {0, 0, 0};
        if (r.cBase) XPLMGetDatavf(r.cBase, base, 0, 3);
        if (r.cTop) XPLMGetDatavf(r.cTop, topm, 0, 3);
        if (r.cCover) XPLMGetDatavf(r.cCover, cov, 0, 3);
        x.clouds = 0;
        for (int i = 0; i < 3; ++i)
            if (cov[i] >= 0.06f && topm[i] > base[i]) { x.cloudBaseFt[x.clouds] = base[i] * mToFt; x.cloudTopFt[x.clouds] = topm[i] * mToFt; x.cloudCover[x.clouds] = cov[i]; ++x.clouds; }
        float alt[13] = {}, dir[13] = {}, kt[13] = {}, dews[13] = {};
        const int n = (r.aAlt && r.aDir && r.aKt) ? std::min({XPLMGetDatavf(r.aAlt, alt, 0, 13), XPLMGetDatavf(r.aDir, dir, 0, 13), XPLMGetDatavf(r.aKt, kt, 0, 13)}) : 0;
        x.aloft = 0;
        for (int i = 0; i < n && i < 13; ++i) { x.aloftFt[x.aloft] = alt[i] * mToFt; x.aloftDir[x.aloft] = dir[i]; x.aloftKt[x.aloft] = kt[i]; ++x.aloft; }
        // the dew point comes in layers too: the one nearest the aircraft
        x.dewC = x.tempC;
        if (r.dew && n > 0 && XPLMGetDatavf(r.dew, dews, 0, 13) >= n) {
            int best = 0;
            for (int i = 1; i < n; ++i) if (std::fabs(alt[i] * mToFt - x.altFt) < std::fabs(alt[best] * mToFt - x.altFt)) best = i;
            x.dewC = std::min(x.tempC, dews[best]);
        }
    }
    xpt::GroundState& g = g_sim.ground;
    g.onGround = !r.onGround || XPLMGetDatai(r.onGround) != 0;
    g.openSam = XPLMFindCommand("openSAM/toggle_jwy") != nullptr && r.jwStatus != nullptr;
    g.jetway = g.openSam ? XPLMGetDatai(r.jwStatus) : 0;
    g.toliss = g_sim.toliss && r.paxDoor != nullptr;
    g.paxDoors = g.cargoDoors = 0;
    // what keeps a jetway from connecting (openSAM: beacon on or not on the ground; and good practice: brake or chocks set, engines off)
    g.beacon = r.beacon && XPLMGetDatai(r.beacon) != 0;
    g.groundSpeedKt = r.gs ? XPLMGetDataf(r.gs) * 1.943844f : 0.0f;
    g.enginesRunning = false;
    if (r.engRun) { int run[8] = {}; const int n = std::min(8, XPLMGetDatavi(r.engRun, nullptr, 0, 0)); XPLMGetDatavi(r.engRun, run, 0, n); for (int i = 0; i < n; ++i) if (run[i]) g.enginesRunning = true; }
    g.parkBrake = (g_sim.toliss && r.tlParkBrake) ? readNum(r.tlParkBrake) > 0.5f : (r.parkBrake && XPLMGetDataf(r.parkBrake) > 0.5f);
    g.chocks = false;
    if (r.isChocked) { int ch[10] = {}; const int n = std::min(10, XPLMGetDatavi(r.isChocked, nullptr, 0, 0)); XPLMGetDatavi(r.isChocked, ch, 0, n); for (int i = 0; i < n; ++i) if (ch[i]) g.chocks = true; }
    // X-Plane's own jetways leave when the plane moves off or flies; what was pressed is forgotten then
    if (!g.onGround || g.groundSpeedKt > 3.0f) { r.xpJetway = false; for (bool& w : r.wantDoor) w = false; }
    if (!g.openSam) g.jetway = r.xpJetway ? 2 : 1;
    // the doors a jetway serves on other aircraft: as many as openSAM drives (X-Plane's own: one bubble, every boarding door)
    if (!g.toliss) {
        g.jwDoors = 1;
        if (g.openSam && r.jwNumber) g.jwDoors = std::max(1, std::min(3, XPLMGetDatai(r.jwNumber)));
        int docked[3] = {0, 0, 0};
        if (g.openSam && r.jwDoorStatus) XPLMGetDatavi(r.jwDoorStatus, docked, 0, 3);
        adoptDocked(g, docked);
        for (int k = 0; k < 3; ++k) g.jwDoorPax[k] = 2 * k;
        g.jwDoors = std::min(g.jwDoors, 2);   // 1L, and 2L ahead of the wing: never a door behind it
        jetwayDoors(g, docked);
        g.paxDoors = g.jwDoors >= 2 ? 8 : 4;   // (the layout with 2L forward)
    }
    if (g.toliss) {
        // the arrays are as long as the largest ToLiss needs: the A319/A320 have two pairs of cabin doors, the A321 four
        const int have = XPLMGetDatavi(r.paxDoor, nullptr, 0, 0);
        // four pairs on the A321s and the widebodies (A330-900, A340-600), two on the A319 and A320s
        const std::string& ic = g_sim.acfIcao;
        const bool long321 = ic.compare(0, 3, "A21") == 0 || ic == "A321";
        const bool wide = ic.compare(0, 2, "A3") == 0 && ic != "A318" && ic != "A319" && ic != "A320" && ic != "A321";
        g.paxDoors = std::min(have, long321 || wide ? 8 : 4);
        XPLMGetDatavi(r.paxDoor, g.paxDoor, 0, g.paxDoors);
        if (r.cargoDoor) { g.cargoDoors = std::min(XPLMGetDatavi(r.cargoDoor, nullptr, 0, 0), 3); XPLMGetDatavi(r.cargoDoor, g.cargoDoor, 0, g.cargoDoors); }   // forward, aft, bulk
        g.deice = offered("toliss_airbus/iscsinterface/animatedDeice") || offered("toliss_airbus/iscsinterface/quickDeice");
        if (!g_refs.probed) { g_refs.probed = true; probeToLiss(); }
        g.chocks = g.chocks || (r.chocks && XPLMGetDatai(r.chocks) != 0);
        g.extPower = r.extPower && XPLMGetDatai(r.extPower) != 0;
        g.hpAir = r.hpAir && XPLMGetDatai(r.hpAir) != 0;
        g.lpAir = r.lpAir && XPLMGetDatai(r.lpAir) != 0;
        // the doors a jetway can reach, as openSAM docks them: 1L; and 2L on the widebodies, and on an A321 / A321neo with the classic
        // door layout (A321ExitConfig 0: the second door ahead of the wing)
        g.jwDoors = 1;
        g.jwDoorPax[0] = 0;
        if (wide && g.paxDoors > 2) { g.jwDoors = 2; g.jwDoorPax[1] = 2; }
        else if (long321 && r.exitConfig && readNum(r.exitConfig) == 0 && g.paxDoors > 2) { g.jwDoors = 2; g.jwDoorPax[1] = 2; }
        int docked[3] = {0, 0, 0};
        if (r.jwDoorStatus) XPLMGetDatavi(r.jwDoorStatus, docked, 0, 3);
        adoptDocked(g, docked);
        jetwayDoors(g, docked);
        // Cabin Ready holds for the phase it was given in: gone once the aircraft takes off, or lands
        if (r.cabinReady && g.onGround != r.cabinReadyGround) r.cabinReady = false;
        g.cabinReady = r.cabinReady;
        // slides: armed if any door's is
        g.slides = -1;
        if (r.slides) {
            int sl[8] = {};
            const int n = std::min(8, XPLMGetDatavi(r.slides, nullptr, 0, 0));
            float slf[8] = {};
            if (XPLMGetDataRefTypes(r.slides) & xplmType_IntArray) XPLMGetDatavi(r.slides, sl, 0, n); else { XPLMGetDatavf(r.slides, slf, 0, n); for (int i = 0; i < n; ++i) sl[i] = slf[i] > 0.5f; }
            g.slides = 0;
            for (int i = 0; i < n; ++i) if (sl[i]) g.slides = 1;
        }
        // cabin lights: the level in percent, else ToLiss's 0 / 1 / 2 (off, dim, bright)
        g.cabinLight = r.cabinPerc ? readNum(r.cabinPerc) / 100.0f : r.cabinLevel ? readNum(r.cabinLevel) / 2.0f : -1.0f;
        if (g.cabinLight >= 0) g.cabinLight = std::max(0.0f, std::min(1.0f, g.cabinLight));
        g.cabinAuto = r.cabinAuto ? (readNum(r.cabinAuto) > 0.5f ? 1 : 0) : -1;
    } else {
        g.slides = -1; g.cabinLight = -1;
    }
}

// The cabin door a jetway is on opens once the jetway has docked, and closes when the jetway leaves. Docked by door comes from
// openSAM; without openSAM, X-Plane's own jetways (every boarding door at once) follow the Connect / Disconnect presses.
// (the door opens 2.5 s after the jetway shows docked, once it has really come to rest against the aircraft; on Disconnect the
// door closes first and the jetway is sent away once it has shut, see jetwayUndockStep)
double g_dockedSince[3] = {-1, -1, -1};
double g_undockAt = -1;          // when the jetways are to be sent away (the doors closing first), -1 none
void followJetways() {
    SimRefs& r = g_refs;
    const xpt::GroundState& g = g_sim.ground;
    if (!g.toliss || !r.paxDoor) return;
    const double now = XPLMGetElapsedTime();
    for (int k = 0; k < g.jwDoors && k < 3; ++k) {
        if (g.jwDocked[k] && g_dockedSince[k] < 0) g_dockedSince[k] = now;
        if (!g.jwDocked[k]) g_dockedSince[k] = -1;
        const bool settled = g.jwDocked[k] && now - g_dockedSince[k] >= 2.5;
        if (settled == r.wasDocked[k]) continue;
        r.wasDocked[k] = settled;
        const int pax = g.jwDoorPax[k];
        if (pax < 0 || pax >= g.paxDoors) continue;
        int mode = settled ? 2 : 0;
        XPLMSetDatavi(r.paxDoor, &mode, pax, 1);
        logMsg("jetway %s door %dL: door %s", settled ? "docked at" : "left", pax / 2 + 1, settled ? "opened" : "closed");
    }
}
void once(const char* cmd);
void undockNow() {
    SimRefs& rr = g_refs;
    if (g_sim.ground.openSam) once("openSAM/undock_jwy"); else { once("sim/ground_ops/jetway"); rr.xpJetway = false; rr.xpJetwayT0 = XPLMGetElapsedTime(); }
}
void jetwayUndockStep() {   // the jetways go once the doors they were on have shut (ToLiss), or after 6 s at most
    if (g_undockAt < 0) return;
    const xpt::GroundState& g = g_sim.ground;
    bool shut = true;
    for (int k = 0; k < g.jwDoors && k < 3; ++k) { const int pax = g.jwDoorPax[k]; if (pax >= 0 && pax < 8 && g.paxDoor[pax] != 0) shut = false; }
    const double now = XPLMGetElapsedTime();
    if ((shut && now >= g_undockAt - 4.5) || now >= g_undockAt) { g_undockAt = -1; undockNow(); logMsg("jetway: sent away (the doors shut)"); }
}

void once(const char* cmd) {
    const XPLMCommandRef c = XPLMFindCommand(cmd);
    if (c) XPLMCommandOnce(c); else logMsg("command %s not found", cmd);
}
void toggleInt(XPLMDataRef ref) { if (ref) XPLMSetDatai(ref, XPLMGetDatai(ref) ? 0 : 1); }

// What was pressed on the Ground page.
// The slides' Auto: as ToLiss does it, armed once the beacon is on with every cabin door shut, disarmed parked with the beacon and
// engines off. Sends ToLiss's own arm / disarm commands, at most once every few seconds.
bool g_slidesAuto = false;
// Arm or Disarm pressed: held so (the aircraft's own arming or disarming undone at once): -1 disarmed, 1 armed (on the ground,
// with the doors shut), 0 none (Auto, or the aircraft's way). Checked every frame, straight from the slides' dataref, so a slide
// the aircraft arms is disarmed again before it is drawn (checked every few frames, it flickered).
int g_slidesHold = 0;
void slidesHoldStep() {
    const SimRefs& r = g_refs;
    const xpt::GroundState& g = g_sim.ground;
    if (!g_slidesHold || !g.toliss || !r.slides) return;
    int sl[8] = {};
    float slf[8] = {};
    const int n = std::min(8, XPLMGetDatavi(r.slides, nullptr, 0, 0));
    if (n <= 0) return;
    if (XPLMGetDataRefTypes(r.slides) & xplmType_IntArray) XPLMGetDatavi(r.slides, sl, 0, n); else { XPLMGetDatavf(r.slides, slf, 0, n); for (int i = 0; i < n; ++i) sl[i] = slf[i] > 0.5f; }
    bool any = false, all = true;
    for (int i = 0; i < n; ++i) { any = any || sl[i]; all = all && sl[i]; }
    static double lastLog = -100, lastArm = -100;
    const double now = XPLMGetElapsedTime();
    if (g_slidesHold < 0 && any) {
        trigger("toliss_airbus/door_commands/disarm_slides");
        if (now - lastLog > 10) { lastLog = now; logMsg("slides: kept disarmed (the aircraft armed them)"); }
    } else if (g_slidesHold > 0 && !all && g.onGround && now - lastArm > 2.0) {
        bool shut = true;
        for (int i = 0; i < g.paxDoors && i < 8; ++i) if (g.paxDoor[i] != 0) shut = false;
        if (shut) { trigger("toliss_airbus/door_commands/arm_slides"); lastArm = now; if (now - lastLog > 10) { lastLog = now; logMsg("slides: kept armed on the ground"); } }
    }
}
void slidesAutoStep() {
    const xpt::GroundState& g = g_sim.ground;
    if (g_slidesHold) return;
    if (!g_slidesAuto || !g.toliss || g.slides < 0) return;
    static double last = -10;
    const double now = XPLMGetElapsedTime();
    if (now - last < 4) return;
    bool doorsShut = true;
    for (int i = 0; i < g.paxDoors && i < 8; ++i) if (g.paxDoor[i] == 2) doorsShut = false;
    if (g.slides == 0 && g.beacon && doorsShut) { trigger("toliss_airbus/door_commands/arm_slides"); last = now; logMsg("slides: auto armed (beacon on, doors shut)"); }
    else if (g.slides == 1 && g.onGround && !g.beacon && !g.enginesRunning) { trigger("toliss_airbus/door_commands/disarm_slides"); last = now; logMsg("slides: auto disarmed (parked, beacon and engines off)"); }
}

void doGround(const xpt::Actions& act) {
    const SimRefs& r = g_refs;
    const xpt::GroundState& g = g_sim.ground;
    switch (act.ground) {
    case xpt::GA_JETWAY: once(g.openSam ? "openSAM/toggle_jwy" : "sim/ground_ops/jetway"); break;
    case xpt::GA_JETWAY_DOOR: {   // a door's Connect / Disconnect: that door only
        // openSAM or X-Plane's own jetways, whichever the stand has: openSAM's commands when it is installed (it also hands X-Plane's
        // own jetways to X-Plane), else X-Plane's. Neither takes a door: the stand's jetways move together. So they are called for the
        // first door connected and sent away with the last one disconnected; each door opens and closes on its own.
        SimRefs& rr = g_refs;
        const int k = std::max(0, std::min(2, act.groundIndex));
        bool before = false, after = false;
        for (bool w : rr.wantDoor) before = before || w;
        rr.wantDoor[k] = !rr.wantDoor[k];
        for (bool w : rr.wantDoor) after = after || w;
        rr.jwPressT[k] = XPLMGetElapsedTime();
        if (!before && after && g_undockAt >= 0) g_undockAt = -1;   // connected again before the jetway left: it stays
        else if (!before && after) {
            if (g.openSam) once("openSAM/dock_jwy"); else { once("sim/ground_ops/jetway"); rr.xpJetway = true; rr.xpJetwayT0 = XPLMGetElapsedTime(); }
        } else if (before && !after) {   // the last door: it closes now (followJetways), the jetways leave when it has shut
            if (g.toliss && rr.paxDoor) g_undockAt = XPLMGetElapsedTime() + 6.0; else undockNow();
        }
        logMsg("jetway: door %d %s (%s jetways)", k + 1, rr.wantDoor[k] ? "connected" : "disconnected", g.openSam ? "openSAM" : "X-Plane's");
        break;
    }
    case xpt::GA_SLIDES_ARM: g_slidesAuto = false; g_slidesHold = 1; if (g.toliss) trigger("toliss_airbus/door_commands/arm_slides"); break;
    case xpt::GA_SLIDES_DISARM: g_slidesAuto = false; g_slidesHold = -1; if (g.toliss) trigger("toliss_airbus/door_commands/disarm_slides"); break;
    case xpt::GA_SLIDES_AUTO: g_slidesAuto = true; g_slidesHold = 0; break;
    case xpt::GA_CABIN_READY:   // the cabin is secure: ToLiss's CABIN READY memo (as its flight attendant panel sends it)
        // pressed again: not ready (ToLiss has no command to take its memo back, so only the tablet's button goes back)
        if (g.toliss && g_refs.cabinReady) g_refs.cabinReady = false;
        else if (g.toliss) { once("AirbusFBW/CheckCabin"); g_refs.cabinReady = true; g_refs.cabinReadyGround = g.onGround; }
        break;
    case xpt::GA_CABIN_LIGHT: {   // the level, 0-100 % in steps of 10; ToLiss's 0 / 1 / 2 switch when there is no percentage
        const int pct = std::max(0, std::min(100, act.groundIndex));
        // the selector (off / dim / bright) moves with the level, and choosing a level leaves Auto
        bool wrote = false;
        if (r.cabinAuto && XPLMCanWriteDataRef(r.cabinAuto)) writeNum(r.cabinAuto, 0.0f);
        if (r.cabinLevel && XPLMCanWriteDataRef(r.cabinLevel)) { writeNum(r.cabinLevel, pct == 0 ? 0.0f : pct <= 50 ? 1.0f : 2.0f); wrote = true; }
        if (r.cabinPerc && XPLMCanWriteDataRef(r.cabinPerc)) { writeNum(r.cabinPerc, (float)pct); wrote = true; }
        if (!wrote) logMsg("cabin lights: no writable ToLiss dataref");
        break;
    }
    case xpt::GA_CABIN_AUTO:
        if (r.cabinAuto && XPLMCanWriteDataRef(r.cabinAuto)) writeNum(r.cabinAuto, readNum(r.cabinAuto) > 0.5f ? 0.0f : 1.0f);
        else logMsg("cabin lights: this aircraft has no cabin light auto setting");
        break;
    case xpt::GA_GPU: if (g.toliss && r.extPower) toggleInt(r.extPower); else once("sim/ground_ops/toggle_gpu_request"); break;
    case xpt::GA_CHOCKS: if (g.toliss) toggleInt(r.chocks); break;
    case xpt::GA_HP_AIR: if (g.toliss) toggleInt(r.hpAir); break;
    case xpt::GA_LP_AIR: if (g.toliss) toggleInt(r.lpAir); break;
    case xpt::GA_TRUCKS: once("sim/ground_ops/service_plane"); break;
    case xpt::GA_PUSH_LEFT: once("sim/ground_ops/pushback_left"); break;
    case xpt::GA_PUSH_STRAIGHT: once("sim/ground_ops/pushback_straight"); break;
    case xpt::GA_PUSH_RIGHT: once("sim/ground_ops/pushback_right"); break;
    case xpt::GA_PUSH_STOP: once("sim/ground_ops/pushback_stop"); break;
    case xpt::GA_DOORS_CLOSE: if (g.toliss) trigger("toliss_airbus/door_commands/all_to_mode_close"); break;
    case xpt::GA_DOORS_AUTO: if (g.toliss) trigger("toliss_airbus/door_commands/all_to_mode_auto"); break;
    case xpt::GA_DEICE: if (g.toliss) trigger("toliss_airbus/iscsinterface/animatedDeice"); break;
    case xpt::GA_DEICE_QUICK: if (g.toliss) trigger("toliss_airbus/iscsinterface/quickDeice"); break;
    case xpt::GA_PAX_DOOR:
    case xpt::GA_CARGO_DOOR: {   // ToLiss door modes: 0 closed, 1 automatic, 2 open
        const bool pax = act.ground == xpt::GA_PAX_DOOR;
        const XPLMDataRef ref = pax ? r.paxDoor : r.cargoDoor;
        const int n = pax ? g.paxDoors : g.cargoDoors, i = act.groundIndex;
        if (!g.toliss || !ref || i < 0 || i >= n) break;
        int v = (pax ? g.paxDoor[i] : g.cargoDoor[i]) == 2 ? 0 : 2;
        XPLMSetDatavi(ref, &v, i, 1);
        logMsg("%s door %d: mode %d", pax ? "cabin" : "cargo", i, v);
        break;
    }
    default: break;
    }
}

float flightLoop(float dt, float, int, void*) {
    findSimRefs();
    if ((XPLMGetCycleNumber() & 7) == 0) { readSimState(); g_sim.ground.slidesAuto = g_slidesAuto; followJetways(); slidesAutoStep(); readMapState(); }
    slidesHoldStep();
    jetwayUndockStep();
    {   // the METAR search's answer
        xpt::MetarAnswer a;
        if (xpt::metarPoll(a)) { g_sim.metar.busy = false; g_sim.metar.icao = a.icao; g_sim.metar.raw = a.raw; g_sim.metar.error = a.error; }
        else g_sim.metar.busy = xpt::metarBusy();
    }
    if (g_pendingGround.ground != xpt::GA_NONE) { doGround(g_pendingGround); g_pendingGround = xpt::Actions(); readSimState(); }
    // The battery: fed while any of the aircraft's buses is live (full in about 1.5 h), otherwise it runs down (about 6 h).
    bool fed = false;
    if (g_drBus) {
        float v[6] = {0, 0, 0, 0, 0, 0};
        const int n = XPLMGetDatavf(g_drBus, v, 0, 6);
        for (int i = 0; i < n; ++i) fed = fed || v[i] > 10.0f;
    }
    const float sec = std::max(0.0f, std::min(1.0f, dt));
    g_battery = std::max(0.0f, std::min(100.0f, g_battery + sec * (fed ? 100.0f / 5400.0f : -100.0f / 21600.0f)));
    g_sim.batteryPct = (int)std::lround(g_battery);
    g_sim.charging = fed;
    if (xpt::takeSettingsChanged()) saveSettings();
    saveSettingsIfChanged(sec);
    chimeStep();
    updateNotify();
    if (xpt::takeNotesChanged()) saveNotes();
    if (g_drZulu) g_sim.zuluSec = (int)XPLMGetDataf(g_drZulu);
    if (g_drIcao) {
        char icao[41] = "";
        const int n = XPLMGetDatab(g_drIcao, icao, 0, 40);
        icao[std::max(0, std::min(40, n))] = 0;
        if (g_sim.acfIcao != icao) { g_sim.acfIcao = icao; loadChecklist(); }
    }
    if ((XPLMGetCycleNumber() & 127) == 0) g_sim.toliss = isToLissAircraft();   // its plugin loads after the aircraft does
    if ((XPLMGetCycleNumber() & 255) == 0) g_sim.wxPlane = XPLMFindPluginBySignature(kWxPlaneSignature) != XPLM_NO_PLUGIN_ID;   // it may load after us
    updateCaptureWindow();
    return -1.0f;   // every frame
}

} // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "XPTablet");
    std::strcpy(outSig, "com.alpineaviation.xptablet");
    std::strcpy(outDesc, "XPTablet " XPT_VERSION " by Alpine Aviation - the cockpit tablet");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    g_drClickX = XPLMFindDataRef("sim/graphics/view/click_3d_x_pixels");
    g_drClickY = XPLMFindDataRef("sim/graphics/view/click_3d_y_pixels");
    g_drZulu = XPLMFindDataRef("sim/time/zulu_time_sec");
    g_drIcao = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    g_drVr = XPLMFindDataRef("sim/graphics/VR/enabled");
    g_drUiScale = XPLMFindDataRef("sim/graphics/misc/user_interface_scale");
    g_drBus = XPLMFindDataRef("sim/cockpit2/electrical/bus_volts");

    // AviTab's interface is taken over only when AviTab itself is not there (it loads before us: plugin folders load in name order)
    g_aviTabRunning = XPLMFindPluginBySignature(kAviTabSignature) != XPLM_NO_PLUGIN_ID || XPLMFindDataRef("avitab/panel_enabled") != nullptr;
    g_ownsAviTab = !g_aviTabRunning;
    registerDatarefs("xptablet");
    registerCommands("XPTablet");
    if (g_ownsAviTab) {
        registerDatarefs("avitab");
        registerCommands("AviTab");
        logMsg(XPT_VERSION ": AviTab is not installed, XPTablet takes the cockpit tablet");
    } else {
        logMsg(XPT_VERSION ": AviTab is installed and keeps the cockpit tablet. XPTablet runs in its window only; remove AviTab to replace it.");
    }

    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    // ImGui's developer warnings (conflicting IDs, recovered errors) never pop up over the cockpit: the tests and the emulator
    // look for them instead (tests/ui_shot.cpp, emulator/web/web.cpp)
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
    xpt::applyStyle();
    loadSettings();
    loadNotes();
    loadShapesFile();

    g_menuItem = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XPTablet", nullptr, 0);
    g_menu = XPLMCreateMenu("XPTablet", XPLMFindPluginsMenu(), g_menuItem, menuHandler, nullptr);
    XPLMAppendMenuItem(g_menu, "Show / hide tablet window", nullptr, 0);
    return 1;
}


PLUGIN_API int XPluginEnable(void) {
    updateCheckStart();   // (once a session, in the background)
    XPLMRegisterKeySniffer(keySniffer, 1, nullptr);   // (before X-Plane's windows: typed keys go to a text box first)
    XPLMRegisterDrawCallback(drawPanel, xplm_Phase_Gauges, 0, nullptr);
    XPLMCreateFlightLoop_t fl;
    fl.structSize = sizeof fl;
    fl.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc = flightLoop;
    fl.refcon = nullptr;
    g_loop = XPLMCreateFlightLoop(&fl);
    XPLMScheduleFlightLoop(g_loop, -1.0f, 1);
    g_sim.wxPlane = XPLMFindPluginBySignature(kWxPlaneSignature) != XPLM_NO_PLUGIN_ID;
    wx::httpGlobalInit();
    logMsg("downloads: %s", wx::httpTlsInfo().c_str());
    xpt::metarStart();
    loadAircraftConfig();
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    updateCheckStop();
    if (xpt::settingsToText() != g_savedSettings) saveSettings();   // nothing changed is lost when the sim or the plugins reload
    XPLMUnregisterKeySniffer(keySniffer, 1, nullptr);
    XPLMUnregisterDrawCallback(drawPanel, xplm_Phase_Gauges, 0, nullptr);
    if (g_loop) { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_capture) { XPLMDestroyWindow(g_capture); g_capture = nullptr; }
    g_captureDown = false;
    xpt::metarStop();
    xpt::media::stop();
    browserStop();
    mapStop();
    wx::httpGlobalCleanup();
}

PLUGIN_API void XPluginStop(void) {
    if (g_win) { XPLMDestroyWindow(g_win); g_win = nullptr; }
    for (const Cmd& c : g_cmds) XPLMUnregisterCommandHandler(c.ref, onCommand, 1, (void*)(intptr_t)c.id);
    g_cmds.clear();
    for (XPLMDataRef r : g_ownedRefs) XPLMUnregisterDataAccessor(r);
    g_ownedRefs.clear();
    if (g_menu) { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
    if (g_menuItem >= 0) { XPLMRemoveMenuItem(XPLMFindPluginsMenu(), g_menuItem); g_menuItem = -1; }
    if (g_ctx) { ImGui::DestroyContext(g_ctx); g_ctx = nullptr; }
    g_fontScale = 0;   // the fonts went with the context: a later start (without unloading) builds them again
    g_fonts = xpt::Fonts();
    g_batches.clear();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int msg, void* param) {
    if (msg == XPLM_MSG_PLANE_LOADED && (intptr_t)param == 0) {   // the user's aircraft
        loadAircraftConfig();
        g_refs.probed = false;
        for (bool& d : g_refs.wasDocked) d = false;
        g_refs.xpJetway = false;
        g_sim.toliss = false;   // looked for again once the new aircraft's plugins are in
        xpt::openApp(xpt::APP_HOME);
    } else if (msg == XPLM_MSG_ENTERED_VR) {
        if (g_win) XPLMSetWindowPositioningMode(g_win, xplm_WindowVR, -1);
    } else if (msg == XPLM_MSG_EXITING_VR) {
        if (g_win) XPLMSetWindowPositioningMode(g_win, xplm_WindowPositionFree, -1);
    }
}
