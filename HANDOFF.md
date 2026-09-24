# 续作交接

最后更新：2026-09-21（Asia/Singapore）

## 用户目标与长期约束

1. Stage 0–4 已完成当前规划范围；若当前阶段完成，继续推进后续 Stage。
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
- 模态 MPC 尚无零空间/主从消元。
- 现有结构瞬态仍是一阶历史格式，不得称为真实结构动力学。
- Gmsh 当前不是完整 CAD 网格器；CAD 改形后的投影重映射、二次体单元和六面体体网格仍是后续扩展。

## 紧接下一步：Stage 5

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
