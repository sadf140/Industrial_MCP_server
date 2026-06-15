#pragma once

#include "industrial_mcp/mcp/json.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace industrial_mcp {

class ObservabilityMetrics {
public:
    void record_tool_call(const std::string& tool_name, bool ok, long long duration_ms);
    void record_alarm_event();

    Json snapshot_json() const;
    std::string prometheus_text(const Json& device_health, const Json& cache_state) const;

private:
    struct ToolStats {
        unsigned long long count = 0;
        unsigned long long errors = 0;
        double duration_seconds_sum = 0.0;
    };

    mutable std::mutex mutex_;
    unsigned long long request_count_ = 0;
    unsigned long long error_count_ = 0;
    unsigned long long alarm_events_ = 0;
    double duration_seconds_sum_ = 0.0;
    std::unordered_map<std::string, ToolStats> tools_;
};

void emit_structured_log(const std::string& level,
                         const std::string& event,
                         const Json& fields = Json::object());

} // namespace industrial_mcp
