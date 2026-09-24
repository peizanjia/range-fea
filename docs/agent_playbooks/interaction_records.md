# 交互记录与运行事件边界

目前有两个不同层级，不能混称：

1. `ExecutionEventJournal` 是 `fea-solver` 的追加式 UTF-8 JSONL **运行事件**，带 run ID、顺序号和事件 ID；可记录启动、完成、失败及接线后的已接受控制步。源：`src/fea-solver/src/execution_event_journal.h`、`execution_event_journal.cpp`、`solver_task.cpp`；测试：`src/fea-solver/tests/tst_execution_event_journal.cpp`、`tst_solver_audit_cli.cpp`。它不保证与模型结果跨文件原子提交，也不自动包含 GUI/Agent 提案和批准。
2. `RInteractionRecordV1` 是通用 **数据契约**，要求记录/父记录 UUID、UTC 时间、actor、session/project/model/revision/run、阶段与命令、参数/选择/提案/批准、前后状态摘要、结果诊断/工件、应用与插件版本。只负责严格解析、验证和序列化，没有 journal writer、Undo/Redo、GUI/Agent hook 或授权机制。源：`src/range-base-lib/include/rbl_interaction_record_v1.h`、`src/range-base-lib/doc/INTERACTION_RECORD_V1.md`；测试：`src/range-base-lib/tests/tst_interaction_record_v1.cpp`。

Agent 可以读取这两种契约以解释来源，但不能仅凭已有运行事件推断“用户已批准”，也不能把尚未写入的提案解释为持久记录。任何写入前先过滤 API 密钥、令牌和口令；引用工件应使用受控 ID/摘要，不把大文件或秘密全文塞进事件。未来 GUI/Agent 共用的 Command Service 建成后，应在同一命令事务边界产生 InteractionRecord，并将结果工件和 run ID 关联；这属于后续应用层工作，不能由本目录文档代替。
