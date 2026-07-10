# ccs-trans 下一阶段开发计划

## 当前进度

`0.2.0` 已完成阶段 0–7 的代码实现：任务路由、双上游配置、findcg Responses `image_gen` transform、改写日志、Chat 独立入口、纯逻辑测试和双 mock upstream 集成测试均已落地。

根据已确认的性能取舍，进程级 WinHTTP session、100 ms 批量日志 writer、错误立即 flush、SSE 序号 chunk 日志、流式响应去聚合、连接总量上限，以及请求/非流式响应/日志限额拆分已经提前完成。阶段 9 仍需 benchmark、客户端取消传播和分阶段 timeout。

阶段 8 的 Release 构建、`0.2.0` 打包、文档同步和包内 exe 集成测试已经完成。当前只剩需要真实凭据和客户端环境的 Codex -> ccs-trans -> findcg `hi` 人工回归。

## 当前目标

让 `ccs-trans` 接管当前 Codex 发往 findcg 的 OpenAI Responses 请求，并在转发前删除请求体中的 `image_gen` 工具声明，避免上游因为图片生成权限返回 `403 permission_error`。

同时，将 Responses 与 Chat Completions 的上游配置拆成两个独立任务：

- Responses 任务：只处理 `/v1/responses` 和 `/v1/responses/`，只在目标上游是 findcg 时执行 `image_gen` 清理。
- Chat Completions 任务：只处理 `/v1/chat/completions`，使用独立的 streamup/upstream URL 和路径，暂时不参与 Responses 的 findcg 特殊改写。

非 findcg 的 Responses 上游、其他 streamup URL、Usage 查询和未知路由不做特殊介入，保持透明转发或原有错误行为。

## 长期演进目标与本轮边界

在当前转发功能稳定后，项目还需要继续支持：

- 性能优化，重点覆盖高并发、长时间 SSE、内存占用、连接复用和日志吞吐。
- Windows 托盘图标后台常驻，以及点击托盘图标显示操作菜单。
- Windows 双击启动时隐式运行，不弹出持续驻留的控制台窗口。
- 配置长期保存、启动时自动加载，以及未来由托盘菜单修改和重载配置。
- macOS 编译和打包，至少提供命令行产物，之后可扩展为菜单栏应用。

这些功能本轮暂不实现，但当前的双任务和请求改写不能继续直接堆在 `Server`、`Proxy` 或 `main` 中。否则性能优化、托盘宿主和 macOS 传输层都会反复改动同一批业务逻辑。

### 当前实现中已确认的结构性问题

| 当前实现                                            | 后续影响                                 | 计划处理方式                                                |
| --------------------------------------------------- | ---------------------------------------- | ----------------------------------------------------------- |
| 每个请求新建 WinHTTP session 和 connection          | TLS、代理发现和连接建立无法充分复用      | 将上游传输抽象为长生命周期对象，并按上游维护连接资源        |
| SSE 边转发时仍把完整响应累积到 `response.body`      | 长流内存随响应总长度增长                 | 流式路径只保留计数和受限日志缓冲，不聚合完整响应            |
| 日志使用全局互斥锁，每条事件立即 `flush`            | 并发请求在磁盘写入处串行                 | 预留异步日志写入器和有界队列，完整性模式下队列满时施加背压  |
| 固定工作线程同步等待上游，客户端队列无界            | 长 SSE 会占满线程，突发连接可能放大内存  | 增加有界接入、取消传播和容量指标；是否全面异步由压测决定    |
| `Proxy` 直接依赖 WinHTTP，`Server` 直接依赖 Winsock | macOS 无法复用网络实现                   | 将监听器和上游传输放到平台适配层，核心只依赖内部 HTTP 类型  |
| `AppConfig` 由 CLI 一次生成并复制到多个模块         | 配置持久化、热重载和托盘修改困难         | 分离持久配置、运行时覆盖和不可变配置快照                    |
| `main` 直接阻塞运行 `Server`                        | 托盘菜单无法统一执行启动、停止和查询状态 | 提取具有 `start/stop/status/reload` 生命周期的 `AppService` |

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

