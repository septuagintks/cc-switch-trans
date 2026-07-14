# ccs-trans 0.5.0-macOS-arm64 验证清单

## 测试信息

```text
实现 commit：dfcea1d
清单 commit：本文件所在 commit
正式候选 ZIP：未生成（Developer ID / notarization blocked）
ad-hoc smoke ZIP：ccs-trans-0.5.0-macOS-arm64-adhoc-smoke.zip
ad-hoc SHA-256：A4D7C858AE2C7410458D1C2792CC7ACE4F44E12C7E8854E4CA5F97ACA26DE972
发行标识：0.5.0-macOS-arm64
macOS 版本：26.5.2 (25F84)
Xcode / SDK：完整 Xcode 不可用；Command Line Tools SDK 26.5；Apple Clang 21.0.0
机器型号：MacBook Air (Mac17,3, Apple M5, 16 GB)
架构：arm64
测试日期：2026-07-14（Asia/Shanghai）
```

测试目标严格限定为 macOS 26、Apple Silicon `arm64`。Intel、Rosetta、Universal 2、
macOS 15 及更早版本不进入兼容矩阵。登录项、代理环境和签名测试前先记录当前状态；不得
把证书、公证凭据、API key、用户 config 或日志放进仓库和候选包。

状态规则：`[x]` 表示列出的合同已在上述实现上自动验证；`[ ]` 表示未执行、仅部分覆盖或受
外部条件阻塞。ad-hoc 结果只证明代码签名结构和包白名单，不替代 Developer ID、公证、
Gatekeeper 或正式候选验证。

## 工具链与干净构建

- [ ] `tools/check_stage13_prerequisites.sh` 完整通过；当前仅 Xcode 版本项失败，Developer ID
  另为 release pending。
- [ ] 当前 host 是 macOS 26 arm64，selected SDK 是 macOS 26，Xcode 为 26 或更新版本；
  OS/arch/SDK 通过，但 active developer directory 仅为 Command Line Tools。
- [x] CMake preset 拒绝非 arm64 architecture 和非 `26.0` deployment target。
- [x] `cmake --preset macos-arm64-release` 可从空 build directory configure。
- [x] `cmake --build --preset macos-arm64-release --clean-first` 完成 CLI 与 `.app` 构建。
- [x] `ctest --preset macos-arm64-release` 10/10 通过。
- [x] warnings-as-errors preset 可 clean build，CTest 10/10 通过，包括 Objective-C++。
- [x] `file`、`lipo -archs` 只报告 `arm64`，不存在 `x86_64` slice。
- [x] `otool -l` 显示最低系统版本为 `26.0`、SDK 为 `26.5`。
- [x] `otool -L` 中 curl 为 `/usr/lib/libcurl.4.dylib`，不存在私有 curl。

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

- [x] `ccs-trans --version`、CMake、Info.plist 和包名均为 `0.5.0`。
- [x] 首次配置准备使用 `~/.ccs-trans/`，root/config 权限的 group/other bits 为零。
- [x] config/profile/rule 单字段命令通过共享 CLI/ConfigDocument 测试。
- [x] config lock、临时文件、原子替换和并发修改检测通过。
- [x] 含 UTF-8、空格和中文的临时 home 下 config、logs、state 和 menu host 正常。
- [x] CLI SIGINT 与 menu SIGTERM 后 listener、worker、curl handle 和 logger drain。
- [ ] 损坏 config 与端口占用已验证；真实不可写日志路径尚未做进程级 macOS 测试。

## Listener 与协议

- [x] listener 仅绑定配置地址，默认值为 `127.0.0.1:15723`；第二 listener 不能绕过冲突。
- [x] partial I/O、header/body limits、客户端断开和 stop 中断结果确定。
- [x] SIGPIPE、EINTR、partial send/receive、fd 回收和客户端断开 adapter 测试通过。
- [x] Responses、Chat Completions、Messages 及三组 Usage 路由通过。
- [x] 普通响应、SSE、query、headers、status/reason 与错误 envelope 通过共享 fixture。
- [x] findcg Profile 只删除目标请求中的 `image_gen`，其他 Profile 不受影响。
- [ ] reload apply/失败保持运行/rollback 已通过 controller 测试；macOS in-flight old generation
  专项尚未单独执行。

