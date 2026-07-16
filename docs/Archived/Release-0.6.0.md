# ccs-trans 0.6.0 发布结果归档

## 发布结论

`0.6.0` 建立 Windows 11 x64 与 macOS 26 arm64 共用的原生主窗口、轻量窗口生命周期、基础
Profile 管理和 Rule descriptor 基线。两个平台继续共享一个数字版本：

```text
0.6.0-Windows-x64
0.6.0-macOS-arm64
```

Windows 使用 WinHTTP current-user system proxy；macOS 使用 selected SDK system libcurl 与进程
proxy environment。配置 schema 仍是 `ccs-trans.config/v2`，Profile/Rule 尚未迁入 SQLite。

## 功能范围

- Win32 与 AppKit 原生主窗口复用现有 tray/menu、ApplicationController 和 runtime；
- 主窗口提供服务 Start/Stop/Reload、Profile 列表、Create/Rename/Remove、Enable/Disable、Apply、
  Discard 与独立 Reload Draft；
- 普通模式关闭后隐藏并复用窗口；轻量模式关闭后销毁窗口资源，listener 继续运行；
- dirty close 和 Quit 使用 Apply/Discard/Cancel，pending command 不会被退出竞态静默丢弃；
- ConfigRepository source revision 拒绝 CLI/GUI stale Apply，显式 Reload/Discard 后才采用外部修改；
- Profile readiness 保留所属 Profile id、未知 Protocol、非法 path、route collision 和 Rule 编译错误；
- Profile 列表显示 enabled/total Rule 数量；
- `set_field`、`remove_field`、`remove_tool` 提供平台无关 descriptor，供后续文本与可视化编辑器共用；
- 运行日志族默认总上限 2 GiB，host log 独立限制 64 MiB；SSE 日志按序号增量记录，不保留完整
  response body。

不进入本版本：SQLite、完整 Profile/全局字段 GUI、文本 Rule 编辑器、可视化 Rule builder、新协议
和新的推测性 Rule 类型。

## 源码溯源

| 项目 | 值 |
| --- | --- |
| 共享功能候选 | `e38794831518ad7a945a064277f390037bde710e` |
| 最终候选 | `c0cfbe2804a19a41f7959d055780c12b23c8dcfe` |
| 候选差异 | 仅稳定 macOS 窗口资源测试的有界 AppKit 回收等待；不修改产品代码、schema 或二进制 |
| 最终源码 | 本文件所在提交，由签名 annotated tag `0.6.0` 指向 |
| Windows 签名 key | `403FEA7B8908480460BF40BA36E772C15CA7CC4E` |
| macOS 候选签名 key | `3DF828FDD419996E11B2FD2881BFB01482975987` |

最终 Windows 与 macOS ZIP 从同一个 `0.6.0` 源码提交构建。ZIP 不能在自身内部记录外部 archive
hash；两个 ZIP 的文件名、字节数和 SHA-256 记录在签名 tag message 与跨平台交接记录中。包内
仍分别提供逐 executable/文件 checksum，并由固定白名单 verifier 解包核对。

## Windows 候选验证

环境：Windows 11 `10.0.22631` x64、CMake 4.3.2、GCC 16.1.0、Ninja 1.13.2、Python 3.14.6、
ImageMagick 7.1.2-25。

| 验证 | 结果 |
| --- | --- |
| 全新 Release / warnings-as-errors build | 81/81 / 81/81，warnings 为 0 |
| Release / warnings CTest | 16/16 / 16/16 |
| shared integration | Release 与 warnings 均 passed |
| tray/main-window integration | Release 与 warnings 均 passed |
| 轻量窗口资源 | 每套 integration 两轮各 100 次；GDI/USER 回到阈值内 |
| 窗口循环 `desktop-16` | 16/16 内容、顺序、长度和结束标记精确；上游零断连 |
| Profile/Rule | Rule summary、descriptor、stale Apply、Reload Draft、selection 与错误归属 passed |
| Rule microbenchmark | 1 KiB/100 KiB/1 MiB，0/1/8/32 Rule passed；空 pipeline 零 parse |

五档短负载全部零请求失败、零上游失败、零 logger writer failure/backpressure，storage 有界：

| Profile | 请求结果 | proxy added TTFB p50 |
| --- | ---: | ---: |
| `smoke` | 40/40 | 0.483 ms |
| `desktop-8` | 8/8 | 6.673 ms |
| `desktop-16` | 16/16 | 6.799 ms |
| `mixed-16` | 16/16，Responses/Chat Usage 各 12/12 | 6.555 ms |
| `stress-50` | 50/50 | 9.668 ms |

`stress-50` 超过 32 worker 后出现约 2 秒 p95 的有界排队；它是压力档，不代表 8-16 路桌面目标。

2 小时 mixed soak 在 `e387948` 上实际运行 `7205.154 s`：

