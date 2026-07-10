# ccs-trans 开发计划

## 当前进度

`0.3.0` 已完成阶段 0–9：在 `0.2.0` 的双任务转发和 findcg Responses 改写基础上，新增合成 benchmark、运行时资源指标、客户端断开取消、分阶段 timeout 及其自动测试。

阶段 9 的修正后基准表明，8/16 路常规 SSE 负载没有 worker 排队，保留同步 worker + WinHTTP 模型；阶段 10 默认 worker 调整为 32，为 16 路长 SSE 之外保留短请求和 Usage 余量。历史 50 路压力负载受 16 个 worker 上限约束，会产生约 2 秒首字节排队，因此继续作为明确的容量边界，而不是立即重写全异步网络栈的理由。

2026-07-11 的 Codex -> ccs-trans -> findcg 实测包含两次 Responses 请求。两次请求都从根级 `tools` 删除了 1 个 `image_gen` namespace 工具，上游均返回 `200`；SSE 分别连续转发 57 和 72 个 chunk，序号无缺口，日志中没有 warning、error 或 4xx/5xx。`0.2.0` 功能验收至此完成。

## 当前开发目标

进入阶段 10：先修复正常负载下日志批写可能停滞的问题，再完成双监听端口与 Usage 归属拆分，最后实现 Windows `%USERPROFILE%/.ccs-trans/` 与 macOS `~/.ccs-trans/` 下的持久 profile、schema/version 校验、CLI 覆盖层和不可变运行快照。阶段 10 是破坏性 CLI 升级，不保留旧参数或别名。

后续阶段保持以下 `0.3.0` 请求改写和传输语义不变，但会调整监听拓扑：

- Responses 任务只处理 `/v1/responses` 和 `/v1/responses/`，只在目标上游是 findcg 时执行 `image_gen` 清理。
- Chat Completions 任务只处理 `/v1/chat/completions`，使用独立上游 URL 和路径，不参与 Responses 的 findcg 特殊改写。
- 非 findcg Responses、两组 Usage 和未知路由继续保持透明转发或原有错误行为。
- Responses 端点组监听 `127.0.0.1:15723`，只接收 Responses 与其 Usage；Chat 端点组监听 `127.0.0.1:15724`，只接收 Chat Completions 与其 Usage。

持久配置工作不得改变 transform 选择、body 内容、SSE 顺序、状态码、错误模型、取消语义或日志完整性语义。

## 长期演进目标与阶段边界

在当前转发功能稳定后，项目还需要继续支持：

- 性能优化，重点覆盖高并发、长时间 SSE、内存占用、连接复用和日志吞吐。
- Windows 托盘图标和 macOS 菜单栏图标后台常驻，以及点击图标显示操作菜单。
- Windows 双击启动时隐式运行，不弹出持续驻留的控制台窗口。
- 配置长期保存、启动时自动加载，以及未来由托盘菜单修改和重载配置。
- macOS 编译和打包，同时提供命令行产物与菜单栏常驻应用。

阶段 9 已完成；持久配置、托盘宿主和 macOS 继续按阶段 10–12 实施。已经建立的任务、transform、transport、日志和服务边界必须保持，后续功能不能重新堆回 `Server`、`Proxy` 或 `main`。

### 当前结构边界状态

| 边界                         | `0.3.0` 状态                                      | 后续工作                                               |
| ---------------------------- | ------------------------------------------------- | ------------------------------------------------------ |
| WinHTTP 生命周期             | 进程级 session、请求级 handle、取消和分阶段 timeout | macOS transport 保持同一语义                          |
| SSE 内存                     | 不聚合完整 response body，只保留 chunk 计数       | 后续增加长时间 soak 回归                               |
| 日志写入                     | 单 writer、有界队列、100 ms 批写、错误 flush 和指标 | 阶段 10/11 增加轮转与保留期                           |
| 接入容量                     | `max-connections` 总量限制、稳定 `503`、断连取消   | 后台宿主展示运行状态                                   |
| 同步 worker                  | 8/16 路常规负载继续使用，50 路按 worker 上限排队   | 常规负载退化时才设计全异步迁移                         |
| Windows 网络 API            | `Proxy`/`Server` 仍依赖 WinHTTP/Winsock            | macOS 阶段抽象平台 transport/listener                  |
| 配置生命周期                 | CLI 生成运行配置，尚无持久配置快照                 | 阶段 10 分离配置文件、CLI 覆盖和不可变快照             |
| 服务生命周期                 | 已有 `AppService start/stop/status/wait`           | 持久配置阶段增加 reload，tray 阶段复用同一服务状态机    |

### 预留原则

1. 现在先稳定模块边界，不提前实现完整异步网络栈、托盘 UI 或配置热重载。
2. Responses 与 Chat Completions 都通过任务描述进入通用流水线，特殊规则以 transform/strategy 插件存在。
3. 核心业务层不包含 WinHTTP、Winsock、Windows 消息循环或 macOS Framework 类型。
4. 日志完整性是显式语义；任何性能模式都不能静默丢失日志。
5. 性能优化先建立基线，再替换瓶颈；不以未经测量的大规模重写作为起点。

## 阶段 0：整理可演进边界

目标：在实现双上游和 `image_gen` 清理前，先形成以后可以替换网络、日志和进程宿主而不改业务规则的最小边界。

构建顺序：

