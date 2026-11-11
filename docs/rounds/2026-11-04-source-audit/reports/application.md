# Application 层续审记录（2026-11-05）

本轮直接在 `/home/erwin/Projects/range-fea` 续审，保留已有 verifier/test 补丁。清单仍为 **72 个 application-lib 路径**；本次按 `9ea1d51..e3d5277` 核对剩余测试的完整历史变更，当前 HEAD 为 `e3d527781add4cd388276392f63a7c9ee9bd5ec2`（未提交工作区另计）。不按批次归因开发日期或模型。逐文件真实深度、检查、SHA-256、发现及待运行步骤见 [application.json](application.json)。

**本次只接管原 10 pending 与 3 partial 测试，不重审已完生产文件。** 十二份测试逐整文件逻辑读完；`tst_native_result_broker.cpp` 阅读本批次全部变更（639 新增行、690 行完整 diff）、现有夹具 1–187 行及必要解析调用点，标为 changed-hunks，不声称审完原有未改测试。此前已完成的 59 行记录原样保留；只刷新本次 13 测试的当前哈希。

**没有执行 CMake、编译、构建、CTest、产品启动或 Provider 调用，没有提交、推送、更新 gitlink 或使用 reset/clean/stash/checkout。** CMakeLists.txt 仅作为文本阅读。测试源码未编译；静态审查与 Python 独立算术均不计为 C++ 验收。

## 覆盖状态

| 深度 | 文件数 | 含义 |
|---|---:|---|
| reviewed_static | 62 | 原 50 项保留，加本次 12 份整文件语义审查；不等同于无缺陷或运行通过 |
| reviewed_changed_hunks | 10 | 原 9 项保留，加 broker 全部历史变更与必要上下文；不声称审完未改部分 |
| partial_static_review | 0 | 本次接管的 3 项已按实际深度完成 |
| pending | 0 | 本次接管的 10 项已完成语义审查 |

前轮完整阅读的重点包括 mechanics、verification、experiment/convergence、native checkpoint recovery、result broker/series、trace、批次认证缓存、数值 transport/bridge、压电配方与工件服务；这些生产源码完成状态本次不重审。模型命令的历史增量仍按 changed-hunks 记录。本次测试阅读覆盖真实夹具、断言、状态/文件恢复、数值 oracle 与必要生产调用上下文，没有把 AST、搜索、未运行、文件列举或哈希更新算作审完。

## APP-002：有限合成工件确实存在到能量判据的静态路径

这里不是“写入 NaN 的节点场绕过 broker”：broker 的 `inspectSupportedVariables()` 会拒绝非有限节点分量。触发是**输入 q/v/a 全部有限，验证器二次能量运算才溢出**。

完整路径与边界如下。

1. `tst_trusted_verification_service.cpp` 中的夹具用 `createCandidate()` 生成一单元杆（L=1000 m、E=4e6 Pa、ρ=3 kg/m³、A=0.001 m²），对应自由端一致质量 m=1 kg、k=4 N/m。`RProblemSetup` 默认质量矩阵为 Consistent。
2. 两帧 `model.write(logical,true)` 产生编号 MODEL 与逻辑 LINK；`RModel::write()` 在 MODEL 提交后才写 LINK。夹具填入匹配的步号、时间、mesh 指纹，以及有限的 q/v/a。
3. `rnative_result_broker.cpp:900–924` 的普通 legacy dynamics 分支调用 `checkpoint.validate()`，核对 available、步数、mesh、time、dt、control，再检查节点场逐分量有限。`rml_structural_dynamics_restart_data.cpp:57–99` 检查有限存储账本，但**不重算能量或验证求解器算子指纹**。夹具的能量账本默认是有限的 0，因此不会在这里被“实际能量无穷大”拦截。
4. `publishResult()` 的编号顺序、时间、来源和哈希检查，以及 `readVerifiedModels()` 的重新读取/最终哈希复查，都不进行能量二次型。返回的是实际解析 MODEL，不是可由调用方注入的数值 DTO。
5. verifier 的 specification、identity、diagnose/modelSupport、有限节点场、零支座、完整 timeGrid 和有限独立矩阵均可满足；dt/16、dt/32 的线性参考积分仍保持有限。随后 `energy()` 才出现无穷大，能量差出现 NaN。旧 `std::max(previous,NaN)` 保持 0，新有限性守卫拒绝。

**结论：没有发现此合成夹具的静态阻断门槛，且独立算术复现与该路径一致；实际 C++ 文件往返仍未执行。** 夹具是 legacy broker 准入回归，不是生产求解器可生成该极端状态、物理小变形有效或重启可接受的证明。生产 checkpoint 语义认证与本工件元数据检查不得混为一谈。

