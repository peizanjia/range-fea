# 下一有界增量：原生混合杆索瞬态与接受态重启

2026-10-11。此处是代码核查后的实施合同，不是已实现/通过声明。前置源冻结为 root `16f83bc`、solver `3f4b180`。完整 CW-03/CW-07、v3 控制、多速率和环形天线仍按原交付计划验收。

## 范围决策

先交付真实 RModel / CLI 的固定质量、固定步长、保持型输入瞬态和完整接受态重启。新增独立 mixed setup/路由；旧 cable / piezo 路由保持原语义和拒绝门槛。首批主动杆使用明确 SI 的有效 k/g/钳制 Ce 及独立电极 port ID，不把任意三维材料张量自动降维。连续作动曲线、外部 v3 控制及设备/GUI 自动绑定保留后续。

首批运行命令来源选择冻结的 FE 步索引表：未覆盖步保持上一命令，每个命令在试步开始转换为完整的 `RStructuralTrussDynamicsInputs`；表、端口顺序与初始输入进入源/restart 身份。它是确定性 native 参考与故障注入入口，不冒称控制器或 v1 节点力协议。控制 provider 后续替换命令来源时沿用相同完整候选接口。

主动杆电压/电荷类别在模型中固定；输出按独立 port 身份保存，普通杆没有电学测量。重启保存全精度原生状态，不以有损显示结果作为重启权威；电学结果格式需要实现前单独冻结，但不得占用共享机械节点身份伪装电极网络。

## 文件所有权与接线

| 工作包 | 独占实施面 | 交付边界 |
| --- | --- | --- |
| 模型持久化 | 新 `rml_structural_truss_dynamics_setup.{h,cpp}`，`rml_problem_setup.{h,cpp}`，`rml_file_io.{h,cpp}`，专用 restart record 与相应模型测试 | 完整且互斥的 passive/active/cable 分类、有效参数/独立端口/固定质量来源、步索引命令/选项；新格式门槛、旧版默认禁用、降版丢字段拒绝 |
| 适配与求解 | 新 `RStructuralTrussDynamicsModelAdapter` / `RStructuralTrussDynamicsRestartAdapter`，`rsolverstress.{h,cpp}` 和相应 solver 测试 | 一次冻结 system，每步验证源绑定；纯候选/结果/重启转换；不临时清空非法设置绕过 helper 门槛 |
| 提交前存储交易 | `rsolvergeneric.{h,cpp}`、`rsolver.{h,cpp}`、`fea-solver/src/native_accepted_checkpoint.{h,cpp}`、`solver_task.cpp` 与 CLI 故障测试 | stage(candidateModel) → publish → noexcept commit；失败 rollback；旧提交后 observer 保持通知语义；不宣称多文件全局原子 |
| 根集成 | CMake、必跑 manifest、格式兼容外层测试、源冻结/验收和交接文档 | 串行构建、真实 CLI、完整回归及独立审查；先子模块后根指针 |

上述包有接口依赖，先冻结 schema 与交易接口再写；不得让两个 writer 修改同一文件。模型/求解/存储事务必须综合验收，单包通过不能关闭完整 native 增量。

## 当前代码证据与风险

旧 `rstructural_cable_model_adapter.cpp:144` 拒绝 cable/piezo 同启；`rsolverstress.cpp:597`、`:2742` 按旧互斥路由分派。应增加独立 mixed 路由，不能只删除检查。复用 `RStructuralStaticTrussModel` 几何/边界/发布时逐项核查其适用门槛。

旧 `RStructuralDynamicsRestartData` 不能表达完整 applied V/Q/L0 和分项功；旧 cable 重启把支座功归入 external work，不能直接用作新物理账。新 restart 需完整 driver state、稳定 member/port 顺序、固定 dt、completed FE steps、命令索引、实际输入及以后新增的 control tick/phase。

`rsolvergeneric.cpp:233` 先写结果再内存提交，`:299` 的 observer 在提交后运行；CLI `solver_task.cpp:902` 使用该 observer。`native_accepted_checkpoint.cpp:397` 在成功发布前更新 observed sequence，需先修复候选序列语义，才能保证同 solver 存储失败后重试。`rsolver.cpp:468` 的文件快照/锁/回滚目前用于 Heat→Stress，可研究复用但不能假定新混合步已经受保护。

## 硬验收

必须实际运行 ASCII/binary CLI 的普通杆＋压电杆＋索与解析支座模型；continuous/split 的完整 q/v/a、输入、分项功、序号及身份一致。修改几何/定律/质量/曲线/port、损坏输入/phase 或有损显示数据不能被当成合法恢复。

逐项注入 Newton、result record/link、checkpoint model/envelope/latest 发布故障；旧接受态与持久恢复点保留，修复后同 solver 重试与不中断参考一致。发布后通知 observer 失败另有明确语义，不能倒写为提交前失败。失败记录包含最后接受状态与恢复资格，不自动放宽容差、更换物理或无限重试。

每个原生增量都需要源冻结、真实 CLI/磁盘参考、独立审查、兼容回归及证据归档。以上路径/行号来自本次只读核查，实施前仍须按实际 checkout 重新确认。
