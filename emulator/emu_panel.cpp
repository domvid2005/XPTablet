// The emulator's control panel: the simulated aircraft and world, every dataref the plugin uses (editable), its commands and menu, and
// its log. Drawn with its own ImGui context (the plugin has its own), into the viewport the caller sets. Anything that calls into the
// plugin (a command, a menu item, one of its datarefs) is done after the panel's frame, with the plugin's context current again.
#include "emu.h"
#include "emugl.h"
#include "imgui.h"
#include "version.h"

#include "XPLMDataAccess.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

extern "C" {
extern const unsigned char xpt_asset_plex_regular[];
extern const unsigned int xpt_asset_plex_regular_size;
extern const unsigned char xpt_asset_plex_semibold[];
extern const unsigned int xpt_asset_plex_semibold_size;
}

namespace emu {
float g_panelFbX = 0;   // where the panel's viewport starts in the framebuffer (pixels), for its scissor rectangles
namespace {

ImGuiContext* g_ctx = nullptr;
ImFont* g_bold = nullptr;
GLuint g_fontTex = 0;
float g_fontScale = 0;
std::vector<std::function<void()>> g_later;   // calls into the plugin, made after the panel's frame
PanelRequests g_req;
char g_filter[128] = "";
bool g_onlyUsed = true;
bool g_logFollow = true;
size_t g_logSeen = 0;

void buildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_regular, (int)xpt_asset_plex_regular_size, 15.0f * scale, &cfg);
    g_bold = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_semibold, (int)xpt_asset_plex_semibold_size, 15.0f * scale, &cfg);
    io.FontGlobalScale = 1.0f / scale;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!g_fontTex) gl.GenTextures(1, &g_fontTex);
    gl.BindTexture(GL_TEXTURE_2D, g_fontTex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
    g_fontScale = scale;
}

// ImGui's triangles with the fixed-function pipeline (the viewport is the panel's area; w x h in its units)
void render(ImDrawData* dd, float w, float h, float px) {
    gl.Enable(GL_BLEND); gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_CULL_FACE); gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_LIGHTING); gl.Disable(GL_ALPHA_TEST);
    gl.Enable(GL_SCISSOR_TEST); gl.Enable(GL_TEXTURE_2D);
    gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.EnableClientState(GL_VERTEX_ARRAY); gl.EnableClientState(GL_TEXTURE_COORD_ARRAY); gl.EnableClientState(GL_COLOR_ARRAY);
    gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity(); gl.Ortho(0, w, h, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vb = cl->VtxBuffer.Data;
        const ImDrawIdx* ib = cl->IdxBuffer.Data;
        for (const ImDrawCmd& c : cl->CmdBuffer) {
            if (c.UserCallback) continue;
            const ImDrawVert* v = vb + c.VtxOffset;
            gl.VertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const char*)v + offsetof(ImDrawVert, pos));
            gl.TexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const char*)v + offsetof(ImDrawVert, uv));
            gl.ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const char*)v + offsetof(ImDrawVert, col));
            const float x0 = c.ClipRect.x, y0 = c.ClipRect.y, x1 = c.ClipRect.z, y1 = c.ClipRect.w;
            if (x1 <= x0 || y1 <= y0) continue;
            gl.Scissor((GLint)(g_panelFbX + x0 * px), (GLint)((h - y1) * px), (GLsizei)((x1 - x0) * px), (GLsizei)((y1 - y0) * px));
            gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)c.GetTexID());
            gl.DrawElements(GL_TRIANGLES, (GLsizei)c.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, ib + c.IdxOffset);
        }
    }
    gl.DisableClientState(GL_VERTEX_ARRAY); gl.DisableClientState(GL_TEXTURE_COORD_ARRAY); gl.DisableClientState(GL_COLOR_ARRAY);
    gl.Disable(GL_SCISSOR_TEST);
}

void heading(const char* t) {
    ImGui::Spacing();
    ImGui::PushFont(g_bold);
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", t);
    ImGui::PopFont();
    ImGui::Separator();
}
void help(const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); ImGui::TextWrapped("%s", t); ImGui::PopStyleColor(); }

