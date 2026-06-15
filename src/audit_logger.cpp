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

void AuditLogger::record(const AuditRecord& record) {
    if (path_.empty()) {
        return;
    }

    Json event = {
        {"timestamp", now_utc_iso8601()},
        {"event", "tool_call"},
        {"request_id", record.request_id},
        {"user_id", record.user_id},
        {"client_id", record.client_id},
        {"tool", record.tool_name},
        {"tool_name", record.tool_name},
        {"device_id", record.device_id},
        {"target_node", record.target_node},
        {"result", record.result},
        {"ok", record.result == "ok"},
        {"elapsed_ms", record.elapsed_ms},
        {"read_only", record.read_only},
        {"arguments", sanitized_arguments(record.arguments)},
    };
    if (!record.old_value.is_null()) event["old_value"] = record.old_value;
    if (!record.new_value.is_null()) event["new_value"] = record.new_value;
    if (!record.error_code.empty()) event["error_code"] = record.error_code;

    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream output(path_, std::ios::app);
    if (output) {
        output << event.dump() << '\n';
    }
}

void AuditLogger::record_tool_call(const std::string& tool_name,
                                   const std::string& device_id,
                                   bool ok,
                                   long long elapsed_ms,
                                   const std::string& error_code,
                                   const Json& arguments,
                                   bool read_only) {
    AuditRecord record;
    record.tool_name = tool_name;
    record.device_id = device_id;
    record.result = ok ? "ok" : "error";
    record.elapsed_ms = elapsed_ms;
    record.error_code = error_code;
    record.arguments = arguments;
    record.read_only = read_only;
    this->record(record);
}

} // namespace industrial_mcp
