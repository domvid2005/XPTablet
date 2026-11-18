// XPTablet - the plugin. Puts the tablet (src/tabletui.cpp) in two places:
//   * on the aircraft's own cockpit tablet, in the area of the 3-D panel texture the aircraft reserved for AviTab. XPTablet speaks
//     the interface aircraft already use for that: the AviTab.json file in the aircraft folder, the avitab/panel_* datarefs and the
//     AviTab/* commands. So it appears where AviTab would, with no change to the aircraft.
//   * in a floating window (also a VR window), for aircraft without a tablet.
// If AviTab itself is installed and running, XPTablet leaves that interface to it and only offers the window.
#include <algorithm>
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

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include "aptdat.h"
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
xpt::Actions g_pendingGround;        // a press on the Ground page, waiting for the flight loop

// X-Plane's weather at the aircraft, the ground services and the ToLiss doors: looked up once, read in the flight loop
struct SimRefs {
    XPLMDataRef windDir = nullptr, windMs = nullptr, vis = nullptr, temp = nullptr, dew = nullptr, qnh = nullptr, rain = nullptr, alt = nullptr;
    XPLMDataRef cBase = nullptr, cTop = nullptr, cCover = nullptr, aAlt = nullptr, aDir = nullptr, aKt = nullptr, onGround = nullptr;
    XPLMDataRef jwStatus = nullptr, jwDoorStatus = nullptr;            // openSAM: the stand's jetway, and docked by door
    XPLMDataRef slides = nullptr, cabinPerc = nullptr, cabinLevel = nullptr, exitConfig = nullptr;   // ToLiss
    bool wasDocked[3] = {false, false, false};                         // for opening and closing the doors with the jetways
    bool xpJetway = false;                                             // without openSAM: X-Plane's own jetways, as last pressed
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
void loadSettings() {
    std::string text;
    if (!settingsPath().empty() && readFile(settingsPath(), text)) xpt::settingsFromText(text);
}
void saveSettings() {
    const std::string path = settingsPath();
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "wb");
    if (!f) { logMsg("cannot write %s", path.c_str()); return; }
    const std::string text = xpt::settingsToText();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
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
        else br::push(e.type, (int)e.x, (int)e.y, e.code);
    }
    if (!act.webNavigate.empty()) br::push(xptb::CMD_NAVIGATE, 0, 0, 0, 0, 0, act.webNavigate);
    switch (act.webNav) {
    case 1: br::push(xptb::CMD_BACK); break;
    case 2: br::push(xptb::CMD_FORWARD); break;
    case 3: br::push(xptb::CMD_RELOAD); break;
    case 4: br::push(xptb::CMD_STOP); break;
    default: break;
    }
    if (act.webTabNew) br::push(xptb::CMD_TAB_NEW, 0, 0, 0, 0, 0, act.webTabUrl);
    if (act.webTabSelect) br::push(xptb::CMD_TAB_SELECT, act.webTabSelect);
    if (act.webTabClose) br::push(xptb::CMD_TAB_CLOSE, act.webTabClose);
    bool loading = false, back = false, fwd = false, kbd = false;
    std::vector<br::TabInfo> tabs;
    int active = 0;
    if (br::info(b.url, b.title, loading, back, fwd, kbd, tabs, active)) {
        b.loading = loading; b.canBack = back; b.canForward = fwd; b.wantsKeyboard = kbd;
        b.tabs.clear();
        for (const br::TabInfo& t : tabs) b.tabs.push_back({t.id, t.title, t.url, t.loading});
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
    logMsg("map: tiles cached in Output/XPTablet/tiles; reading X-Plane's airports");
}
void mapStop() {
    if (!g_mapStarted) return;
    xpt::tiles::stop();
    xpt::aptdat::stop();
    g_mapStarted = false;
    xpt::ourairports::stop();
    g_ourAirportsStarted = false;
    if (g_docThread.joinable()) g_docThread.join();
    g_sim.map.airports.clear();
    g_sim.map.airportPoints.clear();
}

