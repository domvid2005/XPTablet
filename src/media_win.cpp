// Windows: the system's media transport controls (what Spotify, browsers and other players tell Windows: title, artist, cover,
// play / pause / skip), and the audio mixer's sessions: the player's own volume (lowered to 50 % for the radio) and whether a VATSIM
// voice app is making sound (its level meter).
#include "media_platform.h"

#include <chrono>
#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propidl.h>   // (PropVariantClear: the outputs' names)
#ifndef STGM_READ
#define STGM_READ 0x00000000L
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "ole32")

namespace xpt {
namespace media {
namespace platform {
namespace {

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

GlobalSystemMediaTransportControlsSessionManager g_mgr{nullptr};
std::wstring g_sessionApp;          // the shown session's app (its AppUserModelId: "Spotify.exe", "MSEdge", ...)
std::wstring g_lastTrack;           // title + artist the cover was read for
std::string g_trackArt;             // that track's cover, handed over on every poll (so it stays, rather than showing once)
int g_artTries = 0;                 // asked for it this many times (apps often give it a moment after the title)
std::map<DWORD, float> g_baseVol;   // a lowered process's own volume before
float g_factor = 1.0f;              // what it is lowered by now (1: not lowered)

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring lower(std::wstring s) { for (wchar_t& c : s) c = (wchar_t)std::towlower(c); return s; }

GlobalSystemMediaTransportControlsSession current() {
    if (!g_mgr) return nullptr;
    try { return g_mgr.GetCurrentSession(); } catch (...) { return nullptr; }
}

// the output the computer volume controls: chosen in Settings, Media (its endpoint ID), else Windows' default
std::wstring g_outputId;
std::wstring wideOf(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string utf8Of(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
IMMDevice* outputDevice(IMMDeviceEnumerator* en) {   // (released by the caller)
    IMMDevice* dev = nullptr;
    if (!g_outputId.empty() && SUCCEEDED(en->GetDevice(g_outputId.c_str(), &dev)) && dev) {
        DWORD state = 0;
        if (SUCCEEDED(dev->GetState(&state)) && state == DEVICE_STATE_ACTIVE) return dev;
        dev->Release(); dev = nullptr;   // (unplugged or switched off: the default instead)
    }
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) dev = nullptr;
    return dev;
}

// the processes with audio sessions on any of the computer's outputs (a player or a radio app may be sent to a headset, or to one
// of Voicemeeter's inputs, rather than to Windows' default; 2026-11-27): their exe name, volume control and level meter
struct Session { DWORD pid = 0; std::wstring exe; ISimpleAudioVolume* vol = nullptr; IAudioMeterInformation* meter = nullptr; };
void sessionsOf(IMMDevice* dev, std::vector<Session>& out);
std::vector<Session> sessions() {
    std::vector<Session> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) return out;
    IMMDeviceCollection* all = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && all) {
        UINT n = 0;
        all->GetCount(&n);
        for (UINT i = 0; i < n && i < 32; ++i) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(all->Item(i, &dev)) && dev) { sessionsOf(dev, out); dev->Release(); }
        }
        all->Release();
    }
    en->Release();
    return out;
}
void sessionsOf(IMMDevice* dev, std::vector<Session>& out) {
    {
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
            IAudioSessionEnumerator* list = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&list)) && list) {
                int n = 0;
                list->GetCount(&n);
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* c = nullptr;
                    if (FAILED(list->GetSession(i, &c)) || !c) continue;
                    IAudioSessionControl2* c2 = nullptr;
                    if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)) && c2) {
                        Session s;
                        c2->GetProcessId(&s.pid);
                        if (s.pid) {
                            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.pid);
                            if (h) {
                                wchar_t path[MAX_PATH];
                                DWORD len = MAX_PATH;
                                if (QueryFullProcessImageNameW(h, 0, path, &len)) {
                                    std::wstring p(path, len);
                                    const size_t sl = p.find_last_of(L"\\/");
                                    s.exe = lower(sl == std::wstring::npos ? p : p.substr(sl + 1));
                                    if (s.exe.size() > 4 && s.exe.compare(s.exe.size() - 4, 4, L".exe") == 0) s.exe.resize(s.exe.size() - 4);
                                }
                                CloseHandle(h);
                            }
                            c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&s.vol);
                            c->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&s.meter);
                            out.push_back(s);
                        }
                        c2->Release();
                    }
                    c->Release();
                }
                list->Release();
            }
            mgr->Release();
        }
    }
}
void release(std::vector<Session>& v) {
    for (Session& s : v) { if (s.vol) s.vol->Release(); if (s.meter) s.meter->Release(); }
    v.clear();
}
// does this process play the shown media? ("Spotify.exe" / "SpotifyAB.SpotifyMusic_...!Spotify" -> spotify, "MSEdge" -> msedge)
bool isPlayer(const Session& s) {
    if (s.exe.empty() || g_sessionApp.empty()) return false;
    const std::wstring app = lower(g_sessionApp);
    return app.find(s.exe) != std::wstring::npos;
}
// VATSIM (and other) radio voice apps
bool isRadioApp(const std::wstring& exe, std::string& name) {
    static const std::pair<const wchar_t*, const char*> kApps[] = {
        {L"trackaudio", "TrackAudio"}, {L"xpilot", "xPilot"}, {L"swiftguistd", "swift"}, {L"swiftcore", "swift"}, {L"swiftdata", "swift"},
        {L"afv", "Audio for VATSIM"}, {L"audioforvatsim", "Audio for VATSIM"}, {L"vpilot", "vPilot"}, {L"pilot2atc", "Pilot2ATC"},
        {L"sayintentions", "SayIntentions"}, {L"beyondatc", "BeyondATC"}};
    for (const auto& a : kApps) if (exe.find(a.first) != std::wstring::npos) { name = a.second; return true; }
    return false;
}

} // namespace

