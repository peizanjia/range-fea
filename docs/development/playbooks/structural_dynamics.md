# 结构动力学计算 playbook

适用：解释或配置当前二阶结构瞬态基线，诊断错误、选择已实现积分法及核对重启。不是 GUI 自动化命令手册。

## 实际入口与流程

1. 在模型的 `RProblemSetup::getStructuralDynamicsSetup()` 填入并验证 `RStructuralDynamicsSetup`：`enabled`、`timeStep`、`nSteps`、`outputFrequency`、方法与阻尼参数。源：`src/range-model-lib/include/rml_structural_dynamics_setup.h`、`src/range-model-lib/src/rml_structural_dynamics_setup.cpp`。
2. 求解必须走结构应力任务树；`RSolver::run()` 检查二阶设置、重启信息和 stress-only 任务树，再驱动固定步长时间循环。源：`src/range-solver-lib/src/rsolver.cpp` 的 `run()`。
3. `RSolverStress` 建立节点/DOF 方程、约束约化、质量/刚度/载荷并将物理状态投影到自由 DOF；`RStructuralDynamicsIntegrator` 在约化系统 `M a + C v + K q = f` 上计算 trial，接受后才成为结果。源：`src/range-solver-lib/src/rsolverstress.cpp` 的 `prepare()`、`solveStructuralDynamics()`；`src/range-solver-lib/include/rstructural_dynamics_integrator.h`。
4. 模型可保存动态 checkpoint；失败步会恢复已接受的模型、共享状态和积分状态，但模型结果、运行事件 journal 不具跨工件统一磁盘原子性。源：`src/range-model-lib/include/rml_structural_dynamics_restart_data.h`、`src/range-solver-lib/src/rsolvergeneric.cpp` 的步事务、`src/range-solver-lib/src/rsolverstress.cpp` 的事务方法。

## 算法选择与物理量

| 方法 | 当前入口与约束 | 适合先试的情形 |
| --- | --- | --- |
| Newmark 平均加速度 | `RStructuralTimeIntegrationMethod::Newmark`；当前仅 `gamma=0.5, beta=0.25` | 固定步长、线性结构响应基线 |
| 广义 α | `GeneralizedAlpha`，用 `rhoInfinity` 控制高频数值耗散 | 需要抑制非目标高频响应时，与无耗散基线对照 |
| 中心差分 | `CentralDifference`，必须通过保守稳定步长检查；当前仍求解与质量/阻尼有关的矩阵，**不可宣称完全无矩阵求解的显式算法** | 能满足稳定步长且通过精度/能量检查的模型 |

上述方法定义在 `src/range-model-lib/include/rml_structural_dynamics_setup.h`，实现及固定有效算子缓存在 `src/range-solver-lib/src/rstructural_dynamics_integrator.cpp`。Rayleigh 阻尼可用；有限规模模态阻尼另有受限实现，模态与 Rayleigh 同时配置、模态阻尼与移动支承组合会被拒绝。不要为“算法多样”自行切换方法后直接比较控制效果：先固定网格、步长、阻尼、初值和解析/参考误差门槛。

`q/v/a` 是约化自由 DOF 状态；接受态的有效力功率和阻尼耗散不是整结构物理总功率。受支持的移动支承反力、功率、累计功另经 `RSolver::setStructuralSupportResponseObserver()` 获取，当前仅覆盖指定的全局点节点平移与所允许的阻尼/约束组合。源：`src/range-solver-lib/include/rsolver.h`、`rstructural_support_response.h`、`src/range-solver-lib/src/rsolverstress.cpp` 的 `computeSupportSamples()`。

## 并行与耗时边界

并行参数由模型 `RParallelExecutionConfig` 与 CLI 覆写控制，运行时通过 `RParallelExecutionContext` 报告请求、应用及探测线程数；探测值不是持续实际占核数，也不保证加速或位级确定性。元素装配有 OpenMP 每线程缓冲；CG/GMRES 有并行计算，但存在归约与同步开销。固定 `dt/M/C/K` 的积分有效算子已可缓存；`RSolverStress::prepare()` 仍会按步重新装配与检查矩阵，步事务仍深复制模型。优化前应分段测量装配、求解、复制、序列化及 I/O，再以不同网格和 1/4/8 线程验证误差和耗时。源：`src/range-solver-lib/src/rparallel_execution_context.cpp`、`rmatrixsolver.cpp`、`rsolverstress.cpp`、`rsolvergeneric.cpp`。

## 故障先查与证据

- 看到旧一阶 time solver 时，不能将它解释成二阶结构动力学；查询产品能力与 `StructuralDynamicsSetup`。源：`src/range-model-lib/src/rml_structural_product_profile.cpp`。
- 重启失败先核对网格/DOF、固定步长、系统指纹、支承或控制记录；不得删除 checkpoint 后声称是同一次可重放试验。源：`src/range-model-lib/include/rml_structural_dynamics_restart_data.h`、`src/range-solver-lib/src/rsolverstress.cpp` 的 `solveStructuralDynamics()`。
- 数值验证：`src/range-solver-lib/tests/tst_structural_dynamics_integrator.cpp`、`tst_structural_dynamics_fe.cpp`、`tst_structural_dynamics_beam.cpp`、`tst_structural_dynamics_restart_io.cpp`、`tst_structural_dynamics_parallel_repro.cpp`。测试通过仅证明覆盖算例，不代表所有材料、单元和边界组合均已验证。
