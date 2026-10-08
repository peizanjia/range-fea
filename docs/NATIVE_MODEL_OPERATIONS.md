# 原生文档操作与求解接口

本文定义当前 GUI 原生文档与 Agent 操作边界。运行时 `capabilities.query` 和按文档返回的 Core capabilities 是支持矩阵的最终依据；文档中的计划名称不自动成为工具。WF-02 首批受限操作已完成本地验收；后续当前模型控制、统一时程和持久恢复须分别交付，任务进度见仓库的 [工作流交付状态](workflow_delivery_plan.json)。

文件 CLI 的参数、分页、SDK 与文件发布语义见 [Native Document CLI and Python SDK](NATIVE_DOCUMENT_SDK.md)。项目身份、保存快照与 LINK 来源见 [项目目录契约](PROJECT_CATALOG.md)。直接 Core 会话、事务与审计见 [应用服务](APPLICATION_SERVICE.md)。

## 1. 四个接口命名空间与对象所有权

| 接口 | 拥有对象 | 当前用途 | 不代表 |
| --- | --- | --- | --- |
| `range.host/v1` | GUI `Session` 中已经打开的文档及 GUI 主机 | 上下文、目标、Core 命令预览/执行、原生求解 job、结果文件独立打开 | 文件 CLI 会话、独立工作区模板、跨重启的持久 job 服务 |
| `range.application/v1` | 一个 `RModelCommandSession` 持有的模型副本 | 模态、结构动力、材料、边界候选与接受态 Undo/Redo | GUI 身份、磁盘保存、求解任务或运行时批准 |
| `range.workflow/v1` | 一个独立控制工作区 | 已验证四杆／平面梁模板的配置、FE 控制 job、续算、参考误差和接受步遥测 | 当前 GUI 打开的任意模型 |
| `range-model-command` | 命令行传入的 `.ram`/`.rbm` 或 MODEL/LINK 来源链 | 对一个文件快照做 Core 查询、预览或发布到另一个 native 文件 | GUI 文档、GUI job、模型文件求解器或工作区模板 |

GUI 和 Core 命令可能编辑同一组 native 参数，但身份与修订号不能互换。GUI 目标使用 `{documentId, sessionEpoch, documentRevision}`；Core session 使用随机 `modelId` 和只在该 session 内递增的 `revision`；工作区使用 `projectId`、`persistentDocumentId`、`sourceRevision`。文件 CLI 使用终端 native 文件的原始 `sourceSha256`，只读分页身份由 canonical 输入路径与该 SHA 派生。不要把文件路径、列表下标、Core `modelId`、GUI `documentId` 或 job ID 当成同一标识。

`range.workflow/v1` 的 `workflow.configure/run/resume`、job 与 telemetry 属于独立工作区。即使项目目录链接到某个 GUI 文档，工作区模板结果也不会因此变成该 GUI 文档的求解结果。

## 2. Host 与 Agent 请求

GUI host 请求必须使用精确封套；未知字段、缺失字段、未知协议版本、超过 64 KiB 的紧凑 JSON 请求和不在 host 中的 operation 均拒绝：

```json
{
  "apiVersion": "range.host/v1",
  "requestId": "client-request-1",
  "operation": "context.query",
  "parameters": {}
}
```

响应包含相同 `apiVersion`、`requestId`、`status`、`code`，成功时含 `result`，拒绝时含 `message`。host 只在 GUI 线程处理请求。当前 host 只读 operation 包括 `project.query`、`document.project.query`、`context.query`、`capabilities.query`、`document.inputs.query`、`document.targets.query`、`events.read`、`operation.query`、`native.jobs.query` 和 `native.job.query`。`native.result.open` 是只读查询入口，但会在 GUI Session 中创建或复用一个独立结果文档。

`context.query` 不带参数时查询当前选中对象；也可只带一个固定 target。上下文把已接受状态与项目草稿、保存状态分别返回。`acceptedState` 目前只明确给出 modal 核心字段；`project.draft` 和 `project.unsaved` 的状态可能是 `unknown`。没有草稿信息不等于没有未提交改动。上下文若标为 `cached`，只能称为最后已知状态；若为 `unavailable`，不要补默认值或称为当前状态。

`document.inputs.query` 和 `document.targets.query` 必须指定当前 GUI target，并可选传一个 `page` 对象：

