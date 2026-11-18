// Time zones for the Map page (WXPlane's data: Natural Earth 1:10m time zones, public domain; tools/make_tzones.py): the
// boundaries for its Time Zones layer, and each zone's standard UTC offset at a point. Daylight time: src/suntime.h.
#pragma once

#include <vector>

#include "imgui.h"

namespace xpt {

struct TzLine { float lo0, la0, lo1, la1; std::vector<ImVec2> pts; };   // pts: (lon, lat)
const std::vector<TzLine>& tzLines();
// The map's outlines (Natural Earth 1:50m, from WXPlane): kind 0 coast, 1 country border, 2 state / province line, 3 lake
struct GeoLine { int kind; float lo0, la0, lo1, la1; std::vector<ImVec2> pts; };   // pts: (lon, lat)
const std::vector<GeoLine>& basemapLines();
// The standard UTC offset (quarter hours) of the time zone at a point; false where the data has none (the open sea)
bool tzStdQuarterHours(double lat, double lon, int& q);

} // namespace xpt
