# 从 COMSOL 到 Range FEA：全流程可信计算智能体技术调研与开发交接

> 本文现作为 Range 综合附录。按用户后续要求，主报告为 [COMSOL 全流程 Agent 调研](COMSOL_FULL_WORKFLOW_AGENT_REPORT.md)。后续检索确认：2026-09-18 官方已展示现有 Java API 的外部 Agent 闭环；这与 6.4 内置 Chatbot 及 2027 MCP 是不同接入路线，不能用 Chatbot 的边界推断 COMSOL 全部 Agent 能力。已停止本轮构建和测试，之后仅整理报告。

研究日期：2026-11-05（Asia/Singapore）。项目：计算力学课程大作业。仓库：`/home/erwin/Projects/range-fea`，分支 `feature/structural-profile`，根 HEAD `e3d527781add4cd388276392f63a7c9ee9bd5ec2`，含大量保留的未提交修改和子模块修改。本报告区分官方事实、当前源码、历史运行记录、本次实测、设计建议；HEAD 相同不代表源文件相同。

主要读者：接续开发的 AI、课程指导教师及项目维护者。先读执行摘要、差距矩阵、实施顺序和 `developer_handoff.json`；逐项源码定位见 `range_agent_gap.md` / `.json`。官方逐条证据见 `research_agent.json`。压电运行结论须连同 `piezo_validation.md` 与工件收据读取。

## Executive Summary — 执行摘要

本项目最值得形成的成果，是**原生有限元压电闭环控制，加上能保留物理条件、检查数值可信性并交付证据的工作流智能体**。应把这两个方向连成一个可复现的课程任务，而不是仅以聊天窗口、代码量或功能清单作为成果。现有源码已经有作业管理、结构化命令、原生结果查询、独立验证、有限预算修复及收敛研究；缺口集中在压电能力接入 Agent、当前版本运行验证和最终模型到报告交付。[R01][R02]

用户提到的 COMSOL 能力存在重要版本边界。6.3 已引入 Chatbot 和 Java Shell；6.4 加强了模型上下文、文档检索及代码生成。6.4 官方流程仍明确要求用户将生成代码送入 Java Shell 并点击 Run，错误再由用户送回 Chatbot。其公开的 tool calling 说明指向文档浏览，不能直接推导为自主建模工具。[1][3][4][5][12][13] 2026-09-16 发布的 **2027 版本预告**才明确提出通过 COMSOL MCP Server，让外部智能体直接建模、运行、检查结果并决定下一步；预告本身不能证明正式上市日期及完整接口细节。[14]

因此，借鉴 COMSOL 应分两层：第一层是**共享模型 API、可序列化的模型上下文、操作录制和可执行示例、结构化结果读取**；第二层才是有预算、有回执、有恢复的代理循环。COMSOL 的闭源提示词、检索索引、planner 和 MCP 工具 schema 没有在本次来源中披露，报告不会把推测写成复刻方案。[2][5][10][11][14]

Range 当前最明确的连接缺口是：Core 已有 `piezo.configure-experiment`，Agent 工具枚举尚未暴露；可信验证服务只覆盖限定的被动杆/梁，不能因压电底层有检查就宣称 Agent 已完成压电独立验收。现有自动网格修复由位移/频率参考精度失败触发；代数残差与相邻网格 QoI 变化，均不等于严格有限元空间后验误差估计。[R01]

11 月 5 日的审核覆盖了 416 项中的 414 项，深度包含整项静态阅读、差异和修改片段审查，2 项仍为部分完成；同一审核明确没有编译运行其新增 C++ 回归。历史 Windows 通过记录只能证明当时限定范围，不能覆盖今天的修改。[R02][R03] 本次压电定向运行及修复状态另见后文；其通过也不能替代 GUI、真实 Provider 或安装版全流程验收。

## Introduction — 目标、标准与范围

“全流程”应定义为：从一个明确的工程问题出发，取得或创建模型，核对单位、材料、约束、电极及控制要求，形成可审阅修改，完成求解，读取真实结果，执行独立检查，根据证据进行授权范围内的数值修复或收敛研究，最后产生含模型、图表、参数、限制和复现步骤的报告包。无效输入、预算耗尽、取消和中断也属于流程的一部分。单次成功求解与全流程完成应分别验收。

课程范围建议首先闭合一个典型 PZT 多杆受迫振动控制问题，再以分层梁或平板作为扩展验证。保持原计划的三族支持矩阵：三维轴向杆、可在三维任意取向的平面分层梁、平面分层 Q4 板。不得把平面梁表述成任意三维框架，把平板表述成任意曲壳，把理想开路/短路读出表述成已验证的真实电子调理硬件。[R04]

这里的“高标准”可由三个可观察结果体现：同一原生 FE plant 的控制关闭/开启对比及 FE/ROM 差异；独立参考、网格/时间收敛和机电功量平衡；带故障与中断的 Agent 完整执行收据。这是针对本课程的工程研究路线建议，不是新算法优先权声明，也不是未经对照的“优于 COMSOL”结论。已有 COMSOL 官方压电算例足以说明压电建模本身早已有成熟工具，课程贡献需要落实在你的实现、验证与智能体闭环上。[17]

## Main Analysis — COMSOL 公开技术路线与 Range 对照

### 1. 版本和能力边界

