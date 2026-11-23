// macOS (and anything else): no media controls (Apple keeps the system's "Now Playing" to its own apps since macOS 15.4)
#include "media_platform.h"

namespace xpt {
namespace media {
namespace platform {
bool init() { return false; }
void shutdown() {}
bool poll(Info&, std::string&) { return false; }
void send(int) {}
void seek(double) {}
bool setVolume(float) { return false; }
bool radioApps(std::string&) { return false; }
bool playerVol(float&) { return false; }
bool setPlayerVol(float) { return false; }
bool masterVol(float&) { return false; }
bool setMasterVol(float) { return false; }
bool playerMute(bool&) { return false; }
bool setPlayerMute(bool) { return false; }
bool masterMute(bool&) { return false; }
bool setMasterMute(bool) { return false; }
} // namespace platform
} // namespace media
} // namespace xpt
