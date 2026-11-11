# 2026-10-08-workflow-delivery · 全工作流WF-01至WF-14

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

以119条验收补项目/文档身份、求解结果、恢复、流式/会话、任务、调参、扩展、隔离和几何网格。

## 执行到哪里

10月8日先计划/校验，随后逐项实施；10月10日支持范围内10任务done，保留WF10–12 blocked和WF13 planned。

## 问题／中断

长图首图性能、真实科学生成及LPAC身份/隔离硬项失败；集成中有上下文/来源与字节码污染问题。

## 为什么调整、如何调整

用户允许记录并跳过受阻硬项、继续独立工作；不回退非隔离执行，不把局测拼成一次全绿。

## 调整后计划与状态边界

持续任务而非废案；从本轮机器计划和checkpoint定位未关闭条款。

## 先读与证据

- [WORKFLOW_DELIVERY_PLAN.md](plans/WORKFLOW_DELIVERY_PLAN.md)
- [workflow_delivery_plan.json](plans/workflow_delivery_plan.json)
- [WORKFLOW_INTEGRATED_SCOPE.md](acceptance/WORKFLOW_INTEGRATED_SCOPE.md)

## 本轮实体材料

- plans · [WORKFLOW_DELIVERY_PLAN.md](plans/WORKFLOW_DELIVERY_PLAN.md)
- plans · [workflow_delivery_plan.json](plans/workflow_delivery_plan.json)
- design · [CURRENT_MODEL_WORKFLOW.md](design/CURRENT_MODEL_WORKFLOW.md)
- design · [RECOVERY_CONTRACT.md](design/RECOVERY_CONTRACT.md)
- design · [WF10_SERIES_CONTRACT.md](design/WF10_SERIES_CONTRACT.md)
- acceptance · [WORKFLOW_INTEGRATED_SCOPE.md](acceptance/WORKFLOW_INTEGRATED_SCOPE.md)
- acceptance · [WF-01.md](acceptance/WF-01.md)
- acceptance · [WF-02.md](acceptance/WF-02.md)
- acceptance · [WF-03.md](acceptance/WF-03.md)
- acceptance · [WF-04.md](acceptance/WF-04.md)
- acceptance · [WF-05.md](acceptance/WF-05.md)
- acceptance · [WF-06.md](acceptance/WF-06.md)
- acceptance · [WF-07.md](acceptance/WF-07.md)
- acceptance · [WF-08.md](acceptance/WF-08.md)
- acceptance · [WF-09.md](acceptance/WF-09.md)
- acceptance · [WF-10.md](acceptance/WF-10.md)
- acceptance · [WF-11.md](acceptance/WF-11.md)
- acceptance · [WF-12.md](acceptance/WF-12.md)
- acceptance · [WF-14.md](acceptance/WF-14.md)
- acceptance · [integrated-20261010.json](acceptance/integrated-20261010.json)
- checkpoint · [wf03_wf04_checkpoint.md](checkpoints/wf03_wf04_checkpoint.md)
- checkpoint · [wf08_design_checkpoint.md](checkpoints/wf08_design_checkpoint.md)
- checkpoint · [wf09_design_checkpoint.md](checkpoints/wf09_design_checkpoint.md)
- checkpoint · [wf10_long_series_unclosed.md](checkpoints/wf10_long_series_unclosed.md)
- checkpoint · [wf11_provider_generation_unclosed.md](checkpoints/wf11_provider_generation_unclosed.md)
- checkpoint · [wf12_launcher_checkpoint.md](checkpoints/wf12_launcher_checkpoint.md)
- checkpoint · [wf12_lpac_unclosed.md](checkpoints/wf12_lpac_unclosed.md)
- checkpoint · [wf14_mesh_checkpoint.md](checkpoints/wf14_mesh_checkpoint.md)
- checkpoint · [wf14_mesh_gui_checkpoint.md](checkpoints/wf14_mesh_gui_checkpoint.md)
- checkpoint · [wf14_rebind_checkpoint.md](checkpoints/wf14_rebind_checkpoint.md)
- validation-tool · [measure_wf10_series.py](../../../scripts/benchmark/wf10-series/measure_wf10_series.py)
- validation-tool · [test_wf12_lpac_smoke.py](../../../scripts/benchmark/wf12-isolation/test_wf12_lpac_smoke.py)

## 起源于本轮、持续维护的手册

- [PROJECT_CATALOG.md](../../reference/PROJECT_CATALOG.md)（唯一持续正文，不复制为旧快照）
- [NATIVE_MODEL_OPERATIONS.md](../../reference/NATIVE_MODEL_OPERATIONS.md)（唯一持续正文，不复制为旧快照）
- [NATIVE_DOCUMENT_SDK.md](../../reference/NATIVE_DOCUMENT_SDK.md)（唯一持续正文，不复制为旧快照）
- [NATIVE_RESULTS.md](../../reference/NATIVE_RESULTS.md)（唯一持续正文，不复制为旧快照）
- [CONVERSATIONS.md](../../reference/CONVERSATIONS.md)（唯一持续正文，不复制为旧快照）
- [TASK_PLANS.md](../../reference/TASK_PLANS.md)（唯一持续正文，不复制为旧快照）
- [RECOVERY_OPERATIONS.md](../../reference/RECOVERY_OPERATIONS.md)（唯一持续正文，不复制为旧快照）

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
