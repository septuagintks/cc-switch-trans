# ccs-trans 命令行工具设计

## 目标

`ccs-trans` 是一个本地 HTTP 转发命令行工具，默认监听：

```text
http://127.0.0.1:15723
```

工具接收 OpenAI 兼容 API 请求，并将请求原样或按配置转发到指定上游 URL，再把上游响应返回给原请求方。

需要支持两个可并行处理的 OpenAI API 格式任务：

- Responses API: `POST /v1/responses` 或 `POST /v1/responses/`
- Chat Completions API: `POST /v1/chat/completions`

另外支持 Usage 查询请求：

- Usage API: `GET /v1/usage`

Usage 请求只做透明转发，不写入请求链路日志。

工具必须完整记录请求、转发、响应和错误日志，并允许通过命令行参数修改日志路径。

## 设计状态与长期约束

本文前半部分保留 `0.1.x` 已实现协议和 CLI 行为。后续结构设计以本节以及后面的目标配置、模块边界和性能章节为准；它们与早期的单一 `AppConfig -> Server -> Proxy` 草案冲突时，优先采用后续结构。

当前要增加的业务能力是：

- Responses 与 Chat Completions 使用互相独立的上游任务配置。
- 只有目标为 findcg 的 Responses 请求在上游发送前删除根级 `tools` 中的 `image_gen` 工具声明。
- Chat Completions 暂时透明转发，但拥有独立的 transform 入口，供以后改写其他请求。

已确定但暂不实现的长期能力是：

- 性能优化和长时间后台运行。
- Windows 托盘图标、点击菜单和双击隐式启动。
- 配置文件持久化、校验、保存与重载。
- macOS 命令行编译打包，以及后续菜单栏应用。

预留不等于现在实现全部功能。当前必须做的是把任务路由、请求改写、上游传输、日志写入、配置来源和进程宿主分开，使这些部分以后可以独立替换。

目标依赖方向：

```text
CLI host / future tray host / future macOS host
                    |
                    v
                AppService
                    |
                    v
TaskRouter -> TransformPipeline -> UpstreamTransport
                    |                     |
                    +------ Logger -------+
```

核心业务代码只能依赖内部 HTTP 类型和抽象接口，不能依赖 WinHTTP handle、Winsock socket、Windows 消息或 macOS Framework 对象。

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
| `--responses-path` | `/v1/responses/` | 本地 Responses API 路径，匹配时兼容尾斜杠 |
| `--chat-path` | `/v1/chat/completions` | 本地 Chat Completions API 路径 |
| `--usage-path` | `/v1/usage` | 本地 Usage API 路径 |
| `--upstream-responses-path` | 同 `--responses-path` | 上游 Responses API 路径 |
| `--upstream-chat-path` | 同 `--chat-path` | 上游 Chat Completions API 路径 |
| `--upstream-usage-path` | 同 `--usage-path` | 上游 Usage API 路径 |
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
POST http://127.0.0.1:15723/v1/responses
POST http://127.0.0.1:15723/v1/chat/completions
GET  http://127.0.0.1:15723/v1/usage
```

会分别转发到：

```text
POST https://example.com/v1/responses/
POST https://example.com/v1/chat/completions
GET  https://example.com/v1/usage
```

## 路由设计

### Responses API

本地入口：

```text
POST /v1/responses
POST /v1/responses/
```

处理规则：

1. 读取完整 HTTP 请求方法、路径、查询参数、请求头和请求体。
2. 验证方法必须为 `POST`。
3. 不解析或修改 OpenAI Responses JSON 主体，默认透明转发。
4. 本地匹配兼容尾斜杠，因此 `/v1/responses` 与 `/v1/responses/` 都会进入该路由。
5. 将请求发送到：

```text
{upstream-url}{upstream-responses-path}{query-string}
```

6. 将上游状态码、响应头和响应体返回给请求来源。

### Chat Completions API

本地入口：

```text
POST /v1/chat/completions
```

处理规则与 Responses API 相同，转发目标为：

```text
{upstream-url}{upstream-chat-path}{query-string}
```

### Usage API

本地入口：

```text
GET /v1/usage
```

处理规则：

1. 读取完整 HTTP 请求方法、路径、查询参数和请求头。
2. 验证方法必须为 `GET`。
3. 将请求发送到：

```text
{upstream-url}{upstream-usage-path}{query-string}
```

4. 将上游状态码、响应头和响应体返回给请求来源。
5. 不记录 `request_received`、`upstream_request`、`upstream_response`、`response_sent` 和 `request_error` 等请求链路日志。

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

以下结构描述当前 `0.1.x` 的集中配置。增加双任务时不继续平铺更多 `responses_*`、`chat_*` 字段，而应迁移到后面的目标配置模型。

内部配置可以抽象为：

```cpp
struct AppConfig {
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = 15723;

