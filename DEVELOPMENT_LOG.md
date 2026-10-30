# 开发执行与交互记录

> **阅读说明（2026-10-30）：**本文件保留各日期的工程与交互摘要，不是完整聊天逐字稿，也不是统一的当前验收表。当前目标及工作线从[文档索引](docs/README.md)进入，用户决策续接见[续作交互记录](docs/CONTINUATION_LOG.md)，Trusted Agent 当前可攜状态以[2026-10-30 阶段检查点](docs/acceptance/phase-checkpoint-20261030.md)为准。下文的进程、权限、阈值和完成数字仅描述当时快照；本地 `.planning/` 和 `audit/` 记录不随 fresh clone 上传，不能充当远端可复核证据。压电 3/36 与 33/36 的历史台账冲突以各自日期记录，不按旧文字推定整体完成。

## 2026-10-20 21:08 PC-11 axialRod Run04 完成计算、结果登记失败

Release 私有安装 GUI 任务 `2026102019340001` 达到 20,000/20,000 accepted FE steps，solver 自然退出并写出 checkpoint RBM；产品 job record 因 lock ownership/read 错误持续停留在 `queued`，`processStarted=false`。验收脚本没有取得 terminal result，退出码 1；真实 plot 与完整 CSV 步骤未到达，因此 PC-11-A02 仍 pending。用户已把内存门槛改为整机 RAM `<90%`，50ms 采样峰值 76.5929%（90,839 samples）；working-set sampler 因用户要求正常关闭 GUI 后查询已退出 PID 而 INVALID，不用于新门槛。GUI 通过正常 `Quit?` 退出，solver 和 GUI PID 均确认结束。审计收据 `build/audit-20261020/piezo-acceptance/focused-functional-002/pc11-current-gui-r4-run-04/run04-disposition.json`；追加状态 `functional-addendum-20261020-17.json`。整体计划仍 33/36 pass、3/36 pending，`productComplete=false`。下一步先定位/修复运行后 job-record persistence，再续做产品结果读取、绘图和完整 CSV；不重复本次 20k 计算。

## 2026-10-14 14:42 交接退出与量化进度

用户要求按技能整理交接并退出。本轮新增 [当前接管快照](docs/PIEZO_CONTROL_HANDOFF_20261014_1440.md)，刷新 `HANDOFF.md`、续接总册、交互记录及 `.planning/2026-10-13-piezo-control-handoff/`；冻结合同/机器计划未改。梁 `planarBeam-MPC-forced`、板 `flatPlate-MPC-free` 的 runner summary 均 `exitCode=0, qualified=true`，PC-10 局部 FE/持久化矩阵为 29/30（96.7%），余板 `flatPlate-MPC-forced` 在 `initTestCase` 后交接中断，无合格收据。v45 OFF `ALL_BUILD` 编译 GUI 期间 Ctrl+C 退出 1，是主动中断，不是源编译失败。退出后查询无对应 CMake/科学测试/Python 子进程。Save 环境预检的 installed-file 句柄持有修补与 PS5.1 10 案例通过，仍需独立审查。候选控制两项 P2、root-seal/finalizer 正例/聚合、完整候选 19-ID 静态链未完成；静态候选 0/19 PASS。正式验收 **0/36 accepted（0%）**、`productComplete=false`，无产品 GUI 资格。

## 2026-10-14 14:00 PC-10 至 27/30 与 GUI 恢复入口定向通过

梁 `planarBeam-MPC-free`、板 `flatPlate-LQR-forced` 两条长行的 summary 均 exit0/qualified true，PC-10 累计 27/30：轴向科学 8、梁科学 7、板科学 6、三族 RAM/RBM 6；余梁 1／板 2。定向 Qt 通过 `selectionRecoveryShowsModelTreeWithoutChangingGlobalSelection` 在锁住当前来源时的恢复按钮可见、解锁后正常隐藏，修改后 3 PASS／0 FAIL；完整工作台 314.41 秒 1/1 PASS 属该小修前的诊断构建，不能移称修改后全套回归。真实安装 GUI、静态候选及三族 20k 仍无 GO，36 条 pending、`productComplete=false`。

## 2026-10-14 13:58 OFF 诊断工作台复测与恢复入口

首次 OFF v44 选定 CTest 4/5，工作台因诊断配置的私有 Python 路径为空报 backend 路径缺失，内部 17 PASS／30 FAIL／1 SKIP；原失败日志保留。配置 `build/private-python-v3`、重编 GUI 测试目标后，工作台完整 CTest 1/1 PASS，314.41 秒。读 GUI 源码确认 11 顶层模块和压电六页，已有少操作的集中预览／一次确认及保存实验 Run→确认逻辑；单一模型暂不可读时恢复按钮错误隐藏，已改为文件不可用时保持可见，并加入锁住来源的定向 Qt 案例，重编/复测尚未完成。真实安装窗口视觉、紧凑尺寸、键盘/UIA 仍未验。静态 root-bootstrap 正常/失败分支的未锁 mkdir 已移除，两项 P2 经 Sol medium 复审关闭；native mutated-handle 路径在本机未触达。PC-10 仍为 25/30，全部 36 项和产品仍 pending。

## 2026-10-14 13:40 OFF 全目标诊断完成、静态证据链型别复核

OFF v44 隔离全目标诊断构建 exit0，`Release/fea.exe` SHA256 `963A9432EE6E855FA5568D92416345CFB7133313B92FAD826889C18769C055BE`；该构建期间源脚本仍在编辑，不能代替最终同源发行。选定 OFF CTest 已启动。PC-10 板 `flatPlate-LQR-free` summary 为 exit0/qualified true，真 FE／持久化增加至 25/30，剩余梁 2／板 3。Sol medium 确认 finalizer 旧数组型别绕过 P1 已关闭，包树候选／冻结／build 跨源绑定已复审，另补严格标量与 17 个函数级正反例；未运行完整 manifest 链。root-bootstrap 最终 receipt 使用同一已锁父目录 handle 的 native 相对创建，但独立 Sol medium 指出更早的父目录自动创建可在 junction 切换后造成越界目录副作用，同时检测夹具可吞下越界创建后清理的异常；Luna 正修，候选仍 PENDING。freeze／聚合器标量硬化与 OFF Release build-receipt 夹具仍在进行，首次夹具因并发源变更按合同拒绝。无静态 GO、无真实安装 GUI 资格，12 阶段／36 项均 pending，`productComplete=false`。

## 2026-10-14 13:18 v43 模块边界修复、Save 判定覆审与交接更新

四个实际静态 producer 的可信模块边界与 24 个 PS5.1 对抗案例已通过。聚合器现先将实际测试、helper、四 producer 六文件与冻结后来源行逐个核 hash，保持 Windows 只读共享文件句柄跨子测试运行，再复核 hash；独立 Sol medium 审查无剩余 P1/P2。没有完整候选 manifest，故模块 ID 仍未形成候选级 PASS，产品 GO 仍 false。Save 纯判定原把整数 `0`／字符串 `'False'` 误当布尔 false，已改严格 `[bool]` 检查；15 个 PS5.1 案例和独立覆审通过，新单次收据为 `build/audit-20261014/piezo-delivery/gui-fresh-run-v43-output-audit/save-predicate-fixture-v43-f119b41938ff402baf00187fd335f3f6.json`，只证明 no-product-action/PENDING。root-bootstrap 原 12 个合成案例通过，但独立审查在真实 PS5.1 重现预载 `ConvertFrom-Json` 执行的 P1，另指出输出父路径替换 P2；Luna 正修，不采纳为静态 gate。新 OFF 全目标诊断编译与 PC-10 梁／板余下 7 行继续；当前 23/30。跨平台续接页已在顶部写出精确当前位置、运行任务、剩余门槛与操作顺序。冻结 12 阶段／36 项均 pending，`productComplete=false`。

## 2026-10-14 13:08 OFF 新 map 诊断阴性、全目标诊断构建

新 OFF 隔离 `fea.exe` 与公开 API 负测目标构建退出 0，后者 CTest 1/1 PASS。新 `fea-link.map` SHA256 `96F00F796AD5EC7B3FBAF0088535C48D60CEA42B45160998621ECB24BD40DECA`，直接搜索先前旧 Cloud AI 对象、query 与 Agent/OpenAI 命名无命中；尚无 fresh install/inventory/正式独立 map gate，故不将诊断 map 当发行 PASS。OFF 全目标诊断构建已启动；PC-10 梁／板 PI 受迫新增后暂 23/30 行合格。v43 模块 producer 接线的独立 Sol medium 审查发现 helper 内未限定 `Where-Object`／`ForEach-Object` 可被预载函数劫持，修复中；候选模块门槛保持 pending。完整 36 标准及产品仍未交付。

## 2026-10-14 12:59（Asia/Singapore）压电控制续开发

继续 `PIEZO_CONTROL_DELIVERY`，未使用 Codex with ChatGPT。OFF Cloud AI 隔离 v44 GUI 诊断构建仍在编译；旧 OFF map 的 430 个旧 Cloud AI 命中未获新 map 推翻，旧安装不放行。`scripts/packaging/piezo_gui_static_v43/` 已收纳 v43 静态生产／准入工具；诊断源预检 PASS（2086 文件、7 子模块、SHA256 `5d978b100a27e50cd53556ecde028e5f15d05b043cfa280765cb7478d568d463`），后续源码改动使其不具最终冻结资格。监督器三项 P2 清理／哈希漏洞已修复、PS5.1 聚焦测试及独立覆审通过；仍为 fixture-only/no-GO。另经 Sol medium 发现本地模块 helper 测试不能证明实际 producer，候选总表保留该两门槛 PENDING，只提升 parser；另 14 项候选静态门槛尚未通过，实际 producer 接线正在开发。PC-10 新增梁／板 PI-forced，23/30 真 FE／持久化行合格，其余梁／板继续。最终全量同源构建、安装 GUI、三族 20k 性能、完整 36 标准仍待，`productComplete=false`。

## 2026-10-14 12:45 OFF 边界修复、门控复审与持久化工具

- OFF 旧 Cloud AI 查询源码已加条件编译隔离，Cloud/GUI 子库 OFF 构建、公开 API 负测 1/1 与 ON 两库构建通过；独立 Sol medium 无确定 P1/P2。新版完整 OFF `fea.exe` 尚在 v44 隔离诊断构建，未重新安装或产出可判定的 map。
- 独立 map gate 的未解析表间空档经 Sol medium 指出并修复，17/17 合成测试通过；修订程序重放旧真实 map 仍 FAIL，430 条旧 Cloud AI 命中。v43 admission 的失败 source/build receipt 及 OFF map receipt 强制 pin 已补，66/66 合成测试通过。源范围漏扫 `src/build/` 与 static harness 父目录定位已用 Windows PowerShell 5.1 修复／复测；搬至受版本控制的 `scripts/packaging/piezo_gui_static_v43/` 后，66/66 Python、producer 路径、producer→admission 源范围及 3 个拒错案例通过。实物源冻结、包树、静态 GO 和产品动作仍未执行。
- PC-10 真 FE／持久化逐行合格 21/30：梁与板各新增 PI-free；其余两族长跑继续。三族安装版 20k 与 5 秒首图、30 秒 CSV、512000000 字节合并工作集均未实测，12 阶段／36 条保持 pending，`productComplete=false`。

## 2026-10-14 12:11 OFF 链接图独立审查发现 P1

- Sol medium 读取真实 `fea-link.map` 后确认旧 Cloud AI 查询 widget 与 cloud AI request/response 对象已进入 OFF `fea.exe`，且相关方法有实际 RVA；不仅是 archive 中存在。初次只查 `range-ai`／`agent_widget`／`openai` 字符串的阴性结果范围过窄。当前 OFF 安装与 `inventory-v43-off-final-v1.json` 虽在文件／目录／PE 静态检查上 PASS，**不能作为无 AI 的发行包或 GUI GO**。正式 root 中断了当时尚未完成首个用例的 OFF GUI／reader CTest，保留 `ctest-v43-off-final-gui-reader.INTERRUPTED.md`，后续修改 Cloud AI 条件编译后须重建、重新安装、盘点、map 审查及回归。旧 ON 科学测试 EXE 字节未变，PC-10 运行继续。

## 2026-10-14 12:06 v43 双变体终版构建、全新安装与 OFF 实物树

- ON/OFF 两棵 Release 全量终版构建及各自全新安装退出 0。正式 `inventory_v43.py` 对 OFF 安装 `install-off-v43` 得到 3171 文件、261 目录、18 项检查均 PASS；清单 SHA256 `740FAE403C4BFA070D3F80B21851A6C9E41D81F9F9D4055A53F91498BC8B505C`。外部 pin 的 `inventory_adapter_v43.py` 真树重扫 `INVENTORY_TREE_MATCH`，安装共 395456372 字节；它明确不放行 GUI。ON 最终 `install-on-v44` 清单 3173 文件、261 目录、18 PASS，SHA256 `EE39683472A583778FC9BA46188CE0886E9F0ECDE850FA43CE9829E85D0A5964`。两份清单都把发行资格标为链接图尚未独立审定；OFF `fea-link.map` 已生成，SHA256 `F231C73DDA854F612D466B5DA73F00F69E85063BCAEC3F274F6B97CD220A5D5D`，初查没有 `range-ai`、Agent 组件或 `openai` 字符串。
- GUI 新增缺少／多选时的选模直达入口和保存实验 Run 摘要。OFF 工作台整组 CTest 在最后测试夹具清理前 47 PASS／0 FAIL／1 预期 SKIP，清理后新增案例 3 PASS；独立 Sol medium 只读审查未见确定 P1/P2。双变体最终源码重建已完成，但该重建后的完整 GUI 回归、真实安装窗口视觉及屏幕阅读器还要再验。
- PC-10 当前逐行 JSON 19/30 `QUALIFIED`（轴向科学 8、梁科学 3、板科学 2、三族 RAM/RBM 6），剩余梁 5、板 6 行续跑。科学复制 EXE SHA256 与最终 ON Release 目标完全相同 `B57E381A57E66C0807B2C91ABD7AC34A6679C09715893CC3CE6BB76FF05EB62B`。v43 source/package/admission 静态链仍构建，产品 GUI 与三族 20k 均未运行，12 阶段／36 项仍 pending、`productComplete=false`。

## 2026-10-14 11:36 v43 最终路径回归、ON 实物盘点与 PC-10 续跑

- ON 全量 Release 构建 exit 0，首次全新 ON 安装 exit 0。schema-v2 `build/audit-20261014/piezo-delivery/inventory-v43-on-v2.json` 对 3173 文件、18 项检查给出 PASS；首次因私有 `python.exe`／`pythonw.exe` 未指定供应映射的失败报告保留。该清单仍标 `INCOMPLETE_LINK_MAP_NOT_SUPPLIED`，而且 GUI 操作修补尚待，不能作为最终包。OFF 全量 Release 构建仍进行中。为核对 OFF 的实际静态成员，`src/fea/CMakeLists.txt` 增加 OFF Release `fea-link.map` 选项，待重新配置／链接及独立复核。
- v43 最终长路径元数据护栏之后，`tst_piezo_workbench_widget` CTest 通过，内部 46 PASS、0 FAIL、1 预期 SKIP；`tst_piezo_accepted_trace` 与 `tst_solver_snapshot` 通过。`tst_piezo_result_series` 首次四项目录 CTest 因原 300 秒上限超时，失败日志保留；单测分层恢复 45.366 秒 3/3 PASS。在不删测试的前提下将该目标 CTest 上限改为 900 秒，完整单目标 CTest 1/1 PASS、内部 29 PASS／0 FAIL／1 预期 SKIP，260.92 秒。四项首次合跑仍是 75%，不能计为通过；GUI 操作修补后还须重新构建／回归。
- PC-10 真实 FE／重放目前轴向 8/8、梁 LQR-free 1/8、三族 RAM/RBM 6/6 的逐行 JSON `qualificationStatus=QUALIFIED`，合计 15/30；梁其余 7 行与板 8 行正由 `pc10-v43-smoke/run-remaining-v43.py` 两个进程续跑。报告只涵盖当前复制测试 EXE，最终须核对 EXE SHA 与最终源构建是否相同。PC-11 20k 三族安装版首图／CSV／合并工作集尚未测，静态候选尚未 GO。独立 Sol medium 对 v43 清单转接骨架找到 API pin 绕过和必要 check ID 未核对两项 P2，Luna 正修并补空目录差异拒绝；所有 12 阶段／36 项仍 pending，`productComplete=false`。

## 2026-10-14 10:07 新 MSVC Release 编译与聚焦 GUI 测试

- PC-09 的三份已审 GUI 修正及合并修复已合入正式源码；PC-11 读历史与 GUI 分段计时也已合入作诊断。新 `build/vs2026-piezo-msvc-openmp-v43` 配置、`fea`、`fea-solver`、`tst_gui_model_lifecycle` Release 构建已 exit 0。第一次 lifecycle 链接因测试的 `#define private public` 在 MSVC 下改变三个符号名失败；改为两个 GUI 类显式测试友元，重新增量构建成功。旧失败日志 `build/audit-20261014/piezo-delivery/build-v43-gui-release.log` 和修复后日志 `build-v43-gui-release-friend-resume.log` 都保留。
- MSVC OpenMP 替换已合入 CMake、求解器和 CI；ON 两个 Release EXE 的 PE dependents 均有 `VCOMP140.DLL`，不见旧 `libomp`。安装副本与 Redist 源字节、Qt/TetGen/Agent 许可及真实 GUI 仍待验。初次配置用系统 Python 3.14 漏注册 NumPy/SciPy Stage 9 测试；已以私有 Python v3/`RANGE_REQUIRE_NUMERIC_TESTS=ON` 重配，ON 注册 236 项。OFF 独立 Release 配置同样完成，注册 227 项，尚未建置。
- PC-10 正式测试源已合入 24 科学行及 6 RAM/RBM 持久行收据字段；隔离 verifier 经 Sol medium 二次复审无剩余 P1/P2，root 重跑合成用例 20/20 PASS。真实 C++ 目标、30 行收据和同源数值资格尚未执行。当前 `tst_piezo_workbench_widget` 注册 CTest 在跑，结果未定；新安装、v43 GUI 静态 GO、20k 三家族均未开始。
- 隔离的新 v43 安装清单验证器初版合成 10/10，但 Sol medium 复审连续发现第三方 OpenSSL 无 build peer、dumpbin 期间 EXE 改写、项目 EXE supplier 覆盖与移位伪装；前几项已补，最后一项正修，未用其批准安装。冻结 12 阶段/36 项仍 pending、`productComplete=false`。