- 1,136 轮，18,176 条 SSE、Responses/Chat Usage 各 13,632 次，失败均为 0；
- 45,440 次 upstream request 全部完成，2,233,466,880 bytes SSE 数据转发；
- working set `8.13 -> 17.97 MiB`，peak `24.67 MiB`；后半程斜率约 `-0.21 MiB/h`；
- peak private `18.04 MiB`、peak handles 442、peak threads 49；后半程 handle/thread 斜率为负；
- logger 写入 2,288,184 条、642,340,824 bytes，轮转 4 次；writer failure/backpressure 为 0；
- 停止后 connection、queued connection、active worker 和 log queue 均为 0，host exit code 0。

原始 short-load 与 soak JSON 留在 ignored `benchmark-results/`，不进入 Git 或发行包。2 小时结果
JSON SHA-256 为 `38A6F8731D6E02C739E2570CBCAAE51941DB05191B65DE0963DE4CF0A73ABAD2`。

8 小时 CLI idle soak 在 `e387948` 上实际运行 `28,800 s`，共 5,698 次资源采样：

- working set `8,476 -> 1,368 KiB`，peak `8,484 KiB`；后半程斜率约 `+13.6 KiB/h`；
- peak private `1,920 KiB`、peak handles 180、peak threads 19；后半程 handle/thread 斜率均为 0；
- idle 窗口内 runtime log 与 host output 增长均为 0；停止后的 2,640 bytes runtime log 只包含
  startup 与 shutdown/drain 事件；
- 停止后 connection、queued connection、active worker 和 log queue 均为 0，writer failure/backpressure
  为 0，host exit code 0；
- 结果记录 `git_dirty=false`，JSON SHA-256 为
  `E4DCE1C7C867D0BE7D4232291B7CEDF1CCB22D1BACC510CD416AFC333E14AD9E`。

本项验证 CLI listener/worker、WinHTTP proxy watcher、logger writer 和优雅退出的长期空闲资源稳定性；
不把它表述为 Windows tray GUI 的 8 小时常驻测试。tray/main-window 的生命周期证据来自上方自动化
资源循环与 GUI integration。

## macOS 候选验证

环境：macOS 26.5.2 (25F84) arm64、Command Line Tools SDK 26.5、Apple Clang 21.0.0、
CMake 4.4.0。机器没有完整 Xcode.app。

| 验证 | 结果 |
| --- | --- |
| 全新 Release / warnings-as-errors build | 77/77 / 77/77，warnings 为 0 |
| Release / warnings CTest | 14/14 / 14/14 |
| shared integration | Release 与 warnings 均 passed |
| macOS proxy integration | Release passed |
| menu/main-window integration | Release 与 warnings 均 passed |
| 轻量窗口资源 | 100 次生命周期 created/destroyed 精确配对，window/controller 回到基线 |
| 窗口循环 `desktop-16` | 内容、顺序、长度和结束标记精确；上游零断连 |
| Profile/Rule | Rule summary、stale Apply、Reload Draft、selection 与错误归属 passed |
| Rule microbenchmark | 1 KiB/100 KiB/1 MiB，0/1/8/32 Rule passed；空 pipeline 零 parse |

五档短负载全部零请求失败、零上游失败、零 logger writer failure/backpressure，storage 有界：

| Profile | direct | proxied | storage bytes |
| --- | ---: | ---: | ---: |
| `smoke` | 40/40 | 40/40 | 86,044 |
| `desktop-8` | 8/8 | 8/8 | 325,137 |
| `desktop-16` | 16/16 | 16/16 | 599,919 |
| `mixed-16` | 16/16 | 16/16 | 597,774 |
| `stress-50` | 50/50 | 50/50 | 681,027 |

2 小时 CLI mixed soak 在 `c0cfbe2` 上完整通过：

- 1,050 轮，16,800 条 SSE、25,200 次 Usage，失败均为 0；42,000 次代理请求全部完成；
- final drain 的 connection、queue、active worker、log queue 和 upstream failure 均为 0；
- `log_writer_failures=0`、`log_backpressure_count=0`，host exit code 0；
- RSS `8.5 -> 10.4 MiB`，peak `11.8 MiB`，后半程斜率约 `-19 KiB/h`；
- fd `45 -> 120`、threads `16 -> 25` 在初始化后稳定，后半程斜率均为 0；
- 原始 JSON SHA-256 为 `78E2DD5F10E00443E86129DBA82A9B34A82C387F61CAE5D9D376F1C6164F14A9`。

8 小时 AppKit menu idle soak 在 `c0cfbe2` 上实际运行 `28,800.01 s`：

- 5,753 次资源采样，machine CPU percent 为 0；RSS `47.0 -> 29.7 MiB`，peak `47.1 MiB`，
  整体与后半程斜率均为负；
