# 运行验证矩阵：本轮未执行项（2026-11-05）

本轮只做源码/差异审查、回归源编写和获准的轻量检查。**没有配置CMake、编译、构建、执行CTest、启动产品或真实Provider。** 另一会话/用户的手工构建与本审计分开，不继承为本轮通过证据。下列步骤仅记录验证要求，没有自动执行。

## 执行前身份与隔离

- 先固定实际测试的根/七子模块提交以及所有未提交源码字节；当前源在持续修补，旧构建或旧包成功不自动覆盖之后的修改。
- compiler输出放 `build/`，profile、TMP/TEMP/TMPDIR、HOME/USERPROFILE、XDG、APPDATA/LOCALAPPDATA、日志和结果放新的仓内 `artifacts/<run-id>/`，验证解析后的绝对路径后再启动。不要继承用户默认session或配置。
- 私有数值运行时冻结为 CPython 3.13.5 / NumPy 2.2.6 / SciPy 1.15.3。当前Linux 3.14.4 / NumPy 2.3.5、缺SciPy，不具备该冻结验收资格。修改runtime源码后必须生成新同源包与新收据，不刷新旧证据hash。
- 每个目标保留退出码、完整日志/JUnit、平台/Qt/编译器/OpenMP设置、实际执行档与包/源哈希。C++测试未编译时仅称“回归源已补”；Windows skip不计Windows通过。

## 首批针对性回归

| 工作线 | 目标/selector | 必需平台与依赖 | 预期与边界 |
|---|---|---|---|
| 原生作业持久化 | `tst_native_job_registry`：`orphanInterruptionCommitFailurePreservesDurableState`、`gracefulDeletedSenderCannotBypassTerminalEvidence`、`persistentJobRecordReopensAndExternalReplacementIsIsolated` | Qt GUI测试隔离profile；完整v1测试还需实际solver | 锁冲突不改内存/磁盘/hash，不发成功事件；解锁后显式重试一致；成功终态后才释放原lease；超界seq在转换前拒绝且坏证据字节保留。不能据此宣布历史锁释放根因已确定。 |
| Agent证据重入 | `tst_agent_task_runner`：`experimentObservationReentryCannotPublishEvidenceForAnotherTask`、`encodedNativeResultChannelValidation`；`tst_agent_conversation_widget`：`hostObservationCaptureReentryKeepsTaskIdentity` | Qt fake-network，不需真实Provider；后续真实评估另做 | Stop/steer后旧call不跨task发布；正常新task能完成；recorder失败锁存仍fail closed，idle显式重配后才恢复。合法catalog编码通道可查询，非法ID/path仍拒绝。信号fixture不证明真实Host物理证据通过。 |
| 压电草稿/异步 | `tst_piezo_workbench_widget`：`invalidExternalInitialStateCannotReuseThePreviousObject`、`externalConnectionRepliesAreBoundToDocumentAndWholeDraft` | Qt；真实延迟helper集成另需冻结runtime及外部程序 | 非对象JSON不能沿用旧state；编辑重建后仍不可用；回复匹配原UUID/epoch/revision/project/draft与完整草稿才应用。真实延迟成功/FailedToStart、切文档、seed/port变动和销毁需另测。 |
| 验证器/力学 | `tst_trusted_verification_service`的新静力/瞬态/模态正负工件；`tst_trusted_mechanics_service::rejectsAlteredInitialConditionMetadata`；`tst_trusted_experiment_service::meshRefinementClampsBeforeNarrowingAuthorizedBounds` | Qt应用库、实际文件往返；不靠caller DTO | 正常尺度通过；有限大节点场在broker准入后于目标能量/模态门拒绝；disabled/单位/时间键篡改拒绝；DBL_MAX授权上界先限幅后收窄。原1e-6和物理算子不变。 |
| 积分器与分层恢复 | `tst_structural_dynamics_integrator::finiteInputsCannotPublishOverflowedEnergy`；`tst_structural_piezo_layered_native_run`的初值恢复、trace策略冻结/回滚selectors | C++数值目标；OpenMP多核测试至少2可用线程；Windows/MSVC另核对 | 初始/restore/trial能量溢出不发布且accepted不变；正常大数和较小步重试成功；合法seal异初压/无控制动作在facade守门拒绝；关闭trace后普通MODEL确实落盘可恢复。 |
| checkpoint发布/恢复 | `tst_native_accepted_checkpoint`：`traceCertifierLateFileMutation`四行、`periodicRecoveryAcrossPublicationGap`；`tst_piezo_accepted_trace::checkpointRecoverySequencePolicy`七行 | Qt文件系统；真实axial/layered/mixed CLI随后 | callback修改trace/envelope/latest拒绝、回滚且同seq可重试；v1 10→20合法中断恢复，v2必须相邻；错context/不递增拒绝且PENDING/旧指针不动。opaque存储夹具不证明物理恢复。 |

