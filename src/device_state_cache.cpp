#include "industrial_mcp/device_state_cache.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace industrial_mcp {
namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_numeric(const Json& value) {
    return value.is_number();
}

std::optional<double> numeric_value(const Json& value) {
    if (!is_numeric(value)) return std::nullopt;
    return value.get<double>();
}

std::string fixed_status(bool all_ok, bool any_ok) {
    if (all_ok) return "OK";
    if (any_ok) return "DEGRADED";
    return "ERROR";
}

std::string make_alarm_id(const std::string& device_id, const std::string& code, const std::string& timestamp) {
    return device_id + ":" + code + ":" + timestamp;
}

std::string threshold_message(const std::string& variable, const std::string& direction, double value, double threshold) {
    std::ostringstream out;
    out << variable << " is " << direction << " threshold; value=" << value << ", threshold=" << threshold;
    return out.str();
}

} // namespace

DeviceStateCache::DeviceStateCache(const AppConfig& config, OpcUaClient& opcua, const AlarmStore& alarms)
    : config_(config), opcua_(opcua), alarms_(alarms) {}

DeviceStateCache::~DeviceStateCache() {
    stop();
}

bool DeviceStateCache::enabled() const {
    return config_.cache.enabled;
}

void DeviceStateCache::start() {
    if (!config_.cache.enabled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    stop_requested_ = false;
    worker_ = std::thread([this]() { worker_loop(); });
}

void DeviceStateCache::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void DeviceStateCache::worker_loop() {
    refresh_once();
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto interval = std::chrono::milliseconds(config_.cache.poll_interval_ms > 0 ? config_.cache.poll_interval_ms : 2000);
        if (cv_.wait_for(lock, interval, [this]() { return stop_requested_; })) {
            return;
        }
        lock.unlock();
        refresh_once();
    }
}

