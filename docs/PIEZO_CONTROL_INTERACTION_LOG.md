# PIEZO_CONTROL_DELIVERY 用户决策与交互记录

## 2026-10-20 PC-11 内存门槛修订与 Run04

- 用户明确将 PC-11 内存门槛改为**整机物理 RAM 使用率严格低于 90%**，覆盖本记录 2026-10-13 的历史 512,000,000 byte 选择；后续 PC-11 内存评估必须使用整机 RAM 采样。
- Run04 job `2026102019340001` 在私有安装 GUI 中实际完成 20,000 个 FE accepted steps，但任务记录持久化锁报错，状态停在 `queued`；没有产品终态登记、真实结果图或完整 CSV。PC-11-A02 维持 pending，solver 完成不能单独算作验收通过。
- 本次整机 RAM 50ms 采样峰值 76.5929%，90,839 个样本，低于用户选定上限；GUI working-set sampler 因 GUI 正常退出后无法查询 PID 而 INVALID，不用于该门槛判定。
- 用户要求计算结束后退出；solver 自然退出，GUI 经正常 Quit 确认退出，无强杀。验收脚本因未见 terminal registration 退出码 1。完整事实见 [`Run04 收据`](../build/audit-20261020/piezo-acceptance/focused-functional-002/pc11-current-gui-r4-run-04/run04-disposition.json) 和 [`续接交接`](PIEZO_CONTROL_CONTINUATION_HANDOFF.md)。

## 2026-10-20 PC-00 同配置补测

- 当前隔离 RelWithDebInfo 配置下三个原生控制/格式/真实 FE CLI 目标构建 exit 0，CTest 3/3 通过，14.69 秒。收据为 `audit/pc00-registry-fixture-20261020-attempt-03/PC-00-A02-attempt-03-receipt.json`，SHA-256 `78b983c7865ed487fd5fcb46546b9818356f80fbf6eebbeb33e06fe99fa33667`；绑定当前相关源、测试及 solver 可执行文件哈希、JUnit/text log，并确认真实 CLI 子进程已启动。
- 覆盖包含格式 1.3.21 持久化、1.3.20 兼容、原生 PI、RAM/RBM 分段续算后的完整状态比对、tampered restart 拒绝及发布回滚。独立只读审查已保存于 `docs/acceptance/PC-00-A02-A03-independent-review-20261020.md`，明确补入 attempt03 未列出的 `rml_file_io.cpp` 哈希并记录兼容证据边界；PC-00-A02/A03 升为 pass，PC-00 三项均通过，整体正式计数 33/36。
- 旧 Release PID 42236/24960 是带 `--pause-at-accepted-seq=10` 的持久化失败夹具，依用户要求保持运行、未发送信号；它不是 PC-11 的 20,000 步 GUI 运行。整机 RAM 约 66%。

## 2026-10-20 PC-00 持久化锁聚焦验收

- RelWithDebInfo 生命周期目标与最新锁获取修正增量构建均 exit 0；聚焦 QtTest `gracefulStopPersistenceFailureSendsNoStop` **3/0/0**，4.653 秒。收据在 `audit/pc00-registry-fixture-20261020-attempt-02/`，attempt-02 receipt SHA-256 `a035cb28a4089d1ff966de2827ff06ac5b1a96437972ff00261de70d9a417ce6`。
- attempt-01 的测试夹具失败是 Windows 文件共享限制：`QLockFile` 仍持锁时，第二个 `QFile(ReadWrite)` 无法打开该锁文件。修复夹具后，真实锁冲突检查与“老化的同进程活锁”检查分开执行，最后仍验证死 PID 陈旧锁恢复。attempt-01 失败证据保留，不覆盖。
- PC-00-A02 尚需在同一 RelWithDebInfo 源/二进制配置跑格式 1.3.21、原生控制及真实 FE CLI 测试；PC-00-A03 尚需新鲜独立审查和兼容/回滚证据。计划仍为 1/36 pass，不提前升格。旧 Release PIDs 42236/24960 依用户要求继续保留，未发信号或清理。

## 2026-10-20 科学运行时测试续作

