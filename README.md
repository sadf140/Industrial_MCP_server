# Industrial MCP Server

C++20 工业 MCP Server，面向只读工业数据访问场景。当前 P2 版本以 `open62541pp` 作为 OPC UA 客户端基础，以 `nlohmann/json` 处理 JSON-RPC 2.0 与 MCP 消息。

## 当前能力

- MCP JSON-RPC 2.0 over stdio。
- MCP 基础生命周期：`initialize`、`notifications/initialized`、`ping`。
- MCP Tools：`tools/list`、`tools/call`。
- 只读工具：读取 OPC UA 节点、读取设备快照、查询设备状态、查询报警日志、分析报警日志、规则式故障诊断。
- 只读健康检查工具：查询网关运行时间、配置规模、OPC UA 编译状态、审计和报警日志配置状态。
- OPC UA 支持 mock 端点与真实 `opc.tcp://` 端点。
- 设备快照使用单次 OPC UA 会话批量读取配置变量。
- OPC UA 读取和状态检查支持重试次数与重试间隔配置。
- 工具调用审计日志 JSONL，敏感参数会脱敏。
- 报警日志 JSONL 支持基础质量检查、无效记录计数、最新优先查询和老到新的时间线分析。
- Windows / Linux 双端 CMake 构建，主要目标平台为 Linux / Ubuntu。
- GitHub Actions Ubuntu CI 配置。
- MCP 客户端接入示例和运行运维说明。

## 构建

Windows / MinGW:

```powershell
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Ubuntu / Linux:

```bash
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`INDUSTRIAL_MCP_WITH_OPCUA=ON` 默认开启，会构建 `third_party/open62541pp` 及其内部 `open62541`。

## 运行

Windows:

```powershell
.\build\industrial-mcp-server.exe --config config\config.example.json
```

Linux:

```bash
./build/industrial-mcp-server --config config/config.example.json
```

Server 使用 stdio 接收一行一个 JSON-RPC 请求：

```json
{"jsonrpc":"2.0","id":1,"method":"initialize"}
```

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

```json
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
```

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"read_opcua_node","arguments":{"device_id":"pump-1","variable":"temperature"}}}
```

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_device_snapshot","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_gateway_health","arguments":{}}}
```

更多 MCP 客户端配置见 `docs/mcp-client.md`，运行运维说明见 `docs/operations.md`。

## open62541 模拟设备

构建 `INDUSTRIAL_MCP_WITH_OPCUA=ON` 时会同时生成独立模拟设备程序：

Windows:

```powershell
.\build\opcua-sim-server.exe --port 48520
```

Linux:

```bash
./build/opcua-sim-server --port 48520
```

模拟 Server 暴露 `opc.tcp://127.0.0.1:48520`，节点位于 `ns=1;s=Pump1.*`。MCP Server 可使用 `config/config.opcua-sim.json` 进行真实 OPC UA 读取联调。

## 配置

默认配置位于 `config/config.example.json`。

关键字段：

- `server.name` / `server.version`：MCP `initialize` 返回的服务信息。
- `server.read_only`：当前固定按只读边界设计。
- `opcua.connect_timeout_ms`：OPC UA 连接阶段超时配置。
- `opcua.read_timeout_ms`：OPC UA 读请求响应超时。
- `opcua.retry_count`：失败后的重试次数，`1` 表示最多尝试 2 次。
- `opcua.retry_delay_ms`：重试前等待时间。
- `opcua.allow_raw_node_id`：是否允许工具直接读取未配置的 `node_id`。
- `audit.log_path`：工具调用审计 JSONL 输出路径，留空则关闭审计。
- `alarm_log_path`：报警 JSONL 文件路径。
- `devices[].endpoint`：支持 `mock://...` 和 `opc.tcp://...`。
- `devices[].variables[]`：设备变量白名单，包含变量名、节点 ID、数据类型、单位和报警阈值。

默认禁止 raw node id，只允许读取配置中的变量，保持只读安全边界。

## 审计日志

启用 `audit.log_path` 后，每次 `tools/call` 会追加一条 JSONL 记录：

```json
{"timestamp":"2026-06-11T08:00:00Z","event":"tool_call","tool":"read_opcua_node","device_id":"pump-1","ok":true,"elapsed_ms":3,"read_only":true,"arguments":{"device_id":"pump-1","variable":"temperature"}}
```

审计参数会对 `password`、`token`、`secret`、`private_key` 做脱敏。

## 报警日志格式

报警日志采用一行一条 JSON：

```json
{"timestamp":"2026-06-11T08:20:00Z","device_id":"pump-1","severity":"warning","code":"TEMP_HIGH","message":"Bearing temperature exceeded warning threshold","state":"active","source":"motor_bearing","acknowledged":false}
```

P1 会过滤缺少 `timestamp`、缺少 `device_id` 或时间格式不符合基础 UTC ISO-8601 形式的记录，并在查询与分析结果里返回 `invalid_record_count`。

## 当前限制

- OPC UA 目前只覆盖常见标量类型读取。
- OPC UA 数组、结构体、证书、安全策略、用户名密码认证尚未实现。
- 故障诊断仍是规则式原型，结果只用于辅助排查，不能替代现场安全规程。
- 当前 Server 不暴露 OPC UA 写入、方法调用、参数下发或控制能力。
