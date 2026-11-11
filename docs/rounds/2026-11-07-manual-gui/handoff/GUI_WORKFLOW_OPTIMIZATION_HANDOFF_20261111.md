# GUI 全流程优化：2026-11-11 批次交接与清理

## 当前状态

用户要求恢复额度耗尽前的断点，完成当前工作后整理退出。本次完成**项目模型目录、登记源打开、已打开模型复用、迁移冻结快照恢复及拒绝反馈**的收尾；没有把整个GUI优化路线标为完成。没有提交、推送或修改PC/WF/CW验收台账。

原具名计划为`.planning/2026-11-07-manual-gui-workflow-audit/`。总方案见[手动全流程优化方案](../plans/GUI_WORKFLOW_OPTIMIZATION_PLAN.md)，当前源码操作说明见[项目目录](../../../reference/PROJECT_CATALOG.md)。既有未提交源码、子模块和其他任务证据均保留。

## 恢复到的真实断点

计划最后记录“增加项目模型目录”，但本机`project-browser/`已有实现、15:23构建、8项通过的聚焦回归和P系列真实截图，尚未补记到总台账。恢复时还发现该任务隔离Xvfb和旧GUI，后者运行已被最终链接替换的执行档inode。本轮正常退出旧GUI，用当前构建、新的仓内隔离配置和工作区副本补验，最终GUI也正常退出0，所属Xvfb停止，Terminal标签关闭。

源码入口为`src/fea/src/project_documents_dialog.*`、`main_window.cpp`及`Session::openProjectDocument`。保存源按登记来源链、修订及哈希核对；干净源沿用持久身份，已打开模型复用实例；dirty登记项的保存文件和冻结快照按各自规则生成独立副本。失效、锁冲突和篡改不作为成功打开。

## 本轮验证与边界

- 完整项目会话套件最终 **52 passed / 0 failed / 0 skipped，4.490s，exit0**，含Qt初始化/清理。执行前后相关源码及GUI/测试执行档SHA-256一致。
- 首次完整回归51通过/1失败，原日志保留。测试原先在整个JSON中搜索`1e-`子串；本轮改为完整JSON字符串匹配，并加固定身份碰撞与真实草稿泄漏正反对照，再重编、完整复跑。不更改产品草稿权限或持久化要求。
- 最终真实XCB/OpenGL窗口实测：项目菜单→已登记模型；登记梁载入；Show open model复用入口；缺失源明确反馈；900×480主窗/680×350目录；相对冻结快照随工作区迁移后打开独立副本。
- 旧P-16/P-17截图名称不准确反映捕获状态，原件保留；最终布局和恢复证据采用实际查看过的C-13/C-14。
- 未重跑完整产品CTest、Windows、干净安装或整个M1–M7组合；本批次不证明控制减振性能或任意模型恢复。

[本机完整批次索引](../../../../artifacts/manual-gui-workflow-audit-20261107/project-browser/README.md)包含执行日志、哈希收据、模型/工作区和最终截图链接；[总优化汇总](../../../../artifacts/manual-gui-workflow-audit-20261107/OPTIMIZATION_SUMMARY.md)覆盖此前24类及后续优化。`artifacts/`和`.planning/`是本机忽略目录，不随普通clone迁移，异机交接需要另带必要工件。

## 实际清理与保留

清理已完成，不是待执行建议：

| 类别 | 实际内容 | 逻辑字节 |
|---|---|---:|
| pip下载缓存 | `artifacts/local-test/pip-cache/`；不含安装环境 | 52,889,422 |
| 下载/提取工具 | 当前任务`project-browser/tools/`内Debian包、Xvfb、xdotool及其文档 | 3,375,467 |
| Qt/Mesa/字体缓存及过期认证 | 主任务各隔离profile的cache、两份过期X显示认证 | 30,982,247 |
| **合计** | **719个文件/链接** | **87,247,136（83.21 MiB）** |

这些文件原占用89,096,192 bytes（84.97 MiB）文件系统块；不是整盘free变化测量。删除时逐项核对哈希、范围、跟踪状态和运行进程，软链接只删除链接本身。删除前后校验本任务1552个保留文件，0处缺失或变化。逐文件清单在[清理结果](../../../../artifacts/manual-gui-workflow-audit-20261107/project-browser/closeout/cleanup-result.json)。