- 系统 Python 的 NumPy 2.1.3 与项目冻结的 NumPy 2.2.6 不一致，控制器数值测试中的版本断言失败；这是验收环境不匹配。切换仓库私有运行时 `build/private-python`（CPython 3.13.5、NumPy 2.2.6、SciPy 1.15.3）后，五个 `test_piezo_*.py` 模块由标准库 unittest 跑完 **152/152 PASS**，用时 63.011 秒。运行时、日志及相关源码 SHA-256 已存入 `build/audit-20261020/piezo-acceptance/python-piezo-unittest-001/receipt.json`。该诊断不替代真实 FE/GUI 验收，36 项正式台账仍为 1 pass。
- RelWithDebInfo lifecycle 目标隔离构建仍在推进，已过 `range-solver-lib`、`range-application-lib`、`range-native-results`、`range-project`、`range-mesh-worker`，正在编译 `fea-solver`。旧 QtTest 与 solver 测试进程按用户“保持运行直到完成”指示保留；不得杀进程、暂停或清理临时检查点。整机 RAM 约 68%。

## 2026-10-20 锁行为复核与续作状态

- Luna 对注册锁修补作了独立复核，并查证 Qt 6.11 源码：非零 `staleLockTime` 在不具备原生文件锁保护的环境中可能依文件年龄过期仍活着的锁。因此当前修订选择 `setStaleLockTime(0)`，保留同机死 PID／重启检测，不以年龄抢占活动写锁；若锁获取失败，分别记录锁冲突、权限拒绝或文件系统 I/O 错误。代码尚未重新构建/测试。
- 更正此前“`staleLockTime(0)` 禁用自动死锁恢复”的解释：0 只禁用时间戳年龄判断，Qt 仍按同机所有者进程状态识别陈旧锁。10 月 20 日旧观察只证明 GUI 退出后有锁文件存在，未证明退出后的下一次获取尝试失败。
- PC-00 两个原生控制诊断目标当前树 **2/2 PASS、1.14 秒**，收据日志/JUnit 在 `audit/pc00-registry-fixture-20261020-attempt-01/`；这不单独关闭 A02。GUI 生命周期回归因夹具只读设置时间失败而未完成；测试执行进程及 10 步 solver 子进程仍存活，按用户指示原样保留，不发信号、不清理其检查点。夹具已改成 ReadWrite、修正死 PID 样例字段顺序，并加入老化中的活锁断言，待无冲突后重建验证。
- PC-11-A02 的精确持久化失败分支仍未确认。下一次真实 GUI Run＋Confirm 必须保存新应用日志，确认结果 registry/journal/CSV/首图，并记录整机 RAM `<90%` 序列。原旧阻塞 JSON 与 lock 观察文件是不可变证据，不覆写。正式计划仍 **1/36 pass，`productComplete=false`**；PC-11-A01 继续保留功能并等待权利人授权。

## 2026-10-20 性能门槛修订与续算指示

- 用户确认 PC-11 的内存验收改为整机物理 RAM 使用率严格低于 90%；GUI/solver 进程树 working set 只作诊断，不再作为阻断门槛。当前机器性能合同已追加授权和 supersedes 说明；10 月 14 日以前的 512,000,000-byte 记录保留为历史决策/历史结果，不用于本轮判定。
- 用户要求设备持续运行，不暂停或退出，直到验收通过；若确有外部阻塞，保留并原样记录阻塞及过程。PC-11-A01 继续等待 range-ai-lib 授权材料，保留功能。
- 当前 12 阶段/36 项仍需逐项用符合冻结范围的证据验收；局部测试和旧版 PC-10 矩阵不可替代正式收据。

## 2026-10-14 14:42 交接后退出（最新用户指示）

- 用户要求“按技能整理交接文件后退出，并告诉我开发集成相关进度的百分位”。这改变本聊天原先“持续开发至完整交付”的执行时长；下一平台仍接续原冻结目标。当前快照见 [交接页](PIEZO_CONTROL_HANDOFF_20261014_1440.md)。
- 使用 `planning-with-files` 的现有具名计划记录交接，沿用 `astra-orchestrator` 的 root 整合与独立审查边界；未使用 Codex with ChatGPT。交接页给出正式接受度 0/36、局部科学矩阵 29/30 等有分母数字；不凭源码量猜测整体开发百分比。
- 本聊天启动的 v45 诊断构建、板最后一行科学测试通过 Ctrl+C 停止，保留原输出；完成的梁与板行收据不改写。没有产品 GUI/Save/Run/Confirm。

