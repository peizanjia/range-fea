# PC-09～PC-11 实现与验收缺口审计

**观察时间：**2026-10-14 01:12；**交互审查更新：**2026-10-14 07:44（Asia/Singapore）。本页是现行路径和局部执行结果的导航，不是阶段验收收据。冻结标准以 `../PIEZO_CONTROL_DELIVERY.md`、`../piezo_control_plan.json` 及 `piezo_delivery_performance_contract.json` 为准；九项均 `pending`，`productComplete=false`。

| 条目 | 已见实现或局部证据 | 当前缺口与最近下一步 |
|---|---|---|
| PC-09-A01 | `src/fea/src/piezo_workbench_widget.cpp` 绑定当前模型、拾取物理元件并配置器件／端口；`tst_piezo_workbench_widget.cpp` 和 experiment service 测试有局部检查 | 缺三家族当前私有安装 GUI 的真实拾取、读出配置、保存后物理 ID 不变收据；先在 v28 通过准入后分别操作杆、梁、板。 |
| PC-09-A02 | 工作台有集中预览、整套配置一次确认和 Undo／陈旧预览检查；旧 Release 工作台测试 41 PASS、1 SKIP | 缺当前安装窗口对模态、导出、算法和激励默认／高级参数同屏预览、一次批准与 Undo 的实际操作证据；重新跑最终同源 GUI 测试并采窗口轨迹。 |
| PC-09-A03 | 未修改的已保存配方才启用 Run，界面走 Run 预览和 Confirm；源码测试覆盖重开与两项操作 | 缺实际安装 GUI 的“运行＋确认”、无需手填映射／路径／代码 token 的点击与完成收据；新 v28 静态包尚未 GO。 |
| PC-10-A01 | `tst_piezo_pzt_prestress.cpp` 四根非平行 TRUSS1 真 PZT、非零 q0/V0/Q0/f0，独立支反力误差与合力残差约 `1.07e-14 N`；四条 LQR/MPC 自由／受迫 FE 轨迹各 66 步 `QUALIFIED`，八相位 RAM/RBM 行，Qt 14/14、CTest 1/1，`build/audit-20261014/pc10-prestress-multirod/receipt.json` 经独立 Sol 审查无 P1/P2 | 这是轴向有界代表算例，不等于完整安装窗口或任意多杆资格；最终全源码／链接库冻结、与三家族套件同源回归仍需完成。 |
| PC-10-A02 | `tst_piezo_delivery_examples.cpp` 编写杆／梁／板 × 无控制／PI／LQR／MPC × 自由／受迫 24 行、FE/ROM 重放与模态／控制证书；板／梁已有独立局部网格收敛证据 | v42 全量 CTest 在第 114 项开始时中断，当前 24 行尚无完整执行收据；先以当前 v44 Release 运行三家族有界 LQR 行，再跑完整目标及最终冻结全套。 |
| PC-10-A03 | GUI 有接受序列绘图、频谱、完整 CSV、保存重开和诊断路径，局部 Qt 测试可见 | 缺三家族实际安装窗口的 20,000 个接受 FE 步、初始记录、首图／CSV／内存及故障诊断；待 v28 静态审查技术 GO 后逐家族运行。 |
| PC-11-A01 | v44 Release 全目标构建退出 0；v42 私有安装 3167 文件、49 provider、静态字节一致。`../PIEZO_DEPENDENCY_PROVENANCE.md` 记录本地材料与缺口 | 根／子模块尚未最终冻结，234 项完整 CTest 未重跑。`libomp` 再分发、DirectX DLL 来源、`range-ai-lib` 授权及实际链接映射等许可证据待补；私有同机材料不能冒充可移交发布资格。 |
| PC-11-A02 | v42 私有安装静态身份通过；历史 v33 轴向 20k 虽完成，但 family 错配、首图 8.676 秒、合并工作集 517238784 bytes，属于无效／失败诊断 | v28 仍为静态 NO-GO；必须从新安装 GUI 真实运行杆／梁／板各 20k，按 5 秒首图、30 秒完整 20001 行 CSV、512000000 bytes 合计工作集／50 ms 采样逐项记录。 |
| PC-11-A03 | 冻结合同与软件／操作文档路径已定义 | 依赖前面全部验收通过、同源软件包／操作说明／来源许可与独立终审；现有局部绿色结果不能改 `productComplete`。 |

