#include "industrial_mcp/mcp/json.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace industrial_mcp {

std::string now_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

} // namespace industrial_mcp
