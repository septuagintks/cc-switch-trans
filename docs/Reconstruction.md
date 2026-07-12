# ccs-trans 通用化架构

## 目标模型

`ccs-trans` 是本地 LLM API Request Transformation Proxy。业务能力由三类可扩展对象
组合，不依赖 Provider 特判：

```text
Profile   一条本地路由到上游目标的代理链
Protocol  请求方法、能力和本地错误格式
Rule      对主请求 body 执行的一步有序转换
```

一个进程只绑定一个应用 listener。全部 enabled Profiles 同时发布 exact local routes；
同一 Profile 的主请求与可选 Usage 共享协议和上游归属。Usage 不执行主请求 Rule pipeline。

## ConfigDocument

持久配置 schema 为 `ccs-trans.config/v2`：

```text
application
  listener
  runtime
  timeouts
  logging

profiles[id]
  enabled
  protocol
  local.request_path
  local.usage_path?
  upstream.base_url
  upstream.request_path
  upstream.usage_path?
  rules[]
```

Profile id、Protocol id 和 Rule id 是稳定标识，不是显示文本。配置最多 128 Profiles、
256 routes、每 Profile 64 Rules，防止不受控内存和日志 cardinality。

配置不持久化 API key、Authorization、Cookie 或代理凭据。认证 header 由客户端请求携带。

### 草稿与启用

`profile create` 和 `rule add` 创建 disabled 草稿。disabled 的未知未来 Rule 可以保留；
启用 Profile 或 Rule 前必须满足当前 registry 的全部语义。每条 CLI 修改只改变一个字段
或执行一个动作。

保存候选文档前执行：

```text
schema/type validation
  -> enabled Rule factory validation
  -> enabled Profile Protocol validation
  -> full RuntimeCompiler
  -> ConfigStore atomic save
```

因此 canonical route collision、保留路径冲突、不完整 Profile、未知 enabled
Protocol/Rule 和越界日志路径不会写入配置。ConfigStore 使用进程间锁、临时文件、回读
parse/canonical round-trip 与原子替换；陈旧 writer 或任一步失败都保持目标文件字节不变。

## RuntimeCompiler

磁盘对象与请求热路径完全分离：

```text
ConfigDocument
  -> immutable ProtocolRegistry snapshot
  -> immutable RuleRegistry snapshot
  -> compiled RuntimeProfile objects
  -> RouteTable(method, canonical_path)
  -> shared_ptr<const RuntimeSnapshot>
```

RuntimeProfile 只持有 ProtocolHandler 和 compiled pipeline，不携带可编辑 JSON。RouteEntry
持有 Profile、RouteKind、method 和 UpstreamTarget。RouteTable 使用 canonical path 到
method 的两级 hash lookup；请求期不线性扫描 Profiles。

同 path 不同 method 可以共存。完全未知 path 返回 404；已知 path 的错误 method 返回
405 与 `Allow`；非法 path 返回 400。query 在 lookup 前分离，并在 forwarding 时追加到
配置的 upstream path。

## ProtocolRegistry

内置 handler：

| id | 主请求 | Usage | 本地错误 envelope |
| --- | --- | --- | --- |
| `responses` | `POST` | 可选 | OpenAI |
| `chat` | `POST` | 可选 | OpenAI |
| `messages` | `POST` | 可选 | Anthropic |

Handler 声明 request/Usage method、JSON/SSE/Usage capability 和专用 Rule 适用性。透明
请求不会仅因选择 handler 而解析 JSON；上游 response 不重包。

新增 Protocol 的最小路径是实现 handler、注册稳定 id 并增加 fixture/test。不得修改
worker、listener、transport 或按 id 给 Server 增加分支。

## RuleRegistry

当前 Rule：

| type | 作用 |
| --- | --- |
| `set_field` | 设置已存在的 RFC 6901 JSON Pointer 目标 |
| `remove_field` | 删除已存在的非 root JSON Pointer 目标 |
| `remove_tool` | 按 Protocol 的工具布局删除指定工具 |

