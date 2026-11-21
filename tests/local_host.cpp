// A stand-in for the plugin, for Local Connect's end-to-end test: the real server and bridge with a sample flight, on this
// machine's own address. It prints the pairing code (as the sim would show it) and what devices asked for.
//   local_host <page.gz> <data-dir> [port]
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "imgui.h"
#include "localbridge.h"
#include "localserver.h"
#include "localwire.h"

using namespace xpt;

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: local_host <page.gz> <data-dir> [port]\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    ImGui::CreateContext();   // (the tablet's settings and notes are kept by its own code, which wants a context)
    localserver::Config cfg;
    cfg.bindIp = "127.0.0.1"; cfg.allowLoopback = true; cfg.port = argc > 3 ? std::atoi(argv[3]) : 18543;
    cfg.pagePath = argv[1]; cfg.dataDir = argv[2]; cfg.wireVersion = wire::kVersion;
    localserver::start(cfg);
    SimState s;
    s.acfIcao = "A321"; s.zuluSec = 14 * 3600; s.batteryPct = 88; s.fps = 45; s.engineType = 5; s.toliss = true; s.fuelKg = 6200;
    s.map.havePos = true; s.map.lat = 41.7087; s.map.lon = -86.3173; s.map.hdgTrue = 270; s.map.altFt = 799; s.map.onGround = true; s.map.utc = 1795262400;
    s.ground.onGround = true; s.ground.toliss = true; s.ground.paxDoors = 4; s.ground.cargoDoors = 2; s.ground.parkBrake = true;
    s.wx.valid = true; s.wx.windDir = 250; s.wx.windKt = 9; s.wx.visSm = 10; s.wx.tempC = 4; s.wx.dewC = -2; s.wx.qnhHpa = 1019;
    s.dataInfo.push_back({"Running in", "Local Connect's test host"});
    std::string lastCode, lastSettings = settingsToText();
    bool toldOn = false, mapWas = false;
    int devices = -1;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const localserver::Status st = localserver::status();
        if (st.state == -1) { std::printf("error: %s\n", st.error.c_str()); return 1; }
        if (st.state == 2 && !toldOn) { toldOn = true; std::printf("address: %s\n", st.address.c_str()); }
        if (st.pairCode != lastCode) { lastCode = st.pairCode; if (!lastCode.empty()) std::printf("pair: %s %s\n", lastCode.c_str(), st.pairName.c_str()); }
        if (localserver::clients() != devices) { devices = localserver::clients(); std::printf("devices: %d\n", devices); }
        s.zuluSec = 14 * 3600 + (int)now;
        s.map.hdgTrue = 270 + (float)std::fmod(now * 3, 20.0);   // (something that moves)
        localbridge::publish(s, now);
        const localbridge::Took took = localbridge::take(s);
        if (took.settings && settingsToText() != lastSettings) { lastSettings = settingsToText(); std::printf("settings: changed by a device (theme %d)\n", settings().wallpaper); }
        Actions act;
        localbridge::merge(act);
        if (act.mapShown != mapWas) { mapWas = act.mapShown; std::printf("level: map %s\n", mapWas ? "shown" : "hidden"); }
        if (act.ground != GA_NONE) { std::printf("event: ground %d %d\n", act.ground, act.groundIndex); if (act.ground == GA_CHOCKS) s.ground.chocks = !s.ground.chocks; }
        if (!act.metarSearch.empty()) { std::printf("event: metar %s\n", act.metarSearch.c_str()); s.metar = MetarLookup(); s.metar.icao = act.metarSearch; s.metar.raw = act.metarSearch + " 211554Z 25009KT 10SM FEW045 04/M02 A3010"; }
        if (act.media) std::printf("event: media %d\n", act.media);
        if (now > 600) break;   // (a test never needs longer)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    localserver::stop();
    return 0;
}
