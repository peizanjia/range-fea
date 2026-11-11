# 跨设备接管研读笔记：项目理解与续作依据

日期：2026-11-03。研读对象：根 `e3d5277`、`feature/structural-profile` 及其七个固定子模块。

## 1. 本笔记的用途与证据边界

这是新平台查阅计划、工程日志、交互决策、技术契约、验收摘要并抽查源码后的接管理解，不是另一套任务台账，也不是新的产品验收。它补充 [项目复盘](PROJECT_RETROSPECTIVE_20261103.md)，不改写该文件或历史记录。

- 当前可携状态权威仍是 [2026-10-30 阶段检查点](../../2026-10-30-source-handoff/acceptance/phase-checkpoint-20261030.md) 与 [最终迁移交接](../../2026-10-30-source-handoff/acceptance/deletion-handoff-20261030.md)。
- 工作范围分别以课程、WF、CW、PC 原计划及 JSON 为准。旧文中的“当前”“最新”必须结合所在段落日期阅读。
- 本次实际执行：Git/子模块状态核对、三套计划只读校验、CW 换行哈希分析、关键服务/作业持久化/CMake/CI 源码抽查。
- 本次没有编译、执行 CTest、启动应用、调用 Provider、安装依赖或重演历史数值测试。Windows 通过记录不转化为 Linux 通过。
- 完整旧聊天和早期原始需求没有随源码迁移。`DEVELOPMENT_LOG.md`、`CONTINUATION_LOG.md`、`PIEZO_CONTROL_INTERACTION_LOG.md` 是维护过的摘要；`CONVERSATIONS.md` 是产品对话功能设计，不是开发聊天逐字稿。
- `.planning/`、`audit/`、`artifacts/`、旧 `build/` 中的收据、安装候选、报告/答辩草稿和部分比较工具未随克隆交付。不能因文档有链接，就声称本机持有或已复核其原件。9 月初至 9 月 16 日的原始交互仍需旧设备资料补齐。

## 2. 项目到底要做什么

### 长期产品目标

在现有 Range FEA 多物理桌面平台上，建设结构有限元分析、结构动力学/控制研究、GUI/CLI/SDK 共用应用服务，以及受监督 Agent 和受控扩展能力。不是从零实现整个 CAE 平台，也不是删除热、流体、声学、电磁等旧功能。

用户长期强调的不只是“能算”：还要同一真实模型的建模、传感/作动、控制、运行、结果、诊断和恢复贯通，尽量减少重复配置与操作。已保存且未改变的实验，目标是“运行＋确认”；软件承担内部 ID、映射、文件和哈希管理。

### 最近的课程主线

当前课程目标比长期产品窄：杆/梁力学问题描述 → 确定性诊断 → 实际 FE 求解 → 独立结果验证 → 保持物理条件的有界数值修复 → 自动收敛 → Agent 调度 → 安装版报告交付。

核心贡献方向是“物理条件保持的有界修复、误差驱动计算、可追溯结果验收”，不是让语言模型替代数值求解器。三维压电与外部控制是已有平台能力的进阶展示，不是课程同时扩建的必需主线。

来源：[长期路线](../../../development/PROJECT_ROADMAP.md)、[课程计划](../../2026-10-21-trusted-computation/plans/TRUSTED_COMPUTATION_AGENT_PLAN.md)、[CW 合同](../../2026-10-11-control-workbench/plans/CONTROL_WORKBENCH_DELIVERY.md)、[PC 合同](../../../PIEZO_CONTROL_DELIVERY.md)。

## 3. 五套计划不能合并

