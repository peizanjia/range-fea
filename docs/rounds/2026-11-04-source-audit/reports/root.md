# 根 GUI / Host / 原生作业续审（2026-11-05）

## 范围与证据边界

以当前未提交工作树接续 `9ea1d51..e3d5277` 历史批次。暂停清单10个文件的SHA-256全部匹配；既有修补保留。AGENTS.md已撤销，不作为本轮执行规则。所有新增C++回归均未编译、未运行；没有CMake、构建、CTest、产品启动或Provider调用。源码阅读、差异格式检查和独立Python算术不是产品验收。

根清单另由两条独立审查线覆盖 `src/fea-solver/` 与 `src/range-workflow/`；各自详细记录位于仓内 `artifacts/bugfix-audit-20261104/resume-20261105/`。本报告记录主代理实际修补。逐文件真实深度最终归入 `root.json` 和 `coverage.json`，大型历史文件只读本批次完整差异及相关调用链时，不标整文件审完。

## ROOT-RESUME-001：孤儿作业中断登记失败后仍发布内存候选

- **位置**：`src/fea/src/solver_manager.cpp`，`loadNativeJobsFromDiskOnOwner` 的 `INTERRUPTION_PERSIST_FAILED` 分支（约3225行）。
- **触发**：原GUI owner锁已释放，持久记录仍queued/running/cancelling；新manager加载时 `.job-record.lock` 被占用或写入失败。
- **确定原因/影响**：代码先构造interrupted候选，`persistNativeJobRecord`失败后仍无条件insert。内存state与磁盘state不同，`validateNativeJobRecordOnDisk`随后将自身造成的不一致诊断为外部冲突。此处是源码可达缺陷，不证明历史锁释放失败的原因。
- **修补**：只有成功持久化后才替换已加载记录；失败保留原记录、hash/path及独立失败诊断。明确的 `nativeJobRecoveryQuery` 可在解锁后重新核对并提交中断，不从只读查询伪造终态。
- **回归源**：`tst_native_job_registry.cpp::orphanInterruptionCommitFailurePreservesDurableState`。只注册真实完整记录而不排队执行solver；关闭writer后持有记录锁，检查内存/磁盘/hash保持queued，诊断正确；解锁后通过生产恢复查询重试，revision仅增1，再冷重开interrupted。
- **兼容风险**：此前错误显示的interrupted现在保留queued+失败诊断。终态持久化失败后的完整人工核对/恢复产品流程仍未因此实现。
- **实际检查**：调用链与回归源检查；git差异格式检查。未执行Qt测试。
- **待验证**：Linux Qt和Windows Qt分别运行该selector及registry既有锁冲突、immutable-marker/EFS、取消、reopen回归；预期失败不改接受状态，成功重试状态/磁盘一致且不重复启动任务。

## ROOT-RESUME-002：旧回归断言依赖已修复的内存泄漏

- **位置**：`tst_native_job_registry.cpp::gracefulDeletedSenderCannotBypassTerminalEvidence`（约2125行）。
- **触发/原因**：`denyTerminalPersistence=true`仍无条件要求内存state=failed，但磁盘state=cancelling。该预期与现有`updateNativeJob`的失败不发布合同矛盾。
- **修补**：成功行要求failed并且释放lease；失败行要求内存和磁盘均cancelling、原message/hash保持、无自造registry冲突、失败诊断及lease仍在。没有把实际失败改成skip或降低生产验收门槛。
- **待验证**：获授权后运行两行数据化Qt测试及相邻deleted-sender/Stop竞态回归。源码仍未证明测试能编译运行。

## ROOT-RESUME-003：非法外部初始状态JSON沿用上次有效值

- **位置**：`piezo_workbench_widget.cpp::addExternalProgramEditor` 的 `ExternalInitialState` 文本回调（约1504行）。
- **触发**：有效初始状态对象显示后，用户输入未闭合JSON、数组、null或标量。
- **确定原因/影响**：旧代码只invalidatePreview并返回，不改变recipe；随后预览/Run仍使用上一次有效对象，显示输入与实际计算配置不一致。
- **修补**：无效对象输入将草稿字段置为不可接受null，复用既有recipe空值拒绝及savedRecipe匹配守门；有效对象编辑恢复草稿。编辑器重建将该状态显示为null，不伪装成空对象。
- **回归源**：`tst_piezo_workbench_widget.cpp::invalidExternalInitialStateCannotReuseThePreviousObject`，覆盖有效对象、四类非法文本、预览/确认不提交、不改变源版本、重建后仍拒绝，以及修正后的对象恢复。
- **兼容风险**：非法草稿不再保留可运行旧值；不改变合法初始对象或实际控制物理。测试中savedRecipe赋值仅用于隔离UI状态门，不是已保存真实外部实验资格。
- **实际检查**：逐回调追到setValue、requestPreview、previewRun、savedSourceSnapshot与acceptedSeriesCurrent；未启动外部进程。
- **待验证**：Qt GUI selector及真实合法外部controller/excitation实验，编辑错误时Run/Preview/CSV不得使用旧状态；修正后应仍要求完整确认。冻结Python/SciPy和实际外部程序用于后续端到端验证。

