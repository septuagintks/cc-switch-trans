# ccs-trans 项目设计

## 文档状态

| 项目 | 当前状态 |
| --- | --- |
| 实现基线 | 当前源码与发行基线 `0.7.0` |
| 开发分支 | `0.8-A` Qt Quick 独立 GUI/双工具链技术基线；尚未替换生产 Win32 GUI |
| 当前发行版 | `0.7.0-Windows-x64`；`0.7.0-macOS-arm64`（ad-hoc 签名） |
| 语言基线 | ISO C++20，禁用编译器语言扩展 |
| 支持平台 | Windows 11 21H2+ x64；macOS 26 arm64 |
| 本地入口 | 应用级单 listener，默认 `127.0.0.1:15723` |
| 配置根目录 | Windows `%USERPROFILE%/.ccs-trans/`；macOS `~/.ccs-trans/` |
| 业务模型 | 多 Profile、ProtocolRegistry、RuleRegistry、精确 RouteTable |
| 上游网络 | Windows WinHTTP/system proxy；macOS SDK system libcurl/process environment |

本文描述 `0.7.0` 的生产路径。历史重构目标与扩展约束见
[Reconstruction.md](Archived/Reconstruction.md)，
后续顺序见 [DevelopmentPlan.md](DevelopmentPlan.md)，文件归属见
[ProjectStructure.md](ProjectStructure.md)，当前双平台发布结论见
[Release-0.7.0.md](Archived/Release-0.7.0.md)。

`0.8-A` 已加入不会进入当前请求路径的独立 `ccs-trans-gui.exe` 原型。它使用 Qt 6.10.3/官方 MinGW 13.1，
与 GCC 16 runtime 位于互斥 build tree，不链接 `ccs-trans-core`。原型只验证 typed incremental model、
stable-key selection、全局 motion policy、同步期动画隔离、D3D11/software RHI、idle frame、固定部署与
安装卸载。生产 tray 仍持有 0.7.0 Win32 主窗口；在 `0.8-C/D` IPC 与生命周期完成前，两者没有运行时连接。
精确开发合同和资源预算见 [Planning-0.8.0.md](Planning-0.8.0.md)。

## 项目定位

`ccs-trans` 是本地 LLM API Request Transformation Proxy。它负责：

1. 在一个 HTTP listener 接收请求；
2. 按 method + canonical local path 精确选择 Profile；
3. 执行该 Profile 的有序请求 Rule pipeline；
4. 把请求转发到 Profile 自己的 upstream；
5. 原样返回普通响应或增量转发 SSE；
6. 记录可由 `request_id` 与 `generation_id` 串联的结构化日志。

它不负责 Provider 选择、API key 保存、模型故障转移或 Agent 行为。这些职责属于
cc-switch、Codex 或其他上层客户端。

## 当前拓扑

```text
Codex / cc-switch / other client
              |
              v
       one application listener
              |
              v
   bounded global connection admission
              |
              v
      shared FIFO worker queue
              |
              v
 method + canonical path -> RouteEntry
              |
              v
 Profile -> ProtocolHandler -> CompiledPipeline
              |
              v
 UpstreamTransport / WinHTTP or libcurl -> upstream
              |
              +---- incremental SSE callbacks
              +---- structured Logger
```

所有 enabled Profile 同时进入一个 immutable RuntimeSnapshot。listener、worker、
连接容量、timeouts、body limits、logging 和 metrics 是应用级资源，不复制到 Profile。

## 模块边界

```text
CLI management -> ConfigStore
CLI run -> shared runtime loader -> AppService
Windows tray/main window / macOS menu host -> ApplicationController -> AppService
Native main window -> shared presentation contract -> editing/controller services
AppService -> Server
Server -> RouteTable + ProtocolHandler + CompiledPipeline + Logger + UpstreamTransport
UpstreamTransport -> HTTP types + cancellation + platform implementation
```

