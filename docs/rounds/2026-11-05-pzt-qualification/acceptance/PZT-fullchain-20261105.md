# PZT代表算例：当前Linux全数据行资格（2026-11-05）

**结果：原`tst_piezo_pzt_prestress`的12个数据行全部通过，4个科学场景为QUALIFIED，8个RAM/RBM恢复场景覆盖20个相位；16份完整CSV共1072行，逐值回读一致。** 原单次聚合运行因整机内存90.20%停止，未追认为PASS；最终采用12个独立QtTest进程执行原数据行，36个原始QtTest结果含每进程init/cleanup，全部exit0。

本记录是当前脏工作树的原生应用库／真实数值后端资格，覆盖四杆PZT、全状态LQR/MPC、独立时间参考、FE/ROM和恢复／导出；不是GUI、CLI、安装版、真实LLM、硬件或完整PC产品验收。`docs/piezo_control_plan.json`和productComplete未升级。

## 当前数值结果

| 场景 | FE能量范数RMS开/关 | ROM自身闭环开/关 | 原FE相对时间参考误差 |
|---|---:|---:|---:|
| LQR自由 | 0.863579 | 0.863488 | 0.4149% |
| LQR受迫 | 0.927197 | 0.927137 | 0.2604% |
| MPC自由 | 0.992478 | 0.992473 | 0.4721% |
| MPC受迫 | 0.991141 | 0.991137 | 0.2869% |

四科学行保留原66步／67点、约一个第一模态周期；材料、物理参数、控制权重、MPC五步预测域、容差及非退化动作条件不变。半步／四分之一步误差比约0.25；MPC最大KKT指标约2.29e-19。完整基一致性约1e-15是坐标变换／积分对照；首簇截断误差最高11.81%，不能混称截断ROM精确。固定参数下MPC抑振较小，原样保留。

8个恢复行的原测试窗口为N=2时5步、N=3时7步，覆盖LQR/MPC×RAM/RBM×全部20个保存相位，重新打开状态与续算终态codec逐字节匹配。独立RK4使用生产导出算子；独立材料／工作点／切线／反力另有检查，不声称存在第二套通用FE组装器或严格空间后验估计。

## 修正及回归

POSIX transport复用本构建已链接的OpenSSL Crypto进行完整SHA-256，保留所有请求前后完整字节、库存和路径检查，无mtime缓存；未提供Crypto时保持原Qt分支，Windows pin逻辑不变。只改根应用库CMake、transport及其测试，保留已有dirty与Codex改动。

完整transport37通过／9个Windows专用跳过，cache12通过，axial bridge41通过（含原sensor-only与磁盘恢复），LQR/MPC短bridge4通过；原Qt回退分支8个选定结果通过。新增padding／64KiB边界、同长度保mtime改写守门。LQR同一121次操作的guard计时约18.756→3.304秒；不外推为产品通用加速率。

逐行执行按85%稳定15秒准入、50ms采样、90%停止。最终记录峰值整机内存84.12%，本方组RSS最高263.13MiB；未终止其他应用或改变系统配置。

## 构建身份与证据

- 根HEAD `e3d5277`，含未提交字节；冻结清单摘要 `b43923724f0aa7429d94a48766682b3d98fa0215893d7bcda2b49cfc28e31057`。
- PZT测试执行档 SHA-256 `9b0b06e3fb05f44fa4fb2bdd05b06e5a13b44c03d367887edb779b92e3df36f4`，所有行执行前后源码／执行档一致。
- Qt6.10.2/GCC15.2，Python3.14.4/NumPy2.5.4/SciPy1.18.1，OpenSSL3.5.5；不是冻结Windows私有运行时资格。
- [本地详细结果](../../../../artifacts/pzt-fullchain-20261105/RESULTS.md)、[机器结论](../../../../artifacts/pzt-fullchain-20261105/qualification-summary.json)、[完整运行绑定](../../../../artifacts/pzt-fullchain-20261105/qualification-serial-run.json)、[CSV清单](../../../../artifacts/pzt-fullchain-20261105/csv-manifest.json)。
- 原始日志、模型、轨迹、环境和源码清单位于忽略的`artifacts/pzt-fullchain-20261105/`；本摘要不把缺失于新clone的原始收据当成远程可重验文件。新设备需要对应源码、构建和新运行。
- 当前工作区可用`python3 -I -B artifacts/pzt-fullchain-20261105/verify_results.py`重新核对已有输出；原命令、隔离路径与复算注意见详细结果。没有提交、推送或更新gitlink。
