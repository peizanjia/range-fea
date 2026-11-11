# 2026-09-24-dynamics-control · 二阶结构动力学与接受步控制

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

旧一阶瞬态不足以支持惯性振动，先被动二阶动力学、再控制器。

## 执行到哪里

Newmark/广义alpha/中心差分、q/v/a、能量功账、重启与C++/Python实际FE控制；历史49/49到53/53。

## 问题／中断

约束奇异质量、接受态重复推进、旧对象/OpenMP和性能成本。

## 为什么调整、如何调整

约束消元；状态和功账按接受交易推进；清洁串行重建；不将测量等同普遍加速。

## 调整后计划与状态边界

性能和MATLAB stdin当时仍有边界；后续控制适配方案在下一轮调整。

## 先读与证据

- [TECHNICAL_ROUTE_STAGE5.md](design/TECHNICAL_ROUTE_STAGE5.md)
- [TECHNICAL_ROUTE_STAGE6.md](design/TECHNICAL_ROUTE_STAGE6.md)
- [AGENT_INTERFACE_DESIGN.md](design/AGENT_INTERFACE_DESIGN.md)
- [STAGE6_PERFORMANCE_AUDIT.md](design/STAGE6_PERFORMANCE_AUDIT.md)

## 本轮实体材料

- design · [TECHNICAL_ROUTE_STAGE5.md](design/TECHNICAL_ROUTE_STAGE5.md)
- design · [TECHNICAL_ROUTE_STAGE6.md](design/TECHNICAL_ROUTE_STAGE6.md)
- design · [AGENT_INTERFACE_DESIGN.md](design/AGENT_INTERFACE_DESIGN.md)
- design · [STAGE6_PERFORMANCE_AUDIT.md](design/STAGE6_PERFORMANCE_AUDIT.md)

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
