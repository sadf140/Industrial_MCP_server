#include "industrial_mcp/mcp_server.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace industrial_mcp {
namespace {

constexpr const char* kProtocolVersion = "2024-11-05";

bool is_notification(const Json& request) {
    return request.is_object() && !request.contains("id");
}

Json schema_string(const std::string& description = {}) {
    Json schema = {{"type", "string"}};
    if (!description.empty()) schema["description"] = description;
    return schema;
}

Json schema_integer(const std::string& description = {}, int minimum = 0) {
    Json schema = {{"type", "integer"}, {"minimum", minimum}};
    if (!description.empty()) schema["description"] = description;
    return schema;
}

Json schema_boolean(const std::string& description = {}) {
    Json schema = {{"type", "boolean"}};
    if (!description.empty()) schema["description"] = description;
    return schema;
}

Json schema_value(const std::string& description = {}) {
    Json schema = {{"type", Json::array({"boolean", "number", "integer", "string"})}};
    if (!description.empty()) schema["description"] = description;
    return schema;
}

Json object_schema(Json properties, Json required = Json::array()) {
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

Json tool_result(const Json& payload, bool is_error = false) {
    Json result = {
        {"content", Json::array({{{"type", "text"}, {"text", payload.dump()}}})},
        {"structuredContent", payload},
    };
    if (is_error) result["isError"] = true;
    return result;
}

Json tool_error(const std::string& code, const std::string& message) {
    return tool_result({{"error", code}, {"message", message}}, true);
}

bool opcua_compiled() {
#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    return true;
#else
    return false;
#endif
}

std::string required_arg_string(const Json& args, const std::string& key) {
    if (!args.is_object() || !args.contains(key) || !args.at(key).is_string()) {
        throw std::runtime_error("missing required string argument: " + key);
    }
    return args.at(key).get<std::string>();
}

std::string optional_arg_string(const Json& args, const std::string& key) {
    if (args.is_object() && args.contains(key) && args.at(key).is_string()) {
        return args.at(key).get<std::string>();
    }
    return {};
}

std::size_t optional_arg_limit(const Json& args, std::size_t fallback) {
    if (args.is_object() && args.contains("limit") && args.at("limit").is_number_integer()) {
        const auto value = args.at("limit").get<int>();
        if (value > 0) return static_cast<std::size_t>(value);
    }
    return fallback;
}

Json status_to_json(const DeviceStatus& status) {
    return {
        {"device_id", status.device_id},
        {"online", status.online},
        {"endpoint", status.endpoint},
        {"session_state", status.session_state},
        {"last_read_time", status.last_read_time},
        {"error", status.error},
        {"attempts", status.attempts},
        {"latency_ms", status.latency_ms},
        {"disconnect_count", status.disconnect_count},
        {"consecutive_failures", status.consecutive_failures},
        {"last_success_at", status.last_success_at},
        {"last_error_at", status.last_error_at},
        {"last_error", status.last_error},
        {"read_only", true},
    };
}

Json read_result_to_json(const OpcUaReadResult& read,
                         const std::string& device_id,
                         const std::string& node_id,
                         const VariableConfig* variable) {
    Json out = {
        {"device_id", device_id},
        {"node_id", node_id},
        {"ok", read.ok},
        {"value", read.value},
        {"data_type", read.data_type},
        {"timestamp", read.timestamp},
        {"quality", read.quality},
        {"status_code", read.status_code},
        {"error", read.error},
        {"attempts", read.attempts},
    };
    if (variable != nullptr) {
        out["variable"] = variable->name;
        out["unit"] = variable->unit;
        out["description"] = variable->description;
    }
    return out;
}

Json write_result_to_json(const OpcUaWriteResult& write,
                          const std::string& device_id,
                          const VariableConfig& variable) {
    return {
        {"device_id", device_id},
        {"variable", variable.name},
        {"node_id", variable.node_id},
        {"ok", write.ok},
        {"written_value", write.value},
        {"data_type", write.data_type},
        {"timestamp", write.timestamp},
        {"quality", write.quality},
        {"status_code", write.status_code},
        {"error", write.error},
        {"error_code", write.error_code},
        {"attempts", write.attempts},
        {"latency_ms", write.latency_ms},
        {"read_only", false},
    };
}

Json variable_to_json(const VariableConfig& variable) {
    Json out = {
        {"name", variable.name},
        {"node_id", variable.node_id},
        {"data_type", variable.data_type},
        {"unit", variable.unit},
        {"description", variable.description},
        {"writable", variable.writable},
    };
    if (variable.warn_min) out["warn_min"] = *variable.warn_min;
    if (variable.warn_max) out["warn_max"] = *variable.warn_max;
    if (variable.alarm_min) out["alarm_min"] = *variable.alarm_min;
    if (variable.alarm_max) out["alarm_max"] = *variable.alarm_max;
    return out;
}

Json tool(const std::string& name,
          const std::string& description,
          Json input_schema,
          Json output_schema,
          bool read_only = true) {
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", std::move(input_schema)},
        {"outputSchema", std::move(output_schema)},
        {"annotations", {{"readOnlyHint", read_only}}},
    };
}

Json read_node_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"variable", schema_string("Configured variable name. Use this when possible.")},
            {"node_id", schema_string("Explicit OPC UA node id. Disabled unless config allows raw node ids.")},
        },
        Json::array({"device_id"})
    );
}

