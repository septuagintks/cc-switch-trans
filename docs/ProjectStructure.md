# ccs-trans 项目结构

## 目的

本文件定义仓库内目录的职责、依赖方向和生成物边界。源码与设计文档只以 Git 跟踪文件为准；`dist`、日志和构建目录中的副本不能反向覆盖源码。

## 可提交目录

```text
cc-switch-trans/
  CMakeLists.txt
  README.md
  .gitignore

  docs/
    Design.md
    DevelopmentPlan.md
    ProjectStructure.md

  src/
    config/
      config.hpp
      config.cpp
    core/
      app_service.hpp
      app_service.cpp
      http_types.hpp
      request_id.hpp
      request_id.cpp
      task.hpp
      task.cpp
      task_router.hpp
      task_router.cpp
      transform.hpp
      transform.cpp
      url.hpp
      url.cpp
    hosts/
      cli_main.cpp
    logging/
      logger.hpp
      logger.cpp
    server/
      server.hpp
      server.cpp
    transport/
      header_filter.hpp
      header_filter.cpp
      proxy.hpp
      proxy.cpp
    transforms/
      findcg_responses_transform.hpp
      findcg_responses_transform.cpp

  tests/
    unit/
      core_tests.cpp
    integration/
      mock_upstream.py
      run_integration.py

  third_party/
    nlohmann/
      json.hpp
      LICENSE.MIT
```

## 目录职责

| 目录                | 当前职责                              | 后续扩展                                 |
| ------------------- | ------------------------------------- | ---------------------------------------- |
| `docs`              | 协议设计、开发顺序、结构约束          | benchmark 说明、发布流程、平台说明       |
| `src/core`          | HTTP 类型、任务、URL、transform 接口与 AppService | 配置快照和更细生命周期事件       |
| `src/config`        | CLI 参数和运行配置                    | 配置文件、schema 迁移、不可变快照        |
| `src/hosts`         | 进程入口                              | Windows tray host、macOS app host        |
| `src/logging`       | 结构化日志 API、批量 writer 与背压    | 日志轮转和队列指标                        |
| `src/server`        | 本地 HTTP 接入、容量控制和请求编排    | listener 接口和取消传播                   |
| `src/transport`     | 头过滤和进程级 WinHTTP session        | 分阶段 timeout、连接指标、macOS 实现      |
| `src/transforms`    | 按任务隔离的请求改写规则              | Chat Completions 专用规则                 |
| `tests/unit`        | 配置、URL、路由和 transform 纯逻辑测试 | 更多边界与错误用例                        |
| `tests/integration` | 双 mock upstream 与端到端协议测试     | 8/16/50 路 SSE benchmark                  |
| `third_party/nlohmann` | 固定版本 JSON 单头文件与许可证     | profiling 后再评估替换                    |

## CMake 目标

```text
ccs-trans-core
  config + core + logging + server + transport + transforms

ccs-trans
  hosts/cli_main.cpp + ccs-trans-core
```

宿主只负责输入参数、展示错误和控制服务生命周期。协议路由、请求改写和网络传输不能复制到 host 中。

## 本地生成目录

以下目录受 `.gitignore` 管理，可以存在于工作区，但不提交：

| 目录            | 用途                                    | 是否可删除重建                   |
| --------------- | --------------------------------------- | -------------------------------- |
| `build`         | 默认/调试构建和 `compile_commands.json` | 是                               |
| `build-release` | Release 构建                            | 是                               |
| `dist`          | 本地发布包和人工验证日志                | 是，但删除前确认是否仍需诊断日志 |
| `logs`          | 默认运行日志                            | 是，但可能包含排障证据           |
| `tmp`           | 集成测试日志和打包暂存                  | 是                               |
| `.vscode`       | 当前开发者的编辑器设置                  | 是，不作为项目配置来源           |

`.git` 是版本数据库，任何清理脚本都不得操作。工具生成的隐藏目录也不能被构建或打包脚本当作项目输入。

## 依赖规则

```text
hosts -> core/app_service + config
core/app_service -> server
server -> core/config/logging/transport
transport -> core/config
transforms -> core + third_party/nlohmann
logging -> config
core task/url/transform types -> C++ standard library only
```

后续抽象 transport、listener 或 tray 时，平台实现依赖核心接口；核心层不得反向包含 Windows 或 macOS 类型。

## 新文件放置规则

1. OpenAI 请求匹配和改写规则放在 `src/core` 或未来独立的 `src/transforms`，不放入 WinHTTP 实现。
2. Windows、macOS 专用代码使用明确的平台文件名，并在 CMake 中按平台选择。
3. 新的可执行入口放在 `src/hosts`，共享行为进入 `ccs-trans-core`。
4. 端到端行为放在 `tests/integration`；纯函数测试后续放在 `tests/unit`。
5. 打包产物只能从构建输出和仓库文档生成，不能使用旧 `dist` 内容作为输入。
