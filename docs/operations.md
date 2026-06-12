# 运行运维说明

## 健康检查

初始化后建议先调用 `get_gateway_health`。该工具不会打开 OPC UA 会话，只返回本地网关元数据：

- 服务名称和版本。
- 运行时间。
- MCP 传输方式和协议版本。
- OPC UA 编译状态、超时、重试和写入开关。
- 审计日志和报警日志配置状态。
- 已配置设备和变量数量。

## 网络状态

调用 `get_network_status` 可检查设备通信状态：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_network_status","arguments":{"device_id":"pump-1"}}}
```

返回字段包括：

- `online`
- `latency_ms`
- `disconnect_count`
- `consecutive_failures`
- `last_success_at`
- `last_error_at`
- `last_error`
- `attempts`

这些统计为进程内状态，服务重启后清零。当前版本不引入后台轮询，统计由读取、快照、状态查询、写入和网络状态查询按需刷新。

## 写入控制

生产环境建议保持：

```json
{
  "server": {
    "read_only": true
  },
  "opcua": {
    "write_enabled": false
  }
}
```

只有在受控联调或维护场景中才开启：

```json
{
  "server": {
    "read_only": false
  },
  "opcua": {
    "write_enabled": true
  }
}
```

同时，目标变量必须配置：

```json
{
  "name": "temperature",
  "node_id": "ns=1;s=Pump1.Temperature",
  "data_type": "Double",
  "writable": true
}
```

`write_node` 不支持 raw `node_id` 写入，只接受配置白名单里的 `device_id` 和 `variable`。

## 运行日志

进程会把启动、停止和启动失败等结构化 JSON 日志写入 stderr。MCP JSON-RPC 响应只写入 stdout。

启动日志示例：

```json
{"timestamp":"2026-06-12T08:00:00Z","level":"info","event":"server_starting","config_path":"config/config.example.json","server_name":"industrial-mcp-server","server_version":"0.4.0-p2","device_count":1,"read_only":true}
```

## 审计

在配置中设置 `audit.log_path` 后，会启用 JSONL 工具调用审计。留空则关闭审计输出。

以下敏感参数名会被脱敏：

- `password`
- `token`
- `secret`
- `private_key`

写操作审计会记录 `read_only=false`，便于和读操作区分。

## Linux systemd 示例

```ini
[Unit]
Description=Industrial MCP Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/industrial-mcp-server
ExecStart=/opt/industrial-mcp-server/build/industrial-mcp-server --config /etc/industrial-mcp-server/config.json
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

生产环境的 OPC UA 账号、证书和现场配置应放在仓库之外，并由部署系统注入。
