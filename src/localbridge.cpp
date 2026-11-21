// Local Connect's bridge (see localbridge.h).
#include "localbridge.h"

#include <deque>
#include <vector>

#include "localserver.h"
#include "localwire.h"

namespace xpt {
namespace localbridge {
namespace {

std::deque<Actions> g_events;   // requests to carry out, oldest first
Actions g_level;                // what a device is showing (the newest told)
bool g_haveLevel = false;
double g_lightAt = -1e9, g_heavyAt = -1e9, g_settingsAt = -1e9, g_notesAt = -1e9, g_logAt = -1e9;

const int kLight[] = {wire::S_CORE, wire::S_ACARS, wire::S_WX, wire::S_METAR, wire::S_GROUND, wire::S_AIRPORT_LIST, wire::S_PLAN_PREVIEW, wire::S_MEDIA,
                      wire::S_NAV_HITS, wire::S_MAP_CORE, wire::S_MAP_TRAFFIC};
const int kHeavy[] = {wire::S_AIRPORT, wire::S_DOCS, wire::S_CHECKLIST, wire::S_PLAN, wire::S_PLAN_ORIGINAL, wire::S_DATA_INFO, wire::S_BACKGROUNDS, wire::S_VATSIM_ATC,
                      wire::S_MAP_POINTS, wire::S_MAP_NAVAIDS, wire::S_MAP_FIXES, wire::S_MAP_ROUTE, wire::S_MAP_VATSIM};
// (not sent: the browser's state and Local Connect's own; a device shows neither)

}   // namespace

void publish(const SimState& s, double now) {
    const bool watched = localserver::clients() > 0;
    // with nobody connected the state is still kept fresh enough that a device connecting starts from the present
    if (now - g_lightAt >= (watched ? 1.0 / 15 : 1.0)) {
        g_lightAt = now;
        for (int sec : kLight) localserver::publishSection(sec, wire::encodeSection(s, sec));
        // the airports' layouts and VATSIM's airspaces are large and change by being replaced: written out only when they are other objects
        static std::vector<const void*> layouts;
        std::vector<const void*> cur;
        cur.reserve(s.map.airports.size());
        for (const auto& a : s.map.airports) cur.push_back(a.get());
        if (cur != layouts || layouts.empty()) { layouts.swap(cur); localserver::publishSection(wire::S_MAP_AIRPORTS, wire::encodeSection(s, wire::S_MAP_AIRPORTS)); }
        static const void* airspaces = nullptr;
        static bool airspacesSent = false;
        if (s.map.airspaces.get() != airspaces || !airspacesSent) { airspaces = s.map.airspaces.get(); airspacesSent = true; localserver::publishSection(wire::S_MAP_AIRSPACES, wire::encodeSection(s, wire::S_MAP_AIRSPACES)); }
    }
    if (now - g_heavyAt >= (watched ? 0.5 : 2.0)) {
        g_heavyAt = now;
        for (int sec : kHeavy) localserver::publishSection(sec, wire::encodeSection(s, sec));
    }
    if (now - g_settingsAt >= 0.25) { g_settingsAt = now; localserver::publishText('s', settingsToText()); }
    if (now - g_notesAt >= 1.0) { g_notesAt = now; localserver::publishText('n', notesToText()); }
    if (now - g_logAt >= 2.0) { g_logAt = now; localserver::publishText('l', logbookToText()); }
}

Took take(const SimState& s) {
    Took took;
    localserver::Incoming in;
    for (int i = 0; i < 64 && localserver::poll(in); ++i) {
        if (in.kind == 'A') {
            Actions a;
            if (!wire::decodeActions(a, in.bytes)) continue;
            wire::restrictRemote(a, s);
            if (wire::actionsHaveEvent(a) && g_events.size() < 256) g_events.push_back(a);
            g_level = a; g_haveLevel = true;
        } else if (in.kind == 'T') {
            const bool on = settings().localConnect;   // (a device cannot switch Local Connect itself off or on)
            settingsFromText(wire::settingsWithLayoutOf(in.bytes, settingsToText()));   // (its settings; this screen's own arrangement stays)
            settings().localConnect = on;
            took.settings = true;
            g_settingsAt = -1e9;   // (and told to the other devices at once)
        } else if (in.kind == 'N') {
            notesFromText(in.bytes);
            took.notes = true;
            g_notesAt = -1e9;
        }
    }
    if (localserver::clients() == 0) { g_events.clear(); g_haveLevel = false; }   // (nobody there: nothing of theirs lingers)
    return took;
}

void merge(Actions& act) {
    if (g_haveLevel) {
        const Actions& L = g_level;
        if (!act.mapShown && L.mapShown) {
            act.mapShown = true; act.mapLayouts = L.mapLayouts; act.mapPointMinFt = L.mapPointMinFt;
            act.mapLat = L.mapLat; act.mapLon = L.mapLon; act.mapLayoutNm = L.mapLayoutNm; act.mapPointNm = L.mapPointNm; act.mapNavNm = L.mapNavNm; act.mapFixNm = L.mapFixNm;
            act.trafficSelected = L.trafficSelected;
        }
        act.airportsShown = act.airportsShown || L.airportsShown;
        if (act.navSearch.empty()) act.navSearch = L.navSearch;
        if (act.acarsFrom.empty()) act.acarsFrom = L.acarsFrom;
    }
    if (g_events.empty()) return;
    const Actions e = g_events.front();
    g_events.pop_front();
    if (act.xpilotCmd.empty()) act.xpilotCmd = e.xpilotCmd;
    for (const Actions::AcarsOut& m : e.acarsOut) act.acarsOut.push_back(m);
    if (act.ground == GA_NONE) { act.ground = e.ground; act.groundIndex = e.groundIndex; }
    if (act.metarSearch.empty()) act.metarSearch = e.metarSearch;
    if (act.com1Standby == 0) act.com1Standby = e.com1Standby;
    if (act.airportSearch.empty()) act.airportSearch = e.airportSearch;
    if (act.airportOpen.empty()) act.airportOpen = e.airportOpen;
    if (act.docOpen.empty()) act.docOpen = e.docOpen;
    act.docsList = act.docsList || e.docsList;
    act.backgroundsList = act.backgroundsList || e.backgroundsList;
    act.planImport = act.planImport || e.planImport; act.planAccept = act.planAccept || e.planAccept; act.planDismiss = act.planDismiss || e.planDismiss;
    act.planReview = act.planReview || e.planReview; act.planClear = act.planClear || e.planClear; act.planDivert = act.planDivert || e.planDivert;
    act.planUndoDivert = act.planUndoDivert || e.planUndoDivert;
    if (act.media == 0) act.media = e.media;
    if (act.mediaSeek < 0) act.mediaSeek = e.mediaSeek;
    if (act.mediaPlayerVol < 0) act.mediaPlayerVol = e.mediaPlayerVol;
    if (act.mediaMasterVol < 0) act.mediaMasterVol = e.mediaMasterVol;
}

}   // namespace localbridge
}   // namespace xpt
