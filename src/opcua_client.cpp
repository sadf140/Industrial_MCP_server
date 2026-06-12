#include "industrial_mcp/opcua_client.hpp"

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
#include <open62541pp/client.hpp>
#include <open62541pp/exception.hpp>
#include <open62541pp/node.hpp>
#include <open62541pp/types.hpp>
#endif

#include <cstdint>
#include <chrono>
#include <exception>
#include <string_view>
#include <thread>

namespace industrial_mcp {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
std::string status_name(const opcua::BadStatus& status) {
    return std::string(status.what());
}

std::string type_name(const UA_DataType* type) {
    if (type == nullptr) return "Unknown";
    if (type == &UA_TYPES[UA_TYPES_BOOLEAN]) return "Boolean";
    if (type == &UA_TYPES[UA_TYPES_SBYTE]) return "SByte";
    if (type == &UA_TYPES[UA_TYPES_BYTE]) return "Byte";
    if (type == &UA_TYPES[UA_TYPES_INT16]) return "Int16";
    if (type == &UA_TYPES[UA_TYPES_UINT16]) return "UInt16";
    if (type == &UA_TYPES[UA_TYPES_INT32]) return "Int32";
    if (type == &UA_TYPES[UA_TYPES_UINT32]) return "UInt32";
    if (type == &UA_TYPES[UA_TYPES_INT64]) return "Int64";
    if (type == &UA_TYPES[UA_TYPES_UINT64]) return "UInt64";
    if (type == &UA_TYPES[UA_TYPES_FLOAT]) return "Float";
    if (type == &UA_TYPES[UA_TYPES_DOUBLE]) return "Double";
    if (type == &UA_TYPES[UA_TYPES_STRING]) return "String";
    if (type == &UA_TYPES[UA_TYPES_DATETIME]) return "DateTime";
    if (type == &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]) return "LocalizedText";
    if (type == &UA_TYPES[UA_TYPES_QUALIFIEDNAME]) return "QualifiedName";
    return "Unsupported";
}

Json variant_to_json(const opcua::Variant& value, std::string& data_type, std::string& error) {
    if (value.empty()) {
        error = "OPC UA value is empty";
        return nullptr;
    }
    if (!value.isScalar()) {
        error = "OPC UA arrays are not supported by the current adapter";
        return nullptr;
    }

    data_type = type_name(value.type());
    try {
        if (value.isType<bool>()) return value.to<bool>();
        if (value.isType<int8_t>()) return static_cast<int>(value.to<int8_t>());
        if (value.isType<uint8_t>()) return static_cast<int>(value.to<uint8_t>());
        if (value.isType<int16_t>()) return static_cast<int>(value.to<int16_t>());
        if (value.isType<uint16_t>()) return static_cast<int>(value.to<uint16_t>());
        if (value.isType<int32_t>()) return value.to<int32_t>();
        if (value.isType<uint32_t>()) return value.to<uint32_t>();
        if (value.isType<int64_t>()) return std::to_string(value.to<int64_t>());
        if (value.isType<uint64_t>()) return std::to_string(value.to<uint64_t>());
        if (value.isType<float>()) return static_cast<double>(value.to<float>());
        if (value.isType<double>()) return value.to<double>();
        if (value.isType<opcua::String>()) return std::string(value.to<opcua::String>());
        if (value.isType<opcua::DateTime>()) return std::string(value.to<opcua::DateTime>().format("%Y-%m-%dT%H:%M:%SZ"));
        if (value.isType<opcua::LocalizedText>()) return std::string(value.to<opcua::LocalizedText>().text());
        if (value.isType<opcua::QualifiedName>()) return std::string(value.to<opcua::QualifiedName>().name());
    } catch (const std::exception& ex) {
        error = ex.what();
        return nullptr;
    }

    error = "unsupported OPC UA scalar data type: " + data_type;
    return nullptr;
}

opcua::Client make_client(const OpcUaRuntimeConfig& runtime) {
    opcua::Client client;
    const auto timeout = runtime.read_timeout_ms > 0 ? runtime.read_timeout_ms : runtime.connect_timeout_ms;
    client.config().setTimeout(static_cast<uint32_t>(timeout > 0 ? timeout : 3000));
    return client;
}

int attempts_for(const OpcUaRuntimeConfig& runtime) {
    return runtime.retry_count < 0 ? 1 : runtime.retry_count + 1;
}

void retry_delay(const OpcUaRuntimeConfig& runtime) {
    if (runtime.retry_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(runtime.retry_delay_ms));
    }
}
#endif

} // namespace

