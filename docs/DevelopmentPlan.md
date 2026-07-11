# ccs-trans 后续开发计划

## 计划范围

当前 `0.4.0` Windows 实现是重构基线。本文只保留尚未完成的工作，不再记录历史
版本、旧 CLI 迁移和已完成阶段流水。

后续顺序固定为：

```text
阶段 11  通用 Profile / Protocol / Rule 重构
    |
    v
阶段 12  Windows tray、后台常驻、双击启动、开机自启
    |
    v
阶段 13  macOS transport、菜单栏宿主、登录项与打包
```

托盘和 macOS 的功能范围不变，只因通用化重构成为前置工作而顺延。重构目标模型见
[Reconstruction.md](Reconstruction.md)。

## 不变量

任何工作包都必须保持：

1. 普通 JSON、SSE、query、必要 headers、status 和 response headers 可转发。
2. 客户端断开只取消对应上游请求。
3. timeout 分为 resolve、connect、send、response header、stream idle 和 total。
4. 正常日志约 100 ms 批写，错误立即 flush，队列有界且不静默丢记录。
5. SSE 只保留带序号的增量 chunk 日志，不累计完整 response body。
6. 请求和非流式响应分别受 body size limit 约束。
7. config、logs 和 state 保持在用户 `.ccs-trans` 根目录。
8. profile 不保存 API key、Authorization 或 Cookie。
9. reload 对新请求原子生效，in-flight 请求保持旧 generation。
10. 8-16 路 SSE 是常规桌面负载；50 路是压力测试，不是常规 SLO。

## 开发与 Review 规则

阶段 11 拆成可独立 review 的小工作包。每个工作包必须：

1. 只引入一组可解释的模型或行为变化；
2. 先补失败用例和边界测试，再切换生产路径；
3. 检查线程、所有权、取消、错误映射、配置原子性和敏感日志；
4. 运行相关 unit/CTest/Python integration；
5. 涉及路由、JSON、worker、日志或 transport 时运行对应 benchmark；
6. 更新与本工作包直接相关的文档；
7. review 通过后再进入下一个工作包。

源码移动必须和职责改变、CMake/include 更新及测试放在同一个提交中。禁止先创建
空目录或做无法独立验证的整树搬运。

## 阶段 11：通用化重构

### 目标

把当前固定的 Responses/Chat 双端点模型替换为单 listener、多代理 Profile、协议
handler registry 和可配置 rule pipeline。重构完成后，新增 profile 不修改 C++
任务枚举，新增 rule 不修改 `Server`，新增 protocol 不修改 worker/transport。

### 11.0 冻结行为与测量基线

目的：在切换业务模型前，建立可以证明“没有顺手改坏网络层”的对照。

构建顺序：

1. 为当前 findcg Responses 行为保存纯合成 fixture：含 `image_gen`、不含目标工具、
   无 `tools`、非法 JSON 和非 findcg upstream。
2. 固定双 Usage、普通响应、SSE、query、header、取消、六类 timeout、body limit、
   overload 和 logger failure 测试入口。
3. 用同一 Release 构建各跑三次 `desktop-8`、`desktop-16`、`mixed-16` 和
   `stress-50`，记录 commit、exe hash、机器信息和中位数。
4. 增加“透明请求 body bytes 完全相同”断言，后续 pipeline 不能只比较 JSON 语义。
5. 给现有 config document 准备只读 fixture，用于证明新 loader 会安全拒绝旧 schema
   而不是覆盖文件。
6. 冻结 Windows 11 WinHTTP system proxy 与 macOS terminal/libcurl 代理合约，为平台
   transport 测试提供唯一预期。

Review 重点：fixture 不含真实上下文或凭据；benchmark mock backlog、timeout 和日志
策略固定；stress 结果只作为容量行为，不用于承诺 50 路延迟。

完成标准：所有当前行为可由自动测试复现，基线文件不会进入发布包。

阶段 11.0 已完成。基线来自干净提交
`c5d9f212e1910d1d92ef68f70b75fd47a824467a`，Release executable SHA-256 为
`88E4A880F33C2303584B1D44B26E25627E64E93A17675F0244C4298545F21821`。同一构建运行
三次，表中为中位数：