本轮保留 APP-002 实现，并给同一数据化测试增加 `1e-3 m` 正例和 `1e160 m` 溢出反例，同时要求 SDOF 解析位移判据通过。

## 实际修补

### APP-001：保留已有逐方程静力残差修补

未覆盖已有改动：静力自由方程独立归一化，防强轴力掩盖弱轴转角。原 1e-6 门槛、独立位移、支座、能量与身份检查均保留。已有真实工件正负回归仍未运行。

### APP-003：初始条件比对遗漏 enabled 与分量元数据

- 位置：`rtrusted_mechanics_service.cpp:937–1016`。
- 原比对只包含类型与数值历史，并以第一分量的样本数为遍历上限。关闭非零初值分量、修改单位/时间单位或给其他分量追加样本，都可能仍被认作原 ProblemSpec。
- 生产侧 `rsolverstress.cpp:1555–1568` 明确跳过 disabled 分量，因此 enabled 不是展示字段。
- 已把组件数、variable、enabled、SI unit、keyType、keyUnits、每分量完整时间键加入 expected/observed 签名，不改初始数值或求解物理。
- 新回归 `rejectsAlteredInitialConditionMetadata` 覆盖 displacement、velocity、rotation、angularVelocity；各有未修改正例，以及 disabled/value-unit/key-unit/shifted-key/extra-sample 反例。
- 兼容影响：原先误收的非 SI、禁用或异常时间表现在失败；候选构造器生成的三分量 SI、t=0 默认合同已与模型构造源码核对。

### APP-004：网格修复先收窄未限幅的授权上界

- 位置：`rtrusted_experiment_service.cpp:1813–1833`；真实授权范围校验位于 `822–862`。
- 非 convergence-study 授权允许整数值的任意有限 double 上界，包括 DBL_MAX。原 `qMin(int(maximum), qMax(current+1,current*2))` 在限幅前进行越界 double→int 转换；这是 C++ 未定义行为，不应靠平台的偶然结果解释。
- 已改为在 double 中先计算下一步并限幅，检查结果可表示为 int，再转换；同时避免有符号整数倍增溢出。物理、授权范围和阈值未放宽。
- 新回归经真实 authorize 路径验证 4 个单元在上限 6、8、DBL_MAX 时分别建议 6、8、8；无余量时拒绝且不改输出对象。

### APP-005：Linux 正例夹具使用 Windows 专属路径

- 位置：`tst_piezo_axial_experiment_service.cpp:22,118–121`。
- 原 `C:/range-tests/external_force.py` 在 POSIX 上不满足实际解析器的绝对路径检查，正例在到达预期的声明检查前就失败。
- 改用 `QTemporaryDir::filePath()`，保留既有正例与四种畸形声明反例。只提供声明路径，不创建或执行 Python 源。
- 这是回归夹具移植问题，不把它扩大为产品数值缺陷。

### APP-006：模态残差同样会吞掉溢出产生的 NaN

- 位置：`rtrusted_verification_service.cpp:456–467`。
- 四单元均匀固定/自由杆的一致质量最低离散本征对，节点模态为 `sin(jπ/8)`。放大到 `1e308` 后每个分量仍有限，但每个自由行的 `Kφ` 和 `ω²Mφ` 溢出；残差成为 NaN，旧最大残差留下 0，最大尺度成为 inf，最终 `0/inf=0`。
- 模态分支没有瞬态能量门槛补救，frequency 与 modeShape 可同时通过。
- 已显式要求每行累计残差及尺度有限，再用已有有限安全归一化；**不改变模态原范数定义、1e-6 阈值或物理算子**。
- 新 `rejectsOverflowedModalResidualFromFiniteStates` 用独立离散解析本征对发布真实工件：正常幅值全指标正例；有限大幅值经 broker 后仅 modalResidual 失败。不需要生产 solve 来构造夹具。

### APP-007：v1 周期 checkpoint 恢复误要求相邻步

- 位置：`native_checkpoint_recovery.cpp:624–634`。
- 协调器/native-cli 发现后，本域独立核对生产 `native_accepted_checkpoint.cpp:1101–1106`：v1 按最多 10 个接受步/1 秒周期发布完整 MODEL，不是每步发布。旧恢复代码却对所有模式要求 `oldLatest.sequence+1==candidateSeq`，合法 10→20 中断事务因此被拒。
- 最小修补仅将紧邻要求限定于 v2 trace。v1 的严格递增仍由 `validatePointer():412–414` 保证，context、schema、旧 lastvalid 顺序、哈希、事务文件名及 WriterLease 检查全部保留。
- 现有 `tst_piezo_accepted_trace` 增加七组 `checkpointRecoverySequencePolicy` 数据：v1 10→20、10→11 正例；不递增、倒退、旧 context 不符反例；v2 10→11 正例与 10→20 反例。正例检查 restore、重复 restore、finish 后清除 PENDING；反例检查指针和 marker 字节不变。
- 此夹具明确只验证存储恢复协议，使用 opaque MODEL 字节；不声称经过原生物理/Provider 认证。native-cli 生产发布回归由其本域负责，本域未写其他域文件。