    std::string upstream_url;
    std::string responses_path = "/v1/responses/";
    std::string chat_path = "/v1/chat/completions";
    std::string usage_path = "/v1/usage";
    std::string upstream_responses_path = "/v1/responses/";
    std::string upstream_chat_path = "/v1/chat/completions";
    std::string upstream_usage_path = "/v1/usage";

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

### 目标配置模型

概念结构如下，实际字段名可以根据实现调整：

```cpp
enum class ApiTaskKind {
    Responses,
    ChatCompletions,
    Usage,
};

struct UpstreamTarget {
    std::string base_url;
    std::string path;
};

struct TaskConfig {
    ApiTaskKind kind;
    std::string local_path;
    UpstreamTarget upstream;
    std::vector<std::string> transforms;
};

struct AppConfig {
    ListenerConfig listener;
    std::vector<TaskConfig> tasks;
    LogConfig logging;
    RuntimeLimits limits;
};
```

配置系统分为三层：

```text
内置默认值
  <- 持久配置文件
  <- CLI 显式覆盖
  -> 校验后的不可变 ConfigSnapshot
```

命令行覆盖默认不写回磁盘。未来只有显式的保存命令或托盘“应用”操作才持久化。请求开始时获取一个不可变配置快照，保证重载不会改变进行中请求的目标或改写规则。

## 核心模块

```text
src/
  config/
    config.hpp/.cpp          AppConfig 与 CLI 解析
  core/
    http_types.hpp           平台无关 HTTP 类型
    request_id.hpp/.cpp      request_id 生成
  hosts/
    cli_main.cpp             CLI 入口与服务启动
  logging/
    logger.hpp/.cpp          JSON Lines 日志封装
  server/
    server.hpp/.cpp          HTTP 服务端、路由与请求编排
  transport/
    proxy.hpp/.cpp           当前 WinHTTP 上游传输
    header_filter.hpp/.cpp   请求和响应头过滤
```

当前 CMake 将除 host 外的实现编译为 `ccs-trans-core` 静态库，再由 `ccs-trans` CLI 链接。这样未来增加 tray 或 macOS host 时不需要复制代理实现。

### 目标模块边界

后续目录不要求一次性全部建立，但职责边界从下一阶段开始执行：

```text
src/core/
  app_service.*            服务生命周期、状态和优雅停止
  task_router.*            本地路由到任务和上游目标的选择
  transform.*              请求改写接口与结果
  responses_transform.*    findcg Responses 的 image_gen 清理
  http_types.*             平台无关的请求、响应和流事件

src/config/
  config_model.*           持久字段和运行时限制
  config_resolver.*        默认值、文件和 CLI 合并
  config_store.*           未来的原子保存与 schema 迁移

src/transport/
  upstream_transport.*     平台无关接口
  winhttp_transport.*      Windows 上游实现
  listener.*               本地 HTTP listener 接口
  winsock_listener.*       Windows listener 实现

src/logging/
  logger.*                 结构化事件接口
  log_writer.*             当前同步、未来异步批量 writer

src/hosts/
  cli_main.cpp             CLI host
  windows_tray_main.cpp    未来 Windows 托盘 host
  macos_app_main.*         未来 macOS 菜单栏 host
```

`Server` 不再负责判断 findcg 或操作 JSON；`Proxy` 不再从全局配置里猜测请求应发往哪个上游；平台 transport 不知道 `image_gen` 等业务规则。

### Server

职责：

1. 监听指定 host 和 port。
2. 注册 `/v1/responses/`、`/v1/chat/completions` 与 `/v1/usage`。
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

## 请求改写模型

请求流水线固定为：

```text
路由匹配
  -> 生成 TaskContext 和 UpstreamTarget
  -> 依次执行该任务的 RequestTransform
  -> 过滤 hop-by-hop headers
  -> 上游传输
```

每个 transform 返回：

```text
是否命中
是否修改
修改后的 body 所有权
结构化诊断字段
错误类型
```

未命中的 transform 必须复用原始 body，不解析和重新序列化 JSON。findcg `image_gen` 清理只绑定到 `Responses + host(findcg.com)`，不能通过普通字符串替换实现，也不能删除背景文本里的同名词。

## 性能设计空间

### 已知热路径

当前实现存在以下确定成本：

1. 每个请求创建 WinHTTP session 和 connection，连接复用边界过短。
2. 请求从 socket 原始缓冲解析到 `HttpRequest` 时有多次字符串复制。
3. SSE 已发送的 chunk 仍追加到完整响应 body，长流内存持续增长。
4. 所有日志调用竞争同一个 mutex，并且每行立即刷新磁盘。
5. 每个同步 worker 在上游等待和 SSE 生命周期内不可服务其他连接。
6. 接入队列没有容量上限，过载时资源增长不可控。

### 优化顺序

性能工作遵循“测量、局部替换、复测”：

1. 建立非流式、请求改写、并发 SSE、客户端断开和日志开关基线。
2. 先复用进程级上游 session/连接资源。
3. 再去除 SSE 完整聚合，并传播下游取消。
4. 日志改为单 writer、批量写和有界队列。
5. 为接入队列、活动连接和单上游并发设置上限。
6. 只在命中改写时解析 JSON，减少 body 和 headers 的所有权复制。
7. 如果压测仍证明线程被长 SSE 耗尽，再迁移到异步 listener/transport。

必须长期保持的资源不变量：

```text
SSE 内存不随累计流长度线性增长
所有内部队列都有明确上限
客户端断开后上游请求可取消
非改写请求不承担 JSON DOM 成本
连接池按完整上游身份和代理配置隔离
```

### 日志与吞吐取舍

“完整记录”和“最高吞吐”无法同时无成本成立。设计提供明确模式，而不是暗中丢日志：

- 完整性模式：事件和配置允许范围内的 body 不丢失；异步队列满时对请求施加背压；正常关闭时等待 writer 排空。
- 轻量模式：用户显式关闭 body 后只记录大小、哈希和链路元信息，以降低 I/O。
- 无论模式如何，错误、启动停止、改写结果和计数事件都不能丢失。
- SSE body 使用有序 chunk 事件或 writer 内合并方式增量记录，不能为了日志完整性在请求线程中聚合整条流。

JSON 改写第一版使用可靠的结构化 DOM 解析，但只对命中的 findcg Responses 执行。只有 profiling 证明其成为主要成本时，才考虑 SAX 或定向流式重写；这部分复杂度不提前支付。

## 服务生命周期与后台宿主

核心服务需要提供至少以下状态和操作：

```text
Stopped -> Starting -> Running -> Stopping -> Stopped
start(config_snapshot)
stop(graceful_timeout)
status()
reload(new_snapshot)
```

CLI 只负责参数、输出和退出码。未来 Windows tray host 负责图标、点击菜单、通知、单实例和双击隐式启动，但调用同一个 `AppService`。托盘退出必须先停止 listener、取消或等待进行中请求，再排空日志 writer。

Windows CLI 与无控制台 tray host 是否最终做成一个 launcher 或两个 executable，需要通过命令行重定向、双击体验和打包复杂度原型后决定；核心库边界使这个选择不会影响代理逻辑。

## 持久配置设计

配置文件包含 `schema_version`，并通过临时文件写入、完整校验和原子替换保存。默认路径：

```text
Windows: %APPDATA%/ccs-trans/config.json
macOS:   ~/Library/Application Support/ccs-trans/config.json
```

第一版持久化监听设置、任务上游、路径、日志选项、超时和资源限制，不持久化请求中的 `Authorization`、API key、Cookie 等转发凭据。配置损坏时必须报告错误，不能静默使用默认值后监听错误端口或转发到错误上游。

## 跨平台与打包边界

CMake 目标最终分为平台无关 core、平台 transport/listener 和宿主 executable。Windows 继续使用 WinHTTP 是可接受的，只要它实现统一 transport 接口；macOS 可以选用 libcurl、Boost.Beast 或其他支持 TLS、系统代理、SSE 和取消的实现，最终选择由可复现构建与 benchmark 决定。

macOS 交付顺序：

1. 先提供 `arm64` CLI 并运行完整协议测试。
2. 根据用户范围补 `x86_64` 或 Universal 2。
3. 再增加菜单栏 `.app`、应用图标、签名和公证。
4. Windows 与 macOS 使用同一套 mock upstream 样例验证请求改写和流式语义。

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
5. `GET /v1/usage` 可以转发 Usage 查询请求，且不记录请求链路日志。
6. 两个主要接口可以在同一端口并行处理多个请求。
7. 可以通过参数指定上游 URL。
8. 可以通过参数修改日志路径。
9. 日志包含请求、上游请求、上游响应、返回响应和错误信息。
10. 普通 JSON 响应与 SSE 流式响应都能正确转发。
11. 上游异常时返回明确的 OpenAI 风格错误 JSON。
