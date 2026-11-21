// Local Connect, on the phone or tablet: XPTablet's own screens (src/tabletui.cpp, unchanged) compiled to WebAssembly and drawn with
// WebGL2, as a copy of the tablet in the sim. The plugin serves this page over HTTPS on the home network (src/localserver.cpp);
// the page connects back over a WebSocket, pairs once with the code shown in the sim, then takes the sim's state as it changes and
// sends back what the user asks for (src/localwire.h). Built by tools/build_localclient.sh into one page.
//
// What this copy does itself: the map's tiles (fetched by the device), the notification chime, its own keyboard. What only the
// computer can show is left out here altogether (tabletui's deviceCannot): the web apps, the Browser, Documents, the briefing PDF.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "localwire.h"
#include "tabletui.h"

using namespace xpt;

extern "C" {
extern const unsigned char xpt_web_shapes[];
extern const unsigned int xpt_web_shapes_size;
extern const unsigned char xpt_asset_notify[];
extern const unsigned int xpt_asset_notify_size;
}

namespace {

SimState g_s;
Fonts g_f;
float g_scale = 1, g_wantScale = 1;
float g_logW = 800;
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0, g_vao = 0, g_fontTex = 0;
GLint g_locProj = -1, g_locTex = -1, g_locBright = -1;
double g_last = 0;

enum Phase { P_CONNECTING = 0, P_PAIR = 1, P_REFUSED = 2, P_LIVE = 3 };
int g_phase = P_CONNECTING;
std::string g_message = "Looking for XPTablet on your computer...";
std::string g_code;
bool g_reloadOnRetry = false;
int g_have = 0;                         // what has arrived: 1 the state's core, 2 the map's, 4 the settings, 8 the notes
void goLiveWhenReady();
std::string g_lastActions;
double g_lastActionsAt = -1, g_settingsSentAt = -1, g_notesSentAt = -1;
bool g_settingsDirty = false, g_notesDirty = false;

// ---- the browser's side (JavaScript) ----
EM_JS(void, js_ws_connect, (), {
    if (Module.lcWs) { try { Module.lcWs.onclose = null; Module.lcWs.close(); } catch (e) {} }
    Module.lcStop = false;
    const ws = new WebSocket('wss://' + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    Module.lcWs = ws;
    ws.onopen = () => Module._lc_open();
    ws.onmessage = (e) => {
        const u = new Uint8Array(e.data), p = _malloc(u.length || 1);
        HEAPU8.set(u, p);
        Module._lc_message(p, u.length);
        _free(p);
    };
    ws.onclose = () => { if (Module.lcWs !== ws) return; Module.lcWs = null; Module._lc_closed(); if (!Module.lcStop) setTimeout(() => { if (!Module.lcWs && !Module.lcStop) Module.lcConnect(); }, 2000); };
});
EM_JS(void, js_ws_send, (const unsigned char* p, int n), { const ws = Module.lcWs; if (ws && ws.readyState === 1) ws.send(HEAPU8.slice(p, p + n)); });
EM_JS(void, js_ws_stop, (), { Module.lcStop = true; const ws = Module.lcWs; if (ws) { try { ws.close(); } catch (e) {} } });
EM_JS(void, js_reload, (), { location.reload(); });
EM_JS(char*, js_device_kind, (), {
    const ua = navigator.userAgent || "";
    let k = "Computer";
    if (/iPhone|iPod/.test(ua)) k = "iPhone";
    else if (/iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)) k = "iPad";
    else if (/Android/.test(ua)) k = /Mobile/.test(ua) ? "Android phone" : "Android tablet";
    return stringToNewUTF8(k);
});
EM_JS(void, js_battery_watch, (), {
    if (!navigator.getBattery) return;   // (iPhone and iPad browsers do not share it)
    navigator.getBattery().then((b) => {
        const tell = () => Module._lc_battery(Math.round(b.level * 100), b.charging ? 1 : 0);
        b.addEventListener('levelchange', tell); b.addEventListener('chargingchange', tell);
        Module.lcBattery = tell;
        tell();
    }).catch(() => {});
});
EM_JS(void, js_battery_again, (), { if (Module.lcBattery) Module.lcBattery(); });
EM_JS(void, js_chime, (const unsigned char* pcm, int bytes, float vol), {
    try {
        const ctx = Module.chimeCtx || (Module.chimeCtx = new (window.AudioContext || window.webkitAudioContext)());
        const n = bytes >> 1;
        const s16 = new Int16Array(HEAPU8.buffer, pcm, n);
        const buf = ctx.createBuffer(1, n, 48000);
        const d = buf.getChannelData(0);
        for (let i = 0; i < n; ++i) d[i] = s16[i] / 32768;
        const src = ctx.createBufferSource();
        const g = ctx.createGain();
        g.gain.value = vol;
        src.buffer = buf; src.connect(g); g.connect(ctx.destination); src.start();
    } catch (e) {}
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
        Module._lc_tile(key, id);
    };
    img.onerror = () => Module._lc_tile(key, 0);
    img.src = UTF8ToString(url);
});
// the device's token (what pairing gave it), kept by the browser for this address
EM_JS(void, js_store, (const char* key, const char* value), { try { localStorage.setItem(UTF8ToString(key), UTF8ToString(value)); } catch (e) {} });
EM_JS(char*, js_load, (const char* key), {
    let v = null; try { v = localStorage.getItem(UTF8ToString(key)); } catch (e) {}
    return v === null ? 0 : stringToNewUTF8(v);
});