| 版本或记录 | 可核实能力 | 不能由此推导的结论 |
|---|---|---|
| 6.3，2024-11-19 发布 | Chatbot 生成/解释/调试代码；Java Shell 交互操作模型。[12][13] | 首次出现于 6.4；已经自主执行工程任务 |
| 6.4，2025-11-18 发布 | 更丰富的模型附件、文档搜索、兼容供应商、代码录制及显示设置代码复制。[1][2][6] | 所有模型状态自动发送给 LLM；可自主改模型及验收结果 |
| 6.4 累计更新至 U3，6.4.0.429，2026-05-29 | U1 调整 API 连接，U2 更新模型选项映射，U3 增补可搜索的带注释 Java 模型文件。[7] | 基础 6.4 与全部在线文档具有完全相同的 Provider 行为 |
| 2027 预告，2026-09-16 | MCP Server 经已有 COMSOL API 对接外部 agent，预告建模、运行、检查结果和迭代。[14] | 预告即正式发布；已公开认证、权限、工具 schema、故障恢复保证 |

这一区别会改变实施重点。若对标 6.4 的公开助手体验，重点是上下文、文档和可执行 API 示例；若追求用户所说的全流程智能体，则应以 2027 预告描述的循环作为产品目标，再给 Range 加上明确的工程验收和权限边界。这两者共享基础设施，但完成难度不同。[2][4][14]

### 2. COMSOL 如何把软件状态交给模型

6.4 的参考手册描述了用户主动附加的模型信息：选中节点、节点及子树、模型历史，以及图形、文字和文件。节点和历史会用 Java 代码表示；历史还区分当前压缩状态和包含已废弃操作的完整记录。因此它的“model-aware”至少有一条明确实现路径：把可操作模型结构序列化为模型 API 语言，和用户问题一起交给外部 LLM；公开资料没有证明每次请求都自动包含全部活动模型。[3]

官方的 API 使用文档和可录制动作相互补充。用户可把一个实际 GUI 操作录成代码，交给模型扩展，再在真实模型上下文执行。6.4 还加入直接录制到 Java Shell 的入口。这减少模型凭空编造 API 的机会，也使 GUI 动作和代码动作在同一模型上相遇。对 Range 的启发是：应输出当前版本可接受的命令范例和能力表，而非把海量源码塞入提示词。[2][4][5]

Range 建议输出分层上下文：选中实体与物理 ID、当前模型摘要、单位与支持族、压电实验有效性、源 revision/digest、最近一次求解及验证状态。用户应能查看“本次发给模型的附件”。这是 Range 的设计建议，不能写成 COMSOL 内部自动上下文算法。过时文档、历史运行和未执行建议必须带来源标签，不能混进当前模型事实。

### 3. 文档检索和可运行范例

6.4 文档明确将 tool calling 用于浏览 COMSOL 文档；编程指南也承认聊天模型并不掌握全部 COMSOL API。[4] U3 进一步让带注释的模型 Java 文件成为可搜索材料。这表明官方公开路线不仅依赖一般模型知识，还通过文档和真实模型示例补充具体 API 用法。[7]

这可启发 Range 建立一个与构建身份绑定的小型知识包：当前支持矩阵、命令 schema、单位约定、成功示例、失败代码、数值验证说明、经过维护的课程案例。检索结果需带版本、来源和段落；旧手册说“不支持”而源码已实现时，应在文档中修正冲突。现有 `AGENT_OPERATIONS.md` 关于注册表和结果入口的部分文字已经落后源码，不能直接用作能力真值。[R01]

“加入 RAG”不是已证明必要的独立里程碑。先让模型拿到正确的小规模上下文与少量可运行范例，再依据真实错误决定是否引入索引、向量检索或重排。公开资料未披露 COMSOL 的 embedding、向量数据库、混合检索权重、内部提示词或 planner，报告不指定这些为其既成技术事实。

### 4. 为什么 API 层比聊天窗口重要

COMSOL 的模型对象 API 能构造和修改模型、运行研究、绘图及提取数值结果；这些操作能力来自仿真软件的模型 API。[10][11] Java Shell 提供交互执行、补全、停止和错误输出；新建或打开模型会重置 Shell 会话，持久化模型修改仍需保存模型。[5] 因而聊天记录不是模型数据库，Shell 历史也不是完整工程审计日志。

Range 已有 `AgentTaskRunner → ModelCommandHost → Core/服务 → SolverManager → native broker → verification/experiment/convergence` 的基本结构，不需要另建一个绕过原生 FE 的演示服务。应复用这些入口，补齐缺失的压电适配、结果导出和报告，而不是通过 LLM 任意运行 Python 去生成一个替代 plant。[R01]

从机制上可以把执行层分为三类：只读查询直接返回已登记事实；写入操作形成绑定源身份的预览并由 Host 执行；长任务返回 jobId 及可查询的事件/结果。MCP 若以后接入，只应成为这些既有能力的传输适配，不能产生第二套更宽的权限或另一个模型状态。这是 Range 的架构建议；COMSOL 2027 预告仅证实 MCP 经其已有 API 连接 agent，未公开相同的内部划分。[14]

### 5. 错误反馈、Provider 和安全边界

COMSOL 6.4 的公开迭代是：生成代码、送入 Shell、用户 Run、将错误送回 Chatbot、再运行改进代码。官方博客明确说明生成代码不会自动执行，也展示了上下文不足时错误建议的可能性。[3][5][15] 这一机制证明助手可以参与调试，不证明它已自动理解失败的物理根因，或能确保一个“算完”的结果正确。

