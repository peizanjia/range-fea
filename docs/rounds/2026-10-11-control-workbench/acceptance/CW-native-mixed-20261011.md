# 原生混合杆索保持输入增量：验证与边界

2026-10-11。最新源 `7586e45b` 的单次完整 Release 回归 **186/186 通过，0 fail，0 disabled，0 skipped，1640.79 s，exit 0**。在规定保持 L0 与源冻结 C2 支座运动范围内，CW-07 三项验收通过、任务 done；CW-03 保持 PENDING，`productComplete=false`。原 14 需求/16 任务/48 验收不变。

## 可复查来源

- 原生生产源冻结：`92251e9aca5835a869f18ebd5725a19195eca46a`；Qt CLI 断言日志配置：`29f01f4a22ea5691c52aa6c435bdb119ef25e753`；workflow 运行时源：`f80307e6b0b9b5055a305b9b2d96f7fba5a8340d`（单次调用内 verified digest 复用）；最新资格验证源：`7586e45b39afaad158f378229c352c3f812a219e`（仅补充原生测试、Qt 日志配置及 project session 语义断言，原生生产 C++ 未变）。
- model：`6a038b7d65bfbf3d22a60c920e6b2d0b0d4ae44e`，格式 1.3.20，原生分类/命令表/完整 restart 记录的 ASCII 与 binary 持久化及兼容拒绝。
- solver：`a803738142071ae629772a14af39a4671b126edd`，原生适配/接受候选/RSolver 提交前发布与重试，以及微小伸长和 Newmark 数值修正。
- 审计目录：`build/audit-20261011/native-mixed/`。最新 `source-freeze-anchor.json` 的 55 个实际字节 SHA256 已逐个校验一致。此前冻结校验记录保留：最终 54/54、诊断 52/52、原始 51/52 一致；补充测试后对旧清单再次校验，分别为 52/54、50/52、50/52，差异均局限于 `src/fea-solver/tests/CMakeLists.txt` 和 `src/fea-solver/tests/tst_native_mixed_run.cpp`。原始清单不被覆盖。`inventory-186.json` 与 `inventory-anchor-186.json` 保留各冻结的 186 项注册清单。同名 JSON 收录完整最新源指纹及稳定原始证据文件路径、长度和 SHA256。
- 同根模型/solver 独立审查复核了实际 diff 与 `integrated-final-gates.xml`，本增量没有剩余 P1/P2 正确性问题。性能缺口另列；审查本身没有运行测试。

## 已执行的聚焦资格验证

原生七目标串行 CTest：**7/7 通过，0 fail，40.13 s**，原始 `integrated-final-gates.xml` / `integrated-test-final-gates.log`。

| 目标 | 实际验证 |
| --- | --- |
| `tst_element` | TRUSS1 端点、内部点、端点反向与预计算距离的线性插值 |
| `tst_structural_truss_dynamics_setup` | 完整分类/质量来源/物理身份，源指纹，精确 RAM/RBM，隐藏字段降版拒绝及畸形输入事务读取 |
| `tst_structural_truss_assembly` | 独立势能梯度/切线、混合装配、微小伸长不随原点变化、subnormal 弦长和近坍缩压缩的符号 |
| `tst_structural_truss_dynamics` | 独立振子/RK4、命令功与解析支座、松弛/张紧、失败接受态不变；有限初位移与小步长的严格力残差 |
| `tst_structural_truss_dynamics_fe` | 真正 RModel 的 V/Q/passive、原始弦长密度质量/器件固定质量、源 IC、命令保持/端口独立、非共线瞬态、实际磁盘 split、decimal 时钟与精确支座恢复、纳米级压电二阶细化 |
| `tst_native_accepted_checkpoint` | 模型/封套/指针/marker 发布回滚、完整读回、精确 dt、调用边界强制 checkpoint、不可恢复回滚的持久拒绝 |
| `tst_native_mixed_run` | 实际 RSolver 同实例停止/续跑、独立标量 Newmark 参考、九类 checkpoint 故障与结果 record/link 碰撞、observer 提交后语义、回滚毒化、监测及真实 CLI |

真实 CLI 使用现有 `fea-solver`，不是 mock 或另一个演示求解器。六个 continuous/split 行覆盖 RAM/RBM、长小数 dt、极小 dt 静止态和非零初始时间；本次中止在第 4 步，恢复仍以原始 12 步为目标。逐比特比较完整 q/v/a、反力、实际 F/V/Q/L0、端口观测、分项累计功、源/系统身份、cursor 与目标；不可变恢复 MODEL 哈希保留。额外负例覆盖源几何/恢复量篡改与保留输出名称碰撞。上述六个真实 CLI 行使用固定支座；新增移动支座证据见下段。