1. 引入 `TaskConfig` 或等价结构，集中描述任务名、本地路由、上游目标和改写策略。
2. 路由层只生成 `RouteDecision`，不直接选择 `AppConfig` 中的零散字段。
3. 定义请求改写接口，例如 `RequestTransform` 与 `TransformResult`；未命中的请求返回原 body，不进行 JSON 往返序列化。
4. 让代理入口接收解析好的 `UpstreamTarget`，停止在 `Proxy` 内部读取全局单一 `upstream_url`。
5. 将 WinHTTP 细节收敛到传输实现中，业务层只依赖 `UpstreamTransport` 接口。
6. 为服务增加明确的启动、停止和状态边界；本阶段 CLI 仍然是唯一宿主。
7. 为 Logger 建立 sink/writer 边界；普通事件由单 writer 最长约 `100 ms` 批写，错误事件立即 flush，业务代码不依赖文件流细节。

阶段完成标准：

- 新增 Responses 改写不需要修改底层 WinHTTP 实现。
- Chat 新增独立改写时不需要修改 Responses 分支。
- 未来替换为 macOS 传输实现时，任务匹配和改写代码无需变化。
- 未来托盘宿主可以调用服务生命周期，而不是复制 CLI 启动流程。

## 阶段 1：确认现有链路边界

目标：先把当前代码里的路由、配置和转发入口摸清楚，避免把 findcg 特殊逻辑散落到通用代理层。

构建顺序：

1. 阅读 `config`、`server`、`proxy`、`header_filter` 相关模块。
2. 标出本地入口：

```text
POST /v1/responses
POST /v1/responses/
POST /v1/chat/completions
GET  /v1/usage
```

3. 标出现有上游配置项与拼接逻辑。
4. 确认 SSE 流式转发与普通 JSON 转发共用的请求发送入口。
5. 梳理日志字段，确认能记录改写前后的诊断信息，但不泄露更多敏感信息。

阶段完成标准：

- 能明确指出 Responses 和 Chat Completions 各自经过的函数。
- 能明确指出请求 body 在哪里进入上游发送流程。
- 能确定 `image_gen` 清理只需要发生在发送上游之前。

## 阶段 2：拆分 Responses 与 Chat Completions 上游配置

目标：把原本共享的上游 URL 拆成两个独立任务，为后续改写另一系列请求预留入口。

构建顺序：

1. 在任务配置结构中增加独立上游：

```text
--responses-upstream-url
--chat-upstream-url
--responses-upstream-path
--chat-upstream-path
```

2. 本阶段曾保留旧参数兼容；该决定在阶段 10 被明确废止：

```text
--upstream-url
--upstream-responses-path
--upstream-chat-path
```

3. 以下回退规则只描述 `0.2.0`/`0.3.0` 历史实现，不进入下一版：

```text
responses-upstream-url 未设置时回退到 upstream-url
chat-upstream-url 未设置时回退到 upstream-url
responses-upstream-path 未设置时回退到 upstream-responses-path
chat-upstream-path 未设置时回退到 upstream-chat-path
```

4. 启动时在配置摘要里分别打印 Responses 和 Chat Completions 的目标上游。
5. 校验规则调整为：至少有一个任务配置了可用上游；启用某类请求时必须能解析出该类上游。

阶段完成标准：

- Responses 和 Chat Completions 可以指向不同 base URL。
- `0.3.0` 的旧启动命令仍然可用；阶段 10 完成后必须使用新的唯一参数名或持久 profile。
- 日志中能看出每个请求使用的是 `responses` 任务还是 `chat_completions` 任务。

## 阶段 3：识别 findcg Responses 请求

目标：只对“Responses + findcg 上游”启用特殊处理，不影响其他上游。

构建顺序：

1. 增加上游匹配函数，例如：

```text
is_findcg_responses_target(upstream_url, upstream_path)
```

2. 匹配规则先保持保守：

```text
host 等于 www.findcg.com 或 findcg.com
api_type 等于 responses
```

3. 对大小写、尾部斜杠和路径拼接做规范化，避免 `https://www.findcg.com/` 与 `https://www.findcg.com` 判断不一致。
4. 日志中增加布尔字段：

```text
rewrite_enabled
rewrite_reason
```

5. 对非 findcg、Chat Completions、Usage、未知路由明确记录 `rewrite_enabled: false`。

阶段完成标准：

- findcg Responses 请求被识别为需要改写。
- findcg Chat Completions 请求不触发 Responses 改写。
- 非 findcg Responses 请求保持原样。

## 阶段 4：实现 Responses 请求体清理

目标：在转发到 findcg 前删除根级 `tools` 数组中的 `image_gen` namespace/tool 字段。

构建顺序：

1. 在 findcg Responses 对应的 request transform 中解析 JSON body。
2. 只处理根级 `tools` 数组。
3. 删除满足以下条件之一的工具项：

```text
type == "namespace" && name == "image_gen"
name == "image_gen"
namespace == "image_gen"
```

4. 暂不删除普通文本背景里的 `image_gen` 字符串，因为那是上下文说明，不是工具声明。
5. 保留 `view_image`、`web_search`、`tool_search` 和其他工具。
6. 重新序列化 JSON，并更新实际发送 body。
7. 不手动设置旧的 `Content-Length`，继续交给 HTTP 客户端按新 body 计算。

阶段完成标准：

- 原始 Codex 请求中 `tools` 有 `image_gen` 时，上游请求体中不再包含根级 `image_gen` 工具。
- 用户消息和背景文本不被误删。
- 没有 `tools` 或 `tools` 不是数组时，请求保持原样并正常转发。
- JSON 解析失败时不做静默损坏，应返回明确错误或按配置决定是否透明转发。

## 阶段 5：改写日志与可观测性

目标：能从日志里一眼看出这次请求是否被清理，以及清理了什么。

