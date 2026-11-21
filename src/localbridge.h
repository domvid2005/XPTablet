// Local Connect, between the tablet in the sim and the server: what the plugin does each frame so paired devices see the same
// tablet. It sends the state (the parts that change often, often; the large ones seldom or only when they are another object),
// keeps the settings, notes and logbook the same on every device, and brings what devices ask for into the frame's own Actions.
#pragma once
#include "tabletui.h"

namespace xpt {
namespace localbridge {

void publish(const SimState& s, double now);   // now: any clock in seconds

// What devices sent since the last call: their settings and notes are taken in (the caller saves them when told), their requests
// checked (wire::restrictRemote) and kept for merge().
struct Took { bool settings = false, notes = false; };
Took take(const SimState& s);

// Into this frame's Actions: one request a device made (a press, a search: each carried out once), and what devices are showing
// (the map and where it is, the Airports page, a search) wherever the tablet in the sim is not showing the same itself.
void merge(Actions& act);

}   // namespace localbridge
}   // namespace xpt
