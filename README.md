# Industrial MCP Server

Industrial MCP Server 是一个面向工业设备诊断场景的 C++20 MCP 网关。它通过 JSON-RPC 2.0 over stdio 暴露 MCP 工具，让 Codex、VS Code 等 MCP 客户端读取 OPC UA 设备数据、查询设备状态、查看报警历史，并生成适合 LLM 分析的结构化故障诊断上下文。

当前实现以 Linux / Ubuntu 部署为主要目标，同时保持 Windows / MinGW 构建兼容。OPC UA 基础库使用 `open62541pp`，JSON 使用 `nlohmann/json`，构建系统使用 CMake。

## 当前能力

核心 MCP 工具：

- `read_node`：读取配置白名单中的 OPC UA 变量。
- `write_node`：写入配置白名单变量，默认关闭，受只读开关、RBAC、变量白名单和范围约束保护。
- `list_devices`：列出已配置设备、变量和连接状态摘要，不主动访问网络。
- `get_alarm_history`：查询报警历史，支持 `level`，兼容旧 `severity` 参数。
- `diagnose_fault`：基于设备状态缓存、报警历史、通信状态和阈值证据生成 LLM 诊断上下文。
- `get_network_status`：返回设备通信状态、延迟、断连/重连次数、连续失败次数和熔断状态。

增强工具：

- `get_device_state`：读取 MCP Server 内部周期采集缓存，不在本次工具调用中直接访问 OPC UA。
- `get_server_health` / `get_gateway_health`：返回服务、配置、安全、缓存、存储和观测状态。
- `get_device_health`：返回单设备连接状态、缓存状态和最近错误。
- `refresh_device_state`：按需刷新单设备或全部设备缓存。
- `acknowledge_alarm` / `clear_cached_alarm`：报警确认和缓存清理基础能力。
- `prepare_device_action` / `confirm_device_action` / `cancel_device_action`：高风险操作两阶段确认。
- `call_device_method`：调用配置白名单中的 OPC UA Method，默认需要两阶段确认。
- `add_device` / `remove_device` / `enable_device` / `disable_device` / `reload_configuration` / `update_alarm_rule`：运行期内存配置管理工具，默认仅管理员角色可见/可调用。

兼容旧工具名仍保留：`read_opcua_node`、`read_device_snapshot`、`get_device_status`、`query_alarm_logs`、`analyze_alarms`。

## 工程化能力

- MCP 生命周期：`initialize`、`notifications/initialized`、`ping`、`tools/list`、`tools/call`。
- RBAC：默认 `viewer` 只读，`operator` 可执行受控低风险操作，`administrator` 可访问全部工具。
- 工具可见性过滤：默认 `security.hide_unauthorized_tools=true`，普通 LLM 客户端不会看到未授权工具。
- 多设备连接管理：每台设备独立维护 `Disabled / Disconnected / Connecting / Connected / Reconnecting / Faulted` 状态。
- Connector 抽象：`MockConnector` 用于测试和演示，`OpcUaConnector` 包装真实 OPC UA 访问。
- 可靠性：读操作有限重试、指数退避、熔断器 `Closed / Open / HalfOpen`。
- 设备缓存：后台周期采集，输出 `stale`、`cache_age_seconds`、`device_connected` 和 `connection_state`。
- 报警：阈值报警 JSONL 记录，支持 `WARN / CRITICAL / ERROR`，避免同一异常状态重复刷屏。
- 审计：工具调用 JSONL 审计，写操作记录为 `read_only=false`，敏感参数脱敏。
- 可观测性：内置 HTTP 健康接口和 Prometheus text format 指标。
- 存储：默认 JSONL，可选 SQLite3；未编译 SQLite 时自动回退 JSONL。
- 部署资产：`Dockerfile`、`compose.yaml`、`deploy/systemd/industrial-mcp-server.service`、`deploy/prometheus/prometheus.yml`。

## 目录结构

源码已按模块拆分：

```text
include/industrial_mcp/
  alarm/
  connector/
  device/
  diagnosis/
  mcp/
  observability/
  reliability/
  storage/
src/
  alarm/
  connector/
  device/
  diagnosis/
  mcp/
  observability/
  reliability/
  storage/
tests/
  unit/
  integration/
  fault_injection/
```

旧路径头文件仍保留为转发头，例如 `include/industrial_mcp/config.hpp` 会转发到 `include/industrial_mcp/mcp/config.hpp`，用于降低已有代码的迁移风险。

## 构建与测试

Windows / MinGW：

```powershell
cmake -S . -B build-codex-p3 -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON
cmake --build build-codex-p3 --target industrial_mcp_tests --parallel 1
ctest --test-dir build-codex-p3 --output-on-failure
```

Linux / Ubuntu：

```bash
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

拆分测试目标：

```powershell
cmake --build build-codex-p3 --target industrial_mcp_unit_tests --parallel 1
cmake --build build-codex-p3 --target industrial_mcp_integration_tests --parallel 1
cmake --build build-codex-p3 --target industrial_mcp_fault_injection_tests --parallel 1
```

聚合目标 `industrial_mcp_tests` 继续保留，兼容既有验收命令。

## 本地模拟设备

启用 `INDUSTRIAL_MCP_WITH_OPCUA=ON` 后会生成模拟 OPC UA Server：

```powershell
.\build-codex-p3\opcua-sim-server.exe --port 48520
```

Linux：

```bash
./build/opcua-sim-server --port 48520
```

模拟 endpoint：

```text
opc.tcp://127.0.0.1:48520
```

测试节点：

```text
ns=1;s=Pump1.Temperature
ns=1;s=Pump1.Current
ns=1;s=Pump1.Running
ns=1;s=Pump1.Label
```

真实联调配置使用 `config/config.opcua-sim.json`。

## HTTP 健康与指标

服务仍以 stdio MCP 为主协议，同时按配置启动轻量 HTTP 健康/指标接口：

- `/health/live`
- `/health/ready`
- `/health/devices`
- `/metrics`

`/metrics` 只有在 `observability.metrics_enabled=true` 时返回 Prometheus 指标，否则返回禁用说明。

## 安全边界

`write_node` 默认不可用。写入必须同时满足：

- `server.read_only=false`
- `opcua.write_enabled=true`
- 当前角色允许调用 `write_node`
- 目标设备和变量存在于配置白名单
- 目标变量 `writable=true`
- 写入值满足变量 `data_type`、`min`、`max`、`allowed_values`

生产环境建议默认保持只读，并只在受控维护窗口开启写入。

`call_device_method` 只允许调用 `devices[].methods[]` 中声明的方法，不支持 raw `object_id/method_id`。管理类工具只修改当前进程内配置并写审计，服务重启后恢复配置文件状态；`reload_configuration` 会主动丢弃运行期变更并重新读取启动配置文件。

## 当前限制

- HTTP 只提供健康和指标接口，不提供 MCP over HTTP。
- OPC UA Subscription 尚未实现，当前仍使用周期采集和按需读写。
- SQLite 为可选依赖；未检测到 SQLite3 时使用 JSONL。
- 诊断模块生成 LLM 上下文，不在 C++ 内部调用大模型。
- 运行期管理变更不写回 JSON 配置文件，也不写入 SQLite 配置表。

更多客户端配置见 [docs/mcp-client.md](docs/mcp-client.md)，运维部署见 [docs/operations.md](docs/operations.md)，最终验收矩阵见 [docs/final-acceptance-matrix.md](docs/final-acceptance-matrix.md)。
