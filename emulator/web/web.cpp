// XPTablet in the browser: the tablet's real screens (src/tabletui.cpp and friends, unchanged) compiled to WebAssembly with Emscripten
// and drawn with WebGL2, with a small host in place of the plugin: a simulated aircraft, traffic, time and weather, the Ground page's
// doors and jetway, and the downloads the browser can make itself (map tiles, SimBrief, VATSIM, METARs). Settings and notes are kept
// in the browser. Built by emulator/web/build.sh into one HTML page; nothing here is part of the plugin.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "metar.h"
#include "simbrief.h"
#include "tabletui.h"
#include "vatsim_parse.h"

using namespace xpt;

extern "C" {
extern const unsigned char xpt_web_shapes[];
extern const unsigned int xpt_web_shapes_size;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
SimState g_s;
Fonts g_f;
float g_scale = 1, g_wantScale = 1;
float g_logW = 800;                 // the tablet's layout width (its height is always 480)
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0, g_vao = 0, g_fontTex = 0;
GLint g_locProj = -1, g_locTex = -1, g_locBright = -1;
double g_last = 0;

// ---- the simulated aircraft and world ----
struct Ac { std::string icao = "C172"; bool toliss = false; };
Ac g_ac;
double g_lat = 41.7087, g_lon = -86.3173;
float g_hdg = 270, g_alt = 799, g_agl = 0, g_gs = 0, g_vs = 0, g_elev = 799;
bool g_onGround = true, g_moving = false, g_battery = true, g_engines = false, g_beacon = false, g_parkBrake = true;
bool g_openSam = false;
float g_zuluHour = -1, g_sunPitch = 30;
int g_trafficCount = 6;
float g_trafficRange = 12;
struct Tfc { MapState::Traffic t; };
std::vector<MapState::Traffic> g_tfc;
std::mt19937 g_rng(20261118);
float ur(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
const char* kTypes[] = {"A320", "A321", "A20N", "B738", "B38M", "B739", "E75L", "CRJ9", "A333", "B77W", "B789", "C172", "PC12", "DH8D"};
const char* kAirlines[] = {"UAL", "AAL", "DAL", "SWA", "JBU", "SKW", "ENY", "RPA", "FFT", "NKS", "ASA"};

MapState::Traffic makeTraffic(bool ground) {
    MapState::Traffic t;
    const float brg = ur(0, 360), d = ur(0.5f, g_trafficRange);
    t.lat = g_lat + d / 60.0 * std::cos(brg * kPi / 180);
    t.lon = g_lon + d / 60.0 * std::sin(brg * kPi / 180) / std::cos(g_lat * kPi / 180);
    t.type = kTypes[std::uniform_int_distribution<int>(0, 13)(g_rng)];
    char cs[16];
    std::snprintf(cs, sizeof cs, "%s%d", kAirlines[std::uniform_int_distribution<int>(0, 10)(g_rng)], std::uniform_int_distribution<int>(10, 4999)(g_rng));
    t.callsign = cs;
    t.onGround = ground;
    t.hdg = ur(0, 360);
    if (ground) { t.altFt = g_elev; t.gsKt = ur(0, 18); t.lat = g_lat + ur(-0.01f, 0.01f); t.lon = g_lon + ur(-0.012f, 0.012f); }
    else { t.altFt = std::max(1500.0f, g_alt + ur(-6000, 9000)); t.gsKt = ur(140, 460); t.vsFpm = ur(0, 1) < 0.5f ? 0 : ur(-1800, 1800); }
    return t;
}

// ---- the Ground page: a ToLiss's doors, chocks and power; openSAM's jetway ----
int g_jetway = 1;          // openSAM: 1 ready, 2 docked, -1 moving
double g_jetwayUntil = 0;
int g_jetwayTo = 1;
bool g_wantDoor[2] = {false, false};
bool g_xpJetway = false;               // without openSAM: X-Plane's own jetways, as last pressed (sim/ground_ops/jetway)
double g_xpJetwayT0 = -100;            // when they were sent for or away (they take 12 s, as the plugin reckons)
double g_jwPressT[2] = {-100, -100};
bool g_cabinReady = false, g_cabinReadyGround = true;
bool g_wasDocked[2] = {false, false};  // ToLiss: the door under a jetway opens when it docks   // the doors asked for: each connects on its own, the jetways leave with the last one
bool g_slidesAuto = false;             // ToLiss's own rule: armed with the beacon on and the doors shut, disarmed parked
// a pretend music player (the sim reads Windows' or Linux's own); the radio switch on the page stands for xPilot receiving
struct Song { const char* title; const char* artist; const char* album; unsigned char c0[3], c1[3]; };
const Song kSongs[] = {{"Flying Home", "The Pilots", "Cleared for Takeoff", {250, 70, 110}, {80, 40, 200}},
                       {"Clear Skies", "Holding Pattern", "FL350", {40, 180, 230}, {20, 60, 140}},
                       {"Night Approach", "ILS 27", "Glideslope", {255, 170, 60}, {120, 30, 60}},
                       {"Cruise Control", "Mach .78", "Long Haul", {60, 210, 140}, {20, 70, 90}}};
int g_song = 0;
const double kSongLen[4] = {214, 187, 251, 166};
double g_songPos = 0;   // seconds into the song
bool g_playing = true, g_radioRx = false, g_radioTx = false, g_pausedByRadio = false;
double g_radioLast = -10;
GLuint g_artTex[4] = {0, 0, 0, 0};
double g_sbNext = 30;   // the 2-minute SimBrief check
bool g_sbQuiet = false;

// ---- what the browser downloads (JavaScript does the fetching) ----
EM_JS(void, js_fetch_text, (int kind, const char* url), {
    const u = UTF8ToString(url);
    fetch(u).then(r => r.ok ? r.text().then(t => [r.status, t]) : [r.status, ""]).then(([st, t]) => {
        const p = stringToNewUTF8(t); Module._emu_text(kind, st, p); Module._free(p);
    }).catch(e => { const p = stringToNewUTF8(""); Module._emu_text(kind, -1, p); Module._free(p); });
});
EM_JS(void, js_load_tile, (int key, const char* url, int dark), {
    const img = new Image(); img.crossOrigin = "anonymous";
    img.onload = () => {
        let src = img;
        if (dark) {   // the same night colours as the plugin's (xpt::tiles::darken)
            const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
            const x = c.getContext('2d'); x.drawImage(img, 0, 0);
            const d = x.getImageData(0, 0, c.width, c.height), p = d.data;
            for (let i = 0; i < p.length; i += 4) {
                const m = Math.floor((p[i] + p[i + 1] + p[i + 2]) / 3);
                if (m >= 249) { p[i] = 66; p[i + 1] = 74; p[i + 2] = 90; continue; }
                const k = 255 - 2 * m, add = [8, 12, 22];
                for (let j = 0; j < 3; ++j) p[i + j] = Math.min(255, Math.floor(Math.max(0, Math.min(255, p[i + j] + k)) * 82 / 100) + add[j]);
            }
            x.putImageData(d, 0, 0); src = c;
        }
        const gl = GLctx, t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
        gl.generateMipmap(gl.TEXTURE_2D);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        const id = GL.getNewId(GL.textures); t.name = id; GL.textures[id] = t;
        Module._emu_tile(key, id);
    };
    img.onerror = () => Module._emu_tile(key, 0);
    img.src = UTF8ToString(url);
});
EM_JS(void, js_store, (const char* key, const char* value), { try { localStorage.setItem(UTF8ToString(key), UTF8ToString(value)); } catch (e) {} });
EM_JS(char*, js_load, (const char* key), {
    let v = null; try { v = localStorage.getItem(UTF8ToString(key)); } catch (e) {}
    return v === null ? 0 : stringToNewUTF8(v);
});
EM_JS(void, js_status, (const char* t), { const e = document.getElementById('hoststatus'); if (e) e.textContent = UTF8ToString(t); });

enum { K_SIMBRIEF = 1, K_VATSIM = 2, K_METAR = 3 };
struct TileSlot { GLuint tex = 0; bool done = false; };
std::vector<TileSlot> g_tileSlots;                 // by request number (what JavaScript answers with)
std::map<unsigned long long, int> g_tileIndex;      // a tile -> its request number
int g_tilesInFlight = 0;
ImTextureID tile(int src, int z, int x, int y, bool request) {
    const unsigned long long key = ((unsigned long long)(src & 3) << 62) | ((unsigned long long)(z & 31) << 56) | ((unsigned long long)(x & 0x0FFFFFFF) << 28) | (unsigned long long)(y & 0x0FFFFFFF);
    auto it = g_tileIndex.find(key);
    if (it != g_tileIndex.end()) return (ImTextureID)(intptr_t)g_tileSlots[it->second].tex;
    if (request) {
        if (g_tilesInFlight >= 12) return 0;
        const int k = (int)g_tileSlots.size();
        g_tileSlots.push_back({});
        g_tileIndex[key] = k;
        ++g_tilesInFlight;
        char url[160];
        if (src == 1) std::snprintf(url, sizeof url, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", z, y, x);
        // OpenStreetMap's own servers refuse a page opened from disk (it sends them no address of a site), so the emulator takes
        // CARTO's tiles of the same OpenStreetMap data (they allow it, with the credit); the plugin itself uses tile.openstreetmap.org
        else std::snprintf(url, sizeof url, "https://%c.basemaps.cartocdn.com/rastertiles/voyager/%d/%d/%d.png", "abcd"[(x + y) & 3], z, x, y);
        js_load_tile(k, url, src == 2);
    }
    return 0;
}

std::unordered_map<std::string, VatsimPilot> g_pilots;
double g_vatsimAt = -1e9;
bool g_vatsimBusy = false;
std::string g_vatsimStatus = "Asking VATSIM...";
std::string g_vatsimLast;

void loadSaved() {
    if (char* s = js_load("xptablet.settings")) { settingsFromText(s); free(s); }
    if (char* n = js_load("xptablet.notes")) { notesFromText(n); free(n); }
}

void buildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    g_f = loadFonts(g_scale);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    if (!g_fontTex) glGenTextures(1, &g_fontTex);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
}

GLuint g_logoTex = 0, g_iconsTex = 0, g_wxTex = 0;
GLuint uploadLevels(const unsigned char* px, int w, int h, int levels) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    size_t off = 0;
    int lw = w, lh = h;
    for (int l = 0; l < levels; ++l) {
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + off);
        off += (size_t)lw * lh * 4;
        lw = std::max(1, lw / 2); lh = std::max(1, lh / 2);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
void uploadPictures() {
    int w, h, l;
    if (const unsigned char* p = logoPixels(w, h, l)) g_logoTex = uploadLevels(p, w, h, l);
    if (const unsigned char* p = iconsPixels(w, h, l)) g_iconsTex = uploadLevels(p, w, h, l);
    unsigned int bytes = 0;
    if (const unsigned char* p = wxLogoPixels(bytes)) if (bytes >= 256 * 256 * 4) g_wxTex = uploadLevels(p, 256, 256, 1);
}

const char* kVS = R"(#version 300 es
uniform mat4 proj; in vec2 pos; in vec2 uv; in vec4 col; out vec2 vUv; out vec4 vCol;
void main() { vUv = uv; vCol = col; gl_Position = proj * vec4(pos, 0.0, 1.0); })";
const char* kFS = R"(#version 300 es
precision mediump float; uniform sampler2D tex; uniform float bright; in vec2 vUv; in vec4 vCol; out vec4 o;
void main() { vec4 c = vCol * texture(tex, vUv); o = vec4(c.rgb * bright, c.a); })";
GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); std::printf("shader: %s\n", log); }
    return s;
}

