# ccs-trans 0.8.0 初步实施计划

## 文档状态

本文冻结 `0.8.0` 的方向、边界和构建顺序。`0.8-A` 已完成 Qt、双工具链、最小 GUI、部署、资源与
安装器技术原型取证。项目明确限定为非商业使用，因此 Qt、编译器和标注 `Non-commercial use only` 的
Inno Setup 7.0.2 均已冻结；若项目用途变化，必须重新审计安装器许可或切换 WiX。

| 项目 | 当前决定 |
| --- | --- |
| 开发基线 | 签名 tag `0.7.0`，commit `fd66d11cf2352cdfec122ea4a97caca4d23675b4` |
| Windows GUI | Qt Quick/QML 独立进程，放弃现有 Win32/GDI+ 主窗口 |
| Windows tray/runtime | 继续使用轻量 Win32 进程，独占代理 runtime、repository 与启动项管理 |
| macOS GUI | 保留 AppKit，不引入 Qt；只接收共享行为修正和必要文案变化 |
| Windows 安装 | `0.8.0` 增加可卸载、可升级的安装器进程，同时保留 portable ZIP |
| 目标发行物 | Windows portable ZIP、Windows setup EXE、macOS arm64 ZIP |
| 数据路径 | `%USERPROFILE%/.ccs-trans/` 与 `~/.ccs-trans/` 不变，安装和卸载不得删除用户数据 |
| `0.8-A` Qt | Qt `6.10.3` 动态链接，官方 MinGW `13.1.0`，LGPL-3.0-only 履约 |
| Windows 双工具链 | runtime/tray 使用 GCC `16.1`；独立 Qt GUI 使用官方 MinGW `13.1` |
| 图形后端 | 默认 Qt RHI 自动选择，当前 Windows 11 为 D3D11；软件后端只作验证和故障诊断 |
| `0.8-A` 状态 | 完成；非商业项目使用 Inno Setup 7.0.2 |
| `0.8-B` 状态 | 完成；共享编辑、迁移恢复与 Rules 文本合同已冻结 |
| `0.8-C` 状态 | 完成；Windows tray 分层与 `ccs-trans.gui-ipc/v1` 已冻结 |
| `0.8-D` 状态 | 完成；Qt typed GUI skeleton 与生产 sidecar 生命周期已接入 |
| 当前工作包 | `0.8-E`：功能对等、错误交互与视觉/动画验收 |

## `0.8-A` 冻结结果

### 构建与依赖

- 根 CMake 使用互斥的 `CCS_TRANS_BUILD_RUNTIME` 与 `CCS_TRANS_BUILD_QT_GUI`；任何 build tree 只能构建
  一侧，禁止跨 GCC/MinGW ABI 链接 object、静态库或 STL 类型；
- Windows 提供 `windows-runtime-release/warning` 和 `windows-qt-gui-release/warning` 四个 configure、
  build、test preset；Qt target 精确要求 Qt `6.10.3` 与 GNU `13.1.x`；
- Qt 使用 `Core`、`Gui`、`Network`、`Qml`、`Quick`、`QuickControls2`、`Test`、`QuickTest`，动态部署
  实际闭包由 `packaging/windows/qt-runtime-manifest.txt` 固定；不提交 Qt 二进制；
- Qt、Qt Declarative、工具和 MinGW archive 的文件名、字节数与 SHA-256 记录在
  `dependencies/windows-qt.lock.json`；staging 包含 Qt/GCC/MinGW/Winpthreads license 与 Qt SBOM；
- `windeployqt` 只生成候选目录，固定 manifest 才是发布输入；manifest 漂移、缺 license 或 deployed
  self-test 失败都会终止部署；
- 手写 C++ 单文件上限 600 行、QML 上限 300 行；结构检查同时禁止 Qt GUI include runtime、storage、
  config、旧 Win32 window 或直接拼 pipe frame。

### Model 与动画基线

