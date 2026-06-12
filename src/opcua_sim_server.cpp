#include <open62541pp/node.hpp>
#include <open62541pp/server.hpp>
#include <open62541pp/types.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic_bool g_running{true};

void handle_signal(int) {
    g_running = false;
}

uint16_t parse_port(int argc, char** argv) {
    uint16_t port = 48520;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            const int parsed = std::stoi(argv[++i]);
            if (parsed <= 0 || parsed > 65535) {
                throw std::runtime_error("port must be between 1 and 65535");
            }
            port = static_cast<uint16_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: opcua-sim-server --port <port>\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return port;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const uint16_t port = parse_port(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        opcua::ServerConfig server_config{port};
        server_config.setApplicationName("industrial-mcp-opcua-sim-server");
        opcua::Server server{std::move(server_config)};

        opcua::Node objects{server, opcua::ObjectId::ObjectsFolder};
        auto temperature = objects.addVariable({1, "Pump1.Temperature"}, "Pump1.Temperature");
        temperature.writeValue(opcua::Variant{42.25});
        auto current = objects.addVariable({1, "Pump1.Current"}, "Pump1.Current");
        current.writeValue(opcua::Variant{14.2});
        auto running = objects.addVariable({1, "Pump1.Running"}, "Pump1.Running");
        running.writeValue(opcua::Variant{true});
        auto label = objects.addVariable({1, "Pump1.Label"}, "Pump1.Label");
        label.writeValue(opcua::Variant{std::string{"pump-alpha"}});

        std::cerr << "opcua-sim-server listening on opc.tcp://127.0.0.1:" << port << '\n'
                  << "nodes: ns=1;s=Pump1.Temperature, ns=1;s=Pump1.Current, "
                  << "ns=1;s=Pump1.Running, ns=1;s=Pump1.Label\n";

        while (g_running) {
            server.runIterate();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        server.stop();
        std::cerr << "opcua-sim-server stopped\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
