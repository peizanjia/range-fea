# 续作交接

## 2026-10-01 计划修订与持续开发（当前）

- 用户已授权按修订计划开发、审查、验收并自动 commit；覆盖下方历史停止要求。执行清单为 `docs/DEVELOPMENT_PLAN.md`：P0 状态/CI → Stage 10A 共用服务 → 10B/10C 工作流/GUI → Stage 11/12；质量线同步推进。
- 起点根 `46e7318`，模型 `baec0fe`，求解器 `535a6fb`。本日复跑现有 Release CTest 91/91、0 失败、62.52 s；未重新构建、未重跑 MATLAB 实机或干净安装。旧 124.46 s 是此前完整构建后的回归。
- 当前正在修订文档与细化 CI/服务实现；尚未把计划项记作已交付。临时证据放 `build/audit-20261001/`，既有未跟踪用户配置保持原样；不自动 push，不调整系统权限。
- 下方 2026-09-26 及更早内容全部为历史快照；其中“最新”“停止”“仍未完成”等状态仅对该快照有效，当前状态以本节与计划清单为准。

## 2026-09-26 控制器替代路径与阶段缺口修复（最新）

用户要求暂缓需要手动调整本机权限的事项，继续分别修复报错/功能缺口，并考虑 Python 学习型、C++、板端等主流控制部署路径。本轮未更改认证、系统权限或进入 Stage10–11；源代码、测试和交接自动提交，未推送。求解器子模块提交 `535a6fb`，模型仍为 `baec0fe`；根提交见 `git log -1`。

- **MATLAB 替代接口已验证**：旧 R2022a stdin 三种 API 失败记录保留；新 Java TCP 服务器 + Python NDJSON 桥接完成握手、同接受态重复计算、重连接受状态、31秒请求间空闲、实际400步 FE及正常退出。第一次真实FE握手暴露多感测器目录假设，已按ID选择速度并验证全部目录；随后发现 QProcess 强制终止桥接使 MATLAB `Connection reset`，已改为有界 stdin EOF→terminate/kill，回归证明正常清理。远端源代码+增益 SHA256 可固定到桥接参数/求解器指纹，不宣称网络认证。
- **控制器选择**：新增 `range-control-velocity` C++ 进程；PyTorch 离线训练零偏置、符号约束 `1-8-1` tanh 网络，导出 JSON，由无 PyTorch 依赖的 Python 运行时推论。训练域外观测拒绝，状态由求解器显式携带；不是通用强化学习或任意稳定性证明。TCP 桥接也可接纯计算端点，但无实物板卡、串口/CAN、固件部署或硬实时认证。
- **指纹审查修复**：`--policy=path`/`--weights=path` 等 `--key=value` 文件参数现在也纳入控制器版本摘要。实际FE续算测试对两个别名修改有效权重JSON字节，均命中特定控制指纹错误并保持输出文件不变。
- **拉索初始化**：真新起算在首个控制观测前按集中质量平衡求 a0，保留q0/v0；后续步/重启/已有checkpoint不覆盖已接受加速度。已更新独立首步载荷基准，增加非零初位移/速度/载荷、初始加速度感测及checkpoint保留校核。
- **热机几何扩展**：常系数单体 `RStructuralThermoMonolithic1DFEAdapter` 支持任意3D方向共线直杆；标量条件沿确定性正向轴，位移回到全局，应力保持线单元局部轴向xx。旋转/反向连接两步推进的温度、位移、应力与能量等价；弯折网络及横向接受位移拒绝且不发布。其他分区适配器仍保留各自限制。
- **Stage9 精度门槛**：按指定频点逐阶选择完整模态簇，未达到请求误差则失败并保留候选证据。四杆0/5/15/30rad/s，1/2/3模态最大相对误差约140.33%/28.70%/11.06%，1%门槛选择全部4模态（误差1.08e-15）。没有保证频点之间或闭环稳定。
- **最终数值验证**：全目标 Release 构建成功；完整CTest **91/91通过、0失败、124.46s**。400步实际FE对独立NumPy Newmark：C++终态q/v最大差 **1.9764e-11m / 8.7381e-10m/s**；MATLAB TCP **1.9764e-11m / 8.7381e-10m/s**；学习型对教师基准 **4.5855e-7m / 1.90575e-6m/s**。终态能量分别约 **7.22431e-6J / 7.22431e-6J / 7.15962e-6J**。这些只证明该四杆、dt=.005s、400步、指定域的运行。
- **训练验证**：PyTorch `2.9.0+cu126`，显式CPU float64，seed17/600epochs；留出1025点最大/RMS教师力误差 **0.002834872/0.001090645N**，导出推论相对PyTorch最大差 **6.66e-16N**。权重示例与依赖说明随包安装，推论无需训练框架。
- **安装/打包**：`build/controller-adapters-install`在仅安装bin及Windows系统目录的PATH下，C++/学习型各400步、GUI `--help`和自动模态分析均退出0；外部Python以绝对路径调用。ZIP **91,973,142 bytes**，SHA256 `274975a3e102571492e8d3948e0d2f5f90f67df2fec4f1cb4518186e071be6b4`；Inno EXE **62,363,999 bytes**，SHA256 `7a1ce5bb0eb41d373f346e30585e3a446bdef6198fdc5cff2eb480ede00d3048`。13个关键包内文件与安装树逐字节相同；Inno56.062s编译成功。没有做干净VM安装或完整GUI建模终验。
- **审查**：独立只读审查覆盖控制器/传输、学习型推论和训练、拉索/热机数值与自动选阶；发现的MATLAB闲置超时和组合参数指纹问题均修复并复核。无未解决的已确认重大源码发现。

