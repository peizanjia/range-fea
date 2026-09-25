# 续作交接

## 2026-09-25 第三增量结项与停止（最新，优先于历史授权）

- 用户已要求“完成当前阶段并退出”。本轮完成正在进行的 Stage 7 固定电压一维压电静力 FE 增量、验证、自动提交与交接后停止；不要自动继续后续增量或 Stage 8。Stage 7 完整规划尚未验收。
- 模型子模块 `872c060`：1.3.14 端口配置及右电极电荷变量；旧 1.3.13 默认关闭。新块严格 ASCII/binary 解析，拒绝截断、非法开关、非有限数字及不保真的 ID。网格绑定仍为既有五位有效数字坐标指纹，不是精确几何哈希。
- 求解器子模块 `d743997`：共用静态 TRUSS1 层、按元素 ID 映射拉索配置、固定电压小应变杆的稀疏机械求解及线性/物理残差认证；输出 `qR=-Qflux`（C）。仅全局静态位移/点力、零重力、293.15 K；每根 TRUSS1 必须有端口。拒绝 cable+piezo 同启、动力学、模态、控制、重启、MPC 和热场。
- 最终全目标 Release 构建成功；`OMP_NUM_THREADS=4`、CTest `-j 2 --timeout 180` **61/61 通过，0 失败，14.92 秒**。QTest 压电设置 11、压电 FE 10、拉索 FE 16、CLI 14 passed，均 0 failed/0 skipped。CLI 覆盖 ASCII/binary `u=-0.002 m,qR=1.001 C` 真实进程往返，既有 Python 跨程序续算继续通过。
- 证据：`build/audit-20260925/piezo-final-build.log`、`piezo-final-ctest.log`、`piezo-*-qtest-verified.txt`。首轮完整重建 439.00 秒；首轮 59/61 的两个测试问题已修复，原始诊断保留。详见 `docs/RELIABILITY_AUDIT.md`。没有重做干净机器安装或 GUI 操作终验。
- 根提交含 CLI、子模块指针及文档；具体提交号见 `git log -1`。既有未跟踪 `.agents/`、`.codex/`、`AGENTS.md` 保留，未混入功能提交；没有 push。
- 下次仅在用户重新授权后继续。优先入口是 `TECHNICAL_ROUTE_STAGE7.md`：工作点平衡之后的拉索非线性动力事务，或明确 V/A/W/J 契约的电压控制/观测/重启。当前没有压电梁/片、开路电荷约束或内部电势自由度；CG 残差合格不证明唯一性/稳定性。Stage 6 力命令单位 N，不得直接复用为 V。

## 2026-09-25 Stage 7 第二增量（历史快照）

- 当时用户持续推进授权有效；现已被顶部停止要求覆盖。模型为 `2883889`（含 `a7c6040`），求解器为 `a95e6f3`；根提交以 `git log -1` 为准。Stage 7 尚未整体完成。
- 模型 1.3.13 保存逐元素 L0/N0、迭代配置与有限精度网格绑定；旧 1.3.12 默认关闭。新增二进制块拒绝截断/非法编码，绑定拒绝非法端点。条件读取现按完整快照替换，已删除的重力和旧 BC/IC 不会被错误保留。
- 拉索静力采用一致切线、稀疏 Newton/受约束 Armijo 和新鲜力平衡认证，支持显式 TRUSS1、全局位移、点组恒定力；必须零重力、293.15 K。拒绝动力学、模态、控制、重启、MPC、局部坐标和热场。松弛平衡不保证稳定/唯一性，完全松弛受载初态可能因零切线失败；没有非线性动力步事务或 GUI 配置入口。
- 全目标 Release 构建成功（479.78 秒）；`OMP_NUM_THREADS=4`、CTest `-j 2 --timeout 180` **59/59 通过、0 失败、15.28 秒**。QTest 平衡 10、FE 14、CLI 12 passed，均 0 failed/0 skipped；CLI 包含两种格式拉索往返及真实 Python 续算。证据 `build/audit-20260925/cable-full-build.log`、`cable-full-ctest.log`、`cable-*-qtest-verified.txt`。
- 过程问题均已修正并保留日志：环境条件过度拒绝；高预张力下能量下降低于浮点分辨率导致 Armijo 耗尽。后者采用受物理残差下降约束的尺度相关舍入策略，外层容差不变。详见审计和 Stage 7 技术路线。
- 下一入口：先抽取共用静力 TRUSS1 模型/约束/载荷/结果 helper，再实现固定电压的一维压电 FE；独立输出右电极自由电荷 `qR=-Qflux`，单位 C。模型契约拟升至 1.3.14；暂不把电压复用为牛顿控制命令，不宣称压电梁/片验收。此时仅完成只读设计，尚未开始该增量源码变更。
- 构建与测试串行，避免 Qt DLL 部署冲突；ABI 变化后完整重建。已验证本机临时 MSBuild 参数 `/p:UseMultiToolTask=true /p:EnforceProcessCountAcrossBuilds=true /p:CL_MPCount=4`，不改持久环境。既有未跟踪用户配置继续保留。

