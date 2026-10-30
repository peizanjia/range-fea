# 压电交付安装版 GUI 操作验收矩阵

**编制：**2026-10-14（Asia/Singapore）。**状态：待实测。**本表从当前工作树的窗口代码和 `PIEZO_CONTROL_DELIVERY.md` 提取可执行操作故事，不能代替安装版 UIA、窗口截图、点击轨迹或 `piezo_control_plan.json` 的 36 项验收。当前 11 个 `workflowPages` 顶层页签**包括**“Piezo control”；后者内部另有 6 页，不应重复计算为第 12 个顶层页。

## 顶层工作流与主窗口

| 页签或入口 | 关键对象名 | 安装版必须观察的操作 |
| --- | --- | --- |
| Live monitor | `controlMonitor`, `controlSensor`, `controlActuator`, `controlTimeRange`, `controlHold`, `controlExport` | 选实际任务与通道，改变时窗、保持和导出；状态、曲线、控件在参考窗口尺寸内可用。 |
| Experiment | `workflowWorkspace`, `workflowOpen`, `workflowModel`, `workflowConfigure`, `workflowRun`, `workflowResume`, `workflowRecover`, `workflowApply` | 打开工作区、选支持来源、预览并一次批准；输入变更后旧预览拒绝。 |
| Results & history | `workflowJobs`, `workflowCancel`, `workflowResults`, `workflowAudit` | 完成任务的结果、历史和审计归属同一任务；取消只作用于选中且归该页管理的任务。 |
| Workflow FE output | `workflowNativeMode`, `workflowNativeResults` | 主动／被动输出切换后读取对应实际任务通道与样本。 |
| Document FE output | `nativeResultJobChoice`, `nativeRecoveryCheck`, `nativeRecoveryApply`, `documentNativeResults` | 选中断原生任务，先检查 checkpoint 与恢复摘要，再批准本次恢复。 |
| Task plans | `taskPlanChoice`, `taskPlanTree`, `taskPlanNodePreview`, `taskPlanApproveNode` | 选节点，核对来源／参数提案，只批准所选节点。 |
| Run parameters | `runtimeControlWidget`, `runtimeJobChoice`, `runtimeProposalChoice`, `runtimePreview` | 对选中运行任务核对前后参数与接受历史，再应用预期更新。 |
| Compare curves | `seriesWorkbenchWidget`, `seriesSourceSelection`, `seriesRemoveMean` | 选已注册时序，比较时域／频域，核对来源标签与导出。 |
| Geometry commands | `geometrySource`, `geometryCommand`, `geometryPreview`, `geometryApproveApply` | 选择当前模型与命令，预览后一次批准；旧预览拒绝。 |
| Mesh candidates | `meshJobs`, `meshPreviewGenerate`, `meshApproveGenerate`, `meshPreviewAccept`, `meshApproveAccept`, `meshCancel` | 生成和接受候选各自预览／批准；取消只作用于选中网格任务。 |
| Piezo control | `piezoWorkbench`, `piezoPages` | 走下文六页的同一模型完整实验链。 |

主窗口另有 Models dock 的 3 页及 Solver dock 的 7 页；顶层工作流导航与这些 dock 不是同一个页数口径。压电入口需先在 Models → Model 选**唯一**模型，再进 Dynamics & control → Piezo control，并以 `piezoSource` 核实来源身份；零选或多选应给出清楚反馈。GUI 实测须保存页面、窗口大小、缩放、焦点与必要截图／UIA 轨迹，不能只证明控件存在。

## 压电六页与少操作主链

