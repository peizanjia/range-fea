# 开发执行与交互记录

本文件记录项目阶段执行中的关键决策、用户反馈、异常、纠正和验证结果。记录按时间追加；详细命令和代码差异以 Git 提交为准。

## 2026-09-20 Stage 0 交付闭环

- 用户目标：执行并纠错完成 Stage 0、1、2；Stage 3 网格生成优先采用成熟工具；保留原有非结构功能，但不把它们作为本轮开发任务。
- 打包问题：Release 安装树最初未携带 `libomp140.x86_64.dll`。CMake 现会定位 MSVC LLVM OpenMP runtime，缺失时配置失败，并将其安装到 `bin`。
- 测试事故：首次直接运行 Release CTest 时，测试执行文件旁缺少 Qt runtime，Windows 连续显示“找不到 Qt6Core.dll”。用户报告后立即终止 CTest；随后将 `windeployqt` 输出和 `Qt6Test.dll` 部署到测试目录，并为 CTest 增加逐测试超时。
- 纠正结果：`tst_structural_product_profile` 单测通过；Release 全量 CTest 为 28/28 通过，总耗时约 9.74 秒，无错误弹窗。
- 安装树验证：`Qt6Core.dll`、MSVC runtime 和 `libomp140.x86_64.dll` 均存在；从独立安装目录执行 `fea-solver --version` 成功，版本为 1.3.0。
- 交付物：生成 `range-fea-1.3.0-winnt-amd64.zip` 和 Inno Setup `range-fea-1.3.0-winnt-amd64.exe`。
- 环境补充：通过 winget 安装当前用户范围的 Inno Setup 6.7.3，并让 CMake 同时发现系统级与当前用户级安装位置。
- 安装器验证：正式安装器静默安装、CLI 启动和静默卸载均成功。
- GUI 验证：首次启动出现 `Crash report - Range FEA`，确认原因是此前强制终止 GUI 留下的 `range-fea.lck`，锁内 PID 已不存在。该锁已移入忽略的构建验证目录备份。随后从独立 install-tree 启动，主窗口标题为 `Range FEA :: Finite Element Analysis - [Beam] - Range FEA`，界面响应正常，并通过窗口关闭消息正常退出，退出码为 0，锁文件被正常清除。
- 未伪造结论：当前主机没有可直接使用的 Windows Sandbox，干净 Windows 虚拟机终验仍是发布前检查项；本轮以完整 runtime 清单和独立安装树启动作为可执行替代验证。

## 2026-09-20 Stage 1 能力注册与范围纠正

- 纠正 structural-only 草案：移除 GUI、文件打开、材料、结果与报告路径中的全局结构过滤，默认保留原有非结构功能。
- 求解策略：`RSolver` 默认使用 `AllRegisteredSolvers`；`StructuralProduct` 变为显式可选策略，并在选用时拒绝不支持的问题类型、旧一阶瞬态结构任务和无结构公式单元。
- 中央 registry：新增八个能力域，统一记录 registered、implemented、tested、validated 和 profile exposure；提供版本化 JSON 导出/导入，供 GUI 与后续 Agent 查询。
- 单一事实来源：structural profile 的分析、单元、材料和约束判断改为查询中央 registry。
- 通用检查：GUI 求解前明确报告“未配置问题任务”，不再依赖结构产品门禁提供该错误。
- 验证：聚焦测试 3/3 通过；Release 全量构建通过；CTest 28/28 通过，总耗时约 9.06 秒。
- 子模块提交：`range-model-lib` 为 `bf4a825`；`range-solver-lib` 为 `6722d36`。

## 2026-09-20 Stage 2 自由度与单元契约基础

- 新增通用 DOF registry：`UX/UY/UZ/RX/RY/RZ`、电势、温度和压力均有稳定 ID、名称、单位、物理场和分量元数据。
- 新增 ElementDescriptor：覆盖全部现有单元枚举，声明拓扑、阶次、节点数、自然维数、每节点 DOF、候选公式、默认积分点、一致/集中质量能力、材料属性、载荷和结果。
- 梁单元契约明确要求 6 DOF；连续体单元保持 3 平移 DOF；TRI/QUAD 显式列出平面应力、平面应变和轴对称目标公式。
- descriptor JSON 嵌入 capability registry 元数据，后续 GUI 和 Agent 可直接查询，而不需要解析求解器分支。
- 验证：新增 `tst_element_descriptor`；Release 全量 CTest 从 28 项增加到 29 项，29/29 通过，总耗时约 8.63 秒。
- 子模块提交：`range-model-lib` 为 `2caf682`。

