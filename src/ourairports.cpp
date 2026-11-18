#include "ourairports.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "http.h"

namespace xpt {
namespace ourairports {
namespace {

std::thread g_thread;
std::atomic<bool> g_quit{false}, g_ready{false};
std::mutex g_mx;
std::string g_dir, g_status;
std::vector<Info> g_airports;
std::unordered_map<std::string, int> g_byCode;   // ident, ICAO, GPS and local codes

const char* kBase = "https://davidmegginson.github.io/ourairports-data/";

void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_status = s; }

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

// The file from the cache when it is under a month old, else downloaded (the old copy when offline)
bool fetch(const std::string& name, std::string& body) {
    const std::string path = g_dir + "/" + name;
    std::time_t mt = 0;
    std::string cached;
    const bool have = readFile(path, cached, mt);
    if (have && std::difftime(std::time(nullptr), mt) < 30 * 86400.0) { body.swap(cached); return true; }
    setStatus("Downloading " + name + " from OurAirports...");
    std::string err;
    long status = 0;
    if (wx::httpGetCancellable(std::string(kBase) + name, "", body, status, err, 120, 64u << 20, &g_quit) && body.size() > 1000) {
#if defined(_WIN32)
        _mkdir(g_dir.c_str());
#else
        mkdir(g_dir.c_str(), 0755);
#endif
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
        return true;
    }
    if (have) { body.swap(cached); return true; }
    setStatus("OurAirports could not be reached (" + err + ")");
    return false;
}

// CSV rows, quotes and doubled quotes handled; calls row(fields) for each
template <class F> void csv(const std::string& text, F row) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQ = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQ) {
            if (c == '"') { if (i + 1 < text.size() && text[i + 1] == '"') { cur += '"'; ++i; } else inQ = false; }
            else cur += c;
        } else if (c == '"') inQ = true;
        else if (c == ',') { fields.push_back(cur); cur.clear(); }
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            fields.push_back(cur); cur.clear();
            row(fields);
            fields.clear();
        } else cur += c;
    }
    if (!cur.empty() || !fields.empty()) { fields.push_back(cur); row(fields); }
}

void run() {
    std::string airports, freqs;
    if (!fetch("airports.csv", airports) || g_quit) return;
    if (!fetch("airport-frequencies.csv", freqs) || g_quit) return;
    setStatus("Reading OurAirports...");
    std::vector<Info> list;
    std::unordered_map<std::string, int> byCode, byIdent;
    std::unordered_map<std::string, int> col;
    bool header = true;
    csv(airports, [&](const std::vector<std::string>& f) {
        if (header) { for (size_t i = 0; i < f.size(); ++i) col[f[i]] = (int)i; header = false; return; }
        auto get = [&](const char* k) { const auto it = col.find(k); return it != col.end() && it->second < (int)f.size() ? f[it->second] : std::string(); };
        Info in;
        in.found = true;
        in.name = get("name"); in.city = get("municipality"); in.country = get("iso_country"); in.region = get("iso_region");
        in.iata = get("iata_code"); in.type = get("type"); in.wiki = get("wikipedia_link");
        const int idx = (int)list.size();
        list.push_back(in);
        const std::string ident = get("ident");
        byIdent[ident] = idx;
        for (const char* k : {"icao_code", "gps_code", "local_code"}) { const std::string c = get(k); if (!c.empty()) byCode.emplace(c, idx); }
        byCode[ident] = idx;   // the ident wins
    });
    header = true;
    col.clear();
    csv(freqs, [&](const std::vector<std::string>& f) {
        if (header) { for (size_t i = 0; i < f.size(); ++i) col[f[i]] = (int)i; header = false; return; }
        auto get = [&](const char* k) { const auto it = col.find(k); return it != col.end() && it->second < (int)f.size() ? f[it->second] : std::string(); };
        const auto it = byIdent.find(get("airport_ident"));
        if (it == byIdent.end()) return;
        Freq q;
        q.type = get("type"); q.desc = get("description"); q.mhz = std::atof(get("frequency_mhz").c_str());
        if (q.mhz > 0) list[it->second].freqs.push_back(q);
    });
    std::lock_guard<std::mutex> lk(g_mx);
    g_airports.swap(list);
    g_byCode.swap(byCode);
    g_status.clear();
    g_ready = true;
}

} // namespace

void start(const std::string& cacheDir) {
    if (g_thread.joinable()) return;
    g_dir = cacheDir;
    g_quit = false;
    g_thread = std::thread(run);
}
void stop() {
    g_quit = true;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lk(g_mx);
    g_airports.clear(); g_byCode.clear();
    g_ready = false;
}
bool ready() { return g_ready; }
std::string status() { std::lock_guard<std::mutex> lk(g_mx); return g_status; }
Info lookup(const std::string& code) {
    if (!g_ready) return Info();
    std::lock_guard<std::mutex> lk(g_mx);
    const auto it = g_byCode.find(code);
    return it == g_byCode.end() ? Info() : g_airports[it->second];
}

} // namespace ourairports
} // namespace xpt
