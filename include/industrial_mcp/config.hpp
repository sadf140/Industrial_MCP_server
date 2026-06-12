#pragma once

#include "industrial_mcp/json.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace industrial_mcp {

struct ServerConfig {
    std::string name = "industrial-mcp-server";
    std::string version = "0.4.0-p2";
    bool read_only = true;
};

struct OpcUaRuntimeConfig {
    int connect_timeout_ms = 3000;
    int read_timeout_ms = 3000;
    int retry_count = 1;
    int retry_delay_ms = 200;
    bool allow_raw_node_id = false;
};

struct AuditConfig {
    std::string log_path;
};

struct VariableConfig {
    std::string name;
    std::string node_id;
    std::string data_type;
    std::string unit;
    std::string description;
    Json mock_value;
    std::optional<double> warn_min;
    std::optional<double> warn_max;
    std::optional<double> alarm_min;
    std::optional<double> alarm_max;
};

struct DeviceConfig {
    std::string id;
    std::string name;
    std::string endpoint;
    std::unordered_map<std::string, VariableConfig> variables;
};

struct AppConfig {
    ServerConfig server;
    OpcUaRuntimeConfig opcua;
    AuditConfig audit;
    std::string alarm_log_path;
    std::vector<DeviceConfig> devices;
};

class ConfigLoader {
public:
    static AppConfig load_file(const std::string& path);
    static AppConfig load_json(const Json& root, const std::string& base_dir);
};

const DeviceConfig* find_device(const AppConfig& config, const std::string& device_id);
const VariableConfig* find_variable(const DeviceConfig& device, const std::string& name_or_node_id);

} // namespace industrial_mcp
