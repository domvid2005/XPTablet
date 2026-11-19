#include "simbrief.h"
#include <vector>
#include <functional>
#include <set>
#include <map>

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
        p.alternate = str(alt, "icao_code"); p.alternateName = str(alt, "name");
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
        // the load: empty weight, passengers and their bags, freight, the payload of them all; and the limits
        p.oew = str(wts, "oew"); p.payload = str(wts, "payload"); p.freight = str(wts, "freight_added");
        p.paxCount = str(wts, "pax_count_actual"); if (p.paxCount.empty()) p.paxCount = str(wts, "pax_count");
        p.bags = str(wts, "bag_count_actual"); if (p.bags.empty()) p.bags = str(wts, "bag_count");
        {
            const double nPax = std::atof(p.paxCount.c_str()), per = num(wts, "pax_weight"), nBag = std::atof(p.bags.c_str()), perBag = num(wts, "bag_weight");
            if (nPax > 0 && per > 0) p.paxWeight = std::to_string((long long)std::llround(nPax * per));
            if (nBag > 0 && perBag > 0) p.bagWeight = std::to_string((long long)std::llround(nBag * perBag));
        }
        p.maxZfw = str(wts, "max_zfw"); p.maxTow = str(wts, "max_tow"); p.maxLdw = str(wts, "max_ldw");
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

void planFetched(SimState& s, const FlightPlan& p, bool quiet) {
    if (quiet) {
        if (!p.loaded || p.ofpId.empty() || p.ofpId == s.plan.ofpId || p.ofpId == s.planDismissed || p.ofpId == s.planPreview.ofpId) return;
        s.planPreview = p; s.planPreviewOpen = true;
        ++s.newPlanSeq;
        s.newPlanText = p.origin + " to " + p.dest + (p.callsign.empty() ? std::string() : " (" + p.callsign + ")") + ". Tap to review it.";
        return;
    }
    s.plan.busy = false;
    if (p.loaded) { s.planPreview = p; s.planPreviewOpen = true; s.plan.status = s.plan.loaded ? s.plan.status : std::string(); }
    else s.plan.status = p.status;   // a failed fetch keeps the plan there was
}
void planDecide(SimState& s, const Actions& a) {
    if (a.planAccept && s.planPreview.loaded) {
        s.plan = s.planPreview;
        s.plan.busy = false;
        s.plan.status = "Imported " + s.planPreview.importedAt;
        s.planPreview = FlightPlan(); s.planPreviewOpen = false;
        planToRoute(s.plan, s.map);
    } else if (a.planDismiss && s.planPreview.loaded) {   // closed, not forgotten: Review on the page opens it again
        s.planDismissed = s.planPreview.ofpId;
        s.planPreviewOpen = false;
    } else if (a.planReview && s.planPreview.loaded) s.planPreviewOpen = true;
}


// ---- crossing restrictions, from X-Plane's CIFP (one file per airport: "SID:" and "STAR:" records, comma-separated, ARINC 424's
// fields: 2 the procedure, 3 its transition, 4 the fix, 22 the altitude restriction (blank: at, + at or above, - at or below, B
// between the two), 23 and 24 the altitudes ("12000", "FL230"), 27 the speed limit in knots)
static std::string crossText(const std::string& desc, const std::string& a1, const std::string& a2, const std::string& spd) {
    std::string t;
    if (!a1.empty()) {
        if (desc == "+") t = "at or above " + a1;
        else if (desc == "-") t = "at or below " + a1;
        else if (desc == "B" && !a2.empty()) t = "between " + a2 + " and " + a1;   // (the upper one first in the record)
        else t = "at " + a1;
    }
    if (!spd.empty()) t += (t.empty() ? "" : ", ") + spd + " kt";
    return t;
}
int addCrossings(FlightPlan& p, const std::function<std::string(const std::string&)>& cifpText) {
    auto trim = [](std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') ++i; return s.substr(i); };
    // each fix of a procedure that has a restriction: the one on the plan's own transition first (the runway's, or one the route
    // goes through), else the first one found
    auto collect = [&](const std::string& icao, const char* kind, const std::string& proc, const std::string& runway) {
        std::map<std::string, std::string> best, any;
        if (icao.empty() || proc.empty()) return best;
        const std::string text = cifpText(icao);
        std::set<std::string> onRoute;
        for (const FlightPlan::Leg& l : p.legs) onRoute.insert(l.ident);
        const std::string head = std::string(kind) + ":";
        size_t pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(pos, e - pos);
            pos = e + 1;
            if (line.compare(0, head.size(), head) != 0) continue;
            std::vector<std::string> f;
            size_t a = head.size();
            while (a <= line.size()) { size_t c = line.find(',', a); if (c == std::string::npos) c = line.size(); f.push_back(trim(line.substr(a, c - a))); a = c + 1; }
            if (f.size() < 28 || f[2] != proc || f[4].empty()) continue;
            const std::string t = crossText(f[22], f[23], f[24], f[27]);
            if (t.empty()) continue;
            const bool ours = f[3] == "RW" + runway || (f[3].size() > 2 && f[3].compare(0, 2, "RW") == 0 && runway.empty()) || onRoute.count(f[3]) || f[3] == f[4];
            if (ours && !best.count(f[4])) best[f[4]] = t;
            if (!any.count(f[4])) any[f[4]] = t;
        }
        for (const auto& kv : any) if (!best.count(kv.first)) best[kv.first] = kv.second;
        return best;
    };
    const std::map<std::string, std::string> sid = collect(p.origin, "SID", p.sid, p.originRunway);
    const std::map<std::string, std::string> star = collect(p.dest, "STAR", p.star, p.destRunway);
    int n = 0;
    const size_t half = p.legs.size() / 2;
    for (size_t i = 0; i < p.legs.size(); ++i) {   // the SID's first on the first half of the route, the STAR's first on the second
        FlightPlan::Leg& l = p.legs[i];
        const auto& first = i < half ? sid : star;
        const auto& second = i < half ? star : sid;
        std::string c;
        const auto f1 = first.find(l.ident);
        if (f1 != first.end()) c = f1->second;
        else { const auto f2 = second.find(l.ident); if (f2 != second.end()) c = f2->second; }
        if (c.empty()) continue;
        l.cross = c; ++n;
    }
    return n;
}

} // namespace xpt