6.4 支持外部或兼容接口的 LLM，亦可配置自托管服务；这是部署选择，不等于数据自动脱敏、密钥加密存储或供应商不保留数据的保证。[2][3][15] COMSOL 的方法/Java 库安全设置可限制文件、进程、网络等操作，但不能仅凭该页就认定所有 Java Shell 行为具有同样限制，更不能据此推导 LLM 权限控制细节。[9]

Range 应优先复用已经实现的结构化工具和 Host 批准，不为追求“像 COMSOL”而开放任意代码执行。缺材料参数、压电极化方向或约束时先澄清；改变物理问题需重新批准。数值修复可以在预先批准的字段、次数、时间和内存预算内自动执行。Provider 连通性、模拟网络测试、真实工具调度和工程正确性应分别记录结果。

### 6. 模型管理、后处理及全流程的最后一公里

COMSOL 的数值 API 示例包括参数扫描及瞬态结果提取，能取得数值数组与单位；模型管理功能还可支持批处理/集群从版本库读取模型并将输出写回。这些是可供自动化利用的结果和版本基础设施，不是 LLM 自带的长期记忆，也不证明 6.4 Chatbot 会自主解释全部结果。[8][11]

Range 的原生 catalog/channels/series/summary/open/display 已在 Agent 工具中出现，不能继续按旧文档说“没有原生后处理”。但已能读取和显示，不等于已有完整课程报告导出。最终还需要记录观测点、单位、峰值/RMS/衰减指标、控制输入和饱和、FE/ROM 误差、所有收敛层级以及失败限制，并把图表与原始数据绑定同一模型和作业。[R01]

最终报告生成应由确定性程序整理已验证数据，LLM 负责解释证据和组织文字。若某项检查未运行，应写 `notComputed` 并说明原因；不允许模型补写一个好看的误差百分比。完整交付必须在当前安装版上完成模型到报告一次闭环，而不是把服务单测和截图相加宣布完成。

### 7. 当前差距矩阵：已有实现与真正缺项

下面是当前源码静态核查，**不是当前运行通过表**。完整定位和文件 SHA-256 见 `range_agent_gap.json`；这里保留实现决策所需的摘要。`已有未复验`指当前代码存在，但没有与当前字节一致的完整运行证据；`接口未支持`仅指列出的 Agent/服务入口，不表示所有下层库都没有能力。[R01]

| ID | 流程能力 | 当前实现与状态 | 对全流程目标还缺什么 |
|---|---|---|---|
| G01 | 问题澄清与规格 | 已有 ask_user、inspect/diagnose 和完整 ProblemSpec 提案；可信问题族限杆/梁，已有未复验 | 压电课程问题规格、单位/缺参检查、端口和控制目标 |
| G02 | 几何和材料 | 已有坐标/刚体变换/常量材料编辑；已有未复验 | 从空文档到受限压电模板；任意 CAD 布尔/张量材料入口尚不能由当前工具推导 |
| G03 | 网格与重绑定 | mesh.generate/accept/cancel 已有；静力重绑有严格单元/连接条件；已有未复验 | 压电器件、电极、机械约束跨网格的明确重绑与拒绝规则 |
| G04 | 约束、载荷、初值 | 边界设置已有；trusted lane 有限点力与方向约束 | 把机械、激励、初值、电压/电荷及端口角色合并为可验证规格 |
| G05 | 压电与控制配置 | Core 有 `piezo.configure-experiment`；**Agent 枚举未支持** | 严格 schema 窄适配、整实验预览、源绑定、批准、Undo、失效传播 |
| G06 | 求解和作业 | run/cancel/recover 及原生作业引用已有；已有未复验 | 新持久化/checkpoint 回归；历史锁、Windows 路径及失败核对问题闭合 |
| G07 | 诊断 | 有缺支承、单位、材料、退化单元、陈旧来源诊断；未分类失败保留包并停止 | 压电专用诊断分类、可复现故障库；不把未知失败包装成已知根因 |
| G08 | 独立验证 | 静力/模态/瞬态残差、运动学、能量已有；**passive 杆/梁，192 总 DOF 上限** | 压电机电耦合、端口功量及主动控制独立验证；大模型可扩展参考路径 |
| G09 | 有界修复 | 授权字段、物理摘要和预算守门已有；参考位移/频率精度失败可增 segments | 压电修复策略；残差失败与离散误差的分流，防止靠改物理/阈值通过 |
| G10 | 收敛研究 | 网格加密或 dt 减半，固定 1% 相邻 QoI 变化连续两次达标；已有未复验 | 压电电端量/控制 QoI；真实误差对照、适用阶次；应力/反力不在当前通用保证内 |
| G11 | 后处理 | Agent 有原生 catalog/channels/series/summary/open/display；编码电通道已改 | 通用图表/导出窄工具及当前实测；不能把 Python runtime schema 当成 Agent 已暴露工具 |
| G12 | 课程报告/安装交付 | 事实快照已有；完整交付仍属计划/待验收 | 当前安装版模型→求解→验证→收敛→图表→报告、源与贡献清单 |
| G13 | 长任务恢复 | 会话持久化、只读核对、资格查询和部分恢复已有；已有未复验 | 按后端的恢复矩阵、压电控制器状态复验、热 trace 完整性设计 |