## 2026-10-14 08:56 PC-09 合并候选、PC-10 收据审查和 PC-11 归因

- 在 `build/audit-20261014/piezo-gui-integration-sandbox/` 对三份 PC-09 GUI 隔离补丁做同源合并，补齐完整配方确认拦截、预览可展开区、物理 ID 与自动结果关联。Sol medium 合并审查指出接受 trace 未被直接监控、源短暂锁定会丢失 pending 作业；候选已加入紧凑历史权威文件/目录监控、精确作业 ID 的有界重试以及相应 GUI 测试。二次静态复审暂无明确 P1/P2；未合入正式源码、编译或运行。
- PC-10 隔离方案列出 24 科学行、6 持久化行和 15 阶段检查。Sol medium 指出 verifier 容许空数据与自定义空矩阵通过，Luna 正修；现有 9/9 合成测试和 `git apply --check` 不能用于 PC-10 阶段验收。
- PC-11 旧 v33 的 20,001 条记录约 8.195 秒完成认证，首图 8.676 秒，合计峰值 517,238,784 字节，超过 512,000,000 字节门槛且原收据 wrong-family INVALID。热路径计时只存在隔离补丁，不能声称优化有效。Agent-free ON Debug 构建和 GUI v39 外部 runner 可信边界修复正在进行，正式 50 ms step8 未获运行条件。36 项 pending，`productComplete=false`。

## 2026-10-14 08:33 PC-06 线性外力步长细化与 GUI 提案再审

- PC-06 同一个真实四 BEAM1、八步接受外部 `f(t)=0.075+1800t N` 场景增加原网格重放＝真实接受端点，以及按原时间力端点线性插值的 1/2/4/8 倍 FE 步长对 32 倍参考网格的位移/速度比较。粗网格缺陷非空；相邻误差比均小于 0.35，实际约 0.25。最终注释明确该细化共享原生 Newmark、参考网格不是解析真值，不能宣称所有波形或独立解析精度。源修改后旧 v41 Release 目标重新编译，焦点 Qt 3 PASS、0 FAIL；Sol medium 唯读审查细化段无 P1/P2。新 SHA 收据 `build/audit-20261014/piezo-delivery/pc02-pc03-pc06-local-build/receipt-local-refinement.json` 取代同目录旧 PC-06 来源快照，但旧收据保留历史；PC-06 三项仍 pending。
- PC-09 完整配方预览隔离提案的 Sol medium 首审三个 P2（初始电学量单位、标量↔矩阵具体值、直接调用确认绕过）已修，最终 SHA 版本复审无 P1/P2；结果自动关联隔离提案修复了 500 ms GUI 定时器反复 SHA 外部文件问题，Sol medium 最终静态复审无 P1/P2。两者均未合入或运行，且与物理 ID／可展开预览补丁有 `showPreview()` 合并点。完整预览提案有一个非阻断测试不足：其 stale-preview 片段需在修改 steps 前重新生成有效预览；正式合入时修。
- GUI v39 的 28 个活跃 PowerShell 脚本、1877 个命令 AST 与可信 Utility/Management 清单核对 PASS；旧 v42 安装和四项 producer 来源 pin 不适用于之后的新源码／安装，v39 只作诊断。Save ACL 预检 clean/假 Security 路径各 17 案 PASS；manifest/freeze/50 ms step8/最终静态链未结束，产品 NO-GO。Agent-free ON Debug `fea` 仍编译中，OFF 安装完成但不能代替最终新 Release。

## 2026-10-14 08:16 PC-02/03 解析 oracle 与 PC-06 外部时间力局部实跑

- 在旧 v41 Release 树构建新梁/板解析 oracle 和 PC-06 应用测试目标均成功。板 oracle 首跑抛出“Layered native dynamics requires enabled dynamics and consistent mass”；测试场景已与真实梁场景一样启用 Newmark、1e-5 s 步长、8 步及一致质量，重编后梁/板注册 CTest 2/2 PASS。该修复只补场景的求解准入配置，不改独立矩阵/力学断言。
- PC-06 焦点测试首跑指出导出配方电极元数据顺序与网格装配 `portIds` 顺序不同；原错误断言已改为配方 ID 去重/覆盖与实际装配算子列逐个相等两层约束。Sol medium 指出仅做集合比较会漏掉同电压传感端口标签交换，该 P2 已补，最终复审无剩余 P1/P2。重新构建后四个真实 BEAM1、八个接受步、线性时间力、端点反力/电荷/能量账本、源码篡改拒绝与同一步恢复重试焦点 Qt 3 PASS、0 FAIL，约 4.3 秒。SHA 固定输出与 EXE/来源清单在 `build/audit-20261014/piezo-delivery/pc02-pc03-pc06-local-build/receipt-local-final.json`。该例用导出的真 FE 算子检查端点账本，不能代替独立梁算子、全波形、三家族或最终同源 Release 资格。
- Agent-free OFF 新前缀安装 exit 0，ON Debug 构建仍在 `fea`，ON 定向测试和安装待完成。GUI v39 的 PS5.1 模块可信边界独立预检 PASS，manifest/freeze/完整静态链未结束；产品 GUI 仍 NO-GO。PC-09 完整预览隔离提案完成且仅静态适用，结果自动关联提案等待 Sol medium 性能/完整性复审；两个提案都未合入。36 项 pending、`productComplete=false`。

## 2026-10-14 07:49 GUI 全量预览 P1、结果便捷性 P2 与 v38 预检

- PC-09/11 当前源码的 11 个 `workflowPages` 顶层页签含 Piezo control，压电内部另有 6 页；已把逐页对象名和安装版完整故事写入 `docs/evidence/piezo_gui_operation_acceptance_matrix.md`，状态全部仍为待窗口实测。Sol medium 唯读审查确认 P1：集中预览未列出整份 recipe 将提交的端口、支座、阻尼、激励、初始状态等实际变更；确认前必须提供完整可读前后值。P2：Run 后结果图及 CSV 分别要求文件选择器，宜绑定已注册完成任务的认证工件且保留历史显式打开与篡改拒绝。两项仅在独立隔离补丁中准备，尚未改正式来源。全局停止会提示停止所有 solver，Undo 属全局动作；并发/插入编辑的实际操作范围待安装版验证。
- GUI v38 的 ACL 干净/假 Security 优先路径各 17 案 PASS、worker 模块来源与 watchdog 正负例 PASS。source-preflight attempt009 因 v37 摘要残留正确失败，attempt010 修占位与负例后 PASS；首次 freeze 前检因人工抄写旧失败收据内 admission SHA 出错而 fail-closed，原证据保留。Luna 正从不可变收据程序化派生跨文件摘要，重跑来源预检/封存全链；未到 50 ms step8，也未启动产品 GUI。36 项 pending、`productComplete=false`。

## 2026-10-14 07:31 OFF 注册生命周期通过与 PC-06 独立复审

- Agent-free OFF 私有 Python v3 的 `test_extensions.py` 完整直接执行 12/12 PASS（117.690 秒），注册 `tst_gui_model_lifecycle` 15 PASS、0 FAIL、2 SKIP，CTest 50.01 秒、exit 0。直接脚本不等同于 `tst_workflow_extensions` 注册 CTest；后者仍须单独运行。OFF 安装、ON 构建/回归尚未开始，当前为 v38 50 ms GUI supervisor 静态夹具保留低负载窗口。
- PC-06 真实分层梁外部时间函数端点账本新增不中断候选与失败后重试的完整 restart byte 对照，以及从导出 `M/K^E/Ce` 独立重建初始和各端点能量。Sol medium 复审确认原两处 P2 已补，但发现源码改动先触发源 SHA 拒绝，测试错误文案须改为 `external source/dependency SHA256 mismatch; recompile required`；正在做该单行修正，尚未编译/运行。
- GUI v37 Save 判据夹具缺 `Get-Acl` 保持 `STATIC_FAIL/NO-GO`；v38 已从原冻结 150 项 payload 干净重建，正式 isolationRoot 未创建，正补 PowerShell 5.1 Security 模组来源预检，再重新 build/freeze/静态全链。没有产品 GUI 动作，36 项 pending、`productComplete=false`。

## 2026-10-14 07:20 PC-05 双模态解析 ZOH 局部回归与 v37 冻结后测试夹具失败

- `test_piezo_design.py` 新增双模态欠阻尼闭式 ZOH／双输入／加速度直接项独立算例；私有 Python v3 单项通过、同文件完整 20/20 通过，源 SHA `1cadb305b0926eeda7fb1d81fc5af4b3aa49d768e8910ead124915f28862b758`。它不代替真 FE 多模态和安装工件证据，PC-05 仍 pending；详见 `docs/evidence/piezo_pc04_08_gap_audit.md`。
- v37 source-preflight attempt009、watchdog attempt004 重新 PASS 并成功 freeze。PATH-shadow/root-seal 静态步通过，但 save-predicate 测试在关闭 PS 自动加载后调用 `Get-Acl` 时报命令不存在；该调用只在测试夹具，不在产品脚本。v37 冻结树、失败收据保留，未进 supervisor/step8/产品 GUI。待在新 clean sibling 将 ACL 夹具改为经验证的 .NET API 或精确系统模块导入，并重跑全前置链；不能修改 v37 冻结内容。

## 2026-10-14 07:05 OFF 目标完整编译、v37 模块来源实测与旧 workflow 失败归因

- Agent-free OFF `fea.exe` 与 `tst_gui_model_lifecycle.exe` 目标完成增量编译、exit 0；后者 SHA `0D72D4F6A4E8A7383130B209C221EF070C145A0252AA029A2DAE28C319F7C63D`，`fea.exe` SHA `3ED53F5AEF63D9ADF130216B600B49D864C7A52F12D9AD9BA693D757E5B8031C`。前次边界中断 exit 1 与续建 PASS 分开保存；现暂停在测试边界，给 v37 的 50 ms 静态采样留独占窗口。OFF 完整测试/安装和 ON 变体尚未完成。
- v37 在真实 Windows PowerShell 5.1 `-NoProfile` 下执行 build-worker-boundary：系统模块路径与假 Utility 模块优先路径各 exit 0，假模块加载及导出命令副作用均未出现；外部 `worker-boundary-module-trust-attempt003.json` 为 `PASS`。这只是 build 入口；预载 shadow、bootstrap `-VerifyOnly`、全新 freeze/step8/finalizer 尚未完成，产品仍 NO-GO。
- 旧 v39 `tst_workflow_extensions` 的第一次候选验证等候在旧 10 秒 `wait()` 到期，日志只证明该刻工作未终结，不证明 worker 最终失败；验证源未变，当前测试等候改为 45 秒并增加状态诊断，需私有 Python v3 聚焦及完整目标实跑。冻结 36 项仍 pending，`productComplete=false`。
- PC-04/05 唯读审计复核：桥接的模态阻尼预计算请求只传 `damping`，不用用户的 `modeIndices`；先以完整基产生权威 FE `C`，正式初始化再以选模投影同一 `C`。审计初稿所疑“截断基被拿去算全阶 C”不成立，已更正。真正待验的是各阻尼策略在真实 FE 上的共用 C／PSD、全基/截断基误差、多模态 ZOH、非零电荷偏置与安装工件读回；不以静态推断改动生产代码。

## 2026-10-14 06:58 OpenMP 可发布运行时隔离提案复审与 GUI 模块信任边界

- `build/audit-20261014/piezo-delivery/openmp-release-proposal/` 已形成 CMake、solver/test、Windows CI 三份隔离补丁。MSVC `/openmp`/vcomp 的配置探针通过；旧 `-openmp:llvm` 和 `libomp` cache 负例被 fail-closed guard 拒绝。独立 Sol medium 第二轮唯读复审确认先前缓存 P1、CI P2 在提案中关闭，未发现新 P1/P2。六个 live 来源 SHA 仍与提案前像一致，**补丁未合入正式源码**；新 Release 全目标、CTest、安装及 PE import/许可仍待。
- v37 预冻结过程中发现清理后的 PS5.1 wrapper 无法自动解析 `Get-FileHash`；root bootstrap 在自检前使用该函数，16 个包内脚本也有依赖。正在建立精确 `$PSHOME` Utility manifest/PSM1 SHA 门槛和 module shadow 正负例，避免仅在测试 runner 手工导入模块而漏掉产品信任起点。原失败保留，v37 尚未 freeze/step8/产品 GUI。Agent-free OFF lifecycle 增量编译继续，无完整退出码；36 项 pending，`productComplete=false`。

## 2026-10-14 06:41 断点续接、v37 预检及旧 20k 家族协议定位

- 用户继续要求完整开发，明确不使用 Codex with ChatGPT。v37 从 v36 的 123 个冻结 payload 行按字节重建干净候选，原 step8 失败夹具和父版冻结/失败收据保留；正在按 ownership proof schema 分别验证合法短命退出行与完整身份行，尚未冻结或运行正式 step8，产品仍 NO-GO。Agent-free OFF `fea` 因共同私有 Python 默认宏正重编译后段，随后须重跑聚合、OFF 安装及 ON 构建/回归；50 ms supervisor 夹具只在无编译的独占窗口运行。
- 旧 v33 `INVALID/Wrong family` 的首个失败点是验收器逐字比较 manifest `axialRod` 与绘图事件旧内部名 `axial`，并非已证明的 FE 家族数值错误；当前 canonical verifier 24 项轻量测试通过、1 项跳过。旧 8.676s 首图和 517238784-byte 峰值仍独立未达标，梁/板没有旧 GUI 实跑证据。细节已补入 `docs/evidence/piezo_pc09_11_gap_audit.md`。36 项 pending、`productComplete=false`。

## 2026-10-14 06:27 v36 supervisor 夹具脚本失败与 OFF workflow 定位

- v36 从 v35 已核对来源在新 sibling 重建，封存前 watchdog 正负例在外部克隆根 PASS；正式隔离根在冻结前不存在。worker 边界构建、source preflight、freeze PASS，README steps 3–7 各 exit 0，123 项封存 payload 在每步前后哈希一致。step 8 的真实 supervisor 正向夹具已输出 `MEASURED`，但 `test-supervisor-v28.ps1:27` 对 `ownership-proof.json` 每行都直接读取 `.image`；其中一行是采样器按 retained handle、生命周期、无后代核实的合法 `exitedBeforeIdentity=true` 短命 `csc` 子进程，仅有 PID／parent／创建／退出时间而没有 image，PowerShell StrictMode 因缺字段报错。正向测试没有发布 PASS 收据，后续测试、finalizer、产品 GUI 未运行；v36 原封存树、外部错误和 15 个夹具文件保留，结论 **STATIC_FAIL／NO-GO**。新候选须按 proof schema 分别审查已证实退出行和有镜像身份的存活行，不得忽略未知存活者，并重跑全静态链。
- Agent-free OFF recovery 聚合已 PASS 723.64s。lifecycle 注册 timeout 仅该测试提高到 600s 后完整运行 219.075s：14 pass、1 fail、2 skip，唯一失败为 workflow 面板无结果；精确案例指定私有 Python 3.13.5 后 27.668s PASS，确认 build-tree 原默认选中系统 Python 3.14 且缺 SciPy。安装版 `WorkflowProcess` 首选 `bin/../runtime/python/python.exe`，build-tree CMake 正在把已配置的私有 Python 优先写入 `fea`／生命周期测试同一默认宏；重配置后的 `.vcxproj` 均已核对私有路径，正在重建后重跑。OFF 安装／ON 构建仍待。36 项 pending、`productComplete=false`。

## 2026-10-14 06:15 v35 输出根污染与 OFF recovery 通过

- v35 的封存前来源预检、私有 Python verifier 24 项（1 skipped）、PS5.1 parser 34 脚本以及 watchdog 正负契约通过；README／manifest／外部 bootstrap SHA 精确一致。冻结 106 项 payload 长度和 SHA 稳定。正式 README step 3 PATH-shadow 检查正确拒绝已经存在的 `v35-output-01`：封存前 watchdog 夹具误把自身证据写进 manifest 固定的隔离根，导致它不再是新根。没有 step 3 PASS 收据，也未继续 50 ms 夹具／finalizer／产品 GUI。外部 `static-fail-adjudication-v35-step03.json` SHA `4192539390c1c18f8f33070429b426b439a5da76f528e414d7223a5760219fa2` 保存根因与 106 项一致性；v35 **STATIC_FAIL／NO-GO**，原输出和封存树不移动、不清理。Luna 在 clean v36 中把 preflight fixture 证据放到正式隔离根外，并在封存前／step 3 前核对根不存在。
- Agent-free OFF 的 `tst_piezo_solver_recovery` 聚合在临时 `QJsonValueRef` 改为 `int` 快照后完整 PASS，723.64 秒、exit 0；修改前 axial 单例 0xC0000005，修改后 axial/layered 单例均 PASS。`ctest --timeout 600` 不会覆盖 lifecycle 测试已注册的 120 秒属性，第二次仍在 120.08 秒被截断；正在对该聚合项单独提高注册 TIMEOUT、重新配置并完整重跑。OFF workbench、runtime-control、history 已分别通过；OFF 安装与 ON 回归未完成。
- GUI 可用性独立提案只在审计区准备，未改正式来源：物理单元 ID 将显示在杆／梁／板成员表和自动分组预览，集中预览提供有界展开。root 审阅发现少行长文案也可溢出，已要求按实际内容高度触发展开并增加回归；待 Agent-free 释放源码边界后合并、构建与实窗口验证。36 项仍 pending，`productComplete=false`。

