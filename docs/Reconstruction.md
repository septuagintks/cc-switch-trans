# ccs-trans 通用化重构设计

## 文档定位

本文是下一轮重构的目标模型和架构决策，不代表当前代码已经实现。当前稳定行为见
[Design.md](Design.md)，具体构建顺序见 [DevelopmentPlan.md](DevelopmentPlan.md)。

重构目标不是增加另一条特殊规则，而是把 `ccs-trans` 明确定义为：

> LLM API Request Transformation Proxy

它只负责接收请求、根据配置执行请求转换、转发上游并返回响应。Agent、模型供应商
选择、API key 管理和故障转移仍由 cc-switch 或其他上层工具负责。

## 为什么需要重构

当前实现已经具备稳定的转发、SSE、取消、日志、profile 存储和 reload，但业务模型
仍绑定在两个固定端点上：

- `EndpointGroupKind` 只有 Responses 和 Chat；
- `ApiTaskKind` 固定列出两个主任务和两个 Usage 任务；
- `AppConfig` 为每种协议保存一组专用字段；
- `Server` 为两个 listener、两个 router 和 findcg transform 保留显式分支；
- 当前持久 profile 表示“一整套 AppConfig 覆盖”，不能同时描述多条代理链；
- transform 通过字符串名称和 `Server` 中的硬编码对象连接，不能由配置组合。

在这个模型上增加 Messages、新 Provider 或多条同协议链路，会同时修改 config、CLI、
task enum、router、server、日志字段和测试矩阵。问题不在某个类太长，而在核心业务
概念没有稳定下来。

## 核心决策

重构后的系统围绕三个概念组织：

| 概念 | 含义 | 生命周期 |
| --- | --- | --- |
| Profile | 一条完整代理链：本地路由、协议、上游和有序规则 | 配置级 |
| Protocol | 解释协议结构和协议专用规则 | 进程级 registry |
| Rule | 一项可验证、可排序、可独立记录的请求转换 | snapshot 编译级 |

同时明确以下决策：

1. 一个进程只使用一个应用级 HTTP listener。
2. 所有 enabled profile 同时装入同一张路由表。
3. listener、worker、连接容量、timeout、body limit、logging 和 metrics 是应用级
   配置，不复制到每个 profile。
4. profile 只拥有协议、本地路径、上游和规则。
5. 请求按 method + canonical local path 精确选择 profile，不猜 header 或 body。
6. protocol handler 负责协议知识，worker 和 transport 不关心协议。
7. rule 在配置加载时完成校验和编译，请求路径只执行不可变 pipeline。
8. 当前只改写请求；上游响应继续透明转发。
9. 保留同步按需 worker 和 WinHTTP 实现，业务重构不与网络栈重写绑定。
10. 当前配置 schema 与新 profile 含义不兼容，新 schema 不保留运行时兼容层。

## 平台网络与代理策略

代理策略是平台 transport 的固定行为，不属于 Profile，也不保存代理地址、账号或
密码。系统选择“直接连接”和“已选择代理但连接失败”是两种不同状态。

### Windows

支持基线固定为 Windows 11 21H2 x64，不为 Windows 10 或更早版本保留代码、测试或
发布兼容分支。

上游 transport 使用 WinHTTP，并遵守：

1. transport 读取当前用户 Windows 系统代理 snapshot：手动代理使用 WinHTTP named
   proxy；显式 PAC URL 由 `WinHttpGetProxyForUrl` 按请求解析，并把 direct 或 named
   proxy 结果固定到该请求；未配置代理时使用 direct；不读取
   `HTTP_PROXY`/`HTTPS_PROXY` 环境变量。
2. 系统未配置代理，或系统 bypass/PAC 明确选择 direct 时，允许直接连接。
3. 系统已经为目标选择代理后，代理 DNS、连接或转发失败必须返回错误，不能再次
   尝试 direct。
4. 不提供 proxy URL、username、password 或 auth mode 配置；收到 `407` 时返回明确
   proxy authentication error，不保存或提示输入密码。
