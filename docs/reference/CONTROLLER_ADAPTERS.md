# 控制器部署与缺口修复（2026-09-26）

本轮保留求解器的接受步契约：控制器只计算请求值，求解器负责调理、限幅、物理力装配、功账、结果发布与接受状态。Python、C++ 或远端板卡都不能在 `compute` 中直接驱动物理执行器；试步失败不能撤销已发生的物理动作。当前 TCP 路径适用于软件在环和板端纯计算验证，未做真实板卡、串口/CAN、硬实时或闭环实物安全验收。

## 已提供的实现

| 入口 | 用途 | 依赖与边界 |
| --- | --- | --- |
| `control_pid_python.py` | 有显式接受状态的 PID | Python 标准库；已有独立进程续算回归 |
| `stage9/stage9_velocity_controller.py` | 从 FE 模型设计速度反馈、全阶回代 | NumPy/SciPy；包含工作点与模型来源校核 |
| `control_learned_python.py` | 加载固定 JSON 网络权重并推论 | Python 标准库；一个速度感测器、一个 N 力通道，拒绝训练域外输入 |
| `train_control_policy.py` | 离线 PyTorch 监督训练并导出 | CPU float64、固定种子；学习标量速度反馈律，不是强化学习算法 |
| `range-control-velocity` | C++ 常驻进程速度反馈 | Qt Core，命令参数为非负增益 N·s/m |
| `control_tcp_bridge.py` | 标准输入输出 NDJSON ↔ TCP | Python 标准库；v1/v2 信封转发，固定整包截止时间和大小限制，不自动重试 |
| `control_tcp_matlab.m` | MATLAB R2022a 的 v1 TCP 速度反馈 | MATLAB 自带 JVM；不依赖先前失败的 stdin，也不需要 Instrument Control Toolbox |

v1 支持瞬态节点力。v2 仍按现有型别准静态接口解释长度、张力、锚点、梁片 V/C 等通道；桥接器接受 v2 信封不意味着 MATLAB 示例或物理求解器新增了全部 v2 能力。

## Python 学习型控制器

以下命令在仓库根目录执行，路径应按实际安装位置替换：

```powershell
$python = 'D:/ProgramData/anaconda3/python.exe'
& $python src/fea-solver/examples/train_control_policy.py `
  --gain 21.790129968488106 --sensor-id tip-velocity-x --actuator-id tip-force-x `
  --velocity-scale 0.1 --epochs 600 --seed 17 `
  --output build/learned-policy.json

$argsJson = @((Resolve-Path src/fea-solver/examples/control_learned_python.py).Path,
              '--policy', (Resolve-Path build/learned-policy.json).Path) | ConvertTo-Json -Compress
& build/vs2026/Release/fea-solver.exe --file=YOUR_CONTROLLED_MODEL.ram `
  "--control-program=$python" "--control-args-json=$argsJson"
```

模型须启用结构控制并具有相应的感测器/执行器 ID。附带的 `control_learned_velocity_policy.json` 对应四杆 Stage 9 算例。训练环境实测 PyTorch `2.9.0+cu126`，训练显式使用 CPU；运行推论无需 PyTorch/GPU。`control-learning-requirements.txt` 仅用于训练环境。

网络为 `1 → 8 → 1`、零偏置、tanh、正隐层权重和负输出权重，满足零速度输出零力以及瞬时力与速度反向。归一化、物理单位和允许的速度范围写入权重文件；限制到 ±0.1 m/s 的示例不会对域外输入悄悄外推。该符号性质不保证含采样、延迟、限幅和高频模态的闭环稳定性，必须按实际 dt 做 FE 回代。

固定种子 17、600 epochs 的导出示例在 1025 个留出点上的最大/RMS 教师力误差为 `0.0028348718 / 0.0010906448 N`；PyTorch 与导出推论的最大差为 `6.66e-16 N`。这些是拟合及导出误差，不是结构响应或实物控制精度证明。

每次调用只使用请求携带的接受状态；重复同一请求不会累加计数，重新启动程序后仍从求解器提供的状态继续。权重文件路径作为独立参数或 `--policy=path` / `--weights=path` 参数均纳入控制器版本指纹，修改权重后不能沿用旧受控 checkpoint。

## C++ 与板端路径

原生范例以 `range-control-velocity 21.790129968488106` 启动，作为 `--control-program` 传入该可执行文件的绝对路径，`--control-args-json` 为增益字符串数组。它核对全部感测器目录，选取唯一 m/s 感测器，并从接受状态计算候选计数。例程是可复用的协议参考，不是微控制器固件；Qt JSON 和桌面进程运行时不能直接等同 MCU 部署。

对更复杂的学习模型，主流可选链路是 PyTorch → ONNX → ONNX Runtime C++；Cortex-M 可评估 ExecuTorch/CMSIS-NN，但必须针对具体板卡、算子、量化误差和延迟单独验证。本轮实现的是可审查的小网络 JSON 推论，并未交付 ONNX Runtime、ExecuTorch 或板端烧录工具。