void DeviceStateCache::refresh_once() {
    if (!config_.cache.enabled) return;

    for (const auto& device : config_.devices) {
        std::vector<const VariableConfig*> variable_refs;
        variable_refs.reserve(device.variables.size());
        for (const auto& [_, variable] : device.variables) {
            variable_refs.push_back(&variable);
        }

        const auto reads = opcua_.read_nodes(device, variable_refs, config_.opcua);
        DeviceState state;
        state.device_id = device.id;
        state.last_update_time = now_utc_iso8601();
        state.updated_at = std::chrono::steady_clock::now();

        bool any_ok = false;
        bool all_ok = !reads.empty();
        std::string first_error;
        for (std::size_t index = 0; index < variable_refs.size(); ++index) {
            const auto* variable = variable_refs[index];
            const auto& read = reads.at(index);
            if (variable == nullptr) continue;

            CachedVariableState cached;
            cached.name = variable->name;
            cached.node_id = variable->node_id;
            cached.data_type = read.data_type.empty() ? variable->data_type : read.data_type;
            cached.unit = variable->unit;
            cached.description = variable->description;
            cached.value = read.value;
            cached.ok = read.ok;
            cached.quality = read.quality;
            cached.status_code = read.status_code;
            cached.error = read.error;
            cached.timestamp = read.timestamp.empty() ? state.last_update_time : read.timestamp;
            state.variables[variable->name] = cached;

            any_ok = any_ok || read.ok;
            all_ok = all_ok && read.ok;
            if (!read.ok && first_error.empty()) {
                first_error = read.error.empty() ? read.status_code : read.error;
            }

            const auto variable_name = lower_copy(variable->name);
            if (read.ok) {
                if (variable_name == "temperature") state.temperature = numeric_value(read.value);
                if (variable_name == "current") state.current = numeric_value(read.value);
                if (variable_name == "voltage") state.voltage = numeric_value(read.value);
                if (variable_name == "running" && read.value.is_boolean()) state.running = read.value.get<bool>();
            }
        }

        state.online = any_ok;
        state.stale = false;
        state.status = fixed_status(all_ok, any_ok);
        state.last_error = first_error;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            states_[device.id] = state;
        }

        const bool all_failed = !reads.empty() && !any_ok;
        if (all_failed) {
            const std::string key = device.id + "|__communication";
            const auto timestamp = now_utc_iso8601();
            AlarmRecord alarm;
            alarm.alarm_id = make_alarm_id(device.id, "COMMUNICATION_ERROR", timestamp);
            alarm.timestamp = timestamp;
            alarm.device_id = device.id;
            alarm.level = "ERROR";
            alarm.severity = "ERROR";
            alarm.code = "COMMUNICATION_ERROR";
            alarm.message = first_error.empty() ? "OPC UA communication failed for all configured variables" : first_error;
            alarm.source = "device_state_cache";
            alarm.source_node = device.endpoint;
            append_alarm_if_changed(key, "ERROR:COMMUNICATION_ERROR", std::move(alarm));
        } else {
            clear_alarm_state(device.id + "|__communication");
        }

        for (std::size_t index = 0; index < variable_refs.size(); ++index) {
            const auto* variable = variable_refs[index];
            const auto& read = reads.at(index);
            if (variable == nullptr) continue;

            const std::string key = device.id + "|" + variable->name;
            if (!read.ok) {
                if (!all_failed) {
                    const auto timestamp = now_utc_iso8601();
                    AlarmRecord alarm;
                    alarm.alarm_id = make_alarm_id(device.id, "OPCUA_READ_FAILED", timestamp);
                    alarm.timestamp = timestamp;
                    alarm.device_id = device.id;
                    alarm.level = "ERROR";
                    alarm.severity = "ERROR";
                    alarm.code = "OPCUA_READ_FAILED";
                    alarm.message = read.error.empty() ? "OPC UA variable read failed" : read.error;
                    alarm.source = "device_state_cache";
                    alarm.source_node = variable->node_id;
                    append_alarm_if_changed(key, "ERROR:OPCUA_READ_FAILED", std::move(alarm));
                }
                continue;
            }

            const auto value = numeric_value(read.value);
            if (!value) {
                clear_alarm_state(key);
                continue;
            }

            std::string level;
            std::string code;
            std::string direction;
            std::optional<double> threshold;
            if (variable->alarm_max && *value > *variable->alarm_max) {
                level = "CRITICAL";
                code = "VARIABLE_ABOVE_ALARM_MAX";
                direction = "above alarm_max";
                threshold = variable->alarm_max;
            } else if (variable->alarm_min && *value < *variable->alarm_min) {
                level = "CRITICAL";
                code = "VARIABLE_BELOW_ALARM_MIN";
                direction = "below alarm_min";
                threshold = variable->alarm_min;
            } else if (variable->warn_max && *value > *variable->warn_max) {
                level = "WARN";
                code = "VARIABLE_ABOVE_WARN_MAX";
                direction = "above warn_max";
                threshold = variable->warn_max;
            } else if (variable->warn_min && *value < *variable->warn_min) {
                level = "WARN";
                code = "VARIABLE_BELOW_WARN_MIN";
                direction = "below warn_min";
                threshold = variable->warn_min;
            }

            if (level.empty() || !threshold) {
                clear_alarm_state(key);
                continue;
            }

            const auto timestamp = now_utc_iso8601();
            AlarmRecord alarm;
            alarm.alarm_id = make_alarm_id(device.id, code, timestamp);
            alarm.timestamp = timestamp;
            alarm.device_id = device.id;
            alarm.level = level;
            alarm.severity = level;
            alarm.code = code;
            alarm.message = threshold_message(variable->name, direction, *value, *threshold);
            alarm.source = "device_state_cache";
            alarm.source_node = variable->node_id;
            alarm.value = *value;
            alarm.threshold = *threshold;
            append_alarm_if_changed(key, level + ":" + code, std::move(alarm));
        }
    }
}

bool DeviceStateCache::clear_cached_alarm(const std::string& device_id, const std::string& variable) {
    if (device_id.empty()) return false;
    if (variable.empty()) {
        clear_alarm_state(device_id + "|__communication");
        return true;
    }
    clear_alarm_state(device_id + "|" + variable);
    return true;
}