| 模块 | 当前职责 | 禁止承担 |
| --- | --- | --- |
| `src/hosts` | v2 CLI、Windows tray/main window、macOS menu、平台操作、退出码、服务启停 | 路由、规则或 transport 逻辑 |
| `src/app` | start/reload/rollback/stop/wait 生命周期 | 解析协议或平台 UI |
| `src/config` | schema、CLI、原子持久化、runtime 编译 | 请求期扫描 Profile |
| `src/routing` | immutable Profile、两级 hash RouteTable | 解析规则 option |
| `src/protocols` | 协议能力、专用布局、本地错误 envelope | socket/worker 生命周期 |
| `src/rules` | factory、编译、共享 DOM pipeline | 上游选择或响应改写 |
| `src/server` | 单 listener、容量、worker、请求编排 | findcg/provider host 特判 |
| `src/transport` | upstream 接口、headers、WinHTTP/libcurl、SSE、取消、timeout | 修改 JSON 请求 |
| `src/logging` | JSON Lines、批写、flush、背压 | 决定业务规则 |
| `src/presentation` | 主窗口值状态、命令结果、关闭决策、UI preference schema | 平台窗口、文件 I/O、JSON DOM 或 runtime 所有权 |

`ccs-trans-core` 是 CLI、Windows tray 和 macOS menu bar 宿主的共享服务核心。
新增宿主必须复用 `AppService`，不能复制初始化和停止顺序。

## 已冻结的宿主扩展边界

阶段 12 已在 `AppService` 上增加进程无关的 `ApplicationController`：

```text
CLI / Windows tray / macOS menu bar
                 |
                 v
       ApplicationController
        | load/compile config
        | start/stop/reload/status
        | reap service completion
                 v
             AppService
                 v
              Server
```

controller 负责“从磁盘构建可运行服务”的完整编排，后续宿主不再直接拼装 ConfigStore、
RuntimeCompiler 与 AppService。它不包含 Win32、AppKit、注册表、SMAppService 或 shell
类型。打开文件/目录和启动项由窄的平台 adapter 提供，UI 只消费 command result 和
不可变 status snapshot。

托盘/menu bar 生命周期与服务生命周期分离：服务可以 stopped/faulted，而宿主进程继续
存在以允许查看日志和重新启动。全部 mutating command 在专用 control executor 串行执行；
UI 线程不等待配置编译、listener bind、stop/join 或 logger drain。服务异常退出必须被
非阻塞回收，不能因为遗留 joinable thread 阻止再次启动。

阶段 12 不提供 CLI 到 tray 的 IPC 管理面。Windows tray 只保证本交互用户 session 单实例；
CLI `run` 与 tray 的服务所有权冲突由 listener 独占绑定报告。这个约束避免在没有真实需求
时引入认证、版本和并发语义未定义的本地控制协议。

## 主窗口 Presentation 合同

`0.6-A` 建立跨平台主窗口合同，`0.6-B` 实现共享 ViewModel，`0.6-C/D` 已分别接入
Win32 与 AppKit view，`0.6-E` 完成 Profile 管理闭环。`MainWindowState` 聚合
`ApplicationStatus`、按 stable id 排序的 `ProfileListItem`、纯 UI selection、`DraftState`、
最近命令结果和轻量模式。该合同只携带 C++ 值类型，不暴露 HWND、NSObject、JSON DOM、
SQLite handle 或 mutable runtime 对象。

`0.7-E/F` 将该合同扩展为 stable Profile key、application/Profile field descriptor state、Rule 文本
editor state 与 typed field commands。Windows 已验收的主窗口固定为 Profiles、Rules、Settings 三视图：
顶栏聚合服务状态与控制，左侧保持稳定导航，右侧显示当前编辑器，底部统一承载 draft actions。macOS
后续实现以此信息架构、字段顺序、操作位置和整体布局比例为基准，但继续使用 AppKit 原生主题和控件。

Windows 与 macOS 必须共用同一组命令、结果、错误与状态枚举。服务处于 Running 时允许 Stop/
Reload，Stopped 或 Faulted 时允许 Start，生命周期过渡期禁用 mutating action。Profile selection
只改变 presentation state，不改变 enabled 状态、RouteTable 或路由优先级。

