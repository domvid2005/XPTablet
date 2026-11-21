// Local Connect's mirror (see localmirror.h).
#include "localmirror.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "localserver.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace xpt {
namespace mirror {
namespace {

constexpr int kBlock = 16;          // changes are found in 16 x 16 pixel blocks (JPEG's own block size, doubled)
constexpr int kQuality = 91;        // (above 90 the encoder keeps colour at full detail: small coloured text stays clean)
constexpr double kMinGap = 1.0 / 15, kMinGapFast = 1.0 / 24;
constexpr double kHealEvery = 20;   // a whole picture this often, whatever changed (seconds)

struct Job { std::vector<unsigned char> px; int w = 0, h = 0; bool bottomUp = false; };

std::mutex g_mx;
std::condition_variable g_cv;
std::thread g_thread;
bool g_run = false, g_have = false;
std::atomic<bool> g_busy{false};
Job g_job;
double g_lastAsk = -1e9;

void jpgOut(void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, (size_t)size); }
void put16(std::string& o, int v) { o.push_back((char)(v & 255)); o.push_back((char)((v >> 8) & 255)); }

// 'F', the whole picture's size, the piece's place and size, 1 if it is a whole picture, then the piece as a JPEG
std::string frameMessage(const std::vector<unsigned char>& rgb, int W, int H, int x, int y, int w, int h, bool key) {
    std::string m = "F";
    put16(m, W); put16(m, H); put16(m, x); put16(m, y); put16(m, w); put16(m, h);
    m.push_back((char)(key ? 1 : 0));
    if (x == 0 && w == W) stbi_write_jpg_to_func(jpgOut, &m, w, h, 3, rgb.data() + (size_t)y * W * 3, kQuality);
    else {
        std::vector<unsigned char> part((size_t)w * h * 3);
        for (int r = 0; r < h; ++r) std::memcpy(part.data() + (size_t)r * w * 3, rgb.data() + ((size_t)(y + r) * W + x) * 3, (size_t)w * 3);
        stbi_write_jpg_to_func(jpgOut, &m, w, h, 3, part.data(), kQuality);
    }
    return m;
}

void work() {
    std::vector<unsigned char> prev, cur;
    int pw = 0, ph = 0;
    auto lastKey = std::chrono::steady_clock::now() - std::chrono::hours(1);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return !g_run || g_have; });
            if (!g_run) return;
            job = std::move(g_job);
            g_have = false;
        }
        const int W = job.w, H = job.h;
        if (W < 16 || H < 16 || W > 4096 || H > 4096 || job.px.size() < (size_t)W * H * 4) { g_busy.store(false); continue; }
        cur.resize((size_t)W * H * 3);
        for (int y = 0; y < H; ++y) {   // to 3 bytes a pixel, top row first
            const unsigned char* s = job.px.data() + (size_t)(job.bottomUp ? H - 1 - y : y) * W * 4;
            unsigned char* d = cur.data() + (size_t)y * W * 3;
            for (int x = 0; x < W; ++x) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
        }
        const auto now = std::chrono::steady_clock::now();
        bool key = W != pw || H != ph || localserver::wantKey() || std::chrono::duration<double>(now - lastKey).count() > kHealEvery;
        int x0 = W, y0 = H, x1 = 0, y1 = 0;
        if (!key) {   // the box around every block that changed
            for (int by = 0; by < H; by += kBlock) {
                const int bh = std::min(kBlock, H - by);
                for (int bx = 0; bx < W; bx += kBlock) {
                    const int bw = std::min(kBlock, W - bx);
                    bool same = true;
                    for (int r = 0; r < bh && same; ++r) same = std::memcmp(cur.data() + ((size_t)(by + r) * W + bx) * 3, prev.data() + ((size_t)(by + r) * W + bx) * 3, (size_t)bw * 3) == 0;
                    if (!same) { x0 = std::min(x0, bx); y0 = std::min(y0, by); x1 = std::max(x1, bx + bw); y1 = std::max(y1, by + bh); }
                }
            }
            if (x1 > x0 && (long long)(x1 - x0) * (y1 - y0) > (long long)W * H * 7 / 10) key = true;   // (nearly all of it: a whole picture)
        }
        if (key) { localserver::publishFrame(frameMessage(cur, W, H, 0, 0, W, H, true), true); lastKey = now; }
        else if (x1 > x0) localserver::publishFrame(frameMessage(cur, W, H, x0, y0, x1 - x0, y1 - y0, false), false);
        prev.swap(cur);
        pw = W; ph = H;
        g_busy.store(false);
    }
}

}   // namespace

void start() {
    stop();
    { std::lock_guard<std::mutex> lk(g_mx); g_run = true; g_have = false; }
    g_busy.store(false);
    g_thread = std::thread(work);
}

void stop() {
    { std::lock_guard<std::mutex> lk(g_mx); g_run = false; g_have = false; }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_busy.store(false);
}

bool wantsFrame(double now, bool fast) {
    if (g_busy.load() || now - g_lastAsk < (fast ? kMinGapFast : kMinGap)) return false;
    if (localserver::clients() <= 0) return false;
    g_lastAsk = now;
    return true;
}

void submit(std::vector<unsigned char>&& rgbx, int w, int h, bool bottomUp) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_run) return;
        g_job.px = std::move(rgbx); g_job.w = w; g_job.h = h; g_job.bottomUp = bottomUp;
        g_have = true;
    }
    g_busy.store(true);
    g_cv.notify_one();
}

bool nextInput(Input& in) {
    localserver::Incoming m;
    while (localserver::poll(m)) {
        if (m.kind != 'I' || m.bytes.empty()) continue;
        const unsigned char* p = (const unsigned char*)m.bytes.data();
        const size_t n = m.bytes.size();
        const int kind = p[0] & 0x0F;
        in = Input();
        in.kind = kind;
        in.touch = (p[0] & 0x10) != 0;
        if (kind == Input::MOVE || kind == Input::DOWN || kind == Input::UP) {
            if (n < 5) continue;
            in.x = (p[1] | (p[2] << 8)) / 65535.0f; in.y = (p[3] | (p[4] << 8)) / 65535.0f;
        } else if (kind == Input::WHEEL) {
            if (n < 2) continue;
            in.code = p[1] == 1 ? 1 : -1;
        } else if (kind == Input::KEY || kind == Input::CHAR) {
            if (n < 3) continue;
            in.code = p[1] | (p[2] << 8);
            if (kind == Input::KEY ? (in.code != 8 && in.code != 13) : (in.code < 32 || in.code > 126)) continue;
        } else if (kind == Input::GAME_KEY) {
            if (n < 3 || p[1] > 4) continue;
            in.code = p[1]; in.down = p[2] != 0;
        } else if (kind != Input::CANCEL) continue;
        return true;
    }
    return false;
}

}   // namespace mirror
}   // namespace xpt