// ---- the messages' own fields ----
void putVar(std::string& o, unsigned long long n) { while (n >= 0x80) { o.push_back((char)(n | 0x80)); n >>= 7; } o.push_back((char)n); }
void putStr(std::string& o, const std::string& s) { putVar(o, s.size()); o += s; }
bool getStr(const std::string& m, size_t& at, std::string& out) {
    unsigned long long n = 0;
    for (int sh = 0;; sh += 7) { if (at >= m.size() || sh > 28) return false; const unsigned char c = (unsigned char)m[at++]; n |= (unsigned long long)(c & 0x7F) << sh; if (!(c & 0x80)) break; }
    if (n > m.size() - at) return false;
    out = m.substr(at, (size_t)n);
    at += (size_t)n;
    return true;
}
void send(const std::string& m) { js_ws_send((const unsigned char*)m.data(), (int)m.size()); }
// The tablet is shown only once the computer's settings and notes are here as well as the state: a copy that started from its
// own defaults would send those back as if they had been chosen.
void goLiveWhenReady() { if (g_have == 15 && g_phase == P_CONNECTING) { g_phase = P_LIVE; g_message.clear(); } }
void refuse(const std::string& why, bool reload) { g_phase = P_REFUSED; g_message = why; g_reloadOnRetry = reload; js_ws_stop(); }

// ---- the map's tiles: fetched by the device itself ----
struct TileSlot { GLuint tex = 0; bool done = false; };
std::vector<TileSlot> g_tileSlots;
std::map<unsigned long long, int> g_tileIndex;
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
        // CARTO's tiles of OpenStreetMap's data (a page served from a home address sends OpenStreetMap's own servers no site's name,
        // which they ask for; CARTO allows it, with the credit the map shows)
        else std::snprintf(url, sizeof url, "https://%c.basemaps.cartocdn.com/rastertiles/voyager/%d/%d/%d.png", "abcd"[(x + y) & 3], z, x, y);
        js_load_tile(k, url, src == 2);
    }
    return 0;
}

// ---- drawing (as the emulator's page) ----
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
GLuint g_logoTex = 0, g_iconsTex = 0, g_wxTex = 0, g_logoMaskTex = 0;
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
    { float from; if (const unsigned char* p = logoMaskPixels(w, h, l, from)) g_logoMaskTex = uploadLevels(p, w, h, l); }
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
            if (cmd.GetTexID() == g_f.logo) tex = g_logoTex; else if (cmd.GetTexID() == g_f.logoMask) tex = g_logoMaskTex; else if (cmd.GetTexID() == g_f.icons) tex = g_iconsTex; else if (cmd.GetTexID() == g_f.wxLogo) tex = g_wxTex;
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

// what this copy does not take from the computer (it has its own screen, its own tiles; pictures are not sent)
void localize() {
    g_s.pixelScale = g_scale;
    setBattery((float)g_s.batteryPct);          // (the tablet's own battery, as the computer has it)
    g_s.onPanel = false; g_s.panelEnabled = false; g_s.aviTabRunning = false;
    g_s.local.status = -2;                      // (pairing is managed in the sim: no Local Connect page here)
    g_s.wxPlane = false; g_s.wxEmbed = 0;       // (WXPlane draws into the sim's tablet only: the built-in Weather page here)
    g_s.map.tile = tile;
    g_s.browser = BrowserState();
    g_s.browser.status = -3;
    g_s.browser.message = "Web pages are shown on the computer's tablet.";
    g_s.docs.tex = 0; g_s.bgTex = 0; g_s.media.art = 0; g_s.media.artPrev = 0;
}

