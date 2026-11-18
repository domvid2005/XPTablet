// The X-Plane the plugin runs against in the emulator: every XPLM function XPTablet calls, implemented over the emulator's own state.
// Datarefs hold plain values (the simulated aircraft writes them; the control panel edits them); the plugin's own datarefs answer
// through its accessors, as in X-Plane. Windows are drawn in X-Plane's order (flight overlay, floating windows, ...), with a simple
// frame and title bar for decorated ones, which can be dragged and resized as in X-Plane.
#include "emu.h"
#include "emugl.h"
#include "imgui.h"

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>

// the plugin's entry points (plugin.cpp, built in)
extern "C" {
int XPluginStart(char* outName, char* outSig, char* outDesc);
int XPluginEnable(void);
void XPluginDisable(void);
void XPluginStop(void);
void XPluginReceiveMessage(XPLMPluginID from, int msg, void* param);
}

namespace emu {

Gl gl;
bool loadGl(void* (*getProc)(const char*)) {
    bool ok = true;
    auto get = [&](auto& f, const char* n, bool need = true) { f = (std::remove_reference_t<decltype(f)>)getProc(n); if (!f && need) { ok = false; std::fprintf(stderr, "emulator: %s missing\n", n); } };
    get(gl.Enable, "glEnable"); get(gl.Disable, "glDisable"); get(gl.BlendFunc, "glBlendFunc"); get(gl.Viewport, "glViewport");
    get(gl.Scissor, "glScissor"); get(gl.ClearColor, "glClearColor"); get(gl.Clear, "glClear"); get(gl.MatrixMode, "glMatrixMode");
    get(gl.LoadIdentity, "glLoadIdentity"); get(gl.Ortho, "glOrtho"); get(gl.GenTextures, "glGenTextures"); get(gl.DeleteTextures, "glDeleteTextures");
    get(gl.BindTexture, "glBindTexture"); get(gl.TexImage2D, "glTexImage2D"); get(gl.TexParameteri, "glTexParameteri"); get(gl.TexEnvi, "glTexEnvi");
    get(gl.PixelStorei, "glPixelStorei"); get(gl.EnableClientState, "glEnableClientState"); get(gl.DisableClientState, "glDisableClientState");
    get(gl.VertexPointer, "glVertexPointer"); get(gl.TexCoordPointer, "glTexCoordPointer"); get(gl.ColorPointer, "glColorPointer");
    get(gl.DrawElements, "glDrawElements"); get(gl.Begin, "glBegin"); get(gl.End, "glEnd"); get(gl.Vertex2f, "glVertex2f"); get(gl.Color4f, "glColor4f");
    get(gl.LineWidth, "glLineWidth"); get(gl.ReadPixels, "glReadPixels"); get(gl.Finish, "glFinish"); get(gl.PushAttrib, "glPushAttrib");
    get(gl.PopAttrib, "glPopAttrib"); get(gl.GetString, "glGetString"); get(gl.ActiveTexture, "glActiveTexture", false);
    return ok;
}

namespace {

// ---- state -------------------------------------------------------------------------------------------------------------------------
std::map<std::string, std::unique_ptr<Dataref>> g_refs;
std::vector<std::string> g_missing;
std::vector<std::string> g_extraPrefixes;
std::vector<std::string> g_log;
Paths g_paths;
double g_time = 0;
int g_cycle = 0;

struct CmdHandler { XPLMCommandCallback_f cb; int before; void* refcon; };
struct Cmd { std::string name, desc; bool fromPlugin = false; int sent = 0; std::vector<CmdHandler> handlers; };
std::map<std::string, std::unique_ptr<Cmd>> g_cmds;
std::vector<std::string> g_cmdLog;

struct Menu { std::string name; Menu* parent = nullptr; int parentItem = -1; XPLMMenuHandler_f handler = nullptr; void* refcon = nullptr;
              struct Item { std::string name; void* ref = nullptr; bool removed = false; Menu* sub = nullptr; }; std::vector<Item> items; bool destroyed = false; };
std::vector<std::unique_ptr<Menu>> g_menus;
Menu* pluginsMenu() { if (g_menus.empty()) { g_menus.emplace_back(new Menu()); g_menus[0]->name = "Plugins"; } return g_menus[0].get(); }

struct Win {
    XPLMCreateWindow_t p{};
    int l = 0, t = 0, r = 0, b = 0;
    bool visible = false;
    std::string title;
    int layer = xplm_WindowLayerFloatingWindows;
    int decoration = xplm_WindowDecorationNone;
    int minW = 0, minH = 0, maxW = 100000, maxH = 100000;
    long long z = 0;
    bool dead = false;
};
std::vector<std::unique_ptr<Win>> g_wins;
long long g_zNext = 1;
Win* g_focus = nullptr;
Win* g_mouseWin = nullptr;    // the window holding the pointer (pressed in it)
int g_mouseButton = 0;
enum { DRAG_NONE, DRAG_MOVE, DRAG_SIZE } g_drag = DRAG_NONE;
float g_dragX = 0, g_dragY = 0;
int g_dragL, g_dragT, g_dragR, g_dragB;
float g_mouseX = -1, g_mouseY = -1;

struct DrawCb { XPLMDrawCallback_f cb; XPLMDrawingPhase phase; int before; void* refcon; };
std::vector<DrawCb> g_drawCbs;
struct Loop { XPLMFlightLoop_f cb = nullptr; void* refcon = nullptr; float interval = 0; double due = 0; int dueCycle = 0; double last = 0; int counter = 0; bool alive = true; };
std::vector<std::unique_ptr<Loop>> g_loops;

struct FakePlugin { std::string sig, name, path; };
std::vector<FakePlugin> g_fakes;

int g_screenW = 1280, g_screenH = 800;
float g_pxPerBoxel = 1;
View g_view = View::Window;
bool g_started = false;
const int kTitleH = 26;          // the decoration's title bar above a window (boxels)

double g_lastLoop = 0;

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

Win* winOf(XPLMWindowID id) {
    for (auto& w : g_wins) if (w.get() == (Win*)id && !w->dead) return w.get();
    return nullptr;
}
std::vector<Win*> drawOrder() {   // bottom to top
    std::vector<Win*> v;
    for (auto& w : g_wins) if (!w->dead) v.push_back(w.get());
    std::stable_sort(v.begin(), v.end(), [](Win* a, Win* b) { return a->layer != b->layer ? a->layer < b->layer : a->z < b->z; });
    return v;
}
bool decorated(const Win* w) { return w->decoration == xplm_WindowDecorationRoundRectangle || w->decoration == xplm_WindowDecorationSelfDecoratedResizable; }

} // namespace

// ---- logging ------------------------------------------------------------------------------------------------------------------------------
void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::printf("%s\n", s.c_str());
    std::fflush(stdout);
    g_log.push_back(s);
    if (g_log.size() > 2000) g_log.erase(g_log.begin(), g_log.begin() + 500);
}
const std::vector<std::string>& logLines() { return g_log; }
const Paths& paths() { return g_paths; }
double simTime() { return g_time; }

