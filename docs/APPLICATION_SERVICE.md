# 应用服务：模态配置编辑会话 v1

本模块是 Stage 10A 的首个实际配置服务，位于 `src/range-application-lib`。它依赖 Qt Core 和模型库，不依赖 GUI 或求解器。此阶段仅交付内存会话及契约测试；GUI/CLI 接线、文件发布和持久审计须分别验收。

## 所有权与边界

`RModelCommandSession` 构造时复制输入 `RModel`，外部只能读取接受态。所有访问由同一个拥有线程执行；返回的模型引用仅在下次成功命令前有效，不允许跨线程或通过 const_cast 修改。每个实例具有新的 modelId，revision 是该实例的单调版本，不是源文件哈希或 GUI 全局版本。持久化、原模型冲突检查和磁盘发布是调用端的独立职责。

`query()` 返回协议版本、modelId、revision、当前 modal 参数、是否支持模态配置、撤销/重做可用性及剩余命令容量。`canConfigureModal` 只表示模型任务树包含结构模态任务且可编辑配置，不证明该模型一定能求解或通过数值验收。

## 命令

`execute(QJsonObject)` 接收如下精确封套（多余字段、缺失字段和错误类型拒绝）：

```json
{
  "apiVersion": "range.application/v1",
  "commandId": "analysis.modal.configure",
  "commandVersion": 1,
  "modelId": "从query取得",
  "expectedRevision": "从query取得",
  "requestId": "client-request-1",
  "idempotencyKey": "client-operation-1",
  "parameters": {
    "method": "multiple",
    "iterations": 100,
    "modes": 3,
    "tolerance": 1e-9,
    "unit": "1"
  },
  "dryRun": false
}
```

方法只接受 `dominant`/`multiple`；迭代数 1–1000000，模态数 1–1000，容差有限且在 `(0,1]`。全部量无量纲，unit 必须为 `1`，不做隐式单位转换。模式/频率结果字段不作为配置输入。服务不计算模态，也不把配置合法等同于求解可行。

另有 `model.undo` 和 `model.redo`，使用同一封套、空 parameters；只撤销本会话内的配置命令。它们生成新 revision，不恢复旧版本号。空历史返回 `empty_history`。普通新配置清除 redo 分支。即使配置值不变，一次成功命令仍占用一个版本和历史项。

## 事务与重试

- `dryRun=true` 校验并返回 `preview` 和候选 modal，revision、历史和幂等账本均不改变；可用同一个键提交对应正式请求。
- 成功返回 `accepted`、preRevision、新 revision 和配置。候选模型、历史及账本都构造完毕后才交换接受态；拒绝不消耗键或版本。
- 同键、完全相同封套（含 requestId）的重试只在该命令结果仍是当前 revision 时返回原响应，不重复执行。不同内容使用旧键返回 `idempotency_conflict`；其他编辑或撤销后的旧重试返回 `stale_revision`。
- 每个会话最多接受 256 个命令（含撤销/重做）。达到上限拒绝继续提交，不驱逐旧键；调用端应发布或丢弃会话再重新打开。预览仍可使用。
- 请求限制 64 KiB；标识字符串须非空且不超过 128 字符。非法协议/字段为 `invalid_request`，参数为 `invalid_parameters`，跨会话身份为 `wrong_model`，没有模态任务为 `unsupported_analysis`。

这不是跨文件事务或持久 InteractionRecord，也不是已对 Agent 开放的执行权限。CLI/GUI 发布前仍须验证源模型未变及运行状态，不能绕过原有求解前置检查。

## 验证

`tst_model_command_session` 验证真实 RModel 配置、输入隔离、预览、参数反例、身份/版本、幂等冲突、失败原子性、撤销/重做和容量上限。以主构建注册的 CTest 运行，构建和结果记录见 HANDOFF.md。
