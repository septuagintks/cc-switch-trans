# ccs-trans 后续开发计划

## 当前基线

通用化重构已经完成，当前生产模型只有一套：

```text
ConfigDocument
  -> RuntimeCompiler
  -> immutable RuntimeSnapshot
  -> one listener / many Profiles
  -> ProtocolHandler + ordered Rule pipeline
  -> platform UpstreamTransport
```

Windows CLI 已支持同端口多 Profile 的 Responses、Chat Completions、Messages 及各自
Usage；findcg 的 `image_gen` 删除由普通 `remove_tool` Rule 表达。源码、测试、benchmark
与构建目标只保留当前通用模型。

当前稳定约束：

1. Windows 最低版本为 Windows 11 21H2 x64。
2. Windows WinHTTP 跟随当前用户手动系统代理、bypass 与显式 PAC；选定代理失败不直连。
3. 不支持代理账号或密码，不读取、提示或保存代理凭据。
4. macOS 后续链接系统 libcurl，只继承启动进程环境，不读取或激活系统代理。
5. 配置根为 Windows `%USERPROFILE%/.ccs-trans/`、macOS `~/.ccs-trans/`。
6. 正常日志约 100 ms 批写，error 立即 flush；SSE 使用连续序号的增量 chunk 日志。
7. 默认 worker 上限 32、预热 8；8-16 路 SSE 是桌面常规负载，50 路是压力测试。
8. reload 对新请求原子生效，in-flight 请求保持旧 generation。

阶段 11 的冻结基线附加 TTFB p50 为 `desktop-8 10.619 ms`、`mixed-16 9.474 ms`。
最终同机三轮 Release 中位数分别为 `9.625 ms` / `6.552 ms`，附加 p95 中位数为
`7.364 ms` / `0.931 ms`；三轮均零请求失败、零 writer failure、零 backpressure，峰值
working set 中位数约 `12.16 MiB` / `13.73 MiB`。原始结果只保存在 ignored
`benchmark-results/`，不进入发布包。

## 开发规则

后续每个工作包必须独立完成：

1. 先写边界测试，再接生产宿主；
2. review 生命周期、线程、取消、配置原子性、日志敏感信息和平台所有权；
3. 运行相关 unit、CTest 与 Python integration；
4. 涉及请求路径、日志、worker 或 transport 时运行可比 benchmark；
5. 同步 README、Design、ProjectStructure 和用户可见 help；
6. 使用签名 commit，验证后再推送并进入下一包。

禁止为未来宿主复制 config/runtime/server/logger 初始化。CLI、tray 和 menu bar 必须调用
同一应用服务接口。

## 阶段 12：Windows Tray 与后台宿主

### 12.1 图标资产与构建工具

唯一手工维护母版：

```text
assets/icons/ccs-trans-512.png
```

新增 `tools/generate_icons.ps1`，检查 ImageMagick 后生成 Windows 多尺寸 ICO：

```text
256, 128, 64, 48, 32, 24, 20, 16 px
```

ICO 同时用于 executable resource 与 tray。必须实测 100%/125%/150%/200% DPI、浅色与
深色任务栏；若黑色母版在深色任务栏不可辨识，从同一母版生成 Windows 专用描边变体，
不手工维护另一套源图。

### 12.2 应用命令接口

在接 UI 前收敛宿主可调用命令：

```text
start
stop
reload
status
open config
open logs
get/set startup registration
shutdown
```

命令层只编排 `AppService`、`ConfigStore` 和平台 shell 操作，不暴露 Server、socket 或
WinHTTP handle。所有操作串行化，状态来自真实服务和系统启动项，不使用 UI 缓存猜测。

### 12.3 Tray 宿主与单实例

新增 Windows GUI subsystem 的 `ccs-trans-tray.exe`，保留现有 console CLI：

1. 双击 tray executable 不显示控制台并隐式启动全部 enabled Profiles；
2. 当前用户作用域命名 mutex 保证 tray 单实例；
3. 第二次启动通知已有实例并退出；
4. CLI `run` 与 tray 同时占用 listener 时返回明确的所有权冲突；
5. 配置查询和编辑命令不受 tray 单实例锁影响。

