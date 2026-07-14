# ccs-trans 后续开发计划

## 当前基线

当前共享基础版本为 `0.5.0`，Windows 发行标识为 `0.5.0-Windows-x64`，macOS 首个发行标识
计划为 `0.5.0-macOS-arm64`。生产路径已经收敛为一套通用模型：

```text
ConfigDocument
  -> RuntimeCompiler
  -> immutable RuntimeSnapshot
  -> one listener / many Profiles
  -> ProtocolHandler + ordered Rule pipeline
  -> platform UpstreamTransport
```

源码使用 ISO C++20。Windows 基线为 Windows 11 21H2 x64，使用 WinHTTP 和当前用户
系统代理；macOS 尚未实现。配置根固定为 Windows `%USERPROFILE%/.ccs-trans/` 与 macOS
`~/.ccs-trans/`。默认 worker 上限 32、预热 8；8-16 路 SSE 是桌面常规负载，50 路是
有界压力测试。

后续阶段不再修改 Profile/Protocol/Rule 的通用业务模型，除非平台接入暴露了已被测试
证明的接口缺口。

## 开发规则

每个工作包按以下顺序完成：

1. 先冻结边界、状态和失败语义，再写单元或集成测试；
2. 接生产实现后 review 生命周期、线程、取消、配置原子性、日志敏感信息和平台所有权；
3. 运行相关 unit、CTest 与 Python integration；
4. 涉及请求路径、worker、日志、listener 或 transport 时运行可比 benchmark；
5. 同步 README、Design、ProjectStructure、help 与发布白名单；
6. 使用签名 commit，验证并推送后才进入下一工作包。

宿主 UI 不复制 config/runtime/server/logger 初始化，不在 UI 线程运行配置编译、服务停止、
网络请求或磁盘操作。没有 benchmark 证据时，不把同步 listener + 有界 FIFO worker 改写为
全异步网络栈。

## 阶段 12 实现记录

本阶段开始前的审计结论已经冻结：

1. `AppService` 已有 start/stop/reload/status，但构造时必须提供 snapshot，且 `wait()` 是
   面向前台 CLI 的阻塞终态接口。托盘需要在它上方增加可反复启动、停止、重载并回收异常
   退出线程的应用控制器；不能让托盘直接持有 `Server`。
2. `cli_main.cpp` 仍直接执行路径解析、ConfigStore、RuntimeCompiler 和 AppService 编排。
   阶段 12 先抽出共享控制器，再让 CLI 与托盘调用同一入口。
3. 阶段 12 不增加跨进程管理协议。托盘菜单只管理本托盘进程中的服务；CLI `run` 与托盘
   同时启动时，由 listener 独占绑定返回明确的端口所有权冲突。
4. Windows 宿主使用原生 Win32；macOS 宿主使用 Objective-C++/AppKit。平台 UI 类型不得
   进入 `src/app`、routing、rules、protocols 或 transport 公共 header。
5. 图标母版 `assets/icons/ccs-trans-512.png` 已确认是 512x512、32-bit ARGB PNG，SHA-256
   为 `FB40BC4B30BFD403CDDFB0E867C0CA700753E077ACB7473E1F879698A83BBA6D`。
6. 当前 Windows 工具链已有 CMake、Ninja、GCC 16、GNU windres 和 ImageMagick 7.1.2；
   `tools/check_stage12_prerequisites.ps1` 已完整通过。

`tools/check_stage12_prerequisites.ps1` 是阶段 12 的只读环境检查入口。Windows tray、
`0.5.0-Windows-x64` 发布包与 VM 验收均已完成；本节只保留实现边界，最终结果见
`docs/Archived/WindowsValidationCheckResult.md`。

## 阶段 12：Windows Tray 与后台宿主

### 12.1 共享应用控制器

实现状态：已完成。`ApplicationController`、共享 runtime loader 和 AppService 非阻塞退出
回收已经进入 core；CLI `run` 复用同一 loader，console 等待语义不变。

新增进程无关的 `ApplicationController`，拥有 AppPaths、当前 AppService 和最后一次运行
结果，提供以下语义稳定的操作：

```text
start    每次从磁盘重新 load + validate + compile，再启动服务
stop     请求停止、等待线程与 logger drain，托盘进程继续存在
reload   从磁盘编译完整候选；成功后原子应用，失败时保持当前 generation
status   返回真实状态、监听地址、last error/exit code
shutdown 停止服务并结束控制器，不允许后续命令
```