void render(ImDrawData* dd, int fbW, int fbH, float bright) {
    glViewport(0, 0, fbW, fbH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST);
    glUseProgram(g_prog);
    const float L = dd->DisplayPos.x, R = L + dd->DisplaySize.x, T = dd->DisplayPos.y, B = T + dd->DisplaySize.y;
    const float proj[16] = {2 / (R - L), 0, 0, 0, 0, 2 / (T - B), 0, 0, 0, 0, -1, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1};
    glUniformMatrix4fv(g_locProj, 1, GL_FALSE, proj);
    glUniform1i(g_locTex, 0);
    glUniform1f(g_locBright, bright);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g_vao);
    const float sx = fbW / dd->DisplaySize.x, sy = fbH / dd->DisplaySize.y;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, cl->VtxBuffer.Size * (int)sizeof(ImDrawVert), cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, cl->IdxBuffer.Size * (int)sizeof(ImDrawIdx), cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const float x0 = std::max(0.0f, (cmd.ClipRect.x - L) * sx), y0 = std::max(0.0f, (cmd.ClipRect.y - T) * sy);
            const float x1 = std::min((float)fbW, (cmd.ClipRect.z - L) * sx), y1 = std::min((float)fbH, (cmd.ClipRect.w - T) * sy);
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor((int)x0, (int)(fbH - y1), (int)(x1 - x0), (int)(y1 - y0));
            GLuint tex = (GLuint)(intptr_t)cmd.GetTexID();
            // the tablet's fixed picture ids (as the plugin's host gives them) to this page's textures
            if (cmd.GetTexID() == g_f.logo) tex = g_logoTex; else if (cmd.GetTexID() == g_f.icons) tex = g_iconsTex; else if (cmd.GetTexID() == g_f.wxLogo) tex = g_wxTex;
            glBindTexture(GL_TEXTURE_2D, tex);
            const size_t base = cmd.VtxOffset * sizeof(ImDrawVert);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, pos)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, uv)));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, col)));
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (void*)(cmd.IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

// the sun's height above the horizon, roughly
float sunPitchAt(double lat, double lon, std::time_t utc) {
    std::tm tm{};
    gmtime_r(&utc, &tm);
    const double day = tm.tm_yday + 1, hour = tm.tm_hour + tm.tm_min / 60.0;
    const double g = 2 * kPi / 365.0 * (day - 1 + (hour - 12) / 24);
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2 * g) + 0.000907 * std::sin(2 * g);
    const double eqt = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) - 0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
    const double ha = ((hour * 60 + eqt + 4 * lon) / 4 - 180) * kPi / 180, la = lat * kPi / 180;
    const double cz = std::sin(la) * std::sin(decl) + std::cos(la) * std::cos(decl) * std::cos(ha);
    return (float)(90 - std::acos(std::max(-1.0, std::min(1.0, cz))) * 180 / kPi);
}

