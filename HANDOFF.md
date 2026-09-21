# 续作交接

最后更新：2026-09-21 16:38（Asia/Singapore）

## 用户目标与约束

1. 执行、纠错并完成 `PROJECT_ROADMAP.md` 的 Stage 0、Stage 1、Stage 2。
2. Stage 3 网格能力优先集成成熟工具，不重复实现可靠算法；现有 TetGen 继续保留。
3. 保留仓库原有非结构功能；它们不是当前开发任务，不应被全局禁用或删除。
4. 有独立价值且验证通过时自动提交；保持仓库整洁，维护 `DEVELOPMENT_LOG.md` 与本文件。
5. 后续动力学必须预留控制输入、压电/拉索、全状态热耦合和其他控制耦合接口；Agent 最终作为与各阶段任务深度耦合的 GUI 侧栏，并可通过受控接口做二次开发。
6. 若因额度或平台限制停止，必须先在本文件写清目标、进度、验证、故障和下一步。

## 当前分支与已完成提交

- 根仓库分支：`feature/structural-profile`。
- Stage 0 根提交：`f39992c build: complete Windows runtime packaging baseline`。
- Stage 1 根提交：`d770cfe feat: make structural workspace optional`。
- Stage 2 元数据基础根提交：`1c19739 feat: establish stage 2 element metadata foundation`。
- WEDGE6/HEX8 根提交：`b019a6f feat: complete wedge and hexahedral solid support`。
- TRI3/QUAD4 二维公式根提交：`26eb595 feat: complete planar structural formulations`。
- 当前质量矩阵增量子模块提交：
  - `src/range-model-lib`: `b3bfb0f feat: persist structural mass matrix selection`
  - `src/range-solver-lib`: `049d7a8 feat: assemble selectable structural mass matrices`
- 当前轴对称公式增量子模块提交：
  - `src/range-model-lib`: `b453a49 feat: validate axisymmetric surface capability`
  - `src/range-solver-lib`: `0040bad feat: solve axisymmetric structural surfaces`
- 当前通用 equation book 增量子模块提交：
  - `src/range-solver-lib`: `24566c4 refactor: add structural equation book`
- 当前梁截面契约增量子模块提交：
  - `src/range-model-lib`: `90b51db feat: persist beam section properties`
- 当前 BEAM1 结构内核增量子模块提交：
  - `src/range-solver-lib`: `8e98727 feat: implement beam1 structural kernel`
- 当前梁转角与端力矩增量子模块提交：
  - `src/range-model-lib`: `d355af4 feat: add frame rotation and moment contracts`
  - `src/range-solver-lib`: `615f197 feat: solve frame rotations and end moments`
- 二维公式增量子模块提交：
  - `src/range-model-lib`: `dfa284d feat: add structural surface formulations`
  - `src/range-solver-lib`: `40b08cd feat: solve plane stress and plane strain surfaces`
- 上一体单元增量子模块提交：
  - `src/range-model-lib`: `f381f2f feat: implement wedge and hexahedral solids`
  - `src/range-solver-lib`: `88ff9d3 test: validate wedge and hexahedral solids`
- 当前两个新子模块指针、路线图、开发日志和本 HANDOFF 与本文件所在根提交一并交付。

## 阶段状态

### Stage 0：完成

- Windows 测试目录自动部署 Qt runtime 与 `Qt6Test.dll`，已消除 `tst_structural_product_profile.exe` 缺少 `Qt6Core.dll` 的循环弹窗。
- 安装树包含 MSVC runtime 与 `libomp140.x86_64.dll`；独立 `fea-solver --version` 成功。
- ZIP 与 Inno Setup EXE 已生成；静默安装、CLI 启动、静默卸载成功。
- GUI 从独立安装树启动、响应、正常关闭均成功。先前 crash report 是强制终止留下的过期 `range-fea.lck`，不是 Qt runtime 缺失。
- 尚缺发布前的干净 Windows 虚拟机终验；当前主机没有可直接使用的 Windows Sandbox。

