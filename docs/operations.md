# 运行运维说明

## 健康检查

初始化后建议先调用 `get_gateway_health`。该工具不会打开 OPC UA 会话，只返回本地网关元数据：

- 服务名称和版本。
- 运行时间。
- MCP 传输方式和协议版本。
- OPC UA 编译状态、超时和重试配置。
- 审计日志和报警日志配置状态。
- 已配置设备和变量数量。

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

## Linux systemd 示例

示例 unit:

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

生产环境的 OPC UA 账号、证书和现场配置应放在仓库之外。
