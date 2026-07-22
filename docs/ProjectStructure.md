# ccs-trans 项目结构

## 当前目录

```text
cc-switch-trans/
  CMakeLists.txt
  CMakePresets.json
  README.md
  .gitattributes
  .gitignore

  cmake/
    windows-qt-mingw-toolchain.cmake

  dependencies/
    windows-qt.lock.json

  assets/
    icons/
      ccs-trans-512.png

  docs/
    Archived/
      MacOSValidationCheckResult.md
      Planning-0.7.0.md
      Reconstruction.md
      Release-0.5.0.md
      Release-0.6.0.md
      Release-0.7.0.md
      WindowsValidationCheckResult.md
    Design.md
    DevelopmentPlan.md
    Planning-0.8.0.md
    ProjectStructure.md

  src/
    gui/
      windows/
        CMakeLists.txt
        app/
          gui_main.cpp
        controllers/
          command_dispatcher.hpp/.cpp
        diagnostics/
          frame_monitor.hpp/.cpp
        features/
          migration/
          profiles/
          rules/
          settings/
        interaction/
          animation_policy.hpp/.cpp
        ipc/
          bootstrap_reader.hpp/.cpp
          gui_ipc_client.hpp/.cpp
        lifecycle/
          gui_window_controller.hpp/.cpp
        models/
          configuration_field_model.hpp/.cpp
          profile_summary_model.hpp/.cpp
        prototype/
          profile_list_model.hpp/.cpp
          prototype_controller.hpp/.cpp
        state/
          gui_state_store.hpp/.cpp
        tests/
          gui_ipc_client_tests.cpp
          profile_list_model_tests.cpp
          qml_test_main.cpp
          qml/
        ui/
          Main.qml
          PrototypeProbe.qml
          Theme.qml
          components/
          features/
    gui_ipc/
      frame_codec.hpp/.cpp
      json_codec.hpp/.cpp
      json_codec_support.hpp
      json_command_codec.cpp
      json_state_codec.cpp
      json_state_types.hpp/.cpp
      protocol_types.hpp/.cpp
      session.hpp/.cpp
    app/
      application_control.hpp
      application_controller.hpp/.cpp
      application_status.hpp
      app_service.hpp/.cpp
      control_executor.hpp/.cpp
    config/
      app_paths.hpp/.cpp
      application_config.hpp/.cpp
      application_settings.hpp
      composite_config_repository.hpp/.cpp
      config_cli.hpp/.cpp
      config_document.hpp/.cpp
      config_editing_service.hpp/.cpp
      config_repository.hpp
      config_store.hpp/.cpp
      configuration_conversion.hpp
      configuration_editor.hpp/.cpp
      configuration_repository.hpp
      configuration_snapshot.hpp/.cpp
      field_descriptor.hpp/.cpp
      profile_model.hpp
      rules_text.hpp/.cpp
      runtime_compiler.hpp/.cpp
    core/
      cancellation.hpp/.cpp
      http_types.hpp
      inflight_memory_budget.hpp/.cpp
      request_id.hpp/.cpp
      runtime_metrics.hpp/.cpp
      sha256.hpp/.cpp
      timeouts.hpp
      url.hpp/.cpp
    hosts/
      cli_main.cpp
      host_platform.hpp/.cpp
      macos/
        instance_coordinator.hpp/.mm
        main_window.hpp/.mm
        macos_host_platform.hpp/.mm
        menu_app.hpp/.mm
        menu_main.mm
        startup_registration.hpp/.mm
      windows/
        gui_bridge/
          gui_command_router.hpp/.cpp
          gui_ipc_connection.hpp/.cpp
          gui_ipc_server.hpp/.cpp
          gui_outbound_channel.hpp/.cpp
          gui_snapshot_builder.hpp/.cpp
        maintenance/
          maintenance_ipc_server.hpp/.cpp
        platform/
          gui_pipe_security.hpp/.cpp
        tray/
          gui_process_launcher.hpp/.cpp
          gui_session_controller.hpp/.cpp
          tray_icon.hpp/.cpp
          tray_ipc_host.cpp
          tray_menu.hpp/.cpp
          tray_messages.hpp
          tray_runtime_host.cpp
        instance_coordinator.hpp/.cpp
        main_window.hpp/.cpp
        resource_ids.hpp
        startup_registration.hpp/.cpp
        tray_app.hpp/.cpp
        tray_main.cpp
        windows_error.hpp/.cpp
        windows_host_platform.hpp/.cpp
        windows_theme.hpp/.cpp
    logging/
      logger.hpp/.cpp
    presentation/
      main_window_contract.hpp/.cpp
      main_window_view_model.hpp/.cpp
      ui_preferences.hpp/.cpp
      ui_preferences_repository.hpp
      ui_preferences_store.hpp/.cpp
    protocols/
      protocol_handler.hpp/.cpp
      protocol_registry.hpp/.cpp
      responses_handler.hpp/.cpp
      chat_handler.hpp/.cpp
      messages_handler.hpp/.cpp
    routing/
      profile.hpp
      route_table.hpp/.cpp
    rules/
      rule.hpp/.cpp
      rule_registry.hpp/.cpp
      generic_json_rules.hpp/.cpp
      remove_tool_rule.hpp/.cpp
    runtime/
      runtime_snapshot.hpp
    server/
      server.hpp/.cpp
      platform/
        local_socket.hpp
        posix/local_socket.cpp
        windows/local_socket.cpp
    storage/
      sqlite_profile_store.hpp/.cpp
    transport/
      header_filter.hpp/.cpp
      upstream_transport.hpp/.cpp
      macos/
        curl_transport.hpp/.cpp
      windows/
        winhttp_transport.hpp/.cpp

  tests/
    support/
      canonical_temp.hpp
    fixtures/
      stage11/
        config-v2-roundtrip.json
        remove-tool-cases.json
        transparent-request-body.json
    unit/
      config_cli_tests.cpp
      composite_config_repository_tests.cpp
      configuration_editor_tests.cpp
      configuration_snapshot_tests.cpp
      config_document_tests.cpp
      config_editing_service_tests.cpp
      application_config_tests.cpp
      presentation_contract_tests.cpp
      ui_preferences_store_tests.cpp
      main_window_view_model_tests.cpp
      core_tests.cpp
      application_controller_tests.cpp
      control_executor_tests.cpp
      host_platform_tests.cpp
      instance_coordinator_tests.cpp
      local_socket_tests.cpp
      protocol_tests.cpp
      route_table_tests.cpp
      rule_pipeline_tests.cpp
      sqlite_dependency_tests.cpp
      sqlite_profile_store_tests.cpp
      rules_text_tests.cpp
      gui_ipc_tests.cpp
      gui_bridge_tests.cpp
      gui_ipc_server_tests.cpp
      gui_process_launcher_tests.cpp
      gui_session_controller_tests.cpp
      maintenance_ipc_server_tests.cpp
    integration/
      mock_upstream.py
      reload_integration.cpp
      run_integration.py
      run_macos_menu_integration.py
      run_macos_proxy_integration.py
      run_macos_transport_resource_integration.py
      run_qt_tray_lifecycle.py
      run_tray_integration.py
      run_windows_system_proxy_integration.py
    benchmark/
      README.md
      mock_upstream.py
      run_benchmark.py
      run_stage14_soak.py
      run_windows_direct_benchmark.py
      rule_pipeline_benchmark.cpp

  tools/
    build_windows_installer_prototype.ps1
    check_gui_structure.cmake
    check_stage12_prerequisites.ps1
    check_stage13_prerequisites.sh
    deploy_windows_qt_gui.ps1
    generate_icons.ps1
    generate_macos_icons.sh
    package_macos.sh
    package_windows.ps1
    measure_windows_gui_baseline.ps1
    run_windows_qt_tray_integration.ps1
    test_windows_installer_prototype.ps1
    verify_macos_package.sh
    verify_windows_package.ps1

  packaging/
    macos/
      Info.plist.in
      ccs-trans.entitlements
    windows/
      ccs-trans-tray.rc.in
      qt-runtime-manifest.txt
      installer/
        ccs-trans-prototype.iss

  third_party/
    nlohmann/
      json.hpp
      LICENSE.MIT
    sqlite/
      sqlite3.c
      sqlite3.h
      sqlite3ext.h
      NOTICE.md
    qt/
      LGPL-3.0-only.txt
      NOTICE.md
```

