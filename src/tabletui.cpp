#include "tabletui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "version.h"

extern "C" {
extern const unsigned char xpt_asset_plex_regular[];
extern const unsigned int xpt_asset_plex_regular_size;
extern const unsigned char xpt_asset_plex_semibold[];
extern const unsigned int xpt_asset_plex_semibold_size;
extern const unsigned char xpt_asset_plex_bold[];
extern const unsigned int xpt_asset_plex_bold_size;
extern const unsigned char xpt_asset_mono[];
extern const unsigned int xpt_asset_mono_size;
}

namespace xpt {
namespace {

const float kStatusH = 28.0f, kHeaderH = 46.0f;
const float kPi = 3.14159265f;

struct AppDef { const char* name; ImU32 color; };
const AppDef kApps[APP_COUNT] = {
    {"Map", IM_COL32(52, 168, 110, 255)},        {"Airports", IM_COL32(64, 132, 230, 255)},  {"Charts", IM_COL32(236, 152, 50, 255)},
    {"Documents", IM_COL32(120, 132, 156, 255)}, {"Routes", IM_COL32(150, 96, 250, 255)},    {"Weather", IM_COL32(40, 180, 214, 255)},
    {"Browser", IM_COL32(72, 110, 240, 255)},    {"Ground", IM_COL32(246, 190, 48, 255)},    {"Checklists", IM_COL32(60, 190, 130, 255)},
    {"Performance", IM_COL32(236, 92, 104, 255)}, {"Logbook", IM_COL32(170, 122, 84, 255)},  {"Traffic", IM_COL32(40, 190, 170, 255)},
    {"Notes", IM_COL32(244, 206, 80, 255)},      {"Settings", IM_COL32(110, 120, 140, 255)},
};

int g_app = APP_HOME;
float g_px = 1.0f;   // screen pixels per layout unit this frame

// ImGui anti-aliases with an edge one layout unit wide. The tablet is drawn at several pixels per unit, where that edge would be a
// blur several pixels wide: this makes it one screen pixel again, so lines, circles and rounded corners stay crisp.
void sharp(ImDrawList* dl) { dl->_FringeScale = 1.0f / g_px; }
float snap(float v) { return std::floor(v * g_px + 0.5f) / g_px; }   // to the nearest screen pixel
std::vector<ImVec2> g_touches;   // the touch test (Settings): where the screen was pressed

ImU32 shade(ImU32 c, float k) {   // brighter (k > 1) or darker
    auto ch = [&](int s) { return (ImU32)std::min(255.0f, ((c >> s) & 0xFF) * k) << s; };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

void textCentered(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* txt) {
    const float fs = f->FontSize * ImGui::GetIO().FontGlobalScale;
    const ImVec2 sz = f->CalcTextSizeA(fs, 1e9f, 0, txt);
    dl->AddText(f, fs, ImVec2(std::floor(cx - sz.x * 0.5f), y), col, txt);
}
float fontPx(ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

// The default wallpaper: a navy sky that lightens downwards, with a mountain range along the bottom edge.
void wallpaper(ImDrawList* dl, float w, float h) {
    const ImU32 top = IM_COL32(9, 22, 44, 255), bot = IM_COL32(24, 58, 104, 255);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), top, top, bot, bot);
    const float back[][2] = {{0.00f, 0.80f}, {0.12f, 0.62f}, {0.22f, 0.74f}, {0.36f, 0.50f}, {0.50f, 0.72f}, {0.62f, 0.56f}, {0.76f, 0.76f}, {0.88f, 0.60f}, {1.00f, 0.78f}};
    const float front[][2] = {{0.00f, 0.92f}, {0.10f, 0.80f}, {0.24f, 0.90f}, {0.42f, 0.70f}, {0.56f, 0.88f}, {0.70f, 0.76f}, {0.84f, 0.90f}, {1.00f, 0.80f}};
    auto range = [&](const float (*p)[2], int n, ImU32 col) {
        for (int i = 0; i + 1 < n; ++i) {
            const ImVec2 a(p[i][0] * w, p[i][1] * h), b(p[i + 1][0] * w, p[i + 1][1] * h);
            dl->AddQuadFilled(a, b, ImVec2(b.x, h), ImVec2(a.x, h), col);
        }
    };
    range(back, 9, IM_COL32(20, 46, 86, 255));
    range(front, 8, IM_COL32(14, 34, 66, 255));
}

// A small white pictogram for each app, drawn from lines and circles inside a circle of radius r around c.
void icon(ImDrawList* dl, int app, ImVec2 c, float r, ImU32 col) {
    c = ImVec2(snap(c.x), snap(c.y));
    const float t = snap(std::max(2.0f, r * 0.15f));   // a whole number of screen pixels: no half-covered edges
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (app) {
    case APP_MAP:   // a location pin
        dl->AddCircle(P(0, -0.25f), r * 0.50f, col, 24, t);
        dl->AddCircleFilled(P(0, -0.25f), r * 0.17f, col, 12);
        { const ImVec2 pts[3] = {P(-0.42f, 0.05f), P(0, 0.85f), P(0.42f, 0.05f)}; dl->AddPolyline(pts, 3, col, 0, t); }
        break;
    case APP_AIRPORTS:   // a runway seen from above
        dl->AddRect(P(-0.32f, -0.85f), P(0.32f, 0.85f), col, r * 0.08f, 0, t);
        for (int i = 0; i < 4; ++i) dl->AddLine(P(0, -0.60f + i * 0.36f), P(0, -0.42f + i * 0.36f), col, t);
        break;
    case APP_CHARTS:   // an approach plate: a sheet with a profile line
        dl->AddRect(P(-0.60f, -0.80f), P(0.60f, 0.80f), col, r * 0.10f, 0, t);
        dl->AddLine(P(-0.36f, -0.42f), P(0.36f, -0.42f), col, t);
        { const ImVec2 pts[3] = {P(-0.36f, 0.00f), P(0.00f, 0.00f), P(0.36f, 0.44f)}; dl->AddPolyline(pts, 3, col, 0, t); }
        break;
    case APP_DOCUMENTS:   // a folder
        dl->AddRect(P(-0.80f, -0.40f), P(0.80f, 0.64f), col, r * 0.10f, 0, t);
        { const ImVec2 pts[4] = {P(-0.80f, -0.40f), P(-0.62f, -0.66f), P(-0.14f, -0.66f), P(0.04f, -0.40f)}; dl->AddPolyline(pts, 4, col, 0, t); }
        break;
    case APP_ROUTES:   // waypoints joined by legs
        dl->AddLine(P(-0.66f, 0.56f), P(-0.10f, -0.20f), col, t);
        dl->AddLine(P(-0.10f, -0.20f), P(0.66f, -0.56f), col, t);
        dl->AddCircleFilled(P(-0.66f, 0.56f), r * 0.18f, col, 12);
        dl->AddCircleFilled(P(-0.10f, -0.20f), r * 0.18f, col, 12);
        dl->AddCircleFilled(P(0.66f, -0.56f), r * 0.18f, col, 12);
        break;
    case APP_WEATHER:   // a cloud
        dl->AddCircleFilled(P(-0.40f, 0.14f), r * 0.34f, col, 20);
        dl->AddCircleFilled(P(0.02f, -0.14f), r * 0.46f, col, 24);
        dl->AddCircleFilled(P(0.46f, 0.14f), r * 0.32f, col, 20);
        dl->AddRectFilled(P(-0.40f, 0.10f), P(0.46f, 0.47f), col);
        break;
    case APP_BROWSER:   // a globe
        dl->AddCircle(c, r * 0.80f, col, 28, t);
        dl->AddEllipse(c, ImVec2(r * 0.36f, r * 0.80f), col, 0, 24, t);
        dl->AddLine(P(-0.80f, 0), P(0.80f, 0), col, t);
        break;
    case APP_GROUND:   // an aircraft from above
        dl->AddLine(P(0, -0.86f), P(0, 0.80f), col, t * 1.7f);
        dl->AddTriangleFilled(P(0, -0.26f), P(-0.90f, 0.30f), P(0, 0.22f), col);
        dl->AddTriangleFilled(P(0, -0.26f), P(0.90f, 0.30f), P(0, 0.22f), col);
        dl->AddTriangleFilled(P(0, 0.50f), P(-0.38f, 0.86f), P(0.38f, 0.86f), col);
        break;
    case APP_CHECKLISTS:   // a tick
        { const ImVec2 pts[3] = {P(-0.66f, 0.04f), P(-0.20f, 0.52f), P(0.70f, -0.52f)}; dl->AddPolyline(pts, 3, col, 0, t * 1.5f); }
        break;
    case APP_PERFORMANCE:   // a gauge
        dl->PathArcTo(P(0, 0.30f), r * 0.80f, kPi, 2 * kPi, 24);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(0, 0.30f), P(0.42f, -0.26f), col, t * 1.3f);
        dl->AddCircleFilled(P(0, 0.30f), r * 0.13f, col, 12);
        break;
    case APP_LOGBOOK:   // an open book
        dl->AddRect(P(-0.82f, -0.60f), P(0.82f, 0.60f), col, r * 0.08f, 0, t);
        dl->AddLine(P(0, -0.60f), P(0, 0.60f), col, t);
        dl->AddLine(P(-0.56f, -0.24f), P(-0.26f, -0.24f), col, t);
        dl->AddLine(P(-0.56f, 0.10f), P(-0.26f, 0.10f), col, t);
        dl->AddLine(P(0.26f, -0.24f), P(0.56f, -0.24f), col, t);
        dl->AddLine(P(0.26f, 0.10f), P(0.56f, 0.10f), col, t);
        break;
    case APP_TRAFFIC:   // a radar scope
        dl->AddCircle(c, r * 0.82f, col, 28, t);
        dl->AddCircle(c, r * 0.42f, col, 20, t);
        dl->AddLine(c, P(0.58f, -0.58f), col, t);
        dl->AddCircleFilled(P(-0.34f, -0.48f), r * 0.11f, col, 10);
        dl->AddCircleFilled(P(0.30f, 0.52f), r * 0.11f, col, 10);
        break;
    case APP_NOTES:   // a page with lines
        dl->AddRect(P(-0.62f, -0.80f), P(0.62f, 0.80f), col, r * 0.10f, 0, t);
        for (int i = 0; i < 3; ++i) dl->AddLine(P(-0.36f, -0.40f + i * 0.40f), P(0.36f, -0.40f + i * 0.40f), col, t);
        break;
    case APP_SETTINGS:   // a cog
        dl->AddCircle(c, r * 0.44f, col, 24, t * 1.3f);
        for (int i = 0; i < 8; ++i) {
            const float a = i * kPi / 4;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.54f, c.y + std::sin(a) * r * 0.54f), ImVec2(c.x + std::cos(a) * r * 0.86f, c.y + std::sin(a) * r * 0.86f), col, t * 1.5f);
        }
        break;
    default: break;
    }
}

