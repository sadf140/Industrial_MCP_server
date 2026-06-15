#include "industrial_mcp/reliability/device_connection_manager.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace industrial_mcp {
namespace {

long long elapsed_ms_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    ).count();
}

OpcUaReadResult disabled_read_result(const DeviceConfig& device) {
    OpcUaReadResult result;
    result.timestamp = now_utc_iso8601();
    result.quality = "BadDeviceDisabled";
    result.status_code = "BadDeviceDisabled";
    result.error = "device is disabled: " + device.id;
    return result;
}

OpcUaReadResult circuit_open_read_result(const DeviceConfig& device) {
    OpcUaReadResult result;
    result.timestamp = now_utc_iso8601();
    result.quality = "CircuitOpen";
    result.status_code = "CircuitOpen";
    result.error = "circuit breaker is open for device: " + device.id;
    return result;
}

OpcUaWriteResult disabled_write_result(const DeviceConfig& device, const VariableConfig& variable, const Json& value) {
    OpcUaWriteResult result;
    result.timestamp = now_utc_iso8601();
    result.value = value;
    result.data_type = variable.data_type;
    result.quality = "BadDeviceDisabled";
    result.status_code = "BadDeviceDisabled";
    result.error = "device is disabled: " + device.id;
    result.error_code = "DEVICE_DISABLED";
    return result;
}

OpcUaWriteResult circuit_open_write_result(const DeviceConfig& device, const VariableConfig& variable, const Json& value) {
    OpcUaWriteResult result;
    result.timestamp = now_utc_iso8601();
    result.value = value;
    result.data_type = variable.data_type;
    result.quality = "CircuitOpen";
    result.status_code = "CircuitOpen";
    result.error = "circuit breaker is open for device: " + device.id;
    result.error_code = "DEVICE_OFFLINE";
    return result;
}

bool any_read_ok(const std::vector<OpcUaReadResult>& reads) {
    return std::any_of(reads.begin(), reads.end(), [](const auto& read) {
        return read.ok;
    });
}

bool all_read_failed(const std::vector<OpcUaReadResult>& reads) {
    return !reads.empty() && std::none_of(reads.begin(), reads.end(), [](const auto& read) {
        return read.ok;
    });
}

std::string first_read_error(const std::vector<OpcUaReadResult>& reads) {
    for (const auto& read : reads) {
        if (!read.ok && !read.error.empty()) return read.error;
    }
    return "all reads failed";
}

} // namespace

RetryPolicy::RetryPolicy(ReliabilityConfig config) : config_(std::move(config)) {}

int RetryPolicy::max_retry_count() const {
    return std::max(0, config_.max_retry_count);
}

int RetryPolicy::delay_ms(int failure_count) const {
    const int initial = std::max(0, config_.backoff_initial_ms);
    const int max_delay = std::max(initial, config_.backoff_max_ms);
    if (initial == 0 || failure_count <= 0) return 0;

    int delay = initial;
    for (int index = 1; index < failure_count; ++index) {
        if (delay >= max_delay / 2) {
            return max_delay;
        }
        delay *= 2;
    }
    return std::min(delay, max_delay);
}

bool CircuitBreaker::allow_request(int cooldown_ms) {
    if (state_ != CircuitState::Open) return true;
    const auto cooldown = std::chrono::milliseconds(std::max(0, cooldown_ms));
    if (std::chrono::steady_clock::now() - opened_at_ >= cooldown) {
        state_ = CircuitState::HalfOpen;
        return true;
    }
    return false;
}

void CircuitBreaker::record_success() {
    state_ = CircuitState::Closed;
}

void CircuitBreaker::record_failure(int consecutive_failures, int failure_threshold) {
    const int threshold = std::max(1, failure_threshold);
    if (consecutive_failures >= threshold) {
        if (state_ != CircuitState::Open) {
            opened_at_ = std::chrono::steady_clock::now();
        }
        state_ = CircuitState::Open;
        return;
    }
    if (state_ == CircuitState::HalfOpen) {
        opened_at_ = std::chrono::steady_clock::now();
        state_ = CircuitState::Open;
        return;
    }
    state_ = CircuitState::Closed;
}

