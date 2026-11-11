# 当前文档控制与原生结果：实施冻结契约

2026-10-08；基线：WF-02 已验收（代码 `1ca3cd0`，验收 `6145f34`）。本文服务于 WF-03/04 的实现，不代表这些目标接口已交付；机器状态见 `workflow_delivery_plan.json`，产品调用以运行时 capability 为准。

## 1. 对象与所有权

- GUI 文档由 Session 拥有，runtime target 是 documentId/sessionEpoch/documentRevision；持久来源是 projectId/persistentDocumentId/sourceRevision/modelSha256。
- 控制 workspace 与 GUI project catalog 可以位于不同目录。不得因为 workspace 有模板，就用模板替代当前文档；显式验证、复制当前文档的不可变快照。
- source snapshot hash、prepared active/passive native input hash、result manifest hash各自命名，不互相冒充。instanceRef 在后端仅作来源记录，不授予当前 GUI 执行权限。
- GUI/Agent 的源选择只接受固定文档身份和版本。catalogRoot/path 由可信宿主桥接；产品大模型不能传任意路径、原生字节、Qt 方法或批准令牌。直接 SDK 作为其文件/进程的拥有者显式导入自己的项目快照，不声称接管 GUI。
- 根唯一写 `service.py`、`workflow_widget.*` 和共同 CMake/计划/验收。WF03 拥有 native adapter/import 描述、独立 source registry、`jobs.py/templates.py` 与 reference 初值接线。WF04 拥有结果 broker/CLI、solver manifest/host read API、扩展 native source adapter；不同时写 WF03 文件。

## 2. WF03 首版支持矩阵

| 项 | 支持 | 提交前拒绝 |
| --- | --- | --- |
| 共线杆 | 5 节点、4 TRUSS1 构成沿全局 X 的连通链，保留已有 6 POINT 支承/初值/控制点拓扑；有限正段长，可改 X 坐标、材料及截面值 | 其它拓扑/单元、非共线、自由 DOF 模式变化、缺节点/实体引用 |
| 平面梁 | 2 节点、1 BEAM1、2 POINT，沿全局 X，局部 Y 对齐全局 Y；有限正长度，已验证平面自由 DOF 为 tip UY/RZ | 其它拓扑/取向/自由 DOF、非平面、局部轴无法验证 |
| 支承/约束 | 杆根 XYZ=0、沿链 Y/Z=0；梁根平移/转动全固定，tip UX/UZ/RX/RY=0 | 非零/时变支承、MPC、coupling、prescribed motion、未知条件 |
| 材料/截面 | 源模型的常量 solid E/rho/nu，正杆面积；梁正 A/Iy/Iz/J 及原生应力恢复 Wy/Wz/Wt；逐元素由真实 group 数据解析 | 缺值、非有限/非法物性、温度表/不支持材料、附加物理或未验证点质量 |
| 动力路径 | Stress-only，consistent mass，Newmark gamma=.5/beta=.25；首版 Rayleigh alpha/beta=0、modal ratios 为空 | legacy-only 动力、generalized-alpha/centralDifference/其它参数、额外分析、非线性/热/压电/拉索路径 |
| 初值 | 源条件中的常量位移/速度/转角/角速度，投影到导出的真实物理 DOF；零初值仅采用已声明的原生缺省规则 | 冲突、未知/时变初值、无法解析的轴；已有 checkpoint 或保存的四类运动结果当作 fresh 初值 |
| 控制映射 | 末端全局 X（杆）/Y（梁）的 nodal force N 和 translational velocity m/s，实际 nodePhysicalId/globalDirection 可预览，force ±1e6 N；独立验证 B/Sv 符号和系数 | moment/base-motion/其它方向或单位，缺失/重复/歧义端口 |

物理 node IDs 不硬编码成夹具的 7118…/9277。导出 DOF 的 `nodePhysicalId/globalDirection/unit` 是权威：杆夹具的局部标签 `UZ` 实际方向是全局 X。初值不能套用模板 sine/零速度公式，也不能用 profile 的 IC-enabled 谓词代替真实求解器支持检查。

仅允许矩阵内的源；不支持时定位原因并保持原 workspace 配置，不自动退回模板。扩大任何行须新增独立参考和验收。

## 3. WF03 数据面与工作流

目标原生 preflight 协议 `range.native-import/v1`：输入可信 snapshot 路径和 expected SHA；返回 validated modelKind、节点物理 ID/坐标、元素/截面/材料、支持与初值、动力设置、端口映射、拒绝诊断。读取前后验证源；不运行求解、不把路径来自模型文本。