状态模型为 `stopped / starting / running / reloading / stopping / faulted / shutdown`。
`faulted` 表示
服务意外退出或 durability failure，保留 last error/exit code；用户仍可在修复配置或环境后
再次 start。控制器必须能非阻塞发现并回收已退出 AppService，避免 thread 仍 joinable 时
下一次 start 误报 already running。

所有 mutating command 串行执行。重复 stop 幂等；running 时 start、stopped 时 reload 返回
可展示的确定错误。候选配置在编译成功前不影响当前服务。CLI `run` 改用同一 snapshot
加载入口，但仍以前台方式等待服务退出。

测试覆盖：首次启动、停止后重启、异常退出后回收、并发命令串行化、reload 热交换、
restart reload rollback、配置文件缺失/损坏、端口冲突以及 shutdown 后拒绝命令。

最终测试覆盖首次启动、重复 start、失败 reload 保持运行、成功 reload、端口冲突、停止后
重启、幂等 stop、shutdown 后拒绝命令、durability failure 和 tray control executor 状态通知。

### 12.2 宿主平台操作

实现状态：已完成公共 HostPlatform、默认配置创建、Windows Shell adapter、当前用户
startup registration 与 tray 菜单接入，并通过真实 HKCU mutation、移动路径和重启登录验证。

定义不含 Win32/Cocoa 类型的宿主平台接口，承载：

```text
open config file
open logs directory
read startup registration
enable startup registration
disable startup registration
```

Windows 实现使用 `ShellExecuteW` 和
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，不要求管理员权限，不支持机器级
启动项。注册表 value 使用固定名称和带引号的 tray executable 绝对路径：

- 查询只有在 value 存在且目标路径等于当前 executable 时才显示已启用；
- enable 创建或修正 value，重复执行幂等；
- disable 只删除本应用固定 value，重复执行幂等；
- 安装目录变化后菜单显示未启用，再次勾选更新为新路径。

startup value store 可注入 fake；默认测试覆盖 quoting、大小写匹配、缺失值、错误旧路径
修正、enable/disable 和重复操作幂等，不读写真实 HKCU Run。真实注册表 mutation 由显式
opt-in 集成测试在 VM 中执行，测试后恢复原 value 并复核实际用户 hive。

打开配置前确保应用目录存在；若 `config.json` 不存在，以 ConfigStore 原子写入默认 v2
文档后再打开。打开日志只确保目录存在，不伪造日志文件。平台调用失败必须返回原始系统
错误和目标路径，供 UI 记录与精简展示。

### 12.3 图标、资源与构建

实现状态：已完成。ImageMagick 生成脚本、8 帧 ICO、RC/version resource、GUI subsystem
`ccs-trans-tray.exe` target 已进入 CMake，生成物只位于 binary directory。

新增 `tools/generate_icons.ps1`，使用 ImageMagick 从唯一母版生成构建目录内的多尺寸 ICO：

```text
256, 128, 64, 48, 32, 24, 20, 16 px
```

生成脚本校验源尺寸、alpha、输出帧尺寸和非空像素；缺少 `magick` 时立即给出安装/Path
错误。ICO 和 `.rc` 中间文件不提交，由 CMake custom command 生成。Windows tray target
启用 RC language，并把同一 ICO 写入 executable resource 和 `Shell_NotifyIconW`。

图标必须在 100%/125%/150%/200% DPI、浅色/深色任务栏和通知区域 overflow 中实测。
若黑色母版在深色任务栏不可辨识，由脚本从同一 PNG 生成带浅色轮廓的 Windows 变体；
不得手工维护第二份源图。

16px 帧在浅色/深色背景合成检查后确认黑色母版在深色背景对比不足；脚本现从 alpha mask
自动派生白色外轮廓，再生成所有 ICO 帧。浅色背景保留黑色主体，深色背景由轮廓保证边界，
母版未修改且没有新增手工图源。

### 12.4 原生 Tray 宿主

实现状态：已完成原生 Win32 host、隐藏窗口、通知区图标、点击菜单、Explorer 恢复消息、
异步 control executor、独立 host log 和启动时自动 start。console CLI 保持独立。