新增四个 Qt 测试行补充移动 C2 支座覆盖：独立物理参考、发布回滚及真实 CLI RAM/RBM 两行。支座采用二次位移、解析速度与非零加速度；索 609 经张紧、松弛、再张紧，索 811 保持松弛。独立标量参考不调用生产装配、运动求值器、动力学驱动或 restart builder，逐接受步比较 q/v/a、反力、存储能与动能，以及外力、非零支座、电学、参考长度分项累计功。两个真实 CLI 行在第 4 步磁盘中止，恢复到原始 12 步；完整 accepted record 与连续参考逐比特一致，并保留源模型与不可变恢复 MODEL 哈希。另一个测试在第 2 步 `AfterLatest` 注入发布故障，严格检查 accepted state、latest/lastvalid 的原存在性及字节、结果、时钟、源身份与恢复输入哈希不变，修复后同 solver 重试至连续参考。该证据与同源完整回归、独立范围审查共同支持 CW-07 的三项验收通过。

补充资格验证：**2/2 通过，0 fail，46.00 s**（`anchor-qualification-final.xml` / `.log`）；原生目标 37 个 Qt case 通过、0 fail、31.819 s（`native-anchor-qualified-detail.txt`），project session 30 个 Qt case 通过。完整 Release 构建会话 77518 exit 0，`anchor-qualification-release-build.log`。独立 CW-07 范围审查没有剩余实质问题；最新完整回归已通过。

Newton 故障测试使用斜杆与预先冻结的一次迭代上限，重复失败后初始状态、时钟、源身份和恢复权威保持不变。它没有通过修改物理参数或放宽容差制造成功重试。存储故障则在修复后用同一 solver 重试至连续参考。

## 完整构建、回归与 workflow 边界

完整 Release 构建由主代理确认 exit 0（会话 11467），原始 `full-build.log` 保留。以下每行分别对应一次执行，不将不同运行的通过项拼接为单次全量通过。

| 执行 | 结果 | 原始证据 |
| --- | --- | --- |
| 首次全量 | 185/186，1757.30 s；`tst_solver_audit_cli` 失败 | `full-186.xml` / `full-186.log` |
| 日志诊断全量 | 185/186，1629.55 s；`tst_workflow_runtime` 在 180.02 s 被 TIMEOUT | `full-186-diagnostics.xml` / `full-186-diagnostics.log` |
| binding 优化后第三次全量 | 185/186，1639.54 s，exit 8；`tst_project_session` 失败 | `full-186-final.xml` / `full-186-final.log` |
| 补充支座证据后完整回归 | **186/186，0 fail/disabled/skipped，1640.79 s，exit 0** | `full-186-anchor.xml` / `full-186-anchor.log` |

第三次全量中的 `tst_project_session` 共 30 个 Qt case，29 通过、1 失败；失败为对整个 pending JSON 的 `contains("1e-")` 断言。独立只读源审查确认该断言语义不可靠，但失败字符串具体命中位置尚未确认。该次 `tst_workflow_runtime` 176.80 s 通过，audit CLI 也通过；整次仍为失败，不与前两次拼接。后续语义断言及移动支座补充已在源 `7586e45b` 聚焦验证通过；新一轮单次完整回归已通过，前三次失败结果原样保留。

最新完整回归会话 35112 已终止，XML 含全部 186 个目标且均通过；`tst_workflow_runtime` 175.37 s 通过，原 180 s 目标 timeout 不变。该次原生目标 Qt 37 case、0 fail、32.626 s（`native-anchor-full-detail.txt`）；audit CLI Qt 38 case、0 fail、24.143 s（`legacy-audit-anchor-full-detail.txt`）。最终 XML、原始日志、Qt 文件日志及全部源指纹均有独立 SHA256；本次 186/186 是同一轮结果。

首次 audit 失败没有给出断言失败文本；同一二进制隔离执行 38 个 case 全通过（24.7 s），诊断全量中的同目标再次 38 个 case 全通过（Qt 23.934 s、CTest 24.10 s），详见 `legacy-audit-first-detail.txt`、`legacy-audit-full-repeat-detail.txt` 和 `regression-binaries.json`。原因未确认，不能声称已定位修复。

诊断全量的 workflow runtime 12 个 case 均显示 OK，含 cleanup 的 Python 总时间 179.693 s，但整个 CTest 进程超时，因此该次仍是失败。后续 `f80307e6` 仅在一次调用内部复用已核验的文件摘要；所有路径、symlink、预期哈希检查仍保留，每个授权边界重新读取。它不是跨调用缓存。12 个 binding contract case 0.915 s 通过；原 180 s timeout 不变，runtime 独立资格验证 1/1 通过（目标 175.25 s、CTest 总计 175.28 s），binding 相关 7/7 通过（106.90 s）。原始 `runtime-binding-contract.log`、`runtime-binding-qualification.xml` / `.log`、`binding-related.xml` / `.log` 支持这些范围。独立 binding 审查没有实质问题；此处不声称至少 10 s 加速，runtime 的时间裕量仍小。

