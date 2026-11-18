// Where things are: the X-Plane folder the plugin sees, the emulator's own files, and XPTablet's (shapes, the browser helper).
#include "emu.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace emu {

std::string argValue(int argc, char** argv, const char* name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

Paths pathsFromArgs(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exeDir = fs::absolute(fs::path(argv[0]), ec).parent_path();
    Paths p;
    p.emuData = argValue(argc, argv, "--data", (exeDir / "emulator_data").string());
    fs::create_directories(p.emuData, ec);
    // the X-Plane folder: --xplane, else the one saved in emulator.ini, else an empty one of the emulator's own
    std::string saved;
    {
        std::ifstream in(p.emuData + "/emulator.ini");
        std::string l;
        while (std::getline(in, l)) if (l.compare(0, 7, "xplane=") == 0) saved = l.substr(7);
    }
    p.xplaneRoot = argValue(argc, argv, "--xplane", saved.empty() ? p.emuData + "/xplane" : saved);
    if (p.xplaneRoot != saved && argValue(argc, argv, "--xplane", "") != "") {   // remembered for next time
        std::ofstream out(p.emuData + "/emulator.ini");
        out << "xplane=" << p.xplaneRoot << "\n";
    }
    while (p.xplaneRoot.size() > 1 && (p.xplaneRoot.back() == '/' || p.xplaneRoot.back() == '\\')) p.xplaneRoot.pop_back();
    fs::create_directories(p.xplaneRoot + "/Output", ec);
    // XPTablet's own files: beside the program (the build copies them there), else the source tree's
    std::string plug = argValue(argc, argv, "--plugin", "");
    if (plug.empty()) {
        if (fs::exists(exeDir / "XPTablet" / "shapes")) plug = (exeDir / "XPTablet").string();
#ifdef XPT_EMU_SOURCE_ASSETS
        else plug = XPT_EMU_SOURCE_ASSETS;
#endif
    }
    p.pluginDir = plug;
    return p;
}

} // namespace emu
