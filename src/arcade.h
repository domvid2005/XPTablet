#pragma once
// XPTablet's arcade games, all its own and aviation themed: Contrail (a trail that grows), Taxi Rush (a taxiway maze),
// Sky Patrol (drones in formation), Hangar Climb (ramps, ladders and rolling tyres), Cockpit 21 (twenty-one against the
// dealer, for air miles) and Cargo Load (containers dragged into a hold; full rows and columns ship out). Each is a field drawn into a box,
// played with four directions and an action (jump / fire): the computer's keys or the on-screen pad.
#include "imgui.h"

namespace xpt {
namespace arcade {

enum Game { CONTRAIL, TAXI_RUSH, SKY_PATROL, HANGAR_CLIMB, COCKPIT_21, CARGO_LOAD, COUNT };

struct Input {
    bool left = false, right = false, up = false, down = false, action = false;   // held
    bool leftHit = false, rightHit = false, upHit = false, downHit = false, actionHit = false;   // pressed this frame
    // the finger (or the mouse) on the field: where, in screen pixels; pressed this frame, held, let go this frame
    float px = -1e6f, py = -1e6f;
    bool press = false, held = false, release = false;
};

const char* name(int game);
const char* blurb(int game);       // one line for the games list
const char* help(int game);        // how to play, beside the field
float aspect(int game);            // the field's width over its height
bool usesPad(int game);            // played with the directions and the action (else by tapping and dragging on the field)

void reset(int game);              // a new game
void step(int game, const Input& in, float dt);   // dt in seconds (a long frame is taken in small steps)
// the field into the box a..b (kept to its aspect, centred); big / small: fonts for its messages
void draw(int game, ImDrawList* dl, ImVec2 a, ImVec2 b, ImFont* big, float bigPx, ImFont* small, float smallPx);
int score(int game);
int lives(int game);               // -1: the game has none
bool over(int game);
bool started(int game);            // has a game been set up (else reset first)
// a small picture for the games list, centred at c, about r across
void icon(int game, ImDrawList* dl, ImVec2 c, float r);

}  // namespace arcade
}  // namespace xpt
