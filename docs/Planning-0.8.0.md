# ccs-trans 0.8.0 初步实施计划

## 文档状态

本文冻结 `0.8.0` 的方向、边界和构建顺序，但不冻结 Qt 精确版本、Windows GUI 编译器或安装器生成器。
这些依赖决定必须在 `0.8-A` 完成官方来源、许可证、工具链和最小原型取证后再写入正式开工合同。

| 项目 | 当前决定 |
| --- | --- |
| 开发基线 | 签名 tag `0.7.0`，commit `fd66d11cf2352cdfec122ea4a97caca4d23675b4` |
| Windows GUI | Qt Quick/QML 独立进程，放弃现有 Win32/GDI+ 主窗口 |
| Windows tray/runtime | 继续使用轻量 Win32 进程，独占代理 runtime、repository 与启动项管理 |
| macOS GUI | 保留 AppKit，不引入 Qt；只接收共享行为修正和必要文案变化 |
| Windows 安装 | `0.8.0` 增加可卸载、可升级的安装器进程，同时保留 portable ZIP |
| 目标发行物 | Windows portable ZIP、Windows setup EXE、macOS arm64 ZIP |
| 数据路径 | `%USERPROFILE%/.ccs-trans/` 与 `~/.ccs-trans/` 不变，安装和卸载不得删除用户数据 |

## 目标

`0.8.0` 完成以下工作：

1. 把 Windows 主窗口迁移为按需启动的 Qt Quick/QML 独立 GUI 进程；
2. 建立 tray/runtime 与 GUI 之间可版本化、可测试的本地 IPC 合同；
3. 恢复 `0.7.0` Profiles、Rules、Settings、draft、service controls 和轻量模式功能对等；
4. 修复已确认的 Save、draft 保留、migration 提示、Rules 粘贴、错误反馈和渲染问题；
5. 实现 descriptor 驱动的可视化 Rule Builder 和成熟交互；
6. 增加 Windows 安装、升级、卸载和 Qt runtime 固定白名单验证；
7. 删除旧 `main_window.*`、`windows_theme.*` 和只服务于旧窗口的资源代码，不在最终包保留 fallback。

## 不进入范围

- macOS 不迁移到 Qt，不重新设计已经验收的 AppKit 布局与主题；
- GUI 不直接访问 SQLite、config 文件、RuntimeSnapshot、WinHTTP 或请求日志 body；
- 不在安装器中实现自动更新、在线下载、驱动、Windows service 或系统级代理修改；
- 不新增云同步、Provider 选择、API key 存储、自动故障转移或负载均衡；
- 不自行实现通用文本编辑器、渲染引擎或安装事务系统；
- 没有真实请求改写需求和测试样例时，不扩张 Rule 类型。

## 进程拓扑

```text
ccs-trans.exe
  console CLI and explicit administration

ccs-trans-tray.exe
  Win32 tray + ApplicationController + repository + proxy runtime
          |
          | current-user named pipe, ccs-trans.gui-ipc/v1
          v
ccs-trans-gui.exe
  Qt Quick/QML window + typed client models/controllers

ccs-trans-0.8.0-Windows-x64-setup.exe
  install/upgrade/uninstall only; never participates in proxy runtime
```

Qt DLL、QML module 和图形后端只由 `ccs-trans-gui.exe` 加载。用户从不打开 GUI 时，tray 的 idle 内存、
线程、timer 和启动时间不得承担 Qt 成本。GUI 崩溃或退出不能停止 listener，也不能破坏正在进行的 SSE。

### 生命周期

- tray menu、双击 tray icon 或第二桌面实例请求打开 GUI；
- tray icon 的工具提示文本固定且只包含 `ccs-trans`，不追加运行状态、端口、版本或 Profile；
- tray 只允许一个匹配 instance identity 的 GUI 进程，并把后续请求转换为 activate；
- normal mode 关闭窗口时隐藏并保留 GUI 进程；lightweight mode 关闭窗口时退出 GUI 进程；
- GUI 重启后重新 handshake 并获取完整 snapshot，不依赖前一进程内存；
- tray 退出时先通知 GUI 关闭，再 drain runtime；GUI 无响应时只终止 GUI，不跳过 runtime drain；
- GUI executable 缺失、版本不匹配或启动失败时，tray 记录结构化错误并使用应用内可见诊断，不静默失败。

## IPC 合同

Windows 使用 current-user ACL 限制的 named pipe，不开放 TCP 端口。协议初始标识为
`ccs-trans.gui-ipc/v1`，使用有长度前缀的 UTF-8 JSON frame；精确 frame 上限在 `0.8-A` 基准后冻结。

### 必需消息

| 类别 | 消息 |
| --- | --- |
| 会话 | `hello`、`hello_result`、`activate`、`shutdown`、`ping` |
| 状态 | `snapshot_request`、`snapshot`、`state_changed`、`command_status` |
| 服务 | `start`、`stop`、`reload`、`quit_application` |
| draft | `apply`、`discard`、`reload_draft` |
| Profile | `select`、`create`、`remove`、`save`、`set_enabled`、`move` |
| Rules | `replace_text`、`format_text`、可视化 Rule CRUD、排序、预览 |
| Settings | typed field update、reset optional field、UI preference update |
| migration | `storage_status`、`migrate`、`replace_confirmation_required`、`migration_result` |

