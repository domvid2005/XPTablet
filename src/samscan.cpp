#include "guard.h"
#include "samscan.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace xpt {
namespace sam {
namespace {

struct Spot { double lat, lon; int kind; int name; };   // kind 2 a jetway, 1 an airport; name: an index into g_names
std::mutex g_mx;
std::vector<Spot> g_spots;
std::vector<std::string> g_names;
Install g_install;
std::thread g_th;
std::atomic<bool> g_quit{false};

bool readFile(const std::string& path, std::string& out, size_t cap) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < cap) out.append(buf, n);
    std::fclose(f);
    return true;
}
bool isAbs(const std::string& p) { return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')); }
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

// an attribute's value in one tag's text (name="value" or name='value'), case not minded
bool attr(const std::string& tag, const char* name, std::string& v) {
    const std::string lt = lower(tag), key = name;
    for (size_t at = lt.find(key); at != std::string::npos; at = lt.find(key, at + 1)) {
        if (at > 0 && lt[at - 1] != ' ' && lt[at - 1] != '\t' && lt[at - 1] != '\n' && lt[at - 1] != '\r') continue;
        size_t p = at + key.size();
        while (p < lt.size() && (lt[p] == ' ' || lt[p] == '\t')) ++p;
        if (p >= lt.size() || lt[p] != '=') continue;
        ++p;
        while (p < lt.size() && (lt[p] == ' ' || lt[p] == '\t')) ++p;
        if (p >= lt.size() || (lt[p] != '"' && lt[p] != '\'')) continue;
        const char q = lt[p];
        const size_t e = tag.find(q, p + 1);
        if (e == std::string::npos) return false;
        v = tag.substr(p + 1, e - p - 1);
        return true;
    }
    return false;
}

// the jetways a sam.xml places: each <jetway .../> (not <jetways>) with a latitude and longitude
int readSamXml(const std::string& text, std::vector<Spot>& out, int name) {
    int n = 0;
    for (size_t p = text.find('<'); p != std::string::npos; p = text.find('<', p + 1)) {
        if (p + 8 > text.size()) break;
        if (lower(text.substr(p + 1, 7)) != "jetway " && lower(text.substr(p + 1, 7)) != "jetway\t" && lower(text.substr(p + 1, 7)) != "jetway\n" && lower(text.substr(p + 1, 7)) != "jetway\r") continue;
        const size_t e = text.find('>', p);
        if (e == std::string::npos) break;
        const std::string tag = text.substr(p, e - p);
        std::string la, lo;
        if (!attr(tag, "latitude", la) || !attr(tag, "longitude", lo)) continue;
        const double lat = std::atof(la.c_str()), lon = std::atof(lo.c_str());
        if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0)) continue;
        out.push_back({lat, lon, 2, name});
        ++n;
    }
    return n;
}

// a pack's airports: each one's place (its first runway's end, helipad or stand) and name
int readAptDat(const std::string& text, std::vector<Spot>& out, std::vector<std::string>& names) {
    int n = 0;
    bool open = false, placed = false;
    std::string name;
    auto close = [&]() { open = false; placed = false; };
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        const char* l = text.c_str() + s;
        const int code = std::atoi(l);
        if (code == 1 || code == 16 || code == 17) {   // an airport: 1 elevation x x ICAO name...
            close();
            open = true;
            std::vector<std::string> tok;
            std::string cur;
            for (const char* q = l; q < text.c_str() + e; ++q) {
                if (*q == ' ' || *q == '\t' || *q == '\r') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
                else cur += *q;
            }
            if (!cur.empty()) tok.push_back(cur);
            name = tok.size() > 4 ? tok[4] : "";
            for (size_t i = 5; i < tok.size(); ++i) name += (i == 5 ? "  " : " ") + tok[i];
        } else if (open && !placed) {
            double la = 0, lo = 0;
            bool got = false;
            if (code == 100 || code == 102 || code == 1300) {
                std::vector<double> v;
                const char* q = l;
                // (a carriage return is space too: Windows line ends. Missed once, it stopped this loop from moving on and it filled
                // memory until X-Plane ended, 2026-11-22. Each pass now moves on or ends, and at most 40 fields are read)
                const char* end = text.c_str() + e;
                while (q < end && v.size() < 40) {
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
                    const char* b = q;
                    while (q < end && *q != ' ' && *q != '\t' && *q != '\r') ++q;
                    if (q == b) break;
                    v.push_back(std::atof(std::string(b, q).c_str()));
                }
                if (code == 100 && v.size() > 10) { la = v[9]; lo = v[10]; got = true; }
                if (code == 102 && v.size() > 3) { la = v[2]; lo = v[3]; got = true; }
                if (code == 1300 && v.size() > 2) { la = v[1]; lo = v[2]; got = true; }
            }
            if (got && std::fabs(la) <= 90 && std::fabs(lo) <= 180 && !(la == 0 && lo == 0)) {
                names.push_back(name);
                out.push_back({la, lo, 1, (int)names.size() - 1});
                placed = true;
                ++n;
            }
        }
        s = e + 1;
    }
    return n;
}

