# WF09 接受步事务实施断点

2026-10-09。设计/纯codec准备，不是WF09验收或已发布能力。独立只读Sol high任务wf09_transaction_design已核对现有步事务和入口；根决定保留静态模型/控制指纹，另设运行参数状态。开发设计不安装为产品知识。

## 固定与可变身份

baseControllerRevision、原始gain artifact、MODEL/control fingerprint、积分器/时间网格、传感器/执行器拓扑固定。运行中允许首批velocityGainNsm与固定actuator ID的幅值/变化率overlay；不得把热值写回controller.gain全局或原始gain文件。

新的RStructuralControlRuntimeParameters纯状态包括current revision/hash/runtimeControllerRevision和最多128条已接受变更。待用提案、批准令牌、未提交指针不进checkpoint。幅值/变化率只在原始允许范围内，原rate=0表示不限；新rate=0不能放宽原正rate。新幅值与上一接受力±rate×dt的区间无交集时拒绝，不重置旧力作弊。

参数SHA使用可由Python struct复现的二进制v1编码：ASCII marker `range.structural-runtime-parameters/hash/v1`加NUL，baseSHA原32字节，revision u64BE，gain binary64BE，actuator count u32BE；按ID排序，每项UTF8 ID长度u32BE+ID bytes+min/max/rate binary64BE。负零规范为正零。runtimeControllerRevision为两个ASCII摘要字符串分别u32BE长度前缀后串接的SHA256。checkpoint组件仍使用codec自身的严格Qt compact JSON字节，不要求Python复制Qt的浮点JSON拼写。

## 求解事务

向现有Observation/Decision/StepRecord追加可选runtime状态；旧callback字段为空时行为保持。在restart/mapping恢复完毕、构造控制观测时取得接受参数。bridge冻结一个候选，以该候选gain执行pure compute；reply须回显step/request、固定身份、parameter revision/hash。solver用同一候选limits限制该次forces。

候选参数与controller/conditioner、q/v/a、force/work和ledger进入同一step snapshot/store/commit/rollback。store发布的MODEL包括候选参数，仅成功commit后成为接受值；compute、solve或结果写失败完整回滚。现有post-commit observer不能回滚已公开步骤，因此只发布authoritative接受收据；收据发布失败应诊断/停止并保留接受状态。

RCCE新增可选第三组件runtimeParametersState。空组件输出原RCCEv1同字节；有组件输出RCCEv2，新reader读两版并验证第三组件。外层checkpoint/v1不必改，MODEL SHA保护参数。必须补旧v1字节fixture、v2截断/尾随/坏版本/篡改及组合大小反例。

## 入口和恢复边界

- 首批热gain的真实产品路径是WorkflowService/jobs.py/controller_host.py处理WF03已登记native source；直接GUI native受控作业目前没有固定executable/argv，不能宣称已支持。现有GUI pause仅启动前queued、uncontrolled Newmark，需真正补live操作。
- 宿主持久预览/批准、单pending queue和idempotency；native bridge只排队，solver事务返回actual appliedSeq/acceptedTime/revision。批准绑定run/attempt/model/base controller与expected parameter revision，不用每步增长的seq作CAS。
- 同proposal ID+hash重发核对既有状态，ID改hash/错误run/旧revision拒绝。取消已成为trial候选的请求不能称已撤销。停止/断连须唤醒paused等待；PAUSE_NEXT在接受边界确认，没有额外接受步。
- 恢复只使用所选checkpoint里的已接受参数与历史。旧attempt在checkpoint之后的接受事件保留为历史，不能升级恢复后的当前参数；pending提案不自动重播。下一版文档/配置与当前run参数分开。
- Hold display只冻结显示，不暂停求解；查询/完整导出保持独立。没有硬实时声明。

## 具体实现所有权和验证

已完成准备worker仅新增runtime_parameters头/src/test；根已补正dt>0、appliedSeq>0、非空actuator，以及返回新接受状态前64KiB组合容量检查。尚未构建/注册/数值验收。根之后拥有RCCE、stress snapshot/limiter/store/restore、CLI transport/gate、Python/job/recovery、GUI/Capability/CMake。不要并发改相同文件。

独立单杆oracle取m=ρAL/3、k=EA/L，已知切换表与rate/amp交集生成f_n，重新计算a_n=(f_n-kq_n-cv_n)/m，再手写Newmark β=1/4、γ=1/2。控制功账按同一步力乘速度梯形积分；积分器外功按平均端点force·位移增量，分别核对现有定义。不得以第二次native运行作唯一oracle；后续增加registered beam矩阵参考和真实GUI故事。

必测：trial/paused/批准延迟更新、重复/错误hash/version/source、非有限/未知actuator/无交集、compute错回显/timeout、solve及late result-write failure完整回滚、接受后/仅pending时中断恢复、旧checkpoint恢复、pause/resume/cancel/disconnect、v1/v2兼容与真实安装入口。当前不把纯codec成功当WF09-A01–A10交付。
