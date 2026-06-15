# 最终验收矩阵

本矩阵用于第三阶段第 5 轮完成后的外部验收。构建和测试由外部终端执行。

## 核心 MCP 功能

| 功能 | MCP 工具 | 期望结果 | 状态 |
| --- | --- | --- | --- |
| 读取指定 OPC UA 节点值 | `read_node` | 能按 `device_id` 和 `variable` 读取配置白名单变量 | 已实现，待外部构建验证 |
| 写入指定 OPC UA 节点值 | `write_node` | 默认拒绝；开启只读边界、RBAC 和变量白名单后可写；越界或类型错误返回结构化错误 | 已实现，待外部构建验证 |
| 列出当前接入设备 | `list_devices` | 返回设备 ID、名称、endpoint、变量、阈值和连接状态摘要 | 已实现，待外部构建验证 |
| 查询报警历史 | `get_alarm_history` | 支持设备、等级、时间、关键字和 limit 查询；兼容 `severity` | 已实现，待外部构建验证 |
| 生成故障诊断上下文 | `diagnose_fault` | 返回设备状态、报警、阈值、通信和 LLM 关注点上下文 | 已实现，待外部构建验证 |
| 查询网络状态 | `get_network_status` | 返回在线状态、延迟、断连/重连次数、连续失败、最后错误和熔断状态 | 已实现，待外部构建验证 |

## 第三阶段工程化能力

| 能力 | 验收点 | 状态 |
| --- | --- | --- |
| MCP 生命周期 | 支持 `initialize`、`notifications/initialized`、`ping`、`tools/list`、`tools/call` | 已实现 |
| 统一错误与上下文 | 工具结果包含结构化错误、耗时、设备、数据来源和缓存状态 | 已实现 |
| RBAC | `viewer` 只读，`operator` 受控写，`administrator` 全权限 | 已实现 |
| 写入约束 | 支持 `min`、`max`、`allowed_values` 和变量白名单 | 已实现 |
| 高风险操作确认 | 支持 `prepare_device_action`、`confirm_device_action`、`cancel_device_action` 骨架 | 已实现 |
| 多设备连接管理 | 每台设备独立状态、失败计数、重连计数、最近错误 | 已实现 |
| Connector 抽象 | `MockConnector` 与 `OpcUaConnector` 分离 | 已实现 |
| 可靠性 | 读重试、指数退避、熔断 `Closed/Open/HalfOpen` | 已实现 |
| 缓存降级 | `stale`、`cache_age_seconds`、`device_connected`、`connection_state` | 已实现 |
| 观测指标 | `/metrics` 输出 Prometheus text format | 已实现 |
| 健康接口 | `/health/live`、`/health/ready`、`/health/devices` | 已实现 |
| 存储后端 | JSONL 默认，SQLite3 可选编译并可回退 | 已实现 |
| 目录工程化 | `src`、`include`、`tests` 按模块拆分，旧头文件保留转发 | 已实现 |

## 部署资产

| 资产 | 路径 | 状态 |
| --- | --- | --- |
| Docker 镜像构建 | `Dockerfile` | 已提供 |
| Docker Compose | `compose.yaml` | 已提供 |
| systemd 服务 | `deploy/systemd/industrial-mcp-server.service` | 已提供 |
| Prometheus 配置 | `deploy/prometheus/prometheus.yml` | 已提供 |
| 客户端接入文档 | `docs/mcp-client.md` | 已更新 |
| 运维文档 | `docs/operations.md` | 已更新 |

## 外部测试命令

Windows / MinGW：

```powershell
cd E:\workspace\Industrial_MCP_server
cmake -S . -B build-codex-p3 -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON
cmake --build build-codex-p3 --target industrial_mcp_tests --parallel 1
cmake --build build-codex-p3 --target industrial_mcp_unit_tests --parallel 1
cmake --build build-codex-p3 --target industrial_mcp_integration_tests --parallel 1
cmake --build build-codex-p3 --target industrial_mcp_fault_injection_tests --parallel 1
ctest --test-dir build-codex-p3 --output-on-failure
```

Linux / Ubuntu：

```bash
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 手动联调

启动模拟设备：

```powershell
.\build-codex-p3\opcua-sim-server.exe --port 48520
```

Codex / VS Code MCP 配置使用：

```text
config/config.opcua-sim.json
```

手动验收顺序：

```text
list_devices
read_node(device_id=pump-1, variable=temperature)
write_node(device_id=pump-1, variable=temperature, value=55.5)
read_node(device_id=pump-1, variable=temperature)
get_device_state(device_id=pump-1)
get_alarm_history(device_id=pump-1, limit=10)
get_network_status(device_id=pump-1)
diagnose_fault(device_id=pump-1)
get_server_health
get_device_health(device_id=pump-1)
```

## 非目标项

以下能力本轮明确不实现：

- MCP over HTTP 远程传输
- OPC UA Subscription
- C++ 内部直接调用 LLM
- 新增真实设备启停控制动作
