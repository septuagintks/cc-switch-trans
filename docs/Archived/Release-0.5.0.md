# ccs-trans 0.5.0 发布结果归档

## 发布结论

`0.5.0` 于 2026-07-15 完成 Windows 11 x64 与 macOS 26 arm64 的双平台发布收尾。GitHub
Release 为 [v0.5.0](https://github.com/septuagintks/cc-switch-trans/releases/tag/0.5.0)，
状态为正式发布，不是 draft 或 prerelease。

发行标识：

```text
0.5.0-Windows-x64
0.5.0-macOS-arm64
```

Windows 最终资产从 macOS 接入后的共享源码重新构建，用于替换最初的 Windows-only 资产。
macOS 资产使用 ad-hoc 签名。两个平台都不声明商业代码签名身份或操作系统信誉服务通过。

## 源码与标签

| 项目 | 值 |
| --- | --- |
| 最终共享实现 commit | `cd6b4041b7ef09ef4a784c07d0031b06851533f7` |
| macOS 阶段 13 实现 commit | `dfcea1d79514d95bddfe966cc68bbe8c47f83af1` |
| macOS 阶段 14 harness commit | `c8043e3a2f97af616858c55501ccbe13c4da1e03` |
| `0.5.0` tag 指向 | `a2dc442d7343a2d24d36b12e40a1ee7a5fc35826` |
| Windows ZIP | `ccs-trans-0.5.0-Windows-x64.zip`，2,498,767 bytes |
| Windows ZIP SHA-256 | `9A0C08003D07225EE3D7F96EA2716BB3674E7CEB39A892C4D31548491521DD3B` |
| macOS ZIP | `ccs-trans-0.5.0-macOS-arm64.zip`，754,610 bytes |
| macOS ZIP SHA-256 | `9607E512AD6A3518190BAC3EBE85DEC5E8DF312EE8A36186027102689E88E8E4` |

`0.5.0` 是在 macOS 实现合入前创建的 lightweight tag，因此 tag 本身不能完整表示后来补充的
macOS 源码和最终 Windows 重建包。这是本版本接受的发布溯源限制；已发布 tag 不移动、不重写。
GitHub Release API 已确认两份资产的文件名、大小和 digest 与本归档一致。Windows ZIP 内嵌的
发布文档不记录自身 archive hash，以避免自引用；仓库归档和 `.codex` 最终交接记录保存该值。

后续版本必须等两个平台资产都从同一最终 commit 构建、验证完成后，再创建签名 annotated tag。

## Windows 最终回归

Windows 侧在 `cd6b404` 上完成：

- Release clean build：passed；
- warnings-as-errors clean build：passed；
- 两套 CTest：各 12/12 passed；
- Responses、Chat、Messages、Usage、SSE、Rules、reload、limits 和取消集成：passed；
- WinHTTP manual proxy、PAC、bypass、切换、无 direct fallback 和 407 分类矩阵：passed，原系统
  代理设置已恢复；
- 冻结 Windows 旧包 SHA-256 复核：与历史归档一致；
- 当前源码五档短负载：全部零失败，logger backpressure/writer failure 为零。

短负载摘要：

| Profile | 成功/失败 | added TTFB p50/p95 | peak working set/handle/thread | 结果 |
| --- | ---: | ---: | ---: | --- |
| smoke | 40/0 | 0.377/0.736 ms | 11.89 MiB / 283 / 26 | passed |
| desktop-8 | 8/0 | 7.307/13.761 ms | 12.86 MiB / 319 / 30 | passed |
| desktop-16 | 16/0 | 7.872/5.582 ms | 13.77 MiB / 405 / 44 | passed |
| mixed-16 | 16/0 | 7.443/7.808 ms | 14.14 MiB / 422 / 47 | 两组 Usage 均 12/12 |
| stress-50 | 50/0 | 7.310/1981.191 ms | 17.67 MiB / 619 / 79 | 32 worker、peak queue 18 |

`stress-50` 的约 2 秒 p95 是超过 32 worker 后的预期有界排队，不是 8-16 路桌面目标。最终
Windows 重建包执行固定白名单、内部 checksum、版本和静态 archive 验证；本次会话中已有安装版
tray 持有 session 单实例 mutex，当前重建包的 GUI 子进程自动化未重复启动。该环境冲突不作为
产品失败；历史完整 GUI/VM 证据见 [WindowsValidationCheckResult.md](WindowsValidationCheckResult.md)，
用户也已完成当前版本的基本运行确认。

## macOS 发布结果

macOS 侧在 macOS 26.5.2、Apple Silicon arm64、Command Line Tools SDK 26.5 和 Apple Clang
21.0.0 上完成：

- Release 与 warnings-as-errors clean build：passed；
- 两套 CTest：各 10/10 passed；
- 共享协议、process proxy、AppKit menu、单实例与 SIGTERM drain 集成：passed；
- CLI 与 `.app`：arm64-only、minOS 26.0、SDK 26.5、system libcurl；
- 固定白名单、内部 checksum、解包 CLI/menu smoke 与 strict ad-hoc codesign：passed；
- 8/16/mixed/50 路负载与 Rule matrix：passed；
- 用户缩短范围的 15 分钟 mixed 和 30 分钟 AppKit idle：passed，无持续资源增长或日志写入。

完整结果见 [MacOSValidationCheckResult.md](MacOSValidationCheckResult.md)。

## 已接受限制

以下限制不阻塞 `0.5.0`，也不在该版本继续补测：

- macOS 没有完整 Xcode.app，Finder/UI、真实登录项 mutation 和 clean-account archive matrix
  未执行；
- macOS 原 2 小时 mixed、8 小时 idle、睡眠/唤醒、网络切换、注销、关机与磁盘写满未执行；
- macOS resolve/connect/send 缺三个独立确定性 timeout fixture；
- macOS ad-hoc 签名没有发布者身份，不是 notarized 或 Gatekeeper trusted；
- Windows Defender、SmartScreen 和 Authenticode 未验证；
- Windows Explorer 初始化期 tray 注册窗口与会话结束日志尾限制保持历史接受状态；
- Windows 最终 GUI 自动化因当前会话已有安装版 tray 而未重复执行；
- Windows 发行资产替换后与 lightweight tag 不处于同一 commit。

## 版本关闭

`0.5.0` 到此关闭。除 Release 资产上传纠正外，不再向该版本追加功能、迁移或测试承诺。任何
新的代码行为变化先确定后续版本号和范围，再按 [DevelopmentPlan.md](../DevelopmentPlan.md)
重新建立基线、实现、双平台验证、打包和发布记录。
