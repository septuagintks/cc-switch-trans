# ccs-trans 0.7.0 发布结果归档

## 发布结论

`0.7.0` 将 Profile 与有序 Rule 从单体 v2 JSON 配置迁入 SQLite，并让 Windows 与 macOS 原生主窗口
完整管理 Profile 字段、应用字段和 canonical Rule 文本。两个平台继续共享一个数字版本：

```text
0.7.0-Windows-x64
0.7.0-macOS-arm64
```

Windows 使用 WinHTTP current-user system proxy，选中代理失败时不回退直连；macOS 使用 selected SDK
system libcurl 和进程 proxy environment。macOS 包继续采用 ad-hoc 签名，不声明发布者身份、公证或
Gatekeeper 信任。

## 功能范围

- `ccs-trans.config/v3` 只保存应用设置，固定 `profiles.db` 保存 Profile 与 Rule；
- v2 数据只通过显式 `storage migrate` 迁移，保留原文件、SHA-256 manifest 和可恢复 journal；
- SQLite 3.53.3 静态编译，schema v1 使用 WAL、FULL synchronous、foreign keys 和 2 秒 busy timeout；
- Composite repository 统一 config/DB revision、stale 检测、跨文件恢复和 runtime rollback；
- CLI、Win32 与 AppKit 共用 field descriptor、typed command、ConfigurationEditor 和 ViewModel；
- 两平台主窗口均提供 Profiles、Rules、Settings 三视图、Apply、Discard、Reload Draft 和轻量模式；
- Win32 保持轻量圆角主题；AppKit 使用相同信息架构和布局比例，但保留 macOS 原生主题、控件与滚动；
- 全进程默认 512 MiB inflight budget、32 worker、64 connection、有界 control/logger queue 和资源指标；
- 空 Rule pipeline 零 JSON parse，非空 pipeline 最多一次 parse/serialize；请求热路径不访问 SQLite、
  config 文件、GUI 或 descriptor。

不进入本版本：可视化 Rule builder、undo/redo、请求样例 diff、新协议、云同步、自动故障转移、
负载均衡和网络栈重写。这些内容仍按 `0.8.0` 及后续版本规划。

## 源码溯源

| 项目 | 值 |
| --- | --- |
| 开发基线 | `0.6.0` tag，commit `f9fe5a8868c46bc7b982995cca03432ed96e27eb` |
| Windows GUI 验收基线 | `262c7aeb1d2da40cda38f286312c05736bf563c4` |
| macOS GUI 验收基线 | `7132053d9a0125b687c4acfe4a1f154bf712741d` |
| 最终源码 | 本文件所在正式版本提交，由签名 annotated tag `0.7.0` 指向 |
| Windows 提交签名 key | `403FEA7B8908480460BF40BA36E772C15CA7CC4E` |
| macOS 候选签名 key | `3DF828FDD419996E11B2FD2881BFB01482975987` |

Windows 与 macOS 正式 ZIP 必须从同一个最终源码提交构建。ZIP 外部 SHA-256 不写入自身包含的文档，
而记录在签名 tag message、发布资产和跨平台交接记录中；包内仍提供逐 executable/文件 checksum。

## Windows 验证

最终 Windows 候选使用 GCC 16.1.0、CMake 4.3.2 与 Ninja 1.13.2，在两个全新目录分别执行 Release 和
warnings-as-errors 构建。

| 项目 | 结果 |
| --- | --- |
| clean Release / warnings build | 两套均 `112/112`，warnings 为 0 |
| Release / warnings CTest | 两套均 `25/25 passed` |
| shared integration | 两套均 passed |
| tray/main-window integration | 两套均 passed |
| 五档短负载 | `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50` 全部零失败 |
| Rule matrix | 1 KiB/100 KiB/1 MiB、0/1/8/32 Rule passed |
| 短 soak 夹具 | mixed 10 秒、CLI idle 5 秒 passed，用于确认 v3 migration 与 drain 路径 |
| 正式 ZIP | 固定白名单、双 executable checksum/version、解压后 tray integration passed |

`b7ea0ba` 五档候选的 added TTFB p50 分别为 `0.208`、`5.108`、`7.021`、`6.959`、`7.454 ms`。
常规档与
`mixed-16` 无请求失败、Usage starvation、logger backpressure 或 writer failure；50 路峰值 worker 为
32，剩余请求按设计有界排队。1 MiB/32 Rule 的 modified、unchanged、unmatched 平均值分别约为
`4.676`、`3.429`、`3.407 ms`，空 pipeline 继续零 parse/serialize。

原始 benchmark/soak JSON 位于 ignored `benchmark-results/`，不进入 Git 或发行包。最终包 hash 与
archive smoke 结果以 tag 和交接记录为准。

## macOS 验证

macOS `0.7-G` 最终产品候选基于 `7132053`，环境为 macOS 26 arm64、selected macOS SDK、Apple Clang、
CMake/Ninja 和系统 `/usr/lib/libcurl.4.dylib`。

| 项目 | 结果 |
| --- | --- |
| fresh Release / warnings build | 两套均 `107/107`；第二次 build 均 `ninja: no work to do` |
| Release / warnings CTest | 两套均 `23/23 passed` |
| shared/proxy/transport/menu integration | 全部 passed |
| AppKit GUI | 三视图、响应式布局、原生主题与用户人工验收 passed |
| 生命周期与并发 | 100 次轻量窗口循环、资源收敛、16 路 SSE 与 Usage 精确性 passed |
| 架构与依赖 | CLI/app 均 arm64-only，链接 selected SDK system libcurl |
| 正式 ZIP | 最终源码的 ad-hoc 签名、固定白名单、checksum 与解包 smoke 证据由 tag/交接记录保存 |

AppKit 最终窗口最小尺寸为 `800x520`。Windows 布局是信息架构与比例基准，不是逐像素主题合同；
macOS 继续遵守 native appearance、Retina、focus、accessibility 和 Reduce Motion。

## 明确未执行

以下项目不声明为 `0.7.0` passed：

- 两平台均未在 0.7 最终候选上重跑 2 小时 mixed 与 8 小时 GUI idle；0.6 的长时结果不冒充 0.7 证据；
- Windows 未重跑完整 current-user system proxy mutation 矩阵、四档 DPI/Narrator/128 Profile 人工组合；
- macOS 未运行 deterministic cURL fault adapter、完整 VoiceOver 会话、Developer ID、公证或 staple；
- readonly root/disk-full 的完整平台 fault adapter 与 clean-machine 升级/卸载矩阵未扩展执行；
- Windows EXE 未做 Authenticode，Defender 与 SmartScreen 未评估。

这些限制不改变已通过的协议精确性、SQLite/恢复单测、短负载和窗口资源门禁，但缩小了本次发布声明。
后续版本若触及对应路径，必须重新执行风险匹配的专项测试，不能继续引用本归档作为当前通过结果。

## 升级说明

`run`、tray/menu host 和普通 Profile 命令不会静默迁移 v2。升级前执行：

```text
ccs-trans storage status
ccs-trans storage migrate
ccs-trans storage verify
```

成功迁移后，应用设置位于 `config.json`，Profile/Rule 位于固定 `profiles.db`。`0.6.x` 不承诺读取该
v3/SQLite 状态；迁移备份应保留到用户完成实际请求与 GUI 验证。
