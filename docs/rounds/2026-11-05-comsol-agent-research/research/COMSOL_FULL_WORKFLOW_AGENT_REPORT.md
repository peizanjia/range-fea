# COMSOL 全流程 Agent：公开实现路线、能力边界及可借鉴架构

研究日期：2026-11-05。用途：供其他 AI 继续设计仿真软件全流程智能体。**本报告以搜索调研为主，不是实施记录或产品验收证书。** 按用户最新要求，已停止本轮构建和测试；之后仅整理专属报告目录，不再修改产品代码、重配或运行算例。Range 当前源码差距另列在附属报告，不作为本文主体。

## Executive Summary — 核心结论

COMSOL 已经公开了真正的 Agent 工作流，不应把它的能力仅理解成 6.4 内置 Chatbot。需要区分三条路线：**Chatbot＋用户执行代码；外部 Agent＋现有 Java API＋共享服务器模型；2027 预告的官方 MCP Server**。第二条已见官方实际演示，不需要等第三条才能成立。[1][2][3]

公开技术基础是一个覆盖建模、网格、求解及结果的公共 API，以及可由图形界面和程序访问的模型对象。Agent 的规划与工具执行循环在外部 harness 中完成，COMSOL 提供工程模型和执行能力。用户仍可以在 Desktop 中观察与审阅，而不只是让代理在另一个脚本里计算一个独立小模型。[4][5][7][20]

2026 年 9 月 18 日官方文章给出了 Codex 连接活模型、开展求解器实验的实例。它是可借鉴的最小桥接方案，不是已证明完备的通用工程自动驾驶系统。[1] 当前官方技术页还明确描述 Agent 按需查询模型和结果，进行多步建模、诊断及调整；这比一次性给聊天模型附上文件更接近用户想要的效果。[2]

2027 MCP Server 则将接入标准化。2026 年 9 月的发布预告明确说明外部 Agent 可建模、运行、检查结果并决定后续动作；但本次读取的公开下载页仍列 6.4.0.429，发行历史最新条目仍为 6.4。因此本文仅确认官方已预告及展示相关路线，不把 2027 写成已核实正式交付的版本，也不编造其工具 schema。[3][17][18]

对 Range 最有价值的启发，是先补齐**共享模型的可查询/可修改接口、可复现示例、真实求解与结果回读、工程验收条件、可中断的有界执行**，再决定是否采用 MCP。协议本身不会赋予物理判断力。现有 Agent 及压电能力可静态对照，但本轮不继续实施改造。

## Introduction — 调研问题和证据分级

本次问题不是“COMSOL 能不能聊天”，而是它如何把用户目标变成真实模型变更，调用实际求解器，获取可计算的结果，再据此继续工作；同时厘清什么已经有公开实例，什么只见功能描述或未来版本预告。

全文使用四种证据类型：**公开实作**是官方描述的具体实验，仍不等于本机独立复现；**API 合同**是文档可查的函数/状态机制；**产品描述或预告**说明厂商公布的目标与能力；**推导建议**是我们据此提出的 Range 设计。网页和手册没有披露的内部实现一律保留未知。

研究不登录账户、不绕过录像访问限制、不安装 COMSOL、不连接商业许可证或真实模型，也不执行官方示例代码。所列步骤用于理解结构，不是已在用户设备运行的操作。9 月官方实作、技术页和 API 文档互相补充；同一厂商多篇资料仍属于同一证据群，不能视为独立性能验证。

## Main Analysis — 全流程机制拆解

### 1. 三条路线同时存在，不能只按版本给能力贴标签

