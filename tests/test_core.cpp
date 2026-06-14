#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/mcp_server.hpp"
#include "industrial_mcp/opcua_client.hpp"

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
#include <open62541pp/node.hpp>
#include <open62541pp/server.hpp>
#include <open62541pp/types.hpp>
#endif

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace industrial_mcp;

namespace {

std::string temp_file_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

Json load_last_json_line(const std::string& path) {
    std::ifstream input(path);
    std::string line;
    std::string last_line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }
    return Json::parse(last_line);
}

bool has_tool(const Json& tools, const std::string& name) {
    for (const auto& tool : tools) {
        if (tool.contains("name") && tool.at("name").get<std::string>() == name) {
            return true;
        }
    }
    return false;
}

AppConfig make_mock_config(const std::string& audit_path = {}, const std::string& alarm_path = {}) {
    const auto config_json = Json::parse(R"({
        "server": {
            "name": "industrial-mcp-server",
            "version": "0.4.0-p2",
            "read_only": true
        },
        "opcua": {
            "allow_raw_node_id": false,
            "connect_timeout_ms": 1000,
            "read_timeout_ms": 1000,
            "retry_count": 1,
            "retry_delay_ms": 1,
            "write_enabled": false
        },
        "cache": {
            "enabled": false,
            "poll_interval_ms": 2000,
            "stale_after_ms": 10000
        },
        "audit": {
            "log_path": ""
        },
        "alarm_log_path": "",
        "devices": [{
            "id": "pump-1",
            "name": "Pump 1",
            "endpoint": "mock://pump-1",
            "variables": [{
                "name": "temperature",
                "node_id": "ns=2;s=Pump1.Temperature",
                "data_type": "Double",
                "unit": "C",
                "mock_value": 82.5,
                "writable": false,
                "warn_max": 80,
                "alarm_max": 95
            }, {
                "name": "running",
                "node_id": "ns=2;s=Pump1.Running",
                "data_type": "Boolean",
                "mock_value": true,
                "writable": false
            }]
        }]
    })");
    auto config = ConfigLoader::load_json(config_json, ".");
    config.audit.log_path = audit_path;
    config.alarm_log_path = alarm_path;
    return config;
}