主窗口关闭与宿主退出是两个独立命令。关闭 clean 窗口时，普通模式隐藏并缓存平台窗口，轻量
模式销毁平台窗口；dirty draft 必须先选择 Apply、Discard 或 Cancel，进行中的 command 不允许
被关闭竞态打断。Quit 才会退出 tray/menu host 与 runtime。配置已经持久化但 runtime reload 失败
使用 `SavedPendingRuntimeApply` 独立状态，不伪装成完整成功。

UI preference schema 是严格的 `ccs-trans.ui/v1`：

```json
{
  "schema_version": "ccs-trans.ui/v1",
  "main_window": {
    "lightweight_mode": true
  }
}
```

目标路径是 `state/ui.json`，默认启用轻量模式。文件由独立 `UiPreferencesStore` 管理，使用
`state/ui.lock`、同目录临时文件、回读 schema 校验、revision 比较和原子替换。它不复用
`config.lock`；缺失文件读取默认值，损坏或未知 schema 文件不会被静默覆盖。UI preference 不进入
runtime config，也不触发 RuntimeSnapshot 编译。

`0.6-B` 已实现无窗口 `MainWindowViewModel`：

```text
platform view
  -> immutable MainWindowState snapshot
  -> MainWindowViewModel / FIFO control executor
     -> ConfigurationEditor -> ConfigurationRepository
     -> ApplicationControl -> ApplicationController
     -> UiPreferencesRepository -> UiPreferencesStore
```

同一时刻只接受一个 control command，重复命令立即返回 `Busy`。Profile mutation 先修改 candidate
并执行完整 config/protocol/rule/runtime 校验，成功后才替换 draft。Apply 顺序固定为 validate、
repository commit、运行服务 reload/restart；保存成功但 runtime 应用失败时保持
`SavedPendingRuntimeApply`，并根据 controller 最终是 Running 还是 Faulted 提供 Reload 或 Start。

Profile 列表项还携带总 Rule 数与 enabled Rule 数。Profile readiness 的无效路径、未知 Protocol
和 Rule 编译错误始终保留所属 Profile id。GUI 与 CLI 并发修改仍由 Composite repository 的 application
source token 与 SQLite revision 保护：旧 GUI Apply 返回 `RepositoryStale` 且保留 dirty draft，不会
覆盖外部写入。独立的
`ReloadDraft` 命令从磁盘重建编辑快照；dirty 时必须显式选择 Discard 或 Cancel，不能静默覆盖。
重载尽量保留仍存在的 stable selection，否则选择排序后的第一个 Profile。服务 `ReloadService`
只应用已保存配置，与 `ReloadDraft` 的编辑恢复职责严格分开。

ViewModel 的 update handler 通过平台注入 dispatcher 回到 UI 线程。订阅 generation 让窗口注销后
已排队 callback 自动失效，适配轻量模式的反复销毁。dispatcher 或 view callback 异常与 command
执行隔离。Windows tray 与 macOS menu 均构造该 ViewModel，并分别与各自宿主 command 共用一个
FIFO control executor。

Windows 主窗口是独立 top-level HWND，不拥有 runtime。tray 菜单、tray 双击与第二实例通知只负责
显示或激活窗口；关闭主窗口不会停止 listener。普通模式隐藏并缓存 HWND，轻量模式销毁所有 child
HWND/font/tooltip 资源后按最新 immutable snapshot 重建。dirty draft 必须等待 Apply/Discard command
真正完成后才隐藏、销毁或继续退出；pending command 会阻止普通退出并保留其随后产生的 draft。退出期间
先停用 view callback 并排空 ViewModel command，再在同一 executor 上执行 controller shutdown。布局使用
per-monitor-v2 DPI 与固定最小尺寸。`0.7-F` 的 accepted layout 使用顶栏、左侧三项导航、右侧编辑区和
底部 draft actions；WindowsTheme 集中管理 light/dark/high-contrast palette、DPI metrics 与 GDI+
抗锯齿圆角。Settings 以裁剪 viewport 包含单一内容窗口，只移动内容窗口完成按整行对齐的 16 ms
ease-out 滚动；Rules 使用滚动时短暂出现的主题色位置指示器。Profile selection 不再通过全局
disable/enable 重绘无关控件。