| 计划 | 管理内容 | 最新可携状态/本机台账核对 |
| --- | --- | --- |
| 早期产品 Stage 0–13 | 数值、控制、GUI、Agent、扩展和长期质量路线 | 多轮有范围交付；不是目前课程的阶段表 |
| Trusted Agent Stage 0–8 | 可信杆梁课程闭环 | 0–3 有限 Windows/Release 范围接受；4–5 有限本地修复/收敛范围接受；6、8 未接受；7 比较豁免且未接受 |
| WF-01–WF-14 | 原生文档、作业、结果、恢复、对话、计划、扩展、网格与发布 | 10 done；WF-10/11/12 blocked；WF-13 planned；共 119 条验收 |
| CW-01–CW-16 | 主动杆、梁板贴片、索长/锚点及统一工作台 | 3 done、5 in_progress、8 planned；共 48 条；本机证据字节校验有换行问题 |
| PC-00–PC-11 | 完整压电材料/电极/传感、FE/ROM、算法、激励、GUI/安装交付 | PC-00 done，其余 11 planned；3/36 条 pass、33 pending；productComplete=false |

CW done 为 CW-01、CW-02、CW-07。WF done 为 WF-01–09 与 WF-14。不能将这些 done 外推为任意模型/全部产品完成，也不能因 CW/PC 尚未接受就说相关数值代码没有实现。

旧压电文字的“33/36”与目前 JSON 不一致；本次再次核对仍未发现逐条可携证据升级能够消除矛盾。继续使用最新台账，不用 91.7% 表示整个产品完成率。

## 4. 我对实施过程的理解

1. **9 月中下旬：先补数值与可信基线。**从原平台可编译/可计算起，补能力注册、混合 DOF、梁与面/体单元、约束、二阶结构动力学。修过真实的缩尺、质量/坐标变换、热应力/反力和未收敛结果发布问题，不是只增设界面。
2. **9 月下旬：动力学接控制与耦合。**接受步控制、完整状态恢复、拉索/压电、受限热结构、工作点/降阶和独立参考逐步形成。MATLAB stdin 失败后换 TCP 桥，不能继续把旧失败概括为 MATLAB 完全不可用。
3. **10 月初：从计算内核走向产品工作流。**共用应用服务、冻结求解快照、文档身份/Undo、Agent/Host 预览批准、私有 Python、受信扩展和实时曲线形成受控 v1；后来用 WF 计划追踪剩余通用缺口。
4. **10 月 11–20 日：结构控制/压电整合暴露交付瓶颈。**核心、科学参考与恢复测试很多；真实 GUI 长跑、首图/CSV、作业登记、完整同源安装和来源材料未闭合。大量静态夹具修复不等于产品功能新增或最终接受。
5. **10 月 20–30 日：聚焦可信计算 Agent。**增加 mechanics/verification/experiment/convergence 服务，强化身份、预算和 Host 边界，逐项重验杆梁范围；真实 Provider 和安装版全流程仍有失败。最后发布源码/子模块与可携摘要，完成源码恢复交接，而非产品终验。

详见完整阅读的 [开发日志](../../../history/logs/DEVELOPMENT_LOG.md)、[历史交接](../../../history/logs/HANDOFF.md)、[续作决策](../../../history/logs/CONTINUATION_LOG.md)、[压电交互摘要](../../../history/logs/PIEZO_CONTROL_INTERACTION_LOG.md)。历史退出、配额、PID、旧平台模型调度及临时网络地址都是当时记录，不自动授权本机新操作。

## 5. 架构与后续定位地图

| 层 | 主要位置 | 责任 |
| --- | --- | --- |
| 模型 | `src/range-model-lib/`，独立 Git 子模块 | RModel、单元/材料/DOF、稳定物理 ID、分析与控制配置、原生格式/恢复状态 |
| 求解内核 | `src/range-solver-lib/`，独立子模块 | 组装、静力/模态、二阶积分、杆索/分层压电、接受步与回滚 |
| CLI 与原生进程工具 | `src/fea-solver/`、`src/range-application-lib/tools/` | 实际求解、原生模型/结果查询、项目与网格等受限工具 |
| 共用应用服务 | `src/range-application-lib/`，属于根仓 | 命令/会话、项目目录、原生结果 broker、可信力学/验证/实验/收敛、压电桥 |
| GUI/Host | `src/fea/src/` | Session、ModelCommandHost、SolverManager/Task、工作台、Agent、结果与恢复界面 |
| Python 工作流与设计 | `src/range-workflow/python/range_workflow/` | 工作区作业、SDK、任务调度/恢复、科学扩展、压电设计/控制和时间激励 |
| Provider/公共设施 | `range-ai-lib`、`range-base-lib`、`range-cloud-lib`、`range-gui-lib` 子模块 | 供应商协议、基础数据/日志/哈希、网络与公共 Qt 设施 |

