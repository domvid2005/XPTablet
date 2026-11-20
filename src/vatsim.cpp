#include "vatsim.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "http.h"
#include "json_mini.h"
#include "vatsim_parse.h"

namespace xpt {
namespace vatsim {
namespace {

std::thread g_thread;
std::atomic<bool> g_quit{false}, g_want{false};
std::mutex g_mx;
std::condition_variable g_cv;
std::unordered_map<std::string, VatsimPilot> g_pilots;
std::vector<VatsimController> g_ctrls;
std::string g_status = "Asking VATSIM...";
bool g_have = false;


void fetchOnce() {
    std::string body, err;
    long code = 0;
    if (!wx::httpGetCancellable("https://data.vatsim.net/v3/vatsim-data.json", "", body, code, err, 30, 64u << 20, &g_quit)) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_status = g_have ? "VATSIM: not refreshed (" + err + ")" : "VATSIM could not be reached";
        return;
    }
    wx::Json j;
    if (!wx::parseJson(body, j)) { std::lock_guard<std::mutex> lk(g_mx); g_status = "VATSIM's data could not be read"; return; }
    std::unordered_map<std::string, VatsimPilot> pilots;
    readPilots(j, pilots);
    std::vector<VatsimController> ctrls;
    readControllers(j, ctrls);
    std::lock_guard<std::mutex> lk(g_mx);
    g_pilots.swap(pilots);
    g_ctrls.swap(ctrls);
    g_have = true;
    g_status.clear();
}

void run() {
    auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    while (!g_quit) {
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait_for(lk, std::chrono::seconds(1), [] { return g_quit.load(); });
        }
        if (g_quit) break;
        if (!g_want) continue;
        if (std::chrono::steady_clock::now() - last < std::chrono::seconds(30)) continue;   // VATSIM updates every 15 s; 30 is plenty
        last = std::chrono::steady_clock::now();
        fetchOnce();
    }
}

} // namespace

void start() { if (!g_thread.joinable()) { g_quit = false; g_thread = std::thread(run); } }
void stop() {
    g_quit = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lk(g_mx);
    g_pilots.clear(); g_ctrls.clear(); g_have = false; g_status = "Asking VATSIM...";
}
void want(bool on) { g_want = on; }
std::vector<VatsimPilot> allPilots() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<VatsimPilot> out;
    out.reserve(g_pilots.size());
    for (const auto& pr : g_pilots) out.push_back(pr.second);
    return out;
}
std::vector<VatsimController> controllers(bool* have) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (have) *have = g_have;
    return g_ctrls;
}
VatsimPilot lookup(const std::string& callsign, std::string& status) {
    std::lock_guard<std::mutex> lk(g_mx);
    status = g_status;
    const auto it = g_pilots.find(callsign);
    if (it != g_pilots.end()) return it->second;
    VatsimPilot none;
    if (g_have && status.empty()) status = "Not connected to VATSIM (X-Plane's AI, or another network)";
    return none;
}

} // namespace vatsim
} // namespace xpt
