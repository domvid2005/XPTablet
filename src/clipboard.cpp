#include "clipboard.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace xpt {
namespace clipboard {

#if defined(_WIN32)
bool hasText() { return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0; }
bool get(std::string& text) {
    text.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) { text.resize((size_t)n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &text[0], n, nullptr, nullptr); ok = true; }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}
bool set(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 0 || !OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t))) {
        if (wchar_t* w = (wchar_t*)GlobalLock(g)) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w, n);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        }
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
}
#else
// macOS: pbpaste and pbcopy; Linux: wl-paste / xclip / xsel, whichever there is
namespace {
bool readCmd(const char* cmd, std::string& out) {
    out.clear();
    FILE* p = popen(cmd, "r");
    if (!p) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0 && out.size() < (1u << 20)) out.append(buf, n);
    return pclose(p) == 0;
}
bool writeCmd(const char* cmd, const std::string& in) {
    FILE* p = popen(cmd, "w");
    if (!p) return false;
    std::fwrite(in.data(), 1, in.size(), p);
    return pclose(p) == 0;
}
}   // namespace
#if defined(__APPLE__)
bool get(std::string& text) { return readCmd("pbpaste 2>/dev/null", text) && !text.empty(); }
bool set(const std::string& text) { return writeCmd("pbcopy 2>/dev/null", text); }
#else
bool get(std::string& text) {
    return (readCmd("wl-paste -n 2>/dev/null", text) || readCmd("xclip -selection clipboard -o 2>/dev/null", text) ||
            readCmd("xsel -b -o 2>/dev/null", text)) && !text.empty();
}
bool set(const std::string& text) {
    return writeCmd("wl-copy 2>/dev/null", text) || writeCmd("xclip -selection clipboard 2>/dev/null", text) || writeCmd("xsel -b -i 2>/dev/null", text);
}
#endif
bool hasText() { std::string t; return get(t); }
#endif

}   // namespace clipboard
}   // namespace xpt
