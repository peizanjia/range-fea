# WF-05 恢复契约准备

2026-10-09。本文固定下一步接口与验收设计，**不是已实现能力**。WF-05 依赖 WF-02/WF-04；待 WF-04 验收后启动共享文件实施。机器状态仍以 workflow_delivery_plan.json 为准。

## 状态和副作用

- 操作采用 projectId + idempotencyKey + normalizedRequestSha256 识别，operationId 关联意图、候选、批准摘要、提交、工件和观察。相同 key 不同规范请求拒绝；相同已接受请求返回原观察结果，不重新产生副作用。
- 账本位于受管理 operations/，每条记录版本化、原子发布、路径/大小/类型/SHA 验证。阶段区分 intent、accepted、artifact_published、observed、unknown、conflict。查到意图不代表副作用发生，发现文件不代表模型已接受。
- 原始批准令牌/密钥不持久化。批准只在当前实例内有效；重开不能复用旧批准启动操作。历史已接受操作可只读查询。
- GUI 保存先登记意图，再写各 native 文件，最后观察并更新目录。MODEL/LINK/目录不是一个跨文件原子事务；部分成功逐项核对，外部改写报告冲突，不覆盖用户新内容。关闭补记失败保留可重试的持久队列。
- 未保存 GUI 修改的保护范围明确显示：已持久接受快照可恢复；仍 pending 或只在内存中的内容不声称受保护。传统未纳入通知入口保留未知状态。

## 周期接受检查点

- 独立于显示 outputFrequency，最晚每 10 个接受步或 1 s 仿真时间（先到者）持久化。检查点落盘落后超限时等待或失败；不继续接受更多步后假称间隔达标。
- 仅在完整接受事务后捕获 MODEL 状态，包括 q/v/a、积分器、控制器、支承/功账、参数版本及 acceptedSeq/time。失败 trial、外层组合事务未接受状态不得发布。
- 使用不可变 checkpoint MODEL + range.native-checkpoint/v1 envelope，包含模型/控制/runtime/source/package 哈希、策略、acceptedSeq/time、MODEL SHA。验证完成后原子更新 latest 指针，不覆盖唯一有效旧检查点。
- checkpoint 完整性校验与软件来源校验缺一不可。坏/截断/过期/不同网格、控制、dt、参数或环境不能恢复；不凭 progress/time 字样推断资格。

## 作业与 both 模式

- 每个逻辑 job 有独立 attempt，保留原尝试日志。active/passive 分别记录 pending、prepared、running、checkpointed、completed，completed 必须有已验证的实际 manifest。
- active 已完成而 passive 未开始或中断时，不把整体标成功。恢复只复用已验证部分，原 job/source/runtime 和每幀历史 proof 保留，不能冒充新 attempt 产生。
- 新恢复 attempt 固定原输入及获准目标步数，只继续未完成范围。原生 restart 的 nSteps 是追加数，恢复所需追加数必须由目标总步数减去完整接受检查点求得。
- 逻辑轨迹按来源/接受步去重，保留 replay 标记与实测可丢失范围；两条轨迹及原独立比较校核齐全才整体成功。

## 目标接口

以下名称须等代码/schema/capabilities 落地才可调用：

| 接口 | 目标 |
| --- | --- |
| operations.query | 有界查询持久意图、观察、冲突和原操作结果 |
| job.recovery.query | 按 opaque jobId 返回 recoverable/recompute_required/reconcile_required/conflict 与来源证明 |
| workflow.recover | 固定原逻辑作业、选择的 checkpoint SHA 和恢复范围，经新预览/批准创建 attempt |
| native.job.recovery.query | GUI 原生作业重建后的接受检查点与恢复资格 |

GUI/Agent/SDK 使用同一资格判断。恢复不自动执行历史提案，不把 interrupted 改名为 succeeded。恢复 UI 保持其他文档操作、只读查询和取消响应。

## 验收证据

覆盖 WF-05-A01–A10：意图/模型提交/工件发布/终态记账前后强制退出；both active 完成/passive 未开始与运行中的中断；真实程序重开；锁、外部改写、坏日志、坏检查点、磁盘失败；实际间隔和连续/中断恢复 q/v/a、控制器/功账比较。既有完成后 resume 或 output frame journal 不充当这些证据。

固定数值门槛沿用受支持模型分单位规则：位移/转角 1e-8 m/rad、速度/角速度 1e-7 m/s、rad/s；加速度、控制状态、功账另按现有独立基准冻结可检门槛，不从观察结果反推阈值。完整集成回归、独立审查、迁移/恢复手册与安装验证分别记录。
