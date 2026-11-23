// xptbrowser: XPTablet's web browser, a program of its own started by the plugin (src/browser.cpp). It runs Chromium through
// X-Plane's own copy of the Chromium Embedded Framework (CEF 117.1.4, in X-Plane/Resources/dlls/64/cef/<platform>): nothing of
// Chromium is shipped with XPTablet, and as a separate process it cannot clash with X-Plane's own use of CEF.
//
// The page is drawn off screen ("windowless") at the tablet's pixel density; each picture goes to the plugin through shared
// memory (src/browser_ipc.h), and the plugin's commands (address, back, mouse, keys, size) come back the same way.
// Cookies and logins are kept in a profile folder (X-Plane/Output/XPTablet/browser), so sites stay signed in.
//
// Only CEF's C API is used, with libcef loaded at run time, so the program builds from CEF's headers alone
// (third_party/cef, the exact commit of X-Plane's build). Chromium starts this same program again for its other processes
// (renderer, GPU, utility): those find libcef through XPTB_CEF_DIR, which the first one sets.
//
//   xptbrowser --xptb-cef-dir=<X-Plane's cef folder> --xptb-profile=<folder> --xptb-parent=<plugin's process id>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "include/capi/cef_display_handler_capi.h"
#include "include/capi/cef_frame_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_render_handler_capi.h"
#include "include/cef_api_hash.h"

#include "browser_ipc.h"

namespace {

// ---- libcef, loaded at run time ----------------------------------------------------------------------------------------------------
struct Cef {
    int (*execute_process)(const cef_main_args_t*, cef_app_t*, void*) = nullptr;
    int (*initialize)(const cef_main_args_t*, const cef_settings_t*, cef_app_t*, void*) = nullptr;
    void (*do_message_loop_work)() = nullptr;
    void (*shutdown)() = nullptr;
    cef_browser_t* (*create_browser_sync)(const cef_window_info_t*, cef_client_t*, const cef_string_t*, const cef_browser_settings_t*,
                                          cef_dictionary_value_t*, cef_request_context_t*) = nullptr;
    int (*utf8_to_utf16)(const char*, size_t, cef_string_utf16_t*) = nullptr;
    int (*utf16_to_utf8)(const char16_t*, size_t, cef_string_utf8_t*) = nullptr;
    void (*utf16_clear)(cef_string_utf16_t*) = nullptr;
    void (*utf8_clear)(cef_string_utf8_t*) = nullptr;
    const char* (*api_hash)(int) = nullptr;
    int (*version_info)(int) = nullptr;   // 2..5: Chromium's version numbers
} cef;

#if defined(_WIN32)
HMODULE g_lib = nullptr;
template <class F> bool sym(F& f, const char* n) { f = (F)GetProcAddress(g_lib, n); return f != nullptr; }
#else
void* g_lib = nullptr;
template <class F> bool sym(F& f, const char* n) { f = (F)dlsym(g_lib, n); return f != nullptr; }
#endif

std::string g_cefDir;

bool loadCef(std::string& err) {
#if defined(_WIN32)
    const std::string path = g_cefDir + "\\libcef.dll";
    std::wstring w(path.size() + 1, L'\0');
    w.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], (int)w.size()) - 1);
    // LOAD_WITH_ALTERED_SEARCH_PATH: libcef's own neighbours (chrome_elf.dll, ...) are found next to it
    g_lib = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_lib) { err = "cannot load " + path + " (error " + std::to_string(GetLastError()) + ")"; return false; }
#else
    const std::string path = g_cefDir + "/libcef.so";
    g_lib = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!g_lib) { const char* e = dlerror(); err = "cannot load " + path + (e ? std::string(": ") + e : std::string()); return false; }
#endif
    bool ok = true;
    ok &= sym(cef.execute_process, "cef_execute_process");
    ok &= sym(cef.initialize, "cef_initialize");
    ok &= sym(cef.do_message_loop_work, "cef_do_message_loop_work");
    ok &= sym(cef.shutdown, "cef_shutdown");
    ok &= sym(cef.create_browser_sync, "cef_browser_host_create_browser_sync");
    ok &= sym(cef.utf8_to_utf16, "cef_string_utf8_to_utf16");
    ok &= sym(cef.utf16_to_utf8, "cef_string_utf16_to_utf8");
    ok &= sym(cef.utf16_clear, "cef_string_utf16_clear");
    ok &= sym(cef.utf8_clear, "cef_string_utf8_clear");
    sym(cef.api_hash, "cef_api_hash");
    sym(cef.version_info, "cef_version_info");
    if (!ok) { err = "libcef lacks a function XPTablet needs"; return false; }
    return true;
}

// ---- strings ---------------------------------------------------------------------------------------------------------------------
void setStr(cef_string_t* s, const std::string& v) { cef.utf8_to_utf16(v.c_str(), v.size(), s); }
std::string toUtf8(const cef_string_t* s) {
    if (!s || !s->str || !s->length) return std::string();
    cef_string_utf8_t o = {};
    cef.utf16_to_utf8(s->str, s->length, &o);
    std::string r(o.str ? o.str : "", o.length);
    cef.utf8_clear(&o);
    return r;
}
void copyTo(char* dst, size_t n, const std::string& v) { std::snprintf(dst, n, "%s", v.c_str()); }

// ---- shared memory with the plugin -----------------------------------------------------------------------------------------------------
xptb::Shared* g_shm = nullptr;

bool openShared(std::string& err) {
#if defined(_WIN32)
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, (std::string("Local\\") + xptb::shmName() + "_" + std::getenv("XPTB_PARENT")).c_str());
    if (!h) { err = "no shared memory from the plugin"; return false; }
    g_shm = (xptb::Shared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(xptb::Shared));
