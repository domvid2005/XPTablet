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

} // namespace navdata
} // namespace xpt
