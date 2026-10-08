# Agent playbook 索引

本目录是供外部 Agent **按任务检索**的操作导览，不是可执行权限、长期稳定 API 或测试通过的证明。接口真源依次为当前源码、可调用契约与测试；若本目录与源码冲突，先报告冲突，不要猜测后直接操作模型。版本和可用性以当前构建与 capability 查询为准。

| 任务 | 先读 | 再核对 |
| --- | --- | --- |
| 按全工作流方案继续开发、验收及中断续作 | [开发续作](workflow_delivery.md) | [交付方案](../WORKFLOW_DELIVERY_PLAN.md)、[状态清单](../workflow_delivery_plan.json)、`HANDOFF.md` 与规划校验器 |
| 使用当前 GUI/工作流/扩展 Agent | [当前操作手册](../AGENT_OPERATIONS.md) | 运行时 `range.host/v1` / `range.workflow/v1` capability、[Workflow SDK](../WORKFLOW_SDK.md)、[Extension SDK](../EXTENSION_SDK.md) |
| 配置、解释结构瞬态计算 | [结构动力学](structural_dynamics.md) | `src/range-model-lib/include/rml_structural_dynamics_setup.h`、`src/range-solver-lib/include/rstructural_dynamics_integrator.h` |
| 接进程内或进程外控制器 | [Stage 6 控制](stage6_control.md) | `src/range-solver-lib/include/rstructural_control_protocol.h`、`src/fea-solver/src/control_ndjson_transport.h` |
| 解释审计与交互记录 | [记录边界](interaction_records.md) | `src/range-base-lib/include/rbl_interaction_record_v1.h`、`src/fea-solver/src/execution_event_journal.h` |

Agent 工作方式：查询数值 `RCapabilityRegistry` 与运行时宿主/工作流能力；核对模型实例、接受版本、单位和来源，再分析或提出预览。当前已有 GUI 同源模态/Undo/Redo 命令、受监督工具回合、受限杆/梁控制及统计扩展。未开放的材料/载荷/网格/通用求解操作不能通过聊天、文档或私有 C++ 方法绕过；产品授权由确定性宿主校验。不要传递或记录 API 密钥、令牌和口令。

本目录的技术 playbook 保留对应版本边界，不是最新测试总数来源。当前基线和证据读 `DEVELOPMENT_PLAN.md`/`HANDOFF.md`；后续 WF 状态读 JSON。MATLAB 的已验证软件 TCP 路径见 [控制器适配器](../CONTROLLER_ADAPTERS.md)，旧 stdin 路径与硬件部署分别保留限制。开发流程文档属于仓库资料，产品随包知识的版本与检索另按 WF-08 验收。
