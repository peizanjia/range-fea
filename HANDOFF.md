# 续作交接

最后更新：2026-09-20 20:12（Asia/Singapore）

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
- 本轮子模块提交：
  - `src/range-model-lib`: `f381f2f feat: implement wedge and hexahedral solids`
  - `src/range-solver-lib`: `88ff9d3 test: validate wedge and hexahedral solids`
- 上述两个新子模块指针、路线图、开发日志和本 HANDOFF 尚待根仓库提交。

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
- 既有基准：TRUSS1、TETRA1 静力/模态；TRI1/QUAD1 当前为二维膜且本构实际对应平面应变。
- WEDGE6：6 点积分、形函数/导数、质量、Jacobian、数值体积、2 TRI + 3 QUAD 外表面、仿射 patch、三维弹性轴向解析解与反力验证。
- HEX8：`2×2×2` 积分、形函数/导数、质量、Jacobian、数值体积、6 QUAD 外表面、仿射 patch、三维弹性轴向解析解与反力验证。
- capability registry 已将 WEDGE6/HEX8 暴露为 implemented/tested/validated 结构单元。

仍待完成：

1. TRI3/QUAD4：增加显式平面应力、平面应变、轴对称公式选择；补解析、patch、网格收敛和静力/模态证据。
2. 一致质量/集中质量的模型级选择，而不只保留静态元数据。
3. 通用 DOF 装配：`RSolverStress` 仍硬编码每节点 3 个平移 DOF。
4. BEAM1/BEAM2 与框架：实现 `UX/UY/UZ/RX/RY/RZ`、局部坐标、截面和 6 DOF 装配。
5. TRUSS2、TRI2、QUAD2 仍未实现；QUAD2 当前枚举定义为 9 节点。
6. 材料接口的温度/内部状态依赖与后续压电耦合尚未实现。

### Stage 3：尚未接入新后端

- 当前已有 TetGen 四面体体网格。
- 计划：通过统一 mesher 接口保留 TetGen，并优先评估 Gmsh 官方 C++ API 作为线、面、四边形/六面体或混合网格及 CAD 网格后端。
- 集成前先验证许可证、Windows 二进制依赖、节点/单元映射、物理组到命名集合的保真，以及材料/约束/传感器/执行器重映射。

## 本轮验证结果

- `tst_element_volume`：通过。
- `tst_structural_product_profile`：通过。
- `tst_solver_stress`：通过，包含 WEDGE6/HEX8 两个新数据行。
- 聚焦回归：3/3 通过。
- Release 全量 CTest：30/30 通过，总耗时约 12.21 秒。
- 无 Qt DLL 弹窗或测试超时。
- 受影响测试目标的 Release 构建成功。

`ALL_BUILD` 说明：命令在 `fea.vcxproj` 编译阶段超过 10 分钟工具上限。其响应文件把约百个 GUI `.cpp` 一次交给单个没有 `/MP` 的 `cl.exe`；进程持续消耗 CPU且正常响应，没有编译错误。外层超时后仅终止该轮明确核对的 `cmake/MSBuild/cl` PID。三个受影响测试目标随后单独重建成功。不要把这次有界超时记录为全量构建通过，也不要误判为代码死锁。

## 关键测试纠正

- 两个 WEDGE6 拼成方形截面时，端面四节点的等分集中力不是一致等效面力，会产生约 1.8% 的截面畸变。
- 测试现按三角形分摊面积使用节点权重 `1/6, 1/3, 1/3, 1/6`；修正后端位移、轴向应力和总反力均通过 1% 解析解容差。
- 该纠正应保留，不能改回等分节点力后再放宽容差。

## 已知构建/环境事项

- PowerShell 读取中文文档必须使用 `Get-Content -Encoding utf8`，否则显示为乱码；文件本身是 UTF-8。
- `pwsh.exe is not recognized` 是现有 post-build release note 脚本警告，目标仍成功生成。
- MSBuild 的 `/nodemode ... /nodeReuse:true` 进程是正常编译节点，不要当作孤儿进程随意终止。
- 测试顺序仍建议先聚焦再全量，并始终设置 `--timeout`。

## 下一步

1. 提交根仓库的两个子模块指针和三份记录文档。
2. 审核 TRI1/QUAD1 当前二维本构、厚度、局部坐标与结果恢复路径，设计模型级 `PlaneStress/PlaneStrain/Axisymmetric` 选择，避免用 capability 元数据替代实际模型状态。
3. 先实现平面应力/平面应变并建立单元 patch + 多单元解析/收敛测试；轴对称单独实现和验证。
4. 完成二维增量后，再设计通用 DOF equation book 与 6 DOF 梁/框架装配。
5. Stage 2 稳定后进行 Gmsh 适配 spike，不直接把第三方 API 散布到模型/GUI。

推荐命令：

```powershell
git status --short
git submodule status
cmake --build build\vs2026 --config Release --target tst_element_volume tst_structural_product_profile tst_solver_stress --parallel 8
ctest --test-dir build\vs2026 -C Release -R '^(tst_element_volume|tst_structural_product_profile|tst_solver_stress)$' --output-on-failure -j 3 --timeout 120
ctest --test-dir build\vs2026 -C Release --output-on-failure -j 8 --timeout 120
```