## ROOT-RESUME-004：外部连接异步回复未绑定原文档和完整草稿

- **位置**：`piezo_workbench_widget.cpp::inspectExternalConnection`，`externalConnectionRequestIsCurrent`（约1535行）。
- **触发**：检查/握手未返回前，在同Session切到具有相同connection的另一个文档，或仅修改seed、初始状态、端口/反馈等而保持connection对象不变。
- **确定原因/影响**：旧回调只比较setSession代次与connection；loadDocument不推进该代次。旧请求可覆盖新文档/新草稿的初始状态或冻结身份；FailedToStart错误也可能污染新界面。
- **修补**：请求前及回调后核对完整草稿、contextGeneration和原文档UUID/epoch/revision/project/draft，且重新读取当前选择。成功和启动失败回调均使用相同边界，过期结果不应用。
- **回归源**：`externalConnectionRepliesAreBoundToDocumentAndWholeDraft`直接验证生产gate：不变正例、错代次、修改草稿、选择切换前/后、同connection不同seed/初始状态。该测试不冒称执行真实延迟进程。
- **兼容风险**：无关草稿修改也会保守丢弃在途回复，需要显式重试；不取消或推进外部算法状态。
- **待验证**：Windows/Linux GUI下用可控延迟helper分别返回成功/失败，覆盖A→B相同连接、源revision改变、seed/port改变、widget销毁；只有原上下文不变时应用一次。不得把gate源码测试当进程生命周期或Windows验收。

## ROOT-RESUME-005：同步信号/捕获回调造成旧实验证据跨任务归属

- **位置**：`agent_task_runner.cpp` 的query/cancel与wait观察分支，新增`publishExperimentObservation`；`agent_widget.cpp`的`experimentEvidenceObserved`捕获回调。
- **触发**：第一条`experimentObserved`的直接连接同步Stop/steer；或`recordProviderCapture(host_observation)`同步发起新任务再返回成功/失败。
- **确定原因/影响**：runner原先两条信号都发送后才查代次，第二条会使用新activityTaskId搭配旧call/summary。widget原先捕获回调返回后继续用可变lastProviderCaptureContext构造旧host_evidence，或用旧失败取消新任务。
- **修补**：共用发布函数在两次emit之间及之后检查原task/generation/call；query/cancel在Host返回后检查原调用。widget冻结observationContext，回调返回先核对task/context再继续或取消；最后证据写入失败也只能取消原任务。
- **回归源**：runner的`experimentObservationReentryCannotPublishEvidenceForAnotherTask`正常/Stop/steer三行；widget回归由独占GUI测试审查线补充，详见其报告。前者是信号边界fixture，不创建接受的实验、solver或Provider。
- **兼容风险**：被退役任务不再发第二条相关证据；旧第一条事实保留原task身份，新任务不继承旧call。当前任务真正的捕获失败仍拒绝/停止，未降级证据完整性要求。
- **待验证**：Qt direct-connection和fake-network回归，分别验证成功、Stop、steer、recorder失败及新任务继续；再在获准真实Provider/安装链检查评估收据，无跨task/tool绑定。未把本修补归因为历史七失败。

## ROOT-RESUME-006：v1恢复序号在收窄前缺少范围检查

- **位置**：`solver_manager.cpp::nativeJobRecoveryQueryOnOwner`内`readPointer`的acceptedSeq校验（约4425行）。
- **触发**：pointer/envelope哈希及canonical JSON自洽，但acceptedSeq为有限整数double `UINT_MAX+1`或DBL_MAX。
- **确定原因/影响**：旧守门仅查>=1且floor相等，在后续`quint64(seqValue.toDouble())`之前没有上界；DBL_MAX超出目标整数可表示范围，转换具有未定义行为；UINT_MAX+1虽然能表示为quint64，仍超出既有uint接受步合同。更晚的target检查不能保护之前的转换。
- **修补**：在任何转换前要求finite且<=UINT_MAX，与既有目标/seed/compact-trace合同一致；expectedTime非有限也显式拒绝。未改正常clock或容差。
- **回归源**：既有真实v1持久化/reopen测试增加两类hash自洽超界envelope，要求精确命中accepted-state字段诊断、原坏证据字节不被改写，恢复原authority后重新得到原资格。
- **待验证**：Qt目标及UBSan可用构建中的该selector；该回归依赖目标原有真实solver夹具，当前未运行。预期正常seq400不变，超界在转换前拒绝。

