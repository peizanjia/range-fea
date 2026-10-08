# 应用服务：原生模型命令会话 v1

本模块位于 `src/range-application-lib`，依赖 Qt Core 和模型库，不依赖 GUI 或求解器。最初的 Stage 10A 只提供 modal 配置；当前 Core v1 还为 structural dynamics、physical-group material 和 boundary 提供同一套 preview/execute 候选事务，并支持这些已接受编辑的会话内 Undo/Redo。GUI 求解 job、Host 审批和 GUI 既有撤销栈由 `range.host/v1` 宿主负责，不是 Core Session 命令。详见 [原生文档操作契约](NATIVE_MODEL_OPERATIONS.md)。

## 当前能力与兼容边界

`RModelCommandService::capabilities(model)` 是每个模型的实时支持矩阵：命令 ID/version、`supported`、必需参数、方法、单位、可选默认值、目标和约束都以该结果为准。当前 Core 命令是 `analysis.modal.configure`、`analysis.structural_dynamics.configure`、`entity.material.set`、`entity.boundary.set`；不支持的分析/任务树和额外字段必须拒绝。Core 不接受 `analysis.run` 或 `analysis.cancel`，也不持有 GUI 文档或 solver process。

直接 `RModelCommandSession` 的历史命令 ID 为 `model.undo` / `model.redo`；GUI facade 暴露 `document.undo` / `document.redo`，通过当前 GUI 原生撤销栈执行，不能把两个入口的 session token 混用。旧 modal-only 响应和旧 InteractionRecord 保持读取兼容；新增命令扩展字段时仍要求精确 schema。

## 所有权与边界

`RModelCommandSession` 构造时复制输入 `RModel`，外部只能读取接受态。所有访问由同一个拥有线程执行；返回的模型引用仅在下次成功命令前有效，不允许跨线程或通过 const_cast 修改。每个实例具有新的 modelId，revision 是该实例的单调版本，不是源文件哈希或 GUI 全局版本。持久化、原模型冲突检查和磁盘发布是调用端的独立职责。

`query()` 返回协议版本、会话 `modelId`/`revision`、当前 modal 和 structural dynamics 参数、Core capabilities、supported-model-state fingerprint、Undo/Redo 可用性及剩余命令容量。`canConfigureModal` 仍只表示任务树包含结构模态任务且 Core 可配置；`capabilities.commands[].supported` 按当前任务树给出各命令范围。任何配置合法都不证明 native solver 可接受、求解成功或数值结果通过验收。

`query()` 另返回 `lifecycle`（`ready`、`running`、`closed`）和 `busy`。宿主调用 `setRunning(true/false)` 进入或解除运行状态；运行期间拒绝配置、Undo/Redo 和正式重试，返回 `session_busy`，只读查询和预览可继续。`close()` 永久关闭实例，后续命令返回 `session_closed`；重新打开必须新建实例，旧 modelId 返回 `wrong_model`。生命周期切换不修改配置 revision，相同状态的重复切换不重复记日志；关闭后不能重新进入 ready/running。

宿主若在会话外完成原模型编辑、GUI Undo/Redo、重载或替换，必须丢弃旧实例并为新快照创建新会话。revision 只在实例内单调增加；不能把它用作跨实例的 GUI 全局计数器。

外部修改完成后，宿主调用 `recordExternalChange(reason, changedModel, &error)`，reason 只允许 `undo`、`redo`、`reload`、`external_edit`。该事件记录受支持模型编辑面的前后 state fingerprint 和旧身份失效，而非只记录 modal 字段。修改已经发生时函数无论日志是否成功都关闭旧身份；false 表示变更已发生但审计不完整，宿主必须报告，不能继续使用旧快照。新实例取得新的 modelId/revision；日志中的 `invalidated:` revision 是身份失效标记，不是后继实例的版本令牌。

## 命令