SimWeather g_wx;
void initWeather() {
    g_wx.valid = true; g_wx.windDir = 240; g_wx.windKt = 12; g_wx.visSm = 10; g_wx.tempC = 12; g_wx.dewC = 6; g_wx.qnhHpa = 1013; g_wx.rain = 0;
    g_wx.clouds = 2; g_wx.cloudBaseFt[0] = 4500; g_wx.cloudTopFt[0] = 6500; g_wx.cloudCover[0] = 0.4f; g_wx.cloudBaseFt[1] = 12000; g_wx.cloudTopFt[1] = 15000; g_wx.cloudCover[1] = 0.7f;
    g_wx.aloft = 6;
    const float fts[6] = {0, 3000, 6000, 12000, 24000, 34000};
    for (int i = 0; i < 6; ++i) { g_wx.aloftFt[i] = fts[i]; g_wx.aloftDir[i] = 240 + i * 8; g_wx.aloftKt[i] = 12 + i * 14; }
}

void step(double dt, double now) {
    // the aircraft
    if (g_onGround) { g_vs = 0; g_agl = 0; g_alt = g_elev; }
    if (g_moving && g_gs > 0) {
        const double d = g_gs * dt / 3600.0;
        g_lat += d / 60.0 * std::cos(g_hdg * kPi / 180);
        g_lon += d / 60.0 * std::sin(g_hdg * kPi / 180) / std::cos(g_lat * kPi / 180);
        if (!g_onGround) { const float dz = g_vs * (float)dt / 60.0f; g_alt += dz; g_agl = std::max(0.0f, g_agl + dz); }
    }
    // time
    const std::time_t real = std::time(nullptr);
    const std::time_t utc = g_zuluHour < 0 ? real : real - real % 86400 + (std::time_t)(g_zuluHour * 3600);
    g_sunPitch = sunPitchAt(g_lat, g_lon, utc);
    MapState& m = g_s.map;
    m.havePos = true; m.lat = g_lat; m.lon = g_lon; m.hdgTrue = g_hdg; m.gsKt = g_gs; m.altFt = g_alt; m.vsFpm = g_vs; m.aglFt = g_agl; m.onGround = g_onGround;
    m.utc = utc;
    m.haveLocalOffset = true; m.localOffsetS = std::round(g_lon / 15.0) * 3600.0;   // X-Plane's own local time: by the longitude's zone
    m.tile = tile;
    g_s.zuluSec = (int)(utc % 86400);
    g_s.acfIcao = g_ac.icao;
    {   // the Airbus types' bundled checklists (as the plugin finds them), with the usual engine of each model
        static std::string forType = "?", text;
        if (forType != g_ac.icao) {
            forType = g_ac.icao;
            static const char* const types[][3] = {{"A319", "CFM56", "CFM56"}, {"A320", "CFM56", "CFM56"}, {"A20N", "LEAP", "LEAP-1A"}, {"A321", "V2500", "IAE V2500"},
                                                   {"A21N", "LEAP", "LEAP-1A"}, {"A339", "TRENT7000", "Trent 7000"}, {"A333", "", ""}, {"A346", "TRENT500", "Trent 500"}};
            g_s.checklistText.clear(); g_s.checklistSource.clear();
            for (const auto& t : types) if (g_ac.icao == t[0]) {
                if (text.empty()) { if (FILE* fp = std::fopen("/checklists/airbus.txt", "rb")) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n); std::fclose(fp); } }
                g_s.checklistText = checklistFor(text, t[0], t[1]);
                g_s.checklistSource = std::string("Airbus checklists (FlightGear A320-family, GPL-2)") + (t[2][0] ? std::string("  \xC2\xB7  ") + t[2] : "");
            }
        }
    }
    g_s.toliss = g_ac.toliss;
    g_s.batteryPct = 100; g_s.charging = g_battery;
    g_s.autoDimLevel = 1.0f - 0.55f * std::max(0.0f, std::min(1.0f, -g_sunPitch / 8.0f));
    g_wx.altFt = g_alt;
    g_s.wx = g_wx;
    // traffic
    while ((int)g_tfc.size() < g_trafficCount) g_tfc.push_back(makeTraffic(g_tfc.size() % 4 == 3));
    while ((int)g_tfc.size() > g_trafficCount && !g_tfc.empty() && g_tfc.back().callsign != "N555RA") g_tfc.pop_back();
    for (MapState::Traffic& t : g_tfc) {
        const double d = t.gsKt * dt / 3600.0;
        t.lat += d / 60.0 * std::cos(t.hdg * kPi / 180);
        t.lon += d / 60.0 * std::sin(t.hdg * kPi / 180) / std::cos(t.lat * kPi / 180);
        if (!t.onGround) t.altFt = std::max(500.0f, t.altFt + t.vsFpm * (float)dt / 60.0f);
        const double dn = std::hypot((t.lat - g_lat) * 60.0, (t.lon - g_lon) * 60.0 * std::cos(g_lat * kPi / 180));
        if (dn > g_trafficRange * 1.6 && t.callsign != "N555RA") t = makeTraffic(t.onGround);
    }
    m.traffic = g_tfc;
    // the Ground page
    GroundState& gr = g_s.ground;
    gr.onGround = g_onGround; gr.beacon = g_beacon; gr.enginesRunning = g_engines; gr.parkBrake = g_parkBrake; gr.groundSpeedKt = g_gs;
    gr.toliss = g_ac.toliss;
    gr.openSam = g_openSam;
    if (g_openSam && g_jetway == -1 && now >= g_jetwayUntil) g_jetway = g_jetwayTo;
    gr.jetway = g_openSam ? g_jetway : 0;
    if (g_ac.toliss) {
        const bool four = g_ac.icao == "A321" || g_ac.icao == "A21N" || g_ac.icao == "A339" || g_ac.icao == "A346";   // four pairs of doors
        gr.paxDoors = four ? 8 : 4; gr.cargoDoors = 3; gr.deice = true;
        if (g_cabinReady && g_onGround != g_cabinReadyGround) g_cabinReady = false;   // until the next takeoff or landing
        gr.cabinReady = g_cabinReady;
        if (gr.slides < 0) gr.slides = 0;
        if (gr.cabinLight < 0) gr.cabinLight = 0.7f;
    } else { gr.paxDoors = gr.cargoDoors = 0; gr.deice = false; gr.slides = -1; gr.cabinLight = -1; }
    // jetways: openSAM's (they move, by door), else X-Plane's own (one command brings or sends them all), as the plugin reads them
    if (!g_onGround || g_gs > 3) { g_xpJetway = false; g_wantDoor[0] = g_wantDoor[1] = false; }
    if (!g_openSam) gr.jetway = g_xpJetway ? 2 : 1;
    // the jetway doors as the plugin finds them: 1L, and 2L on the four-pair types (openSAM: two)
    gr.jwDoors = g_openSam ? 2 : (g_ac.toliss && gr.paxDoors == 8) ? 2 : 1;
    if (!g_ac.toliss) gr.paxDoors = gr.jwDoors >= 2 ? 8 : 4;   // (the layout with 2L ahead of the wing, as the plugin gives it)
    const bool xpThere = g_xpJetway && now - g_xpJetwayT0 >= 12;
    for (int k = 0; k < 2; ++k) {
        gr.jwDoorPax[k] = 2 * k;
        const bool here = k < gr.jwDoors;
        gr.jwDocked[k] = here && g_wantDoor[k] && (g_openSam ? g_jetway == 2 : xpThere);   // connected once it is there
        gr.jwMoving[k] = here && (g_openSam ? (g_jetway == -1 && g_wantDoor[k] != (g_jetwayTo == 1))
                                            : now - g_xpJetwayT0 < 12 && (g_wantDoor[k] ? g_xpJetway : now - g_jwPressT[k] < 12));
    }
    if (g_ac.toliss) for (int k = 0; k < gr.jwDoors; ++k) if (gr.jwDocked[k] != g_wasDocked[k]) {
        g_wasDocked[k] = gr.jwDocked[k];
        if (2 * k < gr.paxDoors) gr.paxDoor[2 * k] = gr.jwDocked[k] ? 2 : 0;   // the door under it opens, and closes when it leaves
    }
    // the slides, by themselves
    gr.slidesAuto = g_slidesAuto;
    if (g_ac.toliss && g_slidesAuto) {
        bool shut = true;
        for (int k = 0; k < gr.paxDoors; ++k) if (gr.paxDoor[k] == 2) shut = false;
        if (g_beacon && shut) gr.slides = 1;
        else if (g_onGround && !g_beacon && !g_engines) gr.slides = 0;
    }
    // the music, and the radio's effect on it (Settings, Media)
    MediaState& md = g_s.media;
    md.supported = true;
    if (!settings().mediaEnabled) md = MediaState();
    else {
        const Song& so = kSongs[g_song];
        md.active = true; md.app = "Emulator Player"; md.title = so.title; md.artist = so.artist; md.album = so.album;
        md.canPrev = md.canNext = true;
        if (g_playing) { g_songPos += dt; if (g_songPos >= kSongLen[g_song]) { g_song = (g_song + 1) % 4; g_songPos = 0; } }
        md.posSec = g_songPos; md.lenSec = kSongLen[g_song]; md.canSeek = true;
        if (g_song == 3) { md.art = 0; md.artW = md.artH = 0; }   // a song without a cover: the disc is shown
        else if (!g_artTex[g_song]) {   // the cover: the song's two colours, corner to corner, with a ring
            std::vector<unsigned char> px(256 * 256 * 4);
            for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) {
                const float t = (x + y) / 510.0f, r = std::hypot(x - 128.0f, y - 128.0f);
                const float ring = (r > 60 && r < 70) || r < 14 ? 0.35f : 0.0f;
                for (int c = 0; c < 3; ++c) px[(y * 256 + x) * 4 + c] = (unsigned char)std::min(255.0f, so.c0[c] + (so.c1[c] - so.c0[c]) * t + 255 * ring);
                px[(y * 256 + x) * 4 + 3] = 255;
            }
            g_artTex[g_song] = uploadLevels(px.data(), 256, 256, 1);
        }
        if (g_song != 3) { md.art = (ImTextureID)(intptr_t)g_artTex[g_song]; md.artW = md.artH = 256; }
        const bool heard = g_radioRx || g_radioTx || now - g_radioLast < 1.2;
        if (g_radioRx || g_radioTx) g_radioLast = now;
        md.radioTx = g_radioTx;
        const int mode = settings().mediaDuck;
        if (mode == 2 && heard && g_playing) { g_playing = false; g_pausedByRadio = true; }
        if (mode == 2 && !heard && g_pausedByRadio) { g_playing = true; g_pausedByRadio = false; }
        if (mode != 2) g_pausedByRadio = false;
        md.playing = g_playing;
        md.radioRx = heard; md.radioSource = heard ? "xPilot" : "";
        md.ducked = (mode == 1 && heard && g_playing) || (mode == 2 && g_pausedByRadio);
    }
    // Settings, Data
    g_s.dataInfo = {{"Running in", "the browser emulator (no X-Plane)"}, {"Map tiles", "CARTO (OpenStreetMap data) / Esri, loaded by this page"},
                    {"Airports and navaids", "need X-Plane's data: use the desktop emulator with your X-Plane folder"},
                    {"Flight plan", "SimBrief, fetched by this page"}};
}