构建顺序：

1. 在 `upstream_request` 或新增事件中记录：

```text
rewrite_enabled
rewrite_name
removed_tools_count
removed_tools
original_body_size
rewritten_body_size
```

2. `removed_tools` 只记录工具名称和类型，不记录完整参数 schema。
3. 当没有删除任何工具时记录 `removed_tools_count: 0`。
4. 出错时写入 `request_error`，包含 `rewrite_error` 类型。

阶段完成标准：

- 日志能证明 `ccs-trans` 删除了 `image_gen`。
- 日志能证明非 findcg 请求没有被改写。
- 日志不会额外扩大敏感信息暴露面。

## 阶段 6：Chat Completions 独立任务入口

目标：把 Chat Completions 作为独立任务整理好，后续可用于改写另一系列请求。

构建顺序：

1. 为 Chat Completions 建立独立任务分支或策略对象。
2. Chat 请求使用 `chat-upstream-url` 和 `chat-upstream-path`。
3. 当前阶段 Chat 只透明转发，不做 `image_gen` 清理。
4. 日志中标记：

```text
task: "chat_completions"
rewrite_enabled: false
```

5. 保留流式与非流式响应支持。

阶段完成标准：

- Chat Completions 与 Responses 可同时并行处理。
- Chat 的上游可以和 Responses 不同。
- Chat 未来新增改写规则时，不需要改动 Responses findcg 规则。

## 阶段 7：测试与复现

目标：用 mock upstream 和真实日志样例覆盖这次问题。

构建顺序：

1. 增加 mock upstream 测试：接收请求后回显 body。
2. 测试 findcg Responses 规则：

```text
输入 tools 包含 image_gen
输出上游 body 不包含根级 image_gen
其他 tools 保留
```

3. 测试非 findcg Responses：

```text
输入 tools 包含 image_gen
输出上游 body 保持原样
```

4. 测试 Chat Completions：

```text
使用 chat-upstream-url
不触发 Responses 改写
```

5. 测试 SSE：

```text
stream: true 请求改写 body 后仍能流式返回
```

6. 测试错误场景：

```text
非法 JSON
tools 不是数组
上游不可达
```

阶段完成标准：

- 能复现之前 findcg `Image generation is not enabled for this group` 的触发条件。
- 清理后 mock upstream 证明上游收不到根级 `image_gen` 工具。
- 原有路径兼容测试全部通过。

## 阶段 8：文档、打包和回归验证

目标：形成可直接使用的新版本。

构建顺序：

1. 更新 README，给出双上游示例：

```text
ccs-trans --responses-upstream-url https://www.findcg.com/ --chat-upstream-url <chat-url>
```

2. 说明 findcg Responses 的 `image_gen` 清理行为。
3. 说明其他上游不会被自动改写。
4. 重新构建 release exe。
5. 覆盖 dist 包中的可执行文件和文档。
6. 用当前 Codex -> ccs-trans -> findcg 链路发一次 `hi` 验证。

阶段完成标准：

- dist 中的 `ccs-trans.exe` 是新构建版本。
- dist 文档与源码文档一致。
- 当前 Responses 转发任务可以通过 findcg，不再因为根级 `image_gen` 工具被拒绝。

完成状态：已完成。2026-07-11 的真实 Codex 链路验证了改写、上游 `200` 和连续 SSE 转发；实测日志仅作为本地诊断证据保留，不纳入 Git 或发布包。

## 阶段 9：性能基线与低风险优化

目标：在托盘后台常驻和 macOS 移植前，先让核心代理在长时间运行下有可测量、可控制的资源行为。

### 9.0 已有起点

`0.2.0` 已提前完成以下结构优化，阶段 9 直接测量它们，不再以重复改造作为 benchmark 前置条件：

- 进程级 WinHTTP session 和每请求独立 request handle。
- SSE 不聚合完整 response body，使用从 `0` 开始的 chunk 序号日志。
- 单日志 writer、有界队列、普通事件约 100 ms 批写和错误立即 flush。
- `worker-threads` 与 `max-connections` 分离，过载返回稳定 `503`。
- 请求体、非流式响应和日志 body 使用独立限制。

### 9.1 建立 benchmark 工具

构建顺序：

1. 新增 `tests/benchmark/`，与断言协议正确性的 `tests/integration/` 分开。
2. 增加可配置 mock upstream：可控制首字节延迟、chunk 大小、chunk 间隔、总 chunk 数、普通响应大小和客户端中途断开。
3. benchmark runner 负责启动/停止代理与 mock、等待端口就绪、并发发起请求，并在异常路径上回收所有子进程。
4. 输出带 `schema_version`、Git commit、构建类型、CLI 参数、负载参数和机器信息的 JSON 结果；终端摘要只用于阅读，JSON 才是比较来源。
5. 提供短时 smoke profile、8/16 路桌面 profile 和 50 路压力 profile；持续时间、预热和重复次数均显式记录，不能把不同 profile 的结果混在一起。
6. benchmark 只使用合成请求和合成凭据，禁止读取或打包真实 findcg 日志。

至少覆盖：

```text
非流式小响应
约 100 KB 的 Responses 请求改写
8 个、16 个、50 个并发 SSE 长连接
日志 body 开启和关闭两组场景
同一上游与 Responses/Chat 不同上游两组场景
客户端中途断开
```

### 9.2 增加低开销指标并记录 `0.2.0` 基线

先增加只使用原子计数或既有锁保护的进程内指标，再运行基线。指标不得为每个 chunk 新增文件 flush 或无界内存记录。

记录：