`set_field` 不创建中间节点，空 pointer 可替换 root；`remove_field` 禁止删除 root。array
index 只接受无前导零的十进制下标，不接受 `-` 或越界 fallback。Pointer escape 只接受
RFC 6901 `~0`/`~1`。

`remove_tool` 的布局：

```text
Responses  tools[].name 或 tools[].namespace
Chat       tools[].function.name
Messages   tools[].name
```

缺失或非 array 的 root `tools` 保持透明。findcg Profile 使用
`remove_tool(tool=image_gen)`，实现中没有 host 判断。

### Pipeline 性质

```text
empty pipeline     -> zero JSON parse
non-empty pipeline -> parse once, ordered apply, serialize at most once
unmodified         -> reuse original request bytes
rule failure       -> discard candidate DOM, never forward partial changes
```

Trace 只记录 Rule id/type、matched/modified、静态 reason、有界 target/count 和 duration；
不记录配置替换值、第二份完整 body 或长系统提示。

## RequestGeneration 与 Reload

每个 generation 持有：

```text
RuntimeSnapshot
UpstreamTransport
Logger
generation_id
```

worker 在读取 HTTP 请求前获取一次 generation。request body limit、RouteTable、pipeline、
upstream、response limit、timeout 和 logging policy 全程来自同一对象。reload 先完整编译
候选 snapshot，再一次交换 shared pointer；in-flight 请求继续持有旧 generation。

可热交换到新请求：

- Profiles、routes、upstreams 和 Rule 顺序/参数；
- request/response body limit 与 timeout；
- body logging/redaction policy；
- 日志路径，新旧 writer 随 generation 短暂并存。

需要 graceful restart：

- listener host/port；
- worker 上限或 metrics reporter interval；
- 同一日志路径的 level、queue capacity 或 flush interval。

restart 启动失败时 AppService 恢复旧 snapshot。generation id 在进程内单调递增；swap
日志同时记录 previous/new id，所有 request-chain 事件携带 generation id。

## Server 与并发

```text
single exclusive listener
  -> global max-connections admission
  -> bounded FIFO queue
  -> prewarmed/on-demand worker pool
  -> one generation + one route lookup
  -> optional Rule pipeline
  -> UpstreamTransport
```

默认 max connections 64、worker 上限 32、预热 8。超过容量返回 503，不创建无界线程或
队列。客户端断开只取消对应 upstream handle。停止过程停止 accept、处理/取消现有请求、
join worker/reporting/watcher 线程并 drain logger。

listener 只接收 HTTP/1.0/1.1 的单一有效 Content-Length framing，header 上限 64 KiB；
重复/非法 Content-Length、request Transfer-Encoding 和畸形 header 不进入 RouteTable 或
upstream。accept 后的 socket 进入有界 registry；stop 将 registry 置为 closing 并关闭全部
已登记 socket，以唤醒半包读取并触发在途请求 cancellation。stop 后拒绝新登记，worker
只关闭仍归 registry 所有的 socket，避免句柄复用后的二次关闭。普通客户端 cancellation
只影响自身。长生命周期 cancellation token 必须回收 inactive callback，内存上界由并发
请求数决定而不是进程累计请求数。

8-16 路 SSE 是正常桌面负载；50 路只验证有界排队、拒绝和资源释放，不承诺常规延迟。

## UpstreamTransport

Server 只依赖 `UpstreamTransport`：

```text
forward(request, target, cancellation)
forward_streaming(request, target, header callback, chunk callback, cancellation)
proxy_mode()
```

接口完整表达普通响应、SSE、取消、split timeout、response limit 和错误分类，不暴露平台
handle。Windows 实现在 `transport/windows/WinHttpTransport`；macOS 后续实现位于
`transport/macos`，链接系统 libcurl。

平台实现必须删除标准 hop-by-hop header、`Connection` nominated header、
`Proxy-Authorization` 与 `Proxy-Authenticate`；Authorization 等 end-to-end header 保留。
upstream status、reason phrase、end-to-end headers 和 body 不重包，尤其不能为缺少类型的
response 自行添加 `Content-Type`。请求 write 必须处理 partial progress；response read
使用有界 chunk，并在为非流式 body 分配前检查限制。

