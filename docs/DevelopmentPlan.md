# ccs-trans 后续开发计划

## 当前基线

`0.5.0` 已于 2026-07-15 完成 Windows 11 x64 与 macOS 26 arm64 双平台发布。
当前生产合同是 C++20、`ccs-trans.config/v2`、单 listener、多 Profile、精确 RouteTable、
有界 FIFO worker、immutable generation、异步有界日志、Windows WinHTTP system proxy 和
macOS SDK system libcurl process proxy。发行结论与平台验证证据见：

- [Release-0.5.0.md](Archived/Release-0.5.0.md)；
- [WindowsValidationCheckResult.md](Archived/WindowsValidationCheckResult.md)；
- [MacOSValidationCheckResult.md](Archived/MacOSValidationCheckResult.md)。

后续版本按 `0.6.0 -> 0.7.0 -> 0.8.0 -> 0.9.0 -> 1.0.0` 推进。Windows 与 macOS
始终使用同一数字版本，系统和架构只进入发行标识，例如 `<version>-Windows-x64` 与
`<version>-macOS-arm64`。

## 调整后的实现顺序

```text
日志容量上限和轮转
  -> 配置编辑事务与存储抽象
  -> 0.6.0 GUI 壳、轻量模式、基础 Profile 管理
  -> 0.7.0 SQLite Profile/Rule 存储和完整字段编辑
  -> 0.8.0 可视化 Rule 编辑器与交互完善
  -> 0.9.0 缺口补齐和健壮性
  -> 1.0.0 完整验证与稳定发布
```

日志限制放在任何新 GUI 工作之前。GUI 不直接读写 JSON 或 SQLite，只调用共享编辑服务；
`0.6.0` 先建立这个边界，`0.7.0` 才替换 Profile/Rule 的存储后端。请求热路径继续只读取
已编译的 immutable RuntimeSnapshot，不在每个请求中查询文件、GUI 状态或数据库。

## 0.6.0 前置工作

### 1. 日志容量限制

`0.6.0` 的首个 release blocker 是为运行日志实现有界保留，默认上限按二进制
`2 GiB = 2147483648 bytes` 计算。该上限必须限制一个日志族的当前文件与受管理轮转分片之和，
不能只限制当前文件后无限保留归档。

计划合同：

- `logging.max_total_size` 保存字节数，默认 `2147483648`；CLI 使用唯一键
  `logging.max-total-size`，不增加别名；
- `max_total_size` 必须大于可能生成的最大单条日志记录；配置校验同时约束 body limit、queue
  capacity 与记录固定开销，不能靠写入时拆行绕过上限；
- 配置字段可省略并使用默认值，使现有 `ccs-trans.config/v2` 可以直接加载；保存后允许写入
  新字段，不承诺新配置可被 `0.5.0` 反向读取；
- 当前日志路径保持不变，轮转分片使用只由 ccs-trans 管理且可排序的文件名；清理时绝不匹配
  或删除不属于该日志族的文件；
- 轮转、flush、rename 和清理只在 logger writer 线程执行，不让请求 worker 承担文件系统 I/O；
- 只在完整 JSON Lines 记录之间轮转，不拆分单条记录；接近分片边界的记录按完整记录写入；
- 启动时扫描当前文件和受管理分片，恢复容量账本；对升级前已经超过上限的单文件保留最新的
  完整记录并删除最旧内容，不以静默保留超限文件作为兼容策略；
- 限额缩小时，在新配置正式发布前完成可行性校验；新 writer 接管后按时间从旧到新清理；
- rename、删除、磁盘写满或权限错误进入现有 writer failure 通道，不允许悄悄退回无限增长；
- `ccs-trans-host.log` 同样必须有界，但采用独立的小容量日志族，不能挤占运行日志的 2 GiB
  预算，也不能与运行日志共享 writer；
- 增加 rotation 次数、已清理文件数/字节数和当前受管理字节数指标，日志中不得记录被删除内容。

实现时优先在默认文件 sink 内封装轮转与保留策略，保持 Logger 的有界 queue、100 ms 批写、
error 立即 flush 和可注入测试 sink 语义不变。必须覆盖边界前后写入、进程重启恢复、旧版超大
单文件、Unicode/空格路径、配置缩限、并存 generation、删除失败、磁盘写满模拟，以及 Windows
文件关闭后 rename 和 macOS rename 行为。