每个 command 包含 session id、单调 sequence、预期 composite revision 和可选 Profile stable key。响应必须
区分 validation、stale、busy、migration required、saved pending runtime apply、runtime rollback 和 IPC
failure。GUI 不根据英文错误字符串推断行为。

snapshot 可以包含 descriptor、Profile/Rule draft、readiness、service status 和可展示诊断，不包含 API key、
完整请求/响应 body 或日志敏感内容。协议解析失败、未知版本、超限 frame 和重复 sequence 必须有确定退出或
拒绝行为，并接受 malformed/fuzz 测试。

## Windows 代码分层

目标目录按职责划分，不把新窗口重新写成单一类：

```text
src/
  gui_ipc/
    protocol_types.hpp/.cpp
    json_codec.hpp/.cpp
    frame_codec.hpp/.cpp

  hosts/windows/
    platform/
      instance_coordinator.*
      startup_registration.*
      windows_error.*
      windows_host_platform.*
    tray/
      tray_main.cpp
      tray_application.hpp/.cpp
      tray_menu.hpp/.cpp
      gui_process_launcher.hpp/.cpp
    gui_bridge/
      gui_ipc_server.hpp/.cpp
      gui_session.hpp/.cpp
      gui_command_router.hpp/.cpp
      gui_snapshot_builder.hpp/.cpp

  gui/windows/
    app/
      gui_main.cpp
      gui_application.hpp/.cpp
      window_lifecycle.hpp/.cpp
    ipc/
      gui_ipc_client.hpp/.cpp
      qt_json_codec.hpp/.cpp
    state/
      application_state_model.hpp/.cpp
      connection_state_model.hpp/.cpp
    features/
      profiles/
        profile_list_model.hpp/.cpp
        profile_editor_controller.hpp/.cpp
        ProfilesPage.qml
        components/
      rules/
        rules_model.hpp/.cpp
        rules_editor_controller.hpp/.cpp
        RulesPage.qml
        components/
      settings/
        settings_model.hpp/.cpp
        settings_controller.hpp/.cpp
        SettingsPage.qml
      migration/
        migration_controller.hpp/.cpp
        MigrationDialog.qml
    ui/
      Main.qml
      shell/
      components/
      dialogs/
      theme/

packaging/windows/
  installer/
    installer definition and fixed staging manifest
```

### 文件职责约束

- `Main.qml` 只负责根窗口和顶层 composition，不保存业务状态；
- 每个 page 只绑定 typed model/controller，不直接发送 pipe frame；
- QML JavaScript 只处理展示变换，不实现 validation、revision、migration 或 repository 逻辑；
- tray bridge 只翻译 IPC command 与共享 application/presentation API，不复制 ConfigurationEditor；
- protocol codec 不 include Qt、Win32、SQLite 或 runtime 类型；
- transport、session state 与 feature 分开；每个 feature 内再分 model、controller、QML view，禁止一个
  `QObject` 暴露整个应用的所有命令和字段，也禁止根 QML 直接持有 Profile/Rule/Settings draft；
- 手写 C++ implementation 以 600 行、QML 文件以 300 行为检查上限；新增超限文件默认使结构检查失败，
  只有职责单一且在 allowlist 记录原因时才可例外，不能通过机械拆成无语义片段规避；
- 页面、dialog、可复用 control、theme token 和动画分别归档，禁止再次形成数千行窗口文件。

现有 `src/presentation` 在 `0.8.0` 继续服务 AppKit 和共享 typed command，不为 Qt 暴露进程内对象。
Qt feature controller 只消费 IPC DTO；可跨平台复用的校验或编辑行为必须下沉到现有 config/app service，
不能分别复制进 QML 和 AppKit。

## Qt 与工具链边界

`0.8-A` 必须完成以下取证后再锁定版本：

1. 选择一个仍受支持的 Qt 6.x 版本并记录官方下载位置、archive hash、模块与许可证；
2. 确认 Qt Quick、Qt Quick Controls、Qt Test、Qt Quick Test、Network/IPC 所需模块的最小集合；
3. 比较官方 MSVC 与 MinGW Qt 工具链，记录与当前 GCC 16 core build 的关系；
4. 验证 `ccs-trans-gui.exe` 可以在独立 build directory 使用不同编译器，因为它不与 core 共享 C++ ABI；
5. 动态链接 Qt DLL，保留 LGPL notice、license 和 relink 要求；未经单独许可审计不使用静态 Qt；
6. 使用 `windeployqt --qmldir` 生成候选 staging，再由项目固定 manifest 删除偶然 plugin、debug DLL、
   translation 和未使用 QML module；不能把 `windeployqt` 输出直接当作发布白名单。

默认倾向是保留现有 GCC core/tray build，Qt GUI 使用 Qt 官方支持的独立工具链。只有双工具链让 CI、调试、
安装和符号管理明显不可控时，才评估把全部 Windows target 迁到 MSVC；不得在没有完整性能与包体证据时
顺手迁移整个网络/runtime 工具链。

若采用双工具链，根 CMake 必须提供互斥或可组合的 runtime/Qt GUI build option：一个 build tree 只生成
GCC CLI/tray/core，另一个 build tree 只生成 Qt GUI、IPC client 和对应测试。两个 build tree 不共享静态库、
C++ object、STL 类型或生成器缓存，只共享源码形式的 wire schema/codec 测试向量；Windows package script
在干净 staging directory 汇合经过各自 verifier 的产物。不得手工复制 DLL 作为日常构建步骤。