```text
本地代理附加延迟 p50/p95/p99
首字节时间 TTFB
吞吐量和活动连接数
进程 CPU、峰值 RSS/Working Set
单个 SSE 连接随时间增长的内存
日志队列深度、写入延迟和背压次数
上游连接新建数与复用数
```

运行顺序：

1. 先跑无 body 日志的短时 smoke，验证 runner 和结果 schema。
2. 再分别跑 8、16 路常规 SSE，并观察工作集、线程数、handle 数和日志队列高水位。
3. 跑 50 路压力测试，记录过载响应、尾延迟和资源上限，不把它当作桌面常规 SLO。
4. 打开 body 日志重复关键场景，量化完整日志相对无 body 日志的 CPU、I/O 和延迟成本。
5. 将未经优化的 `0.2.0` 结果保存为对照，后续每项优化使用同一 profile 重跑。

负载口径固定为：`8–16` 路并发 SSE 是桌面常规负载，`50` 路是压力测试。性能验收值不在没有基线时拍定；先要求两个不变量：SSE 内存不随累计响应长度线性增长，内部连接队列、日志队列和缓存都有上限。

### 9.3 按证据补齐运行时行为

1. 定义请求级取消信号，由客户端 socket 断开或响应写失败触发；Proxy 收到信号后关闭对应 WinHTTP request handle，释放 worker 和连接容量。
2. 为“客户端断开但上游继续发送”和“断开时恰逢日志背压”增加集成测试，确保取消不会生成第二份错误响应或破坏其他请求。
3. 将单一 timeout 拆为连接、发送、响应头、流空闲和可选总时长；每类 timeout 使用稳定错误分类和日志字段。
4. 用 profiling 决定是否减少 request/header/body 复制；只有命中 findcg transform 时允许承担 JSON DOM 和 rewritten body 分配。
5. 每完成一项都重跑相同 profile，记录 commit 和前后差异；没有可测收益的复杂优化不保留。
6. 只有 8–16 路常规负载仍因同步 worker 出现不可接受的排队或资源占用时，才为完整异步 I/O 编写独立设计和迁移计划。50 路压力结果不能单独触发重写。

### 9.4 日志与 benchmark 数据安全

`--redact-sensitive false` 与 `--log-body true` 组合会按设计记录 Authorization、Cookie、完整请求上下文和响应 chunk。真实回归已经证明该模式适合排障，但日志必须视为敏感凭据文件：

- Git、测试 fixture、benchmark 结果和发布包不得包含真实链路日志。
- 需要共享日志时先使用 `--redact-sensitive true`，并单独审查 body 是否仍含私密上下文。
- benchmark 默认使用合成 Authorization 和合成 body；性能结果只保存聚合指标，不嵌入请求或响应正文。
- 日志轮转、保留期和文件权限随持久配置/后台宿主阶段实现，本阶段先保持现有完整性语义。

### 9.5 性能取舍

| 选择                      | 取舍                                                               | 当前决定                                                                                 |
| ------------------------- | ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 完整日志与最高吞吐        | 完整 body 会产生复制、序列化和磁盘 I/O                             | 普通事件最长约 100 ms 批写，错误立即 flush；队列满时背压，不静默丢日志                   |
| DOM JSON 与流式 JSON 改写 | DOM 实现可靠但会分配完整对象树，流式解析更省内存但复杂             | 使用固定版本 `nlohmann/json`，只解析命中的 findcg Responses；profiling 后再考虑替换      |
| 同步线程池与全异步 I/O    | 同步模型简单，长 SSE 会长期占线程；异步模型扩展性好但重构大        | 8–16 路为常规、50 路为压力；先修明确瓶颈，再由 benchmark 决定是否异步化                  |
| 原生网络 API 与跨平台库   | WinHTTP 集成系统代理方便，但不可移植；跨平台库会增加依赖和打包成本 | 现在先抽象 transport；macOS 阶段再以可复现构建和 SSE 能力选择 libcurl/Boost.Beast 等实现 |
| 配置热重载与简单重启      | 热重载改善托盘体验，但连接池和进行中请求的一致性更复杂             | 使用不可变配置快照预留；第一版持久化允许重启生效，之后再增加安全热重载                   |

### 9.6 阶段完成标准

- benchmark 可重复运行并输出带环境、commit、配置和 profile 的机器可读结果。
- 保存 `0.2.0` 的 8/16 路常规基线和 50 路压力基线，结果可由同一命令重跑。
- SSE 长流内存不随累计响应长度线性增长；日志与接入队列的高水位和背压可观察。
- 客户端断开能终止对应上游请求，且不会影响同进程其他请求。
- 各阶段 timeout 可独立测试，错误分类稳定。
- 优化前后有同 profile 对照，Responses/Chat/Usage/transform/SSE 自动测试不退化。
- 形成同步模型继续使用或异步化的决策记录，结论引用 benchmark 数据。

### 9.7 实施结果与决策记录

阶段 9 已完成以下交付：

- `tests/benchmark/` 提供合成 upstream、Windows 资源采样、8/16/50 路 profile、机器可读 JSON 结果和约 100 KB findcg transform 微基准。
- `performance_snapshot` 记录连接/worker/日志队列高水位、背压、WinHTTP 连接事件、传输字节、失败、取消和各阶段 timeout 计数。
- 一个共享 `WSAPoll` 监控线程观察客户端 socket；断开后通过请求级 cancellation token 关闭对应 WinHTTP request handle，不为每条 SSE 创建 watcher 线程。
- timeout 已拆为 resolve、connect、send、response header、SSE idle 和可选 total；旧 `--timeout-ms` 只作为前五项的兼容回退。
- 同步 WinHTTP 发送改为 `WinHttpSendRequest` 声明长度、`WinHttpWriteData` 写 body、`WinHttpReceiveResponse` 收响应头，使 send 与 response-header timeout 的阶段边界可测试。
- 自动集成测试覆盖 504 响应头超时、已发送前缀后的 SSE idle/total 超时、两个客户端同时断开后 worker 迅速释放，以及每请求独立连续的 chunk 序号。

