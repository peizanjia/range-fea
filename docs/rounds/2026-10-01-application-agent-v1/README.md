# 2026-10-01-application-agent-v1 · 共用应用服务、GUI/Agent与受控扩展v1

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

GUI/CLI/SDK/Agent共享事务，打通配置、预览批准、求解、结果及受信扩展。

## 执行到哪里

10月1日应用服务/必跑门禁；10月3日GUI生命周期、冻结输入、私有Python、真实Provider；10月4日对话布局与原生曲线，历史94→97→105→107。

## 问题／中断

额度中断后先收近断点，再获授权到Stage12；配置与日常操作混杂，旧响应/提案重入也需隔离。

## 为什么调整、如何调整

独立Assistant、底部控制工作区、设置迁移、追问/Send now与接受步遥测；有限v1不包装成任意自然语言操作。

## 调整后计划与状态边界

v1局部交付揭示完整流程缺口，后续拆成WF独立台账。

## 先读与证据

- [WORKFLOW_CONTRACT.md](design/WORKFLOW_CONTRACT.md)
- [GUI_INTERACTION_DESIGN.md](design/GUI_INTERACTION_DESIGN.md)
- [GUI_UX_ACCEPTANCE.md](acceptance/GUI_UX_ACCEPTANCE.md)
- [STAGE12_ACCEPTANCE.md](acceptance/STAGE12_ACCEPTANCE.md)

## 本轮实体材料

- design · [WORKFLOW_CONTRACT.md](design/WORKFLOW_CONTRACT.md)
- design · [GUI_INTERACTION_DESIGN.md](design/GUI_INTERACTION_DESIGN.md)
- acceptance · [STAGE12_ACCEPTANCE.md](acceptance/STAGE12_ACCEPTANCE.md)
- acceptance · [GUI_UX_ACCEPTANCE.md](acceptance/GUI_UX_ACCEPTANCE.md)
- packaging-tool · [build_private_python.py](../../../scripts/packaging/python-runtime/build_private_python.py)
- packaging-tool · [runtime-win-amd64.lock.json](../../../scripts/packaging/python-runtime/runtime-win-amd64.lock.json)
- packaging-tool · [test_private_python.py](../../../scripts/packaging/python-runtime/test_private_python.py)

## 起源于本轮、持续维护的手册

- [AGENT_OPERATIONS.md](../../reference/AGENT_OPERATIONS.md)（唯一持续正文，不复制为旧快照）
- [APPLICATION_SERVICE.md](../../reference/APPLICATION_SERVICE.md)（唯一持续正文，不复制为旧快照）
- [CONTROL_WORKFLOW.md](../../reference/CONTROL_WORKFLOW.md)（唯一持续正文，不复制为旧快照）
- [WORKFLOW_SDK.md](../../reference/WORKFLOW_SDK.md)（唯一持续正文，不复制为旧快照）
- [EXTENSION_SDK.md](../../reference/EXTENSION_SDK.md)（唯一持续正文，不复制为旧快照）

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
