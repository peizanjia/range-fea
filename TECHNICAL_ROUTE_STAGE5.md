# Stage 5 技术路线：二阶结构动力学与可复现执行

更新时间：2026-09-24

状态：实现与集成进行中；本文按当前源码解释算法，不代替构建、数值测试和阶段退出验收。最终完成状态以 `HANDOFF.md`、`DEVELOPMENT_LOG.md` 和 CTest 记录为准。

## 1. 为什么另建二阶路径

历史结构瞬态按 `(M+theta*dt*K)u[n+1]=dt*f[n+1]+(M-(1-theta)*dt*K)u[n]` 推进，只有位移状态，本质是一阶方程的离散化。振动控制需要惯性和速度，目标方程是

```text
M q_ddot + C q_dot + K q = f(t)
C = alpha_M M + beta_K K             （当前可执行阻尼）
```

`q` 可同时包含节点平移和梁的转角自由度。Stage 5 的独立开关 `RStructuralDynamicsSetup::enabled` 选择二阶分支；未开启时保留历史路径，避免改变旧模型的语义。当前二阶入口限制为单一结构应力任务树，不能把多物理场任务自动解释为已耦合的动力分析。

当前步事务把“已接受时间步”作为唯一可发布状态：试步开始前复制模型、共享数据与积分器参考状态；`prepare/solve/process/store/writeResults` 任一阶段失败时恢复这些状态及已接受时间。结构动力续算不执行旧式 `updateOldRecords` 预改写，以免失败步先动旧记录。模型 target 用 `QSaveFile` 先提交，再更新 latest link；若 target 成功而 link 失败，可留下未被 link 引用的孤儿 target。当前深复制成本随模型规模和矩阵非零数增长，后续应以结果/配置 journal 优化；内存不足导致回滚自身失败时也无法承诺严格原子性。

## 2. 模型、单元与全局矩阵的接口

Stage 2 的 `RElementDescriptor` 给出单元节点拓扑和每节点 `RDofType`；`REquationBook` 为实际活动的平移/转角自由度编号。单元例程继续提供局部刚度 `Ke`、质量 `Me` 与等效力 `fe`，装配器将其按节点坐标和约束坐标变换写入全局 `K/M/f`。质量使用既有一致质量或集中质量选项，不引入一套与静力/模态单元不兼容的动力单元。

二阶路径在 `RSolverStress::assemblyMatrix()` 中直接选择 `Ae=Ke`、`Be=Me`、`be=fe`；旧一阶 `M+theta*dt*K` 只留在未启用二阶分析的兼容分支。`RSolverStress::prepare()` 在二阶、旧瞬态及模态下要求装配质量。其好处是同一单元信息可以用于静力、模态和时域，而不是按分析类型复制形函数或质量矩阵实现。

模型层在 `RProblemSetup` 中保存：方法、固定步长、步数、输出频率、Rayleigh 系数、广义 alpha 的 `rhoInfinity`、Newmark 参数、模态阻尼比目录，以及并行执行配置。当前模型文件版本为 1.3.8；ASCII/二进制读写和旧格式默认值由 `tst_problem_setup` 覆盖。目录中的模态阻尼比是配置契约，求解器尚不合成模态阻尼矩阵；一旦设置，当前动力路径明确拒绝而非静默忽略。

## 3. 时间积分算法

`RStructuralDynamicsIntegrator` 仅接收同维度的 `M/C/K`、线性求解配置和外力回调，不依赖 GUI。初始加速度由 `M a0=f(t0)-C v0-K q0` 求得。每步先产生候选状态，调用方再接受或拒绝，因而失败的线性求解不会污染已接受状态。

### 3.1 Newmark 平均加速度

当前产品基线固定 `gamma=1/2`、`beta=1/4`。令

```text
q* = q_n + dt v_n + dt^2 (1/2-beta) a_n
v* = v_n + dt (1-gamma) a_n
(M + gamma dt C + beta dt^2 K) a[n+1]
    = f[n+1] - C v* - K q*
q[n+1] = q* + beta dt^2 a[n+1]
v[n+1] = v* + gamma dt a[n+1]
```

对于线性、无约束时变和适当质量矩阵，这是无条件稳定的隐式基线；稳定不代表任意步长都足够准确，仍需做时间步收敛。模型层允许保存其他 `gamma/beta`，但当前求解入口只接受平均加速度参数，以免配置与实际算法不一致。

### 3.2 广义 alpha

高频谱半径 `rhoInfinity` 在 `[0,1]`。当前实现采用

```text
alpha_m = (2 rhoInfinity - 1)/(rhoInfinity + 1)
alpha_f = rhoInfinity/(rhoInfinity + 1)
gamma = 1/2 - alpha_m + alpha_f
beta = (1 - alpha_m + alpha_f)^2/4
```