修正 benchmark mock 的监听 backlog 后，本机 Release、无 body 日志短时结果如下；这些数字用于同机回归，不作为跨机器 SLO：

| profile | 代理附加 TTFB p50 | 峰值 Working Set | 峰值 worker | 失败 |
| ------- | ----------------- | ---------------- | ----------- | ---- |
| smoke   | `1.058 ms`        | `12.2 MB`        | `4`         | `0`  |
| desktop-8 | `10.535 ms`     | `12.2 MB`        | `8`         | `0`  |
| desktop-16 | `10.640 ms`    | `13.3 MB`        | `16`        | `0`  |
| stress-50 | `1998.996 ms`   | `14.8 MB`        | `16`        | `0`  |

`desktop-16` 开启 body 日志后，附加 TTFB p50 从 `10.640 ms` 增至 `13.116 ms`，日志写入量从约 `0.45 MB` 增至 `2.49 MB`，未触发日志背压。smoke 的 40 个请求只出现 4 次物理连接事件，证明进程级 WinHTTP session 正在复用连接。

早期 `0.2.0` benchmark 使用 Python `ThreadingHTTPServer` 默认 backlog=5；16/50 路同时建连会随机把 direct 或 proxied 一侧分批排队，因此旧高并发 TTFB 只保留为历史原始结果，不用于精确同比。当前 mock 固定 `request_queue_size=128`，后续比较必须使用修正后的 runner。

决策：8–16 路常规桌面负载继续使用同步 worker 模型。50 路压力测试清楚显示连接数高于 `worker-threads` 时会排队，但资源仍有界且无失败；现阶段不承担完整异步 I/O 重构成本。若未来常规负载、后台宿主或 macOS transport 出现不可接受的线程/排队数据，再单独启动异步架构设计。

完成状态：已完成。阶段 10 可以在既有取消、指标和资源边界上开始持久配置实现。

## 阶段 10：日志可靠性、双端口与持久配置

目标：先把日志可靠性、双端口任务边界和 CLI 语义固定下来，再让 CLI 与未来托盘宿主共享一套长期 profile，同时保持无歧义的命令行覆盖能力。

构建顺序：

### 10.1 先定位并修复日志停滞

1. 增加确定性测试，验证普通事件即使后续没有新事件，也会在 `log_flush_interval_ms` 加允许的调度误差内写入并对读取者可见。
2. 验证错误事件在 `log()` 返回前已经 flush，进程正常退出会 drain 全部已接收事件，writer 失败会被明确暴露，不能静默停止写入。
3. 分别测量“等待批次窗口”“队列满背压”和“文件系统 flush 耗时”，日志和指标必须能区分三者，避免把预期的约 `100 ms` 缓冲误判成死锁。
4. 保持有界队列和完整性语义：正常请求线程只应在队列确实满时背压；磁盘永久阻塞无法同时满足零阻塞与零丢失，不能用静默丢日志掩盖故障。
5. 在 8/16 路 SSE、错误立即 flush、停止服务和日志目录暂时不可写等场景回归，正常磁盘条件下不得出现持续增长的 oldest-record age 或无界请求阻塞。

完成状态：已完成。2026-07-11 的 Debug/Release 单元测试覆盖空闲批写、错误同步 flush、析构 drain、日志目录创建失败、注入式 writer flush 失败和慢 sink 容量背压；协议集成测试通过。Release 合成回归中 `desktop-8`/`desktop-16` 均为 0 失败、0 日志背压、0 writer failure，快照分别显示 `1017/1017`、`2009/2009` 条日志已接收/已落盘，空闲 oldest-record age 回到 0。

本工作包 review 修正了三个实现问题：写入中的批次必须继续占用有界容量；batch-window wait 不能混入前序 I/O 排队时间；目录创建失败、sink 返回失败或抛异常以及 failure handler 抛异常都必须收敛为明确状态。新增 `LogSink` 注入边界、`LogWriterStatus`/故障回调，以及 batch wait、write、flush、oldest age 和 writer health 指标。默认文件 sink 的异步故障会写入宿主标准错误，后续 tray host 可替换为用户可见回调。

### 10.2 拆分双监听端点组

1. 用两个明确的端点组替代单 listener + 三个全局任务：

```text
responses endpoint: 127.0.0.1:15723
  POST /v1/responses[/] -> responses upstream /v1/responses/
  GET  /v1/usage        -> responses upstream /v1/usage

chat endpoint: 127.0.0.1:15724
  POST /v1/chat/completions -> chat upstream /v1/chat/completions
  GET  /v1/usage            -> chat upstream /v1/usage
```

2. 每个端点组只有一个 base upstream URL；该组内主请求与 Usage 使用同一个 base URL，但各自保留唯一的 upstream path 字段。
3. 同一路径 `/v1/usage` 由接收它的端口决定归属，不能再通过全局共享 `usage` 任务或共享 `--upstream-url` 猜测目标。
4. 两个 listener 由同一个 `AppService` 管理，任一端口绑定失败都使启动整体失败并回收已启动 listener，避免半运行状态。
5. 连接容量、worker 使用量和指标先保持进程级边界；日志事件增加 listener/endpoint 字段，能够区分 `responses_usage` 与 `chat_usage`。

