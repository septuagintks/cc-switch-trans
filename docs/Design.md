# ccs-trans 项目设计

## 文档状态

| 项目           | 内容                                   |
| -------------- | -------------------------------------- |
| 文档性质       | `0.3.0` 实现基线                       |
| 当前程序版本   | `0.3.0`                                |
| 当前平台       | Windows x64                            |
| 默认监听地址   | `http://127.0.0.1:15723`               |
| 当前阶段主目标 | 持久配置与不可变运行快照                |

本文描述“要构建什么”以及必须遵守的行为和架构边界。具体构建顺序见 [DevelopmentPlan.md](DevelopmentPlan.md)，目录归属见 [ProjectStructure.md](ProjectStructure.md)。

## 项目定位

`ccs-trans` 是运行在本机的 OpenAI 兼容 HTTP 转发服务。它接收 Codex 或其他客户端发来的请求，按任务选择上游，在必要时执行明确配置的请求改写，再把上游响应返回给原客户端。

项目遵循两个默认行为：

1. 没有命中改写规则时保持透明转发。
2. 所有介入行为必须能通过结构化日志证明，不能静默修改请求。

主要本地入口：

```text
POST /v1/responses
POST /v1/responses/
POST /v1/chat/completions
GET  /v1/usage
```

Responses 与 Chat Completions 是两个可并行、可指向不同上游、可拥有不同改写规则的主任务。Usage 是保留的辅助透明转发入口。

## 当前基线

`0.3.0` 当前实现：

- Windows 本地 HTTP 监听和固定工作线程池。
- Responses、Chat Completions 与 Usage 路由。
- Responses 本地路径尾斜杠兼容。
- 普通 JSON 响应和 SSE 流式响应转发。
- 查询参数、主要请求头、状态码和响应头透传。
- JSON Lines 链路日志、body 限制和可选敏感头脱敏。
- Usage 请求不写入普通请求链路日志。
- 上游不可达、超时、请求过大、未知路由和方法错误处理。
- 针对 JSON body 前导空白和额外 HTTP body 分隔符的兼容处理。
- Debug、Release 构建和 Python 集成测试。
- Responses、Chat Completions 独立上游 URL/路径与旧共享上游回退。
- TaskRouter、RequestTransform、UpstreamTarget 和 AppService 生命周期边界。
- findcg Responses 根级 `image_gen` 工具清理和结构化改写日志。
- 进程级 WinHTTP session、独立 request handle 和非流式响应缓冲上限。
- 100 ms 批量日志 writer、错误立即 flush 和有界日志队列。
- SSE 序号 chunk 日志、流式 response body 去聚合和连接总量上限。
- CTest 纯逻辑测试与双 mock upstream 集成测试。
- 合成 8/16/50 路 benchmark、Windows 进程资源采样和 transform 微基准。
- 连接、worker、日志队列、WinHTTP、取消和分阶段 timeout 运行时指标。
- 客户端断开通过共享 socket monitor 传播到对应 WinHTTP request handle。
- resolve、connect、send、response header、SSE idle 和可选 total timeout。
- 2026-07-11 真实 Codex -> ccs-trans -> findcg Responses 回归通过。

当前保留同步 worker 模型：修正后的基准显示 8/16 路常规 SSE 负载没有 worker 排队，50 路压力负载在 16 个 worker 之上按设计排队。阶段 10 开始持久配置；tray 和 macOS 继续后置。

## 0.3.0 范围

### 已交付

1. Responses 与 Chat Completions 使用独立的上游 URL 和路径配置。
2. 旧 `--upstream-url` 和旧上游路径参数继续作为兼容回退。
3. 只有“Responses 任务 + findcg 主机”启用 `image_gen` 工具清理。
4. 清理只作用于请求 JSON 根级 `tools` 数组，不改背景文本或用户输入。
5. Chat Completions 当前保持透明，但建立独立 transform 入口。
6. 日志记录任务选择、规则命中和改写结果。
7. 普通响应、SSE、Usage、错误处理和现有路径兼容不能退化。
8. 先建立任务、改写、传输、日志和服务生命周期边界，为后续性能工作预留替换点。
9. 客户端断开可取消对应上游请求，不影响其他连接。
10. 上游 timeout 按阶段独立配置，并保持稳定的 504 错误分类。
11. 性能指标和 benchmark 只使用有界计数与合成数据。