## GUI 行为修正

### Profile Save

- `Update Profile` 统一改为 `Save`；
- 删除 GUI 中独立 Rename 操作，Profile id/name 与其他字段由一次 Save command 原子提交；
- Save 同时验证唯一 id、protocol、local/upstream path、URL、route collision 和 Rule readiness；
- Save、Enable 或 runtime apply 失败时保留提交前的完整 local draft、selection、caret 和 scroll；
- 禁止失败后从 persisted snapshot 重建输入框；只有显式 Discard 或 Reload Draft 可以覆盖用户输入；
- macOS 只修改对应文案和共享 command 调用，不重构 AppKit 布局。

### migration 与替换

默认 `storage migrate` 继续拒绝覆盖已有 `profiles.db`。新增破坏性替换路径：

```text
ccs-trans storage migrate --replace
Type REPLACE to continue: REPLACE
```

非交互调用必须额外传入显式 confirmation token，精确 CLI 形式在 `0.8-B` 冻结。小写、拼写错误、stdin
非 TTY 且没有 token、备份失败或现有数据库无法验证时都拒绝替换。

替换前必须把现有 `profiles.db`、WAL/SHM 状态和来源信息保存到受管理 backup，完成 hash/可打开性验证后才
进入 durable migration transaction。任何失败保留旧数据库或可恢复 journal，不能先删除再迁移。

tray 检测到 migration required 时打开应用内 owner dialog：

1. 第一次询问是否执行 migration，按钮为 Yes/No；
2. 若已有 `profiles.db`，第二次明确显示将被替换的路径、备份位置和不可逆影响，再询问确认；
3. 取消后保持故障状态并提供再次打开入口，不循环弹窗；
4. GUI 与 CLI 调用同一个 migration service，不复制覆盖逻辑。

### Rules 编辑器

- 输入和粘贴边界统一 CRLF、LF、CR、Unicode line/paragraph separator；
- 保留 JSON 字符串内部转义，不把 `\\n` 当作实际换行；
- 修复粘贴后大空白、段落高度异常或视觉上单行的问题；
- 文本与可视化模式共享 canonical draft，切换模式不丢未知字段、顺序、selection 或 undo history；
- clipboard 测试覆盖 VS Code、浏览器、Windows Terminal、LF/CRLF、tab、Unicode 和大文本。

### 错误与反馈

- 字段 validation 失败时高亮对应 input，显示稳定 error code 对应的就地说明并聚焦首个错误；
- 非字段错误使用以主窗口为 owner 的应用内 modal/dialog，不设置 system-wide topmost；
- Windows notification 只用于后台且用户需要知晓的异步事件，不承担 Save/migration 错误；
- stale、busy、runtime apply failed 和 IPC disconnected 使用不同状态，不统一显示为未知错误。

## 视觉与交互合同

- 所有可交互项具有 hover colour、hover scale、pressed/click scale、keyboard focus 和 disabled 状态；
- scale transform 不改变 layout 占位、hit target 或相邻控件位置；
- 页面、列表、popup、drag/reorder 和滚动至少连续线性动画，可使用更平滑 easing；
- 遵守 Windows animation 和 high contrast 设置，Reduce Motion 时允许 duration 为零；
- 使用 Qt scene graph/RHI 的帧调度，不用每秒全量刷新窗口；service 状态由 IPC event/diff 推送；
- resize、Profile 切换、Rules 滚动、自动状态更新和 popup 展开不得出现白/黑 frame、旧像素行或边框碎片；
- 上下水平分界线贯穿内容区，垂直线按布局比例并与上下线保留 token 化间距，不形成突兀直角交汇；
- Profile list、子视图边框和输入错误边框在 100%/150%/200% DPI 保持一致物理视觉重量；
- 动画和 hover 不模糊文字，不通过逐帧改变 font size 实现缩放。

## 可视化 Rule Builder

Rule Builder 必须由共享 Rule descriptor 生成，不在 QML page 硬编码每种 Rule：

- 添加、删除、复制、启停、拖动排序和类型切换；
- 根据 option descriptor 生成 Boolean、Enumeration、Text、Unsigned、JSON Pointer、Value editor；
- 文本和可视化模式共同编辑一个 canonical draft；
- 提供 undo/redo、搜索、批量启停和稳定 selection；
- 提供用户显式输入且脱敏的本地样例 preview/diff，不读取生产日志；
- preview 使用与 runtime 相同的 compiler/rule pipeline 服务，但在 tray 侧隔离执行并受 body/time budget；
- 空 Rule pipeline 继续零 parse，非空 runtime pipeline 最多一次 parse/serialize。

## Windows 安装器

不手写拥有管理员权限的自定义安装事务。`0.8-A` 比较 Inno Setup 与 WiX；当前优先评估 Inno Setup，原因是
可生成单一离线 setup EXE、支持 per-user install、升级和卸载，且适合 Qt 动态文件树。若 MSI repair、
企业部署或可靠回滚证据表明 WiX 更合适，再冻结为 WiX。

### 安装合同

