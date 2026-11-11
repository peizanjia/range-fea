# COMSOL 全流程 Agent 补充核查

日期：2026-11-05。仅公开资料只读研究；未安装、编译、执行COMSOL示例，未访问账号。此补充修正只研究内置Chatbot可能带来的范围误解：**内置Chatbot手动执行不等于COMSOL不能运行外部智能体。官方已经演示API驱动的闭环；2027 MCP是新增的标准化接口。**

## 交付状态

截至本次公开资料读取，2027和MCP已预告且有厂商内部测试声明，但公开首页/发布历史/下载入口仍指向待发布状态；未能确认正式公开交付。这个结论不否定现有COMSOL API可运行agent闭环，官方9/18已有实作演示。

## 能力与机制边界

- 6.4内置Chatbot仍是用户附上下文、生成代码、Shell手动执行；COMSOL API外部agent路线可以另外存在。
- 2027 MCP是标准化接入的新路线；现有API agent demo不是MCP实现。
- 官方宣称Desktop可审阅，不等于协议有逐次授权、事务回滚或权限隔离。

## 公开资料未解决的接口问题

- 官方2027 MCP可用工具schema、API映射粒度、resources/prompts、协议版本、stdio/HTTP/SSE、认证、会话生命周期、并发、取消/超时/失败恢复。
- MCP是否内置基础许可、需额外模块或FNL、允许何种远程部署/平台/客户端，公开已读材料未给明确合同。
- 官方MCP是否自动全模型上下文、按需读取或截断，公开材料未说明。
- 正式发货日期：未找到公开确认；不能用未来webinar或营销口吻断言已上市。

## 后续研究可操作路线

- 可操作现有路线以F07官方实作/API服务器为准；不依赖尚未证实交付的MCP。
- 后续核实2027时应复查F02/F03/F04，并取得正式release highlights、安装指南与MCP tool schema后再规划集成。
- 第三方仓库只能单列；需要另行固定commit、审查代码和实测，不能将其接口冒充官方。

## 来源登记

每来源保留短摘录不超过25英文词，其他为释义。正文定位含章节与本次web读取行号；动态页面可能改变。官方页面的未检出只能证明本次检索范围内未发现，不能当成不存在证明。

### F01 System-Level Modeling and Agentic AI Take the Spotlight in COMSOL Multiphysics Version 2027

- 类型：official_preview
- URL：https://www.comsol.com/press-release/system-level-modeling-and-agentic-ai-take-the-spotlight-in-comsol-multiphysics-version-2027-14722
- 版本：2027预告
- 日期：2026-09-16
- 定位：Advancing AI-Assisted Simulation with Agentic Workflows；web turn8view1 L64–68
- 短摘录：An agent can build and/or modify a model
- 支持：MCP标准化接口让外部agent经COMSOL API建改模型、运行、检查结果、据结果决定下一步；用户可在Desktop审阅和改进。
- 不支持：正式交付日期；工具名/schema；stdio或HTTP；认证/审批/回滚；跨平台/模块许可证细则。

### F02 COMSOL - Software for Multiphysics Simulation

- 类型：official_current_status
- URL：https://www.comsol.com/
- 版本：动态首页，2027倒计时
- 日期：读取2026-11-05；页面无单独发布日期
- 定位：v2027 banner；web turn19view4 L77–81
- 短摘录：more are coming in v2027
- 支持：当前读取内容仍写倒计时与2027能力即将到来。
- 不支持：精确正式发布日期；私有测试版是否存在；任何MCP协议实现细节。

### F03 Download COMSOL Multiphysics simulation software and COMSOL Server here

- 类型：official_current_status
- URL：https://www.comsol.com/product-download
- 版本：公开下载入口6.4.0.429
- 日期：页面版本日期2026-05-29；读取2026-11-05
- 定位：标题与版本选择；web turn20view0 L44–48,L126–135
- 短摘录：Version 6.4.0.429, May 29, 2026
- 支持：无需登录可见的当前下载版本仍为6.4.0.429；版本列表未列2027；实际下载要求有效订阅许可证或试用关联账号。
- 不支持：登录后特殊授权是否可见2027；MCP Server许可条款；2027已正式出货。

### F04 Release History

- 类型：official_current_status
- URL：https://www.comsol.com/release-history
- 版本：官方发布历史
- 日期：读取2026-11-05
- 定位：版本表首行；web turn16view1 L47–54
- 短摘录：6.4
- 支持：正式发布历史表首行仍6.4于2025-11-18，下一行为6.3于2024-11-19。
- 不支持：单页未列出并非逻辑上证明绝无新版本；须与首页/下载/预告合读。

### F05 Introducing COMSOL Multiphysics Version 2027

- 类型：official_future_event
- URL：https://www.comsol.com/events/webinar/introducing-comsol-multiphysics-version-2027-140322
- 版本：2027介绍webinar
- 日期：活动2026-12-10 11:00–12:00 EST（研究日期之后）
- 定位：页面标题、首部介绍、Webinar Details；web turn16view2 L44–50,L123–129；turn15view3 L51
- 短摘录：December 10, 2026
- 支持：将介绍2027和MCP Server的webinar已公布，活动时间晚于研究日期。
- 不支持：已观看录像；已经完成演示；MCP已交付；工具参数或性能基准。

### F06 Program for the COMSOL Conference 2026 Boston

