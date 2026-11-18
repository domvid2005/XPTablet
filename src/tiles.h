// The Map page's tiles: downloaded on threads of their own, kept on disk, decoded to RGBA. No X-Plane or OpenGL calls here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpt {
namespace tiles {

enum Source { OSM = 0, SATELLITE = 1, SOURCES };
const char* attribution(int source);
int maxZoom(int source);

struct Key {
    int src = 0, z = 0, x = 0, y = 0;
    bool operator==(const Key& o) const { return src == o.src && z == o.z && x == o.x && y == o.y; }
    bool operator<(const Key& o) const { return src != o.src ? src < o.src : z != o.z ? z < o.z : x != o.x ? x < o.x : y < o.y; }
};
struct Tile { Key key; int w = 0, h = 0; std::vector<uint8_t> rgba; bool failed = false; };

void start(const std::string& cacheDir);   // Output/XPTablet/tiles
void stop();
// The tiles the map needs now, most wanted first: the list replaces what was waiting (tiles no longer on screen are not fetched).
void want(const std::vector<Key>& keys);
// A finished tile (decoded, or failed); false when none is waiting.
bool take(Tile& t);

} // namespace tiles
} // namespace xpt
