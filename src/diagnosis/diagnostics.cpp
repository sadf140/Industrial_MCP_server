#include "industrial_mcp/diagnosis/diagnostics.hpp"

#include <vector>

namespace industrial_mcp {
namespace {

std::string arg_string(const Json& args, const std::string& key) {
    if (args.is_object() && args.contains(key) && args.at(key).is_string()) {
        return args.at(key).get<std::string>();
    }
    return {};
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

Json variable_context(const DeviceState& state, const DeviceConfig& device, Json& possible_causes) {
    Json threshold_context = Json::array();
    for (const auto& [name, cached] : state.variables) {
        const auto* variable = find_variable(device, name);
        if (variable == nullptr || !cached.ok || !cached.value.is_number()) continue;
        const double value = cached.value.get<double>();

        Json item = Json::object();
        item["name"] = name;
        item["node_id"] = cached.node_id;
        item["value"] = value;
        item["unit"] = cached.unit;
        item["status"] = "normal";
        if (variable->warn_min) item["warn_min"] = *variable->warn_min;
        if (variable->warn_max) item["warn_max"] = *variable->warn_max;
        if (variable->alarm_min) item["alarm_min"] = *variable->alarm_min;
        if (variable->alarm_max) item["alarm_max"] = *variable->alarm_max;

        if (variable->alarm_max && value > *variable->alarm_max) {
            item["status"] = "critical_high";
            possible_causes.push_back(cause("VARIABLE_ABOVE_ALARM_MAX",
                                            name + " is above alarm_max",
                                            item,
                                            "inspect process load, sensor calibration, cooling/lubrication condition, and related interlocks",
                                            0.78));
        } else if (variable->alarm_min && value < *variable->alarm_min) {
            item["status"] = "critical_low";
            possible_causes.push_back(cause("VARIABLE_BELOW_ALARM_MIN",
                                            name + " is below alarm_min",
                                            item,
                                            "inspect supply condition, sensor wiring, actuator state, and upstream process constraints",
                                            0.78));
        } else if ((variable->warn_max && value > *variable->warn_max) ||
                   (variable->warn_min && value < *variable->warn_min)) {
            item["status"] = "warning";
            possible_causes.push_back(cause("VARIABLE_OUTSIDE_WARNING_RANGE",
                                            name + " is outside warning range",
                                            item,
                                            "continue monitoring trend and compare with recent alarms before taking corrective action",
                                            0.55));
        }

        if (item.at("status").get<std::string>() != "normal") {
            threshold_context.push_back(item);
        }
    }
    return threshold_context;
}

} // namespace

Json DiagnosticsEngine::diagnose(const AppConfig& config,
                                 const AlarmStore& alarms,
                                 const DeviceStateCache& state_cache,
                                 const Json& arguments) {
    const auto device_id = arg_string(arguments, "device_id");
    const auto symptom = arg_string(arguments, "symptom");
    const auto start_time = arg_string(arguments, "start_time");
    const auto end_time = arg_string(arguments, "end_time");

    Json result = Json::object();
    Json limitations = Json::array();
    Json evidence = Json::array();
    Json possible_causes = Json::array();
    Json recommended_focus = Json::array();

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

    const auto cached_state = state_cache.state_for(device_id);
    Json device_state_json = state_cache.state_json(device_id);
    Json device_state = device_state_json.contains("device") ? device_state_json.at("device") : Json(nullptr);
    Json network_context = Json::object();
    network_context["source"] = "device_state_cache";
    network_context["cache_enabled"] = state_cache.enabled();
    network_context["online"] = cached_state ? cached_state->online : false;
    network_context["stale"] = cached_state ? cached_state->stale : true;
    network_context["status"] = cached_state ? (cached_state->stale ? "STALE" : cached_state->status) : "NO_CACHED_STATE";
    network_context["last_error"] = cached_state ? cached_state->last_error : "no cached state collected yet";
    network_context["last_update_time"] = cached_state ? cached_state->last_update_time : "";
    evidence.push_back({{"type", "device_state_cache"}, {"state", device_state}});
    evidence.push_back({{"type", "network_context"}, {"context", network_context}});

    if (!state_cache.enabled()) {
        limitations.push_back("device state cache is disabled");
    }
    if (!cached_state) {
        limitations.push_back("no cached device state is available yet");
        recommended_focus.push_back("confirm whether the cache worker has started and whether OPC UA endpoint is reachable");
    } else if (cached_state->stale) {
        limitations.push_back("cached device state is stale");
        recommended_focus.push_back("distinguish process fault evidence from stale telemetry before taking action");
    }

    if (!cached_state || !cached_state->online) {
        possible_causes.push_back(cause("DEVICE_OFFLINE",
                                        "device communication is offline or no cached successful sample exists",
                                        network_context,
                                        "check network reachability, endpoint URL, OPC UA server state, and security policy",
                                        0.85));
        recommended_focus.push_back("verify communication path before interpreting process variables");
    } else {
        recommended_focus.push_back("compare threshold evidence with recent alarm order and operator symptom");
    }

    AlarmQuery alarm_query;
    alarm_query.device_id = device_id;
    alarm_query.start_time = start_time;
    alarm_query.end_time = end_time;
    alarm_query.limit = 50;
    const auto alarm_analysis = alarms.analyze_json(alarm_query);
    const auto recent_alarms = alarms.query_json(alarm_query);
    Json alarm_evidence = Json::object();
    alarm_evidence["type"] = "alarm_analysis";
    alarm_evidence["analysis"] = alarm_analysis;
    evidence.push_back(alarm_evidence);

    Json threshold_context = Json::array();
    if (cached_state) {
        threshold_context = variable_context(*cached_state, *device, possible_causes);
    }

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
        recommended_focus.push_back("correlate the operator symptom with cached variables and first-out alarms");
    }

    Json alarm_context = Json::object();
    alarm_context["query"] = {
        {"device_id", device_id},
        {"start_time", start_time},
        {"end_time", end_time},
        {"limit", alarm_query.limit},
    };
    alarm_context["analysis"] = alarm_analysis;
    alarm_context["recent"] = recent_alarms;

    if (recommended_focus.empty()) {
        recommended_focus.push_back("validate the cached state against field instrumentation before making maintenance decisions");
    }

    Json llm_context = Json::object();
    llm_context["purpose"] = "Structured industrial fault diagnosis context for an LLM; C++ does not call a model directly.";
    llm_context["device_id"] = device_id;
    llm_context["operator_symptom"] = symptom;
    llm_context["recommended_focus"] = recommended_focus;
    llm_context["constraints"] = Json::array({
        "treat cached telemetry as evidence, not proof",
        "separate communication faults from process faults",
        "verify recommendations against equipment manuals and site safety procedures",
        "do not perform write operations unless maintenance mode and configuration explicitly allow it",
    });

    result["summary"] = possible_causes.empty()
        ? "LLM diagnostic context generated; no local rule identified a clear fault"
        : "LLM diagnostic context generated with local rule-based possible causes";
    result["device_state"] = device_state;
    result["alarm_context"] = alarm_context;
    result["threshold_context"] = threshold_context;
    result["network_context"] = network_context;
    result["llm_context"] = llm_context;
    result["possible_causes"] = possible_causes;
    result["evidence"] = evidence;
    result["recommended_actions"] = actions;
    result["confidence"] = possible_causes.empty() ? 0.25 : 0.65;
    result["limitations"] = limitations;
    return result;
}

} // namespace industrial_mcp