## 目录职责

| 目录 | 职责 |
| --- | --- |
| `src/app` | 进程无关的服务启动、停止、reload、rollback、窄控制接口与共享 control executor |
| `src/config` | v3 application codec、Composite repository/migration、typed draft/descriptor、Rule 文本、CLI、v2 import codec 与 runtime 编译 |
| `src/core` | HTTP 数据、取消、timeout、URL、request id 与全局资源指标 |
| `src/gui_ipc` | GUI/maintenance wire DTO、严格 UTF-8 JSON/frame codec、session、revision 与有界通道基础类型；不依赖 Qt/runtime |
| `src/gui/windows` | 独立 Qt Quick GUI、typed state/model/controller、QML feature、lifecycle、interaction policy、frame probe 与 Qt 测试；当前为生产 sidecar |
| `src/hosts` | CLI、Windows tray、macOS menu bar 与平台窗口/系统操作；Windows GUI bridge/launcher/maintenance 属于宿主层 |
| `src/logging` | JSON Lines、有界队列、批写、2 GiB 日志族轮转/保留、error flush、drain 与 writer health |
| `src/presentation` | 平台无关主窗口合同、异步 ViewModel、Profile draft 命令、`ccs-trans.ui/v1` codec 与独立原子 store |
| `src/protocols` | Responses/Chat/Messages descriptor、校验和本地错误 envelope |
| `src/routing` | immutable RuntimeProfile 与 exact RouteTable |
| `src/rules` | Rule factory、平台无关 descriptor、共享 DOM pipeline、JSON Pointer 与 `remove_tool` |
| `src/runtime` | 不可变 RuntimeSnapshot 的聚合类型 |
| `src/server` | 单 listener、FIFO worker、generation、路由/Rule/transport 编排 |
| `src/storage` | SQLite schema、逐操作 connection/transaction、Profile/Rule stable key、revision 与 CRUD snapshot |
| `src/transport` | 跨平台 upstream 接口、header policy 与平台网络实现 |
| `tests/unit` | 配置 repository/editing、路由、protocol、Rule、logger、生命周期和本地错误合约 |
| `tests/integration` | 单端口协议、桌面宿主、SSE/Usage/reload、取消和平台 proxy 策略 |
| `tests/benchmark` | 8/16/50 路负载、0/1/8/32 Rule 微基准与长时间 soak 采样 |
| `cmake` | 独立 Qt GUI 工具链文件；不存个人绝对路径 |
| `dependencies` | 可审计的外部工具/archive 版本、来源、字节数和 hash，不存二进制 |
| `packaging/windows` | tray 资源、固定 Qt runtime manifest 与 setup 定义 |
| `third_party/qt` | Qt 发行 notice/license；实际 Qt SBOM 在部署时从冻结 SDK staging |

