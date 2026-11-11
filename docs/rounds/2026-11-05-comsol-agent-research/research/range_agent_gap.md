# Range FEA 全流程 Agent 当前能力与差距交接（2026-11-05）

核查仓库：`/home/erwin/Projects/range-fea`；HEAD `e3d527781add4cd388276392f63a7c9ee9bd5ec2`。已有未提交修改全部保留。仅静态阅读，未配置、编译、CTest、启动产品或调用Provider；本报告不颁发任何当前运行PASS。JSON附所引源码/文档SHA-256，行号对应这次读取字节。

## 证据身份与状态

11/05审计明确新增C++回归未编译运行；当前Linux科学环境也未达冻结验收资格。10/30历史有Windows构建、239/246及部分目标通过，并有阶段0–5限定范围验收；这些不覆盖之后的dirty源码。当前观察到实现不等于当前实测。

`docs/audit_20261104/RESUME_REVIEW_20261105.md:3-5` [current_audit_record]; `docs/audit_20261104/RESUME_REVIEW_20261105.md:42-49` [current_audit_record]; `docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:3-10` [unexecuted_validation_plan]; `docs/acceptance/phase-checkpoint-20261030.md:30-52` [historical_checkpoint]

状态：`implemented_but_untested`=当前实现可见，但未见与当前字节匹配运行结果；`unsupported`=指定Agent/服务接口不支持；`plan_only`=完整交付/验收仍待完成；`waived`=用户豁免且未执行。此次没有判为`implemented_current_tested`的数值/GUI闭环。

## 实际调用和证据流

`AgentTaskRunner::toolDefinitions`窄工具 → `document_propose`/`experiment_run` → `ModelCommandHost`预览与Host批准 → Core命令或受限experiment生命周期 → SolverManager真实作业 → hash/source-bound native broker → RTrustedVerificationService独立算子 → RTrustedExperimentService记录/数值修复 → RTrustedConvergenceService相邻QoI比较 → Agent只引用Host事实。另一路`range.workflow/v1`服务有自己的工作区、原生/参考来源、schema及批准，不能冒充任意GUI文档结果。

`src/fea/src/agent_task_runner.cpp:1080-1145` [current_source]; `src/fea/src/model_command_host.cpp:2871-2907` [current_source]; `src/range-application-lib/src/rtrusted_verification_service.cpp:227-264` [current_source]; `docs/WORKFLOW_SDK.md:7-13` [current_contract]

## 13项差距与推荐实现面

### G01 自然语言到完整物理规格 — `implemented_but_untested`

Agent已有ask_user、inspect、diagnose和完整ProblemSpec提案；物理规格限定axialRod/cantileverBeam。自然语言任意压电结构到完整可验证规格未交付。

**建议实现面：** 先固定课程案例输入清单、单位、缺参澄清和来源；为压电建立闭合规格及确定性校验，再接入Agent。

`src/fea/src/agent_task_runner.cpp:1096-1119` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:1232-1279` [current_source]

### G02 几何与材料建模 — `implemented_but_untested`

支持现有节点坐标、刚体变换、常量材料编辑；当前Agent命令枚举没有任意新建CAD/拓扑/几何布尔/压电本构张量建模入口。不能把限定编辑等同完整建模。

**建议实现面：** 课程首版使用有来源的固定几何或原生压电模板；补齐受限建模命令、物理ID及预览/撤销，不开放任意代码。

`src/fea/src/agent_task_runner.cpp:1080-1083` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:1272-1279` [current_source]; `src/range-application-lib/src/rmodel_command_service.cpp:913-933` [current_source]

### G03 网格生成与物理重绑定 — `implemented_but_untested`

Host已交付mesh.generate/accept/cancel，均需fresh manual approval；accept新建未绑定文档。mesh.rebind.static限定静力、单连通线性TRI1/TETRA1等条件；压电重绑定不属于此入口。

**建议实现面：** 明确课程网格路径；若采用此网格服务，补压电材料/电极/机械BC的显式重绑定和拒绝测试。