新增 Windows GUI subsystem target `ccs-trans-tray.exe`，保留 `ccs-trans.exe` console CLI。
tray 使用隐藏顶层窗口、`Shell_NotifyIconW`、`NOTIFYICON_VERSION_4`、`TrackPopupMenu` 和
`TaskbarCreated` 恢复通知区图标。双击 executable 不显示控制台，启动全部 enabled
Profiles；左键或右键点击图标均打开菜单。

菜单固定包含：

```text
运行状态（只读）
启动
停止
重新加载配置
打开配置文件
打开日志目录
开机自启（勾选）
退出
```

菜单打开时读取最新 controller 和注册表状态。Starting/Reloading/Stopping 期间禁用冲突
命令；状态变化通过 `WM_APP` 消息回到 UI。配置 load/compile、start/stop/reload、注册表
与 Shell 操作在一个专用 control executor 串行执行，窗口线程不阻塞等待。

宿主从进程启动起维护独立 JSON Lines 日志
`%USERPROFILE%/.ccs-trans/logs/ccs-trans-host.log`，记录命令、状态转换、单实例、启动项和 UI
错误，不记录请求 headers/body。它不得与 Server 的 `ccs-trans.log` 或自定义 runtime log
共享 writer。失败通知只显示动作、短原因和日志目录；完整系统错误写 host log。host log
本身无法写入时使用 `OutputDebugStringW` 并显示错误，不能静默失败。Explorer 重启后恢复
图标但不重启服务。

状态/启动项每秒在 control executor 刷新一次但不产生轮询日志；只有状态变化、新错误和用户
命令写入 host log。菜单 Start/Stop/Reload 已通过真实 `WM_COMMAND` 进程测试，Open config、
Open logs 和 startup adapter 使用阶段 12.2 的同一实现。

### 12.5 单实例、异常退出与关机

实现状态：已完成 session 级 mutex、第二实例窗口通知、faulted 状态轮询、菜单/WM_CLOSE/
WM_ENDSESSION shutdown 和 worker/logger join。没有 CLI 管理 IPC 或强制终止路径。

tray 使用当前交互用户 session 作用域的命名 mutex 保证单实例。第二实例通过已注册窗口
消息请求现有实例显示菜单，然后退出；本阶段不借此实现任意 CLI 控制或远程管理。

正常 Exit 的顺序固定为：禁用新命令、移除 tray icon、controller shutdown、等待请求取消
与 logger drain、销毁窗口。用户注销和系统关机走相同 request-stop 路径，并记录是否在
系统给定时间内完成。禁止 detach 服务线程或在仍写日志时直接 `TerminateProcess`。

服务自身异常退出时 tray 保持运行、状态转为 faulted，并允许查看日志和重新启动。配置
错误、端口占用、系统代理失败与 upstream 请求失败必须保持不同错误层级；普通 upstream
失败不能把整个 tray 标为 faulted。

进程集成已验证：隔离 `%USERPROFILE%` 自动启动 listener，Stop 后端口关闭，Start 后恢复，
Reload 成功，第二实例通知后退出，WM_CLOSE 完成 controller shutdown、runtime/host logger
drain 和端口释放。真实通知区模式也在交互桌面测试通过；受限自动化环境使用内部环境变量
跳过 NIM_ADD，但不跳过窗口、controller、单实例或退出路径。

### 12.6 Windows 自动验证

实现状态：自动与 VM 矩阵均已完成。Release 与 warnings-as-errors 构建的 CTest 均为
11/11，通过协议集成、tray Start/Stop/Reload/单实例/退出、真实 notification-area icon、
真实 startup/system proxy、DPI/主题、睡眠唤醒、Explorer 重启、重启/注销和 0/1/8/32 Rule
microbenchmark。需要修改 HKCU 或系统代理的测试仍保留显式 confirm 参数。

自动验证分四层：

1. ApplicationController 单测：状态机、命令串行化、重启、reload/rollback、异常回收；
2. 平台 adapter 单测：路径 quoting、注册值判定和幂等操作，默认使用 fake registry；
3. 子进程集成：无控制台启动、单实例、端口冲突、服务 stop/start、Explorer 消息恢复；
4. 现有 CTest、Python integration、Windows system proxy matrix 与完整 benchmark 回归。

真实 HKCU startup 写入测试必须显式 opt-in，并在 finally 中恢复原 value。最终 UI 手工
矩阵覆盖键盘菜单、DPI、浅/深色、Explorer 重启、终端关闭、休眠唤醒、注销和安装目录含空格。