bool init() {
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (...) {}
    try {
        g_mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (...) { g_mgr = nullptr; }
    return g_mgr != nullptr;
}
void shutdown() {
    g_mgr = nullptr;
    try { uninit_apartment(); } catch (...) {}
}

bool poll(Info& info, std::string& artBytes) {
    artBytes.clear();
    auto s = current();
    if (!s) { g_sessionApp.clear(); return false; }
    try {
        g_sessionApp = std::wstring(s.SourceAppUserModelId());
        info.app = narrow(g_sessionApp);
        const auto pb = s.GetPlaybackInfo();
        info.playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        const auto c = pb.Controls();
        info.canPrev = c.IsPreviousEnabled();
        info.canNext = c.IsNextEnabled();
        info.canSeek = c.IsPlaybackPositionEnabled();
        {   // where the track is: the position as of its last update, carried on to now while it plays
            const auto tl = s.GetTimelineProperties();
            const double len = std::chrono::duration<double>(tl.EndTime() - tl.StartTime()).count();
            double pos = std::chrono::duration<double>(tl.Position() - tl.StartTime()).count();
            if (info.playing) {
                const auto since = winrt::clock::now() - tl.LastUpdatedTime();
                const double s2 = std::chrono::duration<double>(since).count();
                if (s2 > 0 && s2 < 3600) pos += s2;
            }
            info.lenSec = len > 0 ? len : -1;
            info.posSec = len > 0 ? std::min(std::max(0.0, pos), len) : -1;
        }
        const auto props = s.TryGetMediaPropertiesAsync().get();
        info.title = narrow(std::wstring(props.Title()));
        info.artist = narrow(std::wstring(props.Artist()));
        info.album = narrow(std::wstring(props.AlbumTitle()));
        // the cover: read per track (it is given as a stream), kept, and asked again until the app gives it
        const std::wstring track = std::wstring(props.Title()) + L"\x01" + std::wstring(props.Artist());
        // apps often give the picture a moment after the title: asked again (up to ~8 tries) until it comes
        if (track != g_lastTrack) { g_lastTrack = track; g_artTries = 0; g_trackArt.clear(); }
        if (g_trackArt.empty() && g_artTries < 8) {
            ++g_artTries;
            const auto thumb = props.Thumbnail();
            if (thumb) {
                const auto stream = thumb.OpenReadAsync().get();
                const uint32_t size = (uint32_t)std::min<uint64_t>(stream.Size(), 16u << 20);
                if (size > 0) {
                    Buffer buf(size);
                    const auto got = stream.ReadAsync(buf, size, InputStreamOptions::None).get();
                    g_trackArt.assign((const char*)got.data(), got.Length());
                }
            }
        }
        artBytes = g_trackArt;
    } catch (...) {
        return !info.title.empty();
    }
    return true;
}

void seek(double sec) {
    auto s = current();
    if (!s) return;
    try {
        const auto tl = s.GetTimelineProperties();
        const int64_t ticks = tl.StartTime().count() + (int64_t)(sec * 1e7);   // 100 ns ticks
        s.TryChangePlaybackPositionAsync(ticks).get();
    } catch (...) {}
}
void send(int what) {
    auto s = current();
    if (!s) return;
    try {
        if (what == 1) s.TryTogglePlayPauseAsync().get();
        else if (what == 2) s.TrySkipPreviousAsync().get();
        else if (what == 3) s.TrySkipNextAsync().get();
        else if (what == 4) s.TryPlayAsync().get();
        else if (what == 5) s.TryPauseAsync().get();
    } catch (...) {}
}

bool setVolume(float factor) {
    g_factor = factor >= 0.999f ? 1.0f : factor;
    std::vector<Session> list = sessions();
    bool any = false;
    for (Session& s : list) {
        if (!s.vol) continue;
        if (factor >= 0.999f) {   // back to what each was
            auto it = g_baseVol.find(s.pid);
            if (it == g_baseVol.end()) continue;
            s.vol->SetMasterVolume(it->second, nullptr);   // (kept until all are back: the same app may have a session on two outputs)
            any = true;
        } else if (isPlayer(s)) {
            float v = 1.0f;
            s.vol->GetMasterVolume(&v);
            const float base = g_baseVol.count(s.pid) ? g_baseVol[s.pid] : v;
            g_baseVol[s.pid] = base;
            s.vol->SetMasterVolume(base * factor, nullptr);
            any = true;
        }
    }
    if (factor >= 0.999f) g_baseVol.clear();
    release(list);
    return any || factor >= 0.999f;
}

bool playerVol(float& v) {
    std::vector<Session> list = sessions();
    bool got = false;
    for (Session& s : list)
        if (s.vol && isPlayer(s)) {
            float x = 1.0f;
            s.vol->GetMasterVolume(&x);
            const auto it = g_baseVol.find(s.pid);
            v = it != g_baseVol.end() ? it->second : x;
            got = true;
            break;
        }
    release(list);
    return got;
}
bool setPlayerVol(float v) {
    v = std::max(0.0f, std::min(1.0f, v));
    std::vector<Session> list = sessions();
    bool any = false;
    for (Session& s : list)
        if (s.vol && isPlayer(s)) {
            auto it = g_baseVol.find(s.pid);
            if (it != g_baseVol.end()) { it->second = v; s.vol->SetMasterVolume(v * g_factor, nullptr); }   // (lowered: it stays lowered from there)
            else s.vol->SetMasterVolume(v, nullptr);
            any = true;
        }
    release(list);
    return any;
}
// the chosen output's own volume (Windows' default: the one in the taskbar)
IAudioEndpointVolume* endpointVolume() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) return nullptr;
    IAudioEndpointVolume* ev = nullptr;
    if (IMMDevice* dev = outputDevice(en)) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&ev);
        dev->Release();
    }
    en->Release();
    return ev;
}
bool masterVol(float& v) {
    IAudioEndpointVolume* ev = endpointVolume();
    if (!ev) return false;
    const bool ok = SUCCEEDED(ev->GetMasterVolumeLevelScalar(&v));
    ev->Release();
    return ok;
}
bool setMasterVol(float v) {
    IAudioEndpointVolume* ev = endpointVolume();
    if (!ev) return false;
    const bool ok = SUCCEEDED(ev->SetMasterVolumeLevelScalar(std::max(0.0f, std::min(1.0f, v)), nullptr));
    ev->Release();
    return ok;
}