- 默认 per-user 安装到 `%LOCALAPPDATA%/Programs/ccs-trans/`，不要求管理员权限；
- 安装 CLI、tray、Qt GUI、最小 Qt DLL/plugin/QML module、图标、licenses 和当前用户文档；
- 安装器通过窄 maintenance command/control endpoint 请求 GUI/runtime 有序退出，等待文件 handle 释放后
  替换程序文件；安装器不复用可编辑 Profile/Settings 的完整 GUI command surface；
- 升级失败不得留下混合版本 executable/Qt DLL，必须回滚或明确保持旧版本可启动；
- 卸载只删除安装目录、快捷方式和 installer metadata，永不删除 `%USERPROFILE%/.ccs-trans/`；
- launch-at-login 的真实状态仍由应用设置管理，安装器不建立第二个互相竞争的启动项来源；
- 安装结束可选择启动 tray，但默认不静默迁移或覆盖用户数据库；
- 继续提供 portable ZIP，供调试、审计和不希望安装的用户使用。

目标文件名：

```text
ccs-trans-0.8.0-Windows-x64.zip
ccs-trans-0.8.0-Windows-x64-setup.exe
ccs-trans-0.8.0-macOS-arm64.zip
```

Windows package verifier 必须分别验证 portable staging 和安装后文件树，包括版本、source commit、Qt plugin
白名单、QML import closure、license、卸载保留数据和干净 VM smoke。没有 Authenticode 证书时继续明确记录
setup/EXE 未签名及 SmartScreen 限制，不伪造发布者身份。

## 性能与资源门槛

`0.8-A` 先测量 Qt 最小原型，再冻结绝对值。至少满足：

- GUI 未启动时 tray 不加载 Qt DLL，idle working set/thread/timer 相对 `0.7.0` 无结构性回退；
- GUI open/close 100 次后 tray handle、thread、pipe session 和 child process current 全部回到基线；
- lightweight close 后 Qt GUI 进程完全退出，不保留 renderer 或 helper process；
- 60 Hz 屏幕滚动和交互动效稳定达到 60 fps；120 Hz 环境按实际 refresh rate 提交 frame；
- idle GUI 不持续 repaint，静止 60 秒的 render frame 和 CPU 采样接近零；
- 128 Profile、8192 Rule 的 model 初始化、筛选和 selection 不阻塞 UI thread；
- GUI 启停、崩溃、IPC reconnect、Profile 保存和 Rule preview 不改变 8/16 路代理延迟与精确性；
- installer、portable ZIP、installed size、GUI cold start 和 private working set 都记录基线，超过冻结预算必须
  带证据重新决策，不能以“Qt 本来就大”为理由跳过。

## 测试矩阵

### 自动测试

- `gui_ipc` frame、JSON codec、version negotiation、ACL、sequence、stale、disconnect 和 malformed input；
- tray command router 使用 fake GUI client 验证全部 command/result，不依赖 QML；
- Qt Test 覆盖 model/controller、selection、draft、error mapping、reconnect 和 lifecycle；
- Qt Quick Test 覆盖 component state、focus、keyboard、popup、drag/reorder、animation end state 和 text fit；
- screenshot/pixel probe 覆盖默认/最小窗口、100%/150%/200% DPI、light/dark/high contrast 与长文本；
- Rules clipboard/newline matrix、visual/text canonical round-trip、undo/redo 与 preview exactness；
- migration default refusal、TTY `REPLACE`、noninteractive token、backup fault、rollback 和两级 GUI confirmation；
- 100 次 GUI process open/close/reconnect，期间运行 `desktop-16` 并校验 SSE/Usage 内容、顺序、长度和结束标记；
- installer clean install、0.7 -> 0.8 upgrade、running-process shutdown、rollback、repair/重复安装、uninstall 和
  user data preservation；
- portable ZIP 与 setup installed tree 都执行 CLI、tray、GUI、migration status 和 version smoke。

### 人工验收

- Profile/Rules/Settings 功能对等与 Save 新语义；
- resize、快速滚动、Profile 高频切换、Rules 编辑滚动和 service 状态更新无闪烁；
- hover/click/focus/disabled/Reduce Motion、60/120 Hz、DPI、主题和高对比；
- IME、Unicode、剪贴板、Tab loop、Narrator、长 URL、128 Profile 与错误 dialog ownership；
- setup 安装、升级、卸载、快捷方式、启动项状态和无管理员账户。

## 工作包与构建顺序

| 工作包 | 内容 | 退出条件 |
| --- | --- | --- |
| `0.8-A` | Qt/工具链/许可证/安装器取证，最小 GUI 与 deploy prototype，0.7 资源基线、源码体量检查 | 精确依赖、hash、工具链和预算冻结 |
| `0.8-B` | Save/rename、draft 保留、newline、migration replace 与共享测试 | 无 GUI 的 CLI/service 合同完整通过 |
| `0.8-C` | `gui-ipc/v1`、named pipe、tray 分层、launcher/session/router | fake client 可完成全部命令与重连 |
| `0.8-D` | Qt process、typed model/controller、QML shell、生命周期 | 三页 skeleton、activate/hide/destroy/reconnect passed |
| `0.8-E` | 0.7 功能对等、错误 dialog、migration UI、视觉/动画与资源测试 | Qt GUI 人工验收通过；删除旧 Win32 GUI |
| `0.8-F` | descriptor-driven Rule Builder、preview、undo/redo、更多已确认 Rule | text/visual/runtime round-trip 与性能门槛通过 |
| `0.8-G` | Qt deploy 白名单、portable ZIP、setup、upgrade/uninstall | 干净 VM 安装矩阵和数据保留通过 |
| `0.8-H` | macOS Save 文案与必要共享行为适配，不改 AppKit 主题布局 | macOS build/CTest/integration/UI 窄复验通过 |
| `0.8-I` | 双平台候选、负载/soak、文档、包、tag、release | 所有执行项、限制与 hash 可审计 |