| Profile | 附加 TTFB p50 | 附加 TTFB p95 | Peak working set |
| --- | ---: | ---: | ---: |
| `desktop-8` | 10.619 ms | 10.661 ms | 13.566 MiB |
| `desktop-16` | 8.985 ms | 10.053 ms | 15.469 MiB |
| `mixed-16` | 9.474 ms | 9.000 ms | 14.977 MiB |
| `stress-50` | 10.689 ms | 1990.585 ms | 18.719 MiB |

三轮所有 profile 均为 0 请求失败、0 logger writer failure、0 logger backpressure。
`mixed-16` Responses/Chat Usage p95 中位数分别为 24.463/23.855 ms，三轮都在 SSE
进行期间完成全部 12 次 Usage；端点最大 queue wait 为 696 us。`stress-50` 最大
queue wait 约 2.002 s，继续作为有界压力行为，不作为桌面 SLO。

合成输入固定在 `tests/fixtures/stage11/`：findcg transform manifest 覆盖命中、
未命中、非法形状与失败关闭；透明 JSON 由集成测试比较上游接收的 byte size、
SHA-256 和 Base64；当前 schema 样例只读加载且不改写源字节。三轮完整原始结果保留
在 ignored `benchmark-results/stage11-baseline-run*.json`，不进入 Git 或发布包。

### 11.1 Windows 系统代理行为

目的：在业务模型重构前修正独立的 Windows 上游代理行为，避免后续把 transport
差异误判为 Profile/Rule 回归。

支持范围：Windows 11 21H2 x64；不增加 Windows 10 条件分支。

构建顺序：

1. 将进程级 WinHTTP default-proxy session 改为当前用户系统代理 snapshot：direct 与
   named proxy 选择对应 WinHTTP access mode；显式 PAC 按请求解析并固定结果。
2. 把平台代理策略封装在 Windows transport 内，不向 AppConfig/Profile 增加 proxy
   URL、模式、账号或密码字段。
3. 使用受控本地 HTTP proxy 和临时当前用户系统代理设置建立集成测试；测试必须在
   finally/RAII 路径恢复原系统设置。
4. 验证系统选择 proxy 时请求确实经过 proxy；proxy 关闭后请求失败，且 mock upstream
   没有收到 direct 请求。
5. 验证系统未配置 proxy 或 bypass 明确匹配时 direct 可用；这不是失败回退。
6. 使用 registry watcher 验证运行中切换系统代理后，新请求使用新 session，in-flight
   请求保持原 session。
7. 验证 `407` 被分类为不支持 proxy authentication，不尝试读取或持久化凭据。
8. 增加不含 proxy 地址/PAC/credential 的启动与错误日志字段。

测试不能修改机器级 `netsh winhttp` 配置；目标是当前用户 Windows 系统代理。测试若
无法使用隔离账户或可靠恢复设置，应拆为显式 opt-in 的本机集成测试，普通 CTest
只运行不改变系统状态的 transport 单元测试。

专项入口固定为 `tests/integration/run_windows_system_proxy_integration.py`，没有
`--confirm-system-proxy-mutation` 时必须拒绝运行。备份写入 ignored `tmp/`；只有全部
注册表值恢复并通知 WinINet 设置刷新后才删除备份。

Review 重点：失败后是否偷偷 direct、系统设置恢复、并发请求切换、PAC/bypass 语义、
407、日志泄密和 WinHTTP handle 生命周期。

完成标准：自动系统代理、失败不回退和运行时切换三项都有可重复证据；现有 direct、
SSE、取消和 timeout 回归通过。

阶段 11.1 已完成。生产 transport 将当前用户设置编译为 direct、named-proxy 或显式
PAC snapshot；PAC 通过 `WinHttpGetProxyForUrl` 按请求解析并固定结果，
auto-detect-only 不触发 WPAD。registry watcher 在变化时发布 session snapshot；
`407` 返回 `proxy_authentication_unsupported`；`server_start` 与
`upstream_request` 记录 `upstream_proxy_mode=windows_system`。普通 CTest 不修改
系统设置，opt-in 专项测试完成以下验证并恢复全部当前用户注册表值：