struct Airport { const char* icao; double lat, lon; float elev; };
const Airport kAirports[] = {
    {"KSBN", 41.7087, -86.3173, 799}, {"KORD", 41.9786, -87.9048, 680}, {"KDTW", 42.2124, -83.3534, 645}, {"KJFK", 40.6398, -73.7789, 13},
    {"KLAX", 33.9425, -118.4081, 125}, {"KDFW", 32.8968, -97.0380, 607}, {"PHNL", 21.3187, -157.9225, 13}, {"EGLL", 51.4706, -0.4619, 83},
    {"LFPG", 49.0097, 2.5479, 392}, {"EDDF", 50.0333, 8.5706, 364}, {"LSZH", 47.4647, 8.5492, 1416}, {"RJTT", 35.5523, 139.7798, 35},
};

void aircraftTab() {
    Sim& s = sim();
    heading("Aircraft");
    const auto& ps = profiles();
    if (ImGui::BeginCombo("Aircraft", ps[s.profile].name.c_str())) {
        for (int i = 0; i < (int)ps.size(); ++i) if (ImGui::Selectable(ps[i].name.c_str(), i == s.profile)) s.profile = i;
        ImGui::EndCombo();
    }
    char ic[16];
    std::snprintf(ic, sizeof ic, "%s", s.icaoOverride.c_str());
    if (ImGui::InputTextWithHint("ICAO type", ps[s.profile].icao.c_str(), ic, sizeof ic)) s.icaoOverride = ic;
    ImGui::Checkbox("Cockpit tablet (the aircraft has an AviTab.json)", &s.cockpitTablet);
    if (s.cockpitTablet) {
        int v = view() == View::Cockpit ? 1 : 0;
        ImGui::TextUnformatted("Show:"); ImGui::SameLine();
        if (ImGui::RadioButton("Window", v == 0)) setView(View::Window); ImGui::SameLine();
        if (ImGui::RadioButton("Cockpit panel", v == 1)) setView(View::Cockpit);
    } else if (view() == View::Cockpit) setView(View::Window);

    heading("Position");
    for (int i = 0; i < (int)(sizeof kAirports / sizeof kAirports[0]); ++i) {
        if (i % 6) ImGui::SameLine();
        if (ImGui::SmallButton(kAirports[i].icao)) { s.lat = kAirports[i].lat; s.lon = kAirports[i].lon; s.altFt = kAirports[i].elev; s.aglFt = 0; s.onGround = true; s.gsKt = 0; s.flying = false; simResetTraffic(); }
    }
    ImGui::InputDouble("Latitude", &s.lat, 0.001, 0.01, "%.5f");
    ImGui::InputDouble("Longitude", &s.lon, 0.001, 0.01, "%.5f");
    ImGui::SliderFloat("Heading (true)", &s.hdg, 0, 359.9f, "%.0f\xC2\xB0");
    ImGui::Checkbox("On the ground", &s.onGround); ImGui::SameLine(); ImGui::Checkbox("Moving", &s.flying);
    if (!s.onGround) {
        ImGui::SliderFloat("Altitude (ft MSL)", &s.altFt, -100, 45000, "%.0f");
        ImGui::SliderFloat("Height above ground (ft)", &s.aglFt, 0, 45000, "%.0f");
        ImGui::SliderFloat("Vertical speed (fpm)", &s.vsFpm, -4000, 4000, "%.0f");
    }
    ImGui::SliderFloat("Ground speed (kt)", &s.gsKt, 0, 520, "%.0f");
    if (ImGui::Button("Take off: 3000 ft above, 250 kt")) { s.onGround = false; s.aglFt = 3000; s.altFt += 3000; s.gsKt = 250; s.vsFpm = 0; s.flying = true; }

    heading("Systems");
    ImGui::Checkbox("Battery", &s.battery); ImGui::SameLine(); ImGui::Checkbox("Engines", &s.engines); ImGui::SameLine(); ImGui::Checkbox("Beacon", &s.beacon);
    ImGui::Checkbox("Parking brake", &s.parkBrake); ImGui::SameLine(); ImGui::Checkbox("Chocks", &s.chocks);

    heading("Other plugins");
    bool changed = false;
    changed |= ImGui::Checkbox("WXPlane", &s.wxPlane); ImGui::SameLine();
    changed |= ImGui::Checkbox("OpenSAM", &s.openSam); ImGui::SameLine();
    changed |= ImGui::Checkbox("AviTab", &s.aviTab);
    if (changed) g_later.push_back([] { simApplyProfile(); });
    help("WXPlane and AviTab are stand-ins: they are found by signature but do not draw. AviTab is noticed when the plugin starts: reload it.");
}