| 路线 | 规划/推理在哪里 | 怎样执行 | 怎样取得反馈 | 公开证据强度 |
|---|---|---|---|---|
| 内置 Chatbot | 外部 LLM，经 COMSOL 对话窗口交互 | 生成 Java API 代码，用户送至 Java Shell 执行 | 用户发送错误、模型附件、图像等 | 6.4 手册有明确操作步骤。[12] |
| 外部 Agent + Java API | 外部 Agent 的 harness 与 LLM | 程序连接 COMSOL Multiphysics server，操作其中的真实模型 | API 数值结果、设置、输出及进度日志 | 官方 2026-09-18 文章给出具体实例；API 文档支持关键机制。[1][5][7] |
| 外部 Agent + 官方 MCP Server | 外部 Agent；服务器提供标准接入 | MCP 连接已有 COMSOL API | 模型/仿真结果用于下一步决策 | 2027 官方预告；本次未取得完整正式工具合同。[3] |
| 合作伙伴 Agent | 合作伙伴系统，例如 Nexus | 按合作伙伴集成方式连接 CAD/仿真等 | 取决于其集成 | 官方联合 webinar 页面证实合作演示；不等于 COMSOL 内置 Agent。[16] |

因此，“6.4 Chatbot 不自动运行代码”与“COMSOL 已有外部 Agent 全流程实例”并不矛盾。前者描述一个 UI 组件，后者利用软件既有程序接口。不能从 Chatbot 的工具菜单推断整个软件生态的自动化上限，也不能把第三方 MCP 仓库当成 2027 官方 MCP Server。

### 2. 共享模型和 GUI 深度集成的基础

COMSOL API 概览明确：多个程序接口共享驱动 UI 的底层类和数据结构，覆盖几何、网格、求解、可视化及结果求值。[4] 这解释了为何代理可以操作真实模型，而不仅仅输出一段脚本供人搬运。

其 client–server 架构允许将计算引擎作为独立进程；Desktop 成为连接它的客户端。同一用户可以有多个连接，所以图形界面与 API 客户端有共享同一个活模型的基础。它是单用户服务器，不能直接按多人 SaaS 服务理解。[7]

现有 Java client–server 路线的许可证条件是已知的：同机客户端/服务器可用任一有效 Multiphysics 许可证，跨计算机连接需 FNL。这与尚未核实的官方 MCP Server 许可条件应分别记录。[7]

模型对象通过 tag 定位；管理接口提供列举模型、获取对象、加载、复制及恢复目录读入等能力。[5] 对接时，必须区分“连接已有模型”和“新加载另一份模型”：两者可能都能求解，却只有前者能支持用户看到代理正在修改的那个对象。**这是对现有 API 的工程推论**；集成方案需要明确模型归属、保存位置和修改后的可见性。

同名的三个 server 也不能混用。COMSOL Multiphysics server 是计算引擎；许可证服务器负责许可证；COMSOL Server™ 是面向仿真应用部署的产品。学习 Agent 连接路线时，应先确认官方文档指的是哪一个，不能因为看到 server 就购买或搭建另一产品。[1][7]

### 3. 已公开的真实 Agent 实例证明了什么

9 月 18 日的官方实例使用 Codex 和轻量 skill，通过 Java 连接服务器模型，并保存交互代码与日志。散热器案例进行了九组求解器参数实验；作者称非线性迭代减少 25%、计算约快 30%，本次未独立复现，也不据此换算精确耗时降幅。文章同时指出：错误改变终止条件也可“减少迭代”，长任务恢复仍需加强。[1]

其可核实贡献是**真实软件动作—结果观察—下一步选择—报告**的闭环。上述比例只属于该例的厂商结果，不能扩展成所有物理场、所有模型或不同机器上的收益，更不能当成 Range 的性能目标。本文没有重复运行该例。

从 API 合同可以独立解释其桥接为何可行：Java 代码可编译后运行，应用既可以连接 server，也可以按独立进程方式使用 COMSOL；这与将应用打包为 COMSOL Compiler™ 的独立仿真 App 是不同事项。[8] 对 AI 开发者而言，最小闭环的关键是稳定执行实际 API 与读回事实，而不是先实现复杂 Agent 编队。

### 4. Agent 如何获取当前上下文

Chatbot 路线依赖用户附加的模型节点、子树、模型历史、文字和图像。手册说明节点/历史用 Java 代码表示；这一过程并不证明它总能自动看到所有模型信息。[12]

Agent 路线则能按需检索模型树、特征设置、选择集、参数、变量和结果，随着任务进展补充信息。官方技术页将这种主动查询与一次性附加模型明确区分，也描述了创建/组合几何和修改后重新检查的能力。[2]

