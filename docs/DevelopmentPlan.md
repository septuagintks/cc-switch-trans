# ccs-trans 后续开发计划

## 当前基线

`0.6.0` 是当前双平台基线，发行标识统一为：

```text
0.6.0-Windows-x64
0.6.0-macOS-arm64
```

当前实现使用 ISO C++20、`ccs-trans.config/v2`、应用级单 listener、多 Profile、精确
RouteTable、有界 FIFO worker、immutable request generation、异步有界日志、Windows WinHTTP
system proxy 和 macOS SDK system libcurl process proxy。Windows tray 与 macOS menu 均提供原生
主窗口、普通/轻量窗口生命周期、服务控制和基础 Profile 管理；CLI 与 GUI 通过同一
ConfigRepository/ConfigEditingService 合同提交配置。

`0.6.0` 的实现、测试、资源结果、发行物溯源和接受限制统一归档在
[Release-0.6.0.md](Archived/Release-0.6.0.md)。历史 `0.5.0` 文档保持只读，不再作为当前状态来源。

## 版本顺序

```text
0.7.0  SQLite Profile/Rule 存储、完整字段编辑、文本 Rule 编辑
  -> 0.8.0  可视化 Rule builder、更多请求改写能力、GUI 交互成熟
  -> 0.9.0  按真实使用补齐能力、修复问题、提高健壮性
  -> 1.0.0  冻结合同、补齐测试、发布完全版
```

每个版本都先完成共享 model/service/repository，再接 Win32 与 AppKit。GUI 不直接读写 JSON、
SQLite 或 RuntimeSnapshot；请求热路径只读取已经编译的 immutable generation，不执行文件 I/O、
SQL、GUI 查询或 Rule descriptor 枚举。

## 0.7.0：SQLite 与完整字段编辑

目标是把 Profile/Rule 从单体 JSON 文档迁入事务型存储，并让 GUI 覆盖 Profile 字段、应用级
配置和文本 Rule 编辑。迁移不能改变路由、SSE、Usage、Rule 顺序或上游转发语义。

### 0.7-A：内存与性能基线

在引入 SQLite 和更复杂编辑器前先固定资源合同：

1. 为 raw HTTP 接收、parsed request body、Rule JSON DOM、serialized rewritten body、buffered
   response、non-streaming send frame 和 body-log staging 建立全进程 inflight-byte 记账；单请求大小
   限制和 logger queue 容量不能代替全局内存预算；
2. 明确预算耗尽时采用有界排队还是本地拒绝，并提供稳定状态码、错误类型和 metrics；不得等待时
   同时保留未记账的大 body；
3. 增加 current/peak retired generation 与在途请求计数，验证连续 reload 加长 SSE 时旧
   RuntimeSnapshot、transport 和 logger 只保留到对应请求结束；指标保持全局低 cardinality；
4. 给 macOS cURL slot 构造和其他 native handle 异常路径补齐 RAII，覆盖分配失败、初始化失败和
   callback 提前终止；为 macOS upstream response headers 增加显式聚合上限；
5. 让 logger 入队在 allocation failure 下保持 queue、pending counters 和 metrics 一致；限制或记账
   producer 在线程本地渲染的记录，并为 GUI control executor 增加容量或等价的 command coalescing；
6. 增加并发大 body、Rule parse/serialize、reload churn、客户端中断、allocation fault injection 和
   shutdown 资源测试；记录 private/RSS、handle/fd、线程、连接、generation、control task 和 logger
   queue 曲线；
7. 保持默认 `worker_threads=32`、`max_connections=64`、日志 queue 16 MiB 和运行日志 2 GiB，
   除非新基准证明需要改变；不以提高上限掩盖副本或生命周期问题。

退出条件：常规 8-16 路负载无资源回退；压力场景的内存、generation、连接和队列有明确上限；
空 Rule pipeline 仍为零 JSON parse，非空 pipeline 最多一次 parse/serialize。

### 0.7-B：SQLite 合同与依赖

1. 选择并固定 SQLite source/version、编译选项、threading mode 和许可证；Windows/macOS 使用同一
   版本，不依赖机器上偶然存在的动态库；
2. schema 包含 Profile stable id、排序、protocol、local/upstream 字段、Rule 顺序、enabled 状态、
   schema version 和 repository revision；
3. 使用外键、唯一约束、CHECK 和事务保证不存在孤立 Rule、重复顺序或部分 Profile；
4. 数据库存放在 `~/.ccs-trans/profiles.db`。listener、runtime、timeouts 和 logging 等应用级设置
   继续由严格 config 文档保存；
5. 定义 WAL/rollback journal、busy timeout、checkpoint、同步级别、临时文件和异常退出恢复策略；
6. 所有 SQL 参数绑定，错误转换为稳定 repository failure，不把 SQL 文本或敏感值写入日志。

退出条件：空库初始化、schema round-trip、约束失败、只读目录、锁冲突、损坏数据库和进程中断均有
确定结果；数据库文件和第三方许可证进入两平台固定包白名单。

