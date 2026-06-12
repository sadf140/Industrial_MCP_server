#pragma once

#include "industrial_mcp/config.hpp"
#include "industrial_mcp/json.hpp"

#include <string>
#include <vector>

namespace industrial_mcp {

struct OpcUaReadResult {
    bool ok = false;
    Json value;
    std::string data_type;
    std::string timestamp;
    std::string quality;
    std::string status_code;
    std::string error;
    int attempts = 0;
};

struct DeviceStatus {
    std::string device_id;
    bool online = false;
    std::string endpoint;
    std::string session_state;
    std::string last_read_time;
    std::string error;
    int attempts = 0;
};

class OpcUaClient {
public:
    OpcUaReadResult read_node(const DeviceConfig& device,
                              const VariableConfig* variable,
                              const std::string& node_id,
                              const OpcUaRuntimeConfig& runtime);
    std::vector<OpcUaReadResult> read_nodes(const DeviceConfig& device,
                                            const std::vector<const VariableConfig*>& variables,
                                            const OpcUaRuntimeConfig& runtime);
    DeviceStatus get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) const;
};

} // namespace industrial_mcp