复跑说明：`docs/CONTROLLER_ADAPTERS.md`。证据：`build/controller-adapters-final-build.log`、`build/controller-adapters-full-ctest.log`、`build/controller-adapters-qtest-details.log`、`build/controller-adapters-validation/`、`build/controller-adapters-package-verification.json`。完整现象/试错记录：`build/audit-20260926/controller-adapter-followup-investigation.md`；临时脚本及结果留在忽略的build目录，未混入产品源码。

仍待开发：一般多维/弯折网络/温变材料单体热机、通用单体CLI派发、跨重网格物理对应和任意任务树事务；更复杂ONNX/MCU推论后端、真实板卡时序/物理施加协议。没有实际板型信息，不能宣称板端固件已交付。按用户指示不再试探Hyper-V/Sandbox权限。旧stdin失败和最初MATLAB外层launcher100秒未就绪的精确原因仍未解决，但直接二进制TCP替代已经可用，不阻挡已验证的C++/Python/MATLAB路径。后续可从上述明确范围继续，不得把本轮进展改写为任意模型的Stage0–9全能力完成。

## 2026-09-26 Stage 9 数值交付与全阶回代（历史）

用户后续已授权：只要此前环境疑难不影响后续开发，推进至 Stage 9；旧的“不进入 Stage 9”停止条件已被该指令覆盖。本轮实现 Stage 9 路线全部八项交付，在下述受限物理范围内验证完成；完成发布核对后停止，不进入 Stage 10。

- 稳定源码：根 `af48742`，求解器 `a6292c8`；模型仍为 `baec0fe`。最后证据/交接提交见 `git log -1`。
- 原生 `RStructuralLinearModel` / `--linear-model-output` 在私有副本组装真实 FE 物理 SI M/C/K/B、q/v/a 感测行和完整工作点/DOF/源哈希。线弹性参考几何静态平衡、非线性缆索收敛后严格绷紧切线均可导出。常支承非零位移和点弯矩经真实 RHS 处理；既有位移结果与旧时间步算子不会污染矩阵。完整模型文件/目标协作锁、读取前后哈希及 QSaveFile 原子发布；失败保留已有结果。
- `stage9_analysis.py` 提供 E/A/B/C/D（含加速度直接馈通）、质量归一化模态截断、CB 静态/固定界面模态、复数 FRF、尺度化实际特征值 PBH、各通道全阶/降阶误差。模态/CB 新鲜残差和质量正交误差可核对；重复模态簇截断、奇异频点和非法矩阵显式失败。ROM 工作点残差明确继承全阶源证据，不声称投影坐标已重新平衡。绝对误差按单位统计。
- `stage9_velocity_controller.py` 离线共点速度回授设计、全阶极点核对、绑定原生源哈希的 NDJSON v1 控制、独立 NumPy 零阶保持 Newmark 回放。输出覆盖实际 ROM 时间域预测误差和投影误差、q/v/a、控制功与被动基线，不把单模态阻尼目标等同全阶精度。
- 全目标 Release 成功；完整 CTest **87/87，44.52 s**，其中 Python 分析 9 项、控制器 21 项。独立只读审查提出的旧模态组装、非零支承、弯矩、旧历史、ROM 向量/目录、PBH 近根、文件覆盖、源身份及时间域误差问题均已修正并有回归；最终源码审查无未解决的重大发现。
- 真 FE 四段杆、dt=0.005 s、400 步主动与被动算例均与独立 NumPy 回放吻合。受控终态最大位移/速度差 **1.98e−11 m / 8.74e−10 m/s**；被动终态差 **2.53e−10 m / 2.84e−8 m/s**。真实 FE 末能量主动 **7.22431e−6 J**、被动 **6.08964e−4 J**，比值 **0.0118633**。两模态/CB（一个界面+两个固定界面模态）在 30 rad/s 的最大相对 FRF 误差分别 **28.70% / 1.2020%**；单模态加速度与通道零点附近仍有较大相对误差，不能假装任意频带准确。
- 本机独立解释器 Python **3.13.5**、NumPy **2.1.3**、SciPy **1.15.3**。CMake 明确配置 `-DPython3_EXECUTABLE=D:/ProgramData/anaconda3/python.exe`（自动发现的 Python3.14 不含这些数值依赖）。最低支持版本声明统一为 Python3.10+。安装包含脚本、依赖清单、字节保真的完整原生算例和矩阵 JSON；解释器/数值库需要单独环境。
- 本机安装树 `build/stage9-install` 的原生导出、已安装 Python 分析/设计、400步 FE 控制与 GUI `--help` 均退出0，PATH 仅保留安装/bin 与 Windows 系统目录，外部 Python 使用绝对路径；这不替代干净 Windows VM 或完整 GUI 建模认证。
- 最终打包成功：`build/vs2026/range-fea-1.3.0-winnt-amd64.zip` **91,927,986 bytes**，SHA256 `0e2f9d414c27de0ffbbeff9bbc4e0a6c368488c931a7bb9f189eb79d2dbbeef8`；Inno `.exe` **62,339,015 bytes**，SHA256 `958978c693a56a8cb7568838462d7bd5348778d37ad05324148f73cb3377fc6f`。ZIP内两脚本、依赖清单、原生/矩阵算例、两份Stage9文档及CLI可执行文件均与源码/安装树逐字节一致；核对见 `build/stage9-package-verification.json`。Inno56.156s编译成功，未在干净VM执行安装。