1. proxy A 接收第一次请求，直连 origin 计数保持 0；
2. A 上阻塞的 in-flight 请求保持 A；同一进程切换到 B 后，新请求由 B 接收；
3. 关闭已选 B 后本地返回 502，直连 origin 计数仍为 0；
4. 系统明确切换 direct 后，origin 才收到第一次请求；
5. auto-detect-only 不执行 WPAD 并直接命中 origin，手动 proxy bypass 同样命中 origin；
6. 显式 PAC 分别验证 proxy 与 direct；
7. PAC 返回 `PROXY dead; DIRECT` 时请求失败且不回退 direct；
8. proxy 返回 `407` 时分类为 `proxy_authentication_unsupported`；
9. 缺少确认参数时专项测试在任何注册表读取/写入前拒绝运行。

性能曾否决两个实现：全量 `AUTOMATIC_PROXY` 因默认 WPAD 增加约 4-6 ms；每请求读取
系统配置也超过常规门槛。最终 watcher + shared snapshot 三轮中位数为
`desktop-8 8.600 ms`、`mixed-16 8.435 ms` 附加 TTFB，均不高于阶段 11.0 基线，
且请求失败、logger failure 和 backpressure 均为 0。

### 11.2 建立新配置文档与领域类型

目的：先稳定数据模型，不立即切换正在工作的 server。

新增核心类型：

```text
ConfigDocument
ApplicationSettings
ProfileDefinition
LocalRoutes
UpstreamDefinition
RuleDefinition
ProtocolId
RuleId
```

构建顺序：

1. 实现 `ccs-trans.config/v2` 的严格 parser/serializer。
2. listener、runtime、timeouts 和 logging 放在应用级；protocol、local、upstream、
   rules 放在 profile 级。
3. 为 profile/rule id、URL、path、数值上限、未知字段和类型错误增加单元测试。
4. profile 和 rule 默认 disabled，允许使用单字段命令逐步构建草稿。
5. enabled 对象执行完整校验；disabled 草稿仍执行类型和局部格式校验。
6. 固定配置文档 4 MiB、128 profiles、256 routes、每 profile 64 rules 的初始上限。
7. 保留当前临时文件验证和原子替换，失败不能改动目标配置。
8. 旧 schema 返回明确错误并保持原文件不变，不在 runtime loader 中增加 fallback。

目录同步：实现落地时把配置文档和运行时配置拆为 `config_document`、`config_store`
和后续 `runtime_compiler`，不要继续让一个 `ProfileStore` 同时承担 schema、CLI value
解析、运行配置合并和 active profile 语义。

Review 重点：JSON 类型边界、整数溢出、路径逃逸、原子写失败、Windows 文件替换、
未知字段、配置大小和对象数量上限。

完成标准：新文档可以独立 round-trip；无效文档不会生成半份 runtime 数据；当前
server 尚未切换也能继续构建和测试。

阶段 11.2 已完成。`config_document` 提供 v2 typed editable domain、严格
parser/serializer、disabled 草稿与 enabled 文档级完整校验；`config_store` 提供跨进程写锁、
源字节修订检查、临时文件回读验证和原子替换。独立 CTest 覆盖 round-trip、默认值、
未知/重复字段、JSON 类型、整数溢出、ID/URL/path、数量上限、陈旧 writer、锁冲突，
以及 v1/超大文件失败后原字节不变。旧 `ProfileStore` 与生产 server 尚未切换，避免
在本工作包混入 CLI 或 runtime 行为变化。

### 11.3 重写配置 CLI

目的：让 CLI 对应新语义，并继续保持“一条命令修改一个字段”。

实现命令：

```text
ccs-trans config show|set|unset
ccs-trans profile list|show|create|remove|enable|disable|set|unset
ccs-trans rule list|show|add|remove|enable|disable|set|unset|move
ccs-trans run [--profile <name>] [--log-level <level>] [--log-path <path>]
```

