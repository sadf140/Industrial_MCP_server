#pragma once

#include "industrial_mcp/json.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace industrial_mcp {

struct AlarmRecord {
    std::string alarm_id;
    std::string timestamp;
    std::string device_id;
    std::string level;
    std::string severity;
    std::string code;
    std::string message;
    std::string state;
    std::string source;
    std::string source_node;
    std::optional<double> value;
    std::optional<double> threshold;
    bool acknowledged = false;
};

struct AlarmQuery {
    std::string device_id;
    std::string start_time;
    std::string end_time;
    std::string level;
    std::string severity;
    std::string keyword;
    std::size_t limit = 100;
};

class AlarmStore {
public:
    explicit AlarmStore(std::string path);

    bool append(AlarmRecord alarm) const;
    bool acknowledge(const std::string& alarm_id,
                     const std::string& device_id,
                     const std::string& user_id,
                     const std::string& message = {}) const;
    std::vector<AlarmRecord> query(const AlarmQuery& query) const;
    Json query_json(const AlarmQuery& query) const;
    Json analyze_json(const AlarmQuery& query) const;

private:
    std::string path_;
    mutable std::mutex mutex_;

    std::vector<AlarmRecord> load_all(std::size_t* invalid_count = nullptr) const;
};

Json alarm_to_json(const AlarmRecord& alarm);

} // namespace industrial_mcp