CircuitState CircuitBreaker::state() const {
    return state_;
}

DeviceConnectionManager::DeviceConnectionManager(const AppConfig& config)
    : config_(config), retry_policy_(config.reliability) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& device : config_.devices) {
        auto& state = states_[device.id];
        state.connection_state = device.enabled ? DeviceConnectionState::Disconnected : DeviceConnectionState::Disabled;
        state.online = false;
        connectors_[device.id] = make_connector_for_device(device);
    }
}

DeviceConnectionManager::ManagedState& DeviceConnectionManager::state_for_locked(const DeviceConfig& device) {
    auto& state = states_[device.id];
    if (!device.enabled) {
        state.connection_state = DeviceConnectionState::Disabled;
        state.online = false;
    }
    return state;
}

DeviceConnectionManager::ManagedState DeviceConnectionManager::state_snapshot_locked(const DeviceConfig& device) const {
    const auto found = states_.find(device.id);
    if (found == states_.end()) {
        ManagedState state;
        state.connection_state = device.enabled ? DeviceConnectionState::Disconnected : DeviceConnectionState::Disabled;
        return state;
    }
    return found->second;
}

bool DeviceConnectionManager::request_allowed_locked(ManagedState& state) {
    return state.circuit.allow_request(config_.reliability.circuit_cooldown_ms);
}

IIndustrialConnector* DeviceConnectionManager::connector_for(const DeviceConfig& device) const {
    const auto found = connectors_.find(device.id);
    if (found == connectors_.end()) return nullptr;
    return found->second.get();
}

void DeviceConnectionManager::mark_connecting(const DeviceConfig& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = state_for_locked(device);
    if (!device.enabled) return;
    state.connection_state = state.consecutive_failures > 0 ? DeviceConnectionState::Reconnecting : DeviceConnectionState::Connecting;
}

void DeviceConnectionManager::record_success(const DeviceConfig& device, int attempts, long long latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = state_for_locked(device);
    if (!device.enabled) return;
    state.online = true;
    state.connection_state = DeviceConnectionState::Connected;
    state.consecutive_failures = 0;
    state.last_success_at = now_utc_iso8601();
    state.last_error.clear();
    state.attempts = attempts;
    state.latency_ms = latency_ms;
    state.next_retry_delay_ms = 0;
    state.circuit.record_success();
}

void DeviceConnectionManager::record_failure(const DeviceConfig& device,
                                             const std::string& error,
                                             int attempts,
                                             long long latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = state_for_locked(device);
    if (!device.enabled) return;
    if (state.online) {
        ++state.reconnect_count;
    } else if (state.consecutive_failures == 0) {
        ++state.reconnect_count;
    }
    state.online = false;
    ++state.consecutive_failures;
    state.last_error_at = now_utc_iso8601();
    state.last_error = error;
    state.attempts = attempts;
    state.latency_ms = latency_ms;
    state.next_retry_delay_ms = retry_policy_.delay_ms(state.consecutive_failures);
    state.circuit.record_failure(state.consecutive_failures, config_.reliability.circuit_failure_threshold);
    state.connection_state = state.circuit.state() == CircuitState::Open
        ? DeviceConnectionState::Faulted
        : DeviceConnectionState::Reconnecting;
}

DeviceConnectionHealth DeviceConnectionManager::health_from_state(const DeviceConfig& device, const ManagedState& state) const {
    DeviceConnectionHealth health;
    health.device_id = device.id;
    health.enabled = device.enabled;
    health.protocol = device.protocol;
    health.endpoint = device.endpoint;
    health.connection_state = device.enabled ? state.connection_state : DeviceConnectionState::Disabled;
    health.online = device.enabled && state.online;
    health.consecutive_failures = state.consecutive_failures;
    health.reconnect_count = state.reconnect_count;
    health.last_success_at = state.last_success_at;
    health.last_error_at = state.last_error_at;
    health.last_read_time = state.last_success_at.empty() ? state.last_error_at : state.last_success_at;
    health.last_error = state.last_error;
    health.circuit_state = state.circuit.state();
    health.next_retry_delay_ms = state.next_retry_delay_ms;
    health.attempts = state.attempts;
    health.latency_ms = state.latency_ms;
    return health;
}

