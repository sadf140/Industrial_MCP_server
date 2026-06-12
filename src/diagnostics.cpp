#include "industrial_mcp/diagnostics.hpp"

#include <sstream>
#include <vector>

namespace industrial_mcp {
namespace {

std::string arg_string(const Json& args, const std::string& key) {
    if (args.is_object() && args.contains(key) && args.at(key).is_string()) {
        return args.at(key).get<std::string>();
    }
    return {};
}

bool is_numeric_json(const Json& value) {
    return value.is_number();
}

Json cause(const std::string& code,
           const std::string& description,
           const Json& evidence,
           const std::string& action,
           double confidence) {
    Json out = Json::object();
    out["code"] = code;
    out["description"] = description;
    out["evidence"] = evidence;
    Json actions = Json::array();
    actions.push_back(action);
    out["recommended_actions"] = actions;
    out["confidence"] = confidence;
    return out;
}

} // namespace

Json DiagnosticsEngine::diagnose(const AppConfig& config,
                                 OpcUaClient& opcua,
                                 const AlarmStore& alarms,
                                 const Json& arguments) {
    const auto device_id = arg_string(arguments, "device_id");
    const auto symptom = arg_string(arguments, "symptom");
    const auto start_time = arg_string(arguments, "start_time");
    const auto end_time = arg_string(arguments, "end_time");

    Json result = Json::object();
    Json limitations = Json::array();
    Json evidence = Json::array();
    Json possible_causes = Json::array();

    const auto* device = find_device(config, device_id);
    if (device == nullptr) {
        result["summary"] = "device not found";
        limitations.push_back("device_id is not present in configuration");
        result["possible_causes"] = possible_causes;
        result["evidence"] = evidence;
        result["recommended_actions"] = Json::array();
        result["confidence"] = 0;
        result["limitations"] = limitations;
        return result;
    }

    const auto status = opcua.get_status(*device, config.opcua);
    Json status_evidence = Json::object();
    status_evidence["type"] = "device_status";
    status_evidence["online"] = status.online;
    status_evidence["session_state"] = status.session_state;
    status_evidence["error"] = status.error;
    status_evidence["latency_ms"] = status.latency_ms;
    status_evidence["disconnect_count"] = status.disconnect_count;
    status_evidence["consecutive_failures"] = status.consecutive_failures;
    status_evidence["last_success_at"] = status.last_success_at;
    status_evidence["last_error_at"] = status.last_error_at;
    evidence.push_back(status_evidence);

    if (!status.online) {
        possible_causes.push_back(cause("DEVICE_OFFLINE",
                                        "device or OPC UA session is offline",
                                        status_evidence,
                                        "check network reachability, endpoint URL, OPC UA server state, and security policy",
                                        0.85));
    } else if (status.consecutive_failures > 0 || status.disconnect_count > 0) {
        possible_causes.push_back(cause("INTERMITTENT_COMMUNICATION",
                                        "recent OPC UA communication failures were observed",
                                        status_evidence,
                                        "inspect network stability, gateway logs, OPC UA server load, and switch port diagnostics",
                                        0.58));
    }

    std::vector<const VariableConfig*> variable_refs;
    variable_refs.reserve(device->variables.size());
    for (const auto& [_, variable] : device->variables) {
        variable_refs.push_back(&variable);
    }
    const auto reads = opcua.read_nodes(*device, variable_refs, config.opcua);
    for (std::size_t index = 0; index < variable_refs.size(); ++index) {
        const auto& variable = *variable_refs[index];
        const auto& read = reads.at(index);
        Json sample = Json::object();
        sample["type"] = "variable_sample";
        sample["name"] = variable.name;
        sample["node_id"] = variable.node_id;
        sample["ok"] = read.ok;
        sample["value"] = read.value;
        sample["quality"] = read.quality;
        sample["error"] = read.error;
        sample["attempts"] = read.attempts;
        evidence.push_back(sample);

        if (!read.ok) {
            limitations.push_back("failed to read variable: " + variable.name);
            continue;
        }

        if (is_numeric_json(read.value)) {
            const double value = read.value.get<double>();
            if (variable.alarm_max && value > *variable.alarm_max) {
                possible_causes.push_back(cause("VARIABLE_ABOVE_ALARM_MAX",
                                                variable.name + " is above alarm_max",
                                                sample,
                                                "inspect process load, sensor calibration, cooling/lubrication condition, and related interlocks",
                                                0.78));
            } else if (variable.alarm_min && value < *variable.alarm_min) {
                possible_causes.push_back(cause("VARIABLE_BELOW_ALARM_MIN",
                                                variable.name + " is below alarm_min",
                                                sample,
                                                "inspect supply condition, sensor wiring, actuator state, and upstream process constraints",
                                                0.78));
            } else if ((variable.warn_max && value > *variable.warn_max) ||
                       (variable.warn_min && value < *variable.warn_min)) {
                possible_causes.push_back(cause("VARIABLE_OUTSIDE_WARNING_RANGE",
                                                variable.name + " is outside warning range",
                                                sample,
                                                "continue monitoring trend and compare with recent alarms before taking corrective action",
                                                0.55));
            }
        }
    }

    AlarmQuery alarm_query;
    alarm_query.device_id = device_id;
    alarm_query.start_time = start_time;
    alarm_query.end_time = end_time;
    alarm_query.limit = 50;
    const auto alarm_analysis = alarms.analyze_json(alarm_query);
    Json alarm_evidence = Json::object();
    alarm_evidence["type"] = "alarm_analysis";
    alarm_evidence["analysis"] = alarm_analysis;
    evidence.push_back(alarm_evidence);

    if (alarm_analysis.contains("total") && alarm_analysis.at("total").get<int>() > 0) {
        possible_causes.push_back(cause("RECENT_ALARM_PATTERN",
                                        "recent alarms exist for this device",
                                        alarm_evidence,
                                        "review the alarm timeline, first-out alarm, repeated codes, and maintenance records",
                                        0.62));
    }
    if (alarm_analysis.contains("invalid_record_count") && alarm_analysis.at("invalid_record_count").get<int>() > 0) {
        limitations.push_back("alarm log contains invalid records: " + std::to_string(alarm_analysis.at("invalid_record_count").get<int>()));
    }

    Json actions = Json::array();
    actions.push_back("verify the diagnosis with field instrumentation and equipment manuals");
    actions.push_back("treat write_node as a controlled maintenance operation only when explicitly enabled and whitelisted");
    actions.push_back("collect more data if confidence is low or evidence is incomplete");

    if (!symptom.empty()) {
        Json symptom_evidence = Json::object();
        symptom_evidence["type"] = "operator_symptom";
        symptom_evidence["symptom"] = symptom;
        evidence.push_back(symptom_evidence);
    }

    result["summary"] = possible_causes.empty()
        ? "no clear fault rule was triggered from available evidence"
        : "diagnostic rules found possible causes; validate evidence before acting";
    result["possible_causes"] = possible_causes;
    result["evidence"] = evidence;
    result["recommended_actions"] = actions;
    result["confidence"] = possible_causes.empty() ? 0.25 : 0.65;
    result["limitations"] = limitations;
    return result;
}

} // namespace industrial_mcp