```json
{
  "apiVersion": "range.host/v1",
  "requestId": "targets-1",
  "operation": "document.targets.query",
  "parameters": {
    "target": {"documentId": "document-uuid", "sessionEpoch": "host-session-uuid"},
    "page": {"limit": 50, "labelQuery": "Point"}
  }
}
```

Page 字段由 Core `targets/context` schema 校验：`limit` 为 1–200，`offset` 或 `cursor` 二选一，可选 `labelQuery`。游标绑定 host 提供的 `documentId` 和当前 `documentRevision`；文档变化、关闭或切换 session 后，旧页/旧目标不能继续写入。`document.inputs.query` 返回接受配置与一页 target 信息；`document.targets.query` 返回 target 候选。标签只用于检索；重名结果以 `candidateIds` 和 `ambiguous` 返回，任何命令都必须传显式 `targetId`。

其余 Host 只读参数固定如下；缺少/多余字段均拒绝：

| operation | `parameters` |
| --- | --- |
| `capabilities.query`、`project.query` | `{}` |
| `context.query` | `{}` 或 `{target:{documentId,sessionEpoch}}` |
| `document.inputs.query`、`document.targets.query` | `{target:{documentId,sessionEpoch}}`，可加 `page` |
| `document.project.query` | `{target:{documentId,sessionEpoch}}` |
| `events.read` | `{afterEventSeq:nonnegative integer,limit:1..100}` |
| `operation.query` | `{operationId}`；只查询当前 Host 内存中的操作 |
| `native.jobs.query` | `{offset:nonnegative integer,limit:1..200}` |
| `native.job.query` | `{jobId}` |
| `native.result.open` | `{jobId,expectedSHA}`，expectedSHA 必须为小写 64 字符 SHA256 |

GUI Agent 通过窄工具适配器访问 host：`host_capabilities`、`context_query`、`document_inputs`、`document_targets`、`document_propose`、`native_jobs`、`native_job`、`native_result_open`。`knowledge_query` 只检索固定 allowlist 文件章节，返回最多 5 条有文件 SHA256 的短片段；它不会增加能力或授权。模型拿不到任意 Qt 方法、shell、文件读取或通用 RPC。工具目录没有 `host_approve`。GUI 的预览批准在可信 host/UI 中完成；显式任务授权也由 runner 在初始文档范围内检查，模型的文字或工具回复不能自行批准。

## 3. Core 命令与支持矩阵

所有 GUI 写入都先走 `command.preview`。普通命令的 `parameters` 精确为 `target`、`expectedDocumentRevision`、`command`、`commandVersion`、`arguments`；执行只接受固定预览 ID 与幂等键：

```json
{
  "apiVersion": "range.host/v1",
  "requestId": "preview-1",
  "operation": "command.preview",
  "parameters": {
    "target": {"documentId": "document-uuid", "sessionEpoch": "host-session-uuid"},
    "expectedDocumentRevision": 12,
    "command": "entity.material.set",
    "commandVersion": 1,
    "arguments": {
      "targetId": "g:18", "name": "Steel",
      "youngModulusPa": 200000000000,
      "densityKgM3": 7850,
      "poissonRatio": 0.3,
      "thermalExpansionPerK": 0.000012
    }
  }
}
```

Core capability 查询按当前模型任务树逐命令返回 `supported`、版本、参数、单位、方法和限制。下面列出当前 Core v1 操作；精确 JSON schema 以目标模型的 `document.inputs.query` / `capabilities.query` 为准。

