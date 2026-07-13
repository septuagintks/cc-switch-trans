# macOS 0.6.0 验证清单

## 测试信息

```text
源码 commit：
候选 ZIP：
ZIP SHA-256：
macOS 版本：
Xcode / SDK：
机器型号：
架构：arm64
测试日期：
```

测试目标严格限定为 macOS 26、Apple Silicon `arm64`。Intel、Rosetta、Universal 2、
macOS 15 及更早版本不进入兼容矩阵。登录项、代理环境和签名测试前先记录当前状态；不得
把证书、公证凭据、API key、用户 config 或日志放进仓库和候选包。

## 工具链与干净构建

- [ ] `tools/check_stage13_prerequisites.sh` 完整通过；Developer ID 缺失只允许阻塞发布签名。
- [ ] 当前 host 是 macOS 26 arm64，selected SDK 是 macOS 26，Xcode 为 26 或更新版本。
- [ ] CMake preset 拒绝非 arm64 architecture 和非 `26.0` deployment target。
- [ ] `cmake --preset macos-arm64-release` 可从空 build directory configure。
- [ ] `cmake --build --preset macos-arm64-release` 完成 CLI 与 `.app` 构建。
- [ ] `ctest --preset macos-arm64-release` 全部通过。
- [ ] warnings-as-errors preset 可 configure、build、CTest。
- [ ] `file`、`lipo -archs` 只报告 `arm64`，不存在 `x86_64` slice。
- [ ] `otool -l` 显示最低系统版本为 `26.0`。
- [ ] `otool -L` 中 curl 来自 `/usr/lib`，不存在 Homebrew/MacPorts/private curl。

推荐顺序：

```text
./tools/check_stage13_prerequisites.sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
cmake --preset macos-arm64-warning
cmake --build --preset macos-arm64-warning
ctest --preset macos-arm64-warning
```

## CLI 与配置

- [ ] `ccs-trans --version` 与 CMake、Info.plist、包名一致。
- [ ] 首次运行在当前账户 home 下使用 `~/.ccs-trans/`，目录和文件仅当前用户可访问。
- [ ] 所有 config/profile/rule 单字段命令与 Windows 行为一致。
- [ ] config lock、临时文件、原子替换和并发修改检测通过。
- [ ] UTF-8、空格和中文路径下 config、logs、state 均正常。
- [ ] SIGINT/SIGTERM 后 listener、worker、curl handle 和 logger 正常 drain。
- [ ] 损坏 config、端口占用、日志不可写分别产生明确错误和非零退出码。

## Listener 与协议

- [ ] listener 仅绑定配置地址，默认 `127.0.0.1:15723`，端口冲突不启用地址复用旁路。
- [ ] partial header/body、超过 limits、客户端半关闭和 stop 中断均有确定结果。
- [ ] SIGPIPE、EINTR、partial send/receive、fd 回收和客户端断开测试通过。
- [ ] Responses、Chat Completions、Messages 及三组 Usage 路由通过。
- [ ] 普通响应、SSE、query、headers、status/reason 与错误 envelope 保持协议合同。
- [ ] findcg Profile 只删除目标请求中的 `image_gen`，其他 Profile 不受影响。
- [ ] reload generation、失败回滚和 in-flight 请求保持旧 generation。

## system libcurl 与代理环境

- [ ] 每个并发请求拥有独立 easy handle，连接缓存和 handle pool 有明确上限。
- [ ] 普通响应、SSE callback、取消与 DNS/connect/send/header/idle/total timeout 全部通过。
- [ ] response body limit、partial callback 和上游提前断开不会泄漏 handle/fd。
- [ ] 未设置 proxy 环境变量时直连，不读取或激活 macOS System Settings 代理。
- [ ] `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY` 和 `NO_PROXY` 的 libcurl 行为有记录。
- [ ] 选定环境代理后连接失败不做 ccs-trans 自行 direct fallback。
- [ ] Terminal 启动继承当前环境；Finder/登录项没有 shell proxy 环境时按预期直连。
- [ ] proxy 环境只在进程启动时确定，不把环境内容或代理凭据写入日志。

## 菜单栏宿主

- [ ] 双击 `.app` 不打开 Terminal，enabled Profiles 自动启动。
- [ ] 浅色/深色、Retina 和辅助功能尺寸下 template icon 清晰。
- [ ] Status、Start、Stop、Reload、Open configuration、Open logs、Launch at Login、Quit 完整。
- [ ] AppKit 对象只在主线程访问，服务命令不阻塞菜单和系统 UI。
- [ ] 第二实例唤起现有实例，不启动第二份 listener。
- [ ] Stop/Start、reload rollback、faulted 恢复和 Quit drain 与 Windows 状态语义一致。
- [ ] 睡眠/唤醒、网络切换、Finder 重启、注销和系统关机无残留进程或损坏日志。

## 登录项

- [ ] `SMAppService.mainAppService` register/unregister 状态与菜单勾选一致。
- [ ] 需要用户批准、被拒绝和系统错误均可见并写入 host log。
- [ ] 登录后只启动 `.app` menu host，不额外启动 CLI 服务。
- [ ] 移动或替换 `.app` 后状态可正确刷新，不缓存旧路径猜测。
- [ ] Terminal 与登录项启动的环境差异已验证，尤其是 proxy 环境变量。

## 性能与长时间运行

- [ ] 8 路、16 路 SSE 无失败，added TTFB 与 Windows 基线可解释。
- [ ] mixed-16 中 Responses/Chat Usage 在 SSE 期间持续完成。
- [ ] stress-50 保持 32 worker、连接、fd、内存和 logger queue 有界。
- [ ] 2 小时 mixed soak 后 RSS、fd、线程和 curl handle 回到稳定区间。
- [ ] 8 小时 idle 不持续轮询磁盘、不产生周期日志膨胀或资源增长。

## 签名、公证与候选包

- [ ] CLI 与 `.app` 使用 Developer ID Application 签名，nested code 签名完整。
- [ ] `codesign --verify --deep --strict --verbose=2` 通过。
- [ ] `spctl --assess --type execute --verbose=4` 通过。
- [ ] ZIP notarization 成功且 ticket 已 staple 到 `.app`。
- [ ] 候选 ZIP 只有签名 `.app`、签名 CLI、README、docs、licenses 和 checksum 白名单。
- [ ] 包内没有用户 `.ccs-trans`、logs、config、benchmark、dSYM、临时 iconset 或构建路径。
- [ ] 从最终 ZIP 在干净账户完成 CLI、Finder `.app`、登录项和端到端 smoke。