开发启发是把上下文分成“长期稳定知识”和“当前模型事实”。前者是版本化文档与可运行示例；后者必须每次由 API 查询，携带模型标识和状态。这里的分层是**建议方案**，不是已披露的 COMSOL 内部提示词结构。若只给 LLM 截图和一份旧模型文件，它仍可能对错误版本提出正确但无效的建议。

### 5. 怎样把人工示范变成可靠 API 操作

COMSOL 提供录制模型操作、复制节点设置代码和交互执行工具。6.4 的 UI 更新加入直接录制到 Java Shell 及复制可见设置的功能，为模型提供真实 API 用法，而不是要求其凭记忆生成所有 property key。[23]

模型对象编程指南展示了嵌入模型、运行求解序列、结果处理及保存/加载的使用关系。[20] 这说明“API 具有全流程覆盖”有执行基础，但**API 覆盖不等于任意自然语言问题一定能正确完成**；Agent 仍可能选错材料、边界或物理假设。

对接续开发的 AI，适合借鉴的是维护小而可运行的示范：一个受限几何模板、一个材料/约束设置、一个求解任务、一个原生结果查询和一个完整报告示例。示范与当前构建相匹配，比堆积大量未维护提示词更容易验证。这是实施建议，当前不在 Range 中执行。

### 6. 几何空间理解与选择集

有限元建模中的“把这个面固定”需要定位到正确实体。命名选择机制可以由几何特征输出对象/域/边界等选择，也能作为后续几何与物理操作的输入。文档区分几何序列中的选择及最终几何上的选择，二者标签规则不同。[9]

通用 selection API 允许明确设置实体、使用命名选择，并根据特征类型限制合法方法；例如边界条件不能接受任何维度的任意选择赋值。[24] 网格选择也有专门规则：依附几何的网格与自行定义几何的网格，能引用的选择来源不同。[21]

因此可借鉴的机制是“语义名称＋实体类型/维度＋查询校验”，不能只缓存一个整数面号。**建议**在重建几何、改变拓扑或网格后重新核对选择数量、空间范围和物理角色；图像辅助可帮助理解，但不能替代 API 对实际选择的确认。命名选择也不是任意 CAD 拓扑变化后自动正确的保证。

### 7. 求解、日志与并发控制

ModelUtil 提供连接/断开及进度输出；进度可写入文件或标准输出，供外部执行器收集。[5] Javadoc 还提供 busy handler、其他客户端阻塞、模型变更回调和恢复目录等接口；多个客户端可连接，但同一时刻只有一个客户端可请求服务器执行操作。[6]

这对全流程 Agent 的含义是：GUI 和 Agent 的共同模型需要协调。**建议的执行器**应能区分服务器忙、编译/API 错误、求解未收敛、任务取消和超时；在有限等待后返回状态，不无限阻塞。一个进程退出、网络断开或聊天结束，也不能自动等同模型已经保存或仿真已成功。

API 具有恢复目录接口并不证明官方示范已实现业务级断点恢复。完整 Agent 恢复还要重建用户目标、模型身份、已应用修改、当前研究、参数试验层级和结果有效性。必须通过运行证据验证这些状态，而不是看到 `loadRecovery` 就宣称“任务可恢复”。

### 8. 后处理为什么能驱动下一轮决策

COMSOL 的编程指南不仅能生成图，还能取回数值数组、单位以及参数/时间序列结果。[10] 平均值等具体数值 API 明确给出结果求值与返回矩阵的行为。[22] 这使 Agent 可以依据可比较的数据决定继续计算，而不必仅从彩色云图猜测。

几何和质量量测也可以通过 API 返回数值及单位；手册还提醒不同结果入口的数组方向可能不同。[25] 对 Agent 而言，数组列的物理含义、时间/参数顺序、单位、数据集和选择集都是结果的一部分。读取成功但解释错列，同样会得到错误工程结论。

**建议**把后处理标准化为“数据集身份—观测定义—值及单位—求值状态—原始数据位置”，报告生成器消费这份结构化结果。若还要由 Agent 选择下一组参数，应先定义评价指标和不允许改变的物理条件。COMSOL 官方 MCP 预告提及验证和报告的目标，但没有因此公开一个通用的数值正确性认证器。[3]