| 内页 | 关键对象名 | 必测决策 |
| --- | --- | --- |
| Source / devices / materials | `piezoTemplateKind`, `piezoUsePickedMembers`, `piezoAxialMembers`, `piezoLayers`, `piezoReplaceLegacy` | 杆、梁、板物理 ID 与拾取成员一致；替换／贴片、材料来源可辨。 |
| Sensors / electrodes / supports | `piezoPorts`, `piezoSupports` | 作动／传感角色、电学读出、连续电极、支座及不支持 DOF 均能明确核对。 |
| Modes / damping / export | `piezoComputeModes`, `piezoAllModes`, `piezoDamping`, `piezoExportMatrices` | 模态、阻尼选择和导出对象属于当前修改后的 FE；陈旧设计失效。 |
| Controllers / state source | `piezoController`, `piezoStateSource`, `piezoControllerOutputs` | PI/PID/PD、LQR/MPC 和全 FE／仅传感器状态选项、映射及限制显示清楚。 |
| Excitation / initial state | `piezoTimeStep`, `piezoSteps`, `piezoAddForce`, `piezoExplicitWorkpoint` | 时间网格、节点力、初始量与外部时间函数来源改动可预览；源变化使旧批准失效。 |
| Accepted results | `piezoShowAcceptedSeries`, `piezoResultPort`, `piezoResultQuantity`, `piezoResultReference`, `piezoResultView`, `piezoResultWindow`, `piezoAcceptedPlot`, `piezoExportAcceptedCsv` | 读取已认证接受记录，核对端口／量／绝对或增量／时频视图与完整 CSV；损坏或跨来源记录拒绝。 |

完整操作故事：拾取当前物理模型并跨六页设置 → `piezoPreview` 一次生成**全部实际变更**的集中、可滚动预览 → `piezoConfirm` 一次提交并形成单组 Undo → 全局 `Ctrl+S` 保存 → 重开后未改动实验用 `piezoRun` 加 `piezoConfirm` 两项主动操作启动 → 完成任务的认证结果直接关联 Accepted results → 同一来源导出 20,001 行完整 CSV。每一步记录点击／输入数、当前模型与工件身份、聚焦对象、预览前后值、是否需要重复选择文件。另测旧预览失效、Undo 后恢复、停止／恢复作用范围、磁盘与源篡改拒绝、并发任务误操作边界。当前代码中的全局 Stop 提示会停止所有求解任务，不能当作已验证的“停止选中压电任务”。

**2026-10-14 11:18 源码与局测状态：**集中预览现已按六页列出完整配方差异，缺少原生 `normalizedArguments` 时禁止确认；聚焦测试 3/3 通过。完成任务按精确 job ID 自动绑定已认证结果，真实小规模原生求解及篡改撤销聚焦测试 3/3 通过，故前版“修正只在隔离提案、未合入”的描述已过期。这些是当前工作树与局测证据，尚无同源安装版 UIA／视觉／三家族 20k 结论。新唯读审查发现 Run 确认页缺步数、步长、总时长和控制器摘要，零选／多选模型时缺工作台内直接返回 Models 的操作；首次 220 px 底部 dock 与 11 列贴片表的窄版可用性也待真窗口检查。原 `pc11_gui_uia.ps1` 覆盖来源打开、Run＋Confirm、接受结果载入和 CSV；未覆盖 PC-09 配置／保存／Undo／重开／停止恢复／篡改。最终必须在**同一新安装源冻结**下验证三家族与正常／窄窗口，并把原始 UIA、截图、任务／认证收据映射到对应 A01～A03。

**2026-10-14 12:13 修补与阻断更新：**源码已加入 `piezoChooseModel`，零选或多选时从工作台直接显示并聚焦 Models → Model 的现有树，不改 Session 选取；Run 集中确认在原生警告前显示保存模型／版本、FE 步数、`dt`、总时长及控制器。OFF 工作台整组测试在最后夹具清理前 47 PASS／0 FAIL／1 预期 SKIP，清理后的新增案例 3 PASS；独立 Sol medium 源码审查未见确定 P1/P2。真实主窗口视觉、窄窗布局和屏幕阅读器仍未测。另实际 OFF 链接图发现旧 Cloud AI 查询代码残留 P1，旧安装版即使清单 PASS 也不可用于产品 GUI 验收；须修复并在新包上重新准入。

**来源入口：**`src/fea/src/workflow_widget.cpp` 的 `workflowPages`，`src/fea/src/piezo_workbench_widget.cpp` 的 `piezoPages`／预览／结果，`src/fea/src/main_window.cpp` 的 dock，`src/fea/src/action.cpp` 的 Save／Undo／Stop，`src/fea/CMakeLists.txt` 的三项 GUI 测试，以及 `scripts/benchmark/pc11_gui_uia.ps1` 的现有安装版 UIA 故事。与性能门槛有关的独立合同见 `piezo_delivery_performance_contract.json`。
