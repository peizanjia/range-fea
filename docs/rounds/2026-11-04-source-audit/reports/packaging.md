# 脚本、打包、CI与小子模块批次审查（2026-11-05 续审）

历史源码批次：9ea1d51 → ad9dbda；覆盖清单包含 e3d5277 后续 JSON 编码修正。模型/日期归属无法从集中提交证明，不归因 Luna。当前审查日期：2026-11-05。

原分配清单仍为 **114 项**，当前 **reviewed_static=108、reviewed_changed_hunks=4、partial=2、pending=0**。API 中断后的续审从 62/17/35 接续，完整阅读另外 45 份文件；4 份 CMake 仅完整阅读 `9ea1d51..e3d5277` 差异，单列 changed-hunks，未冒充全文审查。另列 **8 个**依赖/新增测试文件（含协调器追加的两个最终writer独立回归），不混入 114 项计数。此前 62 个 reviewed 行逐字段保留，不重复验收；剩余两项 partial 是 GUI 路线合同和 required CTest 注册完整性；外部会话 cloud CMake 后由协调器通读当前49行并补足静态审查，不计为本审计实施或构建证据。逐文件 SHA、覆盖与待验项见 [packaging.json](packaging.json)。pending=0 不等于全项目审完或 Windows 验收通过；本轮仍有明确未修合同/证据问题。

## 2026-11-04 历史检查（保留来源）

- 完整阅读range-ai-lib 6文件、range-base-lib 1文件、range-cloud-lib 13文件、range-gui-lib 6文件的小差异，检查Provider model元数据边界、Agent OFF条件编译/稳定枚举以及零工作量进度；未发现本次可确定的新缺陷。range-build-tools固定提交未变化。
- 定向阅读Windows CI、CMake/安装器、GUI性能/终态/登记校验、v43安装清单/静态准入/adapter以及部分PowerShell/C#控制路径；各文件阅读深度见JSON。
- 20份变更Python文件完成AST解析，未产生pyc。
- 10个按文件选取的轻量unittest发现调用：6个exit0；3个v43套件因Windows路径合同与Linux临时目录不适用而失败；另test_installed_profile.py是需要产品可执行文件的CLI，发现0项测试、exit5，因此标NOT_EXECUTED，没有启动产品。
- 轻量测试的TMP/TEMP/TMPDIR、HOME与XDG目录均定向仓内artifacts/bugfix-audit-20261104/packaging，禁pyc；原始输出在该目录logs/及light-checks.json。

## 保留问题与边界

1. v43 Windows清单adapter要求Windows绝对根，Linux/Python3.14的POSIX临时目录会被ntpath.isabs拒绝。保留三个套件失败，未为了Linux绿色放宽Windows包身份合同。需在Windows执行原fixture或另行设计显式跨平台测试适配。
2. 历史记录的 C# `ArrangeMeasurementWindows` 同窗分支风险已在本轮沿调用链核实为**审计工具的布局证据缺陷**并修补，详见 PKG-REVIEW-01；不据此声称历史产品验收已经误通过。
3. 本机未安装pwsh/Windows PowerShell5.1，无法执行PS/C#准入/监督器/UIA/Win32路径；相关未读完文件保持pending/partial。没有安装工具或运行这些脚本。
4. CI的360分钟总上限与单项6小时理论上限之间没有setup/build余量；本次未修改运行预算，仍需后续平台/调度复核。

上述历史轮次没有修改生产源码。本次续审修补如下；没有改写历史测试结果或产品状态。

## 本轮边界与覆盖