### 9. 搜索文档、Provider 及安全设计

6.4 Chatbot 的公开 tool calling 用于文档浏览；其 API 调用能力与外部 Agent 的仿真执行能力应分开。补丁说明还显示文档示例、Provider 后端和模型选项会更新，故“支持 6.4”仍不足以锁定完整环境。[12][13]

COMSOL 方法/Java 库安全设置有文件、网络、外部进程等控制；不能直接推导其覆盖外部 Agent 的全部操作，更不能把模型 API 权限、MCP 权限和 LLM 数据政策当成同一个开关。[11] 未来若采用 MCP，应核对实际安装版的鉴权、访问范围和操作合同，本轮未取得这些正式细节。

**可借鉴的实施要求**是：只读查询和修改动作分开；大范围改变模型、导出敏感数据和长算例有明确预算与授权；保存实际动作和工具回执。公开资料没有披露 COMSOL 采用哪种 embedding、planner、记忆库或私有提示词，不能用常见 Agent 技术栈补写为事实。

### 10. MCP 的真实作用及当前交付状态

官方 2027 预告把 MCP 定义为外部 Agent 与已有 COMSOL API 的标准化交互途径，覆盖建模、运行、结果检查和后续决策。[3] Boston 会议日程还提到 upcoming MCP Server 及与 NVIDIA 模型的测试，说明已有厂商测试线索；这仍不等于可直接取得其正式客户端配置或工具列表。[19]

截至本次读取，发行历史与下载页仍以 6.4 为最新公开项；因此当前更可靠的结论是“已有 API Agent 实作；官方 MCP 产品化正在公开推进”。[17][18] 页面可能继续更新，后续 AI 需要在真实接入前重新核对。

MCP 不会替代建模内核，也不会自动提供物理可信性。**架构推论**：同一套可靠模型操作可供内置 UI、外部 Java 客户端或 MCP 适配使用；替换协议时不应另外创建一个无法与 GUI 核对的模型真值。现有材料不足以判断 COMSOL 官方 MCP 究竟暴露任意代码工具、细粒度工具还是二者组合，这应列为待核实接口问题。

### 11. 各阶段到底有什么证据

| 流程环节 | 已有公开支撑 | 尚不能宣称 |
|---|---|---|
| 自然语言到任务计划 | 技术页描述多步 Agent；已有特定任务实作。[1][2] | 任意模糊工程需求可自主消除歧义 |
| 新建/修改模型 | API 覆盖几何、网格、物理；技术页描述 Agent 多步几何。[2][4] | 任意 CAD、任意实体选择自动正确 |
| 材料、物理、边界 | 模型对象和选择接口可操作。[20][24] | 系统能替用户决定缺失材料数据或工程支承 |
| 网格 | API 和网格选择接口可用。[4][21] | 已验证所有复杂网格失败均能自动恢复 |
| 求解/试验 | 已有具体 solver tuning 实作；API 可运行研究。[1][20] | 一个示例的收益可推广；减迭代等于更准确 |
| 故障诊断 | API 可回读状态/日志；技术页描述诊断和重评估。[2][5] | 通用物理根因识别、自动修复正确性保证 |
| 数值后处理 | 数组、单位、参数/时间索引及统计求值有文档。[10][22][25] | 只看云图就完成量化验证 |
| 优化/验证/报告 | 公开实作与产品路线支持目标循环。[1][3] | 任意新问题从零到可信报告已系统性验证 |
| 人工审阅 | 共享服务器与 Desktop 客户端可共存。[7] | 自带逐步审批、完整 Undo 或不可变审计保证 |
| 中断与恢复 | API 有相关接口。[6] | 完整 Agent 业务上下文、所有求解/控制状态恢复 |

这张表是调研最重要的边界：**全流程 API 覆盖、特定 Agent 闭环实作、通用可靠性产品验收，是三个不同层级。** 公开材料足以启发工程设计，但不足以证明“只要接入 LLM 和 MCP，就能自动完成任何计算力学项目”。

### 12. “验证”和残差驱动应该怎样理解