// ---- datarefs -------------------------------------------------------------------------------------------------------------------------------
// The commands this X-Plane (and its aircraft) has that the plugin may look for; X-Plane keeps commands and datarefs apart.
static bool isCommandName(const std::string& n) {
    static const char* kCmds[] = {"sim/ground_ops/jetway", "sim/ground_ops/pushback_left", "sim/ground_ops/pushback_right", "sim/ground_ops/pushback_stop",
                                  "sim/ground_ops/pushback_straight", "sim/ground_ops/service_plane", "sim/ground_ops/toggle_gpu_request",
                                  "toliss_airbus/door_commands/all_to_mode_auto", "toliss_airbus/door_commands/all_to_mode_close",
                                  "toliss_airbus/door_commands/arm_slides", "toliss_airbus/door_commands/disarm_slides",
                                  "toliss_airbus/iscsinterface/animatedDeice", "toliss_airbus/iscsinterface/quickDeice"};
    for (const char* c : kCmds) if (n == c) return true;
    return false;
}
bool exists(const std::string& name) {
    if (g_refs.count(name) && g_refs[name]->fromPlugin) return true;
    if (isCommandName(name)) return false;
    if (startsWith(name, "sim/")) return true;
    for (const std::string& p : g_extraPrefixes) if (startsWith(name, p)) return true;
    return false;
}
Dataref* dataref(const std::string& name, bool create) {
    auto it = g_refs.find(name);
    if (it != g_refs.end()) return it->second.get();
    if (!create) return nullptr;
    auto d = std::make_unique<Dataref>();
    d->name = name;
    d->types = xplmType_Int | xplmType_Float | xplmType_Double;
    Dataref* p = d.get();
    g_refs[name] = std::move(d);
    return p;
}
void setNum(const std::string& n, double v) { Dataref* d = dataref(n); if (!d->fromPlugin) d->value = v; }
double getNum(const std::string& n) { Dataref* d = dataref(n, false); return d ? d->value : 0; }
void setFloats(const std::string& n, const std::vector<float>& v) { Dataref* d = dataref(n); if (d->fromPlugin) return; d->vf = v; d->types = xplmType_FloatArray; }
void setInts(const std::string& n, const std::vector<int>& v) { Dataref* d = dataref(n); if (d->fromPlugin) return; d->vi = v; d->types = xplmType_IntArray; }
void setBytes(const std::string& n, const std::string& b) { Dataref* d = dataref(n); if (d->fromPlugin) return; d->bytes = b; d->types = xplmType_Data; }
std::vector<Dataref*> allDatarefs() {
    std::vector<Dataref*> v;
    for (auto& kv : g_refs) if (exists(kv.first)) v.push_back(kv.second.get());
    return v;
}
const std::vector<std::string>& missingLookups() { return g_missing; }
void setExtraPrefixes(const std::vector<std::string>& p) { g_extraPrefixes = p; }

// ---- commands ----------------------------------------------------------------------------------------------------------------------------
namespace {
Cmd* cmdOf(const std::string& name, bool create) {
    auto it = g_cmds.find(name);
    if (it != g_cmds.end()) return it->second.get();
    if (!create) return nullptr;
    auto c = std::make_unique<Cmd>();
    c->name = name;
    Cmd* p = c.get();
    g_cmds[name] = std::move(c);
    return p;
}
void sendCommand(Cmd* c, bool fromPlugin) {
    if (fromPlugin) {
        ++c->sent;
        g_cmdLog.push_back(c->name);
        if (g_cmdLog.size() > 300) g_cmdLog.erase(g_cmdLog.begin(), g_cmdLog.begin() + 100);
        log("[X-Plane] command %s%s", c->name.c_str(), c->handlers.empty() ? " (nothing in the emulator handles it)" : "");
    }
    const std::vector<CmdHandler> hs = c->handlers;   // a handler may unregister
    for (int phase : {xplm_CommandBegin, xplm_CommandEnd})
        for (const CmdHandler& h : hs) if (h.cb) h.cb((XPLMCommandRef)c, (XPLMCommandPhase)phase, h.refcon);
}
} // namespace
std::vector<Command> commands() {
    std::vector<Command> v;
    for (auto& kv : g_cmds) v.push_back({kv.second->name, kv.second->desc, kv.second->fromPlugin, kv.second->sent});
    return v;
}
bool runCommand(const std::string& name) {
    Cmd* c = cmdOf(name, false);
    if (!c) return false;
    sendCommand(c, false);
    return true;
}
const std::vector<std::string>& commandLog() { return g_cmdLog; }

// ---- menus ---------------------------------------------------------------------------------------------------------------------------------
std::vector<MenuItem> menuItems() {
    std::vector<MenuItem> v;
    std::function<void(Menu*, const std::string&)> walk = [&](Menu* m, const std::string& path) {
        for (size_t i = 0; i < m->items.size(); ++i) {
            const Menu::Item& it = m->items[i];
            if (it.removed) continue;
            const std::string p = path.empty() ? it.name : path + " > " + it.name;
            if (it.sub && !it.sub->destroyed) { walk(it.sub, p); continue; }
            int idx = 0;
            for (size_t k = 0; k < g_menus.size(); ++k) if (g_menus[k].get() == m) idx = (int)k;
            v.push_back({p, idx, (int)i});
        }
    };
    walk(pluginsMenu(), "");
    return v;
}
void pickMenuItem(const MenuItem& mi) {
    if (mi.menu < 0 || mi.menu >= (int)g_menus.size()) return;
    Menu* m = g_menus[mi.menu].get();
    if (mi.index < 0 || mi.index >= (int)m->items.size() || !m->handler) return;
    m->handler(m->refcon, m->items[mi.index].ref);
}

void setFakePlugin(const std::string& sig, const std::string& name, const std::string& path, bool present) {
    g_fakes.erase(std::remove_if(g_fakes.begin(), g_fakes.end(), [&](const FakePlugin& f) { return f.sig == sig; }), g_fakes.end());
    if (present) g_fakes.push_back({sig, name, path});
}

