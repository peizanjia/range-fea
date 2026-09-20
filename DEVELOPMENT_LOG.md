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
