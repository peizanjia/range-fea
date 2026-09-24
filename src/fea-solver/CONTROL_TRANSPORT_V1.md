# 进程外控制传输 v1（实验性）

`ControlNdjsonTransport` 在同一机器上通过 `QProcess` 的独立标准输入、标准输出和标准错误与控制子进程通信。`program` 必须是绝对可执行文件路径，`arguments` 是独立字符串数组；不调用 shell。`SolverTask` 外部进程接线及 Python PID 已通过 Release 端到端测试。Python 示例见 `examples/control_ndjson_python.py` 和 `examples/control_pid_python.py`；MATLAB `-batch` 可启动，但本机标准输入长驻握手未验证，故不声明有可用 MATLAB 适配器。

每个请求和响应都是一行 UTF-8 JSON，末尾为换行符。所有消息均包含整数 `protocolVersion: 1`、字符串 `requestId` 和字符串 `op`；响应必须原样返回本次 `requestId`。一次会话先握手，再进行多次纯 `compute` 请求。控制器不得依赖未在请求中给出的可变历史；调用方持有已接受的控制器状态，仅在有限元步接受后采用 `nextControllerState`。

握手请求：

```json
{"protocolVersion":1,"requestId":"uuid","op":"handshake","controllerId":"controller.pid","controllerRevision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","modelSnapshotSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","deterministicSeed":"17","capabilities":{"pureStep":true,"controlMode":"scalar-nodal-force","sensors":[{"sensorId":"tip-displacement","unit":"m"}],"actuatorIds":["tip-force"]}}
```

握手响应必须包含 `ok: true`，原样回显 `controllerId/controllerRevision/modelSnapshotSha256/deterministicSeed`，并在 `capabilities` 中确认 `pureStep: true`、`controlMode: "scalar-nodal-force"` 与完全相同的执行器 ID 集合。摘要在启动前确定；控制器版本摘要绑定可执行文件、参数以及存在的脚本/配置文件内容。模型快照摘要绑定本次运行输入；重启时其值可能变化，不能当成跨运行的永久模型 ID。当前只交换标量节点力，单位为 `N`；传感器单位由模型目录给出。

计算请求：

```json
{"protocolVersion":1,"requestId":"uuid","op":"compute","stepId":"42","controllerId":"controller.pid","controllerRevision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","modelSnapshotSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","deterministicSeed":"17","time":0.41,"dt":0.01,"observations":[{"sensorId":"tip-displacement","value":0.001,"unit":"m"}],"acceptedControllerState":{"gain":100.0}}
```

计算响应：

```json
{"protocolVersion":1,"requestId":"uuid","op":"compute","stepId":"42","commands":[{"actuatorId":"tip-force","force":-0.1,"unit":"N"}],"nextControllerState":{"gain":100.0}}
```

`stepId` 和种子使用十进制字符串，避免 JSON 浮点数不能精确表示大整数。每个已协商执行器必须恰有一条命令；已协商传感器必须恰有一条观测。缺字段、重复/未知 ID、单位不符、非有限数值、协议版本或请求号错误、超长消息、进程退出及超时都以失败关闭会话，不输出部分有效命令。标准错误仅作独立诊断，不参与协议解析。求解器集成层执行节点力映射、模型策略规定的延迟/保持/确定性噪声、限幅和变化率约束、检查点及控制功记录；`requested/conditioned/applied` 分别进入接受步审计。

握手绑定本次输入文件摘要、控制器标识/版本和种子；求解器检查点另绑定矩阵/映射指纹。跨重网格的长期稳定实体 ID、经过独立认证的控制器身份、硬实时延迟上界与完整模型查询服务仍未提供。进程可以回显握手字段，但回显本身不是可信认证；生产环境还需要受控程序来源与运行权限。
