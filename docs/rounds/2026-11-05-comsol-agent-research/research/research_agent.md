# COMSOL 6.4 官方技术路线核查

研究日期：2026-11-05。只读官方公开资料，无账号操作或付费API调用。每个来源仅保留一段不超过25英文词的摘录，其余为中文释义。引用位置为本次网页工具正文行号与章节名；网页更新后优先用章节定位。

## 可写入报告的结论（事实）

- 6.4的官方路线是带模型上下文与文档工具的代码生成/咨询助手 + 用户触发的Java Shell执行；模型修改、求解、结果处理能力来自COMSOL API执行层。
- 6.4具备function/tool calling不等于具备自动修改模型工具：已公开手册明确该选项用途为文档浏览；没有在上述6.4来源发现自主建模/求解/结果反馈agent工具清单。
- 2027预告明确引入MCP外部agent闭环；不可将此能力倒灌为6.4现有功能。

## 对 Range FEA 的推断与建议

- 优先建立可靠模型API/命令层和可录制动作，给LLM提供选中节点/子树/当前状态的序列化上下文与可运行范例。
- 将咨询、建议代码和执行命令分层；第一阶段保留用户执行，再逐步加入可验证的结构化工具、执行回执、错误归因和取消。
- 全流程闭环需要显式把求解状态、数值结果、单位和工程验收条件回传；仅展示聊天和Shell不能等同已具备自主仿真agent。
- Model Manager式版本化适合作为模型/结果追溯基础；它与LLM记忆或规划机制不是同一个概念。

## 不确定性

- COMSOL内部提示词、检索索引/embedding、RAG具体实现、planner、记忆结构、工具schema、上下文自动选择算法：公开材料未披露，unknown。
- 6.4 Chatbot API key的磁盘加密、云端保留/训练政策、COMSOL是否另存传输数据：上述资料未充分披露，不能由key UI隐藏或自托管选项推出隐私保证。
- 6.4安全设置对Java Shell的全部实际限制需要安装环境测试；官方该页主要描述方法/Java库。
- 2027正式发布日期和MCP部署/安全配置未在本次6.4研究中核实；预告不能当作已交付接口合同。
- 在线6.4文档会随补丁更新；涉及实际LLM型号或API后端时应锁定具体build。

## 逐项证据

### C01 COMSOL Speeds Simulation with Expanded NVIDIA GPU Support for COMSOL Multiphysics Version 6.4

- URL：https://www.comsol.com/press-release/comsol-speeds-simulation-with-expanded-nvidia-gpu-support-for-comsol-multiphysics-version-64-14612
- 版本：6.4
- 发布/更新日期：2025-11-18
- 定位：新闻稿首段；web turn5view4 L47–52
- 短摘录：today announced the release
- 核实事实：6.4于2025-11-18正式发布。
- 核实事实：Chatbot连接外部LLM，结合文档与当前仿真信息提供模型相关辅助。

### C02 COMSOL Desktop Updates — 6.4 Release Highlights

- URL：https://www.comsol.com/release/6.4/comsol-desktop
- 版本：6.4
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Chatbot Window；Code Generation Additions；web turn1view3 L115–140
- 短摘录：Record Code
- 核实事实：新增文档搜索、API知识和节点/整个模型/文字/图像附件；提供OpenAI、Azure及OpenAI兼容供应商，包括自托管。
- 核实事实：仅Windows。新增录制代码至Java Shell，以及复制节点全部显示设置的代码。

### C03 Using a Chatbot

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_modeling.19.029.html
- 版本：6.4在线参考手册，未绑定补丁build
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Model Builder Nodes / Model History / Running Code in the Java Shell Window / Chatbot Preference Settings；web turn1view0 L30–63, L75–103
- 短摘录：Click the Run button
- 核实事实：节点或节点及子节点以Java代码作为附件；模型历史可选择压缩为当前状态，或包含已废弃设置的完整历史。
- 核实事实：可附文字、CSV、PNG、JPEG、GIF与图形窗口快照；附件可查看详情。
- 核实事实：代码送至Java Shell多行输入后点击Run；运行错误手动Send to Chatbot，然后再送回Shell运行。
- 核实事实：供应商为None/OpenAI/Azure/OpenAI兼容；兼容服务配置Base URL、模型ID、上下文长度和Tool calling；key显示为隐藏文字，无需鉴权时可空。
- 核实事实：默认对话截断阈值100000 token、用户消息5000、存储会话20（上限100）；未披露是否安全加密落盘。

