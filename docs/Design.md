# ccs-trans 项目设计

## 文档状态

| 项目 | 当前状态 |
| --- | --- |
| 实现基线 | `0.4.0` |
| 支持平台 | Windows x64 |
| 本地入口 | Responses `127.0.0.1:15723`，Chat `127.0.0.1:15724` |
| 配置根目录 | `%USERPROFILE%/.ccs-trans/` |
| 网络模型 | 双 listener、共享有界 worker pool、WinHTTP 上游传输 |
| 下一架构目标 | 单 listener、多代理 Profile、Protocol registry、Rule pipeline |

本文描述当前实现和重构期间仍需保持的行为约束。通用化目标模型见
[Reconstruction.md](Reconstruction.md)，后续构建顺序见
[DevelopmentPlan.md](DevelopmentPlan.md)，文件归属见
[ProjectStructure.md](ProjectStructure.md)。

## 项目定位

`ccs-trans` 是本地 OpenAI 兼容 HTTP 转发服务。它接收客户端请求，按本地
端点和路径选择任务，在命中明确规则时修改请求，再将上游响应返回给原客户端。

当前设计遵守以下原则：

1. 未命中改写规则时保持透明转发。
2. 业务规则、路由、传输和宿主生命周期互相隔离。
3. 所有请求改写都产生可由 `request_id` 串联的结构化日志。
4. 内部队列、请求体和非流式响应体都有明确上限。
5. SSE 按 chunk 转发，不因日志或统计累计完整响应。
6. 配置 reload 不改变已经开始处理的请求。

## 当前拓扑

```text
Codex / cc-switch / other client
              |
              +--> 127.0.0.1:15723
              |      POST /v1/responses[/]
              |      GET  /v1/usage
              |
              +--> 127.0.0.1:15724
                     POST /v1/chat/completions
                     GET  /v1/usage

Both listeners
      |
      v
bounded connection admission
      |
      v
shared on-demand worker pool
      |
      v
TaskRouter -> transform pipeline -> Proxy / WinHTTP -> upstream
      |                                  |
      +-------------- Logger -----------+
```

Responses 和 Chat 端点分别拥有自己的监听地址、上游 base URL、主请求路径和
Usage 路径。Usage 的上游归属由接收端点决定，不根据请求内容猜测。

两个 listener 只承担连接接入。它们共享：

- `max-connections` 总容量；
- 一个 FIFO 任务队列；
- 一个按需扩容的 worker pool；
- 一个 logger 和一组 runtime metrics；
- 一个进程级 WinHTTP session。

任一 listener 启动失败都会使整次启动失败并释放已绑定端口。运行中不可恢复的
accept 错误会停止整个服务，避免只剩半套端点继续工作。

## 模块边界

```text
hosts -> config + AppService
AppService -> Server
Server -> routing + transforms + logging + transport
transport -> core HTTP/config types + operating-system APIs
transforms -> core task/transform types + structured JSON
core -> C++ standard library and platform-neutral data types
```

| 模块 | 当前职责 | 禁止承担 |
| --- | --- | --- |
| `src/hosts` | CLI 输入、退出码、服务启停 | 复制路由、改写或网络逻辑 |
| `src/config` | CLI、profile schema、路径、校验、snapshot | 根据请求选择任务 |
| `src/core` | 生命周期、任务类型、路由基础类型、取消、指标 | 持有宿主 UI 状态 |
| `src/server` | listener、容量控制、请求编排、reload | 保存无关上游特例 |
| `src/transport` | HTTP 头过滤、WinHTTP 请求、SSE 回调 | 判断 findcg 或修改 JSON |
| `src/transforms` | 按任务隔离的结构化请求改写 | 创建 socket 或日志文件 |
| `src/logging` | JSON Lines、批写、flush、背压 | 决定业务规则 |

`ccs-trans-core` 静态库包含共享服务实现，`ccs-trans` 可执行目标只提供 CLI
宿主。后续宿主必须复用同一 `AppService`，不能重新实现启动和关闭顺序。

## 配置模型

当前运行时使用一个不可变 `AppConfig` snapshot：

```text
AppConfig
  responses_endpoint
    listen_host / listen_port
    upstream_url
    main_task
    usage_task
  chat_endpoint
    listen_host / listen_port
    upstream_url
    main_task
    usage_task
  logging
  timeouts
  body limits
  worker_threads / max_connections
```

