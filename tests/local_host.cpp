// A stand-in for the plugin, for Local Connect's end-to-end test: the real tablet (its own code, a sample flight), the real server
// and the real mirror, on this machine's own address. Where the plugin takes the tablet's picture with OpenGL, this draws it with
// the tests' renderer. It prints the pairing code (as the sim would show it) and which page the tablet is on.
//   local_host <data-dir> [port]
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "localmirror.h"
#include "localpage.h"
#include "localserver.h"
#include "soft_raster.h"
#include "tabletui.h"
#include "tabrender.h"

using namespace xpt;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: local_host <data-dir> [port]\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const float scale = 1.5f;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyStyle();
    Fonts fonts = loadFonts(scale);
    fonts.wxLogo = (ImTextureID)(intptr_t)2; fonts.logo = (ImTextureID)(intptr_t)3; fonts.logoMask = (ImTextureID)(intptr_t)5; fonts.icons = (ImTextureID)(intptr_t)4;
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex, &tw, &th);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);

    localserver::Config cfg;
    cfg.bindIp = "127.0.0.1"; cfg.allowLoopback = true; cfg.port = argc > 2 ? std::atoi(argv[2]) : 18543;
    cfg.dataDir = argv[1]; cfg.wireVersion = mirror::kPageVersion;
    for (const char* piece : kLocalPage) cfg.page += piece;
    localserver::start(cfg);
    mirror::start();

    SimState s;
    s.acfIcao = "A321"; s.zuluSec = 14 * 3600; s.batteryPct = 88; s.fps = 45; s.engineType = 5; s.toliss = true; s.fuelKg = 6200; s.pixelScale = scale; s.panelPowered = true;
    s.map.havePos = true; s.map.lat = 41.7087; s.map.lon = -86.3173; s.map.hdgTrue = 270; s.map.altFt = 799; s.map.onGround = true; s.map.utc = 1795262400;
    s.ground.onGround = true; s.ground.toliss = true; s.ground.paxDoors = 4; s.ground.cargoDoors = 2; s.ground.parkBrake = true;
    s.wx.valid = true; s.wx.windDir = 250; s.wx.windKt = 9; s.wx.visSm = 10; s.wx.tempC = 4; s.wx.dewC = -2; s.wx.qnhHpa = 1019;
    s.dataInfo.push_back({"Running in", "Local Connect's test host"});
    const float W = 800, H = kLogicalH;
    std::string lastCode;
    bool toldOn = false;
    int devices = -1, app = -1, inputs = 0;
    std::vector<Batch> batches;
    std::atomic<bool> drawing{false};
    std::thread painter;
    const auto t0 = std::chrono::steady_clock::now();
    double last = 0;
    for (;;) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const localserver::Status st = localserver::status();
        if (st.state == -1) { std::printf("error: %s\n", st.error.c_str()); return 1; }
        if (st.state == 2 && !toldOn) { toldOn = true; std::printf("address: %s\n", st.address.c_str()); }
        if (st.pairCode != lastCode) { lastCode = st.pairCode; if (!lastCode.empty()) std::printf("pair: %s %s\n", lastCode.c_str(), st.pairName.c_str()); }
        if (localserver::clients() != devices) { devices = localserver::clients(); std::printf("devices: %d\n", devices); }
        // a device's touches and keys, to the tablet (as the plugin feeds them)
        mirror::Input in;
        while (mirror::nextInput(in)) {
            ++inputs; if (std::getenv("XPT_LC_TRACE")) std::printf("in: %d %.3f %.3f %d\n", in.kind, in.x, in.y, (int)in.touch);
            const float x = in.x * W, y = in.y * H;
            if (in.kind == mirror::Input::MOVE) queuePointer(x, y, -1);
            else if (in.kind == mirror::Input::DOWN) { setTouchInput(in.touch); queuePointer(x, y, 1); }
            else if (in.kind == mirror::Input::UP) { queuePointer(x, y, 0); if (in.touch) queuePointer(-1000, -1000, -1); }
            else if (in.kind == mirror::Input::CANCEL) queuePointer(-1000, -1000, 0);
            else if (in.kind == mirror::Input::WHEEL) io.AddMouseWheelEvent(0, (float)in.code);
            else if (in.kind == mirror::Input::GAME_KEY) { if (currentApp() == APP_GAMES && !tabletTyping()) gameKeyEvent(in.code, in.down); }
            else if (tabletTyping()) tabletKey(in.code, in.kind == mirror::Input::CHAR);
        }
        // the tablet's frame
        s.zuluSec = 14 * 3600 + (int)now;
        io.DisplaySize = ImVec2(W, H);
        io.DeltaTime = (float)std::max(1e-4, std::min(0.5, now - last));
        last = now;
        pumpPointer();   // (a step of the pointer a frame: where it is first, the press on the next)
        ImGui::NewFrame();
        Actions act;
        build(fonts, s, W, H, act);
        ImGui::Render();
        if (currentApp() != app) { app = currentApp(); std::printf("app: %d\n", app); }
        if (!act.metarSearch.empty()) std::printf("metar: %s\n", act.metarSearch.c_str());
        // its picture, when the mirror wants one
        // (drawn on a thread of its own: the tests' renderer takes a fifth of a second, and the tablet must keep its frame rate)
        if (!drawing.load() && mirror::wantsFrame(now, arcadeOnScreen())) {
            if (painter.joinable()) painter.join();
            batches.clear();
            clipDrawData(ImGui::GetDrawData(), batches);
            drawing.store(true);
            painter = std::thread([&, frame = batches]() {
                xpt_test::Canvas cv((int)(W * scale), (int)(H * scale));
                xpt_test::raster(cv, frame, tex, tw, th, scale);
                std::vector<unsigned char> px((size_t)cv.w * cv.h * 4, 255);
                for (size_t i = 0, n = (size_t)cv.w * cv.h; i < n; ++i) for (int k = 0; k < 3; ++k) px[i * 4 + k] = (unsigned char)std::max(0.0f, std::min(255.0f, cv.rgb[i * 3 + k] * 255.0f + 0.5f));
                mirror::submit(std::move(px), cv.w, cv.h, false);
                drawing.store(false);
            });
        }
        if (now > 600) break;   // (a test never needs longer)
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (painter.joinable()) painter.join();
    mirror::stop();
    localserver::stop();
    return 0;
}