控制服务新增显式 source 注册和只读查询：可信 GUI/SDK 提交项目快照引用，服务验证 catalog/版本/SHA/受管理路径后复制到 workspace 的 sourceId 目录。注册后配置只使用 opaque sourceId；普通 Agent 的 workflow.configure 参数不得包含路径。模板仍可通过显式 model 选择；缺省配置编辑保留当前 source，不静默重选模板。

run/resume 的 binding 固定 source 文件、描述、controller/analysis 工具及运行环境；准备出的 active/passive 文件单列 SHA。首次 run 必须从该 source 副本产生 M/C/K/B/Sq/Sv/Sa、DOF catalog 和 q0/v0。分段 resume 保留原 source/参考/映射并核对完整接受 checkpoint，不重新读取已经编辑的 GUI B 版。

独立参考有两层：用源几何/材料/截面独立组装杆/梁解析 M/K，与 native exporter 比较；随后以同一源 q0/v0、明确控制采样和 Newmark 约定进行 NumPy replay。不能只用 native M/K 自我验证，也不能复用旧 template 误差报告。既定终态门槛保持：位移/转角 1e-8 m/rad，速度/角速度 1e-7 m/s、rad/s；新增初速度须核查第一步加速度/控制采样顺序。

GUI “使用当前文档快照”入口先作有界支持预检，再冻结并预览来源/映射；批准前重新核对 runtime/persistent 版本、草稿和来源。运行后编辑/关闭源不改变输入；结果明确显示来源版本及过期状态。无需重启 GUI，也不因新 source 选择停止已有任务。

## 4. WF04 真实结果与来源

`RResults` 没有 time 维度；`RTimeSolver.times` 只是计划网格；checkpoint 只是一条最后接受态。普通 FE 历史来自按 outputFrequency 发布的 numbered MODEL frames；缺步明确稀疏，禁止插值冒充原生接受轨迹。active control telemetry 仅含控制端口采样，passive 不具有该流；phase progress 不是曲线。

首版运动学通道：Stress-only structural dynamics 的 node displacement/velocity/acceleration，存在 frame DOF 时的 rotation/angular velocity/angular acceleration。稳定目标为 nodePhysicalId，轴为全局 XYZ，单位从 RVariable 类型取得；缺字段、维数/finite/目标不匹配不可补零。modal/frequency/static 明确类型化，拒绝把它们当时间序列；复杂局部应力/力矩不在首版时程矩阵内。

成功 result manifest (`range.native-results/v1`) 绑定 jobId、origin source ref、input/package hashes、controller identity/version、明确作用域的 runtime fingerprint、analysis/mode，以及每帧 recordNo/time/relativePath/SHA/物理节点映射。latest logical LINK SHA 与 terminal MODEL SHA分开；WF02 resultSourceBinding 继续保留。所有帧只在该 job result 目录内，完成 manifest 原子发布并登记其 SHA；不能事后扫描任意目录再标成已验证输出。

live 数据仅取已完整发布并核对的输出帧，snapshot/cursor 固定 job/manifest generation；正在写的尾帧标记 pending，不作为有效样本。若新增 accepted-step producer，必须接通实际 solver 接受/结果提交回调并逐量声明数据来源；不能从 scheduled times、checkpoint、reference 或 progress 补造。完成后改为不可变 manifest；原始尝试、稀疏覆盖和复用 restart-history 的来源分别保留。

目标只读操作：result catalog、channel catalog、bounded series、summary，按 opaque artifactId/channelId/source version 调用；GUI 表格、曲线、冻结 CSV 和 Agent 引用同一个 broker。不得让模型传路径。Standalone CLI/SDK 是文件作用域，只有可信 host/workspace 的 registered manifest 才能声明其 job 成功来源。

sourceKind 明确区分 native active/passive、`independent-reference-history` 和 caller data。比较需相同物理模型来源、时间网格/实际覆盖、单位、可比通道和方向。扩展的 native 输入经 source adapter 每次重验 manifest/帧/hash/完成状态；保留 reference/caller 兼容，不能重命名 reference replay 为 native。

## 5. 质量门槛

WF03：改源 E/几何/q0/v0 后的 M/K、真实 FE、独立 reference 与分段连续等价；错误拓扑/方向/支承/初值、篡改/重复批准/取消/关闭反例；非默认当前文档的真实 GUI+安装 SDK 故事。

WF04：真实 FE 帧与独立 reference 对照；分页/尾部/缺口/切换/重连/NaN/源篡改反例；扩展从真实 native 时程计算并与独立统计一致；同源 GUI/CSV/Agent 与安装 SDK 闭环。

每个任务分别全目标构建、完整必跑、独立 Sol high 审查、原生窗口及对应安装验证，冻结代码后才写完整 acceptance evidence/标为 done。WF05 要在 WF04 后做跨重启账本、发布核对与周期 checkpoint 恢复，不能用本任务的内存 job/manifest 替代。
