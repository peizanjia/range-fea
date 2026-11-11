# 交互记录与运行事件边界

目前有两个不同层级，不能混称：

1. `ExecutionEventJournal` 是 `fea-solver` 的追加式 UTF-8 JSONL **运行事件**，带 run ID、顺序号和事件 ID；可记录启动、完成、失败及接线后的已接受控制步。源：`src/fea-solver/src/execution_event_journal.h`、`execution_event_journal.cpp`、`solver_task.cpp`；测试：`src/fea-solver/tests/tst_execution_event_journal.cpp`、`tst_solver_audit_cli.cpp`。它不保证与模型结果跨文件原子提交，也不自动包含 GUI/Agent 提案和批准。
2. `RInteractionRecordV1` 是通用 **数据契约**，要求记录/父记录 UUID、UTC 时间、actor、session/project/model/revision/run、阶段与命令、参数/选择/提案/批准、前后状态摘要、结果诊断/工件、应用与插件版本。只负责严格解析、验证和序列化，没有 journal writer、Undo/Redo、GUI/Agent hook 或授权机制。源：`src/range-base-lib/include/rbl_interaction_record_v1.h`、`src/range-base-lib/doc/INTERACTION_RECORD_V1.md`；测试：`src/range-base-lib/tests/tst_interaction_record_v1.cpp`。

上层已有应用服务 `RModelCommandSession` 的持久记录/发布意图，以及工作区 service 的状态/审计提交；GUI 文档事件和部分操作账本仍为内存状态。当前边界见 `../APPLICATION_SERVICE.md`、`../STAGE12_ACCEPTANCE.md`，项目级恢复与中断检查点由 WF-05 继续交付；基础数据类不负责存储不等于上层没有记录实现。

Agent 可以读取这些契约解释来源，但不能仅凭运行事件推断用户已批准，或把尚未写入的提案描述为持久记录。记录过滤凭据，工件使用受控 ID/摘要。新增命令复用已有事务/审计入口，补齐 GUI 操作与工件的可靠恢复关联；具体缺口和验收按 WF-05/08 执行。