bool outputs(std::vector<Output>& list) {
    list.clear();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) return false;
    // PKEY_Device_FriendlyName ("Speakers (Realtek(R) Audio)"), spelled out here rather than defined through initguid.h
    static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    IMMDeviceCollection* all = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && all) {
        UINT n = 0;
        all->GetCount(&n);
        for (UINT i = 0; i < n && i < 32; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(all->Item(i, &dev)) || !dev) continue;
            Output o;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) { o.id = utf8Of(id); CoTaskMemFree(id); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) o.name = utf8Of(v.pwszVal);
                PropVariantClear(&v);
                ps->Release();
            }
            if (!o.id.empty()) list.push_back(o.name.empty() ? Output{o.id, "Output " + std::to_string(i + 1)} : o);
            dev->Release();
        }
        all->Release();
    }
    en->Release();
    return true;
}
void setOutput(const std::string& id) { g_outputId = wideOf(id); }

bool masterMute(bool& m) {
    IAudioEndpointVolume* ev = endpointVolume();
    if (!ev) return false;
    BOOL b = FALSE;
    const bool ok = SUCCEEDED(ev->GetMute(&b));
    ev->Release();
    m = b != FALSE;
    return ok;
}
bool setMasterMute(bool m) {
    IAudioEndpointVolume* ev = endpointVolume();
    if (!ev) return false;
    const bool ok = SUCCEEDED(ev->SetMute(m ? TRUE : FALSE, nullptr));
    ev->Release();
    return ok;
}
bool playerMute(bool& m) {
    std::vector<Session> list = sessions();
    bool got = false;
    for (Session& s : list)
        if (s.vol && isPlayer(s)) { BOOL b = FALSE; if (SUCCEEDED(s.vol->GetMute(&b))) { m = b != FALSE; got = true; } break; }
    release(list);
    return got;
}
bool setPlayerMute(bool m) {
    std::vector<Session> list = sessions();
    bool any = false;
    for (Session& s : list)
        if (s.vol && isPlayer(s)) { s.vol->SetMute(m ? TRUE : FALSE, nullptr); any = true; }
    release(list);
    return any;
}

bool radioApps(std::string& who) {
    std::vector<Session> list = sessions();
    bool heard = false;
    for (Session& s : list) {
        std::string name;
        if (!s.meter || !isRadioApp(s.exe, name)) continue;
        float peak = 0;
        if (SUCCEEDED(s.meter->GetPeakValue(&peak)) && peak > 0.01f) { heard = true; who = name; break; }
    }
    release(list);
    return heard;
}

} // namespace platform
} // namespace media
} // namespace xpt