决策：

- 删除持久 `active_profile` 和 `profile use`；
- `run` 默认加载全部 enabled profile；
- `run --profile` 只在本次运行编译一个 profile，用于诊断；允许选择配置完整的
  disabled profile，但不修改其持久 enabled 状态；
- `run --log-level` 和 `run --log-path` 是仅有的其他一次性覆盖，不写回配置；
- `profile set` 只修改 profile 字段，`config set` 只修改应用字段；
- `rule set` 只修改一条 rule 的一个 option；
- 不恢复共享 option、短 alias 或参数 fallback；
- `run` 不再承担其他临时配置覆盖，长期配置必须写入文档。

`profile create` 产生 disabled 空草稿；`rule add` 接受稳定 id 和 type，并产生
disabled rule。只有 enable 时要求完整可运行，避免一个命令同时写入多个字段。

Review 重点：命令语法歧义、重复参数、退出码、并发保存、失败后文件字节不变、
help 与 schema key 一致、日志路径仍限制在应用根或显式绝对路径。

完成标准：可以完全通过 CLI 建立两个同协议 profile 和一条有序 rule pipeline，
show 输出与磁盘 JSON 一致。

### 11.4 编译 RuntimeSnapshot 与路由表

目的：把磁盘配置与请求热路径彻底分开。

新增：

```text
RuntimeCompiler
RuntimeSnapshot
RouteKey(method, canonical_path)
RouteEntry
RouteTable
```

构建顺序：

1. parser 只产生可编辑 `ConfigDocument`。
2. compiler 读取 enabled profile，验证完整性并生成 immutable snapshot。
3. route table 使用 method + exact canonical path 建立 hash index。
4. request/usage route 都携带 profile id、protocol、upstream、route kind 和 pipeline。
5. 同一路径不同 method 可给出 405；完全未知路径给出 404。
6. route collision、重复 canonical path 和保留管理路径在 publish 前失败。
7. profile 上限和 route 上限阻止不受控 metrics cardinality 与内存增长。

性能要求：路由不线性扫描 profile；request path 只 canonicalize 一次；RouteEntry
不引用可被 CLI 编辑原地修改的 JSON 对象。

Review 重点：尾斜杠语义、百分号编码、query 分离、大小写规则、collision 诊断、
snapshot 所有权和 reload 并发。

完成标准：纯内存测试可同时路由多个 Responses/Chat/Messages profile 和各自 Usage，
不需要 endpoint/task enum。

### 11.5 建立 ProtocolRegistry

目的：把协议知识从 `Server` 和固定 task 类型中拿走。

首批 handler：

```text
ResponsesHandler
ChatHandler
MessagesHandler
```

构建顺序：

1. 定义最小 `ProtocolHandler` 接口和 capability 描述。
2. registry 使用稳定 protocol id 查找 handler，未知 id 在编译期失败。
3. handler 校验协议必需 route、主请求 method 和专用 rule 适用性。
4. 透明请求不因 handler 存在而解析 JSON。
5. Messages 先实现透明 request/response/SSE 转发和可选 Usage 路由，不立即加入
   Messages 专用 rewrite。
6. 协议错误映射保持 OpenAI/上游透明边界清晰，本地配置错误不伪装成上游错误。

目录同步：新增 `src/protocols`，只在第一个真实 handler 接入时创建；相关旧 task
文件在没有引用后再删除。

Review 重点：handler 是否泄漏 socket/WinHTTP/logger 生命周期，协议选择是否仍需
修改 `Server`，Messages headers 和 SSE content type 是否透传。

完成标准：增加一个测试 protocol 只需要注册 handler 和 fixture，不修改 router、
worker 或 transport。

### 11.6 建立 RuleRegistry 与编译 Pipeline

目的：把 findcg 特例提升为可组合请求转换能力。

第一批规则：

```text
remove_tool          specialized
set_field            generic JSON Pointer
remove_field         generic JSON Pointer
```

后续规则只有在前一批接口稳定后再加入：

