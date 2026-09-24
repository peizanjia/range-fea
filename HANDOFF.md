# 续作交接

最后更新：2026-09-24（Asia/Singapore；Stage 5 完成，本轮按用户要求退出）

## 当前续作快照（先读本节；下文 Stage 4 结项记录为历史快照）

- 用户最新指令：完成当前 Stage 5 后退出；Stage 6 不在本轮范围。中文交互用正体字，仓库技术文档沿用简体字；稳定增量自动提交，子模块先提交、根仓库再提交指针。以下是结项快照，下文 Stage 4 记录是历史资料。
- 根分支 `feature/structural-profile`。模型子模块 `1174547`（模型文件 1.3.10，可选支承功账与 ASCII/二进制续算）；求解器子模块 `761de0a`（接受步支承动力反力/功率/累计功与 observer）。此前 `265abcf` 修复模态阻尼比精度及未初始化的 `RModelData::selected`，`d931c11` 区分旧一阶与新二阶产品标记；求解器 `6972a47` 实现有限规模模态阻尼，`6c5dd91` 新增 384 杆并行基准，`645f1f9` 暴露约化有效力/阻尼功率。根仓库 `523c31f` 接入 CLI 运行事件，`2167d28` 修复默认线程覆写。最终根提交及子模块指针以 `git log -1` / `git submodule status` 为准。
- Stage 5 在约定的**控制输入关闭的被动二阶结构动力学范围**完成：Newmark/广义 alpha/中心差分，Rayleigh/有限规模模态阻尼，MPC 消元、移动支承、磁盘 checkpoint、失败步回滚、可配置并行与运行级审计。移动支承受限行给出物理反力、功率、累计功，和约化自由 DOF 能量账分开；CLI `.events.jsonl` 有 run ID、顺序事件及输入/结果工件哈希，但不是完整 InteractionRecord。
- 主 `build/vs2026` 的 Release **全目标重建成功，CTest 49/49 通过**（`OMP_NUM_THREADS=4`、`-j 2`、`--timeout 180`）。`fea.exe`、`Release/Qt6Core.dll`、`Release/qtlib/platforms/qwindows.dll` 已部署；本轮只验证文件存在及构建，没有重新做干净 Windows 机器上的安装/启动终验。更早的 46/48 Not Run 是未建置新测试的历史尝试，不代表最终结果。
- 支承反力/功仅对全局点节点平移支承及无阻尼/Rayleigh 阻尼开放；MPC/耦合、局部坐标、含转角梁与模态阻尼组合明确拒绝。可选 observer 须在求解前开启；旧 checkpoint 无功账时不能中途开启，已有功账而不继续观察可续算核心状态。支承行 `R=(Ma+Cv+Kq-f)_c` 是支承施于结构的力，结构对支承的反力符号相反；`nodeForce` 不能替代。支承功和自由 DOF 有效力功均不能冒充控制器功率或全系统能量。
- 剩余限制：步事务深复制模型与共享状态；target 成功但 latest link 失败可能留下孤儿 target，模型/事件 journal 不具跨工件统一原子性；系统指纹受旧 ASCII 精度约束；多线程可配置但不保证每个核实际并行、严格确定性或一定加速；移动支承与时变 MPC 的组合仍未接通。Stage 6 才建立控制力来源、逐步传感器—执行器协议、控制器状态回滚和完整 InteractionRecord；Stage 7/8 才做压电/拉索与全状态热耦合。详见 `TECHNICAL_ROUTE_STAGE5.md`。

## 用户目标与长期约束

1. Stage 0–4 已完成当前规划范围；本轮完成 Stage 5 后即退出，Stage 6 留待用户下次要求。
2. 网格优先使用成熟工具：TetGen 保留，Gmsh 作为可选 1D/2D/3D 后端；第三方 API 不扩散到 GUI/求解器。
3. 原有非结构功能保留，不是当前主动开发任务。
4. 稳定、可验证增量自动提交；子模块先提交，根仓库再提交指针和文档。
5. 结构动力学必须支持可配置并行、逐时间步控制输入、压电/拉索、全状态热耦合和外部控制项目接口。
6. Agent 最终作为 GUI 侧栏与 Geometry/Property/Mesh/Analysis/Control/Solve/Results 深度耦合，并通过受控接口做二次开发。
7. 始终维护 `DEVELOPMENT_LOG.md`、本文件和交互/运行记录；若因额度或平台限制停止，先写清目标、进度、验证、故障和下一步。

## 当前分支与提交

- 根仓库分支：`feature/structural-profile`。
- 本轮开始时根 HEAD：`86388e0 feat: complete stages 2 and 3 structural pipeline`。
- `range-model-lib`：`a53e96e feat: add structural interaction model contract`。
- `range-solver-lib`：`ef0b089 feat: enforce static structural MPC equations`。
- 根仓库本轮将提交：两个子模块指针、Stage 4 报告、roadmap、README、开发日志和本 handoff。

## 阶段状态

### Stage 0：完成当前主机交付闭环

- Qt/MSVC/OpenMP 运行时、ZIP、Inno Setup、CLI 与隐藏 GUI 冒烟已验证。
- `Qt6Core.dll` 测试弹窗死循环已修复。
- 仍建议在无开发环境的干净 Windows 虚拟机完成发布终验。

### Stage 1：完成

- 非结构功能保留；structural profile 为显式可选策略。
- capability registry 统一记录 registered/implemented/tested/validated/exposed 并可序列化。

### Stage 2：完成当前规划范围

