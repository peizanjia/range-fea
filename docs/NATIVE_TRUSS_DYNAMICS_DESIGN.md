# CW-03/CW-07 原生混合杆索动力实现契约

2026-10-11。状态为设计冻结；共同装配和步内保持输入的纯动态候选核心已实现并完成聚焦验证，完整原生执行/控制/重启尚未验收。增量证据见 acceptance/CW-dynamics-core-20261011.md；完整需求仍以 CONTROL_WORKBENCH_DELIVERY.md 和 control_workbench_plan.json 为准，本设计不缩小任务。

## 同一结构、同一方程

主要场景：普通桁架杆替换为主动压电杆，或拆分后仅替换一段。TRUSS1 两机械端点共享结构的三平移自由度；电极身份独立于机械节点。普通杆、主动杆、张力仅受拉的拉索必须进入同一个全局残差、切线和积分步，不能分别求解后拼结果。贴片保留另一条梁/平板层合路线。

第一批 native core 为小材料应变、更新杆方向、固定拓扑/质量的三维铰接杆索；Newmark beta=1/4、gamma=1/2，集中质量。弯矩、转动/MPC、阻尼、共享电路与真实卷筒质量流在对应后续实现前明确拒绝。轴向等效器件系数与三维材料张量降维分离，不能把简单矩阵分量冒称任意自由横向边界下的等效参数。

## 元件本构与独立推导

设当前端点差向量为 r，l=|r|，n=r/l，delta=l-L。普通杆 `N=k*delta`、`U=k*delta^2/2`，其中 k=EA/L，允许受拉和受压。

主动杆端电压约定 `V=phi2-phi1`。沿长度的电场为 `-V/L`。将线性 stress-charge 本构积分到轴向等效端口，定义 k>0 [N/m]、g [N/V]、Ce>0 [F]：

```
H(delta,V) = 0.5*k*delta^2 + g*delta*V - 0.5*Ce*V^2
N          = dH/ddelta = k*delta + g*V
Q2         = -dH/dV    = Ce*V - g*delta
U          = H + V*Q2  = 0.5*k*delta^2 + 0.5*Ce*V^2
```

电压给定时切线 `dN/ddelta=k`；电荷给定时 `V=(Q2+g*delta)/Ce`，切线 `kQ=k+g^2/Ce`。每个元件只有显式独立的电极对；共享机械节点不连接电极。开路为保持 Q2 的边界，短路为 V=0；不能把电压驱动输入 V 同时当独立变形测量。电荷边界下消元后的内部能 U 是机械势能；电压边界下用于力求解的势为 H，能量账仍用 U。普通杆没有电学量，结果使用类型/可用性表达，不能补假介电常数。

杆端内力 `[-N*n,+N*n]`。六自由度切线的 3x3 基块为 `km*n*nT+(N/l)*(I-n*nT)`，按 `[+,-;-,+]` 装配；km 为相应电学边界下的轴向切线。有限旋转客观性、端点反向/极化符号和能量梯度必须独立验证。受压杆几何切线可能不正定，不能复用 cable Jacobi-CG 的正曲率假设。

拉索原样使用现有工程应变 kernel：`k=EA/L0`、`N=max(0,N0+k*(l-L0))`、`U=N^2/(2*k)`。初始 N0 是固定预应力参数，必须整体平衡后才作为模态工作点；控制输入为实际 applied L0，而非冒称外力。质量以初始物理质量长度/固定器件质量确定，改变有效 L0 不改变 M，明示为等效长度作动，不是完整卷筒/物质通量模型。

## 纯输入和候选 API

新增 `rstructural_truss_dynamics.{h,cpp}`，再增加单独 RModel adapter 和 RSolverStress 路由，不放宽旧纯 cable 或 static piezo 的拒绝门槛。

- Immutable system：参考几何、稳定物理实体 ID、分类/材料定律、固定集中质量、受控 prescribed DOF 和冻结曲线/端口绑定。
- Applied inputs：每个 cable 的 L0，每个 active rod 的 voltage 或 conductor-charge 边界，以及外力、prescribed u/v/a。所有量均 SI 且维数/身份完整。
- Accepted state：t、q/v/左侧 a、实际 applied inputs、支座反力、机电/参数/外力/支座累计功、控制 tick/FE phase 和外层 controller/queue state。
- Pure evaluate：对共同结构返回 internal force、sparse tangent、signed member force、active electrical output 和物理 U/H。
- Pure refresh：用当前 q/v 与右侧新 input 计算自由 a，prescribed a 使用解析曲线值。
- Pure trialStep：输入 accepted state 和左右/终端 input path，返回完整候选；任何异常不改变 caller 的 accepted 对象。

求解残差 `r=M*a+fint-fexternal`。Newmark 仅更新自由自由度；prescribed u/v/a 使用现有 C2 五次 Hermite 的解析值，不能用终端 u 反算假速度/加速度。共同 sparse tangent 经原生 GMRES 求解后必须检查新鲜线性残差；非线性全局化使用自由残差 merit，不保留 cable 必须为势能下降方向的条件。收敛后重新装配并核查自由物理残差。