## 2026-10-14 当前有效指示

- 从共享工作区现有断点继续完成 `PIEZO_CONTROL_DELIVERY`，**不使用 Codex with ChatGPT**。本地执行、审查与证据归档继续；旧临时地址/pairing 不再调用。
- Luna 优先执行有边界的任务，确属困难推理再用 Sol medium，独立 reviewer 按本项目技能要求为 Sol medium；root 负责架构、集成和最终验收。
- GUI 同时验收视觉平衡、各模块操作逻辑与少操作链；三家族各 20,000 个接受 FE 步外加初始记录、首图 5 秒、完整 CSV 30 秒、GUI+计算子树合并峰值 512,000,000 bytes 的冻结门槛不变。
- 配额只在**新鲜状态确认**剩余 `<=1%` 且有卡时可重置。旧 8%／0 卡文件及 PID 22236 已过期、监控后来停止；没有当前读数或工具时不得把旧快照当成当前授权。
- 当前执行断点、运行中 Agent、冻结包成败及其原始证据以 [`PIEZO_CONTROL_CONTINUATION_HANDOFF.md`](PIEZO_CONTROL_CONTINUATION_HANDOFF.md) 顶部和审计目录为准；下文 2026-10-13 表格是历史交接时的用户选择与事实快照。

## 2026-10-13 最新续作指示

- 用户要求从上一断点继续开发，并明确**不使用 Codex with ChatGPT**。这覆盖下文早前“使用 C2C 节省额度”的选择；当前本地续作不调用该技能或连接。
- 完整交付目标、GUI 视觉与操作便利、Luna 优先及 Sol medium 独立 reviewer、20k 三家族性能合同、重置卡仅在 `<=1%` 且有卡时使用的要求均继续有效。
- 当前执行位置和真实新证据以根 [`HANDOFF.md`](../HANDOFF.md) 顶部与本仓 audit 收据为准；本页旧 10:05 状态为当时快照。

此记录整理本次压电控制开发和跨平台接管相关的用户明确要求。它记录决策与状态，不代替原始聊天、冻结合同或验证收据；若后续用户改变要求，以更新后的明确指示为准。

## 2026-10-13 跨平台续接交接

### 用户决策

| 主题 | 用户确认 | 执行约束 |
|---|---|---|
| 开发连续性 | 意外退出后，从断点继续执行 `PIEZO_CONTROL_DELIVERY`，持续到功能完整交付。 | 先恢复当前状态和既有证据，不从头重跑已完成阶段；不能因本轮交接请求把功能目标缩小。 |
| 额度 | 重置卡只在剩余 1% 时使用。 | 按冻结操作守则，实际使用条件为剩余 `<=1%` 且可用卡数量大于 0；不用卡前不重置，不能购买额度。上一张卡已消耗；本次快照 8%、0 卡。 |
| 性能内存 | 20k 结果的 GUI 与计算合并峰值内存上限选 **512 MB**。 | 数值门槛严格记为 512,000,000 bytes，按 50ms 采样，计入 baseline 和已核实进程树，不扣共享库。 |
| GUI 质量 | 各模块和操作逻辑要合理，视觉感受平衡，操作便捷。 | 真实窗口布局和可操作路径需要验收；保留集中预览/一次确认、少量主动步骤的设计方向。 |
| Agent 调度 | Luna 优先；明确困难问题才用 Sol medium；reviewer 按技能要求仍使用 Sol medium。 | 新平台遵守任务复杂度路由、独立审查与文件所有权边界；不为了并行而拆琐碎任务。 |
| ChatGPT 辅助（历史） | 曾要求使用 `codex-with-chatgpt` 技能节省额度；2026-10-13 后续明确改为**不使用**。 | 当前有效要求是不调用该技能、连接或旧临时配对。保留原选择仅作交互历史。 |
| 网络入口 | 使用临时地址；没有配置域名。 | 不要求自有域名或 DNS；临时 Quick Tunnel 可能变化/失效，使用前重新检查当前 endpoint 和配对状态。 |
| 接管 | 用户确认旧聊天（ID 前缀 `01a12c0e`）已停止，由本聊天接管。 | 本仓是共享工作区；后续接手前仍需查看实时进程与 Git 状态，不假设之前消息代表当前运行状态。 |
| 当前交接 | 用户将到另一个 Agent 平台继续压电控制部分任务，要求把项目文档、交互记录、计划、执行情况和当前断点整理齐全。 | 本页及 `PIEZO_CONTROL_CONTINUATION_HANDOFF.md` 是持久交接资料；handoff 页面提供可复制提示和证据路径。 |

