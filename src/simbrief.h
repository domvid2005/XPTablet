// SimBrief: the address of a user's latest plan, and its answer read into a FlightPlan (shared by the plugin and the browser emulator)
#pragma once
#include <functional>

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
// The SID's and STAR's crossing restrictions (altitude and speed) onto the plan's fixes, from X-Plane's procedure files (CIFP)
// for the departure and the destination: cifpText(icao) returns that airport's file ("" when there is none).
int addCrossings(FlightPlan& p, const std::function<std::string(const std::string&)>& cifpText);
// The plan's SID (its runway's transition, the common part, the route's transition), STAR (the other way about) and an approach to
// the destination's runway (ILS first, else RNAV, else any; its final part, to the runway) as lines for the map. resolveFix finds a
// fix's position near a point (false: not found); the runways' thresholds come from the CIFP's RWY records.
// An airport's runways, SIDs, STARs and approaches from its CIFP text (the Flight Plan page's choices)
AirportProcs readAirportProcs(const std::string& icao, const std::string& cifp);
// A change on the Flight Plan page put into the plan: a runway that the SID (or STAR) does not serve leaves it unset to choose again
void applyProcEdit(FlightPlan& p, int what, const std::string& value, const AirportProcs& dep, const AirportProcs& arr);
// A diversion taken in flight: the route direct from where the aircraft is (PPOS) to the new destination's STAR (SimBrief's, its
// first leg) or to the airport when there is none; no SID. havePos false: the plan as it is.
void divertFrom(FlightPlan& p, bool havePos, double lat, double lon);
// The plan diverted to its alternate (no new SimBrief plan): the alternate the destination, its runway SimBrief's, no STAR yet (chosen
// on the Flight Plan page), direct from the aircraft (from the departure when there is no position); false when it has no alternate
bool divertToAlternate(FlightPlan& p, bool havePos, double lat, double lon);
// The map's route with the procedures as flown: SimBrief's legs while nothing was changed; else the departure, the new SID, the
// route between (from where the SID ends to where the STAR starts), the new STAR and approach, the destination
void routeWithProcedures(const FlightPlan& p, const std::vector<MapState::Procedure>& procs, MapState& m);
std::vector<MapState::Procedure> buildProcedures(const FlightPlan& p, const std::function<std::string(const std::string&)>& cifpText,
    const std::function<bool(const std::string& id, double nearLat, double nearLon, double& lat, double& lon)>& resolveFix);

} // namespace xpt