5. 系统代理在进程运行期间发生变化时，后续新请求应使用新结果，不要求重启服务；
   已经开始的请求不迁移代理。
6. 日志记录 `proxy_mode=windows_system`、错误分类和 WinHTTP error code，但不记录
   代理地址、代理凭据、PAC 内容或带 userinfo 的 URL。

仅启用“自动检测设置”但没有显式 PAC URL 时不执行 WPAD。Windows 默认常把该开关
置为 true，即使用户没有代理；对每批请求执行 WPAD 会引入不可预测的局域网发现和
约 4-6 ms 固定延迟。该取舍不影响代理软件写入的手动系统代理、bypass 或显式 PAC。

Internet Settings 由后台 registry watcher 监控。变化时先构建新 WinHTTP session，
再发布不可变 `shared_ptr` snapshot；请求路径只持 shared lock 复制 session，in-flight
请求继续持有旧 session。

当前 Windows transport 已按上述策略跟随系统代理。专项集成测试使用两个受控
本地 proxy 和一个可直连 origin，验证 A 到 B 的运行时切换、代理关闭后 origin 未被
direct 访问、in-flight 请求保持 A、新请求使用 B、bypass、显式 PAC，以及系统明确
切回 direct 后 origin 才收到请求。

### macOS

macOS 上游 transport 链接系统 libcurl，不读取或修改 macOS System Settings 中的
代理，也不调用 SystemConfiguration 激活系统代理。

1. libcurl 只使用进程启动时继承的 `http_proxy`、`https_proxy`、`all_proxy` 和
   `no_proxy`；同时接受 libcurl 原生支持的大小写形式。
2. CLI 从 terminal 启动时自然继承该 terminal 的环境。进程启动后修改 shell 环境
   不影响已经运行的进程，重启 CLI 后生效。
3. menu bar app 从 Finder 或 Login Item 启动时通常没有 terminal 代理环境，因此
   默认 direct；从带代理环境的 terminal 启动时才随该环境走代理。
4. 不增加持久 proxy 字段，也不读取 macOS 系统代理作为 fallback。
5. 不支持显式代理账号密码；`407` 或 libcurl proxy-auth 错误直接返回并分类记录。

系统 libcurl 的 SDK/linkage、运行时版本和发布目标将在 macOS 构建阶段固定并写入
发布说明，不把 Homebrew 作为最终用户运行依赖。

## 应用级与 Profile 级边界

原草案把 listener/runtime 同时放在根和 profile 中，这会造成冲突。最终边界固定为：

```text
Application
  listener
  runtime
  timeouts
  logging
  profiles[]

Profile
  enabled
  protocol
  local routes
  upstream target/routes
  ordered rules[]
```

原因：一个 listener 和一个共享 worker pool 无法为不同 profile 同时兑现不同线程数
或监听端口。把这些字段放在 profile 中只会产生“选哪个值”的隐式优先级。需要
隔离资源时应启动第二个进程和第二份配置，而不是在一个进程中伪装多套 runtime。

## 配置文档

目标 schema：

```json
{
  "schema_version": "ccs-trans.config/v2",
  "listener": {
    "host": "127.0.0.1",
    "port": 15723
  },
  "runtime": {
    "worker_threads": 32,
    "max_connections": 64,
    "max_request_body_size": 104857600,
    "max_response_body_size": 104857600,
    "metrics_interval_ms": 0
  },
  "timeouts": {
    "resolve_ms": 300000,
    "connect_ms": 300000,
    "send_ms": 300000,
    "response_header_ms": 300000,
    "stream_idle_ms": 300000,
    "total_ms": 0
  },
  "logging": {
    "path": "logs/ccs-trans.log",
    "level": "info",
    "body": true,
    "redact_sensitive": false,
    "body_limit": 1048576,
    "queue_capacity": 16777216,
    "flush_interval_ms": 100
  },
  "profiles": {
    "findcg": {
      "enabled": true,
      "protocol": "responses",
      "local": {
        "request_path": "/findcg/v1/responses",
        "usage_path": "/findcg/v1/usage"
      },
      "upstream": {
        "base_url": "https://www.findcg.com",
        "request_path": "/v1/responses",
        "usage_path": "/v1/usage"
      },
      "rules": [
        {
          "id": "remove-image-gen",
          "enabled": true,
          "type": "remove_tool",
          "tool": "image_gen"
        }
      ]
    }
  }
}
```

