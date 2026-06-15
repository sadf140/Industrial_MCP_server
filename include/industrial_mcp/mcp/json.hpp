#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace industrial_mcp {

using Json = nlohmann::json;

std::string now_utc_iso8601();

} // namespace industrial_mcp