void test_mcp_lifecycle_tools_and_audit() {
    const auto audit_path = temp_file_path("industrial_mcp_audit_test.jsonl");
    std::remove(audit_path.c_str());

    auto config = make_mock_config(audit_path);
    assert(find_device(config, "pump-1") != nullptr);
    assert(find_variable(*find_device(config, "pump-1"), "temperature") != nullptr);

    McpServer server(config);

    const auto initialize = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    assert(initialize.has_value());
    assert(initialize->contains("result"));
    assert(initialize->at("result").at("capabilities").contains("tools"));

    const auto initialized = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    assert(!initialized.has_value());

    const auto ping = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
    assert(ping.has_value());
    assert(ping->at("result").is_object());

    const auto list = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"));
    assert(list.has_value());
    const auto& tools = list->at("result").at("tools");
    assert(tools.is_array());
    assert(!tools.empty());
    assert(tools.at(0).contains("inputSchema"));
    assert(tools.at(0).contains("outputSchema"));
    assert(tools.at(0).at("name").get<std::string>() == "get_gateway_health");
    assert(has_tool(tools, "read_node"));
    assert(has_tool(tools, "write_node"));
    assert(has_tool(tools, "list_devices"));
    assert(has_tool(tools, "get_alarm_history"));
    assert(has_tool(tools, "diagnose_fault"));
    assert(has_tool(tools, "get_network_status"));
    assert(has_tool(tools, "get_device_state"));

    const auto health = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":30,
        "method":"tools/call",
        "params":{"name":"get_gateway_health","arguments":{}}
    })"));
    assert(health.has_value());
    const auto& health_content = health->at("result").at("structuredContent");
    assert(health_content.at("ok").get<bool>());
    assert(health_content.at("read_only").get<bool>());
    assert(health_content.at("server").at("version").get<std::string>() == "0.4.0-p2");
    assert(health_content.at("configuration").at("device_count").get<int>() == 1);
    assert(health_content.at("configuration").at("variable_count").get<int>() == 2);
    assert(health_content.at("uptime_ms").get<long long>() >= 0);
    assert(!health_content.at("cache").at("enabled").get<bool>());

    const auto read = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":4,
        "method":"tools/call",
        "params":{"name":"read_opcua_node","arguments":{"device_id":"pump-1","variable":"temperature","token":"sensitive"}}
    })"));
    assert(read.has_value());
    const auto& read_result = read->at("result");
    assert(read_result.contains("structuredContent"));
    assert(read_result.at("structuredContent").at("ok").get<bool>());
    assert(read_result.at("structuredContent").at("value").get<double>() == 82.5);
    assert(read_result.at("structuredContent").at("attempts").get<int>() == 1);

    const auto audit = load_last_json_line(audit_path);
    assert(audit.at("event").get<std::string>() == "tool_call");
    assert(audit.at("tool").get<std::string>() == "read_opcua_node");
    assert(audit.at("device_id").get<std::string>() == "pump-1");
    assert(audit.at("ok").get<bool>());
    assert(audit.at("read_only").get<bool>());
    assert(audit.at("arguments").at("token").get<std::string>() == "***");

    const auto read_alias = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":40,
        "method":"tools/call",
        "params":{"name":"read_node","arguments":{"device_id":"pump-1","variable":"temperature"}}
    })"));
    assert(read_alias.has_value());
    assert(read_alias->at("result").at("structuredContent").at("ok").get<bool>());
    assert(read_alias->at("result").at("structuredContent").at("value").get<double>() == 82.5);

    const auto devices = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":41,
        "method":"tools/call",
        "params":{"name":"list_devices","arguments":{}}
    })"));
    assert(devices.has_value());
    const auto& devices_content = devices->at("result").at("structuredContent");
    assert(devices_content.at("count").get<int>() == 1);
    assert(devices_content.at("devices").at(0).at("id").get<std::string>() == "pump-1");
    assert(devices_content.at("devices").at(0).at("variables").size() == 2);
    assert(devices_content.at("devices").at(0).at("variables").at(0).contains("writable"));

    const auto network = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":42,
        "method":"tools/call",
        "params":{"name":"get_network_status","arguments":{"device_id":"pump-1"}}
    })"));
    assert(network.has_value());
    const auto& network_content = network->at("result").at("structuredContent");
    assert(network_content.at("count").get<int>() == 1);
    assert(network_content.at("devices").at(0).at("online").get<bool>());
    assert(network_content.at("devices").at(0).at("latency_ms").get<long long>() >= 0);

    const auto write_disabled = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":43,
        "method":"tools/call",
        "params":{"name":"write_node","arguments":{"device_id":"pump-1","variable":"temperature","value":70.0}}
    })"));
    assert(write_disabled.has_value());
    assert(write_disabled->at("result").at("isError").get<bool>());
    assert(write_disabled->at("result").at("structuredContent").at("error").get<std::string>() == "WRITE_DISABLED");

    const auto snapshot = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":5,
        "method":"tools/call",
        "params":{"name":"read_device_snapshot","arguments":{"device_id":"pump-1"}}
    })"));
    assert(snapshot.has_value());
    const auto& snapshot_content = snapshot->at("result").at("structuredContent");
    assert(snapshot_content.at("ok").get<bool>());
    assert(snapshot_content.at("variables").size() == 2);
    for (const auto& item : snapshot_content.at("variables")) {
        assert(item.at("attempts").get<int>() == 1);
    }

    const auto raw_blocked = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":6,
        "method":"tools/call",
        "params":{"name":"read_opcua_node","arguments":{"device_id":"pump-1","node_id":"ns=2;s=Pump1.Temperature"}}
    })"));
    assert(raw_blocked.has_value());
    assert(raw_blocked->at("result").at("isError").get<bool>());

    const auto diag = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":7,
        "method":"tools/call",
        "params":{"name":"diagnose_fault","arguments":{"device_id":"pump-1"}}
    })"));
    assert(diag.has_value());
    assert(diag->at("result").contains("structuredContent"));
    const auto& diag_content = diag->at("result").at("structuredContent");
    assert(diag_content.contains("device_state"));
    assert(diag_content.contains("alarm_context"));
    assert(diag_content.contains("network_context"));
    assert(diag_content.contains("llm_context"));
    assert(diag_content.at("network_context").at("cache_enabled").get<bool>() == false);

    std::remove(audit_path.c_str());
}