macOS 主窗口由一个 `NSWindowController` 管理，不拥有 runtime。menu 的唯一 Open 命令和第二实例
distributed notification 只显示、恢复并激活窗口；关闭窗口不停止 listener。普通模式 order-out 并
复用现有窗口，轻量模式解除 delegate/data source、关闭并释放 window/controller/view，再从最新
snapshot 重建。AppKit 对象只在主线程访问，ViewModel dispatcher 回到主队列；退出先失效 callback、
销毁窗口并排空共享 FIFO，再在同一 executor 上执行 controller shutdown。窗口禁用 state restoration
和自动 tabbing，使用 Auto Layout、固定最小尺寸、显式 Tab loop、默认 Apply 按钮和 accessibility
labels。AppKit 对齐 Windows 已验收的三视图布局与信息密度，但使用原生 NSControl、NSTextView、
NSScrollView、系统 focus ring、appearance 和滚动动画；不移植 Windows palette、owner-draw 或自绘
圆角/滚动条。

## 配置与 CLI

持久布局：

```text
%USERPROFILE%/.ccs-trans/ or ~/.ccs-trans/
  config.json
  profiles.db
  logs/
    ccs-trans.log
    ccs-trans.log.ccs-archive-<sequence>
    ccs-trans.log.ccs-lock
    ccs-trans-host.log (tray/menu host only)
  state/
    repository.lock
    repository-transaction/ (only while recovery is pending)
    migrations/<source-sha256>/
    ui.lock
    ui.json
```

Windows 从 `USERPROFILE` 读取用户目录，macOS 使用账户 home。`config.json` schema 固定为
`ccs-trans.config/v3`，只保存 application settings；Profile 与有序 Rule 固定保存于 SQLite
schema v1 `profiles.db`。非 v3 schema、未知字段、重复 JSON key、错误类型和越界值会被拒绝，
不做 fallback。v2 只作为显式 migration 的只读输入。

```text
ConfigurationSnapshot
  ApplicationSettings
    listener
    runtime
    timeouts
    logging
  RepositoryRevision
    exact application source token
    SQLite profile revision
  ordered profiles
    profile_key (internal stable identity)
    profile_id (unique, renameable)
      enabled
      protocol
      local request/Usage path
      upstream base/request/Usage path
      ordered rules with internal stable rule_key
```

disabled Profile 和 Rule 可作为草稿保存。运行时只编译 enabled Profile 和 enabled
Rule；`run --profile <id>` 可一次性编译一个完整的 disabled Profile，不改写持久状态。

CompositeConfigRepository 持有跨进程 `repository.lock`，以“配置文件是否存在 + 完整 source
bytes”和 SQLite revision 组成 stale token。config-only 使用 verified temporary + atomic replace，
DB-only 使用单 SQLite transaction；combined commit 使用 durable journal 记录 old/new config 与
old/target DB state。恢复只接受 old/old、new/old、old/target、new/target 四种状态；任何外部不匹配
返回 `RecoveryRequired` 且不覆盖数据。

`storage status/migrate/verify` 是唯一迁移命令族。普通 run、GUI 或 Profile/Rule 命令遇到 v2 返回
`MigrationRequired`。migration 保留原始 v2 bytes、SHA-256 与 manifest，在独立 migrating DB 内
事务导入并完成 WAL truncate checkpoint 后才切换；现有目标 DB 永不覆盖。

`ConfigurationEditor` 从 Composite repository 复制独立 `ConfigurationSnapshot` draft，使用 field
descriptor 与 typed command 修改 application/Profile 字段，以 `profile_key` 保持重命名和排序身份，
并通过 RuntimeCompiler 完整校验后提交。失败 draft 不修改 repository 或已发布 generation。旧
ConfigDocument/ConfigStore 只保留 migration codec 与隔离回归，不再是生产入口。