## 依赖方向

```text
hosts -> presentation + app controller + platform host adapter
presentation -> app control/status + config model/editing/repository + control executor
app -> runtime snapshot + server lifecycle
config -> profile model + storage repository + routing definitions + protocols + rules + JSON
runtime compiler -> RouteTable + ProtocolRegistry + RuleRegistry
server -> runtime + logging + UpstreamTransport
platform transport -> transport interface + platform API
protocols/rules/routing -> core data types
core -> C++ standard library
Windows tray -> gui_bridge + gui_ipc + presentation/app/platform host adapter
Windows Qt GUI -> Qt + GUI-owned model/controller + gui_ipc (independent process/build tree)
gui_ipc -> C++ standard library + JSON codec only
```

禁止反向依赖：

- `core`、`routing`、`rules` 不得 include Windows、Cocoa 或 curl 类型；
- `presentation` 不得 include Win32、AppKit、JSON DOM、SQLite 或 mutable RuntimeSnapshot 类型；
- `transport` 不判断 Provider host、Profile id 或 LLM JSON 字段；
- `server` 不按 protocol/rule 字符串写业务分支；
- `hosts` 不复制 compiler、RouteTable、logger 或 transport 初始化；
- `logging` 不决定请求是否改写；
- Composite repository、ConfigurationEditor 与 legacy ConfigStore 都不得原地修改已发布 RuntimeSnapshot。
- `src/gui/windows` 不得 include `runtime`、`server`、`storage`、`config`、WinHTTP 或旧 Win32 window；只
  通过 `gui_ipc` DTO 与 tray 交换状态和命令；
