#pragma once

#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/audit_logger.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/device_state_cache.hpp"
#include "industrial_mcp/diagnostics.hpp"
#include "industrial_mcp/opcua_client.hpp"

#include <chrono>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace industrial_mcp {

class McpServer {
public:
    explicit McpServer(AppConfig config);
    ~McpServer();

    void run(std::istream& input, std::ostream& output);
    std::optional<Json> handle_message(const Json& request);

private:
    struct PendingOperation {
        std::string operation_id;
        std::string user_id;
        std::string client_id;
        std::string device_id;
        std::string action;
        Json arguments = Json::object();
        std::chrono::steady_clock::time_point expires_at;
        bool confirmed = false;
    };

    AppConfig config_;
    OpcUaClient opcua_;
    AlarmStore alarms_;
    AuditLogger audit_;
    DeviceStateCache state_cache_;
    DiagnosticsEngine diagnostics_;
    std::chrono::steady_clock::time_point started_at_;
    std::string started_at_utc_;
    mutable std::mutex operations_mutex_;
    std::unordered_map<std::string, PendingOperation> pending_operations_;
    unsigned long long operation_counter_ = 0;

    Json list_tools() const;
    Json call_tool(const Json& params);
    Json gateway_health() const;
    Json device_health(const std::string& device_id);
};

} // namespace industrial_mcp