G05 是最直接的功能连接缺口：`rmodel_command_service.cpp` 的 Core 命令表和执行逻辑已有压电配置服务，而 `agent_task_runner.cpp` 的 document command schema 尚未枚举。不能通过只改提示词解决；需要让 schema、验证、预览与 Host 真实执行一致。[R01]

G08–G10 是科学验收缺口。已有压电内核测试与控制器证书，可以为新可信服务提供依据；但是服务必须接受正确的原生结果和源身份、重组独立算子，并返回显式适用范围。将一个生产导出矩阵交给另一积分器，只能独立检查积分路径，不能单独证明矩阵组装正确。[R01][R04]

G06/G13 则直接影响演示是否可靠。11 月 5 日审查保留了热 trace 旧前缀同尺寸改写的开放边界，以及历史锁、Win32 error 3 和终态失败核对问题。可恢复不等于任意后端和控制状态均可恢复；应按资格矩阵明确“恢复/重算/待核对”。[R02][R03]

### 8. “残差驱动检查”到底已经做到哪一层

**第一层：离散代数与时间一致性。** 当前可信验证有静力 `K u − f`、模态 `K φ − ω² M φ`、瞬态 `M a + C v + K u − f`，加上运动学和能量检查。计算时需区分自由 DOF 平衡和约束反力，采用逐项尺度及有限数检查，不能靠全局大分量掩盖小分量错误。其价值是发现不平衡、错误帧、积分/阻尼/载荷不一致等问题，但解在粗网格上满足方程，空间离散误差仍可能很大。[R01][R03]

**第二层：参考精度与相邻细化变化。** 当前自动 mesh repair 仅在 required 失败全部是位移/频率参考精度失败时提出增加 segments；残差失败不会直接落入该自动加密分支。现有 convergence service 采用保持时长的 dt 减半或网格细化，固定 1% 相邻 QoI 变化连续两次达标。这给出已实现的停止规则，不自动给出相对精确解 1% 的保证。[R01]

**第三层：有限元空间后验误差估计。** COMSOL 官方说明了 DWR（dual weighted residual）路线：针对目标泛函引入对偶问题，在更高阶有限元空间计算弱残差，结合对偶误差权重形成单元误差贡献。它还明确列出适定性、正则性及对偶近似等条件，且不包含提前终止求解的代数误差。这比简单计算 `K u − f` 多了函数空间、对偶问题和单元级误差信息，Range 当前不能声称已经实现同等通用估计器。[16]

对当前课程，更合理的顺序是先建立可核查的第一、二层，再选择一个线性、适定且有解析或高精度参考的杆/梁 QoI 作为第三层实验。若要实现 DWR 或残差型局部自适应，应另立算法任务：定义弱式、单元/边界跳量、对偶边界条件、富集空间、积分阶次、标记策略和重绑定；在固定问题上计算真实误差与估计量之比，验证收敛与失效边界。不要把现有均匀加密重新命名为“自适应网格”。

### 9. 压电主线的科学验收要求

现有交付合同明确压电不是把位移换一个名字输出：机械作用与电荷读出需共享 `M, K^E, G, Ce` 和工作点；采用约定 `V=φ+−φ−` 时，线性方程为 `M q̈ + C q̇ + K^E q + G V = f`、`Q = Ce V − Gᵀ q`。工作点、约束变换和电端口角色必须随模型保存。开路传感只凝聚对应传感子集，不能把电压作动端口同时冒充独立开路测量。[R04]

建议课程报告至少区分四类证据。第一类是材料和组装正确性：本构条件转换、极化旋转、单位、质量/惯量、互易、有限差分 `∂f/∂V` 与 `∂Q/∂q`、开短路差异。第二类是积分正确性：同一作用输入下与独立积分器对照，逐步细化 dt。第三类是模型简化误差：同载荷 FE/ROM 回放与各自闭环分别比较。第四类是控制效果：关闭/开启同一扰动，报告 RMS、峰值、衰减、控制电压、饱和和必要时 MPC 可行性/KKT。[R04]

独立性要按层说明。使用生产导出的矩阵而独立 RK4 积分，可发现时间推进或状态映射错误，但无法排除生产矩阵共同错误；需再用手算矩阵、独立单元组装、解析极限或外部基准检查。COMSOL 官方剪切驱动梁算例使用特定 PZT-5H、极化方向、几何、电极和静力设定，适合学习可复现实例的写法；它不是本项目不同 PZT-5A 多杆动力学的现成数值参考，不应直接搬用其结果验收你的案例。[17]

LQR 的“全状态投影”与“仅传感器反馈”也要分开。前者可以用于验证最优控制与 FE/ROM 一致性；后者还需观测器、可观/可检测检查、噪声假设及估计状态恢复。观察到了电信号，不代表控制器只使用该电信号。MPC 需报告约束和优化求解证书，不能失败后静默切换另一控制器。[R04]

## Synthesis — 面向 Range 的全流程架构建议

下面全部是**拟议设计**，不是 COMSOL 内部实现说明，也不是 Range 当前已完成功能表。优先沿用现有命令、作业、验证和实验服务；新增能力应成为窄适配和明确合同，而不是旁路。

### 1. 七层职责与最小闭环

