// The Map page's tiles: downloaded on threads of their own, kept on disk, decoded to RGBA. No X-Plane or OpenGL calls here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpt {
namespace tiles {

enum Source { OSM = 0, SATELLITE = 1, DARK = 2, OPENAIP = 3, SOURCES };   // DARK: OpenStreetMap's tiles, recoloured for night (darken);
                                                                     // OPENAIP: openAIP's aeronautical overlay (transparent PNG, the user's API key)
const char* attribution(int source);
int maxZoom(int source);

struct Key {
    int src = 0, z = 0, x = 0, y = 0;
    bool operator==(const Key& o) const { return src == o.src && z == o.z && x == o.x && y == o.y; }
    bool operator<(const Key& o) const { return src != o.src ? src < o.src : z != o.z ? z < o.z : x != o.x ? x < o.x : y < o.y; }
};
struct Tile { Key key; int w = 0, h = 0; std::vector<uint8_t> rgba; bool failed = false; };

// OpenStreetMap's colours turned to a night map: light land goes dark, dark ink goes light, hues kept (water stays blue, roads
// keep their colour), plain white roads a soft grey so they still stand out from the land. RGBA, in place.
inline void darken(uint8_t* px, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, px += 4) {
        const int r = px[0], g = px[1], b = px[2];
        const int m = (r + g + b) / 3;
        int o[3];
        if (m >= 249) { o[0] = 66; o[1] = 74; o[2] = 90; }   // white: roads
        else {
            const int d = 255 - 2 * m;
            const int c[3] = {r, g, b};
            for (int k = 0; k < 3; ++k) {
                int v = c[k] + d;
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                o[k] = v * 82 / 100;
            }
            o[0] += 8; o[1] += 12; o[2] += 22;   // a little blue in the dark
        }
        for (int k = 0; k < 3; ++k) px[k] = (uint8_t)(o[k] > 255 ? 255 : o[k]);
    }
}

void start(const std::string& cacheDir);   // Output/XPTablet/tiles
void setOpenAipKey(const std::string& key);   // the user's openAIP API key (Settings, Accounts); empty: no overlay tiles are asked for
void stop();
// The tiles the map needs now, most wanted first: the list replaces what was waiting (tiles no longer on screen are not fetched).
void want(const std::vector<Key>& keys);
// A finished tile (decoded, or failed); false when none is waiting.
bool take(Tile& t);

} // namespace tiles
} // namespace xpt
