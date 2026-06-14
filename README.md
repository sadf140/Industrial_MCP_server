# Industrial MCP Server

C++20 工业 MCP Server，用于让 Codex / VS Code 等 MCP 客户端通过 JSON-RPC 2.0 over stdio 访问 OPC UA 工业设备数据、报警日志和诊断上下文。

当前版本基于 `open62541pp` 访问 OPC UA，使用 `nlohmann/json` 处理 MCP 与 JSON-RPC 消息，CMake 构建，主要目标平台为 Linux / Ubuntu，同时保持 Windows / MinGW 可构建。

## 核心功能

最终 MCP 主接口包括 6 个工具：

- `read_node`：读取指定设备的配置白名单 OPC UA 节点值。
- `write_node`：写入指定设备的配置白名单 OPC UA 节点值，默认关闭，必须显式开启。
- `list_devices`：列出当前配置接入的工业设备和变量。
- `get_alarm_history`：查询报警历史。
- `diagnose_fault`：基于设备状态缓存、报警日志和网络状态生成故障诊断上下文。
- `get_network_status`：返回设备通信状态、延迟、断连次数和连续失败次数。

第二阶段新增工业可用增强：

- `get_device_state`：读取 MCP Server 内部周期采集的设备状态缓存，不在本次工具调用中直接访问 OPC UA。
- 自动阈值报警：缓存采集时根据 `warn_min/warn_max` 与 `alarm_min/alarm_max` 追加 `WARN` / `CRITICAL` 报警；通信或读取失败追加 `ERROR` 报警。
- `diagnose_fault`：优先使用设备状态缓存、报警历史和通信上下文，输出适合 LLM 分析的结构化现场上下文。

为了兼容前期联调，以下旧工具名仍保留：

- `read_opcua_node`
- `read_device_snapshot`
- `get_device_status`
- `query_alarm_logs`
- `analyze_alarms`
- `get_gateway_health`

## 构建

Windows / MinGW:

```powershell
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux / Ubuntu:

```bash
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`INDUSTRIAL_MCP_WITH_OPCUA=ON` 会构建 `third_party/open62541pp` 及其内部 `open62541`。首次全量构建会明显慢于后续增量构建。

## 运行

Windows:

```powershell
.\build\industrial-mcp-server.exe --config config\config.example.json
```

Linux:

```bash
./build/industrial-mcp-server --config config/config.example.json
```

MCP Server 使用 stdio，一行一个 JSON-RPC 请求。

```json
{"jsonrpc":"2.0","id":1,"method":"initialize"}
```

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

```json
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
```

## 6 个工具调用示例

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_devices","arguments":{}}}
```

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_node","arguments":{"device_id":"pump-1","variable":"temperature"}}}
```

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"write_node","arguments":{"device_id":"pump-1","variable":"temperature","value":55.5}}}
```

```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_alarm_history","arguments":{"device_id":"pump-1","limit":10}}}
```

```json
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"diagnose_fault","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"get_network_status","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"get_device_state","arguments":{"device_id":"pump-1"}}}
```

## open62541 模拟设备

构建 `INDUSTRIAL_MCP_WITH_OPCUA=ON` 时会生成独立模拟设备程序。

Windows:

```powershell
.\build\opcua-sim-server.exe --port 48520
```

Linux:

```bash
./build/opcua-sim-server --port 48520
```

模拟 Server 暴露：

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

真实 OPC UA 联调时使用：

```text
config/config.opcua-sim.json
```

## 写入安全边界

`write_node` 默认关闭，必须同时满足以下条件才允许写入：

- `server.read_only=false`
- `opcua.write_enabled=true`
- 目标变量配置 `writable=true`
- 写入目标必须是配置文件里的变量名，不支持 raw `node_id` 写入

默认示例 `config/config.example.json` 保持只读。`config/config.opcua-sim.json` 仅用于本地模拟联调，已开启安全测试变量写入。

## 配置字段

关键字段：

- `server.name` / `server.version`：MCP `initialize` 返回的服务信息。
- `server.read_only`：全局只读开关，生产环境建议保持 `true`。
- `opcua.connect_timeout_ms`：OPC UA 连接超时。
- `opcua.read_timeout_ms`：OPC UA 请求超时。
- `opcua.retry_count`：失败后重试次数，`1` 表示最多尝试 2 次。
- `opcua.retry_delay_ms`：重试间隔。
- `opcua.allow_raw_node_id`：是否允许 `read_node` 读取显式 `node_id`。
- `opcua.write_enabled`：是否启用 OPC UA 写入。
- `cache.enabled`：是否启用后台周期采集和设备状态缓存。
- `cache.poll_interval_ms`：后台采集周期，默认 `2000`。
- `cache.stale_after_ms`：缓存超过该时长未更新后标记为 stale，默认 `10000`。
- `audit.log_path`：工具调用审计 JSONL 路径，留空关闭审计。
- `alarm_log_path`：报警 JSONL 文件路径。
- `devices[].endpoint`：支持 `mock://...` 和 `opc.tcp://...`。
- `devices[].variables[].writable`：变量是否允许 `write_node` 写入。

## 审计日志

启用 `audit.log_path` 后，每次 `tools/call` 会追加一条 JSONL 记录：

```json
{"timestamp":"2026-06-11T08:00:00Z","event":"tool_call","tool":"write_node","device_id":"pump-1","ok":true,"elapsed_ms":3,"read_only":false,"arguments":{"device_id":"pump-1","variable":"temperature","value":55.5}}
```

参数名 `password`、`token`、`secret`、`private_key` 会脱敏。

## 当前限制

- OPC UA 写入仅支持常见标量类型：Boolean、Double、Float、Int32、UInt32、Int16、UInt16、String。
- 不支持数组、结构体、证书、安全策略、用户名密码认证和 OPC UA 方法调用。
- 网络状态统计为进程内状态，服务重启后清零。
- 设备状态缓存和自动报警状态为进程内状态，服务重启后重新采集和判定。
- 故障诊断仍是规则式辅助诊断，不能替代现场安全规程。
