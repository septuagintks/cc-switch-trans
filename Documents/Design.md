# ccs-trans 命令行工具设计

## 目标

`ccs-trans` 是一个本地 HTTP 转发命令行工具，默认监听：

```text
http://127.0.0.1:15723
```

工具接收 OpenAI 兼容 API 请求，并将请求原样或按配置转发到指定上游 URL，再把上游响应返回给原请求方。

需要支持两个可并行处理的 OpenAI API 格式任务：

- Responses API: `POST /v1/responses/`
- Chat Completions API: `POST /v1/chat/completions`

工具必须完整记录请求、转发、响应和错误日志，并允许通过命令行参数修改日志路径。

## 命令行接口

### 基础参数

```text
ccs-trans \
  --listen-host 127.0.0.1 \
  --listen-port 15723 \
  --upstream-url https://example.com \
  --log-path ./logs/ccs-trans.log
```

### 参数说明

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--listen-host` | `127.0.0.1` | 本地监听地址 |
| `--listen-port` | `15723` | 本地监听端口 |
| `--upstream-url` | 无，必填 | 上游服务基础地址，例如 `https://api.example.com` |
| `--responses-path` | `/v1/responses/` | 本地 Responses API 路径 |
| `--chat-path` | `/v1/chat/completions` | 本地 Chat Completions API 路径 |
| `--upstream-responses-path` | 同 `--responses-path` | 上游 Responses API 路径 |
| `--upstream-chat-path` | 同 `--chat-path` | 上游 Chat Completions API 路径 |
| `--log-path` | `./logs/ccs-trans.log` | 日志文件路径 |
| `--log-level` | `info` | 日志级别：`trace`、`debug`、`info`、`warn`、`error` |
| `--log-body` | `true` | 是否记录完整请求体和响应体 |
| `--timeout-ms` | `300000` | 转发请求超时时间 |
| `--max-body-size` | `104857600` | 单个请求或响应体最大字节数 |
| `--concurrency` | CPU 核心数 | HTTP 处理线程数 |

### 示例

```text
ccs-trans --upstream-url https://example.com
```

本地调用：

```text
POST http://127.0.0.1:15723/v1/responses/
POST http://127.0.0.1:15723/v1/chat/completions
```

会分别转发到：

```text
POST https://example.com/v1/responses/
POST https://example.com/v1/chat/completions
```

## 路由设计

### Responses API

本地入口：

```text
POST /v1/responses/
```

处理规则：

1. 读取完整 HTTP 请求方法、路径、查询参数、请求头和请求体。
2. 验证方法必须为 `POST`。
3. 不解析或修改 OpenAI Responses JSON 主体，默认透明转发。
4. 将请求发送到：

```text
{upstream-url}{upstream-responses-path}{query-string}
```

5. 将上游状态码、响应头和响应体返回给请求来源。

### Chat Completions API

本地入口：

```text
POST /v1/chat/completions
```

处理规则与 Responses API 相同，转发目标为：

```text
{upstream-url}{upstream-chat-path}{query-string}
```

### 不支持的路径

未知路径返回：

```json
{
  "error": {
    "message": "unsupported route",
    "type": "invalid_request_error"
  }
}
```

HTTP 状态码为 `404`。

## 转发策略

### 请求头

默认透传所有请求头，但以下头由本工具重新生成或跳过：

| Header | 处理 |
| --- | --- |
| `Host` | 改为上游 Host |
| `Content-Length` | 由 HTTP 客户端重新计算 |
| `Connection` | 不透传 |
| `Transfer-Encoding` | 由 HTTP 客户端处理 |

`Authorization`、`OpenAI-Organization`、`OpenAI-Project` 等 OpenAI 相关头默认透传。

### 响应头

默认透传上游响应头，但以下头由本工具重新生成或跳过：

| Header | 处理 |
| --- | --- |
| `Content-Length` | 由 HTTP 服务端重新计算 |
| `Connection` | 不透传 |
| `Transfer-Encoding` | 由 HTTP 服务端处理 |

### 流式响应

OpenAI API 常见 `stream: true` 请求会返回 SSE。

设计要求：

1. 检测上游响应头 `Content-Type: text/event-stream`。
2. 不缓存完整响应体，边读边写给客户端。
3. 每个数据块仍记录元信息日志。
4. 如果启用 `--log-body true`，可以记录 SSE 原始片段，但需要按大小限制截断，避免日志无限增长。

## 并发模型

工具需要允许 Responses API 与 Chat Completions API 同端口并行工作。

建议模型：

1. 主线程负责解析 CLI、初始化配置、日志和 HTTP 服务。
2. HTTP 服务使用线程池或异步事件循环处理请求。
3. 每个请求分配唯一 `request_id`。
4. Responses 与 Chat Completions 共用监听器、线程池、日志器和上游 HTTP 客户端连接池。
5. 单个请求的转发和返回互不阻塞其他请求。

推荐 C++ 技术选型：

| 能力 | 推荐库 |
| --- | --- |
| HTTP 服务端 | Boost.Beast、cpp-httplib、Crow、Drogon |
| HTTP 客户端 | Boost.Beast、libcurl、cpp-httplib |
| CLI 参数 | CLI11、cxxopts |
| 日志 | spdlog |
| JSON 校验，可选 | nlohmann/json |

如果优先快速实现，可选：

