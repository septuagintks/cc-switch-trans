# ccs-trans 项目结构

## 当前目录

```text
cc-switch-trans/
  CMakeLists.txt
  README.md
  .gitattributes
  .gitignore

  assets/
    icons/
      ccs-trans-512.png

  docs/
    Design.md
    DevelopmentPlan.md
    ProjectStructure.md
    Reconstruction.md

  src/
    app/
      app_service.hpp/.cpp
    config/
      app_paths.hpp/.cpp
      config_cli.hpp/.cpp
      config_document.hpp/.cpp
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
    logging/
      logger.hpp/.cpp
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
    transport/
      header_filter.hpp/.cpp
      upstream_transport.hpp/.cpp
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
      core_tests.cpp
      protocol_tests.cpp
      route_table_tests.cpp
      rule_pipeline_tests.cpp
    integration/
      mock_upstream.py
      reload_integration.cpp
      run_integration.py
      run_windows_system_proxy_integration.py
    benchmark/
      README.md
      mock_upstream.py
      run_benchmark.py
      run_windows_direct_benchmark.py
      rule_pipeline_benchmark.cpp

  tools/
    package_windows.ps1

  third_party/
    nlohmann/
      json.hpp
      LICENSE.MIT
```

## 目录职责

| 目录 | 职责 |
| --- | --- |
| `src/app` | 进程无关的服务启动、停止、reload、rollback 与 wait 生命周期 |
| `src/config` | v2 文档、CLI 单字段命令、原子持久化、用户路径和 runtime 编译 |
| `src/core` | HTTP 数据、取消、timeout、URL、request id 与全局资源指标 |
| `src/hosts` | CLI 及后续 tray/menu bar 的进程入口 |
| `src/logging` | JSON Lines、有界队列、批写、error flush、drain 与 writer health |
| `src/protocols` | Responses/Chat/Messages descriptor、校验和本地错误 envelope |
| `src/routing` | immutable RuntimeProfile 与 exact RouteTable |
| `src/rules` | Rule factory、共享 DOM pipeline、JSON Pointer 与 `remove_tool` |
| `src/runtime` | 不可变 RuntimeSnapshot 的聚合类型 |
| `src/server` | 单 listener、FIFO worker、generation、路由/Rule/transport 编排 |
| `src/transport` | 跨平台 upstream 接口、header policy 与平台网络实现 |
| `tests/unit` | 配置、路由、protocol、Rule、logger、生命周期和本地错误合约 |
| `tests/integration` | 单端口多协议、SSE/Usage/reload、取消和 Windows proxy 策略 |
| `tests/benchmark` | 8/16/50 路负载与 0/1/8/32 Rule 微基准 |

## 依赖方向

```text
hosts -> app + config commands
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
ccs-trans-core-tests
ccs-trans-config-document-tests
ccs-trans-config-cli-tests
ccs-trans-route-table-tests
ccs-trans-protocol-tests
ccs-trans-rule-pipeline-tests
ccs-trans-reload-integration
ccs-trans-rule-pipeline-benchmark
```

`ccs-trans-core` 继续保持一个静态库。平台 source 由 CMake 条件加入；不会为每个小目录
创建静态库。Windows 只编译 `transport/windows`，macOS 阶段加入
`transport/macos/curl_transport.*`。

## 后续扩展目录

Windows tray 阶段按真实实现增加：

```text
assets/icons/windows/
  ccs-trans.ico
src/hosts/windows/
  tray_main.cpp
  tray_controller.hpp/.cpp
  startup_registration.hpp/.cpp
  instance_coordinator.hpp/.cpp
tools/
  generate_icons.ps1
```

macOS 阶段增加：

```text
assets/icons/macos/
src/hosts/macos/
src/transport/macos/
  curl_transport.hpp/.cpp
packaging/macos/
```

`assets/icons/ccs-trans-512.png` 是唯一手工维护图标母版。ICO、menu bar PNG 和可能的
ICNS 都从它派生，不能反向编辑生成物。

## 文档职责

| 文件 | 主要内容 |
| --- | --- |
| `README.md` | 当前用户命令、配置、运行和验证入口 |
| `docs/Design.md` | 当前实现、不变量和平台边界 |
| `docs/Reconstruction.md` | Profile/Protocol/Rule 通用架构与扩展合约 |
| `docs/DevelopmentPlan.md` | tray、macOS 的构建顺序和验收门槛 |
| `docs/ProjectStructure.md` | 当前目录、依赖、target 与扩展位置 |
| `tests/benchmark/README.md` | 可重复性能命令和结果解释 |

## 本地生成目录

以下目录由 `.gitignore` 管理，可重建且不进入提交：

```text
build/
build-debug/
build-release/
build-warning/
dist/
logs/
tmp/
benchmark-results/
.vscode/
```

发布包必须使用白名单。当前允许 `ccs-trans.exe`、`SHA256SUMS.txt`、README、四篇 docs
和 `THIRD_PARTY_LICENSES/nlohmann-json.MIT`；始终禁止用户 config/state/log、凭据、
真实 body、benchmark 输出、PDB、CMake/Ninja 中间文件和 Python cache。
