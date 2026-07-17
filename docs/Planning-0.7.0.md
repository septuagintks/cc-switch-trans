# ccs-trans 0.7.0 实施计划

## 文档状态

本文是 `0.7.0` 的开工合同，不描述已经发布的功能。当前实现与用户文档仍以 `0.6.0`、
`ccs-trans.config/v2` 和 [Design.md](Design.md) 为准。

| 项目 | 当前值 |
| --- | --- |
| 开发基线 | `0.6.0` tag，commit `f9fe5a8868c46bc7b982995cca03432ed96e27eb` |
| 主开发侧 | Windows |
| macOS 范围 | 共享合同冻结后的平台实现、构建、集成与候选验证 |
| 目标发行包 | `ccs-trans-0.7.0-Windows-x64`、`ccs-trans-0.7.0-macOS-arm64` |
| 开发期版本号 | `0.7-D` 开始写入 v3/SQLite 前显示 `0.7.0-dev`；最终发行提交去掉 suffix |

当前实施进度：`0.7-A1/A2`、`0.7-B` 与 `0.7-C` Windows 实现已完成。进程级 512 MiB budget、
move-only RAII lease、inflight/generation/control metrics、retired generation 生命周期，以及
request/Rule/response/logger staging 记账均已接入；
request header 单次解析、response 分段发送、budget-aware Rule allocator、logger lazy rendering 和有界
ControlExecutor 也已完成。Windows 当前 Release/warnings 均完成 `19/19` CTest；A2 的五档组合基准
满足默认配置门槛，当前 shared/tray integration passed。`0.7-A3` 的 macOS platform-local cURL 硬化、
Release 构建、integration、短负载和 profiler 已在 `b8fc353` 完成；正式退出仍被并行 Windows SQLite
提交的 AppleClang dead-constant warning 与默认 macOS `TMPDIR` symlink 测试夹具阻塞，不能声明完整
A3 passed。SQLite 3.53.3 官方 amalgamation 已按固定 hash vendoring 为独立静态 C 目标；macOS 同
source/options 的 Release build/probe passed，严格 warnings 与默认环境 CTest 仍待 Windows 修复后复验。

## 目标与不进入范围

`0.7.0` 完成三件事：

1. 在扩大 repository 和 GUI 前建立全进程资源预算，修补已识别的内存、generation、logger、
   control queue 和 macOS cURL 异常路径；
2. 把 Profile 与有序 Rule 从 `config.json` 迁入事务型 `profiles.db`，同时保留严格、可人工审计的
   应用设置文档；
3. 让 Win32 与 AppKit 主窗口完整编辑 Profile 字段、应用字段和当前 Profile 的 Rule 文本。

本版本不实现可视化 Rule builder、请求样例 diff、undo/redo、云同步、数据库加密、新协议、自动故障
转移、负载均衡或网络栈重写。可视化 Rule 和成熟编辑体验仍属于 `0.8.0`。

## 已冻结决定

| 主题 | `0.7.0` 决定 |
| --- | --- |
| 应用设置 | `~/.ccs-trans/config.json`，升级为 `ccs-trans.config/v3`，不再包含 `profiles` |
| Profile/Rule | 固定路径 `~/.ccs-trans/profiles.db`，路径不可由配置或 CLI 改写 |
| UI preferences | 继续独立存放在 `state/ui.json`，不进入 SQLite |
| SQLite 来源 | SQLite 官方 amalgamation，项目内固定版本静态编译；不使用机器上的偶然动态库 |
| SQLite 版本 | 官方 amalgamation 3.53.3（`3530300`），来源、archive/file hash 与 source id 固定在 `third_party/sqlite/NOTICE.md` |
| SQLite threading | `SQLITE_THREADSAFE=1`；repository 每次操作使用短生命周期连接，不跨线程共享 connection |
| journal | WAL、`synchronous=FULL`、foreign keys、2 秒 busy timeout；SQL 不进入请求热路径 |
| Profile 身份 | SQLite integer `profile_key` 是 GUI/repository 稳定身份；用户可见 `profile_id` 保持唯一且可重命名 |
| Rule 身份 | SQLite integer `rule_key` 稳定；`rule_id` 在所属 Profile 内唯一；数组顺序由显式 position 保存 |
| Rule options | 每条 Rule 保存一个 canonical JSON object；不为未来 Rule option 动态增加 SQL column |
| 迁移 | v2 导入必须由用户显式执行或在 GUI 明确确认，不在普通启动中静默改写 |
| downgrade | 成功迁移后不承诺 `0.6.x` 读取 v3/SQLite；原 v2 文件保留可审计备份 |
| runtime | 只消费已经完整加载并编译的 immutable snapshot；请求期禁止 SQL、文件 I/O 和 lazy Rule 查询 |
| 内存预算 | 初始默认 `runtime.max_inflight_bytes=536870912`（512 MiB），是上限而非预分配 |
| 预算耗尽 | 不持有未记账的大 buffer 等待；响应未开始时返回 HTTP 503，已开始的流记录错误并关闭 |
| control queue | 有界 FIFO，初始容量 64；mutating command 不合并，重复 refresh/status 可以 coalesce |
| 日志兼容 | 默认继续记录有序 `stream_chunk` 明细；不未经确认改成摘要、采样或 debug-only |
| 并发默认值 | 保持 `worker_threads=32`、`max_connections=64`；不先上 LTO/PGO |
| JSON | 继续使用现有 nlohmann JSON；没有基准证据时不更换库或重写 Rule 遍历 |
| 开发构建 | `0.7.0-dev` 不生成正式发行文件名，migration manifest 记录完整版本与 source commit |
| 平台顺序 | 共享 model/service/repository/descriptor 先完成，随后 Win32，再交接 AppKit |

SQLite 版本已从 2026 官方 download metadata、ZIP 和 `sqlite3.h` 三处交叉确认后冻结为 3.53.3，
不是从开发机缓存或记忆填写。该选择不改变本文已冻结的所有权、schema、事务和构建边界。512 MiB
预算默认值可在 `0.7-A` 候选基准后调整一次，但字段语义和“拒绝而非无界等待”不再改变。

## 0.6.0 性能审计映射

2026-07-16 macOS 侧在 clean `f9fe5a8` 上完成只读性能审计并通过 `.codex` commit `ff5dc81` 回传。
Windows 侧复核代码后得到以下覆盖矩阵：

| 审计项 | 原计划覆盖 | `0.7.0` 处理 |
| --- | --- | --- |
| `P1` SSE 每 chunk info 日志与 producer 渲染放大 | 部分；只计划了 producer staging 记账 | 纳入 `0.7-A1/A2`：固定基线、lazy logging、字段/body staging 预算和 A/B；默认完整 chunk 合同不静默改变 |
| `P1` request 重复 parse/`substr`/raw 重组与 response 完整 frame 副本 | 方向已覆盖，所有权拆分不足 | 纳入 `0.7-A2`：header 单次 parse、body ownership move、精确 reserve、response head/body 分开发送 |
| `P2` macOS cURL 每 chunk 临时 `std::string` | 未明确 | 纳入 `0.7-A3` 观察项；只有 P1 后 profiler/A-B 仍显示收益才改 shared byte-view callback |
| `P3` cancellation monitor 50 ms 重建数组 | 未明确 | 记录指标与 profiler 触发条件；64 连接内无证据时不重写 poll 架构 |
| `P3` macOS menu 每秒 status/startup 查询 | 未明确 | 保留 1 秒行为；idle CPU/wake-up 超过 `0.6.0` 基线时才做 event/coalescing |
| worker 32 / connection 64 | 已计划保持 | 冻结不变；50 路有界排队不是桌面档回退 |
| LTO/PGO | 未列入 | 不进入 `0.7-A`；先处理可测的日志和副本成本 |
| JSON 库/Rule 遍历 | 未计划更换 | 保持 nlohmann；1 MiB 非空约 1.9-2.0 ms、修改约 3.5-3.7 ms，暂无重写依据 |

