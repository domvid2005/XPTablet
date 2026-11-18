// The simulated aircraft and world: what X-Plane's datarefs would say. The control panel changes Sim; simStep() moves things along and
// writes the datarefs the plugin reads (position, time, weather, electrics, TCAS traffic, the aircraft's own).
#include "emu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>

extern "C" void XPluginReceiveMessage(int from, int msg, void* param);

namespace emu {
namespace {

constexpr double kPi = 3.14159265358979323846;
Sim g_sim;
std::vector<TrafficAc> g_traffic;
std::mt19937 g_rng(20261118);
int g_appliedProfile = -1;
bool g_appliedCockpit = false;
bool g_pluginUp = false;

float urand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

const std::vector<Profile> kProfiles = {
    {"Cessna 172 SP (Laminar)", "C172", "Laminar Research/Cessna 172 SP", "Cessna_172SP.acf", false, {}},
    {"Airbus A330-300 (Laminar)", "A333", "Laminar Research/Airbus A330-300", "A330.acf", false, {}},
    {"Boeing 737-800 (Laminar / Zibo)", "B738", "Laminar Research/Boeing 737-800", "b738.acf", false, {"laminar/B738/"}},
    {"ToLiss A321", "A321", "ToLiss A321", "a321.acf", true, {"AirbusFBW/", "toliss_airbus/"}},
    {"ToLiss A320 neo", "A20N", "ToLiss A320neo", "a320.acf", true, {"AirbusFBW/", "toliss_airbus/"}},
    {"ToLiss A346", "A346", "ToLiss A340-600", "a340-600.acf", true, {"AirbusFBW/", "toliss_airbus/"}},
    {"Bombardier CL650 (Hot Start)", "CL60", "CL650", "CL650.acf", false, {"CL650/"}},
};
const char* kTypes[] = {"A320", "A321", "A20N", "B738", "B38M", "B739", "E75L", "CRJ9", "A333", "B77W", "B789", "C172", "PC12", "DH8D"};
const char* kAirlines[] = {"UAL", "AAL", "DAL", "SWA", "JBU", "SKW", "ENY", "RPA", "FFT", "NKS", "ASA", "N"};

// the sun's height above the horizon (degrees), roughly (good to a degree or two)
float sunPitch(double lat, double lon, std::time_t utc) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &utc);
#else
    gmtime_r(&utc, &tm);
#endif
    const double day = tm.tm_yday + 1, hour = tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0;
    const double g = 2 * kPi / 365.0 * (day - 1 + (hour - 12) / 24);
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2 * g) + 0.000907 * std::sin(2 * g);
    const double eqt = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) - 0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
    const double ha = ((hour * 60 + eqt + 4 * lon) / 4 - 180) * kPi / 180;
    const double la = lat * kPi / 180;
    const double cz = std::sin(la) * std::sin(decl) + std::cos(la) * std::cos(decl) * std::cos(ha);
    return (float)(90 - std::acos(std::max(-1.0, std::min(1.0, cz))) * 180 / kPi);
}

std::string pad8(const std::string& s) { std::string o = s.substr(0, 8); o.resize(8, '\0'); return o; }

TrafficAc makeTraffic(bool ground) {
    TrafficAc t;
    const float brg = urand(0, 360), d = urand(0.5f, g_sim.trafficRangeNm);
    t.lat = g_sim.lat + d / 60.0 * std::cos(brg * kPi / 180);
    t.lon = g_sim.lon + d / 60.0 * std::sin(brg * kPi / 180) / std::cos(g_sim.lat * kPi / 180);
    t.type = kTypes[std::uniform_int_distribution<int>(0, (int)(sizeof kTypes / sizeof kTypes[0]) - 1)(g_rng)];
    const char* al = kAirlines[std::uniform_int_distribution<int>(0, (int)(sizeof kAirlines / sizeof kAirlines[0]) - 1)(g_rng)];
    char cs[16];
    if (al[0] == 'N' && al[1] == 0) std::snprintf(cs, sizeof cs, "N%d%c%c", std::uniform_int_distribution<int>(100, 999)(g_rng), 'A' + (char)urand(0, 25), 'A' + (char)urand(0, 25));
    else std::snprintf(cs, sizeof cs, "%s%d", al, std::uniform_int_distribution<int>(10, 4999)(g_rng));
    t.callsign = cs;
    t.onGround = ground;
    t.hdg = urand(0, 360);
    if (ground) { t.altFt = g_sim.altFt - g_sim.aglFt; t.gsKt = urand(0, 18); t.vsFpm = 0; t.lat = g_sim.lat + urand(-0.01f, 0.01f); t.lon = g_sim.lon + urand(-0.012f, 0.012f); }
    else { t.altFt = std::max(1500.0f, g_sim.altFt + urand(-6000, 9000)); t.gsKt = urand(140, 460); t.vsFpm = urand(0, 1) < 0.5f ? 0 : urand(-1800, 1800); }
    return t;
}

