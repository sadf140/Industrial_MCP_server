#pragma once

#include "industrial_mcp/alarm_store.hpp"
#include "industrial_mcp/config.hpp"
#include "industrial_mcp/opcua_client.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace industrial_mcp {

struct CachedVariableState {
    std::string name;
    std::string node_id;
    std::string data_type;
    std::string unit;
    std::string description;
    Json value;
    bool ok = false;
    std::string quality;
    std::string status_code;
    std::string error;
    std::string timestamp;
};

struct DeviceState {
    std::string device_id;
    bool online = false;
    bool stale = true;
    std::string status = "INITIALIZING";
    std::string last_update_time;
    std::string last_error;
    std::optional<double> temperature;
    std::optional<double> current;
    std::optional<double> voltage;
    std::optional<bool> running;
    std::unordered_map<std::string, CachedVariableState> variables;
    std::chrono::steady_clock::time_point updated_at{};
};

class DeviceStateCache {
public:
    DeviceStateCache(const AppConfig& config, OpcUaClient& opcua, const AlarmStore& alarms);
    ~DeviceStateCache();

    DeviceStateCache(const DeviceStateCache&) = delete;
    DeviceStateCache& operator=(const DeviceStateCache&) = delete;

    void start();
    void stop();
    void refresh_once();
    bool clear_cached_alarm(const std::string& device_id, const std::string& variable = {});

    bool enabled() const;
    Json state_json(const std::string& device_id = {}) const;
    std::optional<DeviceState> state_for(const std::string& device_id) const;

private:
    const AppConfig& config_;
    OpcUaClient& opcua_;
    const AlarmStore& alarms_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
    bool started_ = false;
    std::thread worker_;
    std::unordered_map<std::string, DeviceState> states_;
    std::unordered_map<std::string, std::string> active_alarm_signatures_;

    void worker_loop();
    Json state_to_json_locked(const DeviceState& state) const;
    bool is_stale_locked(const DeviceState& state) const;
    long long cache_age_seconds_locked(const DeviceState& state) const;
    void append_alarm_if_changed(const std::string& key, const std::string& signature, AlarmRecord alarm);
    void clear_alarm_state(const std::string& key);
};

} // namespace industrial_mcp
