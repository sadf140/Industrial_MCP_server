#pragma once

#include "industrial_mcp/mcp/json.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace industrial_mcp {

struct ServerConfig {
    std::string name = "industrial-mcp-server";
    std::string version = "0.5.0-p3";
    bool read_only = true;
};

struct TransportConfig {
    std::string mode = "stdio";
};

struct HttpConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
};

struct OpcUaRuntimeConfig {
    int connect_timeout_ms = 3000;
    int read_timeout_ms = 3000;
    int retry_count = 1;
    int retry_delay_ms = 200;
    bool allow_raw_node_id = false;
    bool write_enabled = false;
};

struct AuditConfig {
    std::string log_path;
};

struct CacheConfig {
    bool enabled = true;
    int poll_interval_ms = 2000;
    int stale_after_ms = 10000;
};

struct SecurityConfig {
    bool enabled = true;
    std::string default_role = "viewer";
    bool hide_unauthorized_tools = true;
    std::unordered_map<std::string, std::vector<std::string>> roles;
};

struct TimeoutConfig {
    int mcp_request_ms = 5000;
    int tool_execution_ms = 3000;
    int opcua_request_ms = 1000;
};

struct ReliabilityConfig {
    int max_retry_count = 1;
    int backoff_initial_ms = 100;
    int backoff_max_ms = 30000;
    int circuit_failure_threshold = 3;
    int circuit_cooldown_ms = 5000;
};

struct ObservabilityConfig {
    bool metrics_enabled = false;
    int metrics_port = 9090;
};

struct StorageConfig {
    std::string type = "jsonl";
    std::string sqlite_path;
};

struct VariableConfig {
    std::string name;
    std::string node_id;
    std::string data_type;
    std::string unit;
    std::string description;
    Json mock_value;
    bool writable = false;
    std::optional<double> write_min;
    std::optional<double> write_max;
    std::vector<Json> allowed_values;
    std::optional<double> warn_min;
    std::optional<double> warn_max;
    std::optional<double> alarm_min;
    std::optional<double> alarm_max;
};

struct MethodConfig {
    std::string name;
    std::string object_id;
    std::string method_id;
    std::string description;
    bool enabled = true;
    bool requires_confirmation = true;
    std::vector<std::string> input_types;
    Json mock_result;
};

struct DeviceConfig {
    std::string id;
    std::string name;
    std::string protocol = "opcua";
    std::string endpoint;
    bool enabled = true;
    std::unordered_map<std::string, VariableConfig> variables;
    std::unordered_map<std::string, MethodConfig> methods;
};

struct AppConfig {
    std::string source_path;
    ServerConfig server;
    TransportConfig transport;
    HttpConfig http;
    OpcUaRuntimeConfig opcua;
    CacheConfig cache;
    SecurityConfig security;
    TimeoutConfig timeouts;
    ReliabilityConfig reliability;
    ObservabilityConfig observability;
    StorageConfig storage;
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
DeviceConfig* find_device(AppConfig& config, const std::string& device_id);
const VariableConfig* find_variable(const DeviceConfig& device, const std::string& name_or_node_id);
VariableConfig* find_variable(DeviceConfig& device, const std::string& name_or_node_id);
const MethodConfig* find_method(const DeviceConfig& device, const std::string& method_name);

} // namespace industrial_mcp