CLI 一条命令只修改一个字段或执行一个动作。应用字段、Profile 字段和 Rule option
分别由 `config set`、`profile set`、`rule set` 修改；`profile rename/move` 保持 internal key，
`runtime.max-inflight-bytes` 与其他字段共用 descriptor 范围和类型校验。没有短参数或同义别名，
输出不暴露 internal key、SQL 或敏感值。

## Runtime 编译

`RuntimeCompiler` 的顺序固定为：

```text
validated ConfigurationSnapshot
  -> select enabled/diagnostic Profile
  -> resolve ProtocolHandler
  -> validate protocol capabilities
  -> compile enabled Rules
  -> create immutable RuntimeProfile
  -> canonicalize and add request/Usage routes
  -> reject collisions
  -> publish shared_ptr<const RuntimeSnapshot>
```

compiler 构造时复制 ProtocolRegistry 与 RuleRegistry。之后修改外部 builder 不会影响
该 compiler 或已经发布的 generation。

## 路由

RouteKey 是 uppercase HTTP method + canonical path。RouteTable 内部先按 path，再按
method 做两级 hash lookup；每个请求只 canonicalize 一次，不线性扫描 Profile。

规则固定为：

- path 大小写敏感；根路径以外的一个尾斜杠被移除；
- query 在 HTTP parse 时分离，不进入 RouteKey，并原样附加到 upstream；
- 重复 `/`、dot segment、encoded separator/control、反斜杠和非法 percent escape
  被拒绝；
- path 存在但 method 错误返回 405 与排序后的 `Allow`；
- path 不存在返回 404；非法 path 返回 400；
- 同 method + canonical path 的配置 collision 会拒绝整个 snapshot；
- `/_ccs-trans` 命名空间保留给管理接口。

Usage Route 是 Profile 的可选独立 RouteEntry。它使用同一个 Profile/upstream，但不
执行请求 Rule pipeline，也不记录 request headers、query 或 body。

## Protocol

当前 registry 提供：

| Protocol | 主请求 | Usage | SSE | 本地错误 |
| --- | --- | --- | --- | --- |
| `responses` | POST | GET | transparent | OpenAI envelope |
| `chat` | POST | GET | transparent | OpenAI envelope |
| `messages` | POST | GET | transparent | Anthropic envelope |

ProtocolHandler 声明 method、能力和专用 Rule 适用性。命中 Profile 后产生的 pipeline、
transport 或内部错误按 handler envelope 返回；上游 status/header/body 永远不重包。

## Rule Pipeline

首批 Rule：

```text
set_field(path, value)
remove_field(path)
remove_tool(tool)
```

上述三个既有 Rule 使用平台无关 descriptor，不增加推测性的 Rule 类型。
`RuleDescriptor` 提供稳定 type、显示名 key、是否依赖 Protocol 专用布局和按顺序排列的 option；
`RuleOptionDescriptor` 提供 option 名、显示名 key、`string`/`json_value`/`json_pointer` 值类型、
required 与 order。registry 在 factory 注册时拒绝类型错配、非法 key、重复 option、非连续顺序
和未知值类型，并提供稳定排序枚举及按 type 查询。descriptor 只服务配置编译与编辑界面，不进入
每请求 Rule 执行热路径。

共享文本 draft schema 为 `ccs-trans.rules/v1`：UTF-8、LF、2-space、单 Profile 4 MiB 上限，root
只含 `schema_version` 与有序 `rules`，每条 Rule 固定包含 `id/enabled/type/options`。parser 返回
line/column 与 rule/type/option 上下文；formatter 固定字段顺序。相同 `rule_id` 的内容或位置修改
保留 `rule_key`，新 id 分配新 key。disabled 未知类型可 round-trip，enabled 未知类型在 runtime
validation 被拒绝。

