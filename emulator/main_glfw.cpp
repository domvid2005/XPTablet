// The emulator in a desktop window (Windows, macOS, Linux): the emulated X-Plane screen on the left with the tablet's window (drag its
// title bar to move it, its corner to resize it), the control panel on the right.
//   XPTablet Emulator [--xplane "/path/to/X-Plane 12"] [--data folder] [--plugin folder] [--script steps.txt --out pictures]
#include "emu.h"
#include "emugl.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE   // the emulator loads its own GL functions (emugl.h): no OpenGL headers needed
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

#include "XPLMDefs.h"

namespace {
const float kPanelW = 460;   // window units
GLFWwindow* g_win = nullptr;
float g_scale = 1;           // framebuffer pixels per window unit
int g_winW = 1600, g_winH = 900;
bool g_pressOnPanel = false; // where the held press began
bool g_anyDown = false;
double g_mx = 0, g_my = 0;

float screenW() { return std::max(64.0f, g_winW - kPanelW); }
bool onPanel(double x) { return x >= screenW(); }

void cursorCb(GLFWwindow*, double x, double y) {
    g_mx = x; g_my = y;
    if (g_anyDown ? g_pressOnPanel : onPanel(x)) emu::panelMouse((float)(x - screenW()), (float)y, -1, false, true, 0);
    if (!(g_anyDown && g_pressOnPanel)) emu::mouseMove((float)x, (float)(g_winH - y));
    if (!g_anyDown && onPanel(x)) emu::mouseMove(-1, -1);
}
void buttonCb(GLFWwindow*, int button, int action, int) {
    if (button > 1) return;
    const bool down = action == GLFW_PRESS;
    if (down && !g_anyDown) g_pressOnPanel = onPanel(g_mx);
    if (g_pressOnPanel) emu::panelMouse((float)(g_mx - screenW()), (float)g_my, button, down, true, 0);
    else emu::mouseButton(button, down, (float)g_mx, (float)(g_winH - g_my));
    g_anyDown = down;
}
void scrollCb(GLFWwindow*, double, double dy) {
    if (onPanel(g_mx)) emu::panelMouse((float)(g_mx - screenW()), (float)g_my, -1, false, false, (float)dy);
    else emu::mouseWheel((float)g_mx, (float)(g_winH - g_my), (int)std::lround(dy));
}
void charCb(GLFWwindow*, unsigned int c) {
    if (emu::panelWantsKeys() || onPanel(g_mx)) emu::panelChar(c);
    else emu::keyChar(c);
}
ImGuiKey imguiKey(int k) {
    switch (k) {
    case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace; case GLFW_KEY_DELETE: return ImGuiKey_Delete; case GLFW_KEY_ENTER: return ImGuiKey_Enter;
    case GLFW_KEY_ESCAPE: return ImGuiKey_Escape; case GLFW_KEY_TAB: return ImGuiKey_Tab; case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
    case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow; case GLFW_KEY_UP: return ImGuiKey_UpArrow; case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
    case GLFW_KEY_HOME: return ImGuiKey_Home; case GLFW_KEY_END: return ImGuiKey_End; case GLFW_KEY_A: return ImGuiKey_A; case GLFW_KEY_C: return ImGuiKey_C;
    case GLFW_KEY_V: return ImGuiKey_V; case GLFW_KEY_X: return ImGuiKey_X; case GLFW_KEY_LEFT_CONTROL: return ImGuiMod_Ctrl; case GLFW_KEY_RIGHT_CONTROL: return ImGuiMod_Ctrl;
    case GLFW_KEY_LEFT_SHIFT: return ImGuiMod_Shift; case GLFW_KEY_RIGHT_SHIFT: return ImGuiMod_Shift; case GLFW_KEY_LEFT_SUPER: return ImGuiMod_Super;
    default: return ImGuiKey_None;
    }
}
void keyCb(GLFWwindow*, int key, int, int action, int mods) {
    if (action == GLFW_REPEAT) action = GLFW_PRESS;
    if (emu::panelWantsKeys() || onPanel(g_mx)) {
        const ImGuiKey k = imguiKey(key);
        if (k != ImGuiKey_None) emu::panelKey((int)k, action == GLFW_PRESS);
        return;
    }
    int vk = -1;
    switch (key) {
    case GLFW_KEY_BACKSPACE: vk = XPLM_VK_BACK; break; case GLFW_KEY_ENTER: vk = XPLM_VK_RETURN; break; case GLFW_KEY_ESCAPE: vk = XPLM_VK_ESCAPE; break;
    case GLFW_KEY_TAB: vk = XPLM_VK_TAB; break; case GLFW_KEY_LEFT: vk = XPLM_VK_LEFT; break; case GLFW_KEY_RIGHT: vk = XPLM_VK_RIGHT; break;
    case GLFW_KEY_UP: vk = XPLM_VK_UP; break; case GLFW_KEY_DOWN: vk = XPLM_VK_DOWN; break; case GLFW_KEY_DELETE: vk = XPLM_VK_DELETE; break;
    case GLFW_KEY_HOME: vk = XPLM_VK_HOME; break; case GLFW_KEY_END: vk = XPLM_VK_END; break;
    default: break;
    }
    if (vk >= 0) emu::keySpecial(vk, action == GLFW_PRESS, (mods & GLFW_MOD_SHIFT) ? xplm_ShiftFlag : 0);
}
void* getProc(const char* n) { return (void*)glfwGetProcAddress(n); }
} // namespace

