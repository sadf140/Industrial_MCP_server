#include "industrial_mcp/mcp/mcp_server.hpp"

#include "industrial_mcp/storage/storage_backend.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace industrial_mcp {
namespace {

constexpr const char* kProtocolVersion = "2024-11-05";

enum class ToolRiskLevel {
    ReadOnly,
    LowRiskWrite,
    HighRiskWrite,
    Administrative,
};

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

std::string risk_to_string(ToolRiskLevel risk) {
    switch (risk) {
        case ToolRiskLevel::ReadOnly:
            return "ReadOnly";
        case ToolRiskLevel::LowRiskWrite:
            return "LowRiskWrite";
        case ToolRiskLevel::HighRiskWrite:
            return "HighRiskWrite";
        case ToolRiskLevel::Administrative:
            return "Administrative";
    }
    return "ReadOnly";
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

std::string optional_object_string(const Json& object, const std::string& key) {
    if (object.is_object() && object.contains(key) && object.at(key).is_string()) {
        return object.at(key).get<std::string>();
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

bool optional_arg_bool(const Json& args, const std::string& key, bool fallback) {
    if (args.is_object() && args.contains(key) && args.at(key).is_boolean()) {
        return args.at(key).get<bool>();
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

Json method_result_to_json(const OpcUaMethodResult& call,
                           const std::string& device_id,
                           const MethodConfig& method) {
    return {
        {"ok", call.ok},
        {"device_id", device_id},
        {"method", method.name},
        {"object_id", method.object_id},
        {"method_id", method.method_id},
        {"output_arguments", call.output_arguments},
        {"timestamp", call.timestamp},
        {"quality", call.quality},
        {"status_code", call.status_code},
        {"error", call.error},
        {"error_code", call.error_code},
        {"latency_ms", call.latency_ms},
        {"attempts", call.attempts},
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
    if (variable.write_min) out["min"] = *variable.write_min;
    if (variable.write_max) out["max"] = *variable.write_max;
    if (!variable.allowed_values.empty()) out["allowed_values"] = variable.allowed_values;
    return out;
}

Json method_to_json(const MethodConfig& method) {
    return {
        {"name", method.name},
        {"object_id", method.object_id},
        {"method_id", method.method_id},
        {"description", method.description},
        {"enabled", method.enabled},
        {"requires_confirmation", method.requires_confirmation},
        {"input_types", method.input_types},
    };
}

Json device_to_json(const DeviceConfig& device) {
    Json variables = Json::array();
    for (const auto& [_, variable] : device.variables) {
        variables.push_back(variable_to_json(variable));
    }
    Json methods = Json::array();
    for (const auto& [_, method] : device.methods) {
        methods.push_back(method_to_json(method));
    }
    return {
        {"id", device.id},
        {"name", device.name},
        {"protocol", device.protocol},
        {"enabled", device.enabled},
        {"endpoint", device.endpoint},
        {"endpoint_type", device.endpoint.rfind("mock://", 0) == 0 ? "mock" : "opcua"},
        {"variable_count", static_cast<int>(device.variables.size())},
        {"method_count", static_cast<int>(device.methods.size())},
        {"variables", variables},
        {"methods", methods},
    };
}

Json tool(const std::string& name,
          const std::string& description,
          Json input_schema,
          Json output_schema,
          bool read_only = true,
          ToolRiskLevel risk = ToolRiskLevel::ReadOnly) {
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", std::move(input_schema)},
        {"outputSchema", std::move(output_schema)},
        {"annotations", {{"readOnlyHint", read_only}, {"riskLevel", risk_to_string(risk)}}},
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

Json refresh_device_state_input_schema() {
    return object_schema({{"device_id", schema_string("Optional configured device id.")}});
}

Json acknowledge_alarm_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"alarm_id", schema_string("Alarm id to acknowledge.")},
            {"message", schema_string("Optional acknowledgement message.")},
        },
        Json::array({"device_id"})
    );
}

Json clear_cached_alarm_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"variable", schema_string("Optional variable name. Empty clears device communication alarm state.")},
        },
        Json::array({"device_id"})
    );
}

Json prepare_device_action_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"action", schema_string("Action name, for example write_node or stop.")},
            {"variable", schema_string("Optional variable for write actions.")},
            {"method", schema_string("Optional configured method name for call_device_method actions.")},
            {"value", schema_value("Optional value for write actions.")},
        },
        Json::array({"device_id", "action"})
    );
}

Json operation_id_input_schema() {
    return object_schema({{"operation_id", schema_string("Prepared operation id.")}}, Json::array({"operation_id"}));
}

Json call_device_method_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"method", schema_string("Configured method name.")},
            {"arguments", {{"type", "array"}}},
            {"operation_id", schema_string("Confirmed operation id for high-risk methods.")},
        },
        Json::array({"device_id", "method"})
    );
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

Json add_device_input_schema() {
    return object_schema({{"device", {{"type", "object"}}}}, Json::array({"device"}));
}

Json update_alarm_rule_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"variable", schema_string("Configured variable name.")},
            {"warn_min", {{"type", Json::array({"number", "null"})}}},
            {"warn_max", {{"type", Json::array({"number", "null"})}}},
            {"alarm_min", {{"type", Json::array({"number", "null"})}}},
            {"alarm_max", {{"type", Json::array({"number", "null"})}}},
        },
        Json::array({"device_id", "variable"})
    );
}