### 后续阶段

- 全异步 HTTP 服务端或完整网络栈替换。
- 配置文件持久化与热重载。
- Windows 托盘图标、菜单、开机自启、后台宿主和双击隐式启动。
- macOS transport、菜单栏图标、登录时启动和发布包。

这些后续功能已经影响当前接口设计，但不反向扩大 `0.3.0` 的协议改写范围。

## 设计原则

### 透明优先

非 findcg Responses、所有 Chat Completions，以及没有命中 transform 的请求，都不得承担不必要的 JSON 重新序列化。现有协议兼容性规范化完成后，未修改的 body 应复用原始存储。

### 显式任务

路由结果不是一组散落的布尔判断，而是一个包含任务类型、上游目标、路径和 transform 列表的 `TaskContext`。后续增加 Chat 改写时不修改 Responses 规则。

### 平台隔离

业务规则不能读取或持有 WinHTTP、Winsock、Windows 消息循环或 macOS Framework 类型。平台实现依赖核心接口，核心层不反向依赖平台实现。

### 结构化处理

URL 使用解析后的 scheme/host/port/path 判断，JSON 使用结构化解析器修改。不得通过字符串包含判断识别 findcg，也不得通过文本替换删除 `image_gen`。

### 可观测且失败明确

请求是否被修改、删除了多少工具、发送到哪个任务上游，都必须能用 `request_id` 在日志中串联。解析或改写失败不能发送部分损坏的 body。

## 目标架构

```text
CLI host
   |
   v
AppService
   |
   v
Listener/Server
   |
   v
TaskRouter -> TransformPipeline -> UpstreamTransport
   |                |                  |
   +----------------+------ Logger ----+
```

### 依赖方向

```text
hosts -> app service/config
server -> core/config/logging/transport interfaces
transforms -> core JSON and task types
platform transport/listener -> core interfaces
core -> C++ standard library and selected platform-neutral JSON API
```

### 模块职责

| 模块                  | 职责                                   | 不应承担                      |
| --------------------- | -------------------------------------- | ----------------------------- |
| `config`              | CLI 解析、兼容回退、校验、生成配置快照 | 路由请求、执行网络调用        |
| `core/task_router`    | 从 method/path 生成 `TaskContext`      | 解析 JSON、调用 WinHTTP       |
| `core/transform`      | 定义改写接口和结果所有权               | 选择本地路由、写日志文件      |
| `responses_transform` | findcg 匹配与 `image_gen` 清理         | 处理 Chat、创建连接           |
| `server`              | 接收请求、编排流水线、返回响应         | 保存上游主机特例              |
| `upstream_transport`  | 接收明确的目标并执行普通/流式调用      | 判断 findcg、修改 OpenAI JSON |
| `logging`             | 接收结构化事件并输出                   | 决定业务规则                  |
| `AppService`          | 启动、停止、状态和资源关闭顺序         | CLI 参数展示、托盘 UI         |

当前 CMake 已分为 `ccs-trans-core` 静态库和 `ccs-trans` CLI target，上述核心接口均已落地。阶段 9 在现有模块边界内增加指标与取消语义，不为了目录形式提前创建空模块。

## 任务与配置模型

概念模型：

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
    bool enabled;
    std::string local_path;
    UpstreamTarget upstream;
    std::vector<std::string> transforms;
};

