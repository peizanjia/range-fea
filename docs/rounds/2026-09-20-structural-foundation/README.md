# 2026-09-20-structural-foundation · 保留多物理平台、重点补结构基础

本页按实际目标归轮，不以集中提交日推断授权，不把历史状态当当前权限。

## 目标及来源

最初先做产品Stage0–2，采用成熟网格工具。

## 执行到哪里

可选structural profile、混合DOF、梁/面/体单元、质量、约束和版本化原生文件逐步交付。

## 问题／中断

曾拟只留结构功能；Gmsh C++ ABI和MSVC不兼容，Qt运行库部署也暴露缺口。

## 为什么调整、如何调整

明确保留原热/流/声/电磁能力；Gmsh改C API；按解析、patch和文件往返分增量验证。

## 调整后计划与状态边界

阶段能力保留，任意CAD/壳/非匹配面仍非已接受范围。

## 先读与证据

- [TECHNICAL_ROUTE_STAGE0_3.md](design/TECHNICAL_ROUTE_STAGE0_3.md)
- [TECHNICAL_ROUTE_STAGE4.md](design/TECHNICAL_ROUTE_STAGE4.md)

## 本轮实体材料

- design · [TECHNICAL_ROUTE_STAGE0_3.md](design/TECHNICAL_ROUTE_STAGE0_3.md)
- design · [TECHNICAL_ROUTE_STAGE4.md](design/TECHNICAL_ROUTE_STAGE4.md)

## 接续关系

[完整目标变更与并行工作线](../../history/WORK_HISTORY.md) · [逐文件来源/旧新路径映射](../../history/provenance.json)。各轮验收边界独立；未完成不等于废弃。