Generic Rule 使用 RFC 6901 JSON Pointer。目标必须已经存在，不创建中间对象；array
index 严格拒绝 `-`、前导零、非数字和越界。`set_field` 允许空 pointer 替换 root，
`remove_field` 禁止删除 root。

`remove_tool` 布局由 Protocol 决定：Responses 匹配 root `name`/`namespace`，Chat
匹配 `function.name`，Messages 匹配 root `name`。缺失或非 array 的 `tools` 透明。

执行路径：

```text
empty pipeline -> reuse raw bytes, zero parse
non-empty      -> parse once -> ordered rules on one DOM
modified       -> serialize once
unmodified     -> reuse original bytes
rule error     -> discard candidate DOM, return local error
```

Trace 只包含 rule id/type、matched/modified、静态 reason、有界 target/count 和耗时，
不包含替换值、工具名副本或完整 body。

## 请求生命周期

```text
accept
  -> global capacity admission
  -> queue and worker acquisition
  -> capture one RequestGeneration
  -> receive strict Content-Length request under that generation's limits
  -> parse + RouteTable lookup
  -> optional request pipeline
  -> WinHTTP upstream
  -> stream or buffer response
  -> send client response
  -> release capacity
```

RequestGeneration 持有 RuntimeSnapshot、UpstreamTransport 和 Logger。请求进入编排后一直持有同一
generation；reload 后的新请求读取新 generation，in-flight 请求继续使用旧 Profile、
pipeline、upstream 和限制。旧 generation 在最后一个对应请求结束后自动释放；同时保留的旧
generation 数受 `max_connections` 间接限制，但长 SSE 配合连续 reload 仍可能保留多份 snapshot、
transport 和 logger。当前实现记录 current/peak retired generation、generation drain 与 churn，
用于证明最后一个请求结束后资源归零。

listener 的 request header 上限固定为 64 KiB，只接受 HTTP/1.0/1.1 与单一有效
`Content-Length` framing；重复/非法 Content-Length、request `Transfer-Encoding` 和畸形
header 在本地返回 400。accept 后的 socket 进入有界 client registry；服务停止关闭 registry
中的 socket，以唤醒半包读取并触发全部在途 upstream 取消。普通客户端断开仍只取消自己
的请求。

AppService 提供 `start`、`reload`、`stop`、`wait` 和状态查询。listener/worker/logger
topology 变化要求 graceful restart；失败时重新启动旧 snapshot。Profile、route、rule、
upstream、timeout、body/request limit 和 body logging policy 可通过 generation swap 对
新请求生效。日志路径变化创建新 writer，旧请求完成后旧 writer 才 drain/退出；同路径
writer 参数或进程级 inflight budget 变化要求 restart。CLI 在保存任何包含 enabled Profile 的
候选 snapshot 前运行完整 RuntimeCompiler，因此 route collision 不会落盘。

## 并发与容量

默认 `max_connections=64` 覆盖执行中和排队连接。超过容量返回 503，不建立无界队列。
默认 `worker_threads=32` 是按需 worker 上限；启动预热 `min(8, worker_threads)`。

默认 request/response body 上限分别是 100 MiB。单请求限制保证解析和缓冲有界，但启用 Rule 时
同一请求可能短暂同时持有 raw body、JSON DOM 和 serialized body；多个大请求并行时，单请求限制
不能替代全进程内存预算。当前进程级 inflight budget 默认 512 MiB，覆盖 request/response、SSE
chunk、Rule DOM/output、logger queue 和主要 staging buffer；耗尽时有界拒绝或关闭，不无界等待。
SSE response 始终按 chunk 转发和记录，不为日志或响应处理累计完整流 body。

metrics 只记录全局资源维度，避免 Profile 数量造成无界 cardinality。Profile/protocol/
route 只进入每请求日志。并存 generation 的 logger 使用 active-writer 计数，旧 writer
退出或候选 writer 打开失败不会错误清除现役 writer 的健康状态。

负载口径：

