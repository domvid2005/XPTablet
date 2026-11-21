// Local Connect's wire format (see localwire.h): one field list per struct, used by the writer and the reader alike.
#include "localwire.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <utility>

namespace xpt {

// ---- the field lists -------------------------------------------------------------------------------------------------------------
// v(...) takes each field in turn. A field added to a struct gets its line here, at the END of its list's call (and
// wire::kVersion goes up); the tests compare the structs' sizes with those the lists were written for.

template <class V> static void wireFields(V& v, MercPt& x) { v.all(x.x, x.y); }
template <class V> static void wireFields(V& v, AptGeo::Freq& x) { v.all(x.kind, x.mhz, x.name); }
template <class V> static void wireFields(V& v, AptGeo::RunwayInfo& x) { v.all(x.surface, x.lit, x.displacedA, x.displacedB); }
template <class V> static void wireFields(V& v, AptGeo::Runway& x) { v.all(x.a, x.b, x.widthM, x.idA, x.idB, x.water); }
template <class V> static void wireFields(V& v, AptGeo::Line& x) { v.all(x.kind, x.pts); }
template <class V> static void wireFields(V& v, AptGeo::Stand& x) { v.all(x.p, x.hdg, x.name, x.type, x.uses, x.size, x.ops, x.airlines); }
template <class V> static void wireFields(V& v, AptGeo::Label& x) { v.all(x.p, x.text); }
template <class V> static void wireFields(V& v, AptGeo::Helipad& x) { v.all(x.p, x.id); }
template <class V> static void wireFields(V& v, AptGeo& x) {
    v.all(x.icao, x.name, x.kind, x.elevationFt, x.lat, x.lon, x.freqs, x.runwayInfo, x.lo, x.hi, x.pavement, x.runways, x.lines, x.stands, x.taxiways, x.helipads);
}
template <class V> static void wireFields(V& v, AptPoint& x) { v.all(x.icao, x.lat, x.lon, x.kind, x.name, x.rwyFt); }

template <class V> static void wireFields(V& v, SimWeather& x) {
    v.all(x.valid, x.windDir, x.windKt, x.visSm, x.tempC, x.dewC, x.qnhHpa, x.rain, x.altFt, x.clouds, x.cloudBaseFt, x.cloudTopFt, x.cloudCover, x.aloft, x.aloftFt, x.aloftDir, x.aloftKt);
}
template <class V> static void wireFields(V& v, MetarLookup& x) { v.all(x.busy, x.icao, x.raw, x.error); }
template <class V> static void wireFields(V& v, GroundState& x) {
    v.all(x.onGround, x.openSam, x.jetway, x.toliss, x.paxDoors, x.cargoDoors, x.paxDoor, x.cargoDoor, x.deice, x.chocks, x.extPower, x.hpAir, x.lpAir,
          x.jwDoors, x.jwReach, x.jwDoorPax, x.jwDocked, x.slides, x.beacon, x.enginesRunning, x.parkBrake, x.pushSource, x.bpStarted, x.bpConnected,
          x.landingLights, x.taxiLight, x.groundSpeedKt, x.cabinLight, x.cabinAuto, x.cabinReady, x.jwMoving, x.slidesAuto);
}
template <class V> static void wireFields(V& v, BrowserState::Tab& x) { v.all(x.id, x.title, x.url, x.loading, x.group); }
template <class V> static void wireFields(V& v, BrowserState& x) {   // (not its picture)
    v.all(x.status, x.message, x.texW, x.texH, x.url, x.title, x.loading, x.canBack, x.canForward, x.wantsKeyboard, x.tabs, x.activeTab, x.zoomPct, x.zoomAuto, x.fullscreen);
}
template <class V> static void wireFields(V& v, MediaState& x) {     // (not the covers)
    v.all(x.supported, x.active, x.app, x.title, x.artist, x.album, x.playing, x.canPrev, x.canNext, x.artW, x.artH, x.artPrevW, x.artPrevH, x.artChangedAt, x.artColor,
          x.radioRx, x.posSec, x.lenSec, x.canSeek, x.radioTx, x.ducked, x.radioSource, x.playerVolume, x.masterVolume);
}
template <class V> static void wireFields(V& v, VatsimPilot& x) {
    v.all(x.found, x.hasPlan, x.callsign, x.name, x.cid, x.server, x.rating, x.squawk, x.logon, x.altitudeFt, x.groundspeedKt, x.heading, x.qnhMb, x.lat, x.lon,
          x.rules, x.aircraft, x.aircraftFull, x.departure, x.arrival, x.alternate, x.cruiseAlt, x.cruiseTas, x.depTime, x.enroute, x.fuel, x.route, x.remarks, x.assignedSquawk);
}
template <class V> static void wireFields(V& v, VatsimController& x) { v.all(x.callsign, x.freq, x.name, x.text, x.facility, x.logon); }
template <class V> static void wireFields(V& v, AirspaceArea& x) { v.all(x.id, x.oceanic, x.labelLat, x.labelLon, x.la0, x.lo0, x.la1, x.lo1, x.rings); }
template <class V> static void wireFields(V& v, AirspaceSet::Fir& x) { v.all(x.icao, x.name, x.prefix, x.boundary); }
template <class V> static void wireFields(V& v, AirspaceSet& x) { v.all(x.areas, x.firs); }
template <class V> static void wireFields(V& v, MapState::Traffic& x) { v.all(x.lat, x.lon, x.altFt, x.hdg, x.gsKt, x.vsFpm, x.onGround, x.callsign, x.type); }
template <class V> static void wireFields(V& v, MapState::Navaid& x) { v.all(x.type, x.lat, x.lon, x.freq, x.bearing, x.ident, x.name, x.magCourse, x.gsAngle, x.airport, x.runway, x.dme); }
template <class V> static void wireFields(V& v, MapState::Fix& x) { v.all(x.lat, x.lon, x.ident, x.enroute); }
template <class V> static void wireFields(V& v, MapState::RoutePoint& x) { v.all(x.lat, x.lon, x.ident); }
template <class V> static void wireFields(V& v, MapState::Procedure& x) { v.all(x.name, x.kind, x.pts); }
template <class V> static void wireFields(V& v, FlightPlan::Leg& x) { v.all(x.ident, x.airway, x.alt, x.legNm, x.totalTime, x.type, x.lat, x.lon, x.cross); }
template <class V> static void wireFields(V& v, FlightPlan& x) {
    v.all(x.loaded, x.busy, x.status, x.callsign, x.aircraft, x.registration, x.units, x.origin, x.originName, x.originRunway, x.dest, x.destName, x.destRunway,
          x.alternate, x.alternateName, x.route, x.cruise, x.distanceNm, x.ete, x.costIndex, x.avgWind, x.sid, x.star,
          x.blockFuel, x.tripFuel, x.reserveFuel, x.altnFuel, x.taxiFuel, x.extraFuel, x.zfw, x.tow, x.ldw,
          x.oew, x.paxCount, x.paxWeight, x.bags, x.bagWeight, x.freight, x.payload, x.maxZfw, x.maxTow, x.maxLdw, x.pdfUrl, x.importedAt, x.ofpId, x.legs, x.divertKey);
}
template <class V> static void wireFields(V& v, AirportInfo::Freq& x) { v.all(x.what, x.mhz, x.note); }
template <class V> static void wireFields(V& v, AirportInfo& x) { v.all(x.icao, x.busy, x.found, x.geo, x.name, x.city, x.country, x.region, x.iata, x.freqs, x.ctaf, x.metar); }
template <class V> static void wireFields(V& v, DocEntry& x) { v.all(x.name, x.path, x.folder, x.pdf, x.bytes); }
template <class V> static void wireFields(V& v, DocsState& x) { v.all(x.listed, x.files, x.folders, x.shownPath, x.texW, x.texH, x.light, x.busy, x.error); }   // (not its picture)
template <class V> static void wireFields(V& v, LocalDevice& x) { v.all(x.name, x.kind, x.connected, x.batteryPct, x.charging, x.lastSeen); }
template <class V> static void wireFields(V& v, AcarsMsg& x) { v.all(x.up, x.station, x.type, x.text, x.utc, x.min, x.mrn, x.ra, x.answer); }
template <class V> static void wireFields(V& v, AcarsState& x) { v.all(x.inbox, x.status, x.lastPoll); }
template <class V> static void wireFields(V& v, SimState::NavHit& x) { v.all(x.type, x.ident, x.name, x.lat, x.lon, x.freq); }
template <class V> static void wireFields(V& v, Actions::AcarsOut& x) { v.all(x.to, x.type, x.packet); }
template <class V> static void wireFields(V& v, Actions::WebEvent& x) { v.all(x.type, x.x, x.y, x.code, x.code2, x.ctrl); }
template <class V> static void wireFields(V& v, Actions& x) {
    v.all(x.xpilotCmd, x.acarsOut, x.acarsFrom, x.localForget, x.localPairDeny, x.ground, x.groundIndex, x.metarSearch,
          x.webShown, x.webRetry, x.webX, x.webY, x.webW, x.webH, x.web, x.webNavigate, x.webNav, x.webTabNew, x.webTabGroup, x.webZoom, x.webZoomPct, x.webVolume,
          x.mediaPlayerVol, x.mediaMasterVol, x.webExitFullscreen, x.webTabUrl, x.webTabSelect, x.webTabClose,
          x.mapShown, x.mapLayouts, x.mapPointMinFt, x.airportsShown, x.backgroundsList, x.backgroundWanted, x.com1Standby, x.airportSearch, x.airportOpen, x.navSearch,
          x.docsList, x.trafficSelected, x.planImport, x.planAccept, x.planDismiss, x.planReview, x.planClear, x.planDivert, x.planUndoDivert,
          x.clearTileCache, x.resetBrowserData, x.docOpen, x.media, x.mediaSeek, x.mapLat, x.mapLon, x.mapLayoutNm, x.mapPointNm, x.mapNavNm, x.mapFixNm);
}

// SimState, a section at a time
template <class V> static void wireSection(V& v, SimState& s, int section) {
    MapState& m = s.map;
    switch (section) {
    case wire::S_CORE:
        v.all(s.acfIcao, s.zuluSec, s.onPanel, s.panel, s.panelEnabled, s.panelPowered, s.aviTabRunning, s.wxPlane, s.wxEmbed, s.toliss, s.brightness, s.pixelScale,
              s.batteryPct, s.xpilotLogin, s.xpilotCallsign, s.xpilotCom1, s.xpilotCom2, s.xpilotAircraft, s.xpilotCmds, s.fuelKg, s.charging, s.autoDimLevel, s.mac, s.fps,
              s.engineType);
        break;
    case wire::S_LOCAL: v.all(s.local.status, s.local.address, s.local.error, s.local.pairName, s.local.devices); break;   // (never the pairing code)
    case wire::S_ACARS: v.all(s.acars); break;
    case wire::S_WX: v.all(s.wx); break;
    case wire::S_BROWSER: v.all(s.browser); break;
    case wire::S_METAR: v.all(s.metar); break;
    case wire::S_GROUND: v.all(s.ground); break;
    case wire::S_AIRPORT: v.all(s.airport); break;
    case wire::S_AIRPORT_LIST: v.all(s.airportList, s.airportStatus); break;
    case wire::S_DOCS: v.all(s.docs); break;
    case wire::S_CHECKLIST: v.all(s.checklistText, s.checklistSource); break;
    case wire::S_PLAN: v.all(s.plan); break;
    case wire::S_PLAN_ORIGINAL: v.all(s.planOriginal); break;
    case wire::S_PLAN_PREVIEW: v.all(s.planPreview, s.planPreviewOpen, s.planDismissed, s.newPlanSeq, s.newPlanText); break;
    case wire::S_DATA_INFO: v.all(s.dataInfo); break;
    case wire::S_MEDIA: v.all(s.media); break;
    case wire::S_BACKGROUNDS: v.all(s.backgroundFiles, s.backgroundsFolder, s.bgW, s.bgH, s.bgPicture, s.bgAverage, s.bgAccent); break;   // (not the picture)
    case wire::S_NAV_HITS: v.all(s.navHits); break;
    case wire::S_VATSIM_ATC: v.all(s.vatsimAtc, s.vatsimAtcHave); break;
    case wire::S_MAP_CORE:
        v.all(m.havePos, m.lat, m.lon, m.hdgTrue, m.gsKt, m.utc, m.haveLocalOffset, m.localOffsetS, m.aptIndexing, m.aptCount, m.altFt, m.vsFpm, m.aglFt, m.onGround,
              m.vatsimStatus, m.airspaceStatus);
        break;
    case wire::S_MAP_AIRPORTS: v.all(m.airports); break;
    case wire::S_MAP_POINTS: v.all(m.airportPoints); break;
    case wire::S_MAP_TRAFFIC: v.all(m.traffic); break;
    case wire::S_MAP_NAVAIDS: v.all(m.navaids); break;
    case wire::S_MAP_FIXES: v.all(m.fixes); break;
    case wire::S_MAP_ROUTE: v.all(m.route, m.procedures, m.trafficRoute, m.routeStatus); break;
    case wire::S_MAP_VATSIM: v.all(m.vatsim); break;
    case wire::S_MAP_AIRSPACES: v.all(m.airspaces); break;
    default: v.fail(); break;
    }
}

namespace {

// (both ends are little-endian: x86, ARM and WebAssembly all are; the numbers are copied as they lie in memory)
struct Writer {
    std::string o;
    void fail() {}
    void var(std::uint64_t n) { while (n >= 0x80) { o.push_back((char)(n | 0x80)); n >>= 7; } o.push_back((char)n); }
    void svar(long long n) { var(((std::uint64_t)n << 1) ^ (std::uint64_t)(n >> 63)); }
    void operator()(bool& b) { o.push_back(b ? 1 : 0); }
    void operator()(int& n) { svar(n); }
    void operator()(unsigned int& n) { var(n); }
    void operator()(long long& n) { svar(n); }
    void operator()(float& f) { o.append((const char*)&f, sizeof f); }
    void operator()(double& d) { o.append((const char*)&d, sizeof d); }
    void operator()(std::string& s) { var(s.size()); o.append(s); }
    template <class T> void operator()(std::vector<T>& l) { var(l.size()); for (T& e : l) (*this)(e); }
    template <class T, std::size_t N> void operator()(T (&a)[N]) { for (T& e : a) (*this)(e); }
    template <class A, class B> void operator()(std::pair<A, B>& p) { (*this)(p.first); (*this)(p.second); }
    template <class T> void operator()(std::shared_ptr<const T>& p) { bool has = (bool)p; (*this)(has); if (has) (*this)(const_cast<T&>(*p)); }
    template <class T> auto operator()(T& x) -> decltype(wireFields(*this, x), void()) { wireFields(*this, x); }
    template <class... T> void all(T&... x) { ((*this)(x), ...); }
};

// (it reads what a device sent: every length is checked against what is left, and a list grows as it is read, so a count that
// lies cannot make it reserve more than the message could hold)
struct Reader {
    const unsigned char* p;
    const unsigned char* e;
    bool ok = true;
    void fail() { ok = false; }
    bool need(std::uint64_t n) { if (!ok || (std::uint64_t)(e - p) < n) { ok = false; return false; } return true; }
    std::uint64_t var() {
        std::uint64_t n = 0;
        for (int sh = 0; sh < 64; sh += 7) {
            if (!need(1)) return 0;
            const unsigned char c = *p++;
            n |= (std::uint64_t)(c & 0x7F) << sh;
            if (!(c & 0x80)) return n;
        }
        ok = false;
        return 0;
    }
    long long svar() { const std::uint64_t n = var(); return (long long)(n >> 1) ^ -(long long)(n & 1); }
    void operator()(bool& b) { if (need(1)) { const unsigned char c = *p++; if (c > 1) ok = false; else b = c != 0; } }
    void operator()(int& n) { const long long x = svar(); if (x < INT32_MIN || x > INT32_MAX) ok = false; else if (ok) n = (int)x; }
    void operator()(unsigned int& n) { const std::uint64_t x = var(); if (x > UINT32_MAX) ok = false; else if (ok) n = (unsigned int)x; }
    void operator()(long long& n) { const long long x = svar(); if (ok) n = x; }
    void operator()(float& f) { if (need(sizeof f)) { std::memcpy(&f, p, sizeof f); p += sizeof f; } }
    void operator()(double& d) { if (need(sizeof d)) { std::memcpy(&d, p, sizeof d); p += sizeof d; } }
    void operator()(std::string& s) { const std::uint64_t n = var(); if (need(n)) { s.assign((const char*)p, (std::size_t)n); p += n; } }
    template <class T> void operator()(std::vector<T>& l) {
        const std::uint64_t n = var();
        if (!need(n)) return;   // (every item takes a byte at least)
        l.clear();
        l.reserve((std::size_t)std::min<std::uint64_t>(n, 4096));
        for (std::uint64_t i = 0; i < n && ok; ++i) { l.emplace_back(); (*this)(l.back()); }
    }
    template <class T, std::size_t N> void operator()(T (&a)[N]) { for (T& x : a) (*this)(x); }
    template <class A, class B> void operator()(std::pair<A, B>& pr) { (*this)(pr.first); (*this)(pr.second); }
    template <class T> void operator()(std::shared_ptr<const T>& ptr) {
        bool has = false;
        (*this)(has);
        if (!ok) return;
        if (!has) { ptr.reset(); return; }
        auto made = std::make_shared<T>();
        (*this)(*made);
        if (ok) ptr = std::move(made);
    }
    template <class T> auto operator()(T& x) -> decltype(wireFields(*this, x), void()) { wireFields(*this, x); }
    template <class... T> void all(T&... x) { ((*this)(x), ...); }
};

}   // namespace

namespace wire {

std::string encodeSection(const SimState& s, int section) {
    Writer w;
    wireSection(w, const_cast<SimState&>(s), section);   // (the writer only reads)
    return std::move(w.o);
}

bool decodeSection(SimState& s, int section, const std::string& bytes) {
    if (section < 0 || section >= S_COUNT) return false;
    Reader r{(const unsigned char*)bytes.data(), (const unsigned char*)bytes.data() + bytes.size()};
    wireSection(r, s, section);
    if (section == S_LOCAL) s.local.pairCode.clear();
    return r.ok && r.p == r.e;   // (all of it, and nothing after it)
}

std::string encodeActions(const Actions& a) {
    Writer w;
    w(const_cast<Actions&>(a));
    return std::move(w.o);
}

bool decodeActions(Actions& a, const std::string& bytes) {
    Reader r{(const unsigned char*)bytes.data(), (const unsigned char*)bytes.data() + bytes.size()};
    r(a);
    return r.ok && r.p == r.e;
}

bool actionsHaveEvent(const Actions& a) {
    return !a.xpilotCmd.empty() || !a.acarsOut.empty() || !a.localForget.empty() || a.localPairDeny || a.ground != GA_NONE || !a.metarSearch.empty() ||
           a.webRetry || !a.web.empty() || !a.webNavigate.empty() || a.webNav != 0 || a.webTabNew || a.webZoom != 0 || a.webVolume >= 0 ||
           a.mediaPlayerVol >= 0 || a.mediaMasterVol >= 0 || a.webExitFullscreen || a.webTabSelect != 0 || a.webTabClose != 0 ||
           a.backgroundsList || a.com1Standby != 0 || !a.airportSearch.empty() || !a.airportOpen.empty() || a.docsList ||
           a.planImport || a.planAccept || a.planDismiss || a.planReview || a.planClear || a.planDivert || a.planUndoDivert ||
           a.clearTileCache || a.resetBrowserData || !a.docOpen.empty() || a.media != 0 || a.mediaSeek >= 0;
}

void restrictRemote(Actions& a, const SimState& s) {
    const Actions none;
    // pairing, the computer's browser, the computer's data: not a device's to touch
    a.localForget.clear(); a.localPairDeny = false;
    a.webShown = a.webRetry = a.webTabNew = a.webExitFullscreen = false;
    a.webX = a.webY = a.webW = a.webH = 0;
    a.web.clear(); a.webNavigate.clear(); a.webTabUrl.clear();
    a.webNav = a.webTabGroup = a.webZoom = a.webTabSelect = a.webTabClose = 0;
    a.webZoomPct = none.webZoomPct; a.webVolume = none.webVolume;
    a.clearTileCache = a.resetBrowserData = false;
    // names: only ones the device was shown
    auto among = [](const std::string& x, const std::vector<std::string>& l) { for (const std::string& e : l) if (e == x) return true; return false; };
    if (!a.xpilotCmd.empty() && !among(a.xpilotCmd, s.xpilotCmds)) a.xpilotCmd.clear();
    if (!a.backgroundWanted.empty() && !among(a.backgroundWanted, s.backgroundFiles)) a.backgroundWanted.clear();
    if (!a.docOpen.empty()) { bool listed = false; for (const DocEntry& d : s.docs.files) listed = listed || d.path == a.docOpen; if (!listed) a.docOpen.clear(); }
    // texts: no longer than the pages' own fields make them
    auto cap = [](std::string& x, std::size_t n) { if (x.size() > n) x.resize(n); };
    cap(a.metarSearch, 8); cap(a.airportOpen, 8); cap(a.airportSearch, 64); cap(a.navSearch, 64); cap(a.trafficSelected, 16); cap(a.acarsFrom, 16);
    if (a.acarsOut.size() > 8) a.acarsOut.resize(8);
    for (Actions::AcarsOut& m : a.acarsOut) { cap(m.to, 16); cap(m.type, 16); cap(m.packet, 2048); }
    // numbers: finite, and in the range each means something in
    auto num = [](double x, double lo, double hi, double otherwise) { return (x == x && x >= lo && x <= hi) ? x : otherwise; };   // (x == x: not NaN)
    if (a.ground < GA_NONE || a.ground > GA_BP_RECONNECT) { a.ground = GA_NONE; a.groundIndex = 0; }
    a.groundIndex = std::max(0, std::min(255, a.groundIndex));
    if (a.media < 0 || a.media > 3) a.media = 0;
    a.mediaSeek = num(a.mediaSeek, 0, 86400.0 * 2, -1);
    a.mediaPlayerVol = (float)num(a.mediaPlayerVol, 0, 1, -1); a.mediaMasterVol = (float)num(a.mediaMasterVol, 0, 1, -1);
    a.com1Standby = num(a.com1Standby, 118.0, 137.0, 0);
    a.mapLat = num(a.mapLat, -90, 90, 0); a.mapLon = num(a.mapLon, -180, 180, 0);
    a.mapLayoutNm = num(a.mapLayoutNm, 0, 500, 0); a.mapPointNm = num(a.mapPointNm, 0, 12000, 0);
    a.mapNavNm = num(a.mapNavNm, 0, 2000, 0); a.mapFixNm = num(a.mapFixNm, 0, 2000, 0);
    a.mapPointMinFt = (float)num(a.mapPointMinFt, 0, 30000, 0);
}

Sizes knownSizes() { return Sizes{7680, 1104, 632, 136, 1440, 256, 168, 312}; }

std::uint64_t fingerprint(const std::string& bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
    return h;
}

}   // namespace wire
}   // namespace xpt