### Stage 1：完成

- 非结构功能保留；GUI、文件读取、材料、结果和报告路径不再进行全局 structural-only 拒绝。
- `RSolver` 默认使用 `AllRegisteredSolvers`；`StructuralProduct` 是显式可选策略。
- `RCapabilityRegistry` 统一管理八个能力域及 registered/implemented/tested/validated/profile exposure，并支持版本化 JSON。
- structural profile 查询中央 registry，不再维护重复清单。

### Stage 2：进行中

已完成：

- 通用 DOF registry：`UX/UY/UZ/RX/RY/RZ`、电势、温度、压力。
- ElementDescriptor：拓扑、阶次、节点数、自然维数、DOF、公式、积分、质量、材料、载荷和结果，并嵌入 capability JSON。
- 既有基准：TRUSS1、TETRA1 静力/模态。
- TRI3/QUAD4：曲面组持久化 `PlaneStress`、`PlaneStrain`、`Axisymmetric` 公式状态；三类公式的装配与恢复均已完成，默认平面应变兼容旧文件。轴对称首版采用全局 XY 子午面（X = 半径、Y = 旋转轴、Z = 环向），按 `2*pi*r` 积分且不使用曲面厚度。
- TRI3/QUAD4 验证：四组平面多单元条带解析解覆盖位移、两个正应力分量和反力；平面与轴对称自由热膨胀测试覆盖热等效节点力和热应力恢复；轴对称厚壁圆筒对 Lamé 解完成 2/4/8/16 径向网格收敛验证。
- 轴对称载荷边界：二维区域上的 pressure/area traction 因语义不明确会被拒绝；压力应施加到子午面边界线，待 Stage 4 补齐一致边界线载荷。
- 结构质量矩阵：`RProblemSetup` 已持久化一致/集中质量选择，默认一致质量兼容旧模型；线、面、体单元装配均采用该选择。
- 结构质量验证：TRUSS1、TETRA1、TRI3、QUAD4 的固定—自由杆/膜解析模态均覆盖一致与集中质量，两者分别从解析值上方和下方收敛，误差小于 2%。
- 局部坐标：线/面单元变换矩阵已纠正为节点块对角形式；静力/模态桁架和二维面单元回归均通过。
- WEDGE6：6 点积分、形函数/导数、质量、Jacobian、数值体积、2 TRI + 3 QUAD 外表面、仿射 patch、三维弹性轴向解析解与反力验证。
- HEX8：`2×2×2` 积分、形函数/导数、质量、Jacobian、数值体积、6 QUAD 外表面、仿射 patch、三维弹性轴向解析解与反力验证。
- capability registry 已将 WEDGE6/HEX8 暴露为 implemented/tested/validated 结构单元。
- 通用 equation book：新增节点与 `RDofType` 到连续方程号的稳定映射，支持不同节点具有不同 DOF 集；`RSolverStress` 的系统尺寸、约束消元和全局矩阵装配不再使用 `3*node+component`，而是读取 ElementDescriptor 的每节点 DOF 清单。
- 6 DOF 变换基础：局部约束坐标变换已能分别旋转平移与转角三分量块；混合 3/6 DOF 编号、禁用后重排和清空重建有独立单元测试。
- 梁截面模型：`RLine` 已增加非负 `Iy/Iz/J` 和归一化局部 Y 方向，连同既有面积 `A` 通过模型文件 1.3.3 的 ASCII/二进制格式持久化；旧模型读取时保持零惯性矩和默认方向。
- BEAM1 内核：两节点 3D Euler-Bernoulli 12×12 刚度、一致/集中质量、热轴力、均布线载荷、重力和局部—全局坐标变换已完成；节点转角进入方程、瞬态旧状态和结果恢复。当前没有显式转角边界条件时，全三向平移固定节点临时解释为夹持端。
- BEAM1 验证：局部 Y/Z 两个弯曲方向均复现悬臂三次位移曲线和支座反力；全局 Z 方向梁验证坐标变换；一致/集中质量均通过一阶弯曲频率解析基准。
- 梁转角与端力矩：新增可序列化的全局 `RX/RY/RZ` 转角约束和点实体端弯矩/扭矩，支持非零指定转角；节点转角和节点内力矩现作为正式结果变量输出。旧文件仍保留“三向平移全固定即夹持”的兼容行为，显式 Rotation 条件会覆盖该回退语义。
- 转角/力矩验证：悬臂端弯矩严格复现 `theta=M*L/(E*I)` 和 `v=M*L^2/(2*E*I)`；非零指定端转角严格复现 `v=L*theta/2` 与两端内力矩平衡。
- 数值基础纠错：`Iy/Iz/J` 现随求解缩尺按长度四次方换算；CG/GMRES 使用常规相对残差，并取消“残差不变即收敛”的错误判定。