#else
    const std::string name = std::string("/") + xptb::shmName() + "_" + std::getenv("XPTB_PARENT");
    const int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) { err = "no shared memory from the plugin"; return false; }
    void* p = mmap(nullptr, sizeof(xptb::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    g_shm = p == MAP_FAILED ? nullptr : (xptb::Shared*)p;
#endif
    if (!g_shm || g_shm->magic != xptb::kMagic || g_shm->version != xptb::kVersion) { err = "shared memory of another version"; g_shm = nullptr; return false; }
    return true;
}

void fail(const std::string& why) {
    std::fprintf(stderr, "xptbrowser: %s\n", why.c_str());
    if (g_shm) { copyTo(g_shm->error, sizeof g_shm->error, why); g_shm->state.store(-1); }
}

bool parentAlive(long pid) {
#if defined(_WIN32)
    static HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    return !h || WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

// ---- pages.log (in the browser profile, new each start): what the pages say (console warnings and errors), loads that failed,
// pop-ups, and page processes that stopped; Chromium's own log (xptbrowser.log) does not have the pages' side
FILE* g_pageLog = nullptr;
void pageLog(const char* fmt, ...) {
    if (!g_pageLog) return;
    const std::time_t now = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&now));
    std::fprintf(g_pageLog, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_pageLog, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_pageLog);
    std::fflush(g_pageLog);
}

// ---- the tabs and the shown tab's picture ---------------------------------------------------------------------------------------
int g_viewW = 800, g_viewH = 400;    // device-independent pixels (every tab is this size)
float g_scale = 1.0f;                // screen pixels per one of them

struct Tab {
    int id = 0;                      // ours, given to the plugin
    int group = 0;                   // 0 the Browser's, else a web app's (the plugin keeps them apart; a pop-up joins its opener's)
    cef_browser_t* br = nullptr;
    std::string url, title;
    bool loading = false, back = false, fwd = false;
    bool fullscreen = false;         // the page asked for full screen (a video)
    bool playing = false;            // a video or sound is playing on it (as it last said: asked every second)
    // the zoom: Chromium's level (1.2 to its power); chosen with + / - (kept for this tab), else the one fitting the page's width
    double zoom = 0;
    bool manual = false;
    std::vector<double> fitChecks;   // when to measure the page again (seconds, on g_clock), soonest first
    int fitRounds = 0;               // corrections since the page loaded (a page that keeps growing is not chased for ever)
    std::string host;                // the page's site: a new one starts fitting again from 100 %
    double base = 0;                 // a site with a zoom of its own (shown as 100 %): ForeFlight and VATSIM Radar 65 %, ChartFox 70 %,
                                     // SimBrief 50 %, any other web app's 55 %; there the zoom is that, not fitted (0: none, the page is fitted)
    std::vector<uint8_t> view;       // the newest picture of the page
    int vw = 0, vh = 0;
    std::vector<uint8_t> popup;      // a drop-down list open on the page, drawn over it
    int pw = 0, ph = 0;
    cef_rect_t popupRect = {0, 0, 0, 0};
    bool popupShown = false;
};
std::vector<Tab> g_tabs;
int g_active = -1;                   // index into g_tabs
int g_nextId = 1;
bool g_dirty = false;                // the shown tab has a picture the plugin has not had
struct OpenReq { std::string url; int group; };
std::vector<OpenReq> g_openQueue;   // pages asked for in a new window: opened as tabs (in the opener's group) by the loop

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// the sites whose own zoom is kept (as a level: 1.2 to its power), shown as 100 %
// (the tablet's web apps: those sites at their own zooms (Discord 75 %); every other web app at 55 %)
// The Browser's own tabs (group 0): every site at 55 % of that (its own zoom x 0.55, any other site 55 %), not fitted.
double siteBase(const std::string& host, int group) {
    auto has = [&](const char* d) { return host.find(d) != std::string::npos; };
    double pct = 0;
    if (has("foreflight.com") || has("vatsim-radar.com")) pct = 0.65;
    else if (has("chartfox.org")) pct = 0.70;
    else if (has("simbrief.com")) pct = 0.50;
    else if (has("discord.com")) pct = 0.75;
    const bool web = host.compare(0, 5, "file:") != 0 && !host.empty();
    if (group == 0 && web) pct = (pct > 0 ? pct : 1.0) * 0.55;
    else if (pct == 0 && group != 0 && web) pct = 0.55;
    return pct > 0 ? std::log(pct) / std::log(1.2) : 0.0;
}
std::string hostPart(const std::string& u) {
    const size_t s = u.find("://");
    if (s == std::string::npos) return u;
    const size_t e = u.find_first_of("/?#", s + 3);
    return u.substr(0, e);
}

Tab* tabOf(cef_browser_t* b) {
    if (!b) return nullptr;
    const int bid = b->get_identifier(b);
    for (Tab& t : g_tabs) if (t.br && t.br->get_identifier(t.br) == bid) return &t;
    return nullptr;
}
bool shown(const Tab* t) { return t && g_active >= 0 && g_active < (int)g_tabs.size() && &g_tabs[g_active] == t; }

