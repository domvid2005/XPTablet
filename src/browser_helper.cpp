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

// ---- the tabs and the shown tab's picture ---------------------------------------------------------------------------------------
int g_viewW = 800, g_viewH = 400;    // device-independent pixels (every tab is this size)
float g_scale = 1.0f;                // screen pixels per one of them

struct Tab {
    int id = 0;                      // ours, given to the plugin
    cef_browser_t* br = nullptr;
    std::string url, title;
    bool loading = false, back = false, fwd = false;
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
std::vector<std::string> g_openQueue;   // pages asked for in a new window: opened as tabs by the loop

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
        copyTo(g_shm->tabs[i].title, sizeof g_shm->tabs[i].title, t.title);
        copyTo(g_shm->tabs[i].url, sizeof g_shm->tabs[i].url, t.url);
    }
    if (g_active >= 0 && g_active < (int)g_tabs.size()) {
        const Tab& t = g_tabs[g_active];
        g_shm->activeTab = t.id;
        copyTo(g_shm->url, sizeof g_shm->url, t.url);
        copyTo(g_shm->title, sizeof g_shm->title, t.title);
        g_shm->loading = t.loading; g_shm->canBack = t.back; g_shm->canForward = t.fwd;
    }
    g_shm->infoSeq.fetch_add(1);
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
    if (url && url->length) {
        if (g_tabs.size() < (size_t)xptb::kMaxTabs) g_openQueue.push_back(toUtf8(url));
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
    publishInfo();
}
void CEF_CALLBACK onTitleChange(cef_display_handler_t*, cef_browser_t* b, const cef_string_t* title) {
    Tab* t = tabOf(b);
    if (!t) return;
    t->title = toUtf8(title);
    publishInfo();
}
void CEF_CALLBACK onLoadingState(cef_load_handler_t*, cef_browser_t* b, int loading, int back, int fwd) {
    Tab* t = tabOf(b);
    if (!t) return;
    t->loading = loading != 0; t->back = back != 0; t->fwd = fwd != 0;
    publishInfo();
}
cef_render_handler_t* CEF_CALLBACK getRender(cef_client_t*) { return &g_render; }
cef_life_span_handler_t* CEF_CALLBACK getLife(cef_client_t*) { return &g_life; }
cef_display_handler_t* CEF_CALLBACK getDisplay(cef_client_t*) { return &g_display; }
cef_load_handler_t* CEF_CALLBACK getLoad(cef_client_t*) { return &g_load; }

// Chromium's switches for every process: pages drawn in software (a picture for the plugin, no window of its own; extensions stay
// on: Chromium's PDF viewer is one, for the Documents page), and no
// first-run or background chatter.
void CEF_CALLBACK onBeforeCommandLine(cef_app_t*, const cef_string_t* processType, cef_command_line_t* cl) {
    if (processType && processType->length) return;   // set once, for the browser process; the others inherit them
    for (const char* sw : {"disable-gpu", "disable-gpu-compositing", "no-first-run", "disable-background-networking"}) {
        cef_string_t s = {};
        setStr(&s, sw);
        cl->append_switch(cl, &s);
        cef.utf16_clear(&s);
    }
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
    initBase(g_load);
    g_load.on_loading_state_change = onLoadingState;
    initBase(g_client);
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
    hostDo(g_tabs[index].br, [](cef_browser_host_t* h) { h->was_hidden(h, 0); h->was_resized(h); h->invalidate(h, PET_VIEW); h->set_focus(h, 1); });
    g_dirty = true;
    if (g_shm) g_shm->wantsKeyboard = 0;
    publishInfo();
}
bool openTab(const std::string& url) {
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
        g_tabs.erase(g_tabs.begin() + (long)i);
        if (g_tabs.empty()) { g_active = -1; openTab(""); return; }   // never no tabs: the last one closed comes back as a fresh search page
        int next = g_active;
        if ((int)i < g_active) --next;
        else if ((int)i == g_active) next = std::min((int)i, (int)g_tabs.size() - 1);
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
    case xptb::CMD_TAB_NEW: openTab(c.text); break;
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
        for (Tab& t : g_tabs) hostDo(t.br, [](cef_browser_host_t* hh) { hh->notify_screen_info_changed(hh); hh->was_resized(hh); });
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
    case xptb::CMD_WHEEL: withHost([](cef_browser_host_t* h, const xptb::Cmd& c) { cef_mouse_event_t m = mouseAt(c); h->send_mouse_wheel_event(h, &m, 0, c.c); }, c); break;
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
    if (!profile.empty()) { setStr(&st.root_cache_path, profile); setStr(&st.cache_path, profile + sep + "default"); setStr(&st.log_file, profile + sep + "xptbrowser.log"); }
    setStr(&st.locale, "en-US");
    if (!cef.initialize(&args, &st, &g_app, nullptr)) { fail("Chromium did not start (see xptbrowser.log in the browser profile)"); return 6; }

    if (!openTab("")) { fail("Chromium could not open a page"); cef.shutdown(); return 7; }
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
            const std::vector<std::string> q = std::move(g_openQueue);
            g_openQueue.clear();
            for (const std::string& u : q) openTab(u);
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