Json remove_device_input_schema() {
    return object_schema(
        {
            {"device_id", schema_string("Configured device id.")},
            {"force", schema_boolean("Remove even if the device is enabled.")},
        },
        Json::array({"device_id"})
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
    return tool_name != "write_node" &&
           tool_name != "call_device_method" &&
           tool_name != "acknowledge_alarm" &&
           tool_name != "clear_cached_alarm" &&
           tool_name != "refresh_device_state" &&
           tool_name != "prepare_device_action" &&
           tool_name != "confirm_device_action" &&
           tool_name != "cancel_device_action" &&
           tool_name != "add_device" &&
           tool_name != "remove_device" &&
           tool_name != "enable_device" &&
           tool_name != "disable_device" &&
           tool_name != "reload_configuration" &&
           tool_name != "update_alarm_rule";
}

ToolRiskLevel tool_risk(const std::string& tool_name) {
    if (tool_name == "add_device" || tool_name == "remove_device" || tool_name == "enable_device" ||
        tool_name == "disable_device" || tool_name == "reload_configuration" || tool_name == "update_alarm_rule") {
        return ToolRiskLevel::Administrative;
    }
    if (tool_name == "write_node" || tool_name == "call_device_method" || tool_name == "prepare_device_action" || tool_name == "confirm_device_action") {
        return ToolRiskLevel::HighRiskWrite;
    }
    if (tool_name == "acknowledge_alarm" || tool_name == "clear_cached_alarm" || tool_name == "refresh_device_state" || tool_name == "cancel_device_action") {
        return ToolRiskLevel::LowRiskWrite;
    }
    return ToolRiskLevel::ReadOnly;
}

bool has_tool_permission(const AppConfig& config, const std::string& role, const std::string& tool_name) {
    if (!config.security.enabled) return true;
    const auto found = config.security.roles.find(role.empty() ? config.security.default_role : role);
    if (found == config.security.roles.end()) return false;
    return std::find(found->second.begin(), found->second.end(), "*") != found->second.end() ||
           std::find(found->second.begin(), found->second.end(), tool_name) != found->second.end();
}

bool is_known_tool(const std::string& tool_name) {
    static const std::set<std::string> tools = {
        "get_gateway_health",
        "get_server_health",
        "read_node",
        "read_opcua_node",
        "write_node",
        "list_devices",
        "read_device_snapshot",
        "get_device_state",
        "refresh_device_state",
        "get_device_health",
        "get_device_status",
        "get_network_status",
        "get_alarm_history",
        "query_alarm_logs",
        "analyze_alarms",
        "acknowledge_alarm",
        "clear_cached_alarm",
        "prepare_device_action",
        "confirm_device_action",
        "cancel_device_action",
        "call_device_method",
        "add_device",
        "remove_device",
        "enable_device",
        "disable_device",
        "reload_configuration",
        "update_alarm_rule",
        "diagnose_fault",
    };
    return tools.find(tool_name) != tools.end();
}

std::string call_context_string(const Json& params, const Json& args, const std::string& key, const std::string& fallback) {
    if (params.is_object()) {
        if (params.contains(key) && params.at(key).is_string()) return params.at(key).get<std::string>();
        for (const auto& context_key : {"context", "_meta"}) {
            if (params.contains(context_key) && params.at(context_key).is_object()) {
                const auto value = optional_object_string(params.at(context_key), key);
                if (!value.empty()) return value;
            }
        }
    }
    const auto value = optional_arg_string(args, key);
    return value.empty() ? fallback : value;
}

std::string request_id_string(const Json& id) {
    if (id.is_string()) return id.get<std::string>();
    if (id.is_number_integer()) return std::to_string(id.get<long long>());
    if (id.is_number_unsigned()) return std::to_string(id.get<unsigned long long>());
    if (id.is_null()) return {};
    return id.dump();
}

void refresh_content_text(Json& result) {
    if (!result.is_object() || !result.contains("structuredContent")) return;
    if (!result.contains("content") || !result.at("content").is_array() || result.at("content").empty()) return;
    result["content"][0]["text"] = result.at("structuredContent").dump();
}

Json add_execution_context(Json result,
                           const std::string& request_id,
                           const std::string& tool_name,
                           long long elapsed_ms,
                           const std::string& data_source) {
    if (result.is_object() && result.contains("structuredContent") && result.at("structuredContent").is_object()) {
        auto& content = result["structuredContent"];
        content["request_id"] = request_id;
        content["tool_name"] = tool_name;
        content["duration_ms"] = elapsed_ms;
        content["data_source"] = data_source;
        if (!content.contains("cache")) {
            content["cache"] = Json::object();
        }
        refresh_content_text(result);
    }
    return result;
}

std::string data_source_for_tool(const std::string& tool_name) {
    if (tool_name == "get_device_state" || tool_name == "diagnose_fault") return "cache";
    if (tool_name == "list_devices" || tool_name == "get_gateway_health" || tool_name == "get_server_health") return "configuration";
    if (tool_name == "get_alarm_history" || tool_name == "query_alarm_logs" || tool_name == "analyze_alarms" || tool_name == "acknowledge_alarm") return "alarm_store";
    if (tool_name == "read_node" || tool_name == "read_opcua_node" || tool_name == "read_device_snapshot" || tool_name == "write_node" || tool_name == "call_device_method" ||
        tool_name == "get_device_status" || tool_name == "get_device_health" || tool_name == "get_network_status" || tool_name == "refresh_device_state") {
        return "connection_manager";
    }
    if (tool_name == "add_device" || tool_name == "remove_device" || tool_name == "enable_device" ||
        tool_name == "disable_device" || tool_name == "reload_configuration" || tool_name == "update_alarm_rule") {
        return "runtime_configuration";
    }
    return "server";
}

bool json_values_equal(const Json& lhs, const Json& rhs) {
    return lhs == rhs;
}

std::string validate_write_constraints(const VariableConfig& variable, const Json& value) {
    if (!variable.allowed_values.empty()) {
        const bool allowed = std::any_of(variable.allowed_values.begin(), variable.allowed_values.end(), [&](const Json& allowed_value) {
            return json_values_equal(allowed_value, value);
        });
        if (!allowed) return "value is not in allowed_values for variable: " + variable.name;
    }
    if ((variable.write_min || variable.write_max) && !value.is_number()) {
        return "numeric value is required for min/max constrained variable: " + variable.name;
    }
    if (value.is_number()) {
        const auto parsed = value.get<double>();
        if (variable.write_min && parsed < *variable.write_min) return "value is below configured min for variable: " + variable.name;
        if (variable.write_max && parsed > *variable.write_max) return "value is above configured max for variable: " + variable.name;
    }
    return {};
}

std::string validate_method_arguments(const MethodConfig& method, const Json& arguments) {
    if (!arguments.is_array()) return "arguments must be an array";
    if (arguments.size() != method.input_types.size()) return "argument count does not match input_types";
    for (std::size_t index = 0; index < method.input_types.size(); ++index) {
        const auto type = method.input_types.at(index);
        const auto& value = arguments.at(index);
        if ((type == "Boolean" || type == "Bool") && !value.is_boolean()) return "argument " + std::to_string(index) + " must be Boolean";
        if ((type == "Double" || type == "Float") && !value.is_number()) return "argument " + std::to_string(index) + " must be numeric";
        if ((type == "Int32" || type == "UInt32" || type == "Int16" || type == "UInt16") && !value.is_number_integer() && !value.is_number_unsigned()) {
            return "argument " + std::to_string(index) + " must be integer";
        }
        if (type == "String" && !value.is_string()) return "argument " + std::to_string(index) + " must be String";
    }
    return {};
}

DeviceConfig parse_device_from_argument(const Json& device_json) {
    Json root = {
        {"devices", Json::array({device_json})},
    };
    auto parsed = ConfigLoader::load_json(root, ".");
    if (parsed.devices.empty()) {
        throw std::runtime_error("device argument did not produce a device");
    }
    return std::move(parsed.devices.front());
}

void apply_optional_threshold(const Json& args, const std::string& key, std::optional<double>& target) {
    if (!args.contains(key)) return;
    if (args.at(key).is_null()) {
        target.reset();
        return;
    }
    if (!args.at(key).is_number()) {
        throw std::runtime_error(key + " must be a number or null");
    }
    target = args.at(key).get<double>();
}

} // namespace

