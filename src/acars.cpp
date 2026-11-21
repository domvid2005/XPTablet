#include "acars.h"

#include <cstdio>
#include <cstdlib>

namespace xpt {

static std::string enc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

std::string acarsUrl(const std::string& logon, const std::string& from, const std::string& to, const std::string& type, const std::string& packet) {
    std::string u = "https://www.hoppie.nl/acars/system/connect.html?logon=" + enc(logon) + "&from=" + enc(from) + "&to=" + enc(to) + "&type=" + enc(type);
    if (!packet.empty()) u += "&packet=" + enc(packet);
    return u;
}

bool cpdlcParse(const std::string& packet, int& min, int& mrn, std::string& ra, std::string& text) {
    if (packet.compare(0, 7, "/data2/") != 0) return false;
    std::vector<std::string> f; size_t a = 7;
    for (int i = 0; i < 3; ++i) { const size_t s = packet.find('/', a); if (s == std::string::npos) return false; f.push_back(packet.substr(a, s - a)); a = s + 1; }
    min = std::atoi(f[0].c_str()); mrn = std::atoi(f[1].c_str()); ra = f[2];
    text.clear();
    for (size_t i = a; i < packet.size(); ++i) if (packet[i] != '@') text += packet[i];
    return true;
}

std::string cpdlcPacket(int min, int mrn, const std::string& ra, const std::string& text) {
    return "/data2/" + std::to_string(min) + "/" + (mrn > 0 ? std::to_string(mrn) : std::string()) + "/" + ra + "/" + text;
}

bool acarsParse(const std::string& body, std::vector<AcarsMsg>& out, std::string& err) {
    size_t i = body.find_first_not_of(" \r\n\t");
    if (i == std::string::npos) { err = "no answer"; return false; }
    if (body.compare(i, 2, "ok") != 0) {
        const size_t a = body.find('{'), b = body.rfind('}');
        err = a != std::string::npos && b != std::string::npos && b > a ? body.substr(a + 1, b - a - 1) : body.substr(i, 80);
        return false;
    }
    // each message: {FROM TYPE {PACKET}}, the packet itself may hold braces only in pairs
    i += 2;
    while ((i = body.find('{', i)) != std::string::npos) {
        int depth = 0; size_t e = i;
        for (; e < body.size(); ++e) { if (body[e] == '{') ++depth; else if (body[e] == '}' && --depth == 0) break; }
        if (e >= body.size()) break;
        const std::string m = body.substr(i + 1, e - i - 1);
        i = e + 1;
        const size_t s1 = m.find(' '), s2 = s1 == std::string::npos ? s1 : m.find(' ', s1 + 1);
        const size_t pa = m.find('{'), pb = m.rfind('}');
        if (s1 == std::string::npos || s2 == std::string::npos || pa == std::string::npos || pb == std::string::npos || pb < pa) continue;
        AcarsMsg msg; msg.up = true; msg.station = m.substr(0, s1); msg.type = m.substr(s1 + 1, s2 - s1 - 1); msg.text = m.substr(pa + 1, pb - pa - 1);
        if (msg.type == "cpdlc") { std::string t; if (cpdlcParse(msg.text, msg.min, msg.mrn, msg.ra, t)) msg.text = t; }
        out.push_back(msg);
    }
    return true;
}

} // namespace xpt