- `src/gui_ipc` 不得 include Qt、Win32、presentation、repository、SQLite、server 或 transport；named-pipe
  安全、进程启动和 ViewModel 映射只属于 Windows host；
- GCC 16 runtime 与 MinGW 13.1 Qt GUI 不共享 object、静态库、STL 类型或 C++ ABI，二者只在 staging
  合并可执行文件和运行库。

## CMake 目标

当前目标：

```text
ccs-trans-core
ccs-trans-gui-ipc
ccs-trans
ccs-trans-tray
ccs-trans-menu
ccs-trans-gui
ccs-trans-gui-foundation
ccs-trans-gui-structure-check
ccs-trans-gui-ipc-client-tests
ccs-trans-gui-model-tests
ccs-trans-gui-qml-tests
ccs-trans-gui-ipc-tests
ccs-trans-gui-bridge-tests
ccs-trans-gui-ipc-server-tests
ccs-trans-gui-process-launcher-tests
ccs-trans-gui-session-controller-tests
ccs-trans-maintenance-ipc-server-tests
ccs-trans-core-tests
ccs-trans-sqlite-dependency-tests
ccs-trans-sqlite-profile-store-tests
ccs-trans-control-executor-tests
ccs-trans-local-socket-tests
ccs-trans-config-document-tests
ccs-trans-application-config-tests
ccs-trans-composite-config-repository-tests
ccs-trans-configuration-snapshot-tests
ccs-trans-sha256-tests
ccs-trans-config-cli-tests
ccs-trans-config-editing-service-tests
ccs-trans-configuration-editor-tests
ccs-trans-rules-text-tests
ccs-trans-presentation-contract-tests
ccs-trans-ui-preferences-store-tests
ccs-trans-main-window-view-model-tests
ccs-trans-route-table-tests
ccs-trans-protocol-tests
ccs-trans-rule-pipeline-tests
ccs-trans-application-controller-tests
ccs-trans-host-platform-tests
ccs-trans-instance-coordinator-tests
ccs-trans-reload-integration
ccs-trans-rule-pipeline-benchmark
```

`ccs-trans-core` 继续保持一个静态库。平台 source 由 CMake 条件加入；不会为每个小目录
创建静态库。Windows 编译 WinSock adapter 与 `transport/windows`，macOS 编译 POSIX
adapter 与 `transport/macos/curl_transport.*`。

`CMakePresets.json` 提供 Windows runtime、Windows Qt GUI、macOS 26 arm64 三组独立的 Release 与
warnings-as-errors 入口。Windows Qt preset 固定 Qt 6.10.3/官方 MinGW 13.1 toolchain；macOS preset 固定
selected `macosx` SDK、architecture 和 deployment target。个人路径或凭据只能写入被忽略的
`CMakeUserPresets.json`，不能修改共享 preset。

根选项 `CCS_TRANS_BUILD_RUNTIME` 与 `CCS_TRANS_BUILD_QT_GUI` 必须恰好启用一个，同一 build tree 混建会
在 configure 阶段失败。`ccs-trans-gui-structure-check` 同时进入 runtime 和 Qt CTest，确保后续提交即使只
构建一侧也不会绕过文件体量与依赖边界。600 行上限覆盖 Qt GUI、`src/gui_ipc`、0.8 新增的
tray/bridge/maintenance host 和 `tray_app.*`；旧 `main_window.*` 仅作为待删除 oracle，不作为新代码体量
先例。

Windows 生产宿主 target 为 `ccs-trans-tray`，按需启动独立 `ccs-trans-gui`；macOS app bundle target 为
`ccs-trans-menu`，产物名为 `ccs-trans.app`。Qt target 不链接共享 core，全部业务状态经 GUI IPC 交换。
`0.8-D` 已将旧 Win32/GDI+ 窗口和 theme source 移出生产 tray target，也移除了对应链接库；旧文件暂留作
0.7 行为 oracle，`0.8-E` 功能对等验收后删除。当前 Qt skeleton 是开发态，不作为 0.8 正式发行物。