- 已读迁移交接、原 packaging 报告/JSON；当前分支仍为 `feature/structural-profile`，HEAD 为 `e3d5277`。`main==workbench` 指 C# 窗口句柄分支，不是 Git 分支相等。
- 完整阅读 Windows supervisor → native Job → worker fixture → handshake → terminal → watchdog 链及相关测试；完整阅读 common 身份/单次读取助手、sampler 参数及其候选消费合同相关部分、finalizer 和标量回归。
- 完整阅读 UIA/C# 辅助代码、取消/关闭/诊断/缩放脚本，跟踪共享函数、registry/terminal/verify 调用。`pc11_gui_uia.ps1` 虽已读完正文，复用 profile/Measure 与冻结验收证据的路线语义仍未闭合，保留 partial；候选控制聚合脚本仅审相关部分，也保留 partial。
- 完整阅读 CI workflow-plan 验证器及测试、CTest 元数据解析器及 mock 测试。246 个 required 测试名分属 9 组；未审完每个 CMake 注册条件，清单状态只升为 partial，未执行生成后的 CTest inventory。
- 本轮只改本线脚本/测试、`packaging.json/.md` 和指定 packaging artifacts；不写 coverage 或根总结。不使用已撤销的 AGENTS 要求，不重委派。没有 CMake、编译、构建、CTest、产品启动、安装、Provider，也没有 reset/clean/stash/commit/push/gitlink 修改；保留其他改动和 `.idea`。
- **小子模块来源分离**：第一节的小子模块结论仅属 11-04 历史批次。本轮没有编辑或重新验收小子模块；明确避让另一会话独占的 `range-cloud-lib/src/rcl_http_server.cpp`、新 `tests/tst_http_server_body_limit_9345b7.cpp` 与 `tests/CMakeLists.txt`。最终 SHA 校验发现 `src/range-cloud-lib/tests/CMakeLists.txt` 在 ledger 写入后由该会话改变；本轮未触碰该文件，无编辑冲突。JSON 分别保留历史 reviewed SHA 与当前 SHA，记录 `concurrentSourceChanges`，该项当前降为 partial，不把另一会话修补归入本轮成果。

## 确定缺陷与实际修补

### PKG-REVIEW-01：同窗布局把未测量的工作区条件记成成功

- **位置/触发**：`scripts/benchmark/pc11_gui_native.cs:70-81`；主窗和 workbench 为同一 HWND，实际矩形超出工作区。
- **调用与影响**：`pc11_gui_uia.ps1:341-347,573-608` 用 `LayoutValid` 决定是否继续；`NativeWindowFor` 可返回主窗。先前 `FitWindow` 请求及五个内部曝光采样点不能证明整个矩形仍在工作区内。旧分支直接返回两个 true，属于审计证据缺陷，不是已复现的产品验收误通过。
- **修补/测试**：读取实际 monitor/work area，用现有 `Inside` 判定填入 `WithinWorkArea/LayoutValid`；补 Python 路由 guard 与 C# 精确生产谓词的正/负测试源。旧 HEAD 源码负对照按预期失败，当前源码 guard 通过。
- **兼容风险**：以前被容忍的裁切或最大化边框现在会失败关闭；Windows 多显示器、负坐标、高 DPI、任务栏和独立浮窗路径仍需实测。原尺寸阈值未改。

### PKG-FIX-02：候选 pin 与 worker 握手/终态丢失 JSON 标量类型

- **位置/触发**：`runtime/contract-common-v43.ps1:43-84,192-238`、`worker-handshake-v43.ps1:42`、`supervisor-core-v43.ps1:154`。原代码先 `[string]` 化 pin、`[int]` 化 PID；部分字段只比较值。
- **证据/影响**：PowerShell 集合 `-cne` 是筛选操作，空数组或全等数组不形成拒绝条件；0 可比较为 false，小数 PID 可被转为整数。读取完整生产链和现有测试后确认这些 malformed shape 能绕过原类型合同，但本机未运行 PowerShell，也没有观察到恶意进程或实际 GO。
- **修补**：对象容器/字符串 pin、布尔字段与整数 PID 先验类型检查；提取供真实 worker/supervisor 调用的严格 ready/terminal 校验函数。增加六个 manifest pin/container 负变体和独立 `tests/test-worker-receipt-types-v43.ps1` 正/负测试源。
- **兼容风险**：过去被强制转换的手写 fixture 将拒绝；合法 manifest 的规范化序列化/身份算法不变。新纯 PS 测试独立运行，没有改冻结来源集合、19 项 required IDs 或任何验收 pin。