## 阶段执行规则

- 每个工作包先建立可回退的检查点，再进入下一包；没有通过退出条件时，不把未完成工作混入下一包。
- `0.7.0` 的 Windows ZIP 和旧 Win32 窗口只作为行为、内容和性能 oracle，不作为新代码的长期依赖。
- 触及请求 runtime 的工作必须先通过 CLI/服务层测试，再接入 GUI；GUI 失败不能影响 listener、SSE、Usage
  或日志 writer。
- GUI 交互测试区分三层：无 Qt 的 command/service 单测、Qt model/controller 单测、真实 GUI 进程和安装包
  集成测试。任何一层都不能用截图测试替代业务合同测试。
- 每个阶段结束时记录：实现范围、未完成项、测试命令、资源指标、已知限制和下一阶段输入；正式包只从
  `0.8-I` 的最终干净 commit 构建。

默认依赖链为 `A -> B -> C -> D -> E -> F -> G -> H -> I`。`0.8-A` 的技术原型和 `0.8-B` 的无 UI 合同
可以分别验证，但 Qt 页面、正式安装包和旧 GUI 删除都必须等待前置阶段退出；任何阶段都不得以临时 fallback
绕过前置合同。

## `0.8-A`：技术取证与构建基线

目标是先证明 Qt sidecar、双工具链和安装器可以被可靠构建、部署和卸载，不开始批量实现页面。

1. 从签名 `0.7.0` 检查点建立 Windows Release、warnings-as-errors 和 macOS arm64 Release 基线，记录
   CLI/tray 冷启动、tray idle working set、线程/句柄、现有 ZIP 大小、GUI 100 次开关循环和 `desktop-16`
   响应内容正确性。
2. 比较仍受支持的 Qt 6.x 版本，冻结官方下载来源、archive hash、Core/Gui/Qml/Quick/QuickControls2/
   Network/Test/QuickTest 模块、部署许可和 LGPL notice；Qt 二进制不提交到仓库。
3. 用最小 `ccs-trans-gui.exe` 验证窗口创建、QML module 加载、DPI、键盘 focus、关闭/重启，以及 Qt
   scene graph/RHI 在目标 Windows 11 环境的可用后端。记录自动 RHI 与软件渲染的表现，不在没有证据时
   强制某一图形后端。
4. 对比 Qt 官方支持的 MSVC/MinGW 组合。若保留双工具链，建立 `windows-runtime-*` 和
   `windows-qt-gui-*` 两个独立 CMake build tree、preset、warnings 和 test 入口；确认 staging 阶段才
   合并文件，不能链接跨编译器的 `ccs-trans-core` 静态库。
5. 用 `windeployqt --qmldir` 生成候选目录，写出固定 Qt DLL、platform plugin、QML import 和 license
   manifest；验证删除 debug、无关 translation、未引用 module 后 GUI 仍可启动。
6. 比较 Inno Setup 与 WiX 的 per-user 安装、升级、卸载、回滚、文件占用处理和 license 能力，优先做
   Inno Setup 离线 setup 原型。冻结默认安装目录、开始菜单快捷方式、是否把 CLI 加入当前用户 PATH、
   setup 是否提供安装后启动 tray 等用户可见选项。
7. 建立 GUI 结构检查入口，检查手写 C++/QML 文件体量、禁止层间 include、禁止 QML 访问 repository/pipe
   frame 和禁止旧 GUI 源码进入 Qt target；检查结果作为后续工作包的必需输出。

**`0.8-A` 产物与退出条件**：依赖/工具链/许可证记录、最小 Qt GUI、Qt deploy staging、安装器原型、
资源基线、CMake presets 和结构检查均可在干净目录复现；Qt 精确版本、编译器、安装器生成器、GUI 资源预算
和安装 PATH 策略已经冻结。若最小 GUI 无法在目标机器稳定启动，停止在本阶段，不进入页面开发。

## `0.8-B`：共享编辑语义与迁移合同

本阶段只改平台无关的 config/app/presentation/service，不引入 Qt，不改变请求热路径。

1. 保留现有两段式编辑模型：本地控件的未提交内容先通过 `Save` 写入 tray 侧 draft，`Apply changes` 才
   执行 validate、repository commit 和 runtime reload/restart。GUI 文案将 `Update Profile` 统一为 `Save`。
2. 从 Qt GUI command surface 中删除独立的 `RenameProfile`；Profile id/name 和其他字段由一次 Profile
   Save 原子更新。现有共享 `MainWindowCommand` 和 AppKit 在 `0.8-B` 暂时保留内部映射，直到 `0.8-H`
   完成 macOS Save 适配后再删除；CLI 的 profile 管理命令是否保留独立 rename 不在 GUI IPC 合同中复用，
   避免两个语义混在一起。
3. 为 Save、SetProfileEnabled、Rules 保存、Apply、Discard、Reload Draft 定义稳定 error code、field key、
   draft revision 和 recovery action。任何 validation、stale、busy、persistence 或 runtime apply 失败都
   必须保留调用方的 draft、selection、caret 和 scroll，不用 persisted snapshot 回填输入框。