2. 保留旧参数兼容：

```text
--upstream-url
--upstream-responses-path
--upstream-chat-path
```

3. 兼容规则：

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
- 旧启动命令仍然可用。
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

## 阶段 9：性能基线与低风险优化

目标：在托盘后台常驻和 macOS 移植前，先让核心代理在长时间运行下有可测量、可控制的资源行为。

### 9.1 建立基线

先增加可重复的本地 benchmark，至少覆盖：

```text
非流式小响应
约 100 KB 的 Responses 请求改写
8 个、16 个、50 个并发 SSE 长连接
日志 body 开启和关闭两组场景
同一上游与 Responses/Chat 不同上游两组场景
客户端中途断开
```

记录以下指标：

```text
本地代理附加延迟 p50/p95/p99
首字节时间 TTFB
吞吐量和活动连接数
进程 CPU、峰值 RSS/Working Set
单个 SSE 连接随时间增长的内存
日志队列深度、写入延迟和背压次数
上游连接新建数与复用数
```

负载口径固定为：`8–16` 路并发 SSE 是桌面常规负载，`50` 路是压力测试。性能验收值不在没有基线时拍定；先要求两个不变量：SSE 内存不随累计响应长度线性增长，内部连接队列、日志队列和缓存都有上限。

### 9.2 优先优化顺序

1. 复用进程级 WinHTTP session，每个请求保留独立 request handle，并按 scheme/host/port/proxy 配置隔离连接资源。
2. 流式响应不再累计完整 `response.body`，改用从 `0` 开始编号的 chunk 增量日志。
3. 客户端断开时取消上游请求，尽快释放连接和工作容量。
4. 日志改为单写线程、有界队列和最长约 `100 ms` 的批量落盘；错误事件立即 flush，队列满时阻塞生产者，不丢事件。
5. 移除旧 `--concurrency`，用 `--worker-threads` 表达 worker 数，用 `--max-connections` 限制活动与排队连接总量，并定义明确的过载响应。
6. 减少热路径复制：原始请求体只保留一个所有者，只有命中改写规则时才生成新 body。
7. 将请求体上限、非流式响应缓冲上限和日志 body 上限拆开；SSE 不按累计响应长度截断。
8. 将单一 `timeout_ms` 拆为连接、发送、响应头、流空闲和总时长等超时。
9. 完成上述优化后再次压测；只有线程占用仍是主要瓶颈时，才把服务端和上游传输迁移到异步事件循环。

### 9.3 性能取舍

| 选择                      | 取舍                                                               | 当前决定                                                                                 |
| ------------------------- | ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 完整日志与最高吞吐        | 完整 body 会产生复制、序列化和磁盘 I/O                             | 普通事件最长约 100 ms 批写，错误立即 flush；队列满时背压，不静默丢日志                   |
| DOM JSON 与流式 JSON 改写 | DOM 实现可靠但会分配完整对象树，流式解析更省内存但复杂             | 使用固定版本 `nlohmann/json`，只解析命中的 findcg Responses；profiling 后再考虑替换      |
| 同步线程池与全异步 I/O    | 同步模型简单，长 SSE 会长期占线程；异步模型扩展性好但重构大        | 8–16 路为常规、50 路为压力；先修明确瓶颈，再由 benchmark 决定是否异步化                  |
| 原生网络 API 与跨平台库   | WinHTTP 集成系统代理方便，但不可移植；跨平台库会增加依赖和打包成本 | 现在先抽象 transport；macOS 阶段再以可复现构建和 SSE 能力选择 libcurl/Boost.Beast 等实现 |
| 配置热重载与简单重启      | 热重载改善托盘体验，但连接池和进行中请求的一致性更复杂             | 使用不可变配置快照预留；第一版持久化允许重启生效，之后再增加安全热重载                   |

阶段完成标准：

- benchmark 可重复运行并输出机器可读结果。
- SSE 长流不再保留完整响应，客户端断开能终止对应上游请求。
- 日志和接入队列有上限，过载行为可观察。
- 优化前后结果有对照，不以功能正确性换取吞吐。