// ---- the screen ------------------------------------------------------------------------------------------------------------------------------
void setScreen(int w, int h, float px) {
    g_screenW = std::max(64, w); g_screenH = std::max(64, h); g_pxPerBoxel = std::max(0.5f, px);
    setNum("sim/graphics/misc/user_interface_scale", g_pxPerBoxel);
    setNum("sim/graphics/view/window_width", g_screenW * g_pxPerBoxel);
    setNum("sim/graphics/view/window_height", g_screenH * g_pxPerBoxel);
    // keep the plugin's windows on screen
    for (auto& w2 : g_wins) {
        Win* w3 = w2.get();
        if (w3->dead || w3->layer == xplm_WindowLayerFlightOverlay) continue;
        const int ww = w3->r - w3->l, wh = w3->t - w3->b;
        if (w3->r > g_screenW) { w3->r = g_screenW - 4; w3->l = w3->r - ww; }
        if (w3->t + kTitleH > g_screenH) { w3->t = g_screenH - kTitleH - 4; w3->b = w3->t - wh; }
        if (w3->l < 0) { w3->l = 4; w3->r = w3->l + ww; }
        if (w3->b < 0) { w3->b = 4; w3->t = w3->b + wh; }
    }
}
void setView(View v) { g_view = v; }
View view() { return g_view; }

namespace {
// the cockpit view: the panel rectangle around the tablet's area, letterboxed into the screen
bool panelRect(float& l, float& b, float& w, float& h) {
    const int pl = (int)getNum("avitab/panel_left"), pb = (int)getNum("avitab/panel_bottom");
    const int pw = (int)getNum("avitab/panel_width"), ph = (int)getNum("avitab/panel_height");
    Dataref* dl = dataref("avitab/panel_left", false);
    if (!dl || !dl->fromPlugin) return false;
    int vl = pl, vb = pb, vw = pw, vh = ph;
    if (dl->geti) { vl = ((XPLMGetDatai_f)dl->geti)(dl->readRefcon); }
    Dataref* r;
    if ((r = dataref("avitab/panel_bottom", false)) && r->geti) vb = ((XPLMGetDatai_f)r->geti)(r->readRefcon);
    if ((r = dataref("avitab/panel_width", false)) && r->geti) vw = ((XPLMGetDatai_f)r->geti)(r->readRefcon);
    if ((r = dataref("avitab/panel_height", false)) && r->geti) vh = ((XPLMGetDatai_f)r->geti)(r->readRefcon);
    if (vw <= 0 || vh <= 0) return false;
    const float m = 0.18f;   // some of the panel around it
    l = vl - vw * m; b = vb - vh * m; w = vw * (1 + 2 * m); h = vh * (1 + 2 * m);
    return true;
}
// screen boxels <-> panel pixels in the cockpit view
void cockpitTransform(float& sx, float& sy, float& k) {
    float l, b, w, h;
    if (!panelRect(l, b, w, h)) { sx = sy = 0; k = 1; return; }
    k = std::min(g_screenW / w, g_screenH / h);
    sx = (g_screenW - w * k) * 0.5f - l * k;
    sy = (g_screenH - h * k) * 0.5f - b * k;
}
void setClick3d(float x, float y) {
    if (g_view != View::Cockpit) { setNum("sim/graphics/view/click_3d_x_pixels", -1); setNum("sim/graphics/view/click_3d_y_pixels", -1); return; }
    float sx, sy, k;
    cockpitTransform(sx, sy, k);
    setNum("sim/graphics/view/click_3d_x_pixels", (x - sx) / k);
    setNum("sim/graphics/view/click_3d_y_pixels", (y - sy) / k);
}
} // namespace
bool cockpitArea(float& l, float& b, float& w, float& h) { return panelRect(l, b, w, h); }

// ---- input --------------------------------------------------------------------------------------------------------------------------------------
namespace {
Win* windowAt(float x, float y, bool withTitle) {
    std::vector<Win*> v = drawOrder();
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        Win* w = *it;
        if (!w->visible) continue;
        const int top = w->t + ((withTitle && decorated(w)) ? kTitleH : 0);
        if (x >= w->l && x <= w->r && y >= w->b && y <= top) return w;
    }
    return nullptr;
}
int clickWin(Win* w, float x, float y, XPLMMouseStatus st, int button) {
    if (button == 1) {
        if (w->p.structSize >= (int)sizeof(XPLMCreateWindow_t) && w->p.handleRightClickFunc) return w->p.handleRightClickFunc((XPLMWindowID)w, (int)x, (int)y, st, w->p.refcon);
        return 0;
    }
    return w->p.handleMouseClickFunc ? w->p.handleMouseClickFunc((XPLMWindowID)w, (int)x, (int)y, st, w->p.refcon) : 0;
}
} // namespace

