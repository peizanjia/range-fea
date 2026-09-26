# Stage 9：控制导向分析与模型降阶

2026-09-26 后续增量：`--auto-modal-relative-tolerance` 按给定频点和所有通道选择达到相对 FRF 误差门槛的最低完整模态簇，可用 `--max-auto-modes` 限制阶数；无法达标返回失败和逐候选证据。它不保证频点之间的精度或闭环稳定。四杆 1% 示例须保留全部四模态。MATLAB 已通过 TCP 替代适配器完成真实 FE 回代，旧 stdin 环境问题仍保留为历史排查；详见 `docs/CONTROLLER_ADAPTERS.md`。

本阶段以可核对的原生有限元素模型为起点，交付工作点矩阵、降阶、频率响应、控制诊断与控制器全阶回代。Stage 0 的干净 Windows VM 终验及 Stage 6 的 MATLAB stdin 适配问题不阻挡 C++/Python 路径；它们仍是独立的未关闭项。

## 数据和物理边界

`RStructuralLinearModel::build` 在私有模型副本调用真实结构组装器，只组装，不推进时间或发布结果。线弹性 POINT/TRUSS1/BEAM1 导出参考几何上的常载静态平衡；缆索导出实际收敛平衡处的材料与几何切线，并使用与缆索 Newmark 相同的参考几何集中质量。导出默认串行确定性，模型/目的文件绑定完整原生内容、哈希和协作锁，JSON 用 QSaveFile 原子发布。

仅支持 1–256 个独立机械坐标、最多各 64 个输入/输出；必须有正定质量与稳定正定切线。支持全局节点力和点节点位移/速度/加速度感测。物理节点 ID、坐标顺序、约束局部框架及其全局方向、单位都随矩阵发布。输入条件保持常数；固定支承的非零位移通过真实结构 RHS 消元处理。既有初始位移/速度和动态结果不是此静态工作点。旧时间求解器在副本上关闭，不得混入时间步算子。

MPC/耦合、移动支承、非 293.15 K 热工作点、压电电路状态、缆索松弛切换点及奇异/不稳定系统明确拒绝。没有一般非线性多物理 descriptor 或任意规模降阶能力的认证。

## 数学契约

二阶小扰动模型为

\[
M\delta\ddot q+C\delta\dot q+K\delta q=B_f\delta u,
\quad\delta y=S_q\delta q+S_v\delta\dot q+S_a\delta\ddot q+D_f\delta u.
\]

`operating_point.force` 是长度 n 的常载广义力向量，不是输入通道命令。矩阵对应扰动；工作点的绝对位移及输出偏置另存。令 x=[δq;δv]，导出

\[
E=\mathrm{diag}(I,M),\quad A=\begin{bmatrix}0&I\\-K&-C\end{bmatrix},\quad B=\begin{bmatrix}0\\B_f\end{bmatrix},
\]

\[
C_o=[S_q-S_a M^{-1}K,\ S_v-S_a M^{-1}C],\qquad D_o=D_f+S_a M^{-1}B_f.
\]

所有 M^{-1} 表达通过线性求解实现，不显式形成逆矩阵。加速度的直接馈通不可省略。M 正定的边界排除了无穷特征值/脉冲型约束系统。

模态截断使用 SciPy 广义对称特征分解 KΦ=MΦΛ，质量归一化后投影 M/C/K/B 和全部感测行。截断边界不得切开近频模态簇。Craig–Bampton 保留全部指定界面坐标，内部采用固定界面模态与静态约束模态 Ψ=−Kii^{-1}Kib；矩阵和感测共同投影。指定的界面索引对应导出坐标表，不能用物理节点位置替代。

频响求解 Z(ω)=K+iωC−ω²M，H=(Sq+iωSv−ω²Sa) Z^{-1}Bf+Df，并报告新鲜残差与条件数；ω 单位 rad/s。奇异频点显式失败。全阶/ROM 对比输出各通道绝对误差和相对误差，相对分母 max(|Hfull|,1e−12) 明确记录；通道零点附近必须结合绝对误差解读。

PBH 诊断对实际有限特征值使用尺度化后的 SVD 秩和秩裕度，属于给定数值容差的诊断，不是符号精确证明。未受控/未被观测模态以及重复特征空间必须保留在报告中。

## 控制器和验证

离线设计提供共点节点速度反馈 u=−g y，选定质量归一化模态的估计增益 g=2ω(ζtarget−ζexisting)/(φᵀBf)²。只接受纯速度感测和共点输入/输出，不把该单模态目标当成全阶全部模态的阻尼保证；同时检查全阶闭环极点。

设计文件绑定导出模型和原生源哈希。Python NDJSON v1 控制器在真实 FE 接受步返回物理 N 命令；限制/条件化/接受事务仍由 FE 求解器负责。独立 NumPy Newmark 零阶保持回放用于对照实际 FE 的多自由度终态，并提供全阶/单模态/被动基线及控制功、时间域误差。离散采样稳定性必须由实际步长回代验证，连续极点检查不能代替它。

## 使用及参考

执行入口与可复跑命令见 [docs/STAGE9_CONTROL_ANALYSIS.md](docs/STAGE9_CONTROL_ANALYSIS.md)。验证结果在完成 Release 回归和独立审查后写入交接及开发日志。

数学依据：[SciPy eigh](https://docs.scipy.org/doc/scipy/reference/generated/scipy.linalg.eigh.html)、[NASA Craig–Bampton primer](https://femci.gsfc.nasa.gov/craig_bampton/Primer_on_the_Craig-Bampton_Method.pdf)、[Craig–Bampton 原始论文](https://doi.org/10.2514/3.4741)、[二阶 descriptor 控制分析](https://epubs.siam.org/doi/10.1137/060673977)。