```text
rename_field
append_array
insert_array
merge_object
replace_tool
append_system
rewrite_messages
merge_consecutive_messages
rewrite_thinking
```

构建顺序：

1. 定义 `RuleFactory`、`CompiledRule`、统一 result 和错误类型。
2. registry 在 snapshot 编译时验证 type、typed options 和 protocol 适用性。
3. pipeline 共享一个 JSON DOM：空 pipeline 零 parse，非空 pipeline 最多一次
   parse 和一次 serialize。
4. 未修改时复用原始 body bytes。
5. Generic rule 使用 RFC 6901 JSON Pointer，并为 missing/type conflict 定义严格行为。
6. `remove_tool` 分别验证 Responses、Chat、Messages 的结构，不做协议猜测。
7. 当前 findcg 功能改为 profile 中的 `remove_tool(image_gen)`，删除 host 特判和
   `Server` 中 transform 名称判断。
8. rule 日志只写有界 summary；长文本和值默认记录长度或 digest。

目录同步：新增 `src/rules`。初期按 registry、generic rules 和 specialized rule
分文件，不为每种微小操作创建独立目录。

性能测试：新增 0/1/8/32 rules、不同 JSON body 大小、命中/未命中和修改/不修改
的 microbenchmark，分别记录 parse、apply 和 serialize 时间。

Review 重点：rule 顺序、重复 id、错误中止、部分修改回滚、JSON pointer escaping、
DOM 复制、日志敏感值和 protocol capability。

完成标准：findcg 实测等价；同一 profile 可按顺序运行多条 rule；新增 generic rule
不修改 `Server` 或 transport。

### 11.7 Server 切换为单 Listener

目的：用 profile route table 接管真实请求，移除固定双端点编排。

构建顺序：

1. `Server` 只绑定应用级 listener。
2. acceptor 向现有共享 FIFO worker queue 提交连接，不携带 endpoint enum。
3. worker parse request 后从当前 snapshot 获取 `RouteEntry`。
4. request route 执行 protocol/pipeline，再调用现有 upstream transport。
5. Usage route 直接使用同一 profile 的 Usage target，不执行 request rules。
6. 日志字段从 endpoint/task 改为 profile/protocol/route kind。
7. metrics 保持全局资源维度，profile 维度只使用有界 id。
8. 删除两个 `BoundListener`、两个 router 和 endpoint 固定分支。

默认路径建议：

```text
/findcg/v1/responses
/findcg/v1/usage
/openrouter/v1/chat/completions
/anthropic/v1/messages
```

具体路径来自配置，不在代码中固定。cc-switch endpoint 指向对应 profile prefix。

Review 重点：启动原子性、exclusive bind、overload、method error、query、header、SSE、
客户端断开、worker 扩容、停止时 accept 唤醒和所有线程 join。

完成标准：一个端口同时完成多 profile Responses、Chat、Messages 与各自 Usage；
旧 endpoint enum 和双 listener 配置不再参与生产路径。

### 11.8 Reload、日志与指标收口

目的：让通用模型继承现有 generation 和可观测性，而不是降低可靠性。

构建顺序：

1. reload 编译完整候选 snapshot，成功后一次原子交换。
2. profile/upstream/route/rule 变化热切换新请求。
3. listener、worker 或 writer topology 变化继续受控 restart 和失败回滚。
4. 旧请求保持旧 profile、pipeline 和 upstream，直到完成或取消。
5. 日志加入 profile id、protocol、route kind、rule id/type/duration。
6. 移除 endpoint 固定 metrics，控制 profile label 数量。
7. 保持 Usage 最小日志和 SSE chunk 序号连续。
8. logger failure 仍能使服务进入明确失败路径，不能因 tray 前置而吞掉。

新增集成测试：A profile 的慢请求在 reload 前开始；reload 把同一路由切到 B；旧请求
必须完成于 A，新请求必须到 B。另测 rule 顺序切换、route collision 回滚和配置原子
保存。

完成标准：所有 reload generation 测试通过，日志可按 request/profile/rule 串联，
失败 restart 能恢复旧服务。

