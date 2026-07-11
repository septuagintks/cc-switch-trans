# ccs-trans 开发计划

## 当前起点

当前实现版本为 `0.4.0`，Windows x64 实测和自动化回归均已完成。以下能力作为
后续工作的稳定起点，不再以历史阶段重复描述：

- Responses 端点监听 `127.0.0.1:15723`，并拥有自己的 Usage 路由与上游；
- Chat Completions 端点监听 `127.0.0.1:15724`，并拥有自己的 Usage 路由与上游；
- 两个 listener 共享连接容量、按需 worker pool、logger、metrics 和 WinHTTP session；
- findcg Responses 请求可以删除根级 `image_gen` 工具声明；
- 普通 JSON 与 SSE 均可透明转发，客户端断开会取消对应上游请求；
- 日志按约 100 ms 批写，错误立即 flush，队列有界且不静默丢失记录；
- 配置保存在用户 `.ccs-trans` 根目录，profile 使用 typed schema 和原子保存；
- 不可变配置 snapshot 支持 request generation、hot reload 和失败回滚；
- Debug、Release、CTest、Python integration 和合成 benchmark 已形成固定验证入口。

后续功能必须复用 `AppService`、配置、路由、日志和关闭顺序。宿主 UI、平台代码或
新业务规则不能复制这些实现。

## 固定工程约束

### 协议与透明性

1. 未命中规则的请求保留原始 body，不做无意义的 JSON 重新序列化。
2. URL 和 JSON 使用结构化解析，不使用字符串替换实现业务规则。
3. 每项修改必须在发送上游前完成，并记录可由 `request_id` 关联的结果。
4. 解析失败不能发送部分修改后的请求。
5. Usage 不记录 headers、query 或 body。

### 性能与资源

1. 聚合 8-16 路 SSE 是桌面常规负载，50 路是压力测试。
2. 默认 worker 上限为 32，启动预热 8 个并按队列需求增长。
3. `max-connections` 表示活动与排队连接总量，不能与 worker 数混用。
4. 所有内部队列必须有界，SSE 内存不能随累计流长度增长。
5. 正常日志允许约 100 ms 批写；错误事件立即 flush。
6. SSE 使用带序号增量 chunk 日志，不为日志保留完整 response body。
7. 更换网络模型必须由可重复 benchmark 证明，不能只根据单次高并发离群值决定。

### 配置与兼容性

1. 一个配置字段只有一个规范名称，一次命令只修改一个 profile 字段。
2. 不增加旧 CLI alias、共享 fallback 或隐式参数覆盖。
3. profile 不保存 API key、Authorization、Cookie 或其他请求凭据。
4. reload 对新请求原子生效，in-flight 请求保持原 generation。
5. 需要重启的配置变更必须有失败回滚。

### 平台边界

1. 核心任务、规则和配置类型不能包含 WinHTTP、Winsock、Windows 消息循环或
   macOS Framework 类型。
2. CLI、Windows tray 和 macOS menu bar 必须调用同一应用服务接口。
3. 平台启动项状态必须读取操作系统真实状态，不能只保存一个配置布尔值。

## 阶段 11：Windows 后台常驻与托盘宿主

目标：在保留 CLI 自动化入口的同时，交付可双击启动、可后台常驻的 Windows
图形宿主。

### 11.1 宿主边界

1. 确认 `ccs-trans-core` 对图形宿主暴露的最小接口：配置加载、启动、停止、reload、
   状态查询和最后错误。
2. 图形宿主不直接创建 listener、WinHTTP session 或 logger。
3. 明确采用单一 launcher 还是独立 CLI/tray 可执行文件；选择必须保证命令行退出码
   和无控制台双击体验都清晰。
4. 增加单实例控制。第二次图形启动应激活已有实例或返回明确状态。

### 11.2 托盘菜单

菜单至少包含：

```text
运行状态
启动 / 停止
重新加载配置
打开配置文件
打开日志目录
开机自启（勾选项）
退出
```

点击托盘图标打开菜单。菜单状态必须来自 `AppService` 和系统启动项，而不是 UI
内部缓存。启动、reload 和配置错误通过通知或对话框显示，后台模式不能静默失败。

