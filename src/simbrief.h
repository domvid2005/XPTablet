// SimBrief: the address of a user's latest plan, and its answer read into a FlightPlan (shared by the plugin and the browser emulator)
#pragma once

#include <string>

#include "tabletui.h"

namespace xpt {

std::string simbriefUrl(const std::string& userNameOrId);   // SimBrief's public fetcher, JSON
FlightPlan parseSimbrief(const std::string& body);          // loaded, or status says why not
void planToRoute(const FlightPlan& plan, MapState& map);     // the map's route (and its status line)
// A plan fetched: Import's goes into the preview at once; the 2-minute check's only when SimBrief has one not seen (not the plan in
// use, not one dismissed), and then the notification says so. Nothing replaces the flight plan until the preview's Import.
void planFetched(SimState& s, const FlightPlan& p, bool quiet);
void planDecide(SimState& s, const Actions& a);              // the preview's Import or Dismiss

} // namespace xpt
