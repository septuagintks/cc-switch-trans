# Windows 0.5.0 候选包验证清单

## 测试信息

```text
候选 ZIP：
ZIP SHA-256：
Windows 版本：
VM 架构：x64
显示缩放：
系统代理状态：
测试日期：
```

测试目标仅为 Windows 11 21H2 及以上 x64。先创建 VM snapshot，避免开机自启、系统代理
和日志测试污染长期环境。每项记录通过/失败、截图或日志位置；失败时保留
`%USERPROFILE%/.ccs-trans/` 后再还原 snapshot。

源码工作区已完成不修改真实 startup/system proxy 的自动预检：Release 与
warnings-as-errors CTest 11/11、协议集成、tray 生命周期与真实通知区图标、五档负载、规则
microbenchmark，以及候选 ZIP 解压验包。以下清单保持未勾选，由 VM 对最终 ZIP 独立验收。

## 包完整性

- [ ] ZIP 解压后只有一个 `ccs-trans-0.5.0-windows-x64/` 根目录。
- [ ] `SHA256SUMS.txt` 中 CLI 与 tray 的 SHA-256 均匹配。
- [ ] `ccs-trans.exe --version` 输出 `ccs-trans 0.5.0`。
- [ ] `ccs-trans-tray.exe` 文件属性版本为 `0.5.0`。
- [ ] 包内没有 config、logs、PDB、ICO、benchmark、CMake/Ninja 或 Python cache。
- [ ] 在包含空格和中文的解压路径重复以下基础测试。

## Tray 与菜单

- [ ] 双击 tray executable 不显示 console，全部 enabled Profiles 自动启动。
- [ ] 任务栏浅色/深色模式下图标边界清晰。
- [ ] 100%、125%、150%、200% 缩放下图标清晰、菜单文本不截断。
- [ ] 左键、右键和键盘激活均可打开菜单。
- [ ] Status、Start、Stop、Reload 的 enabled/disabled 状态与实际服务一致。
- [ ] Open configuration 可创建并打开缺失的默认 config。
- [ ] Open logs 打开 `%USERPROFILE%/.ccs-trans/logs/`。
- [ ] 第二次双击只唤起已有菜单，任务管理器始终只有一个 tray 进程。
- [ ] 关闭启动 tray 的终端不影响 tray 或 listener。

## 生命周期与故障

- [ ] Stop 后 listener 端口释放，tray 保持运行；Start 后恢复。
- [ ] 有效配置修改后 Reload 对新请求生效。
- [ ] 损坏 JSON 后 Reload 报错，旧 generation 继续服务；修复后可再次 Reload。
- [ ] listener 端口被占用时 tray 显示 faulted；释放端口后可 Start。
- [ ] CLI `run` 与 tray 争用同一端口时返回明确 bind/ownership 错误。
- [ ] upstream 普通 4xx/5xx 或断连不会把整个 tray 标成 faulted。
- [ ] Exit 后 5 秒内进程、listener、worker 和日志 writer 全部退出。
- [ ] 睡眠/唤醒后菜单、listener、转发和日志继续正常。
- [ ] 重启 Explorer 后 tray 图标恢复，已有服务与 SSE 不重启。
- [ ] 注销、系统重启和关机没有残留进程或损坏日志尾部。

## 开机自启

- [ ] 勾选后 HKCU Run 中只有一个 `ccs-trans` value。
- [ ] value 是带引号的当前 tray executable 绝对路径，没有额外参数。
- [ ] 重复勾选不产生重复 value；取消两次均成功。
- [ ] 注销再登录后 tray 自动启动且不显示 console。
- [ ] 移动程序目录后旧 value 不显示为 enabled；重新勾选更新到新路径。
- [ ] 取消后再次登录不再启动。

从源码目录执行自动 mutation 测试时使用：

```text
python tests/integration/run_tray_integration.py \
  --tray <package>/ccs-trans-tray.exe \
  --cli <package>/ccs-trans.exe \
  --with-tray-icon \
  --confirm-startup-mutation
```

脚本保存并在 finally 中恢复原 `ccs-trans` value；仍建议只在 VM 执行。

## 协议与负载

- [ ] Responses、Chat Completions、Messages 主请求和各自 Usage 均成功。
- [ ] 普通响应、SSE、query 和 end-to-end headers 保持透明。
- [ ] findcg Profile 删除 `image_gen`，其他 Profile 不受影响。
- [ ] 8 路、16 路 SSE 无失败，mixed-16 的两组 Usage 在 SSE 期间持续成功。
- [ ] stress-50 无无界线程、handle、内存或 logger queue 增长。
- [ ] 运行 2 小时 mixed soak 后 working set、handle 和线程数回到稳定区间。
- [ ] 运行 8 小时 idle 后没有周期日志膨胀或资源持续增长。

## Windows 系统代理

- [ ] 系统明确 direct 时直连。
- [ ] 手动 proxy、bypass 和显式 PAC 生效。
- [ ] WPAD-only 不执行自动发现。
- [ ] 已选 proxy 断开时请求失败且不 fallback direct。
- [ ] 407 映射为不支持代理认证，不弹出凭据提示。
- [ ] 运行中切换 proxy 后新请求使用新 session，in-flight 请求保持旧 session。
- [ ] 测试结束后系统代理 registry、Internet Settings 通知和网络行为均恢复。

源码专项矩阵命令：

```text
python tests/integration/run_windows_system_proxy_integration.py \
  --exe <package>/ccs-trans.exe \
  --confirm-system-proxy-mutation
```

## 日志与发布观察

- [ ] runtime log 与 `ccs-trans-host.log` 分离，均为合法 JSON Lines。
- [ ] host log 不含请求 headers/body，idle status poll 不持续产生日志。
- [ ] error 事件立即可见，正常退出前两份日志均 drain。
- [ ] SSE 日志只有连续 chunk sequence，不聚合完整 response body。
- [ ] 记录 Defender/SmartScreen 结果。当前 GPG commit 签名不等于 Authenticode 签名。
- [ ] 从最终 ZIP 而不是 build tree 完成一次完整 smoke，并记录 ZIP SHA-256。