官方参考：[PyTorch ONNX 导出](https://docs.pytorch.org/tutorials/beginner/onnx/export_simple_model_to_onnx_tutorial.html)、[ONNX Runtime C++](https://onnxruntime.ai/docs/get-started/with-cpp.html)、[ExecuTorch Cortex-M](https://docs.pytorch.org/executorch/stable/embedded-arm-cortex-m.html)。

板端若提供 TCP，可在受控本地网络内实现相同 NDJSON 纯计算接口，由桥接程序连接指定地址/端口。请求状态完全由求解器携带，远端不能维护隐藏的推进状态。桥接失败会终止当前试步；不重发不确定请求，不承诺硬实时。硬件物理输出还需要独立的施加/确认、去重、故障停机及时间同步协议。

## MATLAB TCP 替代路径

本机 `-batch` 的三种 stdin API 均失败；新的路径用 MATLAB JVM TCP，已验证握手、重复请求、重连接受状态、31 秒请求间空闲及真实 400 步 FE 控制。直接运行本机实际二进制 `D:/Program Files/MATLAB/R2022a/bin/win64/MATLAB.exe` 可正常结束；最初通过外层 launcher 的 TCP 探测未在 100 秒内就绪，不能由此推断所有 launcher 场景都失效。

先在独立 MATLAB 会话调用：

```matlab
addpath('D:/YOGA/Documents/Projects/range-fea/src/fea-solver/examples');
control_tcp_matlab(45678, 21.790129968488106, 2)
```

服务器只绑定 `127.0.0.1`；第三个参数是允许的求解器连接次数（例如完整运行和一次续算）。等待连接上限 120 秒；已连接后允许 FE 长时间计算，开始收到一行后须在 30 秒内收完，最大行长受限。日志写到 MATLAB 自己的输出，不混入协议。

求解器启动 `control_tcp_bridge.py --port 45678 --endpoint-sha256 HASH`。`HASH` 是 `control_tcp_matlab.m` 原始文件字节、单个 LF、增益的 `%.17g` 字符串依序连接后的 SHA256。服务器握手回报此摘要，桥接器检查；该参数也进入求解器控制器指纹，防止更换远端代码/增益后继续旧 checkpoint。摘要是可信本机端点的一致性检查，不是网络认证。

```powershell
$endpointHash = & $python -c "import hashlib,pathlib; p=pathlib.Path('src/fea-solver/examples/control_tcp_matlab.m'); print(hashlib.sha256(p.read_bytes()+b'\n'+format(21.790129968488106,'.17g').encode()).hexdigest())"
$argsJson = @((Resolve-Path src/fea-solver/examples/control_tcp_bridge.py).Path,
              '--port','45678','--endpoint-sha256',$endpointHash) | ConvertTo-Json -Compress
& build/vs2026/Release/fea-solver.exe --file=YOUR_CONTROLLED_MODEL.ram `
  "--control-program=$python" "--control-args-json=$argsJson" --control-timeout-ms=10000
```

关闭 v1 控制程序时先关闭 stdin，给 200 ms 正常退出；之后才按有界 terminate/kill 回退。这样桥接器能正常关闭 TCP，避免原先 FE 已成功但 MATLAB 因连接重设报错。未启用摘要固定的通用桥接只能证明协议一致，不证明远端控制代码身份。

## 数值缺口与保留范围

- 新起算拉索在第一次观测前计算 `M a0 = f_dead - f_internal(q0)`，保留 q0/v0；已有 checkpoint 或重启状态不重算。控制力更新仍刷新右极限加速度。范围仍是既有集中质量、零固定支承、无阻尼的拉索 Newmark 路径。
- 常系数单体热机直杆现可沿任意三维直线放置；标量载荷/位移沿确定性的正向轴定义，输出位移映射回全局。线单元 `STRESS_X` 保持局部轴向 xx。弯折网络、一般多维热机单体、温变材料、跨重网格和任意任务树事务仍未交付；单体适配器仍是 C++ direct entry。
- Stage 9 新增 `--auto-modal-relative-tolerance 0.01 --frequencies-json '[0,5,15,30]'`，可用 `--max-auto-modes` 限制阶数。逐候选核对所有指定频点/通道，不能切开重复模态簇；未达标返回非零并保留误差证据。四杆示例的 1/2/3 模态最大相对误差约 140.33%/28.70%/11.06%，1% 门槛须保留全部 4 模态。这是诚实的精度/成本选择，不保证采样频点之间的误差或闭环稳定。
- 干净 Windows/Hyper-V/Sandbox 权限事项按用户指示暂缓；本机安装树与模拟端点不等同实机、干净 VM 或完整 GUI 认证。

构建、CTest、MATLAB 过程和拟合证据在忽略目录 `build/controller-adapters-*`；旧 stdin/TCP 排查在 `build/audit-20260926/`。最终验收数字以 `HANDOFF.md` 最新条目为准。
