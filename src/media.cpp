#include "guard.h"
#include "media.h"
#include "media_platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <thread>

#include "http.h"
#include "stb_image.h"

namespace xpt {
namespace media {
namespace {

std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mx;
Now g_now;
std::vector<uint8_t> g_art;
int g_artW = 0, g_artH = 0;
std::vector<int> g_cmds;
double g_seekTo = -1;   // a seek waiting (seconds)
float g_setPlayer = -1, g_setMaster = -1;   // volumes waiting to be set
int g_setPMute = -1, g_setMMute = -1;       // mutes waiting to be set (1 muted, 0 not)
std::chrono::steady_clock::time_point g_posAt;   // when g_now.posSec was read
int g_duckMode = 0;
float g_duckLevel = 0.5f;   // lowered: to this volume
bool g_radioRx = false;
std::string g_radioName;

// the cover: decoded, made at most 512 pixels across (a box filter), RGBA
bool decodeCover(const std::string& bytes, std::vector<uint8_t>& out, int& w, int& h) {
    int iw = 0, ih = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &n, 4);
    if (!px) return false;
    int k = 1;
    while (iw / k > 1024 || ih / k > 1024) ++k;   // (up to 1024: sharp on a large, high-density tablet)
    w = iw / k; h = ih / k;
    out.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned acc[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    const unsigned char* p = px + ((size_t)(y * k + dy) * iw + (x * k + dx)) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                }
            for (int c = 0; c < 4; ++c) out[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / (k * k));
        }
    stbi_image_free(px);
    return true;
}
// a cover given as an address (Linux players): a file, or downloaded
bool fetchCover(const std::string& url, std::string& bytes) {
    if (url.compare(0, 7, "file://") == 0) {
        std::string path = url.substr(7);
        std::string dec;   // %20 and such
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size()) { dec += (char)std::strtol(path.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
            else dec += path[i];
        }
        FILE* f = std::fopen(dec.c_str(), "rb");
        if (!f) return false;
        char buf[65536];
        size_t n;
        bytes.clear();
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() < (16u << 20)) bytes.append(buf, n);
        std::fclose(f);
        return !bytes.empty();
    }
    if (url.compare(0, 4, "http") == 0) {
        long code = 0;
        std::string err;
        return wx::httpGet(url, bytes, code, err, 10, 8u << 20) && code == 200;
    }
    return false;
}

std::string friendlyApp(std::string a) {
    // "Spotify.exe", "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", "MSEdge", "chromium", "org.mpris.MediaPlayer2.spotify" -> a name
    const size_t bang = a.rfind('!');
    if (bang != std::string::npos) a = a.substr(bang + 1);
    if (a.size() > 4 && (a.compare(a.size() - 4, 4, ".exe") == 0 || a.compare(a.size() - 4, 4, ".EXE") == 0)) a.resize(a.size() - 4);
    const size_t slash = a.find_last_of("\\/");
    if (slash != std::string::npos) a = a.substr(slash + 1);
    std::string low = a;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low == "msedge") return "Microsoft Edge";
    if (low == "chrome") return "Chrome";
    if (low == "firefox") return "Firefox";
    if (low == "spotify") return "Spotify";
    if (!a.empty()) a[0] = (char)std::toupper((unsigned char)a[0]);
    return a;
}

