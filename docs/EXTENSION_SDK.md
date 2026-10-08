# Range Workflow 扩展 SDK v1

真实 active/passive FE 时程输入、来源哈希和 GUI 选择见 [NATIVE_RESULTS.md](NATIVE_RESULTS.md)。独立参考与调用方序列保留不同 sourceKind，native 输入在批准绑定和执行前后重验。

本文说明当前 Python 工作流服务支持的扩展接口。它描述的是已实现的窄接口：纯 Python 标准库、数值序列统计、宿主生成表单和结果表格。它不代表扩展可以增加有限元单元、材料、求解器内核或任意 GUI 控件。

协议版本为 `range.extension/v1`，承载命令的工作流协议为 `range.workflow/v1`。调用前先查询 `capabilities.query` 和相应版本化 schema；未知版本必须拒绝，不能按相似字段猜测。

## 1. 能力与信任边界

扩展以独立 Python 子进程执行。普通模式不执行扩展 Python。开发者必须显式以 `--trusted-extensions` 启动服务（或在开发 SDK 中显式设置 `trusted_extensions=True`）。`capabilities.query` 会返回：

```json
{
  "extensionRuntime": {
    "trustedExtensions": true,
    "trustMode": "trusted-developer",
    "osSandbox": false,
    "protocolVersion": "range.extension/v1",
    "maxRuntimeSeconds": 15,
    "maxOutputBytes": 65536,
    "maxConcurrentJobs": 4
  }
}
```

该开关只表明操作者选择了受信开发者模式，不是操作系统隔离。Python 代码以服务账户权限运行，仍可能访问该账户可访问的文件或网络。独立进程、`-I -S`、清理环境变量、超时和输出限制可降低部分风险，但不能限制文件系统、网络、注册表或 CPU 使用。不得向普通用户宣传它是安全的任意代码沙箱。

扩展子进程使用配置的 Python 可执行文件以及 `-I -S`，不继承供应商 API 密钥或调用方任意环境变量。环境只包含运行所需的临时目录变量、Windows 的系统根目录变量，或 POSIX 的默认 PATH。扩展依赖目前必须是空数组；运行时不会执行 pip、联网安装包或导入 NumPy/SciPy。扩展的显式输入/输出均为有界 JSON。

阶段、验证、激活、回滚和运行都通过宿主预览及单独的用户审批。模型工具目录不包含审批能力；模型提出的任务不会自动变成主机批准。`extension.validate` 与 `extension.run` 会执行 Python，因此除宿主审批外还要求可信开发者模式。`extension.stage` 仅保存候选代码，不运行代码。

## 2. 当前 v1 扩展契约

唯一支持的类型是 `numeric-series-statistics`，入口必须是 `extension.py:process`，签名为 `process(series) -> statistics`。不支持额外导入目录、任意入口、依赖安装、磁盘路径输入或扩展自建 GUI。

Manifest 的字段必须完全匹配以下结构；未知字段会被拒绝：

```json
{
  "manifestVersion": 1,
  "extensionId": "series.stats",
  "version": "1.0.0",
  "sdkVersion": "range.extension/v1",
  "kind": "numeric-series-statistics",
  "entryPoint": "extension.py:process",
  "dependencies": [],
  "inputSchema": "range.extension/v1/numeric-series",
  "outputSchema": "range.extension/v1/statistics",
  "gui": {
    "form": [{"id": "series", "type": "numeric-series", "required": true}],
    "table": {
      "columns": [
        {"id": "rms", "label": "RMS", "unit": "valueUnit"},
        {"id": "peakAbs", "label": "Absolute peak", "unit": "valueUnit"},
        {"id": "peakTime", "label": "Peak time", "unit": "timeUnit"}
      ]
    }
  }
}
```

`extensionId` 是 1–128 位 ASCII 字母、数字、点、下划线、冒号或连字符。版本采用 `major.minor.patch`，可带有限长度的连字符后缀。首版只接受上述固定 SDK、类型、入口、schema、表单、表格和空依赖声明；修改 UI 或结果字段需升级 schema/SDK，而不是增加未识别字段。

输入 schema `range.extension/v1/numeric-series`：

```json
{
  "times": [0.0, 1.0],
  "values": [3.0, 4.0],
  "timeUnit": "s",
  "valueUnit": "m/s"
}
```