struct AppConfig {
    ListenerConfig listener;
    TaskConfig responses;
    TaskConfig chat_completions;
    TaskConfig usage;
    LogConfig logging;
    RuntimeLimits limits;
};
```

实际实现可以使用固定字段而非 `vector<TaskConfig>`，但路由和代理层必须通过统一任务类型读取目标，不能继续直接访问全局单一 URL。

### CLI 参数

`0.2.0` 已新增：

| 参数                        | 作用                           | 回退                        |
| --------------------------- | ------------------------------ | --------------------------- |
| `--responses-upstream-url`  | Responses 上游 base URL        | `--upstream-url`            |
| `--chat-upstream-url`       | Chat Completions 上游 base URL | `--upstream-url`            |
| `--responses-upstream-path` | Responses 上游路径             | `--upstream-responses-path` |
| `--chat-upstream-path`      | Chat Completions 上游路径      | `--upstream-chat-path`      |
| `--worker-threads`          | 同步 HTTP worker 数            | 默认至少覆盖桌面常规负载    |
| `--max-connections`         | 活动与排队连接总上限           | 默认覆盖 50 路压力场景      |
| `--max-request-body-size`   | 本地请求体上限                 | `--max-body-size`           |
| `--max-response-body-size`  | 非流式上游响应缓冲上限         | 独立默认值                  |

继续支持：

```text
--listen-host
--listen-port
--upstream-url
--responses-path
--chat-path
--usage-path
--upstream-responses-path
--upstream-chat-path
--upstream-usage-path
--log-path
--log-level
--log-body
--redact-sensitive
--body-log-limit
--timeout-ms
--max-body-size
```

兼容规则：

1. 专用 URL 或路径优先于旧共享参数。
2. 旧参数未被专用参数覆盖时保持 `0.1.0` 行为。
3. 启动时至少有一个主任务能解析出完整上游目标。
4. Usage 在 `0.2.0` 继续使用旧 `--upstream-url` 和 `--upstream-usage-path`；未设置共享 URL 时禁用 Usage 路由。
5. 配置摘要和 `server_start` 日志分别显示三个任务的最终目标及 enabled 状态。
6. 不兼容旧 `--concurrency`；线程数与连接容量分别由 `--worker-threads`、`--max-connections` 表达。
7. `--max-body-size` 仅作为旧请求体上限的回退，不再同时表达响应和日志限制。

双上游示例：

```text
ccs-trans \
  --upstream-url https://www.findcg.com \
  --responses-upstream-url https://www.findcg.com \
  --chat-upstream-url https://chat.example.com \
  --log-path ./logs/ccs-trans.log
```

## 路由行为

| 方法和本地路径              | 任务             | 默认上游路径           | 改写                                |
| --------------------------- | ---------------- | ---------------------- | ----------------------------------- |
| `POST /v1/responses`        | Responses        | `/v1/responses/`       | 仅 findcg 目标执行 `image_gen` 清理 |
| `POST /v1/responses/`       | Responses        | `/v1/responses/`       | 同上                                |
| `POST /v1/chat/completions` | Chat Completions | `/v1/chat/completions` | `0.2.0` 无改写                     |
| `GET /v1/usage`             | Usage            | `/v1/usage`            | 无改写，不记录普通链路日志          |

路由规则：

1. 本地 Responses 路径匹配时忽略尾部斜杠差异。
2. 查询字符串原样追加到选定的上游路径。
3. 主任务路由存在但任务未配置时返回明确的本地配置错误，不能误用另一个任务的上游。
4. 未知路径返回 `404 invalid_request_error`。
5. 已知路径使用错误方法时返回 `405` 并包含 `Allow`。

## findcg Responses 改写

### 命中条件

必须同时满足：

```text
task == Responses
method == POST
upstream hostname == findcg.com 或 www.findcg.com
request body 是需要转发的 JSON
```

hostname 比较忽略大小写，但必须是完整主机名相等。`findcg.com.example.org`、查询参数或 body 中出现 `findcg.com` 都不能触发规则。

### 修改范围

只检查 JSON 根对象的 `tools` 数组。数组元素为对象且满足以下任一条件时删除：

```text
type == "namespace" && name == "image_gen"
name == "image_gen"
namespace == "image_gen"
```

不得修改：

- 用户消息中的 `image_gen` 文本。
- developer/system 背景说明中的 `image_gen` 文本。
- 非根级对象中的同名字段。
- `view_image`、`web_search`、`tool_search` 或其他工具。
- Chat Completions 请求。
- 非 findcg Responses 请求。

### 处理顺序

```text
接收并完成现有 HTTP/JSON 兼容性规范化
  -> 选择 Responses 任务与上游
  -> 精确匹配 findcg hostname
  -> 解析 JSON DOM
  -> 过滤根级 tools
  -> 仅在有删除时序列化新 body
  -> 由 transport 重算 Content-Length