void run() {
    const bool ok = platform::init();
    { std::lock_guard<std::mutex> lk(g_mx); g_now.supported = ok; }
    if (!ok) { while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(200)); return; }
    std::string lastArtKey;
    int artVersion = 0;
    bool ducked = false, pausedByUs = false, lowered = false;
    int duckedMode = 0;
    float duckedLevel = 1.0f;   // the volume it was lowered to
    auto lastHeard = std::chrono::steady_clock::now() - std::chrono::hours(1);
    while (!g_quit) {
        platform::Info info;
        std::string artBytes;
        const bool active = platform::poll(info, artBytes);
        std::string who;
        const bool apps = platform::radioApps(who);
        std::vector<int> cmds;
        double seekTo = -1;
        float setPlayer = -1, setMaster = -1;
        int mode;
        int setPMute = -1, setMMute = -1;
        float level = 0.5f;
        bool rx;
        std::string rxName;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            cmds.swap(g_cmds);
            seekTo = g_seekTo; g_seekTo = -1;
            setPlayer = g_setPlayer; g_setPlayer = -1; setMaster = g_setMaster; g_setMaster = -1;
            setPMute = g_setPMute; g_setPMute = -1; setMMute = g_setMMute; g_setMMute = -1;
            mode = g_duckMode; rx = g_radioRx; rxName = g_radioName; level = g_duckLevel;
        }
        for (int c : cmds) platform::send(c);
        if (seekTo >= 0) { platform::seek(seekTo); info.posSec = seekTo; }
        if (setPlayer >= 0) platform::setPlayerVol(setPlayer);
        if (setMaster >= 0) platform::setMasterVol(setMaster);
        if (setPMute >= 0) platform::setPlayerMute(setPMute != 0);
        if (setMMute >= 0) platform::setMasterMute(setMMute != 0);
        // the volumes: read a few times a second (and at once after one was set)
        static int volTick = 0;
        float pv = -1, mv = -1;
        const bool readVol = (++volTick & 1) == 0 || setPlayer >= 0 || setMaster >= 0 || setPMute >= 0 || setMMute >= 0;
        bool pm = false, mm = false;
        if (readVol) { if (!active || !platform::playerVol(pv)) pv = -1; if (!platform::masterVol(mv)) mv = -1;
                       if (!active || !platform::playerMute(pm)) pm = false; if (!platform::masterMute(mm)) mm = false; }
        // the cover: when the track (or its picture) changes
        const std::string artKey = info.title + "\x01" + info.artist + "\x01" + (artBytes.empty() ? info.artUrl : std::to_string(artBytes.size()));
        if (active && artKey != lastArtKey) {
            lastArtKey = artKey;
            std::string bytes = artBytes;
            if (bytes.empty() && !info.artUrl.empty()) fetchCover(info.artUrl, bytes);
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (!bytes.empty() && decodeCover(bytes, rgba, w, h)) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_art.swap(rgba); g_artW = w; g_artH = h; g_now.artVersion = ++artVersion;
            } else {
                std::lock_guard<std::mutex> lk(g_mx);
                g_art.clear(); g_artW = g_artH = 0; g_now.artVersion = ++artVersion;
            }
        }
        // the radio: heard now, or within the last 1.2 s (so a pause between words does not bring the music back)
        const auto t = std::chrono::steady_clock::now();
        if (rx || apps) lastHeard = t;
        const bool radio = t - lastHeard < std::chrono::milliseconds(1200);
        // undo what was done when the rule changes, or the radio goes quiet
        if (ducked && (!radio || mode != duckedMode)) {
            if (lowered) platform::setVolume(1.0f);
            if (pausedByUs) platform::send(4);
            ducked = lowered = pausedByUs = false;
        }
        if (ducked && lowered && std::fabs(level - duckedLevel) > 0.01f) {   // receiving turned to transmitting (or back): its own level
            platform::setVolume(level); duckedLevel = level;
        }
        if (!ducked && radio && active && mode != 0) {
            if (mode == 1) { lowered = platform::setVolume(level); duckedLevel = level; }
            else if (mode == 2 && info.playing) { platform::send(5); pausedByUs = true; }
            ducked = lowered || pausedByUs;
            duckedMode = mode;
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_now.active = active;
            g_now.app = friendlyApp(info.app);
            g_now.title = info.title; g_now.artist = info.artist; g_now.album = info.album;
            g_now.playing = info.playing; g_now.canPrev = info.canPrev; g_now.canNext = info.canNext;
            g_now.posSec = info.posSec; g_now.lenSec = info.lenSec; g_now.canSeek = info.canSeek && info.lenSec > 0;
            g_posAt = std::chrono::steady_clock::now();
            g_now.radioApp = apps; g_now.radioAppName = apps ? who : rxName;
            g_now.ducked = ducked;
            if (readVol) { g_now.playerVolume = pv; g_now.masterVolume = mv; g_now.playerMuted = pm; g_now.masterMuted = mm; }
        }
        for (int i = 0; i < 5 && !g_quit; ++i) {   // a quarter second, sooner for a command
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_cmds.empty()) break;
        }
    }
    if (ducked) { if (lowered) platform::setVolume(1.0f); if (pausedByUs) platform::send(4); }
    platform::shutdown();
}

} // namespace

void start() {
    if (g_thread.joinable()) return;
    g_quit = false;
    g_thread = std::thread(guarded("media", [] { run(); }));
}
void stop() {
    if (!g_thread.joinable()) return;
    g_quit = true;
    g_thread.join();
    std::lock_guard<std::mutex> lk(g_mx);
    g_now = Now();
    g_art.clear();
}
Now now() {
    std::lock_guard<std::mutex> lk(g_mx);
    Now n = g_now;
    if (n.playing && n.posSec >= 0) {   // carried on from when it was read, so the bar moves smoothly
        n.posSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - g_posAt).count();
        if (n.lenSec > 0) n.posSec = std::min(n.posSec, n.lenSec);
    }
    return n;
}
void seek(double sec) { std::lock_guard<std::mutex> lk(g_mx); g_seekTo = std::max(0.0, sec); }
void setPlayerVolume(float v) { std::lock_guard<std::mutex> lk(g_mx); g_setPlayer = std::max(0.0f, std::min(1.0f, v)); g_now.playerVolume = g_setPlayer; }
void setMuted(bool computer, bool muted) {
    std::lock_guard<std::mutex> lk(g_mx);
    (computer ? g_setMMute : g_setPMute) = muted ? 1 : 0;
    (computer ? g_now.masterMuted : g_now.playerMuted) = muted;
}
void setMasterVolume(float v) { std::lock_guard<std::mutex> lk(g_mx); g_setMaster = std::max(0.0f, std::min(1.0f, v)); g_now.masterVolume = g_setMaster; }
bool takeArt(int& version, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_now.artVersion == version) return false;
    version = g_now.artVersion;
    rgba = g_art; w = g_artW; h = g_artH;
    return true;
}
void command(int what) { std::lock_guard<std::mutex> lk(g_mx); g_cmds.push_back(what); }
void setDuck(int mode, bool radioRx, const std::string& name, float level) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_duckMode = mode; g_radioRx = radioRx; g_radioName = name; g_duckLevel = std::max(0.0f, std::min(1.0f, level));
}

} // namespace media
} // namespace xpt
