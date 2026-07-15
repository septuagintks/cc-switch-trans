# ccs-trans 项目结构

## 当前目录

```text
cc-switch-trans/
  CMakeLists.txt
  CMakePresets.json
  README.md
  .gitattributes
  .gitignore

  assets/
    icons/
      ccs-trans-512.png

  docs/
    Archived/
      MacOSValidationCheckResult.md
      Reconstruction.md
      Release-0.5.0.md
      WindowsValidationCheckResult.md
    Design.md
    DevelopmentPlan.md
    ProjectStructure.md

  src/
    app/
      application_control.hpp
      application_controller.hpp/.cpp
      application_status.hpp
      app_service.hpp/.cpp
      control_executor.hpp/.cpp
    config/
      app_paths.hpp/.cpp
      config_cli.hpp/.cpp
      config_document.hpp/.cpp
      config_editing_service.hpp/.cpp
      config_repository.hpp
      config_store.hpp/.cpp
      runtime_compiler.hpp/.cpp
    core/
      cancellation.hpp/.cpp
      http_types.hpp
      request_id.hpp/.cpp
      runtime_metrics.hpp/.cpp
      timeouts.hpp
      url.hpp/.cpp
    hosts/
      cli_main.cpp
      host_platform.hpp/.cpp
      macos/
        instance_coordinator.hpp/.mm
        macos_host_platform.hpp/.mm
        menu_app.hpp/.mm
        menu_main.mm
        startup_registration.hpp/.mm
      windows/
        instance_coordinator.hpp/.cpp
        main_window.hpp/.cpp
        resource_ids.hpp
        startup_registration.hpp/.cpp
        tray_app.hpp/.cpp
        tray_main.cpp
        windows_error.hpp/.cpp
        windows_host_platform.hpp/.cpp
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
    transport/
      header_filter.hpp/.cpp
      upstream_transport.hpp/.cpp
      macos/
        curl_transport.hpp/.cpp
      windows/
        winhttp_transport.hpp/.cpp

  tests/
    fixtures/
      stage11/
        config-v2-roundtrip.json
        remove-tool-cases.json
        transparent-request-body.json
    unit/
      config_cli_tests.cpp
      config_document_tests.cpp
      config_editing_service_tests.cpp
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
    integration/
      mock_upstream.py
      reload_integration.cpp
      run_integration.py
      run_macos_menu_integration.py
      run_macos_proxy_integration.py
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
    check_stage12_prerequisites.ps1
    check_stage13_prerequisites.sh
    generate_icons.ps1
    generate_macos_icons.sh
    package_macos.sh
    package_windows.ps1
    verify_macos_package.sh
    verify_windows_package.ps1

  packaging/
    macos/
      Info.plist.in
      ccs-trans.entitlements
    windows/
      ccs-trans-tray.rc.in

  third_party/
    nlohmann/
      json.hpp
      LICENSE.MIT
```

## 目录职责

| 目录 | 职责 |
| --- | --- |
| `src/app` | 进程无关的服务启动、停止、reload、rollback、窄控制接口与共享 control executor |
| `src/config` | v2 文档、共享 draft/commit 服务、repository、CLI、原子持久化、用户路径和 runtime 编译 |
| `src/core` | HTTP 数据、取消、timeout、URL、request id 与全局资源指标 |
| `src/hosts` | CLI、Windows tray、macOS menu bar 与平台窗口/系统操作 |
| `src/logging` | JSON Lines、有界队列、批写、2 GiB 日志族轮转/保留、error flush、drain 与 writer health |
| `src/presentation` | 平台无关主窗口合同、异步 ViewModel、Profile draft 命令、`ccs-trans.ui/v1` codec 与独立原子 store |
| `src/protocols` | Responses/Chat/Messages descriptor、校验和本地错误 envelope |
| `src/routing` | immutable RuntimeProfile 与 exact RouteTable |
| `src/rules` | Rule factory、共享 DOM pipeline、JSON Pointer 与 `remove_tool` |
| `src/runtime` | 不可变 RuntimeSnapshot 的聚合类型 |
| `src/server` | 单 listener、FIFO worker、generation、路由/Rule/transport 编排 |
| `src/transport` | 跨平台 upstream 接口、header policy 与平台网络实现 |
| `tests/unit` | 配置 repository/editing、路由、protocol、Rule、logger、生命周期和本地错误合约 |
| `tests/integration` | 单端口协议、桌面宿主、SSE/Usage/reload、取消和平台 proxy 策略 |
| `tests/benchmark` | 8/16/50 路负载、0/1/8/32 Rule 微基准与长时间 soak 采样 |

## 依赖方向

```text
hosts -> presentation + app controller + platform host adapter
presentation -> app control/status + config editing/repository + control executor
app -> runtime snapshot + server lifecycle
config -> routing definitions + protocols + rules + JSON
runtime compiler -> RouteTable + ProtocolRegistry + RuleRegistry
server -> runtime + logging + UpstreamTransport
platform transport -> transport interface + platform API
protocols/rules/routing -> core data types
core -> C++ standard library
```

禁止反向依赖：

- `core`、`routing`、`rules` 不得 include Windows、Cocoa 或 curl 类型；
- `presentation` 不得 include Win32、AppKit、JSON DOM、SQLite 或 mutable RuntimeSnapshot 类型；
- `transport` 不判断 Provider host、Profile id 或 LLM JSON 字段；
- `server` 不按 protocol/rule 字符串写业务分支；
- `hosts` 不复制 compiler、RouteTable、logger 或 transport 初始化；
- `logging` 不决定请求是否改写；
- `ConfigStore` 不原地修改已发布 RuntimeSnapshot。

## CMake 目标

当前目标：

```text
ccs-trans-core
ccs-trans
ccs-trans-tray
ccs-trans-menu
ccs-trans-core-tests
ccs-trans-control-executor-tests
ccs-trans-local-socket-tests
ccs-trans-config-document-tests
ccs-trans-config-cli-tests
ccs-trans-config-editing-service-tests
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

`CMakePresets.json` 当前只提供 macOS 26 arm64 Release 与 warnings-as-errors 入口。preset
固定 build directory、selected `macosx` SDK、architecture 和 deployment target；个人路径
或凭据只能写入被忽略的 `CMakeUserPresets.json`，不能修改共享 preset。

Windows GUI subsystem target 为 `ccs-trans-tray`；macOS app bundle target 为
`ccs-trans-menu`，产物名为 `ccs-trans.app`。
两者只链接共享 core 和各自 host source，不能把 Win32/AppKit source 加入 console CLI。

## 平台实现目录

Windows tray 验证与打包入口：

```text
tests/integration/
  run_tray_integration.py
tools/
  package_windows.ps1
```

ICO 与 RC 生成结果位于 CMake binary directory，不放回 `assets/`。Windows tray source
只编译进 `ccs-trans-tray.exe`；`main_window.*` 持有 Win32 top-level/child HWND 和 DPI 布局，
`tray_app.*` 持有宿主生命周期并把共享 ViewModel callback 派发回 UI thread。`ApplicationController`、
control executor、presentation 与 HostPlatform 接口编译进共享 core，CLI 已复用共享 runtime loader。
`run_tray_integration.py` 使用隔离 instance suffix 自动验证基础 Profile 编辑、dirty close、普通/轻量
窗口、第二实例、两轮各 100 次资源生命周期，以及窗口抖动期间 `desktop-16` 完整回传。

macOS 当前实现：

```text
src/hosts/macos/
  instance_coordinator.hpp/.mm
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
| `docs/Archived/Release-0.5.0.md` | `0.5.0` 双平台发布、溯源、回归和接受限制归档 |
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