`0.7-A0` 在改代码前固定 Windows 与 macOS 对照。除五档请求和 Rule matrix 外，记录
`stream_chunk`/总日志条数与字节、producer/writer CPU、RSS/private、TTFB/total、log queue/backpressure、
chunk 数与 payload bytes。Windows 不能把 macOS 结果外推为本平台 passed；每项 P1 优化先做单变量短
A/B，再组合运行跨版本门槛。

### SSE 日志兼容性决定

“完整日志”与有序增量 chunk 是现有可审计合同，`0.7-A` 默认保持每个 chunk 的 sequence、size 和按
body policy 截断/脱敏后的内容。优化顺序固定为：

macOS `0.6.0` mixed 证据为 `2,015,806` 个 chunk、`2,115,373` 条日志，约 95% 记录密度随 chunk
线性变化；当时 backpressure/writer failure 都是 0，所以这是明确的 CPU/allocation/存储优化候选，不是
现行正确性故障。

1. `Logger::enabled(level)` 与 lazy field builder，让被过滤事件不构造 header JSON、body staging 或
   `LogField` vector；
2. 只有启用 body logging 时才创建 chunk body preview，并纳入 inflight budget；
3. 预估并一次 reserve JSON line，减少 producer 端反复扩容；
4. 评估将 chunk 明细异步渲染到 writer，但不得把借用的 request/chunk view 越过生命周期；
5. 用 A/B 证明 CPU、allocation 或日志吞吐收益，且不增加 backpressure、TTFB 或丢失 sequence。

将 chunk 明细降为 debug/trace、采样或只保留 stream 汇总会改变“完整日志”语义，不在默认优化中
执行。未来若确有需求，只能新增一个明确的日志粒度配置，默认仍为完整，并同步 schema、README、
CLI、GUI、测试和迁移；不能偷偷改变 `logging.level=info` 的现有结果。

### Windows `0.7-A0` 基线

Windows 侧在未修改产品代码的最终 `f9fe5a8` Release 二进制上完成三轮五档短负载。配置均为
32 worker、64 connections、`log_body=false`；`git_dirty=false`，CLI SHA-256 为
`AAF78115606BA4C14EEC8F001D9DD5943396BC54EA625034E2660AB414CF6196`。以下为三轮中位数：

| Profile | added TTFB p50/p95 | proxy total p50 | log records/bytes | peak WS/private |
| --- | ---: | ---: | ---: | ---: |
| `smoke` | `0.364 / 2.015 ms` | `3.926 ms` | `162 / 87,929` | `12.36 / 4.52 MiB` |
| `desktop-8` | `7.387 / 3.821 ms` | `6,030.812 ms` | `1,017 / 323,357` | `12.61 / 4.75 MiB` |
| `desktop-16` | `8.240 / 6.946 ms` | `6,031.931 ms` | `2,009 / 597,915` | `14.18 / 6.66 MiB` |
| `mixed-16` | `8.383 / 8.264 ms` | `6,033.212 ms` | `2,033 / 595,987` | `15.05 / 7.91 MiB` |
| `stress-50` | `8.500 / 1,994.219 ms` | `2,002.474 ms` | `2,217 / 679,077` | `17.23 / 10.75 MiB` |

每档三轮合计均零 stream failure、零 logger backpressure/writer failure；mixed 的 Responses/Chat Usage
各 36/36 且都在流期间完成。mixed 中位数为 1,920 chunk、2,033 条日志、peak log queue 37,054 bytes，
直接复现日志密度与 chunk 线性相关。stress-50 的 max queue wait 中位数约 1.998 s，对应超过 32 worker
后的既有有界排队，不据此修改桌面默认值。

三轮原始 ignored JSON SHA-256：

```text
0.7-A0-windows-f9fe5a8-run1.json  35CD665D66FD64A4629B16B9F28242B91970AAF57BB34BE9A11ECE23A71A6E82
0.7-A0-windows-f9fe5a8-run2.json  9877EFF9922274F279ECC208333FD8A73203528D2018C173CF9EA23DC4676D92
0.7-A0-windows-f9fe5a8-run3.json  02E8347E40B3EA0D334015309005BB2FF26E57F5E519FB19198B12FC4086B90F
```

为覆盖默认完整 body 日志合同，另以 `log_body=true` 对 desktop-16/mixed-16 各跑三轮。两档均零失败、
零 backpressure/writer failure；中位数如下：

| Profile | added TTFB p50/p95 | log records/bytes | peak log queue | peak WS/private |
| --- | ---: | ---: | ---: | ---: |
| `desktop-16` | `7.024 / 18.873 ms` | `2,009 / 2,640,773` | `81,818 bytes` | `14.54 / 6.91 MiB` |
| `mixed-16` | `8.290 / 8.122 ms` | `2,033 / 2,638,872` | `60,362 bytes` | `15.27 / 7.99 MiB` |

事件数与 `log_body=false` 相同，日志字节约增至 4.4 倍，证明优化必须分别报告“事件固定成本”和
“完整 body 转义/存储成本”，不能只跑关闭 body 的场景。原始 ignored JSON SHA-256：

```text
0.7-A0-windows-log-body-f9fe5a8-run1.json  65A35459EDCF7E01D1DC6357CB21221E690548E9847C85905A26439881B24843
0.7-A0-windows-log-body-f9fe5a8-run2.json  10FF452CBF18791C5F8D742475481A7FB92BD342E84D7600A4BD88C7B74D1617
0.7-A0-windows-log-body-f9fe5a8-run3.json  BF48D1A8AE4CE66445A83A40D9F23109E97D952BB37E4792A8EA48556F02AD24
```

生成这些文件时旧 runner 将 annotated `source_ref=0.6.0` 解析为 tag object `39723fc`；同文件的
`git_commit` 正确记录 peeled source `f9fe5a8`。准备提交修正 `resolve_git_ref` 使用 `^{commit}`，后续
结果的 `source_commit` 直接记录 peeled commit；不篡改已生成的原始证据。

同一 Release benchmark 以 25 iterations 完成三轮 1 KiB/100 KiB/1 MiB × 0/1/8/32 Rule matrix。
1 MiB、32 Rule 中位数：未修改 total `3,529.80 us`，修改 total `4,933.80 us`，其中 Rule apply
`21.04 us`；空 pipeline 仍为零 parse/serialize。原始 JSONL SHA-256：