复跑入口：[使用说明](docs/STAGE9_CONTROL_ANALYSIS.md)、[算法和边界](TECHNICAL_ROUTE_STAGE9.md)、`src/fea-solver/examples/stage9/`。首轮和最终证据分别在已忽略 `build/stage9-validation*`；完整回归 `build/stage9-full-ctest.log`，安装工作流 `build/stage9-installed-run/`，构建/安装/打包日志在 `build/stage9-*.log`。

边界：POINT/TRUSS1/BEAM1、1–256独立机械坐标、各≤64输入/输出、固定支承与常载、293.15 K、正定质量/稳定正定切线；仅全局节点力和点节点q/v/a目录。MPC/移动支承、压电电路状态、一般热工作点、缆索切换点、刚体/奇异系统和通用非线性 descriptor 不在认证范围。Stage 0 干净VM、Stage 6 MATLAB长驻适配、一般多维耦合与跨重网格等历史缺口仍独立保留；不得将 Stage 9完成改写为无保留 Stage0–9全部完成。此前401是Codex认证中断，本轮未修改认证配置；它没有破坏原生建置，也没有阻挡已验证C++/Python路径。

## 2026-09-26 Stage 0–8 审查与本轮交付结束（历史）

用户要求恢复401中断、继续补齐Stage 0–8，完成后退出；反复无法排除的环境问题须详细保留临时记录。当前完成本轮受限Stage 7/8增量、全量回归和发布包；不进入Stage 9。不能无保留宣称Stage 0–8全部能力完成：Stage 0干净Windows终验与Stage 6 MATLAB适配器仍未交付，适用范围之外的模型也没有验收。

