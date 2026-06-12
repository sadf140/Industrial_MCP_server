#pragma once

#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/audit_logger.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/diagnostics.hpp"
#include "industrial_mcp/opcua_client.hpp"

#include <iosfwd>
#include <optional>

namespace industrial_mcp {

class McpServer {
public:
    explicit McpServer(AppConfig config);

    void run(std::istream& input, std::ostream& output);
    std::optional<Json> handle_message(const Json& request);

private:
    AppConfig config_;
    OpcUaClient opcua_;
    AlarmStore alarms_;
    AuditLogger audit_;
    DiagnosticsEngine diagnostics_;

    Json list_tools() const;
    Json call_tool(const Json& params);
};

} // namespace industrial_mcp