void mouseMove(float x, float y) {
    g_mouseX = x; g_mouseY = y;
    setClick3d(x, y);
    if (g_drag != DRAG_NONE && g_mouseWin) {
        const int dx = (int)std::lround(x - g_dragX), dy = (int)std::lround(y - g_dragY);
        Win* w = g_mouseWin;
        if (g_drag == DRAG_MOVE) { w->l = g_dragL + dx; w->r = g_dragR + dx; w->t = g_dragT + dy; w->b = g_dragB + dy; }
        else {
            const int nw = std::max(w->minW, std::min(w->maxW, g_dragR - g_dragL + dx)), nh = std::max(w->minH, std::min(w->maxH, g_dragT - g_dragB - dy));
            w->r = w->l + nw; w->b = w->t - nh;
        }
        return;
    }
    if (g_mouseWin && !g_mouseWin->dead) clickWin(g_mouseWin, x, y, xplm_MouseDrag, g_mouseButton);
}
void mouseButton(int button, bool down, float x, float y) {
    g_mouseX = x; g_mouseY = y;
    setClick3d(x, y);
    if (down) {
        Win* w = windowAt(x, y, true);
        if (!w) { if (g_focus) XPLMTakeKeyboardFocus(nullptr); return; }
        if (w->layer == xplm_WindowLayerFloatingWindows) w->z = g_zNext++;
        if (decorated(w) && button == 0) {
            if (y > w->t) {   // its title bar: move it
                g_drag = DRAG_MOVE; g_mouseWin = w; g_dragX = x; g_dragY = y; g_dragL = w->l; g_dragT = w->t; g_dragR = w->r; g_dragB = w->b;
                return;
            }
            if (x > w->r - 14 && y < w->b + 14) {   // its corner: resize it
                g_drag = DRAG_SIZE; g_mouseWin = w; g_dragX = x; g_dragY = y; g_dragL = w->l; g_dragT = w->t; g_dragR = w->r; g_dragB = w->b;
                return;
            }
        }
        if (clickWin(w, x, y, xplm_MouseDown, button)) { g_mouseWin = w; g_mouseButton = button; }
        else if (g_focus && g_focus != w) XPLMTakeKeyboardFocus(nullptr);
        return;
    }
    if (g_drag != DRAG_NONE) { g_drag = DRAG_NONE; g_mouseWin = nullptr; return; }
    if (g_mouseWin && !g_mouseWin->dead && g_mouseButton == button) clickWin(g_mouseWin, x, y, xplm_MouseUp, button);
    g_mouseWin = nullptr;
}
void mouseWheel(float x, float y, int clicks) {
    Win* w = windowAt(x, y, false);
    if (w && w->p.handleMouseWheelFunc) w->p.handleMouseWheelFunc((XPLMWindowID)w, (int)x, (int)y, 0, clicks, w->p.refcon);
}
void keyChar(unsigned int c) {
    if (!g_focus || !g_focus->p.handleKeyFunc || c >= 128) return;
    const char ch = (char)c;
    int vk = (unsigned char)ch;
    if (ch >= 'a' && ch <= 'z') vk = ch - 'a' + XPLM_VK_A;
    const int shift = (ch >= 'A' && ch <= 'Z') ? xplm_ShiftFlag : 0;
    g_focus->p.handleKeyFunc((XPLMWindowID)g_focus, ch, (XPLMKeyFlags)(xplm_DownFlag | shift), (char)vk, g_focus->p.refcon, 0);
    if (g_focus) g_focus->p.handleKeyFunc((XPLMWindowID)g_focus, ch, (XPLMKeyFlags)(xplm_UpFlag | shift), (char)vk, g_focus->p.refcon, 0);
}
void keySpecial(int vk, bool down, int flags) {
    if (!g_focus || !g_focus->p.handleKeyFunc) return;
    char ch = 0;
    if (vk == XPLM_VK_BACK) ch = 8; else if (vk == XPLM_VK_RETURN) ch = 13; else if (vk == XPLM_VK_ESCAPE) ch = 27; else if (vk == XPLM_VK_TAB) ch = 9;
    else if (vk == XPLM_VK_DELETE) ch = 127;
    g_focus->p.handleKeyFunc((XPLMWindowID)g_focus, ch, (XPLMKeyFlags)((down ? xplm_DownFlag : xplm_UpFlag) | flags), (char)vk, g_focus->p.refcon, 0);
}
bool tabletWindowRect(int& l, int& t, int& r, int& b) {
    for (auto& w : g_wins)
        if (!w->dead && w->visible && w->title == "XPTablet") { l = w->l; t = w->t; r = w->r; b = w->b; return true; }
    return false;
}
void setTabletWindowRect(int l, int t, int r, int b) {
    for (auto& w : g_wins) if (!w->dead && w->title == "XPTablet") { w->l = l; w->t = t; w->r = r; w->b = b; }
}
bool tabletPoint(float lx, float ly, float& sx, float& sy) {
    // the plugin fits a 480-unit-high layout, as wide as the window's shape (between 0.6 and 2.6 times as wide as high), centred
    if (g_view == View::Cockpit) {
        float l, b, w, h;
        Dataref* d = dataref("avitab/panel_left", false);
        if (!d || !d->fromPlugin || !panelRect(l, b, w, h)) return false;
        auto geti = [](const char* n) { Dataref* r = dataref(n, false); return (r && r->geti) ? ((XPLMGetDatai_f)r->geti)(r->readRefcon) : 0; };
        const float pl = (float)geti("avitab/panel_left"), pb = (float)geti("avitab/panel_bottom"), pw = (float)geti("avitab/panel_width"), ph = (float)geti("avitab/panel_height");
        // as the plugin's panelPicture: an 800 x 480 picture centred in the rectangle at that shape
        const float aspect = 800.0f / 480.0f;
        float pw2, ph2;
        if (pw / ph > aspect) { ph2 = ph; pw2 = std::floor(ph * aspect); } else { pw2 = pw; ph2 = std::floor(pw / aspect); }
        const float il = pl + std::floor((pw - pw2) * 0.5f), ib = pb + std::floor((ph - ph2) * 0.5f);
        float ox, oy, k;
        cockpitTransform(ox, oy, k);
        sx = ox + (il + lx * pw2 / 800.0f) * k;
        sy = oy + (ib + ph2 - ly * ph2 / 480.0f) * k;
        return true;
    }
    int l, t, r, b;
    if (!tabletWindowRect(l, t, r, b)) return false;
    const float ww = (float)(r - l), wh = (float)(t - b);
    const float logW = 480.0f * std::max(0.6f, std::min(2.6f, ww / wh));
    const float s = std::min(ww / logW, wh / 480.0f);
    const float ox = (ww - logW * s) * 0.5f, oy = (wh - 480.0f * s) * 0.5f;
    sx = l + ox + lx * s;
    sy = t - oy - ly * s;
    return true;
}

// ---- running ----------------------------------------------------------------------------------------------------------------------------------
namespace {
void callPluginStart() {
    char name[256] = "", sig[256] = "", desc[256] = "";
    if (!XPluginStart(name, sig, desc)) { log("[emulator] XPluginStart failed"); return; }
    log("[emulator] started %s (%s): %s", name, sig, desc);
    if (!XPluginEnable()) { log("[emulator] XPluginEnable failed"); return; }
    g_started = true;
    XPluginReceiveMessage(0, XPLM_MSG_PLANE_LOADED, (void*)0);
    // open the tablet's window, as from its menu
    for (const MenuItem& m : menuItems()) if (m.path.find("Show / hide") != std::string::npos) { pickMenuItem(m); break; }
}
void callPluginStop() {
    if (!g_started) return;
    XPluginDisable();
    XPluginStop();
    g_started = false;
    // what a well-behaved plugin leaves behind should be nothing: report what is left
    int wins = 0, loops = 0, cbs = (int)g_drawCbs.size(), plugRefs = 0;
    for (auto& w : g_wins) if (!w->dead) ++wins;
    for (auto& l : g_loops) if (l->alive) ++loops;
    for (auto& kv : g_refs) if (kv.second->fromPlugin) ++plugRefs;
    log("[emulator] after XPluginStop: %d windows, %d flight loops, %d draw callbacks, %d datarefs still registered", wins, loops, cbs, plugRefs);
}
} // namespace

bool start(const Paths& p) {
    g_paths = p;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(p.emuData + "/Output/preferences", ec);
    fs::create_directories(p.xplaneRoot + "/Output", ec);
    log("[emulator] X-Plane folder: %s", p.xplaneRoot.c_str());
    log("[emulator] plugin folder: %s", p.pluginDir.c_str());
    pluginsMenu();
    simApplyProfile();
    callPluginStart();
    return g_started;
}
void stop() { callPluginStop(); }
void restartPlugin() {
    log("[emulator] reloading the plugin");
    callPluginStop();
    callPluginStart();
}

