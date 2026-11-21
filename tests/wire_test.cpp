// Local Connect's wire format: a state filled in every section goes out and comes back the same; what is cut short or made up is
// refused without harm; the pairing code never leaves; a device's requests are kept to what a tablet may ask.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "localwire.h"

using namespace xpt;

static int failures = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); ++failures; } } while (0)

static SimState sample() {
    SimState s;
    s.acfIcao = "A321"; s.zuluSec = 47220; s.onPanel = true; s.panel[2] = 1024; s.panelEnabled = true; s.batteryPct = 73; s.brightness = 0.8f;
    s.xpilotLogin = 1; s.xpilotCallsign = "AAL1234"; s.xpilotCom1 = "KORD_TWR"; s.xpilotAircraft = 12; s.xpilotCmds = {"xpilot/toggle_tcas", "xpilot/ptt"};
    s.fuelKg = 6400.5f; s.charging = true; s.fps = 41.5f; s.engineType = 5; s.autoDimLevel = 0.6f;
    s.local.status = 2; s.local.address = "https://192.168.1.20:8443"; s.local.pairCode = "482913"; s.local.pairName = "Dom's iPhone";
    s.local.devices.push_back({"Dom's iPhone", "iPhone", true, 81, false, "Today 14:32"});
    AcarsMsg am; am.station = "KZNY"; am.type = "cpdlc"; am.text = "CLIMB TO FL360"; am.utc = 1795000000LL; am.min = 4; am.ra = "WU"; s.acars.inbox.push_back(am); s.acars.status = "Polling every minute";
    s.wx.valid = true; s.wx.windDir = 270; s.wx.windKt = 14; s.wx.clouds = 2; s.wx.cloudBaseFt[1] = 4500; s.wx.aloft = 3; s.wx.aloftKt[2] = 88;
    s.browser.status = 1; s.browser.url = "https://example.org"; s.browser.tabs.push_back({3, "Example", "https://example.org", false, 0}); s.browser.tex = 77;
    s.metar.icao = "KSBN"; s.metar.raw = "KSBN 211554Z 27014KT 10SM FEW045 03/M04 A3012";
    s.ground.toliss = true; s.ground.paxDoors = 4; s.ground.paxDoor[2] = 2; s.ground.jwDocked[1] = true; s.ground.cabinLight = 0.4f; s.ground.jwMoving[2] = true; s.ground.slidesAuto = true;
    auto geo = std::make_shared<AptGeo>();
    geo->icao = "KSBN"; geo->name = "South Bend Intl"; geo->elevationFt = 799; geo->freqs.push_back({4, 118.4, "Tower"}); geo->runwayInfo.push_back({1, true, 0, 120});
    geo->pavement = {{0.1, 0.2}, {0.3, 0.4}, {0.5, 0.6}};
    AptGeo::Runway rw; rw.idA = "09R"; rw.idB = "27L"; rw.a = {0.25, 0.37}; geo->runways.push_back(rw);
    AptGeo::Line ln; ln.kind = 1; ln.pts = {{0.1, 0.1}, {0.2, 0.2}}; geo->lines.push_back(ln);
    AptGeo::Stand st; st.name = "A3"; st.airlines = "AAL DAL"; geo->stands.push_back(st);
    geo->taxiways.push_back({{0.5, 0.5}, "B"}); geo->helipads.push_back({{0.6, 0.6}, "H1"});
    s.airport.icao = "KSBN"; s.airport.found = true; s.airport.geo = geo; s.airport.freqs.push_back({"Tower", 118.4, ""}); s.airport.ctaf = {"CTAF", 118.4, "when closed"}; s.airport.metar = "KSBN ...";
    s.airportList.push_back({"KSBN", 41.7, -86.3, 1, "South Bend Intl", 8414}); s.airportStatus = "2 found";
    s.docs.listed = true; s.docs.files.push_back({"chart.pdf", "/docs/chart.pdf", "/docs", true, 123456789012LL}); s.docs.folders = {"/docs"}; s.docs.shownPath = "/docs/chart.pdf"; s.docs.tex = 99;
    s.checklistText = "BEFORE START\n- Beacon: ON"; s.checklistSource = "A321.txt";
    s.plan.loaded = true; s.plan.origin = "KSBN"; s.plan.dest = "KORD"; s.plan.blockFuel = "5400"; s.plan.maxLdw = "77800";
    FlightPlan::Leg lg; lg.ident = "GIJ"; lg.lat = 41.77; lg.cross = "at or above FL220"; s.plan.legs.push_back(lg); s.plan.divertKey = "x";
    s.planOriginal.loaded = true; s.planOriginal.dest = "KMDW";
    s.planPreview.loaded = true; s.planPreview.ofpId = "1795_1"; s.planPreviewOpen = true; s.planDismissed = "1794_9"; s.newPlanSeq = 3; s.newPlanText = "KSBN to KORD";
    s.dataInfo.push_back({"Navigation data", "AIRAC 2611"});
    s.media.supported = s.media.active = true; s.media.title = "Song"; s.media.artColor = 0xFF203040u; s.media.posSec = 12.5; s.media.lenSec = 240; s.media.art = 55; s.media.masterVolume = 0.5f;
    s.backgroundFiles = {"alps.jpg"}; s.backgroundsFolder = "/bg"; s.bgPicture = "alps.jpg"; s.bgAverage = 0x102030; s.bgAccent = 0xFFAA00; s.bgTex = 44; s.bgW = 1920;
    s.navHits.push_back({3, "GIJ", "Gipper", 41.77, -86.32, 115.4f});
    s.vatsimAtc.push_back({"KORD_TWR", "120.750", "Sam", "ILS 28C", 4, "14:02Z"}); s.vatsimAtcHave = true;
    MapState& m = s.map;
    m.havePos = true; m.lat = 41.7; m.lon = -86.32; m.hdgTrue = 271; m.utc = 1795240000LL; m.localOffsetS = -18000; m.aptCount = 37000; m.altFt = 12000; m.vatsimStatus = "Fetching"; m.airspaceStatus = "Reading";
    m.airports.push_back(geo); m.airports.push_back(nullptr);
    m.airportPoints.push_back({"KORD", 41.98, -87.9, 1, "Chicago O'Hare", 13000});
    MapState::Traffic tr; tr.lat = 41.9; tr.callsign = "UAL5"; tr.type = "B738"; tr.onGround = true; m.traffic.push_back(tr);
    MapState::Navaid nv; nv.type = 4; nv.ident = "ISBN"; nv.magCourse = 271; nv.runway = "27L"; nv.dme = true; m.navaids.push_back(nv);
    m.fixes.push_back({41.5, -86.5, "KELSI", false});
    m.route.push_back({41.7, -86.3, "KSBN"}); m.procedures.push_back({"WYNDE2", 1, {{41.8, -87.0, "WYNDE"}}}); m.trafficRoute.push_back({40, -80, "ABC"}); m.routeStatus = "KSBN to KORD, 12 fixes";
    m.vatsim.found = true; m.vatsim.callsign = "UAL5"; m.vatsim.lat = 41.9; m.vatsim.assignedSquawk = "4211";
    auto air = std::make_shared<AirspaceSet>();
    AirspaceArea ar; ar.id = "KZAU"; ar.oceanic = false; ar.labelLat = 41; ar.rings = {{{41.0f, -88.0f}, {42.0f, -88.0f}, {42.0f, -86.0f}}}; air->areas.push_back(ar);
    air->firs.push_back({"KZAU", "Chicago", "CHI", "KZAU"});
    m.airspaces = air;
    return s;
}

