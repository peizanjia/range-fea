# 当前 Range FEA 压电验证记录（2026-11-05）

**完整课程代表链尚未通过。** 已完成真实原生梁模型的 LQR/MPC 短链；四杆 PZT 已生成完整 66 步主动响应，但主动/被动比较、独立数值参考和恢复的整链资格未完成。用户随后要求停止修改、构建和执行，仅保留调研报告，本轮已停止。

## 已确认的运行结果

| 路径 | 已确认结果 | 证据边界 |
|---|---|---|
| 两段 BEAM1 / 全状态 LQR | 8 个原生 FE 接受步；单独运行退出 0；QtTest 3 passed 包括 init/cleanup | 真实案例只有 1 个；断言残差、步/控制计数和 Riccati 证书 |
| 两段 BEAM1 / 全状态 MPC | 8 个原生 FE 接受步；组合日志内该 data row PASS | 组合进程随后中断；短例只检查 warm-start/诊断对象，不等于完整 KKT 数值资格 |
| 四杆 PZT-5A / 预应力 / 全状态 LQR / 强迫响应 | 66 步、67 样本主动轨迹已落盘 | 状态仍为 `ACCEPTED_FE_TRACE_NUMERICAL_CHECKS_PENDING`，`qualified=false` |
| 梁/板 sensor-only 观测器逐相位恢复 | 组合任务中断，未获得该 row PASS | 不宣称梁观测器恢复或板验证通过 |
| PZT RAM/RBM N3 恢复 | 未执行到这些选中 row | 新增的恢复凭据和模型留存代码也未获得运行验证 |

上述执行层级是 application-lib native run 加真实 Python 数值 provider，以及直接运行 Qt 桥接测试；没有验证 fea-solver CLI、GUI、安装包，也没有对三族完整科学示例作整套通过声明。全状态控制案例同时包含电读出；不能据此宣称仅用电传感器反馈的闭环已通过。

可直接核对的现有文件：

- [PZT 原始配方、导出算子及 67 样本](../../../../artifacts/piezo-current-20261105/evidence/PZT-prestress-LQR-forced.json)
- [单独 LQR 完整通过日志](../../../../artifacts/piezo-current-20261105/logs/run-bridge-profile.log)
- [组合 LQR/MPC 通过与随后中断日志](../../../../artifacts/piezo-current-20261105/logs/run-bridge-controllers-observer.log)
- [PZT 运行中断日志](../../../../artifacts/piezo-current-20261105/logs/run-pzt-lqr.log)

## 四杆 PZT 当前证据

测试内置典型 PZT-5A 全材料数据和不同方向的四根 TRUSS1，设置非零位移工作点、拉伸预应力和 12 V 作动偏置，并包含作动电荷、开路电压、短路电荷及理想电荷放大器读出。材料来源是测试声明的典型数据，未作硬件或本轮外部材料文档真实性资格。时间步 `3.76711800354e-06 s`，66 步覆盖第一模态 `1.001300068` 周期；未静默缩短时窗。

- LQR Riccati 相对残差 `1.85237e-13`；导出闭环极点最大模 `0.997157862`。
- 已接受主动轨迹的绝对电压峰 `12.2036290806 V`，tip 位移峰 `6.29126230231e-10 m`。
- 初始工作点独立核对已保存在 JSON 的 `independentPztPrestressPhysics`：材料/几何/固定电荷切线、支座反力和有限差分耦合导数。
- off 轨迹、主动/被动比值、完整 FE/RK4/ROM 资格与磁盘恢复均无完成凭据；现有 JSON 不能作为完整验证通过文件。CSV 导出器已写但未执行，当前没有该代表链的 CSV 或恢复模型留存。

## 构建修正和验证范围

初始 Linux 配置把桥接解释器设为 `/python.exe`。已引入统一 Python 选择变量：Unix 使用所选 `Python3_EXECUTABLE`，Windows 私有目录仍使用 `python.exe`。对 Unix symlink 解释器增加可操作警告，提示 `venv --copies` 和显式解释器路径；未通过 REAL_PATH 改变虚拟环境语义，未弱化 transport 安全规则。本轮只验证任务工件内复制的真实解释器，不能推广为任意 Linux 默认配置可用。

