# Stage 6 控制耦合 playbook

适用：确定控制输入的物理位置、编写/诊断控制器适配器、核对每步输入与功账。2026-09-24 主 Release 53/53 CTest 已通过；这里只描述已验证的 C++/Python 范围及其限制。

上述日期/数字为 v1 基础说明。当前型别控制与 MATLAB 软件 TCP 接线分别见 `../TYPED_FE_CONTROL.md`、`../CONTROLLER_ADAPTERS.md`；GUI/Agent/工作流入口见 `../AGENT_OPERATIONS.md`，后续任务见 `../WORKFLOW_DELIVERY_PLAN.md`。

## 入口、时序与数据所有权

- 模型侧 `RProblemSetup::getStructuralControlSetup()` 保存 `enabled`、协议版本 `1`、`controllerId`、决定性种子；感测器与执行器目录在 `RStructuralInteractionSetup`。源：`src/range-model-lib/include/rml_structural_control_setup.h`、`rml_structural_interaction.h`、`src/range-model-lib/src/rml_problem_setup.cpp`。
- 进程内入口是求解前注册 `RSolver::setStructuralControlCallback(controllerId, controllerRevision, callback)`，可选 `setStructuralControlStepObserver()`。回调接收**已接受**状态时刻的物理单位传感器值、下一时间点、种子和上一个已接受的控制器状态；返回每个执行器的请求力及候选下一状态。回调对同一输入必须纯且可重放；不可在积分器力回调或 CG/GMRES 内迭代偷偷改变内部状态。源：`src/range-solver-lib/include/rsolver.h`、`rstructural_control_protocol.h`、`src/range-solver-lib/src/rsolverstress.cpp` 的 `solveStructuralDynamics()`。
- 每一步顺序是取已接受态观测 → 控制器输出 → 延迟/保持/确定性噪声调理 → 幅值和变化率限制 → 稀疏力向量映射 → 刷新跳变边界力 → 固定命令下 trial/solve → 按输出频率写结果并接受或回滚。候选控制器状态、调理器状态、通道功账和观测记录仅随已接受步提交。源：`src/range-solver-lib/src/rsolverstress.cpp` 的 `prepareControlMapping()`、`solveStructuralDynamics()`、`commitStepTransaction()`；`src/range-solver-lib/include/rstructural_control_force_map.h`。物理力是 `N`，通道端点功率 `W`、累计功 `J`；不得把约化总有效力功率标为控制器功率。

## 进程外原型

`ControlNdjsonTransport` 通过 `QProcess` 标准输入/输出发送 UTF-8 NDJSON，标准错误单独作为诊断，绝不通过 shell 拼接执行。握手需 `protocolVersion=1`、匹配的 `requestId`、`pureStep=true`、`scalar-nodal-force` 与执行器 ID 集合；已验证版本还要求往返核对 `controllerId`、`controllerRevision`（程序/文件参数摘要）、`modelSnapshotSha256`（CLI 输入模型 target 摘要）和十进制字符串种子。`compute` 传 `stepId` 字符串、`time/dt` 秒、逐 ID 标量观测及 `acceptedControllerState` JSON 对象，回复逐 ID `force`（`N`）和 `nextControllerState`。错误版本、未知/重复 ID、非有限值、超长行、超时或子进程退出即失败关闭，不接受半条命令。源：`src/fea-solver/src/control_ndjson_transport.h`、`control_ndjson_transport.cpp`、`src/fea-solver/CONTROL_TRANSPORT_V1.md`、`src/fea-solver/tests/tst_control_ndjson_transport.cpp`。

CLI 整合代码提供 `--control-program=<绝对可执行文件>`、`--control-args-json=<JSON 字符串数组>`、`--control-timeout-ms=1..60000`，并在模型 control setup 已启用时注册 transport 回调；参数是独立 argv，不是 shell 命令。Python 示例：`src/fea-solver/examples/control_ndjson_python.py`（仅适用于 `tip-force` 演示目录）和可配置增益的 `control_pid_python.py`（单传感器/执行器）。源：`src/fea-solver/src/main_task.cpp`、`solver_task.cpp`；端到端集成测试：`src/fea-solver/tests/tst_solver_audit_cli.cpp`。MATLAB 长驻标准输入适配器尚未验证，不应将协议可移植性当作已交付适配器。

## 首版支持与明确拒绝

| 范围 | 当前边界 |
| --- | --- |
| 执行器 | 全局坐标、点节点 `NodalForce`，标量沿 X/Y/Z 或给定非零方向；映射到独立约化平移 DOF。被保持/不存在的 DOF、面力、基座运动、压电与拉索不在首版控制路径。 |
| 传感器 | 全局坐标、点节点位移/速度/加速度沿给定非零方向；目标必须可映射。温度、应力、应变、带局部 frame 的采样尚不在首版回调。 |
| 时钟 | 固定求解步长、每个已接受步取样一次，命令保持到下一步；多速率和实时性能没有证明。 |
| 限制/状态 | 执行器最小/最大及速率限制在 FE 路径；`RStructuralControlConditioner` 已接入，按模型 1.3.12 每通道策略执行整数步延迟/保持/固定种子均匀噪声，最终命令仍受幅值/速率约束。 |
| 重启 | 控制启用状态、协议/种子、映射与通道功账、控制器状态进入动态 checkpoint 并作匹配验证；控制器程序/参数 revision 由 CLI 计算。进程外握手核对输入模型文件摘要，但它不是一个跨重网格操作稳定的模型/DOF revision；模型在 run 中必须冻结。 |

源：`src/range-solver-lib/src/rsolverstress.cpp` 的 `prepareControlMapping()`、`solveStructuralDynamics()`；`src/range-solver-lib/include/rstructural_control_conditioner.h`；`src/range-model-lib/include/rml_structural_dynamics_restart_data.h`；`src/fea-solver/src/solver_task.cpp`。

## 核验与 Agent 禁区

基础算例看 `src/range-solver-lib/tests/tst_structural_control_force_map.cpp`（映射、物理功率、trial/reject）；`tst_structural_dynamics_fe.cpp`（解析保持力、幅值/速率、重启与观察者）；`src/fea-solver/tests/tst_control_ndjson_transport.cpp`（协议失效关闭）；`tst_solver_audit_cli.cpp`（进程外闭环与事件）。外部控制程序应自行保证同一请求/已接受状态可重复输出，保存算法版本并比较无控制基线。

当前已有 GUI 模态命令、受监督 Agent、模板控制工作流和接受步曲线；通用当前模型导入、运行中调参和完整项目恢复按 WF-02/03/05/09 继续交付。型别执行器的内核能力不自动成为模板工作流的可用入口；全状态热耦合和实物硬实时仍有独立范围。`RInteractionRecordV1` 是数据契约，应用服务/工作区已有上层审计，CLI `ControlStepAccepted` 则是求解运行事件。遇到能力缺口按 capability 报告，不直接写模型私有状态或热载未经验证代码。