void writeAircraftFolder() {
    namespace fs = std::filesystem;
    const Profile& p = kProfiles[std::max(0, std::min((int)kProfiles.size() - 1, g_sim.profile))];
    const std::string dir = paths().emuData + "/Aircraft/" + p.folder;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::exists(dir + "/" + p.acfFile)) { FILE* f = std::fopen((dir + "/" + p.acfFile).c_str(), "wb"); if (f) { std::fputs("I\n1200 version\nACF\n", f); std::fclose(f); } }
    const std::string json = dir + "/AviTab.json";
    if (g_sim.cockpitTablet) {
        FILE* f = std::fopen(json.c_str(), "wb");
        if (f) { std::fputs("{ \"panel\": { \"enabled\": true, \"left\": 1024, \"bottom\": 384, \"width\": 1024, \"height\": 640 } }\n", f); std::fclose(f); }
    } else {
        fs::remove(json, ec);
    }
    // the aircraft's systems plugin (ToLiss is known by it), and WXPlane / AviTab / OpenSAM when asked for
    setFakePlugin("XP11.ToLiss.systems", "ToLiss systems", dir + "/plugins/ToLiss/64/lin.xpl", p.toliss);
    setFakePlugin("com.wxplane.xp12.noaa", "WXPlane", paths().xplaneRoot + "/Resources/plugins/WXPlane/64/lin.xpl", g_sim.wxPlane);
    setFakePlugin("org.solhost.folko.avitab", "AviTab", paths().xplaneRoot + "/Resources/plugins/AviTab/64/lin.xpl", g_sim.aviTab);
    std::vector<std::string> prefixes = p.prefixes;
    if (g_sim.openSam) prefixes.push_back("opensam/");
    setExtraPrefixes(prefixes);
    if (p.toliss) {   // the ToLiss datarefs the Ground page uses, at believable values
        if (!dataref("AirbusFBW/PaxDoorModeArray")->vi.size()) setInts("AirbusFBW/PaxDoorModeArray", std::vector<int>(8, 0));
        if (!dataref("AirbusFBW/CargoDoorModeArray")->vi.size()) setInts("AirbusFBW/CargoDoorModeArray", std::vector<int>(4, 0));
        if (!dataref("AirbusFBW/SlideArmedArray")->vi.size()) setInts("AirbusFBW/SlideArmedArray", std::vector<int>(8, 0));
        if (!dataref("AirbusFBW/cabinLightLevel_perc")->touched) setNum("AirbusFBW/cabinLightLevel_perc", 0.7);
    }
}

} // namespace

const std::vector<Profile>& profiles() { return kProfiles; }
Sim& sim() { return g_sim; }
std::vector<TrafficAc>& traffic() { return g_traffic; }

void simApplyProfile() {
    writeAircraftFolder();
    g_appliedProfile = g_sim.profile;
    g_appliedCockpit = g_sim.cockpitTablet;
    if (g_pluginUp) {
        log("[emulator] aircraft: %s%s", kProfiles[g_sim.profile].name.c_str(), g_sim.cockpitTablet ? ", with a cockpit tablet (AviTab.json)" : "");
        XPluginReceiveMessage(0, 102 /* XPLM_MSG_PLANE_LOADED */, (void*)0);
    }
    g_pluginUp = true;
}
void simResetTraffic() {
    g_traffic.clear();
    for (int i = 0; i < g_sim.trafficCount; ++i) g_traffic.push_back(makeTraffic(i % 4 == 3));
}
void simAddIntruder() {
    TrafficAc t;
    const double h = g_sim.hdg * kPi / 180;
    t.lat = g_sim.lat + 0.6 / 60.0 * std::cos(h);
    t.lon = g_sim.lon + 0.6 / 60.0 * std::sin(h) / std::cos(g_sim.lat * kPi / 180);
    t.altFt = g_sim.altFt + 200; t.hdg = std::fmod(g_sim.hdg + 180, 360.0f); t.gsKt = 250; t.vsFpm = -200; t.onGround = false;
    t.callsign = "N555RA"; t.type = "B738";
    g_traffic.push_back(t);
    if (g_sim.onGround || g_sim.aglFt < 1000) log("[emulator] an intruder is ahead; TCAS gives no advisories on the ground or below 1000 ft above it, so it shows red only when very close");
}