完成状态：已完成。两个 listener 在任何 accept/worker 启动前依次完成 exclusive bind/listen，第二个失败会释放第一个并使进程失败；运行期任一 listener 的不可恢复 accept 错误会停止整个服务。两个 acceptor 只负责向同一个 FIFO 队列提交带 endpoint 的 `ClientJob`，继续共享总连接上限、32 worker、logger、metrics、Proxy 和 WinHTTP session。

本工作包 review 将 Windows 端口绑定从 `SO_REUSEADDR` 改为 `SO_EXCLUSIVEADDRUSE`，修正运行期 listener 错误导致半运行的风险，并把本地 405 Usage 与实际上游转发区分为带 `forwarded` 字段的 `usage_completed` summary。`AppService::start()` 增加 readiness 握手，只有 logger、两个 listener、worker 和 acceptor 全部就绪后才同步返回成功；启动失败在返回前恢复 `Stopped` 并提供具体错误。集成测试覆盖双 upstream、两组 Usage 归属、跨端口主路由 404、Usage 敏感头不入日志和第二 listener 绑定失败时的原子回滚；端点级 accepted/rejected/completed/active/queued/queue-wait 指标已进入 `performance_snapshot`。

### 10.3 清理 CLI 并冻结唯一名称

1. 不兼容 `0.3.0` CLI；删除所有标为 legacy 的解析、回退、帮助文本和测试。
2. 删除共享或含义重叠的 `--upstream-url`、`--listen-host`、`--listen-port`、`--usage-path`、`--timeout-ms`、`--max-body-size`、`--upstream-responses-path`、`--upstream-chat-path` 和 `--upstream-usage-path`。
3. 两组监听、主路由、Usage 路由和 upstream path 使用带端点组前缀的唯一参数名；每个参数只映射一个配置字段，每个字段只接受一个参数名，不提供缩写或别名。
4. profile 命令按单一职责拆分为 `list`、`show`、`create`、`remove`、`use`、`set`、`unset`；一次 `set`/`unset` 只修改一个配置键，不通过参数组合推断操作。
5. `run --profile <name>` 读取指定 profile；运行时可以给出多个显式覆盖，但每个覆盖项仍只改变一个字段且默认不写回 profile。
6. 未识别的旧参数直接返回明确错误和迁移提示，不增加隐藏兼容分支。

完成状态：配置模型与运行命令合约已完成。`EndpointGroupConfig` 现在是 base upstream URL、监听地址、主任务和所属 Usage 的唯一所有者，`RouteDecision` 同时携带 endpoint/task 并按请求生成 `UpstreamTarget`。CLI 只接受 `ccs-trans run`、`--help` 和 `--version`；所有运行字段使用唯一长参数，重复参数和布尔别名被拒绝，旧名称逐项返回迁移提示。profile 子命令仍按 10.4 工作包实现，不在本工作包放置空壳命令。

本工作包 review 修正了同 endpoint 主路由/Usage 路由尾斜杠归一化后冲突的问题，并禁止 path 字段包含 query/fragment。单元测试覆盖完整 endpoint 参数映射、Usage 所有权、同监听地址冲突、重复参数、缺少 `run`、严格布尔值、公开 help 和全部旧名称拒绝；当时的单 listener 协议集成与 smoke benchmark 通过，随后 10.2 工作包已激活双 listener。

### 10.4 持久 profile 与目录布局

1. 引入带 `schema_version` 的配置文件，保存命名 profile 和当前默认 profile。
2. 配置优先级固定为：命令行显式参数 > 选中的 profile > 内置默认值。
3. 默认数据根目录固定为 Windows `%USERPROFILE%/.ccs-trans/`、macOS `~/.ccs-trans/`。实现必须通过系统用户目录 API 解析 home，不能只依赖可能缺失或被覆盖的环境变量。
4. 固定默认布局：配置为 `~/.ccs-trans/config.json`，日志目录为 `~/.ccs-trans/logs/`，默认日志为 `~/.ccs-trans/logs/ccs-trans.log`；Windows 文档中的 `~` 等价于 `%USERPROFILE%`。
5. 通过同目录临时文件、完整校验和原子替换保存，避免异常退出留下半份配置；配置文件与日志目录仅授予当前用户所需权限。
6. 将持久字段与临时运行参数分开；命令行覆盖默认不自动写回，只有 profile 修改命令或托盘“应用”才落盘。
7. 使用不可变 `ConfigSnapshot` 提供给请求；重载时新请求使用新快照，进行中请求继续使用旧快照。
8. 第一版不在配置中存储请求头里的 API key、Authorization 或其他转发凭据。

### 10.5 回归、迁移说明与发布

1. 单元测试覆盖唯一 CLI 名称、旧参数拒绝、profile CRUD、schema 校验、路径解析和双端口路由归属。
2. 集成测试同时启动两个 mock upstream，证明两个 `/v1/usage` 分别到达所属主请求的 upstream，且跨端口发送错误主路由时返回 `404`。
3. benchmark 同时覆盖双 listener，确认 8/16 路常规 SSE 和批量日志没有相对 `0.3.0` 的明显退化。
4. README 提供从 `0.3.0` 到新 CLI/profile 的迁移表，但程序本身不承担旧参数兼容。

### 10.6 性能约束与取舍

阶段 10 不重写全异步网络栈，但双 listener、日志修复和配置重载不能破坏阶段 9 已建立的资源边界。

#### Worker 与端点公平性