// The tab list and the shown tab's address, title and state, for the plugin
void publishInfo() {
    if (!g_shm) return;
    g_shm->tabCount = (int32_t)std::min<size_t>(g_tabs.size(), xptb::kMaxTabs);
    for (int i = 0; i < g_shm->tabCount; ++i) {
        const Tab& t = g_tabs[i];
        g_shm->tabs[i].id = t.id;
        g_shm->tabs[i].loading = t.loading;
        g_shm->tabs[i].group = t.group;
        copyTo(g_shm->tabs[i].title, sizeof g_shm->tabs[i].title, t.title);
        copyTo(g_shm->tabs[i].url, sizeof g_shm->tabs[i].url, t.url);
    }
    if (g_active >= 0 && g_active < (int)g_tabs.size()) {
        const Tab& t = g_tabs[g_active];
        g_shm->activeTab = t.id;
        copyTo(g_shm->url, sizeof g_shm->url, t.url);
        copyTo(g_shm->title, sizeof g_shm->title, t.title);
        g_shm->loading = t.loading; g_shm->canBack = t.back; g_shm->canForward = t.fwd;
        g_shm->fullscreen = t.fullscreen;
        g_shm->zoomPct = (int32_t)std::lround(100.0 * std::pow(1.2, t.zoom - t.base));   // (a site's own zoom shows as 100 %)
        g_shm->zoomAuto = t.manual ? 0 : 1;
    }
    g_shm->infoSeq.fetch_add(1);
}
template <class T> void unref(T* p);   // (below)
// the tablet's media volume (every tab: the Browser's and the web apps'), set on every video and sound on the page, and again on what
// comes later
int g_mediaVol = 100;
void applyVolume(Tab& t) {
    if (!t.br) return;
    cef_frame_t* f = t.br->get_main_frame(t.br);
    if (!f) return;
    char js[400];
    std::snprintf(js, sizeof js, "(function(v){window.__xptVol=v;document.querySelectorAll('video,audio').forEach(function(e){e.volume=v;});"
                                 "if(!window.__xptVolObs){window.__xptVolObs=1;document.addEventListener('play',function(e){if(e.target&&e.target.volume!==undefined)e.target.volume=window.__xptVol;},true);}})(%.2f)",
                  g_mediaVol / 100.0);
    cef_string_t s = {}, u = {};
    setStr(&s, js);
    f->execute_java_script(f, &s, &u, 0);
    cef.utf16_clear(&s);
    unref(f);
}
void applyZoom(Tab& t) {
    if (!t.br) return;
    cef_browser_host_t* h = t.br->get_host(t.br);
    if (h) { h->set_zoom_level(h, t.zoom); unref(h); }
}
// asks a page whether a video or sound is playing on it (the answer comes back as a console message, onConsole)
void askPlaying(Tab& t) {
    if (!t.br) return;
    cef_frame_t* f = t.br->get_main_frame(t.br);
    if (!f) return;
    cef_string_t js = {}, u = {};
    setStr(&js, "(function(){var p=0;document.querySelectorAll('video,audio').forEach(function(e){if(!e.paused&&!e.ended&&e.readyState>2&&!e.muted)p=1;});"
                "console.log('__xptplay '+p);})()");
    f->execute_java_script(f, &js, &u, 0);
    cef.utf16_clear(&js);
    unref(f);
}
// asks the page how wide it is against the view (the answer comes back as a console message, onConsole)
void askFit(Tab& t) {
    if (!t.br) return;
    cef_frame_t* f = t.br->get_main_frame(t.br);
    if (!f) return;
    cef_string_t js = {}, u = {};
    setStr(&js, "(function(){var d=document.documentElement,b=document.body;"
                "console.log('__xptfit '+Math.max(d?d.scrollWidth:0,b?b.scrollWidth:0)+' '+window.innerWidth);})()");
    f->execute_java_script(f, &js, &u, 0);
    cef.utf16_clear(&js);
    unref(f);
}
void scheduleFit(Tab& t, std::initializer_list<double> after) {
    const double now = nowS();
    for (double a : after) t.fitChecks.push_back(now + a);
    std::sort(t.fitChecks.begin(), t.fitChecks.end());
}

// Hands the shown tab's newest picture to the plugin, into the buffer it is not reading, once it has taken the previous one.
void publish() {
    if (!g_dirty || !g_shm || g_active < 0 || g_active >= (int)g_tabs.size()) return;
    const Tab& t = g_tabs[g_active];
    if (t.vw <= 0 || t.vh <= 0) return;
    const uint32_t seq = g_shm->frameSeq.load(std::memory_order_acquire);
    if (g_shm->ackSeq.load(std::memory_order_acquire) != seq) return;   // still reading the last one
    const int idx = (int)((seq + 1) & 1u);
    const int w = std::min(t.vw, xptb::kMaxW), h = std::min(t.vh, xptb::kMaxH);
    uint8_t* dst = g_shm->pixels[idx];
    for (int y = 0; y < h; ++y) std::memcpy(dst + (size_t)y * w * 4, t.view.data() + (size_t)y * t.vw * 4, (size_t)w * 4);
    if (t.popupShown && t.pw > 0 && t.ph > 0) {   // the drop-down, where the page put it (its rectangle is in view coordinates)
        const int ox = (int)std::lround(t.popupRect.x * g_scale), oy = (int)std::lround(t.popupRect.y * g_scale);
        for (int y = 0; y < t.ph; ++y) {
            const int ty = oy + y;
            if (ty < 0 || ty >= h) continue;
            const int x0 = std::max(0, -ox), x1 = std::min(t.pw, w - ox);
            if (x1 > x0) std::memcpy(dst + ((size_t)ty * w + ox + x0) * 4, t.popup.data() + ((size_t)y * t.pw + x0) * 4, (size_t)(x1 - x0) * 4);
        }
    }
    g_shm->frameW[idx] = w;
    g_shm->frameH[idx] = h;
    g_shm->frameSeq.store(seq + 1, std::memory_order_release);
    g_dirty = false;
}

// ---- CEF's handlers: one of each, alive as long as the program, so their reference counts do nothing -------------------------------
void CEF_CALLBACK addRef(cef_base_ref_counted_t*) {}
int CEF_CALLBACK release(cef_base_ref_counted_t*) { return 0; }
int CEF_CALLBACK hasOneRef(cef_base_ref_counted_t*) { return 1; }
int CEF_CALLBACK hasAtLeastOneRef(cef_base_ref_counted_t*) { return 1; }
template <class T> void initBase(T& t) {
    std::memset(&t, 0, sizeof t);
    t.base.size = sizeof t;   // CEF reads this to know which members the struct has
    t.base.add_ref = addRef;
    t.base.release = release;
    t.base.has_one_ref = hasOneRef;
    t.base.has_at_least_one_ref = hasAtLeastOneRef;
}
template <class T> void unref(T* p) { if (p) p->base.release(&p->base); }