输入路径要区分 tn-、tn+、tn+1。L0/电学命令可保持，长度变化造成有限参数功和右侧加速度刷新；anchor u/v 不允许跳变，不支持的冲击明确拒绝。平滑 L0 曲线必须证明整段正值，包括 Hermite 区间内部极值；只检查 knot 不能通过验收。

## 功账与交易

完整支座反力为 prescribed DOF 上的 `M*a+fint-fexternal`，包括支座质量惯性。支座功是 `integral(R dot v_support dt)`，不再重复计 anchorTranslation 的能量导数。

电学输入功 `We=integral(V*dQ2)`；物理内部能使用 U，不能用 H 冒称储能。固定 q/v 的电学命令切换，按相应电学路径记录精确储能差；连续步使用明确求积并保留误差/细化证据。

长度参数功 `WL0=integral((partial U/partial L0)*L0dot dt)`，固定 EA/N0。保持型命令在 tn 的固定 q/v 跳变记精确 `U(qn,L0+)-U(qn,L0-)`；之后再计算右侧 a。当前 ControlInput 在 Ntrial=0 拒绝力映射不可微，但 U 的参数导数可为零，合法 slack/taut 转换不能因此拒绝。previous energy/反力必须以旧 inputs 求得，不能以新 L0 重算旧值。

诊断账为 `delta(T+U)-Wexternal-Wsupport-We-WL0`，分开命令跳变与步内功。Newmark 非线性步不保证精确能量守恒，需 dt 细化/独立参考，不能通过调阻尼隐藏误差。

交易为：左侧接受态观测 → controller/queue/held 候选 → 验证整个 input path → 右侧 a 刷新 → 共同 Newton trial → 残差/功证书 → stage 输出/checkpoint → 发布及存储 → noexcept accepted commit。旧 RSolverGeneric accepted observer 在 commit 后调用，且失败不回滚已接受内存，不能作为新 checkpoint 的交易参与者。新路由应增加提交前 checkpoint 发布/回滚接口；旧 observer 保持通知语义。多文件/跨进程发布不宣称全局原子，保留明确恢复状态。

## 有界实施顺序及硬验收

1. 独立 mixed element law 与共同稀疏装配：轴向/横向有限差分、梯度/切线、互易、自由/阻塞、开短路、刚体旋转、受压 bar 与 slack cable 同时存在。
2. 纯动态 driver：全 prescribed anchor 的解析 u/v/a 与非零质量反力；自由质量的独立线性振子；固定端长度作动及 slack/taut，命令跳变左右加速度，功账随 dt 收敛。全程候选无副作用。
3. Native RModel/CLI/完整 checkpoint 与控制连接：全输入身份、错误发布回滚、同 solver retry、各采样相位 split/continuous 等价。旧 v1/v2 回归独立保留。
4. 非共线混合桁架及预应力环形天线模态/瞬态/闭环，一体 GUI 与标准器件/传感绑定、少操作测量和实际安装验收。

完整 CW03/CW07 验收必须走到各自全部条款；前两项局部 kernel/driver 通过只记录增量，不能关闭对应任务。

独立反例至少含：L0<=0/NaN/区间 overshoot、EA/L0 溢出、零当前长度、自由零质量、重复 member/DOF ownership、anchor 非受控支座、未支持 rotation/MPC、电学共享电路、曲线外推、改变冻结定律/质量/绑定、checkpoint input/phase 损坏以及 Newton/result/checkpoint 发布故障。

## 来源与当前代码证据

本构理论依据：[COMSOL Piezoelectricity](https://doc.comsol.com/6.3/doc/com.comsol.help.sme/sme_ug_theory.06.043.html)；端口/电极边界依据：[SDTools full-order piezo model](https://www.sdtools.com/help/piezo/pz_full.html)。以上为物理/接口背景，本项目公式由上述显式端电压约定推导，未复制其程序实现。

现有代码复用点：拉索 tension-only 本构、RStructuralPrescribedMotion、RMatrixSolver GMRES。新 rstructural_truss_assembly 保留原拉索数学定律，采用缩放运算避免 Windows 中间乘法溢出；旧拉索路由不改变。新 rstructural_truss_dynamics 冻结系统副本，以原生 GMRES、新鲜残差和解析支座运动实现保持输入的纯候选步。连续 L0/电学路径、native adapter、控制连接和提交前 checkpoint 发布尚待实现。

旧边界仍保留：rstructural_cable_model_adapter 的全 cable/固定零支座要求；rstructural_cable_dynamics 对所有 DOF 的 Newmark 反算；rstructural_cable_dynamics_restart_adapter 的 immutable L0 指纹；rsolvergeneric 提交后 accepted observer。新核心的局部测试不能扩大这些旧入口能力。