void test_alarm_store_quality_and_ordering() {
    const auto alarm_path = temp_file_path("industrial_mcp_alarm_test.jsonl");
    std::remove(alarm_path.c_str());
    {
        std::ofstream output(alarm_path);
        output << R"({"timestamp":"2026-06-11T08:20:00Z","device_id":"pump-1","severity":"warning","code":"TEMP_HIGH","message":"temperature high","state":"active","source":"motor","acknowledged":false})" << '\n';
        output << R"({"timestamp":"bad-time","device_id":"pump-1","severity":"warning","code":"BROKEN","message":"bad record"})" << '\n';
        output << R"({"timestamp":"2026-06-11T08:32:00Z","device_id":"pump-1","severity":"critical","code":"TEMP_HIGH","message":"temperature critical","state":"active","source":"bearing","acknowledged":true})" << '\n';
        output << R"({"timestamp":"2026-06-11T08:45:00Z","device_id":"pump-1","severity":"warning","code":"VIB_HIGH","message":"vibration high","state":"cleared","source":"sensor","acknowledged":false})" << '\n';
    }

    AlarmStore store(alarm_path);
    AlarmQuery query;
    query.device_id = "pump-1";
    query.limit = 10;

    const auto queried = store.query_json(query);
    assert(queried.at("count").get<int>() == 3);
    assert(queried.at("invalid_record_count").get<int>() == 1);
    assert(queried.at("latest_alarm").at("code").get<std::string>() == "VIB_HIGH");
    assert(queried.at("alarms").at(0).at("timestamp").get<std::string>() == "2026-06-11T08:45:00Z");
    assert(queried.at("alarms").at(0).at("state").get<std::string>() == "cleared");

    const auto analyzed = store.analyze_json(query);
    assert(analyzed.at("total").get<int>() == 3);
    assert(analyzed.at("invalid_record_count").get<int>() == 1);
    assert(analyzed.at("frequent_alarm_codes").at(0).at("code").get<std::string>() == "TEMP_HIGH");
    assert(analyzed.at("latest_alarm").at("code").get<std::string>() == "VIB_HIGH");
    assert(analyzed.at("first_alarm").at("code").get<std::string>() == "TEMP_HIGH");
    assert(analyzed.at("timeline").at(0).at("timestamp").get<std::string>() == "2026-06-11T08:20:00Z");

    McpServer server(make_mock_config({}, alarm_path));
    const auto history = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":50,
        "method":"tools/call",
        "params":{"name":"get_alarm_history","arguments":{"device_id":"pump-1","severity":"warning","limit":2}}
    })"));
    assert(history.has_value());
    const auto& history_content = history->at("result").at("structuredContent");
    assert(history_content.at("count").get<int>() == 2);
    assert(history_content.at("alarms").at(0).at("severity").get<std::string>() == "warning");

    AlarmRecord appended;
    appended.timestamp = "2026-06-11T09:10:00Z";
    appended.device_id = "pump-1";
    appended.level = "WARN";
    appended.code = "CURRENT_HIGH";
    appended.message = "current above warning threshold";
    appended.source_node = "ns=2;s=Pump1.Current";
    appended.value = 17.2;
    appended.threshold = 16.0;
    assert(store.append(appended));

    AlarmQuery level_query;
    level_query.device_id = "pump-1";
    level_query.level = "WARN";
    level_query.limit = 1;
    const auto by_level = store.query_json(level_query);
    assert(by_level.at("count").get<int>() == 1);
    assert(by_level.at("alarms").at(0).at("level").get<std::string>() == "WARN");
    assert(by_level.at("alarms").at(0).at("source_node").get<std::string>() == "ns=2;s=Pump1.Current");
    assert(by_level.at("alarms").at(0).at("value").get<double>() == 17.2);
    assert(by_level.at("alarms").at(0).at("threshold").get<double>() == 16.0);

    std::remove(alarm_path.c_str());
}

