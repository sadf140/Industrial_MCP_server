#pragma once

#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/opcua_client.hpp"

namespace industrial_mcp {

class DiagnosticsEngine {
public:
    Json diagnose(const AppConfig& config,
                  OpcUaClient& opcua,
                  const AlarmStore& alarms,
                  const Json& arguments);
};

} // namespace industrial_mcp
