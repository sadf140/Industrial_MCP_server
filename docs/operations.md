# 运行运维说明

本文说明 Industrial MCP Server 的运行、健康检查、指标、存储、安全写入和部署方式。

## 健康检查

MCP 工具：

```text
get_server_health
get_device_health(device_id=pump-1)
get_network_status(device_id=pump-1)
```

HTTP 接口：

```text
GET /health/live
GET /health/ready
GET /health/devices
```

`/health/live` 表示进程存活。`/health/ready` 返回配置、缓存、设备连接、存储和观测状态。`/health/devices` 返回所有设备的连接状态、熔断状态、最近错误和缓存状态。

## Prometheus 指标

配置：

```json
{
  "observability": {
    "metrics_enabled": true,
    "metrics_port": 9090
  }
}
```

指标接口：

```text
GET /metrics
```

当前输出 Prometheus text format，包含：

- `mcp_requests_total`
- `mcp_request_duration_seconds_count`
- `mcp_request_duration_seconds_sum`
- `mcp_tool_errors_total`
- `opcua_connection_state`
- `opcua_reconnect_total`
- `device_cache_age_seconds`
- `alarm_events_total`

如果 `observability.metrics_enabled=false`，`/metrics` 返回禁用说明。

## 设备状态缓存

配置：

```json
{
  "cache": {
    "enabled": true,
    "poll_interval_ms": 2000,
    "stale_after_ms": 10000
  }
}
```

后台采集线程按周期读取配置变量并更新 `DeviceStateCache`。`get_device_state` 只读缓存，不直接访问 OPC UA。缓存输出包含：

- `device_id`
- `variables`
- `temperature`
- `current`
- `voltage`
- `running`
- `status`
- `last_update_time`
- `stale`
- `cache_age_seconds`
- `device_connected`
- `connection_state`
- `last_error`

## 自动报警

后台采集时按变量阈值生成报警：

- 超过 `warn_min` / `warn_max`：记录 `WARN`
- 超过 `alarm_min` / `alarm_max`：记录 `CRITICAL`
- OPC UA 读取失败或设备离线：记录 `ERROR`

报警结构包含：

```cpp
struct AlarmRecord {
    std::string alarm_id;
    std::string device_id;
    std::string level;
    std::string message;
    std::string source_node;
    double value;
    double threshold;
    std::string timestamp;
};
```

同一设备、同一变量、同一等级的报警不会在同一异常状态下重复刷屏；恢复正常后再次越界会重新触发。

## 存储

默认使用 JSONL：

```json
{
  "storage": {
    "type": "jsonl"
  },
  "audit": {
    "log_path": "../data/audit.example.jsonl"
  },
  "alarm_log_path": "../data/alarms.example.jsonl"
}
```

可选 SQLite：

```json
{
  "storage": {
    "type": "sqlite",
    "sqlite_path": "../data/industrial-mcp.sqlite"
  }
}
```

CMake 检测到 SQLite3 时会定义 `INDUSTRIAL_MCP_WITH_SQLITE=1` 并启用 SQLite 后端。未检测到 SQLite3 时不会构建失败，服务启动后回退 JSONL。

## 安全写入

生产环境建议保持：

```json
{
  "server": {
    "read_only": true
  },
  "opcua": {
    "write_enabled": false
  },
  "security": {
    "enabled": true,
    "default_role": "viewer"
  }
}
```

`write_node` 必须同时满足：

- 全局只读关闭：`server.read_only=false`
- OPC UA 写入开启：`opcua.write_enabled=true`
- 当前角色有工具权限
- 变量存在于配置白名单
- 变量配置 `writable=true`
- 写入值满足 `data_type`、`min`、`max`、`allowed_values`

## 可靠性

配置：

```json
{
  "reliability": {
    "max_retry_count": 1,
    "backoff_initial_ms": 100,
    "backoff_max_ms": 30000,
    "circuit_failure_threshold": 3,
    "circuit_cooldown_ms": 5000
  }
}
```

连接管理层为每台设备独立维护：

- `connection_state`
- `online`
- `consecutive_failures`
- `reconnect_count`
- `last_success_at`
- `last_error_at`
- `last_error`
- `circuit_state`
- `latency_ms`

## Docker

构建并运行：

```bash
docker compose up --build
```

默认端口：

- `8080`：健康接口
- `9090`：指标接口
- `9091`：Prometheus Web UI

如需让 Prometheus 抓取指标，请在挂载的配置中设置：

```json
{
  "observability": {
    "metrics_enabled": true,
    "metrics_port": 9090
  }
}
```

## systemd

仓库提供示例：

```text
deploy/systemd/industrial-mcp-server.service
```

典型部署路径：

```bash
sudo useradd --system --home /var/lib/industrial-mcp --shell /usr/sbin/nologin industrial-mcp
sudo install -d -o industrial-mcp -g industrial-mcp /var/lib/industrial-mcp
sudo install -d /etc/industrial-mcp
sudo cp config/config.example.json /etc/industrial-mcp/config.json
sudo cp build/industrial-mcp-server /usr/local/bin/industrial-mcp-server
sudo cp deploy/systemd/industrial-mcp-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now industrial-mcp-server
```

生产环境的 OPC UA 账号、证书和现场配置应放在仓库之外，由部署系统注入。

## 日志与审计

运行日志写 stderr，MCP JSON-RPC 响应只写 stdout，避免污染 stdio 协议。

审计日志在 `audit.log_path` 非空时启用，记录字段包含：

- `request_id`
- `user_id`
- `client_id`
- `tool_name`
- `device_id`
- `target_node`
- `old_value`
- `new_value`
- `result`
- `error_code`

敏感参数名包含 `password`、`token`、`secret`、`private_key` 时会脱敏。
