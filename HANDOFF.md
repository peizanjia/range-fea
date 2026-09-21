# 续作交接

最后更新：2026-09-21（Asia/Singapore）

## 用户目标与长期约束

1. Stage 0—3 已完成当前规划范围，继续推进并完成 Stage 4。
2. 网格优先使用成熟工具；TetGen 保留，Gmsh 作为可选 1D/2D/3D 后端，不把第三方 API 扩散到 GUI/求解器。
3. 保留原有非结构功能，它们不是当前开发任务。
4. 稳定、可验证的增量自动提交；子模块先提交，再提交根仓库指针与文档。
5. 后续结构动力学必须支持可配置并行、逐时间步控制输入、压电/拉索、全状态热耦合与外部控制项目接口。
6. Agent 最终作为 GUI 侧栏与 Geometry/Property/Mesh/Analysis/Control/Solve/Results 深度耦合，并通过受控接口做二次开发。
7. 始终维护 `DEVELOPMENT_LOG.md`、本文件及交互/运行记录；若因额度或平台限制停止，先写明目标、进度、验证、故障和下一步。

## 当前分支与提交

- 根仓库分支：`feature/structural-profile`。
- 本轮开始时根 HEAD：`dd3981b feat: close frame rotation and moment workflow`。
- `range-model-lib`：`3d2e79c feat: add unified Gmsh meshing pipeline`。
- `range-solver-lib`：`7e8f102 refactor: evaluate solver materials through context`。
- 本轮根里程碑将包含 Gmsh 部署规则、两个子模块指针、Stage 0—3 报告、roadmap、开发日志与本 handoff。

## 阶段状态

### Stage 0：完成当前主机交付闭环

- Qt/MSVC/OpenMP 运行时、ZIP、Inno Setup、CLI 与隐藏 GUI 冒烟已验证。
- `Qt6Core.dll` 测试弹窗死循环已修复。
- 发布前仍需无开发环境的干净 Windows 虚拟机终验。

### Stage 1：完成

- 非结构功能保留；structural profile 是显式可选策略。
- capability registry 统一管理 registered/implemented/tested/validated/exposed 状态并可序列化。

### Stage 2：完成当前规划范围

- 通用 DOF/EquationBook 支持混合 3/6 DOF。
- 已闭环：TRUSS1/2、BEAM1/2、TRI3/6、QUAD4/9、TET4、WEDGE6、HEX8。
- 平面应力、平面应变、轴对称、质量选项、热应变、梁转角/端矩/应力恢复均有自动化证据。
- 材料上下文已支持稳定场键及 committed/trial 内部状态语义；当前实际行为仍是温度查表线弹性，不宣称塑性/压电已实现。

### Stage 3：完成当前规划范围

- TetGen 保留并有闭合立方体回归。
- Gmsh 4.15.2 可选后端覆盖直线、样条曲线、偏置 seed、受约束 TRI、映射 QUAD、箱体 TET、局部尺寸场。
- 支持 TRUSS/BEAM 一二阶线族、TRI3/6、QUAD4/9 与 TET4 映射。
- physical group 转为命名集合；材料/约束/传感器/执行器映射有显式 transferred/rejected 结果。
- 质量摘要覆盖 Jacobian、边长比、歪斜度、最小角和翻转计数。
- 详细技术报告：`TECHNICAL_ROUTE_STAGE0_3.md`。

### Stage 4：下一工作阶段

需要先稽核现有条件/变量/监测点，不重复实现已有固定、滚子、局部坐标、端矩、线载荷、面压、温度等功能。待补重点：

1. 通用多点约束与线性方程；
2. 参考点、刚体区域、分布耦合与 Tie；
3. 基础位移/速度/加速度与初始位移/速度；
4. 统一传感器目录：节点/区域位移、速度、加速度、应变、应力、温度、电荷；
5. 统一执行器目录：节点力、面力、基础运动、热源、自定义输入；
6. 单位、方向、坐标系、稳定 ID、命名集合目标与冲突/刚体模态诊断。

## 本轮验证

- `tst_element`：通过。
- `tst_material`：通过。
- `tst_mesh_pipeline`：通过，含 TetGen 与 Gmsh 1D/2D/3D。
- `tst_structural_product_profile`：通过。
- `tst_solver_stress`：通过。
- 最近跨阶段聚焦回归：5/5，通过，总耗时 8.95 秒。
- Gmsh 单项回归：1/1，通过；无 Qt DLL 弹窗。

## Gmsh 环境

- 本机构建用 SDK：`build/deps/gmsh-4.15.2-Windows64-sdk`（`build/` 已忽略，不提交二进制）。
- 配置参数：`-DRANGE_GMSH_ROOT=D:/YOGA/Documents/Projects/range-fea/build/deps/gmsh-4.15.2-Windows64-sdk`。
- 官方 Windows SDK 用 MinGW 构建；MSVC 必须包含 `gmsh.h_cwrap`，不能直接链接原生 `gmsh.h` C++ ABI。
- 启用后测试目录会复制 `gmsh-4.15.dll`；正式 GUI/CLI 和安装器也有部署规则。
- 未配置 Gmsh 时模型库仍可构建，`getAvailableBackends()` 只报告 TetGen。

## 已知事项

- `pwsh.exe is not recognized` 是既有发布说明 post-build 警告，不影响目标生成。
- 全量 CTest 建议串行；历史上 `tst_line` 与 `tst_solver_acoustic` 并发时存在进程间交互失败，分别运行及串行全量均通过。
- 文档必须以 UTF-8 读取。
- Gmsh 当前参数化垂直切片不是完整 CAD 网格器；一般 CAD 改形后的投影重映射、二次体单元和六面体体网格仍是后续扩展。

## 紧接下一步

1. 提交本轮根仓库里程碑。
2. 稽核 Stage 4 现有类型、序列化、求解器处理和测试矩阵。
3. 先建立稳定的约束方程、传感器和执行器描述器，再接求解器，避免把新概念继续塞入边界条件枚举分支。
4. 每个 Stage 4 增量完成聚焦测试后提交；最终运行完整串行 CTest，更新 roadmap、开发日志和本 handoff。

推荐命令：

```powershell
git status --short
git submodule status
$env:OMP_NUM_THREADS='4'
cmake -S src -B build/vs2026 -DRANGE_BUILD_TESTS=ON -DRANGE_GMSH_ROOT='D:/YOGA/Documents/Projects/range-fea/build/deps/gmsh-4.15.2-Windows64-sdk'
cmake --build build/vs2026 --config Release --parallel 8 --target tst_mesh_pipeline tst_solver_stress
ctest --test-dir build/vs2026 -C Release --output-on-failure -j 1 --timeout 120
```