cef_render_handler_t g_render;
cef_life_span_handler_t g_life;
cef_display_handler_t g_display;
cef_load_handler_t g_load;
cef_client_t g_client;
cef_app_t g_app;

void CEF_CALLBACK getViewRect(cef_render_handler_t*, cef_browser_t*, cef_rect_t* r) { r->x = 0; r->y = 0; r->width = g_viewW; r->height = g_viewH; }
int CEF_CALLBACK getScreenInfo(cef_render_handler_t*, cef_browser_t*, cef_screen_info_t* si) {
    si->device_scale_factor = g_scale;
    si->depth = 24; si->depth_per_component = 8; si->is_monochrome = 0;
    si->rect.x = 0; si->rect.y = 0; si->rect.width = g_viewW; si->rect.height = g_viewH;
    si->available_rect = si->rect;
    return 1;
}
void CEF_CALLBACK onPopupShow(cef_render_handler_t*, cef_browser_t* b, int show) {
    Tab* t = tabOf(b);
    if (!t) return;
    t->popupShown = show != 0;
    if (!show) t->pw = t->ph = 0;
    if (shown(t)) g_dirty = true;
}
void CEF_CALLBACK onPopupSize(cef_render_handler_t*, cef_browser_t* b, const cef_rect_t* r) { if (Tab* t = tabOf(b)) t->popupRect = *r; }
void CEF_CALLBACK onPaint(cef_render_handler_t*, cef_browser_t* b, cef_paint_element_type_t type, size_t, cef_rect_t const*, const void* buffer, int w, int h) {
    Tab* t = tabOf(b);
    if (!t || w <= 0 || h <= 0) return;
    const size_t n = (size_t)w * h * 4;
    if (type == PET_POPUP) { t->popup.assign((const uint8_t*)buffer, (const uint8_t*)buffer + n); t->pw = w; t->ph = h; }
    else { t->view.assign((const uint8_t*)buffer, (const uint8_t*)buffer + n); t->vw = w; t->vh = h; }
    if (shown(t)) g_dirty = true;
}
void CEF_CALLBACK onVirtualKeyboard(cef_render_handler_t*, cef_browser_t* b, cef_text_input_mode_t mode) {
    if (!g_shm || !shown(tabOf(b))) return;
    g_shm->wantsKeyboard = mode != CEF_TEXT_INPUT_MODE_NONE;
    g_shm->infoSeq.fetch_add(1);
}

// A page that opens a window (a link with target=_blank, a sign-in): opened as a tab of its own, shown.
int CEF_CALLBACK onBeforePopup(cef_life_span_handler_t*, cef_browser_t* browser, cef_frame_t*, const cef_string_t* url, const cef_string_t*,
                                cef_window_open_disposition_t, int, const cef_popup_features_t*, cef_window_info_t*, cef_client_t**,
                                cef_browser_settings_t*, cef_dictionary_value_t**, int*) {
    const std::string u = url ? toUtf8(url) : std::string();
    pageLog("pop-up asked for: %s", u.empty() ? "(no address)" : u.c_str());
    // a blank window a page fills in itself (about:blank) cannot be shown as a tab of its own: it would only be a white page
    if (!u.empty() && u.compare(0, 11, "about:blank") != 0) {
        const Tab* opener = tabOf(browser);
        if (g_tabs.size() < (size_t)xptb::kMaxTabs) g_openQueue.push_back({u, opener ? opener->group : 0});
        else {   // no room for another tab: the page itself goes there
            cef_frame_t* f = browser->get_main_frame(browser);
            if (f) { f->load_url(f, url); unref(f); }
        }
    }
    return 1;   // no window of its own
}
void CEF_CALLBACK onAddressChange(cef_display_handler_t*, cef_browser_t* b, cef_frame_t* frame, const cef_string_t* url) {
    Tab* t = tabOf(b);
    if (!t || !frame || !frame->is_main(frame)) return;
    t->url = toUtf8(url);
    const std::string host = hostPart(t->url);
    if (host != t->host) {   // another site: its own fit, from 100 % (or the site's own zoom); a zoom chosen for the tab stays
        t->host = host;
        const double was = t->base;
        t->base = siteBase(host, t->group);
        if (t->manual) t->zoom += t->base - was;   // (chosen: kept against the site's own)
        else if (t->zoom != t->base) { t->zoom = t->base; t->fitChecks.clear(); applyZoom(*t); }
        t->fitRounds = 0;
    }
    publishInfo();
}
void CEF_CALLBACK onTitleChange(cef_display_handler_t*, cef_browser_t* b, const cef_string_t* title) {
    Tab* t = tabOf(b);
    if (!t) return;
    t->title = toUtf8(title);
    publishInfo();
}
int CEF_CALLBACK onConsole(cef_display_handler_t*, cef_browser_t* b, cef_log_severity_t level, const cef_string_t* message, const cef_string_t* source, int line) {
    if (message) {   // the page's width (askFit): wider than the view, it is zoomed out until it fits (never in past 100 %, never under 30 %)
        const std::string m = toUtf8(message);
        if (m.compare(0, 10, "__xptplay ") == 0) {
            if (Tab* t = tabOf(b)) t->playing = m.size() > 10 && m[10] == '1';
            return 1;
        }
        if (m.compare(0, 9, "__xptfit ") == 0) {
            Tab* t = tabOf(b);
            double sw = 0, iw = 0;
            if (t && !t->manual && t->base == 0 && std::sscanf(m.c_str() + 9, "%lf %lf", &sw, &iw) == 2 && iw > 50 && sw > iw + 4 && t->fitRounds < 8) {
                const double factor = std::max(0.30, std::pow(1.2, t->zoom) * iw / sw);
                t->zoom = std::log(factor) / std::log(1.2);
                ++t->fitRounds;
                applyZoom(*t);
                scheduleFit(*t, {0.4});   // (narrower, a page can lay itself out again)
                if (shown(t)) publishInfo();
            }
            return 1;   // (not logged)
        }
    }
    if (level >= LOGSEVERITY_WARNING) {
        const Tab* t = tabOf(b);
        pageLog("[%s] %s (%s:%d) on %s", level >= LOGSEVERITY_ERROR ? "error" : "warning", message ? toUtf8(message).c_str() : "",
                source ? toUtf8(source).c_str() : "", line, t ? t->url.c_str() : "?");
    }
    return 0;   // (Chromium logs it as usual too)
}
void CEF_CALLBACK onLoadError(cef_load_handler_t*, cef_browser_t*, cef_frame_t* frame, cef_errorcode_t code, const cef_string_t* text, const cef_string_t* url) {
    if (code == ERR_ABORTED) return;   // (a load replaced by another: normal)
    pageLog("load failed (%d %s, %s): %s", (int)code, text ? toUtf8(text).c_str() : "", frame && frame->is_main(frame) ? "the page" : "a frame in it",
            url ? toUtf8(url).c_str() : "");
}
void CEF_CALLBACK onFullscreen(cef_display_handler_t*, cef_browser_t* b, int fullscreen) {   // a video's full-screen button
    Tab* t = tabOf(b);
    if (!t) return;
    t->fullscreen = fullscreen != 0;
    publishInfo();
}
void CEF_CALLBACK onLoadingState(cef_load_handler_t*, cef_browser_t* b, int loading, int back, int fwd) {
    Tab* t = tabOf(b);
    if (!t) return;
    const bool was = t->loading;
    t->loading = loading != 0; t->back = back != 0; t->fwd = fwd != 0;
    if (was && !t->loading && !t->manual && t->base == 0) { t->fitRounds = 0; scheduleFit(*t, {0.15, 1.0, 2.5, 5.0}); }
    if (was && !t->loading && !t->manual && t->base != 0) applyZoom(*t);
    if (was && !t->loading && g_mediaVol != 100) applyVolume(*t);   // (a new page: its media at the Browser's volume)   // (the site's own zoom, again: Chromium keeps one per site)   // loaded: fitted (and again as it settles)
    publishInfo();
}
// A page's process that stopped (a crash, or out of memory on a heavy map) leaves a white page: it is loaded again
cef_request_handler_t g_request;
void CEF_CALLBACK onRenderGone(cef_request_handler_t*, cef_browser_t* b, cef_termination_status_t status) {
    const Tab* t = tabOf(b);
    pageLog("a page's process stopped (status %d) on %s: reloading it", (int)status, t ? t->url.c_str() : "?");
    if (b) b->reload(b);
}
cef_request_handler_t* CEF_CALLBACK getRequest(cef_client_t*) { return &g_request; }
cef_render_handler_t* CEF_CALLBACK getRender(cef_client_t*) { return &g_render; }
cef_life_span_handler_t* CEF_CALLBACK getLife(cef_client_t*) { return &g_life; }
cef_display_handler_t* CEF_CALLBACK getDisplay(cef_client_t*) { return &g_display; }
cef_load_handler_t* CEF_CALLBACK getLoad(cef_client_t*) { return &g_load; }

