# 进程外控制传输 v1（实验性）

`ControlNdjsonTransport` 在同一机器上通过 `QProcess` 的独立标准输入、标准输出和标准错误与控制子进程通信。`program` 必须是绝对可执行文件路径，`arguments` 是独立字符串数组；不调用 shell。当前仅实现传输和校验，尚未接入 `SolverTask` 或有限元时间步，因此不是端到端控制功能。Python 示例见 `examples/control_ndjson_python.py`；MATLAB 适配器可实现相同的行协议。

每个请求和响应都是一行 UTF-8 JSON，末尾为换行符。所有消息均包含整数 `protocolVersion: 1`、字符串 `requestId` 和字符串 `op`；响应必须原样返回本次 `requestId`。一次会话先握手，再进行多次纯 `compute` 请求。控制器不得依赖未在请求中给出的可变历史；调用方持有已接受的控制器状态，仅在有限元步接受后采用 `nextControllerState`。

握手请求：

```json
{"protocolVersion":1,"requestId":"uuid","op":"handshake","capabilities":{"pureStep":true,"controlMode":"scalar-nodal-force","sensors":[{"sensorId":"tip-displacement","unit":"m"}],"actuatorIds":["tip-force"]}}
```

握手响应必须包含 `ok: true`，并在 `capabilities` 中确认 `pureStep: true`、`controlMode: "scalar-nodal-force"` 与完全相同的执行器 ID 集合。当前只交换标量节点力，单位为 `N`；传感器单位由模型目录给出。

计算请求：

```json
{"protocolVersion":1,"requestId":"uuid","op":"compute","stepId":"42","time":0.41,"dt":0.01,"observations":[{"sensorId":"tip-displacement","value":0.001,"unit":"m"}],"acceptedControllerState":{"gain":100.0}}
```

计算响应：

```json
{"protocolVersion":1,"requestId":"uuid","op":"compute","stepId":"42","commands":[{"actuatorId":"tip-force","force":-0.1,"unit":"N"}],"nextControllerState":{"gain":100.0}}
```

`stepId` 使用十进制字符串，避免 JSON 浮点数不能精确表示大整数。每个已协商执行器必须恰有一条命令；已协商传感器必须恰有一条观测。缺字段、重复/未知 ID、单位不符、非有限数值、协议版本或请求号错误、超长消息、进程退出及超时都以失败关闭会话，不输出部分有效命令。标准错误仅作独立诊断，不参与协议解析。用户层的限幅、速率、延迟、采样周期、物理执行器映射、力功账、控制器状态检查点及 InteractionRecord 仍须由后续求解器集成层实现，不能由传输层假定。

v1 握手目前只确认传感器单位和执行器 ID 集合，**没有绑定模型 revision/网格与目录指纹、控制器标识或随机种子**。同一 ID 在模型变更后可能指向不同物理位置；集成层在运行期间必须冻结模型，并在正式对外稳定化前补齐上述身份与可重放字段。