`execute(QJsonObject)` 接收如下精确封套（多余字段、缺失字段和错误类型拒绝）：

```json
{
  "apiVersion": "range.application/v1",
  "commandId": "analysis.modal.configure",
  "commandVersion": 1,
  "modelId": "从query取得",
  "expectedRevision": "从query取得",
  "requestId": "client-request-1",
  "idempotencyKey": "client-operation-1",
  "parameters": {
    "method": "multiple",
    "iterations": 100,
    "modes": 3,
    "tolerance": 1e-9,
    "unit": "1"
  },
  "dryRun": false
}
```

以上是 `analysis.modal.configure` 示例；同一封套用于其它 Core v1 命令，`commandId` 决定 `parameters` 的精确 schema。Host/Agent 的外层请求、审批和求解命令见 [NATIVE_MODEL_OPERATIONS.md](NATIVE_MODEL_OPERATIONS.md)。

方法只接受 `dominant`/`multiple`；迭代数 1–1000000，模态数 1–1000，容差有限且在 `(0,1]`。全部量无量纲，unit 必须为 `1`，不做隐式单位转换。模式/频率结果字段不作为配置输入。服务不计算模态，也不把配置合法等同于求解可行。

其余 Core 命令如下；运行时 capabilities 是最后依据，Core 会拒绝表中以外的字段：

| commandId | exact parameters / 单位 |
| --- | --- |
| `analysis.structural_dynamics.configure` | `enabled,method,timeStep,unitSI,steps,outputFrequency,rayleighAlpha,rayleighBeta,rhoInfinity,newmarkGamma,newmarkBeta,modalDampingRatios`；`unitSI=true`，时间 s、Rayleigh alpha 1/s、beta s，其它系数无量纲。只支持 capability 列出的积分方法与组合。 |
| `entity.material.set` | 必需 `targetId,name,youngModulusPa,densityKgM3,poissonRatio,thermalExpansionPerK`；可选 `referenceTemperatureK` 默认 293.15 K。目标为 `g:<physical uint>`；支持范围及材料常数限制见运行时 capability。被编辑属性为表格形式时拒绝隐式转换，未编辑材料属性保留。 |
| `entity.boundary.set` | 必需 `targetId,kind,values,unit`；可选 `timeS` 默认 0 s。`values` 是全局 XYZ 三分量，`null` 关闭该轴；位移用 m，力用 N，并受任务树、组类型和 point 结构限制。 |
| `model.undo` / `model.redo` | 空 `parameters`；只回退/重做当前 Core Session 已接受的支持 delta。GUI Host 的对应命令名是 `document.undo` / `document.redo`。 |

`analysis.run` / `analysis.cancel` 由 GUI Host 执行，不属于以上 Core Session 命令。命令目标 ID 使用持久 physical group ID，不是数组位置或标签；v1.3.18 持久化该 ID，v1.3.17 读取时确定性迁移。完整 Host 支持矩阵见 [NATIVE_MODEL_OPERATIONS.md](NATIVE_MODEL_OPERATIONS.md)。

Undo/Redo 生成新 revision，不恢复旧版本号。空历史返回 `empty_history`。普通新配置清除 redo 分支。即使配置值不变，一次成功命令仍占用一个版本和历史项。

## 事务与重试

- `dryRun=true` 校验并返回候选，不改变 revision、历史和幂等账本；modal 保留 legacy `modal` 响应，其他命令返回 `normalizedParameters`、`before`、`after`、`changedPaths`、`fingerprint` 和 `diagnostics`。同一个精确请求键可用于提交对应正式请求。
- 成功返回 `accepted`、preRevision、新 revision 和当前命令配置。候选模型、history delta 及幂等账本构造完毕后才交换接受态；拒绝不消耗键或版本。
- 同键、完全相同封套（含 requestId）的重试只在该命令结果仍是当前 revision 时返回原响应，不重复执行。不同内容使用旧键返回 `idempotency_conflict`；其他编辑或撤销后的旧重试返回 `stale_revision`。
- 每个会话最多接受 256 个命令（含四类支持编辑及撤销/重做）。达到上限拒绝继续提交，不驱逐旧键；调用端应由可信宿主发布或丢弃会话再重新打开。预览仍可使用。
- 请求限制 64 KiB；标识字符串须非空且不超过 128 字符。非法协议/字段为 `invalid_request`，参数为 `invalid_parameters`，跨会话身份为 `wrong_model`，没有模态任务为 `unsupported_analysis`。