**执行顺序：**先让 v28 完成全部静态正负例和独立审查，取得根代理技术 GO 后运行真实 GUI；性能诊断期间不并发重负载回归。随后冻结源和安装包，完成当前同源必跑／兼容测试、独立终审与逐项许可映射；未满足的条目保持 pending。

**01:58 源码级交互审计补充（未启动窗口）。**`src/fea/src/piezo_workbench_widget.cpp` 目前分六个滚动页签，预览经一次命令提交并归入单组 Undo，未改动配方有 Run＋Confirm 路径；这些只能证明代码入口。集中预览面板固定最大高度 120 px，可能使器件绑定、端口角色、控制器和激励配置难以一次核对；Undo 在全局 Geometry 菜单/工具栏而不在工作台内；大表格在窄窗口的可读性未有安装版证据。`Run saved` 的启用条件按当前模型配方草稿是否修改判断，尚需核实与磁盘保存状态的合同语义；结果记录通过结果页文件选择器载入，尚未证实运行后自动呈现。实际三族与窄/宽窗口操作前，这些均记为待验证的交互风险，不作为已观察的视觉缺陷。

**02:18 旧 v33 性能归因检查（只读、非新资格）。**`build/audit-20261012/piezo-delivery/pc11-current-v33/outputs/axialRod-full/gui-performance.jsonl` 记录首图各阶段 `records_certified=8.1946158 s`、`dataset_ready=8.6518045 s`、`canvas_painted=8.6760589 s`，CSV `9.3348445 s`；该次 `qualification.json` 因 Wrong family 为 INVALID。画布从数据就绪到绘制约 24 ms，主要待量时间在同步 accepted trace 认证/读取和 GUI 来源检查；`src/fea/src/piezo_workbench_widget.cpp` 的 discover、读取后来源核对及 render 中 `acceptedSeriesCurrent(true)`、`src/range-application-lib/src/piezo_result_series.cpp` 的 prefix/末端摘要验证有多次摘要路径，但尚未量出各自耗时，不可直接删减完整性检查。v33 合并工作集峰值 517238784 bytes 出现在确认后约 13.454 s、求解器启动前，分项约 GUI 448389120、Python 60833792、conhost 8015872 bytes；现有证据不支持归咎于结果绘图。新运行应先保留分段计时/进程树样本，再针对实测热点优化。

**06:39 v33 `Wrong family` 定位（只读、非新资格）。**旧包 `pc11-current-v33/tools/pc11_gui_verify.py:90–98` 将 manifest 的 `axialRod` 与绘图/CSV 事件的旧格式 `axial` 逐字比较，首次在绘图终态第 95 行拒绝；旧事件与 CSV 首行都没有 `modelFamily`。这是验收器与事件的家族命名协议错配，不证明 FE 数值或三家族 GUI 资格。当前 `scripts/benchmark/pc11_gui_verify.py` 已有轴向旧名规范化及梁/板必须带 `modelFamily` 的拒错规则；下一干净包须固定该验收器字节，并由新安装程序实际写出可区分梁/板的身份。v33 只有轴向 manifest 和输出，无法推断梁/板是否存在相同故障。上述 8.676 秒与 517238784 bytes 仍各自未达门槛，即便修正命名也不能改写为通过。

**2026-10-14 07:44 GUI 交互源码独立复审（未启动安装窗口）。**`workflowPages` 实际为 11 个顶层页签，其中“Piezo control”占一个；另有压电内部 6 页。此前“11 个标准模块＋6 个压电页”应按实际导航核对，不能凭文字额外虚构第 12 个顶层页。Sol medium 复审确认 P1：`src/fea/src/piezo_workbench_widget.cpp::showPreview` 仅列固定摘要行，未展示将由整份 recipe 一次提交的端口角色/读出、支座、阻尼、激励及初始状态等变更；必须在确认前以可读的前后值完整呈现默认/高级字段，并补有代表性的 widget 验证。P2：Run 后只刷新任务列表，接受态图与 CSV 各自调用文件选择器；应按已注册完成任务的认证工件自动关联结果，并保留历史文件的显式打开入口及原来源/篡改拒绝。全局 Stop 会提示停止全部 solver、Undo 位于全局 Geometry；多任务/插入编辑的实际操作范围要在安装窗口验证，当前不把它们写成已发生的隐藏错误。GUI 可用性、完整预览与自动关联三个修正目前仅为隔离提案，未合入/构建/窗口实测；PC-09/11 项仍 pending。