## 2026-09-20 Stage 2 WEDGE6/HEX8 体单元闭环

- `range-model-lib` 新增 WEDGE6 的三角形 3 点 × 轴向 2 点积分，以及 HEX8 的 `2×2×2` Gauss 积分；两类单元均提供形函数和自然坐标导数。
- 补齐 WEDGE6/HEX8 的数值体积计算和外表面生成：WEDGE6 生成 2 个三角面与 3 个四边形面，HEX8 生成 6 个四边形面。
- 新增 `tst_element_volume`，验证分片一致性、导数和为零、积分总权重、一致质量矩阵对称性、质量向量、Jacobian、仿射线性场梯度再现、体积和边界面拓扑。
- 复用 `RSolverStress` 的通用三维实体装配，以 30 段轴向杆分别验证 WEDGE6 与 HEX8 的端位移、均匀轴向应力和支座反力。
- 测试纠正：两个 WEDGE6 拼成方形截面时，四个端点的等分集中力不是一致等效载荷；改为按三角形分摊面积使用 `1/6, 1/3, 1/3, 1/6` 后，两类单元均精确再现解析轴向解。没有通过放宽容差掩盖该问题。
- capability registry 已将 WEDGE6/HEX8 标记为结构公式 implemented、tested、validated，并暴露给 structural profile。
- 验证：三个聚焦测试 3/3 通过；Release 全量 CTest 30/30 通过，总耗时约 12.21 秒。
- 构建说明：受影响的三个测试目标均成功完成 Release 重建。`ALL_BUILD` 在未修改的 GUI `fea.vcxproj` 巨型单批次编译阶段超过 10 分钟工具上限后被终止；该命令把约百个 GUI 源文件交给单个无 `/MP` 的 `cl.exe`，属于现有构建性能问题，不是编译错误。
- 子模块提交：`range-model-lib` 为 `f381f2f`；`range-solver-lib` 为 `88ff9d3`。

## 2026-09-21 Stage 2 TRI3/QUAD4 二维公式闭环

- 模型状态：`RSurface` 新增持久化的 `PlaneStress`、`PlaneStrain`、`Axisymmetric` 结构公式选择；默认保持 `PlaneStrain` 以兼容旧模型。模型文件版本从 1.3.0 升至 1.3.1，旧文件读取时采用兼容默认值。
- 持久化纠错：新增 ASCII/二进制模型往返测试。定位并修正 Qt 6 文本流读取布尔值时消费字段分隔空格的问题，同时初始化 `RCondition` 原先未初始化的适用范围字段，消除默认环境条件序列化中的未定义行为。
- 坐标变换纠错：线/面单元的局部—全局变换矩阵改为节点块对角形式并显式清零；桁架装配和恢复同步改用各节点自己的局部列。新增单元级正交性与非本节点块为零的测试。
- 二维构成：`RSolverStress` 按曲面组选择平面应力或平面应变矩阵；轴对称作为可持久化契约保留，但求解器会在装配前明确报告尚未实现，不会静默采用错误公式。
- 曲面装配纠错：刚度现在包含厚度并按所有积分点累加；QUAD4 质量不再重复累加既有积分贡献；热等效节点力只使用两个法向热应变分量，并在局部向量完整形成后统一变换到全局坐标。
- 结果恢复纠错：热应力按 `D * [alpha*dT, alpha*dT, 0]` 每积分点扣除，不再混入厚度，也不在节点循环中重复扣除。
- 验证：TRI3/QUAD4 × 平面应力/平面应变共四组多单元条带解析测试通过，覆盖端位移、`sigma_x`、`sigma_y` 和支座反力；QUAD4 两种公式的自由热膨胀与约束横向热应力测试通过；轴对称拒绝诊断通过；原有静力/模态桁架回归通过。
- 全量验证：Release CTest 31/31 通过，总耗时 12.39 秒，无 Qt DLL 弹窗。
- 子模块提交：`range-model-lib` 为 `dfa284d`；`range-solver-lib` 为 `40b08cd`。

## 2026-09-21 Stage 2 结构质量矩阵选择闭环