该入口未授予 Agent 执行权限。CLI/GUI 发布前仍须验证源模型未变及运行状态，不能绕过原有求解前置检查。

## 持久审计与故障边界

InteractionRecord v1 的对象 payload 仍拒绝 JSON null。`entity.boundary.set` 的禁用轴必须保留 null 语义：含 null 轴的日志参数使用 `parameters={"json":"<规范化参数的紧凑JSON文本>"}`，并标记 `selectionSnapshot.parametersEncoding="json-text/v1"`。读取者解析该文本取得原参数；不把禁用轴替换为零位移或零载荷。文本上限 4096 字节，超过则拒绝日志提交且不改变模型。无 null 的参数保持原对象表示；命令响应仍返回原规范化对象。该编码不改变 v1 日志格式、旧记录或严格 payload 校验。

构造函数可接收 `JournalOptions {path, actorId, sourcePath, sourceSha256}`。这些是可信宿主元数据，不从命令 JSON 中读取；空 path 禁用日志。sourcePath/sourceSha256 须同时提供或同时省略，SHA256 为 64 位小写十六进制。普通命令不支持设置 actor、批准状态或日志路径。

日志采用 `{"journalVersion":1,"projectId":"UUID","records":[...]}` 存储封套；每个 records 项均通过 `RInteractionRecordV1::toJson/fromJson`。追加前读取并验证全部已有记录、projectId、唯一 recordId 和连续 parentRecordId，拒绝损坏、截断、未知版本和断链；不会用空日志覆盖坏文件。写入使用非等待 QLockFile 和禁止直接覆盖回退的 QSaveFile，同一日志限制 64 MiB / 100000 条。目录必须已存在，符号链接文件拒绝；并发写入者需要遵守协作锁。

记录包含 actor、会话/模型身份、命令 SHA256、前后 revision、规范参数以及按作用域定义的前后 state hash；可选可信来源包含 source path/SHA256。InteractionRecord 外层仍为 `journalVersion=1`，每条仍为 `schemaVersion=1`。旧记录保留 `stateHashScope=modal_configuration` 且不带 scope version；新支持命令记录 `stateHashScope=supported_model_state`、`stateHashScopeVersion=2`。后者覆盖 Core supported-state fingerprint 的模型编辑面，不是完整 native file SHA256。v1 `modelRevision` 字段继续使用 `session:` 前缀，`selectionSnapshot.preRevision/postRevision` 保存原 API revision。`workflowStage=modal_configuration` 是既有兼容 token；读取命令种类以 `commandId` 为准。未知或非法请求只记录内容摘要、已知命令名及服务生成的错误码/消息，不复制未校验参数、API 凭据或任意未知字段。

预览、拒绝、接受分别记录为 `preview`、`rejected`、`accepted`；无状态变更的幂等重放不重复记录。开始运行和解除运行记录 `running`、`ready`，两者关联相同的宿主会话 runId；解除运行仅表示解除忙状态，不代表求解成功。关闭记录 `closed`。此服务的批准状态为 `not_required`；工作流宿主的批准与作业结果由工作流层记录。