### 2. 并发与日志基线

在 GUI 和持久化改动前固定当前基线：

- 相同配置、相同路径的并发 SSE 必须逐请求校验完整内容、顺序、长度和结束标记；
- `desktop-8`、`desktop-16`、`mixed-16` 与 `stress-50` 必须按每个响应的预期字节数判定，
  HTTP 200 但内容截断仍算失败；
- 日志轮转期间继续要求 `logger backpressure=0`、`writer failure=0`，SSE 不为日志聚合完整 body；
- 日志改动完成后执行 2 小时 mixed 与 8 小时 idle，确认容量稳定、轮转后可继续写入且退出可 drain。

### 3. GUI 与存储边界

在创建主窗口前增加平台无关的配置编辑层，职责包括：

- 产生独立 draft，执行字段级修改、Profile rename 和启用/停用；
- 完整运行 ConfigDocument 校验与 RuntimeCompiler，成功后才原子提交；
- 失败 draft 不影响磁盘配置、当前 RuntimeSnapshot 或正在处理的请求；
- 向 GUI 返回稳定的字段错误、冲突和运行状态，不暴露 Win32、AppKit、JSON DOM 或 SQLite handle；
- CLI 和 GUI 复用同一 command/service，不形成两套写入规则；
- 预留 ProfileRepository/RuleRepository 接口，但 `0.6.0` 仍由现有 ConfigStore 持久化。

这个边界是 `0.7.0` 切换 SQLite 时避免重写 GUI 的必要条件，不在请求路径增加间接层。

## 0.6.0：GUI 主界面与基础管理

目标是让当前 tray/menu bar 宿主拥有可实际管理多 Profile 的主窗口，同时保持轻量常驻能力。

### 功能范围

1. Windows tray 与 macOS menu 增加“打开主界面”；重复触发只激活现有窗口；
2. 菜单增加“轻量模式”勾选项。启用后不缓存已关闭的主窗口，关闭窗口即销毁平台窗口和 view
   资源；listener、tray/menu、controller 与服务状态继续存在；再次打开时从共享状态重建窗口；
3. 主界面提供服务状态、Profile 列表、名称、启用状态以及创建、重命名、删除和选择；
4. 所有修改先进入 draft，明确应用后才持久化并 reload；离开未保存 draft 时必须提供一致处理；
5. Profile 列表支持空状态、校验失败、route collision、服务 stopped/faulted 和 reload rollback；
6. Windows 与 macOS 保持相同字段和状态语义，平台代码只负责原生窗口、控件和辅助功能映射；
7. 扩展 API 请求体 Rule 能力与 Rule descriptor 元数据，但本版本不提前制作最终可视化 Rule
   编辑器；新增能力必须有 CLI、配置和 pipeline 测试入口。

### 架构约束

- 主窗口复用 ApplicationController、AppService 和 control executor，不复制 server/logger 初始化；
- UI 线程不得等待 listener bind、worker join、logger drain、SQLite/file I/O 或长时间 RuntimeCompiler；
- GUI 只保存展示状态和 draft，不成为运行配置的事实来源；
- 关闭或销毁窗口不能停止服务；退出 tray/menu 宿主才执行完整 stop、cancel、join 和 logger drain；
- Rule descriptor 从共享 registry 提供类型、选项、值类型和校验信息，为 `0.7.0` 文本编辑与
  `0.8.0` 可视化编辑共用，避免 UI 硬编码 Rule 类型分支；
- 暂不引入每帧刷新或高频 polling，状态变化通过有界通知或低频快照传递。

### 验收重点

- 多 Profile 创建、rename、enable/disable、删除、冲突回滚与重启持久化；
- 主窗口反复打开/关闭 100 次，轻量模式下窗口资源回到基线且服务不中断；
- tray/menu、CLI 和 GUI 修改使用同一校验结果；
- GUI 打开、隐藏或销毁时，`desktop-16` 的吞吐与延迟无显著回退；
- 两个平台真机检查窗口恢复、DPI/Retina、浅色/深色、键盘焦点和辅助功能基本行为。

## 0.7.0：SQLite Profile 存储与完整字段编辑

目标是把 Profile 与 Rule 从单体 JSON 文档迁入事务型存储，并让 GUI 覆盖 Profile 字段和应用级
配置字段。请求体编辑先使用有校验能力的文本配置界面，其他字段使用适合其类型的输入控件。

