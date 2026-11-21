// Local Connect's wire format: what the plugin and the tablet on a phone say to each other.
//
// The phone runs the tablet's own pages; the plugin sends it the sim's state (SimState) and takes back what the user asked for
// (Actions). Both are written field by field from one list per struct (the same list reads them back), so the two ends cannot
// disagree about the order. Numbers are little-endian; whole numbers are variable-length; a string or a list has its length first.
//
// SimState goes as sections (the weather, the ground services, the map's traffic...), each a blob of its own: the plugin keeps a
// hash of what each device last had and sends only the sections that changed.
//
// What never crosses: textures (a picture's id means nothing on another machine), the map's tile function, and the pairing code
// (it is shown only in the sim: a device that could read it could pair itself).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "tabletui.h"

namespace xpt {
namespace wire {

constexpr int kVersion = 1;   // goes up when a field list changes: a page from another version is told to reload

// SimState's sections
enum Section {
    S_CORE,        // the aircraft, the clock, the battery, xPilot's status...
    S_LOCAL,       // Local Connect's own status and devices (without the pairing code)
    S_ACARS, S_WX, S_BROWSER, S_METAR, S_GROUND, S_AIRPORT, S_AIRPORT_LIST, S_DOCS, S_CHECKLIST,
    S_PLAN, S_PLAN_ORIGINAL, S_PLAN_PREVIEW, S_DATA_INFO, S_MEDIA, S_BACKGROUNDS, S_NAV_HITS, S_VATSIM_ATC,
    S_MAP_CORE,    // the aircraft's position and the map's statuses
    S_MAP_AIRPORTS, S_MAP_POINTS, S_MAP_TRAFFIC, S_MAP_NAVAIDS, S_MAP_FIXES, S_MAP_ROUTE, S_MAP_VATSIM, S_MAP_AIRSPACES,
    S_COUNT
};

// One section of the state, written out / read back into s (the other sections of s are left as they are).
// False from decode: the bytes are not a section of this version (nothing of s is to be trusted then: drop the connection).
std::string encodeSection(const SimState& s, int section);
bool decodeSection(SimState& s, int section, const std::string& bytes);

// What the user asked for, in full.
std::string encodeActions(const Actions& a);
bool decodeActions(Actions& a, const std::string& bytes);

// The parts of Actions that say "this happened now" (a button pressed, a search sent) were set: such a message must be carried out
// once and never dropped. The rest (which page is shown, where the map is) is a level: only the newest matters.
bool actionsHaveEvent(const Actions& a);

// What a paired device may ask for, enforced on what it sent (s: the state it was shown). A device is only ever a tablet: it cannot
// manage pairing (that is done in the sim alone), drive the computer's own browser or delete the computer's data; a file it opens
// must be one the Documents page listed, a background one the folder holds, an xPilot command one the sim has; texts and numbers
// are kept to sizes and ranges the pages themselves could send.
void restrictRemote(Actions& a, const SimState& s);

// The settings are the same on the computer and on its devices, but for the home screen's arrangement (which icons where, the
// favorites bar, the folders): each screen keeps its own, since a device does not show everything the computer does.
// settingsWithLayoutOf: settings' lines without its arrangement, then the arrangement lines of layoutFrom.
std::string settingsWithLayoutOf(const std::string& settings, const std::string& layoutFrom);
std::string layoutOf(const std::string& settings);   // only the arrangement's lines

// A section's fingerprint (FNV-1a, 64 bits): equal bytes, equal fingerprint.
std::uint64_t fingerprint(const std::string& bytes);

// The sizes of the structs the lists were written for, on the build they were checked on (Linux, x86-64, GCC): the tests compare
// them there, so a field added to a struct without its line here is noticed.
struct Sizes { std::size_t simState, mapState, actions, groundState, flightPlan, mediaState, browserState, aptGeo; };
Sizes knownSizes();

}   // namespace wire
}   // namespace xpt
