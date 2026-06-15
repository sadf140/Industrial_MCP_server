#pragma once

#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/mcp/json.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
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
    long long latency_ms = -1;
    int disconnect_count = 0;
    int consecutive_failures = 0;
    std::string last_success_at;
    std::string last_error_at;
    std::string last_error;
};

struct OpcUaWriteResult {
    bool ok = false;
    Json value;
    std::string data_type;
    std::string timestamp;
    std::string quality;
    std::string status_code;
    std::string error;
    std::string error_code;
    int attempts = 0;
    long long latency_ms = -1;
};

struct OpcUaMethodResult {
    bool ok = false;
    Json output_arguments = Json::array();
    std::string timestamp;
    std::string quality;
    std::string status_code;
    std::string error;
    std::string error_code;
    int attempts = 0;
    long long latency_ms = -1;
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
    OpcUaWriteResult write_node(const DeviceConfig& device,
                                const VariableConfig& variable,
                                const Json& value,
                                const OpcUaRuntimeConfig& runtime);
    OpcUaMethodResult call_method(const DeviceConfig& device,
                                  const MethodConfig& method,
                                  const Json& arguments,
                                  const OpcUaRuntimeConfig& runtime);
    DeviceStatus get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime);

private:
    struct NetworkStats {
        bool has_state = false;
        bool online = false;
        long long latency_ms = -1;
        int disconnect_count = 0;
        int consecutive_failures = 0;
        std::string last_success_at;
        std::string last_error_at;
        std::string last_error;
    };

    void update_stats(const std::string& device_id, bool online, long long latency_ms, const std::string& error);
    NetworkStats stats_for(const std::string& device_id) const;

    mutable std::mutex stats_mutex_;
    std::unordered_map<std::string, NetworkStats> stats_;
};

} // namespace industrial_mcp