## 2026-10-14 05:56 v34 静态失败与 Agent-free OFF 定向回归

- v34 在新冻结树完成来源预检、私有 Python verifier 24 项测试（1 项跳过）、PS5.1 语法检查与静态步骤 1–5；106 个封存 payload 的长度和 SHA 匹配。步骤 6 watchdog 正向夹具失败：测试先在脚本作用域 dot-source 真实 toolchain，却将两个 mock 定义于 `global:`，实际调用被脚本作用域真实函数遮蔽，夹具缺 `toolchain-lock-v28.json`。独立 Sol medium 只读终审还确认 README root 启动示例的 bootstrap SHA `bb0da1dc…` 与外部脚本／manifest 实际 `d012a0e5…` 不同，按文档会在执行前拒绝。两项 P2、无其他明确 P1/P2；v34 **STATIC_FAIL／NO-GO**，保留冻结包和外部失败收据，不运行 step8/finalizer/产品 GUI。Luna 在全新 v35 sibling 修测试作用域、README SHA 与精确一致性检查，然后重做全链。
- Agent-free OFF Debug 重新配置固定私有 Python 后，聚焦五项 CTest 中 workbench 全套 354.10 秒 PASS，runtime-control/history PASS；lifecycle 聚合受其注册的 120 秒 TIMEOUT 中止，但原崩溃单例 `solverPublicationDoesNotOverwriteNewerEdits` 已 PASS，零总量进度现输出有限 1.0。solver-recovery 在 `unavailableExternalFunctionRejectsBeforeQueue:axial` 发生 0xC0000005；原二进制单例已重现。测试源码把临时 `jobsQuery()` 的 `QJsonValueRef` 存为 `auto`，之后比较悬垂代理；正在改成 `int` 快照并重跑 axial/layered/聚合，保留前后收据。OFF 安装和 ON 回归仍待。
- PC-10 24 行三家族算例现有 v44 Release 执行档及测试源已固定 SHA，但完整运行尚未开始；因单目标可耗数小时，等 GUI 时序夹具与变体回归无重负载窗口再跑。MSVC 本机探针确认 `/openmp` 使用 `VCOMP140.DLL`，现有两处 `reduction(max:...)` 报 C7660、`omp_get_thread_limit()` 不声明／不链接，替代 `parallel/single` 和局部最大值加 `critical` 小程序已通过；正式来源改动待 Agent-free 释放边界。36 项仍 pending，`productComplete=false`。

## 2026-10-14 05:42 v33 静态失败与 OFF Debug 修复

- v33 封存后前七项静态检查及 step8 supervisor 夹具通过：46 次采样覆盖 2.250485 秒，最大间隔 50.5996 ms，Job 绑定/排空与故障负例符合预期；后续 pinned Python 静态 harness 在 `test-static-v28.py:613` 用 README 逐字短语而非实际契约断言，因等义措辞不同 exit 1。原始外部 `static-test-failure-001`、冻结树、收据全部保留；v33 **STATIC_FAIL**，没有 finalizer、Sol 最终复审或产品启动。v34 新 sibling 正重做静态 harness 的语义检查与全新封存/全链。
- Agent-free OFF 三个 Debug 目标 `fea`、`fea-solver`、`tst_gui_model_lifecycle` 编译通过，首轮五项聚焦 CTest 只有 runtime-control/history 通过。28 个 workbench 失败共因 OFF CMakeCache 的私有 Python 路径为空；已以 SHA pin 的 `build/private-python-v3` 绝对路径重配并核验编译宏，正重新构建。lifecycle 测试在单元素交叉检查产生 `RProgressPrint(0,0)`，原 `rbl_progress.cpp` 把它变成 NaN 并在日志百分比转整数时触发 Qt Debug QFATAL；root 已在 `src/range-base-lib/src/rbl_progress.cpp` 对零总量报告有限的 1.0，Sol medium 唯读审查无 P1/P2，失败测试重跑待定。solver recovery 首轮未处理异常亦待重配后复测。以上首轮失败收据保留，不冒充通过。

## 2026-10-14 05:26 GUI 可用性与 DXC 来源核查

- 独立 Luna 唯读 GUI 流程检查未发现 P1；源码及测试确认未改动的已保存实验按“Run saved → 原生预览/警告 → 一次 Confirm”提交单个任务。两个 P2 待修：成员表和集中预览未显示实际物理单元 ID；预览表固定最大 120 px，长模态/材料/警告列表难以整体审阅。当前 Agent-free OFF/ON 编译与安装验证仍占有相关源码边界，待其完成后修改并重新构建、测试和实测布局。
- 将 Microsoft 官方 DXC v1.8.2502 发布包和对应 NuGet 1.8.2502.8 包下载至 `build/audit-20261013/piezo-delivery/pc11-dxc-official-v1.8.2502/`，归档流 SHA 对比表明两包的 x64 `dxcompiler.dll`/`dxil.dll` 都与 v42 安装字节不同。`candidate-comparison.json` 保存归档与 DLL 摘要；不能把 v42 字节归因于这两个包，PC-11 来源/通知仍未关闭。详情见 `docs/PIEZO_DEPENDENCY_PROVENANCE.md`。
- 用与 v42 安装版完全同 SHA 的构建 EXE 重放当前 Qt 6.11.1 `windeployqt --dry-run --list source`，清单仅列 D3DCompiler，标准错误称当前找不到 DXC/DXIL。旧 `qtlib` 目录残留文件被整目录安装是一个待验证解释，不能当历史证明；下一次从空目录部署并用真实 GUI/绘图验收检查 DXC/DXIL 是否可安全缺席。
- v33 GUI 门禁新 sibling 已建立；canonical/包内 verifier 字节一致，qualification→worker `acceptedSeq` 正负契约在私有 Python 24/24 测试中通过。新包冻结及 README 前七项静态检查通过，停在需要独占低负载的 50 ms supervisor 夹具之前，没有产品 GUI 动作。Agent-free OFF 生命周期目标继续编译，尚未取得定向 CTest/安装/ON 回归结果；36 项 pending、`productComplete=false`。
- PC-10 独立唯读矩阵核对确认 24 行三家族×四控制器×自由/受迫的科学测试源码存在，但 v42 全量 CTest 在该目标开始后中断，v44 仅有链接证据，不能把 24 行记为已运行。轴向多杆预载的四条 66 步轨迹及八个 RAM/RBM 恢复相位已有独立定向收据，仅覆盖局部 A01。GUI 相关测试使用短的直接原生 trace 或合成 ResultStore，未替代三家族实际安装窗口 20k 及首图/CSV/内存验收。PC-10 A01–A03 仍 pending，见 `docs/evidence/piezo_pc09_11_gap_audit.md`。

## 2026-10-14 05:14 GUI v32 独立审查 P1

- v32 全新种子预检、23 项 packaged verifier 家族测试、worker build/freeze 及 PATH/root seal/17 项 Save 判据通过。step5 外部 runner 初次引用错收据文件名，脚本自身 PASS；纠正 runner 后 exit 0，封存 payload 101 项复核 0 mismatch。独立 Sol medium 随即发现产品链 P1：verifier `qualify()` 检查终端 `acceptedSeq=20000` 却没有在 qualification JSON 返回该字段，worker StrictMode 必读，合格运行也会中止于正常 GUI 关闭前。v32 停止后续清单，保留收据；独立结论 `STATIC_FAIL`，未发现另一个可确认 P1/P2。
- v33 隔离修 canonical `pc11_gui_verify.py`、包内固定副本与测试，要求完整 qualification→worker 键契约正负例；重新 build/freeze、全静态清单、复审、root GO 后才可产品动作。当前仍无新 GUI/20k 资格，36 项 pending、`productComplete=false`。

## 2026-10-14 05:08 v31 种子封存污染与 v32

- v31 在源修订过程中发现种子误带 v30 的 100 行 tree receipt、toolchain lock 与 freeze；修订后的 sampler、verifier、README、manifest 不能由旧哈希认证。v31 原件保留并标为 superseded/invalidated。v32 以不含旧 tree/lock/freeze/最终收据的新 sibling 重建，不继承失效静态结论。
- v31 检查还发现包内旧 GUI verifier 用反向家族映射；当前 verifier 已把 compact axial 映射为 axialRod，并要求 layered 携带 planarBeam/flatPlate modelFamily。当前版本 23 项 verifier 正负测试通过，v32 将固定准确字节/SHA 与来源。产品梁/板 GUI 仍未跑。Agent-free OFF solver Debug build exit 0，GUI target 仍在编译；36 项 pending、`productComplete=false`。

## 2026-10-14 04:51 GUI v30 短命子进程取样竞态

- v30 在新隔离根通过 worker 编译／freeze 和静态 step 3–7；第 8 步 supervisor 正向夹具中，已 pin 的 Python PID 42048 在退出附近发生 `QueryFullProcessImageNameW` 错误 5。sampler 保守报 `INVALID/alive-unreadable pinned identity`，没有 supervisor PASS 收据；44 个样本／最大间隔 50.617 ms 仅为诊断。worker 后续成功终止，terminal token/PID 与 Job 排空正确；不能倒推查询当刻已退出。原始收据保留，产品动作均 false。
- 新 v31 sibling 只对已 pin 子进程身份查询失败做同一轮共享、最多 20 ms 的 retained-handle 退出确认；仍存活／不可读继续失败。补延迟 signaled 正例和永不 signaled 负例，再从全新根跑完整静态链，独立审查及 root GO 仍未开始。Agent-free OFF/ON 配置图通过，OFF build 增量进行中；36 项 pending、`productComplete=false`。

## 2026-10-14 04:36 GUI v29 静态链继续受阻，v30 隔离重跑

- v29 全新 output-09 的 README 前 12 步通过；sampler 两条生产启动路径各 73 参数、精确映像 SHA 与父进程白名单通过。supervisor 正向 47 次／2.301 秒取样，最大间隔 51.314 ms，Job 排空和注入故障均符合预期。第 13 步 pinned Python harness 的 `readme_text.index('Get-FileHash') == -1` 在字串本来不存在时抛 `ValueError`，没有 staticIntegrity 收据；依赖后续步骤未运行。v29 封存包和失败证据保留，新 v30 sibling 修 harness 后从全新输出根重跑完整清单。没有产品 GUI/Save/Run/Confirm/20k 新资格。
- Agent-free 可选构建变体（默认保留 Agent）已经过第二轮 Sol medium 源码复审，无剩余实质发现；目前仅是源码层结论，ON/OFF 新配置、编译、定向测试与独立安装正在进行。12 阶段 36 验收项仍 pending，`productComplete=false`。

## 2026-10-14 03:59 GUI v26 静态失败与新 v29 修复

- v26 全新输出根的静态清单前十步通过（PATH、root seal、17 项 Save 判据、supervisor/worker、native Job、PS parser、finalizer 完整性）；第十一项 Python harness 因变量误名 `finalizer_integrity_receipt` 退出 1，第十二项 finalizer 正确拒绝缺失收据，后四步依赖阻断。失败日志原样留存。
- 独立 Sol medium 唯读复审判 `STATIC_FAIL/NO-GO`：watchdog JSON 返回值被 invoke-full 当对象、GUI sampler 少 Job/SHA 参数、private Python 父进程白名单不能容纳真实 fea/solver→Python 链。前两项已在无产品启动的 PowerShell/Python 路径复现。新隔离 `gui-fresh-run-v29-prep` 修三个 P2 与 harness typo，需重新冻结、全静态回归、独立复审与 root 技术 GO；当前无产品 GUI/Save/Run/Confirm/20k 新资格。

## 2026-10-14 02:48 GUI v19 终态竞态与 v20 修复中

- v18 已把封存树逐档 bytes/SHA、隔离 clone 篡改、完整静态收据和真实外部审查/决策文件核验加入准入链；v19 新根执行时发现 `worker-v28.ps1` 直接写 `terminal.json` 与 supervisor 看到文件存在即读取之间的占用/半写竞态。success/deadline、handshake、native Job 启动和 PS parser 局部通过，sampler-stuck 故障被一般失败遮蔽，不能算全静态通过。
- finalizer 路径篡改负例实际拒绝，但测试脚本对 child stderr 的捕获提前失败；Python static harness 对 map key 引号的字串断言过时。原收据均保留。v20 正改终态临时文件完整写入后发布、严格 token/PID 与有界 sharing 重试，补半写负例并修两项 harness；修后须重新冻结、全清单、独立复审。没有最终 GUI GO，更没有产品 Save/Run/Confirm。

## 2026-10-14 02:23 GUI v17 静态链及独立审查

- v17 PATH/preparation、root seal、17 项 Save predicate、supervisor、worker handshake、native Job 边界及 PowerShell parser 静态夹具通过；正向 sampler 46 次、最大间隔约 50.48 ms，故障夹具各自走预期状态。Python static harness 的 ready-file 顺序切片断言假阳性中断，原始失败、封存链与旧收据均保留；无最终静态收据，更无产品 GUI/Save/Run/Confirm。
- 独立 Sol medium 只读复审指出：篡改测试原地修改封存文件而 finalizer 未全树复核；bootstrap-test-pending 初步收据可被启动门槛接受；Python 优化模式可跳过 assert；外部审查与 root 决策哈希没有绑定实际文件内容/状态。Luna 正一次性修 v18，完成后仍需重新冻结、全清单和独立复审。
- v33 旧 INVALID axial 20k 的首图 8.676 秒中，记录认证到 8.195 秒、数据就绪到 8.652 秒、画布绘制约 24 ms；峰值 517238784 bytes 发生在确认后、求解器启动前，不能归咎于绘图。详细源码路径与测量边界已记入 `docs/evidence/piezo_pc09_11_gap_audit.md`。

## 2026-10-14 01:50 v44 构建与 GUI v11 静态阻断

- PC-10 反力测试增量之后，`build/audit-20261014/piezo-delivery/root-release-build-v44/build.log` 的全目标 Release 构建退出 0；目前只有日志/退出码，没有完整同源冻结收据，234 项 CTest 未完成。PC-00 旧格式、PC-08 provider 重试和 PC-10 四杆案例仍只是局部证据，36 项保持 pending。
- GUI v28 静态 v11 已通过 PATH-shadow、root-seal、Save 判据、parser、worker handshake；native-launch 失败注入出现 Job 指派前 direct child=1，未取得该瞬态子进程身份，失败收据与旧 PASS 隔离保存。v12 正补 PID/映像/哈希/创建时间和父进程寿命取证并准备重新封存；尚无最终静态收据/独立审查/root GO，更无产品 GUI、Save、Run、Confirm 或三家族 20k。详情及续接顺序见 `docs/PIEZO_CONTROL_CONTINUATION_HANDOFF.md` 顶部。
- PC-11 私有同机安装的许可盘点发现 `libomp` 再分发、D3D/DXC 精确来源、`range-ai-lib` 授权及最终 Qt/TetGen 映射缺口；已向用户询问 Agent 的交付取舍，在答复前不假定选择。用户要求继续开发且不使用 Codex with ChatGPT。

## 2026-10-14 00:54 PC-10 四杆 PZT 预应力代表案例局部验证

- 既有 `tst_piezo_pzt_prestress.cpp` 的四根非平行 TRUSS1、四个 PZT 端口及非零 q0/V0/Q0/f0 已覆盖多杆预应力基础案例；新增按独立夹具成员内力和外载计算 13 个受约束 DOF 的 `reactions0`，核对两个自由 DOF 零反力与整体力平衡。最大反力误差及合力残差均约 `1.07e-14 N`，受约束反力范数 `11.12 N`。
- Release 聚焦 LQR-free 通过，完整注册 CTest 1/1（227.96 秒），Qt 14/14；四个 LQR/MPC 自由／受迫轨迹各 66 个接受 FE 步且证据状态 `QUALIFIED`，八个 RAM/RBM 相位行执行。`build/audit-20261014/pc10-prestress-multirod/receipt.json` 的 17 个工件哈希与 provider 前后清单匹配；独立 Sol medium 审查无 P1/P2。收据未固定全部链接库或整个未提交源码，仍为 PC-10-A01 局部证据，PC-10 及 36 项均 pending。

## 2026-10-14 00:42 PC-08 真 provider 重试及 GUI 准入进度

- 轴向 sensor-only MPC/RAM N=2 真 FE 的 `postFE` 候选先由实际 Python provider 计算，再单次抛错；接受态 codec 与 MODEL 序列化字节未变，重试及最终字节与无故障基线相同。聚焦 3/3、完整注册目标 1/1（364.15 秒）。v2 收据 `build/audit-20261014/pc08-true-provider-retry-v2/receipt.json` 固定 18 个工件，实际 Python 解释器、93 文件 provider 包、2469 文件私有运行时及前后相同清单；独立 Sol medium 复审无 P1/P2。只覆盖此轴向切片，PC-08 仍 pending。
- 当前源码 Release 全目标构建 `build/audit-20261014/piezo-delivery/root-release-build-v43/build.log` 退出 0；PC-10 四杆非零 PZT 预应力既有目标正补独立支座反力检查并定向验证。GUI v28 夹具发现 Job 分配前 `conhost.exe` 逸出，失败收据保留，改为暂停建进程、先归属 Job 再恢复；尚未静态封存/独立复审，也未启动产品 GUI。最终 234 项同源 CTest 和三家族实际 20k 仍待执行，36 项 pending、`productComplete=false`。

