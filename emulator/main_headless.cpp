// The emulator without a screen (Linux): EGL draws off-screen, a script drives it, and each "shot" step writes a PNG. For tests and CI:
//   xptablet_emu_headless --script steps.txt --out pictures [--xplane /path/to/X-Plane 12] [--size 1280x800] [--scale 2]
#include "emu.h"
#include "emugl.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <thread>
#include <vector>

namespace {
typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext; typedef void* EGLSurface; typedef int EGLint; typedef unsigned int EGLBoolean;
EGLDisplay (*eglGetDisplay)(void*);
EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*);
EGLBoolean (*eglBindAPI)(unsigned int);
EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
void* (*eglGetProcAddress)(const char*);
EGLint (*eglGetError)(void);
void* g_libGL = nullptr;
void* getProc(const char* n) {
    void* p = g_libGL ? dlsym(g_libGL, n) : nullptr;
    return p ? p : eglGetProcAddress(n);
}
template <class F> bool sym(void* lib, F& f, const char* n) { f = (F)dlsym(lib, n); return f != nullptr; }
}

int main(int argc, char** argv) {
    using namespace emu;
    const std::string script = argValue(argc, argv, "--script", "");
    const std::string out = argValue(argc, argv, "--out", ".");
    int W = 1280, H = 800;
    std::sscanf(argValue(argc, argv, "--size", "1280x800").c_str(), "%dx%d", &W, &H);
    const float scale = (float)std::atof(argValue(argc, argv, "--scale", "1").c_str());
    bool withPanel = false;   // --panel: the control panel beside the screen, as in the window
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--panel") withPanel = true;
    const float panelW = withPanel ? 460.0f : 0.0f;
    if (script.empty()) { std::fprintf(stderr, "usage: %s --script steps.txt --out folder [--xplane folder] [--size 1280x800] [--scale 2]\n", argv[0]); return 2; }
    std::error_code ec;
    std::filesystem::create_directories(out, ec);

    // OpenGL through EGL, off-screen. libGL first and global, so the plugin finds the GL functions as it does in X-Plane.
    g_libGL = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl || !g_libGL) { std::fprintf(stderr, "libEGL.so.1 / libGL.so.1 not found\n"); return 1; }
    bool ok = sym(egl, eglGetDisplay, "eglGetDisplay") && sym(egl, eglInitialize, "eglInitialize") && sym(egl, eglBindAPI, "eglBindAPI") &&
              sym(egl, eglChooseConfig, "eglChooseConfig") && sym(egl, eglCreatePbufferSurface, "eglCreatePbufferSurface") &&
              sym(egl, eglCreateContext, "eglCreateContext") && sym(egl, eglMakeCurrent, "eglMakeCurrent") && sym(egl, eglGetProcAddress, "eglGetProcAddress") &&
              sym(egl, eglGetError, "eglGetError");
    if (!ok) { std::fprintf(stderr, "EGL functions missing\n"); return 1; }
    EGLDisplay dpy = nullptr;
    auto getPlatformDisplay = (EGLDisplay(*)(unsigned int, void*, const EGLint*))eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay) dpy = getPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    if (!dpy) dpy = eglGetDisplay(nullptr);
    EGLint maj = 0, min = 0;
    if (!dpy || !eglInitialize(dpy, &maj, &min)) { std::fprintf(stderr, "EGL: no display (0x%x)\n", eglGetError()); return 1; }
    eglBindAPI(0x30A2 /* EGL_OPENGL_API */);
    const EGLint cfgAttr[] = {0x3033, 0x0001 /* SURFACE_TYPE: PBUFFER */, 0x3040, 0x0008 /* RENDERABLE: OPENGL */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) { std::fprintf(stderr, "EGL: no config (0x%x)\n", eglGetError()); return 1; }
    const int sw = (int)(W * scale), pw = (int)((W + panelW) * scale), ph = (int)(H * scale);
    const EGLint pbAttr[] = {0x3057, pw, 0x3056, ph, 0x3038};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    EGLContext ctx = eglCreateContext(dpy, cfg, nullptr, nullptr);
    if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) { std::fprintf(stderr, "EGL: no context (0x%x)\n", eglGetError()); return 1; }
    if (!loadGl(getProc)) return 1;
    std::printf("OpenGL: %s (%s)\n", (const char*)gl.GetString(GL_VERSION), (const char*)gl.GetString(GL_RENDERER));

    const Paths p = pathsFromArgs(argc, argv);
    setScreen(W, H, scale);
    if (withPanel) panelInit(scale);
    if (!start(p)) return 1;
    if (!scriptLoad(script)) return 1;
    std::vector<unsigned char> px((size_t)pw * ph * 4);
    auto shot = [&](const std::string& name) {
        gl.Finish();
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const std::string f = out + "/" + name + ".png";
        const bool w = writePng(f, px.data(), pw, ph);
        log("[emulator] picture: %s", f.c_str());
        return w;
    };
    auto t0 = std::chrono::steady_clock::now(), last = t0;
    for (;;) {   // real time (the plugin's threads download meanwhile), at about 30 frames a second
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        frame(dt);
        gl.Viewport(0, 0, pw, ph);
        gl.ClearColor(0, 0, 0, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.Viewport(0, 0, sw, ph);
        draw();
        if (withPanel) { g_panelFbX = (float)sw; gl.Viewport(sw, 0, pw - sw, ph); panelFrame(panelW, (float)H, scale, dt); gl.Viewport(0, 0, sw, ph); }
        if (!scriptStep(std::chrono::duration<double>(now - t0).count(), shot)) break;
        std::this_thread::sleep_until(now + std::chrono::milliseconds(33));
    }
    stop();
    const int fails = scriptFailures();
    std::printf("%s: %d step(s) failed\n", fails ? "FAILED" : "done", fails);
    return fails ? 1 : 0;
}