### APP-008：轴向 bridge 测试比较助手可误收非有限期望值

- 仅改 `tst_piezo_axial_backend_bridge.cpp`。旧 `close(a,b)` 只检查实际值 a 有限；`close(1,NaN)` 的比较为 false，`close(1,±inf)` 则为 `inf > inf`，都不抛错。
- 加入期望值 b 有限性检查，保留原有限值容差。新增 `numericComparisonRejectsNonfiniteOperands`：相等/近值正例，有限偏差反例，NaN/±inf 在任一侧的六个反例。
- 这是测试断言缺陷，不代表已发现生产输出非有限。标准库独立算术核对通过，C++ 源码未编译、未执行。

### APP-009：canonical JSON 反例未隔离实际拒绝原因

- 仅改 `tst_trusted_holdout_pack_verifier.cpp`。escaped-public 反例原来没有把变更后的原始字节 digest 签入 manifest：即使 canonical 检查丢失，仍会因 digest 不同而拒绝。随后的 newline-key 反例又沿用了坏 public cases，可能被其他拒绝路径掩盖。
- 夹具 `resign()` 增加可选原始 public 字节，按相同字节计算 digest、签名并落盘；escaped-public 负例要求精确 `invalid_canonical_json`。恢复 canonical public 正例后再测重新 pin 过的 newline-key，要求相同精确错误，最后恢复 key 正例。
- 只为核对夹具读取生产 verifier `207–271` 的校验顺序，没有改生产校验。RSA/OpenSSL/Qt 实际执行仍待授权。

## 已执行的轻量检查

产物位于 `artifacts/bugfix-audit-20261104/resume-20261105/application/`。执行前将 TMP、TEMP、TMPDIR、HOME、XDG_CONFIG_HOME、XDG_CACHE_HOME、XDG_DATA_HOME、XDG_STATE_HOME、XDG_RUNTIME_DIR 重定向入该目录；算术脚本还检查 `tempfile.gettempdir()` 的解析结果。Python 使用 `-I -B`，仅标准库。

`arithmetic-checks.json` 中的独立结果如下。

| 场景 | 关键结果 |
|---|---|
| 瞬态正常尺度 | 位移参考误差约 5.327e-8；能量残差约 1.059e-16；新旧能量门槛均通过 |
| 瞬态有限大状态 | q/v/a 有限；参考误差约 5.327e-8；初能量非有限；旧门槛通过，新门槛拒绝 |
| 模态正常尺度 | 离散频率相对连续体约 0.006437；归一化本征残差约 2.400e-17 |
| 模态有限大幅值 | q 全有限、频率仍满足 2%；旧模态门槛通过，新门槛拒绝 |
| 网格授权上界 | 上限 6、8、DBL_MAX 的限幅结果分别为 6、8、8；DBL_MAX 本身不可表示为 int32 |

前轮独立算术不是 broker、C++ 测试或产品 PASS。本次 `remaining-tests-checks.json` 记录 APP-008 旧误收/新拒绝及有限正负例的标准库 IEEE 算术、APP-009 等价 JSON 字节不同但 digest 不同的独立核对，以及 13 行当前哈希、72 项唯一性/计数和其余 59 行原样保留的一致性。Python 只用标准库 `-I -B`，执行前验证全部 profile/temp 路径解析在指定产物目录内。

完整本批次测试 diff 保存为 `remaining-tests-batch.diff`；broker 完整 diff 为 `remaining-native-broker.diff`。新增整文件的历史 blob 与当前文件/已有局部补丁已核对；没有因保存 diff 本身计作审查。两份本次测试补丁 `git diff --check` 通过；没有运行任何 C++、产品或 Provider。

## 未解决/需平台执行确认

完整条件、确定事实/假设、影响、兼容风险和验证预期保存在 JSON `openItems`。