### PKG-FIX-03：sampler 的身份 pin / commandLine 可接受集合

- **位置**：`runtime/sampler-arguments-v43.ps1:92-100,120`。
- **触发/影响**：两个身份字段或 commandLine 是空/全等重复数组时，原比较不能证明精确字符串相等。参数 builder 本身不启动进程。
- **修补/测试**：补字符串标量检查，新增 6 个集合负断言。已读候选 consumer 的 `test-candidate-control-contract-v43.ps1:180-370`，保留其 **13 case / 19 argv** 凭据形状，没有增加 receipt case 行数。Windows 绝对路径、引号转义、50ms 和最小采样合同均未放宽。

### PKG-FIX-04：诊断脚本漏导入共享 Capture 的依赖

- **位置/触发**：`pc11_gui_diagnostic_read.ps1:49,78` 导入 `Capture` 后首次调用；该函数在 `pc11_gui_uia.ps1:329` 调用未导入的 `Assert-WindowExposure`。
- **影响**：新 PowerShell scope 会在首次 reader 前截图时 command-not-found；失败截图也走相同缺失依赖。
- **修补/测试**：导入 exposure guard，同时补 `Action` 的 `Observe-BaselineMemory` 依赖。未导入/执行 harness 的 launch/run 主体。源码正测试通过，HEAD 负对照按预期失败；真实 UIA 诊断未运行。diagnostic-only 与 qualification 边界不变。

### PKG-FIX-05：finalizer 在哈希后重新读取权威文件

- **位置/触发**：`finalize-v43.ps1:56-65,149,178,187,211`。旧候选 manifest/source-freeze/package-tree/aggregate static 四处均 `Assert-Hash` 后重新 `Read-Json`；路径内容若在两次读取间替换，解析内容不再受外部 SHA 覆盖。
- **影响/确定性**：源码中的 TOCTOU 窗口确定；并发替换是触发假设，不是已观察到的受损凭据。即使错误也仍为 no-GO，但会错误证明静态身份。
- **修补/测试**：四处统一读取一个 byte buffer，校验该 buffer 再解析。`test-finalize-scalar-v43.ps1:79-111` 添加哈希后确定性路径替换、后续错 pin 与损坏 JSON 回归源。没有执行 PS 测试；现有 status/required IDs/身份格式未改，也未重签冻结源码 pin。

## API 中断前保留的风险与未完成项（历史状态）

1. **PKG-CI-01（不是已证运行 bug）**：`.github/workflows/windows-regression.yml:18` 为整个 job 的 360 分钟；`src/range-application-lib/CMakeLists.txt:114` 为两个 required 测试各设 21600 秒。setup/build、其他测试及调度消耗同一 job 预算，`-j2` 不消除这些耗时。交接记录的既有 CTest 为 12659.93 秒、7 个其他失败，并未证明总预算导致取消。本轮不改预算或跳过测试；需另行授权的 Windows CI 测量后再决定是否拆分 job。
2. **PKG-REVIEW-02（合同问题未决）**：profile 复用可能跳过当次 source picker，而 `qualify()` 仍要求对应事件；Measure 路线也不写 Full 才有的 terminal/自动绑定动作。是否应被资格化尚需核对冻结合同，当前仍失败关闭。未为了通过而接受较弱身份来源，harness 保留 partial。
3. `runtime/root-bootstrap-v43.ps1`、root/path/seal/reparse 测试仍 pending；source-preflight/freeze/package-tree/release-build 和大部分 static-chain/candidate 聚合仍未完整审查。
4. `inventory_v43.py`、`admission_v43.py`、adapter、OFF link-map 与完整 Python fixture 仍有 partial；三个历史 Linux/Python3.14 Windows-root 失败未转为 pass/skip，也未修改 Windows 生产路径合同。
5. 原 114 项以外的新增/依赖文件单列 `supplementalFiles`；AST、搜索、源码 guard 只作为局部检查，不据此宣称某条链完整或平台验收通过。

