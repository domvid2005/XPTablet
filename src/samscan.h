// SAM jetways in the installed scenery, for telling the pilot when a stand's jetways need openSAM and it is not running.
// SAM-style jetways (Stairport's SAM, and openSAM which drives them today) do not move without the plugin; X-Plane's own
// jetway command does nothing for them. The scenery says where they are: a pack with SAM jetways has a sam.xml at its top,
// listing its jetways with their latitude and longitude (library jetways placed in WED are not listed: for those the pack's
// airports stand in). Read once, on a thread of its own, at start.
#pragma once
#include <string>

namespace xpt {
namespace sam {

struct Install {
    bool scanned = false;          // the scenery has been read (until then nothing is known)
    bool pluginFolder = false;     // Resources/plugins has an openSAM folder (installed, whether or not it started)
    bool library = false;          // Custom Scenery has openSAM_Library (enabled in scenery_packs.ini)
    bool libraryBelowGlobal = false;   // ...but listed below Global Airports: openSAM refuses to start then
    int packs = 0, jetways = 0, airports = 0;   // what was found: packs with a sam.xml, jetways placed in them, their airports
};

// Reads the scenery under the X-Plane folder root (ends in a slash) on a thread; again only after stop().
void start(const std::string& root);
void stop();
Install install();
// The SAM jetways near a place: 2 one within reach of a stand (about 90 m), 1 at an airport whose scenery has SAM jetways
// (within about 2.5 km of it), 0 none (or not read yet). name: the airport's or pack's name, when known.
int nearby(double lat, double lon, std::string* name = nullptr);

// (for the tests) reads a scenery folder now, on this thread
void scanNow(const std::string& root);

}   // namespace sam
}   // namespace xpt
