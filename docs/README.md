# 文档入口：按目标接管，按来源追溯

[先读详细工作复盘](history/WORK_HISTORY.md)：目标怎么提出、执行到哪、为什么调整、后来如何计划。[历史/轮次索引](history/README.md)与[逐文件来源和旧新路径](history/provenance.json)是本次分类依据。

## 从哪里开始

| 需求 | 入口 |
|---|---|
| 追溯一次目标或交互轮次 | [轮次目录](history/README.md) |
| 当前接口、SDK和GUI操作 | [持续手册](reference/README.md) |
| 跨轮产品路线、CI和开发指南 | [开发导航](development/README.md) |
| 文件保存、清理、分支和提交推送 | [仓库维护规范](development/REPOSITORY_MAINTENANCE.md) |
| 最近手动GUI及当前文件整理 | [手动GUI](rounds/2026-11-07-manual-gui/README.md)、[目录整理](rounds/2026-11-11-repository-organization/README.md) |
| 本机证据与脚本 | [artifacts索引](../artifacts/README.md)、[scripts索引](../scripts/README.md) |

## 目标轮次

- [2026-09-17-platform-baseline · 平台可计算基线](rounds/2026-09-17-platform-baseline/README.md)
- [2026-09-20-structural-foundation · 保留多物理平台、重点补结构基础](rounds/2026-09-20-structural-foundation/README.md)
- [2026-09-24-dynamics-control · 二阶结构动力学与接受步控制](rounds/2026-09-24-dynamics-control/README.md)
- [2026-09-25-coupling-reliability · 可靠性、拉索压电、热结构与降阶](rounds/2026-09-25-coupling-reliability/README.md)
- [2026-10-01-application-agent-v1 · 共用应用服务、GUI/Agent与受控扩展v1](rounds/2026-10-01-application-agent-v1/README.md)
- [2026-10-08-workflow-delivery · 全工作流WF-01至WF-14](rounds/2026-10-08-workflow-delivery/README.md)
- [2026-10-11-control-workbench · 统一结构控制工作台CW](rounds/2026-10-11-control-workbench/README.md)
- [2026-10-12-piezo-delivery · 完整压电PC交付与20k产品链](rounds/2026-10-12-piezo-delivery/README.md)
- [2026-10-21-trusted-computation · 课程可信计算Agent原0–8阶段](rounds/2026-10-21-trusted-computation/README.md)
- [2026-10-30-source-handoff · 源码冻结、依赖来源和跨设备交接](rounds/2026-10-30-source-handoff/README.md)
- [2026-11-03-takeover · 新设备复盘与接管理解](rounds/2026-11-03-takeover/README.md)
- [2026-11-04-source-audit · 源码缺陷与数值/打包续审](rounds/2026-11-04-source-audit/README.md)
- [2026-11-05-comsol-agent-research · COMSOL全流程Agent调研](rounds/2026-11-05-comsol-agent-research/README.md)
- [2026-11-05-pzt-qualification · 当前Linux PZT科学/恢复资格](rounds/2026-11-05-pzt-qualification/README.md)
- [2026-11-06-piezo-agent-integration · 压电Agent与真实原生FE接续](rounds/2026-11-06-piezo-agent-integration/README.md)
- [2026-11-07-residual-repair · 残差驱动诊断与修复路线提案](rounds/2026-11-07-residual-repair/README.md)
- [2026-11-07-manual-gui · 不依赖助手的手动GUI全流程优化](rounds/2026-11-07-manual-gui/README.md)
- [2026-11-11-repository-organization · 按历史目标实体整理仓库](rounds/2026-11-11-repository-organization/README.md)

## 留在目录外的明确例外

- [PIEZO_CONTROL_DELIVERY.md](PIEZO_CONTROL_DELIVERY.md)、[piezo_control_plan.json](piezo_control_plan.json)：v43 source scope精确路径输入及合同SHA边界，保留唯一位置，不造副本。PC轮次页引用它们。
- `docs/README.md`：唯一文档入口；`reference`是跨轮维护手册，`history/logs`是跨轮原始日志，均不是漏分类。
- 根`README.md`、`AGENTS.md`、`LICENSE`及Git/IDE必需配置：分别为工程入口、自动读取约束、标准许可与工具配置。根技术路线/长日志/交接已实际搬走，不保留逐文件跳转页。

## 证据与使用规则

普通fresh clone不含本机artifacts/.planning；旧记录中原平台路径是历史线索，不冒充本机文件。14份SHA绑定材料原字节不变，现台账仅更新定位。禁止拼接不同源/平台测试为全绿，禁止借整理重新签旧通过。新工作继续使用具名计划和仓内独立run；新增文档放所属目标轮次或持续手册，不回到docs/根散放。
