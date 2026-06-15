#include "industrial_mcp/alarm/alarm_store.hpp"
#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/observability/observability.hpp"
#include "industrial_mcp/storage/storage_backend.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace industrial_mcp;

namespace {

std::string temp_file_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void test_config_and_metrics() {
    const auto config = ConfigLoader::load_json(Json::parse(R"({
        "server":{"name":"industrial-mcp-server","read_only":true},
        "http":{"host":"127.0.0.1","port":0},
        "observability":{"metrics_enabled":true,"metrics_port":0},
        "storage":{"type":"jsonl"},
        "devices":[{"id":"pump-1","endpoint":"mock://pump-1","variables":[{"name":"temperature","node_id":"ns=2;s=T","data_type":"Double","mock_value":42.0}]}]
    })"), ".");
    assert(config.http.port == 0);
    assert(config.observability.metrics_enabled);
    assert(find_device(config, "pump-1") != nullptr);

    ObservabilityMetrics metrics;
    metrics.record_tool_call("read_node", true, 12);
    metrics.record_tool_call("write_node", false, 24);
    const auto snapshot = metrics.snapshot_json();
    assert(snapshot.at("mcp_requests_total").get<unsigned long long>() == 2);
    assert(snapshot.at("mcp_tool_errors_total").get<unsigned long long>() == 1);
    const auto text = metrics.prometheus_text(Json::array(), Json::object());
    assert(text.find("mcp_requests_total 2") != std::string::npos);
}

void test_storage_jsonl() {
    const auto alarm_path = temp_file_path("industrial_mcp_unit_alarm.jsonl");
    std::remove(alarm_path.c_str());

    StorageConfig storage;
    storage.type = "jsonl";
    AlarmStore store(alarm_path, storage);
    AlarmRecord alarm;
    alarm.timestamp = "2026-06-11T11:00:00Z";
    alarm.device_id = "pump-1";
    alarm.level = "WARN";
    alarm.code = "TEMP_WARN";
    alarm.message = "temperature warning";
    assert(store.append(alarm));

    AlarmQuery query;
    query.device_id = "pump-1";
    const auto alarms = store.query(query);
    assert(alarms.size() == 1);
    assert(alarms.front().code == "TEMP_WARN");

    std::remove(alarm_path.c_str());
}

} // namespace

int main() {
    test_config_and_metrics();
    test_storage_jsonl();
    std::cout << "industrial_mcp_unit_tests passed\n";
    return 0;
}