McpServer::McpServer(AppConfig config)
    : config_(std::move(config)),
      connections_(config_),
      alarms_(config_.alarm_log_path, config_.storage, config_.audit.log_path),
      audit_(config_.audit.log_path, config_.storage, config_.alarm_log_path),
      state_cache_(config_, connections_, alarms_),
      started_at_(std::chrono::steady_clock::now()),
      started_at_utc_(now_utc_iso8601()) {
    state_cache_.start();
    health_http_.start(config_.http.host, config_.http.port, [this](const std::string& path) {
        return handle_http_request(path);
    });
    if (config_.observability.metrics_enabled && config_.observability.metrics_port > 0 &&
        config_.observability.metrics_port != config_.http.port) {
        metrics_http_.start(config_.http.host, config_.observability.metrics_port, [this](const std::string& path) {
            return handle_http_request(path);
        });
    }
}

McpServer::~McpServer() {
    metrics_http_.stop();
    health_http_.stop();
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
            if (method == "notifications/initialized" || method == "notifications/tools/list_changed") {
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
            const auto params = request.contains("params") ? request.at("params") : Json::object();
            return make_response(id, list_tools(params));
        }

        if (method == "tools/call") {
            if (!request.contains("params")) {
                throw std::runtime_error("tools/call requires params");
            }
            const auto& params = request.at("params");
            const auto started = std::chrono::steady_clock::now();
            const std::string tool_name = params.is_object() && params.contains("name") && params.at("name").is_string()
                ? params.at("name").get<std::string>()
                : "";
            const Json args = params.is_object() && params.contains("arguments") ? params.at("arguments") : Json::object();
            const auto request_id = request_id_string(id);
            Json result = call_tool(params);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started
            ).count();
            result = add_execution_context(std::move(result), request_id, tool_name, elapsed, data_source_for_tool(tool_name));

            AuditRecord audit_record;
            audit_record.request_id = request_id;
            audit_record.user_id = call_context_string(params, args, "user_id", "anonymous");
            audit_record.client_id = call_context_string(params, args, "client_id", "mcp-client");
            audit_record.tool_name = tool_name;
            audit_record.device_id = optional_device_id(args);
            audit_record.target_node = optional_arg_string(args, "variable");
            if (audit_record.target_node.empty()) audit_record.target_node = optional_arg_string(args, "method");
            audit_record.result = tool_result_ok(result) ? "ok" : "error";
            audit_record.elapsed_ms = elapsed;
            audit_record.error_code = tool_error_code(result);
            audit_record.arguments = args;
            audit_record.read_only = tool_call_read_only(tool_name);
            if (args.is_object() && args.contains("value")) audit_record.new_value = args.at("value");
            if (result.is_object() && result.contains("structuredContent") && result.at("structuredContent").is_object()) {
                const auto& content = result.at("structuredContent");
                if (content.contains("old_device")) audit_record.old_value = content.at("old_device");
                if (content.contains("old_variable")) audit_record.old_value = content.at("old_variable");
                if (content.contains("removed_device")) audit_record.old_value = content.at("removed_device");
                if (content.contains("new_device")) audit_record.new_value = content.at("new_device");
                if (content.contains("new_variable")) audit_record.new_value = content.at("new_variable");
                if (content.contains("device")) audit_record.new_value = content.at("device");
                if (audit_record.device_id.empty() && content.contains("device_id") && content.at("device_id").is_string()) {
                    audit_record.device_id = content.at("device_id").get<std::string>();
                }
                if (audit_record.device_id.empty() && content.contains("device") && content.at("device").is_object()) {
                    audit_record.device_id = optional_object_string(content.at("device"), "id");
                }
                if (audit_record.device_id.empty() && content.contains("removed_device") && content.at("removed_device").is_object()) {
                    audit_record.device_id = optional_object_string(content.at("removed_device"), "id");
                }
                if (audit_record.target_node.empty() && content.contains("method_id") && content.at("method_id").is_string()) {
                    audit_record.target_node = content.at("method_id").get<std::string>();
                }
            }
            audit_.record(audit_record);
            const bool ok = tool_result_ok(result);
            metrics_.record_tool_call(tool_name, ok, elapsed);
            if (tool_name == "acknowledge_alarm" && ok) {
                metrics_.record_alarm_event();
            }
            emit_structured_log(ok ? "info" : "warn", "tool_call", {
                {"request_id", audit_record.request_id},
                {"user_id", audit_record.user_id},
                {"client_id", audit_record.client_id},
                {"tool_name", audit_record.tool_name},
                {"device_id", audit_record.device_id},
                {"duration_ms", audit_record.elapsed_ms},
                {"result", audit_record.result},
                {"error_code", audit_record.error_code},
            });
            return make_response(id, result);
        }

        return make_error_response(id, -32601, "method not found: " + method);
    } catch (const std::exception& ex) {
        return make_error_response(id, -32602, ex.what());
    }
}

