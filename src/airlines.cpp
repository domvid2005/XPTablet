// Airline ICAO designators (the three letters in apt.dat's stand rows, row 1301, and in call signs) and their names.
// A hand-kept list of the airlines most often seen in X-Plane scenery and on VATSIM; a code not here is shown as it is.
#include "airlines.h"

#include <algorithm>
#include <cstring>

namespace xpt {
namespace {
struct Airline { const char* code; const char* name; };
// sorted by code (checked once, below), so a lookup is a binary search
const Airline kAirlines[] = {
    {"AAL", "American Airlines"}, {"AAR", "Asiana Airlines"}, {"AAY", "Allegiant Air"}, {"ABW", "AirBridgeCargo"}, {"ABX", "ABX Air"},
    {"ACA", "Air Canada"}, {"ADR", "Adria Airways"}, {"AEA", "Air Europa"}, {"AEE", "Aegean Airlines"}, {"AFL", "Aeroflot"},
    {"AFR", "Air France"}, {"AIC", "Air India"}, {"AIJ", "Interjet"}, {"AJX", "Air Japan"}, {"AMX", "Aeromexico"},
    {"ANA", "All Nippon Airways"}, {"ANZ", "Air New Zealand"}, {"ARG", "Aerolineas Argentinas"}, {"ASA", "Alaska Airlines"},
    {"ASH", "Mesa Airlines"}, {"ATN", "Air Transport International"}, {"AUA", "Austrian Airlines"}, {"AVA", "Avianca"},
    {"AWE", "US Airways"}, {"AWI", "Air Wisconsin"}, {"AXM", "AirAsia"}, {"AZA", "ITA Airways (Alitalia)"}, {"AZU", "Azul"},
    {"BAV", "Bamboo Airways"}, {"BAW", "British Airways"}, {"BCS", "European Air Transport (DHL)"}, {"BEL", "Brussels Airlines"},
    {"BOX", "AeroLogic"}, {"BTI", "airBaltic"}, {"BWA", "Caribbean Airlines"}, {"CAL", "China Airlines"}, {"CAO", "Air China Cargo"},
    {"CAY", "Cayman Airways"}, {"CCA", "Air China"}, {"CES", "China Eastern"}, {"CFG", "Condor"}, {"CHH", "Hainan Airlines"},
    {"CJT", "Cargojet"}, {"CKS", "Kalitta Air"}, {"CLX", "Cargolux"}, {"CMP", "Copa Airlines"}, {"CPA", "Cathay Pacific"},
    {"CPZ", "Compass Airlines"}, {"CSA", "Czech Airlines"}, {"CSN", "China Southern"}, {"CTN", "Croatia Airlines"},
    {"CXA", "Xiamen Airlines"}, {"DAL", "Delta Air Lines"}, {"DHK", "DHL Air (UK)"}, {"DLH", "Lufthansa"}, {"DWI", "Arajet"},
    {"EDV", "Endeavor Air"}, {"EIN", "Aer Lingus"}, {"EJA", "NetJets"}, {"ELY", "El Al"}, {"ENY", "Envoy Air"}, {"ETD", "Etihad Airways"},
    {"ETH", "Ethiopian Airlines"}, {"EVA", "EVA Air"}, {"EWG", "Eurowings"}, {"EXS", "Jet2"}, {"EZY", "easyJet"}, {"FDX", "FedEx Express"},
    {"FFT", "Frontier Airlines"}, {"FIN", "Finnair"}, {"FJI", "Fiji Airways"}, {"FLE", "Flair Airlines"},
    {"FRH", "Freedom Airline Express"}, {"GEC", "Lufthansa Cargo"}, {"GFA", "Gulf Air"}, {"GJS", "GoJet Airlines"}, {"GLO", "GOL"},
    {"GTI", "Atlas Air"}, {"HAL", "Hawaiian Airlines"}, {"HVN", "Vietnam Airlines"}, {"IBE", "Iberia"}, {"ICE", "Icelandair"},
    {"IGO", "IndiGo"}, {"JAL", "Japan Airlines"}, {"JBU", "JetBlue"}, {"JIA", "PSA Airlines"}, {"JZA", "Jazz (Air Canada Express)"},
    {"KAL", "Korean Air"}, {"KLM", "KLM"}, {"KQA", "Kenya Airways"}, {"LAN", "LATAM Chile"}, {"LNE", "LATAM Ecuador"},
    {"LOF", "Trans States Airlines"}, {"LOT", "LOT Polish Airlines"}, {"LPE", "LATAM Peru"}, {"LXJ", "Flexjet"},
    {"MAS", "Malaysia Airlines"}, {"MEA", "Middle East Airlines"}, {"MSR", "EgyptAir"}, {"MXY", "Breeze Airways"}, {"NAX", "Norwegian"},
    {"NCA", "Nippon Cargo Airlines"}, {"NKS", "Spirit Airlines"}, {"NOZ", "Norwegian"}, {"NWA", "Northwest Airlines"},
    {"OAL", "Olympic Air"}, {"OMA", "Oman Air"}, {"PAC", "Polar Air Cargo"}, {"PAL", "Philippine Airlines"}, {"PDT", "Piedmont Airlines"},
    {"PGT", "Pegasus Airlines"}, {"PIA", "Pakistan International"}, {"POE", "Porter Airlines"}, {"QFA", "Qantas"}, {"QTR", "Qatar Airways"},
    {"QXE", "Horizon Air"}, {"RAM", "Royal Air Maroc"}, {"RCH", "US Air Force Air Mobility Command"}, {"RJA", "Royal Jordanian"},
    {"RPA", "Republic Airways"}, {"RYR", "Ryanair"}, {"RZO", "SATA Azores Airlines"}, {"SAA", "South African Airways"},
    {"SAS", "SAS Scandinavian"}, {"SCX", "Sun Country Airlines"}, {"SIA", "Singapore Airlines"}, {"SKW", "SkyWest Airlines"},
    {"SLK", "SilkAir"}, {"SVA", "Saudia"}, {"SWA", "Southwest Airlines"}, {"SWG", "Sunwing Airlines"}, {"SWR", "Swiss"},
    {"SXS", "SunExpress"}, {"TAI", "TACA (Avianca El Salvador)"}, {"TAM", "LATAM Brasil"}, {"TAP", "TAP Air Portugal"}, {"TAR", "Tunisair"},
    {"TCX", "Thomas Cook Airlines"}, {"TGW", "Scoot"}, {"THA", "Thai Airways"}, {"THY", "Turkish Airlines"}, {"TOM", "TUI Airways"},
    {"TRA", "Transavia"}, {"TSC", "Air Transat"}, {"TVF", "Transavia France"}, {"UAE", "Emirates"}, {"UAL", "United Airlines"},
    {"UPS", "UPS Airlines"}, {"UZB", "Uzbekistan Airways"}, {"VIR", "Virgin Atlantic"}, {"VIV", "Viva Aerobus"}, {"VJT", "VistaJet"},
    {"VKG", "Sunclass Airlines"}, {"VLG", "Vueling"}, {"VOI", "Volaris"}, {"VOZ", "Virgin Australia"}, {"VRD", "Virgin America"},
    {"VTI", "Vistara"}, {"WEN", "WestJet Encore"}, {"WGN", "Western Global Airlines"}, {"WJA", "WestJet"}, {"WZZ", "Wizz Air"},
};

bool sortedOnce() {
    static const bool ok = std::is_sorted(std::begin(kAirlines), std::end(kAirlines), [](const Airline& a, const Airline& b) { return std::strcmp(a.code, b.code) < 0; });
    return ok;
}
} // namespace

const char* airlineName(const std::string& code) {
    if (code.size() != 3) return nullptr;
    char c[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) c[i] = (char)((code[i] >= 'a' && code[i] <= 'z') ? code[i] - 32 : code[i]);
    if (sortedOnce()) {
        const Airline* e = std::end(kAirlines);
        const Airline* it = std::lower_bound(std::begin(kAirlines), e, c, [](const Airline& a, const char* k) { return std::strcmp(a.code, k) < 0; });
        return (it != e && std::strcmp(it->code, c) == 0) ? it->name : nullptr;
    }
    for (const Airline& a : kAirlines) if (std::strcmp(a.code, c) == 0) return a.name;
    return nullptr;
}

} // namespace xpt
