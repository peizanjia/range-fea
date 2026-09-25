# Stage 7 技术路线：拉索与压电

更新时间：2026-09-25。状态：数学核、拉索配置持久化及受限静力 FE/CLI 已验证，完整 Release 回归 59/59；尚未完成拉索动力学/控制、压电 FE 或本阶段验收。

## 1. 拉索的本构与一致切线

首版明确采用参考面积和工程轴向应变的共旋转两节点索，非完整有限应变材料。参考长度 `L0>0`、轴向刚度 `EA>0`、参考初张力 `N0>=0`；当前端点差 `d=x2-x1`，长度 `l=|d|>0`，方向 `n=d/l`。

```text
k = EA/L0
Ntrial = N0 + k*(l-L0)
N = max(0,Ntrial)
fint = [-N*n; N*n]
Pi = max(0,Ntrial)^2/(2*k)
D = k*n*n^T + (N/l)*(I-n*n^T)      (Ntrial > 0)
Kt = [D,-D; -D,D]
```

松弛分支 `Ntrial<=0` 的力、刚度与能量均为零；零张力转折点采用松弛侧切线，力连续但不可作全域光滑假设。能量零点选在恰好失去张力处，因此有初张力时参考构形能量不为零。初张力下沿索轴刚度为 `EA/L0`，横向相对端点刚度为 `N/l`。不得将几何刚度重复加入材料轴向刚度。

