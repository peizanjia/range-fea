# Stage 10C–12 本地验收记录

日期：2026-10-03。本文记录本轮实际证据，历史 Stage 0–9 的物理支持范围仍以当前开发计划和各能力文档为准。

## 交付范围

- `range.host/v1`：GUI 文档身份、接受版本、上下文、模态命令预览/执行、Undo/Redo、有界事件和操作查询。尚未迁移的旧编辑入口不自动成为 Agent 工具。
- GUI Workflow / Agent tasks / Extensions：共用异步工作流服务，修改后刷新接受状态；任务期间无需重启。求解提交时冻结输入，结束时只向相同文档版本发布，过期结果保留为独立工件。
- `range.workflow/v1`：四杆与平面 BEAM1 控制实验、实际 FE 主动/被动求解、续算、独立参考、取消及恢复；工作区与打开的 GUI 文档分别标识。
- `range.extension/v1`：数值序列样本 RMS、绝对峰值和最早峰值时间扩展；源码、manifest、测试、验证报告和运行环境绑定哈希，候选验证后批准激活，可回退。宿主生成输入表单与带单位结果表。
- 私有 CPython 3.13.5、NumPy 2.2.6、SciPy 1.15.3；固定下载哈希、完整文件清单和许可证，外部 SDK 示例随安装树交付。

## 已执行的专项验收

最终全目标 Release 构建成功；必跑清单 **105/105**；完整 CTest **105/105、0 失败、249.47 s**。构建启用 `RANGE_BUILD_TESTS=ON`、`RANGE_REQUIRE_NUMERIC_TESTS=ON`，没有以缺失数值依赖减少测试注册。远程模型、原生视口及保留生成工作区用例在普通 CTest 中默认跳过，已分别通过下列显式验收；不把默认跳过当作实际调用。

| 项目 | 实际证据 |
| --- | --- |
| DeepSeek 修改 GUI | 真实 `deepSeekAppliesLiveGuiCommand` 通过；3 次响应、8740 tokens；查询后修改所选文档模态迭代次数，验证接受状态和 Undo |
| DeepSeek 生成新扩展 | 真实 `deepSeekGeneratesValidatedExtension` 通过；6 次响应、51274 tokens；模型生成 `acceptance.generated` 源码和测试，宿主独立校验、批准激活及运行；样例 `[3,4]` 得到 RMS `3.5355339059327378`、峰值 `4 m`、峰值时间 `1 s` |
| 生成扩展的 GUI 使用 | `generatedExtensionRunsThroughGui` 对保留的真实生成版本，通过 Extensions 面板选择版本、填写序列、预览、批准和结果表核对；未用假模型回复替代该源码 |
| 求解快照 | `tst_solver_snapshot` 通过；真实求解、排队取消、输入哈希、过期结果保护及冷启动审计 |
| 文档事件面板 | `tst_agent_event_view` 通过；300 个事件溢出后的重同步、后续操作显示、锁定时缓存到解锁后实时状态自动恢复，任务草稿/焦点/密钥保持且不进入记录 |
| 扩展 GUI 生命周期 | `tst_extension_widget` 通过；真实服务验证/激活/运行/新版本/回退、分页刷新选择保留、轮询保留预览与数值结果、独立参考时程 provenance |
| 窗口渲染 | Windows 平台实际 OpenGL framebuffer 有效，截图已检查；不是以 offscreen 测试代替渲染 |
| 安装版 SDK | 仅安装 bin 和 Windows 系统目录 PATH，随包解释器运行 `examples/compare.py --run`；梁与四杆各 20 步主动/被动实际 FE 与独立参考通过 |
| 安装版扩展运行时 | 真实私有解释器核验 2468 个文件（清单 319241 bytes）；stage/validate/activate/run 均成功，1 个候选测试及 3 个独立宿主样例通过，活动版本 runtimeHash 与重新强制核验结果一致 |
| 安装树 GUI 终验 | 缩减 PATH、Windows 平台及随包 Python，运行同源 Application/MainWindow 验收程序：OpenGL 渲染、GUI 梁 20+20 步续算、真实生成扩展的 GUI 预览/批准/结果表全部通过；QtTest 共 5/5（含初始化/清理），58.734 s；临时验收程序和 Qt6Test.dll 随后移除 |
| 必跑清单 | 注册与必跑项均为 105，9 组；清单校验通过，完整测试结果另记 |