4. 明确外部写入冲突：command 携带 base composite revision/source token；冲突返回 `RepositoryStale`，
   GUI 只能选择继续编辑或显式 Reload Draft，不能静默覆盖 CLI/其他实例写入。
5. 完成 `storage migrate --replace` 的精确确认、TTY/non-TTY 行为、已有 `profiles.db` 的备份、WAL/SHM
   处理、hash/可打开性验证和失败回滚；CLI 与 tray 使用同一个 migration service。
6. 统一 Rules 文本换行边界，覆盖 CR/LF/CRLF、Unicode paragraph separator、JSON 字符串中的转义换行、
   tab、Unicode 和大文本；canonical text round-trip 不改变未知字段和规则顺序。
7. 增加纯 C++ 单测和 CLI/integration：Save/rename 原子性、失败保留 draft、enable 失败、stale Apply、
   migration 两次确认、备份故障/磁盘故障/回滚、Rules clipboard fixture 和 runtime 内容不变。

**`0.8-B` 退出条件**：不启动 GUI 也能验证所有编辑、迁移、错误和回滚合同；现有 Windows/macOS host
   仍能编译，`desktop-16`、Usage 和 SSE 回归通过。此阶段不接受通过字符串比对英文错误来判断行为。

## `0.8-C`：Tray 分层与 GUI IPC

先把 Windows tray 从旧窗口实现中拆出，再建立可替换的 GUI transport。

1. 从 `tray_app.cpp` 抽出 tray lifecycle、菜单构建、GUI launcher、instance coordinator、startup
   registration、error presentation 和 runtime shutdown；每个类只拥有一个资源/职责，保留旧窗口适配器
   直到 Qt 功能对等完成。
2. 实现 `gui_ipc/v1` 的 wire envelope：协议标识、message kind、request id、session id、sequence、
   base revision、payload、result/error code 和 source commit。统一 UTF-8、长度前缀、最大 frame、未知
   字段、未知版本和 malformed frame 行为。
3. tray 在启动 GUI 前创建 current-user ACL named pipe、生成 instance identity/一次性 handshake
   information，再启动 `ccs-trans-gui.exe`。校验连接进程身份、版本和 session；同用户第二个 GUI 只请求
   已有会话 activate，不创建第二个 editor。
4. 将 `MainWindowState`/descriptor/command result 映射为 snapshot 和增量 state event。snapshot 只在
   handshake、reconnect 或显式 request 时完整发送；正常状态通知可合并到最新 revision，不能让每秒状态
   刷新触发 GUI 全量重建。
5. GUI 与 tray 的读写都使用有界队列和独立 I/O；不能在 tray runtime executor、Qt UI thread 或 pipe
   callback 中同步等待对方。断线、半 frame、超时、背压、GUI crash 和 tray restart 必须可恢复或明确
   显示 disconnected。
6. 单独定义供 setup 使用的窄 maintenance control endpoint，只允许查询版本、请求有序退出和等待 handle
   释放，不暴露 Profile/Rule/Settings 编辑命令，也不把 installer 当作 GUI client。
7. 先用 fake GUI client/server 完成协议测试，再连接旧 Win32 窗口做行为 oracle；增加 ACL、PID/session、
   sequence/revision、重连、超限、fuzz、断电式半 frame 和 100 次 connect/disconnect 测试。

**`0.8-C` 退出条件**：tray 在无 Qt DLL 加载的情况下正常启动和承载代理；fake client 可以完成所有命令
和结果；GUI 进程反复崩溃/重连不影响 listener、SSE、Usage、日志和 runtime generation；旧窗口仍可作为
临时验证对象，但生产包尚不切换。

## `0.8-D`：Qt GUI 骨架与生命周期

1. 新增 `ccs-trans-gui` target、QML module、资源注册、Qt Test/Quick Test target 和独立 GUI build
   preset；GUI 只链接 `gui_ipc` wire codec、Qt 和必要的 GUI 基础设施，不链接 `ccs-trans-core`。
2. 实现 `gui_main`、application lifecycle、pipe client、handshake、session state、snapshot revision
   和 reconnect。GUI 启动、激活、隐藏、销毁、第二实例和轻量模式先用空页面验证。
3. 建立 `Main.qml` shell、顶栏、tab navigation、窗口尺寸/最小尺寸、theme tokens、通用 Button/Input/
   Toggle/Popup/Dialog、focus/keyboard traversal 和 Reduce Motion 入口；根 QML 不持有业务 draft。
4. 以 Profiles、Rules、Settings、Migration 四个 feature 为边界建立 typed model/controller。model 只
   更新受影响的 row/field，controller 只发 typed command，不允许页面拼 JSON 或访问 pipe frame。
5. 先接入 snapshot 显示、selection、service status、draft phase、command status 和错误映射，再接 Save/
   Apply/Discard/Reload Draft；输入控件必须维护本地 edit buffer，异步 snapshot 不能覆盖 dirty control。
6. tray 以 Qt GUI 为默认开发路径，旧 Win32 window 从生产 `ccs-trans-tray` target 移出，只保留独立
   legacy oracle target 或历史 0.7 包用于比对。此时不提供运行时 fallback。