### 存储顺序

1. 冻结 SQLite schema、主键、排序、外键、唯一约束、schema version 和迁移事务；
2. Windows 与 macOS 使用同一受控 SQLite 版本和编译选项，完成许可证与打包检查；
3. 应用级 listener/runtime/timeouts/logging 继续保存在 config 文档，Profile/Rule 写入
   `~/.ccs-trans/profiles.db`；
4. RuntimeCompiler 从一个只读 repository snapshot 组装完整候选配置，编译成功后才发布 generation；
5. 提供显式的一次性 JSON Profile 导入、导入前备份、失败回滚和可重复检测，不做静默部分迁移；
6. CLI 与 GUI 的增删改均通过同一事务服务，数据库提交与 runtime reload 失败必须有清晰状态；
7. 请求热路径只使用编译后的内存对象，禁止每请求 SQL、lazy Rule 查询或持有数据库锁。

### GUI 范围

- Profile 的 protocol、本地 request/Usage 路径、upstream URL/path 和启用状态使用参数控件；
- listener、worker、连接数、timeouts、logging 等全局配置使用类型匹配的输入框、选择器、开关；
- API 请求体 Rule 使用带语法高亮、schema 校验、定位错误和格式化能力的文本配置界面；
- 文本编辑只操作当前 Profile 的 Rule draft，不允许粘贴任意全局数据库结构；
- 保存前展示字段错误和 route collision，应用后展示 generation/reload 结果；
- 敏感请求 body、日志内容和 Authorization 不进入数据库或 GUI 自动历史。

### 验收重点

- 空库初始化、v2 JSON 导入、重复导入、schema migration、损坏数据库、只读目录、磁盘写满；
- CLI/GUI 并发编辑、进程崩溃恢复、WAL/临时文件清理、备份恢复和卸载后数据保留；
- 128 Profile、256 Route、每 Profile 64 Rule 的读取、编译和 GUI 列表性能；
- 迁移前后相同输入生成等价 RuntimeSnapshot，SSE/Usage/Rule 行为不变。

## 0.8.0：可视化 Rule 编辑与交互成熟

目标是把 `0.7.0` 的文本 Rule 配置升级为成熟的可视化编辑工作流，并继续丰富 API 请求体改写能力。

1. 提供 Rule 列表、启用/停用、添加、删除、复制、拖动排序和类型切换；
2. 根据 Rule descriptor 生成类型匹配的字段编辑器，不在平台 view 中复制 Rule 校验；
3. 文本与可视化模式共享一个 canonical draft，切换模式不会丢字段或改变 Rule 顺序；
4. 提供使用脱敏样例输入的本地预览与差异，不读取生产日志作为默认样例；
5. 根据已确认的真实场景新增 Rule 类型，继续保持空 pipeline 零 parse、非空 pipeline 最多一次
   parse/serialize；
6. 完善导航、批量操作、撤销/重做、错误定位、键盘操作、DPI/Retina 和辅助功能；
7. 对大 Profile 和长 Rule 列表使用增量 view model，禁止编辑动作触发无关页面全量重建。

本版本结束后，普通用户应能不编辑原始数据库或完整配置文件完成 Profile、全局设置与 Rule 管理。

## 0.9.0：能力补齐与健壮性

`0.9.0` 不预设新的大型架构主题，按 `0.6.0` 至 `0.8.0` 的真实使用结果补齐缺失能力并修复问题：

- 配置、数据库和 Rule migration 的异常恢复与诊断；
- GUI/CLI 并发、重复启动、服务 fault、reload rollback 和窗口生命周期竞态；
- 网络切换、睡眠唤醒、系统代理变化、上游慢流、客户端中断和 shutdown；
- 日志轮转、容量缩限、磁盘写满、不可写目录和崩溃尾部恢复；
- 大量 Profile/Rule 下的启动、编译、搜索、编辑和内存占用；
- Windows/macOS 交互差异、打包、签名说明和升级路径；
- 安全审计，包括路径边界、SQL 参数绑定、日志敏感信息和导入文件限制。

只有阻塞 `1.0.0` 完整使用的问题进入本版本；非必要的新协议、云同步、自动故障转移、负载均衡和
全异步网络栈继续留在 `1.x` 评估，避免在稳定期扩大范围。

## 1.0.0：测试补齐与完全版发布