## 2026-10-13 23:58 PC-05 三家族真 FE/ROM 导出局部验证

- 新 `actualMatrixPackageFEAndRom` 三行分别通过轴向杆、分层梁、平板真 FE 原生算子核对 `M/K/C/G/Ce/B`、矩阵包 JSON 写入回读、源/配置/DOF/单位与直接项；一个质量归一模态的连续/精确 ZOH 用独立阻尼振子闭式解验证。
- Release 聚焦 5/5，注册 CTest 1/1，内部 Qt 27/0/1（原有符号链接权限跳过）；`build/audit-20261013/pc05-fe-rom-export/receipt.json` 七个哈希匹配。独立 Sol medium 审查无 P1/P2。只作为 A01/A02 局部证据，多模态 ZOH、独立单元物理 oracle 与 A03 非零工作点读回仍待完成；PC-05 pending。

## 2026-10-13 23:45 PC-06 七波形真 FE 桥接局部验证

- 七种内置时间波形在真轴向 FE 同一力通道的各区间左右端点按独立公式核对；实际接受 q/v/a、三步控制保持、功/耗散/能量与直接原生 FE 试算一致。中途改变 `kp` 导致新 `prepare()` 拒绝且原接受态字节未变；首版末步负例已被修订取代。
- v2 七行聚焦通过、完整注册目标 CTest 1/1（255.06 秒），`build/audit-20261013/pc06-axial-waveforms/receipt-v2.md` 七项 SHA 核对通过；独立 Sol medium 审查无 P1/P2。仍只验证单个 X 通道/六区间集成，不覆盖全部外部文件失效、波形边界或独立求解器精度，PC-06 pending。

## 2026-10-13 23:41 GUI v27 静态审查失败与 v28 修复边界

- v27 final static receipt 已冻结，31 文件链、安装 3167/provider 49/原始权威 10、PATH 假冒与篡改负测通过；但独立 Sol medium 判 STATIC FAIL、产品 GO NO-GO。六项 P2 涉 seal 字段、监督证据目录、结果目录、GUI 进程白名单、Job 归属前启动 Python，以及 bootstrap 先执行后鉴别。没有运行产品 GUI/Save/Run/Confirm/绘图/CSV。
- 仅在新 `build/audit-20261013/piezo-delivery/gui-fresh-run-v28-prep/` 修订并测试有效 seal、进程归属和外部哈希核验；v27 不改写。最终 20k 三家族资格与 5s/30s/512000000 bytes 门槛仍待实测。

## 2026-10-13 23:33 PC-04 真 FE 近重模态簇局部证据

- 两个独立支承的相同分层 BEAM1 悬臂组成八元素真 FE，现行 C++→私有 Python 设计后端产生近重簇 `[0,1]`。完整基独立残差 `8.85e-16`、质量正交 `3.33e-15`；选中 24×2 基残差 `5.24e-17`、正交 `2.22e-16`。合法整簇选择保留相同物理 M/K/P/T 与映射，非法拆簇拒绝。
- 首版定向测试虽然通过，独立 Sol medium 审查发现未验证有效选择确实收缩结果。v2 补 `selectedModes`/Phi/omega 及选中基独立验算，Release 定向 QTest 3/3，二次审查无实质 P1/P2；收据 `build/audit-20261013/pc04-modal-cluster/receipt-v2.json`。首版 receipt 的共享执行档路径已被 v2 重建覆盖，仅作历史记录；PC-04 仍 pending，完整目标及最终源冻结回归未执行。

## 2026-10-13 23:18 PC-03 非零工作点、仿射支座与传感桥接

- 新偏心 BEAM1 真 FE 案例在同一场景验证非零 q0/V0/Q0/f0、`p=0.001` 仿射约束、支反力、fullK 有限差分、自由 DOF 力平衡、电荷方程和接受步传感读出；v3 Release 注册目标 40/40 Qt。`build/audit-20261013/pc03-a01-v3/` 的八个哈希已核对，独立 Sol medium 审查无 P1/P2。生产组装与电学参考共享，故只认定局部一致性证据，PC-03 不结项。
- PC-04 正准备真实 FE 的近重模态簇/整簇选模/拆簇拒绝测试。选模变更 `designConfiguration` 会依法改变完整 source fingerprint；比较同物理 FE 的 M/K，而分别核对每次导出指纹。

## 2026-10-13 23:06 压电交付续作：平板收敛、PC-07 修复及 GUI 准入

- PC-02 真平板 2/4/8/16 网格对独立简支 Mindlin–Navier 参考的 1024 内点 L2 位移误差为 47.45%→12.88%→3.325%→0.878%；连续电极解析 Ce/Q 对各网格一致。Release 目标 9/9 Qt、CTest 1/1，独立 Sol medium 审查无实质 P1/P2；`build/audit-20261013/pc02-plate-mesh-refinement/receipt.json` 固定源、执行档和测试日志。该证据是 PC-02-A03 局部切片，未关闭阶段。
- PC-07 先版真轴向 FE 五控制器测试的单采样/饱和 P2 已修订为两采样独立标量 oracle 与明确限幅、状态、回滚断言。Release 目标 CTest 1/1、Qt 33/33，最终收据 `build/audit-20261013/pc07-basic-fe/receipt-v2.json`；独立 Sol medium 复审无新 P1/P2，仍仅为 PC-07-A01 局部证据。
- v26 GUI Save/Run 静态包独立审查通过，实际 GO 因 PATH `python` 和 admission verifier 冻结链 P2 持续禁止；v27 只在新审计目录静态修复，未运行实际 GUI 或三家族 20k。PC-03 非零工作点/传感桥接继续开发。完整 234 项 CTest 旧批中断，等待最终冻结源重新构建/全跑。用户明确不使用 Codex with ChatGPT，36 项仍 pending、`productComplete=false`。

## 2026-10-13 21:35 压电交付续作：板连续电极与旧完整回归中断

- 用户要求继续冻结 `PIEZO_CONTROL_DELIVERY`，明确不使用 Codex with ChatGPT。12 阶段/36 项状态尚未结项，`productComplete=false`；三家族私有安装真实 20k GUI 验收均未完成。
- PC-02 两 Q4 共享电极真 FE 测试覆盖电荷/开路电压汇总、磁盘重启及非法端口拒绝，Release 完整目标 34/34 Qt 用例通过。独立 Sol medium 审查无实质 P1/P2，但每单元 Ce/G 参考仍使用生产 MITC4 内核，不能用该测试单独关闭 PC-02-A03。梁 2/4/8 网格细化证据及审查另见 `build/audit-20261013/pc02-a03/receipt.json`。
- 234 项 CTest v42 前 113 项通过，第 114 项长时计算中因后续 PC-01/02/03 测试源码变化而中断；自有 CTest 和子进程退出，原日志和 `full-ctest-v42/INTERRUPTED.md` 保留，不宣称完整回归通过。源冻结后须重新构建和完整重跑。
- v25b GUI 静态监督包已通过独立审查；v26 实际 Save/Run 集成仍为静态 NO-GO。未启动新安装的 GUI Save/Run/Confirm/20k 性能故事。PC-03 偏心质量测试及 PC-04–08 缺口核查正在进行。

## 2026-10-13 16:19 v8 原工作区项目清单变更与恢复

- v7 被独立 Sol medium 审查阻止执行：UIA 的 ID/Name OR 查找对共用工具列/页签 ID 产生歧义。v8 修正并经 no-Go/独立审查后仅执行一次唯读诊断；实际选中工作区及原生模型文件，但产品打开模型在原 v33 工作区 `project.json` 新增 draft，原权威 guard FAIL；首图/CSV 均未开始，GUI 以精确 PID 正常退出。
- 保留 v8 失败、原始 guard 和改写后项目文件字节。通过只移除新增 draft 文本重建原 2200 bytes，SHA 严格匹配原冻结值 `725dbe...` 后，以带备份 `File.Replace` 恢复；恢复收据、变更版证据和备份均在 v8 输出目录。恢复后 91 个 pin 零不符。v9 改为经全字节核对的工作区/profile 副本诊断，未执行。
- v40 lazy SDK 镜像静态资料虽齐，Sol reviewer 因共用 build、CTest 定义未全 pin、import receipt 可缺而给 NO-GO。v41 静态包维持 `PREPARED_BLOCKED_BY_ISOLATED_BUILD`；Root 隔离 CMake 配置已完成，Release build 正进行。无 47 项新测或产品完整验收结果。

## 2026-10-13 15:38 压电续作：v40 workflow、v5/v6 GUI 唯读诊断

- 用户最新指示不使用 Codex with ChatGPT，继续冻结 `PIEZO_CONTROL_DELIVERY`。正式 12 阶段/36 项仍 pending，`productComplete=false`。
- v39 workflow mirror 旧 baseline 第 27 项失败保留。定位为 extension 测试 10s 等待短于私有运行时验证＋子进程实际耗时；另发现 job 终态可查询而写入锁尚未释放的竞态。修正服务锁释放顺序、测试等待与夹具释放、CTest 上限。最终测试源码下私有 Python v2 全 10/10 通过，159.984s；新 47 项镜像仍待重冻结/重跑。旧 221 项完整性数字缺独立可重现命令和收据，暂不作为验证事实。
- v5 owned Qt picker 可用，但已浮动 control dock 导致硬编码 Float 控件失效；v6 解决 dock 状态分支后，发现结果控件在原生模型打开前不存在。两次诊断均有失败 receipt、原权威 guard PASS、精确 PID 正常 Quit，没有真实首图/CSV 请求或产品资格。v7 正按先开模型后读结果准备；所有旧审计输出保持不变。

本文件记录项目阶段执行中的关键决策、用户反馈、异常、纠正和验证结果。记录按时间追加；详细命令和代码差异以 Git 提交为准。

## 2026-09-26 Stage 8 持久身份与暂态监测事务

- 模型 `7c11dc5` 将模型内物理节点/元素 ID 保存为 1.3.17 ASCII/binary；旧版读入时确定性补齐。增删实体与文件往返测试通过。审查初版分配会在 `setNNodes` 和旧版读入时产生平方级扫描，已改为单调分配并严格校验下一 ID；聚焦 Release 模型测试 1/1 通过。尚不提供重网格对应映射。
- 求解器 `faae18e` 让耦合场快照直接读取模型 ID，按 ID 查找与 ID 变化拒绝测试通过；`tst_structural_temperature_field` 聚焦 Release 1/1 通过。求解器 `94dc3b6` 对精确暂态两叶 Heat→Stress 增加文件监测点暂存、原子追加及发布失败回滚；已尝试写入却无记录为失败，多文件部分发布与同求解器重试聚焦测试 1/1 通过。
- 根 `70f4783` 合入子模块指针。前一全目标 Release CTest 72/72 不含本增量；仍须重新全量构建/回归与独立审查，再评估 Stage 8 全部交付。

## 2026-09-26 Stage 8 物理 ID 耦合场快照

- 全目标 Release 构建完成，`ctest --test-dir build/vs2026 -C Release -j 2 --timeout 180 --output-on-failure` 72/72 通过。该验证覆盖本节新增的受限两向杆件 FE、暂态两叶事务、参考热容量及 CLI 发布锁；监测点文件输出事务、跨重网格身份与通用耦合方程仍未交付，不能视为 Stage 8 整体验收。
- 新增调用方物理节点/单元 ID 绑定的同网格快照，能按 ID 稀疏查询节点温度及可用运动量、单元热流/热源和实际求解器写出的六分量应力；缺失字段不补零，连续接受时刻可求温度变化率。精确网格与有向连接指纹允许存储重排、拒绝拓扑/几何变更。只覆盖调用方能保证 ID 稳定的同一物理网格，不声称跨重网格映射。
- 独立只读审查发现初版误读求解器不写入的聚合应力变量；已改为读取六个实际分量并修订测试。Release 聚焦目标构建及 `tst_structural_temperature_field` 通过。Stage 8 仍在进行。
- CLI `--thermal-steps=N --thermal-checkpoint=PATH` 已完成精确 Heat→Stress 受限模型的原子 `.tfec` 发布与恢复；进程测试确认完整/分段运行一致，损坏拒绝、失败保留旧快照、控制模式冲突拒绝。审查后把逐步接受审计延后到 checkpoint 发布成功，并修正未指定收敛文件时误在工作目录生成 `.-mxs-CG`；Release 聚焦 CLI 目标与 CTest 通过。其余 Stage 8 门槛未因此自动完成。
- 求解器 `840fd7c` 新增受限真实 RModel TRUSS1 双向热机适配器：显式每杆 Kelvin–Voigt 黏性系数，耗散回热、候选原子发布及时间/六应力分量同步；拒绝未映射边界/初值及非中性环境。独立审查促成任务顺序、时钟、旧汇总应力和条件拒绝修复。实际 FE 的 0.02 J 耗散/热增量、连续步与失败不发布，以及独立同时标量根对分区迭代的核对，Release 聚焦 CTest 2/2 通过。只验证受限一维方程，不是通用单体 FE 或完整热力学第一定律。
- 独立 CLI 审查指出并行写同一 `.tfec` 和百万步审计暂存风险；根 `c15d08f` 加入跨进程锁、发版前原始目标哈希核对及 1000 宏步上限。聚焦测试初次失败，经临时调查记录定位为失败路径審計 `.lock` 未及时释放；显式释放后完整 CLI CTest 1/1 通过。发版后的审计故障现在明确提示 checkpoint 可能已提交，不要求盲目重试。
- 求解器 `1efa357` 把精确单次 Heat→Stress 两叶任务扩至受限暂态逐步事务：两子求解成功后单次结果发布，失败恢复接受时间/索引、模型/共享数据/执行计数与已有结果文件，同一求解器重试。独立审查后加入跨进程输出锁、回滚失败保留备份、计划内旧未来记录和监测文件输出早期拒绝、六位步号扫描。解析两步及失败/重试等 QtTest 16/16 通过；无通用任务树或带监测文件事务声明。调查过程见已忽略的 `build/audit-20260926/transient-heat-stress-investigation.md`。
- 求解器 `8200b07` 增加显式启用的表面/体元素参考热容量：温度暂态在当前导热几何上求解，但以 `X=x-u` 的参考 Jacobian 装配容量；串行预检几何以避免并行循环内退化矩阵异常。QUAD1、QUAD2、TETRA1 变形脉冲的参考加权能量均回到 1 J，默认当前容量对照为 1/4、1/8；退化参考面拒绝且温度保持接受态。Release 聚焦 CTest 1/1 通过，独立只读审查未发现实质问题；扭曲高阶及反向体仍待专门验收。

## 2026-09-26 Stage 8 受限真实 FE 增量

- 求解器 `e464cbd` 接入拉索 Newmark 动力 FE、受限 Heat→节点电势压电杆 FE，以及点热源驱动的瞬态热 FE/宏步末准静力应力 FE；分别增加解析、失效回滚及继续计算对照。
- 查出旧 `POINT` 热单元没有积分点，导致点热源和点热容装配为零；改用一节点直接装配，并通过旧 `HEAT` 边界条件回归。热—压电采用同一个积分自由热应变定义机械应力与电位移，不引入未经配置的独立热释电参数。
- 全目标 Release 建置成功，CTest 70/70 通过；本轮增量不是 Stage 8 整体验收。暂态跨任务事务、双向热回馈、持久化配置/CLI 重启与跨重网格身份仍未交付。
- 后续审查发现每宏步新建热求解器时，后续步以非首次执行进入可能不更新几何尺度；改为每次重算尺度而只对真正首步重置温度。增加正体积 `POINT` 热容解析测试及二进制 `RModel` 磁盘往返后加载单独调度器快照的确定性续算对照。全目标 Release 重建成功；`ctest -j 4` 中 CLI 审计测试一次失败且输出未给出断言，单独重跑通过，`ctest -j 2 --timeout 180` 全套 70/70 通过。并行条件偶发失败未定位，不能把一次重跑当成已修复该问题。
- 加入受限热控 FE 的单文件 checkpoint：二进制模型及版本化调度器状态以 SHA-256 校验，`QSaveFile` 原子替换；恢复验证文件、状态和网格后才替换调用方输出。测试包含第三步磁盘恢复对照、单字节损坏拒绝及失败时保留原输出。全目标 Release 建置和 `ctest -j 2 --timeout 180` 70/70 通过；该格式尚未进入通用 CLI 或 `RProblemSetup`。
- 两个真实热 FE 子步对一个准静力机械宏步的解析基准通过：2 W 加热、每子步 1 s 得节点温度 294.65 K、按杆件平均温度得端位移 0.0075 m、施加热能 4 J。首版测试误以端点温升代替单元平均温升，已纠正测试期望，未更改求解逻辑。
- 模型 1.3.16 新增有版本门控的热控采样周期、加热器/传感器绑定与功率设置，模型子模块 `e47f00d` 的 ASCII/binary 往返及 1.3.15 兼容测试通过。求解器 `schedulerFromModel()` 在磁盘模型往返后驱动实际 1D FE。审查热质容时发现旧热解在变形杆件上按当前长度积分质量，会在参考密度不变时制造热质量；受限热控路由现仅对 TRUSS1 用参考长度修正质量，导热仍依前一接受位移后的当前长度；更新的冷却解析基准通过。通用非热控热解保持既有行为，尚未扩展到二维/三维热弹性质量守恒。
- 独立审查指出热控 checkpoint 未绑定接受时间、外部调度器可能与模型内配置不同、写入/读取文件大小限制不对称。求解器 `2e1a9d2` 增加控制状态 v2 的接受时间（兼读 v1 零起点快照）、模型时钟核对、模型设置指纹核对、对称 1 GiB 限制，并以模型内设置直接恢复 checkpoint；时间失配、设置失配及磁盘继续计算均有聚焦测试。首轮将设置核对误放入 scheduler 构造形成递归崩溃，定位后移至 checkpoint 写入，聚焦测试通过；这属于已纠正的整合错误，尚待本轮最终全套回归。