## 2026-09-25 当前授权与首轮审计快照（历史验证状态）

- 用户最新指令覆盖下方历史停止要求：审查交付文件、开发进度、各阶段计算结果与算法可靠性，持续推进后续 stage，直到收到停止 follow-up。
- 稳定增量自动提交；子模块先提交，根仓库再提交指针。交互用正体中文，技术文档沿用简体中文。构建与原始审计日志放在已忽略的 `build/`，可维护的审计结论放在 `docs/`。
- 本轮起点：根 `74d60dd`、模型 `63a92b1`、求解器 `e8141d1`、基础库 `dca3785`。启动时子模块干净；根目录已有未跟踪的 `.agents/`、`.codex/`、`AGENTS.md`，属于既有用户配置，保留且不混入功能提交。
- 原始基线已经重新全量建置、53/53 CTest 通过；修补后全目标 Release 建置成功，55/55 CTest 通过（`OMP_NUM_THREADS=4`、`-j 2 --timeout 180`、14.26 秒）。求解器 `5fbda37` 拒绝未收敛模态与静力结果并补广义 alpha 收敛/双频验证；`67c400c` 加入已验证拉索/压电数学核。模型版本仍为 1.3.12。
- 根新增 Python 20 步对照 8+12 步跨程序续算测试，含 PID 积分、延迟、保持与固定噪声；CLI QTest 10 passed、0 failed、0 skipped。MATLAB 长驻适配器、干净机器安装和 GUI/Agent 操作接线仍未验证。
- Stage 7 仅数学核与主工程测试注册完成；尚未实现模型持久化、拉索全局平衡/非线性 FE、压电电势约束或 Stage 6 执行器接线，不可把内核测试当作阶段验收。下一步围绕显式模型契约、平衡工作点门控和 FE 接线推进。参考 `TECHNICAL_ROUTE_STAGE7.md`、`docs/RELIABILITY_AUDIT.md`。
- 本轮日志在 `build/audit-20260925/`。一次建置部署与 CLI 并发造成 Qt DLL 复制存取失败，串行重试成功；首轮新广义 alpha 测试将 dt=0.02 错配较小步长误差门槛，改为 dt=0.02/0.01 收敛对照后 55/55。不能把过程失败写成当前求解器缺陷，也不能省略最终验证。

下方为历史阶段快照，其中的“退出”“不要自动进入 Stage 7”等停止要求已被本节最新授权取代。

最后更新：2026-09-24（Asia/Singapore；Stage 6 C++/Python 控制核心结项后退出）

## Stage 6 结项与下一轮接续入口（先读；下方 Stage 5 为历史快照）