| 层 | 职责 | 课程最小可交付 |
|---|---|---|
| 工程规格 | 用户目标、单位、材料来源、允许操作、QoI 与验收阈值 | 一个固定压电案例，缺参明确提问，禁止猜约束 |
| 模型上下文 | 当前状态、实体/端口 ID、revision/digest、支持矩阵和失效项 | 可查看的结构化摘要，选中对象上下文 |
| 规划/对话 | 分解目标，选已支持工具，解释结果和限制 | 单 Agent；状态/证据由 Host 持有 |
| 命令/事务 | 校验、预览、批准、执行、Undo、源身份守门 | 将压电配置窄适配给 Agent，保持整实验事务 |
| 作业执行 | 原生 FE、控制后端、事件、取消、恢复 | 真 jobId、接受帧、结果清单及明确终态 |
| 数值验收 | 独立残差、参考、能量、收敛与预算 | 压电适用范围明确；未知检查不能算通过 |
| 后处理/交付 | 曲线、CSV、图、报告、复现包 | 输入/源/作业/图表/检查一一绑定 |

推荐的任务状态可表示为 `needs_input → specified → previewed → approved → running → verifying → refining → reporting → completed`，并允许进入 `needs_confirmation / failed / cancelled / budget_exhausted / needs_reconciliation`。这是状态设计草案；实现前需映射现有状态机而非另造并行真值。`completed` 必须由主机根据验收合同产生，不能由模型说“完成”触发。

最小闭环应包含一条真实链：用户选择课程 PZT 案例 → 询问或确认扰动/控制目标 → Agent 读取模型和能力 → 提交完整压电实验预览 → Host 批准 → 真实 FE 关闭/开启控制 → 查询原生通道 → 执行独立检查 → 必要时在授权范围细化 dt → 显示同源曲线并导出课程报告。首版允许一个整体确认和必要物理变更确认，不要求每一步只读查询都打断用户。

### 2. 工具合同建议

下表工具名是**候选接口**，需先查现有 schema 与命名规范；不得把这张表直接加入 capability 返回值。尽量让新能力映射到现有服务，保留模型的 source UUID/revision、任务归属、批准失效与工件身份。

| 候选能力 | 关键输入 | 必须返回的证据 | 必须拒绝的情形 |
|---|---|---|---|
| 压电规格检查 | documentRef、单位、材料/极化、机械/电端口、控制/扰动 | 支持族、缺项、合法性、sourceDigest | unsupported family、缺物理、非有限数 |
| 压电配置预览 | 当前 documentRef、expectedRevision、完整实验、物理 ID | diff、previewId、candidateDigest、失效项 | 陈旧源、角色冲突、非法自由度 |
| 执行配置 | previewId、Host approval token、task ownership | revision、事务回执、Undo 关联 | 不复用旧批准、部分应用、跨任务 preview |
| 成对控制实验 | 冻结模型、off/on、激励、步长、控制采样、预算 | jobId、原生 plant 身份、终态、artifactId | off/on 改了模型/载荷但未声明，后端替换 |
| 压电独立验证 | 原生 job/artifact、固定验收合同 | 每项 value/scale/tolerance/status/source | 任意路径替代、错帧/错源、notComputed 当 pass |
| 数值细化 | 原始物理 digest、授权字段和范围、QoI、预算 | 每层误差/变化/成本/停止原因 | 改材料/载荷/BC/控制目标，放宽阈值 |
| 图表/导出 | 已登记 channelId、区间、单位、输出包目标 | 数据哈希、图表参数、来源标签 | reference 冒充 native，progress 当物理解 |
| 报告包 | 上述所有身份和验收状态、模板版本 | manifest、报告、数据、图和复现命令 | 缺关键工件仍声称 complete |

实现要包含大小/时间/嵌套限制；任务归属与源有效性每次核查。工具错误应返回可机器识别的类别和可审阅详情，例如 `stale_source`、`unsupported_physics`、`verification_failed`、`backend_unavailable`；避免把所有失败折叠成供 LLM 猜测的文本。支持 `dry_run/preview` 与正式执行回执，才能在不启动长算例的情况下验证调度。

### 3. 压电可信验证草案

先固定符号和工作点，在自由机械 DOF 上重算 `r_m=M a+C v+K^E q+G V−f`（非线性/预应力情形使用实际切线和对应恢复力，不能以线性式冒充完整非线性平衡）；重算 `r_Q=Q−Ce V+Gᵀq`，并检查开路传感子集的电荷条件。电荷代数关系与端口角色必须一致，不能把理想电压源电压当作独立传感器。

能量账本应从相同符号约定推导，并与实现保存的绝对量/工作点增量一致。以线性可逆系统为例，可取 `U(q,Q)=½qᵀK^Eq+½(Q+Gᵀq)ᵀCe⁻¹(Q+Gᵀq)`；由此电端功率为 `Vᵀ Q̇`。这只是可用于核对符号的候选表达式，需满足可逆的相关电容子块及端口边界假设；不能不经推导把含作动/传感凝聚后的所有端口简单混用。离散时间上要与输入保持、外力端点、耗散及积分误差共同验收，并用正负电压/互易负例检查符号。

控制验证应分离设计与运行：LQR 检查 DARE 残差、适用的可稳定条件与离散闭环极点；MPC 检查步级约束、求解状态及 KKT；轨迹检查实际应用电压及饱和，不用请求动作替代实际动作。步长细化时保持物理时长、扰动和控制采样周期，或显式记录变更，否则结果差异混合了积分与控制离散化。

### 4. 残差到修复的决策规则