请求路径性能验收沿用当前口径：三轮中位数下，8-16 路常规负载 added TTFB p50/p95
不得回退超过 15%/20%，Usage 不得被 SSE 饥饿，50 路必须保持连接、线程、logger queue
和内存有界。tray idle 状态不得持续轮询磁盘或产生高频日志。

`0.5.0-Windows-x64` 本机单轮预检中，`smoke`、`desktop-8`、`desktop-16`、`mixed-16` 和
`stress-50` 全部零失败。8/16 路 SSE added TTFB p50 分别为 `5.936 ms` 和
`6.484 ms`；mixed-16 为 `6.859 ms`，两组 Usage 各 12 次均在 stream 期间完成。
50 路由 32 个 worker 有界处理，peak worker 32、peak queue 25、无 logger backpressure；
因此后半批产生约 2 秒排队和 `1.986 s` 的 added TTFB p95。这是压力档的预期取舍，不能
把 stress-50 当成 50 路低延迟承诺；常规容量目标仍是 8-16 路。

VM 最终验证中五档负载继续全部零失败；2 小时 mixed soak 完成 13,712 条 SSE 和两组各
10,284 次 Usage，8 小时 idle 的 runtime/host log 增长均为 `0 bytes/h`，没有 writer
failure、logger backpressure、残留连接或持续资源增长。

### 12.7 Windows 打包与退出条件

实现状态：已完成并验收。`0.5.0-Windows-x64` 打包脚本和独立验包脚本从 ZIP 解压到临时
目录后检查精确白名单、两个 SHA-256、CLI/资源版本，并运行提取后的 tray 生命周期集成；
脚本兼容 Windows PowerShell 5.1。最终归档 ZIP SHA-256 为
`FD6E2812410F9B9274B9F54B04C6C461D582E1A4C68A69D341C9DC99CB9A005A`。

阶段 12 发行标识为 `0.5.0-Windows-x64`。固定白名单包括：

```text
ccs-trans.exe
ccs-trans-tray.exe
README.md
docs/
THIRD_PARTY_LICENSES/
SHA256SUMS.txt
```

ImageMagick 只作为构建工具，不进入发布包。SHA256SUMS 同时覆盖两个 executable。发布包
不得包含用户 `.ccs-trans`、生成日志、PDB、ICO 中间文件、benchmark 输出或构建机状态。

Windows 阶段已按项目范围通过。Defender 与 SmartScreen 未评估、二进制未做 Authenticode
签名、真实关机后外部开机未执行；Explorer 刚重启时新 tray 的短重试窗口和系统会话结束
日志不保证写出终止事件作为已接受限制归档，不阻塞阶段 13。

## 阶段 13 实现状态

实现状态：共享/macOS 工程实现已在 `dfcea1d` 完成，阶段 14 按用户指示暂不执行。当前机器
完成 Release 与 warnings-as-errors clean build、两套 10/10 CTest、共享协议集成、process proxy
矩阵、AppKit host/单实例/退出 smoke、Mach-O 依赖审计，以及正式文件名的 ad-hoc 固定白名单
archive 验证。完整命令与逐项状态记录在 `docs/MacOSValidationChecklist.md`。

已完成的实现边界：

1. `Server` 保留单份 HTTP/admission/worker/generation/logger 编排，socket 操作抽到
   `src/server/platform`；Windows 等价实现和 POSIX `poll`/signal/fd 生命周期分别落在平台源；
2. macOS `CurlTransport` 只从 selected SDK 链接 system libcurl；easy handle pool 上限等于
   worker 上限，每个 slot 保留上限为 4 的 connection cache；
3. 为满足 100 ms response-header 与 400 ms SSE idle 等亚秒 deadline，首版采用“每个池 slot
   一个持久 multi + 一个 easy”的同步 worker 内 poll loop。它没有改写 shared worker/admission
   为全异步网络栈；选择 multi 的理由是 easy progress callback 在无数据期间无法保证亚秒
   唤醒粒度；
4. CLI 与 AppKit menu host 共用 ApplicationController/AppService。CLI 接管 SIGINT/SIGTERM；
   GUI host 自己在 AppKit 主队列接管进程退出，Server 不抢占 GUI 进程信号；
5. `SMAppService.mainAppService`、AppKit、NSWorkspace、单实例和图标只存在于
   `src/hosts/macos`/packaging 层，未进入 core/routing/rules/protocols 公共 header；
