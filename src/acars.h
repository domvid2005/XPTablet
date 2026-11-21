// Hoppie's ACARS (www.hoppie.nl/acars): the request's address, and its answers read into messages (shared by the plugin and the
// browser emulator). Telex and CPDLC; CPDLC packets are "/data2/MIN/MRN/RA/TEXT" (RA: WU wilco/unable, AN affirm/negative, R roger,
// Y a reply, N or NE none).
#pragma once
#include <string>
#include <vector>

#include "tabletui.h"

namespace xpt {

std::string acarsUrl(const std::string& logon, const std::string& from, const std::string& to, const std::string& type, const std::string& packet);
// "ok {KSBN telex {CLEARED TO KORD}} {KSBN cpdlc {/data2/3//WU/CLIMB TO @FL240@}}" -> messages; false on "error {...}" (err says why)
bool acarsParse(const std::string& body, std::vector<AcarsMsg>& out, std::string& err);
// a CPDLC packet's parts ("/data2/3/1/NE/LOGON ACCEPTED": 3, 1, "NE", "LOGON ACCEPTED"); the text with its @ marks taken out
bool cpdlcParse(const std::string& packet, int& min, int& mrn, std::string& ra, std::string& text);
std::string cpdlcPacket(int min, int mrn, const std::string& ra, const std::string& text);

} // namespace xpt
