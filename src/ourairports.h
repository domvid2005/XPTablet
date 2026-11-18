// OurAirports (ourairports.com, public domain): real-world names, places and radio frequencies for the Airports page, CTAF
// among them. Two CSV files, downloaded on a thread of their own the first time the page opens, kept a month in
// Output/XPTablet/ourairports. No X-Plane calls here.
#pragma once

#include <string>
#include <vector>

namespace xpt {
namespace ourairports {

struct Freq { std::string type, desc; double mhz = 0; };   // type as OurAirports gives it: CTAF, UNIC, TWR, GND, ATIS, APP ...
struct Info { bool found = false; std::string name, city, country, region, iata, type, wiki; std::vector<Freq> freqs; };

void start(const std::string& cacheDir);
void stop();
bool ready();                 // the files are read
std::string status();         // what it is doing, or why it has nothing ("" when ready)
Info lookup(const std::string& code);   // by ICAO, GPS or local code

} // namespace ourairports
} // namespace xpt