| 命令 | 适用范围 | 核心参数与单位 |
| --- | --- | --- |
| `analysis.modal.configure` | 含 Stress Modal 的模型 | 精确字段 `method,iterations,modes,tolerance,unit`；方法 `dominant`/`multiple`，迭代 1–1,000,000，模态数 1–1,000，有限 `0 < tolerance <= 1`，`unit="1"`。不计算模态结果。 |
| `analysis.structural_dynamics.configure` | Stress-only | 精确全量字段 `enabled,method,timeStep,unitSI,steps,outputFrequency,rayleighAlpha,rayleighBeta,rhoInfinity,newmarkGamma,newmarkBeta,modalDampingRatios`；`unitSI` 必须为 `true`；方法 `newmark`、`generalizedAlpha`、`centralDifference`；时间步为 s、Rayleigh alpha 为 1/s、beta 为 s，其余积分/阻尼比为无量纲。时间网格乘积必须有限；Newmark、generalized-alpha 与 Rayleigh/modal damping 组合按 capability 限制。 |
| `entity.material.set` | Stress 或 Stress Modal | 必需 `targetId,name,youngModulusPa,densityKgM3,poissonRatio,thermalExpansionPerK`；可选 `referenceTemperatureK`，默认 293.15 K。写入常数 Solid 的四项 SI 属性；`E>0`、密度 `>0`、`-1<ν<0.5`、温度 `>=0 K`。被编辑属性为表格数据时拒绝替换；其他材料属性保留。 |
| `entity.boundary.set` | Stress 或 Stress Modal；kind 进一步限制 | 必需 `targetId,kind,values,unit`，可选 `timeS`（默认 0 s）。`values` 是全局 XYZ 三分量；`null` 禁用对应轴，不能三轴全禁。位移单位 m，可作用于 Point/Line/Surface/Volume；力单位 N，只允许 Stress-only 的单个、单节点 Point element，表示总节点力。 |
| `document.undo` / `document.redo` | GUI 文档已有对应历史 | `arguments={}`；只操作当前 GUI 文档的既有 Undo/Redo 历史，不创建求解 job。 |

材料和边界的目标 ID 形如 `g:<uint>`，是模型内持久 physical group ID，覆盖 Point、Line、Surface、Volume；不是实体数组位置。当前 native model v1.3.18 序列化保存这些 ID；v1.3.17 读入时确定性迁移。标签重命名/列表重排后仍用查询返回的 `targetId`。重复或非法 physical ID 会使目标查询与编辑拒绝。

`analysis.run` 和 `analysis.cancel` 是 GUI host 命令，不是 `RModelCommandSession` 的 Core model-edit 命令。它们同样需预览和可信批准，见下节。几何/网格编辑、任意边界/载荷形式、其它分析算法、对话直接保存以及任意 UI 操作不在此接口中；不能用相似命令名试探。

## 4. Preview、审批、接受与状态哈希

Core 直接封套协议为 `range.application/v1`，字段固定为 `apiVersion,commandId,commandVersion,modelId,expectedRevision,requestId,idempotencyKey,parameters,dryRun`。GUI host 接收外层 `range.host/v1` 的 `command.preview` / `command.execute`，自行构造 Core 封套；Core `modelId/revision` 不作为 Agent 输入或跨进程身份。

普通预览返回 `status=preview`、规范化参数、`before/after`、`changedPaths`、诊断与候选 fingerprint。modal v1 保留旧返回形状 `modal` 以兼容已有消费者。宿主预览另绑定 target/runtime stamp、输入态 fingerprint、read-set hash、候选 fingerprint 和预览 digest。执行只接受同一 host session 内未过期的 `previewId`，要求可信 host 已批准，并重新验证 revision、项目/草稿冲突、read set 与 candidate。`command.execute` 参数精确为 `{previewId,idempotencyKey}`；同键同预览返回内存账本结果，不同预览复用同键拒绝。host 预览缓存上限 128 条，操作及幂等缓存有界且仅存当前 host session。

状态摘要的作用域彼此不同，不能把它们都称为“模型 SHA”：

| 字段 | 覆盖范围 | 不能推出 |
| --- | --- | --- |
| Core `supportedStateFingerprint` / `inputStateFingerprint` | task mask、modal、structural dynamics、time solver 网格、按 physical ID 排序的 group 类型/标签、完整材料与边界状态，以及 Point group 成员身份/几何 | 不是完整 native 文件字节哈希；selection/visibility 不在其中 |
| `candidateFingerprint` / host `digest` | 当前运行期 source stamp、命令、规范参数、before/after、read-set 与候选验证所需状态 | 不是持久 document ID，也不能跨 GUI session 重用 |
| Project `sourceRevision` / `modelSha256` | Project Catalog 接受的持久文档版本及其登记模型快照/来源 | 不等于 Core `revision` 或 GUI `documentRevision` |
| CLI `sourceSha256` | 被选 MODEL/LINK 链最终 model 文件的原始字节 SHA256；CLI 同时校验链上全部 LINK/source artifact | 不是 GUI document identity |
| Job `inputSnapshotRef` / `inputSnapshotSha256` | 实际序列化求解输入文件及其原始字节 SHA256 | 不是源 GUI 文件身份 |
| Job `inputPackageRef` / `inputPackageSha256` | 实际 input package 目录及按目录工件/引用计算的 package SHA256 | 不是求解结果或完整验证报告 |
| Job `resultArtifactSha256` | manager 已读回、路径匹配并校验的终态 native 结果文件 | 不证明数值结果通过独立工程验收 |