1. 两个 listener 共用进程级有界 worker pool、连接容量、日志 writer 和运行指标，避免简单复制 `Server` 后把线程、队列和 WinHTTP session 成倍增加。
2. `8–16` 路仍表示两个端点组合后的常规 SSE 总负载；`50` 路仍是压力测试。若以后要求两个端点各自同时承载 16 路 SSE，应作为新的 32 路 profile 单独定容量。
3. 当前 16 个 worker 在 16 路长 SSE 下没有短请求余量。阶段 10 默认值改为 32：16 路常规 SSE 加 16 路 Usage/短请求余量；这仍是一个共享进程级 worker pool，不为两个端点各复制一套完整执行层。
4. 增加 `mixed-16` profile：Responses/Chat 合计 16 路 SSE，同时持续请求两组 Usage。验收时 Usage 不得等待某条 SSE 完成，worker queue wait 必须保持有界并可观察。
5. 两个 listener 共享总容量可能产生端点偏置；先记录 endpoint 维度的 accepted/active/queued/rejected 指标。只有 `mixed-16` 证明存在饥饿时，才增加端点配额或独立执行队列。

#### 配置与重载热路径

1. `config.json` 的读取、JSON 解析、schema 校验和 profile 合并只发生在 CLI 操作、启动或显式 reload，禁止逐请求检查文件时间或重新解析配置。
2. 请求开始时只获取一次不可变 `ConfigSnapshot`，后续路由、transform 和上游选择复用同一快照；进行中请求不观察新配置，避免细粒度锁和中途目标切换。
3. profile 原子保存不得持有 logger、worker queue 或 transport 锁；磁盘写入慢只阻塞配置命令，不阻塞代理请求。
4. reload 先离线构造并完整校验新快照，再一次性交换。只有监听地址、上游连接策略或日志目标变化时才重建对应资源；单纯 profile 元数据变化不能重建 WinHTTP session。
5. 旧快照和旧 transport generation 由进行中请求自然释放，不能为等待 drain 持有全局配置锁。

#### 日志吞吐与完整性

1. 保留单 writer 和增量 SSE chunk 日志；不为双 listener 创建两个日志文件 writer，所有事件以 endpoint/task 字段区分。
2. 普通事件仍以约 `100 ms` 为最大批写窗口，错误事件同步等待 flush。正常磁盘下，生产者等待应只来自队列容量耗尽，不能来自 writer 持锁执行文件 I/O。
3. 增加 oldest record age、batch wait、file write/flush duration 和 writer health 指标。`log_backpressure` 只能表示队列容量压力，不能混入错误事件主动等待 flush 的时间。
4. 完整日志与绝不阻塞在磁盘永久故障下不能同时满足。当前继续选择“有界内存 + 背压 + 明确失败”，不选择静默丢弃；后台宿主阶段再补充用户可见的故障通知。
5. 日志移到用户目录后，用本地磁盘路径运行 benchmark；网络盘、同步盘和杀毒软件拦截属于单独故障 profile，不能与正常桌面基线混算。

#### 阶段 10 性能回归门槛

所有比较使用同一台机器、同一 Release 构建、同一 benchmark profile，至少重复 3 次并比较中位数：

| 指标/场景                         | 阶段 10 门槛                                                                  |
| --------------------------------- | ----------------------------------------------------------------------------- |
| `desktop-8` / `desktop-16`        | 请求失败为 0，SSE chunk 连续，工作集不随累计流长度线性增长                    |
| `mixed-16`                        | 两组 Usage 均成功，短请求不等待 SSE 结束，worker queue wait 无持续增长         |
| 附加 TTFB p50/p95                  | 相对修正后的 `0.3.0` 同 profile 中位数退化不超过 15%/20%                     |
| 峰值 Working Set                  | 相对 `0.3.0` 同 profile 增长不超过 15%，并记录新增 listener/worker 的固定成本 |
| 正常日志路径                      | 无 writer failure、无持续 backpressure，普通记录在批写窗口后可见              |
| reload                            | 配置保存/解析期间代理继续服务；切换后新旧请求分别保持各自一致的 snapshot       |
| `stress-50`                       | 资源保持有界、无崩溃；允许按 worker 上限排队，不以其 TTFB 触发异步重写         |

若 TTFB 数值受 mock 或系统调度噪声影响超过门槛，先扩大重复次数并报告原始分布，不为通过门槛删减日志或放宽正确性语义。

### 10.7 可提交工作包

为降低一次性改动风险，按以下顺序形成可独立验证的提交；后一个工作包只能建立在前一个通过测试后：

1. **日志可靠性**：复现测试、writer health/时延指标、停滞修复和 logger 单元测试。
2. **配置模型与 CLI 合约**：引入 endpoint group 模型，删除 legacy 解析，冻结唯一参数名，不改网络监听行为。
3. **双 listener 与 Usage 归属**：实现 `15723`/`15724` 原子生命周期、端口路由和双 mock 集成测试。
4. **持久 profile**：实现用户目录、schema、CRUD、原子保存、默认 profile 和不可变 snapshot。
5. **reload 与性能回归**：资源 generation 切换、`mixed-16`、8/16/50 profile、迁移文档和发布打包。

每个工作包都运行 Release/Debug 构建、单元测试和相关集成测试。第 5 个工作包完成前不进入 tray 或 macOS host，避免把尚未稳定的配置和生命周期接口复制到图形宿主。

目录选择取舍：