void test_device_state_cache_and_auto_alarm() {
    const auto alarm_path = temp_file_path("industrial_mcp_cache_alarm_test.jsonl");
    std::remove(alarm_path.c_str());

    {
        auto config = make_mock_config({}, alarm_path);
        config.cache.enabled = true;
        config.cache.poll_interval_ms = 50;
        config.cache.stale_after_ms = 5000;

        McpServer server(config);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        const auto state = server.handle_message(Json::parse(R"({
            "jsonrpc":"2.0",
            "id":70,
            "method":"tools/call",
            "params":{"name":"get_device_state","arguments":{"device_id":"pump-1"}}
        })"));
        assert(state.has_value());
        const auto& state_content = state->at("result").at("structuredContent");
        assert(state_content.at("count").get<int>() == 1);
        const auto& device_state = state_content.at("device");
        assert(device_state.at("device_id").get<std::string>() == "pump-1");
        assert(device_state.at("online").get<bool>());
        assert(device_state.at("temperature").get<double>() == 82.5);
        assert(device_state.at("running").get<bool>());
        assert(device_state.at("variables").contains("temperature"));

        const auto history = server.handle_message(Json::parse(R"({
            "jsonrpc":"2.0",
            "id":71,
            "method":"tools/call",
            "params":{"name":"get_alarm_history","arguments":{"device_id":"pump-1","level":"WARN","keyword":"temperature","limit":5}}
        })"));
        assert(history.has_value());
        const auto& history_content = history->at("result").at("structuredContent");
        assert(history_content.at("count").get<int>() >= 1);
        assert(history_content.at("alarms").at(0).at("level").get<std::string>() == "WARN");
        assert(history_content.at("alarms").at(0).at("source_node").get<std::string>() == "ns=2;s=Pump1.Temperature");

        const auto diag = server.handle_message(Json::parse(R"({
            "jsonrpc":"2.0",
            "id":72,
            "method":"tools/call",
            "params":{"name":"diagnose_fault","arguments":{"device_id":"pump-1","symptom":"temperature rising"}}
        })"));
        assert(diag.has_value());
        const auto& diag_content = diag->at("result").at("structuredContent");
        assert(diag_content.contains("device_state"));
        assert(diag_content.contains("alarm_context"));
        assert(diag_content.contains("network_context"));
        assert(diag_content.contains("llm_context"));
        assert(diag_content.at("threshold_context").size() >= 1);
        assert(diag_content.at("network_context").at("online").get<bool>());
    }

    std::remove(alarm_path.c_str());
}