`src/fea/src/model_command_host.cpp:2871-2888` [current_source]; `src/fea/src/agent_task_runner.cpp:1867-1867` [current_source]; `src/range-application-lib/src/rmesh_rebind_service.cpp:244-292` [current_source]; `src/range-application-lib/src/rmesh_rebind_service.cpp:533-547` [current_source]

### G04 边界、载荷与初值 — `implemented_but_untested`

Agent支持entity.boundary.set；trusted lane仅pointForce，轴杆只验证ux且横向输入必须零。压电电势/电荷/电极控制不在该通用边界命令闭环内。

**建议实现面：** 为固定压电案例列清机械约束、外载、初始状态、电端口及单位；缺参先澄清，物理改变须新批准。

`src/fea/src/agent_task_runner.cpp:1080-1083` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:1460-1484` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:1521-1553` [current_source]

### G05 压电器件与控制配置的Agent接入 — `unsupported`

Core明确发布piezo.configure-experiment并调用RPiezoExperimentService，但Agent documentCommandSchema不含该命令，工具描述亦未暴露它；底层压电能力不能视为当前Agent可配置能力。

**建议实现面：** 为piezo.configure-experiment提供严格窄适配，复用whole-experiment预览、源身份、批准、撤销及拒绝语义；增加真实Host集成回归。

`src/range-application-lib/src/rmodel_command_service.cpp:920-923` [current_source]; `src/range-application-lib/src/rmodel_command_service.cpp:1130-1172` [current_source]; `src/fea/src/agent_task_runner.cpp:1080-1083` [current_source]; `src/fea/src/agent_task_runner.cpp:1120-1121` [current_source]

### G06 求解与作业可靠性 — `implemented_but_untested`

analysis.run/cancel/recover及真实job引用已实现。11/05持久化和checkpoint修补仍未编译运行；历史锁释放、Win32 error3和终态失败后的核对流程未关闭。

**建议实现面：** 先完成当前源码目标测试和原生CLI回归，再跑固定课程案例；保存源/安装/作业/工件/授权身份及失败原始日志。

`src/fea/src/model_command_host.cpp:2889-2907` [current_source]; `docs/audit_20261104/RESUME_REVIEW_20261105.md:26-29` [current_audit_record]; `docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:16-21` [unexecuted_validation_plan]; `docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:36-38` [open_issue_record]

### G07 错误诊断与故障归因 — `implemented_but_untested`

确定性诊断覆盖缺支承、单位、材料、退化单元、来源陈旧；不可判定的求解失败返回SOLVER_FAILURE_UNCLASSIFIED并保存诊断包/停止。不是任意物理或求解器故障自动根因系统。

**建议实现面：** 固定压电案例补故障分类及可复现负例；未分类时保留证据并停止，不通过乱改支承或材料修复。

