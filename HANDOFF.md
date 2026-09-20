# 续作交接

最后更新：2026-09-20 16:45（Asia/Singapore）

## 本次目标

1. 执行、纠错并完成 `PROJECT_ROADMAP.md` 的 Stage 0、Stage 1、Stage 2。
2. Stage 3 网格能力优先集成成熟工具，不重复实现已有可靠算法。
3. 保留仓库原有非结构功能；它们不是本轮开发任务，不应被全局硬门禁或删除。
4. 在有独立价值且验证通过时自动提交，保持仓库整洁，并维护交互/开发记录。

## 当前进度

### Stage 0

已完成并提交（`f39992c build: complete Windows runtime packaging baseline`）：

- CMake 将 `libomp140.x86_64.dll` 纳入 Windows 安装树，缺失时配置失败。
- 测试构建自动把 Qt runtime 与 `Qt6Test.dll` 部署到测试执行文件旁。
- Inno Setup 探测支持当前用户安装目录 `%LOCALAPPDATA%/Programs/Inno Setup 6`。
- Release 全量构建通过，CTest 28/28 通过。
- 独立 install-tree 中 `fea-solver --version` 成功。
- 已生成：
  - `build/packages/range-fea-1.3.0-winnt-amd64.zip`
  - `build/vs2026/range-fea-1.3.0-winnt-amd64.exe`
- 正式安装器静默安装、CLI 启动和静默卸载均成功。
- GUI 首次安装后启动时出现 `Crash report - Range FEA`，已确认是此前强制终止进程留下的过期 `range-fea.lck`。移走失效锁后，从独立 install-tree 启动得到正常主窗口（标题含 `[Beam]`、`Responding=True`），通过窗口关闭消息正常退出，退出码 0，锁文件正常清除。
- 干净 Windows 虚拟机终验尚未执行；当前系统未发现可直接调用的 Windows Sandbox，查询可选功能还需要管理员权限。

关联文档：`DEVELOPMENT_LOG.md`、`STRUCTURAL_BASELINE.md`、`PROJECT_ROADMAP.md`。

### Stage 1

实现与验证完成，子模块已提交，根仓库指针待提交：

- 已移除 GUI、报告、材料、结果选择器和模型读取路径中的全局 structural-only 过滤/拒绝；非结构模型不会因打开文件被拒绝。
- `RSolver` 默认策略已改为 `AllRegisteredSolvers`；`StructuralProduct` 仅在显式传入时验证结构能力。
- 声学测试已改回默认构造，用于证明默认策略保留非结构求解。
- 新增统一 `RCapabilityRegistry`，覆盖 analysis、element、material、boundary condition、initial condition、environment condition、mesher、solver 八个域；记录 registered、implemented、tested、validated 和 profile exposure，并支持版本化 JSON 序列化。
- `RStructuralProductProfile` 已改为查询中央 registry，而不是重复维护分析/单元/材料/约束清单。
- 新增 registry 域覆盖、full/structural exposure、JSON 往返和非法 JSON 测试；新增默认策略允许热求解器构造的测试。
- `tst_structural_product_profile`、`tst_structural_solver_gate`、`tst_solver_acoustic` 聚焦测试 3/3 通过；Release 全量构建和 CTest 28/28 通过（约 9.06 秒）。
- `range-model-lib` 已提交 `bf4a825 feat: add queryable product capability registry`。
- `range-solver-lib` 已提交 `6722d36 feat: make structural solver policy opt-in`。

未提交位置：根仓库子模块指针、`src/fea/src/solver_setup_checker.cpp`、阶段文档与本交接文件。应先完成根提交，再进入 Stage 2。

### Stage 2

尚未执行。必须先盘点现有 `RElement` 与 `RSolverStress` 的真实实现，再建立通用 DOF 注册和 `ElementDescriptor`。现状已知：TRUSS1、TRI1、QUAD1、TETRA1 有实现；梁、楔形、六面体等能力尚不完整。每个对外开放单元都需要单元测试、解析基准和网格收敛证据。

### Stage 3

尚未接入新工具。当前已有 TetGen 体网格。建议采用统一 `IMesher` 接口：保留 TetGen 作为四面体后端，评估 Gmsh 官方 C++ API 作为线/面/混合网格及后续 CAD 网格后端；先做许可证、二进制依赖、数据映射和命名集合保真验证，再决定集成方式。

## 已知故障与纠正

- 首次 Release CTest 因测试目录缺少 `Qt6Core.dll`，Windows 连续弹出加载错误。已终止 CTest并修复部署规则。
- 第二次单测缺少 `Qt6Test.dll` 并超时。顶层 CMake 现导入 `Qt6::Test` 并复制其 runtime；单测和 28 项全测随后通过。
- 不要再次在 runtime 未验证前并行启动全部 Windows 测试；先运行 `tst_structural_product_profile`，再全量 CTest，并保留 `--timeout`。

## 下一步命令

```powershell
git status --short
Get-Process | Where-Object { $_.ProcessName -match 'fea|ctest|tst_' }
```

提交 Stage 1 根仓库指针与记录，然后审计 Stage 2 的现有自由度、单元公式和测试覆盖。

推荐回归命令：

```powershell
cmake -S src -B build\vs2026
cmake --build build\vs2026 --config Release --target ALL_BUILD --parallel 8
ctest --test-dir build\vs2026 -C Release -R '^tst_structural_product_profile$' --output-on-failure --timeout 30
ctest --test-dir build\vs2026 -C Release --output-on-failure -j 8 --timeout 120
```

## Git 状态约束

- 当前分支：`feature/structural-profile`。
- 最近提交：`f39992c build: complete Windows runtime packaging baseline`。
- Stage 0 已提交。
- 两个子模块和根仓库中存在尚未提交的 Stage 1 实现；提交顺序必须是 model 子模块、solver 子模块、根仓库指针与记录。