```

未删除任何元素时继续使用 transform 输入 body，不为了格式化而生成新 JSON。发生删除时允许 JSON 空白和对象字段顺序变化，但语义必须保持。

### 异常策略

| 情况                           | 行为                                                                     |
| ------------------------------ | ------------------------------------------------------------------------ |
| 根对象没有 `tools`             | 不修改，正常转发                                                         |
| `tools` 不是数组               | 不修改，记录 `removed_tools_count: 0`                                    |
| `tools` 含非对象元素           | 保留该元素，继续检查其他元素                                             |
| findcg Responses JSON 无法解析 | 不发送上游，返回 `400 invalid_request_error`，日志类型为 `rewrite_error` |
| 序列化失败                     | 不发送上游，返回 `500 server_error`，记录 `rewrite_error`                |

选择失败关闭而非透明发送，是为了避免在代理已经承诺清理时把未经检查的 `image_gen` 再发送给 findcg。

## Transform 接口

概念结果：

```cpp
struct TransformResult {
    bool matched = false;
    bool modified = false;
    std::optional<std::string> rewritten_body;
    std::vector<DiagnosticField> diagnostics;
};
```

约束：

1. `modified == false` 时不复制完整 body。
2. transform 不直接写文件日志，只返回结构化诊断。
3. transform 不知道 socket、WinHTTP handle 或客户端发送回调。
4. pipeline 按任务配置顺序执行，后一规则读取前一规则输出。
5. 任一规则失败时终止上游发送并返回明确错误。

`0.2.0` 的 Responses 配置一个 findcg transform，Chat 配置空 pipeline。接口仍按多规则设计，避免以后为 Chat 改写重做调用链。

## HTTP 转发

### 请求头

默认透传请求头，以下 hop-by-hop 或传输控制头由本工具处理：

| Header              | 行为               |
| ------------------- | ------------------ |
| `Host`              | 使用上游 host      |
| `Content-Length`    | 根据最终 body 重算 |
| `Connection`        | 不透传             |
| `Transfer-Encoding` | 由 transport 管理  |

`Authorization`、`OpenAI-Organization`、`OpenAI-Project`、`X-Api-Key` 等端到端头默认透传。

### 响应头

默认透传上游响应头，但 `Content-Length`、`Connection` 和 `Transfer-Encoding` 由本地服务重新管理。

### SSE

上游 `Content-Type` 包含 `text/event-stream` 时：

1. 收到响应头后立即向客户端发送本地响应头。
2. 上游 chunk 按顺序写入客户端，不等待完整响应结束。
3. 客户端发送失败或 socket 断开时停止继续写入，并关闭对应上游 request handle。
4. transform 只发生在请求 body 发送前，不解析或修改 SSE 响应事件。
5. 每个 chunk 以从 `0` 开始的序号增量记录，包含 chunk 大小和允许记录的原始内容。
6. 流式路径不再把已发送内容累积到 `response.body`。

`0.3.0` 已验证 SSE 实时转发、每请求独立连续的序号 chunk 日志、去聚合、客户端取消、流空闲 timeout 和可选总 timeout。

## 日志设计

日志继续使用 JSON Lines，每行一个完整事件，通过 `request_id` 关联。

### 基础事件

```text
server_start
request_received
upstream_request
upstream_response
response_sent
request_error
server_stop
```

Usage 继续不记录 `request_received`、`upstream_request`、`upstream_response`、`response_sent` 和普通 `request_error` 链路；服务级启动停止事件不受影响。

### 任务和改写字段

`upstream_request` 或独立 rewrite 事件至少包含：

```text
request_id
task
upstream_url
upstream_path
rewrite_enabled
rewrite_name
rewrite_reason
removed_tools_count
removed_tools
original_body_size
rewritten_body_size
```

`removed_tools` 只记录名称和类型，不复制完整工具 schema。非 findcg、Chat 和未删除场景也记录明确的 `rewrite_enabled`/计数结果，便于证明代理没有介入。

### 完整性与敏感信息

- `--log-body true` 时按 `body_log_limit` 记录 body，并标记是否截断。
- `--redact-sensitive true` 时脱敏 Authorization、Proxy-Authorization、Cookie、Set-Cookie 和 X-Api-Key。
- `--redact-sensitive false` 与 `--log-body true` 会保留凭据头和完整正文；此类日志不得进入 Git、测试 fixture 或发布包。
- 普通事件由单 writer 批量写入，最长约 `100 ms` 刷盘一次；错误事件必须触发立即 flush。
- 异步日志队列不能静默丢事件；队列满时对生产者施加背压。
- 请求/响应 body 日志和改写诊断不能额外扩大敏感信息范围。

## 错误模型

错误响应保持 OpenAI 风格：

```json
{
  "error": {
    "message": "...",
    "type": "..."
  }
}
```

| 场景                 | HTTP 状态码 | type/日志分类                                      |
| -------------------- | ----------- | -------------------------------------------------- |
| 未知路由             | `404`       | `invalid_request_error`                            |
| 方法不允许           | `405`       | `invalid_request_error`                            |
| 请求体过大           | `413`       | `invalid_request_error`                            |
| findcg body 无法改写 | `400`       | 响应 `invalid_request_error`，日志 `rewrite_error` |
| 主任务没有可用上游   | `500`       | `configuration_error`                              |
| 上游不可达           | `502`       | `upstream_error`                                   |
| DNS 解析超时         | `504`       | `upstream_resolve_timeout`                         |
| 连接超时             | `504`       | `upstream_connect_timeout`                         |
| 请求发送超时         | `504`       | `upstream_send_timeout`                            |
| 响应头超时           | `504`       | `upstream_response_header_timeout`                 |
| SSE 空闲超时         | 已发头后保持 `200` | 日志 `upstream_stream_idle_timeout`          |
| 请求总时长超时       | 未发头时 `504` | 日志 `upstream_total_timeout`；已发流则结束流    |
| 内部异常             | `500`       | `server_error`                                     |

所有非 Usage 链路错误写入 `request_error`，并包含 task、status code 和稳定错误类型。

## 并发与性能边界

`0.3.0` 已建立以下有界资源行为：

- WinHTTP session 在进程生命周期内复用，每个请求拥有独立 request handle。
- SSE 不聚合完整 response body，只记录累计字节、chunk 数和有序增量日志。
- 日志由单 writer 线程批量写入，有界队列满时对生产者施加背压。
- `max-connections` 限制活动与排队连接总量，过载返回稳定 `503`。
- 请求体、非流式响应缓冲和日志 body 分别设置上限。
- 客户端断开由一个共享 `WSAPoll` 线程观察，不按 SSE 连接创建 watcher 线程。
- timeout 通过 Windows thread-pool timer 主动关闭阻塞中的请求 handle。
- `performance_snapshot` 使用原子计数和既有队列锁暴露资源高水位，不保留逐请求指标历史。

当前保留的性能边界：

```text
SSE 生命周期占用同步 worker
8/16 路常规负载最多占用同等数量 worker
50 路压力负载高于 worker-threads 时进入有界连接队列
长时间 soak 和跨平台 transport 仍需后续持续测量
```

性能负载口径固定为：`8–16` 路并发 SSE 是桌面常规负载，`50` 路是压力测试，不是常规 SLO。修正 benchmark backlog 后，8/16 路附加 TTFB p50 约为 `10.5/10.6 ms` 且没有 worker 排队；50 路在 16 worker 配置下增加约 `2 s` TTFB。当前继续使用同步 worker，50 路结果不单独触发网络栈重写。

WinHTTP 采用每进程一个长生命周期 session，每个请求独立 request handle；连接资源按 scheme/host/port 和代理策略隔离。配置或代理策略切换时创建新一代 transport/session，不让进行中请求切换句柄。

JSON 首版使用仓库固定版本的 `nlohmann/json` DOM，只在命中 findcg Responses 规则时解析。请求体、非流式响应缓冲和日志 body 使用三个独立上限；SSE 不设置累计响应上限，但单 chunk 日志仍受日志规则约束。

长期资源不变量：所有内部队列有上限；非改写请求不承担 JSON DOM 成本；SSE 内存不随累计流长度线性增长；客户端断开后对应上游工作必须可终止。具体 benchmark、指标 schema 和优化顺序以 DevelopmentPlan 阶段 9 为准。

## 生命周期与后续宿主

`0.2.0` 的 `AppService` 已提供 `start/stop/status/wait`，供 CLI 统一管理服务。长期接口在持久配置阶段增加 reload 和配置快照：

```text
Stopped -> Starting -> Running -> Stopping -> Stopped
start(config_snapshot)
stop(graceful_timeout)
status()
wait()
reload(new_snapshot)
```

阶段 9 继续复用该生命周期，不实现 tray；取消传播和优雅停止必须通过同一服务对象收束进行中请求。未来 Windows tray host 和 macOS host 不能复制路由、配置或日志初始化代码。

持久配置未来采用：

```text
内置默认值
  <- 配置文件
  <- CLI 显式覆盖
  -> 校验后的不可变 ConfigSnapshot
