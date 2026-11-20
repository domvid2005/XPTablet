#include "airspace.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "airspace_parse.h"
#include "http.h"

namespace xpt {
namespace airspace {
namespace {
std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mx;
std::shared_ptr<const AirspaceSet> g_set;
std::string g_status;
bool g_started = false;

const char* const kBoundaries = "https://raw.githubusercontent.com/vatsimnetwork/vatspy-data-project/master/Boundaries.geojson";
const char* const kDat = "https://raw.githubusercontent.com/vatsimnetwork/vatspy-data-project/master/VATSpy.dat";

bool readAll(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}
void status(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_status = s; }
// the cached copy when it is under a week old, else a fresh one (the cache kept as it was when the download fails)
bool fetch(const std::string& url, const std::string& path, std::string& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto p = fs::u8path(path);
    const bool cached = fs::exists(p, ec);
    if (cached) {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(p, ec);
        if (!ec && age < std::chrono::hours(24 * 7) && readAll(path, out)) return true;
    }
    std::string body, err;
    long code = 0;
    if (wx::httpGetCancellable(url, "", body, code, err, 60, 64u << 20, &g_quit) && !body.empty()) {
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(body.data(), (std::streamsize)body.size());
        out.swap(body);
        return true;
    }
    return cached && readAll(path, out);
}
void run(std::string dir) {
    status("Reading VATSIM's airspaces...");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    std::string geo, dat;
    if (!fetch(kBoundaries, dir + "/Boundaries.geojson", geo) || g_quit) { status("VATSIM's airspaces could not be fetched"); return; }
    fetch(kDat, dir + "/VATSpy.dat", dat);
    auto set = std::make_shared<AirspaceSet>();
    parseBoundaries(geo, set->areas);
    parseFirs(dat, set->firs);
    std::lock_guard<std::mutex> lk(g_mx);
    if (set->areas.empty()) { g_status = "VATSIM's airspaces could not be read"; return; }
    g_set = set;
    g_status.clear();
}
} // namespace

void start(const std::string& cacheDir) {
    if (g_started) return;
    g_started = true; g_quit = false;
    g_thread = std::thread(run, cacheDir);
}
void stop() {
    g_quit = true;
    if (g_thread.joinable()) g_thread.join();
    g_started = false;
    std::lock_guard<std::mutex> lk(g_mx);
    g_set.reset();
}
std::shared_ptr<const AirspaceSet> get(std::string& st) {
    std::lock_guard<std::mutex> lk(g_mx);
    st = g_status;
    return g_set;
}

} // namespace airspace
} // namespace xpt
