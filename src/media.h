// Media controls (what plays on the computer: its cover, play / pause / previous / next) and the radio's effect on the music:
// Windows through the system's media transport controls and per-app volumes; Linux through MPRIS players on D-Bus (loaded at run
// time, so nothing extra is needed to install). Not on macOS. A thread does the work; the plugin reads it each frame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpt {
namespace media {

struct Now {
    bool supported = false;       // the computer's media controls could be reached
    bool active = false;          // a player is there
    std::string app, title, artist, album;
    bool playing = false, canPrev = true, canNext = true;
    int artVersion = 0;           // goes up when the cover changes (takeArt gives it)
    bool radioApp = false;        // Windows: a VATSIM voice app (TrackAudio, swift...) is making sound
    std::string radioAppName;
    bool ducked = false;          // the music is lowered or paused for the radio now
};

void start();
void stop();
Now now();
bool takeArt(int& version, std::vector<uint8_t>& rgba, int& w, int& h);   // the cover (RGBA), if newer than version
void command(int what);                                                   // 1 play / pause, 2 previous, 3 next
// each frame: 0 leave the music, 1 lower it to 50 %, 2 pause it, while radioRx (xPilot's COM1/COM2) or a radio app is heard
void setDuck(int mode, bool radioRx, const std::string& radioName);

} // namespace media
} // namespace xpt