void groundAction(const Actions& a, double now) {
    GroundState& gr = g_s.ground;
    switch (a.ground) {
    case GA_PAX_DOOR: if (a.groundIndex >= 0 && a.groundIndex < 8) gr.paxDoor[a.groundIndex] = gr.paxDoor[a.groundIndex] == 2 ? 0 : 2; break;
    case GA_CARGO_DOOR: if (a.groundIndex >= 0 && a.groundIndex < 4) gr.cargoDoor[a.groundIndex] = gr.cargoDoor[a.groundIndex] == 2 ? 0 : 2; break;
    case GA_DOORS_CLOSE: for (int& d : gr.paxDoor) d = 0; for (int& d : gr.cargoDoor) d = 0; break;
    case GA_DOORS_AUTO: for (int& d : gr.paxDoor) d = 1; break;
    case GA_CHOCKS: gr.chocks = !gr.chocks; break;
    case GA_GPU: gr.extPower = !gr.extPower; break;
    case GA_HP_AIR: gr.hpAir = !gr.hpAir; break;
    case GA_LP_AIR: gr.lpAir = !gr.lpAir; break;
    case GA_SLIDES_ARM: gr.slides = 1; g_slidesAuto = false; break;
    case GA_SLIDES_DISARM: gr.slides = 0; g_slidesAuto = false; break;
    case GA_SLIDES_AUTO: g_slidesAuto = true; break;
    case GA_CABIN_READY: if (g_ac.toliss) { g_cabinReady = true; g_cabinReadyGround = g_onGround; } break;
    case GA_CABIN_LIGHT: gr.cabinLight = std::max(0, std::min(100, a.groundIndex)) / 100.0f; break;
    case GA_JETWAY: case GA_JETWAY_DOOR: {
        if (g_openSam && g_jetway == -1) break;
        const bool any0 = g_wantDoor[0] || g_wantDoor[1];
        if (a.ground == GA_JETWAY_DOOR && a.groundIndex >= 0 && a.groundIndex < 2) { g_wantDoor[a.groundIndex] = !g_wantDoor[a.groundIndex]; g_jwPressT[a.groundIndex] = now; }
        else { const bool on = !(g_openSam ? g_jetway == 2 : g_xpJetway); g_wantDoor[0] = g_wantDoor[1] = on; }
        const bool any1 = g_wantDoor[0] || g_wantDoor[1];
        if (!g_openSam) {   // X-Plane's own: sim/ground_ops/jetway with the first door, again with the last
            if (!any0 && any1) { g_xpJetway = true; g_xpJetwayT0 = now; }
            else if (any0 && !any1) { g_xpJetway = false; g_xpJetwayT0 = now; }
        } else if (!any0 && any1 && g_jetway != 2) { g_jetwayTo = 2; g_jetway = -1; g_jetwayUntil = now + 6; }   // the first door: the jetways come
        else if (any0 && !any1 && g_jetway == 2) { g_jetwayTo = 1; g_jetway = -1; g_jetwayUntil = now + 6; }   // the last door: they go
        break;
    }
    default: break;
    }
}