Json McpServer::list_tools(const Json& params) const {
    Json tools = Json::array();
    const auto role = call_context_string(params, Json::object(), "role", config_.security.default_role);
    auto add_tool = [&](Json item) {
        const auto name = item.at("name").get<std::string>();
        if (!config_.security.hide_unauthorized_tools || has_tool_permission(config_, role, name)) {
            tools.push_back(std::move(item));
        }
    };
    add_tool(tool("get_gateway_health", "Return read-only gateway health, configuration, and runtime metadata.", object_schema(Json::object()), generic_output_schema()));
    add_tool(tool("get_server_health", "Return MCP server health, readiness, configuration, and runtime metadata.", object_schema(Json::object()), generic_output_schema()));
    add_tool(tool("read_node", "Read one configured OPC UA node or variable from a device.", read_node_input_schema(), generic_output_schema()));
    add_tool(tool("read_opcua_node", "Read one configured OPC UA node or variable from a device.", read_node_input_schema(), generic_output_schema()));
    add_tool(tool("write_node", "Write one whitelisted configured OPC UA variable. Disabled unless server, RBAC, and config explicitly allow writes.", write_node_input_schema(), generic_output_schema(), false, ToolRiskLevel::HighRiskWrite));
    add_tool(tool("call_device_method", "Call one whitelisted OPC UA Method for a configured device.", call_device_method_input_schema(), generic_output_schema(), false, ToolRiskLevel::HighRiskWrite));
    add_tool(tool("list_devices", "List configured industrial devices and their OPC UA variables without opening network sessions.", object_schema(Json::object()), generic_output_schema()));
    add_tool(tool("read_device_snapshot", "Read all configured variables for a device.", device_id_input_schema(), generic_output_schema()));
    add_tool(tool("get_device_state", "Return cached device state collected by the gateway without directly reading OPC UA for this call.", optional_device_id_input_schema(), generic_output_schema()));
    add_tool(tool("refresh_device_state", "Force a cache refresh and return cached device state.", refresh_device_state_input_schema(), generic_output_schema(), false, ToolRiskLevel::LowRiskWrite));
    add_tool(tool("get_device_health", "Return one configured device health, communication status, and cache state.", device_id_input_schema(), generic_output_schema()));
    add_tool(tool("get_device_status", "Return read-only device and OPC UA session status.", device_id_input_schema(), generic_output_schema()));
    add_tool(tool("get_network_status", "Return per-device OPC UA communication status, latency, and disconnect counters.", optional_device_id_input_schema(), generic_output_schema()));
    add_tool(tool("get_alarm_history", "Query alarm log records by device, time range, severity, or keyword.", alarm_query_input_schema(), generic_output_schema()));
    add_tool(tool("query_alarm_logs", "Query alarm log records by device, time range, severity, or keyword.", alarm_query_input_schema(), generic_output_schema()));
    add_tool(tool("analyze_alarms", "Summarize alarm counts, frequent codes, and timeline.", alarm_query_input_schema(), generic_output_schema()));
    add_tool(tool("acknowledge_alarm", "Append an alarm acknowledgement record.", acknowledge_alarm_input_schema(), generic_output_schema(), false, ToolRiskLevel::LowRiskWrite));
    add_tool(tool("clear_cached_alarm", "Clear in-memory duplicate-suppression state for one cached alarm.", clear_cached_alarm_input_schema(), generic_output_schema(), false, ToolRiskLevel::LowRiskWrite));
    add_tool(tool("prepare_device_action", "Prepare a high-risk device operation and return a short-lived operation_id.", prepare_device_action_input_schema(), generic_output_schema(), false, ToolRiskLevel::HighRiskWrite));
    add_tool(tool("confirm_device_action", "Confirm a prepared high-risk device operation.", operation_id_input_schema(), generic_output_schema(), false, ToolRiskLevel::HighRiskWrite));
    add_tool(tool("cancel_device_action", "Cancel a prepared high-risk device operation.", operation_id_input_schema(), generic_output_schema(), false, ToolRiskLevel::LowRiskWrite));
    add_tool(tool("add_device", "Add one device to the in-memory runtime configuration.", add_device_input_schema(), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("remove_device", "Remove one device from the in-memory runtime configuration.", remove_device_input_schema(), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("enable_device", "Enable one configured device in memory.", device_id_input_schema(), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("disable_device", "Disable one configured device in memory.", device_id_input_schema(), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("reload_configuration", "Reload the original startup configuration file and discard in-memory management changes.", object_schema(Json::object()), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("update_alarm_rule", "Update threshold alarm rules for one configured variable in memory.", update_alarm_rule_input_schema(), generic_output_schema(), false, ToolRiskLevel::Administrative));
    add_tool(tool("diagnose_fault", "Run rule-based fault diagnosis with evidence and limitations.", diagnose_input_schema(), generic_output_schema()));
    return {{"tools", tools}};
}

Json McpServer::call_tool(const Json& params) {
    const auto name = required_arg_string(params, "name");
    const Json args = params.contains("arguments") ? params.at("arguments") : Json::object();
    if (!is_known_tool(name)) {
        return tool_error("UNKNOWN_TOOL", "unknown tool: " + name);
    }
    const auto role = call_context_string(params, args, "role", config_.security.default_role);
    if (!has_tool_permission(config_, role, name)) {
        return tool_error("PERMISSION_DENIED", "role is not allowed to call tool: " + role + " -> " + name);
    }

    try {
        if (name == "get_gateway_health" || name == "get_server_health") {
            return tool_result(gateway_health());
        }

        if (name == "get_device_health") {
            const auto device_id = required_arg_string(args, "device_id");
            return tool_result(device_health(device_id));
        }

        if (name == "get_device_status") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            return tool_result(device_connection_health_to_json(connections_.get_health(*device, config_.opcua)));
        }

        if (name == "list_devices") {
            Json devices = Json::array();
            for (const auto& device : config_.devices) {
                auto device_json = device_to_json(device);
                device_json["connection"] = connections_.health_json(device);
                devices.push_back(std::move(device_json));
            }
            return tool_result({
                {"count", static_cast<int>(config_.devices.size())},
                {"devices", devices},
                {"read_only", true},
            });
        }

        if (name == "add_device") {
            if (!args.contains("device") || !args.at("device").is_object()) return tool_error("INVALID_ARGUMENT", "add_device requires device object");
            auto device = parse_device_from_argument(args.at("device"));
            if (find_device(config_, device.id) != nullptr) return tool_error("DEVICE_ALREADY_EXISTS", "device already exists: " + device.id);
            const auto device_json = device_to_json(device);
            state_cache_.stop();
            config_.devices.push_back(std::move(device));
            reset_runtime_after_config_change();
            state_cache_.start();
            return tool_result({{"ok", true}, {"device", device_json}, {"read_only", false}});
        }

        if (name == "remove_device") {
            const auto device_id = required_arg_string(args, "device_id");
            const bool force = optional_arg_bool(args, "force", false);
            const auto found = std::find_if(config_.devices.begin(), config_.devices.end(), [&](const auto& device) {
                return device.id == device_id;
            });
            if (found == config_.devices.end()) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            if (found->enabled && !force) return tool_error("DEVICE_IN_USE", "disable device or pass force=true before removal: " + device_id);
            const auto old_device = device_to_json(*found);
            state_cache_.stop();
            config_.devices.erase(found);
            reset_runtime_after_config_change();
            state_cache_.start();
            return tool_result({{"ok", true}, {"removed_device", old_device}, {"read_only", false}});
        }

        if (name == "enable_device" || name == "disable_device") {
            const auto device_id = required_arg_string(args, "device_id");
            auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            const auto old_device = device_to_json(*device);
            state_cache_.stop();
            device->enabled = name == "enable_device";
            reset_runtime_after_config_change();
            state_cache_.start();
            return tool_result({
                {"ok", true},
                {"device_id", device_id},
                {"enabled", device->enabled},
                {"old_device", old_device},
                {"new_device", device_to_json(*device)},
                {"read_only", false},
            });
        }

        if (name == "reload_configuration") {
            if (config_.source_path.empty()) {
                return tool_error("CONFIG_SOURCE_UNAVAILABLE", "server was not started from a configuration file");
            }
            try {
                auto reloaded = ConfigLoader::load_file(config_.source_path);
                state_cache_.stop();
                config_ = std::move(reloaded);
                reset_runtime_after_config_change();
                state_cache_.start();
                return tool_result({
                    {"ok", true},
                    {"source_path", config_.source_path},
                    {"device_count", static_cast<int>(config_.devices.size())},
                    {"read_only", false},
                });
            } catch (const std::exception& ex) {
                return tool_error("CONFIG_RELOAD_FAILED", ex.what());
            }
        }

        if (name == "update_alarm_rule") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto variable_name = required_arg_string(args, "variable");
            auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            auto* variable = find_variable(*device, variable_name);
            if (variable == nullptr) return tool_error("ALARM_RULE_NOT_FOUND", "unknown variable: " + variable_name);
            const auto old_variable = variable_to_json(*variable);
            auto updated_variable = *variable;
            try {
                apply_optional_threshold(args, "warn_min", updated_variable.warn_min);
                apply_optional_threshold(args, "warn_max", updated_variable.warn_max);
                apply_optional_threshold(args, "alarm_min", updated_variable.alarm_min);
                apply_optional_threshold(args, "alarm_max", updated_variable.alarm_max);
            } catch (const std::exception& ex) {
                return tool_error("INVALID_ARGUMENT", ex.what());
            }
            state_cache_.stop();
            *variable = std::move(updated_variable);
            state_cache_.reset();
            state_cache_.start();
            return tool_result({
                {"ok", true},
                {"device_id", device_id},
                {"variable", variable_name},
                {"old_variable", old_variable},
                {"new_variable", variable_to_json(*variable)},
                {"read_only", false},
            });
        }

        if (name == "get_network_status") {
            Json devices = Json::array();
            const auto device_id = optional_arg_string(args, "device_id");
            if (!device_id.empty()) {
                const auto* device = find_device(config_, device_id);
                if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
                devices.push_back(device_connection_health_to_json(connections_.get_health(*device, config_.opcua)));
            } else {
                for (const auto& device : config_.devices) {
                    devices.push_back(device_connection_health_to_json(connections_.get_health(device, config_.opcua)));
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

        if (name == "refresh_device_state") {
            const auto device_id = optional_arg_string(args, "device_id");
            if (!device_id.empty()) {
                const auto* device = find_device(config_, device_id);
                if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
                state_cache_.refresh_device(device_id);
                return tool_result(state_cache_.state_json(device_id));
            }
            state_cache_.refresh_once();
            return tool_result(state_cache_.state_json());
        }

        if (name == "acknowledge_alarm") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            const auto alarm_id = optional_arg_string(args, "alarm_id");
            const auto user_id = call_context_string(params, args, "user_id", "anonymous");
            const auto ok = alarms_.acknowledge(alarm_id, device_id, user_id, optional_arg_string(args, "message"));
            return tool_result({
                {"ok", ok},
                {"device_id", device_id},
                {"alarm_id", alarm_id},
                {"acknowledged_by", user_id},
                {"read_only", false},
            }, !ok);
        }

        if (name == "clear_cached_alarm") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            const auto variable = optional_arg_string(args, "variable");
            const auto ok = state_cache_.clear_cached_alarm(device_id, variable);
            return tool_result({
                {"ok", ok},
                {"device_id", device_id},
                {"variable", variable},
                {"read_only", false},
            }, !ok);
        }

        if (name == "prepare_device_action") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto action = required_arg_string(args, "action");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            if (!device->enabled) return tool_error("DEVICE_DISABLED", "device is disabled: " + device_id);
            if (action == "call_device_method") {
                const auto method_name = required_arg_string(args, "method");
                const auto* method = find_method(*device, method_name);
                if (method == nullptr) return tool_error("METHOD_NOT_FOUND", "unknown method: " + method_name);
                if (!method->enabled) return tool_error("METHOD_DISABLED", "method is disabled: " + method_name);
            }

            PendingOperation operation;
            {
                std::lock_guard<std::mutex> lock(operations_mutex_);
                ++operation_counter_;
                std::ostringstream id;
                id << "op-" << now_utc_iso8601() << "-" << operation_counter_;
                operation.operation_id = id.str();
            }
            operation.user_id = call_context_string(params, args, "user_id", "anonymous");
            operation.client_id = call_context_string(params, args, "client_id", "mcp-client");
            operation.device_id = device_id;
            operation.action = action;
            operation.arguments = args;
            operation.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            const auto operation_id = operation.operation_id;
            {
                std::lock_guard<std::mutex> lock(operations_mutex_);
                pending_operations_[operation_id] = std::move(operation);
            }
            return tool_result({
                {"operation_id", operation_id},
                {"risk", "HIGH"},
                {"description", "Prepared device action: " + action + " on " + device_id},
                {"expires_in_seconds", 60},
                {"requires_confirmation", true},
                {"read_only", false},
            });
        }

        if (name == "confirm_device_action") {
            const auto operation_id = required_arg_string(args, "operation_id");
            std::lock_guard<std::mutex> lock(operations_mutex_);
            const auto found = pending_operations_.find(operation_id);
            if (found == pending_operations_.end()) return tool_error("INVALID_ARGUMENT", "unknown operation_id: " + operation_id);
            if (std::chrono::steady_clock::now() > found->second.expires_at) {
                pending_operations_.erase(found);
                return tool_error("EXECUTION_TIMEOUT", "operation_id has expired: " + operation_id);
            }
            found->second.confirmed = true;
            return tool_result({
                {"operation_id", operation_id},
                {"confirmed", true},
                {"executed", false},
                {"message", "operation confirmed; pass operation_id to the target high-risk tool to execute it"},
                {"read_only", false},
            });
        }

        if (name == "cancel_device_action") {
            const auto operation_id = required_arg_string(args, "operation_id");
            std::lock_guard<std::mutex> lock(operations_mutex_);
            const auto erased = pending_operations_.erase(operation_id);
            return tool_result({
                {"operation_id", operation_id},
                {"cancelled", erased > 0},
                {"read_only", false},
            });
        }

        if (name == "call_device_method") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto method_name = required_arg_string(args, "method");
            const Json method_arguments = args.contains("arguments") ? args.at("arguments") : Json::array();
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            if (!device->enabled) return tool_error("DEVICE_DISABLED", "device is disabled: " + device_id);
            const auto* method = find_method(*device, method_name);
            if (method == nullptr) return tool_error("METHOD_NOT_FOUND", "unknown method: " + method_name);
            if (!method->enabled) return tool_error("METHOD_DISABLED", "method is disabled: " + method_name);
            const auto argument_error = validate_method_arguments(*method, method_arguments);
            if (!argument_error.empty()) return tool_error("INVALID_ARGUMENT", argument_error);

            if (method->requires_confirmation) {
                const auto operation_id = optional_arg_string(args, "operation_id");
                if (operation_id.empty()) return tool_error("METHOD_CONFIRMATION_REQUIRED", "operation_id is required for method: " + method_name);
                std::lock_guard<std::mutex> lock(operations_mutex_);
                const auto found = pending_operations_.find(operation_id);
                if (found == pending_operations_.end()) return tool_error("INVALID_ARGUMENT", "unknown operation_id: " + operation_id);
                if (!found->second.confirmed) return tool_error("METHOD_CONFIRMATION_REQUIRED", "operation_id is not confirmed: " + operation_id);
                if (std::chrono::steady_clock::now() > found->second.expires_at) {
                    pending_operations_.erase(found);
                    return tool_error("EXECUTION_TIMEOUT", "operation_id has expired: " + operation_id);
                }
                if (found->second.device_id != device_id || found->second.action != "call_device_method") {
                    return tool_error("INVALID_ARGUMENT", "operation_id does not match requested device method call");
                }
                const auto prepared_method = optional_arg_string(found->second.arguments, "method");
                if (!prepared_method.empty() && prepared_method != method_name) {
                    return tool_error("INVALID_ARGUMENT", "operation_id was prepared for a different method");
                }
                pending_operations_.erase(found);
            }

            const auto call = connections_.call_method(*device, *method, method_arguments, config_.opcua);
            auto payload = method_result_to_json(call, device_id, *method);
            if (!call.ok) {
                if (!payload.contains("error_code") || payload.at("error_code").get<std::string>().empty()) {
                    payload["error_code"] = "OPCUA_METHOD_FAILED";
                }
                return tool_result(payload, true);
            }
            return tool_result(payload);
        }

        if (name == "read_node" || name == "read_opcua_node") {
            const auto device_id = required_arg_string(args, "device_id");
            const auto* device = find_device(config_, device_id);
            if (device == nullptr) return tool_error("DEVICE_NOT_FOUND", "unknown device_id: " + device_id);
            if (!device->enabled) return tool_error("DEVICE_DISABLED", "device is disabled: " + device_id);

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

            return tool_result(read_result_to_json(connections_.read_node(*device, variable, node_id, config_.opcua), device_id, node_id, variable));
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
            if (!device->enabled) return tool_error("DEVICE_DISABLED", "device is disabled: " + device_id);
            const auto* variable = find_variable(*device, variable_name);
            if (variable == nullptr) return tool_error("VARIABLE_NOT_FOUND", "unknown variable: " + variable_name);
            if (!variable->writable) {
                return tool_error("WRITE_NOT_ALLOWED", "variable is not marked writable in configuration: " + variable_name);
            }
            const auto constraint_error = validate_write_constraints(*variable, args.at("value"));
            if (!constraint_error.empty()) {
                return tool_error("INVALID_ARGUMENT", constraint_error);
            }

            const auto write = connections_.write_node(*device, *variable, args.at("value"), config_.opcua);
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
            if (!device->enabled) return tool_error("DEVICE_DISABLED", "device is disabled: " + device_id);

            Json variables = Json::array();
            std::vector<const VariableConfig*> variable_refs;
            variable_refs.reserve(device->variables.size());
            for (const auto& [_, variable] : device->variables) {
                variable_refs.push_back(&variable);
            }

            const auto reads = connections_.read_nodes(*device, variable_refs, config_.opcua);
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
    std::size_t method_count = 0;
    int mock_devices = 0;
    int opcua_devices = 0;
    Json devices = Json::array();

    for (const auto& device : config_.devices) {
        variable_count += device.variables.size();
        method_count += device.methods.size();
        const bool mock = device.endpoint.rfind("mock://", 0) == 0;
        if (mock) {
            ++mock_devices;
        } else {
            ++opcua_devices;
        }
        devices.push_back({
            {"id", device.id},
            {"name", device.name},
            {"protocol", device.protocol},
            {"enabled", device.enabled},
            {"endpoint_type", mock ? "mock" : "opcua"},
            {"variable_count", static_cast<int>(device.variables.size())},
            {"method_count", static_cast<int>(device.methods.size())},
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
        {"transport", {{"mode", config_.transport.mode}, {"http_host", config_.http.host}, {"http_port", config_.http.port}}},
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
        {"security", {
            {"enabled", config_.security.enabled},
            {"default_role", config_.security.default_role},
            {"role_count", static_cast<int>(config_.security.roles.size())},
        }},
        {"timeouts", {
            {"mcp_request_ms", config_.timeouts.mcp_request_ms},
            {"tool_execution_ms", config_.timeouts.tool_execution_ms},
            {"opcua_request_ms", config_.timeouts.opcua_request_ms},
        }},
        {"reliability", {
            {"max_retry_count", config_.reliability.max_retry_count},
            {"backoff_initial_ms", config_.reliability.backoff_initial_ms},
            {"backoff_max_ms", config_.reliability.backoff_max_ms},
            {"circuit_failure_threshold", config_.reliability.circuit_failure_threshold},
            {"circuit_cooldown_ms", config_.reliability.circuit_cooldown_ms},
        }},
        {"observability", {
            {"metrics_enabled", config_.observability.metrics_enabled},
            {"metrics_port", config_.observability.metrics_port},
        }},
        {"storage", {
            {"type", config_.storage.type},
            {"sqlite_path_configured", !config_.storage.sqlite_path.empty()},
        }},
        {"configuration", {
            {"device_count", static_cast<int>(config_.devices.size())},
            {"variable_count", static_cast<int>(variable_count)},
            {"method_count", static_cast<int>(method_count)},
            {"mock_device_count", mock_devices},
            {"opcua_device_count", opcua_devices},
            {"devices", devices},
        }},
        {"read_only", true},
    };
}

Json McpServer::device_health(const std::string& device_id) {
    const auto* device = find_device(config_, device_id);
    if (device == nullptr) {
        return {{"ok", false}, {"error", "DEVICE_NOT_FOUND"}, {"message", "unknown device_id: " + device_id}};
    }

    Json out = Json::object();
    out["device_id"] = device_id;
    out["name"] = device->name;
    out["protocol"] = device->protocol;
    out["enabled"] = device->enabled;
    out["timestamp"] = now_utc_iso8601();
    out["read_only"] = true;

    const auto connection = connections_.get_health(*device, config_.opcua);
    out["ok"] = connection.online;
    out["status"] = !device->enabled ? "disabled" : (connection.online ? "healthy" : "degraded");
    out["connection"] = device_connection_health_to_json(connection);
    out["cache"] = state_cache_.state_json(device_id);
    return out;
}

Json McpServer::health_live() const {
    return {
        {"ok", true},
        {"status", "live"},
        {"timestamp", now_utc_iso8601()},
        {"server", {{"name", config_.server.name}, {"version", config_.server.version}}},
    };
}

Json McpServer::health_devices() const {
    return {
        {"ok", true},
        {"timestamp", now_utc_iso8601()},
        {"devices", connections_.all_health_json()},
        {"cache", state_cache_.state_json()},
    };
}

Json McpServer::health_ready() const {
    const bool ready = !config_.devices.empty();
    const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at_
    ).count();
    return {
        {"ok", ready},
        {"status", ready ? "ready" : "not_ready"},
        {"timestamp", now_utc_iso8601()},
        {"started_at", started_at_utc_},
        {"uptime_ms", uptime_ms},
        {"device_count", static_cast<int>(config_.devices.size())},
        {"cache", {
            {"enabled", config_.cache.enabled},
            {"state", state_cache_.state_json()},
        }},
        {"devices", connections_.all_health_json()},
        {"storage", {
            {"type", config_.storage.type},
            {"sqlite_compiled", sqlite_storage_compiled()},
            {"sqlite_path_configured", !config_.storage.sqlite_path.empty()},
        }},
        {"observability", {
            {"metrics_enabled", config_.observability.metrics_enabled},
            {"metrics_port", config_.observability.metrics_port},
            {"health_port", config_.http.port},
        }},
    };
}

std::string McpServer::metrics_text() const {
    return metrics_.prometheus_text(connections_.all_health_json(), state_cache_.state_json());
}

void McpServer::reset_runtime_after_config_change() {
    connections_.reset();
    state_cache_.reset();
    std::lock_guard<std::mutex> lock(operations_mutex_);
    pending_operations_.clear();
}

HttpResponse McpServer::handle_http_request(const std::string& path) const {
    if (path == "/health/live") {
        return {200, "application/json", health_live().dump()};
    }
    if (path == "/health/ready") {
        const auto ready = health_ready();
        return {ready.at("ok").get<bool>() ? 200 : 503, "application/json", ready.dump()};
    }
    if (path == "/health/devices") {
        return {200, "application/json", health_devices().dump()};
    }
    if (path == "/metrics") {
        if (!config_.observability.metrics_enabled) {
            return {404, "text/plain; version=0.0.4", "metrics disabled\n"};
        }
        return {200, "text/plain; version=0.0.4", metrics_text()};
    }
    return {404, "application/json", Json({{"error", "NOT_FOUND"}, {"path", path}}).dump()};
}

} // namespace industrial_mcp
