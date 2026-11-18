// X-Plane's airports for the Map page: an index of every airport in the scenery (custom scenery first, in scenery_packs.ini's
// order, then Global Airports), built on a thread of its own at start; and the layouts of the airports around the map's view,
// read from the index on demand. No X-Plane calls here.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aptgeo.h"

namespace xpt {
namespace aptdat {

void start(const std::string& xplaneRoot);   // the X-Plane folder, with its trailing separator
void stop();
// What the map shows: layouts are read for airports within layoutNm of the point (when wanted), positions for those within pointNm.
void setView(double lat, double lon, double layoutNm, double pointNm, bool wantLayouts);
// What is ready: the layouts and positions for the latest view, whether the index is still being built and how many it holds.
void get(std::vector<std::shared_ptr<const AptGeo>>& layouts, std::vector<AptPoint>& points, bool& indexing, int& count);

} // namespace aptdat
} // namespace xpt