- 稳定源码：根 `0b4c7b7`（typed Python/CLI）、`b575ad3`（FE driver集成），求解器 `4a59719`；早期恢复及导数/回呼防护分别见 `4e24c95`、`a63f4f4`、`9e443ae`。模型 `baec0fe`。最终文档提交见 `git log -1`。
- 验证：全目标Release构建退出0；`OMP_NUM_THREADS=4`、完整CTest `-j2 --timeout600` **83/83通过，0失败，38.68s**。CLI QTest **32 passed / 0 failed / 0 skipped**，真实Python子程序执行，不能将此跳过写成通过。型别会话/实际索/梁/力控制/进程协议聚焦5/5。
- Stage 7：实际拉索Newmark NodalForce/N接受步、功账与ASCII/binary续算；L0/N0/两端锚点解析输入导数、实际准静态长度/初张力/规定锚点命令；BEAM1独立片V/C解析弯曲、互易、N/Nm/C认证与原子结果。200步速度回授索振与独立标量递推吻合，末振动能量<被动基线0.001倍，能量变化和控制功一致。
- 外部交付：typed NDJSON v2严格绑定通道物理量/单位；原生完整模型+version1 JSON初始化；.sfec嵌入模型/片/目录及接受控制、条件化与功状态，支持跨程序续算。输入/配置/目标锁与哈希、QSaveFile一次发布，失败保留旧包，日志区分prepared/committed；示例和用法见 `docs/TYPED_FE_CONTROL.md`。默认串行确定性；显式多线程审计的actualThreads是作用域入口探测，非每个kernel的实际线程数。
- 审查修补：忽略原生MPC/非应力任务、回呼重入、配置键拼错取消限幅三个发现均关闭且回归；梁温度/竞争设置/时间曲线拒绝、时钟FMA精确绑定、候选封印和非结构结果回呼防护均有测试。独立只读复查没有发现上述路径仍未关闭的重大缺陷，不等于证明任意工况正确。
- Stage 8：在既定一维/指定二维与BEAM1范围完成显式交付；热残差、Heat→Stress/节点压电、温变材料/热弯曲/热初应力模态、点加热器多速率续算、物理ID/稀疏场、参考热质量、分区/一维常系数单体和耗散回热，以及精确两叶结果事务。常系数单体是C++ direct entry；一般多维单体、跨重网格及任意任务树不在范围。
- 安装/打包：`build/audit-20260926/install-final`，缩减PATH的GUI/CLI --help均退出0；Qt/OpenMP/MSVC、typed示例及说明文件齐全。ZIP约91.88MB，Inno安装器约62.33MB成功生成，ZIP的CLI SHA与安装树一致。这是本机冒烟及包内容核对，没有运行干净VM安装器，也没有作完整GUI建模操作终验。
- 包：`build/audit-20260926/packages-final/range-fea-1.3.0-winnt-amd64.zip`、`build/vs2026/range-fea-1.3.0-winnt-amd64.exe`；SHA记录 `build/audit-20260926/final-package-sha256.json`。最终证据 `final-all-release-build.log`、`final-full-ctest.log`、`typed-cli-qtest-final.txt`、`final-install.log`、`final-package.log`。
- 外部阻碍：Hyper-V无权限、Sandbox需提权；MATLAB R2022a的fgetl(0)、Java System.in、input三种stdin读入均失败，简单batch还发生connector退出崩溃。按用户异常退出规则保留 `build/audit-20260926/external-validation-limits.md` 和 `matlab-stdin-ndjson-investigation.md`；没有可用MATLAB适配器。下次入口：有授权的干净Windows VM终验；MATLAB Engine或明确本地TCP/命名管道桥验证，然后真实FE/恢复验收。
- 拉索初态边界：新起算保留节点u/v但清除a，被动力路径a0默认0，没有任意非平衡q0的平衡加速度投影或自定义a0入口；控制力变化会刷新右极限a。当前索振基准是与a0=0相容的平衡位移初态加初速度。不应推广为任意初态支持；若扩展需明确初始化契约并重验既有首步载荷基准。
- 401：发生在Codex responses认证层，当时构建后来成功退出0。本机是ChatGPT认证，无个人API key/自定义位址。同期官方Codex内部事故与现象相符，但无法逐请求证明根因；没有改认证/计费，密钥未写入日志。详见 `resume-auth-and-regression-investigation.md`。
- 根目录既有未跟踪 `.agents/`、`.codex/`、`AGENTS.md` 保留且不混入功能提交；临时日志只在被忽略build下，没有push。下面是历史快照，旧停止/未完成条目以本节为准。

## 2026-09-26 401 中断恢复与 Stage 7 继续补齐（最新，工作仍进行中）

用户最新要求解释意外中断并继续审查/补齐 Stage 0–8，未授权把剩余开发缺口省略。中断来自 Codex responses 的 401 认证错误，非项目编译错误。本机 auth_mode=chatgpt，无个人 API key/自定义 API 位址；同期 OpenAI 官方事故页 https://status.openai.com/incidents/01M3DCNWMW57HYK8FJ5FBFPA39 记载 Codex 内部故障且已恢复。该关联不是个别请求的服务端根因证明。没有改动认证或计费。

中断前全目标 Release 建置已成功。其后 CTest 79/80 的唯一失败是旧 cable restart 测试仍要求一律拒绝控制；提供合法 controllerId 后改验没有 NodalForce actuator 目录必须拒绝，聚焦通过并自动提交。重新完整执行 **80/80 通过，62.13 s**。本机安装树 CLI/GUI `--help` 均退出0，qwindows.dll/OpenMP/MSVC DLL核对存在；不能替代干净VM实际操作终验。