- fd `45 -> 45`，peak 45；threads `21 -> 17`，peak 21，均无增长；
- idle 期间 request、stream、upstream byte、runtime log 和 host log 增长均为 0；
- 停止后 connection、queued connection、active worker 和 log queue 均为 0，writer healthy，
  writer failure/backpressure 为 0，host exit code 0；
- 结果 JSON SHA-256 为 `874BFF0BA36F9E6DAE1597212326673A6361D68AA031FF07163B7E9A70A3F3F4`。

约 2 小时与 7.5 小时的 live `vmmap` 快照完全稳定：physical footprint `10.7 MiB`、peak
`15.5 MiB`，DefaultMallocZone allocated `2,663 KiB`。两次受系统调试限制的
`leaks --nostacks` 均报告 288 项 / 14,384 bytes，数量和字节没有增长；全部来自三个系统
AppIntents/LNDaemonApplicationInterface `NSXPCConnection` root cycle，没有项目对象。该结果证明固定
系统保留未增长，不表述为项目对象零泄漏的绝对证明。结合 8 小时 RSS/fd/thread 负斜率或零增长，
未发现 macOS 项目内存泄漏。

## 内存与资源审计

候选期间对 GUI、executor、logger、listener/worker、cancellation、RequestGeneration、WinHTTP 与
system libcurl 所有权做了只读审计：

- 线程均在所属对象销毁前 stop/join；ViewModel 使用 FIFO barrier 排空共享 executor；
- GUI callback generation 在窗口销毁后使已排队回调失效；两平台 100 次窗口循环未保留平台对象；
- socket watch 随请求 RAII unwatch，连接 registry 受 `max_connections` 限制；
- logger queue 默认 16 MiB，cURL slot pool 受 worker 上限限制，SSE 不累计完整 response body；
- Windows 以最大 64 KiB chunk 读取上游，macOS 在 append 前检查剩余 response budget；两端均在
  分配或累计超限 response 前拒绝；
- 2 GiB 日志启动恢复以固定 64 KiB buffer 流式压缩，运行期轮转使用 rename，不把完整日志载入
  内存；
- reload 的旧 generation 只保留到对应在途请求结束，同时数量受连接容量间接限制；
- Windows mixed 后半程 working set/handle/thread 斜率均为负；Windows CLI idle 与 macOS menu idle
  均未观察到按时间累计的资源增长。

GCC 16.1.0 使用 `-fanalyzer -Wanalyzer-too-complex` 对 Windows 可编译的 42 个 `src/*.cpp`
translation unit 做了补充静态路径分析，命令失败和 analyzer diagnostics 均为 0。未运行 Windows
sanitizer、Dr. Memory、clang-tidy 或 cppcheck；本机没有这些工具。因此本节是代码所有权审计、
编译器静态分析和运行资源证据，不表述为 sanitizer 通过。

后续 `0.7-A` 必须为 raw/parsed body、JSON DOM、rewritten body、buffered response、send frame 和
body-log staging 增加全进程 inflight-byte budget，并为连续 reload + 长 SSE 增加 retired-generation
指标和 churn 测试。macOS cURL slot 的极端 OOM 构造路径、logger 入队 allocation failure 后的计数
一致性、producer-side log staging、GUI control queue 容量和 macOS response-header 聚合上限也进入
硬化范围；不以提高单请求上限替代内存预算。

## 签名与接受限制

- Windows EXE 没有 Authenticode；Defender 与 SmartScreen 不声明通过；
- macOS CLI 与 `.app` 使用 hardened runtime、无 timestamp 的 ad-hoc 签名；不建立发布者身份，
  不执行 Developer ID、notarization、staple，也不声明 Gatekeeper trust；
- macOS 当前机器只有 Command Line Tools；完整 Xcode、实际 VoiceOver 语音会话和需要真实系统状态
  mutation 的项目按最终交接结果标记；完整 Xcode gate 为 blocked，实际 VoiceOver 语音会话与
  Launch at Login mutation 为 not run；
- macOS build-tree bundle 在最终 packaging 前不是正式签名证据；正式包必须由最终源码提交重新执行
  ad-hoc signing、bundle/archive verify 和解包 smoke；
- `ccs-trans.config/v2` 增加的 `logging.max_total_size` 可从缺字段的旧 v2 使用默认值加载，但保存后的
  新文件不承诺被 `0.5.0` 反向读取；
- 大 body 仍只有单请求上限，没有全进程 inflight-byte budget；这是已识别的后续容量任务，不是本次
  长测中的资源增长。

## 版本关闭

`0.6.0` 关闭后不再追加 SQLite、完整字段编辑或可视化 Rule 功能。下一阶段从
[DevelopmentPlan.md](../DevelopmentPlan.md) 的 `0.7-A` 内存/性能基线开始，再进入 SQLite 与完整
GUI 字段编辑。