### 11.9 清理、性能回归与交付

清理范围：

- 删除旧 schema、active profile、`profile use` 和 endpoint-prefixed 运行字段；
- 删除 `EndpointGroupKind`、固定 task enum、双 router 和 findcg 专用 transform 类；
- 删除已无引用的兼容错误提示和旧测试 fixture；
- 按 [ProjectStructure.md](ProjectStructure.md) 完成源码归位；
- 更新 CLI help、README、schema 示例、发布白名单和第三方许可证。

不得保留“双模型都能跑”的长期分支。切换提交可以短暂同时存在新旧内部类型，但
阶段完成时生产路径和测试只能指向新模型。

性能回归：

| 场景 | 主要门槛 |
| --- | --- |
| `desktop-8` | 0 请求失败、0 writer failure、无持续 queue wait |
| `desktop-16` | 0 请求失败、Usage 可用、内存不随 SSE 累计增长 |
| `mixed-16` | 各协议 Usage 不等待全部 SSE 完成，profile queue wait 有界 |
| `stress-50` | 有界排队/拒绝、停止后资源回收，不要求常规负载延迟 |
| rule microbench | 空 pipeline 近似透明；parse/dump 次数不随 rule 数增长 |

同机同构建三次结果取中位数。常规场景附加 TTFB p50/p95 相对冻结基线退化超过
15%/20%，或 peak working set 增长超过 15%，必须定位后才能交付。单次噪声不直接
触发异步网络栈重写，但正确性、无界增长或 Usage 饥饿属于立即阻断项。

阶段 11 完成标准：

- Profile + Protocol + Rule 成为唯一生产业务模型；
- 单 listener 可同时服务多个代理链；
- findcg 规则由配置表达，Responses/Chat/Messages 均可透明转发；
- config/reload/logging/cancellation/SSE/timeout 性质不退化；
- 全量 review 无高优先级问题；
- Release tests、integration、benchmark 和白名单打包通过。

## 阶段 12：Windows Tray 与后台宿主

### 目标

保留 `ccs-trans.exe` CLI，新增 Windows GUI subsystem 的 `ccs-trans-tray.exe`。
两个宿主链接同一核心库。用户双击 tray 可执行文件后，程序读取持久配置、隐式启动
全部 enabled profile，并在后台常驻。

### 12.1 图标资产

PNG 母版已经归档：

```text
assets/icons/ccs-trans-512.png
```

它是唯一手工维护源。Windows 开发工作包使用 ImageMagick 生成包含多 DPI 尺寸的
ICO，不手工维护多份 PNG：

```text
magick assets/icons/ccs-trans-512.png -define icon:auto-resize=256,128,64,48,32,24,20,16 assets/icons/windows/ccs-trans.ico
```

实现时增加可重复的 `tools/generate_icons.ps1`，验证 ImageMagick 版本、输出尺寸和
alpha。当前母版是黑色单色 + alpha；macOS 将其作为 template image 交给系统着色。
Windows ICO 不会自动随任务栏主题 tint，ImageMagick 也只负责缩放，因此必须实测
深色任务栏。若对比度不足，应从同一母版派生带描边或浅色的 Windows variant，不能
假设系统会修正。生成的 ICO 供可执行资源和 tray 使用；512 PNG 保留给 macOS menu
bar。macOS app bundle 的 Finder 图标是否另生成 ICNS，在阶段 13 打包工作包决定，
不改变 menu bar 使用 PNG 的要求。

### 12.2 宿主与单实例

1. `ccs-trans-tray.exe` 只调用应用服务命令，不复制 config/router/logger 初始化。
2. 使用当前用户作用域的命名 mutex 保证 tray 单实例。
3. 第二次启动通知已有实例显示菜单或状态窗口后退出。
4. 明确 CLI `run` 与 tray 同时启动的所有权冲突，返回可理解错误而非只显示 bind
   failure；配置查询和编辑命令不应被单实例锁阻止。
5. tray 启动、reload、stop 和退出串行化，避免 UI 线程并发调用生命周期接口。

### 12.3 菜单

菜单至少包含：

