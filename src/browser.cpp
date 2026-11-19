#include <algorithm>
#include "browser.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace xpt {
namespace browser {
namespace {

xptb::Shared* g_shm = nullptr;
uint32_t g_infoSeen = 0, g_frameTaken = 0;
bool g_started = false;
std::string g_err;
#if defined(_WIN32)
HANDLE g_map = nullptr;
PROCESS_INFORMATION g_proc = {};
#else
std::string g_shmName;
pid_t g_pid = 0;
#endif

long myPid() {
#if defined(_WIN32)
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

bool makeShared(std::string& err) {
    const size_t size = sizeof(xptb::Shared);
#if defined(_WIN32)
    const std::string name = std::string("Local\\") + xptb::shmName() + "_" + std::to_string(myPid());
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((unsigned long long)size >> 32), (DWORD)(size & 0xFFFFFFFFu), name.c_str());
    if (!g_map) { err = "cannot make shared memory (error " + std::to_string(GetLastError()) + ")"; return false; }
    void* p = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    g_shmName = std::string("/") + xptb::shmName() + "_" + std::to_string(myPid());
    shm_unlink(g_shmName.c_str());
    const int fd = shm_open(g_shmName.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) { if (fd >= 0) close(fd); err = "cannot make shared memory"; return false; }
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (!p) { err = "cannot map shared memory"; return false; }
    g_shm = new (p) xptb::Shared;   // atomics constructed in place; the pixel buffers are left as they are
    g_shm->magic = xptb::kMagic;
    g_shm->version = xptb::kVersion;
    g_shm->zoomAuto = 1;
    g_shm->cmdWrite.store(0); g_shm->cmdRead.store(0); g_shm->heartbeat.store(0); g_shm->ackSeq.store(0);
    g_shm->state.store(0); g_shm->error[0] = 0;
    g_shm->frameSeq.store(0); g_shm->infoSeq.store(0);
    g_shm->url[0] = 0; g_shm->title[0] = 0;
    g_shm->loading = g_shm->canBack = g_shm->canForward = g_shm->wantsKeyboard = 0;
    g_infoSeen = 0; g_frameTaken = 0;
    return true;
}

void dropShared() {
#if defined(_WIN32)
    if (g_shm) UnmapViewOfFile(g_shm);
    if (g_map) CloseHandle(g_map);
    g_map = nullptr;
#else
    if (g_shm) munmap(g_shm, sizeof(xptb::Shared));
    if (!g_shmName.empty()) shm_unlink(g_shmName.c_str());
#endif
    g_shm = nullptr;
}

} // namespace

bool start(const std::string& helper, const std::string& cefDir, const std::string& profile, std::string& err) {
    if (g_started) return true;
    if (!makeShared(err)) { g_err = err; return false; }
    const std::string pid = std::to_string(myPid());
#if defined(_WIN32)
    std::string cmd = "\"" + helper + "\" \"--xptb-cef-dir=" + cefDir + "\" \"--xptb-profile=" + profile + "\" --xptb-parent=" + pid;
    STARTUPINFOA si;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    std::memset(&g_proc, 0, sizeof g_proc);
    if (!CreateProcessA(helper.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, cefDir.c_str(), &si, &g_proc)) {
        err = "cannot start " + helper + " (error " + std::to_string(GetLastError()) + ")";
        g_err = err;
        dropShared();
        return false;
    }
#else
    const std::string a1 = "--xptb-cef-dir=" + cefDir, a2 = "--xptb-profile=" + profile, a3 = "--xptb-parent=" + pid;
    char* argv[] = {const_cast<char*>(helper.c_str()), const_cast<char*>(a1.c_str()), const_cast<char*>(a2.c_str()), const_cast<char*>(a3.c_str()), nullptr};
    if (posix_spawn(&g_pid, helper.c_str(), nullptr, nullptr, argv, environ) != 0) {
        err = "cannot start " + helper;
        g_err = err;
        dropShared();
        return false;
    }
#endif
    g_started = true;
    g_err.clear();
    return true;
}

void stop() {
    if (!g_started) return;
    push(xptb::CMD_QUIT);
    // a moment to close cleanly (cookies written), then it is made to
#if defined(_WIN32)
    if (WaitForSingleObject(g_proc.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(g_proc.hProcess, 0);
    CloseHandle(g_proc.hProcess);
    CloseHandle(g_proc.hThread);
    std::memset(&g_proc, 0, sizeof g_proc);
#else
    for (int i = 0; i < 60; ++i) {
        int st = 0;
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) { g_pid = 0; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_pid) { kill(g_pid, SIGKILL); waitpid(g_pid, nullptr, 0); g_pid = 0; }
#endif
    dropShared();
    g_started = false;
}

bool started() { return g_started; }

int state() {
    if (!g_started || !g_shm) return -2;
#if defined(_WIN32)
    if (g_proc.hProcess && WaitForSingleObject(g_proc.hProcess, 0) != WAIT_TIMEOUT && g_shm->state.load() != -1) {
        DWORD code = 0;
        GetExitCodeProcess(g_proc.hProcess, &code);
        std::snprintf(g_shm->error, sizeof g_shm->error, "the browser stopped (exit code %lu)", (unsigned long)code);
        g_shm->state.store(-1);
    }
#else
    if (g_pid && g_shm->state.load() != -1) {
        int st = 0;
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) {
            g_pid = 0;
            std::snprintf(g_shm->error, sizeof g_shm->error, "the browser stopped (status %d)", st);
            g_shm->state.store(-1);
        }
    }
#endif
    return g_shm->state.load();
}

std::string error() { return g_shm && g_shm->error[0] ? std::string(g_shm->error) : g_err; }

void push(int type, int a, int b, int c, int d, float f, const std::string& text) {
    if (!g_shm) return;
    const uint32_t w = g_shm->cmdWrite.load(std::memory_order_relaxed);
    if (w - g_shm->cmdRead.load(std::memory_order_acquire) >= (uint32_t)xptb::kCmds) {
        // full: the helper is not keeping up (or not there); mouse moves can be dropped, the rest waits a moment
        if (type == xptb::CMD_MOUSE_MOVE) return;
        for (int i = 0; i < 20 && w - g_shm->cmdRead.load(std::memory_order_acquire) >= (uint32_t)xptb::kCmds; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (w - g_shm->cmdRead.load(std::memory_order_acquire) >= (uint32_t)xptb::kCmds) return;
    }
    xptb::Cmd& cmd = g_shm->cmds[w % xptb::kCmds];
    cmd.type = type; cmd.a = a; cmd.b = b; cmd.c = c; cmd.d = d; cmd.f = f;
    std::snprintf(cmd.text, sizeof cmd.text, "%s", text.c_str());
    g_shm->cmdWrite.store(w + 1, std::memory_order_release);
}

void heartbeat() { if (g_shm) g_shm->heartbeat.fetch_add(1, std::memory_order_relaxed); }

bool frame(const uint8_t*& px, int& w, int& h) {
    if (!g_shm) return false;
    const uint32_t seq = g_shm->frameSeq.load(std::memory_order_acquire);
    if (seq == g_frameTaken) return false;
    const int idx = (int)(seq & 1u);
    px = g_shm->pixels[idx];
    w = g_shm->frameW[idx];
    h = g_shm->frameH[idx];
    g_frameTaken = seq;
    return w > 0 && h > 0;
}

void done() { if (g_shm) g_shm->ackSeq.store(g_frameTaken, std::memory_order_release); }

bool info(std::string& url, std::string& title, bool& loading, bool& canBack, bool& canForward, bool& wantsKeyboard, std::vector<TabInfo>& tabsOut, int& activeTab, int& zoomPct, bool& fullscreen, bool* zoomAuto) {
    if (!g_shm) return false;
    const uint32_t s = g_shm->infoSeq.load(std::memory_order_acquire);
    if (s == g_infoSeen) return false;
    g_infoSeen = s;
    url.assign(g_shm->url, strnlen(g_shm->url, sizeof g_shm->url));
    title.assign(g_shm->title, strnlen(g_shm->title, sizeof g_shm->title));
    loading = g_shm->loading != 0; canBack = g_shm->canBack != 0; canForward = g_shm->canForward != 0;
    wantsKeyboard = g_shm->wantsKeyboard != 0;
    tabsOut.clear();
    const int n = std::max(0, std::min((int)g_shm->tabCount, xptb::kMaxTabs));
    for (int i = 0; i < n; ++i) {
        const auto& t = g_shm->tabs[i];
        TabInfo ti;
        ti.id = t.id;
        ti.loading = t.loading != 0;
        ti.group = t.group;
        ti.title.assign(t.title, strnlen(t.title, sizeof t.title));
        ti.url.assign(t.url, strnlen(t.url, sizeof t.url));
        tabsOut.push_back(ti);
    }
    activeTab = g_shm->activeTab;
    zoomPct = g_shm->zoomPct > 0 ? g_shm->zoomPct : 100;
    if (zoomAuto) *zoomAuto = g_shm->zoomAuto != 0;
    fullscreen = g_shm->fullscreen != 0;
    return true;
}

} // namespace browser
} // namespace xpt