// Chromium's switches for every process: pages drawn in software (a picture for the plugin, no window of its own; extensions stay
// on: Chromium's PDF viewer is one, for the Documents page), and no
// first-run or background chatter.
void CEF_CALLBACK onBeforeCommandLine(cef_app_t*, const cef_string_t* processType, cef_command_line_t* cl) {
    if (processType && processType->length) return;   // set once, for the browser process; the others inherit them
    for (const char* sw : {"disable-gpu", "disable-gpu-compositing", "no-first-run", "disable-background-networking", "touch-events"}) {   // (touch-events: pages see the tablet's fingers as touches)
        cef_string_t s = {};
        setStr(&s, sw);
        cl->append_switch(cl, &s);
        cef.utf16_clear(&s);
    }
    // no WebGPU for the pages: without the GPU it cannot start ("Failed to query ID3D11Device from ANGLE"), and a site that picks
    // it when offered (Volanta's map) then draws nothing; without it, they take WebGL, which works in software
    cef_string_t k = {}, v = {};
    // and no AutomationControlled: with it, navigator.webdriver is true and Google's sign-in refuses the browser as "not secure"
    setStr(&k, "disable-blink-features"); setStr(&v, "WebGPU,AutomationControlled");
    cl->append_switch_with_value(cl, &k, &v);
    cef.utf16_clear(&k); cef.utf16_clear(&v);
}

void initHandlers() {
    initBase(g_render);
    g_render.get_view_rect = getViewRect;
    g_render.get_screen_info = getScreenInfo;
    g_render.on_popup_show = onPopupShow;
    g_render.on_popup_size = onPopupSize;
    g_render.on_paint = onPaint;
    g_render.on_virtual_keyboard_requested = onVirtualKeyboard;
    initBase(g_life);
    g_life.on_before_popup = onBeforePopup;
    initBase(g_display);
    g_display.on_address_change = onAddressChange;
    g_display.on_title_change = onTitleChange;
    g_display.on_fullscreen_mode_change = onFullscreen;
    g_display.on_console_message = onConsole;
    initBase(g_load);
    g_load.on_loading_state_change = onLoadingState;
    g_load.on_load_error = onLoadError;
    initBase(g_request);
    g_request.on_render_process_terminated = onRenderGone;
    initBase(g_client);
    g_client.get_request_handler = getRequest;
    g_client.get_render_handler = getRender;
    g_client.get_life_span_handler = getLife;
    g_client.get_display_handler = getDisplay;
    g_client.get_load_handler = getLoad;
    initBase(g_app);
    g_app.on_before_command_line_processing = onBeforeCommandLine;
}

