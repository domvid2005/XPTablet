// Scripts: a list of steps, one per line, for testing without a person (the headless emulator runs nothing else; the window can run one
// with --script too). Positions on the tablet are in its own 800 x 480 layout units, wherever the window or panel puts it.
//
//   wait 1.5                   let 1.5 seconds pass (the plugin's downloads and animations run meanwhile)
//   tap 400 300                tap the tablet at a layout point
//   drag 100 200 300 200 0.4   press, move, release over 0.4 s
//   click 640 400              press and release at a screen point (boxels, y up)
//   wheel 400 300 -2           the wheel over a tablet point
//   type KORD                  type text (to the window holding the keyboard)
//   key return|back|escape|left|right|up|down|tab|delete
//   menu Show                  pick the Plugins-menu item whose path has this text
//   command XPTablet/app_map   run a command
//   set sim/cockpit2/switches/beacon_on 1
//   sim lat|lon|hdg|alt|agl|gs|vs|ground|moving|profile|traffic|range|cockpit|wxplane|opensam|zulu|sun VALUE
//   intruder                   a TCAS RA intruder ahead
//   view window|cockpit
//   window L T R B             put the tablet window there (boxels)
//   reload                     disable, stop, start and enable the plugin
//   shot NAME                  a picture of the screen: NAME.png in the output folder
//   # comments
#include "emu.h"

#include "XPLMDefs.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace emu {
namespace {
std::vector<std::string> g_lines;
size_t g_next = 0;
double g_waitUntil = -1;
struct Pending { double at; std::function<void()> f; };
std::vector<Pending> g_pending;
int g_failures = 0;
} // namespace

bool scriptLoad(const std::string& file) {
    std::ifstream in(file);
    if (!in) { log("[emulator] script %s not found", file.c_str()); return false; }
    std::string l;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') g_lines.push_back(l);
    }
    log("[emulator] script %s: %d steps", file.c_str(), (int)g_lines.size());
    return true;
}
int scriptFailures() { return g_failures; }

// One frame's worth: does the steps that are due. False once the script is over. shot(name) takes a picture.
bool scriptStep(double now, const std::function<bool(const std::string&)>& shot) {
    for (size_t i = 0; i < g_pending.size();) {
        if (now >= g_pending[i].at) { auto f = g_pending[i].f; g_pending.erase(g_pending.begin() + i); f(); } else ++i;
    }
    if (now < g_waitUntil || !g_pending.empty()) return true;
    while (g_next < g_lines.size()) {
        const std::string line = g_lines[g_next++];
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        auto fail = [&](const char* why) { log("[emulator] script: %s: %s", line.c_str(), why); ++g_failures; };
        if (cmd == "wait") { double s = 0; ss >> s; g_waitUntil = now + s; return true; }
        if (cmd == "tap" || cmd == "drag") {
            float x = 0, y = 0, x2 = 0, y2 = 0, dur = 0.3f;
            ss >> x >> y;
            if (cmd == "drag") ss >> x2 >> y2 >> dur;
            float sx, sy;
            if (!tabletPoint(x, y, sx, sy)) { fail("the tablet is not on screen"); continue; }
            mouseMove(sx, sy);
            mouseButton(0, true, sx, sy);
            if (cmd == "tap") { g_pending.push_back({now + 0.05, [sx, sy] { mouseButton(0, false, sx, sy); }}); return true; }
            const int steps = 10;
            for (int k = 1; k <= steps; ++k) {
                const float t = (float)k / steps;
                float ex, ey;
                tabletPoint(x + (x2 - x) * t, y + (y2 - y) * t, ex, ey);
                g_pending.push_back({now + dur * t, [ex, ey] { mouseMove(ex, ey); }});
            }
            float ex, ey;
            tabletPoint(x2, y2, ex, ey);
            g_pending.push_back({now + dur + 0.05, [ex, ey] { mouseButton(0, false, ex, ey); }});
            return true;
        }
        if (cmd == "click") { float x = 0, y = 0; ss >> x >> y; mouseMove(x, y); mouseButton(0, true, x, y); g_pending.push_back({now + 0.05, [x, y] { mouseButton(0, false, x, y); }}); return true; }
        if (cmd == "wheel") {
            float x = 0, y = 0; int c = 0; ss >> x >> y >> c;
            float sx, sy;
            if (!tabletPoint(x, y, sx, sy)) { fail("the tablet is not on screen"); continue; }
            mouseMove(sx, sy); mouseWheel(sx, sy, c);
            continue;
        }
        if (cmd == "type") { std::string rest; std::getline(ss, rest); if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1); for (char c : rest) keyChar((unsigned char)c); continue; }
        if (cmd == "key") {
            std::string k; ss >> k;
            const int vk = k == "return" ? XPLM_VK_RETURN : k == "back" ? XPLM_VK_BACK : k == "escape" ? XPLM_VK_ESCAPE : k == "left" ? XPLM_VK_LEFT
                         : k == "right" ? XPLM_VK_RIGHT : k == "up" ? XPLM_VK_UP : k == "down" ? XPLM_VK_DOWN : k == "tab" ? XPLM_VK_TAB : k == "delete" ? XPLM_VK_DELETE : -1;
            if (vk < 0) { fail("unknown key"); continue; }
            keySpecial(vk, true, 0); keySpecial(vk, false, 0);
            continue;
        }
        if (cmd == "menu") {
            std::string rest; std::getline(ss, rest); if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            bool done = false;
            for (const MenuItem& m : menuItems()) if (m.path.find(rest) != std::string::npos) { pickMenuItem(m); done = true; break; }
            if (!done) fail("no such menu item");
            continue;
        }
        if (cmd == "command") { std::string n; ss >> n; if (!runCommand(n)) fail("no such command"); continue; }
        if (cmd == "set") { std::string n; double v = 0; ss >> n >> v; setNum(n, v); continue; }
        if (cmd == "sim") {
            std::string f; double v = 0; ss >> f >> v;
            Sim& s = sim();
            if (f == "lat") s.lat = v; else if (f == "lon") s.lon = v; else if (f == "hdg") s.hdg = (float)v; else if (f == "alt") s.altFt = (float)v;
            else if (f == "agl") s.aglFt = (float)v; else if (f == "gs") s.gsKt = (float)v; else if (f == "vs") s.vsFpm = (float)v;
            else if (f == "ground") s.onGround = v != 0; else if (f == "moving") s.flying = v != 0; else if (f == "profile") s.profile = (int)v;
            else if (f == "traffic") s.trafficCount = (int)v; else if (f == "range") s.trafficRangeNm = (float)v; else if (f == "cockpit") s.cockpitTablet = v != 0;
            else if (f == "wxplane") { s.wxPlane = v != 0; simApplyProfile(); } else if (f == "opensam") { s.openSam = v != 0; simApplyProfile(); }
            else if (f == "zulu") s.zuluHour = (float)v; else if (f == "sun") { s.autoSun = false; s.sunPitch = (float)v; }
            else fail("unknown sim field");
            continue;
        }
        if (cmd == "intruder") { simAddIntruder(); continue; }
        if (cmd == "view") { std::string v; ss >> v; setView(v == "cockpit" ? View::Cockpit : View::Window); continue; }
        if (cmd == "window") { int l, t, r, b; ss >> l >> t >> r >> b; setTabletWindowRect(l, t, r, b); continue; }
        if (cmd == "reload") { restartPlugin(); continue; }
        if (cmd == "shot") { std::string n; ss >> n; if (!shot(n)) fail("the picture was not written"); continue; }
        fail("unknown step");
    }
    return !g_pending.empty();
}

} // namespace emu
