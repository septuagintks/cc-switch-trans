# ccs-trans 0.5.0-Windows-x64 验证结果归档

本文件记录基础版本 `0.5.0` 的 Windows x64 发行版，规范发行标识为
`0.5.0-Windows-x64`。验证时 ZIP 和内部根目录仍使用旧的小写平台名；归档后只重命名外部
ZIP，不重新压缩，因此 SHA-256 与测试证据保持不变。归档只描述下表给出的二进制，不代表
之后从 `main` 重新构建的产物。

> 发布收尾时，GitHub Windows 资产已替换为最终共享源码的重建包，SHA-256 为
> `9A0C08003D07225EE3D7F96EA2716BB3674E7CEB39A892C4D31548491521DD3B`。本文件继续保存
> Windows-only 候选的完整 VM 证据；最终双平台来源、回归和接受限制见
> [Release-0.5.0.md](Release-0.5.0.md)。

## 结论

**通过（Pass with documented limitations）**。

候选 ZIP 的包完整性、协议、tray 菜单与生命周期、启动项、系统代理、五档短负载、
2 小时 mixed soak、8 小时 idle soak、睡眠唤醒、Explorer 重启、系统重启和注销验证均通过。
没有请求失败、logger backpressure、writer failure、无界资源增长、残留进程或损坏 JSONL 尾部。

阶段 12 和 `0.5.0-Windows-x64` 在项目范围内正式验收完成。以下说明项已经接受，不阻塞该版本，
也不在 `0.5.0` 继续补测或修复：

- VM 的 Windows Defender 服务已停止且 WMI provider 不可用，因此 Defender 未评估。
- ZIP 没有 Mark-of-the-Web，未触发真实 SmartScreen 下载信誉流程；SmartScreen 未评估，
  两个 EXE 均未做 Authenticode 签名。
- 完成了系统重启和两次注销，但没有执行真实关机后由外部重新开机。
- 新 tray 在 Explorer 刚重启约 3 秒、通知区仍未就绪时，一次出现
  `Shell_NotifyIconW(NIM_ADD)` 2 秒重试耗尽；等待 10 秒后通过，实际系统登录自动启动也通过。
- 重启和注销会终止旧 tray 且日志尾合法，但没有写出显式 `session_end` / `server_stop`
  事件；正常菜单 Exit、自动化 Exit 和 soak 退出均能完整 drain。

## 测试信息

| 项目 | 值 |
| --- | --- |
| 发行标识 | `0.5.0-Windows-x64` |
| 归档 ZIP | `dist/ccs-trans-0.5.0-Windows-x64.zip` |
| 验证时文件名 | `ccs-trans-0.5.0-windows-x64.zip` |
| ZIP SHA-256 | `FD6E2812410F9B9274B9F54B04C6C461D582E1A4C68A69D341C9DC99CB9A005A` |
| CLI SHA-256 | `6BE7B8670956A1401E0275669B0B168C501E1AB7B6913A30CEB6B7E917B377FF` |
| tray SHA-256 | `5873C6677BD2FB8B4D5135B3F9007B27F3F8B16A4111B6F1DD041F2ED52E70DB` |
| Windows | Windows 11 专业版 24H2，10.0.26100，x64 |
| VM | VMware20,1，VMware SVGA 3D，2560 x 1440 |
| 原始显示缩放 | 125% |
| 验证缩放 | 100%、125%、150%、200% |
| 原始系统代理 | 手动代理 `127.0.0.1:16384`，原 bypass 列表 |
| 测试日期 | 2026-07-13 至 2026-07-14（Asia/Shanghai） |
| 测试依赖 | Python 3.13.14（VM 中安装，仅用于仓库验证脚本） |

Guest 内没有可用的 VMware host snapshot 命令，因此没有从 VM 内创建 snapshot。所有
mutation 均先备份并在测试后恢复，最终实际 Candy 用户 hive 已再次核对。

## 清单结果

### 包完整性