6. 正式 `ccs-trans-0.5.0-macOS-arm64.zip` 统一使用 ad-hoc 签名；验包脚本除固定白名单、
   checksum 和 Mach-O 合同外，还显式拒绝非 `Signature=adhoc` 的 CLI 或 `.app`。

当前 active developer directory 是 Command Line Tools，机器上没有完整 Xcode.app，因此
完整 prerequisite 仍有一项未满足；真实登录项 mutation 和相应 Finder/登录环境手工证据也
未执行。Developer ID、公证、staple 和 `spctl` 不再是候选生成条件。ad-hoc 候选不建立可验证
的发布者身份，也不声明 Gatekeeper 信任；这是明确接受的发行溯源限制。

## 阶段 13：macOS CLI、菜单栏与打包

### 13.1 平台和工具链基线

macOS 支持线冻结为仅 macOS 26、仅 Apple Silicon `arm64`。使用 macOS 26 SDK、Xcode 26
或更新版本和 Apple Clang C++20 构建，`CMAKE_OSX_DEPLOYMENT_TARGET` 固定为 `26.0`。
不构建或测试 Intel `x86_64` 与 Universal 2，也不支持 macOS 15 及更早版本。

实现直接使用 macOS 26 上的 NSStatusItem、template image、POSIX socket、system libcurl 与
`SMAppService.mainAppService`，不增加旧系统 availability guard、legacy login item、兼容
分支或降级实现。release、性能和手工 UI 验证只以 macOS 26 arm64 环境为准。

开发必须在真实 macOS 或受支持的 macOS runner 上完成。阶段开始先建立 clean configure、
warnings-as-errors、CTest 和 package smoke；Windows 仍在同一提交上完整回归。

### 13.2 本地 Listener 平台化

当前 `Server` 内含 WinSock listener、socket registry、poll 和 cancellation monitor。先抽出
窄的 local socket adapter，并新增 POSIX 实现；HTTP parse、RouteTable、admission、worker、
generation 和日志编排继续只有一份。

adapter 只表达 bind/listen/accept、bounded receive/send、poll disconnect、shutdown/close
和 native error 转换。平台 handle 不进入 routing/rules/protocols；热路径不增加逐字节虚
调用。Windows 抽取前后必须二进制行为和 benchmark 等价，之后再接 kqueue/poll 可验证的
POSIX 实现。

POSIX 测试覆盖 `SO_REUSEADDR`/独占语义差异、SIGPIPE 抑制、EINTR、partial send/receive、
半包 stop、客户端断开、端口冲突、IPv4 loopback 和文件描述符回收。

### 13.3 system libcurl Transport

新增 macOS `CurlTransport`，使用 Xcode SDK 自带 header 并链接系统 `libcurl`，不查找、
复制或运行时加载 Homebrew/MacPorts curl。发布验收使用 `otool -L` 确认依赖来自系统。

实现覆盖普通响应、SSE callback、取消、DNS/connect/send/header/idle/total timeout、
request/response limit、partial callback、status/reason/header 透明转发和统一错误分类。
每个并发请求独占 easy handle；transport 使用上限受 worker 数约束的可复用 handle pool，
保留连接缓存但不让 SSE 独占导致 Usage 无 handle 可用。实际使用每 slot 持久 multi poll
实现亚秒取消/timeout；worker、FIFO admission 和同步请求编排保持不变。

不显式设置代理时让 libcurl 使用进程继承的 `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY`
和 `NO_PROXY`。不读取、激活或同步 macOS System Settings 代理。Terminal 启动的 CLI/app
继承该 Terminal 环境；Finder 或登录项启动通常没有 shell proxy 变量，因此默认直连，
这是预期行为，不做隐藏补偿。

### 13.4 macOS CLI

先让现有 `ccs-trans` CLI 在 macOS 编译和运行，保持相同 config schema、命令、exact route、
日志和错误 envelope。配置根使用当前用户 home 下的 `~/.ccs-trans/`，目录权限保持仅用户
可访问。用同一 mock upstream fixture 验证 Responses/Chat/Messages、Usage、SSE、Rules、
reload、取消、limits 和代理环境矩阵。

### 13.5 AppKit 菜单栏宿主