void statusBar(ImDrawList* dl, const Fonts& f, const SimState& s, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kStatusH), IM_COL32(0, 0, 0, 90));
    const float y = std::floor((kStatusH - fontPx(f.small)) * 0.5f);
    const ImU32 col = IM_COL32(232, 238, 248, 255);
    dl->AddText(f.small, fontPx(f.small), ImVec2(12, y), col, "XPTablet");
    char buf[32] = "--:--Z";
    if (s.zuluSec >= 0) std::snprintf(buf, sizeof buf, "%02d:%02dZ", (s.zuluSec / 3600) % 24, (s.zuluSec / 60) % 60);
    textCentered(dl, f.small, w * 0.5f, y, col, buf);
    if (!s.acfIcao.empty()) {
        const ImVec2 sz = f.small->CalcTextSizeA(fontPx(f.small), 1e9f, 0, s.acfIcao.c_str());
        dl->AddText(f.small, fontPx(f.small), ImVec2(w - 12 - sz.x, y), col, s.acfIcao.c_str());
    }
}

void home(ImDrawList* dl, const Fonts& f, float w, float h) {
    const float cellW = 104, cellH = 108, tile = 62;
    const int cols = std::max(1, std::min((int)APP_COUNT, (int)((w - 24) / cellW)));
    const int rows = (APP_COUNT + cols - 1) / cols;
    const float x0 = std::floor((w - cols * cellW) * 0.5f);
    const float y0 = kStatusH + std::max(10.0f, std::floor((h - kStatusH - rows * cellH) * 0.5f));
    for (int i = 0; i < APP_COUNT; ++i) {
        const float cx = x0 + (i % cols) * cellW, cy = y0 + (i / cols) * cellH;
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("app", ImVec2(cellW, cellH));
        const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
        ImGui::PopID();
        const float k = down ? 0.94f : 1.0f;   // the tile gives a little under the finger
        const ImVec2 c(cx + cellW * 0.5f, cy + 8 + tile * 0.5f);
        const ImVec2 a(c.x - tile * 0.5f * k, c.y - tile * 0.5f * k), b(c.x + tile * 0.5f * k, c.y + tile * 0.5f * k);
        const ImU32 base = kApps[i].color;
        dl->AddRectFilled(a, b, shade(base, down ? 0.86f : hot ? 1.12f : 1.0f), 15.0f * k);
        icon(dl, i, c, tile * 0.32f * k, IM_COL32(255, 255, 255, 255));
        textCentered(dl, f.small, c.x, cy + 8 + tile + 6, IM_COL32(236, 241, 250, 255), kApps[i].name);
        if (clicked) g_app = i;
    }
}