void trafficTab() {
    Sim& s = sim();
    heading("Traffic (X-Plane's TCAS targets)");
    ImGui::SliderInt("Aircraft", &s.trafficCount, 0, 60);
    ImGui::SliderFloat("Within (nm)", &s.trafficRangeNm, 1, 60, "%.0f");
    if (ImGui::Button("New traffic")) simResetTraffic();
    ImGui::SameLine();
    if (ImGui::Button("TCAS RA intruder (head-on, 0.6 nm, +200 ft)")) simAddIntruder();
    help("Traffic moves along its heading; aircraft that wander far away are replaced.");
    if (ImGui::BeginTable("##tfc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Call sign"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Alt"); ImGui::TableSetupColumn("Hdg");
        ImGui::TableSetupColumn("GS"); ImGui::TableSetupColumn("VS");
        ImGui::TableHeadersRow();
        for (const TrafficAc& t : traffic()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(t.callsign.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(t.type.c_str());
            ImGui::TableNextColumn(); if (t.onGround) ImGui::TextUnformatted("ground"); else ImGui::Text("%.0f", t.altFt);
            ImGui::TableNextColumn(); ImGui::Text("%03.0f", t.hdg);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", t.gsKt);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f", t.vsFpm);
        }
        ImGui::EndTable();
    }
}

void worldTab() {
    Sim& s = sim();
    heading("Time");
    bool real = s.zuluHour < 0;
    if (ImGui::Checkbox("The real time now", &real)) s.zuluHour = real ? -1 : 12;
    if (!real) ImGui::SliderFloat("Zulu time (hours)", &s.zuluHour, 0, 23.99f, "%.2f");
    ImGui::Checkbox("Sun from the time and place", &s.autoSun);
    if (!s.autoSun) ImGui::SliderFloat("Sun above the horizon (\xC2\xB0)", &s.sunPitch, -30, 70, "%.0f");
    else ImGui::Text("Sun: %.1f\xC2\xB0 above the horizon", s.sunPitch);
    heading("Weather at the aircraft");
    ImGui::SliderFloat("Wind from (\xC2\xB0T)", &s.windDir, 0, 359, "%.0f");
    ImGui::SliderFloat("Wind (kt)", &s.windKt, 0, 80, "%.0f");
    ImGui::SliderFloat("Visibility (sm)", &s.visSm, 0, 20, "%.1f");
    ImGui::SliderFloat("Temperature (\xC2\xB0""C)", &s.tempC, -50, 45, "%.0f");
    ImGui::SliderFloat("Dew point (\xC2\xB0""C)", &s.dewC, -50, 35, "%.0f");
    ImGui::SliderFloat("QNH (hPa)", &s.qnhHpa, 950, 1050, "%.0f");
    ImGui::SliderFloat("Rain", &s.rain, 0, 1, "%.2f");
    ImGui::SliderInt("Cloud layers", &s.clouds, 0, 3);
    if (s.clouds > 0) {
        ImGui::SliderFloat("Lowest base (ft)", &s.cloudBaseFt, 0, 30000, "%.0f");
        ImGui::SliderFloat("Its top (ft)", &s.cloudTopFt, 0, 40000, "%.0f");
        ImGui::SliderFloat("Cover", &s.cloudCover, 0, 1, "%.2f");
    }
}

std::string arrayText(const Dataref* d) {
    std::string o;
    char b[32];
    const size_t n = !d->vf.empty() ? d->vf.size() : d->vi.size();
    for (size_t i = 0; i < n && i < 8; ++i) {
        if (!d->vf.empty()) std::snprintf(b, sizeof b, "%s%.4g", i ? ", " : "", d->vf[i]); else std::snprintf(b, sizeof b, "%s%d", i ? ", " : "", d->vi[i]);
        o += b;
    }
    if (n > 8) o += ", ...";
    return "[" + std::to_string(n) + "] " + o;
}

void datarefsTab() {
    heading("Datarefs");
    ImGui::InputTextWithHint("##filter", "Filter (e.g. tcas, AirbusFBW, xptablet/)", g_filter, sizeof g_filter);
    ImGui::SameLine();
    ImGui::Checkbox("Only the plugin's", &g_onlyUsed);
    help("Values the simulated aircraft writes (position, time, weather, traffic) are written again every frame.");
    if (ImGui::BeginTable("##refs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.72f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (Dataref* d : allDatarefs()) {
            if (g_onlyUsed && !d->touched && !d->fromPlugin) continue;
            if (g_filter[0] && d->name.find(g_filter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (d->fromPlugin) ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1), "%s", d->name.c_str()); else ImGui::TextUnformatted(d->name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(d);
            ImGui::SetNextItemWidth(-1);
            if (d->fromPlugin) {   // the plugin's own: through its accessors (after the frame, in its context)
                XPLMDataRef r = (XPLMDataRef)d;
                if (d->getf) { float v = XPLMGetDataf(r); if (ImGui::InputFloat("##v", &v, 0, 0, "%.4g", d->writable ? 0 : ImGuiInputTextFlags_ReadOnly)) g_later.push_back([r, v] { XPLMSetDataf(r, v); }); }
                else if (d->geti) { int v = XPLMGetDatai(r); if (ImGui::InputInt("##v", &v, 0, 0, d->writable ? 0 : ImGuiInputTextFlags_ReadOnly)) g_later.push_back([r, v] { XPLMSetDatai(r, v); }); }
                else if (d->getd) { double v = XPLMGetDatad(r); if (ImGui::InputDouble("##v", &v, 0, 0, "%.6g", d->writable ? 0 : ImGuiInputTextFlags_ReadOnly)) g_later.push_back([r, v] { XPLMSetDatad(r, v); }); }
                else ImGui::TextDisabled("(array or data)");
            } else if (!d->bytes.empty()) {
                std::string t;
                for (char c : d->bytes) { if (c == 0) { if (!t.empty() && t.back() != ' ') t += ' '; } else t += c; }
                ImGui::TextUnformatted(t.substr(0, 80).c_str());
            } else if (!d->vf.empty() || !d->vi.empty()) {
                if (ImGui::TreeNode("##arr", "%s", arrayText(d).c_str())) {
                    const size_t n = std::min<size_t>(16, !d->vf.empty() ? d->vf.size() : d->vi.size());
                    for (size_t i = 0; i < n; ++i) {
                        ImGui::PushID((int)i);
                        ImGui::SetNextItemWidth(-1);
                        if (!d->vf.empty()) ImGui::InputFloat("##e", &d->vf[i], 0, 0, "%.4g"); else ImGui::InputInt("##e", &d->vi[i], 0, 0);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            } else {
                ImGui::InputDouble("##v", &d->value, 0, 0, "%.6g");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!missingLookups().empty() && ImGui::CollapsingHeader("Looked for, but not in this X-Plane / aircraft")) {
        for (const std::string& m : missingLookups()) if (!g_filter[0] || m.find(g_filter) != std::string::npos) ImGui::BulletText("%s", m.c_str());
    }
}

void commandsTab() {
    heading("Plugins menu");
    for (const MenuItem& m : menuItems()) {
        ImGui::PushID(m.path.c_str());
        if (ImGui::Button(m.path.c_str())) { const MenuItem mm = m; g_later.push_back([mm] { pickMenuItem(mm); }); }
        ImGui::PopID();
    }
    heading("The plugin's commands");
    if (ImGui::BeginTable("##cmds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Command");
        for (const Command& c : commands()) {
            if (!c.fromPlugin) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(c.name.c_str());
            if (ImGui::SmallButton("Run")) { const std::string n = c.name; g_later.push_back([n] { runCommand(n); }); }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%s", c.name.c_str());
            if (!c.description.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", c.description.c_str()); }
        }
        ImGui::EndTable();
    }
    heading("Commands the plugin sent");
    const auto& cl = commandLog();
    if (cl.empty()) help("None yet (the Ground page sends jetway, GPU and pushback commands).");
    for (size_t i = cl.size() > 12 ? cl.size() - 12 : 0; i < cl.size(); ++i) ImGui::BulletText("%s", cl[i].c_str());
}

void logTab() {
    heading("Log (X-Plane's Log.txt lines)");
    ImGui::Checkbox("Follow", &g_logFollow);
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const auto& L = logLines();
    for (const std::string& l : L) {
        const bool emu = l.compare(0, 10, "[emulator]") == 0 || l.compare(0, 9, "[X-Plane]") == 0;
        if (emu) ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.85f, 1), "%s", l.c_str()); else ImGui::TextUnformatted(l.c_str());
    }
    if (g_logFollow && L.size() != g_logSeen) ImGui::SetScrollHereY(1.0f);
    g_logSeen = L.size();
    ImGui::EndChild();
}

} // namespace

PanelRequests& panelRequests() { return g_req; }

void panelInit(float px) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 5; st.GrabRounding = 5; st.TabRounding = 5;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.14f, 1);
    buildFonts(std::max(1.0f, px));
    ImGui::SetCurrentContext(prev);
}

void panelFrame(float w, float h, float px, double dt) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    if (std::fabs(px - g_fontScale) > 0.01f) buildFonts(std::max(1.0f, px));
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = (float)std::max(1e-4, std::min(0.5, dt));
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(g_bold);
    ImGui::Text("XPTablet emulator");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("plugin " XPT_VERSION);
    if (ImGui::Button("Show / hide tablet")) g_later.push_back([] { for (const MenuItem& m : menuItems()) if (m.path.find("Show / hide") != std::string::npos) { pickMenuItem(m); break; } });
    ImGui::SameLine();
    if (ImGui::Button("Reload plugin")) g_later.push_back([] { restartPlugin(); });
    ImGui::SameLine();
    if (ImGui::Button("Screenshot")) g_req.screenshot = true;
    ImGui::TextDisabled("X-Plane folder: %s", paths().xplaneRoot.c_str());
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Aircraft")) { ImGui::BeginChild("##a"); aircraftTab(); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Traffic")) { trafficTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { ImGui::BeginChild("##w"); worldTab(); ImGui::EndChild(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Datarefs")) { datarefsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Commands")) { commandsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) { logTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::Render();
    render(ImGui::GetDrawData(), w, h, px);
    ImGui::SetCurrentContext(prev);
    // now the calls into the plugin, in its own context
    std::vector<std::function<void()>> later;
    later.swap(g_later);
    for (auto& f : later) f();
}

void panelMouse(float x, float y, int button, bool down, bool moved, float wheel) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    if (moved) io.AddMousePosEvent(x, y);
    if (button >= 0) io.AddMouseButtonEvent(button, down);
    if (wheel != 0) io.AddMouseWheelEvent(0, wheel);
    ImGui::SetCurrentContext(prev);
}
void panelChar(unsigned int c) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGui::GetIO().AddInputCharacter(c);
    ImGui::SetCurrentContext(prev);
}
void panelKey(int key, bool down) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGui::GetIO().AddKeyEvent((ImGuiKey)key, down);
    ImGui::SetCurrentContext(prev);
}
bool panelWantsKeys() {
    if (!g_ctx) return false;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    const bool w = ImGui::GetIO().WantTextInput;
    ImGui::SetCurrentContext(prev);
    return w;
}

} // namespace emu
