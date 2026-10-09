# 集成工作流范围说明

本文面向使用者和集成开发者，概括 WF09、WF10、WF11 与 WF14 的首批接口和适用边界。2026-10-10，产品实现 c83bf5a 完整 179/179 回归及本机私有安装故事通过；补充验收测试 664ea7b 的三个受影响完整目标通过。WF09/WF14 在下述支持范围内接受，WF10/WF11 保留明确硬项，WF12 隔离未通过；这不表示 WF13 的完整产品发布汇合验收完成。代码与工件哈希见 [集成证据清单](acceptance/integrated-20261010.json)。

## 共用操作规则

工作流查询 capability/schema 后，以已登记来源、文档/作业身份和内容 SHA-256 建立操作上下文。修改类操作先生成绑定来源 revision、规范化参数和预览 SHA-256 的预览；宿主展示后由使用者明确批准该预览，执行时再次核对来源。Agent 可以查询和提议，不能自行批准；没有自动批准入口。过期来源或内容变化时应重新预览。

操作记录区分预览、批准、提交和接受。只有接受收据及其接受历史能证明修改已生效；排队或冻结中的请求不算生效。运行、结果和来源 SHA 各自保留身份，不用显示名称或旧结果替代实际来源。

## WF09：原生求解中的接受步调参

适用于已登记的原生线性杆/梁运行来源、单个物理 `NodalForce` 执行器和已验证的标量速度反馈控制器。运行中的 MODEL、基础 `controllerRevision`、算法、拓扑和控制器脚本保持冻结；热更新只改变独立版本化的控制参数。

| SDK 操作 | 用途 |
| --- | --- |
| `runtime.parameters.query` | 查询运行参数、提案及接受历史 |
| `runtime.parameters.preview` | 预览 `velocityGainNsm` 与 `actuatorLimits` |
| `runtime.parameters.submit` | 携带宿主签发的批准令牌提交指定 `proposalId` |
| `runtime.execution.pause/resume/cancel` | 控制已登记运行 |

增益单位为 N·s/m，执行器力为 N，变化率为 N/s。预览绑定参数摘要和来源；接受记录包含参数版本/哈希以及 `appliedSeq`、`acceptedTimeS`。排队或冻结不代表已应用。编码为零的变化率只在原来源本来就是无限变化率时表示该模式；原来源零变化率表示保持力，当前不支持热修改。此接口不覆盖任意模型、任意执行器或全部反馈控制器。

## WF10：原生长序列工作台与只读通道

对成功登记的原生作业及其工件进行只读查询。SDK 提供 `native_result`、`series_sources`、`series_plot`、`series_raw`、`series_export` 和 `series_export_chunk`；对应操作为 `result.catalog.query`、`result.channels.query`、`result.series.query`、`result.summary.query`、`result.plot.sources.query`、`result.plot.query`、`result.raw.query`、`result.raw.export` 与 `result.raw.export.chunk`。查询固定作业、模式、工件、通道和来源哈希，并在读取前后复核身份与字节。

每次读取最多 16 个通道、10,000 帧和 32 MiB 快照。来源明确标记为原生数据、独立参考或派生能量；接受步累计控制功使用 `control:work`，被动运行不补造零控制功。频谱、误差、相图和完整 CSV 按完整数组计算/导出；图表最多显示 1,000 个缩减点，显示缩减不改变原始数据。完整导出可分块读取。

**长序列性能硬项仍未通过。** 两个实际原生作业已生成并接受合计 20,000 条记录，但首幅图未达到 5 秒门槛，SDK 在原 30 秒截止时超时；完整读取/导出的 30 秒门槛和完整峰值内存尚无通过测量。原阈值保留。

## WF11：可信科学扩展 v2 与私有运行时