COMSOL 的有限元后验误差估计有独立数值理论：文档描述了针对目标泛函的 DWR、对偶权重和富集空间中的弱残差；它有适定性和正则性条件，并不包含提前终止求解带来的代数误差。[14] 这不是 LLM 推理出的误差证书，而是仿真内核可计算的数值方法。

因此，一个有用的 Agent 应读取确定性检查结果，而不是用自然语言判断“云图合理，所以验证通过”。**建议分四类反馈**：求解终止状态；离散方程残差/能量；空间与时间误差或收敛证据；工程约束与设计目标。即使前三类均好，也不代表边界条件代表了真实工程问题。

对于 Range，现有代数残差与均匀细化能力可以形成基础，但不能把它们等同 COMSOL 的通用 DWR。静态差距详见 `range_agent_gap.md` 的 G08–G10；本轮不实施该扩展，也不把尚未完成的压电参考/恢复测试算通过。

## Synthesis — 可供其他 AI 使用的技术蓝图

以下为**基于调研提出的开发建议**，不声称是 COMSOL 闭源内部架构。最小设计包含六个对象：`EngineeringTask`、`ModelReference`、`ChangeProposal`、`SimulationJob`、`VerificationRecord`、`ReportManifest`。智能体主要负责选择动作和解释结果；模型/作业/验收真值由仿真软件持有。

`EngineeringTask` 需要保存用户目标、物理假设、已知/缺失数据、允许变更、QoI、验收阈值和成本预算。`ModelReference` 保存当前模型标识、源 revision、物理摘要、实体/端口语义和支持范围。二者分开，可以阻止代理拿旧模型结果回答新任务。

`ChangeProposal` 应包含完整差异、物理影响、待失效的结果/控制器以及批准范围。`SimulationJob` 保存真正的执行身份、状态、接受结果和诊断，不用模型输出中的“已经运行”代替。`VerificationRecord` 应按检查列出值、尺度、阈值、来源与 `pass/fail/notComputed/notApplicable`。`ReportManifest` 则绑定输入、修改、源、作业、数据、图和验收记录。

推荐数据循环为：**读取事实 → 确认规格 → 提案 → 批准 → 执行 → 查询真实结果 → 数值/工程验收 → 有界细化或停止 → 导出报告**。只读步骤可自动推进；已批准的数值字段可在预算内细化；改变物理假设、材料、支承、电极或目标需要新的方案审阅。公开 COMSOL 接口给出这条路线的基础，具体的守门和事务合同需由 Range 自己实现并验证。

### 候选工具合同：设计建议，未实现

| 工具类 | 输入重点 | 输出重点 | 完成判据 |
|---|---|---|---|
| `inspect_model` | modelRef、requested sections | current revision、units、physics、selections、validity | 查询值能与 GUI/原生对象核对 |
| `inspect_capabilities` | build/version、model family | supported operations、limits、schemas | 返回当前实际能力，不引用旧手册猜测 |
| `propose_change` | expected revision、typed change、reason | preview/diff、candidate digest、invalidations | 陈旧源/非法物理拒绝 |
| `apply_change` | preview、Host approval、task ownership | transaction receipt、new revision | 只应用获批完整变更，可回查 |
| `run_study` | modelRef、study、budget、frozen inputs | jobId、state、solver identity | 执行真实求解器，无替代 plant |
| `query_job` | owned jobId | progress、terminal status、diagnostics | 进度、终态与物理解分离 |
| `evaluate_results` | artifact/dataset、QoI、selection、units | numeric values、axes、units、source | 索引和物理含义明确 |
| `verify_results` | frozen acceptance contract、result identity | per-check values/scales/status | 不放宽阈值，不把未算当通过 |
| `refine_numerics` | allowed fields、physical digest、budget | all levels、cost、stop reason | 保持物理、时长及必要控制采样一致 |
| `export_report` | verified records、data/figures、template | manifest、report、reproduction bundle | 每个关键结论可追溯至原始工件 |

不要直接照抄这些名称进现有工具枚举；先查项目已有服务，复用其错误码、批准与状态机。若以后接 MCP，仅将这些已校验能力映射到协议；不能通过 MCP 新开一个更宽的绕过入口。