字段名在 JSON 中使用 `snake_case`。CLI key 使用稳定的点路径，例如
`listener.port`、`runtime.worker-threads`、`local.request-path`。CLI 表示不改变
JSON 的类型约束。

### 配置约束

1. profile 名和 rule id 在各自作用域内唯一且稳定，用于日志和 CLI，不使用数组
   下标作为永久标识。
2. enabled profile 必须拥有完整 protocol、request route 和 upstream target。
3. disabled profile 可以作为草稿保存，但字段仍需满足类型和局部格式校验。
4. rule 默认按数组顺序执行；id 只用于定位，不改变顺序。
5. enabled rule 必须通过 type、参数和 protocol 适用性校验。
6. Usage route 可选；缺失时该 profile 不注册 Usage。
7. 所有 enabled route 的 method + canonical path 必须全局唯一。
8. 本地路径禁止 query、fragment、`..`、重复分隔歧义和保留管理路径。
9. upstream base URL 只接受 `http`/`https`，路径与 query 分开保存。
10. 初始上限固定为：配置文档 4 MiB、128 个 profile、256 条 route、每 profile
    64 条 rule。修改上限必须同时 review 内存、reload 时间和 metrics cardinality。

当前保留管理命名空间为 `/_ccs-trans` 及其子路径。数值上限固定为：

- `worker_threads` 1-1024，`max_connections` 为 worker 数到 65535；
- request/response body、日志 body 与日志 queue 单项最大 1 GiB；
- stage timeout 使用正 `int` 范围，`total_ms` 和 `metrics_interval_ms` 可为 0；
- `flush_interval_ms` 为 1-60000。

这些是配置输入上限，不是推荐运行值。默认仍是 32 workers、64 connections、
100 MiB request/response limit、16 MiB log queue 和 100 ms flush。

disabled profile 允许缺少 protocol/local/upstream，并允许 Usage 两端分两条命令逐步
补齐；其已存在字段仍立即执行类型、ID、URL 和 path 局部校验。enabled profile 必须
完整，且 Usage local/upstream 必须同时存在。rule option 作为 typed JSON 值保存；
option 名、JSON 深度和节点数在 document 层有界，具体 type 的 option 白名单和协议
适用性由阶段 11.6 的 `RuleRegistry` 校验。

`ConfigStore` 使用 `state/config.lock` 做跨进程写互斥，并比较加载时源字节，拒绝陈旧
writer。候选文档通过校验后写临时文件，回读、重新 parse 和 canonical round-trip，
最后用 Windows `MoveFileEx(...REPLACE_EXISTING | WRITE_THROUGH)` 或同文件系统 rename
替换。旧 v1、超限或解析失败后 store 保持 unpublished，不能调用 save 覆盖原文件。

### cc-switch 路径用法

如果 cc-switch 会在 endpoint 后追加 `/v1/responses` 和 `/v1/usage`，可把 Provider
endpoint 配成：

```text
http://127.0.0.1:15723/findcg
```

实际请求将命中：

```text
/findcg/v1/responses
/findcg/v1/usage
```

因此 profile 保存的是最终精确路径，而不是一个需要运行时模糊匹配的 path prefix。
不同客户端路径行为可通过显式 local route 配置适配。

## Profile 语义

一个 profile 表示一条代理链，而不是一套进程配置：

```text
local request
    -> profile
    -> protocol handler
    -> ordered rule pipeline
    -> profile upstream
    -> transparent response
```

例如同一个进程可同时装载：

| Profile | Protocol | Local request | Upstream request |
| --- | --- | --- | --- |
| `findcg` | `responses` | `/findcg/v1/responses` | `https://www.findcg.com/v1/responses` |
| `openrouter` | `chat` | `/openrouter/v1/chat/completions` | `https://openrouter.ai/api/v1/chat/completions` |
| `anthropic` | `messages` | `/anthropic/v1/messages` | `https://api.anthropic.com/v1/messages` |