上述切线是给定力的精确导数，可用所有六个位移方向的中心差分独立核验；整体平移不改变结果，刚体转动使力和切线协变。公式与 [OpenSees CorotTruss 实现](https://github.com/OpenSees/OpenSees/blob/master/SRC/element/truss/CorotTruss.cpp) 的材料/几何切线分解一致。不同应变、应力及面积定义会改变材料项，不应混用。

进入预应力模态与控制线性化前，必须在自由节点验证 `sum(fint)-fext` 平衡。任意指定每根索的张力不保证整体平衡；工作点寻找与残差门控须先于模态频率验收。松弛/重新张紧的真实 FE 路径需要非线性残差迭代和接受态事务，不能直接塞入固定线性算子积分器并宣称完成。

## 2. 一维压电能量与电荷符号

采用应力—电荷形式：

```text
T = c^E*S - e*E
D = e*S + eps^S*E
h(S,E) = 0.5*c^E*S^2 - e*S*E - 0.5*eps^S*E^2
T = dh/dS; D = -dh/dE
```

材料参数 `c^E>0`、`eps^S>0`，极化通过带符号耦合系数 `e` 表示。理论来源：[COMSOL 压电本构与电焓](https://doc.comsol.com/6.3/doc/com.comsol.help.sme/sme_ug_theory.06.043.html)、[Givois、Deü、Thomas，2021，作者稿](https://sam.ensam.eu/bitstream/handle/10985/22658/LISPEN_CS_2021_THOMAS.pdf?isAllowed=y&sequence=2)。

均匀杆长度 `L`、面积 `A`，相对伸长 `u=u_right-u_left`，电压 `V=phi_right-phi_left`，故 `S=u/L`、`E=-V/L`。离散电焓及其梯度为

```text
H = A/(2L) * (c*u^2 + 2e*u*V - eps*V^2)
[F; Qflux] = A/L * [c,e; e,-eps] * [u;V]
```

`Qflux=A*D` 表示介质右端外法向电位移通量；右端导体电极的自由电荷符号与之相反。API、边界条件和传感器必须明确这一区别。电焓 Hessian 的电学块为负，不应把整个耦合块当正定机械刚度交给 CG。

独立解析基准：短路 `V=0` 得 `kV=c*A/L`；开路 `Qflux=0` 得 `V=e*u/eps` 与 `kQ=(c+e^2/eps)*A/L`。开路刚度不得小于短路刚度。固定电压自由伸长为 `u=-e*V/c`。极化反向 `e -> -e` 应反转执行器/传感器符号，却不改变开短路刚度差。混合二阶导数互等是能量互易检查。

这里的短路刚度只针对上述均匀端口模型。一般连续体 FE 即使电极电压固定，内部电势仍可能是自由未知量；必须按真实电学约束分块，对需要消去的电势使用 `Keff=Kuu-Kup*Kpp^-1*Kpu`，不能直接把原始 `Kuu` 当成所有短路模型的机械刚度。

## 3. 实作与验收顺序

1. 独立数学核及解析、有限差分、能量和非法输入测试。
2. 显式材料/单元/边界配置与版本化持久化，拒绝不支持的组合。
3. 拉索平衡工作点与非线性接受步；压电电势约束、静电消元及机械状态接线。
4. 接入 Stage 6 的正确量纲控制命令、观测量、能量账和重启状态。
5. 独立索振和压电梁/片基准、全量回归及交付文件。

仅完成第 1 项不能标记 Stage 7 完成，也不能打开产品级 capability。

### 第二增量的静力边界

模型 `a7c6040` 增加 1.3.13 文件尾部，保存显式 TRUSS1 元素 ID、`L0`、`N0`、网格指纹与迭代容差；旧 1.3.12 读取为未启用配置。ASCII 新字段采用 17 位精度，二进制新块逐字段检查长度，解析成功后才替换目标。绑定复用既有 `v1/5sig` 网格指纹：拓扑 ID 精确，但坐标仅五位有效数字，不是精确几何哈希。

静力网络在自由 DOF 上求解 `r=fint-fext=0`，Newton 方向满足 `Kt*du=-r`，并以总势能的 Armijo 条件回溯。受约束位移在每次试探保持不变。线性内层需重新计算 `b-Ax` 认证；非线性接受判据为 `||r_free|| <= absTol + relTol*||fext_free||`，不以随迭代变化的巨大内力放宽容差。奇异切线、非有限量、线搜索或 Newton 耗尽均明确失败，不添加虚构弹簧。若能量下降已小于浮点分辨率，允许在 `32*epsilon` 乘势能计算尺度内接受候选，但候选物理残差还必须下降至少 10% 或达到原外层容差；最终返回的力平衡认证不变。该规则是数值容许策略，不是大型网络的严格能量误差界。

返回的 `fint-fext` 在支承行表示支承施于结构的反力，在自由行是平衡残差。旧结果变量 `FORCE` 保持节点内力语义，不冒充支承反力。松弛、零载荷构形可以满足力平衡，但没有唯一性或稳定性保证；完全松弛初态在载荷下可能没有可用 Newton 切线，本增量不保证能找到所有可达平衡构形。

只有收敛候选才进入结果写入；数值求解失败不发布候选位移/应力。本路径仍采用既有静力生命周期，不提供 Stage 5 动力学那样的完整步回滚，也不保证模型文件与事件日志的跨工件原子性。不能把这次静力验收描述为非线性动力学接受步事务已经完成。

首个 FE 路径只接受静力纯拉索模型、全局位移条件、点组恒定总力（均分至去重节点）、参考温度 293.15 K 的材料模量。环境温度必须保持参考值，重力分量必须显式设为零；本增量不计算自重，不得将默认重力静默忽略。动力学、模态、控制、重启、MPC、局部坐标和热场等组合须明确拒绝。`RSolverStress` 保持既有类型关系，在通用 run 的首个验证钩子检查配置，禁止构造后切换拉索模式而沿用旧尺度。本增量不提供 GUI 配置入口，也不能代替本阶段的索振、控制或压电梁验收。

## 4. 首个数学核增量验证

新增 `RStructuralCableKernel` 与 `RStructuralPiezoKernel`，不修改既有 FE 路径或能力声明。隔离 VS 18 2026 x64、Qt 6.11.1 Release 建置成功，CTest 2/2 通过。额外导出的 QTest 明细为拉索 8 passed、压电 7 passed（均含 init/cleanup），0 failed、0 skipped。证据在忽略目录 `build/audit-20260925/kernel-check-build/`、`cable-qtest.txt`、`piezo-qtest.txt`。主工程已注册两个测试，全目标 Release 建置成功、55/55 CTest 通过；求解器提交 `67c400c`。

## 5. 拉索静力 FE 与持久化验证

- 模型 `a7c6040`：配置复制、17 位 ASCII/二进制往返、1.3.12 默认回退、截断数据及非法拓扑拒绝。`2883889` 另修复元素组条件读取合并错误，加载明确快照时不再恢复已删除的重力或保留旧 BC/IC。
- 平衡服务 QTest 10 passed：包括轴向解析解、小力尺度、三自由度斜索、预张力松弛、高预张力势能舍入案例及失败门控。
- 实际 FE QTest 14 passed：包括 `EA=500 N, L0=2 m, N0=5 N, F=10 N` 的伸长 `0.02 m`；两索目标长度 `1.25 m`、每索 `100 N`、中心下向 `120 N` 的垂度 `0.75 m`；支持范围和失败不发布结果检查。
- CLI QTest 12 passed：含原有 Python 控制/续算测试，以及新增 ASCII、二进制模型写入—独立求解程序—结果读回，检查位移、内力、应力、参考坐标与完成事件。上述数目包含 init/cleanup，均 0 failed、0 skipped。
- 求解器提交 `a95e6f3`。针对性 CTest 3/3；全目标 Release 重建成功，完整 CTest **59/59、0 失败、15.28 秒**（`OMP_NUM_THREADS=4`、`-j 2 --timeout 180`）。证据为 `cable-full-build.log`、`cable-full-ctest.log`、`cable-integration-ctest-verified.log`、`cable-equilibrium-qtest-verified.txt`、`cable-fe-qtest-verified.txt`、`cable-cli-qtest-verified.txt`。

可复现配置入口见模型 `tests/tst_structural_cable_setup.cpp`、求解器 `tests/tst_structural_cable_fe.cpp` 和根 CLI `src/fea-solver/tests/tst_solver_audit_cli.cpp`。只启用已有 TRUSS1 元素还不会获得拉索语义；必须填写显式 cable setup 并在最终参考网格上 `bindToModel(model)`。
