#pragma once

#include "industrial_mcp/mcp/config.hpp"
#include "industrial_mcp/connector/industrial_connector.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace industrial_mcp {

enum class DeviceConnectionState {
    Disabled,
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Faulted,
};

enum class CircuitState {
    Closed,
    Open,
    HalfOpen,
};

struct DeviceConnectionHealth {
    std::string device_id;
    bool enabled = true;
    std::string protocol = "opcua";
    std::string endpoint;
    DeviceConnectionState connection_state = DeviceConnectionState::Disconnected;
    bool online = false;
    int consecutive_failures = 0;
    int reconnect_count = 0;
    std::string last_success_at;
    std::string last_error_at;
    std::string last_read_time;
    std::string last_error;
    CircuitState circuit_state = CircuitState::Closed;
    int next_retry_delay_ms = 0;
    int attempts = 0;
    long long latency_ms = -1;
};

class RetryPolicy {
public:
    explicit RetryPolicy(ReliabilityConfig config);

    int max_retry_count() const;
    int delay_ms(int failure_count) const;

private:
    ReliabilityConfig config_;
};

class CircuitBreaker {
public:
    bool allow_request(int cooldown_ms);
    void record_success();
    void record_failure(int consecutive_failures, int failure_threshold);

    CircuitState state() const;

private:
    CircuitState state_ = CircuitState::Closed;
    std::chrono::steady_clock::time_point opened_at_{};
};

class DeviceConnectionManager {
public:
    explicit DeviceConnectionManager(const AppConfig& config);

    DeviceConnectionHealth snapshot(const DeviceConfig& device) const;
    DeviceConnectionHealth get_health(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime);
    Json health_json(const DeviceConfig& device) const;
    Json all_health_json() const;

    OpcUaReadResult read_node(const DeviceConfig& device,
                              const VariableConfig* variable,
                              const std::string& node_id,
                              const OpcUaRuntimeConfig& runtime);
    std::vector<OpcUaReadResult> read_nodes(const DeviceConfig& device,
                                            const std::vector<const VariableConfig*>& variables,
                                            const OpcUaRuntimeConfig& runtime);
    OpcUaWriteResult write_node(const DeviceConfig& device,
                                const VariableConfig& variable,
                                const Json& value,
                                const OpcUaRuntimeConfig& runtime);
    OpcUaMethodResult call_method(const DeviceConfig& device,
                                  const MethodConfig& method,
                                  const Json& arguments,
                                  const OpcUaRuntimeConfig& runtime);
    DeviceConnectionHealth refresh_device(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime);
    void reset();

private:
    struct ManagedState {
        DeviceConnectionState connection_state = DeviceConnectionState::Disconnected;
        bool online = false;
        int consecutive_failures = 0;
        int reconnect_count = 0;
        std::string last_success_at;
        std::string last_error_at;
        std::string last_error;
        int next_retry_delay_ms = 0;
        int attempts = 0;
        long long latency_ms = -1;
        CircuitBreaker circuit;
    };

    const AppConfig& config_;
    RetryPolicy retry_policy_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ManagedState> states_;
    std::unordered_map<std::string, std::unique_ptr<IIndustrialConnector>> connectors_;

    ManagedState& state_for_locked(const DeviceConfig& device);
    ManagedState state_snapshot_locked(const DeviceConfig& device) const;
    IIndustrialConnector* connector_for(const DeviceConfig& device) const;
    bool request_allowed_locked(ManagedState& state);
    void mark_connecting(const DeviceConfig& device);
    void record_success(const DeviceConfig& device, int attempts, long long latency_ms);
    void record_failure(const DeviceConfig& device, const std::string& error, int attempts, long long latency_ms);
    DeviceConnectionHealth health_from_state(const DeviceConfig& device, const ManagedState& state) const;
};

std::string to_string(DeviceConnectionState state);
std::string to_string(CircuitState state);
Json device_connection_health_to_json(const DeviceConnectionHealth& health);

} // namespace industrial_mcp
