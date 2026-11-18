// VATSIM's public data feed (data.vatsim.net/v3/vatsim-data.json), for the Map's traffic cards: who is flying under a call sign,
// and their filed flight plan. Fetched on a thread of its own, every 30 seconds while a card is open. No X-Plane calls here.
#pragma once

#include <string>

#include "tabletui.h"

namespace xpt {
namespace vatsim {

void start();
void stop();
void want(bool on);   // a traffic card is open: keep the data fresh
// What VATSIM has for a call sign (found false: not connected), and how the data stands ("" when fresh).
VatsimPilot lookup(const std::string& callsign, std::string& status);

} // namespace vatsim
} // namespace xpt