```text
0.7-A0-windows-rule-f9fe5a8-run1.jsonl  4FD739EB5590500D9195B346A875FBFB54745264EB9C44FA329696E0007C294A
0.7-A0-windows-rule-f9fe5a8-run2.jsonl  44AFA5FB04093323EA39F7E287690CA79F5DAB8D196576884E09833C2660FD95
0.7-A0-windows-rule-f9fe5a8-run3.jsonl  2CDB5C31DDCAA69B95164C201B121150F541FD96F095D556AA67DC81A2D68E71
```

Windows parse/serialize 数字高于本次 macOS 审计，因此两个平台分别保留本机对照，不互相外推。

### Windows `0.7-A2` 结果

Windows 在 exact source `629e0590666d82f9a51f962f72ec70caf44a943b` 上完成 fresh Release/warnings
构建、两套 `17/17` CTest、shared integration 和 tray integration。默认 `log_body=false` 的三轮中位数
如下；五档均零请求失败、零 logger backpressure/writer failure，drain 后 retired generation、control
task、logger queue 和 inflight current 均回到零：

| Profile | added TTFB p50/p95 | proxied total p50 | log records/bytes | peak inflight |
| --- | ---: | ---: | ---: | ---: |
| `smoke` | `0.376 / -7.466 ms` | `3.772 ms` | `162 / 88,275` | `83,238 bytes` |
| `desktop-8` | `6.474 / 7.196 ms` | `6,027.853 ms` | `1,017 / 331,737` | `27,688 bytes` |
| `desktop-16` | `7.350 / 17.974 ms` | `6,030.136 ms` | `2,009 / 606,345` | `44,565 bytes` |
| `mixed-16` | `6.987 / 7.273 ms` | protocol mix | `2,033 / 604,427` | `48,162 bytes` |
| `stress-50` | `8.873 / 1,995.714 ms` | `2,001.868 ms` | `2,217 / 684,663` | `87,174 bytes` |

相对 A0，smoke 只增加 `0.012 ms`，desktop-8/16 分别改善 `0.913/0.890 ms`，满足“5% 或
0.25 ms 取较宽者”的门槛。stress-50 仍表现为超过 32 worker 后的预期有界排队。完整 body 日志三轮
中位数为 desktop-16 `9.145 ms`、mixed-16 `7.412 ms`；同一会话、同一 runner 重建的 exact A0 可执行
文件对照分别为 `7.905 ms`、`7.142 ms`。desktop-16 的 `1.240 ms` 差值保留为 A3 profiler 观察项，
但两组事件数完全一致，A2 writer time 中位数仅为 `12,933/15,019 us`，仍为零 backpressure/failure，
且 WS/private 峰值没有持续增长，因此不重新打开已验证的 request/logger ownership 合同。

1 MiB、32 Rule 三轮中位数为 modified `5,230.68 us`、unchanged `3,661.68 us`、unmatched
`3,590.56 us`；空 pipeline 继续零 parse/serialize。实际 DOM allocation 记账使 modified 相对 A0
增加约 6%，这是资源上限准确性成本；Rule apply 本身仍约 `18.52 us`，不更换 JSON 库或引入第二套
估算记账。

原始 ignored evidence SHA-256：

```text
0.7-A2-windows-629e059-run1.json           A3F56B315E64080DCC2D49C97ABCCCF18EB619999440DB29B28246F7591B26E6
0.7-A2-windows-629e059-run2.json           BCF77896F149E3114C3DE721C17C3085EFE3519DF3589456D23093E740854A05
0.7-A2-windows-629e059-run3.json           FF758DB60FD7CE3608FE1746493E30D630B9761FBF969C8949B819F55A79DCB1
0.7-A2-windows-log-body-629e059-run1.json  40B66596301AFD0A5BC943574795AA10F8B9919BFD43CD6379C9C9906DC3BF76
0.7-A2-windows-log-body-629e059-run2.json  64C8F00EECA6BB2868961099CC98ACCF33276E98356FCCED22A0D80318FCA1B4
0.7-A2-windows-log-body-629e059-run3.json  C3E636998EED22334C3CDF867414BC922E503B6D2CE85C9A3D3F8D09E2BCFB11
0.7-A2-baseline-f9fe5a8-run1.json          B62E1AFE564E15764BB5D8716E0273C82492DC76D05B57BAEE53D08F56832825
0.7-A2-baseline-f9fe5a8-run2.json          A403D5AB096FECDB65D1E7131127673E99A2A2F17A5386A90078A9338C389C10
0.7-A2-baseline-f9fe5a8-run3.json          6524A4F9EF22BFD49E1E35B8D1F0856872A6FE38AB10E57C1B6A11C9AAD655BD
0.7-A2-windows-rule-629e059-run1.jsonl     CE1B0D45A393125843D4EEC6FACBABF62470229C6D70A4F1C767E6427B9ED2F9
0.7-A2-windows-rule-629e059-run2.jsonl     44803C17E3A0E02DC72F4E459F6265661E8102229C48B4D7D74EB70DD527BE1A
0.7-A2-windows-rule-629e059-run3.jsonl     E256A0FA10E3DF8D38178F18A7F6E4289D58F5DC97C9B351CC8DC7E804C92A31
```

### macOS `0.7-A3` 结果

macOS 从委派基线 `62deb98` 开始，在并行 Windows SQLite store 提交 `4febba8` 上重放并完成 exact
source `b8fc353e5b5c9d9ba4a00016cb8bba1e5e5ed518`。产品提交只修改
`src/transport/macos/curl_transport.cpp` 和
`tests/integration/run_macos_transport_resource_integration.py`，已由默认 GPG key
`3DF828FDD419996E11B2FD2881BFB01482975987` 签名并推送。环境为 macOS 26.5.2 build 25F84、
arm64、Command Line Tools SDK 26.5、Apple Clang 21.0.0、CMake 4.4.0 和 Ninja 1.13.2；完整 Xcode/
`xctrace` 不可用，因此 prerequisite 记为一项 blocked，而不是全通过。CLI/app 均为 arm64-only，链接
selected SDK `/usr/lib/libcurl.4.dylib`，开发期版本仍按计划为 `0.6.0`；CLI/app executable SHA-256
分别为 `9E1AB62872B7A741E920E19710D2DB9D8D982C22C35285D869D121F71091B728` 和
`E4B5EDCB556222DC480C27A479B8AD9FB2D1557539CD7D4B7ECC61FCBA0C225C`。

构建与测试状态严格区分如下：

- fresh Release 全构建 `88/88` passed，第二次构建为 `ninja: no work to do.`；
- fresh warnings-as-errors 在 Windows-owned `src/storage/sqlite_profile_store.cpp:27` failed：
  `kTemporaryPositionOffset` 只有定义、没有引用，AppleClang 报 `-Wunused-const-variable`。仅把这一个
  warning 降级的诊断构建完成全部 target，且 `17/17` CTest passed，证明未发现第二个严格编译阻塞；
  该诊断不计作正式 warnings passed，正式 warnings CTest 为 not run；