- 模型配置：`RProblemSetup` 新增 `RMassMatrixType::Consistent/Lumped`，默认一致质量以兼容旧模型；模型文件版本从 1.3.1 升至 1.3.2，ASCII/二进制文件均持久化该选项，旧文件读取时保持一致质量默认值。
- 装配实现：`RSolverStress` 在线、面、体单元质量装配中统一读取模型选择。面单元在局部二维质量矩阵上集中化后再变换到全局；体单元对完整一致质量矩阵执行行和集中化。
- 线单元纠错：原实现只保留局部轴向方向余弦平方对应的全局对角项，遗漏不同全局分量间的耦合项。现改为 `R_t M_local R_t^T` 完整变换；集中质量先在局部节点矩阵上形成，再执行同一变换。
- 模型验证：新增 `tst_problem_setup`，覆盖默认值、复制、非法枚举拒绝以及一致/集中质量的 ASCII/二进制模型往返。
- 求解验证：固定—自由杆解析模态覆盖 TRUSS1 与 TETRA1 的一致/集中质量；TRI3、QUAD4 平面应力条带同时覆盖两种质量矩阵。一致质量频率位于解析值上方、集中质量位于解析值下方，且各算例误差均小于 2%。
- 全量验证：Release CTest 32/32 通过，总耗时 21.16 秒；无 Qt DLL 弹窗或测试超时。受影响目标构建成功，仍仅有既知的 `pwsh.exe is not recognized` post-build 警告。
- 子模块提交：`range-model-lib` 为 `b3bfb0f`；`range-solver-lib` 为 `049d7a8`。

## 2026-09-21 Stage 2 TRI3/QUAD4 轴对称公式闭环

- 公式契约：轴对称首版固定使用全局 XY 子午面，X 为非负半径、Y 为旋转轴、Z 为环向；曲面厚度不参与轴对称积分，体积权重统一为 `2*pi*r*det(J)*w`。
- 求解实现：新增轴对称四分量本构与 `[epsilon_r, epsilon_z, epsilon_theta, gamma_rz]` 应变矩阵，覆盖刚度、质量、体力、总力和热膨胀装配；结果映射为径向、轴向、环向和子午剪应力，并按三维应力状态计算 von Mises 应力。
- 输入诊断：拒绝负半径、偏离全局 XY 子午面的节点和退化 Jacobian；区域 pressure/area traction 因缺少边界法向语义会明确提示改用子午面边界线载荷，不进行静默误算。
- 恢复纠错：曲面结果恢复中的节点力现扣除热等效节点力，修复自由热膨胀位移和应力正确但节点力仍出现伪反力的问题；该修正同时适用于既有平面公式。
- 验证：TRI3 与 QUAD4 均通过自由轴对称热膨胀解析 patch；QUAD4 厚壁圆筒在 2、4、8、16 个径向单元上对 Lamé 位移和径向/轴向/环向应力严格收敛；非法子午面几何拒绝测试通过。
- 全量验证：Release CTest 32/32 通过，总耗时 25.43 秒，无 Qt DLL 弹窗或测试超时。
- 子模块提交：`range-model-lib` 为 `b453a49`；`range-solver-lib` 为 `0040bad`。

## 2026-09-21 Stage 2 通用 equation book

- 新增 `REquationBook`，以稳定的 `RDofDescriptor` 顺序建立节点与 DOF 类型到连续方程号的映射；不同节点可具有不同 DOF 集，停用约束后会确定性重排。
- `RSolverStress` 不再以 `3*node+component` 作为结构方程地址；系统尺寸、位移恢复、约束消元和全局矩阵/质量矩阵装配均查询 equation book。
- 单元装配维数改为读取 ElementDescriptor 的每节点 DOF 清单；现有 3 DOF 桁架、面和体单元行为保持不变，后续 BEAM1 可直接提交每节点 6 DOF 的局部矩阵。
- 局部约束坐标变换已能按 ElementDescriptor 分别变换平移与转角三分量块，为 3D 框架装配保留正确的 6 DOF 变换结构。
- 新增 `tst_equation_book`，覆盖混合 3/6 DOF 编号、DOF 停用后的连续重排和 resize 清空；`tst_solver_stress` 回归通过。
- 全量验证：Release CTest 33/33 通过，总耗时 28.88 秒。
- 子模块提交：`range-solver-lib` 为 `24566c4`。

## 2026-09-21 Stage 2 梁截面持久化契约

- `RLine` 在既有截面面积 `A` 之外新增局部 Y/Z 轴二次面积矩 `Iy/Iz`、Saint-Venant 扭转常数 `J` 和截面局部 Y 参考方向；惯性参数拒绝负值，方向输入归一化并拒绝零向量。
- 模型文件版本从 1.3.2 升至 1.3.3；ASCII 和二进制文件均保存新增截面字段，旧文件读取时使用零惯性矩和全局 Y 默认方向。
- 新增 `tst_line`，覆盖默认值、复制、非法输入拒绝和两类文件格式往返。ASCII 文本会按现有通用写入器的有限有效位数保存浮点数，测试采用与格式精度匹配的数值容差，不要求二进制级相等。
- 全量验证：Release CTest 34/34 通过，总耗时 19.04 秒。
- 子模块提交：`range-model-lib` 为 `90b51db`。

