#include "airspace_parse.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace xpt {
namespace airspace {
namespace {
// a property's value in [b, e): a string or a number, as text ("" when it is not there)
std::string field(const std::string& t, size_t b, size_t e, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = t.find(k, b);
    if (p == std::string::npos || p >= e) return "";
    p = t.find(':', p + k.size());
    if (p == std::string::npos || p >= e) return "";
    ++p;
    while (p < e && std::isspace((unsigned char)t[p])) ++p;
    if (p < e && t[p] == '"') {
        const size_t q = t.find('"', p + 1);
        return q == std::string::npos ? std::string() : t.substr(p + 1, q - p - 1);
    }
    size_t q = p;
    while (q < e && t[q] != ',' && t[q] != '}' && !std::isspace((unsigned char)t[q])) ++q;
    return t.substr(p, q - p);
}
} // namespace

void parseBoundaries(const std::string& t, std::vector<AirspaceArea>& out) {
    size_t pos = 0;
    for (;;) {
        size_t f = t.find("\"Feature\"", pos);
        if (f == std::string::npos) break;
        size_t next = t.find("\"Feature\"", f + 9);
        if (next == std::string::npos) next = t.size();
        pos = next;
        AirspaceArea a;
        a.id = field(t, f, next, "id");
        if (a.id.empty()) continue;
        a.oceanic = field(t, f, next, "oceanic") == "1";
        a.labelLat = std::atof(field(t, f, next, "label_lat").c_str());
        a.labelLon = std::atof(field(t, f, next, "label_lon").c_str());
        size_t c = t.find("\"coordinates\"", f);
        if (c == std::string::npos || c >= next) continue;
        c = t.find('[', c);
        if (c == std::string::npos || c >= next) continue;
        // the nested arrays: a '[' that starts with a number is a point; the array holding points is a ring
        int depth = 0, ringDepth = -1;
        std::vector<std::pair<float, float>> ring;
        for (size_t i = c; i < next; ++i) {
            const char ch = t[i];
            if (ch == '[') {
                ++depth;
                size_t j = i + 1;
                while (j < next && std::isspace((unsigned char)t[j])) ++j;
                if (j < next && (std::isdigit((unsigned char)t[j]) || t[j] == '-')) {   // [lon, lat]
                    char* e1 = nullptr;
                    const double lon = std::strtod(t.c_str() + j, &e1);
                    const char* p2 = std::strchr(e1, ',');
                    if (!p2) break;
                    char* e2 = nullptr;
                    const double lat = std::strtod(p2 + 1, &e2);
                    ring.push_back({(float)lat, (float)lon});
                    ringDepth = depth - 1;
                    const char* close = std::strchr(e2, ']');
                    if (!close) break;
                    i = (size_t)(close - t.c_str());
                    --depth;
                }
            } else if (ch == ']') {
                if (depth == ringDepth && !ring.empty()) { a.rings.push_back(std::move(ring)); ring.clear(); }
                --depth;
                if (depth <= 0) break;
            }
        }
        if (a.rings.empty()) continue;
        a.la0 = a.lo0 = 1e9; a.la1 = a.lo1 = -1e9;
        for (const auto& r : a.rings) for (const auto& p : r) { a.la0 = std::min(a.la0, (double)p.first); a.la1 = std::max(a.la1, (double)p.first); a.lo0 = std::min(a.lo0, (double)p.second); a.lo1 = std::max(a.lo1, (double)p.second); }
        if (a.labelLat == 0 && a.labelLon == 0) { a.labelLat = (a.la0 + a.la1) * 0.5; a.labelLon = (a.lo0 + a.lo1) * 0.5; }
        out.push_back(std::move(a));
    }
}

void parseFirs(const std::string& dat, std::vector<AirspaceSet::Fir>& out) {
    size_t p = 0;
    bool inFirs = false;
    while (p < dat.size()) {
        size_t e = dat.find('\n', p);
        if (e == std::string::npos) e = dat.size();
        std::string line = dat.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { inFirs = line.compare(0, 6, "[FIRs]") == 0; continue; }
        if (!inFirs) continue;
        std::vector<std::string> parts;
        size_t q = 0;
        while (q <= line.size()) { size_t bar = line.find('|', q); if (bar == std::string::npos) bar = line.size(); parts.push_back(line.substr(q, bar - q)); q = bar + 1; }
        if (parts.size() < 4) continue;
        AirspaceSet::Fir fir;
        fir.icao = parts[0]; fir.name = parts[1]; fir.prefix = parts[2]; fir.boundary = parts[3].empty() ? parts[0] : parts[3];
        out.push_back(std::move(fir));
    }
}

const AirspaceSet::Fir* firFor(const AirspaceSet& set, const std::string& cs) {
    // "LON_S_CTR": LON_S first (a sector with its own entry), then LON; "EGTT_CTR": its ICAO code
    std::vector<std::string> parts;
    size_t q = 0;
    while (q <= cs.size()) { size_t u = cs.find('_', q); if (u == std::string::npos) u = cs.size(); parts.push_back(cs.substr(q, u - q)); q = u + 1; }
    if (parts.size() < 2) return nullptr;
    const std::string two = parts.size() >= 3 ? parts[0] + "_" + parts[1] : std::string();
    if (!two.empty()) for (const auto& f : set.firs) if (f.prefix == two || f.icao == two) return &f;
    for (const auto& f : set.firs) if (!f.prefix.empty() && f.prefix == parts[0]) return &f;
    for (const auto& f : set.firs) if (f.icao == parts[0]) return &f;
    return nullptr;
}

} // namespace airspace
} // namespace xpt
