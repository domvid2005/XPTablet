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
#include <type_traits>
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
#include "samscan.h"
#include "crashlog.h"
#include "guard.h"
#include "clipboard.h"
#include "navdata.h"
#include "vatsim.h"
#include "airspace.h"
#include "localnet.h"
#if defined(XPT_LOCAL_CONNECT)
#include "localmirror.h"
#include "localicons.h"
#include "localpage.h"
#include "localserver.h"
#endif
#include <condition_variable>
#include <deque>
#include "simbrief.h"
#include "acars.h"
#include "media.h"
#include "ourairports.h"
#include "stb_image.h"
#include "browser.h"
#include "tiles.h"

#include "imgui.h"
#include "imgui_internal.h"   // (ErrorRecoveryStoreState: the page put back after a caught error)
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

// ---- C++ errors in X-Plane's callbacks: caught and logged, and the tablet carries on. Uncaught, one would end X-Plane on the spot
// (std::terminate; the crash recorder notes it, but the flight is gone). Each callback is registered as Safe<itself, fallback>::call,
// which returns the fallback when its callback threw (a flight loop: -1, called again next frame; a key: 1, X-Plane's own).
void callbackFailed(const char* what) {
    const char* where = xpt::crashlog::here("");
    xpt::crashlog::here(where);
    static int told = 0;
    if (told < 20) { ++told; logMsg("an error, caught: %s (in %s); the tablet carries on", what, where && *where ? where : "a callback"); }
}
template <auto F, auto Fallback> struct Safe;
template <typename R, typename... A, R (*F)(A...), auto Fallback> struct Safe<F, Fallback> {
    static R call(A... a) {
        try { return F(a...); }
        catch (const std::exception& e) { callbackFailed(e.what()); }
        catch (...) { callbackFailed("(not a std::exception)"); }
        if constexpr (!std::is_void_v<R>) return (R)Fallback;
    }
};

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
int g_panelWritesLogged = 0;      // how many of the aircraft's switchings of its tablet were put in the log (the first of each aircraft)
float g_brightness = 1.0f;
float g_mirrorScale = 0;              // Local Connect: the scale a connected device is shown the tablet at (0: none connected)
unsigned g_texChanges = 0;           // counts the pictures given to the graphics card (a web page's, a map tile, a document, a cover): the mirror looks again
double g_lastDeviceDown = -1, g_lastSimDown = -1;   // when a device, and the sim's own mouse, last pressed on the tablet
double g_remotePointerUntil = -1;    // a device's pointer has the tablet until then (the sim's own pointer waits)
bool g_captureClicks = true;   // AviTab.json's panel/disable_capture_window turns this off (the aircraft sends AviTab/click_left itself)
bool g_tookAviTabSig = false;   // XPTablet runs under AviTab's plugin signature (AviTab not installed)
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
int g_logoMaskTex = 0;               // the logo in white (drawn in the theme's colours)
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
    XPLMDataRef landingLights = nullptr, taxiLight = nullptr;          // (the ones BetterPushback watches)
    XPLMDataRef bpStarted = nullptr, bpConnected = nullptr;            // BetterPushback: bp/started, bp/connected
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
// Its top edge stays where AviTab's is (under the aircraft's own strip); below and at the sides it runs on to the rectangle's edges:
// AviTab's centred picture left a band of the rectangle bare under it (the ToLiss: 13 pixels of whatever the texture held there, a
// grey edge that did not fit the screen, 2026-11-26). The layout takes the picture's shape, a little taller than AviTab's.
void panelPicture(float& l, float& b, float& w, float& h) {
    const float rw = (float)g_panelWidth, rh = (float)g_panelHeight;
    float aw, ah;   // AviTab's picture, centred
    if (rw / rh > kPanelAspect) { ah = rh; aw = std::floor(rh * kPanelAspect); }
    else { aw = rw; ah = std::floor(rw / kPanelAspect); }
    const float top = (float)g_panelBottom + std::floor((rh - ah) * 0.5f) + ah;
    l = (float)g_panelLeft; w = rw;
    b = (float)g_panelBottom; h = top - b;
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
    g_updTh = std::thread(xpt::guarded("update check", []() {
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
    }));
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
    g_panelWritesLogged = 0;
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
    if (!g_logoMaskTex) {   // the logo in white, the same way
        int lw = 0, lh = 0, lv = 0; float from = 0;
        const unsigned char* px = xpt::logoMaskPixels(lw, lh, lv, from);
        if (px && lw > 0 && lh > 0) {
            XPLMGenerateTextureNumbers(&g_logoMaskTex, 1);
            XPLMBindTexture2d(g_logoMaskTex, 0);
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
    g_fonts.logoMask = (ImTextureID)(intptr_t)g_logoMaskTex;
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
    if (!act.webFind.empty()) br::push(xptb::CMD_FIND, 0, 0, 0, 0, 0, act.webFind);
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
    if (act.webEdit) br::push(xptb::CMD_EDIT, act.webEdit);
    if (act.paste) { std::string t; if (xpt::clipboard::get(t)) xpt::pasteText(t); }
    {   // the clipboard: whether it holds text, looked at while typing (the keyboard offers Paste)
        static float lookedAt = -100;
        const float now = XPLMGetElapsedTime();
#if defined(_WIN32)
        const float every = 0.5f;
#else
        const float every = 1.5f;   // (asked through a small program: less often)
#endif
        const bool typing = xpt::tabletTyping() || xpt::webTyping();
        if (!typing) g_sim.clipboardHas = false;
        else if (now - lookedAt > every) { lookedAt = now; g_sim.clipboardHas = xpt::clipboard::hasText(); }
    }
    if (act.webTabSelect) br::push(xptb::CMD_TAB_SELECT, act.webTabSelect);
    if (act.webTabClose) br::push(xptb::CMD_TAB_CLOSE, act.webTabClose);
    bool loading = false, back = false, fwd = false, kbd = false;
    std::vector<br::TabInfo> tabs;
    int active = 0, zoom = 100;
    bool full = false;
    bool zAuto = true;
    if (br::info(b.url, b.title, loading, back, fwd, kbd, tabs, active, zoom, full, &zAuto)) {
        b.zoomPct = zoom; b.fullscreen = full; b.zoomAuto = zAuto; b.mediaPlaying = br::mediaPlaying();
        b.loading = loading; b.canBack = back; b.canForward = fwd; b.wantsKeyboard = kbd;
        b.tabs.clear();
        for (const br::TabInfo& t : tabs) b.tabs.push_back({t.id, t.title, t.url, t.loading, t.group});
        b.activeTab = active;
    }
    const uint8_t* px = nullptr;
    int w = 0, h = 0;
    if (br::frame(px, w, h)) {
        if (!g_webTex) XPLMGenerateTextureNumbers(&g_webTex, 1);
        XPLMBindTexture2d(g_webTex, 0); ++g_texChanges;
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
    xpt::airspace::stop();
    if (g_docThread.joinable()) g_docThread.join();
    g_sim.map.airports.clear();
    g_sim.map.airportPoints.clear();
}

// After each UI build: what the map asked for, and the tiles that arrived (a few per frame, so no frame takes long)
// true once every secs (by the clock: the tablet is not drawn every frame, so a check on one frame number in N may never come)
bool every(float& last, float secs) { const float now = XPLMGetElapsedTime(); if (now - last < secs) return false; last = now; return true; }
void mapStep(const xpt::Actions& act) {
    // the airports' runways round the aircraft are wanted for the runway collision alert, map or no map: on the ground or
    // low (under 3000 ft above it), the airport index starts, and when the map is not on screen its view is the aircraft
    const xpt::MapState& ms = g_sim.map;
    const bool nearGround = ms.havePos && (ms.onGround || ms.aglFt < 3000) && xpt::settings().notifyTcas;
    if (act.mapShown || (nearGround && XPLMGetElapsedTime() > 20)) mapStart();
    if (!g_mapStarted) return;
    if (!act.mapShown && nearGround) {
        xpt::aptdat::setView(ms.lat, ms.lon, 4.0, 0.0, true);
        static float t1 = -100;
        if (every(t1, 0.5f)) {
            int n = 0;
            std::vector<xpt::AptPoint> pts;
            xpt::aptdat::get(g_sim.map.airports, pts, g_sim.map.aptIndexing, n);
        }
    }
    if (act.mapShown) {
        xpt::tiles::want(g_tileWants);
        xpt::aptdat::setView(act.mapLat, act.mapLon, act.mapLayoutNm, act.mapPointNm, act.mapLayouts, act.mapPointMinFt);
        xpt::navdata::setView(act.mapLat, act.mapLon, act.mapNavNm, act.mapFixNm);
        static float t2 = -100;
        if (every(t2, 0.12f)) {
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
        XPLMBindTexture2d(tex, 0); ++g_texChanges;
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
// The user's latest plan, from SimBrief's public fetcher with the saved pilot ID, on a thread, when Import is pressed (and every two
// minutes, quietly). X-Plane never waits on it: a fetch asked for while one is on its way waits its turn (g_sbPending) and starts when
// that one has finished, instead of the main thread joining a thread still downloading (up to 25 s of a frozen sim, 2026-11-27).
std::mutex g_sbMx;
bool g_sbDone = false;            // (under g_sbMx) a fetch has finished: its plan, the ID it asked with and whether it was quiet
xpt::FlightPlan g_sbPlan;
std::string g_sbPlanUser;
bool g_sbPlanQuiet = false;
std::atomic<bool> g_sbRunning{false};   // a fetch is on its way (cleared, under g_sbMx, as it finishes)
struct SbPending { bool want = false; std::string user; bool quiet = true; } g_sbPending;   // the next fetch, waiting its turn

// quiet: the 2-minute check (not Import): nothing is shown unless the plan is new
void simbriefFetch(const std::string& user, bool quiet = false) {
    if (!quiet) { g_sim.plan.busy = true; g_sim.plan.status = "Asking SimBrief..."; }
    if (g_sbRunning) {   // one is on its way: this one after it (an Import waiting is not turned into a quiet check)
        const bool importWaiting = g_sbPending.want && !g_sbPending.quiet;
        g_sbPending.want = true; g_sbPending.user = user; g_sbPending.quiet = quiet && !importWaiting;
        return;
    }
    if (g_sbThread.joinable()) g_sbThread.join();   // (finished: it cleared g_sbRunning as its last step, so this does not wait)
    g_sbRunning = true;
    g_sbThread = std::thread(xpt::guarded("SimBrief", [user, quiet] {
        xpt::FlightPlan p;
        try {
            const std::string url = xpt::simbriefUrl(user);
            std::string body, err;
            long code = 0;
            if (!wx::httpGet(url, body, code, err, 25, 32u << 20)) {
                p.status = code == 400 ? "SimBrief has no plan for that ID" : "SimBrief could not be reached";
            } else {
                p = xpt::parseSimbrief(body);
            }
        } catch (const std::exception& e) {   // (caught here, not only by guarded: the page must hear that the fetch has ended)
            p = xpt::FlightPlan(); p.status = "SimBrief's answer could not be read";
            xpt::crashlog::threadFailed("SimBrief", e.what());
        } catch (...) {
            p = xpt::FlightPlan(); p.status = "SimBrief's answer could not be read";
            xpt::crashlog::threadFailed("SimBrief", "(not a std::exception)");
        }
        std::lock_guard<std::mutex> lk(g_sbMx);
        g_sbPlan = std::move(p); g_sbPlanUser = user; g_sbPlanQuiet = quiet; g_sbDone = true;
        g_sbRunning = false;
    }));
}

// The plan's route for the map: the departure, its fixes, the destination
void planToRoute() {
    xpt::planToRoute(g_sim.plan, g_sim.map);
}

// ---- Hoppie's ACARS: one worker sends what the tablet asks to and polls every minute while ACARS is on ------------------------
struct AcarsReq { std::string logon, from, to, type, packet; };
std::mutex g_acMx;
std::condition_variable g_acCv;
std::deque<AcarsReq> g_acQ;
std::vector<xpt::AcarsMsg> g_acIn;   // come up, not yet handed to the tablet
std::string g_acStatus;
std::thread g_acThread;
std::atomic<bool> g_acStop{false};
void acarsWorker() {
    for (;;) {
        AcarsReq r;
        {
            std::unique_lock<std::mutex> lk(g_acMx);
            g_acCv.wait(lk, [] { return g_acStop || !g_acQ.empty(); });
            if (g_acStop) return;
            r = g_acQ.front(); g_acQ.pop_front();
        }
        std::string body, err; long status = 0;
        const bool ok = wx::httpGet(xpt::acarsUrl(r.logon, r.from, r.to, r.type, r.packet), body, status, err, 20, 256 * 1024);
        std::vector<xpt::AcarsMsg> got; std::string perr;
        const bool parsed = ok && xpt::acarsParse(body, got, perr);
        std::lock_guard<std::mutex> lk(g_acMx);
        if (!ok) g_acStatus = "Hoppie unreachable" + (err.empty() ? std::string() : ": " + err);
        else if (!parsed) g_acStatus = "Hoppie: " + perr;
        else { if (r.type == "poll") g_acStatus = "Connected, checked every minute"; for (auto& m : got) g_acIn.push_back(m); }
    }
}
void acarsStep(const xpt::Actions& act) {
    static double lastPoll = -1e9;
    const double now = XPLMGetElapsedTime();
    const bool on = !act.acarsFrom.empty();
    if (on && !g_acThread.joinable()) { g_acStop = false; g_acThread = std::thread(xpt::guarded("ACARS", [] { acarsWorker(); })); }
    {
        std::lock_guard<std::mutex> lk(g_acMx);
        for (const auto& o : act.acarsOut) if (on) g_acQ.push_back({xpt::settings().hoppieCode, act.acarsFrom, o.to, o.type, o.packet});
        if (on && now - lastPoll > 60) { lastPoll = now; g_acQ.push_back({xpt::settings().hoppieCode, act.acarsFrom, "SERVER", "poll", ""}); }
        if (!on) { lastPoll = -1e9; g_acStatus.clear(); }
        for (auto& m : g_acIn) g_sim.acars.inbox.push_back(m);
        g_acIn.clear();
        g_sim.acars.status = g_acStatus;
    }
    g_acCv.notify_one();
}
// xPilot's own commands (its message window, nearby ATC...): looked for now and then (it may load after us), pressed for the tablet
void xpilotStep(const xpt::Actions& act) {
    // (its message window: xPilot 3 names it toggle_text_message_console; xPilot 4's name is not published, so the likely ones
    // are looked for too, and whichever is found is logged)
    static const char* const names[] = {"xpilot/toggle_text_message_console", "xpilot/toggle_messages", "xpilot/toggle_messages_window",
                                        "xpilot/toggle_message_window", "xpilot/toggle_text_messages", "xpilot/toggle_message_console",
                                        "xpilot/toggle_chat", "xpilot/toggle_nearby_atc", "xpilot/toggle_notification_panel", "xpilot/toggle_aircraft_labels"};
    // (by the clock, not by an exact frame number: the tablet is not drawn every frame, and a check on one frame number in 256
    // could fall on frames it never draws, so xPilot was never found, 2026-11-22)
    static float lookedAt = -100, readAt = -100;
    const float now = XPLMGetElapsedTime();
    if (now - lookedAt > 3.0f) {
        lookedAt = now;
        std::vector<std::string> found;
        for (const char* n : names) if (XPLMFindCommand(n)) found.push_back(n);
        if (found != g_sim.xpilotCmds) {
            g_sim.xpilotCmds = found;
            std::string list; for (const std::string& n : found) list += (list.empty() ? "" : ", ") + n;
            logMsg("xPilot: %d command(s) for the tablet: %s", (int)found.size(), list.c_str());
        }
        // its plugin, by its signature: running whatever its version names its commands (xPilot 4 renamed them, 2026-11-22)
        const bool loaded = XPLMFindPluginBySignature("org.vatsim.xpilot") != XPLM_NO_PLUGIN_ID;
        if (loaded != g_sim.xpilotLoaded) {
            g_sim.xpilotLoaded = loaded;
            if (loaded) {   // what this xPilot offers (its datarefs), once, in the log: for matching the tablet to a new version
                const int n = XPLMCountDataRefs();
                std::vector<XPLMDataRef> all((size_t)std::max(0, n));
                if (n > 0) XPLMGetDataRefsByIndex(0, n, all.data());
                std::string names;
                int count = 0;
                for (XPLMDataRef d : all) {
                    XPLMDataRefInfo_t info;
                    info.structSize = sizeof info;
                    XPLMGetDataRefInfo(d, &info);
                    if (info.name && std::strncmp(info.name, "xpilot/", 7) == 0 && count < 60) { names += (count ? ", " : "") + std::string(info.name); ++count; }
                }
                logMsg("xPilot is running (%d of its command(s) known to the tablet); its datarefs: %s", (int)found.size(), names.empty() ? "none" : names.c_str());
            }
        }
    }
    if (!act.xpilotCmd.empty()) if (XPLMCommandRef c = XPLMFindCommand(act.xpilotCmd.c_str())) XPLMCommandOnce(c);
    {   // its aircraft labels on or off: as xPilot saves them (its Config.json, written when they are switched, from its menu too)
        static float cfgAt = -100;
        static std::string cfgPath;
        if (g_sim.xpilotLoaded && (now - cfgAt > 2.0f || !act.xpilotCmd.empty())) {
            cfgAt = !act.xpilotCmd.empty() ? now - 1.5f : now;   // (pressed here: read again in half a second, once xPilot has saved it)
            if (cfgPath.empty()) {
                char path[2048] = "";
                XPLMGetPluginInfo(XPLMFindPluginBySignature("org.vatsim.xpilot"), nullptr, path, nullptr, nullptr);
                std::string p = path;   // .../plugins/xPilot/win_x64/xPilot.xpl -> .../plugins/xPilot/Resources/Config.json
                for (int up = 0; up < 2; ++up) { const size_t s = p.find_last_of("/\\"); if (s != std::string::npos) p.resize(s); }
                cfgPath = p.empty() ? std::string("-") : p + "/Resources/Config.json";
            }
            int labels = -1;
            if (std::FILE* f = std::fopen(cfgPath.c_str(), "rb")) {
                std::string body; char buf[4096]; size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (1u << 20)) body.append(buf, n);
                std::fclose(f);
                const size_t k = body.find("\"ShowAircraftLabels\"");
                if (k != std::string::npos) { size_t v = body.find(':', k); if (v != std::string::npos) { while (++v < body.size() && (body[v] == ' ' || body[v] == '\t')) {} labels = body.compare(v, 4, "true") == 0 ? 1 : body.compare(v, 5, "false") == 0 ? 0 : -1; } }
            }
            g_sim.xpilotLabels = labels;
        }
    }
    if (now - readAt > 0.5f) {   // its connection, callsign, tuned stations and aircraft count (xPilot's own datarefs)
        readAt = now;
        static XPLMDataRef st = nullptr, cs = nullptr, c1 = nullptr, c2 = nullptr, na = nullptr;
        if (!st && (g_sim.xpilotLoaded || !g_sim.xpilotCmds.empty())) {
            st = XPLMFindDataRef("xpilot/login/status"); cs = XPLMFindDataRef("xpilot/login/callsign");
            c1 = XPLMFindDataRef("xpilot/com1_station_callsign"); c2 = XPLMFindDataRef("xpilot/com2_station_callsign"); na = XPLMFindDataRef("xpilot/num_aircraft");
        }
        auto text = [](XPLMDataRef r) { char b[64] = ""; if (r) { const int n = XPLMGetDatab(r, b, 0, 63); b[std::max(0, std::min(63, n))] = 0; } return std::string(b); };
        g_sim.xpilotLogin = st && (g_sim.xpilotLoaded || !g_sim.xpilotCmds.empty()) ? XPLMGetDatai(st) : -1;
        g_sim.xpilotCallsign = text(cs); g_sim.xpilotCom1 = text(c1); g_sim.xpilotCom2 = text(c2);
        g_sim.xpilotAircraft = na ? XPLMGetDatai(na) : 0;
    }
}
void acarsStop() {
    { std::lock_guard<std::mutex> lk(g_acMx); g_acStop = true; g_acQ.clear(); }
    g_acCv.notify_all();
    if (g_acThread.joinable()) g_acThread.join();
}

// The plan's SID, STAR and approach for the map: built again when the plan (or its runways) changes, from the CIFP and the nav data
void proceduresStep() {
    const xpt::FlightPlan& p = g_sim.plan;
    const std::string key = p.loaded ? p.ofpId + "|" + p.origin + p.originRunway + p.sid + "|" + p.dest + p.destRunway + p.star + "|" + std::to_string(p.legs.size()) : std::string();
    static std::string built = "\x01";
    if (key == built) return;
    built = key;
    char sys[2048] = "";
    XPLMGetSystemPath(sys);
    const std::string root = sys;
    g_sim.map.procedures = xpt::buildProcedures(p, [&](const std::string& icao) {
        std::string t;
        if (!readFile(root + "Custom Data/CIFP/" + icao + ".dat", t)) readFile(root + "Resources/default data/CIFP/" + icao + ".dat", t);
        return t;
    }, [](const std::string& id, double nLat, double nLon, double& la, double& lo) {
        const bool hinted = nLat != 0 || nLon != 0;   // (not "near": a macro in the Windows headers)
        float fla = (float)nLat, flo = (float)nLon;
        const XPLMNavType types = id.size() == 4 && !hinted ? xplm_Nav_Airport : (XPLMNavType)(xplm_Nav_Fix | xplm_Nav_VOR | xplm_Nav_NDB | xplm_Nav_DME | xplm_Nav_Airport);
        const XPLMNavRef r = XPLMFindNavAid(nullptr, id.c_str(), hinted ? &fla : nullptr, hinted ? &flo : nullptr, nullptr, types);
        if (r == XPLM_NAV_NOT_FOUND) return false;
        float rla = 0, rlo = 0; char outId[32] = {};
        XPLMGetNavAidInfo(r, nullptr, &rla, &rlo, nullptr, nullptr, nullptr, outId, nullptr, nullptr);
        if (id != outId) return false;
        la = rla; lo = rlo;
        return true;
    });
    if (p.loaded) logMsg("map: %d procedure line(s) for %s %s / %s %s", (int)g_sim.map.procedures.size(), p.sid.c_str(), p.originRunway.c_str(), p.star.c_str(), p.destRunway.c_str());
}

void planStep(const xpt::Actions& act) {
    static bool divertPending = false;   // the plan being fetched is a diversion
    if (act.planImport && !xpt::settings().simbriefUser.empty() && !g_sim.plan.busy) { divertPending = false; simbriefFetch(xpt::settings().simbriefUser); }
    if (act.planDivert && g_sim.plan.loaded && !xpt::settings().simbriefUser.empty() && !g_sim.plan.busy) { divertPending = true; simbriefFetch(xpt::settings().simbriefUser); }
    if (act.planUndoDivert && g_sim.planOriginal.loaded) {   // the plan before the diversion again (the breadcrumbs carry on)
        g_sim.plan = g_sim.planOriginal;
        g_sim.planOriginal = xpt::FlightPlan();
        g_sim.plan.status = "Diversion undone";
        planToRoute();
    }
    {   // every 2 minutes with the SimBrief ID: a new plan is loaded and announced. Never with an ID still being typed (on its first
        // digit it would ask SimBrief for pilot "1", 2026-11-27): the first check comes 5 s after the ID was last changed and finished.
        static double next = 30;
        static std::string lastId;
        const double now = XPLMGetElapsedTime();
        const xpt::Settings& st = xpt::settings();
        if (st.simbriefUser != lastId) { lastId = st.simbriefUser; next = std::max(now + 5.0, 30.0); }
        if (xpt::simbriefIdTyping()) next = std::max(next, now + 5.0);
        bool idle;
        { std::lock_guard<std::mutex> lk(g_sbMx); idle = !g_sbDone; }
        idle = idle && !g_sbRunning && !g_sbPending.want;
        if (st.notifySimbrief && !st.simbriefUser.empty() && !g_sim.plan.busy && idle && now >= next) {
            next = now + 120;
            simbriefFetch(st.simbriefUser, true);
        }
    }
    if (act.planAccept || act.planDismiss || act.planReview) {
        const bool took = act.planAccept && g_sim.planPreview.loaded;
        const xpt::FlightPlan before = g_sim.plan;
        xpt::planDecide(g_sim, act);
        if (took && divertPending && before.loaded) {   // a diversion: the old plan kept for Undo, the trail goes on, the route from here
            divertPending = false;
            if (!g_sim.planOriginal.loaded) g_sim.planOriginal = before;
            const xpt::FlightPlan& orig = g_sim.planOriginal;
            g_sim.plan.divertKey = !orig.divertKey.empty() ? orig.divertKey : !orig.ofpId.empty() ? orig.ofpId : orig.origin + ">" + orig.dest + "@" + orig.importedAt;
            const xpt::MapState& m = g_sim.map;
            auto& legs = g_sim.plan.legs;
            if (m.havePos && !m.onGround && legs.size() >= 2) {   // airborne: the new route picks up where the aircraft is
                auto nm = [](double a1, double o1, double a2, double o2) { const double dy = (a2 - a1) * 60.0, dx = (o2 - o1) * 60.0 * std::cos(a1 * 3.14159265 / 180.0); return std::sqrt(dx * dx + dy * dy); };
                size_t best = 1; double bestDetour = 1e18;
                for (size_t i = 1; i < legs.size(); ++i) {
                    const double d = nm(m.lat, m.lon, legs[i - 1].lat, legs[i - 1].lon) + nm(m.lat, m.lon, legs[i].lat, legs[i].lon) - nm(legs[i - 1].lat, legs[i - 1].lon, legs[i].lat, legs[i].lon);
                    if (d < bestDetour) { bestDetour = d; best = i; }
                }
                xpt::FlightPlan::Leg here;
                here.ident = "PPOS"; here.lat = m.lat; here.lon = m.lon; here.type = "ppos";
                std::vector<xpt::FlightPlan::Leg> nl;
                nl.push_back(here);
                nl.insert(nl.end(), legs.begin() + (long)best, legs.end());
                legs.swap(nl);
            }
            g_sim.plan.status = "Diverted to " + g_sim.plan.dest;
        }
        if (took && !before.divertKey.empty() == false && !divertPending && g_sim.plan.divertKey.empty()) g_sim.planOriginal = xpt::FlightPlan();   // (a new flight: no diversion to undo)
        if (took) planToRoute();
    }
    if (act.planClear) {   // (a fetch on its way is left to finish: X-Plane does not wait for it)
        g_sbPending = SbPending();
        g_sim.planOriginal = xpt::FlightPlan();
        g_sim.plan = xpt::FlightPlan();
        g_sim.plan.status = "Cleared";
        planToRoute();
    }
    xpt::FlightPlan fetched; std::string fetchedUser; bool fetchedQuiet = false, got = false;
    {
        std::lock_guard<std::mutex> lk(g_sbMx);
        if (g_sbDone) { g_sbDone = false; got = true; fetched = std::move(g_sbPlan); g_sbPlan = xpt::FlightPlan(); fetchedUser = g_sbPlanUser; fetchedQuiet = g_sbPlanQuiet; }
    }
    if (!g_sbRunning && g_sbThread.joinable() && got) g_sbThread.join();   // (it has finished: no wait)
    const bool importWaiting = g_sbPending.want && !g_sbPending.quiet;
    if (got && (fetchedUser != xpt::settings().simbriefUser || (fetchedQuiet && importWaiting))) {
        // asked with an ID since changed, or a quiet check overtaken by an Import waiting its turn: not shown
        if (!fetchedQuiet && !importWaiting) { g_sim.plan.busy = false; g_sim.plan.status = std::string(); }
        logMsg("SimBrief: an answer for an earlier %s set aside", fetchedUser != xpt::settings().simbriefUser ? "pilot ID" : "check");
        got = false;
    }
    if (got) {
        const int seq = g_sim.newPlanSeq;
        if (fetched.loaded) {   // the SID's and STAR's crossing restrictions, from X-Plane's procedure files (your AIRAC's first)
            char sys[2048] = "";
            XPLMGetSystemPath(sys);
            const std::string root = sys;
            const int n = xpt::addCrossings(fetched, [&](const std::string& icao) {
                std::string t;
                if (!readFile(root + "Custom Data/CIFP/" + icao + ".dat", t)) readFile(root + "Resources/default data/CIFP/" + icao + ".dat", t);
                return t;
            });
            logMsg("SimBrief: %d crossing restriction(s) from the %s and %s procedures", n, fetched.sid.empty() ? "-" : fetched.sid.c_str(), fetched.star.empty() ? "-" : fetched.star.c_str());
        }
        xpt::planFetched(g_sim, fetched, fetchedQuiet);   // into the preview: the flight plan stays until its Import
        if (g_sim.newPlanSeq != seq) logMsg("SimBrief: new plan %s offered", fetched.ofpId.c_str());
        else if (!fetchedQuiet) logMsg("SimBrief: %s", fetched.loaded ? "plan fetched for review" : fetched.status.c_str());
    }
    if (!g_sbRunning && g_sbPending.want) {   // the fetch that waited its turn
        const SbPending next = g_sbPending;
        g_sbPending = SbPending();
        if (!next.user.empty()) simbriefFetch(next.user, next.quiet);
        else if (!next.quiet) g_sim.plan.busy = false;
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
    {   // from X-Plane's nav data as the map reads it: each name the one nearest the fix before it (X-Plane's own search takes the
        // nearest partial match, so a fix was dropped when another name holding it was closer), and the airways followed fix by fix
        std::vector<std::string> words;
        std::istringstream in(v.route);
        for (std::string w; in >> w;) { if (w == v.departure) continue; if (w == v.arrival) break; words.push_back(w); }
        std::vector<xpt::MapState::RoutePoint> mid;
        if (xpt::navdata::resolveRoute(la, lo, words, mid)) {
            out.insert(out.end(), mid.begin(), mid.end());
            if (!out.empty()) { la = (float)out.back().lat; lo = (float)out.back().lon; }
            if (!v.arrival.empty() && find(v.arrival, xplm_Nav_Airport, la, lo, false, rp)) out.push_back(rp);
            return out;
        }
    }
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
            // no call sign (X-Plane's AI): its type and its slot, so two of a type are not one aircraft (their trails were joined)
            if (t.callsign.empty()) t.callsign = (t.type.empty() ? std::string("TFC") : t.type) + " " + std::to_string(i);
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
// VATSIM's own CTAF for an airport (its AIP: the station marked as the CTAF when no ATC is online), asked once an airport. On VATSIM
// this is the frequency to use (it can differ from the real world's), and without one VATSIM's UNICOM, 122.800.
struct VatsimCtaf { bool done = false; double mhz = 0; std::string station; };
std::mutex g_vcMx;
std::map<std::string, VatsimCtaf> g_vatsimCtaf;
std::thread g_vcThread;
std::atomic<bool> g_vcBusy{false};   // one asked at a time (another airport waits its turn, never holding up the sim)
void vatsimCtafAsk(const std::string& icao) {
    if (g_vcBusy.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_vcMx);
        if (g_vatsimCtaf.count(icao)) return;
        g_vatsimCtaf[icao] = VatsimCtaf();
    }
    if (g_vcThread.joinable()) g_vcThread.join();   // (finished: not busy)
    g_vcBusy.store(true);
    g_vcThread = std::thread(xpt::guarded("VATSIM AIP", [icao] {
        std::string body, err;
        long status = 0;
        VatsimCtaf r;
        r.done = true;
        if (wx::httpGet("https://my.vatsim.net/api/v2/aip/airports/" + icao + "/stations", body, status, err, 10, 1u << 20) && status == 200) {
            // each station: {"callsign":..,"name":..,"frequency":..,"ctaf":true|false}; the one with ctaf true
            for (size_t at = body.find("\"ctaf\""); at != std::string::npos; at = body.find("\"ctaf\"", at + 6)) {
                size_t v = body.find(':', at);
                if (v == std::string::npos) break;
                while (++v < body.size() && body[v] == ' ') {}
                if (body.compare(v, 4, "true") != 0) continue;
                const size_t open = body.rfind('{', at), close = body.find('}', at);
                if (open == std::string::npos || close == std::string::npos) break;
                const std::string obj = body.substr(open, close - open);
                auto field = [&](const char* name) {
                    const size_t k = obj.find(std::string("\"") + name + "\"");
                    if (k == std::string::npos) return std::string();
                    size_t c = obj.find(':', k);
                    if (c == std::string::npos) return std::string();
                    ++c;
                    while (c < obj.size() && (obj[c] == ' ' || obj[c] == '"')) ++c;
                    size_t e = c;
                    while (e < obj.size() && obj[e] != '"' && obj[e] != ',' && obj[e] != '}') ++e;
                    return obj.substr(c, e - c);
                };
                const double mhz = std::atof(field("frequency").c_str());
                if (mhz > 100 && mhz < 140) { r.mhz = mhz; r.station = field("callsign"); break; }
            }
        }
        { std::lock_guard<std::mutex> lk(g_vcMx); g_vatsimCtaf[icao] = r; }
        g_vcBusy.store(false);
    }));
}
// the airport's CTAF as VATSIM has it, over the real world's (once VATSIM has answered)
void vatsimCtafApply(xpt::AirportInfo& a) {
    if (a.icao.empty() || !a.found) return;
    VatsimCtaf v;
    { std::lock_guard<std::mutex> lk(g_vcMx); auto it = g_vatsimCtaf.find(a.icao); if (it == g_vatsimCtaf.end()) return; v = it->second; }
    if (!v.done) return;
    a.vatsimCtaf = v.mhz > 0 ? v.mhz : 122.800;
    a.vatsimCtafNote = v.mhz > 0 ? "VATSIM AIP" + (v.station.empty() ? std::string() : " (" + v.station + ")") : "VATSIM AIP: none set, UNICOM 122.800";
}

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
    if (!g_aptWanted.empty()) { vatsimCtafAsk(g_aptWanted); if (g_sim.airport.vatsimCtaf <= 0) vatsimCtafApply(g_sim.airport); }
    else if (!g_sim.airport.busy && g_sim.airport.found && !g_aptMergedWithOA && xpt::ourairports::ready()) buildAirportInfo();
}

// ---- the Documents page ------------------------------------------------------------------------------------------------------------
// PDFs and pictures from plugins/XPTablet/docs, Output/XPTablet/Documents and AviTab's charts folder. Pictures are read on a thread.
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
    // the docs folder in XPTablet's own folder first (made when missing), then the older Output one; the aircraft's folder is no
    // longer looked in (its manuals filled the list)
    const std::string docs = pluginFolder() + "/docs";
    makeDirs(docs + "/");
    std::vector<Where> where = {{docs, "XPTablet docs", 4}, {mine, "XPTablet docs", 4}};
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
        g_docThread = std::thread(xpt::guarded("documents", [path] {
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
        }));
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
    XPLMBindTexture2d(g_docTex, 0); ++g_texChanges;
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

// ---- a Custom theme's picture: from plugins/XPTablet/backgrounds (listed when asked), loaded when the theme wants it --------------
int g_bgTex = 0;
void backgroundStep(const xpt::Actions& act) {
    namespace fs = std::filesystem;
    const std::string dir = pluginFolder() + "/backgrounds";
    static bool listed = false;
    if (act.backgroundsList || !listed) {
        listed = true;
        makeDirs(dir + "/");
        g_sim.backgroundsFolder = dir;
        g_sim.backgroundFiles.clear();
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().u8string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") g_sim.backgroundFiles.push_back(it->path().filename().u8string());
        }
        std::sort(g_sim.backgroundFiles.begin(), g_sim.backgroundFiles.end());
    }
    if (act.backgroundWanted == g_sim.bgPicture) return;
    g_sim.bgPicture = act.backgroundWanted;
    g_sim.bgTex = 0; g_sim.bgW = g_sim.bgH = 0; g_sim.bgAverage = -1; g_sim.bgAccent = -1;
    if (g_sim.bgPicture.empty() || g_sim.bgPicture.find_first_of("/\\") != std::string::npos) return;
    std::string bytes;
    if (!readFile(dir + "/" + g_sim.bgPicture, bytes)) { logMsg("background: could not read %s", g_sim.bgPicture.c_str()); return; }
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!p) { logMsg("background: not a picture: %s", g_sim.bgPicture.c_str()); return; }
    std::vector<uint8_t> img(p, p + (size_t)w * h * 4);
    stbi_image_free(p);
    while (w > 2048 && h > 2) {   // at most 2048 across: halved until it fits (the tablet is 800 across)
        const int nw = w / 2, nh = h / 2;
        std::vector<uint8_t> half((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c)
            half[((size_t)y * nw + x) * 4 + c] = (uint8_t)((img[((size_t)(2 * y) * w + 2 * x) * 4 + c] + img[((size_t)(2 * y) * w + 2 * x + 1) * 4 + c] +
                                                           img[((size_t)(2 * y + 1) * w + 2 * x) * 4 + c] + img[((size_t)(2 * y + 1) * w + 2 * x + 1) * 4 + c]) / 4);
        img.swap(half); w = nw; h = nh;
    }
    xpt::pictureColors(img.data(), w, h, g_sim.bgAverage, g_sim.bgAccent);   // its background (average, deeper) and accent (its strongest colour)
    if (!g_bgTex) XPLMGenerateTextureNumbers(&g_bgTex, 1);
    XPLMBindTexture2d(g_bgTex, 0); ++g_texChanges;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    XPLMBindTexture2d(0, 0);
    g_sim.bgTex = (ImTextureID)(intptr_t)g_bgTex; g_sim.bgW = w; g_sim.bgH = h;
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

// ---- Logbook: kept in Output/XPTablet/logbook.txt -------------------------------------------------------------------------------
std::string logbookPath() { char sys[1024] = {0}; XPLMGetSystemPath(sys); return std::string(sys) + "Output" + XPLMGetDirectorySeparator() + "XPTablet" + XPLMGetDirectorySeparator() + "logbook.txt"; }
void loadLogbook() { std::string t; if (readFile(logbookPath(), t)) xpt::logbookFromText(t); }
void saveLogbook() {
    std::string sys = logbookPath(); const std::string dir = sys.substr(0, sys.find_last_of("/\\"));
#if defined(_WIN32)
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
    FILE* f = std::fopen(sys.c_str(), "wb"); if (!f) return;
    const std::string t = xpt::logbookToText(); std::fwrite(t.data(), 1, t.size(), f); std::fclose(f);
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
    if (act.mediaPlayerMute >= 0) xpt::media::setMuted(false, act.mediaPlayerMute != 0);
    if (act.mediaMasterMute >= 0) xpt::media::setMuted(true, act.mediaMasterMute != 0);
    // the radios: xPilot says when COM1 or COM2 is receiving (looked for now and then: it may load after us)
    static XPLMDataRef com1 = nullptr, com2 = nullptr, rxOld = nullptr, ptt = nullptr, vu = nullptr;
    static float radioLookedAt = -100;   // (by the clock: the tablet is not drawn every frame)
    const float radioNow = XPLMGetElapsedTime();
    // X-Plane's own radios too: its ATC, ATIS and plugins' voices received on COM1 / COM2, and the pilot's voice sent
    static XPLMDataRef xpRx1 = XPLMFindDataRef("sim/atc/com1_rx"), xpRx2 = XPLMFindDataRef("sim/atc/com2_rx");
    static XPLMDataRef xpTx1 = XPLMFindDataRef("sim/atc/com1_tx"), xpTx2 = XPLMFindDataRef("sim/atc/com2_tx");
    if (radioNow - radioLookedAt > 3.0f && ((!com1 && !rxOld) || !ptt || !vu)) {
        radioLookedAt = radioNow;
        vu = XPLMFindDataRef("xpilot/audio/vu");   // xPilot 4: the level of what is heard now
        com1 = XPLMFindDataRef("xpilot/audio/com1_rx");
        com2 = XPLMFindDataRef("xpilot/audio/com2_rx");
        rxOld = XPLMFindDataRef("xpilot/audio/rx");
        ptt = XPLMFindDataRef("xpilot/ptt");   // 1 while push-to-talk is held
    }
    auto on = [](XPLMDataRef r) {
        if (!r) return false;
        return (XPLMGetDataRefTypes(r) & xplmType_Int) ? XPLMGetDatai(r) != 0 : XPLMGetDataf(r) > 0.5f;
    };
    // receiving: xPilot 4's level of what is heard (its com1_rx and com2_rx say only that a radio is set to receive: they stay on, and
    // the radio showed Receiving all the time, 2026-11-22); older xPilots: their receive flags
    bool pilotRx = false;
    if (vu) {
        static float heardUntil = -1;
        const float level = (XPLMGetDataRefTypes(vu) & xplmType_Float) ? XPLMGetDataf(vu) : (float)XPLMGetDatai(vu);
        if (level > 0.02f) heardUntil = radioNow + 0.4f;   // (held a moment over the gaps between words)
        pilotRx = radioNow < heardUntil;
    } else pilotRx = on(com1) || on(com2) || on(rxOld);
    // X-Plane's ATC: its com1_rx / com2_rx (and _tx) can stay on with nothing said (a radio set to receive), and the media was paused
    // for "X-Plane ATC" with no one talking (2026-11-22). So a flag counts only while it is fresh: one on for more than 20 seconds
    // without a break is a radio's setting, not a voice, until it goes off again. With xPilot connected, X-Plane's ATC is not used.
    auto fresh = [&](XPLMDataRef r, float& since) {
        const bool v = on(r);
        if (!v) { since = -1; return false; }
        if (since < 0) since = radioNow;
        return radioNow - since < 20.0f;
    };
    static float rx1Since = -1, rx2Since = -1, tx1Since = -1, tx2Since = -1;
    const bool xpilotOnline = g_sim.xpilotLogin > 0;
    const bool xpRx = !xpilotOnline && (fresh(xpRx1, rx1Since) | fresh(xpRx2, rx2Since));
    const bool xpTx = !xpilotOnline && (fresh(xpTx1, tx1Since) | fresh(xpTx2, tx2Since));
    // xPilot's push-to-talk: its dataref, and also its command itself (a key or button bound to it: some versions of xPilot do not
    // set the dataref then)
    static bool pttWatched = false;
    if (!pttWatched && radioLookedAt == radioNow) {   // (when the datarefs were just looked for)
        for (const char* name : {"xpilot/ptt", "xpilot/toggle_ptt"}) {
            if (XPLMCommandRef c = XPLMFindCommand(name)) {
                XPLMRegisterCommandHandler(c, [](XPLMCommandRef, XPLMCommandPhase phase, void* toggle) -> int {
                    xpt::crashlog::In crashIn_("xPilot's push-to-talk (watched)");
                    if (toggle) { if (phase == xplm_CommandBegin) g_pttCmd = !g_pttCmd; }
                    else g_pttCmd = phase != xplm_CommandEnd;
                    return 1;   // (xPilot still gets it)
                }, 1, (void*)(intptr_t)(std::strcmp(name, "xpilot/toggle_ptt") == 0));
                pttWatched = true;
            }
        }
    }
    const bool pilotTx = on(ptt) || g_pttCmd;
    const bool rx = pilotRx || xpRx, tx = pilotTx || xpTx;
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
    m.playerVolume = n.playerVolume; m.masterVolume = n.masterVolume; m.playerMuted = n.playerMuted; m.masterMuted = n.masterMuted;
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
            XPLMBindTexture2d(g_artTex, 0); ++g_texChanges;
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
    // OpenGL's functions are looked up before anything here uses them. Local Connect's picture came here first when the tablet had
    // never been drawn in the sim (no cockpit tablet in the aircraft, its window never opened) and a device connected: the fonts'
    // texture was then made through pointers still empty, a call to address 0 (X-Plane closed: 2026-11-22, the A330 at KMDW).
    if (!g_ctx || !loadGl()) return;
    const int cycle = XPLMGetCycleNumber();
    if (cycle == g_frameCycle) return;
    g_frameCycle = cycle;
    ImGui::SetCurrentContext(g_ctx);
    const bool winShown = g_win && XPLMGetWindowIsVisible(g_win);
    float ww = 800, wh = 480;
    if (winShown) windowLayout(ww, wh);
    float aspect;
    if (panelActive()) { float pl, pb, pw, ph; panelPicture(pl, pb, pw, ph); aspect = pw / std::max(1.0f, ph); }   // the picture's shape (to the rectangle's edges)
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
    if (g_mirrorScale > 0) need = std::max(need, g_mirrorScale);   // (a device is shown the tablet at this scale: the letters as fine)
    need = std::max(1.0f, std::min(3.0f, need));
    if (g_fontScale <= 0 || std::fabs(need - g_fontScale) > 0.08f * g_fontScale) buildFonts(need);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_logW, xpt::kLogicalH);
    const auto now = std::chrono::steady_clock::now();
    const float dt = g_lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - g_lastFrame).count() : 1.0f / 60.0f;
    io.DeltaTime = std::max(1e-4f, std::min(0.5f, dt));
    g_lastFrame = now;

    // the pointer: over the floating window (its cursor callback reports that), else on the cockpit tablet, else nowhere
    // (not while a paired device's finger is on it, nor while a press still waits its frame)
    if (!g_captureDown && cycle - g_winHoverCycle > 1 && XPLMGetElapsedTime() > g_remotePointerUntil && !xpt::pointerPending()) {
        float px = 0, py = 0;
        const bool onPanel = panelPointer(px, py);
        if (onPanel) xpt::queuePointer(px, py, -1);
        if (!onPanel && !ImGui::IsMouseDown(0)) xpt::queuePointer(-FLT_MAX, -FLT_MAX, -1);
    }

    g_sim.onPanel = panelActive();
    g_sim.panel[0] = g_panelLeft; g_sim.panel[1] = g_panelBottom; g_sim.panel[2] = g_panelWidth; g_sim.panel[3] = g_panelHeight;
    g_sim.panelEnabled = g_panelEnabled != 0; g_sim.panelPowered = g_panelPowered != 0;
    g_sim.brightness = g_brightness;
    g_sim.aviTabRunning = g_aviTabRunning;
    g_sim.pixelScale = g_fontScale;

    xpt::pumpPointer();   // (the pointer, whoever moved it: a step a frame, each press a frame after the pointer is on its spot)
    ImGui::NewFrame();
    xpt::Actions act;
    xpt::crashlog::In crashIn_("the tablet's frame: building its page");
    {   // (which page, in the record if it crashes)
        static char where[96];
        std::snprintf(where, sizeof where, "the tablet's frame: building its page (app %d)", xpt::currentApp());
        xpt::crashlog::here(where);
    }
    {   // a C++ error while the page is built: ImGui's stacks put back as they were after NewFrame, the page closed, nothing acted on
        ImGuiErrorRecoveryState rs;
        ImGui::ErrorRecoveryStoreState(&rs);
        const char* failed = nullptr; std::string why;
        try { xpt::build(g_fonts, g_sim, g_logW, xpt::kLogicalH, act); }
        catch (const std::exception& e) { failed = "x"; why = e.what(); }
        catch (...) { failed = "x"; why = "(not a std::exception)"; }
        if (failed) {
            callbackFailed(why.c_str());
            ImGui::ErrorRecoveryTryToRecoverState(&rs);
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) if (w->DrawList->_Splitter._Count > 1) w->DrawList->ChannelsMerge();
            act = xpt::Actions();
            xpt::recoverAfterError();
        }
    }
    xpt::crashlog::here("the tablet's frame: drawing it and acting on its presses");
    ImGui::Render();
    xpt::clipDrawData(ImGui::GetDrawData(), g_batches);
#if defined(XPT_LOCAL_CONNECT)
    // Local Connect: a device forgotten, a pairing declined
    if (!act.localForget.empty()) xpt::localserver::forget(act.localForget);
    if (act.localPairDeny) xpt::localserver::denyPairing();
#endif
    {   // the Navigation page's search: X-Plane's navaids and fixes (airports: the Airports app) whose code starts with it, or whose name holds it
        static std::string last;
        if (act.navSearch != last) {
            last = act.navSearch;
            g_sim.navHits.clear();
            if (last.size() >= 2) {
                struct Hit { xpt::SimState::NavHit h; int rank; double d; };
                std::vector<Hit> hits;
                const int want = xplm_Nav_NDB | xplm_Nav_VOR | xplm_Nav_ILS | xplm_Nav_Localizer | xplm_Nav_Fix | xplm_Nav_DME;
                for (XPLMNavRef r = XPLMGetFirstNavAid(); r != XPLM_NAV_NOT_FOUND; r = XPLMGetNextNavAid(r)) {
                    XPLMNavType type = xplm_Nav_Unknown;
                    float la = 0, lo = 0, hgt = 0, hdg = 0;
                    int freq = 0;
                    char id[32] = "", name[256] = "";
                    XPLMGetNavAidInfo(r, &type, &la, &lo, &hgt, &freq, &hdg, id, name, nullptr);
                    if (!(type & want)) continue;
                    const std::string ids = id;
                    int rank = -1;
                    if (ids == last) rank = 0;
                    else if (ids.compare(0, last.size(), last) == 0) rank = 1;
                    else if (last.size() >= 3) {
                        std::string up = name;
                        for (char& c : up) c = (char)std::toupper((unsigned char)c);
                        if (up.find(last) != std::string::npos) rank = 2;
                    }
                    if (rank < 0) continue;
                    Hit h;
                    h.h.type = (int)type; h.h.ident = ids; h.h.name = name; h.h.lat = la; h.h.lon = lo; h.h.freq = (float)freq;
                    h.rank = rank;
                    const double dl = (la - g_sim.map.lat), dlo = (lo - g_sim.map.lon) * std::cos(g_sim.map.lat * 0.0174533);
                    h.d = g_sim.map.havePos ? dl * dl + dlo * dlo : 0;
                    hits.push_back(std::move(h));
                    if (hits.size() > 4000) break;
                }
                std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rank != b.rank ? a.rank < b.rank : a.d < b.d; });
                for (size_t i = 0; i < hits.size() && i < 60; ++i) g_sim.navHits.push_back(hits[i].h);
            }
        }
    }
    if (act.com1Standby > 0) {   // the headset on the Airports page: that frequency into COM1's standby (8.33 kHz steps, in kHz)
        static XPLMDataRef c = XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833");
        if (c) XPLMSetDatai(c, (int)std::lround(act.com1Standby * 1000.0));
    }
    if (!act.metarSearch.empty()) { xpt::metarRequest(act.metarSearch); g_sim.metar = xpt::MetarLookup(); g_sim.metar.icao = act.metarSearch; g_sim.metar.busy = true; }
    if (act.ground != xpt::GA_NONE) g_pendingGround = act;   // carried out from the flight loop, not from inside drawing
    browserStep(act);
    mapStep(act);
    planStep(act);
    proceduresStep();
    acarsStep(act);
    xpilotStep(act);
    mediaStep(act);
    backgroundStep(act);
    {   // the Map's traffic card: its aircraft on VATSIM (and its filed route); VATSIM's world as traffic when chosen
        const bool world = act.mapShown && xpt::settings().mapTraffic && xpt::settings().mapTrafficSource == 1;
        // VATSIM's controllers: for the Airports page, the Map's ATC layer, and the notifications for the flight's airports
        const bool atc = act.airportsShown || (act.mapShown && (xpt::settings().mapVatsimAtc || xpt::settings().mapAirspaces)) || (xpt::settings().vatsimAtcNotes && g_sim.plan.loaded);
        if (act.mapShown && xpt::settings().mapAirspaces) {   // VATSIM's airspaces: read once (kept a week in Output/XPTablet/vatspy)
            char sys[2048] = "";
            XPLMGetSystemPath(sys);
            xpt::airspace::start(std::string(sys) + "Output/XPTablet/vatspy");
            g_sim.map.airspaces = xpt::airspace::get(g_sim.map.airspaceStatus);
        }
        xpt::vatsim::want((act.mapShown && !act.trafficSelected.empty()) || world || atc);
        if ((act.mapShown && !act.trafficSelected.empty()) || world || atc) xpt::vatsim::start();
        static double lastAtc = -100;
        if (XPLMGetElapsedTime() - lastAtc > 5.0) { lastAtc = XPLMGetElapsedTime(); g_sim.vatsimAtc = xpt::vatsim::controllers(&g_sim.vatsimAtcHave); }
        if (act.mapShown && !act.trafficSelected.empty()) {
            g_sim.map.vatsim = xpt::vatsim::lookup(act.trafficSelected, g_sim.map.vatsimStatus);
            static std::string routeKey;
            const xpt::VatsimPilot& v = g_sim.map.vatsim;
            const std::string key = v.found && v.hasPlan && xpt::settings().mapTrafficRoute ? v.callsign + "|" + v.departure + "|" + v.route + "|" + v.arrival : std::string();
            static bool routeFull = false;   // (resolved with the map's nav data: else asked again once it has been read)
            if (key != routeKey || (!key.empty() && !routeFull)) {
                routeKey = key;
                routeFull = key.empty() || xpt::navdata::ready();
                g_sim.map.trafficRoute = key.empty() ? std::vector<xpt::MapState::RoutePoint>() : resolveRoute(v);
            }
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
    { static float t = -100; if (every(t, 0.25f)) dataInfoStep(); }
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
    xpt::crashlog::In crashIn_("drawing the cockpit tablet");
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
    static float lastX = -FLT_MAX, lastY = -FLT_MAX;   // where the pointer last was on the tablet
    float x = 0, y = 0;
    const bool on = panelPointer(x, y);
    if (status == xplm_MouseDown) {
        if (!on) return false;
        lastX = x; lastY = y;
        g_lastSimDown = XPLMGetElapsedTime();
        xpt::queuePointer(x, y, 1);   // (queued: the press lands a frame after the pointer is there, see pumpPointer)
        g_captureDown = true;
        return true;
    }
    if (!g_captureDown) return false;
    if (on) { lastX = x; lastY = y; }   // dragged off the tablet: the pointer stays where it left
    if (status == xplm_MouseUp) { xpt::queuePointer(lastX, lastY, 0); g_captureDown = false; }
    else if (on) xpt::queuePointer(x, y, -1);
    return true;
}

void captureDraw(XPLMWindowID, void*) {}
int captureClick(XPLMWindowID, int, int, XPLMMouseStatus status, void*) {
    xpt::crashlog::In crashIn_("a click on the cockpit tablet"); return (g_captureClicks && panelClick(status)) ? 1 : 0; }
int capturePass(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; }
XPLMCursorStatus captureCursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
int captureWheel(XPLMWindowID, int, int, int wheel, int clicks, void*) {
    xpt::crashlog::In crashIn_("the wheel on the cockpit tablet");
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
// Backspace held down on the computer's keyboard deletes on (as a text box does): after a moment, a character every 1/20 s, while X-Plane
// does not repeat it itself (if it does, its repeats are used)
double g_bsDownAt = -1, g_bsNext = -1;
bool g_bsOsRepeats = false;
void backspaceHeld(bool down, bool up) {
    const double now = XPLMGetElapsedTime();
    if (down) { if (g_bsDownAt >= 0) g_bsOsRepeats = true; else { g_bsDownAt = now; g_bsNext = now + 0.45; g_bsOsRepeats = false; } }
    if (up) g_bsDownAt = -1;
}
void backspaceStep() {
    if (g_bsDownAt < 0 || g_bsOsRepeats) return;
    const double now = XPLMGetElapsedTime();
    if (now < g_bsNext) return;
    g_bsNext = now + 0.05;
    if (xpt::tabletTyping()) xpt::tabletKey(8, false);
    else if (xpt::webTyping() && g_webShown && xpt::browser::started()) { xpt::browser::push(xptb::CMD_KEY, 0, 0, 8, 1); xpt::browser::push(xptb::CMD_KEY, 0, 0, 8, 0); }
    else g_bsDownAt = -1;
}
int keySniffer(char key, XPLMKeyFlags flags, char vkey, void*) {
    xpt::crashlog::In crashIn_("a key");
    if (xpt::gameWantsKeys() && !(flags & (xplm_ControlFlag | xplm_OptionAltFlag))) {   // 2048 pressed: W A S D and the arrows play it
        const unsigned char vk = (unsigned char)vkey;
        const char c = (char)std::tolower((unsigned char)key);
        const int dir = c == 'a' || vk == 37 ? 0 : c == 'd' || vk == 39 ? 1 : c == 'w' || vk == 38 ? 2 : c == 's' || vk == 40 ? 3 : c == ' ' || vk == 32 ? 4 : -1;
        if (dir >= 0) {   // pressed and let go (the arcade games hold them); X-Plane does not see them meanwhile
            if (flags & xplm_DownFlag) xpt::gameKeyEvent(dir, true);
            if (flags & xplm_UpFlag) xpt::gameKeyEvent(dir, false);
            return 0;
        }
    }
    if (!xpt::settings().physicalKeys) return 1;
    if ((flags & xplm_ControlFlag) && !(flags & xplm_OptionAltFlag) && (xpt::tabletTyping() || (xpt::webTyping() && g_webShown && xpt::browser::started()))) {
        // copy, cut, paste and select all, with the computer's clipboard: in the page's field, or the tablet's own box (its whole text)
        const unsigned char vk = (unsigned char)vkey;
        const int what = vk == 'C' ? 1 : vk == 'X' ? 2 : vk == 'V' ? 3 : vk == 'A' ? 4 : 0;
        if (what) {
            if (flags & xplm_DownFlag) {
                if (!xpt::tabletTyping()) xpt::browser::push(xptb::CMD_EDIT, what);
                else if (what == 3) { std::string t; if (xpt::clipboard::get(t)) xpt::pasteText(t); }
                else if (what == 1 || what == 2) { const std::string t = xpt::typedText(); if (!t.empty()) { xpt::clipboard::set(t); if (what == 2) xpt::clearTyped(); } }
            }
            return 0;
        }
    }
    if ((unsigned char)vkey == 8) backspaceHeld((flags & xplm_DownFlag) != 0, (flags & xplm_UpFlag) != 0);
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
    xpt::crashlog::In crashIn_("a key in the window");
    if (losingFocus) return;
    const unsigned char vk = (unsigned char)vkey, ch = (unsigned char)key;
    const bool down = (flags & xplm_DownFlag) != 0, up = (flags & xplm_UpFlag) != 0;
    const bool editKey = vk == 8 || vk == 9 || vk == 13 || vk == 27 || (vk >= 33 && vk <= 40) || vk == 46;
    if ((flags & xplm_ControlFlag) && !(flags & xplm_OptionAltFlag)) {   // copy, cut, paste, select all (as in keySniffer)
        const int what = vk == 'C' ? 1 : vk == 'X' ? 2 : vk == 'V' ? 3 : vk == 'A' ? 4 : 0;
        if (what) {
            if (!down) return;
            if (xpt::tabletTyping()) {
                if (what == 3) { std::string t; if (xpt::clipboard::get(t)) xpt::pasteText(t); }
                else if (what == 1 || what == 2) { const std::string t = xpt::typedText(); if (!t.empty()) { xpt::clipboard::set(t); if (what == 2) xpt::clearTyped(); } }
            } else if (g_webShown && xpt::browser::started()) xpt::browser::push(xptb::CMD_EDIT, what);
            return;
        }
    }
    if (vk == 8) backspaceHeld(down, up);
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
        p.drawWindowFunc = Safe<captureDraw, 0>::call;
        p.handleMouseClickFunc = Safe<captureClick, 0>::call;
        p.handleRightClickFunc = Safe<capturePass, 0>::call;
        p.handleMouseWheelFunc = Safe<captureWheel, 0>::call;
        p.handleKeyFunc = Safe<noKey, 0>::call;
        p.handleCursorFunc = Safe<captureCursor, 0>::call;
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
    xpt::crashlog::In crashIn_("drawing the window");
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
    xpt::crashlog::In crashIn_("a click in the window");
    if (!g_ctx) return 1;
    ImGui::SetCurrentContext(g_ctx);
    float lx, ly;
    winToLayout(x, y, lx, ly);
    if (status == xplm_MouseDown) g_lastSimDown = XPLMGetElapsedTime();
    // queued (see pumpPointer): a press that comes with its position, the sim not having reported the pointer there the frame
    // before, moves the pointer first and lands on the next frame. Given straight to Dear ImGui such presses were lost on the
    // home screen's icons (2026-11-21: apps opened only after many clicks, at a low frame rate)
    xpt::queuePointer(lx, ly, status == xplm_MouseDown ? 1 : status == xplm_MouseUp ? 0 : -1);
    if (status == xplm_MouseDown && g_wantKeys && g_win && !XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(g_win);
    g_winHoverCycle = XPLMGetCycleNumber();
    return 1;
}
int winRightClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }
int winWheel(XPLMWindowID, int x, int y, int wheel, int clicks, void*) {
    xpt::crashlog::In crashIn_("the wheel in the window");
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
        xpt::queuePointer(lx, ly, -1);   // (in the same queue as the presses, so each stays in its order)
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
        p.drawWindowFunc = Safe<winDraw, 0>::call;
        p.handleMouseClickFunc = Safe<winClick, 1>::call;
        p.handleRightClickFunc = Safe<winRightClick, 1>::call;
        p.handleMouseWheelFunc = Safe<winWheel, 1>::call;
        p.handleKeyFunc = Safe<winKey, 0>::call;
        p.handleCursorFunc = Safe<winCursor, 0>::call;
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
void setInt(void* r, int v) {
    int* p = (int*)r;
    // an aircraft switching its tablet on or off (its own pages in turn with the tablet's): the first of them in the log, which
    // shows how that aircraft works its tabs, or that it never does
    if ((p == &g_panelEnabled || p == &g_panelPowered) && g_panelWritesLogged < 30) {   // (every write, the same value too)
        ++g_panelWritesLogged;
        logMsg("the aircraft set the cockpit tablet %s to %d (was %d)", p == &g_panelEnabled ? "enabled" : "powered", v, *p);
    }
    *p = v;
}
float getFloat(void* r) { return *(float*)r; }
void setBrightness(void*, float v) { g_brightness = std::max(0.0f, std::min(1.0f, v)); }
int getOwnsAviTab(void*) { return g_ownsAviTab ? 1 : 0; }
int getInMenu(void*) { return xpt::currentApp() == xpt::APP_HOME ? 1 : 0; }
// avitab/is_in_menu while XPTablet answers to AviTab's signature: never "in its menu". Add-ons that draw their own icon on AviTab's
// home screen take XPTablet for AviTab then (WXPlane up to 5.1.9 drew its WX icon over the home screen, 2026-11-26); they draw
// only when this says the menu shows. xptablet/is_in_menu says how it is.
int getInMenuAsAviTab(void*) { return g_tookAviTabSig ? 0 : getInMenu(nullptr); }
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
    regIntFn(prefix + "/is_in_menu", prefix == "avitab" ? getInMenuAsAviTab : getInMenu);
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

int onCommand(XPLMCommandRef cmd, XPLMCommandPhase phase, void* ref) {
    xpt::crashlog::In crashIn_("a command of XPTablet");
    const int id = (int)(intptr_t)ref;
    {   // the first of them in the log (how an aircraft works its tablet: its own pages in turn with this one)
        static int logged = 0;
        if (phase == xplm_CommandBegin && logged < 40) {
            for (const Cmd& c : g_cmds) if (c.ref == cmd) { ++logged; logMsg("command %d sent to the tablet (id %d)", logged, id); break; }
        }
    }
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
    XPLMRegisterCommandHandler(c, Safe<onCommand, 1>::call, 1, (void*)(intptr_t)id);
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
        r.landingLights = XPLMFindDataRef("sim/cockpit/electrical/landing_lights_on");
        r.taxiLight = XPLMFindDataRef("sim/cockpit/electrical/taxi_light_on");
    }
    // other plugins' datarefs appear when they load (an aircraft's own plugin after the aircraft): looked for until found
    if (!r.jwStatus) r.jwStatus = XPLMFindDataRef("opensam/jetway/status");
    if (!r.jwDoorStatus) r.jwDoorStatus = XPLMFindDataRef("opensam/jetway/door/status");
    if (!r.jwNumber) r.jwNumber = XPLMFindDataRef("opensam/jetway/number");
    if (!r.bpStarted) r.bpStarted = XPLMFindDataRef("bp/started");
    if (!r.bpConnected) r.bpConnected = XPLMFindDataRef("bp/connected");
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

// An aircraft with a cockpit tablet (AviTab.json): its own datarefs about its tablet (named tablet, efb, ipad, pad or avitab, not
// ours), watched for a while after it loads and their changes put in the log: how it switches its own pages in turn with this one
// (the DA42 never touches avitab/panel_enabled, 2026-11-26: its pages could not be reached over this one).
void tabletPagesWatchStep() {
    struct W { XPLMDataRef d; std::string name; bool isInt; float last; };
    static std::vector<W> watched;
    static double loadedAt = -1, lastLook = 0;
    static int logged = 0;
    static std::string forAircraft;
    const double now = XPLMGetElapsedTime();
    char file[512] = "", path[2048] = "";
    XPLMGetNthAircraftModel(0, file, path);
    if (forAircraft != path) { forAircraft = path; watched.clear(); loadedAt = now; logged = 0; }
    if (g_panelWidth <= 0 || loadedAt < 0) return;
    if (now - loadedAt < 15) return;
    if (now - loadedAt > 1800) return;   // (half an hour of it)
    if (loadedAt > 0 && watched.empty()) {
        loadedAt = -loadedAt;   // (looked once)
        const int n = XPLMCountDataRefs();
        std::vector<XPLMDataRef> all((size_t)std::max(0, n));
        if (n > 0) XPLMGetDataRefsByIndex(0, n, all.data());
        const XPLMPluginID me = XPLMGetMyID();
        for (XPLMDataRef d : all) {
            if (!d) continue;
            XPLMDataRefInfo_t info; std::memset(&info, 0, sizeof info); info.structSize = sizeof info;
            XPLMGetDataRefInfo(d, &info);
            if (!info.name || info.owner == me) continue;
            std::string lo; for (const char* c = info.name; *c; ++c) lo += (char)std::tolower((unsigned char)*c);
            if (lo.compare(0, 4, "sim/") == 0) continue;
            if (lo.find("tablet") == std::string::npos && lo.find("efb") == std::string::npos && lo.find("ipad") == std::string::npos && lo.find("avitab") == std::string::npos && lo.find("/pad") == std::string::npos) continue;
            const int t = info.type;
            if (!(t & (xplmType_Int | xplmType_Float))) continue;
            W w{d, info.name, (t & xplmType_Int) != 0, 0};
            w.last = w.isInt ? (float)XPLMGetDatai(d) : XPLMGetDataf(d);
            watched.push_back(w);
            logMsg("tablet pages: watching %s (%s) = %g", info.name, w.isInt ? "int" : "float", w.last);
            if (watched.size() >= 40) break;
        }
        if (watched.empty()) logMsg("tablet pages: the aircraft has no datarefs about its tablet");
        lastLook = now;
        return;
    }
    if (now - lastLook < 0.5 || logged >= 300) return;
    lastLook = now;
    for (W& w : watched) {
        const float v = w.isInt ? (float)XPLMGetDatai(w.d) : XPLMGetDataf(w.d);
        if (v != w.last && std::fabs(v - w.last) > 1e-4f) { logMsg("tablet pages: %s %g -> %g", w.name.c_str(), w.last, v); w.last = v; if (++logged >= 300) break; }
    }
}

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
    g.pushSource = XPLMFindCommand("BetterPushback/start") != nullptr ? 1 : 0;   // (BetterPushback's commands: it is installed)
    g.bpStarted = g.pushSource == 1 && r.bpStarted && XPLMGetDatai(r.bpStarted) != 0;
    g.bpConnected = g.pushSource == 1 && r.bpConnected && XPLMGetDatai(r.bpConnected) != 0;
    g.landingLights = r.landingLights && XPLMGetDatai(r.landingLights) != 0;
    g.taxiLight = r.taxiLight && XPLMGetDatai(r.taxiLight) != 0;
    g.jetway = g.openSam ? XPLMGetDatai(r.jwStatus) : 0;
    // SAM jetways here that only openSAM moves, and openSAM not running: why, as near as can be told
    g.samNear = 0; g.samProblem = 0;
    if (!g.openSam) {
        static XPLMDataRef la = XPLMFindDataRef("sim/flightmodel/position/latitude"), lo = XPLMFindDataRef("sim/flightmodel/position/longitude");
        const xpt::sam::Install in = xpt::sam::install();
        if (in.scanned && la && lo) {
            std::string nm;
            g.samNear = xpt::sam::nearby(XPLMGetDatad(la), XPLMGetDatad(lo), &nm);
            if (g.samNear) {
                g.samName = nm;
                g.samProblem = !in.pluginFolder ? 1 : in.libraryBelowGlobal ? 2 : !in.library ? 4 : 3;
                static std::string told;
                if (told != nm) { told = nm; logMsg("openSAM: SAM jetways %s (%s), openSAM %s", g.samNear == 2 ? "at this stand" : "at this airport", nm.c_str(),
                                                   g.samProblem == 1 ? "not installed" : g.samProblem == 2 ? "not running: openSAM_Library is below Global Airports in scenery_packs.ini" : g.samProblem == 4 ? "not running: openSAM_Library is not in scenery_packs.ini" : "installed but not running"); }
            }
        }
    }
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
    g.jwReach = g.openSam && r.jwNumber ? XPLMGetDatai(r.jwNumber) : -1;
    // the doors a jetway serves on other aircraft: as many as openSAM drives (X-Plane's own: one bubble, every boarding door)
    if (!g.toliss) {
        g.jwDoors = 1;
        if (g.openSam && r.jwNumber) g.jwDoors = std::max(1, std::min(3, XPLMGetDatai(r.jwNumber)));
        // (read into room for more than asked: an accessor that fills its whole array whatever the count asked for cannot run
        // past the end of it here)
        int docked[16] = {0};
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
        int docked[16] = {0};   // (room to spare, as below)
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

bool g_sendingCmd = false;   // a command of ours is being sent (what it sets off is ours too)
void once(const char* cmd) {
    const XPLMCommandRef c = XPLMFindCommand(cmd);
    g_sendingCmd = true;
    if (c) XPLMCommandOnce(c); else logMsg("command %s not found", cmd);
    g_sendingCmd = false;
}
// openSAM and X-Plane's own jetways (a stand with no SAM jetway): openSAM works them with X-Plane's toggle, and its own automatic
// docking presses that toggle again when the beacon goes off. With the jetway already docked from the tablet, that press sent it
// away (2026-11-22, KDTW A50). So X-Plane's toggle is watched: pressed by someone else just after the beacon went off, while the
// tablet has the jetway docked, it is docked again (through openSAM, which keeps its own count of it).
double g_beaconOffAt = -100, g_redockAt = -1;
int jetwayToggleSeen(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    xpt::crashlog::In crashIn_("X-Plane's jetway command (watched)");
    if (phase != xplm_CommandBegin || g_sendingCmd || !g_sim.ground.openSam) return 1;
    bool docked = false;
    for (bool w : g_refs.wantDoor) docked = docked || w;
    const double now = XPLMGetElapsedTime();
    if (docked && now - g_beaconOffAt < 6.0 && g_redockAt < 0) {
        g_redockAt = now + 1.5;   // (after openSAM's own press has gone through)
        logMsg("jetway: openSAM's automatic docking pressed X-Plane's jetway toggle after the beacon went off; docking again");
    }
    return 1;   // (it goes on: openSAM keeps its count)
}
void jetwayWatchStep() {
    static bool watching = false, beaconWas = false;
    if (!watching) if (XPLMCommandRef c = XPLMFindCommand("sim/ground_ops/jetway")) { XPLMRegisterCommandHandler(c, Safe<jetwayToggleSeen, 1>::call, 1, nullptr); watching = true; }
    const double now = XPLMGetElapsedTime();
    const bool beacon = g_sim.ground.beacon;
    if (beaconWas && !beacon) g_beaconOffAt = now;
    beaconWas = beacon;
    if (g_redockAt >= 0 && now >= g_redockAt) { g_redockAt = -1; if (g_sim.ground.openSam) once("openSAM/dock_jwy"); }
}
void jetwayWatchStop() { if (XPLMCommandRef c = XPLMFindCommand("sim/ground_ops/jetway")) XPLMUnregisterCommandHandler(c, Safe<jetwayToggleSeen, 1>::call, 1, nullptr); }
void toggleInt(XPLMDataRef ref) { if (ref) XPLMSetDatai(ref, XPLMGetDatai(ref) ? 0 : 1); }

// What was pressed on the Ground page.
// The slides' Auto: as ToLiss does it, armed once the beacon is on with every cabin door shut, disarmed parked with the beacon and
// engines off. Sends ToLiss's own arm / disarm commands, at most once every few seconds.
bool g_slidesAuto = false;
// Arm or Disarm pressed: held so (the aircraft's own arming or disarming undone at once): -1 disarmed, 1 armed (on the ground,
// with the doors shut), 0 none (Auto, or the aircraft's way). Checked every frame, straight from the slides' dataref, so a slide
// the aircraft arms is disarmed again before it is drawn (checked every few frames, it flickered).
int g_slidesHold = 0;
// XPTablet's cabin light Auto: the level follows the sun as the tablet's auto dim does, all the way down: 100 % with the sun up,
// 0 % once it is 18 degrees below the horizon (full night), in 10 % steps; written only when the step changes (a few times a dusk)
void cabinAutoStep() {
    const SimRefs& r = g_refs;
    if (!xpt::settings().cabinAuto || !g_sim.ground.toliss || (!r.cabinPerc && !r.cabinLevel)) return;
    static double lastAt = -100;
    if (XPLMGetElapsedTime() - lastAt < 2.0) return;
    lastAt = XPLMGetElapsedTime();
    static XPLMDataRef sun = XPLMFindDataRef("sim/graphics/scenery/sun_pitch_degrees");
    if (!sun) return;
    const float t = std::max(0.0f, std::min(1.0f, -XPLMGetDataf(sun) / 18.0f));
    const int pct = (int)std::lround((1.0f - t) * 10.0f) * 10;
    const int now = r.cabinPerc ? (int)std::lround(readNum(r.cabinPerc)) : -1;
    if (now == pct) return;
    if (r.cabinAuto && XPLMCanWriteDataRef(r.cabinAuto)) writeNum(r.cabinAuto, 0.0f);   // (ToLiss's own auto would fight it)
    if (r.cabinLevel && XPLMCanWriteDataRef(r.cabinLevel)) writeNum(r.cabinLevel, pct == 0 ? 0.0f : pct <= 50 ? 1.0f : 2.0f);
    if (r.cabinPerc && XPLMCanWriteDataRef(r.cabinPerc)) writeNum(r.cabinPerc, (float)pct);
}
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
    // Auto: ToLiss arms and disarms them on its own; XPTablet leaves them alone (it only holds them when Arm or Disarm is chosen)
    (void)g;
    return;
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
    case xpt::GA_JETWAY_SET: {   // the jetway menu's choice: these doors (a mask), the others not
        // openSAM docks every jetway it gave the stand's doors with dock_jwy (another plugin cannot pick one: its own window does);
        // the doors not chosen stay shut and show no jetway, so only the chosen door is used
        SimRefs& rr = g_refs;
        bool before = false, after = false;
        for (bool w : rr.wantDoor) before = before || w;
        for (int k = 0; k < 3; ++k) { const bool want = (act.groundIndex >> k) & 1; if (want != rr.wantDoor[k]) rr.jwPressT[k] = XPLMGetElapsedTime(); rr.wantDoor[k] = want; }
        for (bool w : rr.wantDoor) after = after || w;
        if (!before && after && g_undockAt >= 0) g_undockAt = -1;
        else if (!before && after) {
            if (g.openSam) once("openSAM/dock_jwy"); else { once("sim/ground_ops/jetway"); rr.xpJetway = true; rr.xpJetwayT0 = XPLMGetElapsedTime(); }
        } else if (before && !after) {
            if (g.toliss && rr.paxDoor) g_undockAt = XPLMGetElapsedTime() + 6.0; else undockNow();
        }
        logMsg("jetway: doors chosen 0x%x (%s jetways)", act.groundIndex, g.openSam ? "openSAM" : "X-Plane's");
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
    case xpt::GA_BP_PLAN: once("BetterPushback/start_planner"); break;     // its planner: the route drawn on the ground, then Done
    case xpt::GA_BP_CONNECT: once("BetterPushback/connect_first"); break;  // the tug drives up and connects before the route is planned
    case xpt::GA_BP_START: once("BetterPushback/start"); break;
    case xpt::GA_BP_STOP: once("BetterPushback/stop"); break;
    case xpt::GA_BP_DISCONNECT: once("BetterPushback/disconnect"); break;   // the tow bar and headset off (hand signals from then on)
    case xpt::GA_BP_RECONNECT: once("BetterPushback/reconnect"); break;     // the tow back on, waiting
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

// ---- Local Connect: the tablet in the sim, mirrored on phones and tablets on the home network. src/localserver.cpp serves the page
// and the connections; src/localmirror.cpp sends the tablet's picture as it changes; a device's touches are fed to the tablet here
// as the mouse's are. Off until switched on in Settings; it listens on the computer's home-network address only.
void ensureFrame();
#if defined(XPT_LOCAL_CONNECT)
bool g_localRunning = false;

// the few OpenGL functions the picture is taken with (the tablet drawn into a texture of its own, and read back)
struct GLMirror {
    void(XPT_GL* GenFramebuffers)(GLsizei, unsigned int*) = nullptr;
    void(XPT_GL* BindFramebuffer)(GLenum, unsigned int) = nullptr;
    void(XPT_GL* FramebufferTexture2D)(GLenum, GLenum, GLenum, unsigned int, GLint) = nullptr;
    GLenum(XPT_GL* CheckFramebufferStatus)(GLenum) = nullptr;
    void(XPT_GL* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(XPT_GL* Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(XPT_GL* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(XPT_GL* LoadIdentity)(void) = nullptr;
    void(XPT_GL* Ortho)(double, double, double, double, double, double) = nullptr;
    void(XPT_GL* Clear)(unsigned int) = nullptr;
    void(XPT_GL* ClearColor)(float, float, float, float) = nullptr;
    int state = 0;
} glm;
void* glExt(const char* name, const char* older) {   // (newer functions: asked of the driver on Windows; by their older name where the newer is missing)
    for (const char* n : {name, older}) {
        if (!n) continue;
        void* p = nullptr;
#if defined(_WIN32)
        if (HMODULE m = GetModuleHandleA("opengl32.dll")) {
            typedef void*(__stdcall * WglGet)(const char*);
            if (WglGet get = (WglGet)GetProcAddress(m, "wglGetProcAddress")) p = get(n);
            const intptr_t v = (intptr_t)p;
            if (v == 1 || v == 2 || v == 3 || v == -1) p = nullptr;   // (what some drivers answer for "not found")
        }
#endif
        if (!p) p = glSym(n);
        if (p) return p;
    }
    return nullptr;
}
bool loadMirrorGl() {
    if (glm.state == 0) {
        bool ok = true;
        auto get = [&](auto& f, const char* n, const char* older) { f = (typename std::remove_reference<decltype(f)>::type)glExt(n, older); ok = ok && f != nullptr; };
        get(glm.GenFramebuffers, "glGenFramebuffers", "glGenFramebuffersEXT"); get(glm.BindFramebuffer, "glBindFramebuffer", "glBindFramebufferEXT");
        get(glm.FramebufferTexture2D, "glFramebufferTexture2D", "glFramebufferTexture2DEXT"); get(glm.CheckFramebufferStatus, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
        get(glm.GetIntegerv, "glGetIntegerv", nullptr); get(glm.Viewport, "glViewport", nullptr); get(glm.ReadPixels, "glReadPixels", nullptr);
        get(glm.LoadIdentity, "glLoadIdentity", nullptr); get(glm.Ortho, "glOrtho", nullptr); get(glm.Clear, "glClear", nullptr); get(glm.ClearColor, "glClearColor", nullptr);
        glm.state = ok ? 1 : -1;
        if (!ok) logMsg("local connect: this OpenGL cannot draw into a texture, so devices cannot be shown the tablet");
    }
    return glm.state == 1;
}

// The tablet's frame drawn into a texture of its own at the mirror's scale and read back (rows bottom to top, as OpenGL reads).
// Everything of X-Plane's that is touched is put back: the framebuffer bound, the viewport, the matrices, the scissor test.
int g_mirTex = 0, g_mirW = 0, g_mirH = 0;
unsigned int g_mirFbo = 0;
bool captureTablet(std::vector<unsigned char>& out, int& W, int& H) {
    if (!loadGl() || !loadMirrorGl() || g_mirrorScale <= 0) return false;
    constexpr GLenum kFramebuffer = 0x8D40, kColor0 = 0x8CE0, kComplete = 0x8CD5, kFramebufferBinding = 0x8CA6, kViewport = 0x0BA2, kProjection = 0x1701,
                     kScissorTest = 0x0C11, kPackAlignment = 0x0D05, kPackRowLength = 0x0D02, kColorBufferBit = 0x4000;
    W = ((int)std::lround(g_logW * g_mirrorScale) + 1) & ~1;
    H = ((int)std::lround(xpt::kLogicalH * g_mirrorScale) + 1) & ~1;
    if (!g_mirTex) XPLMGenerateTextureNumbers(&g_mirTex, 1);
    if (W != g_mirW || H != g_mirH) {
        XPLMBindTexture2d(g_mirTex, 0);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
        gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        XPLMBindTexture2d(0, 0);
        g_mirW = W; g_mirH = H;
    }
    if (!g_mirFbo) glm.GenFramebuffers(1, &g_mirFbo);
    GLint wasFbo = 0, vp[4] = {0, 0, 0, 0};
    glm.GetIntegerv(kFramebufferBinding, &wasFbo);
    glm.GetIntegerv(kViewport, vp);
    const bool scissor = gl.IsEnabled(kScissorTest) != 0;
    glm.BindFramebuffer(kFramebuffer, g_mirFbo);
    glm.FramebufferTexture2D(kFramebuffer, kColor0, GL_TEXTURE_2D, (unsigned int)g_mirTex, 0);
    bool ok = glm.CheckFramebufferStatus(kFramebuffer) == kComplete;
    if (ok) {
        if (scissor) gl.Disable(kScissorTest);
        glm.Viewport(0, 0, W, H);
        gl.MatrixMode(kProjection); gl.PushMatrix(); glm.LoadIdentity(); glm.Ortho(0, W, 0, H, -1, 1);
        gl.MatrixMode(GL_MODELVIEW); gl.PushMatrix(); glm.LoadIdentity();
        glm.ClearColor(0, 0, 0, 1);
        glm.Clear(kColorBufferBit);
        drawBatches(g_batches, 0.0f, (float)H, g_mirrorScale, g_mirrorScale);   // (as it is drawn in the sim, at full brightness)
        out.resize((size_t)W * H * 4);
        gl.PixelStorei(kPackRowLength, 0);
        gl.PixelStorei(kPackAlignment, 1);
        glm.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        gl.PixelStorei(kPackAlignment, 4);
        gl.MatrixMode(GL_MODELVIEW); gl.PopMatrix();
        gl.MatrixMode(kProjection); gl.PopMatrix();
        gl.MatrixMode(GL_MODELVIEW);
        if (scissor) gl.Enable(kScissorTest);
    } else {
        static bool told = false;
        if (!told) { told = true; logMsg("local connect: the texture for devices' picture could not be drawn into"); }
    }
    glm.BindFramebuffer(kFramebuffer, (unsigned int)wasFbo);
    glm.Viewport(vp[0], vp[1], vp[2], vp[3]);
    return ok;
}

// Each frame X-Plane draws (whether or not the tablet is on screen): with a device connected, the tablet's picture for it
int mirrorDraw(XPLMDrawingPhase, int, void*) {
    xpt::crashlog::In crashIn_("drawing the picture for Local Connect");
    if (!g_localRunning) return 1;
    const bool watched = xpt::localserver::clients() > 0;
    g_mirrorScale = watched ? 1.5f : 0.0f;
    const double now = XPLMGetElapsedTime();
    if (!watched || !xpt::mirror::wantsFrame(now, xpt::arcadeOnScreen())) return 1;   // (a game on screen: more pictures a second)
    if (!g_ctx || !loadGl()) return 1;
    ensureFrame();   // (made now if the tablet was not drawn this frame: its window closed, no cockpit tablet in view)
    // the picture is taken only when the tablet's frame is another one (what it draws, or a picture it draws with), when a device
    // waits for a whole picture, and twice a second regardless (WXPlane's page and the like change without telling)
    static std::uint64_t lastHash = 0;
    static unsigned lastTex = 0;
    static double lastAt = -100;
    std::uint64_t hsh = 1469598103934665603ull;
    for (const xpt::Batch& b : g_batches) {
        hsh = (hsh ^ (std::uint64_t)(intptr_t)b.tex) * 1099511628211ull;
        const unsigned char* p = (const unsigned char*)b.v.data();
        const size_t n = b.v.size() * sizeof(ImDrawVert);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { std::uint64_t w; std::memcpy(&w, p + i, 8); hsh = (hsh ^ w) * 1099511628211ull; }
        for (; i < n; ++i) hsh = (hsh ^ p[i]) * 1099511628211ull;
    }
    if (hsh == lastHash && g_texChanges == lastTex && now - lastAt < 0.5 && !xpt::localserver::wantKey()) return 1;
    lastHash = hsh; lastTex = g_texChanges; lastAt = now;
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (captureTablet(px, w, h)) xpt::mirror::submit(std::move(px), w, h, true);
    return 1;
}

// a device's touches, scrolls and keys: to the tablet, as the sim's mouse and keyboard reach it
void remoteInput() {
    ImGuiIO& io = ImGui::GetIO();
    xpt::mirror::Input in;
    const double now = XPLMGetElapsedTime();
    for (int n = 0; n < 64 && xpt::mirror::nextInput(in); ++n) {
        const float x = in.x * g_logW, y = in.y * xpt::kLogicalH;
        switch (in.kind) {
        // (through the tablet's own stepping of the pointer: where it is first, the press on the next frame, the release on another)
        case xpt::mirror::Input::MOVE: xpt::queuePointer(x, y, -1); g_remotePointerUntil = now + 0.6; break;
        case xpt::mirror::Input::DOWN: xpt::setTouchInput(in.touch); xpt::queuePointer(x, y, 1); g_remotePointerUntil = now + 600; g_lastDeviceDown = now; break;   // (until it lifts)
        case xpt::mirror::Input::UP: xpt::queuePointer(x, y, 0); g_remotePointerUntil = now + (in.touch ? 0.3 : 0.6); break;
        case xpt::mirror::Input::CANCEL: xpt::queuePointer(-1000, -1000, 0); g_remotePointerUntil = now + 0.2; break;
        case xpt::mirror::Input::WHEEL: io.AddMouseWheelEvent(0, (float)in.code); break;
        // keys: into the tablet's own text box, or into a web page's field (as the computer's keyboard reaches it)
        case xpt::mirror::Input::KEY:
            if (xpt::tabletTyping()) xpt::tabletKey(in.code, false);
            else if (xpt::webTyping() && g_webShown && xpt::browser::started()) { xpt::browser::push(xptb::CMD_KEY, 0, 0, in.code, 1); xpt::browser::push(xptb::CMD_KEY, 0, 0, in.code, 0); if (in.code == 13) xpt::browser::push(xptb::CMD_CHAR, 0, 0, 13); }
            break;
        case xpt::mirror::Input::CHAR:
            if (xpt::tabletTyping()) xpt::tabletKey(in.code, true);
            else if (xpt::webTyping() && g_webShown && xpt::browser::started()) xpt::browser::push(xptb::CMD_CHAR, 0, 0, in.code);
            break;
        case xpt::mirror::Input::GAME_KEY: if (xpt::currentApp() == xpt::APP_GAMES && !xpt::tabletTyping()) xpt::gameKeyEvent(in.code, in.down); break;
        }
    }
    if (xpt::localserver::clients() == 0 && g_remotePointerUntil > now + 1) {   // the device left with its finger down: let go
        xpt::queuePointer(-1000, -1000, 0);
        g_remotePointerUntil = now;
    }
}
#endif

void localConnectStep() {
    xpt::LocalConnectState& L = g_sim.local;
#if defined(XPT_LOCAL_CONNECT)
    // who works the tablet now: a device (its keyboard choice), or the sim's mouse; and whether the device should raise its keyboard
    xpt::setDeviceInput(g_localRunning && xpt::localserver::clients() > 0 && g_lastDeviceDown > g_lastSimDown);
    if (g_localRunning) { xpt::mirror::setDeviceTyping(xpt::deviceKeysWanted()); xpt::mirror::setRate(xpt::localRefreshRate()); }
    if (g_localRunning) {   // the tablet's theme, for the page's own screens
        static float themeAt = -100; static std::string themeWas;
        if (every(themeAt, 2.0f)) { std::string t = xpt::localThemeJson(); if (t != themeWas) { themeWas = t; xpt::localserver::setTheme(t); } }
    }
    static std::string servedIp;
    static double lookedAt = -100;
    const double now = XPLMGetElapsedTime();
    auto halt = [&]() { if (g_localRunning) { xpt::mirror::stop(); xpt::localserver::stop(); g_localRunning = false; } g_mirrorScale = 0; };
    if (!xpt::settings().localConnect) {
        if (g_localRunning) { halt(); servedIp.clear(); logMsg("local connect: off"); }
        L = xpt::LocalConnectState();   // (status 0: off; the page shows its switch)
        lookedAt = -100;
        return;
    }
    if (now - lookedAt > 10) {   // (the network can change: looked at again every 10 s)
        lookedAt = now;
        std::string ip = xpt::localIPv4();
        if (!xpt::isPrivateIPv4(ip)) ip.clear();   // (never on an address the internet can reach)
        if (ip != servedIp) {
            halt();
            servedIp = ip;
            if (!ip.empty()) {
                char sys[2048] = "";
                XPLMGetSystemPath(sys);
                xpt::localserver::Config c;
                c.bindIp = ip; c.port = 8443;
                c.dataDir = std::string(sys) + "Output/XPTablet/localconnect";
                for (const char* piece : xpt::kLocalPage) c.page += piece;
                for (auto [mark, png] : { std::pair{std::string("%XPT_ICON%"), xpt::kLocalIconPng}, std::pair{std::string("%XPT_LOGO%"), xpt::kLocalLogoPng} })
                    for (std::size_t at; (at = c.page.find(mark)) != std::string::npos;) c.page.replace(at, mark.size(), png);
                c.wireVersion = xpt::mirror::kPageVersion;
                xpt::localserver::start(c);
                xpt::mirror::start();
                g_localRunning = true;
                logMsg(("local connect: starting on " + ip).c_str());
            }
        }
    }
    if (!g_localRunning) { L = xpt::LocalConnectState(); L.status = 2; return; }   // (on, but no home network: the page says "No network found")
    const xpt::localserver::Status st = xpt::localserver::status();
    L.status = st.state == 0 ? 1 : st.state;
    L.address = st.address; L.error = st.error; L.pairCode = st.pairCode; L.pairName = st.pairName;
    L.devices.clear();
    const std::time_t today = std::time(nullptr);
    std::tm tn{};
#if IBM
    localtime_s(&tn, &today);
#else
    localtime_r(&today, &tn);
#endif
    for (const xpt::localserver::Device& d : st.devices) {
        xpt::LocalDevice o;
        o.name = d.name; o.kind = d.kind; o.connected = d.connected; o.batteryPct = d.batteryPct; o.charging = d.charging;
        if (d.lastSeen > 0) {   // "Today 14:32", "12 Nov"
            const std::time_t t = (std::time_t)d.lastSeen;
            std::tm tm{};
#if IBM
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32];
            if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday) std::strftime(buf, sizeof buf, "Today %H:%M", &tm);
            else std::strftime(buf, sizeof buf, "%d %b", &tm);
            o.lastSeen = buf;
        }
        L.devices.push_back(o);
    }
    if (st.state == 2) remoteInput();
#else
    L.status = -2;   // (built without Local Connect: Settings leaves its page out)
#endif
}

float flightLoop(float dt, float, int, void*) {
    xpt::crashlog::In crashIn_("the flight loop");
    backspaceStep();   // (Backspace held: deleting on)
    xpt::crashlog::here("the flight loop: jetwayWatchStep");
    jetwayWatchStep();   // (openSAM's beacon-off press undoing the tablet's docking)
    xpt::crashlog::here("the flight loop: tabletPagesWatchStep");
    tabletPagesWatchStep();   // (an aircraft's own tablet pages: how it switches them, in the log)
    {   // a thread stopped on an exception (caught): told in the log, from here (the main thread)
        const std::string f = xpt::crashlog::takeFailures();
        size_t s = 0;
        while (s < f.size()) { size_t e = f.find('\n', s); if (e == std::string::npos) e = f.size(); if (e > s) logMsg("%s", f.substr(s, e - s).c_str()); s = e + 1; }
    }
    xpt::crashlog::here("the flight loop: findSimRefs");
    findSimRefs();
    if ((XPLMGetCycleNumber() & 7) == 0) { readSimState(); g_sim.ground.slidesAuto = g_slidesAuto; followJetways(); slidesAutoStep(); readMapState(); }
    xpt::crashlog::here("the flight loop: slidesHoldStep");
    slidesHoldStep();
    xpt::crashlog::here("the flight loop: cabinAutoStep");
    cabinAutoStep();
    xpt::crashlog::here("the flight loop: jetwayUndockStep");
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
    xpt::crashlog::here("the flight loop: localConnectStep");
    localConnectStep();
    if (xpt::takeSettingsChanged()) saveSettings();
    saveSettingsIfChanged(sec);
    xpt::crashlog::here("the flight loop: chimeStep");
    chimeStep();
    xpt::crashlog::here("the flight loop: updateNotify");
    updateNotify();
    if (xpt::takeNotesChanged()) saveNotes();
    if (xpt::takeLogbookChanged()) saveLogbook();
    if (g_drZulu) g_sim.zuluSec = (int)XPLMGetDataf(g_drZulu);
    { static XPLMDataRef fu = XPLMFindDataRef("sim/flightmodel/weight/m_fuel_total"); g_sim.fuelKg = fu ? XPLMGetDataf(fu) : -1.0f; }
    { static XPLMDataRef fr = XPLMFindDataRef("sim/time/framerate_period"); if (fr) { const float per = XPLMGetDataf(fr); g_sim.fps = per > 0 ? 1.0f / per : 0.0f; } }
    if (g_drIcao) {
        char icao[41] = "";
        const int n = XPLMGetDatab(g_drIcao, icao, 0, 40);
        icao[std::max(0, std::min(40, n))] = 0;
        if (g_sim.acfIcao != icao) { g_sim.acfIcao = icao; loadChecklist(); }
    }
    if ((XPLMGetCycleNumber() & 127) == 0) g_sim.toliss = isToLissAircraft();   // its plugin loads after the aircraft does
    if ((XPLMGetCycleNumber() & 63) == 0) {   // the engines: their kind (the checklists follow it), and ToLiss's engine choice changed in its menu
        static XPLMDataRef en = XPLMFindDataRef("sim/aircraft/prop/acf_en_type");
        int t = -1;
        if (en) XPLMGetDatavi(en, &t, 0, 1);
        g_sim.engineType = t;
        static int lastToLissEng = -100;
        int k = -1;
        if (XPLMDataRef r = XPLMFindDataRef("AirbusFBW/EngineTypeIndex")) k = (XPLMGetDataRefTypes(r) & xplmType_Int) ? XPLMGetDatai(r) : (int)XPLMGetDataf(r);
        if (k != lastToLissEng) { const bool first = lastToLissEng == -100; lastToLissEng = k; if (!first) loadChecklist(); }
    }
    if ((XPLMGetCycleNumber() & 255) == 0) g_sim.wxPlane = XPLMFindPluginBySignature(kWxPlaneSignature) != XPLM_NO_PLUGIN_ID;   // it may load after us
    xpt::crashlog::here("the flight loop: updateCaptureWindow");
    updateCaptureWindow();
    return -1.0f;   // every frame
}

} // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "XPTablet");
    // With AviTab not installed, XPTablet answers to AviTab's plugin signature too: some aircraft look for AviTab by it and, not
    // finding it, never switch it off for their own tablet pages (the Aerobask DA42: its other tabs could not be reached, 2026-11-26;
    // it never touches avitab/panel_enabled without it). AviTab loads before us (plugin folders load in name order); its folder
    // is looked for too, in case. Other plugins tell XPTablet by its xptablet/ datarefs (WXPlane does).
    {
        const bool aviTabThere = XPLMFindPluginBySignature(kAviTabSignature) != XPLM_NO_PLUGIN_ID || XPLMFindDataRef("avitab/panel_enabled") != nullptr;
        bool aviTabFolder = false;
        {
            char sys[2048] = "";
            XPLMGetSystemPath(sys);
            std::error_code ec;
            for (const char* name : {"AviTab", "avitab", "Avitab", "AVITAB"})
                aviTabFolder = aviTabFolder || std::filesystem::exists(std::filesystem::u8path(std::string(sys) + "Resources/plugins/" + name), ec);
        }
        g_tookAviTabSig = !aviTabThere && !aviTabFolder;
        std::strcpy(outSig, g_tookAviTabSig ? kAviTabSignature : "com.alpineaviation.xptablet");
    }
    std::strcpy(outDesc, "XPTablet " XPT_VERSION " by Alpine Aviation - the cockpit tablet");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    {   // a crash's record (Windows): written as it happens; the last session's, if any, put in the log now
        char sys[2048] = "";
        XPLMGetSystemPath(sys);
        const std::string dir = std::string(sys) + "Output/XPTablet/";
        makeDirs(dir);
        const std::string last = xpt::crashlog::takeLast(dir);
        if (!last.empty()) {
            logMsg("a crash was recorded in the last session (also in Output/XPTablet/crash-last.txt):");
            std::string line;
            for (size_t i = 0, n = 0; i <= last.size() && n < 400; ++i) {
                if (i == last.size() || last[i] == '\n') { while (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) { logMsg("  %s", line.c_str()); ++n; } line.clear(); }
                else line += last[i];
            }
        }
        xpt::crashlog::install(dir);
        xpt::g_threadFailed = [](const char* name, const char* what) { xpt::crashlog::threadFailed(name, what); };
    }

    g_drClickX = XPLMFindDataRef("sim/graphics/view/click_3d_x_pixels");
    g_drClickY = XPLMFindDataRef("sim/graphics/view/click_3d_y_pixels");
    g_drZulu = XPLMFindDataRef("sim/time/zulu_time_sec");
    g_drIcao = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    g_drVr = XPLMFindDataRef("sim/graphics/VR/enabled");
    g_drUiScale = XPLMFindDataRef("sim/graphics/misc/user_interface_scale");
    g_drBus = XPLMFindDataRef("sim/cockpit2/electrical/bus_volts");

    // AviTab's interface is taken over only when AviTab itself is not there (it loads before us: plugin folders load in name order)
    {   // (ourselves, under AviTab's signature, not counted)
        const XPLMPluginID av = XPLMFindPluginBySignature(kAviTabSignature);
        g_aviTabRunning = (av != XPLM_NO_PLUGIN_ID && av != XPLMGetMyID()) || XPLMFindDataRef("avitab/panel_enabled") != nullptr;
    }
    g_ownsAviTab = !g_aviTabRunning;
    registerDatarefs("xptablet");
    registerCommands("XPTablet");
    if (g_ownsAviTab) {
        registerDatarefs("avitab");
        registerCommands("AviTab");
        logMsg(XPT_VERSION ": AviTab is not installed, XPTablet takes the cockpit tablet%s", g_tookAviTabSig ? " (and answers to AviTab's plugin signature, for aircraft that look for it)" : "");
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
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;   // (a page closed after a caught error: its stacks are put back quietly)
    xpt::applyStyle();
    loadSettings();
    loadNotes();
    loadLogbook();
    loadShapesFile();

    // in the Plugins menu, one item: XPTablet, which shows or hides the tablet window (no submenu, as WXPlane's)
    if (XPLMCommandRef toggle = XPLMFindCommand("XPTablet/toggle_tablet"))
        g_menuItem = XPLMAppendMenuItemWithCommand(XPLMFindPluginsMenu(), "XPTablet", toggle);
    else {   // (no command: the submenu as before)
        g_menuItem = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XPTablet", nullptr, 0);
        g_menu = XPLMCreateMenu("XPTablet", XPLMFindPluginsMenu(), g_menuItem, Safe<menuHandler, 0>::call, nullptr);
        XPLMAppendMenuItem(g_menu, "Show / hide tablet window", nullptr, 0);
    }
    return 1;
}


PLUGIN_API int XPluginEnable(void) {
    {   // the scenery's SAM jetways, for saying when a stand needs openSAM and it is not running (read once, on a thread)
        char sys[2048] = "";
        XPLMGetSystemPath(sys);
        xpt::sam::start(sys);
    }
    updateCheckStart();   // (once a session, in the background)
    XPLMRegisterKeySniffer(Safe<keySniffer, 1>::call, 1, nullptr);   // (before X-Plane's windows: typed keys go to a text box first)
    XPLMRegisterDrawCallback(Safe<drawPanel, 1>::call, xplm_Phase_Gauges, 0, nullptr);
#if defined(XPT_LOCAL_CONNECT)
    XPLMRegisterDrawCallback(Safe<mirrorDraw, 1>::call, xplm_Phase_Window, 0, nullptr);   // (the tablet's picture for paired devices)
#endif
    XPLMCreateFlightLoop_t fl;
    fl.structSize = sizeof fl;
    fl.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc = Safe<flightLoop, -1>::call;
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
    xpt::sam::stop();
#if defined(XPT_LOCAL_CONNECT)
    if (g_localRunning) { xpt::mirror::stop(); xpt::localserver::stop(); g_localRunning = false; }   // (started again by the flight loop when the setting is on)
#endif
    if (xpt::settingsToText() != g_savedSettings) saveSettings();   // nothing changed is lost when the sim or the plugins reload
    XPLMUnregisterKeySniffer(Safe<keySniffer, 1>::call, 1, nullptr);
    XPLMUnregisterDrawCallback(Safe<drawPanel, 1>::call, xplm_Phase_Gauges, 0, nullptr);
#if defined(XPT_LOCAL_CONNECT)
    XPLMUnregisterDrawCallback(Safe<mirrorDraw, 1>::call, xplm_Phase_Window, 0, nullptr);
#endif
    if (g_loop) { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_capture) { XPLMDestroyWindow(g_capture); g_capture = nullptr; }
    g_captureDown = false;
    xpt::metarStop();
    xpt::media::stop();
    browserStop();
    mapStop();
    acarsStop();
    wx::httpGlobalCleanup();
}

PLUGIN_API void XPluginStop(void) {
    jetwayWatchStop();
    if (g_vcThread.joinable()) g_vcThread.join();
    xpt::crashlog::uninstall();
    if (g_win) { XPLMDestroyWindow(g_win); g_win = nullptr; }
    for (const Cmd& c : g_cmds) XPLMUnregisterCommandHandler(c.ref, Safe<onCommand, 1>::call, 1, (void*)(intptr_t)c.id);
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
