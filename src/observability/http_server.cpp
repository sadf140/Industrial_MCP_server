#include "industrial_mcp/observability/http_server.hpp"

#include "industrial_mcp/observability/observability.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace industrial_mcp {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket) {
    closesocket(socket);
}
bool socket_startup() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
void socket_cleanup() {
    WSACleanup();
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle socket) {
    close(socket);
}
bool socket_startup() {
    return true;
}
void socket_cleanup() {}
#endif

std::string reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 404:
            return "Not Found";
        case 503:
            return "Service Unavailable";
        default:
            return "OK";
    }
}

std::string parse_path(const std::string& request) {
    const auto first_space = request.find(' ');
    if (first_space == std::string::npos) return "/";
    const auto second_space = request.find(' ', first_space + 1);
    if (second_space == std::string::npos) return "/";
    auto path = request.substr(first_space + 1, second_space - first_space - 1);
    const auto query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    return path.empty() ? "/" : path;
}

void send_response(SocketHandle client, const HttpResponse& response) {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n"
            << "Content-Type: " << response.content_type << "\r\n"
            << "Content-Length: " << response.body.size() << "\r\n"
            << "Connection: close\r\n\r\n";
    const auto header_text = headers.str();
    send(client, header_text.data(), static_cast<int>(header_text.size()), 0);
    if (!response.body.empty()) {
        send(client, response.body.data(), static_cast<int>(response.body.size()), 0);
    }
}

bool bind_address(SocketHandle server, const std::string& host, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    const auto bind_host = host.empty() ? "127.0.0.1" : host;
    if (bind_host == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_host.c_str(), &address.sin_addr) != 1) {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    return bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
}

} // namespace

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(const std::string& host, int port, Handler handler) {
    if (running_.load()) return true;
    if (port <= 0 || !handler) return false;
    running_ = true;
    worker_ = std::thread([this, host, port, handler = std::move(handler)]() mutable {
        serve(host, port, std::move(handler));
    });
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool HttpServer::running() const {
    return running_.load();
}

void HttpServer::serve(std::string host, int port, Handler handler) {
    if (!socket_startup()) {
        emit_structured_log("warn", "http_socket_startup_failed", {{"port", port}});
        running_ = false;
        return;
    }

    SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == kInvalidSocket) {
        emit_structured_log("warn", "http_socket_create_failed", {{"port", port}});
        running_ = false;
        socket_cleanup();
        return;
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (!bind_address(server, host, port) || listen(server, 16) != 0) {
        emit_structured_log("warn", "http_bind_failed", {{"host", host}, {"port", port}});
        close_socket(server);
        running_ = false;
        socket_cleanup();
        return;
    }

    emit_structured_log("info", "http_server_started", {{"host", host}, {"port", port}});
    while (running_.load()) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(server, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const auto ready = select(static_cast<int>(server + 1), &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        SocketHandle client = accept(server, nullptr, nullptr);
        if (client == kInvalidSocket) continue;

        char buffer[2048] = {};
        const auto received = recv(client, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
        if (received > 0) {
            const auto path = parse_path(std::string(buffer, static_cast<std::size_t>(received)));
            send_response(client, handler(path));
        }
        close_socket(client);
    }

    close_socket(server);
    socket_cleanup();
    emit_structured_log("info", "http_server_stopped", {{"host", host}, {"port", port}});
}

} // namespace industrial_mcp
