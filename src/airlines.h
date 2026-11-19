#pragma once
#include <string>

namespace xpt {
// An airline's name from its ICAO designator ("DAL": "Delta Air Lines"); nullptr when it is not in the list.
const char* airlineName(const std::string& code);
}