- Release 默认环境首次 CTest 为 `16/17`：SQLite store test 使用 `/var/...` 临时根，而 macOS
  `/var` 是指向 `/private/var` 的 symlink，`SQLITE_OPEN_NOFOLLOW` 按安全合同拒绝该路径。保持产品
  `NOFOLLOW` 不变，以 canonical `/private/tmp` 运行完整 CTest 为 `17/17` passed；最小共享修复是 test
  fixture canonicalize `temp_directory_path()`；
- shared integration、macOS proxy integration、新 cURL resource integration 与 menu/main-window
  integration 均 passed。resource integration 验证 64 KiB aggregate response-header cap 稳定返回 502、
  连续失败后的 slot 复用、SSE trailer 不重复发送 response head、客户端断连归类 cancellation，以及
  stop 后 inflight/generation/connection/control/logger queue current 全部归零。

cURL platform-local 实现现在在 libcurl C callback 内 containment C++ exception；区分 transport allocation
failure 与用户 callback exception；stream 已开始后以 `ProxyError` 关闭而不写第二个 HTTP response。
response header 和过滤期第二份 payload 都在 append/copy 前取得 inflight lease，新 status 清理旧
header/body 时同步回滚 lease。easy/multi/slot/header list 使用 owner/RAII，pool 预留回收容量；
`curl_multi_remove_handle` 失败会 poison pool，连续失败时有界放弃仍 attached 的 handle pair，且
poison 使用 `notify_all()` 唤醒全部 waiter。没有把 SSE minimum chunk allowance 单边加到 macOS，
避免与 WinHTTP 合同分叉。

默认 `log_body=false` 三轮五档均零请求失败、零 upstream failure、零 logger backpressure/writer
failure；以下为三轮中位数。当前没有同一工具链、同一 runner 的有效 macOS `0.6.0` 三轮 A0 文件，
因此只报告本机绝对值，不用 Windows 或陈旧 `0.5.0` 文件做正式 5% 跨版本判断。

| Profile | added TTFB p50/p95 | proxied total p50 | log records/bytes | peak inflight/RSS |
| --- | ---: | ---: | ---: | ---: |
| `smoke` | `0.170 / 0.407 ms` | `3.108 ms` | `161 / 86,078` | `88,573 bytes / 9.72 MiB` |
| `desktop-8` | `-1.310 / 1.481 ms` | `6,341.293 ms` | `1,018 / 334,044` | `25,995 bytes / 9.83 MiB` |
| `desktop-16` | `1.051 / 0.664 ms` | `6,342.753 ms` | `2,010 / 608,642` | `49,415 bytes / 10.75 MiB` |
| `mixed-16` | `2.872 / 0.734 ms` | `6,333.029 ms` | `2,034 / 606,718` | `48,849 bytes / 11.03 MiB` |
| `stress-50` | `4.647 / 2,088.194 ms` | `2,106.351 ms` | `2,218 / 687,265` | `97,024 bytes / 12.52 MiB` |

六个 short 文件合计 `486/486` proxied request 成功；六次 mixed 的 Responses/Chat Usage 各 `12/12`
且都在 SSE 期间完成。stress-50 p95 对应超过 32 worker 后约 2.095 秒的有界排队。`log_body=true`
三轮中位数为 desktop-16 `4.565/3.377 ms`、mixed-16 `-0.786/1.733 ms` added p50/p95；事件数保持
`2,010/2,034`，日志字节增至约 4.36 倍，peak inflight 约 `93 KiB`，仍无 failure/backpressure。
一个 stress periodic snapshot 在 server stop 前尚有 `35` 条、`13,016 bytes` log queue/inflight，不能把
short periodic snapshot 写成全部 drain；独立 stop evidence 补齐了最终归零证明。

60 秒 mixed resource 诊断实际运行 66.342 秒/10 轮：`160/160` stream 与两类 Usage 各 `120/120`
成功，400 request/upstream 生命周期配对，`20,148/20,148` log records 写完，所有 current 与失败
指标为零。RSS `8.53 -> 11.14 MiB`，fd/thread 后半程斜率为零；短窗口 RSS 后半程折算
`+2.25 MiB/h` 不能外推为泄漏。120 秒 menu idle CPU 为 `0.000%`、RSS `+32 KiB` 且后半程斜率
为零，fd 恒 45、idle 日志零增长，stop current 全零。三轮 Rule matrix 各 30 条/25 iterations；空
pipeline 仍零 parse/serialize，1 MiB/32 Rule 三轮中位数为 modified total p50 `3,206 us`、unchanged
`1,907 us`、unmatched `1,901 us`，modified 的 parse/rule/serialize mean 为
`1,896.76/12.08/1,326.36 us`。

30 秒 mixed `sample` 中，`response_body_callback` inclusive 约 `234/19,704` samples（单核时间保守
上界约 1.19%，包含日志与 socket send），callback 顶层仅 23 个；临时 string allocation 路径为 5 个，
约 0.025%，不是可见 hotspot。cancellation monitor 的 19,704 个 samples 中，18,196 个阻塞在 `poll`、
1,500 个在 condition timed wait，非等待路径约 8 个，array rebuild/allocation 只有 4 个。60/120 秒 menu
idle 均未观察到 CPU/内存回退；menu sample call graph 为空，且没有 `xctrace` wake-up 计数，所以结论是
“没有达到重写触发条件”，不是“wake-up 为零”。保持 shared string callback、50 ms cancellation poll
和 1 秒 menu polling 架构不变。

以下项目没有被 A3 证据覆盖，不能写成 passed：easy/multi/slist/remove/OOM 与 throwing callback 的
deterministic fault injection；连续 remove failure 下被有界放弃的 attached handles 归零。APPLE-only
fault target 需要根 CMake 接线，留给 Windows/shared 集成。整个 `0.7-A` 也保持 open：两平台仍需在
SSE headers 前统一最小 chunk allowance；`Logger::drain()` 应先释放 batch lease 再发布 flushed
sequence；server 需处理 logger 拒绝的有界 emergency error path、消除 release-before-free 窗口；
大量 header 的 shared filter O(n^2) 仍待后续。没有 profiler 证据支持现在改 byte-view 或 polling。

ignored evidence 位于 `benchmark-results/0.7-A3-macos-b8fc353-final/`，包含 prerequisite、6 个 short
JSON、3 个 Rule JSONL、4 个 soak/profile JSON、两组 PID/sample/vmmap，共 20 个文件；逐文件 hash 在
`SHA256SUMS.txt`，该 manifest SHA-256 为
`3843B03063F2AF6A1266E3152531AFCDD7FA067C053D079A50FAB52B491D89AE`。原始 JSON/sample/log/body 不进入
Git。A3 platform-local 实现和可观察资源路径完成；在 Windows 删除死常量、修正 temp fixture 并由
macOS 重跑正式 warnings/default CTest 前，A3 退出条件仍为 blocked。

## 目标模型与依赖边界

`0.6.0` 的 `ConfigDocument` 同时表示 JSON 文件和完整运行配置。`0.7.0` 将文件格式、编辑模型与运行
模型分开：

```text
ApplicationConfigDocument v3       SqliteProfileStore
        | application settings            | ordered Profiles/Rules
        +------------------+---------------+
                           v
                  ConfigurationSnapshot
             application + ordered profiles + revision
                           |
                    RuntimeCompiler
                           |
                immutable RuntimeSnapshot
```

计划中的共享类型：