void frame(double dt) {
    g_time += dt;
    ++g_cycle;
    simStep(dt);
    // the flight loops that are due
    for (size_t i = 0; i < g_loops.size(); ++i) {
        Loop* l = g_loops[i].get();
        if (!l->alive || l->interval == 0) continue;
        const bool due = l->interval < 0 ? g_cycle >= l->dueCycle : g_time >= l->due;
        if (!due) continue;
        const float since = (float)(g_time - l->last);
        l->last = g_time;
        const float next = l->cb(since, (float)(g_time - g_lastLoop), ++l->counter, l->refcon);
        if (l->alive) XPLMScheduleFlightLoop((XPLMFlightLoopID)l, next, 1);
    }
    g_lastLoop = g_time;
    // the pointer over a window: its cursor callback
    if (Win* w = windowAt(g_mouseX, g_mouseY, false)) if (w->p.handleCursorFunc) w->p.handleCursorFunc((XPLMWindowID)w, (int)g_mouseX, (int)g_mouseY, w->p.refcon);
    g_wins.erase(std::remove_if(g_wins.begin(), g_wins.end(), [](const std::unique_ptr<Win>& w) { return w->dead && w.get() != g_mouseWin; }), g_wins.end());
}

namespace {
void rect(float l, float b, float r, float t, float R, float G, float B, float A) {
    gl.Color4f(R, G, B, A);
    gl.Begin(GL_QUADS); gl.Vertex2f(l, b); gl.Vertex2f(r, b); gl.Vertex2f(r, t); gl.Vertex2f(l, t); gl.End();
}
void standardState() {
    gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity(); gl.Ortho(0, g_screenW, 0, g_screenH, -1, 1);
    gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
}
} // namespace

void draw() {
    standardState();
    // the "world": a dusk sky over the ground (the emulator draws no scenery)
    gl.Begin(GL_QUADS);
    gl.Color4f(0.20f, 0.33f, 0.52f, 1); gl.Vertex2f(0, g_screenH); gl.Vertex2f((float)g_screenW, (float)g_screenH);
    gl.Color4f(0.55f, 0.66f, 0.78f, 1); gl.Vertex2f((float)g_screenW, g_screenH * 0.42f); gl.Vertex2f(0, g_screenH * 0.42f);
    gl.Color4f(0.26f, 0.30f, 0.24f, 1); gl.Vertex2f(0, g_screenH * 0.42f); gl.Vertex2f((float)g_screenW, g_screenH * 0.42f);
    gl.Color4f(0.17f, 0.20f, 0.16f, 1); gl.Vertex2f((float)g_screenW, 0); gl.Vertex2f(0, 0);
    gl.End();
    if (g_view == View::Cockpit) {   // the panel: dark grey, then the gauges phase in panel pixels (where the plugin draws the tablet)
        float sx, sy, k;
        cockpitTransform(sx, sy, k);
        float l, b, w, h;
        if (panelRect(l, b, w, h)) {
            rect(sx + l * k, sy + b * k, sx + (l + w) * k, sy + (b + h) * k, 0.14f, 0.15f, 0.17f, 1);
            gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
            // X-Plane draws gauges in panel pixels: the same transform as the view
            gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity();
            gl.Ortho(-sx / k, (g_screenW - sx) / k, -sy / k, (g_screenH - sy) / k, -1, 1);
            gl.MatrixMode(GL_MODELVIEW);
            XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
            for (const DrawCb& d : std::vector<DrawCb>(g_drawCbs)) if (d.phase == xplm_Phase_Gauges && !d.before) d.cb(d.phase, 0, d.refcon);
            standardState();
        }
    }
    for (Win* w : drawOrder()) {
        if (!w->visible) continue;
        if (decorated(w)) {   // a frame and title bar like X-Plane's
            rect(w->l - 3.0f, w->b - 3.0f, w->r + 3.0f, w->t + (float)kTitleH, 0.10f, 0.12f, 0.16f, 0.96f);
            rect((float)w->l, (float)w->t + 1, (float)w->r, w->t + kTitleH - 2.0f, 0.18f, 0.21f, 0.27f, 1);
            rect(w->r - 12.0f, w->b - 2.0f, w->r + 2.0f, w->b + 2.0f, 0.5f, 0.55f, 0.62f, 1);   // the resize corner
            rect(w->r - 2.0f, w->b - 2.0f, w->r + 2.0f, w->b + 12.0f, 0.5f, 0.55f, 0.62f, 1);
        }
        standardState();
        if (w->p.drawWindowFunc) w->p.drawWindowFunc((XPLMWindowID)w, w->p.refcon);
        standardState();
    }
    // the window titles (drawn last, with the panel's font: done by the caller through windowTitles())
}

// the decorated windows' titles and rectangles, for the caller to label
std::vector<std::pair<std::string, std::array<int, 4>>> windowTitles();
std::vector<std::pair<std::string, std::array<int, 4>>> windowTitles() {
    std::vector<std::pair<std::string, std::array<int, 4>>> v;
    for (Win* w : drawOrder()) if (w->visible && decorated(w)) v.push_back({w->title, {w->l, w->t, w->r, w->b}});
    return v;
}

} // namespace emu

using namespace emu;

template <class T> static int getArray(const std::vector<T>& src, T* out, int off, int max) {
    if (!out) return (int)src.size();
    int n = 0;
    for (int i = off; i < (int)src.size() && n < max; ++i) out[n++] = src[i];
    return n;
}
template <class T> static void setArray(std::vector<T>& dst, const T* in, int off, int count) {
    if (!in || off < 0 || count <= 0) return;
    if ((int)dst.size() < off + count) dst.resize(off + count);
    for (int i = 0; i < count; ++i) dst[off + i] = in[i];
}

