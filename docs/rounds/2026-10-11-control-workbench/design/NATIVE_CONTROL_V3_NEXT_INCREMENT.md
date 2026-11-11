# 下一阶段：原生标准测量与本机 v3 控制

本文件是已核查接缝后的下一增量合同，不是已实现或验收声明。前置原生保持输入源为 root `92251e9`、model `6a038b7`、solver `a803738`；其完整回归证据独立保存。原 14 需求、16 任务、48 验收和分别对照 MATLAB/Simulink 的目标保持。

## 默认路径和本次接入面

标准测量首先直连真实 native 接受态：节点位移/速度/加速度、杆件轴力、独立端口电压/电荷、规定自由度完整支座反力。外部配置使用物理 node/member/port ID，内部数组位置由冻结映射处理。需要方向的量先限定全局方向；未实现的旋转、区域量或设备内动态不得只增加下拉选项。

测量从 native state 与新鲜装配抽取，不能读旧显示变量。电压驱动端口的电压可作为动作记录，不宣传为独立变形测量。虚拟传感默认不改变质量；真实设备质量/安装刚度/电子动态另有显式模型和验证。

首批本机 actions 为节点力、独立端口 V/Q、有效索参考长度。V/Q 模式仍在源中固定，不允许试步中切换。规定锚点的解析曲线继续冻结；不把离散位移动作假装成已经实现的实时 C2 锚点控制。

`RStructuralTrussNativeRun::prepare()` 是接入面。新增 provider 候选包含 controller/conditioner/queue/held action 和时序相位，与 physical state 一起封装在 AcceptedState/PreparedStep。`commit()` 继续仅交换接受态指针；不把控制状态放到提交后 observer。现有 `RSolver::runNativeMixed()` 的 checkpoint 最后发布与故障回滚顺序保持。

## 命令来源和完整恢复

冻结命令表与 live v3 provider 必须是显式不同来源；各自有版本和验证。不能删去现有 `actionForStep()`/restart 的完整命令回放，来允许任意外部输入。

源冻结包含标准绑定、单位、采样/控制周期、conditioning/limits、provider 身份、代码/权重及申报 imports/依赖与环境摘要范围。后台求解源不可变；回调后再次检查来源，禁止闭包修改模型绕过命令接口。控制器使用显式 `step(frame,state) -> (actions,next_state)`，不可隐藏可变历史或驱动物理硬件。

restart 新字段必须进入 MODEL 持久化和完整状态指纹：来源模式/版本、provider revision、opaque controller state、delay/hold/noise/conditioner 状态、requested/conditioned/applied、control tick、FE/control phase、采样相位与周期、最后限幅状态及环境摘要。只写 CLI sidecar 不够。

新格式兼容设计保留已有格式 1.3.20 的 frozen-table 默认源/状态身份；非默认新数据的旧版输出必须拒绝。不能增加几个默认字段后让所有旧 native 源指纹静默失效。

首个闭环切片采用 MODEL 内完整控制状态、纯有状态的内置本机 provider 和 N=1。后续真正 Python 函数/C++常驻程序沿同一候选接口接入；内置 provider 验证不替代外部进程握手、超时、旧回复和 imports/环境身份验收。首版控制源从 MODEL 冻结 spec 在 NativeRun 构造期解析，不能依赖构造后的 setter，因为普通 RSolver、CLI preflight 与 checkpoint extractor 都会先建立 adapter。

拟新增格式 1.3.21，1.3.20 读取得到默认 FrozenTable/v1。默认源和默认状态保留原 source-v1/accepted-state-v1 的精确哈希字节；LocalV3/v1 使用新 domain/version，控制 spec 和完整状态全部入指纹。写出旧版时，任何非默认新资料明确拒绝。model 层的持久化量值枚举不能反向依赖 solver 头文件。

LocalV3 初版要求 heldCommands 为空：背景节点力从源保留，作动力是显式增量；电压/电荷和有效索长是唯一所有者的绝对值。恢复时验证 provider/spec 身份、完整控制状态结构与相位、last applied 重建的完整物理输入，并保留新鲜装配、解析支座、端口、反力和能量证明。现有 frozen restore 回放的是输入日程，没有从初态重新积分全部 FE 历史；完整 controller/FE 历史重放只作为独立可选审计，不能加入每次 build/restore 的必需成本。

提交前完整 expected record 比对、外层 MODEL SHA 和 state fingerprint 提供接受态一致性与故障恢复证据，不宣称对任意外来 MODEL 的历史进行认证。控制器的可变历史只通过 step(frame,state) 的 state 参数传入和 next_state 返回，不能隐藏在提交后 observer 或 provider 成员中。

## 时序及物理单位

沿用 `Ts=N*dtFE`、默认 N=1；FE 计数与 control tick 从第一版就分开保存。N>1 的完整资格另有所有 phase 的恢复证明，不沿用 v2 一个 completedSteps 同时代表控制与 FE。

采样先读左侧接受加速度，provider/conditioner 提案后让已有 `trialStep()` 执行右侧加速度刷新和功账。每个控制 tick 只消耗一次 noise/delay/controller 候选；Newton 迭代不重采样。rate limiter 按 FE 物理 dt；delay/noise 按 control tick。首次 FE 子步及控制候选在同一个外层交易提交，存储失败不提前消耗。

外力的背景/作动组合和端口/索长的所有权须显式定义，不让冻结表和 provider 同时不明地覆盖同一目标。限幅与正长度检查覆盖实际动作，requested/conditioned/applied 分开记录。

## 复用边界与交付

复用 typed quantity/unit、conditioner 实现模式、现有常驻进程/JSON framing、控制器例程和公共发布事务。v1 节点力与 v2 准静态 wire semantics 保持；新增独立 v3 transport/SDK。v2 session 的 N=1 复用仍需状态/采样验证，不能直接宣称多速率成立。

第一交付必须贯通 native RModel→RSolver→真实本机控制器→完整 restart→命名结果；没有演示专用求解器。标准器件/材料编译和四卡 GUI 后续沿用这条路径，用户不手写端口映射、序列化、路径或 token。

硬验证包含独立 sample-and-hold 闭环参考、物理 ID 非单调及多端口、单位/缺失/重复动作/超时/旧回复拒绝、完整 controller 状态和采样相位 split、存储故障同 solver 重试、v1/v2 回归。MATLAB 与 Simulink 分别验证实际适配器/工作流，未实际运行的环境只记未测。

密集日程 replay/源哈希/模型复制和结果存储的性能问题继续独立解决；不能拿几步闭环或纯 driver 测试关闭 20k 首图/CSV/内存门槛。