## 2026-09-21 Stage 2 BEAM1 结构内核

- 求解实现：新增两节点 3D Euler-Bernoulli BEAM1，按每节点 `UX/UY/UZ/RX/RY/RZ` 形成 12×12 局部刚度；覆盖轴向、双轴弯曲与 Saint-Venant 扭转，并通过截面局部 Y 方向构造右手坐标系后变换到全局。
- 质量与载荷：实现一致/集中质量矩阵、热膨胀轴力、均布线载荷、重力和自重；瞬态旧状态装配与位移恢复现保留节点转角。显式转角边界条件尚未建模，当前全三向平移固定节点暂按夹持端同步禁用三转角。
- 缩放纠错：求解前自动缩尺原先只处理长度和截面积，遗漏 `Iy/Iz/J`。现按长度四次方缩放三个截面量，并新增往返测试；该遗漏曾使缩尺 1/2 的梁弯曲刚度恰好放大 16 倍。
- 收敛纠错：CG/GMRES 改用 `||r||/max(||b||,eps)` 相对残差，避免病态尺度系统被过小后向误差误判；同时删除“残差趋势为零即收敛”的错误判据。HEX8 前两次迭代残差均为 1 的回归证明停滞不能代表收敛。
- 验证：BEAM1 两个主惯性轴的悬臂端载荷均严格复现 Euler-Bernoulli 三次位移曲线与支座反力；全局 Z 方向梁验证非平凡坐标变换；一致/集中质量均在 4% 内复现第一弯曲频率。
- 能力边界：BEAM1 内核已具备静力与模态计算证据，但显式转角约束、端弯矩/扭矩、转角/内力矩结果和弯曲应力恢复未闭环，故 capability registry 暂不将其暴露给 structural product profile。
- 并行情况：现有单元装配、矩阵运算和特征值求解已使用 OpenMP；本轮 Release 构建继续使用 `--parallel 8`。路线图新增运行级并行配置、确定性模式、实际线程数审计和未来后端能力要求。
- 验证结果：Release 全量 CTest 串行 34/34 通过，总耗时 9.91 秒；`ctest -j 8` 时其余 33 项及全部求解器测试通过，但 `tst_line` 与 `tst_solver_acoustic` 并发时可重复返回失败，二者分别单独运行均通过，待后续定位测试进程间交互后再恢复全量并行门禁。
- 子模块提交：`range-solver-lib` 为 `8e98727`。

## 2026-09-21 Stage 2 梁转角与端力矩闭环

- 模型契约：新增稳定 ID 的 `Rotation/Rotation X/Y/Z` 与 `Moment/Moment X/Y/Z` 变量，以及可选分量的全局转角约束和点实体端力矩边界条件；单位分别为 `rad` 和 `N*m`，中央 capability registry 与 structural profile 可直接查询。
- 约束实现：转角约束直接停用 equation book 中的 `RX/RY/RZ`，支持非零指定值和矛盾约束诊断。转角方程框架与平移 normal/roller 框架分离，避免全局转角约束被曲面局部方向静默旋转。旧模型没有 Rotation 条件时仍保留“三向平移全固定即夹持”的兼容行为。
- 载荷与结果：点实体 Moment 条件直接装配到共享梁节点的转动方程；BEAM1 结果恢复现累计全局节点内力矩，并持久化三分量节点 Rotation 和 Moment。没有转动自由度的实体模型不会被无意义的零转角/零力矩结果污染。
- 缩放纠错：转角按无量纲处理，力矩按 `kg*m^2/s^2` 缩放；修正后端弯矩算例不再随自动长度缩尺产生 4 倍误差。
- 解析验证：悬臂端弯矩严格复现 `theta=M*L/(E*I)`、`v=M*L^2/(2*E*I)` 和两端力矩平衡；非零指定端转角严格复现 `v=L*theta/2`、`M=E*I*theta/L`。模型元数据与量纲因子另有单元测试。
- 验证结果：聚焦测试 3/3 通过；Release 全量 CTest 串行 34/34 通过，总耗时 12.18 秒。
- 子模块提交：`range-model-lib` 为 `d355af4`；`range-solver-lib` 为 `615f197`。