// ---- opening, showing and closing tabs ---------------------------------------------------------------------------------------------
void hostDo(cef_browser_t* b, void (*fn)(cef_browser_host_t*)) {
    if (!b) return;
    cef_browser_host_t* h = b->get_host(b);
    if (h) { fn(h); unref(h); }
}
void showTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    if (g_active >= 0 && g_active < (int)g_tabs.size() && g_active != index) hostDo(g_tabs[g_active].br, [](cef_browser_host_t* h) { h->was_hidden(h, 1); });
    g_active = index;
    applyZoom(g_tabs[index]);   // (Chromium keeps a zoom per site: another tab of the same site may have changed it)
    hostDo(g_tabs[index].br, [](cef_browser_host_t* h) { h->was_hidden(h, 0); h->was_resized(h); h->invalidate(h, PET_VIEW); h->set_focus(h, 1); });
    g_dirty = true;
    if (g_shm) g_shm->wantsKeyboard = 0;
    publishInfo();
}
bool openTab(const std::string& url, int group) {
    if (g_tabs.size() >= (size_t)xptb::kMaxTabs) return false;
    cef_window_info_t wi;
    std::memset(&wi, 0, sizeof wi);
    wi.windowless_rendering_enabled = 1;
    cef_browser_settings_t bs;
    std::memset(&bs, 0, sizeof bs);
    bs.size = sizeof bs;
    bs.windowless_frame_rate = 30;
    bs.background_color = 0xFFFFFFFF;
    cef_string_t u = {};
    setStr(&u, url.empty() ? std::string(xptb::kStartPage) : url);
    cef_browser_t* b = cef.create_browser_sync(&wi, &g_client, &u, &bs, nullptr, nullptr);
    cef.utf16_clear(&u);
    if (!b) return false;
    Tab t;
    t.id = g_nextId++;
    t.br = b;
    t.url = url;
    t.group = group;
    g_tabs.push_back(std::move(t));
    showTab((int)g_tabs.size() - 1);
    return true;
}
void closeTab(int id) {
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i].id != id) continue;
        cef_browser_t* b = g_tabs[i].br;
        hostDo(b, [](cef_browser_host_t* h) { h->close_browser(h, 1); });
        unref(b);
        const int group = g_tabs[i].group;
        const bool wasShown = (int)i == g_active;
        g_tabs.erase(g_tabs.begin() + (long)i);
        bool groupLeft = false;
        for (const Tab& t : g_tabs) if (t.group == group) groupLeft = true;
        // (the Browser's last tab closed: none is left; the tablet shows its New Tab page)
        (void)groupLeft;
        if (g_tabs.empty()) { g_active = -1; publishInfo(); return; }
        int next = g_active;
        if ((int)i < g_active) --next;
        else if (wasShown) {   // the shown one closed: its neighbour in the same group, else whatever is next
            next = -1;
            for (int k = std::min((int)i, (int)g_tabs.size() - 1); k >= 0 && next < 0; --k) if (g_tabs[k].group == group) next = k;
            for (int k = (int)i; k < (int)g_tabs.size() && next < 0; ++k) if (g_tabs[k].group == group) next = k;
            if (next < 0) next = std::min((int)i, (int)g_tabs.size() - 1);
        }
        g_active = -1;
        showTab(next);
        return;
    }
}

// ---- the plugin's commands ---------------------------------------------------------------------------------------------------------
cef_browser_t* activeBrowser() { return g_active >= 0 && g_active < (int)g_tabs.size() ? g_tabs[g_active].br : nullptr; }
void withHost(void (*fn)(cef_browser_host_t*, const xptb::Cmd&), const xptb::Cmd& c) {
    cef_browser_t* b = activeBrowser();
    if (!b) return;
    cef_browser_host_t* h = b->get_host(b);
    if (h) { fn(h, c); unref(h); }
}
cef_mouse_event_t mouseAt(const xptb::Cmd& c) { cef_mouse_event_t m; m.x = c.a; m.y = c.b; m.modifiers = 0; return m; }
bool g_buttonDown = false;