`times` 与 `values` 长度相同，包含 1–100000 个有限数值，绝对值不超过 `1e100`；时间必须严格递增。单位是长度 1–32 的标签，格式为 `[A-Za-z0-9][A-Za-z0-9./*^_-]{0,31}`。schema 的点数上限不代表所有序列都能装入单个请求：JSON 请求上限为 256 KiB，实际序列另受 96 KiB 规范 JSON 上限约束。

输出 schema `range.extension/v1/statistics` 必须恰好包含 `rms`、`peakAbs`、`peakTime`、`timeUnit`、`valueUnit`。三个数必须有限且绝对值不超过 `1e100`；RMS 和绝对峰值不能为负；单位必须与输入相同。宿主将结果与独立计算对照，误差容限为 `1e-10 + 1e-10 * abs(reference)`。

该首版的 RMS 定义为样本算术均方根 `sqrt(sum(value²) / N)`，不是按时间积分的 RMS。`peakAbs` 是绝对值最大值；出现相同最大值时，`peakTime` 取第一个最大样本对应时间。算法不做单位换算、插值、窗口或滤波。

## 3. 源码与候选测试

扩展源码只需导出 `process(series)`。以下为示例实现：

```python
import math

def process(series):
    values = series["values"]
    times = series["times"]
    peak = max(abs(value) for value in values)
    index = next(i for i, value in enumerate(values) if abs(value) == peak)
    rms = 0.0 if peak == 0 else peak * math.sqrt(
        math.fsum((value / peak) ** 2 for value in values) / len(values)
    )
    return {
        "rms": rms,
        "peakAbs": peak,
        "peakTime": times[index],
        "timeUnit": series["timeUnit"],
        "valueUnit": series["valueUnit"],
    }
```

`tests` 必须符合以下结构，包含 1–16 个唯一 ID 的用例。每个输入和预期输出都使用上述 schema：

```json
{
  "cases": [
    {
      "id": "known-pair",
      "input": {
        "times": [0.0, 1.0],
        "values": [3.0, 4.0],
        "timeUnit": "s",
        "valueUnit": "m"
      },
      "expected": {
        "rms": 3.5355339059327378,
        "peakAbs": 4.0,
        "peakTime": 1.0,
        "timeUnit": "s",
        "valueUnit": "m"
      }
    }
  ]
}
```

测试文件上限为 96 KiB。验证不把生成测试当作唯一正确性依据：宿主另外运行三组固定的独立参考输入，并自己重新计算每组预期统计量；候选声明的 expected 值也必须与宿主结果一致。验证报告记录通过的用例数、独立用例数、结果和 runtime hash，报告最大 64 KiB。

## 4. 命令生命周期

扩展修改命令都使用工作流的预览、宿主审批、执行及异步作业查询。调用方先用 `project.query` 获取当前 `revision`，把它作为 `command.preview.expectedRevision`；预览返回 `previewId`、`previewHash`、规范化参数和绑定快照。宿主审阅预览后，调用 host-only `host.approve`，再用返回的 `approvalToken` 调用 `command.execute`。最终通过 `jobs.query {"jobId":"..."}` 轮询，成功时读取 `job.result`，失败时读取 `job.error`。

```json
{"operation":"project.query","parameters":{}}
{"operation":"command.preview","parameters":{
  "command":"extension.stage",
  "parameters":{"manifest":{},"source":"...","tests":{"cases":[]}},
  "expectedRevision":12
}}
{"operation":"host.approve","parameters":{
  "previewId":"<previewId>","previewHash":"<previewHash>"
}}
{"operation":"command.execute","parameters":{
  "previewId":"<previewId>","approvalToken":"<host-only-token>"
}}
{"operation":"jobs.query","parameters":{"jobId":"<jobId>"}}
```

示例中的 revision、ID 和 token 仅用于说明流程；实际值必须来自服务响应。host approval 不属于模型可调用工具。修改配置或完成另一个作业会改变全局工作流 revision，使旧预览失效；收到 `STALE_REVISION` 时应重新查询、生成预览并再次要求宿主审批，不得重放旧 token 绕过检查。执行请求幂等返回同一个 jobId。

固定命令及参数：

