# 续作交接

最后更新：2026-09-20 16:15（Asia/Singapore）

## 本次目标

1. 执行、纠错并完成 `PROJECT_ROADMAP.md` 的 Stage 0、Stage 1、Stage 2。
2. Stage 3 网格能力优先集成成熟工具，不重复实现已有可靠算法。
3. 保留仓库原有非结构功能；它们不是本轮开发任务，不应被全局硬门禁或删除。
4. 在有独立价值且验证通过时自动提交，保持仓库整洁，并维护交互/开发记录。

## 当前进度

### Stage 0

已完成代码与验证，尚待提交：

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

尚未开始本轮纠正。工作树中已有未提交的 structural profile/capability 实现，但其行为过度：GUI 会过滤或拒绝非结构功能，CLI 默认使用 `StructuralProduct`。应改为：

- 默认 `AllRegisteredSolvers`，保留旧功能；
- structural workspace/profile 为显式可选模式；
- 只有选择结构产品模式时才执行结构能力验证；
- capability registry 可供 GUI、求解器和后续 Agent 查询；
- 更新测试，确保默认模式允许声学/热等已注册功能，显式结构模式仍拒绝不支持配置。

涉及未提交改动：根仓库 `src/fea*` 多个文件，以及子模块 `src/range-model-lib`、`src/range-solver-lib`。不得粗暴还原；应逐项重构。

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

先精确暂存并提交 Stage 0 文件，然后进入 Stage 1；不要把已有 Stage 1 草案混入 Stage 0 提交。

推荐回归命令：

```powershell
cmake -S src -B build\vs2026
cmake --build build\vs2026 --config Release --target ALL_BUILD --parallel 8
ctest --test-dir build\vs2026 -C Release -R '^tst_structural_product_profile$' --output-on-failure --timeout 30
ctest --test-dir build\vs2026 -C Release --output-on-failure -j 8 --timeout 120
```

## Git 状态约束

- 当前分支：`feature/structural-profile`。
- 最近提交：`485fc54 docs: plan dynamics control and agent roadmap`。
- Stage 0 的 CMake 与文档修改尚未提交。
- 两个子模块及 GUI/CLI 中存在先前未提交的 Stage 1 草案，提交 Stage 0 时只精确暂存 Stage 0 文件，避免混入 Stage 1。