安装版梁主动终态最大位移/速度误差为 `2.1142e-17 m / 7.0690e-16 m/s`，转角/角速度误差为 `1.0192e-17 rad / 1.6445e-15 rad/s`。这只证明该模板、步长 0.005 s、20 步的路径。

同一安装解释器下，四杆 20 步主动终态最大位移/速度误差为 `4.1241e-10 m / 6.0150e-9 m/s`，通过既定工作流数值门槛；原始结果为 `stage12-installed-sdk-rod.json`。

安装扩展验收使用实际安装 bin + Windows System32/Windows 的 PATH，解释器 `isolated=1`、`ignore_environment=1`、`no_site=1`、`safe_path=true`，并核对模块路径。示例结果 RMS `3.696845502136472 m/s`，峰值 `4 m/s`、时间 `1 s`。作业时间：stage 0.023 s、validate 25.847 s、activate 3.972 s、run 24.428 s；完整环境核验另测 9.915 s。这些时间包括完整文件核验开销，属于该机器的验收观察，不是性能承诺；计算仍在进程外异步进行。

原始日志、截图与工作区位于忽略的 `build/audit-20261003/`；安装树为 `build/stage12-install/`。生成扩展保存在 `deepseek-extension-qCblqy/`（开发解释器验证），同一模型生成的源码/测试另在 `installed-generated-extension/` 用随包解释器重新验证、激活并运行通过。打开工作区时必须使用与验证匹配的运行环境；换环境不能沿用旧验证。密钥只从本机忽略文件读取，没有写入产品、提交或验收日志。已通过调用的 token 数不包含失败解析响应的未知用量。

远程验收不是固定输出的单元测试：首次扩展请求被严格工具调用检查拒绝，补充分类型诊断后第二次通过；未记录首次原始回复，因此不追认具体字段原因，且没有放宽参数或批准边界以迎合模型。产品在非法协议/参数时明确终止任务，用户可以查看诊断后调整目标并开始新任务。已接受的作业不会因重新对话被隐式重复提交。

## 保留边界

- 扩展执行仅受信开发者模式；进程/超时/环境清理不提供文件、网络或注册表的 OS 沙箱。普通模式拒绝验证及运行生成代码。
- 首版是固定数值序列统计接口，不含任意新 FE 单元、材料、C++ ABI、任意 QWidget、依赖安装或通用参数扫描插件。
- 扩展的参考时程来自独立 Newmark 回放，不能称为原生求解器逐步输出。普通 GUI SolverTask 不自动配置外部控制器；控制实验从 Workflow 入口运行。
- 文档事件环和操作账本在内存中；持久化 InteractionRecord 和工作区记录另行提供，不能宣传崩溃后的全应用 exactly-once。
- 大模型和源码快照的规模性能、干净外部 Windows、远程 CI、实物硬件、升级迁移仍属于独立质量验收。小模型快照在 GUI 线程完成，不承诺大模型零停顿。

## 审查与复跑索引

Luna max 独立审查覆盖供应商/任务授权、GUI 生命周期、求解结果发布、扩展运行时及事件面板。确认的问题均已修复并回归，最后复审没有新增重大问题。重点修复包括历史复制丢失、取消发布竞态、停止期间异步回调、扩展候选选择丢失、轮询使预览失效、刷新清空结果，以及事件缺口后的缓存状态不再恢复。

- `stage12-final-review-build.log`：最终全目标构建。
- `stage12-required-tests.log` / `stage12-inventory.json`：必跑清单。
- `stage12-final-ctest.log`：最终 105 项回归。
- `stage11-deepseek-gui.txt` / `stage12-deepseek-extension-retry.txt` / `stage12-generated-gui.txt`：真实模型与 GUI 链路。
- `stage12-installed-sdk.json` / `stage12-installed-sdk-rod.json`：安装私有解释器下的两个实际 FE 模板。
- `installed-extension/acceptance.json` / `installed-generated-extension.json`：完整环境核验与真实生成源码在安装运行时的再验证。
- `stage12-installed-gui.txt` / `stage12-installed-render.png` 及 `.panel-6/7/8.png`：最终安装树的窗口/工作流/生成扩展终验与截图；正式 `fea.exe --help` 也单独通过。

上述文件均相对于 `build/audit-20261003/`，属于本机原始验收材料。产品文档和可复跑测试在仓库中，密钥和临时工作区不入库。