| 命令 | 参数 | 作用 |
| --- | --- | --- |
| `extension.stage` | `manifest`, `source`, `tests`; 可选 `baseBundleHash` | 创建不可变候选；不执行 Python。base hash 必须是同一扩展仍保留的版本；省略时使用当前活动版本（若有）。 |
| `extension.validate` | `candidateId`, `bundleHash` | 在可信开发者模式运行候选测试和宿主独立参考。只能验证 staged 候选一次。 |
| `extension.activate` | `candidateId`, `bundleHash` | 激活该确切、已验证的 bundle；不修改源码或依赖。 |
| `extension.rollback` | `extensionId`, `expectedActiveBundleHash`, `targetBundleHash` | 把后续调用指向一个仍保留的确切版本。 |
| `extension.run` | `extensionId`, `bundleHash`，以及 `series` 或 `provenance` 二选一 | 对活动版本运行一次带哈希输入快照的计算。 |

stage 成功结果包含 `candidateId`、`bundleHash`、`state: "staged"` 和 manifest/source/tests 三个 SHA-256。validate 成功结果包含 `state: "validated"`、`runtimeHash`、报告哈希和通过用例数。activate 成功结果包含 `state: "active"`、`previousBundleHash` 和 `capabilityRevision`。rollback 成功结果包含新的 `activeBundleHash` 和 `capabilityRevision`。

`extension.run` 的显式序列形式：

```json
{
  "extensionId": "series.stats",
  "bundleHash": "<64-lowercase-hex>",
  "series": {
    "times": [0.0, 1.0],
    "values": [3.0, 4.0],
    "timeUnit": "s",
    "valueUnit": "m/s"
  }
}
```

或者传 `provenance`，且不得同时传 `series`：

```json
{
  "extensionId": "series.stats",
  "bundleHash": "<64-lowercase-hex>",
  "provenance": {"jobId": "<successful-workflow-job>", "signal": "reference.active.sensorVelocity"}
}
```

允许信号为 `reference.active.sensorVelocity`（m/s）、`reference.active.energyJ`（J）及 `reference.passive.energyJ`（J）。服务重新解析成功工作流作业的受管 `referencePath` 工件，验证 artifact SHA-256，并在运行预览中固定序列及来源。此类数据明确标为 `independent-reference-history`：它是独立参考分析历史，不是原生 FE 求解器的 accepted-step 历史。显式输入的结果标为 `caller-supplied`。运行结果包含 extension/version/bundle、`seriesSha256`、来源类型/作业/信号/工件哈希和 `metrics`。

## 5. 只读发现接口

这些接口不产生作业、不激活功能，也不授予执行权限：

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `extension.list` | `{}` | 紧凑摘要：`{apiVersion, capabilityRevision, extensions:[...]}`。根查询最多 128 个扩展，不包含源码或报告。 |
| `extension.list` | `{"extensionId":"series.stats","candidateOffset":0,"candidateLimit":16}` | 单扩展详情：版本列表、候选页、`candidateTotal`、`nextCandidateOffset`。offset 范围 0–64，limit 默认为 16、范围 1–32；翻页时将 `nextCandidateOffset` 作为下一次 offset。 |
| `extension.candidate.get` | `{"candidateId":"<id>"}` | 完整候选 manifest/source/tests、hash、验证、报告、base source 和服务生成的 unified diff。 |
| `extension.version.get` | `{"extensionId":"series.stats","bundleHash":"<sha256>"}` | 活动或保留版本的不可变 manifest/source/tests/报告/hash。 |
| `schema.get` | `{"schemaId":"range.extension/v1/manifest"}` | 版本化 schema。可用 ID 为 manifest、numeric-series、statistics。 |
| `knowledge.query` | `{"query":"numeric series RMS","extensionId":"series.stats"}` | 有界文档片段和可选的活动扩展 GUI/schema 摘要。文档名和路径固定，不接受任意文件路径。 |

`extension.list` 根摘要中每项含 `extensionId`、`registryRevision`、`activeBundleHash`、`activeVersion`、`versionCount`、`candidateCount`。详细查询中候选摘要包含 `candidateId`、`version`、`bundleHash`、`state`、manifest/source/tests/report hash、可选 `baseBundleHash`。要查看实际 source、form 和验证报告，应再调用 candidate/version get；列表不会内嵌源码。

## 6. 不可变版本、并发与资源限制