### 本轮交接时核实的事实

- 根 HEAD `9ea1d51545c52a0adc48c62bc1a65b68775c9ff3`，分支 `feature/structural-profile`；根仓与 model/solver 两子模块均有未提交工作。
- 冻结 PC-00～PC-11 12 阶段/36 条验收仍未完成；最新性能合同保留 20,001 记录、5s 首图、30s CSV、512,000,000 byte 内存门槛。
- v39 workflow 镜像在 baseline 第 27/47 项首次失败；前 26 项通过，`tst_workflow_extensions` 失败，candidate 0/47 未启动，失败后 221 项完整性检查通过。
- v33 轴向 20k Full 结果不合格：Wrong family，首图超时、峰值内存超限；完整 CSV 通过。详情及 SHA 在主 handoff 和审计目录。
- quota 状态文件时间戳为 2026-10-13 10:03:40 Asia/Singapore：8% remaining、0 张卡；监控 PID 22236 在本轮 10:05 检查仍存活。此数据会随时间变化，接手后刷新。
- 所有子 Agent 已结束；没有在运行中的子 Agent。交接不代表源代码或 candidate 已整合。
- C2C 上一次记录选了 project mode 和临时 Quick Tunnel，但 ChatGPT 连接/pairing 未完成，旧 pairing code 已过期。此次 CLI `session -w range-fea` 因路径重复拼接失败，未改动配置；该错误不改变用户选择的“无域名、临时地址”。

## 冻结范围与物理边界记录

- 原完整合同见 [`PIEZO_CONTROL_DELIVERY.md`](PIEZO_CONTROL_DELIVERY.md)，机器计划见 [`piezo_control_plan.json`](piezo_control_plan.json)，性能门槛见 [`evidence/piezo_delivery_performance_contract.json`](evidence/piezo_delivery_performance_contract.json)。12 阶段/36 验收范围 SHA：`d1d5fd76a14fba2b68e4b06fbeb2428da796e98c512e0b8b7940fb1545207dda`。
- 支持族为三维轴向压电杆 `axialRod`、平面轴向—弯曲分层梁 `planarBeam`、分层平板 `flatPlate`；未知/混合的未支持族必须拒绝，不能把 kernel family `axial/layered` 直接当成模型族。
- 用户首要使用体验是功能完整且操作简单：真实模型选择、成组配置、一次集中预览/批准；未变更保存实验尽量只需“运行＋确认”。
- 不通过改写旧资格记录、重命名源身份、伪造运行历史或复用旧安装来制造通过。
- COMSOL + MATLAB 与 COMSOL + Simulink 分别保留比较项；无基准算例时记录未测，不声明优势。依赖 provenance 与发行许可缺口见 `PIEZO_DEPENDENCY_PROVENANCE.md`。

## 与项目其他对话文档的区分

[`CONVERSATIONS.md`](CONVERSATIONS.md) 记录 Range FEA 产品本身的对话组件、分支、历史和存储行为设计，不是本项目开发聊天的逐字记录。项目连续开发状态更新到根 [`HANDOFF.md`](../HANDOFF.md) 与 [`DEVELOPMENT_LOG.md`](../DEVELOPMENT_LOG.md)；本文件记录上述用户决策和接管互动。