- 8-16 路 SSE 是桌面常规负载；
- 50 路是 bounded stress；
- mixed load 中 Responses/Chat Usage 必须在 SSE 完成前持续成功；
- 是否换异步网络模型由 benchmark 与资源指标决定。

## Transport 与系统代理

Windows minimum 是 Windows 11 21H2 x64。WinHTTP transport 读取当前用户手动代理、
bypass 和显式 PAC；registry watcher 为新请求发布新 session，in-flight 请求持有旧
session。

transport 删除标准 hop-by-hop header、`Connection` 指定字段和 proxy authentication
header，但保留 Authorization 等 end-to-end header。上游 status、reason phrase、body 和
end-to-end response headers 原样返回；缺失的 `Content-Type` 不由代理猜测补充。请求体
处理 partial write，response read chunk 固定有界，非流式 body 在分配前执行上限检查。

系统明确 direct 时可直连。一旦系统为目标选择 proxy，DNS/连接/转发失败不能 fallback
direct。407 返回 `proxy_authentication_unsupported`，不提示或保存密码。仅启用自动
检测但没有显式 PAC 时忽略 WPAD。

macOS transport 只从 selected SDK 链接 system libcurl。每个在途请求独占一个 easy handle；
handle pool 上限等于 worker 上限，每个持久 multi slot 的 connection cache 上限为 4。multi
poll loop 以 25 ms 上限检查取消以及 resolve/connect/send/response-header/stream-idle/total
deadline，普通响应额外受 bounded response-body idle 和 size limit 约束。

macOS 只使用启动进程继承的 proxy environment，不读取或激活 System Settings 代理。
Terminal 启动继承 shell 环境；Finder/登录项通常没有 shell proxy 变量，因此默认直连。系统
libcurl 出于 CGI 安全规则忽略大写 `HTTP_PROXY`，接受小写 `http_proxy`；`HTTPS_PROXY`、
`ALL_PROXY` 和 `NO_PROXY` 保持 libcurl 原生语义。代理连接失败不做应用层 direct fallback。

## 日志与安全

Logger 使用有界 byte queue。正常事件按默认约 100 ms 批写；error 事件等待立即 flush；
容量不足时生产者背压，不静默丢事件。启动成功和正常退出前会显式 drain；writer
durability failure 触发 Server 停止并返回非零退出码。

每个运行日志族默认由 `logging.max_total_size=2147483648` 限制为 2 GiB，范围包括 active
文件和 ccs-trans 管理的有序 archive。默认 file sink 在完整 JSONL 记录之间轮转，仅 writer
线程执行运行期 flush/rename/prune；启动时会恢复 archive 序号和容量账本，并把旧版超限单文件
压缩到最新完整记录。每个日志族持有独立 OS 文件锁，避免多进程同时轮转。轮转、清理、旧文件
压缩或锁失败进入既有 writer failure 通道，不回退无限追加。host log 使用独立 64 MiB 日志族。
runtime metrics 记录 rotation、删除文件/字节和当前/峰值受管理容量。

SSE 每个 chunk 单独记录连续序号和大小，绝不为日志累计完整 response body。Usage 只
记录最小完成/错误摘要。请求、Rule、upstream、response 和 chunk 可按 `request_id`、
`generation_id`、Profile、protocol、route kind 串联；generation swap 事件同时记录前后 id。
已开始后中断的 SSE 错误额外记录耗时、chunk 数和累计字节，不保留第二份流内容。

`redact_sensitive` 只遮盖已知敏感 header，不清理 JSON body。启用 body logging 时日志
可能包含完整模型上下文，必须按高敏感文件处理。发布包不得包含用户 config、logs、
benchmark 输出或临时目录。

## 验证状态

当前自动验证包括：

1. ConfigDocument/ConfigStore/CLI、RouteTable、ProtocolRegistry、RuleRegistry 单测；
2. Server 404/405/invalid path、OpenAI/Anthropic local error 单测；
3. reload generation A/B、路径/Rule 顺序/日志路径切换、候选失败回滚与运行中
   ConfigStore 原子保存集成测试；
