# ccs-trans 开发计划

## 开发目标

按最小可用代理到完整 OpenAI 兼容代理的顺序构建 `ccs-trans`。开发过程优先保证每一步都可以编译、运行和验证，再逐步加入并发、完整日志、流式响应和错误处理。

## 阶段 1：项目骨架

目标：建立可编译的 C++ 命令行项目。

构建顺序：

1. 创建基础目录结构：

```text
CMakeLists.txt
src/
  main.cpp
  config.hpp
  server.hpp
  server.cpp
  proxy.hpp
  proxy.cpp
  logger.hpp
  logger.cpp
```

2. 配置 CMake，生成 `ccs-trans` 可执行文件。
3. 确定第三方库引入方式。
4. 先实现一个只打印版本和帮助信息的空程序。
5. 添加基础构建验证命令。

推荐先选轻量实现组合：

```text
cpp-httplib  HTTP 服务端和客户端
CLI11        命令行参数
spdlog       日志
```

阶段完成标准：

- `cmake --build` 可以生成 `ccs-trans`。
- `ccs-trans --help` 可以输出参数说明。
- 程序启动失败时能返回非零退出码。

## 阶段 2：配置和 CLI

目标：让所有关键参数可以通过命令行传入，并集中保存到配置结构。

构建顺序：

1. 实现 `AppConfig`。
2. 解析以下参数：

```text
--listen-host
--listen-port
--upstream-url
--responses-path
--chat-path
--usage-path
--upstream-responses-path
--upstream-chat-path
--upstream-usage-path
--log-path
--log-level
--log-body
--redact-sensitive
--body-log-limit
--timeout-ms
--max-body-size
--concurrency
```

3. 校验 `--upstream-url` 必填。
4. 校验端口范围、超时时间、body 大小限制。
5. 启动时打印最终配置摘要。

阶段完成标准：

- 不传 `--upstream-url` 时启动失败并提示原因。
- 可以通过 `--listen-port` 修改监听端口。
- 默认命令名和文档统一为 `ccs-trans`。

## 阶段 3：最小 HTTP 服务

目标：本地服务可以监听端口，并区分两个 OpenAI 兼容入口。

构建顺序：

1. 实现 `Server` 模块。
2. 启动监听 `listen_host:listen_port`。
3. 注册路由：

```text
POST /v1/responses/
POST /v1/chat/completions
```

4. 对两个路由先返回固定 JSON。
5. 对未知路径返回 OpenAI 风格 `404` 错误。
6. 对非 `POST` 方法返回 `405`。

阶段完成标准：

- `curl http://127.0.0.1:15723/v1/responses/` 能收到明确错误。
- `POST /v1/responses/` 能进入 Responses 处理分支。
- `POST /v1/chat/completions` 能进入 Chat Completions 处理分支。

## 阶段 4：普通请求转发

目标：完成非流式 JSON 请求的透明转发。

构建顺序：

1. 实现 `Proxy` 模块。
2. 拼接上游 URL：

```text
{upstream-url}{upstream-responses-path}
{upstream-url}{upstream-chat-path}
{upstream-url}{upstream-usage-path}
```

3. 透传查询参数。
4. 复制请求头，过滤 `Host`、`Content-Length`、`Connection`、`Transfer-Encoding`。
5. 透传请求体。
6. 将上游状态码、响应头和响应体返回给客户端。
7. 支持 `GET /v1/usage` 透明转发。
8. 实现上游不可达和超时错误。

阶段完成标准：

- 本地 Responses 请求可以转发到指定上游。
- 本地 Chat Completions 请求可以转发到指定上游。
- 本地 Usage 查询请求可以转发到指定上游。
- Usage 查询请求不写请求链路日志。
- 上游返回的 JSON、状态码和主要响应头能传回调用方。

## 阶段 5：请求 ID 和基础日志

目标：每个请求都有可追踪的完整链路日志。

构建顺序：

1. 实现 `request_id` 生成。
2. 初始化文件日志，支持 `--log-path`。
3. 启动时创建日志目录。
4. 使用 JSON Lines 格式写日志。
5. 记录以下事件：

```text
server_start
request_received
upstream_request
upstream_response
response_sent
request_error
server_stop
```

6. 日志中加入 `request_id`、路径、上游地址、状态码、耗时和 body 大小。

阶段完成标准：

- 每次请求至少产生 4 条链路日志。
- 同一个请求的日志可以通过 `request_id` 串起来。
- 修改 `--log-path` 后日志写入新位置。

## 阶段 6：完整日志和脱敏选项

目标：满足“完整记录所有信息”的要求，同时提供可选保护。

构建顺序：