// ==== the XPLM functions ==========================================================================================================================
extern "C" {

// ---- data access ----
XPLM_API XPLMDataRef XPLMFindDataRef(const char* name) {
    if (!name) return nullptr;
    if (!exists(name)) {
        if (std::find(g_missing.begin(), g_missing.end(), name) == g_missing.end()) g_missing.push_back(name);
        return nullptr;
    }
    Dataref* d = dataref(name);
    d->touched = true;
    return (XPLMDataRef)d;
}
XPLM_API XPLMDataTypeID XPLMGetDataRefTypes(XPLMDataRef r) { return r ? (XPLMDataTypeID)((Dataref*)r)->types : xplmType_Unknown; }
XPLM_API int XPLMCanWriteDataRef(XPLMDataRef r) { return r && ((Dataref*)r)->writable ? 1 : 0; }
XPLM_API int XPLMIsDataRefGood(XPLMDataRef r) { return r ? 1 : 0; }
XPLM_API int XPLMGetDatai(XPLMDataRef r) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->geti ? ((XPLMGetDatai_f)d->geti)(d->readRefcon) : 0;
    return (int)std::lround(d->value);
}
XPLM_API void XPLMSetDatai(XPLMDataRef r, int v) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    if (d->fromPlugin) { if (d->seti) ((XPLMSetDatai_f)d->seti)(d->writeRefcon, v); return; }
    d->value = v;
}
XPLM_API float XPLMGetDataf(XPLMDataRef r) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->getf ? ((XPLMGetDataf_f)d->getf)(d->readRefcon) : 0;
    return (float)d->value;
}
XPLM_API void XPLMSetDataf(XPLMDataRef r, float v) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    if (d->fromPlugin) { if (d->setf) ((XPLMSetDataf_f)d->setf)(d->writeRefcon, v); return; }
    d->value = v;
}
XPLM_API double XPLMGetDatad(XPLMDataRef r) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->getd ? ((XPLMGetDatad_f)d->getd)(d->readRefcon) : 0;
    return d->value;
}
XPLM_API void XPLMSetDatad(XPLMDataRef r, double v) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    if (d->fromPlugin) { if (d->setd) ((XPLMSetDatad_f)d->setd)(d->writeRefcon, v); return; }
    d->value = v;
}
XPLM_API int XPLMGetDatavi(XPLMDataRef r, int* out, int off, int max) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->getvi ? ((XPLMGetDatavi_f)d->getvi)(d->readRefcon, out, off, max) : 0;
    return getArray(d->vi, out, off, max);
}
XPLM_API void XPLMSetDatavi(XPLMDataRef r, int* in, int off, int count) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    if (d->fromPlugin) { if (d->setvi) ((XPLMSetDatavi_f)d->setvi)(d->writeRefcon, in, off, count); return; }
    setArray(d->vi, in, off, count);
}
XPLM_API int XPLMGetDatavf(XPLMDataRef r, float* out, int off, int max) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->getvf ? ((XPLMGetDatavf_f)d->getvf)(d->readRefcon, out, off, max) : 0;
    return getArray(d->vf, out, off, max);
}
XPLM_API void XPLMSetDatavf(XPLMDataRef r, float* in, int off, int count) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    if (d->fromPlugin) { if (d->setvf) ((XPLMSetDatavf_f)d->setvf)(d->writeRefcon, in, off, count); return; }
    setArray(d->vf, in, off, count);
}
XPLM_API int XPLMGetDatab(XPLMDataRef r, void* out, int off, int max) {
    Dataref* d = (Dataref*)r;
    if (!d) return 0;
    if (d->fromPlugin) return d->getb ? ((XPLMGetDatab_f)d->getb)(d->readRefcon, out, off, max) : 0;
    if (!out) return (int)d->bytes.size();
    int n = 0;
    for (int i = off; i < (int)d->bytes.size() && n < max; ++i) ((char*)out)[n++] = d->bytes[i];
    return n;
}
XPLM_API void XPLMSetDatab(XPLMDataRef r, void* in, int off, int count) {
    Dataref* d = (Dataref*)r;
    if (!d || !in || count <= 0) return;
    if (d->fromPlugin) { if (d->setb) ((XPLMSetDatab_f)d->setb)(d->writeRefcon, in, off, count); return; }
    if ((int)d->bytes.size() < off + count) d->bytes.resize(off + count);
    std::memcpy(&d->bytes[off], in, count);
}
XPLM_API XPLMDataRef XPLMRegisterDataAccessor(const char* name, XPLMDataTypeID type, int writable, XPLMGetDatai_f ri, XPLMSetDatai_f wi,
                                              XPLMGetDataf_f rf, XPLMSetDataf_f wf, XPLMGetDatad_f rd, XPLMSetDatad_f wd, XPLMGetDatavi_f rvi,
                                              XPLMSetDatavi_f wvi, XPLMGetDatavf_f rvf, XPLMSetDatavf_f wvf, XPLMGetDatab_f rb, XPLMSetDatab_f wb,
                                              void* readRefcon, void* writeRefcon) {
    Dataref* d = dataref(name);
    d->fromPlugin = true; d->types = type; d->writable = writable != 0;
    d->geti = (void*)ri; d->seti = (void*)wi; d->getf = (void*)rf; d->setf = (void*)wf; d->getd = (void*)rd; d->setd = (void*)wd;
    d->getvi = (void*)rvi; d->setvi = (void*)wvi; d->getvf = (void*)rvf; d->setvf = (void*)wvf; d->getb = (void*)rb; d->setb = (void*)wb;
    d->readRefcon = readRefcon; d->writeRefcon = writeRefcon;
    return (XPLMDataRef)d;
}
XPLM_API void XPLMUnregisterDataAccessor(XPLMDataRef r) {
    Dataref* d = (Dataref*)r;
    if (!d) return;
    d->fromPlugin = false;
    d->geti = d->seti = d->getf = d->setf = d->getd = d->setd = d->getvi = d->setvi = d->getvf = d->setvf = d->getb = d->setb = nullptr;
}