bool pageHeader(ImDrawList* dl, const Fonts& f, float w, const char* title) {
    dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, kStatusH + kHeaderH), IM_COL32(10, 24, 48, 235));
    dl->AddLine(ImVec2(0, kStatusH + kHeaderH), ImVec2(w, kStatusH + kHeaderH), IM_COL32(255, 255, 255, 28));
    ImGui::SetCursorScreenPos(ImVec2(0, kStatusH));
    const bool back = ImGui::InvisibleButton("##home", ImVec2(96, kHeaderH));
    const ImU32 col = ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 190, 255, 255);
    const float my = kStatusH + kHeaderH * 0.5f;
    { const ImVec2 pts[3] = {ImVec2(22, my - 8), ImVec2(14, my), ImVec2(22, my + 8)}; dl->AddPolyline(pts, 3, col, 0, 2.0f); }
    dl->AddText(f.body, fontPx(f.body), ImVec2(30, std::floor(my - fontPx(f.body) * 0.5f)), col, "Home");
    textCentered(dl, f.title, w * 0.5f, std::floor(my - fontPx(f.title) * 0.5f), IM_COL32(245, 248, 255, 255), title);
    return back;
}

void placeholderPage(ImDrawList* dl, const Fonts& f, int app, float w, float h) {
    const ImVec2 c(w * 0.5f, kStatusH + kHeaderH + (h - kStatusH - kHeaderH) * 0.42f);
    dl->AddRectFilled(ImVec2(c.x - 48, c.y - 48), ImVec2(c.x + 48, c.y + 48), kApps[app].color, 22.0f);
    icon(dl, app, c, 30, IM_COL32(255, 255, 255, 255));
    textCentered(dl, f.body, c.x, c.y + 64, IM_COL32(210, 220, 236, 255), "This app arrives in a later test build.");
}