- **APP-OPEN-01 文件系统竞态**：恢复和 raw-series 的 QFileInfo→open/read/hash 间隙是否允许不遵守 WriterLease 的 ABA/reparse 替换，尚未建立可复现绕过。已检查协作锁、token/readback/final hash；后续 Linux rename/symlink 与 Windows junction/share-mode 注入需确认失败时保留旧权限和 PENDING，不释放受变更影响的 DTO。
- **APP-OPEN-02 raw CSV 首帧预读**：`rpiezo_artifact_service.cpp:215–227` 的 family.read 位于 64 MiB hash guard 前。此顺序已确认，但资源影响受 MODEL 解码器自身限制影响，未执行超限夹具。需跟踪是否在首次 MODEL 分配前拒绝，且目的文件/报告保持不变。
- **APP-OPEN-03 平台 runtime**：默认发现和编译定义仍使用 `python.exe`，不能从 Linux 静态审查推断 Provider 能工作，也不能私自改冻结 runtime 身份。后续验证必须由所有者指定支持的平台及私有 runtime/package 身份。

所有新增 C++ 正负例都需在解除禁止构建/运行之后编译执行；优先检查 APP-001/002/003/004/005/006/007，再跑受影响完整目标及平台特有负例。当前不升级任何产品验收状态。

## 本次十三文件的完成深度与待运行步骤

以下均相对 `src/range-application-lib/tests/`；每份 JSON 记录有更细的 checks、现有发现和具体平台步骤/预期。

| 文件 | 本次深度 | 授权后重点验证 |
|---|---|---|
| `tst_native_result_broker.cpp` | 全部 changed-hunks + fixture/callsite | Qt6 测试钩子；替换期间仍解析原快照、stale/32 MiB 无 records；实际 native piezo/mixed 及 null history |
| `tst_piezo_artifact_service.cpp` | 整文件 | 批准的私有 runtime；CSV/工件精确数值与哈希、拒绝保留目的文件及旧 report、真实 symlink |
| `tst_piezo_axial_backend_bridge.cpp` | 整文件 | 先 APP-008 无 Provider 助手正负例，再真实 runtime 全套；RAM/RBM 每相位、postFE 失败不推进且 retry 一致 |
| `tst_piezo_certification_batch_cache.cpp` | 整文件 | 无 Provider 的 controlled RPC；两族 exact codec/clock、坏响应双 bank 清除、pending abort、线程与 callback 生命周期 |
| `tst_piezo_delivery_examples.cpp` | 整文件 | 冻结 runtime 的 24 代表性数据行、RK4/ROM/on-off、真实 KKT、near-root cluster、端点账本和恢复 receipts |
| `tst_piezo_experiment_commands.cpp` | 整文件 | preview 可能需 Provider；Undo/新分支 ID、完整指纹 stale、v3→v4 journal、锁失败不改历史 |
| `tst_piezo_local_directions.cpp` | 整文件 | fixture 声明 Python 3.13.5/NumPy 2.2.6/SciPy 1.15.3；旋转协变、局部 P/T 与加速度差分、external 声明/恢复 |
| `tst_piezo_numeric_transport.cpp` | 整文件 | Qt6/QProcess + 选定 Python controlled fixture；两条 deadline 行各等待 61000 ms，全目标超时需超过 122 秒及开销；Windows 真正 share/reparse/extended-path |
| `tst_piezo_pzt_prestress.cpp` | 整文件 | 批准的 runtime；独立材料/预应力/反力/切线/绝对读数、LQR/MPC 与 RAM/RBM 各 held phase；不等于硬件资格 |
| `tst_piezo_result_series.cpp` | 整文件 | 批准的 runtime；exact codec/time/channel bits、hash witness、prefix 先拒绝、in-flight 全 DTO 失效、260 步 pipeline abort/recovery |
| `tst_trusted_experiment_service.cpp` | 整文件 | 无数值 Provider；APP-004 后全套授权/预算/取消/repair/原子存储/逐子任务验证绑定 |
| `tst_trusted_holdout_pack_verifier.cpp` | 整文件 | Qt6/OpenSSL 3 RSA；APP-009 精确 canonical 拒绝和恢复正例后全套签名/信任/协议/Unicode |
| `tst_trusted_mechanics_service.cpp` | 整文件 | 无 Provider；APP-003 四族初值元数据正负例后全套 SI/selector/约束/物理 digest/失效证据 |

所有步骤都只在解除本轮禁止后执行：先解析验证 TMP/TEMP/TMPDIR/HOME/所有 XDG 路径及可选 `RANGE_PIEZO_DELIVERY_EVIDENCE_DIR` 位于本轮指定产物目录，编译输出仅 `build/`。Linux、Windows 都需实际 Qt 文件/序列化验证；Linux 的 Windows 分支 QSKIP 或无权限 symlink skip 不算平台通过。私有 runtime/package 身份由所有者明确提供，不擅自替换。

目前 **pending=0、partial=0**。10 项 changed-hunks 仍不代表未改部分已审完；全部 C++ 测试仍未编译、未执行，APP-OPEN-01/02/03 未关闭。只修改本轮两份测试和 application.json/.md，保留此前工作；未写 coverage.json 或根总结。