- `cpp-httplib` 同时处理服务端和客户端
- `CLI11` 处理命令行
- `spdlog` 处理文件日志

## 日志设计

### 日志路径

通过 `--log-path` 指定日志文件。启动时需要：

1. 检查父目录是否存在。
2. 不存在则创建目录。
3. 无法创建或无法写入时启动失败，并在标准错误输出原因。

### 日志格式

建议使用 JSON Lines，每行一条完整事件，便于检索和后续分析。

示例：

```json
{"time":"2026-07-10T04:30:00.123+08:00","level":"info","event":"request_received","request_id":"01JZ...","method":"POST","path":"/v1/responses/","query":"","client_ip":"127.0.0.1","headers":{"authorization":"Bearer ***"},"body":"{...}"}
```

### 必须记录的事件

| 事件 | 说明 |
| --- | --- |
| `server_start` | 启动配置、监听地址、上游地址、日志路径 |
| `request_received` | 收到本地请求 |
| `upstream_request` | 准备转发到上游 |
| `upstream_response` | 收到上游响应 |
| `response_sent` | 已返回给原请求方 |
| `request_error` | 请求处理失败 |
| `server_stop` | 服务退出 |

### 请求日志字段

```text
time
level
event
request_id
api_type
method
local_path
upstream_url
query
client_ip
headers
body
body_size
```

### 响应日志字段

```text
time
level
event
request_id
status_code
headers
body
body_size
duration_ms
streaming
```

### 敏感信息处理

需求要求“完整地记录所有信息”，因此默认设计可以完整记录请求体和响应体。

但为了降低误泄露风险，建议同时提供：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--redact-sensitive` | `false` | 是否脱敏敏感头 |
| `--body-log-limit` | `1048576` | 单条 body 日志最大字节数，超出后截断 |

当 `--redact-sensitive true` 时，以下头需要脱敏：

```text
Authorization
Proxy-Authorization
Cookie
Set-Cookie
X-Api-Key
```

## 错误处理

### 上游不可达

返回：

```json
{
  "error": {
    "message": "upstream request failed",
    "type": "upstream_error"
  }
}
```

HTTP 状态码建议为 `502`。

### 上游超时

返回：

```json
{
  "error": {
    "message": "upstream request timed out",
    "type": "upstream_timeout"
  }
}
```

HTTP 状态码建议为 `504`。

### 请求体过大

返回：

```json
{
  "error": {
    "message": "request body too large",
    "type": "invalid_request_error"
  }
}
```

HTTP 状态码建议为 `413`。

## 配置结构

内部配置可以抽象为：

```cpp
struct AppConfig {
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = 15723;

    std::string upstream_url;
    std::string responses_path = "/v1/responses/";
    std::string chat_path = "/v1/chat/completions";
    std::string upstream_responses_path = "/v1/responses/";
    std::string upstream_chat_path = "/v1/chat/completions";

    std::filesystem::path log_path = "./logs/ccs-trans.log";
    std::string log_level = "info";
    bool log_body = true;
    bool redact_sensitive = false;
    std::size_t body_log_limit = 1024 * 1024;

    std::chrono::milliseconds timeout = std::chrono::milliseconds(300000);
    std::size_t max_body_size = 100 * 1024 * 1024;
    std::size_t concurrency = std::thread::hardware_concurrency();
};
```

## 核心模块

```text
src/
  main.cpp                 CLI 入口、配置解析、服务启动
  config.hpp               AppConfig 定义
  logger.hpp/.cpp          JSON Lines 日志封装
  server.hpp/.cpp          HTTP 服务端、路由注册
  proxy.hpp/.cpp           请求转发逻辑
  header_filter.hpp/.cpp   请求和响应头过滤
  request_id.hpp/.cpp      request_id 生成
```

### Server

职责：

1. 监听指定 host 和 port。
2. 注册 `/v1/responses/` 与 `/v1/chat/completions`。
3. 为每个请求生成 `request_id`。
4. 调用 `Proxy` 完成上游转发。
5. 处理 404、405、413 等本地错误。

### Proxy

职责：

1. 构造上游 URL。
2. 过滤和重写请求头。
3. 发送 HTTP 请求。
4. 支持普通响应和流式响应。
5. 返回统一的 `ProxyResult`。

### Logger

职责：

1. 写入 JSON Lines 日志。
2. 提供线程安全写入。
3. 支持完整 body 记录、截断记录和敏感头脱敏。
4. 在每条日志中自动补齐时间、级别和事件名。

## 请求处理流程

```text
client
  -> local server
  -> route match
  -> create request_id
  -> log request_received
  -> build upstream request
  -> log upstream_request
  -> send to upstream
  -> receive upstream response
  -> log upstream_response
  -> return response to client
  -> log response_sent
```

## 验收标准

1. 默认启动后监听 `127.0.0.1:15723`。
2. 可以通过参数修改监听端口。
3. `POST /v1/responses/` 可以转发 OpenAI Responses 格式请求。
4. `POST /v1/chat/completions` 可以转发 OpenAI Chat Completions 格式请求。
5. 两个接口可以在同一端口并行处理多个请求。
6. 可以通过参数指定上游 URL。
7. 可以通过参数修改日志路径。
8. 日志包含请求、上游请求、上游响应、返回响应和错误信息。
9. 普通 JSON 响应与 SSE 流式响应都能正确转发。
10. 上游异常时返回明确的 OpenAI 风格错误 JSON。
