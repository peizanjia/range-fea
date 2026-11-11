# PC-04～PC-08 源码与验收缺口审计

**观察时间：**2026-10-13 21:48；**局部证据更新：**2026-10-14 00:42（Asia/Singapore）。**性质：**缺口映射，不是阶段验收收据。冻结范围和逐项状态仍以 `../piezo_control_plan.json` 为准：这五阶段的 15 条 A01～A03 均为 `pending`，`productComplete=false`。

现行生产链包含 `src/range-workflow/python/range_workflow/piezo_design.py` 的广义模态、阻尼、FE/ROM 投影和 ZOH；`piezo_control_design.py` 的 LQR/MPC/观测器；`piezo_excitation.py` 与 `piezo_external_time.py` 的时间力；`piezo_native_backend.py` 的控制候选；以及 `src/range-application-lib/src/piezo_native_backend_bridge.cpp`、`piezo_axial_backend_bridge.cpp` 到原生 FE 的桥接。存在这些实现或局部测试，不等于已通过完整验收。47/47 私有 workflow/scientific 结果不是最终同源冻结证据；v42 234 项 CTest 在 113/234 后中断，第 114 项代表算例没有完成。

| 条目 | 已见现行路径/局部测试 | 当前最重要的验收缺口 |
|---|---|---|
| PC-04-A01 | `piezo_design.py` 模态/选模；真分层梁双悬臂近重簇 `[0,1]` 的 v2 定向测试、独立证书和 Sol 复审见 `build/audit-20261013/pc04-modal-cluster/receipt-v2.json` | 杆、梁、板三家族同源完整通过、选模交互/工件和最终冻结源收据；当前仅梁的真 FE 局部证据。 |
| PC-04-A02 | 同文件的均匀、逐模/簇、Rayleigh、导入 C 与 PSD；轴向导入 C 局测 | 各策略在真 FE 修改后矩阵上产生的权威 C、PSD、单位与来源。 |
| PC-04-A03 | `reduce_model` 使用同一 C 投影，含高阶模态策略 | 同一个完整 C 的全基/整簇截断基比较和遗漏模态误差界。 |
| PC-05-A01 | 两个 C++ bridge 导出 M/K/C/G/Ce/B；三家族真 FE 算子/源/单位/JSON 读回聚焦 5/5、整目标 1/1、Sol 复审见 `build/audit-20261013/pc05-fe-rom-export/receipt.json` | 最终同源冻结收据、完整多模态/工件映射、安装版读回和独立单元物理参考。 |
| PC-05-A02 | 三家族各一模态真实 FE ROM 连续矩阵、直接项及独立阻尼振子闭式精确 ZOH 已定向通过 | 多模态矩阵指数、全部电学/加速度/扰动通道独立 oracle 与完整 FE-ROM 导出比较。 |
| PC-05-A03 | bridge 导出 q0/V0/Q0、P/T、DOF/单位/来源 | 非零工作点和初始电荷偏置的 FE/ROM 绝对与增量读出、持久工件读回、量化误差。 |
| PC-06-A01 | 七类受限激励的同一轴向真 FE 力通道独立端点公式、接受响应与 7 行聚焦/整目标结果，Sol 复审见 `build/audit-20261013/pc06-axial-waveforms/receipt-v2.md` | 其余家族、载荷方向/边界、正式保存重开与最终同源冻结证据。 |
| PC-06-A02 | 外部唯一时间函数、依赖和错误检查；轴向外部力局测 | 同源真 FE 收据同时证明命名力/单位/身份与未声明依赖或错误输入的拒绝。 |
| PC-06-A03 | 分层 Newmark、能量、细化及重试局测；轴向七波形同场景的端点功/耗散/能量与中途源改动拒绝通过 | 三家族同一闭环受迫 FE 的步长细化、故障注入重试、完整功账和最终冻结源收据。 |
| PC-07-A01 | 速度、PI/PID/PD、离散状态空间和动作限制实现；真轴向 FE 两采样独立标量 oracle、整目标 33/33 和 Sol 复审见 `build/audit-20261013/pc07-basic-fe/receipt-v2.json` | 其他 FE 家族、实际 GUI/保存重开和最终同源验收；本项仍 pending。 |
| PC-07-A02 | DARE/K/极点、MPC QP/KKT 实现及局部测试 | 真 FE 各控制样本的残差/约束证书和 QP 失败时不回退到伪动作。 |
| PC-07-A03 | full FE 与 sensor-only 分路；隐藏状态拒读及观测器恢复局测 | 真 FE 的可观性资格、估计误差和不读取隐藏 FE 状态的组合收据。 |
| PC-08-A01 | `tst_piezo_delivery_examples.cpp` 编写了 FE on/off、同载荷回放和独立 ROM 闭环 | 杆/梁/板代表算例的实际完整通过结果；长目标尚未完成。 |
| PC-08-A02 | 轴向、分层各有多采样相位和 RAM/RBM 恢复局测 | 三家族同源各相位磁盘恢复，FE step、control tick、held action 与最终状态字节一致。 |
| PC-08-A03 | 候选状态包含积分、观测器、previousApplied、QP warm start；轴向 sensor-only MPC/RAM N=2 真 provider 的 post-FE 故障后接受态字节不变、重试与无故障基线相同，聚焦 3/3、完整目标 1/1；`build/audit-20261014/pc08-true-provider-retry-v2/receipt.json` 的 Python/provider 来源补证经独立 Sol 复审无 P1/P2 | 其余算法与梁/板实际 provider 故障重试，以及三家族/多相位共同接受资格；此证据只是轴向一个切片。 |

**最短核查路径：**先冻结相关源并重建，收据固定源和执行档 SHA-256；再跑 Python 设计/控制/激励/外部时间及分层原生短目标、轴向与分层两个 FE bridge；最后从 `tst_piezo_delivery_examples` 的三家族代表行取得模态、FE/ROM 回放、LQR/MPC 等证据。聚焦通过不替代最终完整回归、安装、真实 GUI 与独立终审。

**2026-10-14 07:20 局部补证，阶段状态不变。**`src/range-workflow/tests/test_piezo_design.py` 新增 `test_two_mode_damped_zoh_and_acceleration_direct_terms`，用两个独立欠阻尼振子的闭式转移矩阵和恒力平衡位移，核对四状态/双输入精确 ZOH 的 `Ad/Bd`，并核对 `La M^-1 B` 加原有直接项。私有 Python v3 的该单项通过，随后同文件完整 20/20 通过；AST 与 `git diff --check` 通过。测试源 SHA-256 为 `1cadb305b0926eeda7fb1d81fc5af4b3aa49d768e8910ead124915f28862b758`，执行器 SHA-256 为 `5341746f92483a93e44c313de830f2fba2956f0759094404a16b2fed06c9a2ed`。它是纯数值后端局部 oracle，不提供真 FE 多模态、安装工件读回或最终源码冻结证据。另核对分层桥接模态阻尼：预计算仅传 `damping`、以完整基生成权威 FE `C`，正式初始化移除阻尼覆盖后才按 `modeIndices` 选 ROM；先前静态审计怀疑其拿截断基生成完整 `C` 是误判，未改生产代码。
