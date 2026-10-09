# WF08 实施设计断点

2026-10-09，纯设计准备；WF07冻结后完整验收仍在执行，本文件不把WF08标为已实施。开发指令不安装为产品知识。根代理决定如下，验收仍按 WORKFLOW_DELIVERY_PLAN.md A01–A08。

- 调度放在 WorkflowService 外围纯 reducer，服务持久状态升级至 storageVersion 3，兼容读取v1/v2；计划、节点、已接受job及audit在同一_state提交。禁止独立plans.json跨文件提交窗口。批准令牌仅在活实例内存。
- 首批计划工具范围是已发布的 Workflow 命令与受限只读查询。GUI原文档编辑不加入首批DAG；界面任务树和Agent仍通过工作流宿主验证。模型提出的计划不能扩大现有能力或trusted runtime权限。
- 第一条实际扫描固定已登记native source，targetDamping两点0.05/0.10，各点串行configure→run(mode=both, independentReference=true)→来源/manifest/数值校核→有型别条件。每个写节点单独预览和人工批准，不把整计划批准变成所有步骤执行权。
- 条件只使用宿主发布的有限scalar和白名单eq/lt/le/gt/ge，单位及来源严格绑定。成功谓词由宿主代码发布；不接受任意Python、表达式或模型JSONPath。必要条件未满足不能宣称目标达成。
- 计划最多64节点、每节点16依赖、参数扫描最多4点、候选JSON最多128KiB；只读并行最多2，写lane串行。总模型调用最多24、工具调用128；节点模型调用4、工具调用16。提交调用前持久预扣，不在恢复时归零。
- 活动执行预算最多1800s、单节点300s；独立作业等待不用模型循环，仍受24h总deadline约束。暂停不消耗活动执行预算，但计入总deadline。时间来自宿主，并记录其验证范围，不声明硬实时或防系统时钟篡改。
- 节点已有jobId则先查job；submit intent只有previewHash则先execution.query。accepted绑定原job，unobserved保留outcome_unknown并停自动提交。中断作业走现有recovery资格，源/环境变化重新预览；不把进程exit0/模型回答当成功。
- native成功谓词核对job来源/配置、所需active/passive两条、totalSteps/accepted时间和SHA ledger；复用现有末态参考阈值q/转角1e-8、v/角速度1e-7、时间1e-8s，分别声明验证单位和末态范围，不能称整段轨迹验证。
- 人工clarification/approval、paused/cancelled/superseded与未知结果持久化；取消计划只停止新节点，已经提交计算保持独立。恢复废弃未提交批准，保存问题/答复不授予权限。
- 新纯模块所有权：task_plan.py/task_scheduler.py及独立测试；knowledge.py/knowledge/*.json及独立测试。根拥有service/client/GUI/Runner/CMake/必跑清单/验收。共享build/test/native/install串行，不与WF09/10/14同时改同GUI/service文件。
- 知识包以封闭registry返回pack/schema/capability/content哈希与适用状态，发布能力必须有前置、单位、步骤、成功谓词、恢复、可运行例子；操作签名从运行时schema生成。检索不读取AGENTS/SKILL/HANDOFF及本文件。

已有只读探索 `wf08_design_map` 定位：service._load/_commit/_dispatch和command.execute接受job同提交；jobs._run_imported_native_workflow独立参考；native_results.query_registered校验manifest/ledger；recovery.qualify；extensions._knowledge是需替换的无版本MD搜索；client与Qt接线由根完成。以上是实现入口，不是测试证据。