## API 中断前实际检查（保留原结果，不重跑）

输出根为 `artifacts/bugfix-audit-20261104/resume-20261105/packaging/`。每次运行前将 TMP/TEMP/TMPDIR、HOME/USERPROFILE、APPDATA/LOCALAPPDATA、XDG_CONFIG/CACHE/DATA/STATE/RUNTIME 重定向其下，并验证 resolved path、`tempfile.gettempdir()` 和 `Path.home()` 均在仓内；Python 使用 `-B -I`，并禁 pyc/user-site。

| 检查 | 实际结果 | 范围 |
|---|---:|---|
| `test_workflow_plan.py` | 9/9 PASS | 含新 cp1252 stdout 成功/错误 JSON 回归 |
| `test_verify_required_ctest.py` | 7/7 PASS | subprocess runner 为 mock；**没有运行 CTest** |
| `test_pc11_gui_uia_static.py` | 14/14 PASS | 源码路由 guard，不是 C#/UIA 执行 |
| `pc11_gui_terminal_tests.py` | 5/5 PASS | 合成 registry 元数据，不调用 Provider |
| 两个新 GUI guard 对 HEAD 源码负对照 | 2 个预期断言失败，0 意外 error | 证明 guard 会捕获原问题，不证明 Windows 行为 |
| `verify_workflow_plan.py --next --json` | exit 0 | 只读；14 tasks、119 acceptance IDs；未修改状态 |
| required CTest JSON parser | 9 groups / 246 names | 未生成、查询或执行 CTest inventory |
| 5 份相关 Python AST、`git diff --check` | PASS | 仅语法/格式检查 |
| 最终 JSON/覆盖/逐文件 SHA 一致性 | PASS | 114 唯一项、62/17/35 计数、4 个补充文件；分离并发 cloud CMake 修改后全部 SHA 一致 |

合计 **35 个实际 unittest 通过**。日志与命令/env 记录在 `logs/`、`light-checks.json`；没有任何 PowerShell/C# 测试运行，没有工具安装。

## 第一轮平台依赖后续步骤（仍未授权执行）

- **无需编译/产品**：Windows PowerShell 5.1 的 `test-worker-receipt-types-v43.ps1` 应接受合法 ready/terminal、拒绝错类型/错 PID/错 action；`test-sampler-arguments-v43.ps1 -AuditRoot <新仓内目录>` 应保留 13/19 凭据形状与 no-GO；`test-finalize-scalar-v43.ps1 -ReceiptPath <新仓内文件>` 应验证 pinned buffer 不受哈希后路径替换影响。先重定向并验证该平台所有 temp/profile 路径。
- **需要另行允许编译/原生子进程**：`test_pc11_gui_exposure_classifier.ps1`（Add-Type）、supervisor-worker/native Job/assignment-failure/kill-on-close 套件。预期所有有效控制通过、负控制拒绝、fixture/product readiness 仍 PENDING、goPermitted=false。
- **需要另行允许真实产品/CI**：单窗/浮窗布局与诊断首帧、DPI/工作区验证，以及 360 分钟预算测量。先补完相应未审调用链；不把静态修补和合成绿灯当作产品批准。

## API 中断后续审：静态范围与结论