- [x] ZIP 只有一个 `ccs-trans-0.5.0-windows-x64/` 根目录。
- [x] `SHA256SUMS.txt` 中 CLI 与 tray 哈希匹配。
- [x] `ccs-trans.exe --version` 输出 `ccs-trans 0.5.0`。
- [x] tray FileVersion / ProductVersion 均为 `0.5.0`。
- [x] 固定白名单通过；无 config、logs、PDB、ICO、benchmark、CMake/Ninja 或 Python cache。
- [x] 在包含空格和中文的路径中完成验包、集成、tray、负载和启动项移动测试。

### Tray 与菜单

- [x] GUI subsystem，无 console；enabled Profile 自动启动。
- [x] 浅色和深色任务栏中，overflow 图标的黑色主体与白色轮廓均清晰。
- [x] 100%、125%、150%、200% 的实际窗口 DPI 分别为 96、120、144、192；菜单文本无截断。
- [x] 左键、右键和 `NIN_KEYSELECT` 均能打开真实 popup 菜单。
- [x] running / stopped / faulted 下 Status、Start、Stop、Reload 状态与服务一致。
- [x] Open configuration 创建缺失的默认 config；Open logs 打开正确目录。
- [x] 第二实例仅通知已有实例，进程始终唯一。
- [x] 启动器进程退出不影响 GUI tray 或 listener。

### 生命周期与故障

- [x] Stop 释放端口且 tray 保持运行；Start 恢复。
- [x] 有效 reload 对新请求生效。
- [x] 损坏 JSON reload 在约 `0.052 s` 内报错，旧 generation 继续服务，修复后可 reload。
- [x] listener 端口占用时状态为 faulted，释放后可 Start。
- [x] CLI 与 tray 争用端口返回明确 `failed to bind listener`。
- [x] upstream 500 与断连不会把 tray 标记为 faulted。
- [x] Exit 小于 5 秒，listener、worker、logger 均退出并 drain。
- [x] S1 睡眠 `122.532 s` 后同一 tray PID、菜单、listener、转发、SSE 和日志正常。
- [x] Explorer PID 发生变化，tray PID 和 `server_start` 次数不变，进行中的 SSE 完成，图标恢复。
- [x] 重启/注销后旧进程和 listener 无残留，两份 JSONL 尾部合法。
- 未执行（已接受）：真实关机后由外部重新开机。

### 开机自启

- [x] HKCU Run 中唯一 `ccs-trans` value，类型为 REG_SZ。
- [x] value 是带引号的当前 tray 绝对路径，无额外参数。
- [x] 启用/取消/重新启用不会产生重复 value。
- [x] 系统重启并登录后自动启动为唯一 tray，无 console，listener 由该进程拥有。
- [x] 实际移动程序目录后，旧路径显示未启用；重新勾选更新到新路径。
- [x] 强制取消后再次注销/登录，Run、tray 进程和 15723 listener 均不存在。

### 协议与负载

- [x] Responses、Chat Completions、Messages 主请求和 Usage 全部通过。
- [x] 普通响应、SSE、query、end-to-end headers 透明。
- [x] findcg 仅删除 `image_gen`，其他 Profile 不受影响。
- [x] `desktop-8`、`desktop-16`、`mixed-16`、`stress-50` 均零失败。
- [x] stress-50 峰值 15.28 MiB working set、510 handles、51 threads；logger 无 backpressure/failure。
- [x] 2 小时 mixed soak 通过。
- [x] 8 小时 idle soak 通过。

短负载结果：

| Profile | 成功/失败 | proxy TTFB p50 | 峰值 WS | 峰值 handle/thread |
| --- | ---: | ---: | ---: | ---: |
| smoke | 40 / 0 | 8.812 ms | 12.55 MiB | 287 / 26 |
| desktop-8 | 8 / 0 | 30.010 ms | 11.97 MiB | 303 / 27 |
| desktop-16 | 16 / 0 | 43.562 ms | 12.99 MiB | 370 / 35 |
| mixed-16 | 16 / 0，Usage 各 12 / 0 | 43.270 ms | 13.55 MiB | 379 / 35 |
| stress-50 | 50 / 0 | 47.436 ms | 15.28 MiB | 510 / 51 |

2 小时 mixed soak：