- `ApplicationConfigDocument`：只负责 v3 应用字段 codec；
- `ProfileKey` / `RuleKey`：只用于 repository 与 presentation，不进入 URL 或日志 cardinality；
- `RepositoryRevision`：应用源精确字节 token 与 SQLite revision 的组合 token；
- `ConfigurationSnapshot`：应用设置、按 position 排序的 Profile/Rule 和 revision；
- `CompositeConfigRepository`：组合应用 store、SQLite store、跨进程锁和恢复协调器；
- `ConfigurationEditingService`：持有 snapshot draft 及其 base revision；
- `RuntimeCompiler`：接收只读 `ConfigurationSnapshot`，不感知 JSON 文件或 SQLite。

`ConfigDocument`/v2 codec 在迁移测试完成前保留为只读导入模型，不能继续作为 `0.7.0` 的主
repository 接口。presentation header 不得 include SQLite、nlohmann JSON、Win32、AppKit 或 mutable
RuntimeSnapshot 类型。SQLite C API 只出现在 `src/storage` 与对应测试。

## 0.7-A 资源合同

### 全进程 inflight budget

新增进程共享 `InflightMemoryBudget`。所有增长先取得 RAII lease，失败时不得先分配再记账。预算统计
的是项目可控制的动态 payload/staging，不声称等于操作系统 RSS。至少覆盖：

- raw HTTP header/body 和解析后仍被持有的 request body；
- Rule JSON DOM 的 budget-aware allocation；
- rewritten request body；
- non-streaming upstream response body；
- response framing/send staging；
- SSE 当前 chunk 的有界 staging，不累计完整流；
- body-log producer staging 与进入 logger queue 前的渲染结果。

优先通过 move、scatter/gather 或缩短生命周期消除重复副本，再为确实并存的 buffer 分别记账。单请求
request/response limit 与 logger queue capacity 继续存在，但不能替代全进程预算。预算允许小于单请求
上限；这表示极端大请求可能因当前进程资源而收到 503。

HTTP 接收改为一次解析 header metadata，并把 header storage 与 body storage 作为明确 owner 传给业务
层；不得为了兼容额外 JSON separator 用两个 `substr` 重组完整 raw request，也不得在业务层再次从 raw
复制 body。读取已知 `Content-Length` 前精确检查 overflow、body limit 和 budget，再 reserve 最终容量。
`HttpResponse::body` 发送时构造小型 response head 并依次 `send_all(head)`/`send_all(body)`，不拼接完整
frame；错误响应使用同一发送路径。优化后仍保持现有严格 Content-Length、额外 separator、query、
header filter 和客户端中断测试。

Rule pipeline 引入专用 `RuleJson` 类型和可继承预算 owner 的 allocator/resource；Rule factory 与
`CompiledRule` 统一消费该类型，serializer 写入可扩容的 budgeted output sink。每次实际
allocation/deallocation 更新 lease，禁止仅按 `body.size() * multiplier` 估算 DOM，因为大量短 token 的
JSON 可以产生远高于原文大小的节点开销。配置 codec 使用的 JSON DOM 不进入请求热路径，保持独立。
`0.7-A1` 先以内置 512 MiB 默认值和测试注入实现预算，不修改 v2 文件；`0.7-D` 再把同一语义暴露为
v3 的 `runtime.max_inflight_bytes`。

稳定行为：

1. 已知 `Content-Length` 无法取得 lease 时，在读取 body 前返回 HTTP 503；
2. Rule DOM 或 rewrite 无法扩容时，不访问 upstream，返回 HTTP 503、`server_error`，日志 reason 为
   `inflight_budget_exhausted`；
3. non-streaming response 在发送 header 前耗尽预算时返回本地 503；
4. SSE 在开始前保留最小 chunk allowance，运行中不可恢复的预算失败关闭该流并记录 request id；
5. 预算拒绝立即计数，不在持有已有大 buffer 时排队等待其他请求释放。

新增低 cardinality metrics：

```text
inflight_budget_bytes
current_inflight_bytes
peak_inflight_bytes
inflight_budget_rejections
current_generation_requests
peak_generation_requests
current_retired_generations
peak_retired_generations
current_control_tasks
peak_control_tasks
control_tasks_rejected
control_tasks_coalesced
```

generation 在 swap 时由 current 转为 retired，最后一个 request/shared owner 释放时递减；不能按 Profile
或 generation id 创建 metric label。连续 reload 与长 SSE 测试结束后，retired generation、active
logger、连接、worker、control task 和所有 inflight lease 必须回到零。

### 已识别硬化

- `ControlExecutor` 增加容量、状态 snapshot 和拒绝结果；stop 仍 drain 已接受任务并 join；
- refresh/status 可只保留一个待执行实例，Apply/Stop/Quit 等动作保持 FIFO 且绝不静默丢弃；
- logger 在 allocation failure 下保持 queue、pending counters 和 metrics 一致；error 事件保留有界应急
  记录路径，不能为完整 body 绕过预算；
- macOS cURL easy handle/slot/header list 使用完整 RAII，覆盖构造失败、callback 提前停止与 OOM；
- macOS response header 聚合增加显式上限，WinHTTP/cURL 都在 append 前检查 body budget；
- allocation fault injection 只进入测试 adapter，不在发行 CLI 暴露。

`0.7-A3` 先测 cURL callback 临时字符串的 allocation/CPU 占比。只有 P1 日志/body 副本优化后该项仍是
可见热点，才把 `UpstreamTransport::ChunkCallback`、两平台 transport、server sender、mock 与 integration
一起改成同步 `std::span<const std::byte>` 或等价只读 byte view；callback 返回后 view 立即失效，任何
异步日志都必须复制到已记账 storage。禁止只改 macOS 实现造成平台合同分叉。

cancellation monitor 增加 poll cycle、entry copy/build time 和 watched socket current/peak 测量；menu
status 增加 idle timer callback 与实际状态变更计数。只有 profiler 指向该路径，或 idle CPU/wake-up、
8-16 路延迟/CPU 相对 `0.6.0` 基线明显回退时，才实施 buffer reuse/event notification。P3 观察项不
阻塞 SQLite/GUI，也不能以“理论更快”为理由扩大本版本重构范围。

## 0.7-B SQLite 依赖与数据库合同

### 构建策略

新增 `third_party/sqlite/` 和独立 `ccs-trans-sqlite3` 静态目标。CMake 项目启用 C 与 C++，SQLite C
source 不伪装为 C++ 编译。至少验证以下 compile definitions：

```text
SQLITE_THREADSAFE=1
SQLITE_DQS=0
SQLITE_DEFAULT_FOREIGN_KEYS=1
SQLITE_ENABLE_API_ARMOR
SQLITE_LIKE_DOESNT_MATCH_BLOBS
SQLITE_OMIT_DEPRECATED
SQLITE_OMIT_LOAD_EXTENSION
```

不得启用运行时 extension loading、FTS 或项目未使用的可选模块。SQLite source/version/hash、public
domain notice 和项目实际 compile definitions 写入第三方说明，并进入两平台固定包白名单。

`0.7-B` 取证与 Windows 验证结果：