OpcUaReadResult OpcUaClient::read_node(const DeviceConfig& device,
                                       const VariableConfig* variable,
                                       const std::string& node_id,
                                       const OpcUaRuntimeConfig& runtime) {
    OpcUaReadResult result;
    result.timestamp = now_utc_iso8601();

    if (starts_with(device.endpoint, "mock://")) {
        result.attempts = 1;
        if (variable == nullptr) {
            result.quality = "BadNodeIdUnknown";
            result.status_code = "BadNodeIdUnknown";
            result.error = "mock endpoint requires a configured variable for node_id: " + node_id;
            return result;
        }

        result.ok = true;
        result.value = variable->mock_value;
        result.data_type = variable->data_type;
        result.quality = "Good";
        result.status_code = "Good";
        return result;
    }

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    for (int attempt = 1; attempt <= attempts_for(runtime); ++attempt) {
        try {
            opcua::Client client = make_client(runtime);
            client.connect(device.endpoint);

            opcua::Node node{client, opcua::NodeId::parse(node_id)};
            const auto value = node.readValue();
            result.value = variant_to_json(value, result.data_type, result.error);
            result.ok = result.error.empty();
            result.quality = result.ok ? "Good" : "BadDataTypeUnsupported";
            result.status_code = result.quality;
            result.attempts = attempt;

            client.disconnect();
            break;
        } catch (const opcua::BadStatus& ex) {
            result.quality = status_name(ex);
            result.status_code = result.quality;
            result.error = ex.what();
            result.attempts = attempt;
        } catch (const std::exception& ex) {
            result.quality = "BadUnexpectedError";
            result.status_code = "BadUnexpectedError";
            result.error = ex.what();
            result.attempts = attempt;
        }
        if (attempt < attempts_for(runtime)) {
            retry_delay(runtime);
        }
    }
#else
    (void)runtime;
    result.quality = "BadNotConnected";
    result.status_code = "BadNotConnected";
    result.error = "OPC UA live reads are not compiled; use mock:// endpoints or rebuild with OPC UA support";
#endif
    return result;
}

std::vector<OpcUaReadResult> OpcUaClient::read_nodes(const DeviceConfig& device,
                                                     const std::vector<const VariableConfig*>& variables,
                                                     const OpcUaRuntimeConfig& runtime) {
    std::vector<OpcUaReadResult> results;
    results.reserve(variables.size());

    if (starts_with(device.endpoint, "mock://")) {
        for (const auto* variable : variables) {
            results.push_back(read_node(device, variable, variable == nullptr ? "" : variable->node_id, runtime));
        }
        return results;
    }

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    for (int attempt = 1; attempt <= attempts_for(runtime); ++attempt) {
        results.clear();
        try {
            opcua::Client client = make_client(runtime);
            client.connect(device.endpoint);

            for (const auto* variable : variables) {
                OpcUaReadResult result;
                result.timestamp = now_utc_iso8601();
                result.attempts = attempt;
                if (variable == nullptr) {
                    result.quality = "BadNodeIdUnknown";
                    result.status_code = "BadNodeIdUnknown";
                    result.error = "snapshot variable is null";
                    results.push_back(std::move(result));
                    continue;
                }

                opcua::Node node{client, opcua::NodeId::parse(variable->node_id)};
                const auto value = node.readValue();
                result.value = variant_to_json(value, result.data_type, result.error);
                result.ok = result.error.empty();
                result.quality = result.ok ? "Good" : "BadDataTypeUnsupported";
                result.status_code = result.quality;
                results.push_back(std::move(result));
            }

            client.disconnect();
            return results;
        } catch (const opcua::BadStatus& ex) {
            results.clear();
            for (const auto* variable : variables) {
                OpcUaReadResult result;
                result.timestamp = now_utc_iso8601();
                result.attempts = attempt;
                result.quality = status_name(ex);
                result.status_code = result.quality;
                result.error = ex.what();
                result.data_type = variable == nullptr ? "" : variable->data_type;
                results.push_back(std::move(result));
            }
        } catch (const std::exception& ex) {
            results.clear();
            for (const auto* variable : variables) {
                OpcUaReadResult result;
                result.timestamp = now_utc_iso8601();
                result.attempts = attempt;
                result.quality = "BadUnexpectedError";
                result.status_code = "BadUnexpectedError";
                result.error = ex.what();
                result.data_type = variable == nullptr ? "" : variable->data_type;
                results.push_back(std::move(result));
            }
        }
        if (attempt < attempts_for(runtime)) {
            retry_delay(runtime);
        }
    }
#else
    for (const auto* variable : variables) {
        results.push_back(read_node(device, variable, variable == nullptr ? "" : variable->node_id, runtime));
    }
#endif
    return results;
}

DeviceStatus OpcUaClient::get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) const {
    DeviceStatus status;
    status.device_id = device.id;
    status.endpoint = device.endpoint;
    status.last_read_time = now_utc_iso8601();

    if (starts_with(device.endpoint, "mock://")) {
        status.online = true;
        status.session_state = "mock_connected";
        status.attempts = 1;
        return status;
    }

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    for (int attempt = 1; attempt <= attempts_for(runtime); ++attempt) {
        status.attempts = attempt;
        try {
            opcua::Client client = make_client(runtime);
            client.connect(device.endpoint);
            status.online = true;
            status.session_state = "connected";
            client.disconnect();
            break;
        } catch (const opcua::BadStatus& ex) {
            status.session_state = "disconnected";
            status.error = ex.what();
        } catch (const std::exception& ex) {
            status.session_state = "error";
            status.error = ex.what();
        }
        if (attempt < attempts_for(runtime)) {
            retry_delay(runtime);
        }
    }
#else
    (void)runtime;
    status.session_state = "opcua_not_compiled";
    status.error = "OPC UA support is not compiled in this build";
#endif
    return status;
}

} // namespace industrial_mcp
