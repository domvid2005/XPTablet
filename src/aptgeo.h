// An airport's layout as the Map page draws it, read from X-Plane's apt.dat (src/aptdat.cpp). Plain data: shared by the reader
// (a background thread in the plugin) and the drawing (src/tabletui.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace xpt {

// Web Mercator, 0..1 across the world: x east from 180 W, y south from about 85 N (the map tiles' own coordinates).
struct MercPt { double x = 0, y = 0; };
inline MercPt mercOf(double lat, double lon) {
    const double kPi = 3.14159265358979323846;
    lat = std::max(-85.05, std::min(85.05, lat));
    const double s = std::sin(lat * kPi / 180.0);
    MercPt m;
    m.x = (lon + 180.0) / 360.0;
    m.y = 0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi);
    return m;
}
inline void latLonOf(const MercPt& m, double& lat, double& lon) {
    const double kPi = 3.14159265358979323846;
    lon = m.x * 360.0 - 180.0;
    lat = std::atan(std::sinh(kPi * (1 - 2 * m.y))) * 180.0 / kPi;
}

struct AptGeo {
    std::string icao, name;
    int kind = 1;                        // 1 airport, 16 seaplane base, 17 heliport
    float elevationFt = 0;
    double lat = 0, lon = 0;             // the reference point
    // radio: kind 0 ATIS/AWOS, 1 UNICOM/CTAF, 2 clearance delivery, 3 ground, 4 tower, 5 approach, 6 departure
    struct Freq { int kind = 0; double mhz = 0; std::string name; };
    std::vector<Freq> freqs;
    // more about each runway (same order as runways): surface (apt.dat code), lights
    struct RunwayInfo { int surface = 1; bool lit = false; float displacedA = 0, displacedB = 0; };
    std::vector<RunwayInfo> runwayInfo;
    MercPt lo, hi;                       // the layout's bounds
    std::vector<MercPt> pavement;        // taxiways and aprons, as triangles (every three points one)
    struct Runway { MercPt a, b; float widthM = 45; std::string idA, idB; bool water = false; };
    std::vector<Runway> runways;
    // painted lines: 0 taxiway centreline, 1 runway hold position, 2 other hold / ILS critical area, 3 taxiway edge
    struct Line { int kind = 0; std::vector<MercPt> pts; };
    std::vector<Line> lines;
    struct Stand { MercPt p; float hdg = 0; std::string name, type, uses; };   // type: gate, tie_down, hangar...; uses: jets|turboprops...
    std::vector<Stand> stands;
    struct Label { MercPt p; std::string text; };   // a taxiway's name, once along it
    std::vector<Label> taxiways;
    struct Helipad { MercPt p; std::string id; };
    std::vector<Helipad> helipads;
};

// An airport in the index: where it is, without its layout.
struct AptPoint { std::string icao; double lat = 0, lon = 0; int kind = 1; std::string name; };   // kind: 1 land, 16 seaplane base, 17 heliport

// Reads one airport's rows (from its header row to before the next airport) into its layout. False if it has nothing to draw.
bool parseAirport(const std::string& rows, AptGeo& out);

// Triangulates a polygon with holes (points in metres, outer ring first; any winding). Returns indices into the points of all
// rings in order, three per triangle.
std::vector<int> triangulate(const std::vector<std::vector<std::pair<double, double>>>& rings);

} // namespace xpt