### C04 The Chatbot Window — Application Programming Guide

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.14.html
- 版本：6.4在线手册
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：首部、Tool calling、annotation示例；web turn1view1 L1–20, turn6view6 L21–84
- 短摘录：allows the Chatbot to browse the COMSOL documentation
- 核实事实：Tool calling文档明确用途为浏览COMSOL文档。
- 核实事实：General/Programming/Modeling影响提示上下文；聊天理解Java语法但不掌握全部COMSOL API。
- 核实事实：示例录制一个注释节点，将正确API代码交给LLM扩展为循环，再送Shell由用户Run。
- 核实事实：LLM由外部供应商提供，API协议和费用由用户承担；建议适用性须由用户判断。

### C05 The Java Shell and Data Viewer Windows

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.13.html
- 版本：6.4在线手册
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：首段、工具栏、session说明；web turn1view2 L1–40, L64–70
- 短摘录：no content is saved between sessions
- 核实事实：Shell可直接经API修改模型、批量创建特征、检查属性和调试方法；可Run/Stop/录制。
- 核实事实：模型操作以model对象开始，提供补全和对应树节点跳转。
- 核实事实：打开或新建模型重置Shell会话，会话内容不保存；持久化需改model并保存。
- 核实事实：stdout/stderr显示于Shell，方法编辑器运行时进入Debug Log；错误可送回Chatbot。

### C06 New and Improved General Functionality in COMSOL Multiphysics

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_release_text.06.007.html
- 版本：6.4发布说明
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Chatbot；web turn1view4 L9–29
- 短摘录：built-in tools for searching the COMSOL documentation
- 核实事实：Windows默认安装Chatbot组件，可以安装时取消。
- 核实事实：6.4新增OpenAI兼容provider、GPT-5系列、新附件类型与内置文档搜索工具。

### C07 COMSOL Software Version 6.4 Update 3

- URL：https://www.comsol.com/product-update/6.4
- 版本：6.4.0.429 / Update 3（累计包含U1/U2）
- 发布/更新日期：2026-05-29
- 定位：页首；The Chatbot Window；脚注1/2/3；web turn6view5 L44–50,L678–682; turn6view4 L458–462; turn1view5 L486
- 短摘录：The OpenAI Responses API is now used
- 核实事实：U1=6.4.0.343（2026-01-29），U2=378（2026-03-19），U3=429（2026-05-29）。
- 核实事实：U1将OpenAI/Azure连接改用Responses API并修复Gemini 3兼容问题；U2将GPT-5 Low/High选项指向GPT-5.4，旧版本需Custom。
- 核实事实：U3增加带注释的模型Java文件供Documentation窗口与Chatbot搜索具体API示例。

### C08 Model Manager Updates — 6.4 Release Highlights

- URL：https://www.comsol.com/release/6.4/model-manager
- 版本：6.4
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Study Support for Batches and Clusters；web turn5view9 L140–145
- 短摘录：Study Support for Batches and Clusters
- 核实事实：批处理和集群研究可直接使用Model Manager数据库中的模型与数据；输出可存入同一版本管理库。
- 核实事实：支持本地库与Model Manager Server服务库；此为数据/作业基础设施能力，页面未将其描述为LLM规划或记忆。

### C09 Security Settings

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/comsol_ref_running.38.10.html
- 版本：6.4
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Methods and Java Libraries / Internet；web turn8view3 L2–43
- 短摘录：The Enforce security restrictions checkbox is selected by default
- 核实事实：方法与Java库默认启用安全限制；文件默认限临时与应用文件，可单独放行系统属性、外部进程、socket、反射、Model Manager读写等。
- 核实事实：安装永久策略会锁定用户安全设置；清除总开关将完全关闭方法/库安全限制。
- 核实事实：互联网不受信模型可触发临时收紧策略；该互联网功能在Linux不可用。
- 核实事实：这些限制明确主要面向Application Builder方法/外部库；本页没有证明其全部同样适用于Java Shell，也不是Chatbot数据脱敏说明。

