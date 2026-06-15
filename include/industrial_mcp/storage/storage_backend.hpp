#pragma once

#include "industrial_mcp/mcp/config.hpp"

#include <memory>
#include <string>
#include <vector>

namespace industrial_mcp {

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual bool append_alarm_json(const Json& alarm) = 0;
    virtual std::vector<Json> load_alarm_json(std::size_t* invalid_count = nullptr) = 0;
    virtual bool append_audit_json(const Json& audit) = 0;
    virtual std::string backend_name() const = 0;
};

std::unique_ptr<StorageBackend> make_storage_backend(const StorageConfig& storage,
                                                     const std::string& alarm_log_path,
                                                     const std::string& audit_log_path);

bool sqlite_storage_compiled();

} // namespace industrial_mcp
