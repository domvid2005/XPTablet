#include "guard.h"
#include "navdata.h"
#include <set>
#include <map>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace xpt {
namespace navdata {
namespace {

std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mx;
std::condition_variable g_cv;
std::string g_root;
std::vector<MapState::Navaid> g_navs;
std::vector<MapState::Fix> g_fixes;
std::unordered_map<int, std::vector<int>> g_navGrid, g_fixGrid;
struct View { double lat = 0, lon = 0, navNm = 0, fixNm = 0; unsigned seq = 0; } g_view;
std::vector<MapState::Navaid> g_outNavs;
std::vector<MapState::Fix> g_outFixes;
std::atomic<bool> g_loaded{false};
std::unordered_map<std::string, std::vector<std::pair<double, double>>> g_byId;   // a fix's or navaid's name: everywhere it is
std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> g_airways;   // an airway's name: its segments (fix to fix)

int cellOf(double lat, double lon) { return (int)std::floor(lat + 90) * 400 + (int)std::floor(lon + 180); }
double distNm(double la1, double lo1, double la2, double lo2) {
    double dl = lo2 - lo1;
    while (dl > 180) dl -= 360;
    while (dl < -180) dl += 360;
    const double x = dl * std::cos((la1 + la2) * 0.5 * 3.14159265358979 / 180.0), y = la2 - la1;
    return std::sqrt(x * x + y * y) * 60.0;
}
bool exists(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; }
std::string pick(const char* name) {
    const std::string custom = g_root + "Custom Data/" + name;
    return exists(custom) ? custom : g_root + "Resources/default data/" + name;
}
template <class F> void eachLine(const std::string& path, F fn) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char line[1024];
    while (!g_quit && std::fgets(line, sizeof line, f)) fn(line);
    std::fclose(f);
}
// the words of a line
int words(char* s, char** w, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s || *s == '\n' || *s == '\r') break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') ++s;
        if (*s) *s++ = 0;
    }
    return n;
}

void load() {
    std::map<std::string, float> gsOf;   // airport|ident -> the glide slope's angle
    std::set<std::string> dmeOf;         // airport|ident of the DMEs
    // earth_nav.dat (11.50): code lat lon elevation frequency range [variation|bearing] ident [airport|ENRT] region name...
    eachLine(pick("earth_nav.dat"), [&](char* line) {
        char* w[16];
        char copy[1024];
        std::snprintf(copy, sizeof copy, "%s", line);
        const int n = words(line, w, 16);
        if (n < 9) return;
        const int code = std::atoi(w[0]);
        if (code == 6 && n >= 9) {   // a glide slope: its angle x 100000 plus the course ("300087.373": 3.00 degrees); kept for its localizer
            const double gb = std::atof(w[6]);
            gsOf[std::string(w[8]) + "|" + w[7]] = (float)(std::floor(gb / 1000.0) / 100.0);
            return;
        }
        if (code == 12 && n >= 9) dmeOf.insert(std::string(w[8]) + "|" + w[7]);   // (an ILS's DME: same ident and airport)
        if (code != 2 && code != 3 && code != 4 && code != 5 && code != 12 && code != 13) return;
        MapState::Navaid v;
        v.type = code;
        v.lat = std::atof(w[1]); v.lon = std::atof(w[2]);
        const double fr = std::atof(w[4]);
        v.freq = (float)(code == 2 ? fr : fr / 100.0);   // NDB in kHz, the others in 10 kHz
        const double b = std::atof(w[6]);
        // localizers: the true course plus the magnetic one x 360 ("33207.373": 92 magnetic, 87.373 true, KMIA 09's IBUL in
        // Navigraph's 1200 data; checked on all 4117 localizers): the remainder after 360 is the true course. VORs: the variation.
        v.bearing = (float)((code == 4 || code == 5) ? std::fmod(std::fmod(b, 360.0) + 360.0, 360.0) : b);
        v.ident = w[7];
        if (code == 4 || code == 5) {   // its airport and runway, and the magnetic course (the rest of the packed field)
            v.magCourse = (float)std::floor((b - v.bearing) / 360.0 + 0.5);
            if (n >= 11) { v.airport = w[8]; v.runway = w[10]; }
        }
        // the name: the rest of the original line after the region
        if (n >= 10) {
            const char* at = std::strstr(copy, w[9]);
            if (at) { v.name = at + std::strlen(w[9]); while (!v.name.empty() && (v.name[0] == ' ')) v.name.erase(0, 1); while (!v.name.empty() && (v.name.back() == '\n' || v.name.back() == '\r')) v.name.pop_back(); }
        }
        if (code == 12) {   // a DME with a VOR at the same place adds nothing to draw
            for (int i = (int)g_navs.size() - 1; i >= 0 && i >= (int)g_navs.size() - 4; --i)
                if (g_navs[i].ident == v.ident && std::fabs(g_navs[i].lat - v.lat) < 0.01 && std::fabs(g_navs[i].lon - v.lon) < 0.01) return;
        }
        g_navs.push_back(v);
    });
    for (MapState::Navaid& v : g_navs) if ((v.type == 4 || v.type == 5) && !v.airport.empty()) {
        const std::string k = v.airport + "|" + v.ident;
        const auto g = gsOf.find(k);
        if (g != gsOf.end()) v.gsAngle = g->second;
        v.dme = dmeOf.count(k) > 0;
    }
    // earth_fix.dat (11.01): lat lon ident [airport|ENRT] region [type]
    eachLine(pick("earth_fix.dat"), [&](char* line) {
        char* w[8];
        const int n = words(line, w, 8);
        if (n < 4) return;
        const double la = std::atof(w[0]), lo = std::atof(w[1]);
        if (la == 0 && lo == 0) return;
        if (std::strlen(w[2]) > 8 || !((w[0][0] >= '0' && w[0][0] <= '9') || w[0][0] == '-')) return;
        MapState::Fix fx;
        fx.lat = la; fx.lon = lo; fx.ident = w[2];
        fx.enroute = std::strcmp(w[3], "ENRT") == 0;
        g_fixes.push_back(fx);
    });
    for (int i = 0; i < (int)g_navs.size(); ++i) g_navGrid[cellOf(g_navs[i].lat, g_navs[i].lon)].push_back(i);
    for (int i = 0; i < (int)g_fixes.size(); ++i) g_fixGrid[cellOf(g_fixes[i].lat, g_fixes[i].lon)].push_back(i);
    // for filed routes: every name's places, and the airways (earth_awy.dat: fix region type, fix region type, direction, level,
    // base, top, names joined by '-')
    for (const MapState::Navaid& v : g_navs) if (v.type == 2 || v.type == 3 || v.type == 12 || v.type == 13) g_byId[v.ident].push_back({v.lat, v.lon});
    for (const MapState::Fix& fx : g_fixes) g_byId[fx.ident].push_back({fx.lat, fx.lon});
    eachLine(pick("earth_awy.dat"), [&](char* line) {
        char* w[12];
        const int n = words(line, w, 12);
        if (n < 11 || !((w[0][0] >= 'A' && w[0][0] <= 'Z') || (w[0][0] >= '0' && w[0][0] <= '9'))) return;
        if (std::isdigit((unsigned char)w[1][0]) && std::strlen(w[1]) > 2) return;   // (the header)
        const std::string a = w[0], b = w[3];
        char* names = w[10];
        for (char* s = names; *s;) {
            char* e = std::strchr(s, '-');
            std::string nm = e ? std::string(s, e - s) : std::string(s);
            if (!nm.empty()) g_airways[nm].push_back({a, b});
            if (!e) break;
            s = e + 1;
        }
    });
    g_loaded = true;
}