课程服务在本次源码中实际存在：

- `RTrustedMechanicsService`：严格 ProblemSpec/SI、独立杆梁候选、实际读回、诊断；诊断不修改模型。
- `RTrustedVerificationService`：通过原生 broker 读取哈希绑定结果，不接受调用者自填的数值证据；指标区分 pass/fail/notComputed/notApplicable。
- `RTrustedExperimentService`：一次授权、源身份、持久预算/状态机、取消/修复；它本身不调度求解，由 Host 执行。
- `RTrustedConvergenceService`：最多四级、固定 1% 相邻变化规则、连续两次合格才收敛；不是连续两次“求解退出 0”就成功。

关键约束：GUI、SDK、Agent 应复用相同业务规则；Agent 不进 FE 内迭代改物理。project/document/source/job/artifact、Session ID/Host epoch、内容哈希与显示名称各有不同职责，不能用路径、列表行号、旧批准或模型文字代替。

## 6. 已有能力与不能外推的边界

- 原有多类结构单元、混合平移/转角 DOF、静力约束和真正二阶动力学已有历史独立参考。旧一阶瞬态仍为兼容路径，不能当新结构振动入口。
- 原生 FE 是 plant；Python NumPy/SciPy 负责已限定的模态、阻尼、矩阵指数、Riccati/QP 与控制设计，不是独立小模型冒充 FE。
- 接受态、试算态、requested/conditioned/applied 命令必须分开；物理/控制/观测器/QP 状态和功账按相应交易一起接受，失败不可提前消耗状态。
- 最近节点 Tie 不是非匹配面插值；有限凸 TRI 壳到 TET 的网格流程不是通用 CAD；重新划网格不能猜测继承旧载荷/控制/ROM。
- 平面分层梁和分层平板不等于任意曲壳、扭转框架或通用三维压电实体资格。当前源码出现更宽能力也不能自动扩张 PC/课程合同。
- 全阶/ROM 同载荷回放与各自闭环是两种验证；保留全部模态达标不证明降阶收益，也不能用指定频点推广全频带。
- C++/Python/MATLAB 软件控制路径不证明板端固件、实物通信或硬实时。独立进程/Job Object/AST 检查不等于 OS 沙箱；WF-12 未接受时普通生成代码执行仍关闭。

## 7. 目前真正的阻塞

### 7.1 公共作业—结果链

[阶段检查点](../../2026-10-30-source-handoff/acceptance/phase-checkpoint-20261030.md) 记录两个仍失败的模型操作故事：

- 失败诊断包打开绑定输入快照时 Win32 error 3；事后文件存在，短暂路径/文件竞态仍未解释。
- `.job-record.lock`/持久登记失败，磁盘仍 queued，结果查询 `JOB_REGISTRY_CONFLICT`。

本次抽查确认 `src/fea/src/solver_manager.cpp:3329–3370` 的 `updateNativeJob()` 在 `persistNativeJobRecord()` 失败后仍执行 `it.value() = record`；它返回失败且不发成功更新信号，但内存候选与持久事实可能分离。相关另一个调用点还显式处理该行为，因此修复不能只删一行而不检查取消/恢复/租约调用方。这里是源码事实，不是已经在 Linux 重现锁失败，更不是锁根因已查明。

