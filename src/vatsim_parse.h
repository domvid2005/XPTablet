// VATSIM's data feed (v3 JSON) read into pilots by call sign (shared by the plugin and the browser emulator)
#pragma once

#include <string>
#include <unordered_map>

#include "json_mini.h"
#include "tabletui.h"

namespace xpt {
namespace vatsim {
void readPilots(const wx::Json& feed, std::unordered_map<std::string, VatsimPilot>& out);
bool readPilots(const std::string& body, std::unordered_map<std::string, VatsimPilot>& out);   // false: not readable
} // namespace vatsim
} // namespace xpt