API 中的 `documentRevision` 是 GUI 主机接受态/失效 stamp；`sourceRevision` 是 Project Catalog 的 `rN`；Core `revision` 只在一个 model-command session 中递增。任何一项变化都可能让旧 preview 失效，但它们不能互相比较。

InteractionRecord 外层仍是 `journalVersion=1`，每条记录仍是 `schemaVersion=1`。历史 modal-only 记录保留 `stateHashScope="modal_configuration"`。新支持操作记录写 `stateHashScope="supported_model_state"` 和 `stateHashScopeVersion=2`；该作用域版本与 InteractionRecord schema 版本相互独立。现有 `workflowStage` token `modal_configuration` 仍为兼容字段，实际命令以 `commandId` 为准。审计记录成功不代表写盘或求解成功。

## 5. 原生求解、冻结输入、进度、取消和结果

### 提交与冻结输入

`analysis.run` 的 `arguments` 仅允许 `{warningsAccepted: boolean}`。Host 在预览时调用当前 solver validation；有 error、来源 stamp 变化或未明确接受 warnings 时拒绝。批准执行时再次检查 GUI `documentId/sessionEpoch/documentRevision`、项目快照引用和 solver validation，然后由 Project Catalog/Session 建立快照并由 `SolverManager.submitValidated` 验证/排队。提交后的 `SolverTask` 持有脱离 GUI 列表的模型副本、source runtime stamp、输入 snapshot/package hash 和独立结果路径；后续编辑其它文档不会改写该 job 的输入。若在提交前原文档或项目来源已变化，拒绝旧 preview。

`analysis.run` 返回被接受的 `operationId`、真实 `jobId`、source document/session/revision、`sourceSnapshotRef` 和初始 job/progress 状态；后续 job record 使用 `projectSnapshotRef` 字段。`operation.query({operationId})` 查询的是 host session 内有限期的命令操作结果；`native.jobs.query({offset,limit})` 或 `native.job.query({jobId})` 查询的是 SolverManager 注册的求解 job。它们不是同一种账本。

### Job 状态与进度

Native GUI job 目前报告 `queued`、`running`、`cancelling`、`succeeded`、`failed`、`cancelled`。job 查询另含 `sourceRuntimeStamp`、`projectSnapshotRef`、输入快照/包 hash、日志工件引用、进程启动/退出证据、readback 状态和结果身份。`SolverManager` 的 job registry 是当前进程内存态；WF-05 的跨重启持久 job 注册、恢复资格和检查点续作尚未交付。

原生 solver 可通过可信环境握手写入 `range.native-progress/v1` 私有 JSONL。每个记录包含 `protocolVersion,jobId,seq,phaseId,kind,message,pulse,fraction`；`kind` 是 `phase_started`、`fraction` 或 `phase_finished`，`seq` 单调递增，phase ID 随阶段递增，pulse 记录的 fraction 为 `null`。Manager 校验协议和 job ID，诊断错误/缺口，并在 job 查询提供 `progressStatus=pending|available|unavailable`、最新 progress 和有界事件摘要。此进度只表示求解器进度阶段/比例，不是接受步物理时程、结果曲线或收敛证明；进度 journal 不构成跨重启 job 恢复协议。

### 取消与终态

`analysis.cancel` 的参数只含注册 `jobId`；也可附原 target 与 revision，但二者必须精确匹配 job 的 immutable source。取消本身仍预览、批准并重新核对 job source。只允许 `queued/running/cancelling` job；终态拒绝。队列中尚未启动的任务可立即成为 `cancelled`；运行任务进入 `cancelling`，请求只触发 worker-owned process 的取消标记，不能把“请求已接受”说成“已经终止”。响应含 `cancellationRequested=true` 和 `terminalStateDeterminedBySolver=true`；之后用 `native.job.query` 确认最终状态。

