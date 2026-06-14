# MCP 客户端接入

本服务通过 stdio 与 MCP 客户端通信。先完成构建，再让 Codex / VS Code MCP 客户端以命令行方式启动 `industrial-mcp-server`，并传入配置文件。

## 构建

Linux / Ubuntu:

```bash
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows / MinGW:

```powershell
cmake -S . -B build -G Ninja -DINDUSTRIAL_MCP_WITH_OPCUA=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## VS Code MCP 配置示例

建议对可执行文件和配置文件都使用绝对路径。

Linux:

```json
{
  "servers": {
    "industrial-mcp-server": {
      "type": "stdio",
      "command": "/absolute/path/to/Industrial_MCP_server/build/industrial-mcp-server",
      "args": ["--config", "/absolute/path/to/Industrial_MCP_server/config/config.example.json"]
    }
  }
}
```

Windows:

```json
{
  "servers": {
    "industrial-mcp-server": {
      "type": "stdio",
      "command": "E:\\workspace\\Industrial_MCP_server\\build\\industrial-mcp-server.exe",
      "args": ["--config", "E:\\workspace\\Industrial_MCP_server\\config\\config.example.json"]
    }
  }
}
```

## Codex config.toml 示例

你的本机 Codex 配置文件通常位于：

```text
C:\Users\81964\.codex\config.toml
```

mock 配置：

```toml
[mcp_servers.industrial_mcp_server]
enabled = true
command = 'E:\workspace\Industrial_MCP_server\build-codex-p2\industrial-mcp-server.exe'
args = ['--config', 'E:\workspace\Industrial_MCP_server\config\config.example.json']
startup_timeout_sec = 30
```

真实 OPC UA 模拟 Server 配置：

```toml
[mcp_servers.industrial_mcp_server]
enabled = true
command = 'E:\workspace\Industrial_MCP_server\build-codex-p2\industrial-mcp-server.exe'
args = ['--config', 'E:\workspace\Industrial_MCP_server\config\config.opcua-sim.json']
startup_timeout_sec = 30
```

`startup_timeout_sec` 要和 `command`、`args` 同级。不要放到 `[mcp_servers.industrial_mcp_server.env]` 下面；放到 `env` 下面只会作为普通环境变量传给进程，不会改变 Codex 等待 MCP Server 启动的超时时间。

## open62541 模拟 Server 联调

启动模拟 OPC UA Server：

```powershell
.\build-codex-p2\opcua-sim-server.exe --port 48520
```

保持该窗口运行。它会监听：

```text
opc.tcp://127.0.0.1:48520
```

暴露以下测试节点：

```text
ns=1;s=Pump1.Temperature
ns=1;s=Pump1.Current
ns=1;s=Pump1.Running
ns=1;s=Pump1.Label
```

然后让 MCP Server 使用：

```text
config/config.opcua-sim.json
```

## 目标 6 工具联调顺序

在 Codex / VS Code MCP 客户端中依次调用：

```text
list_devices
```

```text
read_node，device_id=pump-1，variable=temperature
```

```text
write_node，device_id=pump-1，variable=temperature，value=55.5
```

```text
read_node，device_id=pump-1，variable=temperature
```

```text
get_alarm_history，device_id=pump-1，limit=10
```

```text
get_network_status，device_id=pump-1
```

```text
get_device_state，device_id=pump-1
```

```text
diagnose_fault，device_id=pump-1
```

## JSON-RPC 烟测试消息

Server 从 stdin 接收一行一个 JSON-RPC 请求。

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
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_network_status","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_device_state","arguments":{"device_id":"pump-1"}}}
```

```json
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_alarm_history","arguments":{"device_id":"pump-1","level":"WARN","limit":10}}}
```

协议响应只写入 stdout。运行日志写入 stderr，避免污染 MCP stdio 传输。