void handle(const Actions& a, double now) {
    if (a.ground != GA_NONE) groundAction(a, now);
    if (a.media == 1) { g_playing = !g_playing; g_pausedByRadio = false; }
    if (a.media == 2) { if (g_songPos > 3) g_songPos = 0; else { g_song = (g_song + 3) % 4; g_songPos = 0; } }   // as players do: back to the start, then the song before
    if (a.media == 3) { g_song = (g_song + 1) % 4; g_songPos = 0; }
    if (a.mediaSeek >= 0) g_songPos = std::min(a.mediaSeek, kSongLen[g_song] - 0.5);
    // every 2 minutes with a SimBrief ID: a new plan is loaded and announced
    if (settings().notifySimbrief && !settings().simbriefUser.empty() && !g_s.plan.busy && now >= g_sbNext) {
        g_sbNext = now + 120; g_sbQuiet = true;
        js_fetch_text(K_SIMBRIEF, simbriefUrl(settings().simbriefUser).c_str());
    }
    // the Weather page's METAR search
    if (!a.metarSearch.empty() && !g_s.metar.busy) {
        g_s.metar = MetarLookup(); g_s.metar.icao = a.metarSearch; g_s.metar.busy = true;
        const std::string url = "https://aviationweather.gov/api/data/metar?ids=" + a.metarSearch + "&format=raw&taf=false";
        js_fetch_text(K_METAR, url.c_str());
    }
    // web pages: not in the browser emulator (X-Plane's own Chromium shows them)
    g_s.browser.status = -3;
    g_s.browser.message = "Web pages open in X-Plane's own Chromium, which the browser emulator doesn't have. Use the desktop emulator with your X-Plane folder, or the sim.";
    // SimBrief
    if (a.planImport && !settings().simbriefUser.empty() && !g_s.plan.busy) {
        g_s.plan.busy = true; g_s.plan.status = "Asking SimBrief..."; g_sbQuiet = false;
        js_fetch_text(K_SIMBRIEF, simbriefUrl(settings().simbriefUser).c_str());
    }
    planDecide(g_s, a);
    if (a.planClear) { g_s.plan = FlightPlan(); g_s.plan.status = "Cleared"; planToRoute(g_s.plan, g_s.map); }
    // a traffic card: its VATSIM details (simulated traffic is not on VATSIM, so most say so)
    if (a.mapShown && !a.trafficSelected.empty()) {
        if (!g_vatsimBusy && now - g_vatsimAt > 30) { g_vatsimBusy = true; g_vatsimAt = now; js_fetch_text(K_VATSIM, "https://data.vatsim.net/v3/vatsim-data.json"); }
        auto it = g_pilots.find(a.trafficSelected);
        if (it != g_pilots.end()) { g_s.map.vatsim = it->second; g_s.map.vatsimStatus.clear(); }
        else { g_s.map.vatsim = VatsimPilot(); g_s.map.vatsimStatus = g_pilots.empty() ? g_vatsimStatus : "Not on VATSIM (simulated traffic)"; }
    }
    // documents: the browser has no files, so one sample chart (drawn here) to open and mark up
    g_s.docs.listed = true;
    if (g_s.docs.folders.empty()) g_s.docs.folders = {"(the browser emulator has no files: the sim lists Output/XPTablet/Documents and the aircraft's folder)"};
    if (g_s.docs.files.empty()) g_s.docs.files = {{"Sample chart.png", "emulator:sample-chart", "XPTablet's Documents", false, 412000}};
    if (!a.docOpen.empty()) {
        static GLuint chart = 0;
        const int W = 800, H = 1040;
        if (!chart) {   // an airport diagram, roughly: a white page, a title band, two runways, taxiways, an apron
            std::vector<unsigned char> px((size_t)W * H * 4, 255);
            auto rect = [&](int x0, int y0, int x1, int y1, int r, int g, int b) {
                for (int y = std::max(0, y0); y < std::min(H, y1); ++y) for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
                    unsigned char* q = &px[((size_t)y * W + x) * 4]; q[0] = (unsigned char)r; q[1] = (unsigned char)g; q[2] = (unsigned char)b; q[3] = 255; }
            };
            auto line = [&](float x0, float y0, float x1, float y1, float th, int r, int g, int b) {
                const int n = (int)std::hypot(x1 - x0, y1 - y0);
                for (int i = 0; i <= n; ++i) { const float t = n ? (float)i / n : 0; const float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
                    rect((int)(x - th), (int)(y - th), (int)(x + th), (int)(y + th), r, g, b); }
            };
            rect(0, 0, W, 6, 40, 40, 40); rect(0, H - 6, W, H, 40, 40, 40); rect(0, 0, 6, H, 40, 40, 40); rect(W - 6, 0, W, H, 40, 40, 40);
            rect(6, 6, W - 6, 110, 230, 234, 240); rect(6, 108, W - 6, 112, 40, 40, 40);
            for (int x = 40; x < W - 40; x += 40) rect(x, 150, x + 1, H - 40, 228, 232, 238);   // a faint grid
            for (int y = 150; y < H - 40; y += 40) rect(40, y, W - 40, y + 1, 228, 232, 238);
            line(140, 820, 660, 300, 16, 70, 74, 82);   // runway 9-27, slanted
            line(400, 190, 400, 930, 14, 70, 74, 82);   // runway 18-36
            for (float t = 0.05f; t < 0.95f; t += 0.06f) line(140 + 520 * t, 820 - 520 * t, 140 + 520 * (t + 0.025f), 820 - 520 * (t + 0.025f), 1.5f, 255, 255, 255);   // centre lines
            for (int y = 210; y < 910; y += 36) rect(399, y, 401, y + 18, 255, 255, 255);
            line(470, 190, 470, 930, 4, 214, 180, 40); line(330, 560, 700, 560, 4, 214, 180, 40); line(560, 420, 700, 420, 4, 214, 180, 40);   // taxiways
            rect(560, 600, 740, 780, 200, 204, 212); rect(600, 640, 720, 740, 150, 156, 168);   // the apron and the terminal
            chart = uploadLevels(px.data(), W, H, 1);
        }
        g_s.docs.shownPath = a.docOpen; g_s.docs.tex = (ImTextureID)(intptr_t)chart; g_s.docs.texW = W; g_s.docs.texH = H;
    }
    // the Airports page: X-Plane's airports are not here
    if (a.airportsShown) g_s.airportStatus = "Airports come from X-Plane's scenery: try them in the desktop emulator with your X-Plane folder.";
    if (takeSettingsChanged()) js_store("xptablet.settings", settingsToText().c_str());
    if (takeNotesChanged()) js_store("xptablet.notes", notesToText().c_str());
}