这解释了为什么“20,000 FE 步算完”并不足以交付结果图、CSV、可信报告。应先用小规模确定性失败/恢复故事修公共链，不默认重复数小时长跑。

### 7.2 回归与运行时

最后托管 Windows 记录：构建和注册清单通过，CTest **239/246，七失败**：

`tst_agent_task_runner`、`tst_agent_conversation_history_widget`、`tst_runtime_control_widget`（timeout）、`tst_project_session`、`tst_workflow_scientific_runtime`、`tst_workflow_scientific_input_inspection`、`tst_workflow_telemetry`。

较早本地也为 239/246，但失败集合不同；最终定向通过不补成同一轮全绿。本次只核对版本化摘要，未下载/重查托管原始日志。私有 Python 未登记 `.pyc` 的完整性问题仍须用干净受控运行时验证，不通过刷新清单接受污染。

### 7.3 课程 Stage 6/8

- Case B attempt006：真实 DeepSeek Pro inspect/diagnose 返回 Request failed，没有合格报告。
- Case C attempt007：仅补丁 INSTALL-07 候选上身份链和八项指标接受，不是完整当前源码安装资格。
- 仍需同一完整来源/构建/安装身份下的杆、梁、数值修复、需补问案例及模型→诊断→求解→验证→报告导出。
- 正式比较/独立留出和干净 Windows 账号首次使用获课程豁免，含义是未执行、未接受，不能产生效果结论；不豁免上述工程故障或全部产品发布门槛。

### 7.4 WF/CW/PC 产品线

WF-10 长序列性能、WF-11 真实科学功能生成、WF-12 LPAC 身份读回及隔离验收仍 blocked；WF-13 是后续发布汇合，不是已经完成的商业产品。

CW 仍缺完整环形结构/工作点模态闭环、非匹配面、完整标准绑定/v3/GUI 等原合同资格；PC 有大量实际实现与科学切片，但必须逐条在最终同源条件下重核，不能简单按台账 planned 当作全无实现，或按旧 33/36 当作基本交付。

### 7.5 来源/贡献与分发

`range-ai-lib` 根未见独立 LICENSE，需确认适用授权；Qt/TetGen、私有 Python、OpenSSL、运行库与实际发布包的通知/来源映射仍未最终闭合。历史 LLVM `debug_nonredist` 问题后来有 vcomp 路线修复，不能无视新记录再概括成当前始终未改；也不能据局部 PE/map/通知盘点就宣称完整合规。个人贡献、AI 协助和课程表述需要用户确认，Git 作者名不等于原创性证据。

## 8. 本机新增核实结果

### 源码恢复

根 HEAD 为 `e3d5277`；七子模块与记录的 gitlinks 一致且工作树干净：ai `66c79da`、base `25cca75`、build-tools `ec4cdb2`、cloud `d34f2f5`、gui `047c060`、model `6a732d9`、solver `880c5d1`。

启动时根仅有未跟踪 `.idea/` 和原复盘文件；本次保留不动。未自动合并上游、更新子模块远端、提交或推送。

### 三套只读计划校验

从仓库根执行，均使用 `python3 -I -B`，不改台账：

| 工具 | 本次结果 |
| --- | --- |
| `scripts/ci/verify_workflow_plan.py --next --json` | ok=true，14 任务/119 条，10 done/3 blocked/1 planned |
| `scripts/ci/verify_piezo_control_plan.py` | valid=true，12 阶段/36 条，productComplete=false |
| `scripts/ci/verify_control_workbench_plan.py --json` | valid=false，17 条 evidence changed 引用，涉及四份 JSON |

四份为 `CW-01-tracking-report.json`、`CW-foundation-20261011.json`、`CW-dynamics-core-20261011.json`、`CW-native-mixed-20261011.json`。仅在内存将 LF 规范为 CRLF 后，四份均与各自台账 SHA 完全匹配。没有修改证据文件、重盖哈希或撤销历史接受。后续应明确“冻结原始字节”与“语义规范化”策略及迁移验证，不能盲目刷新摘要。