Json device_id_input_schema() {
    return object_schema({{"device_id", schema_string("Configured device id.")}}, Json::array({"device_id"}));
}

Json optional_device_id_input_schema() {
    return object_schema({{"device_id", schema_string("Optional configured device id.")}});
}

Json write_node_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"variable", schema_string("Configured writable variable name.")},
            {"value", schema_value("Boolean, number, integer, or string value matching the configured data_type.")},
        },
        Json::array({"device_id", "variable", "value"})
    );
}

Json alarm_query_input_schema() {
    return object_schema({
        {"device_id", schema_string("Optional configured device id.")},
        {"start_time", schema_string("Inclusive ISO-8601 UTC start time.")},
        {"end_time", schema_string("Inclusive ISO-8601 UTC end time.")},
        {"level", schema_string("Optional level filter. Compatible with severity.")},
        {"severity", schema_string("Optional severity filter.")},
        {"keyword", schema_string("Optional case-insensitive keyword.")},
        {"limit", schema_integer("Maximum records to return.", 1)},
    });
}

Json diagnose_input_schema() {
    return object_schema({
        {"device_id", schema_string("Configured device id.")},
        {"symptom", schema_string("Optional operator-observed symptom.")},
        {"start_time", schema_string("Inclusive ISO-8601 UTC start time for alarm analysis.")},
        {"end_time", schema_string("Inclusive ISO-8601 UTC end time for alarm analysis.")},
    }, Json::array({"device_id"}));
}

Json generic_output_schema() {
    return {{"type", "object"}};
}

Json make_response(const Json& id, const Json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

Json make_error_response(const Json& id, int code, const std::string& message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}},
    };
}

std::string optional_device_id(const Json& args) {
    if (args.is_object() && args.contains("device_id") && args.at("device_id").is_string()) {
        return args.at("device_id").get<std::string>();
    }
    return {};
}

bool tool_result_ok(const Json& result) {
    return !result.contains("isError") || !result.at("isError").is_boolean() || !result.at("isError").get<bool>();
}

std::string tool_error_code(const Json& result) {
    if (!result.contains("structuredContent") || !result.at("structuredContent").is_object()) {
        return {};
    }
    const auto& content = result.at("structuredContent");
    if (content.contains("error") && content.at("error").is_string()) {
        return content.at("error").get<std::string>();
    }
    return {};
}

bool tool_call_read_only(const std::string& tool_name) {
    return tool_name != "write_node";
}

} // namespace

McpServer::McpServer(AppConfig config)
    : config_(std::move(config)),
      alarms_(config_.alarm_log_path),
      audit_(config_.audit.log_path),
      state_cache_(config_, opcua_, alarms_),
      started_at_(std::chrono::steady_clock::now()),
      started_at_utc_(now_utc_iso8601()) {
    state_cache_.start();
}

McpServer::~McpServer() {
    state_cache_.stop();
}

void McpServer::run(std::istream& input, std::ostream& output) {
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        try {
            const auto message = Json::parse(line);
            const auto response = handle_message(message);
            if (response.has_value()) {
                output << response->dump() << '\n';
                output.flush();
            }
        } catch (const Json::parse_error& ex) {
            output << make_error_response(nullptr, -32700, ex.what()).dump() << '\n';
            output.flush();
        } catch (const std::exception& ex) {
            output << make_error_response(nullptr, -32603, ex.what()).dump() << '\n';
            output.flush();
        }
    }
}

