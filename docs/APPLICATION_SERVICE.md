# 应用服务：模态配置编辑会话 v1

本模块是 Stage 10A 的原生配置服务，位于 `src/range-application-lib`。它依赖 Qt Core 和模型库，不依赖 GUI 或求解器。服务提供模态配置候选事务、显式会话生命周期及可选持久 InteractionRecord v1 日志；CLI 单文件发布和 GUI 既有撤销栈仍由宿主负责。

## 所有权与边界

`RModelCommandSession` 构造时复制输入 `RModel`，外部只能读取接受态。所有访问由同一个拥有线程执行；返回的模型引用仅在下次成功命令前有效，不允许跨线程或通过 const_cast 修改。每个实例具有新的 modelId，revision 是该实例的单调版本，不是源文件哈希或 GUI 全局版本。持久化、原模型冲突检查和磁盘发布是调用端的独立职责。

`query()` 返回协议版本、modelId、revision、当前 modal 参数、是否支持模态配置、撤销/重做可用性及剩余命令容量。`canConfigureModal` 只表示模型任务树包含结构模态任务且可编辑配置，不证明该模型一定能求解或通过数值验收。

`query()` 另返回 `lifecycle`（`ready`、`running`、`closed`）和 `busy`。宿主调用 `setRunning(true/false)` 进入或解除运行状态；运行期间拒绝配置、Undo/Redo 和正式重试，返回 `session_busy`，只读查询和预览可继续。`close()` 永久关闭实例，后续命令返回 `session_closed`；重新打开必须新建实例，旧 modelId 返回 `wrong_model`。生命周期切换不修改配置 revision，相同状态的重复切换不重复记日志；关闭后不能重新进入 ready/running。

宿主若在会话外完成原模型编辑、GUI Undo/Redo、重载或替换，必须丢弃旧实例并为新快照创建新会话。revision 只在实例内单调增加；不能把它用作跨实例的 GUI 全局计数器。

外部修改完成后，宿主调用 `recordExternalChange(reason, changedModel, &error)`，reason 只允许 `undo`、`redo`、`reload`、`external_edit`。该事件明确记录操作原因、旧/新模型的模态参数摘要及旧身份失效，而非笼统记录关闭。这里修改已经发生，函数无论日志是否成功都关闭旧身份；false 表示变更已发生但审计不完整，宿主必须报告故障，不能把旧快照继续用于命令。新实例拥有新的 modelId/revision；日志中的 `invalidated:` revision 是身份失效标记，不作为后继实例的版本令牌。

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

方法只接受 `dominant`/`multiple`；迭代数 1–1000000，模态数 1–1000，容差有限且在 `(0,1]`。全部量无量纲，unit 必须为 `1`，不做隐式单位转换。模式/频率结果字段不作为配置输入。服务不计算模态，也不把配置合法等同于求解可行。

另有 `model.undo` 和 `model.redo`，使用同一封套、空 parameters；只撤销本会话内的配置命令。它们生成新 revision，不恢复旧版本号。空历史返回 `empty_history`。普通新配置清除 redo 分支。即使配置值不变，一次成功命令仍占用一个版本和历史项。

## 事务与重试

- `dryRun=true` 校验并返回 `preview` 和候选 modal，revision、历史和幂等账本均不改变；可用同一个键提交对应正式请求。
- 成功返回 `accepted`、preRevision、新 revision 和配置。候选模型、历史及账本都构造完毕后才交换接受态；拒绝不消耗键或版本。
- 同键、完全相同封套（含 requestId）的重试只在该命令结果仍是当前 revision 时返回原响应，不重复执行。不同内容使用旧键返回 `idempotency_conflict`；其他编辑或撤销后的旧重试返回 `stale_revision`。
- 每个会话最多接受 256 个命令（含撤销/重做）。达到上限拒绝继续提交，不驱逐旧键；调用端应发布或丢弃会话再重新打开。预览仍可使用。
- 请求限制 64 KiB；标识字符串须非空且不超过 128 字符。非法协议/字段为 `invalid_request`，参数为 `invalid_parameters`，跨会话身份为 `wrong_model`，没有模态任务为 `unsupported_analysis`。

该入口未授予 Agent 执行权限。CLI/GUI 发布前仍须验证源模型未变及运行状态，不能绕过原有求解前置检查。

## 持久审计与故障边界

构造函数可接收 `JournalOptions {path, actorId, sourcePath, sourceSha256}`。这些是可信宿主元数据，不从命令 JSON 中读取；空 path 禁用日志。sourcePath/sourceSha256 须同时提供或同时省略，SHA256 为 64 位小写十六进制。普通命令不支持设置 actor、批准状态或日志路径。