在 `t_n+(1-alpha_f)dt` 评估外力，并对 `a/v/q` 取对应加权值建立有效矩阵。`rhoInfinity=1` 退化为无额外高频耗散的平均加速度形式；较小值引入算法耗散。能量账本中的残差会同时包含这种数值耗散，不应误报为物理 Rayleigh 阻尼。

### 3.3 中心差分

积分器保留中心差分选项，按半步速度更新：先解 `(M+dt C/2)v[n+1/2]`，再用 `q[n+1]=q_n+dt v[n+1/2]`。它是条件稳定算法。当前对严格对角占优的正质量矩阵采用 `2*sqrt(min_i(M_ii-sum_{j!=i}|M_ij|)/max_i sum_j|K_ij|)` 的保守上界，并在 `dt>=0.95*上界` 时拒绝；不能证明该条件的矩阵则要求使用集中质量或隐式法。这个界通常比精确谱半径给出的临界步长更小，但不会把未知稳定性默认为安全。使用一致质量或阻尼时仍有质量相关线性求解，因此不能声称它总是“无需求解线性系统的显式法”。

## 4. 约束消元与模态一致性

Stage 4 静力 MPC 采用拉格朗日乘子，但把乘子块直接带入 `K phi=lambda M phi` 会使质量矩阵奇异。Stage 5 当前源码用增量行约简识别相关、冗余和矛盾约束，然后对模态与二阶动力使用独立坐标：

```text
C_q q = d,       q = q_p + T z,       C_q T = 0
K_r = T^T K T,   M_r = T^T M T,     f_r = T^T (f - K q_p)
C_r = T^T C T
```

其中 `T` 由自由列与主元列构造，`q_p` 是固定约束的特解；解后 `z` 再映射回物理节点自由度。局部坐标约束先转为活动方程，已指定自由度从约束右端消去。这让模态 `K_r phi=lambda M_r phi` 保留非奇异质量路径，也让动力的 `K/M/C/f` 使用同一独立坐标。

当前边界必须明确：上述动态式假定 `q_p` 不随时间变化。若基础位移是 `q_p(t)`，正确右端还必须扣除 `M q_p_ddot+C q_p_dot`；仅扣 `K q_p` 不足以代表基础速度/加速度激励。非齐次 MPC 的模态解释也需另设工作点或拒绝。没有这些处理前，不应把 Stage 4 基础运动目录称为已完成的动态基础激励。

### 4.1 移动支承的分块方程（数学核已验证，FE 接线未完成）

将已指定支承自由度记为 `g(t)`、其余未知自由度记为 `q_f`，完整矩阵按 `f/c` 分块；对自由行有：

```text
M_ff q_f_ddot + C_ff q_f_dot + K_ff q_f
  = f_f - M_fc g_ddot - C_fc g_dot - K_fc g
q_f = T z + p(t)
f_z = T^T [f_f - K_fc g - C_fc g_dot - M_fc g_ddot
                - K_ff p - C_ff p_dot - M_ff p_ddot]
```

`RStructuralDynamicsSupportCoupling` 已把有效力及 `q/v/a` 重建封装并通过两节点一致质量解析测试：仅有支承加速度 `A` 时，自由端初始加速度应为 `-A/2`。这个测试也说明不能只在现有已消元的 `ff` 矩阵上改右端：装配必须额外保存 `K_fc/M_fc`，Rayleigh 阻尼据此生成 `C_fc`，而运动接口必须同时给出解析或显式约定的 `g/g_dot/g_ddot`。当前 FE 路径尚未做这些，因此会拒绝移动支承，而不是从位移曲线暗中差分出不可靠的加速度。

## 5. 状态、力回调与能量账本

已实现的内部状态 `RStructuralDynamicsState` 保有时间、`q/v/a`、外力、动能、应变能、累计外力功、累计阻尼耗散和能量残差。`trialStep()` 生成候选；`acceptStep()` 提交，`rejectStep()` 丢弃；`checkpoint()/restore()` 对已提交积分器状态做内存快照与恢复。步长在第一次提交后固定，避免在未实现自适应误差控制时隐式改变数值语义。

监测量为

```text
E_k = v^T M v / 2,        E_s = q^T K q / 2
R_E = E_k + E_s - E_initial - W_external + D_damping
```

外力功按相邻端点力平均乘位移增量，阻尼耗散按端点功率梯形积分。它们适合线性基准的能量自检；广义 alpha 的算法耗散、时变约束做功、非保守执行器和控制器功率需单独入账。目前回调签名能在给定时间和已接受状态下填入力向量，是 Stage 6 控制器接入的低层挂点；它不是已稳定的传感器—控制器—执行器协议。回调可被一次 trial 多次调用，外部控制器若有内部状态，必须自己保证可重放并与拒绝/回滚同步。