- 官方 URL：`https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip`；archive size
  `2,945,929` bytes；官方与本地一致的 SHA3-256 为
  `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`；本地 SHA-256 为
  `646421e12aac110282ef8cc68f1a62d4bb15fc7b8f09da0b53e29ee690500431`；
- header 确认 `SQLITE_VERSION_NUMBER=3053003`，source id 为
  `2026-06-26 20:14:12 d4c0e51e4aeb96955b99185ab9cde75c339e2c29c3f3f12428d364a10d782c62`；
- 只 vendor `sqlite3.c/.h/sqlite3ext.h`，不包含 shell；`NOTICE.md` 保存 file hash、compile definitions
  与官方 public-domain 声明，Windows/macOS package whitelist 均已接入；
- fresh Windows Release 与 warnings-as-errors 全构建无项目/第三方 warning，两套 CTest 均
  `18/18`；dependency probe 验证版本/source id、七个 compile option、serialized threading、默认 FK、
  JSON、DQS=0、API armor 和静态链接；
- Windows package + static verifier passed；macOS package scripts 通过 shell syntax，但同 source/options
  的真实 Apple Clang/system platform build 尚未执行，不能声明 macOS passed。

每次 repository operation 新建 connection、配置、执行事务并关闭。初始运行参数：

```text
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 2000;
PRAGMA wal_autocheckpoint = 256;
PRAGMA journal_size_limit = 16777216;
PRAGMA mmap_size = 0;
PRAGMA trusted_schema = OFF;
```

连接必须确认实际 journal mode；不能静默从 WAL 降级。普通 commit 后允许 PASSIVE checkpoint；备份或
干净维护时仅在没有其他 writer 的情况下使用 TRUNCATE。所有 statement、transaction、connection、
backup handle 和错误字符串都由 RAII owner 管理。

新库固定 4096-byte page，`max_page_count=65536`，把主数据库限制在 256 MiB；load 时同时执行
`sqlite3_limit`，至少把 SQL text 限制到 1 MiB、column/variable/attached database 限制到项目实际需要。
Rule `options_json` 单条不超过 1 MiB，单 Profile canonical Rule 文本不超过 4 MiB，完整 Profile/Rule
逻辑 payload 不超过 64 MiB。SQLite dependency probe 必须验证 `json_valid` 可用；任一上限拒绝都转换为
稳定 repository error，不能依赖 SQLite 默认的 GiB 级上限。

### schema v1

schema 采用显式 migration version，不直接依赖 `PRAGMA user_version` 作为业务 revision：

```sql
CREATE TABLE repository_meta (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_version INTEGER NOT NULL CHECK (schema_version >= 1),
  revision INTEGER NOT NULL CHECK (revision >= 0),
  migrated_from_sha256 TEXT
);

CREATE TABLE profiles (
  profile_key INTEGER PRIMARY KEY AUTOINCREMENT,
  profile_id TEXT NOT NULL COLLATE BINARY UNIQUE,
  position INTEGER NOT NULL UNIQUE CHECK (position >= 0),
  enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),
  protocol TEXT,
  local_request_path TEXT,
  local_usage_path TEXT,
  upstream_base_url TEXT,
  upstream_request_path TEXT,
  upstream_usage_path TEXT
);

CREATE TABLE rules (
  rule_key INTEGER PRIMARY KEY AUTOINCREMENT,
  profile_key INTEGER NOT NULL REFERENCES profiles(profile_key) ON DELETE CASCADE,
  rule_id TEXT NOT NULL COLLATE BINARY,
  position INTEGER NOT NULL CHECK (position >= 0),
  enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),
  type TEXT NOT NULL,
  options_json TEXT NOT NULL CHECK (json_valid(options_json)),
  UNIQUE (profile_key, rule_id),
  UNIQUE (profile_key, position)
);
```

另设 `schema_history`/`migration_history` 保存 schema migration id、来源 schema/hash 和完成时间，不
保存原始配置全文。position 连续性、Profile/Rule 数量、id/path/URL/Protocol/descriptor 等语义由共享
validator 在事务提交前和 load 后同时校验；数据库 CHECK/UNIQUE/FK 是第二道防线。

Profile 上限仍为 128、Route 上限 256、每 Profile Rule 上限 64。`options_json` 必须是 canonical JSON
object，单条和组合 snapshot 均设置显式大小上限。数据库损坏、unsupported schema 或完整性检查失败
时禁止自动创建空库覆盖原文件。

`AUTOINCREMENT` 用于保证已删除的 internal key 不被未来对象复用；它不是用户可见序号。Profile/Rule
重排在一个 transaction 内先把受影响 position 移入不会碰撞的临时区，再写回从 0 开始的连续位置，
不能直接交换两个受 UNIQUE 约束的值。commit 前后都验证连续性和 stable key 未变化。

`0.7-C` Windows store 已实现：

- `AppPaths::profiles_database` 固定为应用根下的 `profiles.db`；public storage header 不暴露 SQLite C
  API 或 nlohmann 类型，SQLite handle、statement 和 transaction 只存在于实现文件；
- 每次 open/load/save/verify 使用短生命周期 FULLMUTEX connection，逐连接设置并确认 WAL、FULL
  synchronous、FK、busy timeout、checkpoint、journal、mmap、trusted schema、page count 和 SQLite
  limits；load/save 在变更前执行 integrity/FK/schema 检查；
- bulk snapshot transaction 覆盖 Profile/Rule create/read/update/delete、rename/reorder 和 stable key；旧
  id/position 先移入保留临时区，整个 snapshot 验证完成后 revision 只增加一次；完全 no-op 不增加；
- 输入执行 Profile/Route/Rule、canonical options、1 MiB/4 MiB/64 MiB 与数量限制；磁盘中的非 canonical
  JSON、非连续 position、未知 schema object 或语义非法数据按 corruption/unsupported schema 拒绝；
- failure tests 覆盖 FK/UNIQUE/JSON CHECK、stale、writer busy、未知 key、超限、损坏字节、未来 schema、
  外来 schema object、AUTOINCREMENT 不复用和失败后内容/revision 不变。

fresh Windows Release 与 warnings-as-errors 全构建通过，最终串行 CTest 均 `19/19`；Release shared
integration 与 tray/main-window integration passed。macOS 编译与平台文件系统/WAL 验证仍待回传。

## 0.7-D 组合提交、revision 与恢复

### revision

应用设置 revision 沿用当前 ConfigStore 的严格语义：保存加载时“文件是否存在 + 完整 source bytes”
作为 opaque token；v3 文件仍有 4 MiB 上限，不能只以时间戳、size 或 hash 代替精确比较。Profile
revision 使用 `repository_meta.revision`。draft 保存以下 base token：

```text
RepositoryRevision { application_source_token, profile_revision }
```

任何一部分变化都返回 `Stale`，不进行 last-writer-wins。数据库 revision 每个成功 Profile/Rule
transaction 只增加一次。外部手工编辑 config 即使没有 revision 字段，也会改变 source token。

### 跨文件 commit coordinator

应用设置与 Profile 可能在同一次 GUI Apply 中变化，JSON 原子 rename 与 SQLite transaction 无法由文件
系统天然组成一个事务。组合 repository 使用 `state/repository.lock` 和 durable recovery journal：

