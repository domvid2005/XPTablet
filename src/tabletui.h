// The tablet's user interface, drawn with Dear ImGui and nothing else: no X-Plane calls here, so the same code runs inside the
// plugin and in the off-line picture test. The host (src/plugin.cpp) gives it the sim's state and gets the user's requests back.
#pragma once

#include <string>

#include "imgui.h"

namespace xpt {

constexpr float kLogicalH = 480.0f;   // the tablet is always laid out 480 units tall; its width follows the screen's shape

enum App { APP_HOME = -1, APP_MAP, APP_AIRPORTS, APP_CHARTS, APP_DOCUMENTS, APP_ROUTES, APP_WEATHER, APP_BROWSER, APP_GROUND,
           APP_CHECKLISTS, APP_PERFORMANCE, APP_LOGBOOK, APP_TRAFFIC, APP_NOTES, APP_SETTINGS, APP_COUNT };

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* mono = nullptr;
};

struct SimState {
    std::string acfIcao;          // "A321", "" when unknown
    int zuluSec = -1;             // seconds since 00:00Z, -1 when unknown
    bool onPanel = false;         // drawn on the aircraft's own tablet (else only the floating window)
    int panel[4] = {0, 0, 0, 0};  // left, bottom, width, height on the panel texture
    bool panelEnabled = false, panelPowered = false;
    bool aviTabRunning = false;   // AviTab is installed and active: the cockpit tablet stays AviTab's
    bool wxPlane = false;         // WXPlane is installed
    float brightness = 1.0f;
    float pixelScale = 1.0f;      // screen pixels per layout unit
};

struct Actions {
    bool setBrightness = false;
    float brightness = 1.0f;
};

Fonts loadFonts(float pixelScale);   // (re)builds the font atlas for this many pixels per layout unit
void applyStyle();
void build(const Fonts& f, const SimState& s, float w, float h, Actions& act);   // one frame of the tablet, w x h layout units

int currentApp();
void openApp(int app);   // APP_HOME: the home screen
const char* appName(int app);

} // namespace xpt
