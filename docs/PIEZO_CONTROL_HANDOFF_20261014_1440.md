# PIEZO_CONTROL_DELIVERY 接管快照（2026-10-14 14:40，Asia/Singapore）

本页是给下一 Agent 平台的**当前断点**。长期背景、历史失败、用户决策和证据索引分别见 [续接总册](PIEZO_CONTROL_CONTINUATION_HANDOFF.md)、[交互记录](PIEZO_CONTROL_INTERACTION_LOG.md)、根目录 [HANDOFF](../HANDOFF.md) 与 [DEVELOPMENT_LOG](../DEVELOPMENT_LOG.md)。以本页时间戳为当前状态；历史收据不可拼接成同源资格。用户本轮要求整理交接后退出，故正在运行的本聊天任务已停止。

## 接管事实和边界

- 工作区 `D:\YOGA\Documents\Projects\range-fea`，分支 `feature/structural-profile`，根 HEAD `9ea1d51545c52a0adc48c62bc1a65b68775c9ff3`。根仓及 model/solver 子模块存在大量未提交改动、未跟踪脚本和证据；接手先重新检查 `git status`、子模块与进程。不要 reset、clean、stash 或覆盖审计目录，也不要把未审文件一把提交。
- [冻结合同](PIEZO_CONTROL_DELIVERY.md) SHA-256 `5de3d06bca0f049cdd6231b2b71c60c126ca852657ff617eb2662653bcd4b61c`；[机器计划](piezo_control_plan.json) 为 12 阶段、36 条验收，**0/36 accepted，36/36 pending，`productComplete=false`**。这是正式交付完成度 **0%**，不代表源码工作量为零。没有可信的统一“开发完成百分比”：不同功能工作量及风险不等权，不能从代码行数或局部测试推算。
- 用户要求最终功能完整、GUI 视觉和各模块操作逻辑合理，保留选模型/器件→集中预览→一次确认；未变更的已保存实验尽量只需运行→确认。Luna 优先做有边界任务；困难问题和独立 reviewer 用 Sol medium；root 负责整合与最终验收。**不使用 Codex with ChatGPT**。用户使用临时地址、没有已配置域名。
- 三家族 `axialRod`、`planarBeam`、`flatPlate` 各须在同源私有安装的真实 GUI 完成 20,000 accepted FE 步加初始记录共 20,001 条；首图实际 canvas paint ≤5s；20,001 行完整 CSV 原子提交 ≤30s；GUI 与计算子进程树工作集同时峰值 ≤512,000,000 bytes，50ms 采样。该产品性能资格目前 **0/3 家族**。旧 v33 杆例因 family 错配、首图 8.676s、峰值 517,238,784 bytes 不合格。
- 重置卡只在新鲜观测剩余 `≤1%` 且有卡时使用；没有当前额度读数或重置工具。旧 8%/0 卡是过期快照，不得使用。没有产品 GO，不启动 Save/Run/Confirm。尚未做公开发布或推送。

## 本次退出时的可核实执行状态

