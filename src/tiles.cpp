#include "tiles.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "http.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace xpt {
namespace tiles {
namespace {

std::string g_dir;
std::mutex g_mx;
std::condition_variable g_cv;
std::vector<std::thread> g_threads;
std::atomic<bool> g_quit{false};
std::vector<Key> g_waiting;          // most wanted first
std::set<Key> g_busy;                // being fetched
std::deque<Tile> g_done;
std::map<Key, std::time_t> g_failedAt;   // not asked again for a minute

// OpenStreetMap's standard tiles (its tile usage policy: a real user agent, tiles kept, few connections; attribution shown) and
// Esri's World Imagery (attribution shown).
std::string urlOf(const Key& k) {
    char b[256];
    if (k.src == SATELLITE) std::snprintf(b, sizeof b, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", k.z, k.y, k.x);
    else std::snprintf(b, sizeof b, "https://tile.openstreetmap.org/%d/%d/%d.png", k.z, k.x, k.y);
    return b;
}
std::string fileOf(const Key& k) {
    char b[128];
    std::snprintf(b, sizeof b, "/%s/%d/%d/%d.%s", k.src == SATELLITE ? "satellite" : "osm", k.z, k.x, k.y, k.src == SATELLITE ? "jpg" : "png");
    return g_dir + b;
}
void makeDirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/' && path[i] != '\\') continue;
        const std::string d = path.substr(0, i);
#if defined(_WIN32)
        _mkdir(d.c_str());
#else
        mkdir(d.c_str(), 0755);
#endif
    }
}
bool readFile(const std::string& p, std::string& out, std::time_t& mtime) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    mtime = st.st_mtime;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    out.resize((size_t)st.st_size);
    const size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return !out.empty();
}

bool decode(const std::string& bytes, Tile& t) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return false;
    t.w = w; t.h = h;
    t.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

void worker() {
    while (!g_quit) {
        Key k;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_quit.load() || !g_waiting.empty(); });
            if (g_quit) return;
            k = g_waiting.front();
            g_waiting.erase(g_waiting.begin());
            g_busy.insert(k);
        }
        Tile t;
        t.key = k;
        const std::string file = fileOf(k);
        std::string bytes;
        std::time_t mt = 0;
        const bool cached = readFile(file, bytes, mt);
        const bool fresh = cached && std::difftime(std::time(nullptr), mt) < 30 * 86400.0;   // kept a month, then asked for again
        bool ok = false;
        if (fresh) ok = decode(bytes, t);
        if (!ok) {
            std::string body, err;
            long status = 0;
            if (wx::httpGetCancellable(urlOf(k), "", body, status, err, 20, 4u << 20, &g_quit) && decode(body, t)) {
                ok = true;
                makeDirs(file);
                FILE* f = std::fopen(file.c_str(), "wb");
                if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
            } else if (cached) {
                ok = decode(bytes, t);   // offline: the old copy is better than nothing
            }
        }
        t.failed = !ok;
        std::lock_guard<std::mutex> lk(g_mx);
        g_busy.erase(k);
        if (!ok) g_failedAt[k] = std::time(nullptr);
        g_done.push_back(std::move(t));
        while (g_done.size() > 64) g_done.pop_front();
    }
}

} // namespace

const char* attribution(int source) { return source == SATELLITE ? "Imagery: Esri, Maxar, Earthstar Geographics" : "\xC2\xA9 OpenStreetMap contributors"; }
int maxZoom(int source) { return source == SATELLITE ? 19 : 19; }

void start(const std::string& cacheDir) {
    if (!g_threads.empty()) return;
    g_dir = cacheDir;
    g_quit = false;
    for (int i = 0; i < 2; ++i) g_threads.emplace_back(worker);   // two connections at most (OpenStreetMap's policy)
}

void stop() {
    g_quit = true;
    g_cv.notify_all();
    for (auto& t : g_threads) if (t.joinable()) t.join();
    g_threads.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    g_waiting.clear(); g_busy.clear(); g_done.clear();
}

void want(const std::vector<Key>& keys) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_waiting.clear();
    const std::time_t now = std::time(nullptr);
    for (const Key& k : keys) {
        if (g_busy.count(k)) continue;
        const auto f = g_failedAt.find(k);
        if (f != g_failedAt.end() && std::difftime(now, f->second) < 60) continue;
        if (std::find(g_waiting.begin(), g_waiting.end(), k) == g_waiting.end()) g_waiting.push_back(k);
        if (g_waiting.size() >= 48) break;
    }
    if (!g_waiting.empty()) g_cv.notify_all();
}

bool take(Tile& t) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_done.empty()) return false;
    t = std::move(g_done.front());
    g_done.pop_front();
    return true;
}

} // namespace tiles
} // namespace xpt
