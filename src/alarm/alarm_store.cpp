#include "industrial_mcp/alarm/alarm_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace industrial_mcp {
namespace {

std::string optional_string(const Json& object, const std::string& key) {
    if (object.is_object() && object.contains(key) && object.at(key).is_string()) {
        return object.at(key).get<std::string>();
    }
    return {};
}

bool optional_bool(const Json& object, const std::string& key) {
    return object.is_object() && object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>();
}

std::optional<double> optional_double(const Json& object, const std::string& key) {
    if (object.is_object() && object.contains(key) && object.at(key).is_number()) {
        return object.at(key).get<double>();
    }
    return std::nullopt;
}

bool valid_iso8601_utc_prefix(const std::string& value) {
    return value.size() >= 20 && value[4] == '-' && value[7] == '-' && value[10] == 'T' &&
           value[13] == ':' && value[16] == ':' && value[19] == 'Z';
}

bool in_range(const std::string& value, const std::string& start, const std::string& end) {
    if (!start.empty() && value < start) return false;
    if (!end.empty() && value > end) return false;
    return true;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lower_copy(haystack).find(lower_copy(needle)) != std::string::npos;
}

std::string canonical_level(const AlarmRecord& alarm) {
    return alarm.level.empty() ? alarm.severity : alarm.level;
}

} // namespace

AlarmStore::AlarmStore(std::string path)
    : path_(std::move(path)), backend_(make_storage_backend(storage_, path_, {})) {}

AlarmStore::AlarmStore(std::string path, StorageConfig storage, std::string audit_path)
    : path_(std::move(path)),
      storage_(std::move(storage)),
      backend_(make_storage_backend(storage_, path_, std::move(audit_path))) {}

std::vector<AlarmRecord> AlarmStore::load_all(std::size_t* invalid_count) const {
    std::vector<AlarmRecord> alarms;
    if (invalid_count != nullptr) *invalid_count = 0;

    const auto records = backend_->load_alarm_json(invalid_count);
    for (const auto& json : records) {
        try {
            AlarmRecord alarm;
            alarm.alarm_id = optional_string(json, "alarm_id");
            alarm.timestamp = optional_string(json, "timestamp");
            alarm.device_id = optional_string(json, "device_id");
            alarm.level = optional_string(json, "level");
            alarm.severity = optional_string(json, "severity");
            if (alarm.level.empty()) alarm.level = alarm.severity;
            if (alarm.severity.empty()) alarm.severity = alarm.level;
            alarm.code = optional_string(json, "code");
            alarm.message = optional_string(json, "message");
            alarm.source_node = optional_string(json, "source_node");
            alarm.value = optional_double(json, "value");
            alarm.threshold = optional_double(json, "threshold");
            alarm.state = optional_string(json, "state");
            alarm.source = optional_string(json, "source");
            alarm.acknowledged = optional_bool(json, "acknowledged");
            if (!alarm.timestamp.empty() && !alarm.device_id.empty() && valid_iso8601_utc_prefix(alarm.timestamp)) {
                alarms.push_back(std::move(alarm));
            } else if (invalid_count != nullptr) {
                ++(*invalid_count);
            }
        } catch (...) {
            if (invalid_count != nullptr) {
                ++(*invalid_count);
            }
        }
    }

    std::sort(alarms.begin(), alarms.end(), [](const AlarmRecord& lhs, const AlarmRecord& rhs) {
        return lhs.timestamp < rhs.timestamp;
    });
    return alarms;
}

