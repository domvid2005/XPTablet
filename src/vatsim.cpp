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

namespace xpt {
namespace vatsim {
namespace {

std::thread g_thread;
std::atomic<bool> g_quit{false}, g_want{false};
std::mutex g_mx;
std::condition_variable g_cv;
std::unordered_map<std::string, VatsimPilot> g_pilots;
std::string g_status = "Asking VATSIM...";
bool g_have = false;

std::string str(const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asString() : std::string(); }
double num(const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asNum(0) : 0.0; }

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
    const wx::Json* list = j.get("pilots");
    if (list && list->type == wx::Json::Arr)
        for (const wx::Json& p : list->arr) {
            VatsimPilot v;
            v.found = true;
            v.callsign = str(&p, "callsign");
            v.name = str(&p, "name");
            v.cid = str(&p, "cid");
            v.server = str(&p, "server");
            v.squawk = str(&p, "transponder");
            v.altitudeFt = (int)num(&p, "altitude");
            v.groundspeedKt = (int)num(&p, "groundspeed");
            v.heading = (int)num(&p, "heading");
            v.qnhMb = (int)num(&p, "qnh_mb");
            v.logon = str(&p, "logon_time");
            if (v.logon.size() >= 16) v.logon = v.logon.substr(11, 5) + "Z";   // "2026-11-18T05:12:33..." -> "05:12Z"
            const int rating = (int)num(&p, "pilot_rating");
            static const char* ratings[] = {"no rating", "PPL", "IR", "CMEL", "ATPL", "flight instructor", "flight examiner"};
            v.rating = rating == 0 ? "no rating" : rating == 1 ? "PPL" : rating == 3 ? "IR" : rating == 7 ? "CMEL" : rating == 15 ? "ATPL" : rating == 31 ? "flight instructor" : rating == 63 ? "flight examiner" : std::to_string(rating);
            (void)ratings;
            const wx::Json* fp = p.get("flight_plan");
            if (fp && fp->type == wx::Json::Obj) {
                v.hasPlan = true;
                v.rules = str(fp, "flight_rules") == "V" ? "VFR" : "IFR";
                v.aircraft = str(fp, "aircraft_short"); if (v.aircraft.empty()) v.aircraft = str(fp, "aircraft");
                v.aircraftFull = str(fp, "aircraft");
                v.departure = str(fp, "departure"); v.arrival = str(fp, "arrival"); v.alternate = str(fp, "alternate");
                v.cruiseAlt = str(fp, "altitude"); v.cruiseTas = str(fp, "cruise_tas");
                v.depTime = str(fp, "deptime"); v.enroute = str(fp, "enroute_time"); v.fuel = str(fp, "fuel_time");
                v.route = str(fp, "route"); v.remarks = str(fp, "remarks");
                v.assignedSquawk = str(fp, "assigned_transponder");
            }
            pilots[v.callsign] = std::move(v);
        }
    std::lock_guard<std::mutex> lk(g_mx);
    g_pilots.swap(pilots);
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
    g_pilots.clear(); g_have = false; g_status = "Asking VATSIM...";
}
void want(bool on) { g_want = on; }
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
