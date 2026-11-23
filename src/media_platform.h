// What each computer offers to media.cpp (media_win.cpp, media_linux.cpp; media_none.cpp elsewhere). Called from its thread only.
#pragma once

#include <string>

namespace xpt {
namespace media {
namespace platform {

struct Info {
    std::string app, title, artist, album, artUrl;
    bool playing = false, canPrev = true, canNext = true;
    double posSec = -1, lenSec = -1;   // where the track is and how long it is (-1: not told)
    bool canSeek = false;
};
bool init();                                    // false: no media controls here
void shutdown();
bool poll(Info& info, std::string& artBytes);   // true when a player is there; artBytes: the cover as a file (PNG/JPEG) when it is given so
void send(int what);                            // 1 play / pause, 2 previous, 3 next, 4 play, 5 pause
void seek(double sec);                          // to this point of the track
bool setVolume(float factor);                   // the player's volume to factor x what it was (1: back to it); false: cannot
bool radioApps(std::string& who);               // a radio app is making sound now (Windows; elsewhere false)
bool playerVol(float& v);                       // the player's own volume, 0..1 (as the user set it, also while lowered for the radio)
bool setPlayerVol(float v);
bool masterVol(float& v);                       // the computer's own volume (Windows: the speakers'), 0..1; false: not here
bool setMasterVol(float v);
bool playerMute(bool& m);                       // muted (the player's sound, as Windows' Volume Mixer shows it); false: not known
bool setPlayerMute(bool m);
bool masterMute(bool& m);                       // the computer's sound muted (Windows: the speakers', as the taskbar shows it)
bool setMasterMute(bool m);

} // namespace platform
} // namespace media
} // namespace xpt