## 本轮修复与反例

- 原点量化：使用参考弦与位移差；小伸长采用有理化长度差，大变形/近坍缩采用当前弦长差。缩放平均长度防止半个 subnormal 弦长下溢。
- Newmark 修正：以加速度为修正变量，前向恢复 `q=qpred+βdt²a`、`v=vpred+γdt a`；有效切线解仍等价于平均加速度法，保留物理残差检查。没有放宽容差。
- 用户选项：明确拒绝 Consistent 质量及不支持的旧控制/热/MPC/材料温度字段；默认重力的解析测试夹具明确设零，未在求解器中忽略重力。
- 源/结果：新运行使用源 IC，不复用旧显示位移；候选只发布新字段；prepared 结果与显示元数据独立封存；恢复精确检查规定 q/v/a，不能让微小支座偏移借松弛零力逃过验证。
- 发布：结果与监测可回滚，checkpoint 是最后发布；失败后同 solver 的 FE/cursor 不提前走动。不可变恢复输入使用独立工作输出。

补充测试的初次构建 18843 因反力字段访问器类型错误失败，修正构建 73639 后通过。初次聚焦 30244 为 1/2 通过、46.22 s；`anchor-first-detail.txt` 明确记录 5 pass、1 fail、4.441 s，独立物理参考及两个真实 CLI 行已通过，唯一失败是回滚测试夹具误读不存在的 baseline lastvalid。随后改为严格核对原存在性及字节；没有生产改动或容差放宽。对应首次/修正构建及 CTest、file diagnostics 原样保留。

反例与初次失败日志原样保留：`fe-diagnostics-before-fix.txt`、`fe-integrated-first-detail.txt`、`host-integrated-first-detail.txt`、`host-integrated-fixed-detail.txt`、相应构建及 CTest 日志。测试夹具/误用 disabled validator 的问题与真实生产缺陷区分，初次失败不改写成成功。

## CW-07 验收结论

任务状态 done，三项 status=pass。独立范围审查与最终同源完整回归共同支持以下结论。

| 验收 | 通过依据 | 范围 |
| --- | --- | --- |
| CW-07-A01 | 原生 held L0 命令、解析 C2 支座运动及独立标量张力参考；RAM/RBM 实际 CLI 记录 | 规定有效参考长度与源冻结平滑支座，张力由力学求解产生 |
| CW-07-A02 | 正长度拒绝、只受拉索的张紧/松弛/再张紧、支座 q/v/a、反力及独立分项功账 | 609 索切换与 811 索持续松弛；非零支座功、电学和 L0 命令功 |
| CW-07-A03 | 候选接受、九类发布故障、移动支座 AfterLatest 回滚、同 solver 重试，以及两格式真实磁盘 split/resume | accepted state/权威指针/结果/源身份保持；恢复与连续记录逐比特一致 |

这是固定集中质量、保持输入 L0 和源冻结 C2 支座的有限资格范围，不是连续长度 provider、完整卷筒硬件、v3 外部控制器或 GUI 产品资格。CW-03 及其他未满足任务不随 CW-07 自动关闭。
## 保留的硬门槛

固定集中质量、保持 V/Q/L0/外力；源中固定解析支座曲线。没有连续电学/L0 provider、外部 v3 控制、任意多速率/controller 恢复、共电极网络、标准 member/port 传感绑定或材料/器件自动编译与 GUI。

密集命令日程仍有历史重放、完整源哈希与完整模型复制的二次增长；索引化/不可变快照及规模测量属于待关闭的性能问题。纯适配器 10k 步证据不表示完整 RSolver/CLI 大模型性能已通过。CW-13 的首图/CSV/内存门槛保持。

预应力环形天线平衡/模态/瞬态闭环、动态梁板贴片、非匹配面、FE/ROM、四卡少操作流程与干净安装，仍按原 14 需求/16 任务/48 验收推进。首要产品目标继续是简化操作与减少点击。`productComplete=false`。

MATLAB 与 Simulink LiveLink 两条对照均未测量。用户没有既有示例，已允许研究或暂不对比；当前按暂缓对比记录，不再请求旧示例路径，也不因此暂停功能开发。`livelink-registry-observation.json` 只记录 Windows 注册表中的 COMSOL/MATLAB 安装条目，未验证实际二进制版本或模块许可，不能作为 LiveLink 性能或交付证据。