| 工作线 | 已有证据 | 尚欠与结论 |
|---|---|---|
| PC-10 同一旧科学 EXE 的 FE/持久化矩阵 | **29/30 行合格（96.7%，仅此局部矩阵）**：轴向科学 8、梁科学 8、板科学 7、三族 RAM/RBM 6。新增梁 `planarBeam-MPC-forced` 和板 `flatPlate-MPC-free` 的 [summary](../build/audit-20261014/piezo-delivery/pc10-v43-smoke/flatPlate-batch/summary.jsonl) 为 `exitCode=0, qualified=true`；梁 [summary](../build/audit-20261014/piezo-delivery/pc10-v43-smoke/planarBeam-batch/summary.jsonl) 亦如此。科学 EXE 原 pin `B57E381A57E66C0807B2C91ABD7AC34A6679C09715893CC3CE6BB76FF05EB62B`。 | 板 `flatPlate-MPC-forced` 在 Qt `initTestCase` 后被本轮退出中断，**没有资格收据，不计数**；须在独立输出目录重跑。最终源码/二进制冻结后仍须核对并重做要求的同源资格。29/30 不是整个 PC-10 阶段或产品交付完成度。 |
| OFF 构建/安装 | v44 诊断 `ALL_BUILD` exit0；v44 新 `fea-link.map` SHA `96F00F796AD5EC7B3FBAF0088535C48D60CEA42B45160998621ECB24BD40DECA`，旧 Cloud AI 命名模式零命中；OFF workbench 配好私有 Python 后 1/1 PASS（314.41s），其后小修的定向 Qt 3/0 PASS。 | v45 隔离 `ALL_BUILD` 于编译 GUI 时因交接退出被 Ctrl+C 中断，退出码 1 **不是编译失败结论**；见 [诊断日志](../build/audit-20261014/piezo-delivery/build-v45-off-diagnostic-all.log)。源码脚本仍在改动，尚无最终同源 Release build receipt、fresh OFF install/schema-v2 inventory、正式独立 map gate、全量 CTest。旧 OFF 安装 map 的 430 个 Cloud AI 命中仍是失败历史，不能发行。 |
| v43 静态准备链 | 源预检/冻结/包树/模块边界/解析器等局部夹具已有通过证据；聚合器仅提升 **7/19** 固定 ID（36.8%，只代表此前的局部静态准备），其余 12 `PENDING`。root-bootstrap 31 个合成情境、Save 纯判据 24、Save 环境预检 10、取样器台账 13、PATH-shadow 5 路有聚焦通过证据。 | **实际候选绑定 0/19 PASS，静态 GO 不存在。** [v43 工具目录](../scripts/packaging/piezo_gui_static_v43/) 新增脚本仍需完整候选、冻结、包树与同源重跑。Save 环境预检刚修 installed-file SHA 的 A/B 句柄窗口，PS5.1 夹具 10 PASS，但修后尚未独立 Sol 复审。root-bootstrap native mutated-handle 分支未实测。 |
| 候选控制/根封条/最终化 | `test-candidate-control-contract-v43.ps1` 的局部夹具通过；`test-finalize-integrity-candidate-v43.ps1` 在无真实候选时只做真实 finalizer 负例，保持 PENDING。root-seal fixture 已修来源字段、路径分隔符与同字节读哈希；当前明确报告 PENDING。 | Sol medium 指出的候选控制两项 P2 仍未修：Save receipt 的 `sourceSha256` 必须恰好绑定两个生产脚本，且候选 OFF/schema-v2/PASS 与冻结的本仓 root、v43/OFF/Release/staticOnly/no-GO 字段必须严格核验。root-seal 尚缺合成完整候选正例、篡改负例、真实 finalizer no-GO 运行收据及聚合器接线；finalize_integrity 候选正例/聚合接线也未完成。不得把源码文本扫描当作运行时封条证明。 |
| GUI | 已有 11 个顶层模块和压电 6 页源码、集中预览/确认流程；单模型来源暂不可读时的 Choose model 恢复入口定向 Qt 3 PASS。 | 私有安装窗口的 11+6 页面视觉/响应式/键盘/UIA、真实 Save/Run/Confirm/Undo/恢复/篡改故事均未验收。聚焦 Qt 不能证明可用性或性能。 |

本聊天启动的梁长跑已经正常完成；v45 诊断构建和板最后一行在交接时通过 Ctrl+C 停止。退出后进程查询未发现匹配 `v45`/`pc10` 的 CMake、测试或 Python 子进程；另一平台仍应重新核实。板最后一行留下的 `flatPlate-MPC-forced.qt.txt` 只有 `initTestCase` PASS，不得用它当科学合格证据。

## 下一平台优先顺序

1. 核对工作树/子模块/运行进程与 [冻结计划](piezo_control_plan.json)，检查上表原始 JSON 和 SHA。保持本聊天停止，避免共享工作树并发编辑。不要尝试旧临时 C2C 地址。
2. 先收敛静态源：修候选控制两项 P2；为 root-seal 与 finalizer 补完整候选正反例及实际运行证据，接入 19-ID 聚合；对刚修改的 Save 环境预检和 finalizer gate 做独立 Sol medium 复审。测试仍为 no-GO 时按 PENDING 记，任何失败留原收据。
3. 在脚本和源码稳定后，以新的独立审计根做 `source-preflight-v43.ps1`、正式 v43 Release build receipt、source freeze、package tree、静态全链。v45 构建可增量续跑用于诊断，最终必须重建并核对同一源码快照。做 fresh OFF 安装/schema-v2 清单/独立 map，按当前 manifest 执行要求的 CTest；旧绿项不能拼接。
4. 在独立 Sol medium 最终复审与 root 技术 GO 之后，才跑真实安装 GUI 的 11+6 页面故事及三族 20k/首图/CSV/内存。板科学最后一行另建输出根重跑，并在最终同源冻结后复核全部科学矩阵。原始失败不能覆盖。
5. 将 36 条正式证据逐项写回计划；只有全部验收、兼容/来源/许可边界和可运行软件完成，才可将 `productComplete` 改为 true。

复制给接手 Agent：**先读本页及链接原始证据，按当前仓库状态复核；不得把 29/30 局部科学矩阵、7/19 静态夹具或诊断编译换算为整机通过。**