void test_write_node_policy_and_audit() {
    const auto audit_path = temp_file_path("industrial_mcp_write_audit_test.jsonl");
    std::remove(audit_path.c_str());

    auto config = make_mock_config(audit_path);
    config.server.read_only = false;
    config.opcua.write_enabled = true;
    auto* device = const_cast<DeviceConfig*>(find_device(config, "pump-1"));
    assert(device != nullptr);
    device->variables.at("temperature").writable = true;

    McpServer server(config);
    const auto write = server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":60,
        "method":"tools/call",
        "params":{"name":"write_node","arguments":{"device_id":"pump-1","variable":"temperature","value":70.0}}
    })"));
    assert(write.has_value());
    const auto& content = write->at("result").at("structuredContent");
    assert(content.at("ok").get<bool>());
    assert(content.at("read_only").get<bool>() == false);
    assert(content.at("written_value").get<double>() == 70.0);

    const auto audit = load_last_json_line(audit_path);
    assert(audit.at("tool").get<std::string>() == "write_node");
    assert(audit.at("read_only").get<bool>() == false);
    assert(audit.at("arguments").at("value").get<double>() == 70.0);

    device->variables.at("temperature").writable = false;
    McpServer blocked_server(config);
    const auto blocked = blocked_server.handle_message(Json::parse(R"({
        "jsonrpc":"2.0",
        "id":61,
        "method":"tools/call",
        "params":{"name":"write_node","arguments":{"device_id":"pump-1","variable":"temperature","value":70.0}}
    })"));
    assert(blocked.has_value());
    assert(blocked->at("result").at("isError").get<bool>());
    assert(blocked->at("result").at("structuredContent").at("error").get<std::string>() == "WRITE_NOT_ALLOWED");

    std::remove(audit_path.c_str());
}

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
void test_opcua_integration() {
    constexpr uint16_t port = 48520;
    opcua::ServerConfig server_config{port};
    server_config.setApplicationName("industrial-mcp-test-server");
    opcua::Server ua_server{std::move(server_config)};

    opcua::Node objects{ua_server, opcua::ObjectId::ObjectsFolder};
    const auto writable = opcua::VariableAttributes{}.setAccessLevel(
        opcua::AccessLevel::CurrentRead | opcua::AccessLevel::CurrentWrite
    );
    auto temperature = objects.addVariable({1, "Pump1.Temperature"}, "Pump1.Temperature", writable);
    temperature.writeValue(opcua::Variant{42.25});
    auto running = objects.addVariable({1, "Pump1.Running"}, "Pump1.Running", writable);
    running.writeValue(opcua::Variant{true});
    auto label = objects.addVariable({1, "Pump1.Label"}, "Pump1.Label", writable);
    label.writeValue(opcua::Variant{std::string{"pump-alpha"}});

    bool running_server = true;
    std::thread server_thread([&] {
        while (running_server) {
            ua_server.runIterate();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ua_server.stop();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    DeviceConfig device;
    device.id = "pump-1";
    device.endpoint = "opc.tcp://127.0.0.1:48520";
    VariableConfig temperature_variable;
    temperature_variable.name = "temperature";
    temperature_variable.node_id = "ns=1;s=Pump1.Temperature";
    temperature_variable.data_type = "Double";
    temperature_variable.writable = true;
    VariableConfig running_variable;
    running_variable.name = "running";
    running_variable.node_id = "ns=1;s=Pump1.Running";
    running_variable.data_type = "Boolean";
    VariableConfig label_variable;
    label_variable.name = "label";
    label_variable.node_id = "ns=1;s=Pump1.Label";
    label_variable.data_type = "String";

    OpcUaRuntimeConfig runtime;
    runtime.read_timeout_ms = 2000;
    runtime.connect_timeout_ms = 2000;
    runtime.retry_count = 1;
    runtime.retry_delay_ms = 1;

    OpcUaClient client;
    const auto status = client.get_status(device, runtime);
    assert(status.online);
    assert(status.attempts >= 1);

    const auto read = client.read_node(device, &temperature_variable, temperature_variable.node_id, runtime);
    assert(read.ok);
    assert(read.data_type == "Double");
    assert(read.value.get<double>() == 42.25);
    assert(read.attempts >= 1);

    const auto write = client.write_node(device, temperature_variable, Json(55.5), runtime);
    assert(write.ok);
    assert(write.latency_ms >= 0);
    const auto read_after_write = client.read_node(device, &temperature_variable, temperature_variable.node_id, runtime);
    assert(read_after_write.ok);
    assert(read_after_write.value.get<double>() == 55.5);

    const auto bad_write = client.write_node(device, temperature_variable, Json("not-a-number"), runtime);
    assert(!bad_write.ok);
    assert(bad_write.error_code == "INVALID_VALUE_TYPE");

    const std::vector<const VariableConfig*> variables{&temperature_variable, &running_variable, &label_variable};
    const auto reads = client.read_nodes(device, variables, runtime);
    assert(reads.size() == 3);
    assert(reads.at(0).ok);
    assert(reads.at(0).value.get<double>() == 55.5);
    assert(reads.at(1).ok);
    assert(reads.at(1).value.get<bool>());
    assert(reads.at(2).ok);
    assert(reads.at(2).value.get<std::string>() == "pump-alpha");
    for (const auto& item : reads) {
        assert(item.attempts >= 1);
    }

    running_server = false;
    server_thread.join();
}
#endif

} // namespace

int main() {
    test_mcp_lifecycle_tools_and_audit();
    test_alarm_store_quality_and_ordering();
    test_device_state_cache_and_auto_alarm();
    test_write_node_policy_and_audit();
#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    test_opcua_integration();
#endif
    std::cout << "industrial_mcp_tests passed\n";
    return 0;
}