## 2026-09-25 Stage 7/8 续作与 Qt 弹窗纠正

- 后续增量：稳态 Heat→Stress 顺序任务在整任务收敛后单次发布结果；故障注入覆盖 Stress 解算失败、最终结果写入失败、同一求解器修复重试和暂态任务在旧记录清理前拒绝。1.3.15 节点压电 ASCII/binary CLI 往返均通过，ASCII 输出按现有八位有效数字精度检查。
- 热弯曲验证经历两项夹具问题：角点重复 POINT 组使温度留在 293.15 K；材料属性构造时的 293.15 K 零值点与只在 350 K 写入的常数共同形成虚假的温度依赖。修正后 QUAD2 细网格曲率误差 0.695%、端位移误差 0.490%，粗到细呈收敛趋势；完整调查在 `build/audit-20260925/thermal-bending-investigation.md`，未以放宽验收界限掩盖问题。

- 用户要求继续至 Stage 8 并审查；新增 1.3.15 节点电势/导体电荷模型契约和一维 FE adapter、拉索非线性动力候选步，以及热求解认证、实际 Heat→Stress 与温度相关模态 FE 基准、独立热机/热控制候选步。Stage 7/8 整体验收尚未完成，具体缺口见 `HANDOFF.md`。
- 用户提供 `tst_structural_heat_certification` 的 Qt 平台插件弹窗。原因是新测试初始使用 Qt GUI 测试入口；改用 `QTEST_APPLESS_MAIN` 并重新构建后，设置不存在的 `QT_QPA_PLATFORM` 单独运行仍退出码 0，确认测试不需图形平台插件。
- 独立审查发现热 CG 新鲜残差认证把严格容差放宽到 1e-8、热控制正最小功率导致合法关闭态失效；两项已修复并回归。审查还指出跨 Heat→Stress 任务缺统一回滚事务，保留为 Stage 8 验收门槛。
- 全目标 Release 构建成功；修复后 CTest 69/69 通过。现有 `.agents/`、`.codex/`、`AGENTS.md` 未跟踪配置保持原状；日志留在已忽略的 `build/`。

## 2026-09-20 Stage 0 交付闭环

- 用户目标：执行并纠错完成 Stage 0、1、2；Stage 3 网格生成优先采用成熟工具；保留原有非结构功能，但不把它们作为本轮开发任务。
- 打包问题：Release 安装树最初未携带 `libomp140.x86_64.dll`。CMake 现会定位 MSVC LLVM OpenMP runtime，缺失时配置失败，并将其安装到 `bin`。
- 测试事故：首次直接运行 Release CTest 时，测试执行文件旁缺少 Qt runtime，Windows 连续显示“找不到 Qt6Core.dll”。用户报告后立即终止 CTest；随后将 `windeployqt` 输出和 `Qt6Test.dll` 部署到测试目录，并为 CTest 增加逐测试超时。
- 纠正结果：`tst_structural_product_profile` 单测通过；Release 全量 CTest 为 28/28 通过，总耗时约 9.74 秒，无错误弹窗。
- 安装树验证：`Qt6Core.dll`、MSVC runtime 和 `libomp140.x86_64.dll` 均存在；从独立安装目录执行 `fea-solver --version` 成功，版本为 1.3.0。
- 交付物：生成 `range-fea-1.3.0-winnt-amd64.zip` 和 Inno Setup `range-fea-1.3.0-winnt-amd64.exe`。
- 环境补充：通过 winget 安装当前用户范围的 Inno Setup 6.7.3，并让 CMake 同时发现系统级与当前用户级安装位置。
- 安装器验证：正式安装器静默安装、CLI 启动和静默卸载均成功。
- GUI 验证：首次启动出现 `Crash report - Range FEA`，确认原因是此前强制终止 GUI 留下的 `range-fea.lck`，锁内 PID 已不存在。该锁已移入忽略的构建验证目录备份。随后从独立 install-tree 启动，主窗口标题为 `Range FEA :: Finite Element Analysis - [Beam] - Range FEA`，界面响应正常，并通过窗口关闭消息正常退出，退出码为 0，锁文件被正常清除。
- 未伪造结论：当前主机没有可直接使用的 Windows Sandbox，干净 Windows 虚拟机终验仍是发布前检查项；本轮以完整 runtime 清单和独立安装树启动作为可执行替代验证。

## 2026-09-20 Stage 1 能力注册与范围纠正

- 纠正 structural-only 草案：移除 GUI、文件打开、材料、结果与报告路径中的全局结构过滤，默认保留原有非结构功能。
- 求解策略：`RSolver` 默认使用 `AllRegisteredSolvers`；`StructuralProduct` 变为显式可选策略，并在选用时拒绝不支持的问题类型、旧一阶瞬态结构任务和无结构公式单元。
- 中央 registry：新增八个能力域，统一记录 registered、implemented、tested、validated 和 profile exposure；提供版本化 JSON 导出/导入，供 GUI 与后续 Agent 查询。
- 单一事实来源：structural profile 的分析、单元、材料和约束判断改为查询中央 registry。
- 通用检查：GUI 求解前明确报告“未配置问题任务”，不再依赖结构产品门禁提供该错误。
- 验证：聚焦测试 3/3 通过；Release 全量构建通过；CTest 28/28 通过，总耗时约 9.06 秒。
- 子模块提交：`range-model-lib` 为 `bf4a825`；`range-solver-lib` 为 `6722d36`。

## 2026-09-20 Stage 2 自由度与单元契约基础

- 新增通用 DOF registry：`UX/UY/UZ/RX/RY/RZ`、电势、温度和压力均有稳定 ID、名称、单位、物理场和分量元数据。
- 新增 ElementDescriptor：覆盖全部现有单元枚举，声明拓扑、阶次、节点数、自然维数、每节点 DOF、候选公式、默认积分点、一致/集中质量能力、材料属性、载荷和结果。
- 梁单元契约明确要求 6 DOF；连续体单元保持 3 平移 DOF；TRI/QUAD 显式列出平面应力、平面应变和轴对称目标公式。
- descriptor JSON 嵌入 capability registry 元数据，后续 GUI 和 Agent 可直接查询，而不需要解析求解器分支。
- 验证：新增 `tst_element_descriptor`；Release 全量 CTest 从 28 项增加到 29 项，29/29 通过，总耗时约 8.63 秒。
- 子模块提交：`range-model-lib` 为 `2caf682`。

## 2026-09-20 Stage 2 WEDGE6/HEX8 体单元闭环

- `range-model-lib` 新增 WEDGE6 的三角形 3 点 × 轴向 2 点积分，以及 HEX8 的 `2×2×2` Gauss 积分；两类单元均提供形函数和自然坐标导数。
- 补齐 WEDGE6/HEX8 的数值体积计算和外表面生成：WEDGE6 生成 2 个三角面与 3 个四边形面，HEX8 生成 6 个四边形面。
- 新增 `tst_element_volume`，验证分片一致性、导数和为零、积分总权重、一致质量矩阵对称性、质量向量、Jacobian、仿射线性场梯度再现、体积和边界面拓扑。
- 复用 `RSolverStress` 的通用三维实体装配，以 30 段轴向杆分别验证 WEDGE6 与 HEX8 的端位移、均匀轴向应力和支座反力。
- 测试纠正：两个 WEDGE6 拼成方形截面时，四个端点的等分集中力不是一致等效载荷；改为按三角形分摊面积使用 `1/6, 1/3, 1/3, 1/6` 后，两类单元均精确再现解析轴向解。没有通过放宽容差掩盖该问题。
- capability registry 已将 WEDGE6/HEX8 标记为结构公式 implemented、tested、validated，并暴露给 structural profile。
- 验证：三个聚焦测试 3/3 通过；Release 全量 CTest 30/30 通过，总耗时约 12.21 秒。
- 构建说明：受影响的三个测试目标均成功完成 Release 重建。`ALL_BUILD` 在未修改的 GUI `fea.vcxproj` 巨型单批次编译阶段超过 10 分钟工具上限后被终止；该命令把约百个 GUI 源文件交给单个无 `/MP` 的 `cl.exe`，属于现有构建性能问题，不是编译错误。
- 子模块提交：`range-model-lib` 为 `f381f2f`；`range-solver-lib` 为 `88ff9d3`。

## 2026-09-21 Stage 2 TRI3/QUAD4 二维公式闭环

- 模型状态：`RSurface` 新增持久化的 `PlaneStress`、`PlaneStrain`、`Axisymmetric` 结构公式选择；默认保持 `PlaneStrain` 以兼容旧模型。模型文件版本从 1.3.0 升至 1.3.1，旧文件读取时采用兼容默认值。
- 持久化纠错：新增 ASCII/二进制模型往返测试。定位并修正 Qt 6 文本流读取布尔值时消费字段分隔空格的问题，同时初始化 `RCondition` 原先未初始化的适用范围字段，消除默认环境条件序列化中的未定义行为。
- 坐标变换纠错：线/面单元的局部—全局变换矩阵改为节点块对角形式并显式清零；桁架装配和恢复同步改用各节点自己的局部列。新增单元级正交性与非本节点块为零的测试。
- 二维构成：`RSolverStress` 按曲面组选择平面应力或平面应变矩阵；轴对称作为可持久化契约保留，但求解器会在装配前明确报告尚未实现，不会静默采用错误公式。
- 曲面装配纠错：刚度现在包含厚度并按所有积分点累加；QUAD4 质量不再重复累加既有积分贡献；热等效节点力只使用两个法向热应变分量，并在局部向量完整形成后统一变换到全局坐标。
- 结果恢复纠错：热应力按 `D * [alpha*dT, alpha*dT, 0]` 每积分点扣除，不再混入厚度，也不在节点循环中重复扣除。
- 验证：TRI3/QUAD4 × 平面应力/平面应变共四组多单元条带解析测试通过，覆盖端位移、`sigma_x`、`sigma_y` 和支座反力；QUAD4 两种公式的自由热膨胀与约束横向热应力测试通过；轴对称拒绝诊断通过；原有静力/模态桁架回归通过。
- 全量验证：Release CTest 31/31 通过，总耗时 12.39 秒，无 Qt DLL 弹窗。
- 子模块提交：`range-model-lib` 为 `dfa284d`；`range-solver-lib` 为 `40b08cd`。

## 2026-09-21 Stage 2 结构质量矩阵选择闭环

- 模型配置：`RProblemSetup` 新增 `RMassMatrixType::Consistent/Lumped`，默认一致质量以兼容旧模型；模型文件版本从 1.3.1 升至 1.3.2，ASCII/二进制文件均持久化该选项，旧文件读取时保持一致质量默认值。
- 装配实现：`RSolverStress` 在线、面、体单元质量装配中统一读取模型选择。面单元在局部二维质量矩阵上集中化后再变换到全局；体单元对完整一致质量矩阵执行行和集中化。
- 线单元纠错：原实现只保留局部轴向方向余弦平方对应的全局对角项，遗漏不同全局分量间的耦合项。现改为 `R_t M_local R_t^T` 完整变换；集中质量先在局部节点矩阵上形成，再执行同一变换。
- 模型验证：新增 `tst_problem_setup`，覆盖默认值、复制、非法枚举拒绝以及一致/集中质量的 ASCII/二进制模型往返。
- 求解验证：固定—自由杆解析模态覆盖 TRUSS1 与 TETRA1 的一致/集中质量；TRI3、QUAD4 平面应力条带同时覆盖两种质量矩阵。一致质量频率位于解析值上方、集中质量位于解析值下方，且各算例误差均小于 2%。
- 全量验证：Release CTest 32/32 通过，总耗时 21.16 秒；无 Qt DLL 弹窗或测试超时。受影响目标构建成功，仍仅有既知的 `pwsh.exe is not recognized` post-build 警告。
- 子模块提交：`range-model-lib` 为 `b3bfb0f`；`range-solver-lib` 为 `049d7a8`。

## 2026-09-21 Stage 2 TRI3/QUAD4 轴对称公式闭环

- 公式契约：轴对称首版固定使用全局 XY 子午面，X 为非负半径、Y 为旋转轴、Z 为环向；曲面厚度不参与轴对称积分，体积权重统一为 `2*pi*r*det(J)*w`。
- 求解实现：新增轴对称四分量本构与 `[epsilon_r, epsilon_z, epsilon_theta, gamma_rz]` 应变矩阵，覆盖刚度、质量、体力、总力和热膨胀装配；结果映射为径向、轴向、环向和子午剪应力，并按三维应力状态计算 von Mises 应力。
- 输入诊断：拒绝负半径、偏离全局 XY 子午面的节点和退化 Jacobian；区域 pressure/area traction 因缺少边界法向语义会明确提示改用子午面边界线载荷，不进行静默误算。
- 恢复纠错：曲面结果恢复中的节点力现扣除热等效节点力，修复自由热膨胀位移和应力正确但节点力仍出现伪反力的问题；该修正同时适用于既有平面公式。
- 验证：TRI3 与 QUAD4 均通过自由轴对称热膨胀解析 patch；QUAD4 厚壁圆筒在 2、4、8、16 个径向单元上对 Lamé 位移和径向/轴向/环向应力严格收敛；非法子午面几何拒绝测试通过。
- 全量验证：Release CTest 32/32 通过，总耗时 25.43 秒，无 Qt DLL 弹窗或测试超时。
- 子模块提交：`range-model-lib` 为 `b453a49`；`range-solver-lib` 为 `0040bad`。

## 2026-09-21 Stage 2 通用 equation book

- 新增 `REquationBook`，以稳定的 `RDofDescriptor` 顺序建立节点与 DOF 类型到连续方程号的映射；不同节点可具有不同 DOF 集，停用约束后会确定性重排。
- `RSolverStress` 不再以 `3*node+component` 作为结构方程地址；系统尺寸、位移恢复、约束消元和全局矩阵/质量矩阵装配均查询 equation book。
- 单元装配维数改为读取 ElementDescriptor 的每节点 DOF 清单；现有 3 DOF 桁架、面和体单元行为保持不变，后续 BEAM1 可直接提交每节点 6 DOF 的局部矩阵。
- 局部约束坐标变换已能按 ElementDescriptor 分别变换平移与转角三分量块，为 3D 框架装配保留正确的 6 DOF 变换结构。
- 新增 `tst_equation_book`，覆盖混合 3/6 DOF 编号、DOF 停用后的连续重排和 resize 清空；`tst_solver_stress` 回归通过。
- 全量验证：Release CTest 33/33 通过，总耗时 28.88 秒。
- 子模块提交：`range-solver-lib` 为 `24566c4`。

## 2026-09-21 Stage 2 梁截面持久化契约

- `RLine` 在既有截面面积 `A` 之外新增局部 Y/Z 轴二次面积矩 `Iy/Iz`、Saint-Venant 扭转常数 `J` 和截面局部 Y 参考方向；惯性参数拒绝负值，方向输入归一化并拒绝零向量。
- 模型文件版本从 1.3.2 升至 1.3.3；ASCII 和二进制文件均保存新增截面字段，旧文件读取时使用零惯性矩和全局 Y 默认方向。
- 新增 `tst_line`，覆盖默认值、复制、非法输入拒绝和两类文件格式往返。ASCII 文本会按现有通用写入器的有限有效位数保存浮点数，测试采用与格式精度匹配的数值容差，不要求二进制级相等。
- 全量验证：Release CTest 34/34 通过，总耗时 19.04 秒。
- 子模块提交：`range-model-lib` 为 `90b51db`。

## 2026-09-21 Stage 2 BEAM1 结构内核

- 求解实现：新增两节点 3D Euler-Bernoulli BEAM1，按每节点 `UX/UY/UZ/RX/RY/RZ` 形成 12×12 局部刚度；覆盖轴向、双轴弯曲与 Saint-Venant 扭转，并通过截面局部 Y 方向构造右手坐标系后变换到全局。
- 质量与载荷：实现一致/集中质量矩阵、热膨胀轴力、均布线载荷、重力和自重；瞬态旧状态装配与位移恢复现保留节点转角。显式转角边界条件尚未建模，当前全三向平移固定节点暂按夹持端同步禁用三转角。
- 缩放纠错：求解前自动缩尺原先只处理长度和截面积，遗漏 `Iy/Iz/J`。现按长度四次方缩放三个截面量，并新增往返测试；该遗漏曾使缩尺 1/2 的梁弯曲刚度恰好放大 16 倍。
- 收敛纠错：CG/GMRES 改用 `||r||/max(||b||,eps)` 相对残差，避免病态尺度系统被过小后向误差误判；同时删除“残差趋势为零即收敛”的错误判据。HEX8 前两次迭代残差均为 1 的回归证明停滞不能代表收敛。
- 验证：BEAM1 两个主惯性轴的悬臂端载荷均严格复现 Euler-Bernoulli 三次位移曲线与支座反力；全局 Z 方向梁验证非平凡坐标变换；一致/集中质量均在 4% 内复现第一弯曲频率。
- 能力边界：BEAM1 内核已具备静力与模态计算证据，但显式转角约束、端弯矩/扭矩、转角/内力矩结果和弯曲应力恢复未闭环，故 capability registry 暂不将其暴露给 structural product profile。
- 并行情况：现有单元装配、矩阵运算和特征值求解已使用 OpenMP；本轮 Release 构建继续使用 `--parallel 8`。路线图新增运行级并行配置、确定性模式、实际线程数审计和未来后端能力要求。
- 验证结果：Release 全量 CTest 串行 34/34 通过，总耗时 9.91 秒；`ctest -j 8` 时其余 33 项及全部求解器测试通过，但 `tst_line` 与 `tst_solver_acoustic` 并发时可重复返回失败，二者分别单独运行均通过，待后续定位测试进程间交互后再恢复全量并行门禁。
- 子模块提交：`range-solver-lib` 为 `8e98727`。