`src/range-application-lib/src/rtrusted_mechanics_service.cpp:2040-2059` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:2089-2092` [current_source]; `src/range-application-lib/src/rtrusted_mechanics_service.cpp:2245-2310` [current_source]

### G08 独立残差与物理可信性验证 — `implemented_but_untested`

已实现独立静力K*u-f、模态K*phi-omega²*M*phi、瞬态M*a+C*v+K*u-f、运动学与能量检查；仅native-passive rod/beam，dense参考上限192总DOF。当前未实现通用压电机电耦合独立验证。

**建议实现面：** 为课程压电模型建立机械/电荷平衡、电端口功和机电能量账本及独立参考。现有残差是离散代数/时间一致性检查，不是Galerkin空间后验误差估计。

`src/range-application-lib/src/rtrusted_verification_service.cpp:247-264` [current_source]; `src/range-application-lib/src/rtrusted_verification_service.cpp:426-426` [current_source]; `src/range-application-lib/src/rtrusted_verification_service.cpp:467-467` [current_source]; `src/range-application-lib/src/rtrusted_verification_service.cpp:524-558` [current_source]; `src/range-application-lib/src/rtrusted_verification_service.cpp:728-732` [current_source]

### G09 保持物理的自动修复 — `implemented_but_untested`

授权数值字段、物理摘要digest及预算守门已实现。当前自动mesh proposal仅当required失败全部为位移/频率精度失败时增segments；静力残差失败不会直接触发此自动加密。

**建议实现面：** 先复验该边界，再为压电增加明确数值修复策略。保留阈值、材料、几何物理、BC/载荷和控制律；越权或物理变更进入needs_confirmation。

`src/range-application-lib/src/rtrusted_experiment_service.cpp:921-923` [current_source]; `src/range-application-lib/src/rtrusted_experiment_service.cpp:1783-1838` [current_source]; `src/range-application-lib/src/rtrusted_experiment_service.cpp:1884-1900` [current_source]

### G10 网格/时间步收敛 — `implemented_but_untested`

已实现uniform segments翻倍或dt减半且保持时长；固定1%相邻QoI变化、至少两次连续达标。适用位移、静力应变能、选定模态频率。应力/反力收敛、通用压电收敛、Richardson/GCI或解析真误差保证未由此实现。

**建议实现面：** 课程明确QoI和观察位置，保留所有层级/误差/成本/曲线；压电单独实现及验证电端量/控制性能QoI，避免把相邻变化称绝对误差。

`src/range-application-lib/src/rtrusted_convergence_service.cpp:387-416` [current_source]; `src/range-application-lib/src/rtrusted_convergence_service.cpp:480-528` [current_source]; `src/range-application-lib/src/rtrusted_convergence_service.cpp:563-624` [current_source]; `src/range-application-lib/src/rtrusted_convergence_service.cpp:699-770` [current_source]; `docs/acceptance/phase-checkpoint-20261030.md:49-49` [historical_checkpoint]

### G11 原生后处理与图表 — `implemented_but_untested`

Agent已有native catalog/channels/series/summary、open和display；压电编码通道语法已加入。Python runtime有plot/raw export schema，但Agent工具表未提供任意通用plot/export工具，不能把runtime可调用等同模型工具可用。

**建议实现面：** 固定课程后处理图和物理量单位；把已验证的曲线/场显示/CSV导出接口按需要窄适配给Agent，保持native/reference/caller来源区分。

`src/fea/src/agent_task_runner.cpp:103-106` [current_source]; `src/fea/src/agent_task_runner.cpp:1135-1143` [current_source]; `src/fea/src/model_command_host.cpp:3080-3085` [current_source]; `src/range-workflow/python/range_workflow/service.py:1530-1564` [current_source]

### G12 可提交报告与安装版全流程 — `plan_only`

Agent会报告Host验证事实，预算耗尽时追加Host证据快照；但完整当前安装版模型→求解→诊断→收敛→后处理→报告导出仍未完成验收。工具表不含专门的课程报告导出工具。

**建议实现面：** 实现一个固定案例可回查工件的报告包（输入、来源、推导、独立验证、收敛、图表、限制），然后由安装版跑完整Provider/Host/solver闭环并保存收据。

`src/fea/src/agent_task_runner.cpp:1858-1859` [current_source]; `src/fea/src/agent_task_runner.cpp:3924-3924` [current_source]; `docs/acceptance/phase-checkpoint-20261030.md:50-52` [historical_checkpoint]; `docs/audit_20261104/RESUME_REVIEW_20261105.md:46-49` [current_audit_record]

### G13 会话恢复与长期任务 — `implemented_but_untested`

会话持久化、只读核对、job recovery入口已有；恢复不复制批准。普通GUI native恢复主要限未启用外部控制的Newmark结构动力学；Workflow杆/梁active/passive为另一域。热trace旧前缀同尺寸改写边界仍开放。

**建议实现面：** 复验当前会话/任务/源身份及中断恢复；课程提供明确可恢复/需重算/待核对矩阵；压电恢复逐后端与控制器状态验证。

`docs/CONVERSATIONS.md:15-28` [current_contract]; `docs/RECOVERY_OPERATIONS.md:7-19` [current_contract]; `src/fea/src/agent_task_runner.cpp:1131-1134` [current_source]; `docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:36-37` [open_issue_record]

## 支持 / 拒绝矩阵

| 入口 | 支持范围 | 必须拒绝/不能推导 |
| --- | --- | --- |
| `trusted_problem_inspect/diagnose` | 完整SI杆/梁ProblemSpec、指定当前文档或detached候选 | 任意压电/3D多物理规格、猜测缺失物理 |
| `experiment_run` | 显式solves 1–8、repairs 0–3、modelTurns 1–12、精确授权、隔离候选 | Auto模式不自动授予精确experiment批准；不属于当前task的实验查询 |
| `document_propose` | schema枚举的几何/材料/BC/分析/网格/求解/恢复 | 未枚举命令、任意路径/代码/旧ID重映射；当前不含piezo.configure-experiment |
| `mesh.generate/accept/cancel` | 源UUID/revision绑定、fresh manual approval | Auto绕过；accept继承旧物理ID/未经重绑直接作为原模型 |
| trusted verifier | passive杆/梁静力/模态/瞬态；独立K/M/C和能量；192总DOF | arbitrary压电、active控制、超限矩阵；notComputed视为pass |
| 自动mesh repair | 仅位移/频率参考精度失败，授权segments范围内均匀加密 | 残差失败直接加密；静默改物理、放宽阈值、越预算 |
| convergence study | 固定1%连续两次相邻QoI变化、uniform mesh/dt；保时长 | 由相邻变化保证真误差；应力/反力/通用压电QoI；caller放宽阈值 |
| `native_result_query/display` | 登记job/artifact/channel，接受帧、终态验证及独立显示文档 | 任意文件路径；progress当物理解/成功；reference冒充native |
| recovery | 资格查询+新预览/批准；特定checkpoint后端 | 跨重启复用旧批准；无checkpoint模态/混合/控制路径保证可恢复 |

矩阵证据逐项见G01–G13；“拒绝”含明确源码守门和工具接口未暴露两种情况，不声称所有下层实现均不存在。

## 需要明确区分的历史豁免

10/30 checkpoint:51明确三臂180槽对照/独立holdout因没有案例集未执行；:52明确新Windows账户首次使用被豁免。二者不是本轮待修缺陷，不应私自重新设为课程阻塞项；也不代表Stage 7/8通过。Stage 6真实Provider及Stage 8当前安装版全流程仍开放。

## 文档冲突

Older manual lists only four Core commands and says registry memory-only/result channel unavailable; current source exposes geometry/mesh/recovery/result tools and audit records persistence changes. Runtime capabilities are the authority; do not downgrade implemented features based on stale prose.

`docs/AGENT_OPERATIONS.md:57-77` [stale_contract_text]; `src/fea/src/agent_task_runner.cpp:1080-1143` [current_source]

## 下一步验证命令（本次未执行）

先固定root/子模块/dirty源码哈希、构建身份与隔离profile，再按运行矩阵执行。以下是交接命令模板，不表示此次运行：

```text
host_capabilities {} -> range.host/v1 capabilities.query
workflow_capabilities {} -> range.workflow/v1 capabilities.query
ctest --test-dir <fresh-build> -R "^(tst_trusted_mechanics_service|tst_trusted_verification_service|tst_trusted_experiment_service|tst_trusted_convergence_service|tst_agent_task_runner)$" --output-on-failure
```

Use selectors and platform/identity/isolation requirements in docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:14-21; CTest test names are not necessarily executable names (tst_agent_task_runner is hosted by tst_gui_model_lifecycle --agent-runner, src/fea/CMakeLists.txt:774).

当前服务测试注册在`src/range-application-lib/CMakeLists.txt:234-262`；11/05新selectors及平台要求在`docs/audit_20261104/RUNTIME_VALIDATION_20261105.md:14-21`。完整安装模型→报告与真实Provider应在该批回归稳定后单独留原始收据，不能由mock网络、源码guard或过去的Windows通过继承。
