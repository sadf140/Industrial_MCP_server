#pragma once

#include "industrial_mcp/json.hpp"

#include <mutex>
#include <string>

namespace industrial_mcp {

struct AuditRecord {
    std::string request_id;
    std::string user_id;
    std::string client_id;
    std::string tool_name;
    std::string device_id;
    std::string target_node;
    Json old_value;
    Json new_value;
    std::string result;
    std::string error_code;
    long long elapsed_ms = 0;
    Json arguments = Json::object();
    bool read_only = true;
};

class AuditLogger {
public:
    explicit AuditLogger(std::string path);

    void record(const AuditRecord& record);

    void record_tool_call(const std::string& tool_name,
                          const std::string& device_id,
                          bool ok,
                          long long elapsed_ms,
                          const std::string& error_code,
                          const Json& arguments,
                          bool read_only = true);

private:
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace industrial_mcp
