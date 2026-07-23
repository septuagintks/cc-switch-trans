# ccs-trans 后续开发计划

## 当前基线

`0.7.0` 是当前双平台基线，发行标识统一为：

```text
0.7.0-Windows-x64
0.7.0-macOS-arm64
```

`0.7.0` 已完成进程级 inflight budget、SQLite 3.53.3/schema v1、`ccs-trans.config/v3`、固定
`profiles.db`、组合恢复 journal、显式 v2 migration、typed command、canonical Rule 文本，以及
Win32/AppKit Profiles、Rules、Settings 三视图。请求热路径仍只读取 immutable generation，不访问
JSON 文件、SQLite、GUI 或 descriptor。

实现、双平台验证、发行物策略和明确未执行项统一归档在
[Release-0.7.0.md](Archived/Release-0.7.0.md)。完整实施取舍保留在
[Planning-0.7.0.md](Archived/Planning-0.7.0.md)，不再作为当前待办来源。历史 `0.5.0/0.6.0` 文档
保持只读。

`0.8-A` 已完成：Qt `6.10.3`/官方 MinGW `13.1` 独立 GUI、GCC 16 runtime 双工具链、typed model/动画
稳定性原型、固定 Qt staging、资源预算和安装/卸载技术原型均已通过。项目已明确限定为非商业使用，
Inno Setup 7.0.2 的 `Non-commercial use only` 路径正式冻结；若未来用途变化，必须在后续 setup 发布前
重新审计许可或切换 WiX。`0.8-B` 已完成原子 Profile Save、结构化错误字段、draft/base revision、
Rules 换行与前向兼容边界，以及可验证恢复的 `storage migrate --replace`。`0.8-C` 已完成 Windows tray
分层、共享 wire codec、`ccs-trans.gui-ipc/v1`、受限 maintenance endpoint、GUI 子进程 bootstrap/PID
绑定和 snapshot/delta bridge。`0.8-D` 已完成 Qt GUI typed client/model/controller、三页 QML skeleton、
本地 dirty edit buffer、首次 draft/revision 门控和独立进程生命周期。`0.8-E` 已完成 0.7 功能对等、应用内
错误与 migration 交互、视觉/动画人工验收、8/16 路 SSE 联调，并删除旧 Win32/GDI+ GUI 和 HWND 自动化；
生产 tray 不提供 fallback。当前开发入口为 `0.8-F`：可视化 Rule Builder、preview 和 undo/redo。

## 版本顺序

```text
0.8.0  Windows Qt Quick 独立 GUI、可视化 Rule builder、安装器与交互成熟
  -> 0.9.0  按真实使用补齐能力、修复问题、提高健壮性
  -> 1.0.0  冻结合同、补齐测试、发布完全版
```

每个版本都先完成共享 model/service/repository。Windows 从 `0.8.0` 起由轻量 Win32 tray/runtime 持有
业务状态和代理服务，按需启动独立 Qt Quick/QML GUI；macOS 继续使用 AppKit。GUI 不直接读写 JSON、
SQLite 或 RuntimeSnapshot；请求热路径只读取已经编译的 immutable generation，不执行文件 I/O、
SQL、GUI 查询或 Rule descriptor 枚举。

## 0.8.0：Windows GUI 重构、安装器与 Rule Builder

详细架构、风险、测试矩阵和 `0.8-A` 至 `0.8-I` 构建顺序见
[Planning-0.8.0.md](Planning-0.8.0.md)。本版本冻结以下主线：

1. 新建按需启动的 `ccs-trans-gui.exe`，使用 Qt Quick/QML；代理 runtime、repository、tray 和开机启动
   管理继续留在 `ccs-trans-tray.exe`；
2. 两个进程通过 current-user named pipe 上的版本化 IPC 合同交换 typed snapshot、command 和 result；
   GUI 不链接或直接操作 runtime/SQLite/config；
3. 按 app、transport、model、controller、page、component、dialog、theme 分层，增加源码体量检查，禁止
   用单一窗口类或根 QML 文件承载全部状态与行为；
4. `0.8-E` 已在 Windows GUI 功能对等后删除旧 Win32/GDI+ `main_window.*`、`windows_theme.*` 和
   对应不可达测试/资源，不保留 fallback；
5. 根据 Rule descriptor 生成可视化 Rule Builder，支持增删、复制、启停、排序、预览、undo/redo，
   并与文本模式共享 canonical draft；
6. 实现 Windows per-user setup EXE 的安装、升级、回滚与卸载，同时保留 portable ZIP；卸载永不删除
   `%USERPROFILE%/.ccs-trans/`；