`1.0.0` 以冻结、验证和发布为主，不再安排新的核心交互模型。

### 冻结内容

- CLI 命令和退出码；
- config 与 SQLite schema、迁移和备份合同；
- Profile/Protocol/Rule descriptor 与 GUI 编辑语义；
- 日志轮转、默认 2 GiB 上限和故障行为；
- Windows/macOS 窗口、tray/menu、轻量模式、启动项和退出行为；
- 发布包布局、系统/架构标识和最低平台范围。

### 发布验证

1. 两个平台 Release clean build、warnings-as-errors、全部 CTest 与 integration；
2. `desktop-8`、`desktop-16`、`mixed-16`、`stress-50` 和 Rule microbenchmark；
3. 2 小时 mixed、8 小时 idle、日志多轮轮转和 GUI 长时间常驻；
4. 干净用户目录安装、`0.5.x/0.6.x/0.7.x` 数据迁移、回滚和卸载保留数据；
5. Windows 11 x64 与 macOS 26 arm64 真机 GUI、启动项、代理、睡眠唤醒和网络切换；
6. 从同一最终 commit 构建两个固定白名单发行包并做 archive smoke；
7. 记录 commit、SHA-256、工具链、签名策略、已执行测试和明确接受限制；
8. README、Design、DevelopmentPlan、ProjectStructure 与最终行为一致后创建并推送签名 tag。

## 跨版本性能与质量门槛

触及 listener、worker、transport、logger、repository、RuntimeCompiler 或 Rule pipeline 时，至少运行：

- `smoke`：快速发现协议和采样器错误；
- `desktop-8`、`desktop-16`：常规桌面 SSE 负载；
- `mixed-16`：Responses/Chat SSE 与两组 Usage 并行；
- `stress-50`：验证 32 worker 下排队、连接和资源保持有界；
- 1 KiB、100 KiB、1 MiB，0/1/8/32 Rule microbenchmark。

固定要求：

- 常规档零请求失败，Usage 不被 SSE 饿死，logger backpressure/writer failure 为零；
- 每个成功响应校验预期长度；需要时校验完整内容和请求归属，不能只看聚合字节数；
- 空 Rule pipeline 零 JSON parse；非空 pipeline 最多一次 parse/serialize；
- SQLite、GUI、日志轮转和状态通知不得进入请求期锁竞争；
- GUI 不可见且服务配置未变化时，CPU 与 wake-up 应接近当前 tray/menu 基线；
- 50 路允许超过 worker 上限后的有界排队，不把压力档 p95 当作 8-16 路目标；
- 内存、handle/fd、线程、连接、SQLite statement、logger queue 或 GUI 对象持续增长均视为失败。

## 每个版本的构建顺序

1. 冻结本版本需求、不进入范围、数据合同和可自动验证的验收条件；
2. 先实现共享 model/service/repository 接口与单元测试；
3. 完成迁移、失败回滚和无 UI 的 CLI/integration 路径；
4. 实现共享 view model，再分别接入 Win32 与 AppKit view；
5. 执行风险对应的协议、并发、GUI、数据库、日志和性能测试；
6. 从同一最终 commit 构建 Windows/macOS 包，核对固定白名单和 archive smoke；
7. 更新 README、Design、ProjectStructure、版本归档和跨设备 `.codex/HandOff.md`；
8. 工作区干净且证据可审计后提交、推送、创建 tag 和 Release。

平台边界保持不变：Windows 使用 WinHTTP current-user system proxy，选中代理失败不回退直连；
macOS 使用 selected SDK system libcurl 和进程 proxy environment，不激活 System Settings proxy。
Win32/AppKit/SQLite/curl/WinHTTP 类型不得进入 `core/routing/rules/protocols` 公共 header。

## 完成定义

任一后续版本只有同时满足以下条件才结束：

- 需求、不进入范围、实现和迁移行为一致；
- 两个平台适用的 clean build、CTest、integration 和风险驱动真机测试完成；
- 性能、资源、日志和数据库指标无未解释回退；
- README、Design、DevelopmentPlan、ProjectStructure 与实际命令和目录一致；
- 两个平台包来自同一最终 commit，固定白名单与 archive smoke 通过；
- tag、Release 资产、SHA-256 和签名策略可审计；
- 未执行项明确标为 `not run` 或 `accepted limitation`，不以另一平台结果代替。