// ---- windows ----
XPLM_API XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) {
    auto w = std::make_unique<Win>();
    std::memcpy(&w->p, p, std::min((size_t)p->structSize, sizeof(XPLMCreateWindow_t)));
    w->l = p->left; w->t = p->top; w->r = p->right; w->b = p->bottom; w->visible = p->visible != 0;
    if (p->structSize >= (int)sizeof(XPLMCreateWindow_t)) { w->layer = p->layer; w->decoration = p->decorateAsFloatingWindow; }
    w->z = g_zNext++;
    Win* raw = w.get();
    g_wins.push_back(std::move(w));
    return (XPLMWindowID)raw;
}
XPLM_API void XPLMDestroyWindow(XPLMWindowID id) {
    Win* w = winOf(id);
    if (!w) return;
    w->dead = true; w->visible = false;
    if (g_focus == w) g_focus = nullptr;
    if (g_mouseWin == w) g_mouseWin = nullptr;
}
XPLM_API void XPLMGetScreenSize(int* w, int* h) { if (w) *w = g_screenW; if (h) *h = g_screenH; }
XPLM_API void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) { if (l) *l = 0; if (t) *t = g_screenH; if (r) *r = g_screenW; if (b) *b = 0; }
XPLM_API void XPLMGetWindowGeometry(XPLMWindowID id, int* l, int* t, int* r, int* b) {
    Win* w = winOf(id);
    if (!w) return;
    if (l) *l = w->l; if (t) *t = w->t; if (r) *r = w->r; if (b) *b = w->b;
}
XPLM_API void XPLMSetWindowGeometry(XPLMWindowID id, int l, int t, int r, int b) { if (Win* w = winOf(id)) { w->l = l; w->t = t; w->r = r; w->b = b; } }
XPLM_API int XPLMGetWindowIsVisible(XPLMWindowID id) { Win* w = winOf(id); return w && w->visible ? 1 : 0; }
XPLM_API void XPLMSetWindowIsVisible(XPLMWindowID id, int v) {
    Win* w = winOf(id);
    if (!w) return;
    w->visible = v != 0;
    if (!w->visible && g_focus == w) g_focus = nullptr;
}
XPLM_API void XPLMTakeKeyboardFocus(XPLMWindowID id) {
    Win* w = id ? winOf(id) : nullptr;
    if (g_focus && g_focus != w && g_focus->p.handleKeyFunc) g_focus->p.handleKeyFunc((XPLMWindowID)g_focus, 0, 0, 0, g_focus->p.refcon, 1);   // losing it
    g_focus = w;
}
XPLM_API int XPLMHasKeyboardFocus(XPLMWindowID id) { return id ? (g_focus == winOf(id) && g_focus != nullptr) : (g_focus == nullptr); }
XPLM_API void XPLMBringWindowToFront(XPLMWindowID id) { if (Win* w = winOf(id)) w->z = g_zNext++; }
XPLM_API int XPLMIsWindowInFront(XPLMWindowID id) { std::vector<Win*> v = drawOrder(); return !v.empty() && v.back() == winOf(id); }
XPLM_API void XPLMSetWindowPositioningMode(XPLMWindowID id, XPLMWindowPositioningMode mode, int) {
    if (mode != xplm_WindowPositionFree && winOf(id)) log("[X-Plane] window positioning mode %d (the emulator keeps windows free)", (int)mode);
}
XPLM_API void XPLMSetWindowResizingLimits(XPLMWindowID id, int minW, int minH, int maxW, int maxH) {
    if (Win* w = winOf(id)) { w->minW = minW; w->minH = minH; w->maxW = maxW; w->maxH = maxH; }
}
XPLM_API void XPLMSetWindowTitle(XPLMWindowID id, const char* t) { if (Win* w = winOf(id)) w->title = t ? t : ""; }
XPLM_API int XPLMWindowIsPoppedOut(XPLMWindowID) { return 0; }
XPLM_API int XPLMWindowIsInVR(XPLMWindowID) { return 0; }
XPLM_API void XPLMGetMouseLocationGlobal(int* x, int* y) { if (x) *x = (int)g_mouseX; if (y) *y = (int)g_mouseY; }
XPLM_API int XPLMRegisterDrawCallback(XPLMDrawCallback_f cb, XPLMDrawingPhase phase, int before, void* refcon) {
    g_drawCbs.push_back({cb, phase, before, refcon});
    return 1;
}
XPLM_API int XPLMUnregisterDrawCallback(XPLMDrawCallback_f cb, XPLMDrawingPhase phase, int before, void* refcon) {
    const size_t n = g_drawCbs.size();
    g_drawCbs.erase(std::remove_if(g_drawCbs.begin(), g_drawCbs.end(), [&](const DrawCb& d) { return d.cb == cb && d.phase == phase && d.before == before && d.refcon == refcon; }), g_drawCbs.end());
    return g_drawCbs.size() != n;
}

// ---- graphics ----
XPLM_API void XPLMSetGraphicsState(int fog, int texUnits, int lighting, int alphaTest, int alphaBlend, int depthTest, int depthWrite) {
    (void)depthWrite;
    if (fog) gl.Enable(GL_FOG); else gl.Disable(GL_FOG);
    if (lighting) gl.Enable(GL_LIGHTING); else gl.Disable(GL_LIGHTING);
    if (alphaTest) gl.Enable(GL_ALPHA_TEST); else gl.Disable(GL_ALPHA_TEST);
    if (alphaBlend) { gl.Enable(GL_BLEND); gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); } else gl.Disable(GL_BLEND);
    if (depthTest) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
    if (texUnits > 0) gl.Enable(GL_TEXTURE_2D); else gl.Disable(GL_TEXTURE_2D);
}
XPLM_API void XPLMBindTexture2d(int tex, int unit) {
    if (unit > 0 && gl.ActiveTexture) gl.ActiveTexture(GL_TEXTURE0 + unit);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)tex);
    if (unit > 0 && gl.ActiveTexture) gl.ActiveTexture(GL_TEXTURE0);
}
XPLM_API void XPLMGenerateTextureNumbers(int* out, int n) {
    std::vector<GLuint> t(std::max(0, n));
    if (n > 0) gl.GenTextures(n, t.data());
    for (int i = 0; i < n; ++i) out[i] = (int)t[i];
}

// ---- processing ----
XPLM_API XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* p) {
    auto l = std::make_unique<Loop>();
    l->cb = p->callbackFunc; l->refcon = p->refcon; l->last = g_time;
    Loop* raw = l.get();
    g_loops.push_back(std::move(l));
    return (XPLMFlightLoopID)raw;
}
XPLM_API void XPLMDestroyFlightLoop(XPLMFlightLoopID id) { for (auto& l : g_loops) if (l.get() == (Loop*)id) { l->alive = false; l->interval = 0; } }
XPLM_API void XPLMScheduleFlightLoop(XPLMFlightLoopID id, float interval, int) {
    for (auto& l : g_loops)
        if (l.get() == (Loop*)id) {
            l->interval = interval;
            if (interval < 0) l->dueCycle = g_cycle + (int)std::lround(-interval);
            else if (interval > 0) l->due = g_time + interval;
        }
}
XPLM_API float XPLMGetElapsedTime(void) { return (float)g_time; }
XPLM_API int XPLMGetCycleNumber(void) { return g_cycle; }

