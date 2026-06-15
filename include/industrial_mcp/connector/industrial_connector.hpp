#pragma once

#include "industrial_mcp/connector/opcua_client.hpp"

#include <memory>
#include <string>
#include <vector>

namespace industrial_mcp {

class IIndustrialConnector {
public:
    virtual ~IIndustrialConnector() = default;

    virtual OpcUaReadResult read_node(const DeviceConfig& device,
                                      const VariableConfig* variable,
                                      const std::string& node_id,
                                      const OpcUaRuntimeConfig& runtime) = 0;
    virtual std::vector<OpcUaReadResult> read_nodes(const DeviceConfig& device,
                                                    const std::vector<const VariableConfig*>& variables,
                                                    const OpcUaRuntimeConfig& runtime) = 0;
    virtual OpcUaWriteResult write_node(const DeviceConfig& device,
                                        const VariableConfig& variable,
                                        const Json& value,
                                        const OpcUaRuntimeConfig& runtime) = 0;
    virtual DeviceStatus get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) = 0;
};

class OpcUaConnector final : public IIndustrialConnector {
public:
    OpcUaReadResult read_node(const DeviceConfig& device,
                              const VariableConfig* variable,
                              const std::string& node_id,
                              const OpcUaRuntimeConfig& runtime) override;
    std::vector<OpcUaReadResult> read_nodes(const DeviceConfig& device,
                                            const std::vector<const VariableConfig*>& variables,
                                            const OpcUaRuntimeConfig& runtime) override;
    OpcUaWriteResult write_node(const DeviceConfig& device,
                                const VariableConfig& variable,
                                const Json& value,
                                const OpcUaRuntimeConfig& runtime) override;
    DeviceStatus get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) override;

private:
    OpcUaClient client_;
};

class MockConnector final : public IIndustrialConnector {
public:
    OpcUaReadResult read_node(const DeviceConfig& device,
                              const VariableConfig* variable,
                              const std::string& node_id,
                              const OpcUaRuntimeConfig& runtime) override;
    std::vector<OpcUaReadResult> read_nodes(const DeviceConfig& device,
                                            const std::vector<const VariableConfig*>& variables,
                                            const OpcUaRuntimeConfig& runtime) override;
    OpcUaWriteResult write_node(const DeviceConfig& device,
                                const VariableConfig& variable,
                                const Json& value,
                                const OpcUaRuntimeConfig& runtime) override;
    DeviceStatus get_status(const DeviceConfig& device, const OpcUaRuntimeConfig& runtime) override;
};

std::unique_ptr<IIndustrialConnector> make_connector_for_device(const DeviceConfig& device);

} // namespace industrial_mcp