每个 `TaskConfig` 包含 method、本地路径、上游路径、transform 名称和日志策略。
`TaskRouter` 输出明确的 endpoint、task 和 `UpstreamTarget`；后续步骤不再读取
模糊的共享 URL。

配置字段遵守一对一规则：一个 CLI 参数只修改一个字段，一个字段只有一个
规范参数名。不存在短参数、同义参数、共享 fallback 或重复参数覆盖。

这里的 endpoint group 是当前实现基线，不是后续继续扩展协议的目标模型。阶段 11
将应用级 listener/runtime 与代理链拆开：一个应用配置持有多个 enabled Profile，
每个 Profile 只保存 protocol、本地精确路由、upstream 和有序 rules。新模型通过
编译后的 `RuntimeSnapshot` 继续满足本节的不可变 generation 约束。

## 持久 Profile

持久数据布局：

```text
%USERPROFILE%/.ccs-trans/
  config.json
  logs/
    ccs-trans.log
  state/
```

Windows 通过账户目录 API 解析用户主目录，不接受环境变量替换配置根。相对日志
路径必须留在 `.ccs-trans` 内；绝对路径可显式指向其他位置。

当前生产 `run` 仍读取 `ccs-trans.config/v1`，但 v2 editable domain 与独立
`ConfigStore` 已实现，尚未接入当前 server。v2 store 严格拒绝未知字段、错误 JSON
类型、旧 schema、非法路径和越界数量；保存持有跨进程 write lock，检查加载时源字节
未变化，写入临时文件并回读 round-trip 后再原子替换。任何失败都不覆盖目标文件。
Authorization、Cookie、API key 和其他凭据不属于 profile schema，只随每次 HTTP
请求转发。

v2 `config/profile/rule/run` 命令 parser 与管理命令 executor 也已独立实现并通过临时
应用根测试，但生产 `cli_main` 暂不分流到它。若管理命令先写 v2、`run` 仍读 v1，
一次成功编辑就会让当前服务无法启动；因此 host 激活与 RuntimeSnapshot/单 listener
在 11.7 同一提交切换。当前可执行文件在此之前继续完整使用旧生产 CLI，不形成一个
写新 schema、读旧 schema 的危险中间态。

v2 `RuntimeCompiler`、`RuntimeSnapshot` 与 `RouteTable` 已完成但同样尚未接入生产
server。compiler 只复制 enabled profile，或为 `run --profile` 单独编译一个完整草稿；
route 持有 immutable `RuntimeProfile`、独立 upstream target 和 enabled rule 定义副本。
RouteTable 使用 canonical path 的外层 hash 与 method 的内层 hash，lookup 不扫描
profile。编译失败不替换调用方已有 snapshot。

`ProtocolRegistry` 已接入 compiler。Responses、Chat、Messages handler 提供稳定 id、
主/Usage method、Usage/SSE/JSON capability、专用 rule 适用性和本地错误 envelope；
RuntimeProfile 直接持有 immutable handler。compiler 构造时复制 registry snapshot，
后续外部注册不会改变已发布 generation。OpenAI handlers 生成 OpenAI error object，
Messages 生成 Anthropic error envelope；上游 response 仍透明，不由 handler 改写。

当前 schema 中一个 profile 表示整套 `AppConfig` 覆盖并可被选为 active profile。
重构后的 Profile 改为“一条可同时启用的代理链”，两种含义不能混用。v2 loader
明确拒绝旧结构并保留原文件，不建立双模型 fallback；在 11.7 server 切换前，两套
内部类型只作为开发期隔离存在，不会让一个 `run` 同时接受两种 schema。详细决策见
[Reconstruction.md](Reconstruction.md)。

运行配置合并顺序：

```text
built-in defaults
      -> active profile or --profile
      -> explicit run options
      -> validated immutable ConfigSnapshot
```

命令行覆盖只影响本次运行，不回写 profile。

## 路由与任务

| 接收端点 | method / local path | 任务 | 上游目标 |
| --- | --- | --- | --- |
| Responses | `POST /v1/responses[/]` | `responses` | Responses URL + request path |
| Responses | `GET /v1/usage` | `responses_usage` | Responses URL + Usage path |
| Chat | `POST /v1/chat/completions` | `chat_completions` | Chat URL + request path |
| Chat | `GET /v1/usage` | `chat_usage` | Chat URL + Usage path |

