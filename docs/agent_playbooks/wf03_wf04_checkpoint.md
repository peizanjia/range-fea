# WF03/WF04 断点交接：2026-10-08

用户明确要求新建 session 后继续，本 session 已停止功能推进并收拢工作。此文件记录未验收 WIP，不能据已注册测试或部分绿色结果宣称交付。

## 1. 读取顺序与已接受基线

1. 最新用户指令、`AGENTS.md`、`.agents/skills/astra-orchestrator/SKILL.md`。
2. `HANDOFF.md` 顶部及本文件。
3. `docs/workflow_delivery_plan.json`、`WORKFLOW_DELIVERY_PLAN.md` WF03/04/05 条款。
4. `docs/CURRENT_MODEL_WORKFLOW.md`：支持矩阵、来源桥接、接口方向和文件所有权。
5. 当前源代码和下面未闭合发现，再分配实现。

分支 `feature/structural-profile`。WF01 验收 `58a5405`；WF02 基础 `dcd11ef`、最终代码 `1ca3cd08bb1f39ccb6ad5932f571ddf1cb07255d`、验收 `6145f34`；WF03/04 启动记录 `8a2ffb0`。模型库 `9d6a60ed42bab535ad5b8bda193092d3c38deed4`、MODEL 1.3.18。最后整体通过的产品代码仍是 WF02，不是当前 WIP。

本次退出保存 solver 子模块 WIP `e535296`（observer/测试），根 WIP 提交可用 `git log --oneline --grep='preserve WF-03/WF-04 handoff'` 查找；不是验收冻结。未 push，所有子代理已结束，未遗留本 session 构建/测试进程。

WF02 证据 `docs/acceptance/WF-02.md`：全目标通过、115/115（282.41s）、原生 Windows 14/14（无跳过）、安装私有 SDK/CLI/知识查询通过。安装树 `build/stage12-install` 仍对应该验收范围；没有安装当前 WF03/04。

WF03/04 状态继续 `in_progress`，无完成证据；WF05 未开始。用户原授权顺序 WF01→WF02→WF04→WF05，追加允许 WF02 完成后启动 WF03；继续时按依赖完成 WF03/04 后再 WF05。WF06/其它任务 eligible 不等于本次已启动。

## 2. 已写入的 WIP 与接口

### WF03

- `native/import_model.*` + `native/main.cpp`：`describe --input FILE --expected-sha SHA`，协议 `range.native-import/v1`；`prepare-source --input FILE --expected-sha SHA --output FILE --steps N --dt S [--output-frequency N] [--passive]`。
- describe 返回实际 nodes/physicalId/coordinates/初值四向量、elements 材料/截面、pointElements、supports/freeDofs、全局 ports、dynamics、originalControl。局部 DOF 标签不是全局轴：原四杆导出的 `UZ` 实际是全局 X。
- 原生 `RStructuralTarget.nodeId` 实际是节点数组位置。prepare-source 从 physicalId 定位 position，核对后赋给 native target；不改变 ModelLib/solver 的旧语义。
- fresh active 私有副本重建 actuator `source-tip-force` 与三 sensors `source-tip-displacement/velocity/acceleration`，controllerId `stage9-velocity-feedback`；保留 source seed，清旧 conditioning。passive 清控制映射。已有 GUI 源不被修改。
- 新 `--output-frequency`：旧 prepare 默认 steps；prepare-source 默认 1；source resume 显式传 1。prepare-source 成功响应仍 `range.workflow.native/v1`，包含 origin sourceSha256、preparedSha256/sha256、sourcePath/sourceFiles、resume=false、output/mapping。
- `model_sources.py`：`register_snapshot_source(workspace,catalog_root,snapshot_ref,native_path)`、`read_model_source`、`list_model_sources`。snapshot_ref 是 projectId/PID/sourceRevision/modelSha256/instanceRef 五字段；native_path 由可信服务配置显式注入，不从 env/PATH 选执行文件。
- registry 位于 workspace/model-sources/source-UUID，index 绑定不可变 manifest/model hashes。首次注册核对原目录前后、复制和 native preflight；以后只核对登记副本/ledger，不要求已编辑的 GUI B 版仍等于 A。path/manifestPath 是内部可信字段，不向模型提供任意路径参数。
- `jobs.py/source_reference.py/stage9_velocity_controller.py`：新增 sourceId 分支，binding 键 `modelInput`、`sourceManifest`，从源独立组装解析 M/K 并比较 native exporter，投影实际 q0/v0，支持可选 `--initial-v-json`，保存 prepared 输入与 solver working copy 的不同哈希，续算保留来源。
- `service.py`（根整合）：configure 可选 sourceId；显式 model 无 sourceId 选择模板，省略二者保留当前 source；project modelBinding 区分 template/native-snapshot。新增 model.source.register/query、model.sources.query；注册操作 trustedOwnerOnly 当前仅是描述性标记，见下方安全边界待复核。
- `WorkflowWidget::setSession(Session*)` 和 MainWindow 接线；“Use current document snapshot”实际按钮、来源选择、版本/哈希/端口展示。点击前有拓扑数量预检，随后在 GUI 捕获快照并异步登记；configure 批准前及提交前重查 source runtime/project/revision/hash/draft。尚未编译/窗口验证。

