// The Weather page's METAR search: asks NOAA's Aviation Weather Center for one airport's latest report, on a worker thread.
#pragma once

#include <string>

namespace xpt {

struct MetarAnswer {
    std::string icao;    // what was asked for
    std::string raw;     // the report, empty when there is none
    std::string error;   // why not, when raw is empty
};

void metarStart();                            // plugin enable: starts the worker
void metarStop();                             // plugin disable: stops it (waits for a download in progress to give up)
void metarRequest(const std::string& icao);   // replaces a request that has not started yet
bool metarBusy();
bool metarPoll(MetarAnswer& out);             // true once per finished request

} // namespace xpt