DeviceConnectionHealth DeviceConnectionManager::snapshot(const DeviceConfig& device) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_from_state(device, state_snapshot_locked(device));
}

DeviceConnectionHealth DeviceConnectionManager::get_health(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) {
    if (!device.enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        return health_from_state(device, state_for_locked(device));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = state_for_locked(device);
        if (!request_allowed_locked(state)) {
            state.connection_state = DeviceConnectionState::Faulted;
            state.online = false;
            return health_from_state(device, state);
        }
    }

    auto* connector = connector_for(device);
    if (connector == nullptr) {
        record_failure(device, "connector is not configured for device: " + device.id, 0, -1);
        return snapshot(device);
    }

    mark_connecting(device);
    const auto status = connector->get_status(device, runtime);
    if (status.online) {
        record_success(device, status.attempts, status.latency_ms);
    } else {
        record_failure(device, status.error.empty() ? status.last_error : status.error, status.attempts, status.latency_ms);
    }
    return snapshot(device);
}

Json DeviceConnectionManager::health_json(const DeviceConfig& device) const {
    return device_connection_health_to_json(snapshot(device));
}

Json DeviceConnectionManager::all_health_json() const {
    Json devices = Json::array();
    for (const auto& device : config_.devices) {
        devices.push_back(health_json(device));
    }
    return devices;
}

OpcUaReadResult DeviceConnectionManager::read_node(const DeviceConfig& device,
                                                   const VariableConfig* variable,
                                                   const std::string& node_id,
                                                   const OpcUaRuntimeConfig& runtime) {
    if (!device.enabled) return disabled_read_result(device);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = state_for_locked(device);
        if (!request_allowed_locked(state)) {
            state.connection_state = DeviceConnectionState::Faulted;
            state.online = false;
            return circuit_open_read_result(device);
        }
    }

    OpcUaReadResult result;
    auto* connector = connector_for(device);
    if (connector == nullptr) {
        result = circuit_open_read_result(device);
        result.status_code = "ConnectorNotFound";
        result.quality = "ConnectorNotFound";
        result.error = "connector is not configured for device: " + device.id;
        record_failure(device, result.error, 0, -1);
        return result;
    }
    const int max_attempts = retry_policy_.max_retry_count() + 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        mark_connecting(device);
        const auto started = std::chrono::steady_clock::now();
        result = connector->read_node(device, variable, node_id, runtime);
        if (result.ok) {
            record_success(device, result.attempts, elapsed_ms_since(started));
            return result;
        }
        record_failure(device, result.error, result.attempts, elapsed_ms_since(started));
        if (attempt < max_attempts) {
            const auto delay = retry_policy_.delay_ms(attempt);
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return result;
}

