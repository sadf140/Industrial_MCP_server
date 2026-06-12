#include "industrial_mcp/config.hpp"
#include "industrial_mcp/mcp_server.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string config_path = "config/config.example.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: industrial-mcp-server --config <config.json>\n";
            return 0;
        }
    }

    try {
        auto config = industrial_mcp::ConfigLoader::load_file(config_path);
        industrial_mcp::McpServer server(std::move(config));
        server.run(std::cin, std::cout);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