先构造完整候选模型/历史/账本，再原子写入 accepted 记录，最后通过不抛异常的指针交换提交内存状态。日志失败返回 `journal_failure`，不改变模型、revision、历史或幂等键；生命周期方法返回 false 且保留原状态。宿主必须处理失败返回值。accepted 仅证明该配置候选已获服务接受，不证明原生模型已保存；进程在日志写入后、内存交换前中断，也不会产生任何模型文件发布事件。新进程重新打开模型时取得新身份，不从日志恢复内存撤销栈或幂等账本。

原子文件提交前，宿主调用 `recordPublicationIntent(outputPath, stagedPath, &error)`，先持久记录 publicationId、规范目标路径、实际暂存文件 SHA256、原目标是否存在及原目标 SHA256。写入失败时不得提交模型；意图待完成期间拒绝继续编辑该会话。随后实际完成文件提交，再调用 `recordArtifactPublished(outputPath, &outputSha256, &error)`，读取目标文件字节计算 SHA256，并验证其匹配该意图，成功后追加关联相同 publicationId 的 `published` 事件。文件已经发布后日志若失败，文件保留，函数返回 false；同一会话可在修复日志故障后补记，成功补记的重复调用不会重复追加。旧的 GUI 发布后记账调用仍可没有预先意图，这一路径不具备 CLI 的崩溃恢复保证。

CLI 的 `--reconcile --journal interactions.json` 在新进程调用 `reconcilePublications()`，读取完整有效日志，只处理未关联 published/reconciled 的意图，最多 256 条。它在协作目标锁下检查当前文件，并将 `model.artifact.reconciled` 观察事件原子写入原日志；不会重写模型，不依赖临时目录继续存在，也不恢复内存账本。返回的 `observationOnly=true` 明确这不是模拟求解或新配置接受。

- `artifact_present`：目标实际字节匹配预期，且提交前不存在该内容；覆盖“模型已提交、published 日志尚未写入”的中断状态。
- `not_published`：目标仍不存在，或仍等于记录的旧字节；覆盖提交前中断。它描述当前文件状态，不证明外部写入者从未改动过文件。
- `already_present`：提交前就存在相同预期字节；无法据此证明此次提交发生过，不记录为新的发布成功。
- `conflict`：当前字节既不匹配预期，也不匹配记录的旧状态；保留文件并报告冲突。
- `busy`、`unreadable`、`unresolved_path`：无法取得可信观察，保留该意图待后续处理；不追加完成记录。

已经写入的相同观察不重复记录。冲突仍保留为待解决项，再次恢复会重新检查目标并继续报告冲突；内容相同则标记 alreadyRecorded，不增加重复事件，内容变更则记录新观察。恢复中日志写失败返回 `reconciliation_incomplete`，逐项 `auditRecorded` 和 `journalUpdated` 表明已持久完成的部分，退出码 3。正常恢复返回 `reconciled`；冲突或未解决项目也返回退出码 3。外部不遵守协作锁的改写仍可能改变后续磁盘状态，不能把一次哈希观察当作永久事实。日志与模型文件是两个独立原子文件；上述意图、观察及故障返回构成显式恢复协议。

## 验证

`tst_model_command_session` 验证真实 RModel 配置、输入隔离、预览、参数反例、身份/版本、幂等冲突、失败原子性、撤销/重做和容量上限，并覆盖关闭/重开/运行限制、v1 记录链、拒绝脱敏、锁冲突/坏日志不改变接受态、外部修改后的真实摘要和强制失效、发布意图/文件不匹配与补记。CLI 测试在原会话消失、暂存文件删除后启动新进程，分别恢复提交前/提交后、旧文件未变、内容冲突和原本已有相同内容的状态，并检查不改写文件及恢复记录幂等性。以主构建注册的 CTest 运行，构建和结果记录见 HANDOFF.md。


## CLI 文件入口与 GUI Host 接线