void runCommand(const xptb::Cmd& c) {
    cef_browser_t* br = activeBrowser();
    switch (c.type) {
    case xptb::CMD_NAVIGATE:
        if (br) {
            cef_frame_t* f = br->get_main_frame(br);
            if (f) { cef_string_t u = {}; setStr(&u, c.text); f->load_url(f, &u); cef.utf16_clear(&u); unref(f); }
        }
        break;
    case xptb::CMD_BACK: if (br) br->go_back(br); break;
    case xptb::CMD_FORWARD: if (br) br->go_forward(br); break;
    case xptb::CMD_RELOAD: if (br) br->reload(br); break;
    case xptb::CMD_STOP: if (br) br->stop_load(br); break;
    case xptb::CMD_TAB_NEW: openTab(c.text, c.b); break;
    case xptb::CMD_EDIT:   // copy, cut, paste, select all: in the frame with the focus (a field in a frame of the page), else the page
        if (br) {
            cef_frame_t* f = br->get_focused_frame(br);
            if (!f) f = br->get_main_frame(br);
            if (f) {
                if (c.a == 1) f->copy(f); else if (c.a == 2) f->cut(f); else if (c.a == 3) f->paste(f); else if (c.a == 4) f->select_all(f);
                unref(f);
            }
        }
        break;
    case xptb::CMD_MEDIA_VOLUME:
        g_mediaVol = std::max(0, std::min(100, c.a));
        for (Tab& t : g_tabs) applyVolume(t);
        break;
    case xptb::CMD_EXIT_FULLSCREEN:   // (this Chromium has no call for it: the page is asked, as its own Esc would)
        if (br) {
            cef_frame_t* f = br->get_main_frame(br);
            if (f) {
                cef_string_t js = {}, u = {};
                setStr(&js, "if (document.fullscreenElement) document.exitFullscreen(); else if (document.webkitFullscreenElement) document.webkitExitFullscreen();");
                f->execute_java_script(f, &js, &u, 0);
                cef.utf16_clear(&js);
                unref(f);
            }
            // and Esc, as the page's own way out (a player that keeps full screen to itself)
            withHost([](cef_browser_host_t* h, const xptb::Cmd&) {
                cef_key_event_t k;
                std::memset(&k, 0, sizeof k);
                k.windows_key_code = 27;
                k.type = KEYEVENT_RAWKEYDOWN; h->send_key_event(h, &k);
                k.type = KEYEVENT_KEYUP; h->send_key_event(h, &k);
            }, c);
        }
        break;
    case xptb::CMD_ZOOM:   // Chromium's zoom levels: 1.2 to the power of the level (0.5 a step: about 10 %)
        if (g_active >= 0 && g_active < (int)g_tabs.size()) {
            Tab& t = g_tabs[g_active];
            if (c.a == 0) { t.manual = false; t.zoom = t.base; t.fitRounds = 0; if (t.base == 0) scheduleFit(t, {0.15, 1.0}); }   // back to fitting (or the site's own)
            else if (c.a == 3) { t.manual = true; t.fitChecks.clear(); t.zoom = t.base + std::log(std::max(25, std::min(300, c.b)) / 100.0) / std::log(1.2); }   // a percentage typed
            else { t.manual = true; t.fitChecks.clear(); t.zoom = std::max(-6.0, std::min(4.0, t.zoom + 0.5 * c.a)); }
            applyZoom(t);
            publishInfo();
        }
        break;
    case xptb::CMD_TAB_CLOSE: closeTab(c.a); break;
    case xptb::CMD_TAB_SELECT:
        for (size_t i = 0; i < g_tabs.size(); ++i) if (g_tabs[i].id == c.a) showTab((int)i);
        break;
    case xptb::CMD_RESIZE: {
        const int w = std::max(50, std::min(c.a, xptb::kMaxW)), h = std::max(50, std::min(c.b, xptb::kMaxH));
        const float s = std::max(0.5f, std::min(c.f, 4.0f));
        const bool scaleChanged = std::fabs(s - g_scale) > 0.01f;
        if (w == g_viewW && h == g_viewH && !scaleChanged) break;
        g_viewW = w; g_viewH = h; g_scale = s;
        // the picture must fit the shared buffer: a smaller density rather than a cut-off page
        while (g_viewW * g_scale > xptb::kMaxW || g_viewH * g_scale > xptb::kMaxH) g_scale *= 0.9f;
        for (Tab& t : g_tabs) {   // (a page fitted to the old size fits again: wider, it may need less zooming out)
            hostDo(t.br, [](cef_browser_host_t* hh) { hh->notify_screen_info_changed(hh); hh->was_resized(hh); });
            if (!t.manual && t.base == 0) { if (t.zoom != 0) { t.zoom = 0; applyZoom(t); } t.fitRounds = 0; scheduleFit(t, {0.3, 1.2}); }
        }
        publishInfo();
        break;
    }
    case xptb::CMD_MOUSE_MOVE: withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); if (g_buttonDown) m.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON; h->send_mouse_move_event(h, &m, 0); }, c); break;
    case xptb::CMD_MOUSE_LEAVE: withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); h->send_mouse_move_event(h, &m, 1); }, c); break;
    case xptb::CMD_MOUSE_DOWN:
        g_buttonDown = true;
        withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); h->set_focus(h, 1); h->send_mouse_click_event(h, &m, MBT_LEFT, 0, 1); }, c);
        break;
    case xptb::CMD_MOUSE_UP:
        g_buttonDown = false;
        withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); h->send_mouse_click_event(h, &m, MBT_LEFT, 1, 1); }, c);
        break;
    case xptb::CMD_TOUCH:   // a finger on the page: Chromium's own touch handling (scrolling with momentum, panning maps, taps as clicks)
        withHost([](cef_browser_host_t* h, const xptb::Cmd& c) {
            cef_touch_event_t t;
            std::memset(&t, 0, sizeof t);
            t.id = 1; t.x = (float)c.a; t.y = (float)c.b; t.radius_x = t.radius_y = 6.0f; t.pressure = c.c == 2 || c.c == 3 ? 0.0f : 1.0f;
            t.type = c.c == 0 ? CEF_TET_PRESSED : c.c == 1 ? CEF_TET_MOVED : c.c == 2 ? CEF_TET_RELEASED : CEF_TET_CANCELLED;
            t.pointer_type = CEF_POINTER_TYPE_TOUCH;
            if (c.c == 0) h->set_focus(h, 1);
            h->send_touch_event(h, &t);
        }, c);
        break;
    case xptb::CMD_WHEEL: withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); if (c.f > 0.5f) m.modifiers |= EVENTFLAG_CONTROL_DOWN; h->send_mouse_wheel_event(h, &m, c.d, c.c); }, c); break;
    case xptb::CMD_CHAR:
        withHost([](cef_browser_host_t* h, const xptb::Cmd& c) {
            cef_key_event_t k;
            std::memset(&k, 0, sizeof k);
            k.type = KEYEVENT_CHAR;
            k.windows_key_code = c.c;
            k.character = k.unmodified_character = (char16_t)c.c;
            h->send_key_event(h, &k);
        }, c);
        break;
    case xptb::CMD_KEY:
        withHost([](cef_browser_host_t* h, const xptb::Cmd& c) {
            cef_key_event_t k;
            std::memset(&k, 0, sizeof k);
            k.type = c.d ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            k.windows_key_code = c.c;
            h->send_key_event(h, &k);
            if (c.d && c.c == 13) {   // Enter also types a carriage return (it submits a form)
                k.type = KEYEVENT_CHAR; k.character = k.unmodified_character = 13;
                h->send_key_event(h, &k);
            }
        }, c);
        break;
    case xptb::CMD_FOCUS: withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { h->set_focus(h, c.a); }, c); break;
    default: break;
    }
}

std::string argValue(int argc, char** argv, const char* key) {
    const size_t n = std::strlen(key);
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], key, n) == 0 && argv[i][n] == '=') return argv[i] + n + 1;
    return std::string();
}