## Windows工具与打包

- PowerShell 5.1（本机无pwsh）运行worker ready/terminal标量、sampler argv、finalizer同缓冲区hash/parse正负源回归。保留13 case /19 argv及no-GO合同；集合、数值布尔、小数PID、替换文件必须拒绝。
- C#/UIA Add-Type、原生Job/handle、root-bootstrap handle-relative创建、reparse和共享锁行为只能在获准Windows环境检查；这些会编译或启动子进程，本轮没有执行。
- 同窗/浮窗、多显示器负坐标、DPI/任务栏工作区分别检验实际布局；超界矩形不得标LayoutValid。诊断Capture全部依赖在全新PS作用域可用。
- source preflight/freeze/package-tree/release-build/finalizer须绑定同一源码/安装/候选身份。更改脚本后旧冻结收据失效，不能只重钉hash追认原PASS。
- producer的独立single-use回归要求三生产者×3控制（9项），最终fixture/aggregate的独立single-use回归要求七脚本×3控制（21项）；本机只跑源码guard及旧HEAD负对照，没有执行PowerShell。七处最终输出已补CreateNew，但中间合成文件及crash-safe发布不在该结论内。
- **静态合同阻塞而不只是缺Windows**：现有含单引号路径的PS黄金canonical SHA与Python JSON规范不同，尚未改算法或重签；relative-create夹具硬要求仓外TEMP和固定build/audit根，与本轮仓内产物政策冲突，不能直接启动其上层final-readiness/static链。需先确定唯一canonical字节合同，并在不削弱reparse控制的前提下解决隔离设计。
- **仍有两项部分审查**：UIA的profile-reuse/Measure是否具有完整qualification所需source-picker/自动绑定证据尚未闭合；246个required CTest名称虽已全文读过，全部条件注册尚未逐条确认，生成inventory也未执行。不能将此状态记为完整准入通过。
- CI的360分钟总预算与两个21600秒单项上限是预算风险；先测实际setup/build/测试耗时和调度，再决定架构调整。本轮没有认定历史失败由总预算造成，也未改预算。

## 必须保留的开放设计/历史材料

1. **热trace历史前缀修改**：在真实seq0..5之后同尺寸翻转seq1、保留header/terminal；检查热追加与冷重开差异。当前热路径仅在受控目录/合作writer边界成立，冷读取全量校验会拒绝。后续owned handle/文件身份pin或不可变chunk方案要与Windows共享、Linux metadata以及20001记录复杂度一起设计，不能用每步全扫O(n²)掩盖性能问题。
2. **历史故障**：原 `.job-record.lock`、Win32 error3缺实际原路径/完整日志，终态失败后的显式核对产品流程、七个Hosted失败和完整安装模型→报告仍未关闭。本地239/246与Hosted239/246失败集合不同，不能拼成全绿。
3. **旧材料**：PC旧33/36与当前3/36冲突；PC00原始receipt/JUnit、Stage7评估脚本和Stage8草稿、旧完整构建/安装收据需旧设备/备份提供或作为新工作重建。缺失不证明历史从未执行，也不构成当前源码已验收。
4. **来源/许可**：最终包的传递依赖/Qt/TetGen/OpenSSL/私有Python和range-ai-lib授权及个人贡献仍需精确材料；代码审查、源码发布或历史inventory PASS不代替最终发行资格。

完整触发、文件位置、补丁风险及平台要求分别保存在 `root.md`、`application.json/.md`、`numerical_submodules.json/.md`、`packaging.json/.md` 与仓内各域review报告。
