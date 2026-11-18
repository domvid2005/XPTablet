// What each computer offers to media.cpp (media_win.cpp, media_linux.cpp; media_none.cpp elsewhere). Called from its thread only.
#pragma once

#include <string>

namespace xpt {
namespace media {
namespace platform {

struct Info { std::string app, title, artist, album, artUrl; bool playing = false, canPrev = true, canNext = true; };
bool init();                                    // false: no media controls here
void shutdown();
bool poll(Info& info, std::string& artBytes);   // true when a player is there; artBytes: the cover as a file (PNG/JPEG) when it is given so
void send(int what);                            // 1 play / pause, 2 previous, 3 next, 4 play, 5 pause
bool setVolume(float factor);                   // the player's volume to factor x what it was (1: back to it); false: cannot
bool radioApps(std::string& who);               // a radio app is making sound now (Windows; elsewhere false)

} // namespace platform
} // namespace media
} // namespace xpt