所有 enabled profile 默认同时生效。目标模型不再保存 `active_profile`，也不保留
`profile use`，因为单 active profile 会让同端口多路由失去意义。

`ccs-trans run --profile <name>` 保留为一次性诊断模式：只把指定 profile 编译进
本次 runtime snapshot，不修改配置文件，也不改变其他 profile 的 enabled 状态。
它可以选择 disabled profile，但该 profile 必须通过完整运行校验；这允许启用前测试
一条已经配置完整的代理链。

## 路由模型

配置编译阶段生成不可变 route index：

```text
RouteKey = HTTP method + canonical path
RouteEntry
  profile id
  route kind: request | usage
  protocol handler
  upstream target
  compiled rule pipeline
  logging policy
```

请求路径只做一次 query 分离和 canonicalization，再使用 hash lookup。不得每次线性
扫描 profile，也不得根据协议枚举重复查表。

路由规则：

1. 主请求 method 由 protocol descriptor 定义，当前三种协议均为 `POST`。
2. Usage method 默认 `GET`，但 route 是否存在由 profile 显式配置。
3. query 原样附加到对应 upstream path。
4. 路径存在但 method 错误时返回 405；完全未知路径返回 404。
5. 配置阶段发现 route collision 时拒绝整个 snapshot，不使用声明顺序决定胜负。
6. route 只持有 immutable/shared 数据，不引用可被配置编辑原地修改的对象。

canonical path 语义固定为：

- path 大小写敏感，根路径除外的一个尾斜杠会被移除；
- query 在 HTTP parse 阶段单独保存，不进入 RouteKey，转发时原样附加到 upstream；
- 重复 `/`、raw/encoded dot segment、encoded `/` 或 `\\`、反斜杠、raw/encoded 控制字符、
  query/fragment 和无效 percent escape 被拒绝；
- percent hex 统一大写，ASCII unreserved 字符会解码，因此 `%66oo` 与 `foo` 会在
  编译阶段产生 collision；其他 encoded octet 保持 percent 形式；
- 配置与请求 lookup 调用同一个 `canonicalize_http_path`，每次请求只生成一次
  canonical string；内部索引为 `path -> method -> RouteEntry` 两级 hash；
- 同 canonical path 的不同 method 可以共存；method 不匹配返回 405 和稳定排序的
  Allow 集合，path 不存在返回 404，非法 path 单独分类。

当前 compiler 已从 ProtocolRegistry descriptor 取得主请求与 Usage method，不在
RuntimeCompiler 或请求路径中根据 protocol id 分支。registry 在 compiler 构造时复制为
immutable snapshot；后续对外部 builder 的注册不会改变现有 runtime generation。

## Protocol Handler

初始 registry 固定支持：

```text
responses
chat
messages
```

handler 负责：

- protocol id、主请求 method 和协议能力声明；
- 校验 profile 上游与本地 route 是否满足该协议要求；
- 编译协议专用 rule；
- 为 rule 提供协议字段定位和结构化辅助函数；
- 生成稳定的协议错误分类。

handler 不负责：

- 接受 socket；
- 选择 profile；
- 创建 worker；
- 读取或写入配置文件；
- 执行 WinHTTP；
- 聚合 SSE response body。

透明 profile 不应因为选择了某个 handler 就自动解析 JSON。只有非空且需要 body 的
pipeline 才进入 JSON 解析路径。

当前三个 descriptor 的主请求均为 `POST`、Usage 为 `GET`，且都声明 transparent SSE、
JSON request body 和 `remove_tool` 专用 rule capability。Responses/Chat 使用 OpenAI
本地错误 envelope，Messages 使用 Anthropic envelope；这只应用于命中 profile 后由
本地产生的协议错误，上游 status/header/body 不重包。Messages 在 11.5 不执行任何
专用 rewrite；阶段 11.7 已把三个 handler 与透明普通响应/SSE/Usage 接入生产 Server。