7. macOS 保持 AppKit 主题和布局，只同步 `Save` 文案及共享行为修正，不引入 Qt。

`0.8-A` 已冻结 GUI 进程基线：Qt model 采用 stable key 和增量 row/role 更新，不允许正常 delta 触发
model reset；批量同步每个事件循环最多 32 次 mutation 且最多占用约 2 ms UI-thread 时间，之后必须让出
事件循环。model sync 期间暂停结构和 delegate 中间动画，只保留确定最终状态，防止 revision delta、
selection 与动画队列互相放大。IPC 初始 snapshot 只含 summary 与 selected draft，之后按 revision delta
更新；frame 上限为 16 MiB。

`0.8-C` 已冻结 tray/GUI 进程边界：共享 `ccs-trans-gui-ipc` 只包含 UTF-8 DTO、严格 JSON codec、4-byte
little-endian frame、session/revision tracker，不依赖 Qt 或 runtime。Windows tray 使用 current-user DACL
named pipe；pipe 名只暴露 SID、规范化配置根目录和 instance identity 的 hash。tray 通过受限继承的匿名
pipe 向 suspended GUI 子进程发送一次性 256-bit token/session，再绑定实际 PID 后恢复进程。可靠消息进入
有界队列，普通状态只合并到最新 revision；无法排队的可靠消息会断开会话而不是静默丢失。安装器使用
独立 `ccs-trans.maintenance-ipc/v1`，只能 query version、request shutdown 和查询 release 状态，不能访问
编辑命令。`wait_for_release.timeout_ms` 的长轮询及“响应写完后再退出”协调留在 `0.8-G` 与正式安装器一起
完成；当前 endpoint 返回可重试的瞬时 `release_ready`/`draining` 状态。

`0.8-D` 已冻结 Qt sidecar 生命周期：tray 在首次 `LoadDraft` 命令结束且状态 revision 为正之前排队 Open，
之后按需启动或激活 `ccs-trans-gui.exe`；加载失败状态也可进入 GUI，由 `0.8-E` 的 migration/recovery UI
承载恢复。
GUI 只继承一次性 bootstrap handle，通过 `QLocalSocket` 接收 typed snapshot/delta；正常关闭隐藏并复用进程，
轻量关闭或隐藏后切入轻量模式会退出 GUI，post-auth 断线由下一次 Open 创建新进程/token/session。正常 delta
使用 row insert/remove/move/dataChanged，不触发 model reset；Profile/Rules 本地 dirty buffer 不被异步 snapshot
覆盖。`0.8-E` 最终验证通过 runtime `32/32`、Qt `8/8` CTest；真实双构建树联调覆盖第二实例、crash
relaunch、graceful quit、GUI 生命周期期间 8/16 路 SSE 完整性和零 GUI runtime diagnostic。旧 Windows
窗口源码与自动化已删除，只保留历史 0.7 包作外部 oracle。

`0.8.0` 为 Windows Qt GUI 冻结动效合同：所有可交互内容必须同时提供可辨识的 hover colour、hover
scale 和 pressed/click scale 状态，包括导航、按钮、列表行、菜单项、开关、输入容器、Rule 操作和
可点击状态项。缩放只作用于视觉 transform，不改变布局占位、hit target 或相邻控件位置；键盘 focus
与 touch/辅助功能路径提供等价状态，不能把 hover 作为唯一反馈。任何位置、尺寸、展开、折叠、排序或
页面切换等动态移动不得瞬移，至少使用连续线性插值；允许使用更平滑曲线，但不能低于线性动画的连续
程度。遵守 Windows animation/high-contrast/Reduce Motion 设置：减少动态时允许把 duration
降为零或最短，但最终 colour/scale/focus 状态仍必须正确。动画期间不得阻塞 UI thread、重排请求热
路径、模糊文本或造成 hover 抖动，并增加帧时间、快速指针进出、连续点击、键盘操作和资源回收测试。

实现优先级固定为 `model identity/revision 正确性 -> draft 不丢失 -> animation end state -> 帧流畅度 ->
视觉细节`。不得为了维持动画而丢弃业务 delta，也不得用整页重建、周期性 repaint 或延迟覆盖 dirty draft
换取表面稳定。60/120 Hz 帧率、idle 零持续 repaint、Reduce Motion 和 high contrast 都是 `0.8-E/I` 的
发布门槛，不是最后阶段才补的装饰项。

版本结束时，用户应能完全通过 GUI 管理 Profile、全局配置和 Rule，不需要直接编辑数据库或完整
配置文件。

## 0.9.0：能力补齐与健壮性