// ---- utilities ----
XPLM_API void XPLMGetSystemPath(char* out) { std::snprintf(out, 512, "%s/", g_paths.xplaneRoot.c_str()); }
XPLM_API void XPLMGetPrefsPath(char* out) { std::snprintf(out, 512, "%s/Output/preferences/Set X-Plane.prf", g_paths.emuData.c_str()); }
XPLM_API const char* XPLMGetDirectorySeparator(void) { return "/"; }
XPLM_API void XPLMDebugString(const char* s) { if (s) log("%s", s); }
XPLM_API void XPLMEnableFeature(const char*, int) {}
XPLM_API int XPLMHasFeature(const char*) { return 1; }
XPLM_API int XPLMIsFeatureEnabled(const char*) { return 1; }
XPLM_API XPLMCommandRef XPLMFindCommand(const char* name) {
    if (!name) return nullptr;
    Cmd* c = cmdOf(name, false);
    if (c) return (XPLMCommandRef)c;
    if (!isCommandName(name)) return nullptr;   // X-Plane's and the aircraft's own commands are there; datarefs and others are not
    if (!startsWith(name, "sim/")) { bool ok = false; for (const std::string& p : g_extraPrefixes) if (startsWith(name, p)) ok = true; if (!ok) return nullptr; }
    return (XPLMCommandRef)cmdOf(name, true);
}
XPLM_API void XPLMCommandOnce(XPLMCommandRef r) { if (r) sendCommand((Cmd*)r, true); }
XPLM_API void XPLMCommandBegin(XPLMCommandRef r) { if (r) sendCommand((Cmd*)r, true); }
XPLM_API void XPLMCommandEnd(XPLMCommandRef) {}
XPLM_API XPLMCommandRef XPLMCreateCommand(const char* name, const char* desc) {
    Cmd* c = cmdOf(name, true);
    c->fromPlugin = true;
    c->desc = desc ? desc : "";
    return (XPLMCommandRef)c;
}
XPLM_API void XPLMRegisterCommandHandler(XPLMCommandRef r, XPLMCommandCallback_f cb, int before, void* refcon) { if (r) ((Cmd*)r)->handlers.push_back({cb, before, refcon}); }
XPLM_API void XPLMUnregisterCommandHandler(XPLMCommandRef r, XPLMCommandCallback_f cb, int before, void* refcon) {
    if (!r) return;
    auto& h = ((Cmd*)r)->handlers;
    h.erase(std::remove_if(h.begin(), h.end(), [&](const CmdHandler& x) { return x.cb == cb && x.before == before && x.refcon == refcon; }), h.end());
}

// ---- planes ----
XPLM_API void XPLMGetNthAircraftModel(int index, char* file, char* path) {
    const Profile& p = profiles()[std::max(0, std::min((int)profiles().size() - 1, sim().profile))];
    const std::string dir = g_paths.emuData + "/Aircraft/" + p.folder;
    if (file) std::snprintf(file, 256, "%s", index == 0 ? p.acfFile.c_str() : "");
    if (path) std::snprintf(path, 512, "%s", index == 0 ? (dir + "/" + p.acfFile).c_str() : "");
}

// ---- plugins ----
XPLM_API XPLMPluginID XPLMGetMyID(void) { return 1; }
XPLM_API int XPLMCountPlugins(void) { return 1 + (int)g_fakes.size(); }
XPLM_API XPLMPluginID XPLMGetNthPlugin(int i) { return i >= 0 && i <= (int)g_fakes.size() ? i + 1 : XPLM_NO_PLUGIN_ID; }
XPLM_API XPLMPluginID XPLMFindPluginByPath(const char*) { return XPLM_NO_PLUGIN_ID; }
XPLM_API XPLMPluginID XPLMFindPluginBySignature(const char* sig) {
    if (!sig) return XPLM_NO_PLUGIN_ID;
    if (std::strcmp(sig, "com.alpineaviation.xptablet") == 0) return 1;
    for (size_t i = 0; i < g_fakes.size(); ++i) if (g_fakes[i].sig == sig) return (XPLMPluginID)(i + 2);
    return XPLM_NO_PLUGIN_ID;
}
XPLM_API void XPLMGetPluginInfo(XPLMPluginID id, char* name, char* path, char* sig, char* desc) {
    std::string n, p, s, d;
    if (id == 1) {
#if defined(_WIN32)
        const char* plat = "win_x64";
#elif defined(__APPLE__)
        const char* plat = "mac_x64";
#else
        const char* plat = "lin_x64";
#endif
        n = "XPTablet"; p = g_paths.pluginDir + "/" + plat + "/XPTablet.xpl"; s = "com.alpineaviation.xptablet"; d = "XPTablet (in the emulator)";
    } else if (id >= 2 && id - 2 < (int)g_fakes.size()) {
        n = g_fakes[id - 2].name; p = g_fakes[id - 2].path; s = g_fakes[id - 2].sig; d = n + " (stand-in)";
    }
    if (name) std::snprintf(name, 256, "%s", n.c_str());
    if (path) std::snprintf(path, 512, "%s", p.c_str());
    if (sig) std::snprintf(sig, 256, "%s", s.c_str());
    if (desc) std::snprintf(desc, 256, "%s", d.c_str());
}
XPLM_API int XPLMIsPluginEnabled(XPLMPluginID id) { return id >= 1 && id <= 1 + (int)g_fakes.size(); }
XPLM_API void XPLMSendMessageToPlugin(XPLMPluginID id, int msg, void* param) {
    if (id == 1) { XPluginReceiveMessage(0, msg, param); return; }
    if (id >= 2 && id - 2 < (int)g_fakes.size()) log("[X-Plane] message %d to %s (a stand-in: it does not answer)", msg, g_fakes[id - 2].name.c_str());
}

// ---- menus ----
XPLM_API XPLMMenuID XPLMFindPluginsMenu(void) { return (XPLMMenuID)pluginsMenu(); }
XPLM_API XPLMMenuID XPLMFindAircraftMenu(void) { return nullptr; }
XPLM_API XPLMMenuID XPLMCreateMenu(const char* name, XPLMMenuID parent, int parentItem, XPLMMenuHandler_f handler, void* refcon) {
    auto m = std::make_unique<Menu>();
    m->name = name ? name : ""; m->parent = (Menu*)parent; m->parentItem = parentItem; m->handler = handler; m->refcon = refcon;
    Menu* raw = m.get();
    g_menus.push_back(std::move(m));
    if (raw->parent && parentItem >= 0 && parentItem < (int)raw->parent->items.size()) raw->parent->items[parentItem].sub = raw;
    return (XPLMMenuID)raw;
}
XPLM_API void XPLMDestroyMenu(XPLMMenuID m) { if (m) ((Menu*)m)->destroyed = true; }
XPLM_API void XPLMClearAllMenuItems(XPLMMenuID m) { if (m) ((Menu*)m)->items.clear(); }
XPLM_API int XPLMAppendMenuItem(XPLMMenuID m, const char* name, void* ref, int) {
    if (!m) return -1;
    Menu* mm = (Menu*)m;
    mm->items.push_back({name ? name : "", ref});
    return (int)mm->items.size() - 1;
}
XPLM_API void XPLMRemoveMenuItem(XPLMMenuID m, int index) { if (m && index >= 0 && index < (int)((Menu*)m)->items.size()) ((Menu*)m)->items[index].removed = true; }
XPLM_API void XPLMCheckMenuItem(XPLMMenuID, int, XPLMMenuCheck) {}
XPLM_API void XPLMEnableMenuItem(XPLMMenuID, int, int) {}

} // extern "C"