Responses 本地路径同时接受有无尾斜杠形式。其他未配置路径返回结构化 404；已知
路径使用错误 method 时返回结构化 405。未配置对应 endpoint upstream 时，该端点
不会接受可转发任务。

查询参数保留在目标路径中。Hop-by-hop 头和需要由 WinHTTP 重建的头会被过滤，
其他请求头和响应头保持透传。

## 请求改写

当前唯一业务改写为 `remove_findcg_image_gen`，只有同时满足以下条件才执行：

1. 任务为 Responses；
2. 上游 URL 解析后的 host 精确等于 `findcg.com` 或 `www.findcg.com`；
3. 请求体是合法 JSON；
4. 根级 `tools` 是数组并包含目标工具声明。

改写使用 `nlohmann/json` 结构化解析。它只删除根级工具数组中的目标项，不会
搜索或修改背景文本、用户输入、嵌套对象或字符串内容。未命中时继续使用原始 body，
避免透明请求承担不必要的重新序列化成本。

解析或规则执行失败时，请求不会携带部分修改后的 body 发往上游。改写结果记录
规则名、匹配结果、删除数量和 request/task 标识，但业务判断不依赖日志成功与否。

## 请求生命周期

```text
accept
  -> connection admission
  -> parse and enforce request limit
  -> capture ConfigSnapshot generation
  -> route
  -> transform
  -> send upstream
  -> stream or buffer response
  -> send client response
  -> release connection capacity
```

每个请求在开始编排时捕获一个 `shared_ptr<const ConfigSnapshot>` generation。
reload 之后的新请求读取新 generation，已经进行中的请求继续使用旧上游、路径、
限制和日志策略，直到自然完成或取消。

`AppService` 提供 `start`、`reload`、`stop`、`wait` 和状态查询。停止顺序保证：

1. 停止接受新连接；
2. 唤醒接入和 worker 等待；
3. 取消仍在执行的上游请求；
4. 合并线程；
5. drain 并关闭日志 writer。

## 并发与容量

默认 `max-connections=64`，覆盖正在执行和排队的连接。超过容量的新连接会收到
明确的过载响应，不会无限增长内存。

默认 `worker-threads=32` 表示最大 worker 数，不是启动时固定线程数。服务预热
8 个 worker，在队列出现需求时增长，空闲时不创建全部线程。同步 worker 模型的
当前负载口径为：

- 聚合 8-16 路 SSE 是桌面常规负载；
- 50 路连接是压力测试和容量边界；
- Usage 必须在混合 SSE 负载中保持可用；
- 是否更换异步网络模型必须由 benchmark 和资源指标证明。

所有内部队列都必须有上限。SSE 内存占用不得随累计流长度线性增长。

## 上游传输

Windows transport 使用一个进程级 WinHTTP session，每个请求拥有独立 request
handle。WinHTTP 按 scheme、host 和 port 管理连接复用。

当前 transport 读取 Windows 11 21H2 当前用户系统代理 snapshot：direct 使用
WinHTTP no-proxy session，手动系统代理使用 named-proxy session；显式 PAC URL 由
`WinHttpGetProxyForUrl` 按请求解析，再把 direct 或 named-proxy 结果固定到该请求。
Internet Settings watcher 在变化时发布新 session；请求只复制 shared snapshot，
in-flight 请求保留旧 session。仅启用 auto-detect 时不执行 WPAD，避免无代理桌面环境
承担网络发现和固定延迟。

系统明确 direct 或 bypass 时可以直连；系统已经选择代理而该代理失败时不得回退
direct；`407` 被归类为不支持代理认证。启动和 upstream request 日志记录
`upstream_proxy_mode=windows_system`，不记录代理地址、PAC 或凭据。

目标 macOS transport 链接系统 libcurl，只继承启动进程的 terminal proxy 环境，
不读取或修改 macOS 系统代理。Finder/Login Item 启动且没有这些环境变量时直接
连接。完整平台合约见 [Reconstruction.md](Reconstruction.md)。

超时按阶段独立配置：

```text
resolve
connect
send
response header
SSE stream idle
optional total request
```

阶段超时和客户端断开都通过 `CancellationToken` 关闭对应 WinHTTP request
handle。取消只影响目标请求，不关闭共享 session 或其他连接。

普通响应在 `max-response-body-size` 内缓冲后返回。SSE 响应收到 chunk 后立即送往
客户端并递增日志序号，不保留完整 `response.body`。客户端发送失败会触发取消，
worker 随后释放。

