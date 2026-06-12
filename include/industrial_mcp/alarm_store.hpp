#pragma once

#include "industrial_mcp/json.hpp"

#include <string>
#include <vector>

namespace industrial_mcp {

struct AlarmRecord {
    std::string timestamp;
    std::string device_id;
    std::string severity;
    std::string code;
    std::string message;
    std::string state;
    std::string source;
    bool acknowledged = false;
};

struct AlarmQuery {
    std::string device_id;
    std::string start_time;
    std::string end_time;
    std::string severity;
    std::string keyword;
    std::size_t limit = 100;
};

class AlarmStore {
public:
    explicit AlarmStore(std::string path);

    std::vector<AlarmRecord> query(const AlarmQuery& query) const;
    Json query_json(const AlarmQuery& query) const;
    Json analyze_json(const AlarmQuery& query) const;

private:
    std::string path_;

    std::vector<AlarmRecord> load_all(std::size_t* invalid_count = nullptr) const;
};

Json alarm_to_json(const AlarmRecord& alarm);

} // namespace industrial_mcp
