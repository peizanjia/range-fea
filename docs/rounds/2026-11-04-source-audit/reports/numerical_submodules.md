# 模型/求解器子模块续审记录

日期：2026-11-05（Asia/Singapore）。范围仍为根 `9ea1d51..ad9dbda` 固定的 model `30dba89..6a732d9` 与 solver `60c05bb..880c5d1` 批次，加当前未提交修补。不能据此归因某个开发模型。

## 覆盖与结论边界

- 清单仍为 **93 个批次变更文件**。原 **35 个 pending** 中，本轮完成 **32 个整文件静态合同审查**，另 **3 个仅完整变更段/调用链审查**；另对此前已审的 **12 个生产文件** 做完整补充复核，其余 **46 个沿用上轮范围**。原58项均标 `inheritedReview`，不会以当前 SHA 重新盖成重审。
- 状态计数是 **90 项 `reviewed_static`（包含58项继承状态）+ 3 项 `reviewed_changed_hunks`**，不是“93完整审完”。后者恰为 `rml_model.cpp`、`rml_file_io.cpp`、`rsolver.cpp`，不包含全部历史代码。逐项 `reviewDepth`、`currentPassReview`、行段、SHA、字节/行数及检查内容见 JSON；静态状态均不等于 C++、数值验收或产品通过。
- 本轮新增确认并修补 **2 个问题**，保留并独立复核暂停前 `N-SOLVER-TRACE-001`；新增 **4 个回归 selector**。solver 当前共 5 个修改文件（含原补丁）；本代理未修改 model 源码，三个保留文件的外部改动另行记录。
- 禁令保持：未配置 CMake、编译、构建、运行 CTest/C++、启动产品；未 reset/clean/stash/checkout/commit/push/更新 gitlink。未采用撤销的 AGENTS.md 分工、技能或提交规则；未重委派；未写根总结或 coverage。
- 外部兼容修复独占 `rml_structural_piezo_readout_setup.cpp`、`rml_structural_piezo_layered_setup.cpp`、`rml_structural_native_control.cpp`；本代理均未写入。其原审查 SHA 只代表读取快照，**外部兼容改动独立发生，不能沿用为改后审查结论，最终由协调代理重核**。JSON逐项记录reserved路径与snapshot/current hash的区别。

## N-SOLVER-TRACE-001 — trace 生命周期与接受步事务复核

**P1，原修补保留，运行验证待执行。**

触发链：注册 participant → 开启完整接受步 trace → 清空 participant。旧代码跳过传统 MODEL 输出，同时失去 trace 发布者，成功计算可能没有持久化结果链。

证据与修补：

- `src/range-solver-lib/src/rsolver.cpp:1217–1231`：trace 开启时拒绝空 participant。
- `rsolver.cpp:1488–1495,1518–1549`：每次 invocation 保留 shared_ptr 和 trace bool 快照。`stage()` 回调改变下一轮设置时，本轮 snapshot 准备与普通 MODEL 发布不能使用不同策略；shared_ptr 同时延长当前 publisher 生命周期。
- `rsolver.cpp:1527–1549,1552–1594`：stage 后再次验证候选模型；monitor/result 在前，恢复 authority 的 publish 最后；之后仅 noexcept commit/晋升。失败逆序回滚；不完整回滚禁用重试并保留 accepted model。通知位于提交后。
- `src/range-solver-lib/include/raccepted_step_publication.h:20–30` 明确禁止 publication publish/commit 修改候选；冻结指针并不认证 participant 自己是否真实落盘，也不是对任意不守合同回调的沙箱。
- `src/range-solver-lib/src/rsolvergeneric.cpp:652–800`：普通 MODEL 写及 frame 通知受 display frequency 控制；trace 下不产生这些 MODEL frame 是现有策略。关闭 trace 后仍可恢复普通持久化路线。
- 只读追至 `src/fea-solver/src/solver_task.cpp:1320–1388`，注册顺序为 participant → trace → 提交后 receipt observer。真实 checkpoint store 的落盘/恢复归根 CLI 集成验证，不由测试替身代替。

回归源：

1. 原 `tst_structural_piezo_layered_native_run.cpp::tracePublicationCannotLoseItsParticipant` 保留，未注册/清空拒绝与一次 stage/publish/commit。
2. `…cpp:481` `tracePolicyAndParticipantAreFrozenPerInvocation`：首次 stage 同时关闭 trace、替换 participant；本轮两步仍用原 publisher 且无 MODEL frame/文件；下一轮才使用替代者并真实写 MODEL、读回其 restart、重建 Run。
3. `…cpp:532` `tracePublicationFailureRollsBackAndRetries`：发布阶段确实计数后抛异常；rollback 一次、commit 零次、source 无新 restart、同实例重试后恰好接受一步。计数断言防止提前失败被误认成发布回滚覆盖。