4. 单端口 Responses/Chat/Messages、三组 Usage、findcg Rule、普通响应、SSE、query、
   headers、body limits、overload、timeouts、取消和日志 Python 集成测试；
5. Windows system proxy A/B、in-flight、dead proxy、direct、WPAD-only、bypass、PAC 和
   407 专项矩阵；
6. `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50` 与 0/1/8/32 Rule
   microbenchmark；
7. ApplicationController 启停/reload/端口冲突/shutdown，以及 HostPlatform 默认配置和
   fake startup registry 单测；
8. Windows tray control executor/单实例单测，以及真实 GUI 子进程的自动启动、主窗口/Profile
   draft、Rule 摘要、CLI/GUI stale Apply 与 Reload Draft、普通/轻量生命周期、Start/Stop/Reload、
   第二实例、通知区图标、日志 drain 和退出集成测试；
9. Windows 候选 ZIP 的固定白名单、双 executable hash、CLI/资源版本与解压后 tray
   生命周期验证；
10. Windows 11 24H2 VM 的 startup/system proxy、四档 DPI、主题、睡眠唤醒、Explorer
    重启、重启/注销、2 小时 mixed soak 与 8 小时 idle 验证。
11. macOS POSIX adapter 的 port conflict、partial I/O、SIGPIPE、EINTR、disconnect、stop
    interrupt 与 fd 回收单测；Release/warnings 两套 23/23 CTest；
12. macOS 同一 Python fixture 的三协议/Usage/SSE/reload/cancellation/limits/timeouts 集成，
    process proxy direct/HTTP/HTTPS CONNECT/ALL_PROXY/NO_PROXY/no-fallback 矩阵；
13. macOS AppKit host 的 Unicode/空格 home、自动 service、单实例通知、SIGTERM drain，以及
    正式文件名 ad-hoc 固定白名单 ZIP 的签名校验、解包 CLI/menu smoke。
14. macOS AppKit 主窗口的 Profile draft、Rule 摘要、CLI/GUI stale Apply 与 Reload Draft、dirty
    close、普通/轻量生命周期、第二实例激活、Retina/主题/键盘/accessibility probe、100 次资源
    生命周期、pending Quit、Start/Stop/Reload，以及窗口循环期间 `desktop-16` 的内容、顺序、
    长度、结束标记和零上游断连。

`0.7.0` 发行边界：

- listener 的共享编排使用 Windows/POSIX local-socket adapter；Windows 使用 WinHTTP，macOS
  使用 selected SDK system libcurl；
- v3 应用设置、SQLite Profile/Rule、Composite repository、显式 migration 和 immutable runtime
  snapshot 是唯一生产存储路径；v2 只保留为只读迁移输入；
- 两平台原生主窗口共用 presentation、repository、controller 和 FIFO executor，不建立第二套 runtime；
  Profiles/Rules/Settings、typed draft、stale recovery、Rule text 和普通/轻量生命周期语义已对齐；
- Windows 两个全新目录分别完成 `112/112` Release/warnings build、各 `25/25` CTest、shared 与 tray
  integration；五档短负载、Rule matrix、100 次窗口资源循环和 `desktop-16` 精确回传通过；
- macOS 产品候选完成两套 `107/107` build、各 `23/23` CTest、shared/proxy/transport/menu integration、
  100 次 AppKit 资源循环、响应式布局 probe 和 `desktop-16` 精确回传；
- `0.7.0` 未重跑两平台 2 小时 mixed、8 小时 GUI idle、完整 DPI/辅助功能和 fault-adapter 组合矩阵；
  `0.6.0` 的长时结果不冒充当前版本证据；
- Windows EXE 未做 Authenticode，Defender/SmartScreen 未评估；macOS 正式 ZIP 使用 ad-hoc
  签名，不执行 Developer ID、公证或 staple，也不声明发布者身份或 Gatekeeper 信任；
- 完整证据、性能数字和未执行项见 [Release-0.7.0.md](Archived/Release-0.7.0.md)。