Registry 对未知/重复 protocol、非法 method、Usage capability 和已知 specialized rule
不适用组合在 snapshot 发布前失败。未知 generic rule 不由 protocol 层猜测，交给
RuleRegistry。增加测试 protocol 只需注册一个 handler；RouteTable、worker 和 transport
不需要修改。

## Rule Pipeline

### Rule 定义

每条 rule 至少包含：

```text
id
enabled
type
typed options
```

配置加载时，`RuleRegistry` 根据 type 查找 factory，factory 校验参数和 protocol，
然后生成只读 `CompiledRule`。未知 type、未知字段、错误类型和不支持的协议组合会
在启动或 reload 前失败。

请求执行过程：

```text
raw body
  -> pipeline empty? -> reuse raw body
  -> parse JSON once
  -> Rule 1
  -> Rule 2
  -> ...
  -> modified? serialize once : reuse raw body
```

一条 pipeline 中所有 JSON rule 共享一个 DOM。不能让每条 rule 各自 parse/dump。
rule 返回统一结果：matched、modified、reason 和有界 summary；日志不保存第二份
完整 body。

阶段 11.6 已实现上述编译和执行核心。`RuntimeSnapshot` 同时持有 immutable
`ProtocolRegistry` 与 `RuleRegistry` generation，每个 `RuntimeProfile` 持有编译后的
只读 pipeline。空 pipeline 对任意 body 都保持零 parse；非空 pipeline 最多一次 parse
和一次 serialize。rule 失败时 candidate DOM 整体废弃，调用方只能继续使用原始 body，
不会看到部分修改。

### Generic Rule

Generic rule 使用 RFC 6901 JSON Pointer 定位，不理解具体 LLM 协议。首批候选：

```text
set_field
remove_field
rename_field
append_array
insert_array
merge_object
```

每种操作都必须定义：目标不存在时行为、类型冲突行为、数组越界行为以及是否允许
创建中间对象。默认采用严格失败，避免拼错路径后静默发送错误请求。

首批已实现 `set_field` 与 `remove_field`。两者都只定位已存在目标，不创建中间层；
`set_field` 允许空 pointer 替换 root，相同值记为 matched 但不序列化；`remove_field`
禁止 root。array token 使用严格十进制 index，不接受 `-`、前导零、非数字或越界值。

### Specialized Rule

Specialized rule 理解协议结构，例如：

```text
remove_tool
replace_tool
append_system
rewrite_messages
merge_consecutive_messages
rewrite_thinking
```

同一个 rule type 可以为多个 protocol 注册实现，但不要求所有协议都支持。比如
`remove_tool` 的 Responses、Chat 和 Messages 表示可能不同，适用矩阵在配置编译时
验证，不在请求运行中 fallback。

当前 findcg 行为迁移为普通配置：在 `findcg` profile 上启用
`remove_tool(tool=image_gen)`。新 rule 不再硬编码 findcg host；是否应用由 profile
的显式 pipeline 决定。

`remove_tool` 的三个首批实现已编译进 registry：Responses 检查 root tool 的 `name`
或 `namespace`，Chat 检查 `function.name`，Messages 检查 root `name`。root 必须是
object；`tools` 缺失或不是 array 时保持透明；匹配项按原顺序稳定删除，全部删除后保留
空 array。阶段 11.7 已让生产 Request Route 执行该 pipeline，并移除 Server 中的
findcg host/transform 名称特判。

### Rule 日志

每条 rule 记录：

```text
request_id
profile_id
protocol
rule_id
rule_type
matched
modified
reason
bounded change summary
duration_us
```

配置中的长文本、完整替换值和系统提示默认不写日志。必要时记录长度或 digest，避免
把 profile 中的敏感业务文本复制到日志。

## Runtime Snapshot 与 Reload

目标 runtime generation：

```text
RuntimeSnapshot
  validated application settings
  immutable route index
  compiled protocol handlers/references
  compiled pipelines
  transport/logging policies
```

构建流程：

```text
parse ConfigDocument
  -> schema/type validation
  -> profile/rule validation
  -> compile route index and pipelines
  -> validate topology
  -> publish shared_ptr<const RuntimeSnapshot>
```

