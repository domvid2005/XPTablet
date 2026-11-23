// Local Connect's mirror: paired devices show the picture of the tablet in the sim, and their touches work it.
//
// The plugin hands over the tablet's picture a few times a second while a device is connected (submit). A worker thread finds what
// changed since the last one, compresses that part (JPEG) and gives it to the server (src/localserver.h) for every device; a device
// that has just connected or fell behind gets a whole picture. Touches, scrolls and keys come back as small messages (nextInput),
// which the plugin feeds to the tablet as it does the mouse's.
#pragma once
#include <vector>

namespace xpt {
namespace mirror {

constexpr int kPageVersion = 4;   // the page's and these messages' version (see src/localpage.h)

void start();
void stop();

// A device is connected, the worker is free, and it is time for the next picture (15 a second at most; 24 when fast is asked
// for: a game on screen).
bool wantsFrame(double now, bool fast = false);
void setRate(int perSecond);   // pictures a second (5 to 30; a game on screen: at least 24)
// The tablet's picture: w x h pixels, 4 bytes each (red, green, blue, unused), rows top to bottom (or bottom to top, as OpenGL reads).
void submit(std::vector<unsigned char>&& rgbx, int w, int h, bool bottomUp);

struct Input {
    enum Kind { MOVE = 0, DOWN = 1, UP = 2, WHEEL = 3, KEY = 4, CHAR = 5, CANCEL = 6, GAME_KEY = 7 };
    int kind = MOVE;
    float x = 0, y = 0;   // where, as fractions of the picture (0..1 across, 0..1 down)
    int code = 0;         // WHEEL: +1 / -1; KEY: 8 backspace, 13 enter; CHAR: the character; GAME_KEY: 0 left, 1 right, 2 up, 3 down, 4 action
    bool down = false;    // GAME_KEY: pressed (or let go)
    bool touch = false;   // a finger (not a mouse)
};
bool nextInput(Input& in);   // the next thing a device did, if any
// The tablet is typing and the device should raise its own keyboard (sent with each picture, so the device knows)
void setDeviceTyping(bool on);

}   // namespace mirror
}   // namespace xpt