## 2026-09-21 Stage 2 梁转角与端力矩闭环

- 模型契约：新增稳定 ID 的 `Rotation/Rotation X/Y/Z` 与 `Moment/Moment X/Y/Z` 变量，以及可选分量的全局转角约束和点实体端力矩边界条件；单位分别为 `rad` 和 `N*m`，中央 capability registry 与 structural profile 可直接查询。
- 约束实现：转角约束直接停用 equation book 中的 `RX/RY/RZ`，支持非零指定值和矛盾约束诊断。转角方程框架与平移 normal/roller 框架分离，避免全局转角约束被曲面局部方向静默旋转。旧模型没有 Rotation 条件时仍保留“三向平移全固定即夹持”的兼容行为。
- 载荷与结果：点实体 Moment 条件直接装配到共享梁节点的转动方程；BEAM1 结果恢复现累计全局节点内力矩，并持久化三分量节点 Rotation 和 Moment。没有转动自由度的实体模型不会被无意义的零转角/零力矩结果污染。
- 缩放纠错：转角按无量纲处理，力矩按 `kg*m^2/s^2` 缩放；修正后端弯矩算例不再随自动长度缩尺产生 4 倍误差。
- 解析验证：悬臂端弯矩严格复现 `theta=M*L/(E*I)`、`v=M*L^2/(2*E*I)` 和两端力矩平衡；非零指定端转角严格复现 `v=L*theta/2`、`M=E*I*theta/L`。模型元数据与量纲因子另有单元测试。
- 验证结果：聚焦测试 3/3 通过；Release 全量 CTest 串行 34/34 通过，总耗时 12.18 秒。
- 子模块提交：`range-model-lib` 为 `d355af4`；`range-solver-lib` 为 `615f197`。

## 2026-09-21 Stage 2 二次单元、梁应力与材料状态闭环

- 梁截面增加 `Wy/Wz/Wt`，模型文件升级至 1.3.4 并向后兼容；BEAM1 完成弯曲极纤维和扭转 von Mises 应力恢复并正式暴露。
- TRUSS2、TRI6、QUAD9 完成二次形函数、积分、边界拓扑与结构求解；BEAM2 采用两个精确 BEAM1 跨装配，中点为实际六自由度节点。
- 新增材料评估上下文，统一传入温度等场、时间、应变和 committed state，返回 trial state；既有温度查表求解改走该接口。
- 验证：二次形函数/拓扑、二次杆解析解、TRI6/QUAD9 线性场 patch、BEAM2 悬臂和材料状态测试通过。
- 子模块提交：`range-model-lib` 为 `c8c4750`、`3085f11`；`range-solver-lib` 为 `1fb4c5e`、`7e8f102`。梁截面/应力提交分别为 `db1cd9b`、`30cb0a7`。

## 2026-09-21 Stage 3 统一网格管线

- 保留 TetGen 封闭三角面四面体化，新增 Gmsh 4.15.2 可选成熟后端；公共请求/结果不暴露第三方类型。
- 覆盖偏置直线、样条曲线、TRUSS/BEAM 一二阶映射、受约束 TRI、transfinite 映射 QUAD、箱体 TET 和局部 Ball 尺寸场。
- Gmsh physical group 转换为稳定命名集合；材料、约束、传感器和执行器以 stable ID + named set 显式成功/拒绝重映射。
- 统一质量摘要覆盖最小 Jacobian、最大边长比、最大歪斜、最小角和翻转单元计数。
- Windows SDK 的 MinGW C++ ABI 与 MSVC 不兼容，已采用 Gmsh 官方 C API C++ 包装层；运行 DLL和许可证纳入部署/安装规则。
- `tst_mesh_pipeline` 覆盖 TetGen 立方体和 Gmsh 一维/二维/三维网格；跨阶段聚焦回归 5/5 通过。
- 子模块提交：`range-model-lib` 为 `3d2e79c`。
- 详细设计与公式：`TECHNICAL_ROUTE_STAGE0_3.md`。

## 2026-09-21 Stage 4 结构约束与控制端口定义闭环

- 模型契约：新增 `RStructuralInteractionSetup`，统一保存线性方程、刚体/分布/Tie 耦合、传感器和执行器；全部对象使用稳定 ID、命名集合、单位、方向和坐标系元数据。
- 持久化：模型文件从 1.3.4 升至 1.3.5，ASCII/Binary 往返通过；修复新增 ASCII 字段中带空格名称必须显式加引号的问题，旧文件继续读为空结构设置。
- 耦合算法：刚体区域使用 `u_i=u_r+theta_r×r_i`，分布耦合使用等权平均，Tie 使用容差内最近节点匹配，随后统一展开为 `sum(a_i*q_i)=d`。
- 诊断：覆盖重复 ID、非法节点/DOF、空方程、单位或方向错误、执行器变量不匹配、矛盾指定值和六刚体模态约束秩。
- 静力执行：MPC 在既有边界条件消元后形成拉格朗日乘子鞍点系统；全局系数会转换到节点局部方程，相关方程约简、冗余跳过、矛盾拒绝。
- 数值纠错：初版无量纲约束块相对 `K` 过小，GMRES 会在位移仍违反约束时满足整体相对残差；现按最大对角刚度缩放 `C/d`，不改变位移解，只改善鞍点系统尺度。
- 求解策略：有 MPC 时自动从 CG 切换 GMRES 并关闭 Jacobi；模态 MPC 因乘子质量矩阵奇异而明确拒绝，等待后续零空间/主从消元。
- 结构初始速度和温度现可在问题定义中使用；真实 `q_dot/q_ddot` 消费、基础速度/加速度和逐步控制输入仍属于 Stage 5/6。
- 验证：`tst_problem_setup` 10/10 通过；完整 `tst_solver_stress` 通过，包含解析 MPC 与矛盾方程用例。
- 全量验证：模型 ABI 变化后重建全部 35 个测试目标；Release 串行 CTest 35/35 通过，总耗时 13.28 秒。重建同时发现并修正一个仍假设 BEAM1 未实现的陈旧 structural-profile 测试。
- 子模块提交：`range-model-lib` 为 `a53e96e`；`range-solver-lib` 为 `ef0b089`。
- 详细设计与边界：`TECHNICAL_ROUTE_STAGE4.md`。

## 2026-09-24 Stage 5 二阶动力学续作（进行中，非结项）

- 用户将较早的“最近任务后退出”指令覆盖为持续开发，要求多子代理并行，直到明确结束 follow-up 才暂停；本轮模型、积分器、并行运行时、载荷解析、移动支承数学核、重启 I/O、旋转初态和异常原子性分别并行推进，主代理负责接线与整合验证。
- 二阶方程 `M q_ddot + C q_dot + K q = f(t)` 已接入结构求解：Newmark 平均加速度、广义 alpha、中心差分、Rayleigh 阻尼、真实 `t0` 力、位移/速度/加速度和梁角运动写回。模态/动力 MPC 采用 `q=q_p+Tz` 消元，避免乘子系统的奇异质量矩阵；非零静态 MPC 右端按物理单位正确换算。
- 模型版本进至 1.3.8：持久化时间积分/并行配置、初始转角/角速度以及已接受状态 checkpoint。checkpoint 含 q/v/a/外力、累计外力功与阻尼耗散、固定步长和网格/DOF/系统指纹；重启拒绝网格、时间、步长、方程顺序和约化系统不匹配。系统指纹量化到 5 有效位以适配旧 ASCII 模型的 6 位数值输出，属于防误接线检查，不是严格逐位同一性证明。
- Windows Qt 测试执行通过 CTest 给所有子模块 QTest 注入 Qt DLL 目录 PATH；无需重装 Qt。直接运行未部署 exe 时仍须正确 PATH，避免旧的 `Qt6Core.dll` 弹窗。模型写盘顺序现先提交数据目标，再更新 latest link，失败不会让 link 预先指向无效目标。
- 模型 ASCII 结果变量的多词名称/单位写出已加引号，修复 `Von Mises stress` 读回；模型子模块提交 `fe57b3b`，其独立模型测试 16/16 通过。磁盘续算测试在主、另一独立构建各连续 20/20 通过；第三构建最初 OpenMP fork 崩溃，经 `--clean-first` 全依赖重编后也连续 20/20 通过，原因是 ABI 变更期间混用旧对象，不是当前源码可复现故障。
- 求解器提交 `855453c`：内存/磁盘 checkpoint 续算、系统指纹、初始转角/角速度、载荷解析与中心差分 CFL 测试；每步模型/共享数据/积分器快照，在准备或写出失败后回滚，旧结果 target 与 latest link 保持不变，同一 solver 对象可修正输入再试。为避免预修改旧工件，结构动力续算跳过 legacy `updateOldRecords`。独立 Release 聚焦回归 7/7 通过；本轮根目录 Release 全量 CTest 尚待模型 ABI 稳定后执行。
- 事务现为正确性基线：每步深复制模型和参考矩阵，时间/内存开销随模型大小及非零矩阵项增长；后续应换结果/配置 journal。若 target 已提交而 latest link 失败，会留孤儿 target，尚无统一多工件磁盘事务；原始错误会保留，但 OOM 导致的回滚再次失败不保证原子性。系统指纹受旧 ASCII 六位有效数精度限制，不能当作严格逐位相同的证明。
- 下一增量已按并行边界拆分：模型层版本化五次 Hermite `(t,g,v,a)` 移动支承历程，求解器保留 `K_fc/M_fc` 并合成 `C_fc` 接入 FE，另做多线程重复性/耗时诊断测试。首版移动支承与 MPC/局部 frame 组合将明确拒绝，不能把现有分块数学核误报为产品 FE 支持；之后再做全量 CTest、更新技术路线与 handoff 并提交根指针。
- 模型子模块随后提交 `dbf4f42`（1.3.9 Hermite 支承运动契约）与 `1fd3c9e`（motion ASCII knot 限域 17 位精度），model-only 15/15 CTest 通过。求解器工作树已接入全局点节点平移移动支承：`K_fc/M_fc` 稀疏装配、Rayleigh `C_fc`、物理/求解尺度换算、受约束节点 q/v/a 回填、轨迹指纹及磁盘续算。独立 Release 聚焦 7/7 通过，包含两节点杆解析、ASCII/Binary 8+12 对照 20 步与改 knot 拒绝；主建置全依赖 Release 正重编，未计作结项。
- 支承轨迹求解前覆盖域预检已改用模型 `evaluate()` 的同一端点容差；新增终点短 `1e-13 s` 的负例，必须在接受首步前拒绝，独立 Release FE/重启回归 2/2 通过。移动支承 FE 接线提交 `43a96b0`；`nodeForce` 只含单元弹性内力，不是含 `M a+C v` 的完整动力支承反力，不能用于控制功率。
- 并行重复性基准提交 `f4fe095`。独立干净 Release 建置后 CTest 连跑 10/10，通过串行、2 线程、4 线程各三次对照：节点与 checkpoint 状态、外力、能量和耗散数值漂移均在设定容差内。该 32 单元/10 步小样本中串行中位约 27.9 ms，2/4 线程约 83.8/105.4 ms，不能宣称并行加速；基准只报告耗时，不设速度硬门槛。主建置全量 CTest 仍待完成。
- 正并行审查下一增量：模态阻尼需有模态残差、质量正交、重根和规模护栏；结构化 InteractionRecord 应与可移植模型配置分离；完整动力支承反力需保留受限行并在接受步后算 `r_c=(M a+C v+K q-f)_c`，不能直接复用 `nodeForce`。这些均尚未计作已实现。
- 主 `build/vs2026` 的 Release 全目标建置成功，包括 `fea.exe`、`windeployqt` 与测试运行时镜像；已核对 `Release/Qt6Core.dll` 和 `qtlib/platforms/qwindows.dll` 存在。第一次全量 CTest 因建置期间 FE 源更新后未重链 solver 库，出现旧预检错误消息、44/45；单独增量重编 solver/FE/restart 后，两项聚焦 2/2，原有完整 CTest 45/45 通过（`-j 2`、`OMP_NUM_THREADS=4`、10.46 秒），不应把混合对象误判为当前源码故障。
- 命令行求解器 `--nthreads` 省略时读未初始化字段的 UB 已修：缺省不覆写 OpenMP，显式选项才生效；`SolverInput` 的监测文件与线程覆写在复制/指派时完整保留。根提交 `2167d28`，独立 Release 测试 1/1 通过。移动支承 BC mask 的 `5e-13` 微小非零值审查后改为严格零且有限，独立 FE/重启 2/2 通过，求解器提交 `c1dc466`；新的主全量回归仍待执行。
- 后续完成模态阻尼有限规模首版：模型提交 `265abcf` 将 ratio ASCII 精度局部提至 17 位；同时发现并修复 `RModelData::selected` 默认未初始化导致原始 ASCII 布尔字节偶发为 `0x20`、读档错位的旧故障。模型独立 Release `tst_problem_setup` 连跑 20/20，模型全量 15/15。求解器 `6972a47` 在 MPC 消元坐标上合成 `C_r=Σ2ζ_i√λ_i(M_rφ_i)(M_rφ_i)^T`，增加正定、残差、正交、重根与规模护栏，拒绝与 Rayleigh/移动支承混用；主建置模态、FE、续算、特征值和模型聚焦 5/5 通过。`6c5dd91` 再加 384 杆 × 串行/2/4 线程 × 三次并行诊断，独立 Release 重复 3/3，通过数值对照而不设置耗时门槛。
- 用户最新要求改为“当前 Stage 5 完成后退出”，不再持续推进 Stage 6。CLI 执行事件 journal 接线、支承动力反力/功率、旧一阶产品标记 QA 正并行处理中。主 `build/vs2026` 48 项 CTest 的一次尝试为 46 项通过、2 项新 CLI/journal 测试因未建置而 Not Run；必须完成全目标重建后重新全量运行，不能写成 48/48 通过。

## 2026-09-24 Stage 5 被动二阶动力学结项

- 本节覆盖并取代上节末尾的「进行中」快照。Stage 5/6 边界已修订为：本阶段验收控制输入关闭时的被动二阶求解、可复现执行、接受态约化有效力/阻尼功率、受支持的支座反力/功，以及运行级审计；控制器特有功率和逐步 InteractionRecord 属 Stage 6。此界线避免将尚未定义来源的总力误算为控制力。
- 模型 `1174547` 升至 1.3.10，在 checkpoint 中持久化可选支承反力、功率、累计功及独立受限算子指纹，兼容 1.3.9 无功账旧文件；求解器 `761de0a` 在约束消元前保留 `M_c/K_c/f_c`，用 `R_c=(M a+C v+K q-f)_c` 计算支承施于结构的反力，并以 `P_c=R_c·v_c` 和梯形积分计算累计功。真实 `t0` 和接受步使用各自的装配行；观察回调在结果写出和步提交成功后通知，失败试步不通知且回滚。固定/移动杆解析、外力符号、ASCII/二进制双向续算及写失败测试均通过。首版只支持全局点节点平移且无阻尼/Rayleigh 阻尼，MPC/耦合、局部 frame、转角梁与模态阻尼组合明确拒绝。
- 求解器 `645f1f9` 提供接受态 reduced 有效力/阻尼功率样本；它们和支承端物理功率是不同口径，都不能直接代表控制器功率或完整系统能量。CLI `523c31f` 的追加式 `.events.jsonl` 记录 run ID、连续序号、输入/结果工件 SHA-256 与并行执行报告；writer/QProcess 正负路径通过，但不承诺跨模型、link 与 journal 的单一事务。
- 旧一阶应力瞬态与新二阶产品 profile 已分开标记，GUI 相关设置文字及帮助文档作了区分。384 杆基准在本机四线程相对串行只略快，CI 不设置速度阈值；多线程数值对照在设定误差范围内，但不宣称严格确定性。
- 验证：`cmake --build build/vs2026 --config Release --parallel 4` 全目标成功（含 `fea.exe`、测试及 Qt runtime 部署）；`cmake -E env OMP_NUM_THREADS=4 ctest --test-dir build/vs2026 -C Release --output-on-failure -j 2 --timeout 180` 为 **49/49 通过、0 失败、9.28 秒**。`Release/Qt6Core.dll` 与 `Release/qtlib/platforms/qwindows.dll` 均在位；尚未在干净 Windows 机器做安装/启动终验。
- Stage 5 按 `PROJECT_ROADMAP.md` 退出条件完成，本轮依用户要求停止，不进入 Stage 6。后续首先定义带来源标识的 `f_control`、传感器/执行器逐步协议、控制器状态检查点及 InteractionRecord，再考虑压电/拉索和全状态热耦合。已知限制详见 `HANDOFF.md` 与 `TECHNICAL_ROUTE_STAGE5.md`；历史尝试的 46/48 Not Run 与早期 45/45 属过程记录，不覆盖最终 49/49。

## 2026-09-24 Stage 6 控制耦合续作（进行中）

