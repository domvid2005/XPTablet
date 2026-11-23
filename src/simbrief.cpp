#include "simbrief.h"
#include <vector>
#include <functional>
#include <set>
#include <map>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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
        {   // the briefing's parts: weather (METAR and TAF of departure, destination, alternates), NOTAMs, the ATC plan, the OFP's text
            const wx::Json* wxj = j.get("weather");
            auto reports = [&](const char* key, const std::string& head) {   // a string, or a list of them (several alternates)
                const wx::Json* v = wxj ? wxj->get(key) : nullptr;
                if (!v) return;
                std::vector<std::string> texts;
                if (v->type == wx::Json::Arr) { for (const wx::Json& e : v->arr) texts.push_back(e.asString()); } else texts.push_back(v->asString());
                for (std::string& t : texts) { while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back(); if (!t.empty()) p.weather.push_back({head, t}); }
            };
            const std::string o = p.origin, d = p.dest, a = p.alternate.empty() ? std::string("Alternate") : p.alternate;
            reports("orig_metar", o + " METAR"); reports("orig_taf", o + " TAF");
            reports("dest_metar", d + " METAR"); reports("dest_taf", d + " TAF");
            reports("altn_metar", a + " METAR"); reports("altn_taf", a + " TAF");
            reports("toaltn_metar", "Takeoff Alternate METAR"); reports("toaltn_taf", "Takeoff Alternate TAF");
            reports("eualtn_metar", "En Route Alternate METAR"); reports("eualtn_taf", "En Route Alternate TAF");
            auto notamsOf = [&](const wx::Json* apt, const std::string& icao) {
                const wx::Json* n = apt ? apt->get("notam") : nullptr;
                if (!n) return;
                auto one = [&](const wx::Json& e) {
                    std::string t = str(&e, "notam_text"); if (t.empty()) t = str(&e, "notam_raw"); if (t.empty()) t = str(&e, "text"); if (t.empty() && e.type == wx::Json::Str) t = e.str;
                    if (t.empty()) return;
                    const std::string id = str(&e, "notam_id");
                    p.notams.push_back({icao + (id.empty() ? std::string() : "  " + id), t});
                };
                if (n->type == wx::Json::Arr) { for (const wx::Json& e : n->arr) one(e); } else one(*n);
            };
            notamsOf(org, p.origin); notamsOf(dst, p.dest); notamsOf(alt, p.alternate);
            p.atcPlan = str(atc, "flightplan_text");
            const wx::Json* tx = j.get("text");
            const std::string html = str(tx, "plan_html");
            if (!html.empty()) {   // markup out: <br> and the end of a block as new lines, entities decoded, the rest as it is (it is a <pre>)
                std::string out;
                for (size_t i = 0; i < html.size();) {
                    if (html[i] == '<') {
                        const size_t e = html.find('>', i);
                        if (e == std::string::npos) break;
                        std::string tag = html.substr(i + 1, e - i - 1);
                        for (char& c : tag) c = (char)std::tolower((unsigned char)c);
                        if (tag.compare(0, 2, "br") == 0 || tag == "/div" || tag == "/p" || tag == "/h1" || tag == "/h2" || tag == "/h3" || tag == "/tr") out += '\n';
                        if (tag.compare(0, 2, "h2") == 0 && tag.find("page-break") != std::string::npos) out += "\n\f";   // (a new page)
                        i = e + 1;
                    } else if (html[i] == '&') {
                        const size_t e = html.find(';', i);
                        const std::string ent = e == std::string::npos || e - i > 8 ? std::string() : html.substr(i, e - i + 1);
                        if (ent == "&amp;") out += '&'; else if (ent == "&lt;") out += '<'; else if (ent == "&gt;") out += '>'; else if (ent == "&quot;") out += '"';
                        else if (ent == "&nbsp;" || ent == "&#160;") out += ' '; else if (ent == "&#39;" || ent == "&apos;") out += '\'';
                        else { out += '&'; ++i; continue; }
                        i = e + 1;
                    } else if (html[i] == '\r') ++i;
                    else out += html[i++];
                }
                p.ofpText = out;
            }
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


// ---- the plan's procedures as lines (SID, STAR, approach), from X-Plane's CIFP --------------------------------------------------------
namespace {
struct ProcRec { std::string proc, trans, fix, rtype; };
std::string trimS(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') ++i; return s.substr(i); }
std::vector<std::string> procLines(const std::string& text, const char* head) {
    std::vector<std::string> out; const size_t hl = std::strlen(head);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
        if (text.compare(pos, hl, head) == 0) out.push_back(text.substr(pos + hl, e - pos - hl));
        pos = e + 1;
    }
    return out;
}
std::vector<ProcRec> procRecs(const std::string& text, const char* head) {
    std::vector<ProcRec> out;
    for (const std::string& line : procLines(text, head)) {
        std::vector<std::string> f; size_t a = 0;
        while (a <= line.size() && f.size() < 6) { size_t c = line.find(',', a); if (c == std::string::npos) c = line.size(); f.push_back(trimS(line.substr(a, c - a))); a = c + 1; }
        if (f.size() >= 5) out.push_back({f[2], f[3], f[4], f[1]});
    }
    return out;
}
// "N41422140" / "W086181618": degrees, minutes, seconds and hundredths
bool dmsTok(const std::string& t, double& v) {
    if (t.empty()) return false;
    const char h = t[0]; const bool lat = h == 'N' || h == 'S';
    if (!lat && h != 'E' && h != 'W') return false;
    const size_t dd = lat ? 2 : 3;
    if (t.size() < 1 + dd + 6) return false;
    for (size_t i = 1; i < 1 + dd + 6; ++i) if (t[i] < '0' || t[i] > '9') return false;
    const double d = std::atof(t.substr(1, dd).c_str()), m = std::atof(t.substr(1 + dd, 2).c_str()), sec = std::atof(t.substr(3 + dd, 4).c_str()) / 100.0;
    v = d + m / 60.0 + sec / 3600.0; if (h == 'S' || h == 'W') v = -v;
    return true;
}
std::map<std::string, std::pair<double, double>> runwayEnds(const std::string& text) {   // "RW27" -> its threshold
    std::map<std::string, std::pair<double, double>> out;
    for (const std::string& line : procLines(text, "RWY:")) {
        const size_t c = line.find(',');
        const std::string id = trimS(line.substr(0, c));
        double la = 0, lo = 0, v; bool gl = false, go = false;
        std::string tok;
        for (size_t i = 0; i <= line.size(); ++i) {
            const char ch = i < line.size() ? line[i] : ';';
            if (ch == ';' || ch == ',' || ch == ' ') {
                if (!gl && dmsTok(tok, v) && (tok[0] == 'N' || tok[0] == 'S')) { la = v; gl = true; }
                else if (!go && dmsTok(tok, v) && (tok[0] == 'E' || tok[0] == 'W')) { lo = v; go = true; }
                tok.clear();
            } else tok += ch;
        }
        if (gl && go && !id.empty()) out[id] = {la, lo};
    }
    return out;
}
// a transition for the runway: "RW27", "RW09B" (both of 09L and 09R), "ALL"
bool forRunway(const std::string& trans, const std::string& rwy) {
    if (trans.size() < 3 || trans.compare(0, 2, "RW") != 0) return trans == "ALL";
    if (rwy.empty() || trans == "RW" + rwy) return true;
    std::string digits = rwy; while (!digits.empty() && !(digits.back() >= '0' && digits.back() <= '9')) digits.pop_back();
    return trans == "RW" + digits + "B" || (trans == "RW" + digits && digits != rwy);
}
}   // namespace

std::vector<MapState::Procedure> buildProcedures(const FlightPlan& p, const std::function<std::string(const std::string&)>& cifpText,
    const std::function<bool(const std::string&, double, double, double&, double&)>& resolveFix) {
    std::vector<MapState::Procedure> out;
    if (!p.loaded) return out;
    std::set<std::string> onRoute; for (const FlightPlan::Leg& l : p.legs) onRoute.insert(l.ident);
    auto anchor = [&](const std::string& icao, double& la, double& lo) {
        for (const FlightPlan::Leg& l : p.legs) if (l.ident == icao && (l.lat != 0 || l.lon != 0)) { la = l.lat; lo = l.lon; return true; }
        return resolveFix(icao, 0, 0, la, lo);
    };
    // fixes in order, each found near the one before; a runway's own fix at its threshold; repeats and far-off finds left out
    auto line = [&](const std::vector<const ProcRec*>& recs, const std::map<std::string, std::pair<double, double>>& rw, double la, double lo, double aLa, double aLo) {
        std::vector<MapState::RoutePoint> pts;
        for (const ProcRec* r : recs) {
            if (r->fix.empty() || (!pts.empty() && pts.back().ident == r->fix)) continue;
            MapState::RoutePoint rp; rp.ident = r->fix;
            const auto it = rw.find(r->fix);
            if (it != rw.end()) { rp.lat = it->second.first; rp.lon = it->second.second; }
            else if (!resolveFix(r->fix, la, lo, rp.lat, rp.lon)) continue;
            const double dLat = (rp.lat - aLa) * 60, dLon = (rp.lon - aLo) * 60 * std::cos(aLa * 3.14159265 / 180);
            if (dLat * dLat + dLon * dLon > 250.0 * 250.0) continue;
            pts.push_back(rp); la = rp.lat; lo = rp.lon;
        }
        return pts;
    };
    // split a SID or STAR into its runway part, its common part and the route's transition
    auto parts = [&](const std::vector<ProcRec>& recs, const std::string& proc, const std::string& rwyId,
                     std::vector<const ProcRec*>& rwy, std::vector<const ProcRec*>& common, std::vector<const ProcRec*>& enr) {
        std::string rwTrans, enTrans;
        for (const ProcRec& r : recs) {
            if (r.proc != proc) continue;
            if (r.trans.compare(0, 2, "RW") == 0) { if (forRunway(r.trans, rwyId) && (rwTrans.empty() || rwTrans == r.trans)) { rwTrans = r.trans; rwy.push_back(&r); } }
            else if (r.trans.empty() || r.trans == "ALL") common.push_back(&r);
            else if (onRoute.count(r.trans) && (enTrans.empty() || enTrans == r.trans)) { enTrans = r.trans; enr.push_back(&r); }
        }
    };
    // the departure
    if (!p.sid.empty() && !p.origin.empty()) {
        const std::string text = cifpText(p.origin);
        const auto rw = runwayEnds(text);
        const std::vector<ProcRec> recs = procRecs(text, "SID:");
        std::vector<const ProcRec*> rwy, common, enr;
        parts(recs, p.sid, p.originRunway, rwy, common, enr);
        double la = 0, lo = 0;
        if (!(rwy.empty() && common.empty() && enr.empty()) && anchor(p.origin, la, lo)) {
            std::vector<const ProcRec*> all = rwy; all.insert(all.end(), common.begin(), common.end()); all.insert(all.end(), enr.begin(), enr.end());
            MapState::Procedure pr; pr.kind = 0; pr.name = p.sid + (p.originRunway.empty() ? "" : " RW" + p.originRunway);
            double sLa = la, sLo = lo;
            const auto th = rw.find("RW" + p.originRunway);
            if (th != rw.end()) { MapState::RoutePoint rp; rp.ident = th->first; rp.lat = th->second.first; rp.lon = th->second.second; pr.pts.push_back(rp); sLa = rp.lat; sLo = rp.lon; }
            for (const MapState::RoutePoint& q : line(all, rw, sLa, sLo, la, lo)) if (pr.pts.empty() || pr.pts.back().ident != q.ident) pr.pts.push_back(q);
            if (pr.pts.size() >= 2) out.push_back(pr);
        }
    }
    // the arrival, then the approach
    if (!p.dest.empty()) {
        const std::string text = cifpText(p.dest);
        const auto rw = runwayEnds(text);
        double la = 0, lo = 0;
        const bool haveApt = anchor(p.dest, la, lo);
        double lLa = la, lLo = lo;
        if (!p.star.empty() && haveApt) {
            const std::vector<ProcRec> recs = procRecs(text, "STAR:");
            std::vector<const ProcRec*> rwy, common, enr;
            parts(recs, p.star, p.destRunway, rwy, common, enr);
            std::vector<const ProcRec*> all = enr; all.insert(all.end(), common.begin(), common.end()); all.insert(all.end(), rwy.begin(), rwy.end());
            double sLa = la, sLo = lo;
            if (!all.empty()) for (const FlightPlan::Leg& l : p.legs) if (l.ident == all.front()->fix && (l.lat != 0 || l.lon != 0)) { sLa = l.lat; sLo = l.lon; }
            MapState::Procedure pr; pr.kind = 1; pr.name = p.star; pr.pts = line(all, rw, sLa, sLo, la, lo);
            if (pr.pts.size() >= 2) { lLa = pr.pts.back().lat; lLo = pr.pts.back().lon; out.push_back(pr); }
        }
        if (!p.destRunway.empty() && haveApt) {
            const std::vector<ProcRec> recs = procRecs(text, "APPCH:");
            const std::string& rwyId = p.destRunway;
            const bool lettered = !(rwyId.back() >= '0' && rwyId.back() <= '9');
            auto fits = [&](const std::string& proc) {   // "I28C", "R28CY", "R28C-Z" for 28C; "I27" but not "I27L" for 27
                if (proc.size() < 1 + rwyId.size() || proc.compare(1, rwyId.size(), rwyId) != 0) return false;
                if (proc.size() == 1 + rwyId.size()) return true;
                const char n = proc[1 + rwyId.size()];
                if (n >= '0' && n <= '9') return false;
                return lettered || !(n == 'L' || n == 'R' || n == 'C');
            };
            std::string pick;
            for (const char pref : {'I', 'R', 'L', '\0'}) {
                for (const ProcRec& r : recs) if (fits(r.proc) && (pref == 0 || r.proc[0] == pref)) { pick = r.proc; break; }
                if (!pick.empty()) break;
            }
            if (!pick.empty()) {
                std::vector<const ProcRec*> fin;
                for (const ProcRec& r : recs) {
                    if (r.proc != pick || r.rtype == "A") continue;   // (its transitions left out: the final part only)
                    fin.push_back(&r);
                    if (r.fix.compare(0, 2, "RW") == 0) break;      // to the runway (the missed approach after it, left out)
                }
                const char k = pick[0];
                MapState::Procedure pr; pr.kind = 2;
                pr.name = std::string(k == 'I' ? "ILS " : k == 'R' ? "RNAV " : k == 'L' ? "LOC " : k == 'D' ? "VOR " : k == 'N' ? "NDB " : "APP ") + rwyId;
                pr.pts = line(fin, rw, lLa, lLo, la, lo);
                if (pr.pts.size() >= 2) out.push_back(pr);
            }
        }
    }
    return out;
}

} // namespace xpt
