# 2026-11-07-manual-gui · 不依赖助手的手动GUI全流程优化

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

沿M1–M7实测建模、配置、网格、求解、结果与交付，先取证再按摩擦修复。

## 执行到哪里

11月7日完成多类布局、参数、结果和压电优化。11月10–11日补齐探针、报告及项目模型目录。目录整理后续作从空白模型先画几何、后设分析。

## 问题／中断

额度中断、旧结果/关闭分量误读、探针stale-node、日志清空、项目目录缺入口，以及高DPI/网格/减振未闭合。

## 为什么调整、如何调整

复用同一具名计划，隔离真实桌面，保留失败；补来源/快照恢复与小窗；默认MPC几乎无减振按实记录。

## 调整后计划与状态边界

项目目录及前一批清理已交接。几何优先路径已实测，整体路线继续保留压电调参、反力平衡、候选网格及高 DPI 等验收项。

## 几何优先续作

- 新建和打开无分析任务的模型时直接进入工作区。Problem 面板提供 **Set up analysis**。任务编辑可取消，应用前检查模型是否仍在原修订以及是否有作业占用模型。
- 未配置任务时，启动求解先提示设置分析，省去提前选保存路径。多选时提示具体模型及单选操作。材料检查保留前面发现的错误。
- 以 `9c58d53` 为基线，在 Ubuntu 26.04.1、Qt 6.10.2 的开发工作区构建。隔离 XCB/OpenGL 实测空白模型、绘制 1 m 线段、取消任务编辑、添加模态任务、保存和重开，另核对 900×480 下的模型和分析面板。此次梁线模型用于操作回放，求解验收另行进行。
- 项目会话完整复跑 **52 项通过**。两条几何导出用例改为验证无强制弹窗，且未配置分析的导出模型可保存。验证使用当前含其他开发改动的工作区，提交仅含本续作差异。 [本机验收索引](../../../artifacts/manual-gui-workflow-audit-20261107/README.md)

## 先读与证据

- [GUI_WORKFLOW_OPTIMIZATION_PLAN.md](plans/GUI_WORKFLOW_OPTIMIZATION_PLAN.md)
- [GUI_WORKFLOW_OPTIMIZATION_HANDOFF_20261111.md](handoff/GUI_WORKFLOW_OPTIMIZATION_HANDOFF_20261111.md)
- [README.md](../../../artifacts/manual-gui-workflow-audit-20261107/README.md)

## 本轮实体材料

- plans · [GUI_WORKFLOW_OPTIMIZATION_PLAN.md](plans/GUI_WORKFLOW_OPTIMIZATION_PLAN.md)
- handoff · [GUI_WORKFLOW_OPTIMIZATION_HANDOFF_20261111.md](handoff/GUI_WORKFLOW_OPTIMIZATION_HANDOFF_20261111.md)

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