void scan(const std::string& root) {
    namespace fs = std::filesystem;
    Install in;
    std::vector<Spot> spots;
    std::vector<std::string> names;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::u8path(root + "Resources/plugins"), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (lower(it->path().filename().u8string()).find("opensam") != std::string::npos && it->is_directory(ec)) in.pluginFolder = true;
    std::string ini;
    if (readFile(root + "Custom Scenery/scenery_packs.ini", ini, 4 << 20)) {
        bool globalSeen = false;
        size_t s = 0;
        while (s < ini.size() && !g_quit) {
            size_t e = ini.find('\n', s);
            if (e == std::string::npos) e = ini.size();
            std::string line = ini.substr(s, e - s);
            s = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.compare(0, 14, "SCENERY_PACK *") == 0) { if (line.find("GLOBAL_AIRPORTS") != std::string::npos) globalSeen = true; continue; }
            if (line.compare(0, 13, "SCENERY_PACK ") != 0) continue;   // (disabled packs and the header)
            std::string p = line.substr(13);
            if (!isAbs(p)) p = root + p;
            if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
            std::string folder = p.substr(0, p.size() - 1);
            const size_t sl = folder.find_last_of("/\\");
            const std::string packName = sl == std::string::npos ? folder : folder.substr(sl + 1);
            const std::string lp = lower(packName);
            if (lp.find("opensam_library") != std::string::npos) { in.library = true; if (globalSeen) in.libraryBelowGlobal = true; continue; }
            if (lp.find("sam_library") != std::string::npos || lp == "sam") continue;   // (the library itself places no jetways)
            std::string xml;
            if (!readFile(p + "sam.xml", xml, 32 << 20)) continue;
            ++in.packs;
            names.push_back(packName);
            const int nm = (int)names.size() - 1;
            in.jetways += readSamXml(xml, spots, nm);
            std::string apt;
            if (readFile(p + "Earth nav data/apt.dat", apt, 64 << 20)) {
                in.airports += readAptDat(apt, spots, names);
            }
        }
    }
    in.scanned = true;
    std::lock_guard<std::mutex> lk(g_mx);
    g_spots.swap(spots);
    g_names.swap(names);
    g_install = in;
}

double metres(double la1, double lo1, double la2, double lo2) {
    const double k = 3.14159265358979 / 180.0;
    const double x = (lo2 - lo1) * k * std::cos((la1 + la2) * 0.5 * k), y = (la2 - la1) * k;
    return std::sqrt(x * x + y * y) * 6371000.0;
}

}   // namespace

void start(const std::string& root) {
    if (g_th.joinable()) return;
    g_quit = false;
    g_th = std::thread(guarded("SAM scan", [root]() { scan(root); }));
}
void stop() { g_quit = true; if (g_th.joinable()) g_th.join(); }
void scanNow(const std::string& root) { scan(root); }
Install install() { std::lock_guard<std::mutex> lk(g_mx); return g_install; }

int nearby(double lat, double lon, std::string* name) {
    std::lock_guard<std::mutex> lk(g_mx);
    int best = 0;
    double bestD = 1e18;
    for (const Spot& s : g_spots) {
        const double d = metres(lat, lon, s.lat, s.lon);
        const int k = s.kind == 2 ? (d <= 90 ? 2 : 0) : (d <= 2500 ? 1 : 0);
        if (k > best || (k == best && k > 0 && d < bestD)) {
            best = k;
            bestD = d;
            if (name && s.name >= 0 && s.name < (int)g_names.size()) *name = g_names[s.name];
        }
    }
    return best;
}

}   // namespace sam
}   // namespace xpt
