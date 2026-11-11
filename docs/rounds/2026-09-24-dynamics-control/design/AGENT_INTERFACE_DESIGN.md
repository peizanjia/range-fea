# Agent 全流程接口设计与模块手册策略

2026-10-01 增量：已新增 `range-application-lib` 的受限模态配置编辑会话，当前可用命令与模型所有权边界见 `docs/APPLICATION_SERVICE.md`。下文通用 Command/Query/Agent 封套仍为目标设计，不是该首个服务的已开放接口；CLI/GUI 的受限接线不表示通用工作流 Agent 已交付。

状态：架构决策草案（2026-09-24）。本文件规定 Stage 6 控制协议与后续 Stage 9–12 GUI/Agent 集成的边界；所列未来命令不是当前已开放的 API。实际可调用能力必须以运行时 capability 查询和契约测试为准。

## 1. 结论：手册是导航，不是执行契约

为各模块编写类似 skill 的短手册是有价值的：它能教 Agent 何时使用几何、网格、材料、求解、后处理工具，以及各工具的物理假设和禁区。但仅靠 Markdown 让外部 API 直接操作软件不够可靠。文字会落后于代码，无法机器验证参数、单位、权限、版本和副作用，也不应让 Agent 按文档绕过 GUI 直接修改 `RModel`。

采用两层结构：

1. **可执行真源**：版本化 Command/Query/Capability/Artifact/InteractionRecord 契约；每个操作有机器可读 schema、前置条件、单位、坐标系、错误码、权限、副作用、幂等键、回滚语义和契约测试。GUI、CLI、Agent 和进程外客户端调用同一应用服务。运行时查询只返回当前模型、当前 profile 和已安装后端真正可用的能力。
2. **按需检索手册**：每阶段一份短 playbook，引用契约 ID 和实例，说明建模决策、验证方式、常见失败与安全边界。Agent 只读取当前阶段需要的片段，不一次塞入全仓库文档。手册不得声明 schema 或 capability 未暴露的操作已经实现。

当前可复用基础是 `range-model-lib` 的结构 capability registry、Stage 4 传感器/执行器稳定 ID 目录、Stage 5 的 CLI run-level `.events.jsonl`。这些还不是统一应用服务或 Agent 命令工具。`range-ai-lib` 的对话能力也不能当成已具备模型修改权限。

## 2. 统一操作封套（目标格式）

以下 JSON 是未来 Command 服务的**设计示例**，不是现有 CLI 的输入格式：

```json
{
  "apiVersion": "range.fea.commands/v1",
  "requestId": "uuid",
  "idempotencyKey": "caller-stable-key",
  "actor": {"type": "agent", "id": "assistant-instance"},
  "projectId": "project-id",
  "modelId": "model-id",
  "expectedModelRevision": "sha256-or-monotonic-revision",
  "workflowStage": "mesh",
  "commandId": "mesh.generate",
  "commandVersion": 1,
  "parameters": {},
  "dryRun": true
}
```

响应必须给出 `accepted/rejected`、新 revision、诊断代码、受影响对象、预计或实际工件、审计 record ID。服务端在应用命令前校验 revision、能力、单位、目标集合、拓扑约束和权限；高影响动作先 `dryRun`/预览，再由用户确认，随后用相同 revision 与幂等键提交。Agent 的自然语言建议、工具调用及用户批准均作为结构化记录，不用聊天文本代替。

Node 的当前 `nodeId` 是网格内索引，不是跨重网格永久 ID；命名集合或稳定实体 ID 也必须绑定 `modelRevision`。控制客户端取用的 DOF、传感器和执行器目录需要附 revision、单位、坐标系及有效期。若重网格或方程排序改变，旧目录应失效，不能默默把旧索引解释为新自由度。

## 3. 各阶段应暴露的查询与命令

| 阶段 | 必要查询 | 可授权命令 | 执行前核验 |
| --- | --- | --- | --- |
| Geometry/Assembly | 实体树、坐标系、命名集合、revision | 导入、创建、变换、装配关系 | 几何有效性、对象引用、撤销范围 |
| Property/Interaction | 单元兼容性、材料、截面、约束秩 | 指定材料、截面、约束、载荷 | 单位、DOF、约束冲突、目标集合 |
| Mesh | 后端能力、尺寸场、网格质量 | 生成/重建网格 | 后端可用性、质量门槛、旧 ID 失效及映射 |
| Analysis/Solve | 方法、时间步、线程、线性系统状态 | 配置分析、提交求解 | 稳定性、支持的组合、工件目录、计算资源 |
| Control | 传感器/执行器目录、接受态、协议版本 | 建立会话、提交控制命令 | 时序、限幅/速率、控制器状态、超时回退 |
| Results | 工件 hash、字段、误差/能量账 | 查询、比较、导出报告 | run ID、model revision、单位及来源 |

此表是接口规划，不表示所有命令已经实现。每个命令的 schema 应从实际应用服务定义生成，手册只引用 `commandId@version`。

## 4. Stage 6 控制协议与 Agent 工具的关系

控制器走低延迟、固定时步协议；Agent 走高层、可审查的应用命令协议。二者共用稳定 ID、单位、revision、能力和审计格式，但 Agent 不得在 CG/GMRES 内迭代或同一 trial 中临时改写材料、网格或控制矩阵。控制回调在接受态上调用一次，命令经验证后作为本步保持输入；结果写出成功、步被接受后才提交控制器状态与功账。进程外 Python/MATLAB 适配器首版目标为同机非硬实时，不承诺延迟上界；超时和协议错误必须拒绝或使用明确配置的安全输入。

对于全状态，控制平面可用小型版本化 JSON 消息，但不应每步序列化完整大向量。后续数据平面可用带 `shape/dtype/order/revision/lifetime` 的二进制切片或共享内存；未经生命周期和校验设计，不暴露裸指针或内部 Qt 对象。

## 5. 手册模板与发布门槛

每个模块手册只需覆盖：任务意图、可查询 capability、操作 ID、输入/输出示例、前置条件、单位/坐标、失败代码、验证算例、性能边界、不可用功能。每条操作必须关联可执行契约测试；schema 或实现改动时同一提交更新示例并运行测试。接口版本与模型文件版本分别管理，不能因为 C++ 头文件可访问就宣布外部协议稳定。

发布前最少验证：未知字段/版本拒绝、过期 revision 拒绝、重复幂等键、权限不足、无效单位/DOF、超时/断联、失败后回滚、同种子重放、审计事件与结果工件关联。Agent 二次开发只能通过受控扩展包、隔离构建、测试及差异审查发布；不在主 GUI 进程中即时编译并加载未经审查代码。