## system libcurl 与代理环境

- [x] 每个在途请求独占 easy+multi slot；pool=`worker_threads`，每 slot connection cache=4。
- [ ] 普通响应、SSE、取消、header/idle/total timeout 已通过；resolve/connect/send deadline 已
  实现但尚缺三个可确定复现的独立网络夹具。
- [x] response body limit、partial callback、客户端提前断开与重复请求未发现 handle/fd 泄漏。
- [x] 清空 proxy 环境时直连；实现没有读取或激活 macOS System Settings 代理。
- [x] 代理矩阵记录：大写 `HTTP_PROXY` 被 system libcurl 忽略，小写 `http_proxy` 生效；
  `HTTPS_PROXY` 收到 CONNECT，`ALL_PROXY` 生效，`NO_PROXY` bypass。
- [x] 已选环境代理连接失败返回 502/504，origin 计数证明没有 direct fallback。
- [ ] Terminal 环境已验证；Finder/登录项无 shell proxy 环境尚未实测。
- [x] 每个测试进程从启动环境确定 proxy；环境内容和带凭据 proxy URL 未进入日志。

## 菜单栏宿主

- [ ] `.app` executable 自动启动 enabled Profiles，`LSUIElement=true`；Finder 双击未手工验证。
- [ ] 浅色/深色、Retina 和辅助功能尺寸下 template icon 清晰。
- [ ] Status/Start/Stop/Reload/Open/Launch at Login/Quit 已实现并通过编译；逐菜单手工点击未执行。
- [x] AppKit 对象限定在主线程，服务命令经 control executor，自动 service/quit smoke 通过。
- [x] 第二实例通过 flock + distributed notification 命中现有实例，不启动第二份 listener。
- [x] controller 的 Stop/Start/reload rollback/faulted 恢复和 host Quit drain 测试通过。
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
- [x] ad-hoc CLI/`.app` 的 `codesign --verify --deep --strict --verbose=2` 通过；非 Developer ID。
- [ ] `spctl --assess --type execute --verbose=4` 通过。
- [ ] ZIP notarization 成功且 ticket 已 staple 到 `.app`。
- [ ] 正式候选 ZIP 未生成；ad-hoc smoke ZIP 的同一固定白名单验证通过。
- [x] ad-hoc 包内没有 `.ccs-trans`、logs、config、benchmark、dSYM、临时 iconset 或构建路径。
- [ ] 从最终 ZIP 在干净账户完成 CLI、Finder `.app`、登录项和端到端 smoke。

## 自动证据摘要

```text
cmake --build --preset macos-arm64-release --clean-first       passed
ctest --preset macos-arm64-release                             10/10 passed
cmake --build --preset macos-arm64-warning --clean-first       passed, -Werror
ctest --preset macos-arm64-warning                             10/10 passed
python3 tests/integration/run_integration.py ...                passed
python3 tests/integration/run_macos_proxy_integration.py ...    passed
python3 tests/integration/run_macos_menu_integration.py ...     passed (build tree)
tools/verify_macos_package.sh ...-adhoc-smoke.zip               passed
python3 tests/integration/run_macos_menu_integration.py ...     passed (extracted ZIP)
file / lipo / otool -l / otool -L                              arm64, minOS 26.0,
                                                               SDK 26.5, /usr/lib/libcurl
```

阶段 14 的 8/16/50 路负载、2 小时 mixed soak、8 小时 idle、睡眠唤醒和网络切换按用户指示
全部为 `not run`。完整 Xcode、Developer ID Application identity 与 notary credential 缺失
阻塞正式 codesign/notarization/staple/Gatekeeper/登录项/Finder 候选验收；不把 ad-hoc 结果
表述为正式发行通过。
