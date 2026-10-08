# 当前文档控制与原生结果

接口版本：range.native-results/v1、range.workflow/v1、range.host/v1。实际验收状态见 workflow_delivery_plan.json 与 WF-03/WF-04 证据，接口存在和测试注册不代表验收通过。

## 文档来源与批准

在 Dynamics & control → Experiment 打开工作区，选择支持文档，点击 **Use current document snapshot**。可信宿主冻结接受态并登记不可变副本，不用模板替代文档。来源显示版本、模型/manifest SHA、物理节点和全局端口。重开工作区恢复登记列表；旧 GUI 实例仍过期，不自动获得新实例批准。

首版支持以下矩阵，超范围时返回具体字段、诊断代码与原因，不回退模板。

| 来源 | 支持范围 |
| --- | --- |
| 四杆链 | 5 节点、4 TRUSS1、6 POINT；全局 X 共线连通，根 XYZ 固定、全链 Y/Z 固定 |
| 平面梁 | 2 节点、1 BEAM1、2 POINT；全局 +X、局部 Y 对齐全局 Y；根六自由度固定，末端仅 UY/RZ 自由 |
| 材料/截面 | 常量 solid E/rho/nu；正杆面积；梁正 A/Iy/Iz/J/Wy/Wz/Wt，后者用于真实应力恢复 |
| 动力学 | Stress-only、consistent mass、Newmark gamma=.5/beta=.25，Rayleigh alpha/beta=0、无 modal ratios |
| 初态/端口 | 声明的常量 IC；末端全局 X（杆）或 Y（梁）的 nodal force N、velocity m/s |

Fresh source 拒绝保存的位移/转角/速度/角速度结果，初态来自 IC。实际 force 限制 ±1e6 N，独立参考同限幅。全零初态可成功；被动末能量零时比值为 null，表示不可定义。

配置和 fresh run 的批准前、批准返回后均核 runtime 身份、持久版本、SHA、草稿和持久化可用性。提交后 A 不受 B 编辑或来源关闭影响。Resume 固定原接受结果，不要求原 GUI 仍打开。

```python
source = client.register_model_source(catalog_root, snapshot_ref)
preview = client.preview("workflow.configure", {
    "sourceId": source["sourceId"], "dt": 0.005, "steps": 40, "targetDamping": 0.1})
# 可信宿主取得授权后才调用 approve/execute。
job = client.execute(preview, client.approve(preview))
```

snapshot_ref 只含 projectId、persistentDocumentId、sourceRevision、modelSha256、instanceRef。文件/进程拥有者 SDK 显式提供目录；产品 Agent 不接受任意路径、注册文件或自批。登记已发生但审计失败返回 SOURCE_REGISTERED_AUDIT_FAILED 和已存在 sourceId；重开后先查询列表再重试。

## 真实 FE 数据

普通作业使用 host `native.result.catalog/channels/series/summary.query`；工作区使用 `result.catalog/channels/series/summary.query` 和 active/passive mode。模型工具为 native_result_query、workflow_native_result。只接受登记 job/artifact/channel 身份，不接受路径。

| 操作 | 内容 |
| --- | --- |
| catalog | 实际幀、来源、输入/控制/runtime 哈希、analysis、验证范围、分页历史 proof |
| channels | 稳定 node:physicalId:quantity:axis、全局轴、变量/单位，缺量不补零 |
| series | channelId、分页 recordNo/timeS/value/originKind/origin/historyProof |
| summary | 同一实际样本的 min/max/mean/RMS |

页面上限 **200**，参数 offset/limit 或 cursor。游标固定 artifact/generation；每次核 manifest、terminal MODEL、全部幀和已知祖先，不只请求页。OutputFrequency 缺步保持稀疏，不补计划时间或插值；静态/模态不能当时程。平移与角量单位分别 m、m/s、m/s²、rad、rad/s、rad/s²。无旋转 DOF 的杆节点没有角通道。

Live 只读 solver 接受事务后发布的完整 MODEL 幀，不读 partial journal 尾行。自动来源等待首幀、跟随 generation，分页中固定快照，成功后转 immutable manifest。显式 artifactId 保持固定；Hold 冻结显示而求解/采集继续。

produced 为当前 job 产生；reused-history 保留原 job/source/runtime、原/复制幀 SHA 和祖先 proof。旧结果没有 manifest 时 legacy-history-unattributed 原来源 null，验证只覆盖捕获 restart 输入和复制字节。已有祖先校验失败不能降级 legacy；跨 session 首次捕获范围 captured-source-input，不宣称具有旧作业账本证明。

## GUI、SDK、扩展

Workflow FE output 展示实际 active/passive 幀；Document FE output 展示普通文档作业。曲线、表格、summary、冻结 CSV 使用同一 broker。CSV 每行保留身份、单位、范围、来源及逐幀 origin/historyProof，分页后的历史不丢来源。

普通完成工件可点击 **Show terminal result field**，或使用 host `native.result.display` / Agent `native_result_display`，精确参数为 jobId/artifactId/channelId。宿主核验工件后在独立结果文档定位物理节点，显示该 incident entity group 的完整变量模长；时程仍为所选全局分量，模长不冒充分量。显示不修改源模型、数值版本或 Undo。旧的 native.result.open(jobId, expectedSHA) 保持可用。

```python
catalog = client.native_result(job_id, mode="passive", limit=1)
channels = client.native_result(job_id, "channels", mode="passive", limit=200)
history = client.native_series(job_id, channels["channels"][0]["channelId"], mode="passive")
from range_workflow import compare_native_series
comparison = compare_native_series(active_history, passive_history)
```

比较只允许同模型 SHA、物理通道、单位、全局轴和实际时间网格，无隐式变换。NativeResultsClient(executable, manifest, expected_sha) 是文件作用域 SDK，不单凭调用方 DTO 宣称 GUI job 成功；工作区另查成功状态和工件账本。

Extensions 可选真实 active/passive FE，provenance 为 jobId/mode/artifactId/channelId。服务在绑定、执行前后重验源，保存 manifest SHA、逐幀来源摘要、范围和固定 bundle/runtime。Independent reference/caller-supplied 保留兼容入口与 sourceKind，不改名为 FE。旧结果可读；缺新证明的旧 template resume 明确 SOURCE_REFERENCE_UNAVAILABLE，需重算，不造模板哈希。

SDK/CLI 随安装发布。可信扩展仍为开发者模式，不新增 OS 隔离或跨崩溃恢复承诺；后续 WF-05/WF-10/WF-11/WF-12 范围独立。