7. 做 process lifecycle、Qt model/controller、IPC reconnect、focus、长文本、DPI 和最小/默认窗口测试；
   先实现结构稳定，再进入 0.8-E 的视觉细节。

**`0.8-D` 退出条件**：三页 skeleton 可由 tray 打开并完成 handshake、activate、hide、normal close、
lightweight destroy、reconnect 和 graceful quit；GUI 不可见时不持续 repaint；结构检查无新增超限文件。

## `0.8-E`：功能对等与旧 GUI 删除

按 Profiles、Rules、Settings、服务控制、migration 的顺序完成 0.7 功能对等，不并行扩张新 Rule 类型。

1. Profiles：稳定 key 列表、创建/删除/排序/选择、字段 descriptor、Enable、Save（含 rename）、Apply、
   Discard、Reload Draft、stale 保留和 dirty close。
2. Rules：文本编辑、格式化、换行/粘贴修复、规则摘要、启停状态、保存到 draft、Apply 和错误定位；可视化
   Rule Builder 留给 `0.8-F`，本阶段只提供稳定文本合同。
3. Settings：typed input、布尔/枚举/数值约束、应用级 field error、不可用状态、UI preference 和轻量模式。
4. 服务与窗口：Start/Stop/Reload/退出、运行状态、SavedPendingRuntimeApply、GUI 崩溃不影响服务、tray
   菜单打开/关闭/轻量模式、第二实例 activate、固定 `ccs-trans` tray tooltip 和应用内 owner dialog。
5. 将旧窗口中已确认的视觉修复转成 Qt 原生状态：无白/黑 frame 和残留像素、无整页刷新闪烁、popup/scroll
   稳定、输入文字居中、圆角/抗锯齿、hover colour/scale、pressed scale、keyboard focus、DPI/高对比和
   Reduce Motion。所有动画只改变视觉层，不改变布局占位或请求线程。
6. 将现有 `run_tray_integration.py` 中依赖 HWND/control id 的部分迁移为 GUI IPC、Qt Quick Test、必要的
   Windows UI Automation 或 test-only automation；生产程序不得暴露测试后门。保留真实 8/16 路 proxy
   content/ordering/length/end-marker 校验。
7. 通过 Qt GUI 功能、键盘、IME、辅助功能、资源生命周期、并发和人工验收后，从 CMake、resource、
   include、package script、tests 中删除 `main_window.*`、`windows_theme.*`、GDI+/owner-draw 依赖；
   再跑一次全量 Windows core/tray/GUI 测试确认没有不可达旧路径。

**`0.8-E` 退出条件**：Qt GUI 完成 0.7 功能对等且通过人工验收，旧 Win32 GUI 已删除，最终 Windows
tray 不再链接 GDI+ GUI 资源；GUI 生命周期和 UI 问题不以“重启窗口”规避。

## `0.8-F`：可视化 Rule Builder

1. 将现有 Rule descriptor 扩展为可被 Qt 消费的稳定 schema：类型、显示名 key、字段类型、范围、枚举、
   默认值、验证和 preview 支持能力；未知类型保持文本模式可编辑，不被可视化模式静默丢弃。
2. 实现虚拟化 Rule list 和稳定 key：添加、删除、复制、启停、类型切换、拖动排序、搜索、批量启停和
   selection；8192 Rule 的筛选/选择不能阻塞 UI thread。
3. 文本模式与可视化模式共享 canonical draft 和 undo/redo。优先使用 Qt `QUndoStack` 或等价成熟组件，
   命令必须可合并、可撤销、可重做，切换模式不改变未知字段、顺序、换行和 selection。
4. 根据 descriptor 生成 Boolean、Enumeration、Text、Unsigned、JSON Pointer、JSON value editor；
   不在 QML 页面为每个 Rule 类型复制一套 validation。
5. 实现本地脱敏样例 preview/diff：样例由用户显式提供，preview 请求 tray 使用同一 Rule compiler，受
   body/time budget 限制，不读取生产日志、不触发真实上游请求、不改变 runtime generation。
6. 增加 text/visual/runtime round-trip、unknown field、排序、undo/redo、preview exactness、长文本、
   大列表、剪贴板和取消 preview 测试；验证空 Rule pipeline 仍零 parse，非空 pipeline 仍最多一次
   parse/serialize。

**`0.8-F` 退出条件**：已确认的 Rule 类型可以在文本、可视化和 runtime 三个方向无损往返；未确认的
类型仍安全可编辑；preview 与真实 compiler 结果一致，性能门槛和 UI 线程约束通过。

## `0.8-G`：Windows 部署与安装器

1. 固定两个 build tree 的 staging 输入，先分别验证 `ccs-trans.exe`/tray/runtime 与 `ccs-trans-gui.exe`
   的版本、source commit、架构和 warnings，再在全新 staging 目录合并；禁止从开发目录直接压包。
2. 生成 portable ZIP：CLI、tray、GUI、Qt DLL/plugin/QML import、图标、README、当前文档、第三方 licenses
   和 SHA256 manifest。用户配置、日志、数据库、PDB、CMake/Ninja 文件和临时资源一律排除。
3. 实现 per-user setup EXE：安装到 `%LOCALAPPDATA%/Programs/ccs-trans/`，创建明确命名的开始菜单/桌面
   入口，按 `0.8-A` 冻结的选项处理 PATH 和安装后启动；不要求管理员权限，不在安装时迁移或覆盖数据库。
