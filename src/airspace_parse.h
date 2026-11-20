// VATSIM's airspaces (the VATSpy data project: Boundaries.geojson and VATSpy.dat): their outlines, and which FIR each controller
// call sign belongs to. Shared by the plugin and the browser emulator; no X-Plane calls.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tabletui.h"

namespace xpt {
namespace airspace {

// Boundaries.geojson's features read into areas: each its id, its outline rings (lat, lon), box and label point.
// A simple scanner (the file is large: no general JSON tree).
void parseBoundaries(const std::string& geojson, std::vector<AirspaceArea>& out);
// VATSpy.dat's [FIRs] section: ICAO|NAME|CALLSIGN PREFIX|BOUNDARY.
void parseFirs(const std::string& dat, std::vector<AirspaceSet::Fir>& out);
// the FIR a center or FSS call sign is for ("CHI_CTR", "LON_S_CTR", "EGTT_CTR"), else nullptr
const AirspaceSet::Fir* firFor(const AirspaceSet& set, const std::string& callsign);

} // namespace airspace
} // namespace xpt
