#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace industrial_mcp {

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const std::string& path)>;

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(const std::string& host, int port, Handler handler);
    void stop();
    bool running() const;

private:
    void serve(std::string host, int port, Handler handler);

    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace industrial_mcp