int main(int argc, char** argv) {
    using namespace emu;
    if (!glfwInit()) { std::fprintf(stderr, "GLFW could not start\n"); return 1; }
    // a compatibility context, as X-Plane gives plugins (fixed-function drawing works)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    g_win = glfwCreateWindow(g_winW, g_winH, "XPTablet Emulator", nullptr, nullptr);
    if (!g_win) { std::fprintf(stderr, "no OpenGL window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(g_win);
    glfwSwapInterval(1);
    if (!loadGl(getProc)) return 1;
    glfwSetCursorPosCallback(g_win, cursorCb);
    glfwSetMouseButtonCallback(g_win, buttonCb);
    glfwSetScrollCallback(g_win, scrollCb);
    glfwSetCharCallback(g_win, charCb);
    glfwSetKeyCallback(g_win, keyCb);

    int fbW = 0, fbH = 0;
    glfwGetWindowSize(g_win, &g_winW, &g_winH);
    glfwGetFramebufferSize(g_win, &fbW, &fbH);
    g_scale = g_winW > 0 ? (float)fbW / g_winW : 1.0f;
    log("[emulator] OpenGL %s (%s)", (const char*)gl.GetString(GL_VERSION), (const char*)gl.GetString(GL_RENDERER));
    setScreen((int)screenW(), g_winH, g_scale);
    panelInit(g_scale);
    const Paths p = pathsFromArgs(argc, argv);
    if (!start(p)) log("[emulator] the plugin did not start: see the log");
    const std::string script = argValue(argc, argv, "--script", "");
    const std::string out = argValue(argc, argv, "--out", p.emuData + "/screenshots");
    bool scripted = !script.empty() && scriptLoad(script);
    std::error_code ec;
    std::filesystem::create_directories(out, ec);

    auto t0 = std::chrono::steady_clock::now(), last = t0;
    while (!glfwWindowShouldClose(g_win)) {
        glfwPollEvents();
        glfwGetWindowSize(g_win, &g_winW, &g_winH);
        glfwGetFramebufferSize(g_win, &fbW, &fbH);
        if (g_winW <= 0 || g_winH <= 0) { glfwWaitEvents(); continue; }
        g_scale = (float)fbW / g_winW;
        setScreen((int)screenW(), g_winH, g_scale);
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        frame(dt);
        const int sw = (int)std::lround(screenW() * g_scale);
        gl.Viewport(0, 0, fbW, fbH);
        gl.ClearColor(0.05f, 0.06f, 0.08f, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.Viewport(0, 0, sw, fbH);
        draw();
        auto shot = [&](const std::string& name) {
            std::vector<unsigned char> px((size_t)sw * fbH * 4);
            gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
            gl.ReadPixels(0, 0, sw, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            const std::string f = out + "/" + name + ".png";
            log("[emulator] picture: %s", f.c_str());
            return writePng(f, px.data(), sw, fbH);
        };
        if (panelRequests().screenshot) {
            panelRequests().screenshot = false;
            char name[64];
            std::snprintf(name, sizeof name, "xptablet_%lld", (long long)std::time(nullptr));
            shot(name);
        }
        if (scripted && !scriptStep(std::chrono::duration<double>(now - t0).count(), shot)) {
            scripted = false;
            log("[emulator] script finished: %d step(s) failed", scriptFailures());
        }
        g_panelFbX = (float)sw;
        gl.Viewport(sw, 0, fbW - sw, fbH);
        panelFrame(kPanelW, (float)g_winH, g_scale, dt);
        glfwSwapBuffers(g_win);
    }
    stop();
    glfwDestroyWindow(g_win);
    glfwTerminate();
    return 0;
}