- 类型：official_conference_program
- URL：https://www.comsol.com/conference/program/boston
- 版本：2026会议；MCP仍称upcoming
- 日期：会议2026-10-07至09；页面无独立发布日期
- 定位：Accelerating Semiconductor Simulation with NVIDIA CUDA-X Libraries and AI；Generative and Agentic AI with COMSOL；web turn22view2 L449–465
- 短摘录：upcoming COMSOL MCP Server
- 支持：官方议程称MCP Server即将推出，并称COMSOL已用NVIDIA Nemotron Ultra成功测试AI辅助建模工作流。
- 支持：同页短课描述Java API让agent创建、运行、检查、评估模型。
- 不支持：Nemotron测试可复现脚本/参数/结果；MCP只有该模型可用；已交付版本；已公开客户端配置。

### F07 Using Agentic Workflows with COMSOL Multiphysics

- 类型：official_mechanism_demo
- URL：https://www.comsol.com/blogs/using-agentic-workflows-with-comsol-multiphysics/
- 版本：实际API agent路线；正文未明确锁定COMSOL版本build
- 日期：2026-09-18
- 定位：Understanding Interactions / How to Connect / Heat Sink Model / Considerations；web turn22view1 L65–94,L110–162
- 短摘录：Running nine experiments
- 支持：示范Codex通过skill/wrapper编译Java程序连接共享的Multiphysics服务器模型，查询、修改、求解后断开，读取输出继续规划。
- 支持：公开散热器求解器调参示例：9次实验；作者报告非线性迭代减少25%、作者称计算约快30%（未独立复现，未换算耗时降幅）。
- 支持：展示另一组压缩性选项比较和报告；强调检查物理量、选择集及长作业恢复。
- 不支持：这是2027 MCP实现；精确6.4补丁；通用30%加速保证；严谨独立基准；完整生产级超时恢复。

### F08 Generative and Agentic AI Workflows in COMSOL Multiphysics

- 类型：official_capability_overview
- URL：https://www.comsol.com/technologies/generative-and-agentic-ai-workflows
- 版本：未绑定具体版本的技术概览
- 日期：未标发布日期；读取2026-11-05
- 定位：首部 / The Chatbot Window / Creating Geometry Objects / Finding Modeling Mistakes；web turn22view0 L49–72
- 短摘录：An entire model, or selected parts of one, can be attached
- 支持：明确区分Chatbot生成/解释/调试代码与agent直接调用API完成多步骤；模型或局部可主动附到prompt。
- 支持：agent可创建组合几何、查模型结构、改参数/物理场/研究并评估结果；修改应按物理假设审阅。
- 不支持：MCP传输或工具定义；所有工作流都已经自动可靠完成；Chatbot默认自动读取全模型。

### F09 Program for the COMSOL Conference 2026 Cambridge

- 类型：official_conference_program
- URL：https://www.comsol.com/conference/program/cambridge
- 版本：2026会议/2027预览
- 日期：会议2026-09-23至25
- 定位：Introducing COMSOL Multiphysics Version 2027 / Generative and Agentic AI with COMSOL Multiphysics；web turn20view4 L165,L302–303,L675
- 短摘录：Introducing COMSOL
- 支持：官方会议已安排2027预览及生成式/智能体AI课程；可作为后续索取讲义和录像的具体线索。
- 不支持：本次实际观看课程或录像；MCP schema或正式交付。

### F10 Ching-Chiang/comsol-mcp README

- 类型：third_party_repository
- URL：https://github.com/Ching-Chiang/comsol-mcp/blob/main/README.md
- 版本：main可变分支；非COMSOL官方产品
- 日期：读取2026-11-05；未固定commit
- 定位：README 主要特性/环境要求/MCP配置/推荐工作流/未来聚焦；web turn23view0 L173–216,L245–285
- 短摘录：visible-main
- 支持：独立仓库声称共享服务器模型、50工具、主模型锁和快照；配置提供Python MCP host启动示例。
- 支持：README仍将材料、网格配置、完整结果导出列作后续重点；可作为机制参考线索。
- 不支持：COMSOL官方MCP工具集合；已经独立验证；跨平台支持；覆盖全流程的成熟度保证。

### F11 Cai-aa/CAE-Agent-Hub MCP/COMSOL README

- 类型：third_party_repository
- URL：https://github.com/Cai-aa/CAE-Agent-Hub/blob/main/MCP/COMSOL/README.md
- 版本：main可变分支；第三方
- 日期：读取2026-11-05；未固定commit
- 定位：首部/Contents/Preferred MPh Workflow/Codex Configuration；web turn23view1 L174–195,L215–260
- 短摘录：FastMCP stdio server
- 支持：明确独立MPh会话与Desktop共享模式不同；MPh经JPype/Java API，另有Java bridge备选。
- 支持：README明确FastMCP stdio和Codex配置，以及开模型/改参数/解算/评估/导出/保存工具。
- 不支持：这些stdio与工具名属于COMSOL2027官方接口；本次实际运行或验证可靠性；无COMSOL许可证即可仿真。

## 未成功读取的探测路径

- https://www.comsol.com/release/2027 — web open无法访问；不能据此单独推断404或不存在
- https://doc.comsol.com/2027/doc/com.comsol.help.comsol/html_COMSOLMultiphysicsManual.html — web open无法访问；未发现可读2027官方在线API/MCP手册
