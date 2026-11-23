// X-Plane's radio navaids and fixes for the Map page (earth_nav.dat, earth_fix.dat: Custom Data first, as X-Plane does, else
// Resources/default data), read on a thread of their own the first time the map asks; then the ones around the view on request.
// No X-Plane calls here.
#pragma once

#include <string>
#include <vector>

#include "tabletui.h"

namespace xpt {
namespace navdata {

void start(const std::string& xplaneRoot);
void stop();
void setView(double lat, double lon, double navNm, double fixNm);
void get(std::vector<MapState::Navaid>& navs, std::vector<MapState::Fix>& fixes);
// a filed route's words (after its departure) as points, from (lat, lon) on: its fixes and navaids (each the one of that name nearest
// the point before it), its airways followed fix by fix (earth_awy.dat), coordinates ("4530N07500W", "45N075W"). SIDs, STARs, speeds
// and levels are passed over. False until the data has been read (the map's first look starts it).
bool ready();   // read (the route resolver can be used)
bool resolveRoute(double lat, double lon, const std::vector<std::string>& words, std::vector<MapState::RoutePoint>& out);

} // namespace navdata
} // namespace xpt
