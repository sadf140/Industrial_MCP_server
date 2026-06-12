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

Json load_first_json_line(const std::string& path) {
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    return Json::parse(line);
}

AppConfig make_mock_config(const std::string& audit_path = {}, const std::string& alarm_path = {}) {
    const auto config_json = Json::parse(R"({
        "server": {
            "name": "industrial-mcp-server",
            "version": "0.3.0-p1",
            "read_only": true
        },
        "opcua": {
            "allow_raw_node_id": false,
            "connect_timeout_ms": 1000,
            "read_timeout_ms": 1000,
            "retry_count": 1,
            "retry_delay_ms": 1
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
                "warn_max": 80,
                "alarm_max": 95
            }, {
                "name": "running",
                "node_id": "ns=2;s=Pump1.Running",
                "data_type": "Boolean",
                "mock_value": true
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

    const auto audit = load_first_json_line(audit_path);
    assert(audit.at("event").get<std::string>() == "tool_call");
    assert(audit.at("tool").get<std::string>() == "read_opcua_node");
    assert(audit.at("device_id").get<std::string>() == "pump-1");
    assert(audit.at("ok").get<bool>());
    assert(audit.at("read_only").get<bool>());
    assert(audit.at("arguments").at("token").get<std::string>() == "***");

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

    std::remove(alarm_path.c_str());
}

#ifdef INDUSTRIAL_MCP_WITH_OPCUA
void test_opcua_integration() {
    constexpr uint16_t port = 48520;
    opcua::ServerConfig server_config{port};
    server_config.setApplicationName("industrial-mcp-test-server");
    opcua::Server ua_server{std::move(server_config)};

    opcua::Node objects{ua_server, opcua::ObjectId::ObjectsFolder};
    auto temperature = objects.addVariable({1, "Pump1.Temperature"}, "Pump1.Temperature");
    temperature.writeValue(opcua::Variant{42.25});
    auto running = objects.addVariable({1, "Pump1.Running"}, "Pump1.Running");
    running.writeValue(opcua::Variant{true});
    auto label = objects.addVariable({1, "Pump1.Label"}, "Pump1.Label");
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

    const std::vector<const VariableConfig*> variables{&temperature_variable, &running_variable, &label_variable};
    const auto reads = client.read_nodes(device, variables, runtime);
    assert(reads.size() == 3);
    assert(reads.at(0).ok);
    assert(reads.at(0).value.get<double>() == 42.25);
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
#ifdef INDUSTRIAL_MCP_WITH_OPCUA
    test_opcua_integration();
#endif
    std::cout << "industrial_mcp_tests passed\n";
    return 0;
}