std::optional<Json> McpServer::handle_message(const Json& request) {
    if (!request.is_object()) {
        return make_error_response(nullptr, -32600, "invalid request");
    }

    const bool notification = is_notification(request);
    const Json id = request.contains("id") ? request.at("id") : Json(nullptr);

    try {
        const auto method = required_arg_string(request, "method");

        if (notification) {
            if (method == "notifications/initialized") {
                return std::nullopt;
            }
            return std::nullopt;
        }

        if (method == "initialize") {
            Json result = {
                {"protocolVersion", kProtocolVersion},
                {"serverInfo", {{"name", config_.server.name}, {"version", config_.server.version}}},
                {"capabilities", {{"tools", Json::object()}}},
                {"instructions", "Read-only industrial MCP server for OPC UA data, alarm analysis, and diagnostic advice."},
            };
            return make_response(id, result);
        }

        if (method == "ping") {
            return make_response(id, Json::object());
        }

        if (method == "tools/list") {
            return make_response(id, list_tools());
        }

        if (method == "tools/call") {
            if (!request.contains("params")) {
                throw std::runtime_error("tools/call requires params");
            }
            const auto& params = request.at("params");
            const auto started = std::chrono::steady_clock::now();
            Json result = call_tool(params);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started
            ).count();

            const std::string tool_name = params.is_object() && params.contains("name") && params.at("name").is_string()
                ? params.at("name").get<std::string>()
                : "";
            const Json args = params.is_object() && params.contains("arguments") ? params.at("arguments") : Json::object();
            audit_.record_tool_call(
                tool_name,
                optional_device_id(args),
                tool_result_ok(result),
                elapsed,
                tool_error_code(result),
                args,
                tool_call_read_only(tool_name)
            );
            return make_response(id, result);
        }

        return make_error_response(id, -32601, "method not found: " + method);
    } catch (const std::exception& ex) {
        return make_error_response(id, -32602, ex.what());
    }
}

Json McpServer::list_tools() const {
    Json tools = Json::array();
    tools.push_back(tool("get_gateway_health", "Return read-only gateway health, configuration, and runtime metadata.", object_schema(Json::object()), generic_output_schema()));
    tools.push_back(tool("read_node", "Read one configured OPC UA node or variable from a device.", read_node_input_schema(), generic_output_schema()));
    tools.push_back(tool("read_opcua_node", "Read one configured OPC UA node or variable from a device.", read_node_input_schema(), generic_output_schema()));
    tools.push_back(tool("write_node", "Write one whitelisted configured OPC UA variable. Disabled unless server and config explicitly allow writes.", write_node_input_schema(), generic_output_schema(), false));
    tools.push_back(tool("list_devices", "List configured industrial devices and their OPC UA variables without opening network sessions.", object_schema(Json::object()), generic_output_schema()));
    tools.push_back(tool("read_device_snapshot", "Read all configured variables for a device.", device_id_input_schema(), generic_output_schema()));
    tools.push_back(tool("get_device_state", "Return cached device state collected by the gateway without directly reading OPC UA for this call.", optional_device_id_input_schema(), generic_output_schema()));
    tools.push_back(tool("get_device_status", "Return read-only device and OPC UA session status.", device_id_input_schema(), generic_output_schema()));
    tools.push_back(tool("get_network_status", "Return per-device OPC UA communication status, latency, and disconnect counters.", optional_device_id_input_schema(), generic_output_schema()));
    tools.push_back(tool("get_alarm_history", "Query alarm log records by device, time range, severity, or keyword.", alarm_query_input_schema(), generic_output_schema()));
    tools.push_back(tool("query_alarm_logs", "Query alarm log records by device, time range, severity, or keyword.", alarm_query_input_schema(), generic_output_schema()));
    tools.push_back(tool("analyze_alarms", "Summarize alarm counts, frequent codes, and timeline.", alarm_query_input_schema(), generic_output_schema()));
    tools.push_back(tool("diagnose_fault", "Run rule-based fault diagnosis with evidence and limitations.", diagnose_input_schema(), generic_output_schema()));
    return {{"tools", tools}};
}