候选源码、manifest、测试和报告以 SHA-256 保存；活动版本用精确 bundle hash 标识。激活只原子切换活动指针，旧版本保留。正在运行的调用固定 bundle/runtime/input hash；激活或回滚只影响后续调用，不会让运行中的作业改读新活动指针。当前实现保留每扩展最多 64 个候选和 32 个已激活版本，不自动删除仍被注册的版本。

项目工作流使用一个项目执行槽；扩展工作使用独立槽，最多 4 个并发扩展作业，因此可以与 FE 计算同时运行。对同一 extensionId 的 stage/validate/activate/rollback 注册表写操作串行冲突；对不同扩展的写入不互相覆盖，提交时以扩展级 registry revision 做比较交换。新请求仍需匹配全局 project revision，所以并发完成的任一作业都可能使尚未执行的旧预览过期。

| 内容 | 上限 |
| --- | ---: |
| 单条工作流 JSON 请求 | 256 KiB |
| 扩展源码 UTF-8 | 64 KiB |
| 扩展测试 JSON / 单次数值序列 JSON | 96 KiB |
| 候选测试用例 | 16（另加 3 个宿主参考用例） |
| 子进程运行时间 | 15 秒 |
| 子进程单个 JSON 输出 / 验证报告 | 64 KiB |
| 每扩展候选 / 激活版本 | 64 / 32 |
| 扩展并发作业 | 4 |

Python 路径、解释器文件 SHA-256、Python 版本、相邻 `runtime-lock.json` 与 `runtime-files.json` 哈希、`extension_child.py` harness 哈希、隔离启动参数和协议版本共同形成 runtime identity/hash。只有两种清单都不存在时，profile 才会标记为 `development`；缺少其中一种或清单校验失败会拒绝运行。它不会伪装成打包锁定环境。验证、运行或回滚目标需要与当前 runtime hash 一致，否则返回 `RUNTIME_CHANGED`。

打包运行时的 `runtime-files.json` 列出运行时相对路径及 SHA-256。宿主会拒绝绝对路径、路径穿越、符号链接、缺失或未列出的文件，并核对解释器和锁文件哈希。完整文件哈希在验证/运行等 runtime 身份检查中使用；缓存命中仍逐文件检查文件系统元数据，发现变化后会重新哈希。`runtime-files.json` 自身哈希也包含在 runtime identity 中。该校验用于发现已打包环境被改动，不构成操作系统隔离或对同账户恶意写入的安全承诺。

尚未执行的扩展源码/测试/数值输入预览共占用最多 8 MiB 的规范化参数额度。超出时返回 `PENDING_PREVIEW_LIMIT`；批准并执行一个动作会压缩旧 revision 的待处理预览元数据，旧预览之后会以 `STALE_REVISION` 被拒绝。工作区整体状态文件另有 64 MiB 上限。

## 7. 错误和处置

服务错误以 `range.workflow/v1` 错误信封返回机器码和通用信息。常见情况：

| 错误码 | 含义与处理 |
| --- | --- |
| `INVALID_PARAMETERS` / `UNSUPPORTED_VERSION` | 字段、范围或 schema 版本不符；查询 schema 后修正，不要移除宿主约束。 |
| `STALE_REVISION` / `EXTENSION_CONFLICT` | 项目预览或该扩展注册表已经变化；重新查询并生成新预览。 |
| `TRUSTED_RUNTIME_REQUIRED` | 服务没有显式启用受信开发者模式；普通模式不能执行代码。 |
| `EXTENSION_TAMPERED` / `SOURCE_CHANGED` | 固定源码、文件路径、工件或哈希发生变化；停止该版本调用并检查存储。 |
| `EXTENSION_TEST_FAILED` / `EXTENSION_REFERENCE_MISMATCH` | 候选或运行输出与宿主独立统计值不同；不要激活或呈现为通过。 |
| `TIMEOUT` / `EXTENSION_OUTPUT_LIMIT` | 子进程超时、输出格式错误或超过上限；保留旧活动版本。 |
| `EXTENSION_BUSY` | 扩展并发上限或同一扩展写冲突；待作业结束后重新查询。 |
| `RUNTIME_CHANGED` | 当前解释器/锁身份与验证版本不符；在目标 runtime 下重新验证候选。 |

被拒绝的请求和失败作业不会激活候选。任何 GUI 或 Agent 都只能把 `jobs.query` 返回的成功状态/结果作为成功证据；不能根据模型文字、进程启动或预览成功推断扩展已运行。