- 用户本轮要求完成 Stage 6 后退出，注意高效可并行算法、积分法选择、外部 API/Agent 契约、模块 playbook、自动提交、仓库整洁及交接；最新明确要求当前任务结束后退出。**不要自动进入 Stage 7**。本轮起点为根 `a40eb52`、模型 `1174547`、求解器 `761de0a`，Stage 5 基线 49/49。
- 已完成并验证的 Stage 6 C++/Python 核心：模型子模块 `63a92b1`（1.3.12 控制配置、每通道调理策略与 checkpoint）；求解器子模块 `e8141d1`（线程隔离 CSR、接受态一次控制、稀疏节点力映射、N/W/J 专属功账、RCCE 状态、延迟/保持/固定种子噪声、双通道及重启）；基础库 `dca3785`（严格 InteractionRecord v1 **数据契约**）。根仓库的 NDJSON 传输、`SolverTask` 实际接线、Python PID 和 CLI 正负用例已通过主 Release 回归；最终根提交号以 `git log -1` 为准。
- 已确认设计边界：现有 Newmark、广义 alpha、中心差分三法，先不盲增算法；控制器只在接受态步边界计算一次，命令在本步零阶保持，不进入单元装配或 CG/GMRES 内迭代。固定算子有效矩阵可按 dt/方法缓存，其他性能问题先分段计时和基准验证；现有每步重装配及模型深复制仍可能主导耗时。
- checkpoint 已保存控制指纹、顺序通道 ID、最近接受命令、各通道端点功率/累计功、至多 1 MiB 的复合控制器/调理状态和确定性种子；旧无控制模型照旧读取。1.3.11 的旧 **受控** raw-state checkpoint 缺调理历史，1.3.12 明确拒绝其受控续算，不会假装重播一致。外部 v1 协议有身份摘要、请求 ID、目录白名单、超时和失败关闭；不保证硬实时或认证控制程序。GUI/Agent 操作记录需待 Stage 10–11 共用命令服务接线，Stage 6 只做控制步事件及 InteractionRecord 数据契约。
- 验证：`cmake --build build/vs2026 --config Release --parallel 4` 全目标成功；`OMP_NUM_THREADS=4`、CTest Release `-j 2 --timeout 180` **53/53 通过、0 失败**。`tst_solver_audit_cli` 覆盖外部 fixture、Python PID、失配与超时；输出频率跳步时审计不虚构持久工件。`Release/Qt6Core.dll` 和 `Release/qtlib/platforms/qwindows.dll` 存在，先前报错的 `tst_structural_product_profile.exe -silent` 直接运行退出码 0。仍未在干净 Windows 主机做安装终验。
- 性能稽核见 `docs/STAGE6_PERFORMANCE_AUDIT.md`：32 杆串行/2/4 线程 24.726/68.365/83.695 ms，384 杆 175.868/158.218/144.151 ms，均为本机每组三次中位；小算例并行变慢，384 杆 4 线程仅本机 1.220×。装配和 CG/GMRES 局部已用 OpenMP、固定 `dt` 有效算子缓存、CSR 去全局锁；每步仍重装配/比对 M/K、重建预条件器并深拷贝模型，不宣称整体最优或大模型加速。
- 保留问题：MATLAB R2022a `-batch` 基本启动通过，但用标准输入做长驻 NDJSON 握手未验证，**没有可声明可用的 MATLAB 适配器**；外部 Python 跨进程中断/恢复仍缺独立 CLI 对照（内核重启已验证）。1.3.11 旧受控 raw checkpoint 缺调理历史，1.3.12 明确 fail-closed；旧被动文件继续读。压电/拉索和全状态热耦合属 Stage 7/8；GUI/Agent 操作记录及命令服务属 Stage 10/11。
- 下一轮若继续 Stage 6 缺口，先验证 MATLAB 可靠长驻 IPC 或采用命名管道/TCP 适配器，再做外部进程跨进程续算对照；若接受 C++/Python 已验收范围进入 Stage 7，先围绕压电/拉索建立基准，不要跳过上述限定。每步重装配/深复制优化需先加分段耗时、内存和大模型 A/B，不可直接删除事务校验。


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