### 结果

结果若是 LINK，`resultArtifactSha256` 仍表示登记的 logical 文件原始字节，不能与 terminal MODEL 的 SHA 混用。Worker 读回前后验证完整来源链，registry 固定 `resultSourceBinding={logicalSourcePath,resolvedSourcePath,savedArtifactHash,sourceArtifacts:[{path,sha256},...]}`；其中 savedArtifactHash 是 terminal MODEL 的 SHA。打开和复用结果都重新比较完整固定链，即使 LINK 字节未变，只要 terminal 文件被改写也拒绝。

`succeeded` 要求进程已启动并退出、退出码为 0、result readback 完整、报告路径匹配登记路径、SHA256 有效且与 manager 读回值相符。任一证据缺失时不能报 succeeded。Agent 可用 `native.result.open({jobId,expectedSHA})` 打开登记的成功结果；host 重新核对 job 状态、hash 和 native source binding 后，把结果加载为独立 GUI 文档，并记录 `sourceJob` / source project provenance。此操作不覆盖源文档；结果文档后来可独立编辑，Host 会标注 modified-since-open。

当前没有普通 GUI 求解结果的 `result.series.query`、任意 channel/curve pagination 或 CSV freeze-export 接口。`native.result.open` 只打开一个已验证 native 结果文件；job progress 也不是结果时程。WF-04 的通道目录、结果时程和跨 GUI/Agent 图表接口尚待实现。已有 `range.workflow/v1` `telemetry.query` 只对应固定控制工作区的接受步样本，不能代替普通文档 solver result channel。

## 6. 明确拒绝范围与后续边界

`document.inputs.query`/文件 `context` 的 `configuration.structuralControl` 只读返回原生接受态的 enabled、controlProtocolVersion、controllerId、deterministicSeed；seed 使用十进制字符串保留 uint64 精度。`conditioningPage` 有界返回 actuatorId、delaySteps、holdSteps、noiseAmplitudeN。上下文的同一 page offset/limit/cursor 分别作用于群组和 conditioning 两个目录；labelQuery 分别过滤群组标签和 actuatorId。每个目录报告自身 total/hasMore/nextCursor，游标绑定相同 source/revision，最多 200 项。原生配置校验失败时 available=false 并报告诊断，不宣称可用，也不生成控制器运行态或传感器/执行器物理映射；这些仍由对应控制服务验证。

边界命令的 null 轴在命令和预览中表示禁用该全局分量。为保留 InteractionRecord v1 的严格 payload 约束，含 null 轴的审计参数采用 `parameters={"json":"<完整规范化参数JSON>"}`、`selectionSnapshot.parametersEncoding="json-text/v1"`，文本最多 4096 字节；日志读取者须解码后比较参数。禁用轴与零值轴不会混淆，原命令响应和候选指纹保持原参数语义。

Host/Core 必须拒绝 unknown operation/version/fields、非 SI 或不匹配单位、未公开命令、无效/重复 group ID、错误实体类型、label 代替 target ID、过期游标、关闭文档、sessionEpoch/revision 不符、已知 pending draft/Project Catalog 冲突、busy/model task、过期/已消费 preview、缺少可信审批、job source 不匹配、terminal job cancel 和未验证结果 hash。被拒绝的编辑不能改变模型/输出。`OBSERVED_MODIFIED_UNLOGGED` 表示 GUI 模型已改变、但对应 audit 未完成；响应中的 `outcome=observed_modified_unlogged` / `modelModified=true` 是已发生变更的证据，不能笼统称为正常 accepted。CLI `journal_failure` 可能发生在发布前或发布后，必须同时检查 `status` 和是否已发布。失败、取消请求和结果验证状态都按返回事实报告。

当前受限交付包含 Core 四类配置、GUI host preview/approve/execute、原生 job registry、progress journal、cancel request 和 verified result document open。WF-03 的“将支持矩阵内的当前文档快照接入受限控制工作流”、WF-04 的一般 native result channels/curves、WF-05 的 durable job/operation recovery 与 checkpoint restore 仍为后续范围。新增行为必须先由运行时 capability 公布并补充拒绝矩阵，不得把计划中的 `analysis.series.query`、`document.snapshot.create` 等名称当成当前 operation。