## 平台实现目录

Windows tray 验证与打包入口：

```text
tests/integration/
  run_qt_tray_lifecycle.py
  run_tray_integration.py
tools/
  run_windows_qt_tray_integration.ps1
  package_windows.ps1
```

ICO 与 RC 生成结果位于 CMake binary directory，不放回 `assets/`。`tray_app.*` 只保留宿主/message-loop、
首次 draft completion/revision 门控与 ViewModel callback 派发；icon/menu、runtime command/exit、GUI IPC
host 分别位于 `tray/`、`tray_runtime_host.cpp` 与 `tray_ipc_host.cpp`。GUI launcher/session、named-pipe
connection/server、snapshot bridge 和 maintenance endpoint 继续按资源所有权拆分。旧 `main_window.*` 与
`windows_theme.*` 不再编译，只在 `0.8-E` 完成 Qt 功能对等前作源码 oracle。
`ApplicationController`、control executor、presentation 与 HostPlatform 接口编译进共享 core，CLI 已复用
共享 runtime loader。
`run_windows_qt_tray_integration.ps1` 先用固定 manifest 部署 Qt closure，再合并 runtime build tree 并调用
`run_qt_tray_lifecycle.py`；它验证真实 handshake、普通 hide/reuse、第二实例 activate、crash 后 fresh
session、轻量 destroy 与 graceful quit。`run_tray_integration.py` 暂作 0.7 Win32 功能 oracle，其 Profile、
dirty-close、资源循环与 `desktop-16` 断言在 `0.8-E` 迁移到 Qt automation。

`ccs-trans-gui-ipc` 是两套 Windows build tree 各自编译的源码级 wire library。Runtime 侧使用
`gui_bridge` 把共享 ViewModel 映射为 snapshot/delta 和 command completion；Qt 侧只链接同一 DTO/codec，
不链接 `ccs-trans-core`。当前测试覆盖 frame/JSON/session/revision、ACL identity/PID/token、command bridge、
有界 state delivery、100 次 named-pipe reconnect、真实 suspended child bootstrap/activate/shutdown，以及
maintenance endpoint 对 GUI traffic 的拒绝。

Windows Qt 验证入口：

```text
cmake/windows-qt-mingw-toolchain.cmake
dependencies/windows-qt.lock.json
src/gui/windows/
packaging/windows/qt-runtime-manifest.txt
packaging/windows/installer/ccs-trans-prototype.iss
tools/check_gui_structure.cmake
tools/deploy_windows_qt_gui.ps1
tools/build_windows_installer_prototype.ps1
tools/test_windows_installer_prototype.ps1
tools/measure_windows_gui_baseline.ps1
tools/run_windows_qt_tray_integration.ps1
```

`src/gui/windows/prototype` 只验证 stable-key 增量 model、每轮最多 32 mutation/2 ms 的 UI-thread batch、
同步期动画暂停和 deterministic end state，不是正式业务 model。`deploy_windows_qt_gui.ps1` 从
`windeployqt` 候选输出重建 staging，再与固定 129 文件 manifest 比对并运行软件 RHI self-test；setup
prototype 只能消费这个已验证 staging。项目已明确限定为非商业使用，安装器冻结为 Inno Setup 7.0.2；
若项目用途变化，必须重新审计许可或切换 WiX。

正式 Qt 代码按 `ipc -> state/models -> controllers/features -> lifecycle -> QML pages/components` 分层。
`PrototypeProbe.qml` 与 `prototype/` 仅保留 model/scene-graph 压力和 idle repaint 自测，不参与生产页面；
生产 `Main.qml` 不持有 wire JSON 或业务 draft。正常 snapshot/delta 只更新受影响 row/role，窗口首次有效
snapshot 自动显示一次，后续显示必须来自 tray Activate。

macOS 当前实现：