新增 Objective-C++ `.app` host，使用 `NSApplication`、`NSStatusItem` 与原生 `NSMenu`，复用
阶段 12 的 ApplicationController 和同一命令集合。AppKit 对象只存在于主线程；服务命令
在 control executor 执行，结果派发回 main queue。

菜单内容与 Windows 一致，将“开机自启”显示为“登录时启动”。图标从 512px 母版派生为
template PNG，验证浅色/深色、Retina 和辅助功能尺寸；Finder app icon 另行生成 ICNS，
不能把 template icon 当作 app icon。

### 13.6 登录项、签名与发布

使用 `SMAppService.mainAppService` 读取真实状态并 register/unregister；菜单不缓存猜测。
拒绝、需要用户批准和系统错误都必须可见并记录。登录项只启动 `.app` menu host，不启动
第二份 CLI 服务。

阶段 13 使用同一基础版本 `0.5.0`，发行标识为 `0.5.0-macOS-arm64`。先生成 arm64 ZIP，
文件名为 `ccs-trans-0.5.0-macOS-arm64.zip`，内容为 ad-hoc 签名 `.app`、ad-hoc 签名 CLI、
README、docs、licenses 和 checksum。签名固定使用 hardened runtime、无 timestamp 的
`codesign --sign -`；不读取签名身份或公证凭据，不执行 notarization、staple 或 `spctl`。
DMG 是可选发布外壳，不阻塞首个 ZIP。下载后被 quarantine 的包可能需要用户显式批准运行，
发布记录不得把它描述为 Developer ID、notarized 或 Gatekeeper trusted。

共享 fixture、CLI/menu smoke 和包内容审计已通过。长期运行、登录项/Finder/Terminal 环境
对比与干净账户候选 smoke 尚未执行；阶段 14 负载、2 小时 mixed soak 和 8 小时 idle 本轮
明确不执行。

## 阶段 14：跨平台加固与发布纪律

1. 运行 Windows/macOS 8-16 SSE 桌面负载、50 SSE 压力、mixed Usage 与 Rule benchmark；
2. 分别执行 8 小时 idle、2 小时 mixed soak，记录 working set/RSS、handle/fd、线程、logger
   queue、连接复用和失败计数趋势；
3. 验证睡眠/唤醒、网络切换、代理变化、Explorer/Finder 重启、注销、系统关机和磁盘写满；
4. 建立 Windows 11 x64 与 macOS 26 arm64 的 clean-machine release matrix；
5. 固定版本号、配置 schema、最低系统、签名策略、checksum 和回滚说明模板；
6. 每个平台从发布 archive 而不是 build tree 完成一次端到端 smoke；
7. 复核 Windows 已接受的 notification-area 初始注册窗口和 session-end 日志限制，按当时
   风险决定修复或继续保留，不把它们混入 macOS listener/transport 首次接入。

内存、线程、handle/fd 或 logger queue 随累计请求持续增长即视为失败。平台差异必须留在
adapter/host 内；不得用 Provider 特判或复制 Server 来修复平台问题。

## 性能与架构取舍

- 保留同步 listener + 有界 FIFO worker。只有常规负载持续 queue wait、Usage 饥饿、50 路
  资源失控，或平台 transport 无法满足取消/timeout 合约时才评估异步重写。
- Rule pipeline 保持 empty 零 parse、non-empty 最多一次 parse/serialize；宿主状态展示不
  读取或复制请求 body。
- UI/control executor 与请求 worker 完全分离。状态刷新使用事件或低频 timer，不进入
  request logger，不让任务栏操作影响 TTFB。
- POSIX socket 抽取保持批量 buffer 和现有 admission 边界，不为了“跨平台”增加每次
  send/receive 的堆分配或通用动态多态链。
- WinHTTP session 与 curl easy handle 都按 generation/并发边界复用。连接池有上限，配置
  reload 后旧 generation 随 in-flight 请求自然释放。
- 日志继续约 100 ms 批写、error 立即 flush、SSE 增量 chunk，不为 UI、重试或发布诊断
  保留第二份完整 response body。

## 暂不进入范围

- GUI 内完整 JSON/Profile/Rule 编辑器；
- Provider/API key/模型管理；
- 自动故障转移或负载均衡；
- response transformation pipeline；
- CLI 到 tray 的远程控制协议或 HTTP 管理接口；
- 自动更新器；
- Intel `x86_64` 与 Universal 2 构建；
- 没有 benchmark 依据的全异步网络栈。