void liveFrame(double now) {
    localize();
    Actions act;
    build(g_f, g_s, g_logW, kLogicalH, act);
    wire::restrictRemote(act, g_s);   // (only what a device may ask is sent at all; the computer checks again)
    const std::string bytes = wire::encodeActions(act);
    // a press goes at once; where the map is and which page is shown, when it changes, ten times a second at most
    if (wire::actionsHaveEvent(act) || (bytes != g_lastActions && now - g_lastActionsAt > 0.1)) {
        send("A" + bytes);
        g_lastActions = bytes; g_lastActionsAt = now;
    }
    if (takeSettingsChanged()) { g_settingsDirty = true; js_store("xptablet.lc.layout", wire::layoutOf(settingsToText()).c_str()); }   // (and its arrangement kept by the browser)
    if (g_settingsDirty && now - g_settingsSentAt > 0.25) { send("T" + settingsToText()); g_settingsDirty = false; g_settingsSentAt = now; }
    if (takeNotesChanged()) g_notesDirty = true;
    if (g_notesDirty && now - g_notesSentAt > 1.0) { send("N" + notesToText()); g_notesDirty = false; g_notesSentAt = now; }
    takeLogbookChanged();   // (the logbook is the computer's)
    static int lastChime = -1;
    const int seq = chimeSeq();
    if (lastChime >= 0 && seq != lastChime && settings().notifyVolume > 0.001f) js_chime(xpt_asset_notify, (int)xpt_asset_notify_size, settings().notifyVolume);
    lastChime = seq;
}

void frame() {
    const double now = emscripten_get_now() / 1000.0;
    const double dt = g_last > 0 ? std::min(0.5, now - g_last) : 1.0 / 60;
    g_last = now;
    if (std::fabs(g_wantScale - g_scale) > 0.01f) { g_scale = g_wantScale; buildFonts(); }
    g_f.logo = (ImTextureID)(intptr_t)1000001; g_f.logoMask = (ImTextureID)(intptr_t)1000004; g_f.icons = (ImTextureID)(intptr_t)1000002; g_f.wxLogo = g_wxTex ? (ImTextureID)(intptr_t)1000003 : 0;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_logW, kLogicalH);
    io.DeltaTime = (float)std::max(1e-4, dt);
    pumpPointer();
    ImGui::NewFrame();
    if (g_phase == P_LIVE) liveFrame(now);
    else {
        const int r = localClientScreen(g_f, g_logW, kLogicalH, g_phase, g_message, g_code);
        if (r == 1 && g_phase == P_PAIR) { std::string m = "P"; putStr(m, g_code); send(m); g_message = "Checking..."; }
        if (r == 2) {
            if (g_reloadOnRetry) js_reload();
            else { g_phase = P_CONNECTING; g_message = "Looking for XPTablet on your computer..."; js_ws_connect(); }
        }
    }
    ImGui::Render();
    int fbW = 0, fbH = 0;
    emscripten_get_canvas_element_size("#canvas", &fbW, &fbH);
    const float bright = g_phase != P_LIVE ? 1.0f : settings().autoDim ? std::max(0.1f, g_s.autoDimLevel) : std::max(0.1f, std::min(1.0f, settings().brightness));
    render(ImGui::GetDrawData(), fbW, fbH, bright);
}

}   // namespace

