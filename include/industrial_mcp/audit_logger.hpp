#pragma once

#include "industrial_mcp/json.hpp"

#include <mutex>
#include <string>

namespace industrial_mcp {

class AuditLogger {
public:
    explicit AuditLogger(std::string path);

    void record_tool_call(const std::string& tool_name,
                          const std::string& device_id,
                          bool ok,
                          long long elapsed_ms,
                          const std::string& error_code,
                          const Json& arguments);

private:
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace industrial_mcp
