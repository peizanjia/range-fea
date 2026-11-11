# Range FEA 源码审查续作汇总（2026-11-05）

根基线 `e3d527781add4cd388276392f63a7c9ee9bd5ec2`，分支 `feature/structural-profile`。本次从暂停工作树续作，最初10个暂停文件的SHA-256全部匹配；原修补与 `.idea/` 保留。没有提交、推送或更新子模块gitlink。

**本会话没有运行CMake、编译、构建、CTest、产品或真实Provider。全部新增C++回归仍未编译运行。** 另一会话的Qt/Linux兼容修复和用户手动构建独立记录，不转化为本审计通过。AGENTS.md已撤销；旧聊天/摘要不作为本轮操作授权。

## 覆盖

历史清单仍为416个路径，来源是根 `9ea1d51..e3d5277` 和对应子模块变更，并非可精确按10月15日或模型名称归属的全部代码。各状态如下。

| 台账深度 | 数量 |
|---|---:|
| reviewed_static：整文件静态/继承的已记录静态范围 | 321 |
| reviewed_diff：完整批次差异审查 | 51 |
| reviewed_changed_hunks：完整变更段与必要调用上下文 | 42 |
| partial：部分审查或源快照变化尚未闭合 | 2 |

上述数字不能相加称为“416个整文件审完”，也不表示无缺陷或运行验收通过。`reviewed_static`中保留上轮已明确标记的继承范围；数值域58项为继承状态，其中12项本轮补充重核、46项未重新全审。大型旧文件的未改正文不因本批次变更段审查自动得到资格。

当前总状态：`resumed_review_incomplete`。未完成/源漂移路径以及各分报告身份见 [整合检查](../../../../artifacts/bugfix-audit-20261104/resume-20261105/coverage-integration.json)；当前源哈希、清单计数和gitlink只读核验见 [一致性检查](../../../../artifacts/bugfix-audit-20261104/resume-20261105/final-integrity-check.json)。哈希检查本身不是逻辑审查。

## 已写入的确定修补

| 工作线 | 本轮主要修补 | 详细证据 |
|---|---|---|
| GUI/Agent与作业 | 孤儿作业interrupted提交失败不发布内存候选；实验观察/捕获同步回调隔离旧task；非法外部JSON不可沿用旧state；异步回复绑定完整草稿与源；v1序号收窄前守门；编码电极通道只读查询兼容 | [根域记录](root.md) |
| 可信力学/验证/实验 | 初值enabled/单位/完整时间键一致性；模态有限输入溢出拒绝；授权网格上界先限幅再收窄；v1周期checkpoint恢复保留10→20合法间隔 | [应用记录](application.md) |
| 数值子模块 | 分层恢复绑定源初始电压及无控制动作；通用积分器完整能量账本有限性/失败原子性；保留并增强trace策略和publisher冻结回归 | [数值记录](numerical_submodules.md) |
| 原生checkpoint | 最后certifier回调置于磁盘authority终次校验之前；latest指针在回调之后核对；补故障/回滚/同序号重试及周期恢复源 | [原生CLI记录](../../../../artifacts/bugfix-audit-20261104/resume-20261105/native-cli/review.md) |
| Python工作流 | 极端有限table跨度插值；外部进程异常清理；capabilities与规范百分号编码通道语法一致 | [工作流逐文件记录](../../../../artifacts/bugfix-audit-20261104/resume-20261105/workflow/review.json) |
| 打包与Windows工具 | 同窗工作区布局证据；worker/sampler标量身份；诊断共享依赖；finalizer同字节hash/parse及后续producer审查修补 | [打包记录](packaging.md) |

还补了测试自身的生命周期、同步观察时序和正负夹具目标守门。已确认的夹具问题不自动解释旧七个Hosted失败；逐项修补和未修竞态以GUI测试报告为准。没有降低物理门槛、修改物理模型或追认旧验收。

## 实际检查及限制

- 独立标准库算术支持静力跨行尺度、瞬态/模态有限输入溢出、积分器能量-only溢出及序号边界机制；没有执行C++ broker/solver。
- 已保存Python定向unittest、mock CTest元数据/源码guard及独立编码oracle日志。工作流4个schema断言导入了系统NumPy 2.3.5但未做科学计算；与冻结数值环境严格区分。
- 原始 `git diff --check` 对保留CRLF源码的新行报CR-at-EOL；单次 `core.whitespace=cr-at-eol` 检查通过，真实行尾空格/tab另核对；没有改Git配置或重写冻结换行。
- JSON/计数/逐文件hash和gitlink一致性检查只认证记录与当前字节的对应关系。API中断的追加轮未冒充完成，剩余范围重新有界接管。

## 未关闭事项

1. 热trace对不合作外部进程同尺寸改写旧前缀的完整性边界仍开放；冷读取全量校验会拒绝。owned handle/不可变分段设计须与平台共享语义和20001条性能共同验证，未引入O(n²)每步全扫。
2. 历史锁释放、旧Win32 error3、终态失败后的显式核对/恢复产品流程，仍缺原始运行链；不猜根因。
3. 新增C++、Windows PowerShell/C#/UIA、真实Provider、完整安装模型→报告以及冻结运行时回归均未执行。当前Linux科学环境缺SciPy，不能据轻量绿灯升级验收。
4. PC当前3/36，不是旧摘要33/36；差异原因未证实。PC00原receipt/JUnit、Stage7评估工具、Stage8草稿及最终来源/授权材料缺失。README现行入口已删除未经证据支持的“不同基线”解释，原历史记录保留。

下一阶段的具体验证目标、平台/依赖、隔离路径和预期均已单列在 [运行验证矩阵](RUNTIME_VALIDATION_20261105.md)。这些是本任务尚未获准执行的验证部分，不是已经通过的验收。