Windows 合约：当前用户手动 proxy、bypass 与显式 PAC 可用；WPAD-only 不执行自动发现；
系统选定 proxy 后失败不 direct fallback；407 映射为不支持代理认证。macOS 合约：只
继承启动环境中的代理变量，不读取或激活系统代理。

## 日志与安全

Logger 使用有界 byte queue。正常事件约 100 ms 批写，error 等待立即 flush；容量不足
时生产者背压。`server_start` 在启动成功回调前 drain，正常退出前再次 drain。durability
failure 触发 Server 停止和非零退出。

并存 generation 使用 active-writer 计数；旧 writer 退出或候选 writer 打开失败不会把
当前 writer 标为不健康。SSE 只记录连续 `chunk_sequence`、大小和可选有界 chunk body，
不累计完整流。Usage summary 不记录 request headers、query、body 或 client IP。
已发送 headers 后中断的 SSE 在错误事件中记录 duration、chunk count 和累计字节，不能
尝试发送第二个 HTTP error response。

`redact_sensitive` 遮盖已知敏感 headers，但不会清理 JSON body。启用 body logging 可能
记录完整模型上下文，日志必须按敏感数据处理。

Windows tray 使用独立 `logs/ccs-trans-host.log` 记录宿主启动、状态变化、菜单命令、
单实例和 shutdown。它复用同一异步 Logger 语义但不与 runtime log 共享 writer，不记录
请求 headers/body；周期 status poll 没有变化或新错误时不产生日志。

## 宿主扩展模型

CLI、Windows tray 与 macOS menu bar 共享同一服务控制路径：

```text
host UI / CLI
  -> ApplicationController
       -> AppPaths + ConfigStore + RuntimeCompiler
       -> AppService
            -> Server
```

`ApplicationController` 以磁盘配置为命令边界：start/reload 每次重新 load 并完整 compile，
候选失败不会影响当前 generation。它管理 stopped/starting/running/reloading/stopping/faulted
状态、last error、exit code 和异常退出后的 thread 回收。AppService 继续只处理一个已编译
snapshot 的运行、rollback 和停止，不感知 tray/menu bar。

宿主平台 adapter 只负责打开配置/日志路径和用户级启动项。Windows 使用 ShellExecute 与
HKCU Run，macOS 使用 Workspace/SMAppService；平台对象不进入 controller。UI command 在
独立 control executor 串行执行，结果通过宿主 event loop 回传。UI 线程和 request worker
之间没有任务队列共享、请求 body 复制或日志反向依赖。

阶段 12 不建立通用 IPC 管理协议。tray 的第二实例通知仅用于唤起已有菜单；CLI 与 tray
并发运行依靠 listener 绑定冲突确定所有权。若以后确有远程管理需求，应独立设计协议版本、
认证、当前用户权限、超时和状态一致性，不能复用业务 API route 临时拼接。

跨平台 listener 抽取只封装 socket 系统调用和 native error。HTTP framing、admission、
worker、RouteTable、generation、Rule 与 logger 仍在共享 Server 中。macOS CurlTransport
同样只实现 UpstreamTransport，不承担 Profile 或 proxy 策略之外的业务判断。

## 扩展合约

新增能力不得破坏以下边界：

1. 新增 Profile 只修改配置，不修改 C++ enum 或 Server。
2. 新增 generic Rule 只实现 factory/compiled rule，不修改 transport。
3. 新增 Protocol 不修改 listener、worker 或平台网络实现。
4. 新增宿主只调用 app/config command，不复制 runtime 初始化。
5. 新增平台 transport 不泄漏平台类型到公共 core/routing/rules header。
6. 发布包使用白名单，不包含用户 `.ccs-trans`、日志、凭据或本机 benchmark。
7. 新增 UI command 必须定义可重入/幂等语义、执行线程、失败状态与 shutdown 行为。
8. 新增 local socket 平台实现不得复制 HTTP parser、worker queue 或 request orchestration。