1. 在日志中记录完整请求头、请求体、响应头、响应体。
2. 实现 `--log-body false`。
3. 实现 `--body-log-limit`，超限时截断并标记 `truncated: true`。
4. 实现 `--redact-sensitive true`。
5. 脱敏以下头：

```text
Authorization
Proxy-Authorization
Cookie
Set-Cookie
X-Api-Key
```

阶段完成标准：

- 默认能看到完整 body。
- 关闭 `--log-body` 后只记录 body 大小。
- 开启脱敏后敏感头不以原文写入日志。

## 阶段 7：并发处理

目标：Responses 与 Chat Completions 在同端口下可以并行处理多个请求。

构建顺序：

1. 配置 HTTP 服务线程数或线程池。
2. 将 `--concurrency` 接入服务启动逻辑。
3. 确保日志写入线程安全。
4. 确保上游 HTTP 客户端不会共享非线程安全状态。
5. 增加并发压测脚本或手动验证命令。

阶段完成标准：

- 多个 Responses 请求可以同时处理。
- Responses 与 Chat Completions 请求可以同时处理。
- 并发日志不交叉、不破坏 JSON Lines 格式。

## 阶段 8：SSE 流式响应

目标：支持 OpenAI `stream: true` 场景。

构建顺序：

1. 检测上游 `Content-Type: text/event-stream`。
2. 对流式响应启用边读边写。
3. 保留 SSE 响应头。
4. 记录流开始、流片段大小、流结束和总耗时。
5. 当 `--log-body true` 时记录 SSE 原始片段，并受 `--body-log-limit` 限制。
6. 处理客户端提前断开和上游提前断开。

阶段完成标准：

- `stream: true` 请求能实时返回事件。
- 客户端不会等上游完整结束后才收到内容。
- 流式请求也能通过 `request_id` 查看完整日志。

## Usage 查询参考信息

脚本编写说明：
配置格式：
({
  request: {
    url: "{{baseUrl}}/v1/usage",
    method: "GET",
    headers: { "Authorization": "Bearer {{apiKey}}" }
  },
  extractor: function(response) {
    const remaining = response?.remaining ?? response?.quota?.remaining ?? response?.balance;
    const unit = response?.unit ?? response?.quota?.unit ?? "USD";
    return {
      isValid: response?.is_active ?? response?.isValid ?? true,
      remaining,
      unit
    };
  }
})
extractor 返回格式（所有字段均为可选）：
• isValid: 布尔值，套餐是否有效
• invalidMessage: 字符串，失效原因说明（当 isValid 为 false 时显示）
• remaining: 数字，剩余额度
• unit: 字符串，单位（如 "USD"）
• planName: 字符串，套餐名称
• total: 数字，总额度
• used: 数字，已用额度
• extra: 字符串，扩展字段，可自由补充需要展示的文本
💡 提示：
• 变量 {{apiKey}} 和 {{baseUrl}} 会自动替换
• extractor 函数在沙箱环境中执行，支持 ES2020+ 语法
• 整个配置必须用 () 包裹，形成对象字面量表达式

## 阶段 9：错误处理完善

目标：所有常见失败都返回明确的 OpenAI 风格错误。

构建顺序：

1. 请求体超过 `--max-body-size` 返回 `413`。
2. 上游连接失败返回 `502`。
3. 上游超时返回 `504`。
4. 本地路由不存在返回 `404`。
5. 方法不允许返回 `405`。
6. 内部异常返回 `500`。
7. 所有错误写入 `request_error` 日志。

阶段完成标准：

- 每种错误都有稳定状态码。
- 每种错误都有 JSON 响应。
- 每种错误都能在日志中定位原因。

## 阶段 10：验证和打包

目标：形成可以交付和复测的工具。

构建顺序：

1. 编写基础集成测试：

```text
Responses 普通 JSON 转发
Chat Completions 普通 JSON 转发
未知路径
上游不可达
日志路径修改
并发请求
SSE 流式响应
```

2. 准备本地 mock upstream。
3. 编写 README 使用示例。
4. 增加版本参数：

```text
ccs-trans --version
```

5. 生成 Release 构建。

阶段完成标准：

- 测试覆盖主要转发路径。
- 用户只看 README 就能启动和验证工具。
- Release 产物包含 `ccs-trans` 可执行文件。

## 推荐实现顺序总览

```text
1. 项目骨架
2. CLI 配置
3. 本地 HTTP 路由
4. 普通 JSON 转发
5. 请求 ID 和基础日志
6. 完整日志与脱敏选项
7. 并发处理
8. SSE 流式响应
9. 错误处理完善
10. 测试、文档和 Release
```

这个顺序的核心原则是：先做出能启动、能接请求、能转发的最小闭环，再逐步补齐可靠性和可观测性。