## ROOT-RESUME-007：Agent读工具误拒规范编码的实际电极通道

- **位置**：`agent_task_runner.cpp::validToolArguments` 的 `native_result_query` / `workflow_native_result` 分支；新增 `validNativeResultChannel`。
- **触发与原因**：生产 `PiezoResultSeries::readoutSignals` 使用 `QUrl::toPercentEncoding(portId)` 生成通道，实际合法 `piezo:port:sense%20one:readout:absolute` 或含 `~` 的名字被通用 `validIdentifier` 拒绝。broker按真实catalog精确匹配，本身支持这些通道；错误发生在Agent发送读请求之前。
- **影响与修补**：Agent拿到真实catalog仍不能查询对应电极。仅两个read工具的channelId允许128-byte封闭压电词汇及QUrl解码/重编码往返一致的规范UTF-8编码；普通job/artifact/reference/path规则不变，catalog存在性仍由broker确认。节点场display没有电极场意义，不扩张它的支持范围。
- **回归源**：`encodedNativeResultChannelValidation` 正/负数据覆盖普通节点、空格/UTF-8/波浪线/编码分隔符、work通道，以及非规范转义、非法UTF-8、裸路径/换行、未知quantity/axis、空名/超长；每行还检查非法jobId和manifestPath仍拒绝。仅validateTurn源码夹具，无网络、工作流或FE执行。
- **兼容风险/待验证**：旧的虚构 `piezo:` 自由字符串现在在Agent边界先拒绝；合法producer通道不再因百分号误拒。需Qt下运行selector并用真实catalog的series/summary读回验证，与Python公开capabilities/selector语法一致；本轮未编译或运行。

## 回归夹具续审与仍开放的覆盖缺口

- `ROOT-TESTS-001`：两个真实worker的BlockingQueuedConnection以及deleted-sender提交观察者，原先使用长寿命manager作context且捕获测试栈变量。现用在见证变量之后声明的局部QObject，提前失败/cleanup泵事件前先撤销回调。仅修测试，不改生产或断言。
- `GUI-T01..06`：补局部timer/menu context、recorder见证析构顺序、finished spy提前与0/1ms行；精确rod preview测试辅助门只接受缺省或真正空数组的授权范围，拒绝五类错误类型及非空范围。完整历史差异范围和待执行项目见 `native-cli/gui-tests-review.json/.md`。
- `APP-008/009`：numeric close同时拒绝任一侧NaN/Inf；holdout canonical负例签名绑定精确变异字节，恢复独立正例并断言目标错误码，避免别的拒绝门掩盖缺失检查。
- **未修覆盖缺口 `ROOT-TESTS-002..004`**：标准GUI入口清除了 `RANGE_APPLICATION_PROFILE_DIR`，依赖它的headless verifier子测试不可达且硬编码`fea.exe`；`processStarted`是历史事实而非活跃子进程屏障；部分wait/retirement旧测试以大模型耗时和零延迟timer假定事件泵重入，可能在正确实现上假失败。未降低断言、添加skip或用sleep伪造确定性；需设计显式可控pending/terminal边界及隔离CLI profile后再运行，不能据静态发现认定旧七失败根因。详见 `numerical/root-tests-review.json`。

## 尚未关闭的运行/设计问题

- `.job-record.lock`历史释放失败、旧Win32 error 3、终态持久化失败后的显式核对产品流程：缺旧原始路径/日志，不能猜根因。
- 热trace对不合作外部进程的同尺寸历史前缀改写仍是确认的完整性边界；冷读取全量校验会拒绝。不能用每FE全前缀O(n²)扫描草率代替增量文件身份设计，见native-cli报告。
- 七个历史Hosted失败与完整安装模型→报告链仍需冻结来源和真实环境；本轮没有将其标为通过。
- 本机无pwsh、冻结SciPy缺失，未放宽Windows路径合同，也未刷新旧验收哈希来接受修改后的源码。
- 另一个用户会话有独占Qt兼容修补（cloud HTTP与三个model文件）；不属于本轮写入或构建证据。最终coverage须记录审阅hash与后续外部修改区别。