void frame() {
    const double now = emscripten_get_now() / 1000.0;
    const double dt = g_last > 0 ? std::min(0.5, now - g_last) : 1.0 / 60;
    g_last = now;
    if (std::fabs(g_wantScale - g_scale) > 0.01f) { g_scale = g_wantScale; buildFonts(); }
    g_f.logo = (ImTextureID)(intptr_t)1000001; g_f.icons = (ImTextureID)(intptr_t)1000002; g_f.wxLogo = g_wxTex ? (ImTextureID)(intptr_t)1000003 : 0;
    step(dt, now);
    g_s.pixelScale = g_scale;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_logW, kLogicalH);
    io.DeltaTime = (float)std::max(1e-4, dt);
    ImGui::NewFrame();
    Actions act;
    build(g_f, g_s, g_logW, kLogicalH, act);
    ImGui::Render();
    handle(act, now);
    int fbW = 0, fbH = 0;
    emscripten_get_canvas_element_size("#canvas", &fbW, &fbH);
    const float bright = settings().autoDim ? g_s.autoDimLevel : std::max(0.1f, std::min(1.0f, settings().brightness));
    render(ImGui::GetDrawData(), fbW, fbH, bright);
}

void resetTraffic() { g_tfc.clear(); for (int i = 0; i < g_trafficCount; ++i) g_tfc.push_back(makeTraffic(i % 4 == 3)); }

} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void emu_mouse(float x, float y, int down) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(x, y); if (down >= 0) io.AddMouseButtonEvent(0, down != 0); }
EMSCRIPTEN_KEEPALIVE void emu_wheel(float dy) { ImGui::GetIO().AddMouseWheelEvent(0, dy); }
EMSCRIPTEN_KEEPALIVE int emu_typing() { return tabletTyping() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void emu_key(int code) { if (tabletTyping()) tabletKey(code, false); }   // 8 back, 13 enter
EMSCRIPTEN_KEEPALIVE void emu_char(int c) { if (tabletTyping() && c >= 32 && c < 127) tabletKey(c, true); }
EMSCRIPTEN_KEEPALIVE void emu_scale(float s) { g_wantScale = std::max(1.0f, std::min(3.0f, s)); }
EMSCRIPTEN_KEEPALIVE float emu_width() { return g_logW; }
EMSCRIPTEN_KEEPALIVE void emu_shape(float aspect) { g_logW = kLogicalH * std::max(0.6f, std::min(2.6f, aspect)); }
EMSCRIPTEN_KEEPALIVE void emu_tile(int key, int tex) { if (key >= 0 && key < (int)g_tileSlots.size()) { g_tileSlots[key].tex = (GLuint)tex; g_tileSlots[key].done = true; --g_tilesInFlight; } }
EMSCRIPTEN_KEEPALIVE void emu_text(int kind, int status, const char* text) {
    const std::string body = text ? text : "";
    if (kind == K_SIMBRIEF) {
        FlightPlan p;
        if (status < 0) p.status = "SimBrief could not be reached from this page";
        else if (status == 400) p.status = "SimBrief has no plan for that ID";
        else if (status != 200) p.status = "SimBrief answered " + std::to_string(status);
        else p = parseSimbrief(body);
        planFetched(g_s, p, g_sbQuiet);   // into the preview: the flight plan stays until its Import
        g_sbQuiet = false;
    } else if (kind == K_VATSIM) {
        g_vatsimBusy = false;
        std::unordered_map<std::string, VatsimPilot> p;
        if (status == 200 && vatsim::readPilots(body, p)) { g_pilots.swap(p); g_vatsimStatus.clear(); }
        else g_vatsimStatus = status < 0 ? "VATSIM could not be reached from this page" : "VATSIM's data could not be read";
    } else if (kind == K_METAR) {
        g_s.metar.busy = false;
        std::string raw = body;
        while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ')) raw.pop_back();
        if (status == 200 && !raw.empty()) g_s.metar.raw = raw.substr(0, raw.find('\n'));
        else g_s.metar.error = status < 0 ? "aviationweather.gov could not be reached from this page" : "No METAR for " + g_s.metar.icao;
    }
}
// the host's controls (the page around the tablet)
EMSCRIPTEN_KEEPALIVE void emu_aircraft(const char* icao, int toliss) { g_ac.icao = icao ? icao : "C172"; g_ac.toliss = toliss != 0; openApp(APP_HOME); }
EMSCRIPTEN_KEEPALIVE void emu_place(double lat, double lon, float elev) { g_lat = lat; g_lon = lon; g_elev = elev; g_alt = elev; g_agl = 0; g_onGround = true; g_moving = false; g_gs = 0; resetTraffic(); }
EMSCRIPTEN_KEEPALIVE void emu_set(int what, double v) {
    switch (what) {
    case 1: g_hdg = (float)v; break;
    case 2: g_gs = (float)v; break;
    case 3: g_onGround = v != 0; if (!g_onGround && g_agl < 10) { g_agl = 3000; g_alt = g_elev + 3000; if (g_gs < 120) g_gs = 250; g_moving = true; } break;
    case 4: g_moving = v != 0; break;
    case 5: g_agl = (float)v; g_alt = g_elev + (float)v; break;
    case 6: g_vs = (float)v; break;
    case 7: g_trafficCount = (int)v; break;
    case 8: g_zuluHour = (float)v; break;
    case 9: g_s.wxPlane = v != 0; break;
    case 10: g_openSam = v != 0; break;
    case 11: g_battery = v != 0; break;
    case 12: g_beacon = v != 0; break;
    case 13: g_parkBrake = v != 0; break;
    case 14: g_engines = v != 0; break;
    case 15: g_radioRx = v != 0; break;
    case 16: g_radioTx = v != 0; break;
    case 20: g_wx.windDir = (float)v; break;
    case 21: g_wx.windKt = (float)v; break;
    case 22: g_wx.visSm = (float)v; break;
    case 23: g_wx.tempC = (float)v; break;
    case 24: g_wx.qnhHpa = (float)v; break;
    case 25: g_wx.rain = (float)v; break;
    default: break;
    }
}
EMSCRIPTEN_KEEPALIVE void emu_intruder() {
    MapState::Traffic t;
    const double h = g_hdg * kPi / 180;
    t.lat = g_lat + 0.6 / 60.0 * std::cos(h);
    t.lon = g_lon + 0.6 / 60.0 * std::sin(h) / std::cos(g_lat * kPi / 180);
    t.altFt = g_alt + 200; t.hdg = std::fmod(g_hdg + 180, 360.0f); t.gsKt = 250; t.vsFpm = -200; t.callsign = "N555RA"; t.type = "B738";
    g_tfc.push_back(t);
}
EMSCRIPTEN_KEEPALIVE void emu_new_traffic() { resetTraffic(); }
EMSCRIPTEN_KEEPALIVE void emu_battery(float pct) { setBattery(pct); }
// a sample SimBrief plan, as the 2-minute check would find it (each press a newer one): the notification, then the import window
EMSCRIPTEN_KEEPALIVE void emu_sample_plan() {
    static int n = 0;
    ++n;
    FlightPlan p;
    p.loaded = true; p.callsign = "AAL" + std::to_string(1233 + n); p.aircraft = g_ac.icao; p.registration = "N123AA"; p.units = "lbs";
    p.importedAt = "now"; p.ofpId = "sample/" + std::to_string(n);
    p.origin = "KSBN"; p.originName = "South Bend Intl"; p.originRunway = "27"; p.dest = "KORD"; p.destName = "Chicago O'Hare Intl"; p.destRunway = "28C";
    p.alternate = "KMDW"; p.alternateName = "Chicago Midway Intl";
    p.route = "GIJ DCT PMM DCT OBK"; p.sid = "SBN2"; p.star = "WATSN4"; p.cruise = "FL240"; p.distanceNm = "92"; p.ete = "38 min"; p.costIndex = "30"; p.avgWind = "head 22 kt";
    p.blockFuel = "9800"; p.tripFuel = "4200"; p.reserveFuel = "2900"; p.altnFuel = "1600"; p.taxiFuel = "600"; p.extraFuel = "500"; p.zfw = "141200"; p.tow = "150400"; p.ldw = "146200";
    struct Fx { const char* id; double lat, lon; const char* alt; };
    const Fx fx[] = {{"KSBN", 41.7087, -86.3173, ""}, {"GIJ", 41.7685, -86.3186, "FL240"}, {"PMM", 42.4644, -86.1059, "FL240"},
                     {"OBK", 42.2206, -87.9520, "FL240"}, {"WATSN", 42.0500, -87.7000, "FL110"}, {"KORD", 41.9786, -87.9048, ""}};
    for (const Fx& f : fx) { FlightPlan::Leg l; l.ident = f.id; l.lat = f.lat; l.lon = f.lon; l.alt = f.alt; l.legNm = "18"; l.totalTime = "0:08"; p.legs.push_back(l); }
    planFetched(g_s, p, true);
}
EMSCRIPTEN_KEEPALIVE void emu_reset_settings() { settingsFromText(""); settings() = Settings(); js_store("xptablet.settings", ""); openApp(APP_HOME); }
EMSCRIPTEN_KEEPALIVE float emu_alt() { return g_alt; }
EMSCRIPTEN_KEEPALIVE float emu_gs() { return g_gs; }
EMSCRIPTEN_KEEPALIVE int emu_on_ground() { return g_onGround ? 1 : 0; }
}

