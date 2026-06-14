#pragma once

#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/audit_logger.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/device_state_cache.hpp"
#include "industrial_mcp/diagnostics.hpp"
#include "industrial_mcp/opcua_client.hpp"

#include <chrono>
#include <iosfwd>
#include <optional>
#include <string>

namespace industrial_mcp {

class McpServer {
public:
    explicit McpServer(AppConfig config);
    ~McpServer();

    void run(std::istream& input, std::ostream& output);
    std::optional<Json> handle_message(const Json& request);

private:
    AppConfig config_;
    OpcUaClient opcua_;
    AlarmStore alarms_;
    AuditLogger audit_;
    DeviceStateCache state_cache_;
    DiagnosticsEngine diagnostics_;
    std::chrono::steady_clock::time_point started_at_;
    std::string started_at_utc_;

    Json list_tools() const;
    Json call_tool(const Json& params);
    Json gateway_health() const;
};

} // namespace industrial_mcp
