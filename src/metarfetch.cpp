#include "metarfetch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "http.h"

namespace xpt {
namespace {

std::thread g_thread;
std::mutex g_m;
std::condition_variable g_cv;
std::atomic<bool> g_stop{false};
bool g_running = false, g_busy = false, g_haveAnswer = false;
std::string g_want;
MetarAnswer g_answer;

void work() {
    for (;;) {
        std::string icao;
        {
            std::unique_lock<std::mutex> l(g_m);
            g_cv.wait(l, [] { return g_stop.load() || !g_want.empty(); });
            if (g_stop) return;
            icao.swap(g_want);
            g_busy = true;
        }
        MetarAnswer a;
        a.icao = icao;
        std::string body, err;
        long status = 0;
        // the latest report as plain text; no report gives an empty answer (HTTP 204 or 200 with nothing in it)
        const bool ok = wx::httpGetCancellable("https://aviationweather.gov/api/data/metar?ids=" + icao + "&format=raw", "", body, status, err, 12, 64u * 1024u, &g_stop);
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) body.pop_back();
        const size_t nl = body.find('\n');   // one report: the first line
        if (nl != std::string::npos) body.resize(nl);
        if (ok && !body.empty()) a.raw = body;
        else if (ok || status == 204) a.error = "No report found for " + icao + ".";
        else a.error = "Could not reach the weather service (" + (err.empty() ? "HTTP " + std::to_string(status) : err) + ").";
        std::lock_guard<std::mutex> l(g_m);
        g_answer = a;
        g_haveAnswer = true;
        g_busy = false;
    }
}

} // namespace

void metarStart() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_running) return;
    g_stop = false;
    g_running = true;
    g_thread = std::thread(work);
}

void metarStop() {
    {
        std::lock_guard<std::mutex> l(g_m);
        if (!g_running) return;
        g_stop = true;
        g_running = false;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> l(g_m);
    g_want.clear(); g_busy = false; g_haveAnswer = false;
}

void metarRequest(const std::string& icao) {
    {
        std::lock_guard<std::mutex> l(g_m);
        if (!g_running || icao.empty()) return;
        g_want = icao;
    }
    g_cv.notify_all();
}

bool metarBusy() {
    std::lock_guard<std::mutex> l(g_m);
    return g_busy || !g_want.empty();
}

bool metarPoll(MetarAnswer& out) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_haveAnswer) return false;
    out = g_answer;
    g_haveAnswer = false;
    return true;
}

} // namespace xpt
