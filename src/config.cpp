#include "industrial_mcp/config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace industrial_mcp {
namespace {

std::string read_text_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string required_string(const Json& object, const std::string& key) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw std::runtime_error("missing required string field: " + key);
    }
    return object.at(key).get<std::string>();
}

std::string optional_string(const Json& object, const std::string& key, const std::string& fallback = "") {
    if (!object.contains(key) || !object.at(key).is_string()) {
        return fallback;
    }
    return object.at(key).get<std::string>();
}

bool optional_bool(const Json& object, const std::string& key, bool fallback) {
    if (!object.contains(key) || !object.at(key).is_boolean()) {
        return fallback;
    }
    return object.at(key).get<bool>();
}

int optional_int(const Json& object, const std::string& key, int fallback) {
    if (!object.contains(key) || !object.at(key).is_number_integer()) {
        return fallback;
    }
    return object.at(key).get<int>();
}

std::optional<double> optional_number(const Json& object, const std::string& key) {
    if (!object.contains(key) || !object.at(key).is_number()) {
        return std::nullopt;
    }
    return object.at(key).get<double>();
}

std::vector<Json> optional_json_array(const Json& object, const std::string& key) {
    if (!object.contains(key) || !object.at(key).is_array()) {
        return {};
    }
    std::vector<Json> values;
    for (const auto& value : object.at(key)) {
        values.push_back(value);
    }
    return values;
}

std::unordered_map<std::string, std::vector<std::string>> default_roles() {
    return {
        {"viewer", {
            "get_gateway_health",
            "get_server_health",
            "list_devices",
            "get_device_state",
            "get_device_health",
            "get_alarm_history",
            "query_alarm_logs",
            "analyze_alarms",
            "diagnose_fault",
            "get_network_status",
            "read_node",
            "read_opcua_node",
            "read_device_snapshot",
            "get_device_status",
        }},
        {"operator", {
            "get_gateway_health",
            "get_server_health",
            "list_devices",
            "get_device_state",
            "get_device_health",
            "get_alarm_history",
            "query_alarm_logs",
            "analyze_alarms",
            "diagnose_fault",
            "get_network_status",
            "read_node",
            "read_opcua_node",
            "read_device_snapshot",
            "get_device_status",
            "refresh_device_state",
            "acknowledge_alarm",
            "clear_cached_alarm",
            "prepare_device_action",
            "confirm_device_action",
            "cancel_device_action",
            "write_node",
        }},
        {"administrator", {"*"}},
    };
}

std::string resolve_path(const std::string& base_dir, const std::string& value) {
    if (value.empty()) return value;
    std::filesystem::path path(value);
    if (path.is_absolute()) return path.string();
    return (std::filesystem::path(base_dir) / path).lexically_normal().string();
}

} // namespace

AppConfig ConfigLoader::load_file(const std::string& path) {
    const auto root = Json::parse(read_text_file(path));
    const auto base_dir = std::filesystem::absolute(std::filesystem::path(path)).parent_path().string();
    return load_json(root, base_dir);
}