- 通用 DOF/EquationBook 支持混合 3/6 DOF。
- 已闭环 TRUSS1/2、BEAM1/2、TRI3/6、QUAD4/9、TET4、WEDGE6、HEX8。
- 平面应力、平面应变、轴对称、质量选项、热应变、梁转角/端矩/应力恢复已有自动验证。
- 材料上下文支持稳定场键、时间和 committed/trial 状态；当前实际行为仍是温度查表线弹性，不宣称塑性/压电已实现。

### Stage 3：完成当前规划范围

- TetGen 封闭面四面体化保留；Gmsh 4.15.2 可选后端覆盖直线/曲线、TRI/QUAD/TET、偏置 seed、局部尺寸场和命名集合。
- TRUSS/BEAM 一二阶、TRI3/6、QUAD4/9、TET4 映射和质量摘要均有回归。
- 详细报告：`TECHNICAL_ROUTE_STAGE0_3.md`。

### Stage 4：完成当前规划范围

- `RStructuralInteractionSetup` 持久化通用线性方程、刚体/分布/Tie 耦合、传感器和执行器；模型版本 1.3.5，旧文件兼容。
- 传感器目录覆盖节点/区域位移、速度、加速度、应变、应力、温度、电荷；执行器目录覆盖节点力、面力、基础运动、热源和自定义输入。
- 所有对象具有 stable ID、单位、方向、坐标系、命名集合目标；执行器另有上下限和速率限制。
- 刚体耦合按 `u_i=u_r+theta_r×r_i` 展开；分布耦合当前等权；Tie 当前按容差内最近节点匹配。
- 静力 MPC 已真实进入求解：全局—局部坐标转换、已知值消元、秩约简、冗余跳过、矛盾拒绝、拉格朗日乘子和 GMRES 切换。
- 数值纠错：约束块按最大对角刚度缩放，修复无量纲约束相对刚度过小导致 GMRES 提前满足整体残差而违反 MPC 的问题。
- 结构初始速度和温度现在允许定义；真实二阶状态消费仍属于 Stage 5。
- 模态 MPC 明确拒绝，后续必须采用约束消元，不能使用导致奇异质量矩阵的乘子形式。
- 详细报告：`TECHNICAL_ROUTE_STAGE4.md`。

## 本轮验证

- 全部 35 个 CTest 目标已在模型 ABI 变化后重新构建。
- Release 串行 CTest：35/35 通过，总耗时 13.28 秒。
- `tst_problem_setup`：10/10 通过，包含 1.3.5 ASCII/Binary 往返和 Stage 4 模型校验。
- `tst_solver_stress`：完整通过，包含解析 MPC 和矛盾 MPC 拒绝。
- 全量重建暴露了一个陈旧测试：它仍假设 BEAM1 未实现；现已改为验证 structural profile 接受已完成的 BEAM1，测试通过。
- 无 Qt DLL 弹窗；仍有既知、无害的 `pwsh.exe is not recognized` 发布说明 post-build 警告。

## Gmsh 环境

- 本机构建 SDK：`build/deps/gmsh-4.15.2-Windows64-sdk`（`build/` 已忽略，不提交二进制）。
- 配置参数：`-DRANGE_GMSH_ROOT=D:/YOGA/Documents/Projects/range-fea/build/deps/gmsh-4.15.2-Windows64-sdk`。
- 官方 Windows SDK 为 MinGW 构建；MSVC 使用 `gmsh.h_cwrap` C ABI 包装层。
- 测试、GUI/CLI 与安装规则会部署 Gmsh DLL 和许可证。

## 已知限制

- Stage 4 传感器/执行器是可持久化和可校验目录；逐时间步采样、命令应用、延迟/饱和执行与 InteractionRecord 属于 Stage 5/6。
- 刚体耦合的参考节点若需要偏置转动效应，必须实际具有转角 DOF；专用参考点 DOF 尚未实现。
- 分布耦合尚无面积/长度/用户权重；Tie 尚无非匹配面的形函数投影。
- 模态/二阶动力 MPC 已有独立坐标消元，但与时变支承运动的组合尚未接通；非零/时变 MPC 及局部框架须继续按现有拒绝边界处理。
- 旧版结构瞬态仍保留一阶历史路径，不能把它误标为新二阶结构动力学；必须显式启用 `RStructuralDynamicsSetup`。
- Gmsh 当前不是完整 CAD 网格器；CAD 改形后的投影重映射、二次体单元和六面体体网格仍是后续扩展。

## 历史 Stage 4 交接时建议的下一步：Stage 5（现已完成约定范围）

1. 建立独立 `q/qDot/qDDot` 状态和可提交/回滚的时间步状态容器。
2. 先实现 Newmark 平均加速度基线与 Rayleigh 阻尼，再做广义 alpha。
3. 定义可持久化 `RParallelExecutionConfig`：Auto、SerialDeterministic、FixedThreads；统一装配、线性/特征值和时间步 execution context。
4. 把初始位移/速度、基础位移/速度/加速度和 Stage 4 执行器接入真实时间步。
5. 引入约束消元，使 MPC 同时一致变换 `K/M/C/f`，然后开放模态和动力 MPC。
6. 建立单自由度自由振动、阶跃/脉冲/谐波、阻尼衰减、梁横振、时间步收敛和能量平衡测试。
7. 每个稳定增量测试后提交；阶段结束更新 roadmap、技术说明、开发日志和本 handoff。

推荐命令：

```powershell
git status --short
git submodule status
$env:OMP_NUM_THREADS='4'
cmake -S src -B build/vs2026 -DRANGE_BUILD_TESTS=ON -DRANGE_GMSH_ROOT='D:/YOGA/Documents/Projects/range-fea/build/deps/gmsh-4.15.2-Windows64-sdk'
cmake --build build/vs2026 --config Release --parallel 2 --target tst_problem_setup tst_solver_stress
ctest --test-dir build/vs2026 -C Release --output-on-failure -j 1 --timeout 120
```