### C10 Working with Model Objects

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.30.html
- 版本：6.4
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：embedded model说明；模型构建代码；Loading Another Model；web turn5view1 L1, L363–369
- 短摘录：run a study, and extract numerical results
- 核实事实：Model Builder有自动创建的嵌入model对象，自动代码生成针对该对象；MPH保存/加载也是该对象。
- 核实事实：API可操作几何、网格、研究和结果；官方示例含求解器runAll和绘图run。
- 核实事实：加载其他模型后仍可用API改参数、求解并提取结果，但常规应用图形显示功能默认针对嵌入模型，其他模型绘图需专门代码。

### C11 Getting Numerical Data

- URL：https://doc.comsol.com/6.4/doc/com.comsol.help.comsol/application_programming_guide.15.77.html
- 版本：6.4
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Evaluating Quantities For a Parametric Sweep / Computing the Average Along a Boundary / Retrieving Data from a Transient Simulation；web turn5view2 L472–484,L847–859,L1239–1316
- 短摘录：Retrieving Data from a Transient Simulation
- 核实事实：官方API示例覆盖参数扫描和瞬态解的数值结果提取，求值组run之后用getReal取二维数组并获取单位。
- 核实事实：这些证明执行API的后处理能力，不证明6.4 Chatbot自动读取结果并自主决策。

### C12 Application Builder Updates — 6.3 Release Highlights

- URL：https://www.comsol.com/release/6.3/application-builder
- 版本：6.3
- 发布/更新日期：页面未标日期；按6.4版本路径或版本发布说明定位
- 定位：Chatbot / Java Shell；web turn1view7 L113–129
- 短摘录：A new Chatbot window is available
- 核实事实：6.3已引入Chatbot与Java Shell：LLM生成/调试Java API代码，Shell交互修改模型。
- 核实事实：6.4不能被写成Chatbot或Java Shell首次出现的版本。

### C13 COMSOL Releases Version 6.3 of COMSOL Multiphysics

- URL：https://www.comsol.com/press-release/comsol-releases-version-63-of-comsol-multiphysics-14481
- 版本：6.3
- 发布/更新日期：2024-11-19
- 定位：首段及下一段；web turn6view3 L50–52
- 短摘录：November 19, 2024
- 核实事实：6.3正式发布日期2024-11-19；新闻稿将交互Java修改模型与聊天编码/咨询辅助分别描述。

### C14 System-Level Modeling and Agentic AI Take the Spotlight in COMSOL Multiphysics Version 2027

- URL：https://www.comsol.com/press-release/system-level-modeling-and-agentic-ai-take-the-spotlight-in-comsol-multiphysics-version-2027-14722
- 版本：2027预告，非6.4
- 发布/更新日期：2026-09-16
- 定位：首段与 Advancing AI-Assisted Simulation with Agentic Workflows；web turn6view0 L49–51; turn8view1 L64–68
- 短摘录：An agent can build and/or modify a model
- 核实事实：这是2026-09-16对当年秋季将推出2027版本的预告，单凭此页不能认定正式上市日期。
- 核实事实：官方明确将上一版本Chatbot生成式咨询与新2027 MCP Server区分。
- 核实事实：预告MCP使外部agent直接经COMSOL API建模/改模型、运行仿真、检查结果并据此决定后续步骤，用户可在Desktop审阅。
- 核实事实：此页未命名Chat Assistant产品，也未披露MCP认证、权限、传输或工具schema。

### C15 Using the Chatbot Window in COMSOL Multiphysics

- URL：https://ws-bos.comsol.com/blogs/using-the-chatbot-window-in-comsol-multiphysics
- 版本：同时说明6.3/6.4，已于2026更新
- 发布/更新日期：2025-06-12；更新2026-02-27
- 定位：编辑注；Step 1；Create a Geometry；Debug Code；Find Model Setup Mistakes；web turn8view0 L56–73,L98–108,L214,L225,L267
- 短摘录：but it will not run the code
- 核实事实：LLM不安装在COMSOL内部，窗口只负责与LLM通信；可连接云端或本地部署模型。
- 核实事实：官方明确示例生成圆柱Java代码不会自动运行，需送Shell再Run。
- 核实事实：文档搜索自6.4可用；模型上下文不足会产生误报，官方示例也指出部分建议无效。
- 核实事实：博客初始日期早于6.4，但正文已2026-02-27更新，不能把整页当成6.3原始行为证据。