## 阶段 10：持久配置

目标：允许 CLI 和未来托盘宿主共享一套长期配置，同时保持命令行覆盖能力。

构建顺序：

1. 引入带 `schema_version` 的配置文件，并提供 `--config <path>`。
2. 配置优先级固定为：命令行显式参数 > 配置文件 > 内置默认值。
3. 默认配置位置使用平台规范目录：Windows 使用 `%APPDATA%/ccs-trans/`，macOS 使用 `~/Library/Application Support/ccs-trans/`。
4. 通过临时文件、校验和原子替换保存，避免异常退出留下半份配置。
5. 将持久字段与临时运行参数分开；命令行覆盖默认不自动写回，只有显式保存或托盘“应用”才落盘。
6. 使用不可变 `ConfigSnapshot` 提供给请求；重载时新请求使用新快照，进行中请求继续使用旧快照。
7. 第一版不在配置中存储请求头里的 API key、Authorization 或其他转发凭据。

阶段完成标准：

- 重启后能恢复监听地址、两个任务上游、日志路径和性能相关选项。
- 配置损坏时拒绝静默回退，并给出可定位错误。
- CLI、后台宿主和未来 macOS 应用使用同一个解析与校验模块。

## 阶段 11：后台常驻、托盘菜单与双击启动

目标：Windows 用户可以通过图形宿主管理代理，同时不影响现有 CLI 自动化使用方式。

构建顺序：

1. 将 `AppService` 编译为核心库，由 CLI host 和 tray host 共同调用。
2. 增加单实例控制，第二次启动时激活现有托盘实例或返回明确状态。
3. 托盘图标菜单至少提供：运行状态、启动/停止、重新加载配置、打开日志目录、打开配置、退出。
4. 点击托盘图标显示菜单；双击程序时由图形宿主加载持久配置并隐式启动服务。
5. 启动失败通过通知或对话框显示，不能因为没有控制台而静默失败。
6. CLI target 继续保留标准输出、退出码和脚本兼容性；是否最终采用一个 launcher 或两个可执行文件，在原型验证后决定。
7. 自动开机启动作为独立选项处理，不与“双击隐式运行”绑定。

阶段完成标准：

- 关闭终端不影响 tray host 中正在运行的服务。
- 菜单操作和 CLI 使用相同的服务状态机及配置校验。
- 后台退出会停止监听、取消上游请求并完整关闭日志 writer。

## 阶段 12：macOS 编译与打包

目标：业务规则、配置和测试跨平台复用，只替换必要的网络与宿主实现。

构建顺序：

1. 清除核心 target 对 Winsock、WinHTTP 和 Windows 类型的直接依赖。
2. 为 macOS 接入上游 transport 和本地 HTTP listener，验证系统代理、TLS、SSE 和取消语义。
3. 在 CI 或固定构建环境中覆盖 Apple Silicon `arm64`；按实际用户需求再提供 `x86_64` 或 Universal 2。
4. 先打包 CLI 产物，再为菜单栏后台应用建立 `.app` target。
5. 规划配置目录、日志目录、应用图标、签名、公证和压缩包/DMG 交付流程。
6. 使用同一套 mock upstream 与协议测试验证 Windows 和 macOS 行为一致。

阶段完成标准：

- macOS 构建不包含 Windows 条件分支之外的系统类型泄漏。
- Responses、Chat Completions、Usage、改写规则和 SSE 测试跨平台通过。
- 产物架构、最低系统版本和是否签名在发布说明中明确。

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
9. 建立性能基线，按连接复用、流式内存、日志和背压顺序优化
10. 加入持久配置和不可变配置快照
11. 增加 Windows tray host、菜单和双击隐式启动
12. 完成 macOS transport、构建和打包
```

核心原则：特殊处理只绑定到 findcg 的 Responses 任务；Chat Completions 从现在开始作为独立任务演进；除明确命中的改写规则外，代理保持透明。当前只提前建立会影响未来成本的边界，不在没有基线时提前重写全部网络模型。
