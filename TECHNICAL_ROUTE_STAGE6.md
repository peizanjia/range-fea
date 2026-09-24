# Stage 6 技术路线：控制耦合、功率归属与外部协议

更新时间：2026-09-24

状态：实现进行中。本文件先固定算法和接口语义；完成情况以源码、聚焦测试、主 Release 全量 CTest 与 `HANDOFF.md` 为准。Stage 5 的被动二阶计算已完成，本阶段不得把未接通的协议样例当成实际控制闭环。

## 1. 控制输入在方程中的位置

Stage 5 已有约化结构方程 `M_r a + C_r v + K_r q = f_passive`。Stage 6 只在右端增加来源可辨的控制力：

```text
M_r a + C_r v + K_r q = f_passive(t) + sum_j b_rj u_j
b_rj = T^T b_phys,j
```

`u_j` 是按 Stage 4 执行器稳定 ID 标识、以物理 N 表示的标量命令；`b_phys,j` 是节点/方向的稀疏力形状，`T` 是既有 MPC 独立坐标变换。固定网格、约束、材料和时间步长下，`b_rj` 可以在求解前构造一次；每步只需 `O(sum_j nnz(b_rj))` 的向量加法和功率点积。控制器**不能**进入单元装配、有效矩阵构造或 CG/GMRES 的每次内迭代。若目标是受约束支承自由度、局部/非节点执行器尚无明确定义，首版必须拒绝，不能把力悄悄丢掉。

## 2. 接受步事务与零阶保持

每个固定时间步按以下状态机推进：

```text
accepted(q_n,v_n,a_n, controllerState_n, ledger_n)
  -> sample sensors at t_n
  -> controller.compute exactly once
  -> validate and limit requested actuator commands
  -> hold applied commands on [t_n,t_n+dt]
  -> refresh a_n if the held force jumped
  -> integrator trial (force callback may run more than once)
  -> result write succeeds -> accept controller state/ledger -> notify observer
  -> any pre-commit failure -> restore previous accepted state and retry safely
```

为什么要在 `t_n` 刷新加速度：新控制命令在区间起点阶跃改变；若仍沿用旧力求得的 `a_n`，Newmark 的预测量会把本应零阶保持的输入误表现为端点渐变。刷新需解一次原质量矩阵方程，不应伪装成零成本；改变右端不应迫使固定 `M/C/K` 或有效矩阵重组。

广义 alpha 会在区间内部时刻评价外力，中心差分会在端点评价外力；因此积分器的力回调必须是由已确定命令计算的纯函数，不能在回调中再推进 PID、随机数或延迟队列。控制器候选状态只在时间步及结果工件均成功提交后接受；失败步重试必须从同一个已接受状态和随机种子得到同一命令。

## 3. 命令约束与控制功账

限幅及变化率由 Stage 4 执行器目录中的 `minimum/maximum/rateLimit` 提供；应同时记录 `requested` 与 `applied`，不能静默截断。给定本步保持后的命令，单通道物理功率和功为

```text
P_j(t) = u_j * b_phys,j^T v_phys(t)
W_j[n+1] = W_j[n] + dt/2 * (P_j(t_n) + P_j(t_n+dt))
```

这里两个端点均使用**本步的同一个** `u_j`；若下一步命令改变，应另开区间，不能把前一步的终点命令与后一步的起点命令平均成虚构斜坡。`W_j` 是节点力对结构做的功，不能与 Stage 5 的约化总有效力功、支承端做功或物理全系统能量混为一谈。checkpoint 必须保存通道顺序、最近接受命令、端点功率、累计功、控制器接受态及配置/映射指纹，恢复时逐项核验。

## 4. 现有积分方法与并行性能边界

软件已有 Newmark 平均加速度、广义 alpha 和中心差分；先为既有方法补 FE 选择、控制阶跃、续算和稳定性测试，不以增加算法数量替代质量验证。中心差分当前含阻尼质量矩阵求解，不能声称是“完全无迭代”的显式法。若以后增加真正集中质量显式路径，需要重新验证质量近似、CFL、能量和控制保持语义。

已发现的性能关注点：每步 `prepare()` 重装配固定算子，每步事务深复制模型与共享数据，固定步长有效矩阵可能反复稀疏插入，`RMatrixSolver` 仍处理 CSR 快取/预条件器。优化顺序是先加 `assembly / reduction / control / linear solve / result commit / I/O` 分段计时，再做 32/384/更大规模、1/4/8 线程 A/B；只缓存确定不随时间、材料或几何变化的对象。并行降级、数值漂移、内存峰值及重启一致性必须一同报告，不设无根据的加速比硬门槛。

## 5. 进程外协议与未来 Agent

同机进程外原型使用版本化 NDJSON 控制平面、独立标准错误、请求 ID、目录白名单、单请求超时及失败关闭；主程序直接以可执行文件与独立参数数组启动，不经 shell。外部程序收到接受态时间、`dt`、物理单位传感器值及接受态控制器状态，返回按 actuator ID 标识的候选命令和候选状态。当前传输类及 Python 样例独立通过测试，**仍待接入 `SolverTask` 与 FE 时间步**。MATLAB 可实现相同协议，但必须经过实际进程启动和握手测试后才可称为验证过的适配器。大规模全状态后续采用带 shape/dtype/revision/lifetime 的二进制数据平面，不能每步无界 JSON 复制。

未来 Agent/GUI 操作与高频控制使用不同调用节奏，但共用稳定 ID、版本、单位、模型 revision、能力和审计契约。可执行 schema 与契约测试是真源，短模块手册是按需检索的教学入口；详见 `AGENT_INTERFACE_DESIGN.md`。Stage 6 的控制步记录可先落地，尚未存在的 Agent 模型命令不能被假装已记录。完整 GUI/Agent 全流程操作接线依赖后续统一应用服务与侧栏改造。

## 6. 退出验证清单（未完成项保持未完成）

- 进程内 PID/LQR 或等价解析控制基准：至少单自由度衰减和多自由度传感器/执行器映射；控制关闭严格退化到 Stage 5。
- 限幅、速率、延迟、噪声和保持器在固定种子下可重播；命令阶跃与 `t_n` 加速度一致。
- ASCII/二进制受控 checkpoint、进程内/外续算、失败 trial/写出失败/控制进程超时与状态回滚。
- 每通道物理功率/累计功与独立解析值对照，审计记录关联 run ID、命令、观测、接受态及结果工件；错误不能被日志吞掉。
- 外部 Python 原型端到端与可行时 MATLAB 适配器握手/控制；协议版本不匹配、错 ID、超时均明确失败。
- 固定算子缓存及 CSR 并发安全测试，完整 Release CTest，关键规模/线程数的精度、耗时和内存对照；用户可配置并行但不许夸称所有核都已并行。
