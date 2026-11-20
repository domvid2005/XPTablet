// VATSIM's airspaces for the Map: the VATSpy data project's Boundaries.geojson and VATSpy.dat, fetched once (on a thread of its
// own) into a cache folder and read from it again for a week. No X-Plane calls here.
#pragma once

#include <memory>
#include <string>

#include "tabletui.h"

namespace xpt {
namespace airspace {

void start(const std::string& cacheDir);   // (again: nothing more once it is running or done)
void stop();
std::shared_ptr<const AirspaceSet> get(std::string& status);   // null until read; status says how it stands

} // namespace airspace
} // namespace xpt
