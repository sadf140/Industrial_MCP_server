#include "industrial_mcp/config.hpp"
#include "industrial_mcp/json.hpp"
#include "industrial_mcp/mcp_server.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {

constexpr const char* kBuildVersion = "0.4.0-p2";

void emit_log(const std::string& level, const std::string& event, const industrial_mcp::Json& fields = industrial_mcp::Json::object()) {
    auto record = fields.is_object() ? fields : industrial_mcp::Json::object();
    record["timestamp"] = industrial_mcp::now_utc_iso8601();
    record["level"] = level;
    record["event"] = event;
    std::cerr << record.dump() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/config.example.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--version") {
            std::cerr << "industrial-mcp-server " << kBuildVersion << '\n';
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: industrial-mcp-server --config <config.json>\n"
                      << "       industrial-mcp-server --version\n";
            return 0;
        }
    }

    try {
        auto config = industrial_mcp::ConfigLoader::load_file(config_path);
        emit_log("info", "server_starting", {
            {"config_path", config_path},
            {"server_name", config.server.name},
            {"server_version", config.server.version},
            {"device_count", static_cast<int>(config.devices.size())},
            {"read_only", config.server.read_only},
        });
        industrial_mcp::McpServer server(std::move(config));
        server.run(std::cin, std::cout);
        emit_log("info", "server_stopped");
        return 0;
    } catch (const std::exception& ex) {
        emit_log("fatal", "server_failed", {{"error", ex.what()}});
        return 1;
    }
}