- 完整读完原剩余清单中的 **45 份打包文件**，包括 901 行 root-bootstrap、祖先句柄/相对创建/清理分支、root/path/seal/reparse 测试、四个生产者、candidate-control/static-chain/final-readiness 聚合、Save predicate/环境绑定、inventory/admission/adapter/OFF link-map 及其完整 Python fixture。全文件为历史区间内新增，全文阅读覆盖该区间全部新增差异；修改的三个生产者同时审查本轮补丁。
- 完整读完 **4 份 CMake 历史 diff**：`src/CMakeLists.txt`、`src/fea/CMakeLists.txt`、`src/fea-solver/tests/CMakeLists.txt`、`src/range-workflow/CMakeLists.txt`。仅标 `reviewed_changed_hunks`，没有声称所有未改上下文和每个 required 测试的注册条件已经闭合。
- 续审交付时**保留3 partial**：GUI profile-reuse/Measure 的资格化路线合同未决；required CTest manifest 尚未逐条闭合全部注册；cloud `tests/CMakeLists.txt` 当时是外部未读delta。协调器后来完整读取cloud当前49行，故最终只保留前两项partial；不改外部源、不继承其运行结论。SHA检查不代替这次新增逻辑阅读。
- 本续审没有重委派、没有运行此前 35 项测试，没有 CMake/构建/编译/CTest/安装/产品/Provider/PowerShell，也没有提交、推送或 gitlink 操作。物理阈值、19-ID 顺序、13-case/19-argv 凭据、原黄金 pin 与冻结凭据均未改写。

### PKG-FIX-06：三个生产者的 single-use 输出竞态

`source-preflight-v43.ps1`、`freeze-v43.ps1`、`package-tree-v43.ps1` 原先先做 `File.Exists`，最终用 `WriteAllText`，两者之间若另一个 writer 创建同名凭据，会覆盖已有审计证据；失败凭据分支也受影响。确定的是源码竞态窗口，不是已观测到真实并发覆盖或错误 GO。

最小修补只把这三个生产者改为 `FileMode.CreateNew` 并确保 Dispose；JSON 深度、UTF-8 无 BOM、尾换行、状态和无产品动作合同保持原样。新增独立 `test-producer-single-use-v43.ps1`，只抽取真实 writer 函数，包含正常字节、已有凭据拒绝及序列化期间注入竞争凭据三种控制，每个生产者三种，共 9 个；没有加入冻结 receipt case 集合。该 PS fixture **NOT_EXECUTED**。

新增 Python 源码 guard **3/3 PASS**，检查独占创建、覆盖写入 mutant 和断开安全 helper 调用的 mutant；三个旧 `e3d5277` 生产者均被 guard 按预期拒绝。源码 guard 不等于 PowerShell 并发实测。证据为 `producer-single-use-checks.json`、`continuation-negative-controls.json` 和 `logs/producer-single-use-source.log`。

### PKG-CONTRACT-03：跨语言 canonical JSON 身份不一致（未改）

现有 `test-root-seal-boundary-v43.ps1:68-92` 黄金向量包含 `src/o'neil.cpp`，其注释和 pin 明确采用 PS5.1 的 `'` 转义。Python `admission_v43.py` 声明的 `json.dumps(..., ensure_ascii=False, sort_keys=True, separators=(',',':'))` 保留字面单引号。同组七行数据计算结果为：

- Python canonical SHA：`24dd324578beec04d429e638c5b410bf63a98e1fc51d3065cd58c17b34482f13`
- 对单引号作源码注明的 PS 转义后：`4a8f4b687fd7f0d3e5bd8c9577898fa9cc4065c2f918033f5baf05857fabed55`，**精确等于现有 PS 黄金 pin**。

这证明现有黄金向量与 Python 规范的差异；没有执行 PS 来声称平台实测。合法路径含单引号时可能导致 producer/admission 失败关闭；不是验收绕过。修复将涉及冻结序列化身份合同，因此**没有改算法、黄金向量或重签 pin**。后续需明确 canonical bytes 的唯一权威，并在 Windows 比较单引号、HTML 敏感字符和补充 Unicode 的精确字节/排序。

### PKG-POLICY-04：root reparse fixture 与本轮产物政策冲突

