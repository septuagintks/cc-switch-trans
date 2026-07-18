# ccs-trans 后续开发计划

## 当前基线

`0.7.0` 是当前双平台基线，发行标识统一为：

```text
0.7.0-Windows-x64
0.7.0-macOS-arm64
```

`0.7.0` 已完成进程级 inflight budget、SQLite 3.53.3/schema v1、`ccs-trans.config/v3`、固定
`profiles.db`、组合恢复 journal、显式 v2 migration、typed command、canonical Rule 文本，以及
Win32/AppKit Profiles、Rules、Settings 三视图。请求热路径仍只读取 immutable generation，不访问
JSON 文件、SQLite、GUI 或 descriptor。

实现、双平台验证、发行物策略和明确未执行项统一归档在
[Release-0.7.0.md](Archived/Release-0.7.0.md)。完整实施取舍保留在
[Planning-0.7.0.md](Archived/Planning-0.7.0.md)，不再作为当前待办来源。历史 `0.5.0/0.6.0` 文档
保持只读。

## 版本顺序

```text
0.8.0  可视化 Rule builder、更多请求改写能力、GUI 交互成熟
  -> 0.9.0  按真实使用补齐能力、修复问题、提高健壮性
  -> 1.0.0  冻结合同、补齐测试、发布完全版
```

每个版本都先完成共享 model/service/repository，再接 Win32 与 AppKit。GUI 不直接读写 JSON、
SQLite 或 RuntimeSnapshot；请求热路径只读取已经编译的 immutable generation，不执行文件 I/O、
SQL、GUI 查询或 Rule descriptor 枚举。

## 0.8.0：可视化 Rule 与交互成熟

1. 根据 Rule descriptor 生成类型匹配的可视化字段，不在 Win32/AppKit view 硬编码 Rule 类型；
2. 提供 Rule 添加、删除、复制、启停、拖动排序和类型切换；
3. 文本与可视化模式共享一个 canonical draft，切换模式不丢字段、不改变顺序；
4. 提供使用用户显式输入且脱敏的本地样例预览和 diff，不默认读取生产日志；
5. 仅根据已确认的真实请求改写场景增加 Rule 类型，并继续保持请求期一次 parse/serialize 合同；
6. 增加 undo/redo、批量操作、错误定位、搜索、键盘操作和完整辅助功能；
7. 对长 Rule 列表使用增量更新和稳定 selection，禁止编辑动作触发无关页面全量重建。

`0.8.0` 同时冻结双平台 GUI 动效合同：所有可交互内容必须同时提供可辨识的 hover colour、hover
scale 和 pressed/click scale 状态，包括导航、按钮、列表行、菜单项、开关、输入容器、Rule 操作和
可点击状态项。缩放只作用于视觉 transform，不改变布局占位、hit target 或相邻控件位置；键盘 focus
与 touch/辅助功能路径提供等价状态，不能把 hover 作为唯一反馈。任何位置、尺寸、展开、折叠、排序或
页面切换等动态移动不得瞬移，至少使用连续线性插值；允许使用更平滑曲线，但不能低于线性动画的连续
程度。遵守 Windows animation/high-contrast 与 macOS Reduce Motion 设置：减少动态时允许把 duration
降为零或最短，但最终 colour/scale/focus 状态仍必须正确。动画期间不得阻塞 UI thread、重排请求热
路径、模糊文本或造成 hover 抖动，并增加帧时间、快速指针进出、连续点击、键盘操作和资源回收测试。

版本结束时，用户应能完全通过 GUI 管理 Profile、全局配置和 Rule，不需要直接编辑数据库或完整
配置文件。

## 0.9.0：能力补齐与健壮性

`0.9.0` 不预设新的大型架构主题，只处理 `0.7.0` 至 `0.8.0` 真实使用暴露的问题：

- config/database/Rule migration 的异常恢复、诊断和备份；
- GUI/CLI 并发、重复启动、service fault、reload rollback 和窗口生命周期竞态；
- 网络切换、睡眠唤醒、系统代理变化、上游慢流、客户端中断和 shutdown；
- 日志轮转、容量缩限、磁盘写满、不可写目录和崩溃尾部恢复；
- 大量 Profile/Rule 下的启动、编译、搜索、编辑和内存占用；
- Windows/macOS 交互差异、打包、签名说明和升级路径；
- 安全审计，包括路径边界、SQL 参数绑定、日志敏感信息和导入文件限制。

