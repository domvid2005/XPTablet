#include "aptdat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace xpt {
namespace aptdat {
namespace {

struct Entry { std::string icao; double lat = 0, lon = 0; int kind = 1; int file = 0; long long offset = 0, length = 0; bool placed = false; };

std::mutex g_mx;
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_quit{false};
std::string g_root;
// the index (written by the thread while it is built; read only by it)
std::vector<std::string> g_files;
std::vector<Entry> g_entries;
std::unordered_map<int, std::vector<int>> g_grid;   // 1-degree cells: entries
std::atomic<bool> g_indexing{false};
std::atomic<int> g_count{0};
// the view asked for, and what is ready for it
struct View { double lat = 0, lon = 0, layoutNm = 0, pointNm = 0; bool layouts = false; unsigned seq = 0; } g_view;
std::vector<std::shared_ptr<const AptGeo>> g_readyLayouts;
std::vector<AptPoint> g_readyPoints;

int cellOf(double lat, double lon) { return (int)std::floor(lat + 90) * 400 + (int)std::floor(lon + 180); }
double distNm(double la1, double lo1, double la2, double lo2) {
    const double kD = 3.14159265358979323846 / 180.0;
    double dl = lo2 - lo1;
    while (dl > 180) dl -= 360;
    while (dl < -180) dl += 360;
    const double x = dl * std::cos((la1 + la2) * 0.5 * kD), y = la2 - la1;
    return std::sqrt(x * x + y * y) * 60.0;
}

bool isAbs(const std::string& p) { return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')); }

// the apt.dat files, highest priority first
std::vector<std::string> aptFiles(const std::string& root) {
    std::vector<std::string> out;
    const std::string global = root + "Global Scenery/Global Airports/Earth nav data/apt.dat";
    bool globalPlaced = false;
    FILE* f = std::fopen((root + "Custom Scenery/scenery_packs.ini").c_str(), "rb");
    if (f) {
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.compare(0, 14, "SCENERY_PACK *") == 0 && s.find("GLOBAL_AIRPORTS") != std::string::npos) { out.push_back(global); globalPlaced = true; continue; }
            if (s.compare(0, 13, "SCENERY_PACK ") != 0) continue;   // SCENERY_PACK_DISABLED and the header lines
            std::string p = s.substr(13);
            if (!isAbs(p)) p = root + p;
            if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
            out.push_back(p + "Earth nav data/apt.dat");
        }
        std::fclose(f);
    }
    if (!globalPlaced) out.push_back(global);
    return out;
}

// Reads every airport's header and place from one file (positions from its first runway, helipad, datum or pavement node).
void indexFile(int fileIdx, std::unordered_map<std::string, int>& seen) {
    FILE* f = std::fopen(g_files[fileIdx].c_str(), "rb");
    if (!f) return;
    std::vector<char> buf(4 << 20);
    std::string carry;
    long long base = 0;   // file offset of carry's first byte
    int cur = -1;         // the entry being read (-1: none, -2: a repeat, skipped)
    long long curStart = 0;
    auto closeCur = [&](long long at) { if (cur >= 0) g_entries[cur].length = at - g_entries[cur].offset; cur = -1; };
    auto handle = [&](const char* s, const char* e, long long at) {
        if (s == e) return;
        // the row code
        int code = 0;
        const char* p = s;
        while (p < e && *p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
        if (p == s || (p < e && *p != ' ' && *p != '\t')) return;
        if (code == 1 || code == 16 || code == 17 || code == 99) {
            closeCur(at);
            if (code == 99) return;
            // 1 elevation deprecated deprecated ICAO name
            char tok[5][64] = {};
            int n = 0;
            const char* q = s;
            while (q < e && n < 5) {
                while (q < e && (*q == ' ' || *q == '\t')) ++q;
                const char* b = q;
                while (q < e && *q != ' ' && *q != '\t') ++q;
                if (q > b) { const size_t L = std::min<size_t>(63, (size_t)(q - b)); std::memcpy(tok[n], b, L); tok[n][L] = 0; ++n; }
            }
            if (n < 5) { cur = -2; return; }
            const std::string icao = tok[4];
            if (seen.count(icao)) { cur = -2; return; }
            Entry en; en.icao = icao; en.kind = code; en.file = fileIdx; en.offset = at;
            g_entries.push_back(en);
            cur = (int)g_entries.size() - 1;
            seen[icao] = cur;
            curStart = at;
            return;
        }
        if (cur < 0) return;
        Entry& en = g_entries[cur];
        if (en.placed && code != 1302) return;
        auto num = [&](int idx) {   // the idx-th word as a number
            const char* q = s;
            for (int k = 0; k <= idx; ++k) {
                while (q < e && (*q == ' ' || *q == '\t')) ++q;
                if (k == idx) return std::strtod(q, nullptr);
                while (q < e && *q != ' ' && *q != '\t') ++q;
            }
            return 0.0;
        };
        if (code == 100) { en.lat = (num(9) + num(18)) * 0.5; en.lon = (num(10) + num(19)) * 0.5; en.placed = true; }
        else if (code == 101) { en.lat = (num(4) + num(7)) * 0.5; en.lon = (num(5) + num(8)) * 0.5; en.placed = true; }
        else if (code == 102 && !en.placed) { en.lat = num(2); en.lon = num(3); en.placed = true; }
        else if ((code == 111 || code == 112 || code == 1300) && !en.placed) { en.lat = num(1); en.lon = num(2); en.placed = true; }
        else if (code == 1302) {
            const char* q = s + 4;
            while (q < e && *q == ' ') ++q;
            if (e - q > 9 && std::strncmp(q, "datum_lat", 9) == 0) { const double v = num(2); if (v != 0) { en.lat = v; en.placed = true; } }
            else if (e - q > 9 && std::strncmp(q, "datum_lon", 9) == 0) { const double v = num(2); if (v != 0) en.lon = v; }
        }
        (void)curStart;
    };
    while (!g_quit) {
        const size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got == 0) break;
        carry.append(buf.data(), got);
        size_t pos = 0;
        while (true) {
            const size_t nl = carry.find('\n', pos);
            if (nl == std::string::npos) break;
            size_t le = nl;
            if (le > pos && carry[le - 1] == '\r') --le;
            handle(carry.data() + pos, carry.data() + le, base + (long long)pos);
            pos = nl + 1;
        }
        carry.erase(0, pos);
        base += (long long)pos;
    }
    if (!carry.empty()) handle(carry.data(), carry.data() + carry.size(), base);
    std::fseek(f, 0, SEEK_END);
    closeCur((long long)std::ftell(f));
    std::fclose(f);
}

std::shared_ptr<const AptGeo> loadLayout(const Entry& en) {
    auto g = std::make_shared<AptGeo>();
    if (en.length <= 0 || en.length > (64ll << 20)) return nullptr;
    FILE* f = std::fopen(g_files[en.file].c_str(), "rb");
    if (!f) return nullptr;
    std::string rows((size_t)en.length, '\0');
#if defined(_WIN32)
    _fseeki64(f, en.offset, SEEK_SET);
#else
    fseeko(f, (off_t)en.offset, SEEK_SET);
#endif
    const size_t got = std::fread(&rows[0], 1, rows.size(), f);
    std::fclose(f);
    rows.resize(got);
    if (!parseAirport(rows, *g)) return nullptr;
    if (g->icao.empty()) g->icao = en.icao;
    return g;
}

void run() {
    g_indexing = true;
    g_files = aptFiles(g_root);
    std::unordered_map<std::string, int> seen;
    for (int i = 0; i < (int)g_files.size() && !g_quit; ++i) indexFile(i, seen);
    for (int i = 0; i < (int)g_entries.size(); ++i) if (g_entries[i].placed) g_grid[cellOf(g_entries[i].lat, g_entries[i].lon)].push_back(i);
    g_count = (int)g_entries.size();
    g_indexing = false;
    // layouts read so far, most recently used first
    std::list<std::pair<int, std::shared_ptr<const AptGeo>>> cache;
    std::map<int, bool> failed;
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
        // the airports around the view, nearest first
        const double reach = std::max(v.pointNm, v.layouts ? v.layoutNm : 0.0);
        const int dLat = (int)std::ceil(reach / 60.0) + 1;
        const int dLon = std::min(180, (int)std::ceil(reach / 60.0 / std::max(0.05, std::cos(v.lat * 3.14159265358979 / 180.0))) + 1);
        std::vector<std::pair<double, int>> near;
        for (int a = -dLat; a <= dLat; ++a)
            for (int b = -dLon; b <= dLon; ++b) {
                double lo = v.lon + b;
                if (lo >= 180) lo -= 360;
                if (lo < -180) lo += 360;
                const auto it = g_grid.find(cellOf(v.lat + a, lo));
                if (it == g_grid.end()) continue;
                for (int i : it->second) {
                    const double d = distNm(v.lat, v.lon, g_entries[i].lat, g_entries[i].lon);
                    if (d <= reach) near.push_back({d, i});
                }
            }
        std::sort(near.begin(), near.end());
        std::vector<AptPoint> points;
        for (const auto& pr : near) {
            if (pr.first > v.pointNm || points.size() >= 600) break;
            const Entry& en = g_entries[pr.second];
            points.push_back({en.icao, en.lat, en.lon, en.kind});
        }
        std::vector<std::shared_ptr<const AptGeo>> layouts;
        if (v.layouts) {
            int want = 0;
            for (const auto& pr : near) {
                if (pr.first > v.layoutNm || want >= 10 || g_quit) break;
                ++want;
                auto it = std::find_if(cache.begin(), cache.end(), [&](const std::pair<int, std::shared_ptr<const AptGeo>>& c) { return c.first == pr.second; });
                std::shared_ptr<const AptGeo> g;
                if (it != cache.end()) { g = it->second; cache.splice(cache.begin(), cache, it); }
                else if (!failed.count(pr.second)) {
                    g = loadLayout(g_entries[pr.second]);
                    if (!g) failed[pr.second] = true;
                    else { cache.push_front({pr.second, g}); if (cache.size() > 24) cache.pop_back(); }
                }
                if (g) layouts.push_back(g);
                // what is ready so far goes out at once, so the nearest airport shows before the rest are read
                std::lock_guard<std::mutex> lk(g_mx);
                if (g_view.seq != done) break;   // the view moved on
                g_readyLayouts = layouts;
                g_readyPoints = points;
            }
        }
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_view.seq == done) { g_readyLayouts = layouts; g_readyPoints = points; }
    }
}

} // namespace

