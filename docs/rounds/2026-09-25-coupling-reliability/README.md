# 2026-09-25-coupling-reliability · 可靠性、拉索压电、热结构与降阶

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

先审数值可信性，再扩拉索、压电、热结构、工作点/ROM与控制分析。

## 执行到哪里

有范围的静力、二阶/准静态控制、RAM/RBM、热结构事务和真实四杆独立参考逐步记录，最终历史91/91。

## 问题／中断

未收敛结果发布、缩尺/质量与热反力问题；MATLAB三种stdin握手失败；干净机权限受阻。

## 为什么调整、如何调整

修数值发布门禁；MATLAB改Java TCP+Python NDJSON桥并实测；用户暂缓系统权限事项而不假称通过。

## 调整后计划与状态边界

一般耦合、曲壳/非匹配面、硬实时和降阶全频精度不扩大；进入应用服务/GUI。

## 先读与证据

- [TECHNICAL_ROUTE_STAGE7.md](design/TECHNICAL_ROUTE_STAGE7.md)
- [TECHNICAL_ROUTE_STAGE8.md](design/TECHNICAL_ROUTE_STAGE8.md)
- [TECHNICAL_ROUTE_STAGE9.md](design/TECHNICAL_ROUTE_STAGE9.md)
- [RELIABILITY_AUDIT.md](acceptance/RELIABILITY_AUDIT.md)
- [STAGE0_8_AUDIT_20260926.md](acceptance/STAGE0_8_AUDIT_20260926.md)

## 本轮实体材料

- design · [TECHNICAL_ROUTE_STAGE7.md](design/TECHNICAL_ROUTE_STAGE7.md)
- design · [TECHNICAL_ROUTE_STAGE8.md](design/TECHNICAL_ROUTE_STAGE8.md)
- design · [TECHNICAL_ROUTE_STAGE9.md](design/TECHNICAL_ROUTE_STAGE9.md)
- acceptance · [RELIABILITY_AUDIT.md](acceptance/RELIABILITY_AUDIT.md)
- acceptance · [STAGE0_8_AUDIT_20260926.md](acceptance/STAGE0_8_AUDIT_20260926.md)
- acceptance · [STAGE9_CONTROL_ANALYSIS.md](acceptance/STAGE9_CONTROL_ANALYSIS.md)

## 起源于本轮、持续维护的手册

- [CONTROLLER_ADAPTERS.md](../../reference/CONTROLLER_ADAPTERS.md)（唯一持续正文，不复制为旧快照）
- [TYPED_FE_CONTROL.md](../../reference/TYPED_FE_CONTROL.md)（唯一持续正文，不复制为旧快照）

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
