#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/mcp/mcp_server.hpp"

#include <cassert>
#include <iostream>

using namespace industrial_mcp;

namespace {

AppConfig make_config() {
    return ConfigLoader::load_json(Json::parse(R"({
        "server":{"name":"industrial-mcp-server","version":"0.5.0-p3","read_only":true},
        "http":{"host":"127.0.0.1","port":0},
        "cache":{"enabled":true,"poll_interval_ms":1000,"stale_after_ms":10000},
        "devices":[{"id":"pump-1","name":"Pump 1","endpoint":"mock://pump-1","variables":[
            {"name":"temperature","node_id":"ns=2;s=Pump1.Temperature","data_type":"Double","unit":"C","mock_value":82.5,"warn_max":80,"alarm_max":95},
            {"name":"running","node_id":"ns=2;s=Pump1.Running","data_type":"Boolean","mock_value":true}
        ]}]
    })"), ".");
}

Json call(McpServer& server, const std::string& name, Json arguments = Json::object()) {
    Json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", name}, {"arguments", std::move(arguments)}}},
    };
    const auto response = server.handle_message(request);
    assert(response.has_value());
    return response->at("result").at("structuredContent");
}

void test_mcp_core_tools() {
    auto config = make_config();
    McpServer server(config);

    const auto initialized = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
    assert(initialized.has_value());
    const auto listed = server.handle_message(Json::parse(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    assert(listed.has_value());

    assert(call(server, "list_devices").at("count").get<int>() == 1);
    assert(call(server, "read_node", {{"device_id", "pump-1"}, {"variable", "temperature"}}).at("ok").get<bool>());
    assert(call(server, "read_device_snapshot", {{"device_id", "pump-1"}}).at("ok").get<bool>());
    assert(call(server, "get_network_status", {{"device_id", "pump-1"}}).at("devices").at(0).at("online").get<bool>());
    assert(call(server, "get_alarm_history", {{"device_id", "pump-1"}}).contains("alarms"));
    assert(call(server, "diagnose_fault", {{"device_id", "pump-1"}}).contains("llm_context"));
    assert(server.health_live().at("ok").get<bool>());
    assert(server.health_ready().at("ok").get<bool>());
}

} // namespace

int main() {
    test_mcp_core_tools();
    std::cout << "industrial_mcp_integration_tests passed\n";
    return 0;
}