已提交的新 Stage 7 交付：受限 BEAM1 梁片静态 V/C FE、独立2/4/8网格与互易测试、分别以 N/Nm/C 认证残差、私有候选seal及四输出原子发布；真实拉索 Newmark NodalForce接受步控制与功账、ASCII/binary分段控制续算、时钟/控制配置/物理目录回呼竄改拒绝；真实FE预张力几何刚度与松弛再张紧。后续解析 `B=-df_int/dinput` 支持 L0/N0/两端锚点及相应势能导数，尚只是线性化，不等于长度/张力/锚点接受步控制接线。回呼注入任意非结构结果也已拒绝、回滚并可重试。新增三个聚焦目标 **3/3通过**；最新包含这些增量的全目标回归还待最终执行，不能把80/80扩写成81/81。

正在补：有独立量纲的 Stage 6 typed-control companion及实际FE消费；V/C 不得塞进原v1牛顿命令。Stage7完整退出条件、干净VM终验与MATLAB适配仍未关闭。诊断、构建、测试日志统一在忽略的 `build/audit-20260926/`；401与回归记录为 `resume-auth-and-regression-investigation.md`。根/子仓提交以git log为准；用户未跟踪配置保留，未push。

## 2026-09-26 Stage 8 全量 Release 基线与 Stage 0–8 缺口（最新）

本轮已提交：持久物理 ID 分配/读档失败原子性和 capability registry 的独立验证标记及可选 Gmsh 可用性；高刚度/高连接度热—节点压电残差舍入证书；ASCII 结果变量 17 位精度；精确两叶 Heat→Stress 监测/结果跨进程锁；拉索 Newmark ASCII/binary 接受态续算与逐分量篡改拒绝；TRUSS2、BEAM1/2、TRI1/2、QUAD2、TETRA1、WEDGE1、HEXA1 的四网格解析模态收敛；真实 Heat→Modal 材料刚度与受限 BEAM1 Heat→Stress→Modal 热初应力几何刚度；一维常系数 TRUSS1 RModel 单体热机 FE 适配器。热—应力—节点压电联合 FE 的高刚度回归、300 支杆热力相消和失效回滚均有聚焦测试。子仓库提交及根指针以本轮 `git log`、`git submodule status` 为准。

全目标 `cmake --build build/vs2026 --config Release --parallel 2` 成功；其后完整 `ctest --test-dir build/vs2026 -C Release -j 2 --output-on-failure --timeout 600` **78/78 通过，0 失败，69.42 s**。首次全套曾因模型格式升到 1.3.17 后三个旧测试仍断言 1.3.16 而得 75/78；已仅修正四处预期并重建三个目标，最终全套重新执行通过。较早交接中的 72/72、75 项和“待重测”均为历史快照。

Stage 8 的显式交付物在受限一维/指定二维基准已有上述真 FE、能量/残差、控制采样与续算证据；单体适配器仍为 C++ direct entry、限常系数 x 向 TRUSS1，BEAM1 热预应力仅接受同次静态 Heat→Stress 的轴力。跨重网格对应、一般多维双向单体耦合和任意任务树事务不在已验证范围。Stage 7 仍缺 Stage 6 拉索力及长度/张力/锚点控制、压电梁/片与 V/C 控制；当前正在继续补齐，不得把上述 78/78 宣称为 Stage 0–8 全部完成。Stage 0 的干净 Windows VM 安装终验受本机 Hyper-V/Windows Sandbox 权限阻挡，现象见已忽略的 `build/audit-20260926/external-validation-limits.md`；Stage 6 MATLAB R2022a `-batch` 长驻 stdin 握手已用三条读取路径实测失败，不存在可交付的 MATLAB 适配器，详见已忽略的 `build/audit-20260926/matlab-stdin-ndjson-investigation.md`。完整逐项证据与限制见 `docs/STAGE0_8_AUDIT_20260926.md`。

## 2026-09-26 Stage 0–8 交付复核早期快照（以下本节为历史）