worker 开始读取 HTTP 请求前只获取一次 generation。reload 原子交换 generation；
in-flight 请求继续持有旧 route、pipeline、upstream、限制和日志策略。listener、worker、
metrics reporter 或同路径 writer 拓扑变化使用受控 restart 和失败回滚；profile、route、
upstream、rule、timeout、body limit、body logging 与日志路径可对新请求热切换。每个
RequestGeneration 有进程内单调 id，swap 日志链接前后 id。

配置编辑对象、JSON DOM 和 runtime snapshot 必须分开，不能为了 CLI set 在运行中
原地修改已发布对象。

## CLI 目标

应用级配置：

```text
ccs-trans config show
ccs-trans config set <key> <value>
ccs-trans config unset <key>
```

代理链：

```text
ccs-trans profile list
ccs-trans profile show <name>
ccs-trans profile create <name>
ccs-trans profile remove <name>
ccs-trans profile enable <name>
ccs-trans profile disable <name>
ccs-trans profile set <name> <key> <value>
ccs-trans profile unset <name> <key>
```

规则：

```text
ccs-trans rule list <profile>
ccs-trans rule show <profile> <id>
ccs-trans rule add <profile> <id> <type>
ccs-trans rule remove <profile> <id>
ccs-trans rule enable <profile> <id>
ccs-trans rule disable <profile> <id>
ccs-trans rule set <profile> <id> <key> <value>
ccs-trans rule unset <profile> <id> <key>
ccs-trans rule move <profile> <id> <position>
```

运行：

```text
ccs-trans run
ccs-trans run --profile <name>
ccs-trans run --log-level <level>
ccs-trans run --log-path <path>
```

`profile create` 默认创建 disabled 草稿，`rule add` 默认创建 disabled rule。这样每个
`set` 仍只修改一个字段，同时不会要求一条命令携带整组配置。enable 操作执行完整
可运行性校验。

`run` 不再接受其他配置覆盖。`--profile`、`--log-level` 和 `--log-path` 是仅有的
一次性运行选项，不写回配置；长期设置通过 `config/profile/rule` 命令逐项修改。
三个字段都只有上述一个规范名称，不增加短 alias 或同义命令。

CLI 细节固定为：

- `config unset` 把单个应用字段恢复为内建默认值；若与其他字段形成无效组合则失败；
- `profile/rule enable` 不隐式补字段，候选文档必须先通过对应完整校验；
- `rule move` 的 position 为 1-based，范围是当前 pipeline 的 `1..size`；
- `rule set` 的 value 若是合法 JSON 则保留 boolean/number/object/array/null 类型，
  否则作为字符串；看似 JSON 的字符串可显式传 JSON quoted string；
- show/list 输出保持 JSON 类型，并由 canonical document serializer 派生，不维护第二套
  字段拼写；
- 修改先作用于文档副本，完整校验和 `ConfigStore` 原子保存成功后才发布结果。

`config_cli` parser/executor 在 11.3 独立完成，并在 11.7 与 v2 runtime、单 listener
一起进入生产 host。CLI 保存前会对所有 enabled Profile/Rule 执行 ProtocolRegistry 与
RuleRegistry 语义校验；disabled 未知 Rule 仍可作为未来草稿保存。

## Schema 切换

新模型改变了 profile 的含义，无法把 `ccs-trans.config/v1` 当作同一结构继续读取。
处理原则：

1. runtime loader 只接受目标 schema；
2. 遇到旧 schema 时明确报错并保持原文件不变；
3. 不做字段 fallback、双模型常驻或静默自动迁移；
4. 若真实配置数量证明有需要，可另做一次性离线转换命令，但转换器不进入请求路径；
5. 首次写新 schema 前先通过临时文件完整验证，继续使用原子替换。

这不是保留旧版兼容，而是保证不误毁用户已有配置。

## 性能设计

重构不能牺牲现有 8-16 路桌面负载。实现时保留以下不变量：

