# ccs-trans 项目结构

## 目的

本文同时定义当前可构建目录和阶段 11 的目标目录。当前树必须始终能通过 CMake
构建；目标树只在对应模块真正实现时逐步落地，不提前创建空壳。

Git 跟踪的源码、文档和源资产是唯一 source of truth。`build`、`dist`、日志或测试
临时目录中的副本不能反向覆盖仓库文件。

## 当前可提交目录

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
    config/
      app_paths.hpp/.cpp
      config.hpp/.cpp
      config_document.hpp/.cpp
      config_store.hpp/.cpp
      profile_store.hpp/.cpp
    core/
      app_service.hpp/.cpp
      cancellation.hpp/.cpp
      http_types.hpp
      request_id.hpp/.cpp
      runtime_metrics.hpp/.cpp
      task.hpp/.cpp
      task_router.hpp/.cpp
      timeouts.hpp
      transform.hpp/.cpp
      url.hpp/.cpp
    hosts/
      cli_main.cpp
    logging/
      logger.hpp/.cpp
    server/
      server.hpp/.cpp
    transforms/
      findcg_responses_transform.hpp/.cpp
    transport/
      header_filter.hpp/.cpp
      proxy.hpp/.cpp

  tests/
    fixtures/
      stage11/
        config-v1-read-only.json
        config-v2-roundtrip.json
        findcg-transform-cases.json
        transparent-request-body.json
    unit/
      config_document_tests.cpp
      core_tests.cpp
    integration/
      mock_upstream.py
      reload_integration.cpp
      run_integration.py
      run_windows_system_proxy_integration.py
    benchmark/
      README.md
      mock_upstream.py
      run_benchmark.py
      transform_benchmark.cpp

  third_party/
    nlohmann/
      json.hpp
      LICENSE.MIT
```

`assets/icons/ccs-trans-512.png` 是唯一手工维护图标母版。Windows ICO 和未来可能的
macOS bundle 资源都从它生成，不能反向编辑派生文件。

## 当前职责

| 目录 | 当前职责 | 已知重构点 |
| --- | --- | --- |
| `src/config` | v2 editable document/store、旧 CLI/AppConfig/ProfileStore、用户路径 | 11.3 起逐步移除旧模型 |
| `src/core` | AppService、任务枚举、router 基础、取消、指标、transform 接口 | app/routing/runtime 职责混合 |
| `src/hosts` | CLI 入口 | 将增加 Windows tray 和 macOS menu bar |
| `src/logging` | JSON Lines、批写、flush、背压、writer health | 保持独立，仅调整标签 |
| `src/server` | 双 listener、worker、路由编排、reload | 改为单 listener + RouteTable |
| `src/transforms` | findcg Responses 特定规则 | 改为通用 RuleRegistry/Pipeline |
| `src/transport` | headers、WinHTTP system proxy、streaming、cancel、timeout | 后续拆平台实现 |
| `tests/unit` | 配置、路由、URL、日志和 transform | 按新领域模块拆 fixture |
| `tests/integration` | 双上游、协议、reload generation | 扩展多 profile/Messages/rules |
| `tests/benchmark` | 8/16/50 路和 transform 微基准 | 扩展 profile/rule 维度 |
| `tests/fixtures` | 纯合成、可跨测试层复用的协议/config 输入 | v2 schema 与新 rule cases |

## 阶段 11 目标目录

```text
src/
  app/
    app_service.hpp/.cpp

  config/
    app_paths.hpp/.cpp
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
    protocol_handler.hpp
    protocol_registry.hpp/.cpp
    responses_handler.hpp/.cpp
    chat_handler.hpp/.cpp
    messages_handler.hpp/.cpp

  routing/
    profile.hpp
    route_table.hpp/.cpp

  rules/
    rule.hpp
    rule_registry.hpp/.cpp
    generic_json_rules.hpp/.cpp
    remove_tool_rule.hpp/.cpp

  runtime/
    runtime_snapshot.hpp/.cpp

  server/
    server.hpp/.cpp

  transport/
    header_filter.hpp/.cpp
    upstream_transport.hpp
    windows/
      winhttp_transport.hpp/.cpp