用户当前要求先完成 Stage 8 全部交付，再审查并补齐 Stage 0–8；稳定增量自动提交，完成后退出。精确两叶暂态 Heat→Stress 的文件监测、结果 latest link 和时间步记录现在按实际目标共用跨进程锁；求解器 `896e81a` 的跨角色并发聚焦测试通过。求解器 `2f36d61` 新增真实 Heat→Modal FE 顺序基准，均匀 300/400 K 求解后的频率比符合 `sqrt(0.5)`；另有 2/4/8 段空间收敛断言正在待测，不能将尚未执行的断言计作证据。模型 `cf67497` 修复物理 ID 耗尽时部分扩容及读档失败覆盖旧对象，聚焦 Release 测试通过。热—应力—节点压电联合真实 FE、热流/电荷平衡与发布失败同求解器重试在求解器 `49f3d52` 聚焦测试通过；高刚度试验的机械新鲜残差认证失败单独记录于忽略的 `build/audit-20260926/thermal-piezo-investigation.md`，不以较小材料尺度推论任意刚度可靠。

独立 Stage 0–8 审查指出：Stage 7 已有受限拉索 Newmark FE，旧文档误称未接 FE；其磁盘重启和 Stage 6 控制接线仍缺，压电梁/片及电压控制亦未验收。Stage 8 的通用双向/单体多维耦合、跨重网格物理对应和热预应力几何刚度模态频移仍没有真实 FE 验收；当前一维单体核仅为独立候选。Stage 1/2 capability registry 把元素 `validated` 直接等同 `tested`，Gmsh 未在 mesher 清单中；须修正并验证。Stage 0 无干净 Windows VM 的发布终验，Stage 6 MATLAB 长驻适配器无实测握手。不能在这些缺口关闭前把 Stage 0–8 整体标成完成。最新全目标 Release 回归尚待本轮全部增量落定后执行；此前 72/72 是历史基线，不覆盖现有 75 个 CTest 目标。准确提交与工作树以 `git log -1`、`git submodule status` 和 `git status --short` 为准。

## 2026-09-26 Stage 8 持久物理 ID 与暂态监测结果事务

模型子模块 `7c11dc5` 将节点/单元非零物理 ID 及单调分配计数器写入 1.3.17 ASCII/binary 模型；1.3.16 及更旧文件读入时按原位置确定性分配起始 ID。新增、删除、复制及文件往返聚焦测试通过；它保持已有网格实体身份，但不推断重网格前后的对应关系。求解器 `faae18e` 使全温度/机械场快照直接使用模型内 ID，无须调用方另传列表；聚焦 Release 建置和 `tst_structural_temperature_field` 1/1 通过。

求解器 `94dc3b6` 让精确两叶暂态 Heat→Stress 的文件监测点进入逐步事务：暂存写入、结果发布后原子追加，部分多文件发布失败时恢复旧监测文件与模型结果并允许同一求解器重试。已尝试写入却无新记录会失败；缺变量/网格外点仍按既有语义警告跳过。聚焦 Release 建置及 `tst_structural_heat_stress_coupling` 1/1 通过。根 `70f4783` 已更新两子模块指针。仍待全目标回归、单体可逆热弹性基准及 Stage 8 全部验收，不能凭聚焦结果结项。

## 2026-09-26 Stage 8 场快照与待整合 CLI

同网格物理 ID 耦合场快照已增加，独立审查发现并修复应力六分量映射；聚焦 Release 构建及测试通过。当前真实输出中的应力分量顺序为 `[xx,yy,zz,yz,xz,xy]`，线/面单元局部坐标，体单元全局坐标。物理 ID 由调用方提供，只能在其稳定性已经得到保证的网格上使用；暂无自动重网格映射。根提交 `ad4886b` 已加入 `fea-solver --thermal-steps=N --thermal-checkpoint=PATH`：从 1.3.16 配置模型或 `.tfec` 启动，精确 Heat→Stress 顺序下宏步全成功才原子发布，分段续算/损坏拒绝/失败保留旧快照的进程测试通过；逐步接受事件在发布后记录。未指定收敛文件不再意外生成 `.-mxs-CG`，旧测试残留已清理。Stage 8 仍未验收；其后还须核对 Stage 0–8 已记录的真实能力缺口，特别是 Stage 7 控制接口和 Stage 0/6 外部环境验证。

求解器 `840fd7c` 新增独立的一维 RModel 双向热机 FE 适配器，真实杆件黏性耗散回热及候选原子发布通过解析/失败测试；分区迭代与同一受限方程的独立同时标量根一致。它要求调用方显式输入每个当前单元索引的固体黏性系数，拒绝非受限一维网格/边界与启用的模型时间调度器；无模型配置持久化、通用单体 FE、可逆热弹性热项或结构惯性。Release 聚焦 CTest 2/2 通过，完整集成回归尚待进行。

