# MCP 客户端接入

Industrial MCP Server 使用 JSON-RPC 2.0 over stdio 与 MCP 客户端通信。Codex、VS Code 等客户端应以命令行方式启动 `industrial-mcp-server`，并通过 `--config` 传入配置文件。

## 构建

Windows / MinGW：

```powershell
cd E:\workspace\Industrial_MCP_server
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

## Codex 配置

Codex 的 `config.toml` 通常位于：

```text
C:\Users\<你的用户名>\.codex\config.toml
```

只读 mock 配置：

```toml
[mcp_servers.industrial_mcp_server]
enabled = true
command = 'E:\workspace\Industrial_MCP_server\build-codex-p3\industrial-mcp-server.exe'
args = ['--config', 'E:\workspace\Industrial_MCP_server\config\config.example.json']
startup_timeout_sec = 30
```

open62541 模拟 OPC UA Server 联调配置：

```toml
[mcp_servers.industrial_mcp_server]
enabled = true
command = 'E:\workspace\Industrial_MCP_server\build-codex-p3\industrial-mcp-server.exe'
args = ['--config', 'E:\workspace\Industrial_MCP_server\config\config.opcua-sim.json']
startup_timeout_sec = 30
```

`startup_timeout_sec` 必须与 `command`、`args` 同级。不要放到 `[mcp_servers.industrial_mcp_server.env]` 下；放到 `env` 只会作为普通环境变量传给进程，不会改变 Codex 等待 MCP Server 启动的超时时间。

## VS Code MCP 配置

建议对可执行文件和配置文件都使用绝对路径。

Windows：

```json
{
  "servers": {
    "industrial-mcp-server": {
      "type": "stdio",
      "command": "E:\\workspace\\Industrial_MCP_server\\build-codex-p3\\industrial-mcp-server.exe",
      "args": ["--config", "E:\\workspace\\Industrial_MCP_server\\config\\config.example.json"]
    }
  }
}
```

Linux：

```json
{
  "servers": {
    "industrial-mcp-server": {
      "type": "stdio",
      "command": "/opt/industrial-mcp-server/build/industrial-mcp-server",
      "args": ["--config", "/etc/industrial-mcp/config.json"]
    }
  }
}
```

## open62541 模拟 Server

启动模拟 OPC UA Server：

```powershell
.\build-codex-p3\opcua-sim-server.exe --port 48520
```

保持该 PowerShell 窗口运行。模拟 endpoint：

```text
opc.tcp://127.0.0.1:48520
```

配置文件使用：

```text
config/config.opcua-sim.json
```

## 6 个核心工具验收顺序

在 Codex 或 VS Code MCP 客户端中依次调用：

```text
list_devices
```

```text
read_node(device_id=pump-1, variable=temperature)
```

```text
write_node(device_id=pump-1, variable=temperature, value=55.5)
```

```text
read_node(device_id=pump-1, variable=temperature)
```

```text
get_alarm_history(device_id=pump-1, limit=10)
```

```text
get_network_status(device_id=pump-1)
```

```text
get_device_state(device_id=pump-1)
```

```text
diagnose_fault(device_id=pump-1)
```

只读 `config.example.json` 下 `write_node` 应被拒绝；`config.opcua-sim.json` 用于本地模拟写入联调。

## JSON-RPC 烟测消息

Server 从 stdin 接收一行一个 JSON-RPC 请求，协议响应只写 stdout，运行日志写 stderr。

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
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_devices","arguments":{}}}
```

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_node","arguments":{"device_id":"pump-1","variable":"temperature"}}}
```

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_device_state","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"diagnose_fault","arguments":{"device_id":"pump-1"}}}
```

## 第三阶段新增工具

```text
get_server_health
get_device_health(device_id=pump-1)
refresh_device_state(device_id=pump-1)
acknowledge_alarm(device_id=pump-1, alarm_id=<alarm_id>)
clear_cached_alarm(device_id=pump-1, alarm_id=<alarm_id>)
prepare_device_action(device_id=pump-1, action=stop)
confirm_device_action(operation_id=<operation_id>)
cancel_device_action(operation_id=<operation_id>)
```
