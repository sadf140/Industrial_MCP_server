#include "industrial_mcp/observability/observability.hpp"

#include <iostream>
#include <sstream>

namespace industrial_mcp {
namespace {

std::string label_escape(const std::string& value) {
    std::string out;
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

std::string json_string(const Json& object, const std::string& key) {
    if (object.is_object() && object.contains(key) && object.at(key).is_string()) {
        return object.at(key).get<std::string>();
    }
    return {};
}

double json_number(const Json& object, const std::string& key, double fallback = 0.0) {
    if (object.is_object() && object.contains(key) && object.at(key).is_number()) {
        return object.at(key).get<double>();
    }
    return fallback;
}

bool json_bool(const Json& object, const std::string& key) {
    return object.is_object() && object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>();
}

} // namespace

void ObservabilityMetrics::record_tool_call(const std::string& tool_name, bool ok, long long duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++request_count_;
    if (!ok) ++error_count_;
    duration_seconds_sum_ += static_cast<double>(duration_ms) / 1000.0;
    auto& tool = tools_[tool_name.empty() ? "unknown" : tool_name];
    ++tool.count;
    if (!ok) ++tool.errors;
    tool.duration_seconds_sum += static_cast<double>(duration_ms) / 1000.0;
}

void ObservabilityMetrics::record_alarm_event() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++alarm_events_;
}

Json ObservabilityMetrics::snapshot_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json tools = Json::object();
    for (const auto& [name, stats] : tools_) {
        tools[name] = {
            {"count", stats.count},
            {"errors", stats.errors},
            {"duration_seconds_sum", stats.duration_seconds_sum},
        };
    }
    return {
        {"mcp_requests_total", request_count_},
        {"mcp_tool_errors_total", error_count_},
        {"mcp_request_duration_seconds_sum", duration_seconds_sum_},
        {"mcp_request_duration_seconds_count", request_count_},
        {"alarm_events_total", alarm_events_},
        {"tools", tools},
    };
}

std::string ObservabilityMetrics::prometheus_text(const Json& device_health, const Json& cache_state) const {
    const auto snapshot = snapshot_json();
    std::ostringstream out;
    out << "# TYPE mcp_requests_total counter\n";
    out << "mcp_requests_total " << snapshot.at("mcp_requests_total").get<unsigned long long>() << "\n";
    out << "# TYPE mcp_request_duration_seconds summary\n";
    out << "mcp_request_duration_seconds_count " << snapshot.at("mcp_request_duration_seconds_count").get<unsigned long long>() << "\n";
    out << "mcp_request_duration_seconds_sum " << snapshot.at("mcp_request_duration_seconds_sum").get<double>() << "\n";
    out << "# TYPE mcp_tool_errors_total counter\n";
    out << "mcp_tool_errors_total " << snapshot.at("mcp_tool_errors_total").get<unsigned long long>() << "\n";
    if (snapshot.contains("tools") && snapshot.at("tools").is_object()) {
        for (auto it = snapshot.at("tools").begin(); it != snapshot.at("tools").end(); ++it) {
            out << "mcp_tool_errors_total{tool=\"" << label_escape(it.key()) << "\"} "
                << it.value().at("errors").get<unsigned long long>() << "\n";
        }
    }
    out << "# TYPE alarm_events_total counter\n";
    out << "alarm_events_total " << snapshot.at("alarm_events_total").get<unsigned long long>() << "\n";

    out << "# TYPE opcua_connection_state gauge\n";
    out << "# TYPE opcua_reconnect_total counter\n";
    if (device_health.is_array()) {
        for (const auto& device : device_health) {
            const auto id = label_escape(json_string(device, "device_id"));
            out << "opcua_connection_state{device_id=\"" << id << "\"} " << (json_bool(device, "online") ? 1 : 0) << "\n";
            out << "opcua_reconnect_total{device_id=\"" << id << "\"} "
                << static_cast<unsigned long long>(json_number(device, "reconnect_count")) << "\n";
        }
    }

    out << "# TYPE device_cache_age_seconds gauge\n";
    if (cache_state.is_object() && cache_state.contains("devices") && cache_state.at("devices").is_array()) {
        for (const auto& device : cache_state.at("devices")) {
            out << "device_cache_age_seconds{device_id=\"" << label_escape(json_string(device, "device_id")) << "\"} "
                << json_number(device, "cache_age_seconds", -1.0) << "\n";
        }
    } else if (cache_state.is_object() && cache_state.contains("device") && cache_state.at("device").is_object()) {
        const auto& device = cache_state.at("device");
        out << "device_cache_age_seconds{device_id=\"" << label_escape(json_string(device, "device_id")) << "\"} "
            << json_number(device, "cache_age_seconds", -1.0) << "\n";
    }
    return out.str();
}

void emit_structured_log(const std::string& level, const std::string& event, const Json& fields) {
    auto record = fields.is_object() ? fields : Json::object();
    record["timestamp"] = now_utc_iso8601();
    record["level"] = level;
    record["event"] = event;
    std::cerr << record.dump() << '\n';
}

} // namespace industrial_mcp
