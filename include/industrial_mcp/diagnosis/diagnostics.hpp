#pragma once

#include "industrial_mcp/alarm/alarm_store.hpp"
#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/device/device_state_cache.hpp"

namespace industrial_mcp {

class DiagnosticsEngine {
public:
    Json diagnose(const AppConfig& config,
                  const AlarmStore& alarms,
                  const DeviceStateCache& state_cache,
                  const Json& arguments);
};

} // namespace industrial_mcp