Json DeviceStateCache::state_json(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::object();
    out["enabled"] = config_.cache.enabled;
    out["poll_interval_ms"] = config_.cache.poll_interval_ms;
    out["stale_after_ms"] = config_.cache.stale_after_ms;
    out["timestamp"] = now_utc_iso8601();
    out["read_only"] = true;

    if (!device_id.empty()) {
        const auto found = states_.find(device_id);
        if (found == states_.end()) {
            out["count"] = 0;
            out["device"] = nullptr;
            out["error"] = find_device(config_, device_id) == nullptr ? "DEVICE_NOT_FOUND" : "NO_CACHED_STATE";
            return out;
        }
        out["count"] = 1;
        out["device"] = state_to_json_locked(found->second);
        return out;
    }

    Json devices = Json::array();
    for (const auto& device : config_.devices) {
        const auto found = states_.find(device.id);
        if (found != states_.end()) {
            devices.push_back(state_to_json_locked(found->second));
        } else {
            Json pending = Json::object();
            pending["device_id"] = device.id;
            pending["online"] = false;
            pending["stale"] = true;
            pending["status"] = "INITIALIZING";
            pending["last_update_time"] = "";
            pending["last_error"] = "no cached state collected yet";
            pending["variables"] = Json::object();
            devices.push_back(pending);
        }
    }
    out["count"] = static_cast<int>(devices.size());
    out["devices"] = devices;
    return out;
}

std::optional<DeviceState> DeviceStateCache::state_for(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = states_.find(device_id);
    if (found == states_.end()) return std::nullopt;
    auto state = found->second;
    state.stale = is_stale_locked(state);
    return state;
}

Json DeviceStateCache::state_to_json_locked(const DeviceState& state) const {
    Json variables = Json::object();
    for (const auto& [name, variable] : state.variables) {
        variables[name] = {
            {"name", variable.name},
            {"node_id", variable.node_id},
            {"data_type", variable.data_type},
            {"unit", variable.unit},
            {"description", variable.description},
            {"value", variable.value},
            {"ok", variable.ok},
            {"quality", variable.quality},
            {"status_code", variable.status_code},
            {"error", variable.error},
            {"timestamp", variable.timestamp},
        };
    }

    Json out = Json::object();
    out["device_id"] = state.device_id;
    out["online"] = state.online;
    out["stale"] = is_stale_locked(state);
    out["cache_age_seconds"] = cache_age_seconds_locked(state);
    out["device_connected"] = state.online && !is_stale_locked(state);
    out["status"] = is_stale_locked(state) ? "STALE" : state.status;
    out["last_update_time"] = state.last_update_time;
    out["last_error"] = state.last_error;
    out["temperature"] = state.temperature ? Json(*state.temperature) : Json(nullptr);
    out["current"] = state.current ? Json(*state.current) : Json(nullptr);
    out["voltage"] = state.voltage ? Json(*state.voltage) : Json(nullptr);
    out["running"] = state.running ? Json(*state.running) : Json(nullptr);
    out["variables"] = variables;
    return out;
}

bool DeviceStateCache::is_stale_locked(const DeviceState& state) const {
    if (state.updated_at == std::chrono::steady_clock::time_point{}) return true;
    const auto stale_after = std::chrono::milliseconds(config_.cache.stale_after_ms > 0 ? config_.cache.stale_after_ms : 10000);
    return std::chrono::steady_clock::now() - state.updated_at > stale_after;
}

long long DeviceStateCache::cache_age_seconds_locked(const DeviceState& state) const {
    if (state.updated_at == std::chrono::steady_clock::time_point{}) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - state.updated_at
    ).count();
}

void DeviceStateCache::append_alarm_if_changed(const std::string& key, const std::string& signature, AlarmRecord alarm) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = active_alarm_signatures_.find(key);
        if (found != active_alarm_signatures_.end() && found->second == signature) {
            return;
        }
        active_alarm_signatures_[key] = signature;
    }
    alarms_.append(std::move(alarm));
}

void DeviceStateCache::clear_alarm_state(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_alarm_signatures_.erase(key);
}

} // namespace industrial_mcp