## 日志与指标

日志格式为每行一个 JSON object。普通请求链至少可以通过 `request_id` 关联：

```text
request_received
route_selected
transform_result
upstream_request
response_chunk / upstream_response
response_sent / request_error
```

正常日志默认允许约 100 ms 批量窗口；错误事件要求立即 flush。writer 只有一个，
待写容量按总 pending bytes 计算并包含正在写入的 batch。容量耗尽时生产者背压，
记录不得静默丢弃。

指标区分：

- batch window 等待；
- pending-capacity 背压等待；
- 实际文件 write 和 flush 时间；
- 当前/最大队列记录数与字节数；
- 最老 pending 记录年龄与已写记录最大年龄；
- writer 健康状态和失败次数；
- endpoint queue wait、活动连接和 worker 高水位；
- WinHTTP 阶段耗时、timeout 和 cancellation。

Usage 不进入包含 headers、query 和 body 的普通请求链。它只写最小 completion 或
rejection 事件，保留 endpoint、task、target、HTTP 状态和耗时。

## Reload 语义

reload 先解析并完整校验新配置，再决定应用方式：

- 上游、路由、timeout、body 限制、日志内容策略和连接容量可由新 generation
  应用于后续请求；
- listener 地址、worker 拓扑、部分日志 writer 或 metrics 生命周期变化需要受控
  重启；
- 重启失败时恢复旧 snapshot 和旧服务，不能留下部分应用状态；
- in-flight 请求不迁移 generation。

配置保存和运行时 reload 是两个独立动作。写入成功不代表未校验配置可以直接进入
运行状态。

## 错误与安全

本地协议错误使用 OpenAI 风格 JSON error body，并带稳定的 HTTP 状态码。主要
分类包括未知路由、method 错误、请求过大、上游不可达、阶段 timeout、响应过大、
改写失败和服务过载。

日志是高敏感数据。`redact-sensitive=true` 只处理已知敏感 header，不清理 JSON
body 中的密钥或上下文。发布包、测试 fixture 和 benchmark 不得从真实日志复制数据。

发布包使用明确白名单，只包含可执行文件、用户文档和第三方许可证。用户配置、
日志、benchmark 输出和临时目录都不得进入包内。

## 验证基线

当前验证层次：

1. `ccs-trans-core-tests` 覆盖配置、URL、路由、transform、日志边界和错误分支。
2. `ccs-trans-reload-integration` 验证旧请求保持旧 generation、新请求切换新上游，
   并验证运行中 profile 原子保存与读取。
3. Python 集成测试覆盖双 endpoint、Usage、普通响应、SSE、timeout、取消和错误。
4. benchmark 覆盖 `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50`
   以及 transform 微基准。
5. `tests/fixtures/stage11` 固定 findcg transform 矩阵、透明 request bytes 和当前
   schema 只读样例，供重构前后复用。

`mixed-16` 同时运行 8 路 Responses SSE 和 8 路 Chat SSE，并持续向两个 endpoint
发送 Usage。验收要求两组 Usage 都不等待全部 SSE 完成，且 logger 不报告 writer
failure 或未预期的 backpressure。

## 当前边界

- 上游 transport 和 listener 仍是 Windows 实现。
- Windows 自动系统代理已实现；macOS system-libcurl transport 尚未实现。
- 当前协议模型固定为 Responses、Chat Completions 及各自 Usage。
- 当前 transform 注册方式足以承载已实现规则，但通用规则配置仍未实现。
- tray、开机自启、双击后台运行和 macOS 菜单栏宿主尚未实现。

已批准的演进方向是：

1. 先把双 endpoint 模型改为应用级单 listener 与按精确 local path 路由的多
   Profile。
2. 用 Protocol registry 承载 Responses、Chat 和 Messages 的协议知识。
3. 用配置期编译的 Rule pipeline 替换 `Server` 中的 transform 名称和 findcg host
   特判。
4. 复用当前 worker、WinHTTP、logger、cancellation、timeout 和 reload generation，
   不在业务重构中同时重写网络栈。
5. 重构完成后再实现 Windows tray，随后实现 macOS transport 与菜单栏宿主。

新增协议和规则不能继续堆成 `Server` 或 `Proxy` 中的条件分支。目标目录和迁移顺序
分别以 [ProjectStructure.md](ProjectStructure.md) 与
[DevelopmentPlan.md](DevelopmentPlan.md) 为准。