最小 GUI 不是未来页面布局，而是 model/scene graph 风险探针。它已经验证 128 Profile、每 Profile 64 Rule
（总计 8192 Rule）的 typed `QAbstractListModel`、stable-key selection、增量 data/insert/remove/move 和
4096 次连续 mutation。正常状态变化不得用 model reset 或重建整页；结构同步每个事件循环最多处理 32 次
mutation，同时受 2 ms UI-thread 时间预算约束，达到任一上限就归还事件循环。

model sync 期间暂停结构、位移、delegate colour/scale 等中间动画，只提交确定的最终 model state；同步结束
后再允许用户交互动效。这样 animation queue 不会反向拖慢 revision delta，也不会因被移除 delegate 的
未完成动画破坏 selection。后续正式 model 必须继续满足：

1. stable key 是 selection、draft、error 与 reorder 的身份，row index 只作瞬时 view 坐标；
2. controller 分批消费 snapshot/delta，QML 不循环解释 wire JSON，不在动画回调中提交业务命令；
3. hover colour、hover scale、pressed scale、focus、popup、页面、滚动和 reorder 使用统一 motion token；
4. scale 只改变视觉 transform，不改变 layout/hit target；位移至少连续线性插值，默认使用平滑 easing；
5. Reduce Motion 将 duration 降为零但保持最终状态；high contrast 不依赖仅靠颜色表达状态；
6. animation/model 测试分别验证 end state、事件循环让出、stable selection、零 idle repaint 和资源回收，
   不以“肉眼看起来流畅”替代确定性断言。

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
`ccs-trans.gui-ipc/v1`，使用 4 字节 little-endian 长度前缀的 UTF-8 JSON frame，单 frame 上限固定为
`16 MiB`。声明长度超过上限、长度溢出、非 UTF-8 或不完整 frame 都在进入 JSON parser 前拒绝。

pipe identity 由 current-user SID、canonical 配置根和 instance identity 派生；公开 pipe name 只包含版本与
这些输入的 hash，不暴露原始 SID/路径。测试 instance suffix 必须进入同一派生过程，防止测试与真实 tray
串线。ACL 只允许当前用户会话所需身份，GUI 仍须完成 version、source/session 和一次性 handshake 校验，
不能把“能打开 pipe”当作完整身份验证。

初次连接只发送 application/service 状态、Profile summary 列表、descriptor 和当前 selected Profile draft；
其他 draft 在选择时按需获取。之后以单调 composite revision 发送 typed delta，不重复发送完整 snapshot。
客户端发现 revision gap、重连或显式 reload 时丢弃未应用 delta 并请求新 snapshot；本地 dirty edit buffer
不会被异步 snapshot 自动覆盖。

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

`0.8-A` 已冻结 Qt `6.10.3` 与官方 MinGW `13.1.0`。保留 GCC 16 runtime/tray build，不迁移 WinHTTP、
server、SQLite 或共享 core。选择 MinGW Qt 的理由是官方 archive 可固定 hash、与现有 GNU 诊断习惯一致，
同时独立进程边界消除了和 GCC 16 runtime 的 C++ ABI 交换；MSVC 不再作为 `0.8.0` 主路径。

Qt 只动态链接。发行 staging 必须附带 LGPL-3.0-only、Qt notice、QtBase/QtDeclarative SPDX SBOM、GCC
Runtime Library Exception、MinGW-w64 与 Winpthreads license。未经新的许可证审计不允许改为静态 Qt，
也不允许为了减少 DLL 数量把 runtime/tray 链接进 Qt target。

两个 build tree 不共享静态库、C++ object、STL 类型或生成器缓存，只共享源码形式的 wire schema/codec
测试向量。Windows package script 在干净 staging directory 汇合经过各自 verifier 的产物，不允许从 Qt
安装目录或开发 build tree 手工挑 DLL 进入发行包。

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