构建依次暴露 glibc `finite(double)` 与局部 helper 歧义，以及 OpenSSL `BN_ULONG` 多词宏类型的函数式转换错误。已按当时授权作局部重命名和单处 `static_cast`；逆变换与 HEAD 一致的证据已保存，数值/安全验证条件未改。PZT 目标最终构建退出 0（62.07 s），bridge 目标退出 0（16.30 s）；此前三次失败日志全部保留。

运行时是 Python 3.14.4、NumPy 2.5.4、SciPy 1.18.1、Qt 6.10.2、GCC 15.2.0。任务内使用 `venv --copies`，通过局部 `.pth` 引用已有 NumPy/SciPy，不安装系统包、不写全局配置。TMP/TEMP/XDG 和工件全部定位到 `artifacts/piezo-current-20261105`。源码、数值依赖和测试二进制身份记录见配套 JSON 的 `evidenceIdentity`。

## 耗时与停止状态

单独 8 步梁 LQR 的 profile 完整运行耗时 19.407 s；121 次 provider 操作中，源码/解释器 pre/post guard 合计 18.756 s，占记录操作时间 98.82%。独立有界 GDB 副本的栈也采样到 `hashFile -> verifySources -> request(force) -> AxialNativeRun::certifyRuntime/prepare`，支持“身份哈希是主要耗时”的判断；不是已发现的数值发散或进程互等。该诊断副本 9 秒后已停止，没有更改系统 ptrace 设置或安全护栏。

PZT 正式组合运行在 244.90 s 收到 SIGTERM，退出 -15；bridge 组合运行在 167.29 s 收到 SIGTERM，退出 -15。两份日志记录发送 PID 104311。收到停止通知后核查，本任务 runner、测试、Python 子进程和 GDB 均已不存在，未向无法确认归属的进程发送信号。没有自动回退此前改动，也没有在停止要求后补跑测试或构建。

## 保留的文件改动

以下是停止通知前本任务产生的 12 个源码/脚本改动；不包含既有 Claude 改动。前三个 solver 文件属于子模块，需单独查看子模块 diff。

| 文件 | 改动 |
|---|---|
| `src/CMakeLists.txt` | 统一Unix所选Python路径，并对symlink解释器提供venv --copies诊断；保留Windows private/python.exe分支 |
| `src/fea-solver/CMakeLists.txt` | 使用统一桥接Python变量 |
| `src/fea-solver/tests/CMakeLists.txt` | 使用统一桥接Python变量 |
| `src/range-application-lib/CMakeLists.txt` | 应用库及压电测试使用统一桥接Python变量 |
| `src/range-solver-lib/src/rstructural_truss_dynamics.cpp` | 局部finite helper及调用机械重命名checkedFinite |
| `src/range-solver-lib/src/rstructural_piezo_native_readout.cpp` | 同类局部finite重命名 |
| `src/range-solver-lib/src/rstructural_piezo_axial_operators.cpp` | 同类局部finite重命名 |
| `src/range-application-lib/src/piezo_native_backend_bridge.cpp` | 同类局部finite重命名 |
| `src/range-application-lib/src/piezo_axial_backend_bridge.cpp` | 同类局部finite重命名 |
| `src/range-application-lib/src/rtrusted_holdout_pack_verifier.cpp` | 仅将BN_ULONG函数式转换改为static_cast<BN_ULONG> |
| `src/range-application-lib/tests/tst_piezo_pzt_prestress.cpp` | 增加恢复逐相位JSON凭据、原生保存文件留存；本次未执行到这些新增路径 |
| `scripts/benchmark/validate_piezo_current_20261105.py` | 新增定向构建/选定测试/CSV导出runner；CSV导出尚未执行 |

停止通知后仅新增/更新本专属目录中的 `piezo_validation.md` 和 `piezo_validation.json`。后续若重新允许执行，应先协调 Claude 的源码/构建所有权，再补齐科学资格与恢复证据；本报告不启动这些动作。
