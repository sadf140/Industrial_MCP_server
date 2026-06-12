#include "industrial_mcp/audit_logger.hpp"

#include "industrial_mcp/json.hpp"

#include <fstream>
#include <utility>

namespace industrial_mcp {
namespace {

Json sanitized_arguments(const Json& arguments) {
    Json out = arguments.is_object() ? arguments : Json::object();
    for (const auto& key : {"password", "token", "secret", "private_key"}) {
        if (out.contains(key)) {
            out[key] = "***";
        }
    }
    return out;
}

} // namespace

AuditLogger::AuditLogger(std::string path) : path_(std::move(path)) {}

void AuditLogger::record_tool_call(const std::string& tool_name,
                                   const std::string& device_id,
                                   bool ok,
                                   long long elapsed_ms,
                                   const std::string& error_code,
                                   const Json& arguments) {
    if (path_.empty()) {
        return;
    }

    Json event = {
        {"timestamp", now_utc_iso8601()},
        {"event", "tool_call"},
        {"tool", tool_name},
        {"device_id", device_id},
        {"ok", ok},
        {"elapsed_ms", elapsed_ms},
        {"read_only", true},
        {"arguments", sanitized_arguments(arguments)},
    };
    if (!error_code.empty()) {
        event["error_code"] = error_code;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream output(path_, std::ios::app);
    if (output) {
        output << event.dump() << '\n';
    }
}

} // namespace industrial_mcp