4. 升级流程先通过 maintenance endpoint 请求 tray drain GUI/runtime，等待文件句柄释放，再以临时目录和
   可恢复替换完成升级。失败时不得留下混合版本 DLL/EXE；回滚后旧 tray、GUI 和配置必须仍可启动。
5. 卸载只删除安装目录、快捷方式、安装器 metadata 和本安装版本拥有的 PATH 项，永不删除
   `%USERPROFILE%/.ccs-trans/`、日志、profiles.db、备份或用户手工文件。
6. 分别验证 portable 与 installed tree：启动、版本、CLI、tray、GUI、migration status、Start/Stop/
   Reload、安装/升级/重复安装/卸载、无管理员账户、正在运行时安装和用户数据保留。
7. 若没有 Authenticode 证书，包内和发布说明明确标记 setup/EXE 未签名及 SmartScreen 限制；不把未签名
   包伪装成可信发行物。

**`0.8-G` 退出条件**：干净 Windows 11 VM 完成 clean install、0.7 -> 0.8 upgrade、失败回滚、重复安装、
uninstall 和数据保留；portable/setup 的实际文件树都通过固定白名单和 GUI smoke。

## `0.8-H`：macOS 窄适配

macOS 不迁移 Qt、不改 AppKit 主题、布局、滚动或交互设计，只做共享语义和文案所必需的变更：

1. 将 Rename/Update Profile 的对应可见文案统一为 `Save`，保持 AppKit 原生按钮、输入、弹窗和布局；
2. 接入 `0.8-B` 已冻结的 Save/error/revision 共享行为，失败保留 local draft，不复制 Windows IPC；
3. 仅在共享 enum/descriptor 变化导致编译或测试失败时做最小适配，禁止顺手重构 `main_window.mm`；
4. 在 macOS 26 arm64 上执行 CMake/CTest、menu integration、Save/stale/dirty-close、`desktop-16` 和
   资源生命周期窄复验；结果通过交接文档返回 Windows 主开发侧。

**`0.8-H` 退出条件**：macOS 原生主题和已验收布局无回归，文案与共享 Save 行为一致，构建、集成和
代理内容校验通过；没有引入 Qt 或额外架构分支。

## `0.8-I`：候选、长测与发布收尾

1. 从同一个最终干净 commit 构建 Windows portable ZIP、Windows setup EXE 和 macOS arm64 ZIP，核对
   版本名 `0.8.0-Windows-x64`/`0.8.0-macOS-arm64`、source commit、包内文档、license 和 checksum。
2. 执行 Release/warnings-as-errors、全量 CTest、协议 fuzz、GUI process 100 次启动/退出、DPI/高对比/
   Reduce Motion、IME/Unicode/clipboard、accessibility、tray tooltip 精确文本和 setup VM 矩阵。
3. 执行 `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50`，期间校验 SSE/Usage 的内容、顺序、
   长度和结束标记；GUI 崩溃、重连和安装关停不能改变 proxy 结果。
4. 在最终候选包上完成 2 小时 mixed 和 8 小时 GUI idle/服务 idle，采集 private/RSS、线程、句柄、pipe
   session、logger queue、CPU/wake-up、包体、冷启动和日志轮转指标；超预算的指标必须记录决策，不隐藏。
5. 更新 README、Design、ProjectStructure、DevelopmentPlan、Release-0.8.0 和平台验证结果；旧计划只读
   归档，当前文档只保留最新状态和明确限制。
6. 先提交源码和文档，再从该 commit 构建包并写入 checksum；完成签名 annotated tag、主仓库 push、
   release asset 上传和 `.codex/HandOff.md` 收尾。包 hash 不回写到已构建源码 commit，避免形成循环。

**`0.8-I` 退出条件**：所有工作包退出条件、测试结果、限制、包路径、hash 和交接信息可审计；未执行的
Defender/SmartScreen、平台兼容性或人工项目明确列为限制，`0.8.0` 才可发布。

旧 Win32 GUI 在 `0.8-D` 前只作为行为参照和回归 oracle，不接受新的视觉补丁。`0.8-E` 只有在 Qt GUI
功能、键盘、辅助功能、资源和并发测试通过后才删除旧文件；最终源码、portable ZIP 和 setup 中均不得存在
旧 GUI fallback 或不可达 GDI+ theme 代码。

## 开工前未决决策

以下项目不阻塞本文成立，但必须在 `0.8-A` 结束前冻结：

1. Qt 精确版本、下载 hash、模块和开源许可证履行方式；
2. Qt GUI 使用官方 MSVC 还是 MinGW toolchain，以及双工具链 CI 入口；
3. IPC frame 上限、snapshot 增量策略和 named pipe identity 格式；
4. Inno Setup 或 WiX，setup 是否需要未来 Authenticode 接口预留；
5. Qt GUI 冷启动、working set、portable ZIP、setup 和 installed size 的最终预算；
6. `0.8-F` 新增 Rule 类型的真实请求样例与明确语义；
7. 2 小时 mixed 与 8 小时 GUI idle 在 Windows/macOS 候选上的具体执行环境。

在这些决定冻结前，可以实现与 UI 框架无关的 `0.8-B` 单元测试和失败优先合同，但不能开始批量 QML 页面、
提交 Qt 二进制依赖或生成正式安装包。
