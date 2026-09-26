# Stage 9 控制分析：原生 FE → 降阶 → 控制设计 → 全阶回代

2026-09-26 增量：可以用 `--auto-modal-relative-tolerance 0.01 --frequencies-json '[0,5,15,30]'` 自动选择达到指定采样频点误差门槛的完整模态簇；`--max-auto-modes` 限制最大阶数。此选项不与显式 `--modes` 或 CB 边界同时使用。报告 `modalSelection` 保留各阶误差；未达标返回1并写入 `errors`。四杆示例要求1%时选择4模态；限制到2模态会明确失败，不能把28.70%误差作为1%认证。只证明给定频点，绝不保证频点之间或闭环稳定。更多控制器部署与真 FE 对照见 `CONTROLLER_ADAPTERS.md`。

CLI 原生程序负责真实 FE 组装及求解，离线分析使用独立 Python 3.10+、NumPy、SciPy。Python 解释器和数值依赖不打入 Windows 安装程序；依赖清单随示例安装。本机验证版本最终写入交接。源码示例在 `src/fea-solver/examples/stage9/`，安装后在 `examples/control/stage9/`。

## 导出

模型须有稳定的节点力致动器和节点位移/速度/加速度感测器目录。原生完整 `.ram` / `.rbm` 是输入；latest link 不能作为矩阵导出源。目的目录须预先存在。

```powershell
python -m pip install -r stage9-requirements.txt
fea-solver --file=stage9_truss4.ram --linear-model-output=linear.json
python stage9_analysis.py --model linear.json --output analysis.json --modes 2 --boundary-indices-json '[3]' --frequencies-json '[0,5,15,30]'
```

这里的界面 3 是矩阵坐标表的索引；频率单位 rad/s。报告有全阶和降阶的 E/A/B/C/D、投影基底、模态与 Craig–Bampton 模型、复数频响、条件数/残差、PBH 诊断以及各频点的绝对/相对误差。退出 0 表示请求的分析都成功；退出 1 表示部分请求失败，具体原因在 `errors`；退出 2 是输入/输出错误。不得把有 errors 的报告作为完整验收。

导出 `M,C,K` 尺寸 n×n，`B` n×m，`Sq/Sv/Sa` p×n，`D` p×m。位移、速度、加速度感测有不同单位；加速度一阶模型包含 `Sa M⁻¹B` 直接馈通。`operating_point` 保存静态常载平衡，矩阵描述该点附近的扰动。n 向量 `force` 是广义载荷，不是 m 向量通道命令。DOF 的物理节点 ID、顺序、约束局部框架及全局方向必须保持一起使用。

## 离线设计和时间域回代

```powershell
python stage9_velocity_controller.py --model linear.json --target-damping 0.1 --sensor-id tip-velocity-x --actuator-id tip-force-x --output gain.json
python stage9_velocity_controller.py --replay-model linear.json --gains gain.json --dt 0.005 --steps 400 --initial-q-json '[0.000382683432365,0.000707106781187,0.000923879532511,0.001]' --output reference.json
```

初始坐标必须依据 `dofs` 和 `globalDirection` 投影；上面的数组只适用于随附四段轴向杆算例的排序/方向。设计是共点速度反馈 `u=−g y`，目标是选定模态的阻尼估计，同时报告全阶闭环极点。不是通用 LQR/MPC，也不保证每个被截断模态达到该阻尼。连续极点检查与实际离散采样的 FE 回代分别核对。

独立 NumPy 回放使用 Newmark 平均加速度及接受边界速度采样的零阶保持力，并在每次换力后更新接受边界加速度。报告全阶/单模态终态、被动基线、时间域误差、机械能与控制功。真实 FE 由以下 NDJSON v1 子程序接线；PowerShell 用 ConvertTo-Json 避免混淆进程参数边界：

```powershell
$stage9Python = (Get-Command python).Source
$stage9Args = @((Join-Path $PWD 'stage9_velocity_controller.py'), '--gains', (Join-Path $PWD 'gain.json')) | ConvertTo-Json -Compress
fea-solver --file=stage9_truss4.ram "--control-program=$stage9Python" "--control-args-json=$stage9Args" --control-timeout-ms=10000
```

执行 FE 会按既有求解器规则发布原生接受结果；复跑或重新导出应使用原始完整模型副本。随附模型启用四段杆 Newmark 动力学、400 步与原生位移初始条件，并已有输入/感测目录。求解器承担幅值、变化率、延迟、保持和接受事务；连续设计不自动涵盖这些非理想条件。

## 认证边界

原生导出限制为 POINT/TRUSS1/BEAM1、1–256 个独立机械坐标、各不超过64个输入/输出、固定支承、恒定负载与293.15 K。线弹性是参考几何组装；缆索必须收敛且严格处于绷紧分支，使用真实平衡切线和集中质量。非零固定支承位移及点弯矩经原生组装处理。

MPC/耦合、移动支承、电路/压电状态、一般热工作点、松弛切换、奇异质量和不稳定切线均显式拒绝。数值 PBH 秩是给定尺度/容差下的诊断。CB 保留指定界面坐标；模态簇边界及奇异频点会失败。相对频响误差在全阶通道零点附近需结合绝对误差解释。

详细算法、原始依据和验证边界见 `TECHNICAL_ROUTE_STAGE9.md`、`HANDOFF.md`。MATLAB 长驻进程与干净 Windows VM 的历史环境缺口仍独立保留，不影响本阶段 C++/Python 工作流。

## 2026-09-26 实测证据

全目标 Release 构建完成，完整 CTest **87/87 通过，44.52 s**。新增 4 个 CTest 目标覆盖原生导出、Python 分析、Python 控制器及真 FE CLI 工作流。Python 分析 9 项、控制器 21 项数值/协议测试通过；原生导出还有杆件/梁/缆索、支承位移、弯矩、既有历史、局部框架和失败拒绝测试。

四段轴向杆、dt=0.005 s、400 步的实际 FE 主动/被动算例与独立 NumPy Newmark 回放吻合。受控终态最大位移差 1.98e−11 m、速度差 8.74e−10 m/s；实际 FE 末振动能量比 0.0118633。模态归一化误差 6.76e−16、广义特征残差 1.40e−15，CB 静态约束残差为0。

这不是任意频带的 ROM 精度保证：随附四自由度模型仅保留两个模态时，0/5/15/30 rad/s 各通道最大相对频响误差为 7.77%/6.46%/9.89%/28.70%；保留一个界面和两个固定界面模态的 CB 对应为 0%/0.0030%/0.1694%/1.2020%。单模态时间域还会丢失高频加速度成分，零点附近相对误差可大于1。报告分别给出完整 ROM 预测误差与投影截断误差。绝对频响误差依输出/输入单位分别统计，不跨不同单位取物理最大值；ROM 的工作点残差明确是原始全阶平衡的证据。

本机版本 Python 3.13.5、NumPy 2.1.3、SciPy 1.15.3。CMake 自动发现的另一 Python 3.14 缺少数值依赖，实测配置以 `-DPython3_EXECUTABLE=D:/ProgramData/anaconda3/python.exe` 指定上述解释器。便携算例的原生字节及矩阵 JSON 禁止 Git 换行转换，保持源哈希可核对。
