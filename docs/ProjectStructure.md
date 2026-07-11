# ccs-trans 项目结构

## 目的

本文定义当前仓库目录、依赖方向、文件放置规则和生成物边界。Git 跟踪的源码与
文档是唯一 source of truth；构建目录、发布目录和运行日志中的副本不能反向覆盖
仓库文件。

## 当前目录

```text
cc-switch-trans/
  CMakeLists.txt
  README.md
  .gitattributes
  .gitignore

  docs/
    Design.md
    DevelopmentPlan.md
    ProjectStructure.md

  src/
    config/
      app_paths.hpp/.cpp
      config.hpp/.cpp
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
    transport/
      header_filter.hpp/.cpp
      proxy.hpp/.cpp
    transforms/
      findcg_responses_transform.hpp/.cpp

  tests/
    unit/
      core_tests.cpp
    integration/
      mock_upstream.py
      reload_integration.cpp
      run_integration.py
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

## 目录职责

| 目录 | 当前职责 | 后续允许扩展 |
| --- | --- | --- |
| `docs` | 当前设计、构建顺序、结构约束 | 平台与发布说明 |
| `src/config` | 用户目录、CLI、profile schema、原子保存、snapshot | schema 演进与配置观察 |
| `src/core` | 平台中立类型、任务、生命周期、取消、指标 | 稳定应用命令接口 |
| `src/hosts` | 可执行入口 | Windows tray、macOS menu bar |
| `src/logging` | JSON Lines、批写、flush、背压、健康指标 | 轮转与保留策略 |
| `src/server` | listener、容量控制、请求编排、reload | 平台 listener 接口 |
| `src/transport` | header 过滤、WinHTTP、取消、timeout | 平台 transport 实现 |
| `src/transforms` | 按任务隔离的结构化请求改写 | 新 transform 与规则注册 |
| `tests/unit` | 纯逻辑和可注入边界测试 | 新规则、配置和状态机 |
| `tests/integration` | 双上游、协议、取消、timeout、reload | tray 与跨平台协议回归 |
| `tests/benchmark` | 8/16/50 路负载和 transform 微基准 | soak 与跨平台对照 |
| `third_party/nlohmann` | 固定 JSON 单头文件与许可证 | profiling 后再评估替换 |

## CMake 目标

```text
ccs-trans-core
  config + core + logging + server + transport + transforms

ccs-trans
  src/hosts/cli_main.cpp + ccs-trans-core

ccs-trans-core-tests
  tests/unit/core_tests.cpp + ccs-trans-core

ccs-trans-reload-integration
  tests/integration/reload_integration.cpp + ccs-trans-core

ccs-trans-transform-benchmark
  tests/benchmark/transform_benchmark.cpp + ccs-trans-core
```

宿主 target 只处理输入、展示错误和生命周期控制。路由、请求改写、网络传输和
日志初始化必须位于共享库中。

## 依赖规则

```text
hosts -> config + core/AppService
core/AppService -> server
server -> core + config + logging + transport + transforms
transport -> core/config + selected platform APIs
transforms -> core + third_party/nlohmann
logging -> config + core metrics
core data types -> C++ standard library
```

允许平台实现依赖平台中立接口，不允许核心接口反向暴露 Windows 或 macOS 类型。
findcg、协议字段或 JSON 结构判断不能进入 transport。窗口、托盘和登录项状态不能
进入 server。

## 文件放置规则

1. CLI、tray 和 menu bar 的入口放在 `src/hosts`。
2. 可由多个宿主复用的状态机和命令进入 `ccs-trans-core`。
3. OpenAI 请求匹配和结构化修改放在 `src/transforms` 或平台中立核心规则层。
4. Windows、macOS 专用实现使用明确目录或文件名，并由 CMake 按平台选择。
5. 纯函数和配置边界测试放在 `tests/unit`。
6. socket、上游和进程级行为放在 `tests/integration`。
7. 性能 fixture 只使用合成请求，放在 `tests/benchmark`。
8. 第三方源码和许可证必须同目录固定，不能依赖开发机临时安装副本。

## 本地生成目录

以下目录受 `.gitignore` 管理，可删除并由构建或测试重新生成：

| 目录 | 用途 | 注意事项 |
| --- | --- | --- |
| `build` | 默认构建 | 不作为发布输入之外的 source of truth |
| `build-debug` | Debug 构建 | 可重建 |
| `build-release` | Release 构建 | 可重建 |
| `dist` | 本地发布暂存和 ZIP | 可能包含运行后的日志副本 |
| `logs` | 仓库内显式日志路径 | 可能包含凭据和完整上下文 |
| `tmp` | 集成测试、打包和临时文件 | 可重建 |
| `benchmark-results` | 本机合成性能结果 | 不提交原始机器数据 |
| `.vscode` | 当前开发者编辑器设置 | 不作为程序配置 |

`.git` 是版本数据库，任何清理命令都不得操作。未知隐藏目录不能被打包脚本递归
收集。

## 发布边界

发布包只能通过明确白名单组装：

```text
ccs-trans.exe
README.md
docs/Design.md
docs/DevelopmentPlan.md
docs/ProjectStructure.md
THIRD_PARTY_LICENSES/nlohmann-json.MIT
```

不得递归压缩已运行过的 `dist/<package>/`。以下内容不得进入 ZIP 或 Git：

- `.ccs-trans/config.json`；
- 任何日志；
- Authorization、Cookie、API key 或真实请求 body；
- benchmark 输出与机器资源样本；
- CMake 中间文件、PDB 和测试缓存。