### 11.3 双击与后台生命周期

1. 双击程序时读取 active profile 并隐式启动服务。
2. 关闭控制台或托盘菜单不应误杀另一个宿主拥有的服务实例。
3. 退出命令必须停止 listener、取消上游请求、合并线程并 drain logger。
4. 系统关机和用户注销路径也应执行有界关闭。

### 11.4 开机自启

1. 使用当前用户级注册方式，不要求管理员权限。
2. 勾选状态每次打开菜单时读取操作系统实际注册状态。
3. 启用、禁用和重复操作保持幂等。
4. 开机自启与“双击后隐式运行”是两个独立行为，不共享一个不明确的布尔字段。

### 11.5 测试

- 服务状态机测试覆盖连续启动、停止、reload 和失败恢复；
- 单实例测试覆盖首次启动、二次激活和异常退出后的恢复；
- 启动项测试使用当前用户隔离 fixture 或可回滚测试键；
- tray 运行时继续执行双 endpoint、Usage、SSE 和取消集成测试；
- 无控制台运行产生错误时，用户能够看到并定位日志。

完成标准：

- 双击可在后台启动 active profile；
- 终端关闭不影响 tray 宿主中的服务；
- 菜单状态与真实服务、真实启动项一致；
- GUI 和 CLI 的路由、日志、配置校验、reload 与关闭行为一致；
- 发布包不包含用户配置、日志或测试数据。

## 阶段 12：macOS 支持与菜单栏宿主

目标：复用协议、配置、规则、日志和测试，只替换必要的平台网络与宿主实现，交付
Apple Silicon CLI 和菜单栏应用。

### 12.1 核心去平台依赖

1. 审计 public header，清除 Windows 类型和 include 泄漏。
2. 为本地 listener 和上游 transport 定义能够表达流式回调、取消和分阶段错误的
   平台接口。
3. Windows 实现迁入明确的平台目录，并保持现有行为与 benchmark 基线。
4. 公共配置路径继续使用 `~/.ccs-trans/` 语义，不把 app bundle 内目录作为数据根。

### 12.2 macOS 网络实现

1. 实现本地 HTTP listener 和 TLS 上游 transport。
2. 验证系统代理、DNS、证书、SSE、客户端取消和各阶段 timeout。
3. 保持 request/response header 过滤规则与 Windows 一致。
4. 使用同一组 mock upstream 和协议 fixture 做跨平台对照。

### 12.3 菜单栏应用

macOS 菜单至少包含：

```text
运行状态
启动 / 停止
重新加载配置
打开配置文件
打开日志目录
登录时启动（勾选项）
退出
```

菜单栏宿主复用与 Windows tray 相同的应用命令接口。登录项优先使用满足最低系统
版本的 `SMAppService`，状态显示以系统查询结果为准。

### 12.4 构建与发布

1. 首先支持 Apple Silicon `arm64`。
2. 根据实际需求再决定 `x86_64` 或 Universal 2。
3. 固定最低 macOS 版本和工具链。
4. 建立 `.app`、CLI、签名、公证及压缩包或 DMG 流程。
5. 发布说明明确架构、最低系统版本、签名与公证状态。

完成标准：

- 核心 target 不依赖 Windows 系统类型；
- Responses、Chat、Usage、SSE、改写、reload 和日志测试跨平台通过；
- 菜单栏可以管理服务并显示真实登录项状态；
- 退出和系统注销会有界停止服务并关闭 logger；
- macOS 发布包不携带用户数据或凭据。

## 每阶段 Review 顺序

每个可独立交付的小工作包按以下顺序结束：

1. 先做针对性测试和静态审查；
2. 检查错误路径、资源上限、线程关闭和敏感日志；
3. 执行相关 CTest/Python integration；
4. 涉及网络、worker 或日志时重跑对应 benchmark；
5. 更新 Design、DevelopmentPlan、ProjectStructure 和 README；
6. 检查发布白名单和工作区状态后再提交。

Windows tray 完成后再开始 macOS 平台实现。两者之间复用的是应用服务接口和核心
能力，不是复制宿主代码或平台 API 包装。
