# Stage 7 技术路线：拉索与压电

更新时间：2026-09-25。状态：首个数学核增量已集成和验证；尚未完成模型持久化、真实 FE/控制接线或本阶段验收。

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

## 4. 首个数学核增量验证

新增 `RStructuralCableKernel` 与 `RStructuralPiezoKernel`，不修改既有 FE 路径或能力声明。隔离 VS 18 2026 x64、Qt 6.11.1 Release 建置成功，CTest 2/2 通过。额外导出的 QTest 明细为拉索 8 passed、压电 7 passed（均含 init/cleanup），0 failed、0 skipped。证据在忽略目录 `build/audit-20260925/kernel-check-build/`、`cable-qtest.txt`、`piezo-qtest.txt`。主工程已注册两个测试，全目标 Release 建置成功、55/55 CTest 通过；求解器提交 `67c400c`。