void settingsPage(ImDrawList* dl, const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    const float top = kStatusH + kHeaderH;
    ImGui::SetCursorScreenPos(ImVec2(18, top + 14));
    ImGui::BeginChild("##settings", ImVec2(std::min(360.0f, w * 0.5f - 24), h - top - 28), 0, ImGuiWindowFlags_NoBackground);
    sharp(ImGui::GetWindowDrawList());
    ImGui::PushFont(f.bold); ImGui::TextUnformatted("Display"); ImGui::PopFont();
    float br = s.brightness;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##brightness", &br, 0.1f, 1.0f, "Brightness %.2f")) { act.setBrightness = true; act.brightness = br; }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushFont(f.bold); ImGui::TextUnformatted("Status"); ImGui::PopFont();
    ImGui::PushFont(f.mono);
    ImGui::Text("Version      %s", XPT_VERSION);
    ImGui::Text("Aircraft     %s", s.acfIcao.empty() ? "-" : s.acfIcao.c_str());
    ImGui::Text("Shown on     %s", s.onPanel ? "cockpit tablet" : "window only");
    if (s.panel[2] > 0 && s.panel[3] > 0) ImGui::Text("Panel area   %d,%d  %dx%d", s.panel[0], s.panel[1], s.panel[2], s.panel[3]);
    else ImGui::TextUnformatted("Panel area   none (no AviTab slot)");
    ImGui::Text("Enabled      %s   Powered  %s", s.panelEnabled ? "yes" : "no", s.panelPowered ? "yes" : "no");
    ImGui::Text("Pixel scale  %.2f", s.pixelScale);
    ImGui::Text("WXPlane      %s", s.wxPlane ? "found" : "not installed");
    ImGui::PopFont();
    if (s.aviTabRunning) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 190, 90, 255));
        ImGui::TextWrapped("AviTab is also installed, so the cockpit tablet still belongs to it. Remove AviTab from Resources/plugins to let XPTablet take its place.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // touch test: press anywhere in the box, a mark must appear exactly under the pointer
    const ImVec2 a(std::floor(w * 0.5f) + 6, top + 14), b(w - 18, h - 14);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 80), 10.0f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 40), 10.0f);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##touch", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (g_touches.empty() || std::fabs(g_touches.back().x - m.x) + std::fabs(g_touches.back().y - m.y) > 3) g_touches.push_back(m);
        if (g_touches.size() > 400) g_touches.erase(g_touches.begin());
    }
    dl->PushClipRect(a, b, true);
    for (const ImVec2& p : g_touches) dl->AddCircleFilled(p, 4.0f, IM_COL32(255, 184, 64, 220), 10);
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        dl->AddLine(ImVec2(m.x - 12, m.y), ImVec2(m.x + 12, m.y), IM_COL32(255, 255, 255, 200));
        dl->AddLine(ImVec2(m.x, m.y - 12), ImVec2(m.x, m.y + 12), IM_COL32(255, 255, 255, 200));
    }
    dl->PopClipRect();
    textCentered(dl, f.small, (a.x + b.x) * 0.5f, a.y + 8, IM_COL32(200, 212, 232, 255), g_touches.empty() ? "Touch test: press and drag here" : "Touch test");
    ImGui::SetCursorScreenPos(ImVec2(b.x - 70, b.y - 34));
    if (!g_touches.empty() && ImGui::Button("Clear", ImVec2(60, 26))) g_touches.clear();
}

} // namespace

