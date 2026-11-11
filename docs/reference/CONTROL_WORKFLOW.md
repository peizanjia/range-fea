# 杆／梁振动控制工作流

本模块是 Stage 10B 的进程工作流入口，复用原生 FE、Stage 9 分析和控制器设计。它不是任意 GUI 选中模型的求解入口，也不是已完成的 Stage 11 Agent 或 Stage 12 插件 SDK。GUI 目前交付的是模态配置生命周期；工作流面板仍待后续开发。

## 环境与启动

需要 Python 3.10+、NumPy、SciPy，以及同一构建/安装中的 `fea-solver`、`range-workflow-native`。源码 Python 包位于 `src/range-workflow/python`，安装后位于 `share/range-workflow`；将相应目录加入 `PYTHONPATH`。算例目录在源码中为 `src/fea-solver/examples/stage9`，安装后为 `examples/control/stage9`。Windows 原生工具还需要安装 bin 或对应 Qt bin 在 PATH 中。

```text
python -m range_workflow --stdio --workspace <绝对工作目录> \
  --solver <fea-solver绝对路径> --native <range-workflow-native绝对路径> \
  --examples <安装或源码examples/stage9目录> --python <Python绝对路径>
```

以上是参数示意，换行符按所在 shell 调整。工作目录由一个服务进程独占；重开会产生新会话，使旧批准失效。stdout 每行一个 JSON，诊断使用 stderr。

## 版本化接口

请求精确字段：`apiVersion: "range.workflow/v1"`、`requestId`、`operation`、`parameters`。回复精确字段：`apiVersion`、`requestId`、`status`、`code`、`result`。查询 `capabilities.query` 获取当前命令及参数 JSON Schema，不依赖本文复制的字段列表进行校验。

| 操作 | 用途 |
| --- | --- |
| `project.query` | 当前项目、模型模板、配置及 revision |
| `command.preview` | 规范化命令，绑定 revision、工具和来源文件哈希 |
| `host.approve` | 可信宿主在审核固定预览后批准；不在 Agent 工具目录中 |
| `command.execute` | 验证批准并返回任务 ID；相同执行请求不重复启动 |
| `jobs.query` / `job.cancel` | 查询状态或请求取消 |
| `results.query` | 查询已成功结果；失败、取消、中断任务不可作为成功结果读取 |
| `audit.query` | 查询持久化交互记录 |

命令为 `workflow.configure`、`workflow.run`、`workflow.resume`。配置包含秒制 `dt`、步数 `steps`、无量纲 `targetDamping` 及模板 `model`。模板明确限定为 `four-rod` 和 `cantilever-beam`，不导入用户任意文件。运行模式为 `active`、`passive` 或 `both`，要求 `independentReference: true`；续算传原任务 `jobId` 与 `additionalSteps`。

## Python 宿主调用

```python
from range_workflow import WorkflowClient

with WorkflowClient(workspace, solver, native, examples, python) as client:
    preview = client.preview("workflow.configure", {
        "model": "four-rod", "dt": 0.005, "steps": 400, "targetDamping": 0.1,
    })
    print(preview)  # 在宿主界面审核该固定内容及来源摘要。
    # 仅在用户批准或已授权的批处理范围内调用 approve。
    if input("批准此预览？输入 yes：") == "yes":
        job = client.execute(preview, client.approve(preview))
        print(client.wait(job["jobId"]))
```

配置成功后，以相同预览、批准、执行顺序提交 `workflow.run` 的 `{"mode":"both","independentReference":true}`。任务状态成功后读取 `results.query`，再按需预览续算。这里的客户端是工作流接线组件，稳定扩展协议、兼容矩阵及自动开发验收仍属于后续 Stage 12。

## 工件、数值证据与恢复

- 每个任务有独立 UUID 目录，保存输入快照、导出矩阵、增益、控制脚本、参考解、日志、实际 FE 检查点和校核 JSON。来源及续算依赖按内容哈希再次校验，修改增益、控制实现或检查点会拒绝续算。
- 四杆和梁分别使用实际 TRUSS1 和 BEAM1。梁保留端点弯曲平移及转角自由度；平移误差使用 m、m/s，转动误差使用 rad、rad/s，能量使用 J。两种模型都与独立 NumPy Newmark 回放比较。模板验证不扩大现有求解器支持矩阵。
- 成功、失败及审计与工作流状态存入同一原子替换的 `workflow-state.json`。取消终止求解和控制子进程；服务崩溃后未终结任务标记为 `interrupted`，不推断为成功，也不自动重试。
- 已失败任务的中间文件用于诊断，不能仅按文件存在判断完成。重试需要新预览和批准；续算只接受已成功且哈希仍匹配的来源任务。
- 进程目录和资源限制不等于操作系统安全沙箱。该入口只运行宿主指定的已安装工具；没有接入生成代码或远程模型。

验收证据及未完成项以 `../HANDOFF.md` 顶部为准。