### Linux 就绪程度

本机已有 `build/release/` 与 `build/dependency-check/` 的 CMakeCache、Makefile 等配置产物；两者 `RANGE_BUILD_TESTS=OFF`、`RANGE_REQUIRE_NUMERIC_TESTS=OFF`、私有 Python 为空，未发现生成的顶层 CTestTestfile 或 `fea`/`fea-solver` 可执行文件。因此“已存在 build 目录”不代表编译/测试通过。

本机 Qt Core 配置为 6.10.2，源码顶层最低 Qt 为 6.10；README 的 6.8 提示已旧。历史 Windows 使用 6.11.1。是否允许 6.10.2 作为新的验证基线、其他模块/编译/运行是否完整仍待实际验证，本次没有下结论为必然缺依赖或必然可构建。

当前必跑 manifest 是 **9 组/246 项**；`CI_VALIDATION.md` 中较早的 91+3 数量、LLVM OpenMP 指引不是当前运行配方。完整清单要以实际配置与 manifest 校验核对；Windows 专属安装/LPAC/PE 检查不能原样当 Linux 验收。

## 9. 后续开展工作的建议顺序（尚未执行）

1. **建立本机可复现验证基线。**固定 Qt/编译器/Python/数值依赖和平台支持矩阵；新建启用测试的配置，核对必跑注册，不沿用测试关闭的目录冒充验收。处理 CW 证据换行兼容但保留原始收据身份。
2. **优先修公共作业与诊断链。**以小模型定位持久化、锁、内存/磁盘状态、快照生命周期和失败包；补失败不误成功、租约保留/释放、重开核对的反例，逐项定位七项托管失败。
3. **完成课程 Stage 6/8。**先冻结完整当前源码安装候选，再进行新的明确 Host Apply 和真实 Provider 故事；逐个绑定 request/task/experiment/authorization/job/artifact/manifest/source，导出可重查报告。
4. **继续所选产品增量。**按原 PC/WF/CW 合同区分已实现未接受、确实尚未实现和环境阻塞；独立推进，但不默认扩张当前课程范围。性能实测与重负载回归分开，减少无证据长跑。
5. **最终交付审查。**在精确包身份上做完整回归、GUI 操作、依赖通知、来源/贡献/AI 表述与发布审核；比较研究待案例资源到位后另按正式合同开展。

这些是接管后的排序建议，不是新的执行授权、开发完成承诺或验收条件变更。本轮止于理解和记录。

## 10. 后续不可丢失的原则

- 不删除非结构旧能力、不退回已有混合工作、不为通过而换模型/改物理/放宽误差或删除失败条款。
- 材料、边界、载荷、物理假设缺项需要补问；自主修复只在本次授权的数值范围内。
- 新 Host 预览使用新身份并明确批准，不继承旧设备批准、历史摘要或 Agent 自称的授权。
- 课程杆梁范围：静力位移 1%、指定模态频率 2%；修复预算最多 8 solve/3 repair/12 model turn；收敛最多四级、连续两次变化低于 1% 且验证通过。
- PC 三家族每族 20,000 接受步加初始记录，首图 5 秒、完整 20,001 行 CSV 30 秒；最新 PC 内存标准是整机物理 RAM 严格低于 90%，不是旧 512,000,000-byte 门槛。该修订不自动重写 WF/CW 独立合同。
- 求解完成、退出 0、注册成功、结果验证、GUI 可用和安装交付是不同门槛。
- 所有项目产物留在 checkout：编译用 `build/`，profile/temp/log/截图/安装候选用 `artifacts/<run-id>/`；运行前核实路径隔离。同一构建目录不并发修改/构建/部署/测试。
- 子模块是独立 Git 仓库；未来经授权提交时先子模块、再根 gitlinks。不隐式合并 upstream 或使用 `submodule update --remote` 恢复检查点。
