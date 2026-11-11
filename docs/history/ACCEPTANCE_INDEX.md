# 验收记录导航

本目录保留可随源码交接的验收摘要、范围冻结和审查记录，不存构建树/大日志。日期和PASS只对记录中的源、包、平台和范围有效。当前状态仍由各工作流机器台账及明确的新收据决定；旧记录不按年龄删除，也不据此升级现有脏工作树。

| 分类 | 入口与系列 | 使用方法 |
|---|---|---|
| 源码/课程交接 | [phase-checkpoint-20261030.md](../rounds/2026-10-30-source-handoff/acceptance/phase-checkpoint-20261030.md)、[source-provenance-license-inventory-20261030.md](../rounds/2026-10-30-source-handoff/acceptance/source-provenance-license-inventory-20261030.md)、[deletion-handoff-20261030.md](../rounds/2026-10-30-source-handoff/acceptance/deletion-handoff-20261030.md) | 保留阶段边界、来源/许可和删除交接，不代表产品终验。 |
| 通用工作流 | [集成清单](../rounds/2026-10-08-workflow-delivery/acceptance/integrated-20261010.json)、[WF-01](../rounds/2026-10-08-workflow-delivery/acceptance/WF-01.md)至[WF-12](../rounds/2026-10-08-workflow-delivery/acceptance/WF-12.md)、[WF-14](../rounds/2026-10-08-workflow-delivery/acceptance/WF-14.md) | 逐任务查看实测/未关闭条款；唯一状态表是[workflow_delivery_plan.json](../rounds/2026-10-08-workflow-delivery/plans/workflow_delivery_plan.json)。没有WF-13验收文件不等于WF-13通过。 |
| 控制工作台 | [CW-01](../rounds/2026-10-11-control-workbench/acceptance/CW-01.md)、[追踪报告](../rounds/2026-10-11-control-workbench/acceptance/CW-01-tracking-report.json)、[CW-02](../rounds/2026-10-11-control-workbench/acceptance/CW-02.md)、[CW-06-A01](../rounds/2026-10-11-control-workbench/acceptance/CW-06-A01.md)、[动力核心](../rounds/2026-10-11-control-workbench/acceptance/CW-dynamics-core-20261011.md)、[混合模型](../rounds/2026-10-11-control-workbench/acceptance/CW-native-mixed-20261011.md) | 与[CW台账](../rounds/2026-10-11-control-workbench/plans/control_workbench_plan.json)对应；同名JSON是机器证据，不是应删除的重复文档。 |
| PC00冻结与审查 | [早期审查](../rounds/2026-10-12-piezo-delivery/acceptance/PC-00-review-20261012.md)、[A01冻结](../rounds/2026-10-12-piezo-delivery/acceptance/PC-00-A01-freeze-20261020.md)、[A01独审](../rounds/2026-10-12-piezo-delivery/acceptance/PC-00-A01-independent-review-20261020.md)、[A02/A03独审](../rounds/2026-10-12-piezo-delivery/acceptance/PC-00-A02-A03-independent-review-20261020.md) | 保留冻结、git状态文本和独审，按[PC台账](../piezo_control_plan.json)核对，不沿用冲突的旧计数。 |
| 当前Linux科学切片 | [PZT-fullchain-20261105.md](../rounds/2026-11-05-pzt-qualification/acceptance/PZT-fullchain-20261105.md) | 原生数值/恢复/CSV资格，不是GUI、安装或全PC产品资格。 |

完整文档分类见[docs总索引](../README.md)。契约、代表算例规范和操作矩阵另见[证据导航](../rounds/2026-10-12-piezo-delivery/evidence/README.md)。大工件只在本机`artifacts/`，新clone缺失原日志必须明确说明，不改写历史摘要弥补。