int main() {
    const SimState src = sample();
    // every section: out, back into a fresh state, out again: the same bytes
    SimState back;
    std::size_t total = 0;
    for (int sec = 0; sec < wire::S_COUNT; ++sec) {
        const std::string a = wire::encodeSection(src, sec);
        total += a.size();
        if (!wire::decodeSection(back, sec, a)) { std::printf("FAIL: section %d does not read back\n", sec); ++failures; continue; }
        if (wire::encodeSection(back, sec) != a) { std::printf("FAIL: section %d reads back differently\n", sec); ++failures; }
        if (wire::encodeSection(SimState(), sec) == a) { std::printf("FAIL: the sample leaves section %d empty (nothing tested)\n", sec); ++failures; }
        // cut short anywhere, or with something after it: refused
        for (std::size_t n = 0; n < a.size(); n += (a.size() > 400 ? 7 : 1)) { SimState t; if (wire::decodeSection(t, sec, a.substr(0, n))) { std::printf("FAIL: section %d accepted cut to %zu of %zu\n", sec, n, a.size()); ++failures; break; } }
        { SimState t; CHECK(!wire::decodeSection(t, sec, a + "x"), "bytes after a section are refused"); }
    }
    CHECK(!wire::decodeSection(back, wire::S_COUNT, ""), "an unknown section is refused");
    CHECK(!wire::decodeSection(back, -1, ""), "a negative section is refused");
    // spot checks through the nesting
    CHECK(back.acfIcao == "A321" && back.xpilotCmds.size() == 2 && back.fuelKg == 6400.5f && back.panel[2] == 1024, "the core fields arrive");
    CHECK(back.ground.paxDoor[2] == 2 && back.ground.jwDocked[1] && !back.ground.jwDocked[0] && back.ground.jwMoving[2] && back.ground.slidesAuto, "the ground state arrives");
    CHECK(back.airport.geo && back.airport.geo->runways.size() == 1 && back.airport.geo->runways[0].idB == "27L" && back.airport.geo->pavement.size() == 3 && back.airport.geo->pavement[2].y == 0.6, "an airport's layout arrives");
    CHECK(back.map.airports.size() == 2 && back.map.airports[0] && !back.map.airports[1], "a missing layout stays missing");
    CHECK(back.map.airspaces && back.map.airspaces->areas[0].rings[0].size() == 3 && back.map.airspaces->areas[0].rings[0][1].first == 42.0f, "the airspaces arrive");
    CHECK(back.docs.files[0].bytes == 123456789012LL && back.map.utc == 1795240000LL && back.map.localOffsetS == -18000, "large and negative numbers arrive");
    CHECK(back.plan.legs[0].cross == "at or above FL220" && back.planPreviewOpen && back.newPlanSeq == 3, "the flight plans arrive");
    CHECK(back.media.artColor == 0xFF203040u && back.media.posSec == 12.5, "the media state arrives");
    // what never crosses
    CHECK(back.local.pairCode.empty() && wire::encodeSection(src, wire::S_LOCAL).find("482913") == std::string::npos, "the pairing code is never sent");
    CHECK(back.local.devices.size() == 1 && back.local.address == src.local.address, "Local Connect's status arrives");
    CHECK(back.browser.tex == 0 && back.docs.tex == 0 && back.media.art == 0 && back.bgTex == 0, "textures are not sent");
    { SimState t; t.local.pairCode = "111111"; wire::decodeSection(t, wire::S_LOCAL, wire::encodeSection(src, wire::S_LOCAL)); CHECK(t.local.pairCode.empty(), "a device never holds a pairing code"); }
    // a section only changes its own part
    { SimState t; t.acfIcao = "B738"; wire::decodeSection(t, wire::S_WX, wire::encodeSection(src, wire::S_WX)); CHECK(t.acfIcao == "B738" && t.wx.windKt == 14, "a section leaves the others alone"); }
    // fingerprints
    CHECK(wire::fingerprint(wire::encodeSection(src, wire::S_GROUND)) == wire::fingerprint(wire::encodeSection(back, wire::S_GROUND)), "equal sections, equal fingerprints");
    { SimState t = src; t.ground.chocks = !t.ground.chocks; CHECK(wire::fingerprint(wire::encodeSection(src, wire::S_GROUND)) != wire::fingerprint(wire::encodeSection(t, wire::S_GROUND)), "a change changes the fingerprint"); }

    // Actions
    Actions a;
    a.xpilotCmd = "xpilot/toggle_tcas"; a.acarsOut.push_back({"KZNY", "cpdlc", "/data2/4//N/WILCO"}); a.acarsFrom = "AAL1234"; a.ground = GA_PAX_DOOR; a.groundIndex = 2;
    a.metarSearch = "KORD"; a.web.push_back({1, 10.5f, 20.5f, 0, 0, 0}); a.mapShown = true; a.mapLat = 41.7; a.mapLon = -86.3; a.mapLayoutNm = 12; a.com1Standby = 120.75;
    a.navSearch = "GIJ"; a.trafficSelected = "UAL5"; a.planImport = true; a.docOpen = "/docs/chart.pdf"; a.media = 1; a.mediaSeek = 33.5; a.backgroundWanted = "alps.jpg";
    const std::string ab = wire::encodeActions(a);
    Actions a2;
    CHECK(wire::decodeActions(a2, ab) && wire::encodeActions(a2) == ab, "what the user asked for goes out and comes back");
    CHECK(a2.acarsOut.size() == 1 && a2.acarsOut[0].packet == "/data2/4//N/WILCO" && a2.ground == GA_PAX_DOOR && a2.groundIndex == 2 && a2.com1Standby == 120.75 && a2.mediaSeek == 33.5, "its fields arrive");
    for (std::size_t n = 0; n < ab.size(); ++n) { Actions t; if (wire::decodeActions(t, ab.substr(0, n))) { std::printf("FAIL: actions accepted cut to %zu\n", n); ++failures; break; } }
    CHECK(wire::actionsHaveEvent(a), "a press is an event");
    { Actions lv; lv.mapShown = true; lv.mapLat = 41; lv.airportsShown = true; lv.navSearch = "GIJ"; lv.trafficSelected = "UAL5"; lv.acarsFrom = "AAL1"; CHECK(!wire::actionsHaveEvent(lv), "what is merely shown is not an event"); }
    CHECK(!wire::actionsHaveEvent(Actions()), "nothing asked is not an event");
    { Actions one; one.planAccept = true; CHECK(wire::actionsHaveEvent(one), "Import is an event"); }
    { Actions one; one.media = 3; CHECK(wire::actionsHaveEvent(one), "Next is an event"); }

    // a device's requests, kept to a tablet's
    {
        Actions r = a;
        r.localForget = "Dom's iPad"; r.localPairDeny = true; r.resetBrowserData = true; r.clearTileCache = true; r.webNavigate = "https://evil.example"; r.webTabNew = true; r.webShown = true;
        wire::restrictRemote(r, src);
        CHECK(r.localForget.empty() && !r.localPairDeny, "a device cannot manage pairing");
        CHECK(!r.resetBrowserData && !r.clearTileCache && r.webNavigate.empty() && !r.webTabNew && !r.webShown && r.web.empty(), "a device cannot drive the computer's browser or delete its data");
        CHECK(r.docOpen == "/docs/chart.pdf" && r.xpilotCmd == "xpilot/toggle_tcas" && r.backgroundWanted == "alps.jpg" && r.ground == GA_PAX_DOOR && r.com1Standby == 120.75, "what the pages offer goes through");
        r.docOpen = "/etc/passwd"; r.xpilotCmd = "sim/operation/quit"; r.backgroundWanted = "../../secret.png"; r.ground = 9999; r.com1Standby = 1e9; r.mapLat = std::nan(""); r.mediaSeek = -5; r.media = 12;
        r.metarSearch = std::string(5000, 'A'); r.acarsOut.assign(50, {"X", "telex", std::string(100000, 'p')}); r.mediaMasterVol = 7;
        wire::restrictRemote(r, src);
        CHECK(r.docOpen.empty(), "a file not on the Documents page cannot be opened");
        CHECK(r.xpilotCmd.empty(), "a command that is not xPilot's cannot be run");
        CHECK(r.backgroundWanted.empty(), "a picture outside the backgrounds folder cannot be asked for");
        CHECK(r.ground == GA_NONE && r.com1Standby == 0 && r.mapLat == 0 && r.mediaSeek == -1 && r.media == 0 && r.mediaMasterVol == -1, "numbers out of range are dropped");
        CHECK(r.metarSearch.size() == 8 && r.acarsOut.size() == 8 && r.acarsOut[0].packet.size() == 2048, "texts are kept to their sizes");
    }

    // made-up bytes: read without harm (run under the sanitizers too), and whatever is accepted writes back out
    {
        std::mt19937 rng(20261121);
        int accepted = 0;
        for (int it = 0; it < 20000; ++it) {
            std::string junk((std::size_t)(rng() % 96), '\0');
            for (char& c : junk) c = (char)(rng() % 4 == 0 ? rng() % 256 : rng() % 4);
            const int sec = (int)(rng() % wire::S_COUNT);
            SimState t;
            if (wire::decodeSection(t, sec, junk)) { ++accepted; wire::encodeSection(t, sec); }
            Actions ta;
            if (wire::decodeActions(ta, junk)) { ++accepted; wire::restrictRemote(ta, src); wire::encodeActions(ta); }
        }
        // a valid message with bytes flipped
        const std::string good = wire::encodeSection(src, wire::S_AIRPORT);
        for (int it = 0; it < 4000; ++it) { std::string b = good; b[rng() % b.size()] = (char)(rng() % 256); SimState t; if (wire::decodeSection(t, wire::S_AIRPORT, b)) wire::encodeSection(t, wire::S_AIRPORT); }
        { std::string huge; huge.push_back((char)0xFF); huge.push_back((char)0xFF); huge.push_back((char)0xFF); huge.push_back((char)0xFF); huge.push_back((char)0x0F); SimState t; CHECK(!wire::decodeSection(t, wire::S_MAP_TRAFFIC, huge), "a list claiming more than the message holds is refused"); }
        std::printf("wire_test: %zu bytes for the sample state, %d made-up messages happened to be well-formed\n", total, accepted);
    }

    // the structs the lists were written for (checked where they were measured: Linux, x86-64, GCC)
#if defined(__linux__) && defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__GLIBCXX__)
    {
        const wire::Sizes k = wire::knownSizes();
        const bool same = k.simState == sizeof(SimState) && k.mapState == sizeof(MapState) && k.actions == sizeof(Actions) && k.groundState == sizeof(GroundState) &&
                          k.flightPlan == sizeof(FlightPlan) && k.mediaState == sizeof(MediaState) && k.browserState == sizeof(BrowserState) && k.aptGeo == sizeof(AptGeo);
        if (!same) {
            std::printf("FAIL: a struct changed (SimState %zu, MapState %zu, Actions %zu, GroundState %zu, FlightPlan %zu, MediaState %zu, BrowserState %zu, AptGeo %zu): "
                        "add the new field to its list in src/localwire.cpp, raise wire::kVersion, then update knownSizes()\n",
                        sizeof(SimState), sizeof(MapState), sizeof(Actions), sizeof(GroundState), sizeof(FlightPlan), sizeof(MediaState), sizeof(BrowserState), sizeof(AptGeo));
            ++failures;
        }
    }
#endif
    if (failures) { std::printf("wire_test: %d failed\n", failures); return 1; }
    std::printf("wire_test: ok\n");
    return 0;
}