残差不通过时，先查数据来源、帧/时间、单位、算子一致性、约束反力、积分与载荷端点；这类故障不能通过一律加网格修复。参考解误差或相邻细化变化过大时，才在已经授权的网格/步长范围内提出数值细化。若修改支承、材料、极化、电极或控制律才可能满足目标，应视为新物理方案，重新预览并批准。

每次修复记录原始失败、依据、允许字段、前后配置、物理 digest、源身份、资源成本和验收结果。达到迭代或资源预算后保留最佳已验证结果与未达指标，返回明确停止原因。无法判定根因时输出诊断包；“智能体知道什么时候停止”应成为可演示能力，而非靠不断尝试碰巧通过。

## Recommendations — 实施顺序和可执行验收

以下阶段为**本报告新增建议 D0–D6**，不改写旧 PC、WF、CW 或 Trusted Agent 0–8 编号，也不把历史豁免改成阻塞。不要同时推进所有宽泛扩展；先拿到一个证据完整的最小全流程。

| 次序 | 交付范围 | 主要实现位置/复用点 | 完成条件 |
|---|---|---|---|
| D0 | 当前源码与构建基线；压电代表链 | CMake、PZT/三族 delivery tests、bridge | 目标构建和选定数值用例通过；source/binary/env、原始日志/CSV/恢复工件可核对 |
| D1 | 当前可信 Agent 回归 | `RUNTIME_VALIDATION_20261105.md` 对应 selectors | 新增持久化、任务隔离、非有限数、checkpoint 等相关回归实跑；失败逐项记录 |
| D2 | 压电 Core→Agent 接通 | `AgentTaskRunner` schema/验证、`ModelCommandHost`、`RPiezoExperimentService` | 真 Host 正常流+陈旧源/未批准/非法端口/Undo 负例；真实模型发生预期变化 |
| D3 | 压电独立验收和细化 | 独立 operators/reference、可信服务 | 材料/组装、机电平衡、积分/ROM/控制分别验收；notComputed 显式；2 个故意错误被检出 |
| D4 | 原生后处理与报告包 | broker/channel、display/export、模板渲染 | 同源 off/on 曲线、电量/动作、CSV 和报告可重开；每个数字追溯到工件 |
| D5 | 当前安装版真实 Provider 全链 | 已有 Provider/Host/solver，不新增旁路 | 一次用户目标到报告，含真实批准、一次可解释数值修复或明确无需修复、取消/恢复验证 |
| D6 | 学术增强及可选 MCP | 局部误差估计、适配现有窄工具 | 有解析/高精度参考的单一 QoI 算法研究；MCP 不扩大权限；各自独立验收 |

D1 和 D2 的服务/Host 测试不能被 mock Provider 对话替代。D3 应在小模型上先排除共同矩阵错误，再扩展规模。D5 的真实 Provider 成本和授权沿用项目现有配置，不把密钥写入工件。本次研究没有执行真实 Provider 调用。

高质量课程演示建议设置三段可复查脚本：正常建模与压电闭环；故意给出一个可诊断的无效输入；中断后核对/恢复及报告重开。每段展示任务目标、软件实际操作、检查结果和证据位置。若采用“无支承”负例，智能体必须询问真实支承而不是擅自固定节点；若采用步长过大负例，要事先定义可修复字段及保持不变的物理条件。

### 数值与控制验收表建议

| 检查 | 基线要求 | 报告内容 |
|---|---|---|
| 源/模型一致 | off/on 相同物理与扰动，控制开关为唯一预定差异 | digest、模型参数、端口/材料表 |
| 静态/工作点 | 自由 DOF 平衡及非零偏置一致，反力单列 | 分量尺度、残差、容差来源 |
| 电学 | 正确开路/短路/电荷放大器语义，互易/正负极性 | 电压/电荷单位与端口约定 |
| 时间推进 | 独立积分对照，至少多个 dt 层级 | 误差对 dt 和成本，不只 pass |
| FE/ROM | 同输入回放与独立闭环分开 | 选模态、截断范围、QoI 误差 |
| LQR/MPC | 实际求解证书和实际电压/约束 | 稳定性/可行性、饱和与性能代价 |
| 控制效果 | 事先指定峰值/RMS/衰减等评价窗 | off/on 数值，不只视觉好看 |
| 恢复 | 中断前后状态/动作/时间序列一致 | RAM/RBM、控制状态、逐相位身份 |
| Agent | 正常、缺参、越权、陈旧源、失败停止 | 完整工具调用/Host/作业/检查收据 |
| 交付 | 安装版实际运行与源/二进制绑定 | 可重开模型、数据、图、报告、复现命令 |

阈值应来自算法误差、参考精度、物理量尺度和课程问题要求，在运行前冻结。不得为获得漂亮结果临时放宽。大模型不能使用超过适用范围的小型 dense verifier 直接宣布通过；需返回范围限制，或单独设计经验证的稀疏/分块检查。

## Limitations — 未完成事项与结论边界

本次没有安装或实际运行 COMSOL，没有其商业许可证下的可复现操作录像；关于 COMSOL 的事实来自官方文档和新闻稿。官方来源适合确认公开接口与操作步骤，但同一厂商的多个页面不构成独立性能评测。没有实测 Range 对 COMSOL 的速度、正确率、操作次数或成本优势。