- Windows 目录不会随 Roaming Profile 漫游；这是固定本机配置的预期行为。
- 两个平台都使用隐藏目录，便于 CLI 与图形宿主共享，但不符合 macOS 沙盒应用的标准容器路径。首版 macOS `.app` 按非沙盒签名/公证发行；若未来进入 Mac App Store，需要增加容器目录迁移或兼容层。
- 配置文件位于数据根目录，日志和运行状态使用 `logs/`、`state/` 子路径；日志可能包含凭据，创建目录和文件时应使用当前用户可访问的最小权限。
- 程序不能把当前工作目录、应用 bundle 或可执行文件目录当作持久配置根目录。

阶段完成标准：

- 重启后能恢复两个监听端点、两个端点组上游、两组 Usage 路径、日志路径和性能相关选项。
- 配置损坏时拒绝静默回退，并给出可定位错误。
- CLI、后台宿主和未来 macOS 应用使用同一个解析与校验模块。
- 正常文件系统与 8/16 路常规负载下，普通日志按批次持续落盘；只有队列容量耗尽时允许生产者背压。
- `15723` 的 Usage 只到达 Responses upstream，`15724` 的 Usage 只到达 Chat upstream。
- `mixed-16` 下两组 Usage 均保持可用，配置 reload 不阻塞或改变进行中请求的目标。

## 阶段 11：Windows 后台常驻、托盘菜单与双击启动

目标：Windows 用户可以通过图形宿主管理代理，同时不影响现有 CLI 自动化使用方式。

构建顺序：

1. 将 `AppService` 编译为核心库，由 CLI host 和 tray host 共同调用。
2. 增加单实例控制，第二次启动时激活现有托盘实例或返回明确状态。
3. 托盘图标菜单至少提供：运行状态、启动/停止、重新加载配置、打开日志目录、打开配置、开机自启勾选项、退出。
4. 点击托盘图标显示菜单；双击程序时由图形宿主加载持久配置并隐式启动服务。
5. 启动失败通过通知或对话框显示，不能因为没有控制台而静默失败。
6. CLI target 继续保留标准输出、退出码，并稳定阶段 10 确立的新命令合约；不恢复 `0.3.0` 的 legacy 参数。是否最终采用一个 launcher 或两个可执行文件，在原型验证后决定。
7. 开机自启勾选项必须读取并显示操作系统中的实际注册状态，切换时幂等更新当前用户启动项；不能只保存一个可能与系统状态分离的配置布尔值。
8. 自动开机启动作为独立选项处理，不与“双击隐式运行”绑定。Windows 首选当前用户级启动注册，不要求管理员权限。

阶段完成标准：

- 关闭终端不影响 tray host 中正在运行的服务。
- 菜单操作和 CLI 使用相同的服务状态机及配置校验。
- 后台退出会停止监听、取消上游请求并完整关闭日志 writer。
- 开机自启勾选状态与 Windows 实际启动注册一致，启用、禁用和重复操作都可验证。

## 阶段 12：macOS 编译与打包

目标：业务规则、配置和测试跨平台复用，只替换必要的网络与宿主实现，并交付与 Windows 托盘等价的 macOS 菜单栏常驻入口。

构建顺序：

1. 清除核心 target 对 Winsock、WinHTTP 和 Windows 类型的直接依赖。
2. 为 macOS 接入上游 transport 和本地 HTTP listener，验证系统代理、TLS、SSE 和取消语义。
3. 在 CI 或固定构建环境中覆盖 Apple Silicon `arm64`；按实际用户需求再提供 `x86_64` 或 Universal 2。
4. 先打包 CLI 产物，再为菜单栏后台应用建立 `.app` target；菜单栏图标是正式交付项，不是可选扩展。
5. macOS 菜单至少提供：运行状态、启动/停止、重新加载配置、打开日志目录、打开配置、登录时启动勾选项、退出，并与 Windows 使用同一组应用命令接口。
6. 登录时启动勾选项读取系统实际状态；满足最低系统版本时优先使用 `SMAppService`，否则在确定最低版本后评估受控 LaunchAgent 兼容实现。
7. 规划 `~/.ccs-trans/` 下的配置/日志布局、应用图标、签名、公证和压缩包/DMG 交付流程。
8. 使用同一套 mock upstream 与协议测试验证 Windows 和 macOS 行为一致。

阶段完成标准：

- macOS 构建不包含 Windows 条件分支之外的系统类型泄漏。
- Responses、Chat Completions、Usage、改写规则和 SSE 测试跨平台通过。
- 产物架构、最低系统版本和是否签名在发布说明中明确。
- 菜单栏图标可管理服务并正确显示登录时启动状态；退出会完整停止服务和日志 writer。

## 推荐实现顺序总览

```text
0. 整理任务、改写、传输、日志和服务生命周期边界
1. 确认现有链路边界
2. 拆分 Responses 与 Chat Completions 上游配置
3. 识别 findcg Responses 请求
4. 删除根级 tools 中的 image_gen
5. 增加改写日志
6. 整理 Chat Completions 独立任务入口
7. 补测试和真实样例复现
8. 更新文档、打包、回归验证
9. 建立 benchmark 与资源指标，补取消传播和分阶段 timeout，再决定是否异步化
10. 修复日志停滞，拆分 `15723`/`15724` 端点组，清理 legacy CLI，再加入持久 profile 和不可变配置快照
11. 增加 Windows tray host、菜单、开机自启和双击隐式启动
12. 完成 macOS transport、菜单栏 host、登录时启动、构建和打包
```

核心原则：特殊处理只绑定到 findcg 的 Responses 任务；Chat Completions 从现在开始作为独立任务演进；除明确命中的改写规则外，代理保持透明。当前只提前建立会影响未来成本的边界，不在没有基线时提前重写全部网络模型。