- route index 在 snapshot 构建时完成，单请求不线性扫描 profile；
- pipeline 和 rule 参数只编译一次；
- 空 pipeline 不解析 JSON；
- 非空 pipeline 最多 parse 一次、serialize 一次；
- 未修改 body 继续复用原始 bytes；
- 日志 summary 有界，不复制完整 DOM 或 SSE；
- profile/rule 指标 label 来自有上限的配置集合；
- 所有队列、配置大小、profile 数和 rule 数有上限；
- worker、logger、WinHTTP session 继续进程级共享；
- SSE、取消和 timeout 行为不因业务模型改变。

先用当前同步模型完成重构并跑同一 benchmark。只有 8/16 路、`mixed-16` 或明确的
目标平台数据证明执行层成为瓶颈时，才单独设计异步 listener/transport。不要同时
改业务模型、配置 schema 和网络并发模型，否则无法定位回归来源。

## 可观测性变化

现有 endpoint label 改为：

```text
profile_id
protocol
route_kind
```

进程级 worker、连接、logger 和 transport 指标保持全局。profile 级只记录路由数、
请求数、失败数、queue wait 和 rule 汇总，不为任意 JSON path 或动态 rule value
创建 metrics label。

Usage 继续采用最小日志策略。它记录 profile、protocol、target、status 和 duration，
不进入 request body/rule pipeline。

## 目录目标

目录按稳定职责组织，不为每个小 rule 建一层空目录：

```text
src/
  app/
    app_service.*
  config/
    app_paths.*
    config_document.*
    config_store.*
    runtime_compiler.*
  core/
    cancellation.*
    http_types.hpp
    request_id.*
    runtime_metrics.*
    timeouts.hpp
    url.*
  hosts/
    cli_main.cpp
  logging/
    logger.*
  protocols/
    protocol_handler.*
    protocol_registry.*
    responses_handler.*
    chat_handler.*
    messages_handler.*
  routing/
    profile.hpp
    route_table.*
  rules/
    rule.*
    rule_registry.*
    generic_json_rules.*
    remove_tool_rule.*
  runtime/
    runtime_snapshot.*
  server/
    server.*
  transport/
    header_filter.*
    upstream_transport.hpp
    windows/
      winhttp_transport.*
```

现有文件在对应接口工作包落地时移动，CMake、include 和测试同一提交更新。不会先
创建空目录或做纯路径搬运。

## 复用与替换边界

直接复用并保持行为：

- `AppService` 的启动、停止、wait、reload 和回滚语义；
- cancellation、request id、HTTP types、URL parser 和 split timeout；
- logger 的批写、错误 flush、背压和健康状态；
- WinHTTP session、流式 callback、客户端取消和响应限制；
- worker 预热/按需增长、连接总量限制和 benchmark harness。

需要替换：

- 两个固定 `EndpointGroupConfig`；
- 当前“整套 AppConfig 覆盖”的 `ProfileStore`；
- endpoint/task 枚举驱动的 `TaskRouter`；
- `Server` 内两个 router 和 transform 名称判断；
- findcg host 与 `image_gen` 规则绑定；
- endpoint 固定维度的部分日志与 metrics。

## 完成判据

重构完成时必须同时满足：

1. 一个 listener 可同时服务多个 enabled profile。
2. Responses、Chat、Messages 分别通过 protocol registry 路由。
3. Usage 精确跟随命中的 profile upstream。
4. findcg 行为由 profile rule 表达，不再由 host 特判。
5. 新增 profile 不修改 C++ enum 或 `Server` 分支。
6. 新增 generic rule 不修改 router、transport 或 worker。
7. route collision、未知 protocol/rule 和不完整 enabled profile 在启动前失败。
8. reload 后旧请求保持旧 generation，新请求使用新 route/pipeline。
9. 透明请求 body 字节不变，SSE、取消、timeout、日志和限制行为不退化。
10. `desktop-8`、`desktop-16`、`mixed-16` 和 `stress-50` 使用同一口径回归。
11. 文档、CLI help、schema fixture、目录和发布白名单保持一致。

完成通用化重构后，再在这个稳定应用接口上实现 Windows tray 和 macOS 菜单栏宿主。
