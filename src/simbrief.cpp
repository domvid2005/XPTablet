#include "simbrief.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "json_mini.h"

namespace xpt {
namespace {
std::string hmOf(double sec) {   // 4500 -> "1h 15m"
    if (sec <= 0) return std::string();
    const int m = (int)std::lround(sec / 60.0);
    char b[24];
    if (m >= 60) std::snprintf(b, sizeof b, "%dh %02dm", m / 60, m % 60); else std::snprintf(b, sizeof b, "%d min", m);
    return b;
}
std::string clockOfSec(double sec) {   // 2520 -> "0:42"
    const int m = (int)std::lround(sec / 60.0);
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", m / 60, m % 60);
    return b;
}
} // namespace

std::string simbriefUrl(const std::string& user) {
    std::string q;
    for (unsigned char c : user) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.') q += (char)c; else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); q += b; } }
    const bool id = !user.empty() && std::all_of(user.begin(), user.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    return std::string("https://www.simbrief.com/api/xml.fetcher.php?") + (id ? "userid=" : "username=") + q + "&json=1";
}

FlightPlan parseSimbrief(const std::string& body) {
    FlightPlan p;
    wx::Json j;
    if (!wx::parseJson(body, j)) p.status = "SimBrief's answer could not be read";
    else {
        auto str = [](const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asString() : std::string(); };
        auto num = [](const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asNum(0) : 0.0; };
        const wx::Json* gen = j.get("general");
        const wx::Json* org = j.get("origin");
        const wx::Json* dst = j.get("destination");
        const wx::Json* alt = j.get("alternate");
        if (alt && alt->type == wx::Json::Arr && !alt->arr.empty()) alt = &alt->arr[0];
        const wx::Json* fuel = j.get("fuel");
        const wx::Json* wts = j.get("weights");
        const wx::Json* times = j.get("times");
        const wx::Json* acf = j.get("aircraft");
        const wx::Json* atc = j.get("atc");
        const wx::Json* prm = j.get("params");
        const wx::Json* files = j.get("files");
        const wx::Json* nav = j.get("navlog");
        const wx::Json* fix = nav ? nav->get("fix") : nullptr;
        p.origin = str(org, "icao_code"); p.originName = str(org, "name"); p.originRunway = str(org, "plan_rwy");
        p.dest = str(dst, "icao_code"); p.destName = str(dst, "name"); p.destRunway = str(dst, "plan_rwy");
        p.alternate = str(alt, "icao_code");
        p.callsign = str(atc, "callsign");
        if (p.callsign.empty()) p.callsign = str(gen, "icao_airline") + str(gen, "flight_number");
        p.aircraft = str(acf, "icaocode"); if (p.aircraft.empty()) p.aircraft = str(acf, "icao_code");
        p.registration = str(acf, "reg");
        p.units = str(prm, "units");
        p.route = str(gen, "route");
        const double fl = num(gen, "initial_altitude");
        if (fl > 0) { char b[16]; std::snprintf(b, sizeof b, fl >= 18000 ? "FL%03.0f" : "%.0f ft", fl >= 18000 ? fl / 100 : fl); p.cruise = b; }
        p.distanceNm = str(gen, "route_distance");
        p.ete = hmOf(num(times, "est_time_enroute"));
        p.costIndex = str(gen, "costindex");
        const double wind = num(gen, "avg_wind_comp");
        if (wind != 0) { char b[24]; std::snprintf(b, sizeof b, "%s%.0f kt", wind > 0 ? "tail " : "head ", std::fabs(wind)); p.avgWind = b; }
        p.blockFuel = str(fuel, "plan_ramp"); p.tripFuel = str(fuel, "enroute_burn"); p.reserveFuel = str(fuel, "reserve");
        p.altnFuel = str(fuel, "alternate_burn"); p.taxiFuel = str(fuel, "taxi"); p.extraFuel = str(fuel, "extra");
        p.ofpId = str(prm, "request_id") + "/" + str(prm, "time_generated");
        p.zfw = str(wts, "est_zfw"); p.tow = str(wts, "est_tow"); p.ldw = str(wts, "est_ldw");
        {   // the briefing: SimBrief's PDF of the OFP
            const wx::Json* pdf = files ? files->get("pdf") : nullptr;
            const std::string dir = str(files, "directory"), link = str(pdf, "link");
            if (!link.empty()) p.pdfUrl = link.compare(0, 4, "http") == 0 ? link : dir + link;
        }
        auto addLeg = [&](const wx::Json& f) {
            FlightPlan::Leg l;
            l.ident = str(&f, "ident"); l.airway = str(&f, "via_airway"); l.type = str(&f, "type");
            if (l.airway == "DCT") l.airway.clear();
            const double a = num(&f, "altitude_feet");
            if (a > 0) { char b[16]; std::snprintf(b, sizeof b, a >= 18000 ? "FL%03.0f" : "%.0f", a >= 18000 ? a / 100 : a); l.alt = b; }
            l.legNm = str(&f, "distance");
            l.totalTime = clockOfSec(num(&f, "time_total"));
            l.lat = num(&f, "pos_lat"); l.lon = num(&f, "pos_long");
            if (str(&f, "is_sid_star") == "1") { if (p.legs.empty() || p.sid.empty()) { if (p.star.empty() && p.legs.size() < 8) p.sid = l.airway; else p.star = l.airway; } }
            if (l.ident == "TOC" || l.ident == "TOD") return;
            p.legs.push_back(l);
        };
        if (fix && fix->type == wx::Json::Arr) for (const wx::Json& f : fix->arr) addLeg(f);
        else if (fix && fix->type == wx::Json::Obj) addLeg(*fix);
        if (p.origin.empty() || p.dest.empty()) {
            const wx::Json* fe = j.get("fetch");
            p.status = "SimBrief: " + (fe ? str(fe, "status") : std::string("no plan"));
        } else {
            p.loaded = true;
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &now);
#else
            gmtime_r(&now, &tm);
#endif
            char b[16];
            std::snprintf(b, sizeof b, "%02d:%02dZ", tm.tm_hour, tm.tm_min);
            p.importedAt = b;
            p.status = "Imported " + p.origin + " to " + p.dest + " at " + b;
        }
    }
    return p;
}

void planToRoute(const FlightPlan& p, MapState& m) {
    m.route.clear();
    if (!p.loaded) { m.routeStatus = "Import a plan on the Flight Plan page"; return; }
    for (const auto& l : p.legs) if (l.lat != 0 || l.lon != 0) m.route.push_back({l.lat, l.lon, l.ident});
    m.routeStatus = p.origin + " to " + p.dest + " (SimBrief)";
}

} // namespace xpt