template <class T> void around(const std::vector<T>& all, const std::unordered_map<int, std::vector<int>>& grid, const View& v, double nm, size_t cap, std::vector<T>& out) {
    out.clear();
    if (nm <= 0) return;
    const int dLat = (int)std::ceil(nm / 60.0) + 1;
    const int dLon = std::min(180, (int)std::ceil(nm / 60.0 / std::max(0.05, std::cos(v.lat * 3.14159265358979 / 180.0))) + 1);
    std::vector<std::pair<double, int>> near;
    for (int a = -dLat; a <= dLat; ++a)
        for (int b = -dLon; b <= dLon; ++b) {
            double lo = v.lon + b;
            if (lo >= 180) lo -= 360;
            if (lo < -180) lo += 360;
            const auto it = grid.find(cellOf(v.lat + a, lo));
            if (it == grid.end()) continue;
            for (int i : it->second) { const double d = distNm(v.lat, v.lon, all[i].lat, all[i].lon); if (d <= nm) near.push_back({d, i}); }
        }
    std::sort(near.begin(), near.end());
    for (const auto& p : near) { if (out.size() >= cap) break; out.push_back(all[p.second]); }
}

void run() {
    load();
    unsigned done = 0;
    while (!g_quit) {
        View v;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return g_quit.load() || g_view.seq != done; });
            if (g_quit) break;
            if (g_view.seq == done) continue;
            v = g_view;
        }
        done = v.seq;
        std::vector<MapState::Navaid> navs;
        std::vector<MapState::Fix> fixes;
        around(g_navs, g_navGrid, v, v.navNm, 600, navs);
        around(g_fixes, g_fixGrid, v, v.fixNm, 1500, fixes);
        std::lock_guard<std::mutex> lk(g_mx);
        g_outNavs.swap(navs);
        g_outFixes.swap(fixes);
    }
}

} // namespace