兼容风险：开启 trace 时 detach 由静默接受变为显式异常；显式关闭后仍允许 detach。当前 invocation 冻结策略，后续 invocation 使用新配置。没有改变网格、display frequency 或物理计算。

## N-LAYERED-RESTORE-002 — 分层恢复允许更换声明的初始电压/无控制动作

**P1，已修补源码，静态可达性确认；C++ 未执行。**

触发/影响：同一 source/driver/config 下，生产 `RStructuralPiezoLayeredDynamics::initialize({不同电压})` 能产生自洽且 seal 有效的物理状态；旧 facade 不把 `physical.initialActuatorVoltages` 与声明源初值相等绑定。无控制器状态还可通过 production trial 获得不同 held 电压，或修改不参与无包络 conditioner 等式的 `previousApplied`。恢复因此可能改写原初始电边界、能量基线或引入声明中不存在的动作。

证据：

- `src/range-solver-lib/src/rstructural_piezo_layered_dynamics.cpp:192–203,282–318`：driver 支持显式初压并自行计算能量/seal；其任务是物理自洽认证，不负责 facade 源初值政策。
- 原 facade `certifyRuntime` 仅比较 lastApplied 与 physical held，及动作表内部 conditioner 自洽；轴向 facade `rstructural_piezo_axial_native_run.cpp:183–189` 已有零步/无控制边界合同，分层缺失。
- `src/range-solver-lib/src/rstructural_piezo_layered_native_run.cpp:124–139` 新增实际 compiled initial voltage 绑定、零步/无控制 held 绑定、首步/无控制 previous 绑定、零步 opaque/frame 合同。使用 adapter 编译后的初值，兼容显式工作点对初压的解释；不修改通用 driver 的合法能力。

回归：`src/range-solver-lib/tests/tst_structural_piezo_layered_native_run.cpp:565` `restoredInitialAndUncontrolledVoltagesMatchSource`。先正常恢复零步/接受步；再用真实 driver 构造、调用 `physical.restore` 确认可接受的物理 seal，仅在 facade 层拒绝异初压/无控制动作；另外隔离零步 pending request 与 previousApplied 负例。各负例精确匹配目标守卫异常文本，不接受任意提前异常；不是靠损坏字节、错 hash 或无效 fixture 制造拒绝。

兼容风险：正常运行生成的 restart 字节和物理公式不变；此前可通过但不满足源声明的状态开始拒绝。任意自定义控制器的历史语义仍依其明确声明的 validator；本轮未把 hash 当真实性签名。

## N-DYNAMICS-FINITE-003 — 有限状态/线性解仍可发布溢出的能量账本

**P1，已修补源码；标准库独立 binary64 复核支持机制，C++ 未执行。**

触发/影响：通用积分器在 initialize、trial/updateEnergy、restore 中只检输入/线性解，二次能量或功运算可在其后溢出，仍进入 accepted/candidate/checkpoint。例子为 `M=1,C=K=0,v0=1e200` 的初始能量 Inf；另一例 `v0=1e154,f=1e140,h=1e14` 的 `q1=1.5e168,v1=2e154,a1=1e140` 均有限，而动能/功为 Inf、balance 为 NaN。

证据与修补：

- `src/range-base-lib/src/rbl_rvector.cpp:198–208` 的 dot 是二进制乘加；`quadraticEnergy` 先计算 dot 再乘 0.5。仅线性残差认证不能覆盖后续二次运算。
- `src/range-solver-lib/src/rstructural_dynamics_integrator.cpp:86–95` 新增完整状态/能量有限性检查。
- `…cpp:370–394` initialize 在独立局部 state 完成 force、acceleration、energy 后才赋给 accepted；失败不覆盖此前状态。
- `…cpp:517–521` trial 通过 energy/state 检查后才置 candidateReady。
- `…cpp:606–613` restore 在局部重算并认证能量后才替换 accepted。
- 分层外层 seal/derive 可以另行拒绝非有限值，但这不能关闭通用积分器接口的缺陷。

回归：`src/range-solver-lib/tests/tst_structural_dynamics_integrator.cpp:577` `finiteInputsCannotPublishOverflowedEnergy`，覆盖新建失败不可读、正常大数正例、重复初始化失败保持 accepted、restore 重算溢出保持 accepted、有限 q/v/a 的能量-only trial 失败及较小 h 重试。回归精确匹配目标能量门异常文本，提前线性求解异常不能冒充通过；实际到达和通过仍待C++执行。

兼容风险：此前返回 Inf/NaN 状态的输入现在抛 `std::runtime_error`；普通可表征状态的方程、运算顺序、算法和阈值不变。初始化 force 回调仍接收显式初始 state；不再将尚未完成认证的初始 state 暴露为 accepted。

## 本轮调用链/数学合同覆盖