非必要的新协议、云同步、自动故障转移、负载均衡和全异步网络栈留到 `1.x` 评估，避免稳定期扩大
范围。

## 1.0.0：冻结与完全版

`1.0.0` 以冻结、验证和发布为主，不再引入新的核心交互或存储模型。

冻结内容：

- CLI 命令、退出码与 repository error；
- config、SQLite schema、迁移、备份和恢复合同；
- Profile/Protocol/Rule descriptor 与 GUI 编辑语义；
- 内存预算、generation 生命周期、日志轮转和故障行为；
- Windows/macOS 窗口、tray/menu、轻量模式、启动项和退出行为；
- 发行包布局、系统/架构标识和最低平台范围。

发布验证：

1. 两平台 Release/warnings clean build、全部 CTest 与 integration；
2. 五档短负载、Rule microbenchmark、并发大 body 和 generation churn；
3. 2 小时 mixed、8 小时 idle、日志多轮轮转和 GUI 长时间常驻；
4. 干净用户目录安装、`0.5.x/0.6.x/0.7.x` 数据迁移、回滚和卸载保留数据；
5. Windows 11 x64 与 macOS 26 arm64 真机 GUI、启动项、代理、睡眠唤醒和网络切换；
6. 同一最终 commit 的双平台固定白名单包、checksum、签名策略和 archive smoke；
7. README、Design、ProjectStructure、release archive、tag 与发行资产完全一致。

## 跨版本质量门槛

触及 listener、worker、transport、logger、repository、RuntimeCompiler 或 Rule pipeline 时至少运行：

- `smoke`、`desktop-8`、`desktop-16`、`mixed-16`、`stress-50`；
- 1 KiB、100 KiB、1 MiB 与 0/1/8/32 Rule microbenchmark；
- 与改动对应的取消、reload、日志、数据库、GUI 或内存专项。

固定要求：

- 常规档零请求失败，Usage 不被 SSE 饿死，logger backpressure/writer failure 为零；
- 每个成功响应校验内容、顺序、长度和结束标记，不能只看 HTTP 200 或聚合字节数；
- 空 Rule pipeline 零 parse；非空 pipeline 最多一次 parse/serialize；
- SQL、GUI、日志轮转和状态通知不得进入请求期锁竞争；
- private/RSS、handle/fd、线程、连接、generation、SQLite statement、logger queue 和 GUI 对象不得
  持续增长；
- GUI 不可见且服务配置未变化时，CPU、timer 和 wake-up 接近当前 tray/menu 基线；
- 50 路允许超过 32 worker 后的有界排队，不把压力档 p95 当作 8-16 路桌面目标。

## 构建与发布顺序

1. 冻结本版本需求、不进入范围、数据合同和可自动验证的验收条件；
2. 实现共享 model/service/repository 与单元测试；
3. 完成迁移、失败回滚和无 UI 的 CLI/integration；
4. 实现共享 view model，再分别接入 Win32 与 AppKit；
5. 执行风险对应的协议、并发、GUI、数据库、日志、内存和性能测试；
6. 更新版本、README、Design、ProjectStructure 与 release archive，创建最终源码 commit；
7. 从该 commit 构建 Windows/macOS 包，核对固定白名单、checksum、版本和 archive smoke；
8. 把两个 ZIP SHA-256 写入签名 annotated tag 和交接记录，推送后再上传发行资产。

平台边界保持不变：Windows 使用 WinHTTP current-user system proxy，选中代理失败不回退直连；
macOS 使用 selected SDK system libcurl 和进程 proxy environment，不激活 System Settings proxy。
Win32/AppKit/SQLite/curl/WinHTTP 类型不得进入 `core/routing/rules/protocols` 公共 header。

任一版本只有在需求、实现、迁移、两平台测试、资源指标、文档、同提交发行包、tag 与明确未执行项
全部可审计时才算完成。