科学扩展协议为 `range.extension/v2`。输入为均匀时间网格上的 2–10,000 个样本、1–4 个通道；各通道必须声明 quantity、axis 和与 quantity 匹配的单位：displacement=m、velocity=m/s、acceleration=`m/s^2`、force=N、energy/controlWork=J、dimensionless=1。时间单位为 s，频率为 Hz。当前只支持完整复数 RFFT、端点正确的单边幅值/PSD、矩形或周期 Hann 窗、可选去均值，以及因果、零初值的一阶 Butterworth 低通。滤波顺序为先滤波、再去均值、最后加窗；低通截止频率使用 Hz。结果包括 filtered time、amplitude、PSD、real、imag、phase；分页显示上限为 1,000 点，底层结果保留完整数组。

扩展运行时先使用 `extension.runtime.verify` 启动后台全文件字节校验，再通过 `extension.runtime.verification.query` 查询关联验证。只有当前环境下的 `ready` 收据可继续 v2 预览、批准和执行；这张收据证明校验时的完整文件及环境戳状态，不是对运行期间 DLL 的持续监控。私有 CPython/NumPy/SciPy 运行时按锁定文件和哈希准备；受信开发模式不等同于操作系统沙箱。

**真实提供商生成验收未通过。** 受信科学核心和手写候选的验证故事不等于真实提供商自主生成；最近记录的高 effort 试次未到 typed input inspection，stage 与 approval 均为零。普通模式的生成代码执行保持关闭：WF12 的 LPAC 身份/能力读取在 `TokenIsLessPrivilegedAppContainer` class 46 查询中返回 Win32 87，严格验收未通过；没有回退为普通进程执行。

## WF14：几何、网格和静态重绑

几何命令 `geometry.node_coordinates.set` 使用显式物理节点 ID；`geometry.rigid_transform` 使用显式中心、轴、角度和位移。首批支持范围为 POINT、TRUSS1、BEAM1，坐标单位为 m，刚体轴角单位为 deg，并保留全局方向定义。

网格命令为 `mesh.generate`、`mesh.accept`、`mesh.cancel` 和只读 `mesh.job.query`。当前网格范围是单一连通、凸、向外闭合的 TRI1 壳与均匀材料，生成线性 TETRA1；不包括通用 CAD、非凸或多区域自动网格，以及猜测跨网格映射。worker 使用冻结输入，并绑定来源、参数及固定程序的 SHA-256。

人工接受网格会从原来源创建独立文档；新物理节点/单元 ID 与旧网格不交叉，旧载荷、控制、结果、ROM 和派生映射清空，分析任务重置为 `NONE`。接受网格不会自动创建静力模型。随后必须显式预览并批准 `mesh.rebind.static`，为经验证的未绑定四面体网格指定 SI 材料、全局节点支持/力和原生静力任务；单位分别为 E：Pa、泊松比：1、密度：kg/m³、位移：m、力：N。重绑预览构造候选，人工批准执行后原子应用到当前文档，不求解或发布结果；可用 Undo/Redo 恢复有哈希绑定的历史状态。

静力求解及结果检查必须读取新文档产生的真实原生结果来源。旧网格结果仍属于旧来源，不能显示成新网格结果；能力描述或预览报告也不能替代求解结果。

## 验收与硬项记录

- [WF09 接受步调参](acceptance/WF-09.md)
- [WF10 长序列工作台](acceptance/WF-10.md)：[20k 性能硬项记录](agent_playbooks/wf10_long_series_unclosed.md)
- [WF11 科学扩展 v2](acceptance/WF-11.md)：[真实提供商生成未闭合记录](agent_playbooks/wf11_provider_generation_unclosed.md)
- [WF12 LPAC 隔离验收](acceptance/WF-12.md)：class 46 未通过，普通模式执行保持关闭
- [WF14 几何、网格与静态重绑](acceptance/WF-14.md)
- 客户端和私有 Python 运行边界见 [工作流 SDK 手册](WORKFLOW_SDK.md)；接口版本、预览批准和状态语义见 [工作流契约](WORKFLOW_CONTRACT.md)。