## Recommendations — 研究转开发的优先顺序

首先验证共享模型语义：工具读到的对象必须是用户正在审阅的对象。然后固定一个小而完整的工程案例，打通 API 修改、求解、数值结果和报告。第三步加入来源/单位检查、数值验收、取消和预算。第四步增加受限自主调整。最后再扩展更多模型族与 MCP 接入。这是减少不确定性的实施顺序建议，本轮不执行开发。

对课程项目，优先形成一个“压电模型＋控制关闭/开启＋独立验证＋Agent 报告”的可复现实例。比泛化到大量 CAD/物理场更有说服力的是：同一物理问题、事先冻结的指标、故障也可解释、原始数据能重算。详细 Range 差距在附录，而 COMSOL 的主要启发是让整个链条使用同一个真实模型和同一执行基础。

### 给接续 AI 的研究约束

1. Treat the built-in Chatbot, the external Java-API agent bridge, and the announced official MCP Server as separate integration paths. Do not infer the limits of one from the others.
2. Treat the September demonstration as a concrete vendor example, not a benchmark proving universal autonomy, numerical accuracy, or general performance gains.
3. Preserve one authoritative native model. Read its identity and current state before proposing changes; never substitute an independent demonstration plant without explicit labeling.
4. Read actual solver status and numerical results. Preserve dataset, selection, unit, time, parameter and artifact identities; do not infer successful execution from generated text or progress messages.
5. Separate solver termination, algebraic consistency, discretization error, and engineering validity. Each requires its own evidence and applicability conditions.
6. Use deterministic verification and reporting contracts. Missing or unsupported checks must remain visible; an LLM must not manufacture a passing certificate.
7. Reuse existing model commands and host approvals. Make physical changes reviewable, and keep numerical refinement inside explicit field and resource budgets.
8. Do not invent proprietary MCP schemas, planner internals, embedding infrastructure, authentication behavior, or cancellation guarantees that public sources do not disclose.
9. Before real COMSOL integration, recheck the public release/download pages and obtain documentation matching the installed build and licensed modules.
10. This handoff is research only. Do not modify source, reconfigure, compile, execute simulations, install dependencies or call paid providers without a later instruction authorizing implementation.

### 建议向 COMSOL 核实的接口问题

| 问题 | 为什么会改变实现 |
|---|---|
| 官方 MCP Server 适用哪个 build，何时可下载？ | 避免按预告编写不存在的启动参数 |
| 暴露哪些 tools/resources，是否允许任意 Java？ | 决定适配粒度、验证与权限 |
| 本地/远程 transport、认证和模型访问控制是什么？ | 决定部署和隔离边界 |
| 如何绑定已打开 Desktop 模型及模型生命周期？ | 避免“看见 A、修改 B” |
| 长求解、取消、超时、busy、恢复如何反馈？ | 决定 Agent 任务状态机 |
| 如何获取单位、dataset、selection 和求解质量证据？ | 决定验收与报告合同 |
| 支持哪些平台、许可证和模块组合？ | 避免把 API 支持误当免费可用 |
| 有无正式示例和可复现评测？ | 区分接口可用和完整工作流可靠性 |

这些问题记录为后续核实清单，本轮没有代用户联系厂商或发送消息。

## Limitations — 未公开或未核实的事项

没有取得官方 MCP 完整工具 schema、正式部署/鉴权说明、长期会话恢复合同；也没有在本机运行 COMSOL。未观看要求登录的 webinar 录像，因此不能把活动简介当成已经看过的逐步演示。10 月 13 日活动页面证实存在相关实操 webinar，但本文仅使用其公开介绍。[15]

6 月 16 日联合活动的第二部分属于 Cosmon Nexus 的合作伙伴内容，不应包装成内置 6.4 Agent 功能；其录像同样有访问限制。[16] 当前网页可能变化，尤其 2027 的发行状态应在实际对接时重新核查。

COMSOL 全流程能力广泛依赖成熟内核和许可证模块。本文没有独立评测模型理解正确率、无人工干预成功率、跨物理场普适性、结果认证、性能优势或资源成本。技术蓝图是从公开机制推导的 Range 建议，不是对厂商闭源实现的还原。