根 `c15d08f` 完成热 CLI 独立审查修复：同目标跨进程锁、发版前存在性/SHA-256 核对、每命令最多 1000 宏步、发版后审计异常明确标记 checkpoint 可能已提交。审查中 CLI 聚焦 CTest 一度退出 1 且摘要被截断；逐项运行定位到失败路径残留审计 `.lock`，修正退出前明确释放日志及目标锁后，完整 CLI 聚焦 CTest 1/1 通过。详细试验保留在已忽略临时文件 `build/audit-20260926/cli-test-investigation.md`；该问题现已定位并解决。

求解器 `1efa357` 将精确两叶 Heat→Stress 任务扩到受限暂态逐步事务，聚焦 QtTest 16/16 通过。两子任务成功后发布；失败恢复模型/共享数据/计数、已接受时间与结果文件，同一求解器可重试。独立审查后加入输出锁、旧未来记录早期拒绝、六位以上步号识别、磁盘回滚失败时保留备份路径；带文件输出的 monitoring point 暂态模式早期拒绝。临时审计在 `build/audit-20260926/transient-heat-stress-investigation.md`。完整 Release 集成回归及二/三维热质量增量仍待完成；Stage 8 未结项。

求解器 `8200b07` 新增显式启用的面/体参考热容量。QUAD1、QUAD2、TETRA1 变形脉冲解析测试及退化参考面拒绝通过；独立只读审查未见实质计算缺陷。该开关不自动进入所有求解路径，调用方须声明参考密度语义后启用。全目标 Release/CTest 仍待最后整合复核；Stage 8 未结项。

整合复核：全目标 Release 构建成功，`ctest --test-dir build/vs2026 -C Release -j 2 --timeout 180 --output-on-failure` **72/72 通过**。此数字覆盖新增暂态两叶事务、面/体参考热质量、CLI 热控锁与两向一维 FE，但只证明列明的受限能力；Stage 8 的通用监测输出事务、跨重网格稳定身份、广义双向热回馈/单体策略等仍不能据此宣布完成。根与子模块的准确版本以 `git log -1` 为准。

## 2026-09-26 Stage 8 续作快照

独立审查纠正：求解器 `2e1a9d2` 将热控接受时间加入状态 v2，模型时钟必须与其一致；模型持久配置启用时 checkpoint 的排程器指纹必须一致；写入与读取共用 1 GiB 上限。自模型内设置直接恢复、时间/设置失配拒绝及磁盘续算聚焦测试通过。v1 状态按零起点推算时间，其他起点在模型时钟校验时拒绝。完整回归需随下一整合提交复核。

当前整合增量：模型子模块 `e47f00d`（文件 1.3.16）持久化受限热控配置，ASCII/binary 及旧 1.3.15 默认读取验证通过；求解器子模块 `340cdaa` 从模型设置建立调度器，磁盘模型往返后驱动真实 FE，并在受限 TRUSS1 热控路由中用参考长度保留热质量、当前长度装配导热。更新的变形后冷却解析测试通过。完整 Release 建置成功，`ctest -j 2 --timeout 180` **70/70 通过**。这仍不是通用 CLI 热控重启、暂态跨场任务事务或双向热力学耦合；Stage 8 未结项。独立审查正在继续。

续作增量：受限热控 FE 适配器现在可将二进制 `RModel` 与调度器状态写成一个 SHA-256 校验、`QSaveFile` 原子替换的 checkpoint；恢复先验证完整内容、配置和网格，再更改输出。磁盘恢复续算与损坏文件拒绝测试通过，求解器子模块及根提交以 `git log -1` 为准。全目标 Release 建置成功，`ctest -j 2 --timeout 180` 70/70 通过。`ctest -j 4` 中 CLI 审计偶发失败一次，单独及 `j=2` 全套重跑均通过，未定位并行条件成因。此 checkpoint 尚无模型内配置、CLI 命令或通用跨任务事务；Stage 8 仍未验收。

求解器子模块 `e464cbd` 已提交受限拉索 Newmark 动力真实 FE、Heat→节点电势压电杆真实 FE、点加热器瞬态热 FE 与宏步末准静力应力 FE。修复了旧 `POINT` 热单元因无积分点而静默遗漏点热源/点热容的问题；原有点 `HEAT` 边界条件有回归测试。热控适配器采用程序内候选模型副本，并按有序坐标/拓扑哈希拒绝明显变更的网格；这不是跨重网格物理 ID，也没有持久配置、CLI 重启或任务树暂态结果事务。全目标 Release 建置成功，CTest **70/70 通过**。Stage 8 尚未完成：双向热回馈、跨场暂态接受/失败原子性、版本化控制状态和文件往返等仍须完成，不能凭当前测试数宣称结项。用户要求完成 Stage 8 并审查后退出；若反复试错仍遇无法解决的疑难问题，先在临时文件记录现象、分析与试验，再于其余内容可交付时如实退出。既有未跟踪 `.agents/`、`.codex/`、`AGENTS.md` 保留。

