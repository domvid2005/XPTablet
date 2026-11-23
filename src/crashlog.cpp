#include "crashlog.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <exception>
#include <stdexcept>
#endif

namespace xpt {
namespace crashlog {

#if defined(_WIN32)
namespace {
HANDLE g_file = INVALID_HANDLE_VALUE;
PVOID g_veh = nullptr;
volatile LONG g_told = 0;
HMODULE g_self = nullptr;
char g_path[1024] = "";

void out(const char* s) {
    if (g_file == INVALID_HANDLE_VALUE && g_path[0]) {
        g_file = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(g_file, s, (DWORD)std::strlen(s), &w, nullptr);
        FlushFileBuffers(g_file);
    }
}
void frame(DWORD64 pc) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    if (!base) base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    char b[512];
    std::snprintf(b, sizeof b, "  %s+0x%llx%s\r\n", base, (unsigned long long)(m ? pc - (DWORD64)m : pc), m && m == g_self ? "   <- XPTablet" : "");
    out(b);
}
// the call stack from a context, by the unwind tables (x64): each frame's place, at most 48 of them
void walkCtx(CONTEXT* c) {
#if defined(_M_X64)
    CONTEXT ctx = *c;
    if (!ctx.Rip && !IsBadReadPtr((const void*)ctx.Rsp, 8)) {   // a call to address 0 (a function pointer that was empty): the caller's
        out("  called from:\r\n");                               // return address is on top of the stack: the walk goes on from there
        ctx.Rip = *(DWORD64*)ctx.Rsp;
        ctx.Rsp += 8;
    }
    for (int i = 0; i < 48 && ctx.Rip; ++i) {
        frame(ctx.Rip);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!f) {   // a leaf function: its return address on top of the stack
            if (IsBadReadPtr((const void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            PVOID hd = nullptr;
            DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, f, &ctx, &hd, &ef, nullptr);
        }
    }
#else
    (void)c;
#endif
}
void walk(CONTEXT* c) {
    __try { walkCtx(c); } __except (EXCEPTION_EXECUTE_HANDLER) { out("  (the stack could not be read further)\r\n"); }
}
void walkHere() {
    CONTEXT c;
    RtlCaptureContext(&c);
    walk(&c);
}
void header(const char* what) {
    char b[512];
    const std::time_t t = std::time(nullptr);
    char ts[64] = "";
    std::tm lt;
    if (localtime_s(&lt, &t) == 0) std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &lt);
    std::snprintf(b, sizeof b, "\r\n==== %s, thread %lu, %s ====\r\n", what, (unsigned long)GetCurrentThreadId(), ts);
    out(b);
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_IN_PAGE_ERROR:
    case 0xC0000374:   // heap corruption
        break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    // (only faults in XPTablet's code, or with XPTablet's code on the stack, are worth the record; others are handled by their own)
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &m);
    if (InterlockedIncrement(&g_told) > 8) return EXCEPTION_CONTINUE_SEARCH;
    char b[512];
    std::snprintf(b, sizeof b, "exception 0x%08lx at %p%s", (unsigned long)code, ep->ExceptionRecord->ExceptionAddress, m == g_self ? " (in XPTablet)" : "");
    header(b);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        std::snprintf(b, sizeof b, "  %s address 0x%llx\r\n", ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "executing",
                      (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        out(b);
    }
    walk(ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}
void onTerminate() {
    header("std::terminate (an exception nobody caught, or a thread replaced while running)");
    try {
        if (std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& e) {
        char b[600];
        std::snprintf(b, sizeof b, "  exception: %s\r\n", e.what());
        out(b);
    } catch (...) { out("  exception: (not a std::exception)\r\n"); }
    walkHere();
    std::abort();
}
void onAbort(int) { header("abort"); walkHere(); }
void onPurecall() { header("pure virtual function call"); walkHere(); std::abort(); }
void onBadParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    // a C runtime call given a bad argument: told, and the call returns its error rather than ending the process
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) > 4) return;
    header("C runtime call with an invalid parameter (survived)");
    walkHere();
}
}   // namespace

void install(const std::string& dir) {
    std::snprintf(g_path, sizeof g_path, "%scrash.txt", dir.c_str());
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&install, &g_self);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, veh);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    _set_purecall_handler(onPurecall);
    _set_invalid_parameter_handler(onBadParameter);
}
void uninstall() {
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}
#else
void install(const std::string&) {}
void uninstall() {}
#endif

namespace {
std::mutex g_failMx;
std::string g_failures;
}
void threadFailed(const char* name, const char* what) {
    char b[700];
    std::snprintf(b, sizeof b, "the %s thread stopped on an exception: %s", name ? name : "?", what ? what : "?");
#if defined(_WIN32)
    header(b);
    walkHere();
#endif
    std::lock_guard<std::mutex> lk(g_failMx);
    g_failures += b;
    g_failures += '\n';
}
std::string takeFailures() {
    std::lock_guard<std::mutex> lk(g_failMx);
    std::string s;
    s.swap(g_failures);
    return s;
}
std::string takeLast(const std::string& dir) {
    const std::string p = dir + "crash.txt";
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    std::remove((dir + "crash-last.txt").c_str());
    std::rename(p.c_str(), (dir + "crash-last.txt").c_str());
    return ss.str();
}

}   // namespace crashlog
}   // namespace xpt