逐文件行段和具体检查见 JSON。重要的证据区分如下。

- **持久化与事务**：1.3.22/23/24 typed 格式及旧版本拒绝、短读与候选赋值、8/16/32 MiB 范围、负零/UTF-8/opaque、verified handle 不追 LINK、完整实验发布 no-throw 交换。`RModel` 格式层仅认证 source/record，物理 runtime 由 facade 认证。
- **材料/核函数/装配**：`cE=(sE)^-1,e=d*cE,epsilonS=epsilonT-d*cE*dT`，工程 Voigt；free-stress Schur；梁 Hermite/偏心 I0/I1/I2；MITC4 协变剪切 tying 与膜/弯曲/剪切；TET4/仿射 HEX8 J^-T 与内部势凝聚；质量不重复叠加，P/PT 保持虚功，传感器开短路/放大器有各自边界。
- **动态/控制/激励边界**：实际 M/C/K 与电切线、Newmark/alpha/CD、原矩阵局部误差而非仅全局范数、canonical 名义 dt 与实际端点 h、proposal→held→postFE、prepareFE/bindForceState 事务 ledger、左右端点激励、动作限幅率与 tick/phase、提交前后读出分界。
- **后处理**：F=Kq+GV、Q=CeV−Gᵀq、开路 GGᵀ/Ce、执行器直接项、C/V/N/Nm/m/rad 单位、大偏置小信号、source physical ID 到 display vectors、实际电极贡献聚合。
- **oracle 独立性**：重复调用 production element 重组装只证明映射/聚合；独立 z 矩/Hermite、2×2/标量 Newmark、Navier/EB、非线性 RK4 双细网格参考分别认证对应公式。没有把全部测试名称当成运行结果。
- **未泛化**：未把示例网格当任意网格收敛证明，未把 source hash 当认证签名，未把任意回调纯度视为由指针冻结自动保证，未凭薄板/极端尺度推测更改阈值。

## 实际检查与产物

全部项目产物位于 `artifacts/bugfix-audit-20261104/resume-20261105/numerical/`。

- `checks/check_arithmetic.py`：Python **3.14.4**，仅标准库，`python3 -I -B`。启动检查前显式设置并验证 HOME/TMPDIR/TMP/TEMP/XDG 路径在本目录，`tempfile.gettempdir()` 实际路径也验证。
- `checks/arithmetic-results.json` / `checks/arithmetic.log`：**4 项独立算术检查通过**——有限 Newmark 状态的二次账本溢出；混合电端口 Fraction 解析 F/Q/放大器；梁截面 z 矩 Fraction 参考；相同二进制端点不等于相同名义 dt。这些都不是生产 C++ 执行。
- model/solver 两个 `git diff --check` 均 exit 0，仅为格式检查。
- `checks/{model,file_io,solver}.diff`：读取的批次差异证据。
- `checks/review-inventory.json`：逐文件 current SHA/line counts/review depth、两个子模块 HEAD、总 inventory digest；JSON报告同时保存对应字段。
- 无新增 C++ 通过结论；无提交或 gitlink 更新；根应用/GUI其他工作区修改未触碰。

## 获授权环境后必须执行的步骤与预期

1. **针对性 C++**：先编译并执行新增四 selector 与原 trace selector；正例必须真实进入目标调用，异初值/无控制动作拒绝，能量-only 溢出不发布状态，当前 invocation 策略不变，ordinary MODEL 能读回。
2. **动态数值回归**：integrator 全 suite（CG/GMRES、弱耦合、Newmark/alpha/CD）、truss damped forcing、layered dynamics；原解析/收敛/原矩阵门槛全部保持。失败不能靠改阈值或 skip 关闭。
3. **恢复矩阵**：layered/axial 全 RAM/RBM split/phase、不同 start time、sensor-only 与 solid/TET/HEX；正常字节等价恢复，无控制参数不产生新动作；异名义 dt 即便相同端点也拒绝。
4. **真实发布/故障恢复**：fea-solver 的真实 NativeAcceptedCheckpointStore，trace 写入、最终 checkpoint、恢复 authority、stage/publish 故障、回滚失败禁重试；观测 receipt 序列只反映已提交 FE。当前测试替身未执行真实磁盘 trace。
5. **平台条件**：OpenMP 的 layered `serialPolicyRestoresParallelHost` 明确要求至少两可用线程；Windows/MSVC 下验证 binary64 long double/FMA、_dup/handle 非继承、QSaveFile/文件锁路径。Linux静态阅读不替代这些结果。
6. **应用侧边界**：激励 provider/导入/设计控制器（MPC/LQR 等）/频谱与显示的整体实现及数值验收由主线继续对账；本域仅认证对应接口与求解器物理链。所有测试未来同样需先将 profile/tmp/log 隔离至仓库并验证。