bool AlarmStore::append(AlarmRecord alarm) const {
    if (path_.empty()) return false;
    if (alarm.timestamp.empty()) alarm.timestamp = now_utc_iso8601();
    if (alarm.level.empty()) alarm.level = alarm.severity;
    if (alarm.severity.empty()) alarm.severity = alarm.level;
    if (alarm.source.empty()) alarm.source = "device_state_cache";
    if (alarm.state.empty()) alarm.state = "active";
    if (alarm.alarm_id.empty()) {
        alarm.alarm_id = alarm.device_id + ":" + alarm.code + ":" + alarm.timestamp;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return backend_->append_alarm_json(alarm_to_json(alarm));
}

bool AlarmStore::acknowledge(const std::string& alarm_id,
                             const std::string& device_id,
                             const std::string& user_id,
                             const std::string& message) const {
    AlarmRecord alarm;
    alarm.alarm_id = alarm_id.empty() ? device_id + ":ACK:" + now_utc_iso8601() : alarm_id + ":ACK:" + now_utc_iso8601();
    alarm.timestamp = now_utc_iso8601();
    alarm.device_id = device_id;
    alarm.level = "INFO";
    alarm.severity = "INFO";
    alarm.code = "ALARM_ACKNOWLEDGED";
    alarm.message = message.empty() ? "alarm acknowledged by " + user_id : message;
    alarm.state = "acknowledged";
    alarm.source = "mcp_tool";
    alarm.source_node = alarm_id;
    alarm.acknowledged = true;
    return append(std::move(alarm));
}

std::vector<AlarmRecord> AlarmStore::query(const AlarmQuery& query) const {
    auto alarms = load_all();
    std::vector<AlarmRecord> result;
    const auto level_filter = query.level.empty() ? query.severity : query.level;
    for (auto it = alarms.rbegin(); it != alarms.rend(); ++it) {
        const auto& alarm = *it;
        if (!query.device_id.empty() && alarm.device_id != query.device_id) continue;
        if (!level_filter.empty() && lower_copy(canonical_level(alarm)) != lower_copy(level_filter)) continue;
        if (!in_range(alarm.timestamp, query.start_time, query.end_time)) continue;
        if (!contains_case_insensitive(alarm.message + " " + alarm.code + " " + alarm.source_node, query.keyword)) continue;
        result.push_back(alarm);
        if (result.size() >= query.limit) break;
    }
    return result;
}

Json AlarmStore::query_json(const AlarmQuery& query) const {
    std::size_t invalid_count = 0;
    (void)load_all(&invalid_count);
    const auto alarms = this->query(query);
    Json items = Json::array();
    for (const auto& alarm : alarms) {
        items.push_back(alarm_to_json(alarm));
    }

    Json out = Json::object();
    out["count"] = static_cast<int>(alarms.size());
    out["invalid_record_count"] = static_cast<int>(invalid_count);
    out["alarms"] = items;
    if (!alarms.empty()) {
        out["latest_alarm"] = alarm_to_json(alarms.front());
    }
    return out;
}

Json AlarmStore::analyze_json(const AlarmQuery& query) const {
    std::size_t invalid_count = 0;
    (void)load_all(&invalid_count);
    const auto alarms = this->query(query);
    std::map<std::string, int> by_code;
    std::map<std::string, int> by_severity;
    for (const auto& alarm : alarms) {
        by_code[alarm.code]++;
        by_severity[canonical_level(alarm)]++;
    }

    Json frequent = Json::array();
    std::vector<std::pair<std::string, int>> by_code_sorted(by_code.begin(), by_code.end());
    std::sort(by_code_sorted.begin(), by_code_sorted.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) return lhs.second > rhs.second;
        return lhs.first < rhs.first;
    });
    for (const auto& [code, count] : by_code_sorted) {
        Json item = Json::object();
        item["code"] = code;
        item["count"] = count;
        frequent.push_back(item);
    }

    Json severity = Json::object();
    for (const auto& [key, count] : by_severity) {
        severity[key] = count;
    }

    Json timeline = Json::array();
    for (auto it = alarms.rbegin(); it != alarms.rend(); ++it) {
        const auto& alarm = *it;
        Json item = Json::object();
        item["timestamp"] = alarm.timestamp;
        item["code"] = alarm.code;
        item["level"] = canonical_level(alarm);
        item["severity"] = canonical_level(alarm);
        timeline.push_back(item);
    }

    Json out = Json::object();
    out["total"] = static_cast<int>(alarms.size());
    out["invalid_record_count"] = static_cast<int>(invalid_count);
    out["by_severity"] = severity;
    out["frequent_alarm_codes"] = frequent;
    out["timeline"] = timeline;
    if (!alarms.empty()) {
        out["latest_alarm"] = alarm_to_json(alarms.front());
        out["first_alarm"] = alarm_to_json(alarms.back());
    }
    out["summary"] = alarms.empty() ? "no alarms matched the query" : "alarms matched the query; inspect frequent_alarm_codes and timeline";
    return out;
}

Json alarm_to_json(const AlarmRecord& alarm) {
    const auto level = canonical_level(alarm);
    Json out = Json::object();
    out["alarm_id"] = alarm.alarm_id;
    out["timestamp"] = alarm.timestamp;
    out["device_id"] = alarm.device_id;
    out["level"] = level;
    out["severity"] = level;
    out["code"] = alarm.code;
    out["message"] = alarm.message;
    out["source_node"] = alarm.source_node;
    if (alarm.value) out["value"] = *alarm.value;
    if (alarm.threshold) out["threshold"] = *alarm.threshold;
    out["state"] = alarm.state;
    out["source"] = alarm.source;
    out["acknowledged"] = alarm.acknowledged;
    return out;
}

} // namespace industrial_mcp