```text
运行状态
启动
停止
重新加载配置
打开配置文件
打开日志目录
开机自启（勾选）
退出
```

点击图标显示菜单。状态来自 `AppService` 和操作系统实际启动项，不使用可能失真的
UI 缓存。后台启动或 reload 失败通过通知/对话框展示摘要，并给出日志位置。

### 12.4 开机自启与关闭

1. 使用当前用户级启动注册，不要求管理员权限。
2. 启动项指向 tray host，并使用明确的后台启动参数。
3. 菜单每次打开都读取实际注册值；启用、禁用和重复操作幂等。
4. 开机自启与双击隐式运行是独立概念。
5. 退出、用户注销和系统关机执行有界 stop、request cancellation、thread join 和
   logger drain。

### 12.5 验收

- 双击不会出现多余控制台窗口；
- 终端关闭不影响 tray 管理的服务；
- 菜单状态与真实 service/startup state 一致；
- tray 与 CLI 的配置校验、路由、规则、reload 和日志行为相同；
- 图标在 100%/125%/150%/200% DPI 和浅色/深色任务栏可识别；
- 发布包包含两个宿主、ICO 资源、README 和许可证，不含用户配置或日志。

## 阶段 13：macOS 支持与菜单栏宿主

### 目标

复用 Profile/Protocol/Rule、配置、日志、指标和测试，只替换必要的 listener、上游
transport、平台路径与宿主实现。先交付 Apple Silicon CLI，再交付菜单栏 `.app`。

### 13.1 平台接口

1. 审计所有 public header，移除 Windows include 和系统类型泄漏。
2. 定义 listener 与 upstream transport 接口，完整表达 streaming callback、取消、
   split timeout 和错误分类。
3. 将 Winsock/WinHTTP 移入 `transport/windows` 或明确 platform 目录。
4. Windows 行为和 benchmark 先保持不变，再接入 macOS 实现。

### 13.2 macOS 网络与路径

1. 使用系统 API 解析 account home，配置根为 `~/.ccs-trans/`。
2. 实现本地 HTTP listener、TLS upstream、terminal 环境代理、DNS、SSE 和取消；
   transport 链接系统 libcurl，不读取或修改 macOS 系统代理。
3. 保持 header 过滤、body limit、timeout 和错误映射跨平台一致。
4. 用同一 mock upstream 和 config fixture 做 Windows/macOS 对照。

### 13.3 菜单栏与登录项

菜单与 Windows 保持同一应用命令集合：

```text
运行状态
启动 / 停止
重新加载配置
打开配置文件
打开日志目录
登录时启动（勾选）
退出
```

menu bar 使用 `assets/icons/ccs-trans-512.png` 派生的 PNG 资源，并按 template image
行为验证浅色/深色菜单栏。登录项优先使用最低系统版本支持的 `SMAppService`，勾选
状态读取系统真实结果。

### 13.4 构建与发布

1. 首先支持 `arm64`，再根据用户需求决定 `x86_64` 或 Universal 2。
2. 固定最低 macOS 版本、Xcode/Clang 和依赖来源。
3. 先验证 CLI，再建立 `.app`、签名、公证及 ZIP/DMG 流程。
4. 发布说明明确架构、最低版本、签名和公证状态。
5. 发布包不包含 `.ccs-trans`、日志、真实请求或本机 benchmark 数据。

阶段 13 完成标准：Responses、Chat、Messages、Usage、rules、SSE、取消、reload、
日志和配置测试跨平台通过；菜单栏可管理服务并显示真实登录项；退出完整释放资源。

## 暂不进入范围

以下工作只有在数据证明必要时再单独立项：

- 全异步 listener/transport 重写；
- response transformation pipeline；
- GUI 内完整 JSON/profile/rule 编辑器；
- Provider/API key/模型管理；
- 自动故障转移和负载均衡；
- 远程管理接口；
- 无需求依据的 Universal 2 或 Linux 桌面宿主。

保持这些边界可以让本轮重构集中解决业务通用性，而不把 cc-switch 的职责重新搬进
`ccs-trans`。