- 实际负载时间 `120.37 min`，857 轮。
- SSE 成功 13,712，失败 0；Responses / Chat Usage 各 10,284 次成功。
- working set `14.09 -> 13.84 MiB`，后半程斜率 `0.042 MiB/h`。
- handles `323 -> 334`，斜率 `0.359/h`；threads `37 -> 36`，斜率 `0.09/h`。
- 最终 connection、queue、active worker 和 logger queue 均为 0。
- runtime JSONL 1,731,979 条，writer failure / backpressure 均为 0。

8 小时 idle soak：

- 实际 `8.000 h`，480 个分钟样本，同一 tray 进程。
- working set `14.31 -> 13.31 MiB`，斜率 `-0.2579 MiB/h`。
- handles `300 -> 286`，斜率 `0/h`；threads `22 -> 18`，斜率 `-0.023/h`。
- idle 期间 host log 始终 `710 bytes`，runtime log 始终 `3098 bytes`，均为 `0 bytes/h`。
- 结束后菜单、listener、转发正常，进程退出码 0。

### Windows 系统代理

- [x] direct、手动 proxy、bypass、显式 PAC 均通过。
- [x] WPAD-only 不执行自动发现。
- [x] 已选 proxy 断开与 PAC dead proxy 不 fallback direct。
- [x] 407 映射为 `proxy_authentication_unsupported`。
- [x] 运行中 A/B 切换后，新请求使用新 session，in-flight 请求保持旧 session。
- [x] 测试 `finally` 恢复 registry 并发送 Internet Settings 通知。
- [x] 最终实际 Candy 用户 hive 再次确认 `ProxyEnable=1`、`ProxyServer=127.0.0.1:16384`、原 bypass 和空 PAC。

### 日志与发布观察

- [x] runtime 与 host log 分离，均为合法 JSON Lines。
- [x] host log 不含请求 header/body；runtime 敏感数据未泄漏。
- [x] error 立即可见；菜单 Exit、自动化 Exit、soak 退出均 drain。
- [x] SSE `chunk_sequence` 从 0 连续递增，response body 未聚合记录。
- [x] 从最终 ZIP 解压产物完成完整 smoke，并记录 ZIP SHA-256。
- 未评估（非阻塞）：Defender 不可用，`WinDefend` 为 Stopped，WMI provider 返回
  `0x80041013`，VM 中无可用 `MpCmdRun`。
- 未评估（非阻塞）：SmartScreen 未触发，ZIP 无 `Zone.Identifier` / MOTW。
- [x] Authenticode 结果已记录：CLI 和 tray 均为 `NotSigned`。

## 已接受限制与观察

1. 已运行 tray 在 Explorer 重启后能恢复图标且不重启服务，这是清单要求并已通过。单独的
   新 tray 若在 Explorer 重启约 3 秒时启动，曾因通知区未就绪耗尽 2 秒重试并退出；等待
   10 秒后通过，实际登录启动也通过。`0.5.0` 保留该限制，后续版本按实际风险决定是否延长
   注册重试或等待 `TaskbarCreated`。
2. `WM_ENDSESSION` 当前触发异步 shutdown，但实际重启/注销日志中没有 `session_end` 与
   `server_stop` 事件。Windows 确实终止旧进程，日志尾仍合法；`0.5.0` 接受该结果，后续版本
   按实际需求决定是否增加有界同步等待或更早的 session-end 协调。

## 证据索引

以下路径相对于 VM 测试证据目录。原始日志、截图和长时间采样不进入 Git 或发布包，归档
保留结果摘要、哈希和证据文件名，以避免提交运行日志、用户路径或可能含敏感请求的数据。

- `short-load.json`：五档短负载。
- `extended/extra-validation.json`：tray 菜单、reload、fault、CLI 争用、Explorer 与日志断言。
- `ui/`：四档 DPI、浅/深色菜单和通知区图标截图。
- `sleep/sleep-validation.json`：S1 睡眠/唤醒。
- `startup/startup-move-validation.json`：真实 HKCU startup 与移动路径。
- `long/mixed/mixed-soak.json`：2 小时 mixed soak 完整采样与结果。
- `long/idle/idle-soak.json`：8 小时 idle 完整采样与结果。
- `reboot/reboot-after.json`、`reboot/logout-after.json`：重启与注销恢复检查。
- `reboot/actual-home-evidence/`：真实用户目录的重启/注销日志副本。