void start(const std::string& xplaneRoot) {
    if (g_thread.joinable()) return;
    g_root = xplaneRoot;
    g_quit = false;
    g_thread = std::thread(guarded("navigation data", [] { run(); }));
}
void stop() {
    g_quit = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_loaded = false;
    g_navs.clear(); g_fixes.clear(); g_navGrid.clear(); g_fixGrid.clear(); g_byId.clear(); g_airways.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    g_outNavs.clear(); g_outFixes.clear();
}
bool ready() { return g_loaded; }
bool resolveRoute(double lat, double lon, const std::vector<std::string>& words, std::vector<MapState::RoutePoint>& out) {
    if (!g_loaded) return false;
    double la = lat, lo = lon;
    std::string prevId, airway;
    auto nearest = [&](const std::string& id, double& rla, double& rlo) {   // that name's place nearest the last point (not across the world)
        const auto it = g_byId.find(id);
        if (it == g_byId.end()) return false;
        double best = 1e18;
        for (const auto& p : it->second) { const double d = distNm(la, lo, p.first, p.second); if (d < best) { best = d; rla = p.first; rlo = p.second; } }
        return best < 1500;
    };
    auto add = [&](const std::string& id, double pla, double plo) { out.push_back({pla, plo, id}); la = pla; lo = plo; prevId = id; };
    auto coord = [&](const std::string& t, double& rla, double& rlo) {   // 4530N07500W, 45N075W, 4530N075W...
        size_t ns = t.find_first_of("NS");
        if (ns == std::string::npos || ns < 2 || ns + 4 > t.size()) return false;
        const std::string a = t.substr(0, ns), b = t.substr(ns + 1, t.size() - ns - 2);
        const char ew = t.back();
        if ((ew != 'E' && ew != 'W') || a.find_first_not_of("0123456789") != std::string::npos || b.find_first_not_of("0123456789") != std::string::npos) return false;
        auto deg = [](const std::string& s, int degDigits) { const double d = std::atof(s.substr(0, degDigits).c_str()); const double m = s.size() > (size_t)degDigits ? std::atof(s.substr(degDigits, 2).c_str()) : 0; return d + m / 60.0; };
        if (a.size() != 2 && a.size() != 4) return false;
        if (b.size() != 3 && b.size() != 5) return false;
        rla = deg(a, 2) * (t[ns] == 'S' ? -1 : 1); rlo = deg(b, 3) * (ew == 'W' ? -1 : 1);
        return true;
    };
    for (std::string t : words) {
        const size_t slash = t.find('/');
        if (slash != std::string::npos) t = t.substr(0, slash);
        if (t.empty() || t == "DCT") continue;
        double pla = 0, plo = 0;
        if (g_airways.count(t) && !prevId.empty()) { airway = t; continue; }   // along it to the next fix named
        if (coord(t, pla, plo)) { add(t, pla, plo); airway.clear(); continue; }
        if (t.size() > 5 || t.find_first_of("0123456789") != std::string::npos) { airway.clear(); continue; }   // a SID, STAR or level
        if (!nearest(t, pla, plo)) { airway.clear(); continue; }
        if (!airway.empty()) {   // the airway's fixes between the last one and this one, in order (the shortest way along its segments)
            const auto& segs = g_airways[airway];
            std::unordered_map<std::string, std::vector<std::string>> adj;
            for (const auto& s : segs) { adj[s.first].push_back(s.second); adj[s.second].push_back(s.first); }
            std::unordered_map<std::string, std::string> from;
            std::vector<std::string> q{prevId};
            from[prevId] = prevId;
            for (size_t i = 0; i < q.size() && !from.count(t); ++i)
                for (const std::string& nx : adj[q[i]]) if (!from.count(nx)) { from[nx] = q[i]; q.push_back(nx); }
            if (from.count(t)) {
                std::vector<std::string> path;
                for (std::string c = from[t]; c != prevId; c = from[c]) path.push_back(c);
                for (auto it = path.rbegin(); it != path.rend() && out.size() < 400; ++it) { double ila, ilo; if (nearest(*it, ila, ilo)) add(*it, ila, ilo); }
            }
            airway.clear();
        }
        add(t, pla, plo);
        if (out.size() >= 400) break;
    }
    return true;
}
void setView(double lat, double lon, double navNm, double fixNm) {
    std::lock_guard<std::mutex> lk(g_mx);
    const double moved = distNm(lat, lon, g_view.lat, g_view.lon);
    const double reach = std::max(navNm, fixNm);
    if (g_view.seq && moved < std::max(0.5, reach * 0.15) && std::fabs(navNm - g_view.navNm) < 0.2 * std::max(1.0, navNm) &&
        std::fabs(fixNm - g_view.fixNm) < 0.2 * std::max(1.0, fixNm) && (navNm > 0) == (g_view.navNm > 0) && (fixNm > 0) == (g_view.fixNm > 0)) return;
    g_view.lat = lat; g_view.lon = lon; g_view.navNm = navNm; g_view.fixNm = fixNm;
    ++g_view.seq;
    g_cv.notify_all();
}
void get(std::vector<MapState::Navaid>& navs, std::vector<MapState::Fix>& fixes) {
    std::lock_guard<std::mutex> lk(g_mx);
    navs = g_outNavs;
    fixes = g_outFixes;
}

} // namespace navdata
} // namespace xpt
