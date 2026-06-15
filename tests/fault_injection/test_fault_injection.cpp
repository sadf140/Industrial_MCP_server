#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/mcp/mcp_server.hpp"
#include "industrial_mcp/reliability/device_connection_manager.hpp"

#include <cassert>
#include <iostream>

using namespace industrial_mcp;

namespace {

AppConfig make_fault_config() {
    return ConfigLoader::load_json(Json::parse(R"({
        "server":{"name":"industrial-mcp-server","read_only":false},
        "http":{"host":"127.0.0.1","port":0},
        "security":{"enabled":true,"default_role":"administrator"},
        "opcua":{"write_enabled":true},
        "reliability":{"max_retry_count":0,"circuit_failure_threshold":1,"circuit_cooldown_ms":0},
        "cache":{"enabled":false},
        "devices":[
            {"id":"pump-1","endpoint":"mock://pump-1","variables":[{"name":"temperature","node_id":"ns=2;s=T","data_type":"Double","mock_value":82.5,"writable":true,"min":0,"max":120}]},
            {"id":"disabled","enabled":false,"endpoint":"mock://disabled","variables":[{"name":"temperature","node_id":"ns=2;s=D","data_type":"Double","mock_value":1.0}]}
        ]
    })"), ".");
}

Json call(McpServer& server, const std::string& name, Json arguments) {
    Json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", name}, {"arguments", std::move(arguments)}}},
    };
    const auto response = server.handle_message(request);
    assert(response.has_value());
    return response->at("result");
}

void test_faults() {
    auto config = make_fault_config();
    McpServer server(config);

    const auto disabled = call(server, "read_node", {{"device_id", "disabled"}, {"variable", "temperature"}});
    assert(disabled.at("isError").get<bool>());
    assert(disabled.at("structuredContent").at("error").get<std::string>() == "DEVICE_DISABLED");

    const auto out_of_range = call(server, "write_node", {{"device_id", "pump-1"}, {"variable", "temperature"}, {"value", 999.0}});
    assert(out_of_range.at("isError").get<bool>());
    assert(out_of_range.at("structuredContent").at("error").get<std::string>() == "INVALID_ARGUMENT");

    const auto unknown_operation = call(server, "confirm_device_action", {{"operation_id", "missing-op"}});
    assert(unknown_operation.at("isError").get<bool>());
    assert(unknown_operation.at("structuredContent").at("error").get<std::string>() == "INVALID_ARGUMENT");
}

void test_circuit_breaker_failure_and_recovery() {
    auto config = make_fault_config();
    DeviceConnectionManager manager(config);
    const auto* device = find_device(config, "pump-1");
    assert(device != nullptr);
    const auto* variable = find_variable(*device, "temperature");
    assert(variable != nullptr);

    const auto failed = manager.read_node(*device, nullptr, "ns=2;s=missing", config.opcua);
    assert(!failed.ok);
    assert(to_string(manager.snapshot(*device).circuit_state) == "Open");

    const auto recovered = manager.read_node(*device, variable, variable->node_id, config.opcua);
    assert(recovered.ok);
    assert(to_string(manager.snapshot(*device).circuit_state) == "Closed");
}

} // namespace

int main() {
    test_faults();
    test_circuit_breaker_failure_and_recovery();
    std::cout << "industrial_mcp_fault_injection_tests passed\n";
    return 0;
}