void simStep(double dt) {
    Sim& s = g_sim;
    if (s.profile != g_appliedProfile || s.cockpitTablet != g_appliedCockpit) simApplyProfile();
    const Profile& p = kProfiles[std::max(0, std::min((int)kProfiles.size() - 1, s.profile))];
    // the aircraft moves when flying (or taxiing)
    if (s.onGround) { s.vsFpm = 0; s.aglFt = 0; }
    if (s.flying && s.gsKt > 0) {
        const double d = s.gsKt * dt / 3600.0;   // nm
        s.lat += d / 60.0 * std::cos(s.hdg * kPi / 180);
        s.lon += d / 60.0 * std::sin(s.hdg * kPi / 180) / std::cos(s.lat * kPi / 180);
        if (!s.onGround) { const float dz = s.vsFpm * (float)dt / 60.0f; s.altFt += dz; s.aglFt = std::max(0.0f, s.aglFt + dz); }
    }
    // time
    const std::time_t now = std::time(nullptr);
    std::time_t utc = now;
    if (s.zuluHour >= 0) { utc = now - now % 86400 + (std::time_t)(s.zuluHour * 3600); }
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &utc);
#else
    gmtime_r(&utc, &tm);
#endif
    const double zulu = tm.tm_hour * 3600.0 + tm.tm_min * 60.0 + tm.tm_sec + std::fmod(simTime(), 1.0);
    const double local = std::fmod(zulu + std::round(s.lon / 15.0) * 3600.0 + 86400.0, 86400.0);   // X-Plane's local time: by the longitude's zone
    setNum("sim/time/zulu_time_sec", zulu);
    setNum("sim/time/local_time_sec", local);
    setNum("sim/time/local_date_days", tm.tm_yday);
    if (s.autoSun) s.sunPitch = sunPitch(s.lat, s.lon, utc);
    setNum("sim/graphics/scenery/sun_pitch_degrees", s.sunPitch);
    // the aircraft
    setBytes("sim/aircraft/view/acf_ICAO", pad8(s.icaoOverride.empty() ? p.icao : s.icaoOverride) + std::string(32, '\0'));
    setNum("sim/flightmodel/position/latitude", s.lat);
    setNum("sim/flightmodel/position/longitude", s.lon);
    setNum("sim/flightmodel/position/elevation", s.altFt / 3.28084);
    setNum("sim/flightmodel/position/y_agl", s.aglFt / 3.28084);
    setNum("sim/flightmodel/position/true_psi", s.hdg);
    setNum("sim/flightmodel/position/groundspeed", s.gsKt / 1.943844);
    setNum("sim/flightmodel/position/vh_ind_fpm", s.vsFpm);
    setNum("sim/flightmodel/failures/onground_any", s.onGround ? 1 : 0);
    setInts("sim/flightmodel/engine/ENGN_running", std::vector<int>(8, s.engines ? 1 : 0));
    setNum("sim/cockpit2/switches/beacon_on", s.beacon ? 1 : 0);
    setNum("sim/cockpit2/controls/parking_brake_ratio", s.parkBrake ? 1 : 0);
    setNum("sim/flightmodel2/gear/is_chocked", s.chocks ? 1 : 0);
    setFloats("sim/cockpit2/electrical/bus_volts", std::vector<float>(6, s.battery ? 28.0f : 0.0f));
    setNum("sim/graphics/VR/enabled", 0);
    // the weather at the aircraft
    setNum("sim/weather/aircraft/wind_now_direction_degt", s.windDir);
    setNum("sim/weather/aircraft/wind_now_speed_msc", s.windKt / 1.943844);
    setFloats("sim/weather/aircraft/wind_direction_degt", std::vector<float>(13, s.windDir));
    setFloats("sim/weather/aircraft/wind_speed_kts", std::vector<float>(13, s.windKt));
    setFloats("sim/weather/aircraft/wind_altitude_msl_m", {0, 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000});
    setNum("sim/weather/aircraft/visibility_reported_sm", s.visSm);
    setNum("sim/weather/aircraft/temperature_ambient_deg_c", s.tempC);
    setNum("sim/weather/aircraft/dewpoint_deg_c", s.dewC);
    setNum("sim/weather/aircraft/qnh_pas", s.qnhHpa * 100.0);
    setNum("sim/weather/aircraft/precipitation_on_aircraft_ratio", s.rain);
    std::vector<float> base(3, 0), tops(3, 0), cover(3, 0);
    for (int i = 0; i < std::min(3, s.clouds); ++i) { base[i] = (s.cloudBaseFt + i * 6000) / 3.28084f; tops[i] = (s.cloudTopFt + i * 6000) / 3.28084f; cover[i] = s.cloudCover; }
    setFloats("sim/weather/aircraft/cloud_base_msl_m", base);
    setFloats("sim/weather/aircraft/cloud_tops_msl_m", tops);
    setFloats("sim/weather/aircraft/cloud_coverage_percent", cover);
    // traffic: moved along, the ones far away replaced
    if ((int)g_traffic.size() < s.trafficCount) while ((int)g_traffic.size() < s.trafficCount) g_traffic.push_back(makeTraffic(g_traffic.size() % 4 == 3));
    while ((int)g_traffic.size() > s.trafficCount && !g_traffic.empty() && g_traffic.back().callsign != "N555RA") g_traffic.pop_back();
    for (TrafficAc& t : g_traffic) {
        const double d = t.gsKt * dt / 3600.0;
        t.lat += d / 60.0 * std::cos(t.hdg * kPi / 180);
        t.lon += d / 60.0 * std::sin(t.hdg * kPi / 180) / std::cos(t.lat * kPi / 180);
        if (!t.onGround) t.altFt = std::max(500.0f, t.altFt + t.vsFpm * (float)dt / 60.0f);
        const double dn = std::hypot((t.lat - s.lat) * 60.0, (t.lon - s.lon) * 60.0 * std::cos(s.lat * kPi / 180));
        if (dn > s.trafficRangeNm * 1.6 && t.callsign != "N555RA") t = makeTraffic(t.onGround);
    }
    std::vector<float> la(64, 0), lo(64, 0), el(64, 0), ps(64, 0), vs(64, 0), v(64, 0);
    std::vector<int> wow(64, 0);
    std::string ids(512, '\0'), types(512, '\0');
    la[0] = (float)s.lat; lo[0] = (float)s.lon; el[0] = s.altFt / 3.28084f; ps[0] = s.hdg;
    const int n = std::min(63, (int)g_traffic.size());
    for (int i = 0; i < n; ++i) {
        const TrafficAc& t = g_traffic[i];
        la[i + 1] = (float)t.lat; lo[i + 1] = (float)t.lon; el[i + 1] = t.altFt / 3.28084f; ps[i + 1] = t.hdg; vs[i + 1] = t.vsFpm; v[i + 1] = t.gsKt / 1.943844f; wow[i + 1] = t.onGround;
        ids.replace((i + 1) * 8, 8, pad8(t.callsign));
        types.replace((i + 1) * 8, 8, pad8(t.type));
    }
    setNum("sim/cockpit2/tcas/indicators/tcas_num_acf", n + 1);
    setFloats("sim/cockpit2/tcas/targets/position/lat", la);
    setFloats("sim/cockpit2/tcas/targets/position/lon", lo);
    setFloats("sim/cockpit2/tcas/targets/position/ele", el);
    setFloats("sim/cockpit2/tcas/targets/position/psi", ps);
    setFloats("sim/cockpit2/tcas/targets/position/vertical_speed", vs);
    setFloats("sim/cockpit2/tcas/targets/position/V_msc", v);
    setInts("sim/cockpit2/tcas/targets/position/weight_on_wheels", wow);
    setBytes("sim/cockpit2/tcas/targets/flight_id", ids);
    setBytes("sim/cockpit2/tcas/targets/icao_type", types);
}

} // namespace emu