### 12.4 点击菜单

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

启动/reload 失败显示简短错误与日志位置。退出、用户注销和系统关机执行有界 stop、请求
取消、线程 join 与 logger drain。

### 12.5 开机自启

使用当前用户级注册，不要求管理员权限。启动项指向 tray host 的明确后台模式；菜单
每次打开都读取实际注册值。启用、禁用和重复操作必须幂等，且开机自启与双击启动互不
绑定。

### 12.6 Windows 验收与打包

- tray 与 CLI 的配置校验、路由、Rule、reload、日志和错误映射完全一致；
- 终端关闭不影响 tray 管理的服务；
- 图标在目标 DPI/主题清晰，菜单文本不截断；
- 单实例、CLI/tray 冲突、开机自启和关机 drain 有自动或可重复本机测试；
- 发布白名单加入 `ccs-trans-tray.exe` 与 ICO resource，不包含用户 `.ccs-trans`。

## 阶段 13：macOS CLI、菜单栏与打包

### 13.1 平台基线

先固定最低 macOS 版本、Xcode/Clang 与系统 libcurl 能力。首个目标为 Apple Silicon
`arm64`；是否提供 `x86_64` 或 Universal 2 由实际需求决定。若登录项采用
`SMAppService`，最低系统版本必须与该 API 的支持范围一致。

### 13.2 macOS 网络实现

在现有 `UpstreamTransport` 下新增 `src/transport/macos/curl_transport.*`：

1. 链接系统 libcurl，不打包私有 curl；
2. 继承 terminal/tray 启动环境中的 `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY` 与
   `NO_PROXY` 语义；
3. 不读取或修改 macOS System Settings 代理；
4. 实现普通响应、SSE callback、取消、DNS/connect/send/header/idle/total timeout 与
   response body limit；
5. 与 Windows 使用同一 mock upstream、fixture 和错误分类断言。

本地 listener 通过平台实现接入现有 Server 编排。公共 header 不得泄漏 WinSock、
WinHTTP、CFNetwork 或 curl 类型。

### 13.3 菜单栏宿主

macOS menu bar 提供与 Windows 相同的应用命令集合，并确认有菜单栏图标。菜单栏图标由
`assets/icons/ccs-trans-512.png` 派生 PNG，以 template image 验证浅色/深色模式。

菜单包含运行状态、启动/停止、reload、打开配置、打开日志、登录时启动勾选和退出。
登录项读取系统真实状态；优先使用目标最低版本支持的 `SMAppService`。

### 13.4 macOS 发布

1. 先验证 CLI，再建立 `.app` bundle；
2. 根据分发方式完成 codesign、公证及 ZIP/DMG；
3. 发布说明写明架构、最低版本、签名和公证状态；
4. Finder app icon 与 menu bar template image 分开处理；
5. 包内不含用户配置、日志、真实请求、benchmark 或构建机状态。

## 性能与架构取舍

继续保留同步 listener + 有界 FIFO worker 模型，原因是当前桌面负载和压力测试均无证据
要求全异步重写。以下任一现象才单独立项异步网络栈：

- 8-16 路 SSE 出现持续 queue wait 或 Usage 饥饿；
- 50 路压力下出现无界内存/线程增长或无法及时停止；
- 同机三轮中位数显示常规负载附加 TTFB p50/p95 退化超过 15%/20%；
- 平台 transport 无法在现有 callback/cancellation 接口下正确实现。

Rule pipeline 保持空 pipeline 零 parse，非空 pipeline 最多一次 parse/serialize。日志不能
为 SSE 或非流式响应额外保留第二份完整 body。平台 UI 不得把请求处理搬到 UI 线程。

## 暂不进入范围

- GUI 内完整 JSON/Profile/Rule 编辑器；
- Provider/API key/模型管理；
- 自动故障转移或负载均衡；
- response transformation pipeline；
- 远程管理接口；
- 没有需求依据的 Universal 2 或全异步重写。