// After each UI build: what the map asked for, and the tiles that arrived (a few per frame, so no frame takes long)
void mapStep(const xpt::Actions& act) {
    if (act.mapShown) mapStart();
    if (!g_mapStarted) return;
    if (act.mapShown) {
        xpt::tiles::want(g_tileWants);
        xpt::aptdat::setView(act.mapLat, act.mapLon, act.mapLayoutNm, act.mapPointNm, act.mapLayouts);
        if ((XPLMGetCycleNumber() & 7) == 0) {
            int n = 0;
            xpt::aptdat::get(g_sim.map.airports, g_sim.map.airportPoints, g_sim.map.aptIndexing, n);
            g_sim.map.aptCount = n;
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
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
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
    if (!act.airportsShown) return;
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
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, g_docW, g_docH, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_docPixels.data());
    XPLMBindTexture2d(0, 0);
    d.tex = (ImTextureID)(intptr_t)g_docTex;
    d.texW = g_docW; d.texH = g_docH;
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

float userBrightness() { return std::max(0.1f, std::min(1.0f, xpt::settings().brightness)); }

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
float getZero(void*) { return 0.0f; }
int getZeroI(void*) { return 0; }

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
    regFloat(prefix + "/map/latitude", getZero, nullptr, nullptr);
    regFloat(prefix + "/map/longitude", getZero, nullptr, nullptr);
    regIntFn(prefix + "/map/zoom", getZeroI);
    regFloat(prefix + "/map/vertical_range", getZero, nullptr, nullptr);
}

enum { CMD_TOGGLE, CMD_HOME, CMD_CLICK, CMD_WHEEL_UP, CMD_WHEEL_DOWN, CMD_NOP, CMD_APP0 = 100 };

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
    regCmd(p + "/app_routes", "Tablet: routes", CMD_APP0 + xpt::APP_ROUTES);
    regCmd(p + "/app_maps", "Tablet: map", CMD_APP0 + xpt::APP_MAP);
    regCmd(p + "/app_plane_manual", "Tablet: aircraft documents", CMD_APP0 + xpt::APP_DOCUMENTS);
    regCmd(p + "/app_notes", "Tablet: notes", CMD_APP0 + xpt::APP_NOTES);
    regCmd(p + "/app_navigraph", "Tablet: charts", CMD_APP0 + xpt::APP_CHARTS);
    regCmd(p + "/app_about", "Tablet: settings", CMD_APP0 + xpt::APP_SETTINGS);
    // the map's own controls: accepted now so aircraft buttons bound to them do nothing rather than fail, wired up with the map
    for (const char* n : {"zoom_in", "zoom_out", "recentre", "pan_left", "pan_right", "pan_up", "pan_down"})
        regCmd(p + "/" + n, "Tablet: map control", CMD_NOP);
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
    if (!g.onGround || g.groundSpeedKt > 3.0f) r.xpJetway = false;
    if (!g.openSam) g.jetway = r.xpJetway ? 2 : 1;
    // the doors a jetway serves on other aircraft: as many as openSAM drives (X-Plane's own: one bubble, every boarding door)
    if (!g.toliss) {
        g.jwDoors = 1;
        if (g.openSam && r.jwNumber) g.jwDoors = std::max(1, std::min(3, XPLMGetDatai(r.jwNumber)));
        int docked[3] = {0, 0, 0};
        if (g.openSam && r.jwDoorStatus) XPLMGetDatavi(r.jwDoorStatus, docked, 0, 3);
        for (int k = 0; k < 3; ++k) { g.jwDoorPax[k] = 2 * k; g.jwDocked[k] = k < g.jwDoors && (g.openSam ? docked[k] != 0 : r.xpJetway); }
        g.paxDoors = 2 * (g.jwDoors + 1);
    }
    if (g.toliss) {
        // the arrays are as long as the largest ToLiss needs: the A319/A320 have two pairs of cabin doors, the A321 four
        const int have = XPLMGetDatavi(r.paxDoor, nullptr, 0, 0);
        const bool long321 = g_sim.acfIcao.compare(0, 3, "A21") == 0 || g_sim.acfIcao == "A321";
        g.paxDoors = std::min(have, long321 ? 8 : 4);
        XPLMGetDatavi(r.paxDoor, g.paxDoor, 0, g.paxDoors);
        if (r.cargoDoor) { g.cargoDoors = std::min(XPLMGetDatavi(r.cargoDoor, nullptr, 0, 0), 3); XPLMGetDatavi(r.cargoDoor, g.cargoDoor, 0, g.cargoDoors); }   // forward, aft, bulk
        g.deice = offered("toliss_airbus/iscsinterface/animatedDeice") || offered("toliss_airbus/iscsinterface/quickDeice");
        if (!g_refs.probed) { g_refs.probed = true; probeToLiss(); }
        g.chocks = g.chocks || (r.chocks && XPLMGetDatai(r.chocks) != 0);
        g.extPower = r.extPower && XPLMGetDatai(r.extPower) != 0;
        g.hpAir = r.hpAir && XPLMGetDatai(r.hpAir) != 0;
        g.lpAir = r.lpAir && XPLMGetDatai(r.lpAir) != 0;
        // the doors a jetway can reach: 1L, and 2L on an A321 with the classic door layout (A321ExitConfig 0), as openSAM docks them
        g.jwDoors = 1;
        g.jwDoorPax[0] = 0;
        if (g_sim.acfIcao == "A321" && r.exitConfig && readNum(r.exitConfig) == 0 && g.paxDoors > 2) { g.jwDoors = 2; g.jwDoorPax[1] = 2; }
        int docked[3] = {0, 0, 0};
        if (r.jwDoorStatus) XPLMGetDatavi(r.jwDoorStatus, docked, 0, 3);
        // X-Plane's own jetways (without openSAM) connect at every boarding door at once, so each door is docked together
        for (int k = 0; k < 3; ++k) g.jwDocked[k] = k < g.jwDoors && (g.openSam ? docked[k] != 0 : r.xpJetway);
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
    } else {
        g.slides = -1; g.cabinLight = -1;
    }
}