### 0.7-C：迁移与 repository

1. ConfigRepository 演进为可组合 snapshot：应用设置来自 config，Profile/Rule 来自 SQLite；
2. 提供显式的一次性 v2 JSON Profile 导入，导入前备份原文，整批事务提交，失败完整回滚；
3. 重复导入必须可识别，不静默覆盖已经编辑的数据库；成功后保留可审计 migration marker；
4. CLI 与 GUI 共享 optimistic revision/stale 检测，跨进程并发修改不得 last-writer-wins；
5. repository commit 成功但 runtime reload 失败继续使用 `SavedPendingRuntimeApply`，不伪装为数据库
   回滚；
6. RuntimeCompiler 从一个只读组合 snapshot 构建完整 generation，编译成功后才发布；请求期禁止
   SQL、lazy Rule 查询或持有数据库锁。

退出条件：迁移前后相同输入得到等价 RuntimeSnapshot；CLI/GUI 并发、崩溃恢复、备份恢复、重复
导入和卸载后数据保留均有自动测试。

### 0.7-D：Profile 与全局字段 GUI

1. Profile 页面增加 protocol、本地 request/Usage path、upstream base/request/Usage path 与启用
   状态；按字段类型使用输入框、选择器和 checkbox；
2. 全局设置页覆盖 listener、worker、连接数、body limits、timeouts 和 logging；二元设置使用
   checkbox，枚举使用菜单，数值使用带范围校验的输入控件；
3. Windows 与 macOS 消费相同 field descriptor、错误和 command result，不在平台 view 复制校验；
4. draft 导航、Apply/Discard/Reload、dirty close、stale recovery 与当前 Profile 管理保持一致；
5. 大 Profile 列表和字段更新使用增量 view model，不因单字段编辑重建无关窗口；
6. 完成 DPI/Retina、light/dark、键盘焦点、accessibility label、长 URL/Unicode 和最小尺寸验证。

退出条件：普通用户不编辑 config/数据库即可完整建立、修改和诊断 Profile 与应用设置。

### 0.7-E：文本 Rule 编辑

1. 当前 Profile 的 Rule draft 使用独立文本编辑界面，不允许粘贴任意全局数据库结构；
2. 文本格式具有稳定 canonical schema，支持格式化、语法错误定位、descriptor 校验和 Rule 顺序；
3. 编辑器只操作 draft；保存前统一执行 Protocol capability、Rule compile、route collision 和完整
   RuntimeCompiler 校验；
4. 敏感 request body、Authorization、日志内容和生产请求样例不进入编辑器历史；
5. 文本模型成为 `0.8.0` 可视化 builder 的唯一 canonical draft，避免两套 Rule 表示互相转换丢失。

退出条件：现有 `set_field`、`remove_field`、`remove_tool` 可通过文本界面完整增删改、排序、启停，
CLI/GUI round-trip 不改变未知但合法的字段顺序语义。

### 0.7-F：候选与发布

1. 两平台 clean Release、warnings-as-errors、全部 CTest、shared/proxy/GUI integration；
2. JSON 导入、SQLite transaction/lock/recovery、CLI/GUI stale 和 runtime rollback 矩阵；
3. 128 Profile、256 Route、每 Profile 64 Rule 的加载、编译、列表和编辑性能；
4. 五档短负载、Rule microbenchmark、并发大 body、reload churn、2 小时 mixed、8 小时 idle；
5. 从同一最终 commit 构建 `<version>-Windows-x64` 与 `<version>-macOS-arm64`，完成固定白名单、
   checksum、版本、签名策略与解包 smoke；
6. 归档 commit、工具链、测试计数、SHA-256、未执行项和接受限制后创建签名 annotated tag。

## 0.8.0：可视化 Rule 与交互成熟

1. 根据 Rule descriptor 生成类型匹配的可视化字段，不在 Win32/AppKit view 硬编码 Rule 类型；
2. 提供 Rule 添加、删除、复制、启停、拖动排序和类型切换；
3. 文本与可视化模式共享一个 canonical draft，切换模式不丢字段、不改变顺序；
4. 提供使用用户显式输入且脱敏的本地样例预览和 diff，不默认读取生产日志；
5. 仅根据已确认的真实请求改写场景增加 Rule 类型，并继续保持请求期一次 parse/serialize 合同；
6. 增加 undo/redo、批量操作、错误定位、搜索、键盘操作和完整辅助功能；
7. 对长 Rule 列表使用增量更新和稳定 selection，禁止编辑动作触发无关页面全量重建。

版本结束时，用户应能完全通过 GUI 管理 Profile、全局配置和 Rule，不需要直接编辑数据库或完整
配置文件。

## 0.9.0：能力补齐与健壮性

`0.9.0` 不预设新的大型架构主题，只处理 `0.6.0` 至 `0.8.0` 真实使用暴露的问题：

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