1. 获取跨进程 lock，恢复或拒绝任何未完成 journal；
2. 重读 config source bytes 与 DB revision，比较 draft base token；
3. 写并验证新的 v3 config 临时文件；
4. `BEGIN IMMEDIATE`，应用 Profile/Rule delta 并产生 target DB revision，但暂不 commit；
5. 在 `state/repository-transaction/` 写入 old/new config、SHA-256、old/target DB revision 和阶段，确保落盘；
6. 原子替换 config；
7. commit SQLite；
8. 精确校验最终 config bytes 与 DB revision 后删除 journal。

恢复规则：

- config 与 DB 都是 old：删除未生效 journal；
- config 是 new、DB 是 old：恢复 old config；
- config 是 old、DB 是 target：从 journal 原子写回 new config，完成 roll forward；
- config 与 DB 都是 target：删除已完成 journal；
- config 不精确等于 journal old/new，或 DB revision 不是 old/target：返回 `RecoveryRequired`，拒绝启动
  服务，不覆盖疑似外部修改。

“确保落盘”要求 Windows 对文件执行 `FlushFileBuffers` 并使用 write-through replace，macOS 对文件执行
`fsync` 并在 rename 后同步父目录；journal manifest 自身也以 temporary + atomic replace 更新。进程内
若 config 已替换但 SQLite commit 返回失败，必须在释放 repository lock 前恢复 old config；测试在上述
每个阶段注入中断并从全新进程执行 recovery。

恢复判断以 journal 保存的 old/new 实际字节做精确比较，SHA-256 只用于 manifest 审计和快速诊断。
共享 SHA-256 helper 必须通过标准 known-answer vectors，但不作为身份认证或 stale 正确性的唯一依据。

仅修改 DB 时使用单 SQLite transaction；仅修改 config 时继续使用 verified temporary + atomic replace。
两者仍共用 repository lock 和 composite stale token。repository commit 成功后的 runtime reload 是独立
阶段；失败继续返回 `SavedPendingRuntimeApply`，不能回滚已经持久化且可能已被其他进程读取的数据。

全新用户目录创建 v3 config 与 schema v1 同样属于跨文件初始化，必须复用 coordinator journal；进程
中断后只能恢复到“两个文件都不存在”或“两个文件都完整可加载”，不能留下 v3 config + 缺失 DB 或
空 config + 已存在 DB 的半初始化状态。

## 0.7-D v2 迁移

新增唯一命令族，不提供 legacy alias：

```text
ccs-trans storage status
ccs-trans storage migrate
ccs-trans storage verify
```

普通 `run`、Profile/Rule 管理或 GUI 发现 v2 数据时返回稳定 `MigrationRequired`；tray/menu 可提供调用
同一 migration service 的显式确认入口。全新用户目录可自动创建空 v3 + schema v1，因为不存在可覆盖
用户数据。

迁移步骤：

1. 持有 repository lock，完整 parse/validate v2，并编译等价 RuntimeSnapshot；
2. 计算原始 v2 bytes SHA-256，在 `state/migrations/` 写只读备份与 manifest；
3. 在 `profiles.db.migrating` 创建 schema，把全部 Profile/Rule 作为一个 transaction 导入并 round-trip；
4. 生成只含 application settings 的 v3 config；
5. 写 migration journal 后切换 DB/config；每个断电窗口都可由 source hash、migration marker 和目标
   revision 确定继续或回滚；
6. 重新加载组合 snapshot，与迁移前语义比较后才标记完成。

临时数据库在切换前执行 FULL/TRUNCATE checkpoint 并关闭全部 connection，确认没有未合并的
`profiles.db.migrating-wal`/`-shm`；之后再把单个完整数据库文件原子改名为目标。不能只 rename WAL 主
文件而遗漏 sidecar。目标已存在时绝不使用 replace-existing。

存在非空 `profiles.db`、来源 hash 不匹配、备份不可写、磁盘写满、lock busy 或任一校验失败时不覆盖
任何现有数据。重复执行同一来源返回 `AlreadyMigrated`；不同来源要求用户先处理冲突。成功迁移保留
原 v2 备份，不自动删除或上传。

## 0.7-E 共享编辑与 CLI

新增平台无关 field descriptor：stable key、scope、input kind、required/optional、范围、枚举、显示名 key
和 runtime apply impact。至少覆盖：

- listener host/port；
- worker threads、max connections、request/response/inflight limits 与 metrics interval；
- 全部 timeout；
- logging path、level、body、redaction、body/queue/total limits 与 flush interval；
- Profile protocol、local request/Usage path、upstream base/request/Usage path 与 enabled。

ViewModel command 使用 typed field value，不让 Win32/AppKit 拼配置 key 或重复数值/URL 校验。Profile 列表
以 `profile_key` 保持 selection，重命名只改变 `profile_id`。当前 CLI 的 `config/profile/rule` 一命令一
动作语义保持不变，底层切换到 composite repository；输出不得暴露 internal key、SQL 或敏感值。

Rule 文本使用唯一 canonical draft schema：

```json
{
  "schema_version": "ccs-trans.rules/v1",
  "rules": [
    {
      "id": "remove-image-gen",
      "enabled": true,
      "type": "remove_tool",
      "options": {"tool": "image_gen"}
    }
  ]
}
```

Rule array 顺序有语义；JSON object key 顺序没有语义，Format 可按固定顺序重写。UTF-8、LF、2-space
indent，单 Profile 文本上限 4 MiB。parser 返回 line/column、Rule id/type/option 和 descriptor 错误；
disabled 的未知但语法合法 Rule 可 round-trip，启用未知 Rule 仍被 RuntimeCompiler 拒绝。编辑器只操作
draft，不自动保存，不读取生产日志或请求 body。

文本 schema 不暴露 `rule_key`。同一 `rule_id` 的 option、enabled、type 或 position 修改保留 internal key；
删除 id 并增加新 id 视为删除/新增，分配新的 key。不得用位置或内容相似度猜测“重命名”，避免错误
关联 `0.8.0` 的 selection/undo 状态。

## 0.7-F Windows GUI

Win32 主窗口改为三个稳定视图：Profiles、Rules、Settings。Profiles 使用列表与字段面板；Rules 使用
当前 Profile 的原生 multiline text editor、Format 与校验结果；Settings 按 descriptor 创建 typed
controls 和滚动布局。避免 section/card 套叠，不为每个字段创建独立顶层窗口。

Windows 同阶段完成轻量主题优化，macOS 不随本工作包改视觉主题。实现继续使用系统 Win32、DWM 和
Common Controls；优先通过 owner-draw background/border、圆角宿主和无边框 child control 组合实现，
不引入 WebView、Qt 或常驻第三方主题运行时。除滚动条、文本 caret、分隔线等本质线性元素外，可见的
窗口、导航项、按钮、输入容器、列表 selection、tooltip、菜单和状态面都不得保留完全直角的矩形
轮廓；原生 edit/list view 若不能直接圆角，放入带 clipping/padding 的圆角宿主，而不是绘制方形边框。