- 用户新指令：继续开发至 Stage 6 完成，特别核对主迭代及其他耗时流程的可并行效率、现有/新增动力积分算法选择、面向外部 API 和未来全流程 Agent Copilot 的接口契约，并维持自动提交、仓库清洁、技术解释及交接。启动点：根 `a40eb52`、模型 `1174547`、求解器 `761de0a`，Stage 5 全量 49/49 是上一基线，不是 Stage 6 测试结果。
- 架构审查：当前 FE 求解入口已有 Newmark 平均加速度、广义 alpha、中心差分三法；中心差分仍要解含阻尼的矩阵，不能称为无矩阵求解的显式法。积分器固定算子却每步重建有效稀疏矩阵/线性求解器；单元装配虽已有 OpenMP 线程缓冲，仍每步全量重装配和深复制模型。先优化可证明不改语义的固定 `dt/M/C/K` 有效矩阵缓存，同时计划分段计时、不同规模/线程数/误差的 A/B 测试；不靠新增算法掩盖瓶颈。
- 控制协议决定：在已接受的 `q/v/a` 上采样传感器并调用控制器一次，验证/限制以稳定 actuator ID 标识的节点力命令，本步零阶保持，再试算、接受或回滚；控制器不在矩阵组装、CG/GMRES 内迭代或积分器可重复调用的力回调中推进内部状态。命令阶跃改变时须以新力刷新 `t_n` 初始加速度，避免把零阶保持误当端点斜坡；额外质量矩阵求解须进入性能账。
- 并行子任务正在实现模型 1.3.11 控制配置/账本、求解器控制接口与接受态功账、CLI 进程外版本化 NDJSON 传输（尚未接到 `SolverTask`）。根仓库新增 `AGENT_INTERFACE_DESIGN.md` 草案：以后 GUI/CLI/Agent 共用版本化 Command/Query/Capability/Artifact/InteractionRecord 真源，模块短手册只作按需导航；目前不能把草案当已开放 API。

## 2026-09-24 Stage 6 C++/Python 控制核心结项与交互交接

- 本节覆盖上一节的「进行中」快照。按用户本轮指令在 Stage 6 当前任务后退出，不进入 Stage 7；完整续作入口见 `HANDOFF.md` 顶部。多代理并行完成模型、求解器、传输/记录契约、playbook 和性能稽核；各子模块先做独立测试与提交，再由根仓库整合与提交指针。
- 模型 `63a92b1` 升至 1.3.12，持久化控制 ID/协议/种子、每通道 delay/hold/noise 策略，以及顺序通道命令/功率/功/控制器原始状态和调理器状态。求解器 `e8141d1` 在接受步外调用纯控制回调，Stage4 点节点力经 MPC 约化稀疏映射；在主迭代之外执行调理、限幅/速率、零阶保持和物理 N/W/J 控制功账。写出/试步失败回滚控制器与调理器候选状态；旧 1.3.11 受控 raw checkpoint 因缺调理历史明确拒绝重启，旧被动模型继续读。
- 求解器线程隔离 CSR 快取并核对地址复用/矩阵值变更，固定步长有效算子复用；恶意同步回调修改传感器/执行器目录会在当步被拒并回滚。FE 测试覆盖 Newmark/广义 alpha/中心差分的零控制被动等价、解析保持力、幅值/速率、双 DOF 通道、速度反馈抑振、延迟/保持/固定噪声重启重播、功账及 observer 失败语义。
- CLI 以绝对可执行程序和独立 argv 启动 v1 NDJSON 子进程，握手绑定控制器 ID/版本摘要、本次输入模型快照摘要与种子；请求/响应 ID、目录、单位、超时和错误均严格校验。Python PID 样例已实际接 FE；控制步事件分别记录 requested/conditioned/applied、传感器、状态 SHA、N/W/J 和是否真的写出结果工件，RunFailed 保留结果可能已存在的提示。基础库 `dca3785` 新增严格 InteractionRecord v1 数据格式；GUI/Agent 写入钩子依赖 Stage10/11 共用命令服务，不能假称已接线。
- 主 Release 全目标重编成功，CTest **53/53 通过，0 失败**；`tst_solver_audit_cli` 在输出频率 2 时证实未写出步骤不附假工件。`Qt6Core.dll` 与 `qwindows.dll` 已部署，`tst_structural_product_profile.exe -silent` 直接退出码 0。32/384 杆串行/2/4 线程本机中位耗时分别为 24.726/68.365/83.695 ms 与 175.868/158.218/144.151 ms；多线程不保证加速，待分段计时、大模型/内存对照及消除每步重装配与深复制。详见 `TECHNICAL_ROUTE_STAGE6.md` 和 `docs/STAGE6_PERFORMANCE_AUDIT.md`。
- 本机 MATLAB R2022a `-batch "disp(1+1)"` 可启动并输出 2，但标准输入长驻握手尝试未在限时内得到回复，已中止，无 MATLAB 适配器交付声明；外部 Python 跨进程恢复亦未独立验证。Agent 模块 playbook 已入 `docs/agent_playbooks/`，机器可验证契约与 capability 才是真源，自动二次开发需待 Stage12 受控扩展流程。

## 2026-09-25 连续阶段审计与 Stage 7 首增量（进行中）

- 用户要求审查交付文件、开发进度、各阶段计算结果与算法，继续后续 stage 直到停止 follow-up，维持自动提交与整洁目录；本指令覆盖历史 Stage 5/6 结项后退出要求。起点根 `74d60dd`、模型 `63a92b1`、求解器 `e8141d1`。既有未跟踪 `.agents/`、`.codex/`、`AGENTS.md` 保留，不混入功能提交。
- 根提交 `54fd503` 建立 `docs/RELIABILITY_AUDIT.md`、更新当前授权入口，并将路线图早期“无梁/无二阶动力学”等描述明确标记为历史状态。原始 Release 全目标重新构建中，本轮尚未宣称 53/53 重跑通过。
- 源码审查发现普通模态求解的未收敛结果仍可发布；正在准备子空间/反幂迭代失败关闭与广义残差检查，并补零容差精确解测试。静力线性求解也缺最终残差门控，正在追查与修补；不改变非结构功能的求解语义。
- Stage 7 新增纯拉索、均匀端口压电数学核。拉索使用参考面积工程应变、仅受拉、初张力材料/几何一致切线；压电使用有明确介质外向通量符号的电焓 Hessian。隔离 VS 18 2026 x64 / Qt 6.11.1 Release 构建成功，CTest 2/2，QTest 明细拉索 8 passed、压电 7 passed（含 init/cleanup），均无失败或跳过；主工程注册仍待原始基线完成。
- Astra 独立原始码审查确认数学核公式与测试一致，并发现模态补丁零容差比较问题；补丁已改为 `<=` 并新增标量精确解回归，经复审确认。尚未把源码审查等同于模态补丁运行验收。
- Stage 6 Python 外部进程 20 步对照 8+12 步续算测试已准备，包含 PID 积分状态、跨保持区间切分、延迟和固定噪声；尚未执行。广义 alpha 待补双频耗散与时间收敛测试；公式已对照一次来源，能量口径仅为诊断，不宣称逐步物理能量单调。
- 构建、临时补丁、独立测试工程与原始日志全部在忽略目录 `build/audit-20260925/`。稳定增量执行完验证后，按子模块先、根指针后提交；Stage 7 FE/模型/控制接线尚未完成。
- 本增量完成：求解器提交 `5fbda37`（模态/静力失败关闭和广义 alpha 验证）及 `67c400c`（拉索/压电数学核与主工程注册）。Release 全目标建置最终成功，完整 CTest **55/55、0 失败、14.26 秒**；CLI QTest **10 passed、0 failed、0 skipped**，包含真实 Python 跨程序续算。
- 过程纠正：全量建置部署与同时执行 CLI 发生 Qt DLL 复制存取冲突，串行重试成功；首轮 54/55 中唯一失败为新广义 alpha 测试的 dt=0.02 与绝对误差门槛不匹配，保持二阶收敛/误差判准并改用 dt=0.02/0.01 后全量通过。求解器积分公式未为此修改。
- 根将提交新 Python 续算回归、求解器指针、可靠性审计及 Stage 7 技术路线。继续 Stage 7 模型/FE/控制接线，尚不进入 Stage 8；不把数学核验证当成完整 Stage 7 验收。

## 2026-09-25 Stage 7 静力拉索 FE 与持久化增量

- 模型 `a7c6040` 新增显式拉索配置、1.3.13 文件尾部、旧版默认回退和截断/非法拓扑拒绝；`2883889` 修复元素组条件读取合并错误，防止删除的重力被恢复、重用对象残留旧边界/初始条件。
- 求解器 `a95e6f3` 新增稀疏 Newton/Armijo 平衡、受限静力 FE 适配器与生命周期早期门控。只支持显式 TRUSS1、全局位移、点组恒定力、零重力和参考温度；不把静力力平衡当作动力学、唯一性或稳定性认证。
- 新线性内层容差曾以固定下限吞掉小量级右端项，已改为相对当前右端项并补小力尺度回归。FE 首轮因默认环境条件被过度拒绝而失败，改为检查具体温度和重力值；高预张力垂度另揭露能量舍入导致 Armijo 耗尽，独立标量复现后增加严格残差下降约束的舍入容许策略，外层力容差不变。
- 根 CLI 新增 ASCII/二进制模型写入、独立求解、结果读回与完成事件对照。QTest 平衡 10、FE 14、CLI 12 passed（含 init/cleanup），均 0 failed、0 skipped。
- 全目标 Release 重建成功，479.78 秒；`OMP_NUM_THREADS=4`、CTest `-j 2 --timeout 180` **59/59 通过、0 失败、15.28 秒**。日志和失败尝试均留在忽略目录 `build/audit-20260925/`，以 `cable-full-*` 和 `cable-*-verified` 为最终证据。
- 本机已验证临时 MSBuild MTT、跨项目编译进程上限 4 的命令参数；不修改持久环境，编译/运行串行避免 DLL 部署冲突。下一增量先抽取共用静力 TRUSS1 helper，再接固定电压一维压电 FE 与带明确符号的电极电荷输出；继续执行直到用户停止。


## 2026-09-25 Stage 7 第三增量结项并停止

- 用户 follow-up 要求“完成当前阶段并退出”；完成正在进行的固定电压压电杆静力增量后停止，不继续后续 stage。完整 Stage 7 尚未验收。
- 模型 `872c060`：1.3.14 端口持久化、严格新块解析及独立 C 单位电荷变量。求解器 `d743997`：共用 static TRUSS1 helper、拉索按 ID 映射、固定电压小应变杆稀疏求解与双重残差认证。CLI 两种格式验证 u=-0.002 m、qR=1.001 C。
- 独立审查修复 ID 空白改写与非法 ASCII flag 静默关闭；首轮测试发现结果精度断言及诊断顺序问题，已修正且保留过程日志，没有放宽物理收敛判据。
- 全目标 Release 成功；最终 CTest 61/61、0 失败、14.92 秒。压电配置/FE、拉索 FE、CLI QTest 11/10/16/14 passed，均无失败/跳过。自动子模块提交后根提交指针、CLI 和交接；不 push。用户既有未跟踪配置保留。
- 已更新 HANDOFF、路线图、Stage 7 技术路线、README 和可靠性审计。后续入口与已验证范围写入 HANDOFF；本轮至此停止。

## 2026-09-26 Stage 9 工作点、降阶与真 FE 控制交付

- 用户授权此前环境疑难不阻挡时推进至Stage9，覆盖旧Stage8退出条件；本轮完成后停止，未进入Stage10。Stage0干净VM和Stage6 MATLAB适配器仍是独立未关闭项。
- 求解器 `a6292c8` 增加真实SI工作点M/C/K/B/q-v-a目录导出和杆/梁/严格绷紧缆索切线测试。独立审查发现模态入口会受旧位移、旧时间算子、支承非零位移与点弯矩影响，已改为私有结构动力组装、禁止时间推进/结果发布，全部回归通过。
- 根 `af48742` 增加CLI原子/源哈希导出、SciPy模态/CB/descriptor/FRF/PBH、全阶与ROM误差、离线共点速度控制设计及实际NDJSON/FE接线。审查修正源身份、输入覆盖、ROM力/目录、近特征根秩和时间域投影/预测误差混淆；绝对频响误差按单位统计。冻结后独立审查无剩余重大发现。
- 完整全目标Release成功、CTest **87/87（44.52s）**。真实4段杆400步主动与被动FE都对照独立NumPy Newmark；受控q/v终态最大差1.98e−11m/8.74e−10m/s，实际末能量比0.0118633。两模态在30rad/s的相对FRF最大误差28.70%，CB为1.2020%；不将降阶等同任意频带准确。
- 源码/安装示例收拢在 `examples/stage9` / `examples/control/stage9`；原生/矩阵算例保留字节哈希。Python3.13.5+NumPy2.1.3+SciPy1.15.3实测；最低声明统一Python3.10+，解释器/库是独立依赖。CMake需显式选定已有数值库的Anaconda，自动发现的3.14不具备这些依赖。
- 本机安装树在隔离PATH完成原生导出、已安装分析/设计、400步控制和GUI帮助启动。最终ZIP与Inno生成成功，Stage9脚本/依赖/算例/文档及CLI逐字节核对通过；包大小/哈希在HANDOFF与忽略的 `build/stage9-package-verification.json`。详细证据保留于忽略的stage9构建/测试/安装工作流目录，没有提交临时日志或缓存。


## 2026-10-01 计划修订与下一阶段授权

- 用户要求根据状态审查修订并细化计划，随后开发、独立审查、验收，稳定增量自动提交；覆盖旧的 Stage 9 停止限制。
- 计划统一入口为 `docs/DEVELOPMENT_PLAN.md`，分解 P0/10A/10B/10C/11A/11B/12A/12B，安装、迁移、恢复和性能为持续质量线；数值扩展与硬件需求分开记录。
- 核对根 `46e7318` 和干净子模块；现有 Release CTest 91/91、0 失败、62.52 s，未重建或重跑外部安装/MATLAB。发现 CI 未启用测试、Python 数值依赖缺失可减少测试注册，列入 P0.2。
- 修正路线图旧近期顺序、MATLAB TCP 状态、单体热机三维共线范围及 ADR 状态。规划不是实现声明；后续逐项记录提交、验证和剩余边界。

- 10A.1：root-owned `range-application-lib` 提供真实 RModel 模态配置、查询、预览、幂等、版本和撤销/重做。候选状态整体复制后交换，失败不改变接受态；上限不驱逐幂等键。Release 构建及聚焦 CTest 1/1 通过；独立审查未发现重大问题，补分支/同键修复/几何保留用例。首轮测试临时 QJsonValueRef 悬空已修复，不是服务版本回退。GUI/CLI 和持久事务尚未交付。

- P0.2：新增不签名/不发布的 Windows 回归工作流，显式 Python/数值依赖和 OpenMP runtime；按名称校验必跑 92 项，拒绝缺测/禁用必跑项。审查发现 disabled 漏洞和工具链目录 runtime 未入 PATH，均修复；工具测试 7/7。严格数值配置、全目标 Release 构建、清单 92 项及完整 CTest 92/92（64.20 s）通过。证据在 `build/audit-20261001/`。远程 workflow 未运行，不宣称托管 runner 已验收。

## 2026-10-01 模态 GUI/CLI 共用服务接线

- 新增 root-owned `range-model-command`，查询完整原生文件及 SHA256，严格验证参数并预览，正式输出经同目录暂存及 QSaveFile 发布。源/参数/目标协作锁与发布前哈希复核保护正常并发，失败保持旧输出；不声称对不遵守锁的外部写入者提供原子比较交换，也不提供跨进程幂等账本。
- GUI 模态设置通过同一服务验证；选中模型全部验证后再修改，保留既有 Undo/Redo，活动任务期间拒绝。测试覆盖控件参数、真实 CLI ASCII/二进制往返、源哈希过期、非法单位/字段、路径别名、latest-link 和锁冲突；完整窗口操作及内存分配异常时的跨模型回滚不在本次完成范围。
- 实际子进程测试发现模型日志混入 stdout，导致 JSON 解析失败；CLI 在解析启动参数后保留 JSON 输出描述符，将原 stdout 日志重定向 stderr。修复后新增 3/3 聚焦测试通过，独立审查无未解决重大问题；POSIX 分支尚未执行。
- 最终全目标 Release 构建成功；清单验证 94 必跑项，完整 CTest **94/94、0 失败、69.62 s**。安装树生成成功，新 CLI 与说明存在，缩减 PATH 的安装版 `--help` 退出 0。没有重跑远程 CI、干净 VM 或 MATLAB 实机。原始证据统一留在忽略的 `build/audit-20261001/`。
- 计划拆出 10A.2b 完整窗口终验及 10A.3a/3b 生命周期/持久审计；此增量不等于 Stage 10 完整验收。产品代码、对应测试、主题说明分别归入模块和 docs；自动提交稳定增量，保留用户未跟踪配置，不 push。

## 2026-10-03 中断收尾：模型生命周期与杆/梁工作流

- 用户先要求继续到 Stage 12，额度中断后改为只完成最近断点并整理退出；子代理由 Astra ultra 改为 Luna max。本轮止于受限 Stage 10A/10B 软件入口，不接入工作流 Agent 或自动开发；Stage 12 草稿原样保存在忽略的 build/audit-20261003/deferred-stage12。
- 原生模态服务补齐运行/关闭/外部 UndoRedo 与重载失效、InteractionRecord 持久日志、真实工件 SHA256、发布前意图及新进程只读核对。GUI 宿主持有稳定逻辑身份、独立编辑 revision 和快照；真实 Application/MainWindow 测试覆盖单/多模型、历史、保存重开和拒绝路径。
- 完整窗口初次测试超时，定位为 ModelIO 改到 GUI 线程后触发材料树旧 BlockingQueuedConnection 自锁；改为自动连接。独立审查发现关闭审计失败后条目丢失，现保留待重试且报错；重载后审计失败也返回失败作业而不谎称完整成功。GUI 保存不是跨文件原子事务，关闭审计没有跨进程重试队列。
- 新 range.workflow/v1 服务具有精确参数目录、绑定来源/revision 的预览与宿主批准、单作业预算、不可变任务目录、Windows 子进程树取消和重开后的 interrupted 状态。原生 helper 生成主动/被动模型、解析 native-link、查询真实位移/转角及检查点。杆/梁工作流在实际 FE 后与独立 NumPy Newmark 回放比较。
- 梁接线校验发现导出 DOF 后缀属于局部约束坐标基；四杆全局 X 和梁全局 Y 的平移都可能标为 UZ，真实方向由 globalDirection 给出。修正按物理模板轴与 U/R 类别处理初值、投影和不同单位的误差。适配器回复现在校验协议版本及操作对应字段，并有错误版本反例。
- 全目标 Release 成功，必跑清单 97 项，完整 CTest 97/97（102.08 s）。工作流 8 项含实际杆/梁续算、篡改/取消/服务崩溃；安装包树缩减 PATH 的 GUI/CLI help 和实际梁 100+100 步主动/被动续算通过。未做远程 CI、干净机器、OpenGL 渲染或远程模型调用。GUI 子模块 a5be2bc，根生命周期提交 9a2afe1；最终工作流提交见 Git 历史。用户配置保留，不 push。

