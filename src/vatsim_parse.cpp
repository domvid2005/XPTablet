#include "vatsim_parse.h"

namespace xpt {
namespace vatsim {
namespace {
std::string str(const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asString() : std::string(); }
double num(const wx::Json* o, const char* k) { const wx::Json* v = o ? o->get(k) : nullptr; return v ? v->asNum(0) : 0.0; }
} // namespace

void readPilots(const wx::Json& feed, std::unordered_map<std::string, VatsimPilot>& out) {
    const wx::Json* list = feed.get("pilots");
    if (list && list->type == wx::Json::Arr)
        for (const wx::Json& p : list->arr) {
            VatsimPilot v;
            v.found = true;
            v.callsign = str(&p, "callsign");
            v.name = str(&p, "name");
            v.cid = str(&p, "cid");
            v.server = str(&p, "server");
            v.squawk = str(&p, "transponder");
            v.altitudeFt = (int)num(&p, "altitude");
            v.groundspeedKt = (int)num(&p, "groundspeed");
            v.heading = (int)num(&p, "heading");
            v.lat = num(&p, "latitude"); v.lon = num(&p, "longitude");
            v.qnhMb = (int)num(&p, "qnh_mb");
            v.logon = str(&p, "logon_time");
            if (v.logon.size() >= 16) v.logon = v.logon.substr(11, 5) + "Z";   // "2026-11-18T05:12:33..." -> "05:12Z"
            const int rating = (int)num(&p, "pilot_rating");
            static const char* ratings[] = {"no rating", "PPL", "IR", "CMEL", "ATPL", "flight instructor", "flight examiner"};
            v.rating = rating == 0 ? "no rating" : rating == 1 ? "PPL" : rating == 3 ? "IR" : rating == 7 ? "CMEL" : rating == 15 ? "ATPL" : rating == 31 ? "flight instructor" : rating == 63 ? "flight examiner" : std::to_string(rating);
            (void)ratings;
            const wx::Json* fp = p.get("flight_plan");
            if (fp && fp->type == wx::Json::Obj) {
                v.hasPlan = true;
                v.rules = str(fp, "flight_rules") == "V" ? "VFR" : "IFR";
                v.aircraft = str(fp, "aircraft_short"); if (v.aircraft.empty()) v.aircraft = str(fp, "aircraft");
                v.aircraftFull = str(fp, "aircraft");
                v.departure = str(fp, "departure"); v.arrival = str(fp, "arrival"); v.alternate = str(fp, "alternate");
                v.cruiseAlt = str(fp, "altitude"); v.cruiseTas = str(fp, "cruise_tas");
                v.depTime = str(fp, "deptime"); v.enroute = str(fp, "enroute_time"); v.fuel = str(fp, "fuel_time");
                v.route = str(fp, "route"); v.remarks = str(fp, "remarks");
                v.assignedSquawk = str(fp, "assigned_transponder");
            }
            out[v.callsign] = std::move(v);
        }
}

bool readPilots(const std::string& body, std::unordered_map<std::string, VatsimPilot>& out) {
    wx::Json j;
    if (!wx::parseJson(body, j)) return false;
    readPilots(j, out);
    return true;
}

} // namespace vatsim
} // namespace xpt
