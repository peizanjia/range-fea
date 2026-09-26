# 型别控制的实际 FE、进程协议与续算

此入口把接受边界、纯控制器状态和按通道条件化扩展到拉索长度/初张力/规定锚点位移及压电梁 V/C，使用 NDJSON v2。原有 Stage 6 力控制 v1/Newmark 路径继续使用原协议。这里的梁和参数控制采用准静态平衡宏步，不是压电振动积分器或电路仿真。

## 从原生模型启动

`fea-solver` 支持完整 `.ram`/`.rbm` 模型与版本 1 JSON 配置；模型链接不作为初始化输入。拉索模型须启用受支持的 native cable 配置。压电模型使用直线 +X、局部 Y 为 +Y 的 BEAM1/平面 XY 悬臂，机械条件作用于点组；不启用 native 杆件 piezo/cable 或动力/热控设置。活动边界和环境表只接受一个常数值，梁保持 293.15 K；非控制时间曲线或温度场明确拒绝。

配置示例在 `src/fea-solver/examples/typed_cable_config.json` 与 `typed_piezo_beam_config.json`。它们是配置模板，必须与用户原生模型的单元/节点索引、材料、支承及初始物理值一致。不能仅凭模板得到完整 FE 模型。

初始化输入必须是完整模型内容；`RModel::write(path)` 默认生成 latest 链接，C++ 导出时用 `write(path,false)`，GUI 导出则选实际编号的模型工件。型别 CLI 默认使用串行确定性策略，显式 `--nthreads` 可选择固定线程并记录实际线程数及降级诊断；多线程不承诺跨机器位级重放。

PowerShell 示例（程序路径自行指定）：

```powershell
$controllerArgs = ConvertTo-Json -Compress -InputObject @(
    'D:/path/control_typed_ndjson_python.py', 'normal',
    '{"length":1.98,"tension":8.0,"anchor":0.05}')
& 'D:/path/fea-solver.exe' --file='D:/path/cable.rbm' `
    --typed-config='D:/path/typed_cable_config.json' --typed-steps=3 `
    --typed-checkpoint='D:/path/accepted.sfec' --typed-controller-id=python-typed-v2 `
    --control-program='D:/path/python.exe' --control-args-json=$controllerArgs
```

配置必须包含 `version=1`、`mode=cable|piezo_beam`、十进制字符串 `seed`、秒单位 `dt`、控制器实现/版本标识 `controller` 和有序 `channels`。通道包含 `id`、`quantity`、`initial`，以及成员/片的 `element` 或锚点的 `node`/`axis`（0/1/2 对应 X/Y/Z）。可选 `minimum`、`maximum`、`rate`、`delay`、`hold`、`noise` 分别按通道的 SI 单位和步数解释；省略上下限/速率代表不限幅/不限速，JSON 非有限值仍拒绝。初始值必须精确等于模型输入。

`quantity` 为 `cable_reference_length` (m)、`cable_initial_tension` (N)、`cable_anchor_displacement` (m)、`voltage` (V) 或 `charge` (C)。长度必须正，初张力非负；锚点命令是已有规定分量的绝对位移。压电增加 `electricMode=voltage|charge` 与 `patches`；片参数 `area/thickness/offset/modulus/coupling/permittivity/initial` 分别采用 m²/m/m/Pa/C·m⁻²/F·m⁻¹/V 或 C，每片是独立电极对。

## 接受态续算与事务

再次运行时以 `.sfec` 作为 `--file`，省略 `--typed-config`。`--typed-steps` 表示本次追加的步数（1–1000），控制器标识仍必须精确一致。输入与输出可为同一个 `.sfec`。包内包含原生二进制模型、片输入、通道策略/绑定以及已接受的时钟、控制器状态、延迟/保持队列、已施加命令与累计功。

每次调用在内存中完成全部请求步，发布前重新核对输入、配置及目标文件的路径/哈希，持有规范路径锁，以 QSaveFile 一次发布。失败前原有接受包保持不变；新程序从该包重试，不读试算控制器状态。`*.events.ndjson` 区分 `TypedStepPrepared` 与 `TypedCheckpointCommitted`，试算事件不是已写入磁盘的接受证明。包发布后的日志故障显式报告已经提交，不伪称回滚。

## 功账和数值边界

拉索沿目录顺序分别改变参数或支承，每个通道的功为该步骤驻值总势能变化；顺序可改变多通道功的分配，所有通道和具有望远镜相消关系。压电的导体端口电荷为 `Q=C*V-g^T*u`，步功为线性可逆路径的 `midpoint(V)*ΔQ`。输出 `averagePowerW=stepWorkJ/dt` 为区间平均功率；它不是瞬时电流测量。拉索初张力与参考长度变化不等同于固定外力输入。

控制器请求只包含已接受观测和 opaque state。Python 示例读取标准输入，标准输出仅写协议 JSON；它演示常值命令和纯计数器状态。实际控制律须由用户替换，并为控制器版本设置稳定标识。协议严格核对通道 ID、物理量、单位、有限值、请求/步骤 ID、状态编码以及配置摘要，不能用 N 命令驱动 V/C。

当前验证包含独立拉索参数导数差分、真实 FE 参数/锚点解析位移与功账、压电 2/4/8 梁片解析弯曲及互易、V/C 等价和磁盘续算。总体回归与当前限制见 `HANDOFF.md` 和阶段审计。本接口不承诺一般曲梁/壳片、非匹配重网格、压电动态电路或硬实时保证。
