#include "industrial_mcp/connector/industrial_connector.hpp"

#include <chrono>
#include <string_view>

namespace industrial_mcp {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

long long elapsed_ms_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    ).count();
}

} // namespace

OpcUaReadResult OpcUaConnector::read_node(const DeviceConfig& device,
                                          const VariableConfig* variable,
                                          const std::string& node_id,
                                          const OpcUaRuntimeConfig& runtime) {
    return client_.read_node(device, variable, node_id, runtime);
}

std::vector<OpcUaReadResult> OpcUaConnector::read_nodes(const DeviceConfig& device,
                                                        const std::vector<const VariableConfig*>& variables,
                                                        const OpcUaRuntimeConfig& runtime) {
    return client_.read_nodes(device, variables, runtime);
}

OpcUaWriteResult OpcUaConnector::write_node(const DeviceConfig& device,
                                            const VariableConfig& variable,
                                            const Json& value,
                                            const OpcUaRuntimeConfig& runtime) {
    return client_.write_node(device, variable, value, runtime);
}

OpcUaMethodResult OpcUaConnector::call_method(const DeviceConfig& device,
                                              const MethodConfig& method,
                                              const Json& arguments,
                                              const OpcUaRuntimeConfig& runtime) {
    return client_.call_method(device, method, arguments, runtime);
}

DeviceStatus OpcUaConnector::get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) {
    return client_.get_status(device, runtime);
}

OpcUaReadResult MockConnector::read_node(const DeviceConfig& device,
                                         const VariableConfig* variable,
                                         const std::string& node_id,
                                         const OpcUaRuntimeConfig& runtime) {
    (void)runtime;
    const auto started = std::chrono::steady_clock::now();
    OpcUaReadResult result;
    result.timestamp = now_utc_iso8601();
    result.attempts = 1;
    if (variable == nullptr) {
        result.quality = "BadNodeIdUnknown";
        result.status_code = "BadNodeIdUnknown";
        result.error = "mock endpoint requires a configured variable for node_id: " + node_id;
        return result;
    }
    if (variable->mock_value.is_null()) {
        result.quality = "BadNoData";
        result.status_code = "BadNoData";
        result.error = "mock variable has no mock_value: " + variable->name;
        return result;
    }

    result.ok = true;
    result.value = variable->mock_value;
    result.data_type = variable->data_type;
    result.quality = "Good";
    result.status_code = "Good";
    (void)device;
    (void)started;
    return result;
}

std::vector<OpcUaReadResult> MockConnector::read_nodes(const DeviceConfig& device,
                                                       const std::vector<const VariableConfig*>& variables,
                                                       const OpcUaRuntimeConfig& runtime) {
    std::vector<OpcUaReadResult> results;
    results.reserve(variables.size());
    for (const auto* variable : variables) {
        results.push_back(read_node(device, variable, variable == nullptr ? "" : variable->node_id, runtime));
    }
    return results;
}

OpcUaWriteResult MockConnector::write_node(const DeviceConfig& device,
                                           const VariableConfig& variable,
                                           const Json& value,
                                           const OpcUaRuntimeConfig& runtime) {
    (void)device;
    (void)runtime;
    const auto started = std::chrono::steady_clock::now();
    OpcUaWriteResult result;
    result.timestamp = now_utc_iso8601();
    result.value = value;
    result.data_type = variable.data_type;
    result.ok = true;
    result.quality = "Good";
    result.status_code = "Good";
    result.attempts = 1;
    result.latency_ms = elapsed_ms_since(started);
    return result;
}

OpcUaMethodResult MockConnector::call_method(const DeviceConfig& device,
                                             const MethodConfig& method,
                                             const Json& arguments,
                                             const OpcUaRuntimeConfig& runtime) {
    (void)device;
    (void)arguments;
    (void)runtime;
    const auto started = std::chrono::steady_clock::now();
    OpcUaMethodResult result;
    result.timestamp = now_utc_iso8601();
    result.attempts = 1;
    if (!method.enabled) {
        result.quality = "BadMethodInvalid";
        result.status_code = "BadMethodInvalid";
        result.error = "method is disabled: " + method.name;
        result.error_code = "METHOD_DISABLED";
        result.latency_ms = elapsed_ms_since(started);
        return result;
    }
    result.ok = true;
    result.output_arguments = method.mock_result.is_null() ? Json::array() : method.mock_result;
    result.quality = "Good";
    result.status_code = "Good";
    result.latency_ms = elapsed_ms_since(started);
    return result;
}

DeviceStatus MockConnector::get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) {
    (void)runtime;
    const auto started = std::chrono::steady_clock::now();
    DeviceStatus status;
    status.device_id = device.id;
    status.endpoint = device.endpoint;
    status.online = true;
    status.session_state = "mock_connected";
    status.last_read_time = now_utc_iso8601();
    status.attempts = 1;
    status.latency_ms = elapsed_ms_since(started);
    status.last_success_at = status.last_read_time;
    return status;
}

std::unique_ptr<IIndustrialConnector> make_connector_for_device(const DeviceConfig& device) {
    if (starts_with(device.endpoint, "mock://")) {
        return std::make_unique<MockConnector>();
    }
    return std::make_unique<OpcUaConnector>();
}

} // namespace industrial_mcp