### WF04

- `RNativeResultBroker`（QtCore）与 `range-native-results` 文件 CLI：publishResult(resultDir,latestPath,provenance)，query(manifestPath,expectedSha,operation,args)，catalog/channels/series/summary。
- manifest `range.native-results/v1` 位于 resultDir/native-result.manifest.json，记录 source/runtime/control、logicalLatest 与 terminalModel、实际 numbered frames、节点物理 ID 映射、实际变量。静态/模态明确类型，不当成时程。
- channelId `node:<physicalId>:<quantity>:<X|Y|Z>`，单位来自原生 RVariable，实际节点运动学。编号最少五位但可增长；时间从接受 checkpoint，稀疏 outputFrequency 缺步不插值。
- GUI SolverTask/Manager 新增 prequeue provenance 和 copied manifest signal/registry，冻结 solver 字节和声明的 runtime/normalized args；controlVersionSha 绑定 input snapshot+control declaration+args。scope 只声明软件输入，不是整个 OS/外部控制器代码证明。
- solver 库新增 `RSolverResultFrame`、`RSolver::setResultFrameObserver`；泛型 writeResults 返回后排队、接受事务完成后发通知；Heat→Stress enclosing transaction 和失败步丢弃排队通知。
- `NativeResultJournal`：可信环境 RANGE_NATIVE_JOB_ID、RANGE_NATIVE_RESULT_JOURNAL_PATH、RANGE_NATIVE_RESULT_DIRECTORY；private 16MiB JSONL `range.native-result-frame/v1`，保存真实已发布帧的 path/seq/recordNo/time/SHA。不是 progress，不补计划步骤。
- 尚未接通：GUI 父任务启用/轮询 live result journal、运行中 broker 查询；Host/Agent 结果 API、通用 GUI 原生曲线/表格/冻结 CSV、Python native result SDK、控制工作流的结果 manifest 桥接及扩展 native source adapter。

## 3. 必须先修复的独立审查发现

以下均为未闭合，不因已有单元测试通过而降级。行号可能随修改移动，以符号为准。

| ID | 级别 | 问题与定位 | 修复/回归要求 |
| --- | --- | --- | --- |
| W3-01 | P2 | import_model.cpp describeModel nodes.position 为 X 排序 rank；source_reference.py build_source_reference 比较 native nodePosition 的数组 index | 保存真实内部 index 或独立字段，按 physicalId 映射；registry 允许合法位置置换；新增非空间顺序节点存储夹具 |
| W3-02 | P2 | describe 未声明初值填 0，prepareSource 却保留 RResults；native fresh 会恢复已保存 displacement/rotation/velocity/angular velocity 后再按显式 IC 覆盖 | 对 fresh source 拒绝已保存初态，或明确批准的清除策略；不能 reference 用 0、native 用旧变量；新增保存变量且 IC 不覆盖的反例 |
| W3-03 | P2 | prepareSource actuator 限幅 ±1e6 N，replay _trajectory 未限幅 | 使用实际已验证 actuator bounds 做 replay 或在批准前拒绝不可支持源；新增饱和初速度用例 |
| W3-04 | P2 | jobs._run_imported_native_workflow mode=both 对 passiveEnergy=0 判失败 | 合法全零 q0/v0 保持成功，ratio 明确 undefined/不可用；GUI 不能把 null 当 0% |
| W4-01 | P1 | validNativeResultManifestDescriptor 只核对 LINK SHA，不把 terminalModel SHA/path 绑定到 WF02 resultSourceBinding；readback X 后替换同名 MODEL Y 可让 manifest 对应 Y | worker 发布后及 manager success gate 比较/重新 hash terminal 与已登记 X；注入 resultArtifactCreated→manifest 间替换，拒绝成功 |
| W4-02 | P2 | validateManifestShape 只核查非 terminal 帧路径，不核所有 frame SHA | immutable artifact 查询边界核对所有声明帧；修改非请求页帧时 catalog/channels/另一页 series 也须拒绝，或经新范围决策明确验证作用域后改协议 |
| W4-03 | P2 | copyRestartHistoryToResult 复制旧帧，manifest 全部标为新 job/source/runtime 来源 | 显式 reused-history index 和每帧 origin/原 job-source-hash；续算来源不能冒充本次产生 |
| W4-04 | P2 | hasNativeBeam 后给所有节点列角量，truss-only 节点的 solver 散射默认 0 被当成角时程 | 按节点真实 element/DOF availability 排除无旋转 DOF 的 angular channels；混合梁/杆节点反例 |
| W4-05 | P2 | nativeResultCatalogExpected 登记但终态不使用；任何 UNSUPPORTED_ANALYSIS_MODE 都能绕过 manifest 要求 | 仅 frozen input 本就 unsupported 时豁免；应支持的 dynamics 结果变 unsupported 必须失败；伪 copied signal/错误分析输出反例 |

