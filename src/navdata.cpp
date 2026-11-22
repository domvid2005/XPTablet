#include "guard.h"
#include "navdata.h"
#include <set>
#include <map>

#include <algorithm>
#include <atomic>
#include <chrono>
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
    g_navs.clear(); g_fixes.clear(); g_navGrid.clear(); g_fixGrid.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    g_outNavs.clear(); g_outFixes.clear();
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
