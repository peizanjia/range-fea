# Stage 6 技术路线：控制耦合、功率归属与外部协议

更新时间：2026-09-24

状态：2026-09-24 已完成 Stage 6 的 C++/Python 数值控制链、控制步事件和 InteractionRecord 数据契约，主 Release 53/53 CTest 通过。MATLAB 长驻进程适配器尚未验证；GUI/Agent 操作接线按依赖移至 Stage 10–11。细节以源码、测试与 `HANDOFF.md` 为准。

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
  -> scheduled result write succeeds (or no write scheduled) -> accept controller state/ledger -> notify observer
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

| 方法 | 已实现路径的用途与代价 | 本阶段选择原则 |
| --- | --- | --- |
| Newmark 平均加速度 | 隐式二阶基线；固定步长可复用有效矩阵，但每步仍需线性求解。 | 首选正确性基准、低频振动与控制闭环对照。 |
| 广义 alpha | 带 `rhoInfinity` 的高频数值耗散，可压制网格高频伪振；也需隐式求解。 | 当离散高频噪声污染控制观测时，先量化低频相位/幅值误差再使用。 |
| 中心差分 | 有保守 CFL 上限；当前实现仍求解含阻尼质量矩阵及端点加速度，非纯对角显式。 | 小步长、短时波动或与隐式法交叉验证；不能因名称误判其成本。 |

选项已经在 `RStructuralDynamicsSetup` 持久化；只有独立精度/稳定性与真实规模耗时证据表明缺口时，才考虑 HHT、能量动量法或集中质量显式分支。新增算法必须共用相同接受步控制接口、物理单位、约束约化、功账和重启语义。

已发现的性能关注点：每步 `prepare()` 重装配固定算子，每步事务深复制模型与共享数据，固定步长有效矩阵可能反复稀疏插入，`RMatrixSolver` 仍处理 CSR 快取/预条件器。优化顺序是先加 `assembly / reduction / control / linear solve / result commit / I/O` 分段计时，再做 32/384/更大规模、1/4/8 线程 A/B；只缓存确定不随时间、材料或几何变化的对象。并行降级、数值漂移、内存峰值及重启一致性必须一同报告，不设无根据的加速比硬门槛。

## 5. 进程外协议与未来 Agent

同机进程外原型使用版本化 NDJSON 控制平面、独立标准错误、请求 ID、目录白名单、单请求超时及失败关闭；主程序直接以可执行文件与独立参数数组启动，不经 shell。外部程序收到接受态时间、`dt`、物理单位传感器值及接受态控制器状态，返回按 actuator ID 标识的候选命令和候选状态。传输层还绑定控制器 ID/版本摘要、本次输入模型快照 SHA-256 及十进制固定种子；摘要回显不是程序来源认证。`SolverTask` 接线和 Python PID 算例已通过主 Release 端到端 CLI 回归。MATLAB 可实现相同协议，但必须经过实际进程启动和握手测试后才可称为验证过的适配器。大规模全状态后续采用带 shape/dtype/revision/lifetime 的二进制数据平面，不能每步无界 JSON 复制。

控制调理器以固定整数步实现零阶保持、延迟和均匀噪声。噪声由种子、稳定执行器 ID、接受步号决定，不使用进程可变随机状态；`RCCE` 复合 checkpoint 保存控制器原始字节状态及调理器状态，试步失败不提交。命令顺序为原始请求→调理→幅值/速率限制→稀疏物理力映射；逐步审计分别记录请求、调理后和实际施加值。1.3.11 的受控 raw checkpoint 因缺少调理历史明确拒绝跨版本受控续算；这是一条安全的兼容边界，不影响旧被动模型读取。独立求解器 FE/调理器测试及主 Release 全量回归均已通过。

未来 Agent/GUI 操作与高频控制使用不同调用节奏，但共用稳定 ID、版本、单位、模型 revision、能力和审计契约。可执行 schema 与契约测试是真源，短模块手册是按需检索的教学入口；详见 `AGENT_INTERFACE_DESIGN.md`。Stage 6 的控制步记录可先落地，尚未存在的 Agent 模型命令不能被假装已记录。完整 GUI/Agent 全流程操作接线依赖后续统一应用服务与侧栏改造。

## 6. 退出验证与明确未完成项

- 已验证：单自由度速度反馈降低机械能，Python PID 进程外实际闭环，双独立 DOF/执行器线性叠加，三种积分方法的零控制与被动基准一致。尚未提供独立 Riccati 求解与 LQR 设计器；可经相同回调接入预计算增益。
- 已验证：限幅、速率、2 步延迟、2 步保持和固定种子噪声；失败后重启逐步命令/功账与不中断运行一致，命令阶跃在 `t_n` 刷新加速度。
- 已验证：模型 1.3.12 ASCII/二进制受控 checkpoint、进程内续算、试步失败回滚、外部进程超时失败关闭；Python 端到端已运行。外部 Python **跨进程重启**的独立 QProcess 对照尚未专门加入 CLI 测试，不能把内核重启测试冒充此验证。
- 已验证：每通道 N/W/J 稀疏力与功账、`requested/conditioned/applied` 逐步事件；事件带 run ID、控制器版本、种子、状态 SHA 和实际写出的结果工件路径。模型文件与事件 journal 尚无跨文件原子提交。
- 已验证：传输版本/请求号/未知执行器/非法数值/超时拒绝。MATLAB R2022a 的 `-batch` 能启动，但本机标准输入长驻握手试验未得响应，未交付可验证 MATLAB 适配器；语言无关协议允许后续采用可靠进程/IPC 适配器。
- 已验证：固定步长有效算子缓存、线程隔离 CSR 并发/地址复用回归、Release 全目标编译与 **53/53 CTest**，`tst_structural_product_profile.exe` 直接启动成功且已部署 `Qt6Core.dll`。32/384 杆 1/2/4 线程基准结果见 `docs/STAGE6_PERFORMANCE_AUDIT.md`；32 杆多线程变慢、384 杆 4 线程仅本机 1.220×，每步重装配/深复制与大模型内存指标仍是后续优化。

结论须分层表述：Stage 6 的已验证 C++/Python 核心范围可移交下一阶段；MATLAB 长驻适配器、外部控制进程跨进程续算测试、完整 GUI/Agent 操作记录、硬实时和大模型性能优化仍未完成。后两项按 Stage 10–11/性能后续工作处理，不得用本阶段测试代替。