void start(const std::string& xplaneRoot) {
    if (g_thread.joinable()) return;
    g_root = xplaneRoot;
    g_quit = false;
    g_thread = std::thread(run);
}

void stop() {
    g_quit = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_entries.clear(); g_grid.clear(); g_files.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    g_readyLayouts.clear(); g_readyPoints.clear();
    g_count = 0;
}

void setView(double lat, double lon, double layoutNm, double pointNm, bool wantLayouts) {
    std::lock_guard<std::mutex> lk(g_mx);
    // a new request only when the view moved noticeably (the map is redrawn every frame)
    const double moved = distNm(lat, lon, g_view.lat, g_view.lon);
    if (moved < std::max(0.2, layoutNm * 0.15) && std::fabs(layoutNm - g_view.layoutNm) < 0.25 * std::max(1.0, layoutNm) &&
        std::fabs(pointNm - g_view.pointNm) < 0.25 * std::max(1.0, pointNm) && wantLayouts == g_view.layouts && g_view.seq) return;
    g_view.lat = lat; g_view.lon = lon; g_view.layoutNm = layoutNm; g_view.pointNm = pointNm; g_view.layouts = wantLayouts;
    ++g_view.seq;
    g_cv.notify_all();
}

void get(std::vector<std::shared_ptr<const AptGeo>>& layouts, std::vector<AptPoint>& points, bool& indexing, int& count) {
    std::lock_guard<std::mutex> lk(g_mx);
    layouts = g_readyLayouts;
    points = g_readyPoints;
    indexing = g_indexing;
    count = g_count;
}

} // namespace aptdat
} // namespace xpt