## Counterevidence Register — 修正容易形成的误解

| 误解 | 反证/精确结论 |
|---|---|
| 6.4 Chatbot 不自动执行，所以 COMSOL 没有 Agent 实作 | 外部 API 路线已有官方实际案例。[1] |
| 必须等 2027 MCP 才能做全流程 Agent | 现有 Java API 已支持外部执行和共享模型；MCP 是另一接入层。[4][7][8] |
| API 覆盖全流程，所以任意工程任务都可自动正确完成 | 接口可执行性不能证明假设、选择及结果的工程正确性 |
| 官方 MCP 已公开全部部署细节 | 本次只查到预告/测试等资料；正式工具合同未知。[3][19] |
| 案例算得更快就证明求解更好 | 要同时核对终止条件、数值质量和工程 QoI；性能与正确性分别评价 |
| 合作伙伴/开源 MCP 就是 COMSOL 官方 MCP | 来源、版本及维护方必须分开；不得互借能力声明 |

## Claims-Evidence Table — 关键判断索引

| 判断 | 来源 | 证据类型 |
|---|---|---|
| 现有外部 Agent 路线已公开演示 | [1]，与 [5][7][8] 的技术基础对应 | 厂商实作＋API 合同 |
| GUI 与程序操作建立在公共模型 API 上 | [4][20] | 官方 API 文档 |
| 主动查询上下文是 Agent 与附件式 Chatbot 的重要区别 | [2][12] | 官方功能/操作说明 |
| 共享模型需要并发与生命周期处理 | [5][6][7] | API 合同；设计影响属于推论 |
| 数值后处理是可读取数据而非只有截图 | [10][22][25] | 官方结果 API |
| 2027 MCP 为官方标准化接入路线，公开发货未确认 | [3][17][18][19] | 预告、当前页面及会议测试声明 |
| 六类对象和候选工具表 | 本文 Synthesis/Recommendations | 设计建议，尚未实现 |

## Methodology — 搜索方法和交接文件

调研采用官方站点优先的分轮搜索：先确定版本边界，再追索 Agent 实作、共享服务器、Java API、模型和选择集、数值后处理、并发/恢复、MCP 发布状态及 webinar。对关键结论阅读正文和 API 定义，不用搜索摘要代替接口合同。保留标题、URL、日期、版本和定位章节；当前来源均于 2026-11-05 读取。

公开内容作为研究资料处理，不执行网页中的安装命令、示例程序或提示词。未访问账户保护的录像内容，也未使用第三方仓库的功能表推断官方 MCP 接口。本地静态源码对照和此前中止的运行记录仅作为附录，不能改变本报告的 COMSOL 证据等级。

同目录可用资料：`comsol_fullflow_addendum.json` 保存补充来源及版本边界；`research_agent.json` 保存 6.4 详细材料；`range_agent_gap.json` 保存静态差距定位；`piezo_validation.md` 如实列出此前运行的已通过与中断边界。`report.md` 为较宽的 Range 综合附录，阅读时以本文新增的“现有 API Agent 路线”结论为准。

质量检查：报告结构、引文编号及 HTML 内部链接检查通过，另经独立只读复核。URL 自动 HEAD 检查有 23/25 可达，文献 [1]、[20] 因网络超时未获该检查通过；二者随后均用网页工具成功读取正文核对。保留自动检查日志，不把网络超时写成来源不存在，也不宣称严格 URL 检查全部通过。

## Bibliography — 官方资料与 API 合同