仍待完成：

1. BEAM1 产品闭环与 BEAM2：BEAM1 尚缺截面模数契约以及弯曲/扭转应力恢复；完成后才能在 structural product profile 启用。BEAM2 尚未实现。
2. TRUSS2、TRI2、QUAD2 仍未实现；QUAD2 当前枚举定义为 9 节点。
3. 材料接口的温度/内部状态依赖与后续压电耦合尚未实现。

### Stage 3：尚未接入新后端

- 当前已有 TetGen 四面体体网格。
- 计划：通过统一 mesher 接口保留 TetGen，并优先评估 Gmsh 官方 C++ API 作为线、面、四边形/六面体或混合网格及 CAD 网格后端。
- 集成前先验证许可证、Windows 二进制依赖、节点/单元映射、物理组到命名集合的保真，以及材料/约束/传感器/执行器重映射。

## 本轮验证结果

- `tst_problem_setup`：通过，覆盖质量矩阵默认值、复制、非法值拒绝及一致/集中质量的 ASCII/二进制往返。
- `tst_surface`：通过，覆盖默认值、复制、无效公式及 ASCII/二进制往返。
- `tst_element`：通过，包含线/面局部—全局变换矩阵块对角测试。
- `tst_solver_stress`：通过；除既有静力、热应变、约束和质量矩阵回归外，覆盖 TRI3/QUAD4 轴对称自由热膨胀、厚壁圆筒 Lamé 解网格收敛、非法子午面几何拒绝，以及新增的 BEAM1 双轴静力、旋转坐标系与一致/集中质量模态解析基准。
- `tst_equation_book`：通过，覆盖混合 3/6 DOF 节点的确定性编号、约束停用后的连续重排和 resize 清空语义。
- `tst_line`：通过，覆盖梁截面默认值、复制、非法负值/零方向拒绝以及模型 ASCII/二进制往返。
- `tst_scales`：通过，新增梁截面面积二次矩/扭转常数按长度四次方缩放及往返恢复。
- `tst_iteration_info_value`：通过，确认残差停滞不会被误判为收敛。
- Release 全量 CTest（`OMP_NUM_THREADS=4`，串行调度）：34/34 通过，总耗时 12.18 秒。
- 无 Qt DLL 弹窗或测试超时。
- 受影响测试目标的 Release 构建成功。

全量测试并行调度说明：两次 `ctest -j 8` 均为 33/34，通过的 33 项包含全部求解器测试；仅 `tst_line` 在与 `tst_solver_acoustic` 并发时返回失败，两个目标分别运行以及完整串行 CTest 均通过。当前先把构建并行与求解器 OpenMP 并行保留，完整回归门禁使用串行调度，后续定位该测试进程间交互后再恢复全并行门禁。

`ALL_BUILD` 说明：命令在 `fea.vcxproj` 编译阶段超过 10 分钟工具上限。其响应文件把约百个 GUI `.cpp` 一次交给单个没有 `/MP` 的 `cl.exe`；进程持续消耗 CPU且正常响应，没有编译错误。外层超时后仅终止该轮明确核对的 `cmake/MSBuild/cl` PID。三个受影响测试目标随后单独重建成功。不要把这次有界超时记录为全量构建通过，也不要误判为代码死锁。