// The cabin door a jetway is on opens once the jetway has docked, and closes when the jetway leaves. Docked by door comes from
// openSAM; without openSAM, X-Plane's own jetways (every boarding door at once) follow the Connect / Disconnect presses.
void followJetways() {
    SimRefs& r = g_refs;
    const xpt::GroundState& g = g_sim.ground;
    if (!g.toliss || !r.paxDoor) return;
    for (int k = 0; k < g.jwDoors && k < 3; ++k) {
        if (g.jwDocked[k] == r.wasDocked[k]) continue;
        r.wasDocked[k] = g.jwDocked[k];
        const int pax = g.jwDoorPax[k];
        if (pax < 0 || pax >= g.paxDoors) continue;
        int mode = g.jwDocked[k] ? 2 : 0;
        XPLMSetDatavi(r.paxDoor, &mode, pax, 1);
        logMsg("jetway %s door %dL: door %s", g.jwDocked[k] ? "docked at" : "left", pax / 2 + 1, g.jwDocked[k] ? "opened" : "closed");
    }
}

void once(const char* cmd) {
    const XPLMCommandRef c = XPLMFindCommand(cmd);
    if (c) XPLMCommandOnce(c); else logMsg("command %s not found", cmd);
}
void toggleInt(XPLMDataRef ref) { if (ref) XPLMSetDatai(ref, XPLMGetDatai(ref) ? 0 : 1); }

// What was pressed on the Ground page.
void doGround(const xpt::Actions& act) {
    const SimRefs& r = g_refs;
    const xpt::GroundState& g = g_sim.ground;
    switch (act.ground) {
    case xpt::GA_JETWAY: once(g.openSam ? "openSAM/toggle_jwy" : "sim/ground_ops/jetway"); break;
    case xpt::GA_JETWAY_DOOR: {   // a door's Connect / Disconnect: openSAM docks (or undocks) every jetway at the stand
        const int k = std::max(0, std::min(2, act.groundIndex));
        if (g.openSam) {
            once(g.jwDocked[k] ? "openSAM/undock_jwy" : "openSAM/dock_jwy");
        } else {   // X-Plane's own jetways: a toggle with no state to read, so what was pressed is kept (followJetways opens the doors)
            once("sim/ground_ops/jetway");
            g_refs.xpJetway = !g_refs.xpJetway;
        }
        break;
    }
    case xpt::GA_SLIDES_ARM: if (g.toliss) trigger("toliss_airbus/door_commands/arm_slides"); break;
    case xpt::GA_SLIDES_DISARM: if (g.toliss) trigger("toliss_airbus/door_commands/disarm_slides"); break;
    case xpt::GA_CABIN_LIGHT: {   // the level, 0-100 % in steps of 10; ToLiss's 0 / 1 / 2 switch when there is no percentage
        const int pct = std::max(0, std::min(100, act.groundIndex));
        if (r.cabinPerc && XPLMCanWriteDataRef(r.cabinPerc)) writeNum(r.cabinPerc, (float)pct);
        else if (r.cabinLevel && XPLMCanWriteDataRef(r.cabinLevel)) writeNum(r.cabinLevel, pct == 0 ? 0.0f : pct <= 50 ? 1.0f : 2.0f);
        else logMsg("cabin lights: no writable ToLiss dataref");
        break;
    }
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
    if ((XPLMGetCycleNumber() & 7) == 0) { readSimState(); followJetways(); readMapState(); }
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
    XPLMUnregisterDrawCallback(drawPanel, xplm_Phase_Gauges, 0, nullptr);
    if (g_loop) { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_capture) { XPLMDestroyWindow(g_capture); g_capture = nullptr; }
    g_captureDown = false;
    xpt::metarStop();
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