```

命令行覆盖默认不自动写回。配置文件使用 `schema_version`、完整校验和原子替换，不保存转发请求中的 Authorization/API key/Cookie。

持久根目录固定为 Windows `%USERPROFILE%/.ccs-trans/`、macOS `~/.ccs-trans/`。代码通过系统 API 获取当前用户 home，环境变量和 `~` 只用于文档显示。该选择便于 CLI 与图形宿主共享，也意味着 Windows 配置不参与 Roaming Profile、macOS 首版按非沙盒发行；未来若采用 App Sandbox，需要提供容器目录迁移或兼容层。

Windows 托盘和 macOS 菜单栏使用同一组宿主命令：查询状态、启动/停止、重新加载配置、打开日志目录、打开配置、切换当前用户开机/登录自启、退出。自启勾选状态以操作系统实际注册为准，配置文件不能作为唯一事实来源。

## 跨平台边界

Windows 当前使用 Winsock listener 和 WinHTTP transport。macOS 阶段可以选择 libcurl、Boost.Beast 或其他实现，但选择必须满足：

- TLS 与系统代理行为可验证。
- 支持 SSE 增量读取。
- 支持客户端取消向上游传播。
- 可在 CI 或固定环境中复现构建。
- 不改变 task、transform、日志和测试语义。

CMake 继续保持 core、平台实现和 host 分层。macOS 先交付 `arm64` CLI，再根据需求提供 `x86_64`/Universal 2；菜单栏 `.app` 是 macOS 正式交付项。菜单栏图标必须提供与 Windows 托盘等价的服务控制和登录时启动勾选项。

## 测试策略

### `0.3.0` 单元测试

当前已覆盖以下可独立运行的纯逻辑测试：

- URL hostname 规范化和 findcg 精确匹配。
- TaskRouter 的方法、路径、尾斜杠和 disabled task 行为。
- CLI 专用参数、旧参数回退和校验。
- 根级 `tools` 中不同形式的 `image_gen` 删除。
- 无 `tools`、非数组、混合元素、非法 JSON 和无删除场景。
- 非命中 transform 不创建 rewritten body。
- timeout CLI 拆分、旧参数回退和 total=0 行为。
- 取消 token 的幂等触发、立即回调和注销行为。
- 运行时指标和批量日志 writer 的计数、高水位与错误 flush。

### `0.3.0` 集成测试

`tests/integration` 的 mock upstream 可同时启动两个实例并回显目标与 body，当前覆盖：

1. Responses 与 Chat 请求到达不同上游。
2. findcg 匹配使用可注入/可测试目标规则，或通过纯逻辑测试证明 hostname 匹配后在 mock 中验证改写结果。
3. 非 findcg Responses 保留 `image_gen`。
4. Chat 不执行 Responses 改写。
5. 改写后的 `Content-Length` 正确。
6. `stream: true` 改写后仍能实时返回 SSE。
7. Usage、404、405、413、502 和 504 行为不退化。
8. 日志能证明任务选择与删除数量。
9. 两个客户端断开后对应上游请求被取消，worker 能立即处理后续请求。
10. response-header、SSE idle 和 total timeout 分类稳定，已发送的 SSE 前缀不被伪装成第二个错误响应。
11. 每条 SSE 的 chunk 序号按 `request_id` 独立从 0 连续递增。
12. `performance_snapshot` 包含失败和各阶段 timeout 计数。

真实 findcg 只用于人工回归，不作为自动测试依赖。

### 真实链路验收

2026-07-11 使用发布包执行 Codex -> ccs-trans -> findcg 回归，结构化日志得到以下结果：

| 请求 | 原始/改写后工具数 | 删除结果                        | 上游  | SSE 结果                  |
| ---- | ----------------- | ------------------------------- | ----- | ------------------------- |
| A    | `10 -> 9`         | 删除 1 个 `namespace/image_gen` | `200` | 57 个 chunk，序号 `0..56` |
| B    | `15 -> 14`        | 删除 1 个 `namespace/image_gen` | `200` | 72 个 chunk，序号 `0..71` |

两条链路都没有 warning、error 或 4xx/5xx，改写后上游 body 中不再包含目标工具。真实日志含 Authorization 和完整 Codex 上下文，只保留在 Git 忽略的本地诊断目录，不作为测试 fixture 或发布内容。

## `0.3.0` 验收结果

以下验收项均已通过：

1. 旧单上游命令仍可启动并通过现有集成测试。
2. Responses 与 Chat Completions 可以同时使用不同 base URL 和路径。
3. 两个任务可在同一监听端口并行处理普通和流式请求。
4. findcg Responses 根级 `image_gen` 工具不会出现在实际上游 body 中。
5. 背景文本、其他工具、Chat 和非 findcg Responses 不被误改。
6. 改写失败不会发送损坏或未经检查的请求。
7. 日志可以用 `request_id` 证明目标任务、目标上游和改写结果。
8. `Server`、WinHTTP transport 和 CLI host 中不包含硬编码的 `image_gen` 业务判断。
9. fresh Debug/Release 构建和全部自动测试通过。
10. dist 可执行文件与仓库文档来自同一次 Release 构建，并通过 Codex -> ccs-trans -> findcg 真实回归。
11. 客户端取消、分阶段 timeout、运行时指标和 8/16/50 路合成 benchmark 通过自动验证。
12. 8/16 路常规负载继续采用同步 worker；50 路排队作为压力容量边界记录。

## 已决策与延期决策

| 事项                  | 结论                                              |
| --------------------- | ------------------------------------------------- |
| findcg 识别           | 解析 URL 后精确匹配 `findcg.com`/`www.findcg.com` |
| 改写范围              | 仅 Responses 根级 `tools`                         |
| JSON 策略             | 固定版本 `nlohmann/json` DOM；仅命中目标时解析    |
| 无删除 body           | 复用输入，不重新序列化                            |
| 非法 JSON             | 失败关闭，不发送上游                              |
| Chat 当前行为         | 独立任务，透明转发                                |
| 旧 CLI                | 保留上游与请求体参数回退；不兼容 `--concurrency`  |
| 网络模型              | `0.3.0` 保留同步实现和流 callback                 |
| 常规/压力负载         | 8–16 路 SSE 为常规，50 路为压力测试               |
| 日志落盘              | 普通事件约 100 ms 批写，错误事件立即 flush        |
| SSE 日志              | 带序号 chunk 增量记录，不保留完整 response body   |
| WinHTTP 生命周期      | 每进程一个 session，每请求独立 request handle     |
| 容量参数              | `--worker-threads` 与 `--max-connections` 分离     |
| body 上限             | 请求、非流式响应缓冲、日志分别配置                |
| 完整异步 I/O          | 8/16 路无排队，当前不重写；常规负载退化时重评     |
| tray 单/双 executable | tray 原型阶段决定                                 |
| macOS transport 库    | 以构建、代理、SSE、取消和 benchmark 结果决定      |
| 持久配置根目录        | Windows `%USERPROFILE%/.ccs-trans/`；macOS `~/.ccs-trans/` |
| 双平台后台菜单        | Windows 托盘和 macOS 菜单栏均为必需交付项，包含自启勾选项  |

## 阶段 9 结果与阶段 10 入口

阶段 9 已按以下顺序完成：

```text
1. tests/benchmark runner、可配置 mock 和 JSON 结果 schema
2. 低开销连接、日志队列、背压和 transport 指标
3. 保存 0.2.0 的 8/16/50 路基线
4. 请求级取消信号和客户端断开传播
5. 连接、发送、响应头、流空闲和总 timeout 拆分
6. 同 profile 前后对比与同步/异步 I/O 决策记录
```

阶段 9 没有混入 tray、持久配置或 macOS transport。修正后的 8/16 路结果不支持立即重写完整异步 I/O；下一步按 DevelopmentPlan 阶段 10 实现配置文件 schema、平台用户目录、CLI 覆盖层和不可变运行快照。
