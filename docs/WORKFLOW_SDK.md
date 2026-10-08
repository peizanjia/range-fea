# 进程 SDK 与私有 Python 运行环境

## 使用边界

`range_workflow.WorkflowClient` 是受信客户端 SDK，使用 `range.workflow/v1` JSON-lines 协议启动独立工作区服务。GUI、外部 Python 客户端和 Agent 复用该服务的参数校验、预览、批准及作业记录。GUI 打开的文档另由 `range.host/v1` 管理；不能通过工作区 SDK 推断当前 GUI 模型状态。

外部进程不能同时占有同一工作区。连接超时或进程退出后，重新打开工作区并查询持久化作业状态，再决定是否继续。GUI 宿主的内存事件游标与跨进程的工作区审计版本是不同概念。

若工作区已有宿主 `project.json`，服务保持相同 projectId；与既有工作流状态的身份冲突会明确拒绝。`client.project_catalog()` 读取目录元数据，`client.project_catalog(persistent_id)` 筛选文档；`client.project_snapshot(persistent_id)` 在返回冻结路径前检查来源版本与实际 SHA-256。元数据查询的 `snapshotBytesVerified=false` 不代表已验证模型字节。目录格式、CLI 创建方法及生命周期见 `PROJECT_CATALOG.md`。

## 安装后的独立客户端

安装目录包含 `bin/`、`runtime/python/`、`share/range-workflow/`、`examples/control/stage9/`。私有解释器的路径配置已经包含 SDK，不需要安装系统 Python 或设置 PYTHONPATH。

```powershell
& '<安装目录>/runtime/python/python.exe' '<安装目录>/share/range-workflow/examples/compare.py' `
  --installation '<安装目录>' --workspace '<用户可写工作区>'
```

默认只返回能力目录。加 `--run` 明确授权示例的固定配置和 20 步梁主动／被动对照；`--model four-rod` 使用四杆参考模型。返回的完整结果包含独立参考误差和源工件，不把失败的作业当作成功。

## 客户端生命周期

```python
from range_workflow import WorkflowClient

with WorkflowClient(workspace, solver, native, examples) as client:
    capabilities = client.request("capabilities.query")
    state = client.request("project.query")
    preview = client.preview("workflow.run", {
        "mode": "both", "independentReference": True
    }, expected_revision=state["revision"])
    # 此处由受信宿主展示固定预览并取得授权；不要交给模型自行批准。
    approval = client.approve(preview)
    accepted = client.execute(preview, approval)
    job = client.wait(accepted["jobId"])
    if job["state"] == "succeeded":
        result = client.request("results.query", {"jobId": job["jobId"]})
```

在把 SDK 用作模型工具时，只暴露经过白名单筛选的查询和提案入口；不要暴露通用 `request`、`approve` 或批准凭据。工具参数必须由本地服务再次验证。现有 GUI Agent 已将模型请求与宿主批准分开。

## 运行中的原生控制样本

不用等待 `client.wait()` 才能观察控制信号：

```python
page = client.telemetry(accepted["jobId"], after_seq=0, limit=256)
for sample in page["samples"]:
    print(sample["timeS"], sample["sensors"], sample["actuators"])
cursor = page["nextSeq"]  # 下一次传入 after_seq；hasMore=True 时继续读取
```

底层为只读 `telemetry.query`，固定任务/模式，不接受任意文件路径。空页不等于计算完成；查看 `state`、`availability` 和 `hasMore`。样本来自 `ControlStepAccepted` 日志，传感器值对应接受状态时间，实际施加力/功率/累计功携带原生执行记录语义。被动计算没有该控制通道时明确返回不可用，不填造零曲线。GUI 暂停显示只冻结图像，不停止数据收集或求解器。完整交互约定见 `GUI_INTERACTION_DESIGN.md`。

## 私有运行环境构建

构建器使用 CPython 官方嵌入包和固定哈希的 NumPy/SciPy wheel，保留包内许可文件。首次准备缓存需要网络，缓存完整后可离线组装。最终产品不包含运行时 pip 安装流程，也不读取用户 site-packages。

```powershell
python scripts/packaging/build_private_python.py --cache build/runtime-cache --output build/private-python
cmake -S src -B build/vs2026 -DRANGE_PRIVATE_PYTHON_DIRECTORY='<新运行环境绝对路径>'
cmake --install build/vs2026 --config Release --prefix '<安装目录>'
```

已有缓存时加 `--offline`。已有输出目录会拒绝覆盖；更新依赖应修改锁文件、生成新目录、运行测试后随新版本安装。运行环境 ID、归档哈希及展开文件清单分别保存在 `runtime-lock.json` 和 `runtime-files.json`，源码中的锁文件不含密钥或用户路径。

CPython 嵌入包的隔离路径行为见 [Python 官方文档](https://docs.python.org/3.13/using/windows.html#the-embeddable-package)。这种包适合随应用分发，但不等于安全沙箱。开发用系统解释器只是未附带私有环境时的回退路径；不能以开发机成功代替干净机器安装验收。

## 扩展与兼容

扩展使用独立的 `range.extension/v1` 版本、声明式输入输出和受限界面描述。生命周期命令仍通过 SDK 的 preview/approve/execute 流程执行；候选代码、测试、报告和激活版本必须分别记录。具体命令 schema 由运行时查询，不能使用本手册中未列出的假定接口。

当运行时不具备已验证的操作系统隔离后端时，任意 Python 扩展只能在显式受信开发者模式运行。SDK 的 `trusted_extensions=True` 必须来自用户／宿主配置，不能映射成模型可调用工具。普通模式默认拒绝扩展代码执行。
