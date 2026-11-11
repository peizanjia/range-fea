# 证据合同与审计矩阵导航

本目录主要是验收合同、规范和整理后的差距矩阵，**不是“每个JSON都表示已通过”的原始运行目录**。一些字段被计划校验器按路径/SHA引用，保持原文件名和内容边界；真实日志、截图、模型在对应本机run中。

| 文件 | 角色 |
|---|---|
| [piezo_delivery_performance_contract.json](piezo_delivery_performance_contract.json) | PC性能/记录数/首图/CSV等合同；读取后续supersedes修订，不凭旧阈值评价新运行。 |
| [piezo_representative_cases_spec.json](piezo_representative_cases_spec.json) | 杆/梁/板代表算例规范；不等同于已实测收据。 |
| [piezo_dependency_provenance.json](piezo_dependency_provenance.json) | 依赖来源材料，与[来源说明](../design/PIEZO_DEPENDENCY_PROVENANCE.md)配套；不自动替代发行许可审查。 |
| [piezo_gui_operation_acceptance_matrix.md](piezo_gui_operation_acceptance_matrix.md) | 历史安装版GUI操作脚本、对象名及后续修补/缺测；不是所有窗口已经通过的证书。 |
| [piezo_incremental_checkpoint_20261013.json](piezo_incremental_checkpoint_20261013.json) | 当时的增量检查点，保留其版本和未完成项。 |
| [piezo_pc04_08_gap_audit.md](piezo_pc04_08_gap_audit.md) | PC04–08范围差距审计，不能替代新源码对应实测。 |
| [piezo_pc09_11_gap_audit.md](piezo_pc09_11_gap_audit.md) | GUI/科学整合/安装终验差距，结合[PC台账](../../../piezo_control_plan.json)读取。 |

[验收摘要](../../../history/ACCEPTANCE_INDEX.md)、[文档总索引](../../../README.md)、[最新手动GUI批次](../../2026-11-07-manual-gui/handoff/GUI_WORKFLOW_OPTIMIZATION_HANDOFF_20261111.md)分别负责已运行范围、导航和当前续作，不创建另一套重复状态表。本次整理没有改写本目录原合同或重钉旧证据哈希。