日志采用 `{"journalVersion":1,"projectId":"UUID","records":[...]}` 存储封套；每个 records 项均通过 `RInteractionRecordV1::toJson/fromJson`。追加前读取并验证全部已有记录、projectId、唯一 recordId 和连续 parentRecordId，拒绝损坏、截断、未知版本和断链；不会用空日志覆盖坏文件。写入使用非等待 QLockFile 和禁止直接覆盖回退的 QSaveFile，同一日志限制 64 MiB / 100000 条。目录必须已存在，符号链接文件拒绝；并发写入者需要遵守协作锁。

记录包含 actor、会话/模型身份、命令 SHA256、前后 revision、参数、模态配置前后摘要及源文件路径/SHA256。v1 的 modelRevision 字段使用 `session:` 前缀以满足其 token 语法，selectionSnapshot 的 preRevision/postRevision 保存原 API revision。`stateHashScope=modal_configuration` 明确摘要只覆盖模态参数，并非完整模型文件哈希。未知或非法请求只记录内容摘要、已知命令名及服务生成的错误码/消息，不复制未校验参数、API 凭据或任意未知字段。

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


## CLI 文件入口与 GUI 接线

`range-model-command` 使用同一服务验证配置。先查询完整原生模型（不接受 latest-link）：

```powershell
range-model-command --input input.ram --query
```

查询输出为 JSON，包含 `sourceSha256`。创建参数 JSON（根对象仅允许这两个字段）：

```json
{"sourceSha256":"查询所得SHA256","parameters":{"method":"multiple","iterations":100,"modes":3,"tolerance":1e-9,"unit":"1"}}
```

```powershell
range-model-command --input input.ram --parameters modal.json --dry-run
range-model-command --input input.ram --parameters modal.json --output configured.ram
range-model-command --input input.ram --parameters modal.json --output configured.ram --journal interactions.json
range-model-command --reconcile --journal interactions.json
```

CLI 每次创建独立内存会话，源 SHA256 承担跨进程过期检查，不承诺跨进程幂等账本或撤销。正式输出必须与源模型和参数文件不同；支持 `.ram/.rbm`。输出先在同目录临时子目录序列化，再经 QSaveFile 单文件发布；失败保持旧输出。输入/参数/目标使用有序协作锁，发布前复核路径和哈希；这不构成对不遵守锁的外部写入者的原子比较交换。stdout 输出 JSON、诊断走 stderr；`published` 只在文件提交后返回。配置成功不代表该模型已求解，原结果不被冒充为新配置的结果。

`--journal` 可用于预览或发布，必须与输入、参数及输出路径不同。查询本身不写事件；CLI 的文件打开、参数文件封套及源 SHA256 检查发生在服务命令前，这些入口错误不会生成服务记录。日志不可写或已有记录损坏时，正式配置在发布前失败并保持已有输出。输出提交后发布日志失败则返回 `status=published_unlogged`、`code=journal_failure`、退出码 3，并报告已发布路径及可用的实际 SHA256；文件不会删除或回滚。无日志模式下若提交后的哈希校验失败，返回 `published_unverified` 和退出码 3。正常成功退出码为 0，发布前拒绝为 2。

现有 GUI 模态设置调用同一服务校验；所有选中模型校验通过后才更新配置，并进入既有 GUI Undo/Redo 栈。活动任务期间拒绝修改；容差输入上限与服务一致。GUI 会话生命周期由宿主持有并在源模型变化时失效；这不是全应用版本体系。多模型配置在验证层面先全检，内存分配失败时尚无跨模型统一回滚保证。完整窗口操作、保存重开和一般任务取消的集成验收以 HANDOFF.md 中实际证据为准。

GUI 的逻辑模型 UUID 独立于列表位置，原生编辑会话 revision 则随保存、重开、撤销/重做、重载和旧入口修改失效。关闭审计失败会记录错误并保留待补记会话，后续查询/执行或关闭事件会重试；析构时再尝试一次。退出时仍失败的记录没有跨进程持久重试队列。保存或重载已改变文件/内存后审计失败，任务明确报告失败及已经发生的修改，不承诺回滚已发布字节。

验证入口：`tst_model_command_cli`（真实进程、日志与原生文件发布/失败保护）、`tst_modal_command_widget`（控件到服务参数一致性）、`tst_gui_model_lifecycle`（实际 Application/MainWindow/Session、单/多模型配置、Undo/Redo、保存重开、重载、队列限制、审计锁失败）。完整对象图测试使用隐藏窗口和独立临时设置目录，不触碰用户会话；不等于 OpenGL 视口渲染、可访问性或干净机器终验。