std::vector<OpcUaReadResult> DeviceConnectionManager::read_nodes(const DeviceConfig& device,
                                                                 const std::vector<const VariableConfig*>& variables,
                                                                 const OpcUaRuntimeConfig& runtime) {
    if (!device.enabled) {
        return std::vector<OpcUaReadResult>(variables.size(), disabled_read_result(device));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = state_for_locked(device);
        if (!request_allowed_locked(state)) {
            state.connection_state = DeviceConnectionState::Faulted;
            state.online = false;
            return std::vector<OpcUaReadResult>(variables.size(), circuit_open_read_result(device));
        }
    }

    std::vector<OpcUaReadResult> reads;
    auto* connector = connector_for(device);
    if (connector == nullptr) {
        auto result = circuit_open_read_result(device);
        result.status_code = "ConnectorNotFound";
        result.quality = "ConnectorNotFound";
        result.error = "connector is not configured for device: " + device.id;
        record_failure(device, result.error, 0, -1);
        return std::vector<OpcUaReadResult>(variables.size(), result);
    }
    const int max_attempts = retry_policy_.max_retry_count() + 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        mark_connecting(device);
        const auto started = std::chrono::steady_clock::now();
        reads = connector->read_nodes(device, variables, runtime);
        if (any_read_ok(reads)) {
            const int attempts = reads.empty() ? 0 : reads.front().attempts;
            record_success(device, attempts, elapsed_ms_since(started));
            return reads;
        }
        record_failure(device, first_read_error(reads), reads.empty() ? 0 : reads.front().attempts, elapsed_ms_since(started));
        if (attempt < max_attempts && all_read_failed(reads)) {
            const auto delay = retry_policy_.delay_ms(attempt);
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return reads;
}

OpcUaWriteResult DeviceConnectionManager::write_node(const DeviceConfig& device,
                                                     const VariableConfig& variable,
                                                     const Json& value,
                                                     const OpcUaRuntimeConfig& runtime) {
    if (!device.enabled) return disabled_write_result(device, variable, value);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = state_for_locked(device);
        if (!request_allowed_locked(state)) {
            state.connection_state = DeviceConnectionState::Faulted;
            state.online = false;
            return circuit_open_write_result(device, variable, value);
        }
    }

    auto* connector = connector_for(device);
    if (connector == nullptr) {
        auto result = circuit_open_write_result(device, variable, value);
        result.status_code = "ConnectorNotFound";
        result.quality = "ConnectorNotFound";
        result.error = "connector is not configured for device: " + device.id;
        result.error_code = "DEVICE_OFFLINE";
        record_failure(device, result.error, 0, -1);
        return result;
    }

    mark_connecting(device);
    const auto write = connector->write_node(device, variable, value, runtime);
    if (write.ok) {
        record_success(device, write.attempts, write.latency_ms);
    } else {
        record_failure(device, write.error, write.attempts, write.latency_ms);
    }
    return write;
}

DeviceConnectionHealth DeviceConnectionManager::refresh_device(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) {
    return get_health(device, runtime);
}

std::string to_string(DeviceConnectionState state) {
    switch (state) {
        case DeviceConnectionState::Disabled:
            return "Disabled";
        case DeviceConnectionState::Disconnected:
            return "Disconnected";
        case DeviceConnectionState::Connecting:
            return "Connecting";
        case DeviceConnectionState::Connected:
            return "Connected";
        case DeviceConnectionState::Reconnecting:
            return "Reconnecting";
        case DeviceConnectionState::Faulted:
            return "Faulted";
    }
    return "Disconnected";
}

std::string to_string(CircuitState state) {
    switch (state) {
        case CircuitState::Closed:
            return "Closed";
        case CircuitState::Open:
            return "Open";
        case CircuitState::HalfOpen:
            return "HalfOpen";
    }
    return "Closed";
}

Json device_connection_health_to_json(const DeviceConnectionHealth& health) {
    return {
        {"device_id", health.device_id},
        {"enabled", health.enabled},
        {"protocol", health.protocol},
        {"endpoint", health.endpoint},
        {"connection_state", to_string(health.connection_state)},
        {"session_state", to_string(health.connection_state)},
        {"online", health.online},
        {"consecutive_failures", health.consecutive_failures},
        {"disconnect_count", health.reconnect_count},
        {"reconnect_count", health.reconnect_count},
        {"last_success_at", health.last_success_at},
        {"last_error_at", health.last_error_at},
        {"last_read_time", health.last_read_time},
        {"last_error", health.last_error},
        {"error", health.last_error},
        {"circuit_state", to_string(health.circuit_state)},
        {"next_retry_delay_ms", health.next_retry_delay_ms},
        {"attempts", health.attempts},
        {"latency_ms", health.latency_ms},
        {"read_only", true},
    };
}

} // namespace industrial_mcp