```

这个目录是职责地图，不要求每个名称一次性出现。具体迁移顺序：

| 当前文件/职责 | 目标位置 | 迁移触发点 |
| --- | --- | --- |
| `core/app_service.*` | `app/app_service.*` | 新 RuntimeSnapshot 接入 AppService |
| `config/profile_store.*` | 删除；`config/config_store.*` 已接替持久化职责 | 新 CLI 接入后移除旧 v1 store |
| `config/config.*` | 保留 CLI 解析后删除；领域类型已进入 `config_document.*` | `runtime_compiler.*` 接入后删除旧 AppConfig |
| `core/task*` | `routing/profile.hpp` + `route_table.*` | 固定 endpoint/task enum 被移除 |
| `core/transform.*` | `rules/rule.*` | RuleRegistry 接入 |
| `transforms/findcg_*` | `rules/remove_tool_rule.*` | findcg 行为改为 profile rule |
| `transport/proxy.*` | `transport/windows/winhttp_transport.*` | transport interface 稳定 |

每次移动必须同时更新：

- CMake source list；
- include path；
- namespace/类型名；
- unit 和 integration tests；
- 本文当前目录；
- `rg` 检查，确保旧 include 和类型无残留。

## 阶段 12/13 扩展目录

Windows tray 开始后再增加：

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

macOS 开始后再增加：

```text
assets/icons/macos/
src/hosts/macos/
src/transport/macos/
packaging/macos/
```

平台目录只包含平台实现。tray/menu controller 通过同一应用命令接口调用核心服务，
不得复制 config parser、RouteTable、RuleRegistry、logger 或 transport 初始化。

平台 transport 的代理边界固定为：Windows 11 21H2 使用 WinHTTP 自动跟随当前用户
手动系统代理、bypass 与显式 PAC，代理失败不回退 direct；auto-detect-only 不执行
WPAD。macOS 链接系统 libcurl，只继承启动进程环境，不读取或修改 macOS 系统代理。
代理地址和凭据不进入公共 config/profile 类型。

## CMake 目标演进

当前目标：

```text
ccs-trans-core
ccs-trans
ccs-trans-core-tests
ccs-trans-reload-integration
ccs-trans-transform-benchmark
```

阶段 11 内部模块仍链接进一个 `ccs-trans-core`，不因目录增加创建大量静态库。只有
当平台构建确实需要不同 source set 时再拆接口/实现 target。

阶段 12 目标：

```text
ccs-trans             console CLI subsystem
ccs-trans-tray        Windows GUI subsystem
ccs-trans-core        shared application/proxy implementation
```

阶段 13 目标按 macOS 工具链确定 CLI 与 `.app` targets。宿主 target 只负责进程/UI
适配，共享请求行为必须来自 core。

## 依赖方向

目标依赖：

```text
hosts -> app + config commands
app -> runtime + server + logging lifecycle
config -> routing/rule definitions + JSON
runtime compiler -> routing + protocols + rules
server -> runtime snapshot + logging + transport interface
protocols -> core HTTP/JSON helpers + rule capabilities
rules -> core + structured JSON
platform transport -> transport interface + platform APIs
core -> C++ standard library
```

禁止依赖：

- `core`、`routing`、`rules` 引入 Winsock、WinHTTP、Windows UI 或 Cocoa 类型；
- `transport` 判断 Provider host、profile 名或 LLM JSON 字段；
- `server` 根据 protocol/rule id 写业务 `if` 分支；
- `hosts` 复制服务启动、reload、路由或日志初始化；
- `logging` 决定请求是否改写；
- config store 原地修改已发布 RuntimeSnapshot。

## 测试目录目标

继续按测试层次而非源码目录一比一复制：

```text
tests/unit/
  config_document_tests.cpp
  route_table_tests.cpp
  protocol_tests.cpp
  rule_pipeline_tests.cpp
  logger_tests.cpp

tests/integration/
  mock_upstream.py
  proxy_integration.py
  reload_integration.cpp
  run_windows_system_proxy_integration.py
  tray_lifecycle_tests.cpp        # Windows 阶段再增加

tests/benchmark/
  run_benchmark.py
  transform_benchmark.cpp

tests/fixtures/
  stage11/
    config-v1-read-only.json
    config-v2-roundtrip.json
    findcg-transform-cases.json
    transparent-request-body.json
```

在现有单个 `core_tests.cpp` 影响 review 或编译定位前不机械拆文件。开始拆分时每个
test target 只链接必要模块，fixture 不读取真实 `.ccs-trans` 或日志。

## 文档职责

| 文件 | 职责 |
| --- | --- |
| `README.md` | 用户可见的当前运行、构建、配置和验证入口 |
| `docs/Design.md` | 当前实现基线、稳定不变量和已批准演进方向 |
| `docs/Reconstruction.md` | Profile/Protocol/Rule 目标模型与关键架构决策 |
| `docs/DevelopmentPlan.md` | 从当前状态开始的构建顺序、review 和验收门槛 |
| `docs/ProjectStructure.md` | 当前/目标目录、依赖和迁移规则 |
| `tests/benchmark/README.md` | 可重复性能测试命令和结果解释 |

同一事实只设一个主要归属：详细 schema 在 Reconstruction，阶段顺序在
DevelopmentPlan，目录树在 ProjectStructure；其他文档只摘要并链接，避免再次积累
互相矛盾的完成记录。

## 本地生成目录

以下目录由 `.gitignore` 管理，可删除并重新生成：

| 目录 | 用途 | 风险 |
| --- | --- | --- |
| `build` | 默认构建 | 非 source of truth |
| `build-debug` | Debug 构建 | 可重建 |
| `build-release` | Release 构建 | 可重建 |
| `dist` | 发布暂存与 ZIP | 运行后可能出现日志副本 |
| `logs` | 显式仓库内日志 | 可能含凭据和完整上下文 |
| `tmp` | 测试、打包和临时文件 | 可重建 |
| `benchmark-results` | 本机性能结果 | 含机器特征，不提交 |
| `.vscode` | 开发者设置 | 不作为程序配置 |

`.git` 和未知工具隐藏目录不是清理目标。递归清理前必须解析并核对绝对路径位于仓库
根内。

## 发布边界

发布包使用明确白名单，不递归压缩运行过的目录。当前允许项：

```text
ccs-trans.exe
README.md
docs/Design.md
docs/DevelopmentPlan.md
docs/ProjectStructure.md
docs/Reconstruction.md
THIRD_PARTY_LICENSES/nlohmann-json.MIT
```

阶段 12 再加入 tray executable 和 ICO resource。以下内容始终禁止进入 Git/ZIP：

- 用户 `config.json`、state 和日志；
- Authorization、Cookie、API key、真实请求/响应 body；
- benchmark 输出、机器资源样本和测试临时配置；
- CMake/Ninja 中间文件、PDB、Python cache；
- 从旧 `dist` 复制回来的文档或二进制。