void setEnv(const char* k, const std::string& v) {
#if defined(_WIN32)
    SetEnvironmentVariableA(k, v.c_str());
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
    // where X-Plane's CEF is: from the command line the first time, then passed to Chromium's own processes in the environment
    std::string dir = argValue(argc, argv, "--xptb-cef-dir");
    if (dir.empty() && std::getenv("XPTB_CEF_DIR")) dir = std::getenv("XPTB_CEF_DIR");
    if (dir.empty()) { std::fprintf(stderr, "xptbrowser is started by XPTablet\n"); return 2; }
    g_cefDir = dir;
    setEnv("XPTB_CEF_DIR", dir);
    const std::string parent = argValue(argc, argv, "--xptb-parent");
    if (!parent.empty()) setEnv("XPTB_PARENT", parent);

    std::string err;
    if (!loadCef(err)) {
        if (!parent.empty() && openShared(err)) fail(err); else std::fprintf(stderr, "xptbrowser: %s\n", err.c_str());
        return 3;
    }
    initHandlers();
    cef_main_args_t args;
    std::memset(&args, 0, sizeof args);
#if defined(_WIN32)
    args.instance = GetModuleHandleW(nullptr);
#else
    args.argc = argc; args.argv = argv;
#endif
    // Chromium's other processes are this program too: they stop here
    const int code = cef.execute_process(&args, &g_app, nullptr);
    if (code >= 0) return code;

    // ---- the browser process
    if (!openShared(err)) { std::fprintf(stderr, "xptbrowser: %s\n", err.c_str()); return 4; }
    if (cef.api_hash) {   // X-Plane's CEF must be the build these headers are from
        const char* h = cef.api_hash(0);
        if (!h || std::strcmp(h, CEF_API_HASH_PLATFORM) != 0) {
            fail(std::string("X-Plane's CEF is another version (API ") + (h ? h : "?") + ", XPTablet expects 117.1.4)");
            return 5;
        }
    }
    const std::string profile = argValue(argc, argv, "--xptb-profile");
    char self[4096] = "";
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, self, sizeof self);
    const char sep = '\\';
#else
    const ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sl > 0) self[sl] = 0;
    const char sep = '/';
#endif
    cef_settings_t st;
    std::memset(&st, 0, sizeof st);
    st.size = sizeof st;
    st.no_sandbox = 1;
    st.windowless_rendering_enabled = 1;
    st.persist_session_cookies = 1;      // stay signed in
    st.persist_user_preferences = 1;
    st.log_severity = LOGSEVERITY_WARNING;
    st.background_color = 0xFFFFFFFF;
    setStr(&st.browser_subprocess_path, self);
    setStr(&st.resources_dir_path, dir);
    setStr(&st.locales_dir_path, dir + sep + "locales");
    if (!profile.empty()) g_pageLog = std::fopen((profile + sep + "pages.log").c_str(), "w");
    if (!profile.empty()) { setStr(&st.root_cache_path, profile); setStr(&st.cache_path, profile + sep + "default"); setStr(&st.log_file, profile + sep + "xptbrowser.log"); }
    setStr(&st.locale, "en-US");
    // (the user agent stays Chromium's own: one claiming another version made sites send code this Chromium cannot run, and
    // refuse it as an unsupported browser)
    if (!cef.initialize(&args, &st, &g_app, nullptr)) { fail("Chromium did not start (see xptbrowser.log in the browser profile)"); return 6; }

    if (!openTab("", 0)) { fail("Chromium could not open a page"); cef.shutdown(); return 7; }
    g_shm->state.store(1);

    // the loop: the plugin's commands, Chromium's work, the newest picture; until told to stop or the plugin is gone
    const long parentPid = parent.empty() ? 0 : std::atol(parent.c_str());
    uint32_t lastBeat = g_shm->heartbeat.load();
    auto lastBeatAt = std::chrono::steady_clock::now();
    for (bool quit = false; !quit;) {
        for (uint32_t r = g_shm->cmdRead.load(); r != g_shm->cmdWrite.load(std::memory_order_acquire); ++r) {
            const xptb::Cmd& c = g_shm->cmds[r % xptb::kCmds];
            if (c.type == xptb::CMD_QUIT) quit = true; else runCommand(c);
            g_shm->cmdRead.store(r + 1, std::memory_order_release);
        }
        cef.do_message_loop_work();
        if (!g_openQueue.empty()) {   // pages a page asked to open in a new window
            const std::vector<OpenReq> q = std::move(g_openQueue);
            g_openQueue.clear();
            for (const OpenReq& r : q) openTab(r.url, r.group);
        }
        {   // each page asked, every second, whether it is playing a video or sound; the tablet told whether any is
            static double askedAt = 0;
            const double now = nowS();
            if (now - askedAt > 1.0) {
                askedAt = now;
                bool any = false;
                for (Tab& t : g_tabs) { askPlaying(t); any = any || t.playing; }
                g_shm->mediaPlaying = any ? 1 : 0;
            }
        }
        {   // the pages due to be measured for their fit
            const double now = nowS();
            for (Tab& t : g_tabs)
                if (!t.manual && !t.fitChecks.empty() && now >= t.fitChecks.front()) { t.fitChecks.erase(t.fitChecks.begin()); askFit(t); }
        }
        publish();
        const uint32_t beat = g_shm->heartbeat.load();
        const auto now = std::chrono::steady_clock::now();
        if (beat != lastBeat) { lastBeat = beat; lastBeatAt = now; }
        // the plugin gone, or silent for long (X-Plane paused draws nothing: so only a long silence, with its process gone, ends this)
        if (parentPid && !parentAlive(parentPid)) quit = true;
        if (std::chrono::duration<double>(now - lastBeatAt).count() > 600.0) quit = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (Tab& t : g_tabs) { hostDo(t.br, [](cef_browser_host_t* h) { h->close_browser(h, 1); }); }
    for (int i = 0; i < 50; ++i) { cef.do_message_loop_work(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    for (Tab& t : g_tabs) unref(t.br);
    g_tabs.clear();
    cef.shutdown();
    return 0;
}