2027 部分以 2026-09-16 的预告为依据，正式发行状态、MCP 工具定义、鉴权、传输、权限隔离、取消/恢复语义仍未在本次调查中核实。6.4 在线手册也可能随补丁更新；实施 Provider 兼容性时需固定具体 build，而不能仅写“支持 6.4”。[7][14]

当前审查有“已实现但未测试”的大量项目，不能用旧 Windows 验收或本轮定向数值测试覆盖它们。11 月 5 日审核自身为 `resumed_review_incomplete`：321 项静态阅读、51 项差异审查、42 项修改片段审查、2 项部分完成；其中承接的旧数值审查并非全部重新逐文件阅读。完整当前 C++、Windows、Provider 和安装版模型到报告仍需相应证据。[R02][R03]

压电合同 PC 系列的可携验收记录为 **3/36**，不能继续引用早期 33/36 作为当前事实；缺少旧原始收据时，不推测差异原因，也不因有一个代表用例通过就重写全部 PC 状态。Trusted Agent 的 180 槽对照/独立 holdout 及新 Windows 账户首次使用已有历史豁免，未执行并不等于通过，也不应被本报告擅自重新设为课程阻塞。[R02][R05]

热 trace 同尺寸旧前缀改写、历史持久化锁/Windows 路径失败及终态失败后核对，仍需分别处理；冷读全量拒绝测试不能自动证明热路径持续完整性。恢复资格必须绑定实际后端、checkpoint、控制/估计器状态，不宜承诺所有工作均可无损恢复。[R02][R03]

## Counterevidence Register — 容易误判的反证

| 直觉或旧说法 | 本次反证 | 对开发的影响 |
|---|---|---|
| “COMSOL 6.4 已是自主全流程 Agent” | 6.4 公开流程需用户 Run；2027 预告才明确 MCP agent 循环。[3][4][14][15] | 学习其模型 API 和上下文；自主闭环另行设计验收 |
| “Range 只有聊天，没有原生结果工具” | 当前源码已有 query/display/open 及网格/恢复命令，旧手册落后。[R01] | 补缺口并修文档，不重写重复基础设施 |
| “压电模型跑通，所以 Agent 压电全流程完成” | Core 有压电命令，Agent schema 未暴露；可信 verifier 限 passive 杆/梁。[R01] | 接通工具和独立验收，再做真实 Provider 安装版链 |
| “残差小就是 FE 解准确” | 离散平衡不保证空间误差；COMSOL 另有 DWR/富集弱残差理论。[16] | 代数检查、参考误差、收敛、后验估计分别报告 |
| “修过/审过就等于通过” | 审核明确没跑新增 C++ 回归；历史身份不覆盖当前 dirty。[R02][R03] | 当前源/二进制与运行收据逐项绑定 |
| “电信号可见就是传感器闭环” | 全状态投影与观测器模式合同分开。[R04] | 演示明确反馈量，观测器另验收 |

## Claims-Evidence Table — 主要判断到证据的映射

| 判断 | 证据 | 类型/适用性 |
|---|---|---|
| 6.4 为上下文辅助和用户执行代码路线 | [2][3][4][5][15] | 官方功能/操作说明；非性能比较 |
| 模型 API 是建模、求解、结果处理公共执行基础 | [5][10][11] | 官方 API 示例；不等于 Chatbot 自主调度 |
| 2027 预告外部 MCP agent 循环 | [14] | 官方前瞻声明；唯一该版本直接来源，非出货实测 |
| 当前 Agent 压电配置和可信验证存在连接缺口 | [R01] 的 G05/G08 及带哈希源码定位 | 当前静态证据；运行结果另列 |
| 当前 mesh repair 不是残差失败直接驱动 | [R01] G09，experiment service 1783–1838 | 当前源码决策逻辑 |
| 当前 convergence 不等于真误差保证 | [R01] G10 对照 [16] | 源码规则与理论边界 |
| 当前完整产品链尚未验收 | [R02][R03][R05] | 本机可携文档；历史原设备原始收据不全 |
| 七层架构、D0–D6 和候选工具合同 | 本报告综合建议 | 设计提案，无 COMSOL 内部实现断言 |

## Bibliography — COMSOL 官方来源