## 关键测试纠正

- 两个 WEDGE6 拼成方形截面时，端面四节点的等分集中力不是一致等效面力，会产生约 1.8% 的截面畸变。
- 测试现按三角形分摊面积使用节点权重 `1/6, 1/3, 1/3, 1/6`；修正后端位移、轴向应力和总反力均通过 1% 解析解容差。
- 该纠正应保留，不能改回等分节点力后再放宽容差。
- 线/面单元的局部—全局变换必须是节点块对角矩阵；恢复旧的跨节点重复列会让二维刚度秩亏。桁架的 `B` 矩阵必须读取 `Rt[3*m+component][m]`，不能固定读取第 0 列。
- 平面单元刚度必须乘厚度并逐积分点累加；热应力恢复不乘厚度，且二维各向同性热应变只有两个法向分量。
- 线单元质量矩阵必须通过 `R_t M_local R_t^T` 完整变换；仅保留方向余弦平方的全局对角项会遗漏不同全局分量之间的耦合。面单元集中质量应先在局部二维矩阵上形成，再变换到全局坐标。
- 梁截面 `Iy/Iz/J` 的物理量纲是长度四次方，求解缩尺必须使用 `lengthScale^4`；只缩放节点和截面积会系统性放大或缩小弯曲/扭转刚度。
- 转角是无量纲量，力矩的量纲是 `kg*m^2/s^2`；若把端力矩留在默认单位缩尺中，长度缩尺 1/2 时梁转角会放大 4 倍、回写内力矩会缩小 4 倍。
- 迭代求解器不能把残差趋势为零当作收敛；块结构系统可能在前几步保持相同残差，必须以明确残差阈值判断。

## 已知构建/环境事项

- PowerShell 读取中文文档必须使用 `Get-Content -Encoding utf8`，否则显示为乱码；文件本身是 UTF-8。
- `pwsh.exe is not recognized` 是现有 post-build release note 脚本警告，目标仍成功生成。
- MSBuild 的 `/nodemode ... /nodeReuse:true` 进程是正常编译节点，不要当作孤儿进程随意终止。
- 测试顺序仍建议先聚焦再全量，并始终设置 `--timeout`。
- 当前完整 CTest 使用 `-j 1` 作为稳定门禁；`tst_line` 与 `tst_solver_acoustic` 的并发失败尚待定位，不能把该问题归因于 BEAM1 数值实现。
- 现有装配与矩阵计算已使用 OpenMP，可由 `OMP_NUM_THREADS` 临时控制；模型级 `Auto/SerialDeterministic/FixedThreads` 配置、确定性归约和实际线程数记录已写入 Stage 5 规划，尚未实现为持久化接口。

## 下一步

1. 为梁截面补充向后兼容的弯曲/扭转截面模数契约，完成 BEAM1 弯曲与扭转应力恢复，并在闭环后更新 capability registry；随后实现 BEAM2。
2. 补 TRUSS2、TRI2、QUAD2，或先通过 ADR 明确二次单元的优先级与 QUAD2 节点约定。
3. 扩展材料接口的温度/内部状态依赖，为后续压电和热耦合保留稳定契约。
4. Stage 2 稳定后进行 Gmsh 适配 spike，不直接把第三方 API 散布到模型/GUI。

推荐命令：

```powershell
git status --short
git submodule status
$env:OMP_NUM_THREADS='4'
cmake --build build\vs2026 --config Release --target tst_scales tst_iteration_info_value tst_solver_stress --parallel 8
ctest --test-dir build\vs2026 -C Release -R '^(tst_scales|tst_iteration_info_value|tst_solver_stress)$' --output-on-failure -j 3 --timeout 120
ctest --test-dir build\vs2026 -C Release --output-on-failure -j 1 --timeout 120
```