`0.9.0` 不预设新的大型架构主题，只处理 `0.7.0` 至 `0.8.0` 真实使用暴露的问题：

- config/database/Rule migration 的异常恢复、诊断和备份；
- GUI/CLI 并发、重复启动、service fault、reload rollback 和窗口生命周期竞态；
- 网络切换、睡眠唤醒、系统代理变化、上游慢流、客户端中断和 shutdown；
- 日志轮转、容量缩限、磁盘写满、不可写目录和崩溃尾部恢复；
- 大量 Profile/Rule 下的启动、编译、搜索、编辑和内存占用；
- Windows/macOS 交互差异、打包、签名说明和升级路径；
- 安全审计，包括路径边界、SQL 参数绑定、日志敏感信息和导入文件限制。

非必要的新协议、云同步、自动故障转移、负载均衡和全异步网络栈留到 `1.x` 评估，避免稳定期扩大
范围。

## 1.0.0：冻结与完全版

`1.0.0` 以冻结、验证和发布为主，不再引入新的核心交互或存储模型。

冻结内容：

- CLI 命令、退出码与 repository error；
- config、SQLite schema、迁移、备份和恢复合同；
- Profile/Protocol/Rule descriptor 与 GUI 编辑语义；
- 内存预算、generation 生命周期、日志轮转和故障行为；
- Windows/macOS 窗口、tray/menu、轻量模式、启动项和退出行为；
- 发行包布局、系统/架构标识和最低平台范围。

发布验证：

1. 两平台 Release/warnings clean build、全部 CTest 与 integration；
2. 五档短负载、Rule microbenchmark、并发大 body 和 generation churn；
3. 2 小时 mixed、8 小时 idle、日志多轮轮转和 GUI 长时间常驻；
4. 干净用户目录安装、`0.5.x/0.6.x/0.7.x` 数据迁移、回滚和卸载保留数据；
5. Windows 11 x64 与 macOS 26 arm64 真机 GUI、启动项、代理、睡眠唤醒和网络切换；
6. 同一最终 commit 的双平台固定白名单包、checksum、签名策略和 archive smoke；
7. README、Design、ProjectStructure、release archive、tag 与发行资产完全一致。

## 跨版本质量门槛

触及 listener、worker、transport、logger、repository、RuntimeCompiler 或 Rule pipeline 时至少运行：

- `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50`；
- 1 KiB、100 KiB、1 MiB 与 0/1/8/32 Rule microbenchmark；
- 与改动对应的取消、reload、日志、数据库、GUI 或内存专项。

固定要求：

- 常规档零请求失败，Usage 不被 SSE 饿死，logger backpressure/writer failure 为零；
- 每个成功响应校验内容、顺序、长度和结束标记，不能只看 HTTP 200 或聚合字节数；
- 空 Rule pipeline 零 parse；非空 pipeline 最多一次 parse/serialize；
- SQL、GUI、日志轮转和状态通知不得进入请求期锁竞争；
- private/RSS、handle/fd、线程、连接、generation、SQLite statement、logger queue 和 GUI 对象不得
  持续增长；
- GUI 不可见且服务配置未变化时，CPU、timer 和 wake-up 接近当前 tray/menu 基线；
- 50 路允许超过 32 worker 后的有界排队，不把压力档 p95 当作 8-16 路桌面目标。

## 构建与发布顺序

1. 冻结本版本需求、不进入范围、数据合同和可自动验证的验收条件；
2. 实现共享 model/service/repository 与单元测试；
3. 完成迁移、失败回滚和无 UI 的 CLI/integration；
4. 实现共享 command/service 合同，再按版本计划接入 Windows Qt IPC client 或 macOS AppKit；
5. 执行风险对应的协议、并发、GUI、数据库、日志、内存和性能测试；
6. 更新版本、README、Design、ProjectStructure 与 release archive，创建最终源码 commit；
7. 从该 commit 构建 Windows/macOS 包，核对固定白名单、checksum、版本和 archive smoke；
8. 把两个 ZIP SHA-256 写入签名 annotated tag 和交接记录，推送后再上传发行资产。

平台边界保持不变：Windows 使用 WinHTTP current-user system proxy，选中代理失败不回退直连；
macOS 使用 selected SDK system libcurl 和进程 proxy environment，不激活 System Settings proxy。
Qt/Win32/AppKit/SQLite/curl/WinHTTP 类型不得进入 `core/routing/rules/protocols` 公共 header。

任一版本只有在需求、实现、迁移、两平台测试、资源指标、文档、同提交发行包、tag 与明确未执行项
全部可审计时才算完成。
