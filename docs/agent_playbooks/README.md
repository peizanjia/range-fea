# Agent playbook 索引

本目录是供外部 Agent **按任务检索**的操作导览，不是可执行权限、长期稳定 API 或测试通过的证明。接口真源依次为当前源码、可调用契约与测试；若本目录与源码冲突，先报告冲突，不要猜测后直接操作模型。版本和可用性以当前构建与 capability 查询为准。

| 任务 | 先读 | 再核对 |
| --- | --- | --- |
| 配置、解释结构瞬态计算 | [结构动力学](structural_dynamics.md) | `src/range-model-lib/include/rml_structural_dynamics_setup.h`、`src/range-solver-lib/include/rstructural_dynamics_integrator.h` |
| 接进程内或进程外控制器 | [Stage 6 控制](stage6_control.md) | `src/range-solver-lib/include/rstructural_control_protocol.h`、`src/fea-solver/src/control_ndjson_transport.h` |
| 解释审计与交互记录 | [记录边界](interaction_records.md) | `src/range-base-lib/include/rbl_interaction_record_v1.h`、`src/fea-solver/src/execution_event_journal.h` |

Agent 工作方式：先查询 `RCapabilityRegistry::getBuiltIn().toJson()` 所给能力及产品验证结果（`src/range-model-lib/include/rml_capability_registry.h`、`rml_structural_product_profile.h`）；确认模型 revision、单位、稳定目标 ID 与当前阶段；以只读查询解释现状，再产生待验证的操作建议。**目前没有统一的 Command Service/Query API 及 GUI 同源的 Agent 工具调用闭环**；不能把聊天指令、文档或 `RModel` 私有 C++ 方法当作已经授权的修改接口。高影响模型修改、自动编程和外部进程启动都需由宿主应用确认权限与范围。不要传递或记录 API 密钥、令牌和口令。

本目录不代替 `PROJECT_ROADMAP.md` 的 Stage 6–12 规划，也不表示所有规划功能已经实现。正在整合的代码须待独立构建、聚焦测试与全量回归通过后才可写成“可交付”。