[1] COMSOL (2025). "COMSOL Speeds Simulation with Expanded NVIDIA GPU Support for COMSOL Multiphysics Version 6.4". [发布新闻稿](https://www.comsol.com/press-release/comsol-speeds-simulation-with-expanded-nvidia-gpu-support-for-comsol-multiphysics-version-64-14612). 发布 2025-11-18；访问 2026-11-05。

[2] COMSOL. "COMSOL Desktop Updates — 6.4 Release Highlights". [Desktop 新功能](https://www.comsol.com/release/6.4/comsol-desktop). 6.4，Chatbot Window / Code Generation Additions；访问 2026-11-05。

[3] COMSOL. "Using a Chatbot". [6.4 参考手册](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_modeling.19.029.html). Model Builder Nodes / Model History / Running Code / Preferences；访问 2026-11-05。

[4] COMSOL. "The Chatbot Window — Application Programming Guide". [6.4 编程指南](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.14.html). Tool calling 与录制代码示例；访问 2026-11-05。

[5] COMSOL. "The Java Shell and Data Viewer Windows". [6.4 Java Shell](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.13.html). Run/Stop/Record、会话和错误输出；访问 2026-11-05。

[6] COMSOL. "New and Improved General Functionality in COMSOL Multiphysics". [6.4 发布说明](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_release_text.06.007.html). Chatbot；访问 2026-11-05。

[7] COMSOL (2026). "COMSOL Software Version 6.4 Update 3". [累计补丁说明](https://www.comsol.com/product-update/6.4). U3 2026-05-29，含 U1/U2 标记；访问 2026-11-05。

[8] COMSOL. "Model Manager Updates — 6.4 Release Highlights". [Model Manager 新功能](https://www.comsol.com/release/6.4/model-manager). Study Support for Batches and Clusters；访问 2026-11-05。

[9] COMSOL. "Security Settings". [6.4 安全设置](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_running.38.10.html). Methods and Java Libraries / Internet；访问 2026-11-05。

[10] COMSOL. "Working with Model Objects". [6.4 模型对象 API](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.30.html). 嵌入模型、构建示例、Loading Another Model；访问 2026-11-05。

[11] COMSOL. "Getting Numerical Data". [6.4 数值结果提取](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.77.html). 参数扫描与瞬态数据；访问 2026-11-05。

[12] COMSOL. "Application Builder Updates — 6.3 Release Highlights". [6.3 新功能](https://www.comsol.com/release/6.3/application-builder). Chatbot / Java Shell；访问 2026-11-05。

[13] COMSOL (2024). "COMSOL Releases Version 6.3 of COMSOL Multiphysics". [6.3 发布新闻稿](https://www.comsol.com/press-release/comsol-releases-version-63-of-comsol-multiphysics-14481). 发布 2024-11-19；访问 2026-11-05。

[14] COMSOL (2026). "System-Level Modeling and Agentic AI Take the Spotlight in COMSOL Multiphysics Version 2027". [2027 预告](https://www.comsol.com/press-release/system-level-modeling-and-agentic-ai-take-the-spotlight-in-comsol-multiphysics-version-2027-14722). 发布 2026-09-16，Advancing AI-Assisted Simulation with Agentic Workflows；访问 2026-11-05。

[15] COMSOL (2025). "Using the Chatbot Window in COMSOL Multiphysics". [官方使用文章](https://ws-bos.comsol.com/blogs/using-the-chatbot-window-in-comsol-multiphysics). 首发 2025-06-12，更新 2026-02-27；访问 2026-11-05。

[16] COMSOL. "Error Estimation — Theory and Variables". [6.4 后验误差理论](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_solver.36.015.html). Theory Background / Accuracy；访问 2026-11-05。

[17] COMSOL. "Piezoelectric Shear-Actuated Beam". [6.4 官方压电梁算例](https://doc.comsol.com/6.4/doc/com.comsol.help.models.mems.shear_bender/shear_bender.html). Model Definition / Implementation / Results；访问 2026-11-05。

## Methodology — 方法、本地证据与续作要求

本轮先阅读最新审核、运行验证计划和压电交付合同，再让独立工作单元分别核对官方资料、当前 Agent 源码与压电代表链。搜索限定官方 COMSOL 站点和其文档站，逐条核对正文、版本、发布日期和可定位章节；不使用论坛转帖证明技术机制。搜索 CLI 在本机未发现，使用可用的 Web 工具回退。未复制整站文档；短摘录仅存证据索引，报告采用释义。

检索覆盖的主题为 6.3/6.4 版本沿革、Chatbot 上下文、Java Shell/录制/API、文档检索、Provider、安全设置、模型管理、数值后处理、2027 MCP 预告及空间后验误差。没有把“未找到公开实现”推成“厂商内部不存在”。17 个官方来源属于同一厂商证据群；对模型机制有多页交叉支持，对 2027 MCP 则明确只有前瞻性直接来源。

本地证据编号如下。源码细节以带 SHA-256 的 `range_agent_gap.json` 定位；行号会随修复变化，续作时先核对当前文件而非盲信旧行号。

- [R01] 本次源码核查：[能力差距明细](range_agent_gap.md)、[结构化源码定位](range_agent_gap.json)。
- [R02] 最新静态审核：[RESUME_REVIEW_20261105.md](../../2026-11-04-source-audit/reports/RESUME_REVIEW_20261105.md)。
- [R03] 当前版本待运行矩阵：[RUNTIME_VALIDATION_20261105.md](../../2026-11-04-source-audit/reports/RUNTIME_VALIDATION_20261105.md)。
- [R04] 压电范围及验收合同：[PIEZO_CONTROL_DELIVERY.md](../../../PIEZO_CONTROL_DELIVERY.md)。
- [R05] 历史验收及豁免：[phase-checkpoint-20261030.md](../../2026-10-30-source-handoff/acceptance/phase-checkpoint-20261030.md)。

后续 AI 应先读 `developer_handoff.json`，重新确认工作树、相关子模块、Python 和构建身份，保留所有既有未提交修改。先关闭 D0/D1 当前版本证据，再按依赖推进 D2–D5。不要重开用户豁免的 180 槽对照任务；不要把本报告建议接口当成现有工具；不要把测试代码调用的数值后端称为真实 LLM Provider。

**最新执行约束：** 用户随后要求本轮只做 COMSOL 调研，Claude 正并行工作。上述 D0–D6 是未来建议，不授权继续实施。本轮已完成的有限运行和中断情况以 [piezo_validation.md](piezo_validation.md) 为准：短链有通过，完整 PZT 的参考对照及恢复未完成，不能宣布全例通过。此前 12 个源码/脚本改动保留在现场，未自动回退，清单见该报告。