## 2026-09-25 新授权：继续至 Stage 8 并审查

后续稳定增量：求解器 `e3d3108` 把精确的稳态单次 Heat→Stress 顺序任务作为结果发布事务，两子任务均收敛后才写出一次；应力失败、结果写入失败均回滚模型/共享数据/执行计数，同一求解器修复后可重试。暂态或多次根迭代明确拒绝，不借此声称一般跨场宏步原子性。求解器 `abbb3e5` 加入 QUAD2 穿厚度稳态热梯度悬臂梁 FE 基准，24×6 到 48×12 网格的曲率误差从 2.78% 降至 0.695%，端点位移误差从 2.45% 降至 0.49%；细网格端点 −5.37355 mm 对解析 −5.40000 mm。夹具初始偏差来自材料表在 293.15 K 的零值点和只在 350 K 设置常数，已修正，调查记录在已忽略的 `build/audit-20260925/thermal-bending-investigation.md`。根 `c28936b` 增加 1.3.15 节点压电 ASCII/binary 实际 CLI 往返；ASCII 结果八位有效数字按存储精度验收。新一轮全目标 Release 构建成功，CTest 70/70 通过。用户最新要求是在 Stage 8 真正完成后退出；若反复无法定位疑难问题，先把现象、分析和试错写入临时文件，在其他环节可交付时如实注明未完范围后退出。

前次验证快照：模型子模块 `29deff8` 和求解器子模块 `df3e56f` 已提交，根增量为 `69f6325`。新增节点电势/导体电荷一维 FE adapter、热场持久结果重新载入、Heat→Stress 和温度相关模态真实 FE 基准、独立一维热机与多速率调度核。全目标 Release 构建成功，CTest **69/69 通过**。`tst_structural_heat_certification` 已使用 `QTEST_APPLESS_MAIN`；故意设置不存在的 `QT_QPA_PLATFORM` 后单独运行仍退出码 0，原 Qt 平台插件弹窗属于旧 GUI 测试入口而非热求解错误。独立审查发现并已修复热残差门槛无条件放宽至 1e-8、正最小加热功率下关机态误拒绝；修复后聚焦及完整 CTest 均通过。审查同时确认跨 Heat→Stress 任务尚无统一接受步回滚，不能据独立核结果宣称 Stage 8 完成。待继续：实际热—压电 FE、拉索动力/控制 FE、热弯梁、多速率控制实际 FE 接线、跨任务故障注入/重启验收。既有未跟踪配置保留。

用户最新指令覆盖下方“完成后退出”的历史要求：继续开发，直到 Stage 8 交付物及验收条件真实完成并完成独立审查。当前基线为根 `2d68fa2`、模型 `872c060`、求解器 `d743997`，最终 Release 61/61。现正逐项核对 Stage 7 未完成的电势/电荷、拉索非线性动力与控制接口，以及 Stage 8 全状态热—结构耦合。不得用既有静力端口或一般热/应力单独求解冒充完整阶段验收。既有未跟踪配置继续保留。

首个进行中增量：求解器子模块 `a9e927f` 加入拉索非线性 Newmark 动力候选步、含节点电势与导体电荷边界的 1D 压电稀疏核、按调用方稳定物理节点 ID 对齐的精确网格热场快照，并修正热求解器未认证收敛仍可能发布结果的问题。四个新增聚焦目标通过；在模型 ABI 改动前现有构建的 CTest **65/65 通过**。这些核尚未接入完整 Stage 7/8 模型、控制、重启和 CLI 路径，不能判定阶段完成。热测试最初使用 GUI Qt 入口触发平台插件弹窗，已改为 `QTEST_APPLESS_MAIN`；同时补齐夹具的热材料属性，避免零自由温度未知数产生伪基准。模型 1.3.15 节点电边界和热—结构后续工作正在进行；需在其完成后全目标重建与独立审查。

第二个进行中增量：模型子模块 `a7adef3` 已将节点电势/导体电荷边界与精确网格绑定保存为 1.3.15，旧 1.3.14 文件按 LegacyTerminalVoltage 读取；模型库编译及相关三个聚焦目标通过。新增真实 `Heat → Stress` FE 顺序任务基准，解析的自由伸长和固定杆压应力均通过。压电 nodal FE adapter 与分区热—杆候选步仍在整合；当前测试通过不代表热—压电联合、控制/重启、截面热弯曲或 Stage 8 全体验收。

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