Json McpServer::call_tool(const Json& params) {
    const auto name = required_arg_string(params, "name");
    const Json args = params.contains("arguments") ? params.at("arguments") : Json::object();

    try {
        if (name == "get_gateway_health") {
            return tool_result(gateway_health());
        }

        if (name == "get_device_status") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            return tool_result(status_to_json(opcua_.get_status(*device, config_.opcua)));
        }

        if (name == "list_devices") {
            Json devices = Json::array();
            for (const auto& device : config_.devices) {
                Json variables = Json::array();
                for (const auto& [_, variable] : device.variables) {
                    variables.push_back(variable_to_json(variable));
                }
                devices.push_back({
                    {"id", device.id},
                    {"name", device.name},
                    {"endpoint", device.endpoint},
                    {"endpoint_type", device.endpoint.rfind("mock://", 0) == 0 ? "mock" : "opcua"},
                    {"variable_count", static_cast<int>(device.variables.size())},
                    {"variables", variables},
                });
            }
            return tool_result({
                {"count", static_cast<int>(config_.devices.size())},
                {"devices", devices},
                {"read_only", true},
            });
        }

        if (name == "get_network_status") {
            Json devices = Json::array();
            const auto device_id = optional_arg_string(args, "device_id");
            if (!device_id.empty()) {
                const auto* device = find_device(config_, device_id);
                if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
                devices.push_back(status_to_json(opcua_.get_status(*device, config_.opcua)));
            } else {
                for (const auto& device : config_.devices) {
                    devices.push_back(status_to_json(opcua_.get_status(device, config_.opcua)));
                }
            }
            return tool_result({
                {"timestamp", now_utc_iso8601()},
                {"count", static_cast<int>(devices.size())},
                {"devices", devices},
                {"read_only", true},
            });
        }

        if (name == "get_device_state") {
            return tool_result(state_cache_.state_json(optional_arg_string(args, "device_id")));
        }

        if (name == "read_node" || name == "read_opcua_node") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);

            const auto variable_name = optional_arg_string(args, "variable");
            const auto explicit_node_id = optional_arg_string(args, "node_id");
            const VariableConfig* variable = nullptr;
            std::string node_id = explicit_node_id;

            if (!variable_name.empty()) {
                variable = find_variable(*device, variable_name);
                if (variable == nullptr) return tool_error("VARIABLE_NOT_FOUND", "unknown variable: " + variable_name);
                node_id = variable->node_id;
            } else if (!node_id.empty()) {
                if (!config_.opcua.allow_raw_node_id) {
                    return tool_error("RAW_NODE_ID_DISABLED", "explicit node_id reads are disabled by configuration");
                }
                variable = find_variable(*device, node_id);
            } else {
                return tool_error("INVALID_ARGUMENT", "read_node requires variable or node_id");
            }

            return tool_result(read_result_to_json(opcua_.read_node(*device, variable, node_id, config_.opcua), device_id, node_id, variable));
        }

        if (name == "write_node") {
            if (config_.server.read_only) {
                return tool_error("WRITE_DISABLED", "server.read_only must be false before write_node can be used");
            }
            if (!config_.opcua.write_enabled) {
                return tool_error("WRITE_DISABLED", "opcua.write_enabled must be true before write_node can be used");
            }
            const auto device_id = required_arg_string(args, "device_id");
            const auto variable_name = required_arg_string(args, "variable");
            if (!args.contains("value")) {
                return tool_error("INVALID_ARGUMENT", "write_node requires value");
            }

            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            const auto* variable = find_variable(*device, variable_name);
            if (variable == nullptr) return tool_error("VARIABLE_NOT_FOUND", "unknown variable: " + variable_name);
            if (!variable->writable) {
                return tool_error("WRITE_NOT_ALLOWED", "variable is not marked writable in configuration: " + variable_name);
            }

            const auto write = opcua_.write_node(*device, *variable, args.at("value"), config_.opcua);
            auto payload = write_result_to_json(write, device_id, *variable);
            if (!write.ok) {
                if (!payload.contains("error_code") || !payload.at("error_code").is_string() || payload.at("error_code").get<std::string>().empty()) {
                    payload["error_code"] = "OPCUA_WRITE_FAILED";
                }
                payload["error"] = payload.at("error").get<std::string>().empty()
                    ? "write failed"
                    : payload.at("error").get<std::string>();
                return tool_result(payload, true);
            }
            return tool_result(payload);
        }

        if (name == "read_device_snapshot") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);

            Json variables = Json::array();
            std::vector<const VariableConfig*> variable_refs;
            variable_refs.reserve(device->variables.size());
            for (const auto& [_, variable] : device->variables) {
                variable_refs.push_back(&variable);
            }

            const auto reads = opcua_.read_nodes(*device, variable_refs, config_.opcua);
            for (std::size_t index = 0; index < variable_refs.size(); ++index) {
                const auto* variable = variable_refs[index];
                const auto& read = reads.at(index);
                variables.push_back(read_result_to_json(read, device_id, variable->node_id, variable));
            }
            return tool_result({
                {"device_id", device_id},
                {"timestamp", now_utc_iso8601()},
                {"variables", variables},
                {"ok", std::all_of(variables.begin(), variables.end(), [](const Json& item) {
                    return item.contains("ok") && item.at("ok").is_boolean() && item.at("ok").get<bool>();
                })},
                {"read_only", true},
            });
        }

        if (name == "get_alarm_history" || name == "query_alarm_logs" || name == "analyze_alarms") {
            AlarmQuery query;
            query.device_id = optional_arg_string(args, "device_id");
            query.start_time = optional_arg_string(args, "start_time");
            query.end_time = optional_arg_string(args, "end_time");
            query.level = optional_arg_string(args, "level");
            query.severity = optional_arg_string(args, "severity");
            query.keyword = optional_arg_string(args, "keyword");
            query.limit = optional_arg_limit(args, 100);
            return tool_result(name == "analyze_alarms" ? alarms_.analyze_json(query) : alarms_.query_json(query));
        }

        if (name == "diagnose_fault") {
            return tool_result(diagnostics_.diagnose(config_, alarms_, state_cache_, args));
        }

        return tool_error("UNKNOWN_TOOL", "unknown tool: " + name);
    } catch (const std::exception& ex) {
        return tool_error("TOOL_EXECUTION_FAILED", ex.what());
    }
}

