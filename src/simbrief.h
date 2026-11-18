// SimBrief: the address of a user's latest plan, and its answer read into a FlightPlan (shared by the plugin and the browser emulator)
#pragma once

#include <string>

#include "tabletui.h"

namespace xpt {

std::string simbriefUrl(const std::string& userNameOrId);   // SimBrief's public fetcher, JSON
FlightPlan parseSimbrief(const std::string& body);          // loaded, or status says why not
void planToRoute(const FlightPlan& plan, MapState& map);     // the map's route (and its status line)

} // namespace xpt