其它整合检查：AgentTaskRunner 当前是固定工具白名单，没有任意 backend RPC；workflow_propose 只允许指定 commands/精确参数。`model.source.register` 不应通过模型驱动调用 catalogRoot；建议像 host.approve 一样作为可信宿主入口，不依赖 capabilities 中的 trustedOwnerOnly 标记。注册已写 immutable source 后 `_commit` 审计失败需明确 source 已存在，避免伪称未发生。GUI 注册可与待批准预览交叠，须核对 revision/旧批准失效；重开后 sources selector 尚未从 registry 恢复，job tooltip 的 modelBinding 元数据也待统一。

## 4. 已验证与未验证

- `WF-03/preflight-fixed-tests.log`：native import（describe/环境分类修复时版本）、registry mock、旧 native adapter 三项 CTest 3/3，3.76s。此后 prepare-source/输出频率及 GUI 又变化，不作为最新完整通过。
- `WF-04/broker-fixed-build.log`、`broker-tests.log`：broker/CLI 构建和该 QtTest 通过，0.77s；上述材料性审查发现仍未修复。
- `WF-03/import-runtime-build.log`：当时 native/fixture/fea-solver/observer/journal 目标构建完成；observer/journal 测试没有运行。native worker 后续仍改过 prepare-source，GUI 也随后落代码，必须重新构建。
- Worker 报告 source_reference 4/4、model_sources 7/7，Python compilation通过；前者独立公式/初速度局部证据，后者 mock native describe。实际 imported FE run/resume 尚未跑。
- 收尾仅执行 Python py_compile、plan validator、required inventory 和 diff-check，均通过；122 required/122 registered，不等于 122 passed。日志 `build/audit-20261008/handoff-{plan,inventory}.json`。
- 没有当前 WIP 的全目标/完整回归、Windows GUI、安装、远程大模型或最终审查闭合。最近可用安装树仍是已验收 WF02。

## 5. 继续执行

```powershell
git status --short
git log -4 --oneline
git submodule status
python scripts/ci/verify_workflow_plan.py --next --json
$env:CL='/MP4'
cmake --build build/vs2026 --config Release --parallel 2
```

先按 W3/W4 发现分配明确文件所有权修复；共享 build/vs2026 构建、CTest、安装始终根串行。原生 reader target.nodeId 的 position 语义、公开 physicalId 和数学轴必须分开。

```powershell
ctest --test-dir build/vs2026 -C Release -R '^(tst_workflow_native_import|tst_workflow_model_sources|tst_workflow_source_reference|tst_workflow_imported_runtime|tst_native_result_broker|tst_native_result_frame_observer|tst_native_result_journal|tst_native_job_registry|tst_workflow_runtime)$' --output-on-failure
python scripts/ci/verify_required_ctest.py --build-dir build/vs2026 --configuration Release --manifest scripts/ci/required-ctest-tests.json --inventory-out build/required-ctest-inventory.json
```

修复后再接通剩余数据面/UI/SDK/Agent/扩展，完成 A01–A08，最后全目标+完整必跑+真实窗口+安装+Sol high 复审；代码冻结后写 acceptance evidence。WF05 待依赖完成，不提前启动。

环境：Windows/PowerShell，Qt `D:/ProgramData/Qt/6.11.1/msvc2022_64`，共享 build/vs2026；用 CL=/MP4 和 --parallel 2。QtTest 不用 -vs；原生窗口需要 Qt bin/plugins 和 QT_QPA_PLATFORM=windows。Python 为现有 anaconda3；安装私有 Python 位于 build/stage12-install/runtime/python/python.exe。

执行子代理 Luna max，reviewer GPT-6.1 Sol high；根模型/effort 由用户选择。不要把用户 `.agents/`、`.codex/`、`AGENTS.md` 或 `.env.local` 混入提交/删除；不 push；不打印 API key。DeepSeek 已由用户配置，但本轮没有远程模型验收。

重置卡：用户已授权仅剩余 1%/0% 时使用。已在真实 1% 处消耗一张，服务 reset 后100%、可用卡由3→2；本轮收尾 probe 为91%/2张，未再消费。工具脚本在忽略目录 WF02/codex_usage_probe.py --stdio --redeem，走官方 account/rateLimits/read→条件消费→再次读取，用持久幂等键。下一 session 先重新读实时额度，不用此数值当当前状态。