int main() {
    EmscriptenWebGLContextAttributes at;
    emscripten_webgl_init_context_attributes(&at);
    at.majorVersion = 2; at.alpha = 0; at.antialias = 0; at.premultipliedAlpha = 0;
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &at);
    if (ctx <= 0) { std::printf("WebGL2 is not available\n"); return 1; }
    emscripten_webgl_make_context_current(ctx);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, shader(GL_VERTEX_SHADER, kVS));
    glAttachShader(g_prog, shader(GL_FRAGMENT_SHADER, kFS));
    glBindAttribLocation(g_prog, 0, "pos"); glBindAttribLocation(g_prog, 1, "uv"); glBindAttribLocation(g_prog, 2, "col");
    glLinkProgram(g_prog);
    g_locProj = glGetUniformLocation(g_prog, "proj"); g_locTex = glGetUniformLocation(g_prog, "tex"); g_locBright = glGetUniformLocation(g_prog, "bright");
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glGenBuffers(1, &g_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigDebugHighlightIdConflicts = false;
    applyStyle();
    loadShapes(std::string((const char*)xpt_web_shapes, xpt_web_shapes_size));
    loadSaved();
    uploadPictures();
    g_scale = g_wantScale = (float)EM_ASM_DOUBLE({ return window.emuScale ? window.emuScale() : 1; });
    buildFonts();
    initWeather();
    resetTraffic();
    g_s.plan.status = "";
    planToRoute(g_s.plan, g_s.map);
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