ccs-trans storage migrate --replace --confirm REPLACE
```

第二种形式是自动化和非 TTY 调用的唯一替换入口。token 精确区分大小写；小写、拼写错误、stdin 非 TTY
且没有 token、备份失败或现有数据库无法验证时都拒绝替换。非 TTY 缺少 token 时不得读取 stdin。

替换前必须把现有 `profiles.db`、WAL/SHM 状态和来源信息保存到受管理 backup，完成 hash/可打开性验证后才
进入 durable migration transaction。受管路径固定为
`state/migrations/replaced-db-<source+database SHA-256>/`，保留只读、无 sidecar 的 `profiles.db` 和
`manifest.json`。恢复同时核对 repository revision、migration provenance 和物理数据库 hash；实际数据库
损坏或既非 old/target 时，只能从匹配且再次验证通过的受管备份恢复。Busy/普通 I/O 不触发推测性回滚。
任何失败保留旧数据库或可恢复 journal，不能先删除再迁移。

tray 检测到 migration required 时打开应用内 owner dialog：

1. 第一次询问是否执行 migration，按钮为 Yes/No；
2. 若已有 `profiles.db`，第二次明确显示将被替换的路径、备份位置和不可逆影响，再询问确认；
3. 取消后保持故障状态并提供再次打开入口，不循环弹窗；
4. GUI 与 CLI 调用同一个 migration service，不复制覆盖逻辑。

### Rules 编辑器

- 输入和粘贴边界统一 CRLF、LF、CR、Unicode line/paragraph separator；
- 保留 JSON 字符串内部转义，不把 `\\n` 当作实际换行；
- 修复粘贴后大空白、段落高度异常或视觉上单行的问题；
- 文本与可视化模式共享 canonical draft；未知 Rule 类型可无损保留 `options` 内任意 JSON，但 root/Rule
  wrapper 仍为严格 schema，已知 Rule 类型仍拒绝 descriptor 未声明的 option；
- 切换模式不丢 Rule 顺序、stable key、selection 或 undo history；
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

不手写拥有管理员权限的自定义安装事务。`0.8-A` 已用 Inno Setup `7.0.2-x64` 证明单一离线 setup EXE、
per-user 自定义目录、固定 staging 安装、installed-tree self-test 和静默卸载可行；但该编译器明确输出
`Non-commercial use only`。本项目已明确限定为非商业使用，因此 `0.8.0` 正式路径冻结为 Inno Setup
7.0.2；若未来可能商业使用，必须在下一次 setup 发布前改为 WiX 或取得有效商业许可。

### 安装合同

- 默认 per-user 安装到 `%LOCALAPPDATA%/Programs/ccs-trans/`，不要求管理员权限；
- 安装器可提供“加入当前用户 PATH”选项，默认不勾选；只修改本安装项拥有的精确 path entry，卸载时只
  删除该 entry，不触碰系统 PATH 或用户的 portable 路径；
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

`0.8-A` 在 Windows 11 build `22631`、32 logical processors 上冻结以下开发机预算。正式候选仍须在干净
Windows 11 VM 重测；预算只能凭测量证据调整，不能因功能增加静默放宽。

| 指标 | `0.8-A` 实测 | 后续门槛 |
| --- | ---: | ---: |
| tray cold start（无测试图标） | 228 ms | `<= 500 ms` |
| tray private / working set | 3.01 / 12.67 MiB | private `<= 8 MiB`，且不加载 Qt DLL |
| GUI main-window cold start | 355 ms | `<= 750 ms` |
| GUI private / working set | 92.37 / 88.07 MiB | 各 `<= 128 MiB` |
| GUI thread / handle | 122 / 922 | `<= 160 / 1200` |
| D3D11 4096 incremental mutations | 36 ms | `<= 250 ms` |
| software 4096 incremental mutations | 68 ms | `<= 500 ms` |
| settled idle | 0.0% CPU；1 秒 0 frame | CPU `<= 0.5%`；1 秒 `<= 2` frame |
| GUI process lifecycle | 100 次，0 failure，0 residual | 必须保持 0 / 0 |
| Qt fixed staging | 129 files，54.73 MiB | `<= 70 MiB` |
| setup prototype | 14.69 MiB | `<= 25 MiB` |

上述 tray 资源采样使用已有 test-only 无图标宿主，以免自动测试依赖 Explorer 通知区域状态；真实 icon、
tooltip 和 menu 仍由独立 tray integration/人工测试覆盖。当前桌面会话在最终复验时拒绝新增通知图标，但
同一轮开发早先真实 icon integration 已通过，代码未改动；进入候选前仍需在干净 Explorer 会话复验。

持续满足：

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
| `0.8-A` | Qt/工具链/许可证/安装器取证，最小 GUI 与 deploy prototype，0.7 资源基线、源码体量检查 | 完成 |
| `0.8-B` | Save/rename、draft 保留、newline、migration replace 与共享测试 | 完成：无 GUI 的 CLI/service 合同通过 |
| `0.8-C` | `gui-ipc/v1`、named pipe、tray 分层、launcher/session/router | 完成：fake client 可完成全部命令与重连 |
| `0.8-D` | Qt process、typed model/controller、QML shell、生命周期 | 完成：三页 skeleton、activate/hide/destroy/reconnect passed |
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

**`0.8-A` 执行结果**：依赖/工具链记录、最小 Qt GUI、Qt deploy staging、安装器原型、资源基线、CMake
presets 和结构检查均已实现。Runtime Release/warning 各 `26/26` CTest、Qt GUI Release/warning 各 `4/4`
CTest 通过；无图标 tray integration 的窗口生命周期与 `desktop-16` 通过；D3D11 与软件 RHI self-test、
固定 129 文件 staging、隔离安装、installed-tree self-test、卸载和 100 次 GUI process lifecycle 通过。

可重复入口：

```powershell
cmake --preset windows-runtime-release
cmake --build --preset windows-runtime-release
ctest --preset windows-runtime-release
cmake --preset windows-qt-gui-release
cmake --build --preset windows-qt-gui-release
ctest --preset windows-qt-gui-release
./tools/deploy_windows_qt_gui.ps1
./tools/build_windows_installer_prototype.ps1
./tools/test_windows_installer_prototype.ps1
./tools/measure_windows_gui_baseline.ps1
```

Qt 精确版本、编译器、IPC 上限/同步策略、GUI 资源预算、安装 PATH 策略和 Inno Setup 7.0.2 非商业
许可路径均已冻结，`0.8-A` 正式关闭。prototype setup 仍只是技术产物，完整 runtime、升级与卸载合同在
`0.8-G` 实现后才能成为发行候选。

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
   tab、Unicode 和大文本；canonical round-trip 保留 Rule 顺序，以及未知 Rule 类型的任意 `options`。root、
   Rule wrapper 和已知 Rule descriptor 的边界继续严格，不把“前向兼容”扩张为任意未知字段。
7. 增加纯 C++ 单测和 CLI/integration：Save/rename 原子性、失败保留 draft、enable 失败、stale Apply、
   migration 两次确认、备份故障/磁盘故障/回滚、Rules clipboard fixture 和 runtime 内容不变。

**`0.8-B` 退出条件**：不启动 GUI 也能验证所有编辑、迁移、错误和回滚合同；现有 Windows/macOS host
   仍能编译，`desktop-16`、Usage 和 SSE 回归通过。此阶段不接受通过字符串比对英文错误来判断行为。

**`0.8-B` 执行结果**：Profile id 与全部字段由 `SaveProfile` 一次原子更新，rename 保留 stable key；命令
携带可选 draft/base revision，stale、validation、route collision 和 persistence failure 均返回 typed code、
稳定 field key 与 recovery action。失败 batch 完整回滚 tray draft，不改变 selection 或 revision。复合 base
revision 使用不透明 SHA-256 token，ViewModel 不再匹配英文错误字符串。独立 `RenameProfile` 仅暂留给 AppKit
兼容层，并计划在 `0.8-H` 删除。

`storage migrate --replace [--confirm REPLACE]` 已实现交互/非交互确认、旧数据库 checkpoint、流式 SHA-256、
语义快照验证、只读无 sidecar 受管备份、物理 hash journal 和崩溃恢复。测试覆盖 old/new config 与
old/target database 四组合、同 revision 但物理 hash 不同、实际数据库损坏、受管备份损坏，以及替换成功后
旧数据库字节/语义完全保留。Rules 输入在 JSON 字符串外统一 CRLF、CR、U+2028、U+2029；字符串内
escape/Unicode 保持数据语义，并覆盖 tab、Unicode、256 KiB 文本和严格 schema 边界。

Windows 侧退出验证为 Runtime Release/warnings-as-errors 各 `26/26` CTest、Qt GUI
Release/warnings-as-errors 各 `4/4` CTest。Release `desktop-16` 与 `mixed-16` 均为零失败并校验精确 SSE
字节数；`mixed-16` 的 Responses/Chat Usage 各 `12/12` 在 stream 期间完成，upstream failure、logger
backpressure 和 writer failure 均为零。此 Windows 工作区不能执行 macOS 编译；共享源码的 macOS
Release/warning 与 AppKit 窄复验仍按 `0.8-H` 的平台交接门槛执行。

## `0.8-C`：Tray 分层与 GUI IPC

先把 Windows tray 从旧窗口实现中拆出，再建立可替换的 GUI transport。

1. 从 `tray_app.cpp` 抽出 tray lifecycle、菜单构建、GUI launcher、instance coordinator、startup
   registration、error presentation 和 runtime shutdown；每个类只拥有一个资源/职责，保留旧窗口适配器
   直到 Qt 功能对等完成。
2. 实现 `ccs-trans.gui-ipc/v1` 的 wire envelope：协议标识、message kind、request id、session id、sequence、
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

**`0.8-C` 执行结果**：共享 `ccs-trans-gui-ipc` 已实现严格 UTF-8 JSON envelope/DTO、4-byte
little-endian frame、单 frame 16 MiB 上限、client/server sequence 与 revision tracker。该 target 在 GCC 16
runtime 与 Qt MinGW 13.1 build tree 中分别从源码编译，不共享 object、静态库或 STL ABI。tray 侧新增
current-user DACL named pipe、SID/config-root/instance hash identity、一次性 256-bit token、受限匿名
bootstrap handle、suspended child/PID 绑定、独立 reader/writer、有界可靠队列及可合并 state delta。
snapshot 只在握手、显式请求或 baseline 缺失时发送；仅 revision 变化不会产生空 delta。

`MainWindowState`、field descriptor、Rules draft、selection、application status 和 typed command result 已由
bridge 映射；现有 ViewModel command 可经 fake GUI client 往返，留给 `0.8-F` 的可视化 Rule/preview command
会稳定返回 unsupported，而不会误路由到旧 editor。`ccs-trans.maintenance-ipc/v1` 使用独立 pipe 和 current-user
校验，只接受 query version、request shutdown、wait for release，GUI envelope 会被拒绝。当前 wait request
返回可重试的瞬时 `release_ready`/`draining`；`timeout_ms` 长轮询及 response-drain/进程退出协调属于正式
installer transaction，在 `0.8-G` 完成。

tray 已拆出 icon、menu、runtime shutdown、GUI launcher/session、IPC host 和 maintenance server；
`tray_app.cpp` 保持在 600 行以内。JSON codec 按 envelope、command/maintenance、state 和 state type
拆分，结构检查已覆盖 `src/gui_ipc`、新 tray/bridge/maintenance host 和 Qt GUI 的 600 行上限，并禁止
wire layer 反向 include owner layer。在 `0.8-C` 检查点，生产入口仍显示旧 Win32 窗口且不加载 Qt DLL；
该临时状态已由 `0.8-D` 的生产 sidecar 切换取代。`0.8-C` 验证覆盖
malformed/unknown/oversize/半 frame、单次输入内总量超过 16 MiB 的
多个合法 frame、PID/token/session/source/sequence/revision、背压、snapshot/delta、command result、100 次
连接循环，以及真实 suspended child 的 bootstrap/hello/snapshot/activate/shutdown。Windows runtime
warnings-as-errors `32/32` CTest、Qt warnings-as-errors `5/5` CTest 和真实 tray integration 均通过。

## `0.8-D`：Qt GUI 骨架与生命周期

1. 新增 `ccs-trans-gui` target、QML module、资源注册、Qt Test/Quick Test target 和独立 GUI build
   preset；GUI 只链接 `gui_ipc` wire codec、Qt 和必要的 GUI 基础设施，不链接 `ccs-trans-core`。
2. 实现 `gui_main`、application lifecycle、pipe client、handshake、session state、snapshot revision
   和 reconnect。tray 只在初次 draft 加载完成并产生正 revision snapshot 后允许 GUI 握手，不能把
   revision `0` 的占位状态交给 client。GUI 启动、激活、隐藏、销毁、第二实例和轻量模式先用空页面验证。
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

**`0.8-D` 执行结果**：`ccs-trans-gui.exe` 的生产入口现从受限继承 handle 读取 bootstrap，以
`QLocalSocket` 完成 hello/session/source 校验并消费正 revision snapshot/delta。Qt 侧按 Profiles、Rules、
Settings、Migration 分出 state store、typed model、controller、window lifecycle 和 QML page；正常 model
变化使用 insert/remove/move/dataChanged，本地 Profile/Rules dirty buffer 在异步状态更新时保持不变。首个有效
snapshot 自动显示一次，之后普通 hide 不会被状态刷新重新打开；tray Activate 才重新显示。轻量 close、隐藏后
启用轻量模式、tray Shutdown 和 post-auth 断线分别执行明确的退出路径。

生产 `ccs-trans-tray` 已移除 `main_window.cpp`、`windows_theme.cpp` 及 GDI+/DWM/theme 链接，不提供运行时
fallback；旧源码与 0.7 包只作 oracle。tray 在首次 `LoadDraft` 结束且 revision 为正之前排队 Open，加载失败
也允许启动 GUI，为下一阶段 migration/recovery UI 留出通路。验证通过 runtime warnings-as-errors `32/32`、
Qt warnings-as-errors `6/6` CTest；真实 staging 联调把 GCC 16 tray/runtime 与 MinGW 13.1 Qt closure 合并后，
覆盖 handshake、普通 hide/reuse、第二实例 activate、GUI crash 后 fresh session、轻量 destroy 与 graceful
quit。`0.8-E` 继续完成 field editor、dirty-close/dialog、功能对等和视觉人工验收，不把 skeleton 当作发行版。

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
4. 升级流程先通过 maintenance endpoint 请求 tray drain GUI/runtime，使用有上限的
   `wait_for_release.timeout_ms` 长轮询，并保证 release response 完整写出后 tray 才关闭 endpoint/进程；
   再以临时目录和可恢复替换完成升级。失败时不得留下混合版本 DLL/EXE；回滚后旧 tray、GUI 和配置必须
   仍可启动。
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

旧 Win32 GUI 已在 `0.8-D` 从生产 target 移出，不接受新的视觉补丁，也不存在运行时 fallback。源文件暂作
行为参照和回归 oracle；`0.8-E` 只有在 Qt GUI 功能、键盘、辅助功能、资源和并发测试通过后才删除旧文件。
最终源码、portable ZIP 和 setup 中均不得存在旧 GUI 或不可达 GDI+ theme 代码。

## 剩余决策与后续输入

Qt `6.10.3`、官方 MinGW `13.1`、双工具链入口、动态 LGPL 履约、16 MiB IPC frame、按需 snapshot +
revision delta、pipe identity 输入、资源预算和 PATH 默认策略已在 `0.8-A` 冻结。Qt 二进制仍不进入仓库。

项目已确认严格限制为非商业使用，正式安装器继续使用 Inno Setup 7.0.2。安装定义仍预留 Authenticode
signing command/interface，但没有证书时不阻塞本地 prototype。用途一旦变化，此决定立即失效，必须
切换 WiX 或取得有效商业许可。

后续仍需在相应阶段提供或冻结：

1. `0.8-F` 新增 Rule 类型的真实请求样例与明确语义；没有样例就只实现已有 descriptor；
2. `0.8-I` 的 2 小时 mixed 与 8 小时 GUI idle 在 Windows/macOS 候选上的具体执行环境；
3. 干净 Windows 11 会话中的真实 tray icon、DPI、60/120 Hz、high contrast 与辅助功能人工验收。

`0.8-A` 至 `0.8-D` 已完成，当前从 `0.8-E` 开始恢复功能对等并完成视觉/动画验收；`0.8-G` 继续按已冻结
的非商业 Inno Setup 路径实现正式 setup，并补完 maintenance wait/退出时序。