```text
src/hosts/macos/
  instance_coordinator.hpp/.mm
  main_window.hpp/.mm
  menu_main.mm
  menu_app.hpp/.mm
  macos_host_platform.hpp/.mm
  startup_registration.hpp/.mm
src/server/platform/
  local_socket.hpp
  windows/local_socket.cpp
  posix/local_socket.cpp
src/transport/macos/
  curl_transport.hpp/.cpp
packaging/macos/
  Info.plist.in
  ccs-trans.entitlements
tools/
  check_stage13_prerequisites.sh
  generate_macos_icons.sh
  package_macos.sh
  verify_macos_package.sh
```

`assets/icons/ccs-trans-512.png` 是唯一手工维护图标母版。ICO、menu bar PNG 和可能的
ICNS 都从它派生到 build/package directory，不能反向编辑或提交生成物。实际 socket
文件拆分可在保持上述依赖边界的前提下调整，不要求为每个 adapter 建静态库。

`main_window.*` 持有 AppKit `NSWindowController`、Auto Layout、三视图、typed draft、dirty-close 和
普通/轻量窗口生命周期；布局按 Windows 已验收的信息架构与比例对齐，同时使用 AppKit 原生主题、控件
与滚动。`menu_app.mm` 继续唯一持有 `ApplicationController`、共享 control executor、
ViewModel、menu/status item、distributed notification 与退出编排；窗口不创建第二套 runtime。
`run_macos_menu_integration.py` 通过隔离 `HOME` 和仅测试启用的 scoped notification 自动验证窗口
功能、Rule 摘要、CLI/GUI stale Apply 与显式 Reload Draft、100 次资源生命周期、pending Quit，
以及窗口循环期间 `desktop-16` 精确回传。

`package_macos.sh` 生成正式文件名的固定白名单 ZIP，并统一对 CLI 和 `.app` 使用 ad-hoc
签名。`verify_macos_package.sh` 校验包名、内部 checksum、arm64 slice、strict codesign 和
`Signature=adhoc`；发布路径不读取 Developer ID 或公证凭据。

## 文档职责

| 文件 | 主要内容 |
| --- | --- |
| `README.md` | 当前用户命令、配置、运行和验证入口 |
| `docs/Design.md` | 当前实现、不变量和平台边界 |
| `docs/Archived/Reconstruction.md` | 阶段 11 Profile/Protocol/Rule 通用化架构归档 |
| `docs/DevelopmentPlan.md` | 后续版本进入顺序、性能门槛、发布纪律和延后项 |
| `docs/Planning-0.8.0.md` | Windows Qt Quick 独立 GUI、IPC、Rule Builder、安装器、分层与测试的当前实施计划 |
| `docs/Archived/Planning-0.7.0.md` | `0.7.0` 资源预算、SQLite、迁移、GUI、测试与提交的冻结实施记录 |
| `docs/Archived/Release-0.5.0.md` | `0.5.0` 双平台发布、溯源、回归和接受限制归档 |
| `docs/Archived/Release-0.6.0.md` | `0.6.0` GUI/Profile/Rule 基线、双平台候选验证和发行溯源归档 |
| `docs/Archived/Release-0.7.0.md` | `0.7.0` SQLite/typed GUI、双平台候选验证、限制和发行溯源归档 |
| `docs/Archived/WindowsValidationCheckResult.md` | `0.5.0-Windows-x64` 验证结果归档 |
| `docs/Archived/MacOSValidationCheckResult.md` | `0.5.0-macOS-arm64` 验证结果归档 |
| `docs/ProjectStructure.md` | 当前目录、依赖、target 与扩展位置 |
| `tests/benchmark/README.md` | 可重复性能命令和结果解释 |

## 本地生成目录

以下目录由 `.gitignore` 管理，可重建且不进入提交：

```text
build/
build-debug/
build-release/
build-warning/
build-windows-release/
build-windows-warning/
build-macos-release/
build-macos-warning/
dist/
logs/
tmp/
benchmark-results/
.vscode/
```

发布包必须使用平台白名单。Windows 保持双 executable、manifest、README、docs 与第三方
license；macOS 只允许签名 `.app`、签名 CLI、`SHA256SUMS`、README、指定 docs 和 licenses。
始终禁止用户 config/state/log、凭据、真实 body、benchmark 输出、PDB/dSYM、临时 iconset、
CMake/Ninja 中间文件和 Python cache。