当前 `RSolverStress` 每个求解步重装配力向量并用相邻端点线性插值送给积分器；首步另在真实 `t0` 装配外力，所以斜坡载荷的初始加速度不再错误使用 `t1` 力。`q/v/a` 的平移以及梁转角对应角速度/角加速度写回结果变量。模型版本 1.3.8 已定义并保存含网格、DOF 和系统指纹的 checkpoint；内存续跑及 ASCII/二进制磁盘读回续跑对照均已通过聚焦回归。它只涵盖当前结构求解器状态，不含未来 Stage 6 控制器内部状态；输出频率跳过的中间步也没有可供跨进程恢复的磁盘 checkpoint。

## 6. 并行执行上下文

`RParallelExecutionConfig` 有 `Auto`、`SerialDeterministic`、`FixedThreads`，并记录固定线程请求与“希望确定性归约”的配置。`RSolver::run()` 创建运行级 `RParallelExecutionContext`：在创建它的主机线程上设置 OpenMP 的线程数和动态团队策略，使当前运行内的装配、线性/特征值求解与时间步共享该线程局部运行时配置，离开作用域时恢复原值。

执行报告包含请求、应用、实测和可用线程数、OpenMP 后端、降级与诊断。请求超过可用处理器或运行时限制时可明确降级；日志打印当前运行报告。这是并行度配置与观测闭环，不等于所有并行核都具有确定性归约：当前只有单线程能保证此标志为真，多线程虽可请求确定性，具体核尚须固定归约顺序。也尚无完整 InteractionRecord 持久化、重复运行漂移与加速比基准。`FixedThreads` 控制的是 OpenMP 团队上限和默认团队，不保证每个小矩阵求解都会实际使用相同数量的工作线程。

## 7. 验证地图与退出条件

当前源码中有三层测试入口：

| 层级 | 用例 | 主要证明内容 |
| --- | --- | --- |
| 模型格式 | `tst_problem_setup` | 1.3.8 配置与 checkpoint 的 ASCII/二进制往返、旧版兼容、非法输入拒绝及 target/link 写入顺序 |
| 积分器 | `tst_structural_dynamics_integrator`、`tst_structural_dynamics_loads` | 自由/阻尼振动、阶跃/有限脉冲/谐波、二阶收敛、广义 alpha 取样、中心差分 CFL、候选拒绝与内存检查点 |
| 有限元/执行 | `tst_solver_stress`、`tst_structural_dynamics_fe`、`tst_structural_dynamics_beam`、`tst_parallel_execution_context` | 单杆与多步 FE 的 q/v/a、模态—时域梁频率、旋转量、MPC 消元及线程数作用域/对照 |
| 续算/失败 | `tst_structural_dynamics_restart_io`、`tst_structural_dynamics_failure_atomicity` | ASCII/二进制磁盘读回续算、重启拒绝、准备/写出失败后尺度与 checkpoint 回滚、旧 target/link 不变及同一 solver 重试；聚焦回归已通过 |

测试文件存在不等于测试通过；最终结果应由该轮 Release 全量 CTest 和报告记录确认。当前仍需：移动支承 FE 与非零/时变 MPC 的正确组合、串行和多线程重复性及加速比基准、失败步跨多个工件的统一磁盘原子性，以及更完整的独立解析或参考程序误差表。已验证梁横振与模态—时域频率、脉冲/谐波和步长收敛、Rayleigh 衰减；这些局部验证不等于 Stage 5 结项。

## 8. 下一阶段接口边界

Stage 5 的有限元状态和时间步事务是 Stage 6 控制耦合的基础，但尚未实现 `IController/ISensor/IActuator/ICouplingTransport`、逐步观测/饱和/时延、多速率、外部 Python/MATLAB 协议或结构化 InteractionRecord。Stage 7 压电/拉索和 Stage 8 全状态热—结构耦合更不应由现有力回调推定完成。后续应让控制输入在 `trial/accept/reject` 边界生效，以稳定 DOF/传感器/执行器 ID 交换，并给控制器内部状态同样的检查点和回滚语义；GUI/Agent 只通过这一应用服务层调用求解器。

读代码时可按 `RStructuralDynamicsSetup` → `RSolver::run()` → `RSolverStress::prepare()/applyLinearConstraints()/solveStructuralDynamics()` → `RStructuralDynamicsIntegrator` → `RSolverStress::store()` 的顺序追踪一次时间步。这条链分别回答“用户配置什么、单元装配什么、约束怎样减少自由度、状态怎样推进、结果怎样保存”。