Fonts loadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the data is static
    cfg.OversampleH = 2; cfg.OversampleV = 1;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0};
    const float s = std::max(1.0f, pixelScale);
    Fonts f;
    f.body = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_regular, (int)xpt_asset_plex_regular_size, 15.0f * s, &cfg, ranges);
    f.bold = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_semibold, (int)xpt_asset_plex_semibold_size, 15.0f * s, &cfg, ranges);
    f.small = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_semibold, (int)xpt_asset_plex_semibold_size, 12.5f * s, &cfg, ranges);
    f.title = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_plex_bold, (int)xpt_asset_plex_bold_size, 19.0f * s, &cfg, ranges);
    f.mono = io.Fonts->AddFontFromMemoryTTF((void*)xpt_asset_mono, (int)xpt_asset_mono_size, 12.5f * s, &cfg, ranges);
    io.FontDefault = f.body;
    io.FontGlobalScale = 1.0f / s;
    return f;
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.WindowBorderSize = 0; st.FrameRounding = 8; st.GrabRounding = 8; st.ChildRounding = 8;
    st.FramePadding = ImVec2(10, 8);       // finger-sized controls
    st.ItemSpacing = ImVec2(8, 7);
    st.GrabMinSize = 26;
    st.ScrollbarSize = 16;
    st.AntiAliasedLinesUseTex = false;   // the baked line textures are made for one pixel per unit and blur when scaled
    st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.16f);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.22f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.66f, 1.0f, 1);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.78f, 1.0f, 1);
    st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.12f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.20f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.28f);
}

void build(const Fonts& f, const SimState& s, float w, float h, Actions& act) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##tablet", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    g_px = std::max(1.0f, s.pixelScale);
    ImGuiStyle& st = ImGui::GetStyle();
    st.CircleTessellationMaxError = 0.30f / g_px;   // curves are as smooth on a dense screen as on a plain one
    st.CurveTessellationTol = 1.25f / g_px;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    sharp(dl);
    wallpaper(dl, w, h);
    statusBar(dl, f, s, w);
    if (g_app < 0 || g_app >= APP_COUNT) {
        home(dl, f, w, h);
    } else {
        dl->AddRectFilled(ImVec2(0, kStatusH), ImVec2(w, h), IM_COL32(8, 18, 36, 170));   // pages sit on a darkened wallpaper
        const int app = g_app;
        if (pageHeader(dl, f, w, kApps[app].name)) g_app = APP_HOME;
        if (app == APP_SETTINGS) settingsPage(dl, f, s, w, h, act);
        else placeholderPage(dl, f, app, w, h);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

int currentApp() { return g_app; }
void openApp(int app) { g_app = (app >= 0 && app < APP_COUNT) ? app : (int)APP_HOME; }
const char* appName(int app) { return (app >= 0 && app < APP_COUNT) ? kApps[app].name : "Home"; }

} // namespace xpt
