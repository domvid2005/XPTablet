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
} // namespace platform
} // namespace media
} // namespace xpt