extern "C" {
// ---- the connection ----
EMSCRIPTEN_KEEPALIVE void lc_open() {
    std::string m = "H";
    putVar(m, (unsigned long long)wire::kVersion);
    char* tok = js_load("xptablet.lc.token");
    putStr(m, tok ? std::string(tok) : std::string());
    if (tok) free(tok);
    char* kind = js_device_kind();
    const std::string k = kind ? kind : "Device";
    if (kind) free(kind);
    putStr(m, k);   // (browsers do not tell a device's own name: its kind stands for it)
    putStr(m, k);
    send(m);
}
EMSCRIPTEN_KEEPALIVE void lc_closed() {
    if (g_phase == P_REFUSED) return;
    g_phase = P_CONNECTING; g_message = "Looking for XPTablet on your computer..."; g_code.clear();
    g_have = 0; g_lastActions.clear(); g_settingsDirty = g_notesDirty = false;
}
EMSCRIPTEN_KEEPALIVE void lc_message(const unsigned char* p, int n) {
    if (n < 1) return;
    const std::string m((const char*)p, (size_t)n);
    const char type = m[0];
    if (type == 'h') {
        if (n < 2) return;
        std::string text;
        size_t at = 2;
        getStr(m, at, text);
        const int st = (unsigned char)m[1];
        if (st == 1) {   // in: the token, when this was the pairing
            if (!text.empty()) js_store("xptablet.lc.token", text.c_str());
            g_code.clear(); g_message = "Loading the tablet..."; g_phase = P_CONNECTING;
            js_battery_again();
        }
        else if (st == 0) { g_phase = P_PAIR; g_code.clear(); g_message.clear(); }
        else if (st == 4) { g_phase = P_PAIR; g_code.clear(); g_message = "That is not the code. Tries left: " + text + "."; }
        else if (st == 2) refuse(text.empty() ? std::string("Reload the page: XPTablet was updated.") : text, true);
        else refuse(text.empty() ? std::string("The computer turned this device away.") : text, false);
        return;
    }
    if (type == 'S') {
        if (n < 2) return;
        const int sec = (unsigned char)m[1];
        if (!wire::decodeSection(g_s, sec, m.substr(2))) { refuse("The computer and this page do not match. Reload the page.", true); return; }
        if (sec == wire::S_CORE) g_have |= 1;
        if (sec == wire::S_MAP_CORE) g_have |= 2;
        goLiveWhenReady();
        return;
    }
    if (type == 's') {   // the computer's settings (not sent back), with this device's own arrangement of the home screen
        const std::string theirs = m.substr(1);
        std::string mine;
        if (g_have & 4) mine = settingsToText();   // (already running: the arrangement as it is now)
        else if (char* saved = js_load("xptablet.lc.layout")) { mine = saved; free(saved); }
        if (wire::layoutOf(mine).empty()) {   // the first time: the computer's arrangement, less what a device cannot show, closed up
            settingsFromText(theirs);
            layoutHomeCompact();
            js_store("xptablet.lc.layout", wire::layoutOf(settingsToText()).c_str());
        } else settingsFromText(wire::settingsWithLayoutOf(theirs, mine));
        takeSettingsChanged(); g_settingsDirty = false; g_have |= 4; goLiveWhenReady();
        return;
    }
    if (type == 'n') { notesFromText(m.substr(1)); takeNotesChanged(); g_notesDirty = false; g_have |= 8; goLiveWhenReady(); return; }
    if (type == 'l') { logbookFromText(m.substr(1)); takeLogbookChanged(); return; }
}
EMSCRIPTEN_KEEPALIVE void lc_battery(int pct, int charging) {
    std::string m = "B";
    m.push_back((char)(std::max(0, std::min(100, pct)) + 1));
    m.push_back((char)(charging ? 1 : 0));
    send(m);
}
// ---- the page's canvas, pointer and keys ----
EMSCRIPTEN_KEEPALIVE void lc_mouse(float x, float y, int down) { queuePointer(x, y, down); }
EMSCRIPTEN_KEEPALIVE void lc_wheel(float dy) { ImGui::GetIO().AddMouseWheelEvent(0, dy); }
EMSCRIPTEN_KEEPALIVE int lc_typing() { return tabletTyping() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void lc_touch(int on) { setTouchInput(on != 0); }
EMSCRIPTEN_KEEPALIVE int lc_physical_keys() { return settings().physicalKeys ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void lc_key(int code) { if (tabletTyping()) tabletKey(code, false); }
EMSCRIPTEN_KEEPALIVE void lc_char(int c) { if (tabletTyping() && c >= 32 && c < 127) tabletKey(c, true); }
EMSCRIPTEN_KEEPALIVE void lc_scale(float s) { g_wantScale = std::max(1.0f, std::min(3.0f, s)); }
EMSCRIPTEN_KEEPALIVE void lc_shape(float aspect) { g_logW = kLogicalH * std::max(0.6f, std::min(2.6f, aspect)); }
EMSCRIPTEN_KEEPALIVE void lc_tile(int key, int tex) { if (key >= 0 && key < (int)g_tileSlots.size()) { g_tileSlots[key].tex = (GLuint)tex; g_tileSlots[key].done = true; --g_tilesInFlight; } }
EMSCRIPTEN_KEEPALIVE int lc_phase() { return g_phase; }   // (for the tests: 0 connecting, 1 pairing, 2 turned away, 3 live)
EMSCRIPTEN_KEEPALIVE void lc_connect() { js_ws_connect(); }
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
    setRemoteClient(true);
    setMapCredit("\xC2\xA9 OpenStreetMap contributors \xC2\xA9 CARTO");
    loadShapes(std::string((const char*)xpt_web_shapes, xpt_web_shapes_size));
    uploadPictures();
    g_scale = g_wantScale = (float)EM_ASM_DOUBLE({ return window.lcScale ? window.lcScale() : 1; });
    buildFonts();
    EM_ASM({ Module.lcConnect = () => Module._lc_connect(); });
    js_ws_connect();
    js_battery_watch();
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