另外归档而未删除：

- 根目录`.-mxs-CG.`：634 bytes残差数据，移至当前批次`closeout/recovered/root-stray-convergence.cvg`，保留字节及原mtime。
- 主任务`tmp/inspect-load.cpp`：2075 bytes诊断源，移至`diagnostics/inspect-load.cpp`。

**保留：**唯一可复用当前构建`build/linux-review`、两个相互依赖的科学Python前缀、源码和用户配置、原始模型/CSV/PDF/截图、失败日志、快照及历史验收证据。旧profile和失败runtime经逐成员SHA核验后存入`artifacts/archive/`，不是丢弃；当前模型链和最新profile保持原路径。未清理`.idea/`。被清理的GUI工具与认证须在下次实测前重新准备，旧PID/认证不可复用。

### 追加的目录级整理

用户进一步要求整理`artifacts/`、`docs/`和`scripts/`，允许删除确认的废案并建立索引。本次分类后：

- `artifacts/`新增[总索引](../../../../artifacts/README.md)，主GUI任务新增[目录导航](../../../../artifacts/manual-gui-workflow-audit-20261107/README.md)，历史材料统一进入[压缩归档索引](../../../../artifacts/archive/README.md)。
- `docs/`新增[验收索引](../../../history/ACCEPTANCE_INDEX.md)和[证据合同索引](../../2026-10-12-piezo-delivery/evidence/README.md)，总索引补审计、研究、复盘和接管入口；`scripts/`新增[工具总索引](../../../../scripts/README.md)及[benchmark导航](../../../../scripts/benchmark/README.md)。修正研究README本机绝对链接、历史运行语气与v43模板文件名，未移动被CMake/CI/哈希合同引用的文件。本次未找到可确定为废案且无有效引用的受维护文档或脚本，因此未删除其源码。
- 9组不活跃GUI profile、2组失败临时运行根及非FEA的本地配置备份，共282个原文件、3,428,786 bytes，归档为3个包共545,295 bytes；每个文件大小/SHA一致后才移除展开目录。当前有来源身份绑定的模型、最新配置和运行时不移动。
- 删除两个无当前调用的旧辅助执行档：`build/axial-trace-verifier/test`（142,240 bytes，正常CMake测试目标保留）和`artifacts/piezo-agent-20261105/deepseek_tool_smoke`（1,138,208 bytes，C++源保留）。另移除353个空目录，包括已无内容的`artifacts/local-test/`、`artifacts/comsol-report-render-20261105/`和废弃辅助构建目录。
- **永久删除合计721个文件/链接、88,527,584 bytes（84.43 MiB）逻辑大小**；归档文件不计永久删除。前述清理阶段的保留哈希核验与随后的归档是两个步骤，分别有收据。[目录整理收据](../../../../artifacts/manual-gui-workflow-audit-20261107/project-browser/closeout/repository-organization.json)记录实际删除、空目录和归档。
- `artifacts/python-tests`虽名含tests，却实际提供当前PZT运行时所用NumPy/SciPy，不能删；`piezo-current-20261105/python`是CMake正在引用的解释器，也不移动。没有为了扩大清理量误删有效依赖。

最终核验：14份新建/更新导航文档共355处本地链接，0断链；3个归档包282成员逐项SHA一致。最初1552个保留文件中1270原路径不变、280经归档保留、2份总台账明确更新，无遗失；8个相关源码/执行档身份与最终52项回归收据一致。无产品、solver、Xvfb或构建残留进程。详情见[最终整理核验](../../../../artifacts/manual-gui-workflow-audit-20261107/project-browser/closeout/final-organization-check.json)。

## 暂停后的下一步

本批次已收尾，按用户要求停止。整体路线仍保留：压电闭环调参和同口径减振指标、反力平衡/原生探针、Linux候选网格全链、高DPI/键盘与安装冷启动等。工作区迁移快照手动恢复已测，**应用启动时自动恢复上次工作区仍未验收**。若以后继续，沿用原计划和既有证据，不重新建立另一套GUI优化台账。