`test-root-bootstrap-relative-create-v43.ps1:17-34` 要求 receipt 位于固定 `build/audit-20261014/piezo-delivery`，且 temp **不得在仓内**。`test-final-readiness-candidate-v43.ps1` 会调用它；上层 static-chain 又调用 final-readiness。因此这条链并非“有 Windows 后重定向 TEMP 即可运行”，在当前只允许指定 artifacts 根的政策下仍被阻塞。未运行、未把 UNRESOLVED 算 PASS，也未改成接受较弱 reparse 结果；需要单独解决 fixture 隔离设计或授权边界。

### PKG-EVIDENCE-05：fixture/聚合输出 writer 同类竞态及协调器补修

续审交付时，三个producer之外的 `test-static-chain`、`test-root-seal-boundary`、`test-finalize-integrity-candidate`、`test-preflight-gates`、`test-static-gates`、`test-source-scope`、`test-negative-static-chain` 最终输出仍有 check-then-`WriteAllText`。协调器随后复核这七处最终writer并按 **PKG-FIX-07** 补为局部 `Write-V43FinalReceiptNoOverwrite` / `FileMode.CreateNew`，`finally Dispose`；原JSON深度、UTF-8无BOM、尾换行、case/ID/status/pin及产品no-GO标记未变。

新增 `test_final_receipt_single_use_v43.py` 源码guard **3/3 PASS**，七份旧HEAD源均被负对照拒绝；这不是PowerShell并发实测。独立 `test-final-receipt-single-use-v43.ps1` 抽取七个真实writer函数，要求有效字节、已有文件拒绝和序列化期间竞争三种控制，共21项，**未执行**。证据在 `packaging/final-receipt-checks/`。

修补仅关闭这七处最终OutputPath的覆盖窗口，不泛化到所有中间合成fixture写入；也不等于crash-safe原子发布或完整reparse-race防护。竞争创建现在应报错且保留原字节；修改后的源码必须重新冻结并生成新收据，不能重钉旧黄金pin追认原PASS。


## 续审实查与待 Windows 验证

| 项目 | 实际结果或预期 |
|---|---|
| 原 62 个 reviewed 行、原 35 项测试结果 | 逐字段保留；未重跑、未重新验收 |
| 新增 Python source guard | 3 tests PASS；含正源码及两类 mutant 拒绝 |
| 三个旧 HEAD 生产者负对照 | 3 个预期 guard 拒绝；仅源码文本 |
| 45 份文件完整静态阅读 | reviewed_static；Windows/合同 openItem 不因此关闭 |
| 四份 CMake 完整历史差异 | reviewed_changed_hunks；无生成 inventory 或构建 |
| SHA/唯一性/计数 | 114 项；107/4/3/0；6 个 supplemental；全部当前 SHA 匹配 |
| Windows PS5.1 独立 single-use fixture | 未运行；预期 9 控制通过，竞争 sentinel 不变 |
| 原三份 Windows-root Python fixture | 未重跑；历史 Linux 失败仍保留，不放宽路径合同 |
| root bootstrap / root seal / PATH / supervisor / final-readiness | 未运行；原生编译、子进程和固定目录政策均需分别授权；31-case/5-route/19-ID 和 no-GO 预期不变 |
| release-build / test-build-off | 未运行；实际会配置/构建，不能称为无需构建的轻量测试 |

续审产物均在 `artifacts/bugfix-audit-20261104/resume-20261105/packaging/`。新测试运行前已重定向并验证 TMP/TEMP/TMPDIR、HOME/USERPROFILE、APPDATA/LOCALAPPDATA 和 XDG 全部目录，`tempfile.gettempdir()`、`Path.home()` 解析均在仓内；Python 为 `-I -B`，只用标准库。`continuation-audit-integrity.json` 为本次身份记录，原 `audit-integrity.json` 仍保留第一轮 62/17/35 历史结果。

程序性偏差如实保留：本续审 Bash 的实际 cwd 经验证为仓库，但调用文本未按要求逐条写显式 `cd` 前缀，不能声称完整符合命令形式约束。一次负对照计算曾因引号 SyntaxError 失败、未生成结果，修正后的成功记录单独保留；未因此执行任何受禁动作。