主题使用一个共享 Windows palette/metrics 对象管理 radius、padding、border、surface、text、focus 和
状态色，支持 Windows 11 light/dark 与高对比模式。圆角不得缩小可点击区域、截断长 URL/Unicode、破坏
DPI scaling 或造成列表滚动时重绘抖动。优先复用系统 brush/font/icon，缓存按 DPI/theme 分代并在窗口
销毁时释放；只有实测 GDI 无法满足抗锯齿或后续动画时才局部使用系统 Direct2D，不引入自带渲染引擎。

共享 ViewModel 负责 draft、busy、dirty、stale、Apply/Discard/Reload、migration required 和
SavedPendingRuntimeApply；Win32 只负责 HWND 生命周期、DPI、focus、accessibility 与消息派发。单字段更新
不得重建整个窗口或 Profile 列表。完成默认/最小尺寸、100%/150%/200% DPI、light/dark、长 URL、
Unicode、Tab loop、Narrator label 与 128 Profile 列表验证。

## 0.7-G macOS GUI

共享 descriptor、command 和 repository 合同冻结并在 Windows 通过后再交接 macOS。AppKit 使用相同
Profiles/Rules/Settings 信息架构和结果语义；平台代码只实现 NSControl/NSTextView、Auto Layout、
Retina、appearance、focus 与 accessibility。不得为 macOS 建第二套 validation、SQLite wrapper 或
runtime。

`0.7-G` 保持当前 macOS 视觉主题，不为了 Windows 圆角化重写 AppKit 外观；只实现 0.7 新增字段、Rule
文本和 repository 状态的功能对等，并继续遵守现有 native appearance、Retina 与 accessibility 合同。

交接退出条件包括 Release/warnings build、全部 CTest、shared/proxy/menu integration、v2 migration、
SQLite lock/recovery、文本 Rule、100 次窗口资源循环和 16 路 SSE 精确回传。

## 0.7-H 候选与发布

### 数据与故障矩阵

- fresh v3、v2 migration、重复 migration、readonly root、disk full fault adapter；
- WAL lock/busy、constraint、corruption、unsupported schema、unfinished commit/migration journal；
- CLI/GUI/config external edit 的 composite stale；
- repository saved + runtime apply failed、restart rollback failed；
- 128 Profile、256 Route、每 Profile 64 Rule 的 load/validate/compile/edit；
- backup 可恢复且卸载/升级不删除用户数据库。

### 资源与性能门槛

- `0.7-A` 加固后 `smoke`/`desktop-8`/`desktop-16` 的 proxy added TTFB 相对 `0.6.0` 不回退超过
  5% 或 0.25 ms（取较宽者）；
- 8-16 路零失败、Usage 不被 SSE 饿死；50 路只允许受 worker/connection/budget 明确约束的有界行为；
- budget、retired generation、control queue、SQLite statement/connection、logger queue 和 GUI 对象均有
  current/peak 证据，drain 后回到零；
- 128 Profile/8192 Rule 的 repository load + validate + compile Release 目标不超过 1 秒，GUI 首次列表
  发布目标不超过 500 ms；
- 空 Rule pipeline 继续零 JSON parse，非空 pipeline 最多一次 DOM parse/serialize；
- SQLite/config/GUI descriptor 查询不进入 request hot path。

开始 `0.7-A1` 实现前，使用 `0.6.0` 最终源码和相同工具链把五档短负载至少重跑三次，保存 ignored
JSON 并以中位数作为本机对照；发布归档中的历史数字只用于 sanity check，不直接与不同日期/机器的
单次结果做 5% 判断。

最终执行两平台 clean Release/warnings、全部 CTest 与 integration、五档短负载、Rule microbenchmark、
并发大 body、reload churn、2 小时 mixed 和 8 小时 GUI idle。从同一最终 commit 构建双平台固定
白名单包，把 ZIP hash 写入签名 annotated tag 后再发布资产。

## 实施与提交顺序

| 工作包 | 主体 | 提交边界 | 退出条件 |
| --- | --- | --- | --- |
| `0.7-A0` | 未改代码基线 | Windows/macOS 日志、CPU、内存、延迟证据 | 三次短测中位数与审计证据可复核 |
| `0.7-A1` | budget/metrics/generation | 共享类型、RAII lease、单测 | current/peak/rejection 与 drain 正确 |
| `0.7-A2` | server/Rule/logger/control | 接入全部 Windows 可测路径 | 并发大 body、fault、reload churn passed |
| `0.7-A3` | macOS resource hardening | cURL/header RAII 与平台测试 | macOS build/integration/resource passed |
| `0.7-B` | SQLite dependency | vendored source、CMake、license、probe | 双平台同 version/options，基础 API passed |
| `0.7-C` | schema/store | SQLite RAII、schema/revision/CRUD | transaction/constraint/lock/corruption tests passed |
| `0.7-D` | composite/migration | v3 codec、coordinator、v2 import | crash-window/recovery/equivalence passed |
| `0.7-E` | shared edit/CLI | descriptors、typed commands、Rule text | CLI/VM round-trip、stale/validation passed |
| `0.7-F` | Windows GUI | 三视图与 Windows integration | DPI/accessibility/resource/SSE passed |
| `0.7-G` | macOS GUI | AppKit parity 与 macOS integration | Retina/accessibility/resource/SSE passed |
| `0.7-H` | release | soak、文档、双平台包、tag | 所有发行证据可审计 |

每个工作包保持独立可回归；不把 SQLite 引入、数据迁移、两平台 GUI 和版本升版塞进同一提交。涉及
共享合同的 commit 先在 Windows 完成并推送，再通过 `.codex/HandOff.md` 指派窄 macOS 工作包。macOS
回传后由 Windows 继续主开发，最终发行过程沿用 `0.6.0` 的双平台同提交与公开资产下载复验流程。

## 开工检查表

开始 `0.7-A1` 前：

- [x] `0.6.0` 双平台发行、公开资产与签名 tag 已验证；
- [x] 主项目和 `.codex` worktree clean，Windows 持有主开发权；
- [x] 内存、generation、control queue、SQLite、迁移、GUI 与 Rule 文本边界已写明；
- [x] 已接收 `.codex@ff5dc81` 性能审计并完成 P1/P2/P3 覆盖矩阵；
- [x] Windows `0.7-A0` 三轮五档短负载与三轮 Rule matrix 已归档为 ignored evidence；
- [x] benchmark annotated tag 已改为 peeled commit，并以 `0.6.0` smoke 验证；
- [x] 准备阶段 Release/warnings CTest `16/16`、两套 shared integration、Release tray integration passed；
- [x] 为 budget/generation metrics 添加失败优先单元测试；control metrics 留在 `0.7-A2`；
- [x] 实现共享 RAII budget，不改变 v2 on-disk schema；
- [x] 跑 `0.7-A1` Release/warnings CTest 与 shared/tray integration；
- [x] 完成 `0.7-A2` Windows 实现、fresh 双构建、integration、Rule matrix 与组合基准对照；
- [x] 通过交接仓库指派 `0.7-A3` macOS 窄验证；平台结果与共享 blocker 已回传。

没有需要用户在开工前额外选择的阻塞项。SQLite 精确版本与 512 MiB 默认值的唯一允许调整窗口分别
是 `0.7-B` 官方依赖取证和 `0.7-A` 候选基准；调整必须带证据并同步本文。