`range-model-command` 使用同一 Core 服务校验 `.ram`/`.rbm` 文件。`--input` 可以是完整 MODEL，也可以是 native LINK；CLI 用 `RProjectCatalog::resolveSourceBinding` 解析整条来源链。`--query` 的 `sourceSha256` 是终端 model 文件原始字节 SHA256；读取/发布前会重新校验链上所有 LINK 与 model。输出不得覆盖 logical input、任一中间 LINK、终端 model、参数或 page 文件。

```powershell
range-model-command --input input.ram --query
range-model-command --input input.ram --capabilities
range-model-command --input input.ram --context --page page.json
range-model-command --input input.ram --targets --page page.json
```

参数 JSON 顶层只允许 `sourceSha256` 和 `parameters`。省略 `--command` 时兼容原 modal CLI；指定 `--command` 后使用当前 Core capability 描述的任一支持操作。`--dry-run` 只预览；执行须提供独立 `--output`。分页 cursor 以 canonical 输入路径和 source SHA 为身份，可跨 CLI 进程继续，同源改变后拒绝：

```json
{"sourceSha256":"查询所得终端 model SHA256","parameters":{"method":"multiple","iterations":100,"modes":3,"tolerance":1e-9,"unit":"1"}}
```

```powershell
range-model-command --input input.ram --parameters modal.json --dry-run
range-model-command --input input.ram --command entity.material.set --parameters material.json --output configured.ram
range-model-command --input input.ram --command entity.boundary.set --parameters boundary.json --output configured.ram --journal interactions.json
range-model-command --reconcile --journal interactions.json
```

CLI 每次创建独立内存 session；`sourceSha256` 提供跨进程 stale-source 检查，不承诺跨进程 Core 幂等账本或 Undo。它只校验、预览并发布文件，不启动 GUI `analysis.run` job，也不会改动同一路径已打开的 GUI 文档。GUI 原生求解从 `range.host/v1` 预览/批准/提交，详见 [NATIVE_MODEL_OPERATIONS.md](NATIVE_MODEL_OPERATIONS.md)。

正式输出先在同目录临时子目录序列化，再经 QSaveFile 单文件发布；失败保持旧输出。Source LINK、参数和输出用协作锁，发布前复核 source binding 与输入参数哈希；这不构成对不遵守锁的外部写入者的原子比较交换。stdout 只输出 JSON，诊断走 stderr。`published` 只表示文件发布完成，不表示模型已求解。

`--journal` 可用于预览或发布，且必须与 source chain、参数和输出分离。已提交输出后日志失败返回 `published_unlogged` / 退出码 3，文件不回滚；无 journal 时发布后哈希检查失败返回 `published_unverified` / 退出码 3。发布前拒绝退出码 2。`--reconcile` 只观察未完成发布意图并记账，不重新发布或恢复模型。

GUI 将同一 `RModelCommandService` 用于 modal、structural dynamics、material、boundary 候选；通过 `range.host/v1` 增加 target/source revision、受信审批和既有 GUI Undo/Redo。solver 与 cancellation 属于 host/`SolverManager`，不是 Core Service 或 CLI。相关 source、job、progress、结果和拒绝边界见 [NATIVE_MODEL_OPERATIONS.md](NATIVE_MODEL_OPERATIONS.md)。

GUI 文档逻辑 UUID 独立于列表位置；编辑 session stamp 会随已知保存、重开、撤销/重做、重载和旧入口修改失效。关闭审计失败会留下待补记会话，并由后续查询/执行/关闭事件重试；析构时再尝试一次，但当前没有跨进程持久补记队列。保存或重载已经改变文件/内存后若审计失败，必须报告已发生的变化，不承诺回滚已发布字节。

验证入口包括 `tst_model_command_service`（Core 参数、目标、候选与 fingerprint）、`tst_model_command_session`（会话事务、Undo/Redo、日志和发布观察）、`tst_model_command_cli`（真实 CLI 文件发布）及 GUI lifecycle/Agent 测试。测试通过范围与全应用安装、可访问性或所有原生结果视图验收是不同结论。