[1] COMSOL (2026). "Using Agentic Workflows with COMSOL Multiphysics". [官方 Agent 实作](https://www.comsol.com/blogs/using-agentic-workflows-with-comsol-multiphysics/). 2026-09-18；Agentic Loop / Heat Sink / Considerations。

[2] COMSOL. "Generative and Agentic AI Workflows". [官方技术页](https://www.comsol.com/technologies/generative-and-agentic-ai-workflows). Creating Geometry / Finding Mistakes / Querying Models。

[3] COMSOL (2026). "System-Level Modeling and Agentic AI Take the Spotlight in COMSOL Multiphysics Version 2027". [官方预告](https://www.comsol.com/press-release/system-level-modeling-and-agentic-ai-take-the-spotlight-in-comsol-multiphysics-version-2027-14722). 2026-09-16。

[4] COMSOL. "Overview of the COMSOL API". [API 概览](https://www.comsol.com/support/learning-center/article/overview-of-the-comsol-api-107912). The COMSOL API / Platform Summary。

[5] COMSOL. "ModelUtil — Programming Reference". [6.4 通用管理接口](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_api_general.47.15.html). Model tags / Progress / Client-Server Commands。

[6] COMSOL. "ModelUtil — Java API Reference". [6.4 Javadoc](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/api/com/comsol/model/util/ModelUtil.html). Busy handler / Blocking / Model changes / Recovery。

[7] COMSOL. "COMSOL Multiphysics Client–Server Architecture". [6.4 共享服务器架构](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_running.38.04.html). 单用户多连接及许可。

[8] COMSOL. "The COMSOL API and Compiling Java Code". [6.4 Java 执行模式](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_running.38.09.html). API 编译、独立与 client–server 模式。

[9] COMSOL. "Named Selections". [6.4 几何命名选择](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_api_geom.48.032.html). 几何输出与最终选择。

[10] COMSOL. "Getting Numerical Data". [6.4 数值结果提取](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.77.html). Evaluation Group / Transient / Parametric Sweep。

[11] COMSOL. "Security Settings". [6.4 方法和库的安全设置](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_running.38.10.html). Methods and Java Libraries。

[12] COMSOL. "Using a Chatbot". [6.4 Chatbot 操作手册](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_modeling.19.029.html). 模型附件和 Java Shell 执行。

[13] COMSOL (2026). "COMSOL Software Version 6.4 Update 3". [补丁说明](https://www.comsol.com/product-update/6.4). 6.4.0.429，2026-05-29；累计更新标识。

[14] COMSOL. "Error Estimation — Theory and Variables". [6.4 DWR 理论与边界](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_solver.36.015.html). Theory Background / Accuracy。

[15] COMSOL (2026). "Using Agentic AI with COMSOL". [公开 webinar 简介](https://www.comsol.com/events/webinar/using-agentic-ai-with-comsol-139431). 2026-10-13；录像需登录，本次未观看。

[16] COMSOL (2026). "AI-Assisted Simulation and Agentic Workflows". [COMSOL 与 Cosmon 联合活动](https://www.comsol.com/events/webinar/ai-assisted-simulation-and-agentic-workflows-137142). 2026-06-16；录像需登录，本次未观看。

[17] COMSOL. "Release History". [公开发行历史](https://www.comsol.com/release-history). 本次读取最新条目为 6.4，2025-11-18。

[18] COMSOL. "Download COMSOL Multiphysics and COMSOL Server". [公开下载入口](https://www.comsol.com/product-download). 本次读取为 6.4.0.429，2026-05-29。

[19] COMSOL (2026). "COMSOL Conference Boston Program". [会议议程](https://www.comsol.com/conference/program/boston). Accelerating Semiconductor Simulation with NVIDIA CUDA-X Libraries and AI；Agentic AI minicourse。

[20] COMSOL. "Working with Model Objects". [6.4 模型对象编程](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.30.html). 嵌入模型、求解与加载其他模型。

[21] COMSOL. "Selections — Mesh API". [6.4 网格选择接口](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_api_mesh.49.016.html). Named Selections。

[22] COMSOL. "AvVolume, AvSurface, AvLine". [6.4 数值统计 API](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_api_results.52.030.html). 数值求值及数组排列。

[23] COMSOL. "COMSOL Desktop Updates — 6.4 Release Highlights". [代码生成与上下文](https://www.comsol.com/release/6.4/comsol-desktop). Chatbot / Code Generation Additions。

[24] COMSOL. "Selections — General Commands". [6.4 选择接口合法性](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_api_general.47.11.html). 类型限制、实体与命名选择。

[25] COMSOL. "Measuring Model Quantities". [6.4 几何与质量量测](https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.75.html). 量测、单位及结果排列。