AppConfig ConfigLoader::load_json(const Json& root, const std::string& base_dir) {
    if (!root.is_object()) {
        throw std::runtime_error("config root must be an object");
    }

    AppConfig config;
    config.security.roles = default_roles();

    if (root.contains("server") && root.at("server").is_object()) {
        const auto& server = root.at("server");
        config.server.name = optional_string(server, "name", config.server.name);
        config.server.version = optional_string(server, "version", config.server.version);
        config.server.read_only = optional_bool(server, "read_only", config.server.read_only);
    }

    if (root.contains("transport") && root.at("transport").is_object()) {
        const auto& transport = root.at("transport");
        config.transport.mode = optional_string(transport, "mode", config.transport.mode);
    } else if (root.contains("transport") && root.at("transport").is_string()) {
        config.transport.mode = root.at("transport").get<std::string>();
    }

    if (root.contains("http") && root.at("http").is_object()) {
        const auto& http = root.at("http");
        config.http.host = optional_string(http, "host", config.http.host);
        config.http.port = optional_int(http, "port", config.http.port);
    }

    if (root.contains("opcua") && root.at("opcua").is_object()) {
        const auto& opcua = root.at("opcua");
        config.opcua.connect_timeout_ms = optional_int(opcua, "connect_timeout_ms", config.opcua.connect_timeout_ms);
        config.opcua.read_timeout_ms = optional_int(opcua, "read_timeout_ms", config.opcua.read_timeout_ms);
        config.opcua.retry_count = optional_int(opcua, "retry_count", config.opcua.retry_count);
        config.opcua.retry_delay_ms = optional_int(opcua, "retry_delay_ms", config.opcua.retry_delay_ms);
        config.opcua.allow_raw_node_id = optional_bool(opcua, "allow_raw_node_id", config.opcua.allow_raw_node_id);
        config.opcua.write_enabled = optional_bool(opcua, "write_enabled", config.opcua.write_enabled);
    }

    if (root.contains("cache") && root.at("cache").is_object()) {
        const auto& cache = root.at("cache");
        config.cache.enabled = optional_bool(cache, "enabled", config.cache.enabled);
        config.cache.poll_interval_ms = optional_int(cache, "poll_interval_ms", config.cache.poll_interval_ms);
        config.cache.stale_after_ms = optional_int(cache, "stale_after_ms", config.cache.stale_after_ms);
    }

    if (root.contains("security") && root.at("security").is_object()) {
        const auto& security = root.at("security");
        config.security.enabled = optional_bool(security, "enabled", config.security.enabled);
        config.security.default_role = optional_string(security, "default_role", config.security.default_role);
        if (security.contains("roles") && security.at("roles").is_object()) {
            for (auto it = security.at("roles").begin(); it != security.at("roles").end(); ++it) {
                if (!it.value().is_array()) continue;
                std::vector<std::string> tools;
                for (const auto& item : it.value()) {
                    if (item.is_string()) tools.push_back(item.get<std::string>());
                }
                config.security.roles[it.key()] = std::move(tools);
            }
        }
    }

    if (root.contains("roles") && root.at("roles").is_object()) {
        for (auto it = root.at("roles").begin(); it != root.at("roles").end(); ++it) {
            if (!it.value().is_array()) continue;
            std::vector<std::string> tools;
            for (const auto& item : it.value()) {
                if (item.is_string()) tools.push_back(item.get<std::string>());
            }
            config.security.roles[it.key()] = std::move(tools);
        }
    }

    if (root.contains("timeouts") && root.at("timeouts").is_object()) {
        const auto& timeouts = root.at("timeouts");
        config.timeouts.mcp_request_ms = optional_int(timeouts, "mcp_request_ms", config.timeouts.mcp_request_ms);
        config.timeouts.tool_execution_ms = optional_int(timeouts, "tool_execution_ms", config.timeouts.tool_execution_ms);
        config.timeouts.opcua_request_ms = optional_int(timeouts, "opcua_request_ms", config.timeouts.opcua_request_ms);
    }

    if (root.contains("observability") && root.at("observability").is_object()) {
        const auto& observability = root.at("observability");
        config.observability.metrics_enabled = optional_bool(observability, "metrics_enabled", config.observability.metrics_enabled);
        config.observability.metrics_port = optional_int(observability, "metrics_port", config.observability.metrics_port);
    }

    if (root.contains("storage") && root.at("storage").is_object()) {
        const auto& storage = root.at("storage");
        config.storage.type = optional_string(storage, "type", config.storage.type);
        config.storage.sqlite_path = resolve_path(base_dir, optional_string(storage, "sqlite_path", config.storage.sqlite_path));
    }

    if (root.contains("audit") && root.at("audit").is_object()) {
        const auto& audit = root.at("audit");
        config.audit.log_path = resolve_path(base_dir, optional_string(audit, "log_path", config.audit.log_path));
    }

    if (root.contains("alarm_log_path") && root.at("alarm_log_path").is_string()) {
        config.alarm_log_path = resolve_path(base_dir, root.at("alarm_log_path").get<std::string>());
    }

    if (!root.contains("devices") || !root.at("devices").is_array()) {
        throw std::runtime_error("config must contain devices array");
    }

    for (const auto& device_json : root.at("devices")) {
        if (!device_json.is_object()) {
            throw std::runtime_error("device item must be an object");
        }

        DeviceConfig device;
        device.id = required_string(device_json, "id");
        device.name = optional_string(device_json, "name", device.id);
        device.protocol = optional_string(device_json, "protocol", device.protocol);
        device.endpoint = required_string(device_json, "endpoint");
        device.enabled = optional_bool(device_json, "enabled", device.enabled);

        if (!device_json.contains("variables") || !device_json.at("variables").is_array()) {
            throw std::runtime_error("device " + device.id + " must contain variables array");
        }

        for (const auto& variable_json : device_json.at("variables")) {
            if (!variable_json.is_object()) {
                throw std::runtime_error("variable item for device " + device.id + " must be an object");
            }

            VariableConfig variable;
            variable.name = required_string(variable_json, "name");
            variable.node_id = required_string(variable_json, "node_id");
            variable.data_type = optional_string(variable_json, "data_type", "unknown");
            variable.unit = optional_string(variable_json, "unit");
            variable.description = optional_string(variable_json, "description");
            variable.mock_value = variable_json.contains("mock_value") ? variable_json.at("mock_value") : Json(nullptr);
            variable.writable = optional_bool(variable_json, "writable", variable.writable);
            variable.write_min = optional_number(variable_json, "min");
            variable.write_max = optional_number(variable_json, "max");
            variable.allowed_values = optional_json_array(variable_json, "allowed_values");
            variable.warn_min = optional_number(variable_json, "warn_min");
            variable.warn_max = optional_number(variable_json, "warn_max");
            variable.alarm_min = optional_number(variable_json, "alarm_min");
            variable.alarm_max = optional_number(variable_json, "alarm_max");
            device.variables.emplace(variable.name, std::move(variable));
        }

        config.devices.push_back(std::move(device));
    }

    return config;
}

const DeviceConfig* find_device(const AppConfig& config, const std::string& device_id) {
    for (const auto& device : config.devices) {
        if (device.id == device_id) return &device;
    }
    return nullptr;
}

const VariableConfig* find_variable(const DeviceConfig& device, const std::string& name_or_node_id) {
    const auto by_name = device.variables.find(name_or_node_id);
    if (by_name != device.variables.end()) return &by_name->second;
    for (const auto& [_, variable] : device.variables) {
        if (variable.node_id == name_or_node_id) return &variable;
    }
    return nullptr;
}

} // namespace industrial_mcp
