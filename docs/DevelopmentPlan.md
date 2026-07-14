# ccs-trans 后续开发计划

## 当前基线

`0.5.0` 已于 2026-07-15 完成 Windows 11 x64 与 macOS 26 arm64 双平台发布收尾，后续不再以
阶段 11-14 的编号继续追加工作。发行结论、资产来源、验证范围和已接受限制见
[Release-0.5.0.md](Archived/Release-0.5.0.md)。平台证据分别归档在：

- [WindowsValidationCheckResult.md](Archived/WindowsValidationCheckResult.md)；
- [MacOSValidationCheckResult.md](Archived/MacOSValidationCheckResult.md)。

当前生产合同是：C++20、`ccs-trans.config/v2`、单 listener、多 Profile、精确 RouteTable、
有界 FIFO worker、按 generation 热更新、异步有界日志、Windows WinHTTP system proxy 和
macOS SDK system libcurl process proxy。后续版本必须在明确需求推动下修改这些合同，不为
“顺便清理”重写已经验证的请求热路径。

下一版本号和产品范围尚未冻结。开始新版本前，先由用户确认目标，再修改 CMake、CLI、资源、
bundle 和包名中的版本；在此之前仓库继续报告 `0.5.0`。

## 后续版本构建顺序

### 1. 冻结版本范围

1. 写明用户问题、目标平台、协议/Profile/Rule 影响和不进入范围；
2. 决定版本号以及是否修改 config schema、CLI 或发布兼容性；
3. 把可自动验证的验收条件写入本文件或独立清单；
4. 确认 Windows 与 macOS 哪一侧主开发，跨设备开发时更新 `.codex/HandOff.md`；
5. 先建立基线测试结果，再开始实现。

同一版本不得在平台间使用不同数字版本。系统和架构只进入发行标识，例如
`<version>-Windows-x64` 与 `<version>-macOS-arm64`。

### 2. 先改共享模型

按以下依赖方向实现，避免在 host 或 transport 中复制业务逻辑：

```text
ConfigDocument / CLI
  -> RuntimeCompiler / RuntimeSnapshot
  -> ProtocolRegistry / RuleRegistry / RouteTable
  -> Server request orchestration
  -> Windows/macOS adapter
  -> tray/menu host
```

配置候选必须完整编译成功后才原子发布。in-flight 请求继续持有旧 generation；失败候选不得
影响当前 listener、route、transport 或 logger。平台类型不能进入
`core/routing/rules/protocols` 公共 header。

### 3. 补齐平台实现

Windows 继续使用 WinHTTP 与当前用户系统代理；选中代理失败后不回退直连，不支持代理密码。
macOS 继续只链接 selected SDK 的 system libcurl，只读取进程 proxy environment，不激活
System Settings proxy。平台差异留在 `src/server/platform`、`src/transport/<platform>` 和
`src/hosts/<platform>`。

GUI host 只复用 ApplicationController、AppService 和 control executor。不得复制 config
加载、RuntimeCompiler、Server 或 logger 初始化。AppKit 对象留在主线程；Win32 tray 和
AppKit menu 操作不得占用请求 worker。

### 4. 自动验证

每次共享请求路径变化至少执行：

```text
Windows Release clean build + CTest
Windows warnings-as-errors clean build + CTest
Windows shared protocol integration
Windows system-proxy integration（仅在明确允许临时修改并确认恢复时）

macOS Release clean build + CTest
macOS warnings-as-errors clean build + CTest
macOS shared protocol integration
macOS process-proxy integration
```

host、startup/login item、listener 或 transport 变化还要执行对应真机集成。测试状态必须区分
`passed`、`failed`、`not run` 和 `accepted limitation`，不能用另一平台结果替代。

### 5. 性能门槛

默认保持同步 listener + 有界 FIFO worker，不预设异步重写。触及 listener、worker、transport、
logger 或 Rule pipeline 时，运行以下固定矩阵：

- `smoke`：快速发现协议和采样器错误；
- `desktop-8`、`desktop-16`：桌面常规 SSE 负载；
- `mixed-16`：Responses/Chat SSE 与两组 Usage 并行；
- `stress-50`：确认 32 worker 下排队、连接和资源保持有界；
- 1 KiB、100 KiB、1 MiB，0/1/8/32 Rule microbenchmark。

常规档必须零请求失败、Usage 不被 SSE 饿死、logger backpressure/writer failure 为零。50 路
允许超过 worker 上限后的有界排队，不把压力档 p95 当作 8-16 路目标。空 Rule pipeline 必须
保持零 JSON parse；非空 pipeline 最多一次 parse/serialize；SSE 日志不得聚合完整 body。

长时间测试按改动风险决定：网络、worker、日志或宿主生命周期变化时恢复 2 小时 mixed、8 小时
idle；纯文档、CLI 展示或不进入热路径的窄改动不机械重跑。持续增长的内存、handle/fd、线程、
连接或 logger queue 一律视为失败。

### 6. 打包与发布

1. 从同一最终源码 commit 构建两个平台的 Release 包；
2. 从空临时目录运行固定白名单和内部 checksum 验证；
3. 从 archive 而不是 build tree 完成 CLI 和桌面宿主 smoke；
4. 记录系统、架构、工具链、签名策略、commit 与 SHA-256；
5. 两个平台资产准备完成后再创建一个签名 annotated tag；
6. 标签必须指向生成两个发行包的最终源码提交，不再复用或移动旧 tag；
7. 发布后通过远端 API 核对资产名称、大小和 digest，再归档结果。

Windows 包名固定使用 `Windows-x64` 大小写；macOS 使用 `macOS-arm64`。发布包继续禁止 config、
state、logs、真实 body、凭据、benchmark 原始结果、PDB/dSYM 和临时构建目录。

## 0.5.0 延后项

以下项目已明确不阻塞 `0.5.0`，只在后续需求或相关改动触发时重新排期：

1. macOS 完整 Xcode.app 工具链复核；
2. Finder 双击、真实 `SMAppService` register/unregister、登录环境、浅/深色、Retina 与辅助功能
   手工验收；
3. macOS resolve/connect/send 三类独立确定性 timeout fixture；
4. macOS 2 小时 mixed、8 小时 idle、睡眠唤醒、网络切换、Finder 重启、注销、关机、磁盘写满
   与 clean-machine matrix；
5. Windows Defender、SmartScreen 和 Authenticode；
6. macOS Developer ID、notarization、staple 与 Gatekeeper trust；
7. Windows Explorer 初始化期 tray 注册重试窗口和会话结束日志尾限制；
8. 在没有已运行安装版 tray 的独立会话中，对最终源码重建包再执行一次 GUI 自动化。

这些条目不是默认承诺。决定处理时应进入对应后续版本范围，并重新定义验收条件。

## 待确定方向

下一版本尚未选择产品主题。开始前应从真实使用问题中选定一个主要方向，例如新 Rule、协议能力、
配置/管理体验、发行可信度或运行诊断；不要同时铺开多个互不相关的大改动。未经 benchmark 证明，
不进入全异步网络栈、自动故障转移、负载均衡或完整 GUI 配置编辑器。

## 完成定义

后续版本只有同时满足以下条件才结束：

- 需求和不进入范围与实现一致；
- 两个平台适用的 clean build、CTest、integration 和风险驱动测试完成；
- README、Design、DevelopmentPlan、ProjectStructure 与实际命令/目录一致；
- 两个平台包来自同一最终 commit，固定白名单与 archive smoke 通过；
- tag、Release 资产、SHA-256 和签名策略可审计；
- 工作区干净，提交和 tag 已推送，未执行项及接受理由已归档。
