// The tablet's web browser runs as a program of its own (xptbrowser, src/browser_helper.cpp) using X-Plane's own Chromium
// Embedded Framework, outside X-Plane's process, so it can never collide with X-Plane's copy of it. It talks to the plugin
// through one block of shared memory with this layout: commands from the plugin in a ring, and the page's pictures from the
// helper in two buffers (it fills the one the plugin is not reading, then says so).
#pragma once

#include <atomic>
#include <cstdint>

namespace xptb {
constexpr const char* kStartPage = "https://duckduckgo.com/";   // a new tab, and the one left when the last is closed

constexpr uint32_t kMagic = 0x58505442;   // "XPTB"
constexpr uint32_t kVersion = 3;         // 2: tabs; 3: tab groups (the Browser's, and each web app's), zoom
constexpr int kMaxW = 2048, kMaxH = 1536;  // the largest page picture, pixels
constexpr int kCmds = 64;                  // commands the ring holds
constexpr int kTextLen = 2048;             // a URL, or a few typed characters
constexpr int kMaxTabs = 16;               // pages open at once, all groups together

enum CmdType : int32_t {
    CMD_NONE, CMD_NAVIGATE, CMD_BACK, CMD_FORWARD, CMD_RELOAD, CMD_STOP,
    CMD_RESIZE,       // a: width, b: height (the view, in device-independent pixels); f: screen pixels per one of them
    CMD_MOUSE_MOVE,   // a, b: where, in the view
    CMD_MOUSE_DOWN, CMD_MOUSE_UP, CMD_MOUSE_LEAVE,
    CMD_WHEEL,        // a, b: where; c: how far down (negative) or up, in pixels
    CMD_CHAR,         // c: the character (UTF-16)
    CMD_KEY,          // c: Windows key code (VK_BACK 8, VK_RETURN 13, VK_TAB 9, arrows 37-40, VK_DELETE 46); d: 1 down, 0 up
    CMD_FOCUS,        // a: 1 the page has the keyboard, 0 not
    CMD_QUIT,
    CMD_TAB_NEW,      // text: the address (empty: the search page, kStartPage); b: its group (0 the Browser, else a web app's); shown
    CMD_TAB_CLOSE,    // a: the tab's id
    CMD_TAB_SELECT,   // a: the tab's id
    CMD_ZOOM,         // a: +1 closer, -1 farther, 0 back to 100 % (the shown tab)
    CMD_EXIT_FULLSCREEN   // the shown page leaves full screen (a video's)
};
// Navigation, pointer and keys always go to the tab shown.

struct Cmd {
    int32_t type, a, b, c, d;
    float f;
    char text[kTextLen];
};

struct Shared {
    uint32_t magic, version;
    // ---- plugin -> helper
    std::atomic<uint32_t> cmdWrite;    // the plugin's next slot
    std::atomic<uint32_t> cmdRead;     // the helper's next slot
    Cmd cmds[kCmds];
    std::atomic<uint32_t> heartbeat;   // counted up by the plugin every frame
    std::atomic<uint32_t> ackSeq;      // the newest picture the plugin has taken
    // ---- helper -> plugin
    std::atomic<int32_t> state;        // 0 starting, 1 running, -1 failed (error says why)
    char error[512];
    std::atomic<uint32_t> frameSeq;    // pictures published; the newest is in buffer frameSeq & 1
    int32_t frameW[2], frameH[2];
    std::atomic<uint32_t> infoSeq;     // the page's address, title and state below changed
    char url[kTextLen];
    char title[256];
    int32_t loading, canBack, canForward;
    int32_t wantsKeyboard;             // a text field on the page has the focus
    // the tabs, in order (with infoSeq): url, title and the rest above are the shown tab's
    int32_t tabCount, activeTab;       // activeTab: the shown tab's id
    struct Tab { int32_t id, loading, group; char title[96]; char url[256]; } tabs[kMaxTabs];   // group: as CMD_TAB_NEW's
    int32_t zoomPct;                   // the shown tab's zoom, percent
    int32_t fullscreen;                // the shown tab is full screen (a video): the tablet gives it the whole screen
    uint8_t pixels[2][kMaxW * kMaxH * 4];   // BGRA, rows top to bottom
};

inline const char* shmName() { return "XPTabletBrowser"; }

} // namespace xptb