Json McpServer::gateway_health() const {
    std::size_t variable_count = 0;
    int mock_devices = 0;
    int opcua_devices = 0;
    Json devices = Json::array();

    for (const auto& device : config_.devices) {
        variable_count += device.variables.size();
        const bool mock = device.endpoint.rfind("mock://", 0) == 0;
        if (mock) {
            ++mock_devices;
        } else {
            ++opcua_devices;
        }
        devices.push_back({
            {"id", device.id},
            {"name", device.name},
            {"endpoint_type", mock ? "mock" : "opcua"},
            {"variable_count", static_cast<int>(device.variables.size())},
        });
    }

    const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at_
    ).count();

    return {
        {"ok", true},
        {"timestamp", now_utc_iso8601()},
        {"started_at", started_at_utc_},
        {"uptime_ms", uptime_ms},
        {"server", {{"name", config_.server.name}, {"version", config_.server.version}, {"read_only", config_.server.read_only}}},
        {"mcp", {{"transport", "stdio"}, {"protocol_version", kProtocolVersion}}},
        {"opcua", {
            {"compiled", opcua_compiled()},
            {"connect_timeout_ms", config_.opcua.connect_timeout_ms},
            {"read_timeout_ms", config_.opcua.read_timeout_ms},
            {"retry_count", config_.opcua.retry_count},
            {"retry_delay_ms", config_.opcua.retry_delay_ms},
            {"allow_raw_node_id", config_.opcua.allow_raw_node_id},
            {"write_enabled", config_.opcua.write_enabled},
        }},
        {"audit", {{"enabled", !config_.audit.log_path.empty()}, {"log_path_configured", !config_.audit.log_path.empty()}}},
        {"alarms", {{"log_path_configured", !config_.alarm_log_path.empty()}}},
        {"cache", {
            {"enabled", config_.cache.enabled},
            {"poll_interval_ms", config_.cache.poll_interval_ms},
            {"stale_after_ms", config_.cache.stale_after_ms},
        }},
        {"configuration", {
            {"device_count", static_cast<int>(config_.devices.size())},
            {"variable_count", static_cast<int>(variable_count)},
            {"mock_device_count", mock_devices},
            {"opcua_device_count", opcua_devices},
            {"devices", devices},
        }},
        {"read_only", true},
    };
}

} // namespace industrial_mcp