## 2026-10-03 实时 GUI Agent 与受控自动扩展规划

- 根据新的规划请求扩充 `docs/WORKFLOW_CONTRACT.md` 第7–18节，并同步当前开发清单和交接；只修改文档。
- 明确统一状态/命令/事件、运行快照与编辑版本分离、任务授权与供应商工具适配、本地MD/schema/运行时事实的职责、私有Python和不可变扩展环境、声明式GUI及新版本激活/回退。
- Luna max只读核查发现旧GUI求解仍访问活模型、部分写入通知覆盖不足及全局忙状态阻塞查询，列为10C前置改造，未宣称已经修复。
- 查阅Qt、CPython、Microsoft、JSON Schema及MCP官方资料；粗估15–27人周含质量验收，阶段号不作完成比例。本轮未构建、未跑数值回归、未调用远程模型或安装运行时。

## 2026-10-03 Stage 10C–12 实施与真实 DeepSeek 验收

- 用户重新授权实施至 Stage 12，随后指定本机 DeepSeek 配置；子代理按最新要求使用 Luna max。根负责接口、集成和终验，子代理分别完成任务循环、扩展 GUI、求解快照、Python 扩展运行时及独立审查；共享构建串行，不推送。
- GUI 新增 Workflow / Agent tasks / Extensions。文档 UUID、接受版本与文件哈希分工明确；模态预览/执行和 Undo/Redo 走同源服务。SolverTask 输入固定到独立任务目录，后台不读活模型；结果跨 GUI 线程验证版本后发布，过期或取消结果保留。发现 QList<Model> 复制丢失历史，补齐文档列表及结果发布的历史保存与事务交换。
- Agent 使用异步供应商适配、固定任务范围、宿主批准、预算、取消和关联 ID；扩展提案始终单独审查。DeepSeek 实测返回额外 `index`，补有限非负整数兼容并从标准历史中移除；错误正文/凭据不进入 UI 或日志。真实 GUI 模态修改通过，真实模型生成的统计扩展通过独立校验、批准激活和数值运行。
- 扩展提供不可变源码/测试/manifest、独立宿主数值基准、声明式输入/结果、版本激活及回退；源码、运行环境、报告和输入均绑定哈希。只允许显式受信开发者模式执行，独立进程不被描述为 OS 沙箱。运行时完整清单在执行前后强制核验，防止同尺寸/恢复 mtime 绕过缓存。
- 打包脚本锁定 CPython 3.13.5、NumPy 2.2.6、SciPy 1.15.3 的来源和 SHA256；安装私有解释器隔离用户环境，SDK 示例在缩减 PATH 下完成梁实际 FE 对照。Windows OpenGL 与三面板截图检查通过。
- 集成测试暴露并修复：模态生命周期构造参数遮蔽、控制器启用的快照测试夹具缺启动参数、扩展候选分页后选择丢失、作业列表轮询使预览失效、版本详情刷新清空数值结果。最终验收数字和证据索引集中记录在 `docs/STAGE12_ACCEPTANCE.md`，不将中途失败误写为通过。
- 补齐默认收起的文档事件查看面板，覆盖事件缺口重同步、锁定缓存到解锁后实时数据恢复；不读取密钥或修改用户草稿。最终全目标 Release、105 必跑清单和完整 CTest **105/105（249.47 s）** 通过，独立复审无新增重大问题。真实生成源码另在安装私有解释器下再次验证/激活/运行成功；AI 子模块 `e15b7fe`，根提交包含全部集成、说明和复跑测试。

## 2026-10-04 对话侧栏与原生控制实时曲线

- 用户指出预览中配置和日常操作混杂，要求按 Codex/Copilot 对话和 Simulink 曲线体验改进。完成独立 Assistant 及底部控制面板；供应商/凭据移到应用设置，模型/effort/权限留在输入区。问题、批准、追问和工作中发送通过真实控件调用标准 runner 接口。
- 新一轮只继承有界、非授权的历史数据，重新读取宿主状态；提交的计算继续运行。独立审查发现同步信号重入可能继续旧工具，已补任务代次/调用 ID 检查和回归。端点在落盘之前拒绝 URL 凭据；DeepSeek 思考元数据有界保留且不作为普通回答展示。
- 新只读 `telemetry.query` 从原生事件日志读取接受步。控制图显示实际传感器/施加力，支持窗口、读值、冻结显示和 CSV；明确区分独立参考。审查修复未接受事件混入、冻结导出快照身份以及缓存压力问题。
- Release 全目标成功；必跑清单 107/107，完整回归 107/107（248.95 s）。实际窗口检查后补安全 Markdown 与列表边界修复，再全目标构建并运行相关 4 项回归（25.02 s）。最终真实 DeepSeek 修改+追问、Windows/OpenGL、梁 100+100 步共 QtTest 5/5（19.954 s）；本轮三次联调共 13 响应、50406 tokens。
- AI 子模块提交 `21bbdc1`。Luna max 独立复审无剩余重大问题。具体安装核验、工件和功能边界见 `docs/GUI_UX_ACCEPTANCE.md`，后续路线见 `docs/GUI_INTERACTION_DESIGN.md`；未将界面改进表述为已达到商业软件全部成熟度。

## 2026-10-08 全工作流方案、验收与续作工具

- 用户在缺口核查后要求明确开发方案/验收并详细记录，保持仓库整洁。本轮交付规划和只读规划工具；WF 功能全部 planned，原产品基线未改变。
- 新 `WORKFLOW_DELIVERY_PLAN.md` 与 JSON 清单覆盖 14 个任务/119 条验收：模型/命令/结果贯通、持久恢复、流式与会话、长任务知识、运行中调参/图表、科学扩展、隔离环境、几何/网格与发布质量；各任务有依赖、范围、拒绝条件和可核查故事。状态只存 JSON，验收规则只存详细方案。
- 新开发 playbook 明确任务选取、来源/版本/权限、验证、证据格式、完成条件、提交和中断交接。校验器检查结构/引用/依赖/最新条款证据，推荐下一任务但不执行任务；不宣称能验证日志真实性或 Git 提交存在。
- Luna max 实现规划清单/校验器并独立审查方案。修复硬编码重复任务定义、阻塞原因缺字段、进行中任务绕过依赖、旧通过覆盖新失败、标签/持久身份定义、检查点间隔及 both 模式中断漏项；补 WF-14 避免几何/网格写入口在首批分析命令验收后失去跟踪。
- 清理 README/roadmap/计划和旧 playbook 的过时说明，开发文档与产品知识边界明确。Windows CI 接入规划校验，主程序/SDK/求解器/安装运行时不增加规划目标操作。
- 实测规划检查 14 项/119 条且下一任务 WF-01；CI 工具单元测试 14/14，含规划工具 7 项；文档本地链接/锚点与 CI YAML 检查通过。补 Codex worktree 位置回归，避免合法 checkout 因父目录含 `.codex` 而被拒绝。记录在忽略的 `build/audit-20261008/workflow-plan/`；没有复用历史 107 项当本轮新功能验收。


## 2026-10-09 WF03/WF04闭合与持续目标扩展

实现冻结ae2cd80；完成当前源控制/原生broker/live/SDK/Agent/逐幀CSV/场定位，原九项与后续独立审查全部修复。最终125/125、655.46s及安装私有SDK/Windows3/3通过；详见WF03/WF04验收文档。保留失败历史：ASCII舍入、beam恢复模量、模板控制器ID硬编码、测试自注入、setElement默认入群组、restart追加语义、PickList压缩索引越界；不放宽任何数值门槛。

用户授权延长至顺序4；新skill执行Luna max优先，Sol只解决具体未决推理。WF05/WF06启动，依赖门槛与WF13外部终验独立保留。Root配置/用户文件/凭据不变，不push。


## 2026-10-09 WF05–WF07验收与顺序3持续实施

WF05/06冻结ed57d60，完整132/132及真实Windows/私有SDK/DeepSeek通过；记录分别见acceptance/WF05/WF06。WF07冻结6294aee，完整136/136、零失败1578.11s；历史/分支/只读多层权限、原task持久核对引用、64未观察容量及20MiB收据预留实现，独立终审三P2闭合。首次边界终态fixture与Windows原生保存对话框自动化失败保留，后续修复具实际回归。真实native并行使用fixture供应商，实际DeepSeek旁路在主编辑后执行，分开记录；不冒称实际供应商全过程求解并行或外部干净机器。

WF08依赖已满足，纯DAG/单state提交/版本knowledge设计冻结后启动；WF09独立接受参数codec及WF10绘图新控件准备不改变已测接口，尚未交付。当前skill允许需要方法/诊断直接Sol medium、紧耦合问题Sol high；明确执行Luna max，reviewer根继承。用户配置、凭据、记忆保持原样，不push。

## 2026-10-10 顺序3/4集成验收与明确硬项

产品实现先冻结54d82ff，完整179实际运行176通过/3失败，1647.25s。闭合两项生产回归：同步Geometry/Mesh/Workflow上下文读取提前刷新目录，将一次接受修改拆为两次来源版本；以及GUI重启捕获直接读取compact originRef而遗漏完整origin。修复保留同步批准失效、来源CAS、全文件/祖先SHA和严格恢复；只读刷新排队合并，捕获读取完整broker分页。新跨文档Undo和201个MODEL跨页/末页篡改反例通过。SDK旧内嵌manifest测试改用真实broker，未放宽数值阈值。

实现冻结c83bf5a4ebd5fde57420e281c2b1c2a3b63b254b，全量构建及179/179实际回归通过1569.51s。独立审查另外修复私有运行时组装/契约测试缺-B引起字节码污染；实际离线组装与导入计算通过，后续运行时仍0缓存。私有安装build/stage12-integrated-c83bf5a：缩减PATH下安装SDK扫描32.678s、接受步调参20.656s、MeshSDK9项1.880s、科学Windows3/3零skip173.146s。受控GUI测试程序来自测试树，SDK/解释器/Qt/原生工具来自安装目录，不冒称外部干净机器或人测。

逐条审查发现缺少WF14联合故事/几何修改后真实控制以及WF10键盘/主题/DPI证据。补充测试冻结664ea7b43dcebad5f423e85a877f0e12211267e2，只有测试和测试源编译定义变化，产品实现不变；三个受影响完整目标3/3通过104.59s。实际Windows图表16/16（native深浅主题、窄宽、Fit文字、键盘、不同单位分图）2.084s，键盘及双实际原生工作台4/4为23.190s；实际DPR>1被断言。几何联合3/3为23.958s：同GUI会话内梁端117 X3.3→3.333、人工预览应用/真实快照→主被动20接受步，再独立TRI壳网格/显式静态重绑/原生位移视口。两个支持领域使用两份文档，不能冒称同文档CAD几何→网格。主/被动终态q/v误差约1e-15/1e-12，完整20点独立对照限active Y速度；其余时程做完整性/有限值/时网和终态校核。

WF08/WF09/WF14按首批支持范围接受。WF10 A08保留20k首次绘图5秒失败和完整导出/峰值未验；WF11 A02保留真实DeepSeek高effort生成失败及stage/approval=0，受信手写候选不代替模型生成；WF12 A01保留LPAC class46 Win32 87，worker未恢复运行，A02-A09未验证、ordinary OFF。按用户授权记录后跳过，未降低原验收门槛。WF13发布汇合仍planned。工件SHA、版本、安装二进制一致性见docs/acceptance/integrated-20261010.json，原失败保留在build。只做本地提交，不push，用户配置/skill/凭据/记忆不改。

## 2026-10-11 控制工作台 CW-02 原生静力基础验收

- 用户主场景确定为主动压电杆替换桁架杆/其中一段，贴片另作为梁/平板范式。完整14需求/16任务/48验收不缩减，首要目标少操作。
- Model 4ae7188：主动/被动分类、1.3.19兼容持久化与nearest-node Tie完整覆盖；solver ebb196f：三维混合静力装配、无普通杆假电端口、精确输入与结果一致性、独立斜杆/异材料串联/闭合三角参考及RAM/RBM实际CLI。
- 全目标Release构建exit0，完整required CTest180/180、0fail、1499.60s；最后受影响9/9及实际CLI目标通过。独立源码/验收审查无新confirmed material问题。raw JUnit被CTest相对test-dir写到build/vs2026/build/...，原样归档并验证SHA，不重跑或伪造XML。
- Catalog bce470e独立24/24，合成2材料/10器件、严格SI/本构转换。私有安装Python3.13.5目录/solver SHA一致，缩减PATH help通过。真实native目录绑定、校准数据、干净机器/新GUI仍未验证。
- CW-02 done；CW-06仅A01 pass，面投影/插值/功/收敛保持pending；CW-04 in_progress，productComplete=false。报告docs/acceptance/CW-foundation-20261011.json与CW-02.md/CW-06-A01.md。
- 下一CW03/CW07共同动力设计冻结并独立审查，明确端口功与内部能、有效索长固定质量、解析anchor运动、左右加速度及precommit checkpoint。旧postcommit observer不满足新存储失败回滚保证。pure mixed assembly首批writer仅新增三个文件，root统一接线和验收，不把kernel当成完整动力产品。

## 2026-10-30 current-source phase checkpoint and handoff

- Reviewed the recent GitHub Windows Regression notification. It failed before configuration because the Qt composite action looked for its bundled JavaScript at the wrong root. The workflow now calls the pinned upstream `action/` entry point; the next hosted run after push must verify it.
- Refreshed personal `origin` and `upstream` refs. The personal feature branch was at `9ea1d515`; it contains `origin/main` and has diverged from upstream main. Six submodule commits are published and match their root gitlinks; no upstream merge is part of this phase.
- Incremental Windows Release build after final edits passed. Partial targeted tests passed for the Agent runner, extension widget, external piezo integration, and focused dock resizing. Failure-diagnostic snapshot opening and durable native-job registry locking still fail in focused model-operation tests; two scientific Python tests need a clean private runtime. Full CTest remains 239/246 from the pre-final-fix baseline.
- Kept meaningful future work visible: the 180-slot comparison is waived only for this course submission and remains unperformed research work; Case B live provider evidence, complete installed GUI flow, Linux build, dependency/contribution review, and final release qualification are open. Do not mark those plans obsolete or accepted.
- Updated the portable checkpoint, documentation index, and continuation records. Curated interaction/decision records under `docs/` are the cross-device history; raw attempt data, profiles, and logs stay in ignored `artifacts/` or `.planning/`.

## 2026-10-13 PIEZO_CONTROL_DELIVERY 跨平台交接与 v39 首失败

- 用户确认从断点持续推进到功能完整交付，重置卡只在剩余 1% 时使用；20k 合并进程树内存上限为 512 MB；GUI 需兼顾视觉平衡与操作便利。任务调度 Luna 优先、明确困难才 Sol medium、独立 reviewer 使用 Sol medium。用户选临时地址且未配置域名，并确认旧聊天已停止、由当前工作区接管。
- 新增 `docs/PIEZO_CONTROL_CONTINUATION_HANDOFF.md` 汇总当前分支/提交/脏工作区、冻结范围、验收数字、审计路径、当前断点、分步接续顺序和可复制提示；新增 `docs/PIEZO_CONTROL_INTERACTION_LOG.md` 记录上述明确决策与交互事实。原项目 `docs/CONVERSATIONS.md` 是产品对话组件设计文档，不作为开发聊天记录。`docs/PIEZO_CONTROL_DELIVERY.md`、计划 JSON 与性能合同均未修改。
- 最新 Release CTest mirror v39 baseline 在 47 个目标中的第 27 个停止：26 pass，`tst_workflow_extensions` 1 fail。实际失败为 `test_extension_lane_hot_activation_and_pinned_run_survive` 的 `AssertionError: extension job did not finish`；10 tests 耗时 98.246s。candidate `NOT_STARTED`，无 CMake build/cold measurement。失败后 221 项 runner/source/package 完整性核对通过。完整原始日志和 immutable partial receipt 路径见新 handoff；后续需先复现并诊断此失败，不能放宽超时冒充通过。
- 最新 v33 轴向 20k Full 的已核实门槛：20,000 FE steps + 初始记录完成；原qualification仍 `INVALID/Wrong family`；首图 8.6760589s fail，完整 CSV 9.3348445s / 20,001 行 pass，核实进程树峰值 517,238,784 bytes fail。梁/板完整 20k GUI 资格未完成；全部 36 项仍 pending，不能报告产品交付完成。
- 本次状态快照根 HEAD `9ea1d51545c52a0adc48c62bc1a65b68775c9ff3`，quota 文件记录于 2026-10-13 10:03:40 Asia/Singapore 为 8% remaining、0 cards；监控 PID 22236 在 10:05 仍存活。无正在执行的子 Agent。本轮未运行产品回归或改动冻结验收门槛。
