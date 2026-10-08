# 项目目录、身份与不可变模型快照 v1

接口：`range.project/v1`，存储 `project.json` 的 `storageVersion=1`。本模块是 WF-01 的共享 Qt Core 目录及 Python 只读镜像；完成证据另记 WF 验收报告。它不将独立模板控制实验自动改为当前 GUI 文档求解。

## 身份与生命周期

- `projectId/persistentDocumentId/sourceRevision(rN)` 持久化；`range.host/v1` 的运行期 `documentId/sessionEpoch/documentRevision` 保留原含义，通过实例引用关联。
- 同名文档允许存在；显示标签不能解析为唯一修改目标。原生源路径及其 LINK/目标文件别名具有唯一所有者。
- 新文档、复制文档使用新持久 ID；复制保留 origin，默认无保存路径。关闭释放实例所有权但不 tombstone；重新打开同一已登记源保留持久 ID、创建新运行期引用。
- 当前文档的保存/另存为保留持久 ID，以原生 write 返回的逻辑基名为来源；过期读出的模型不能冒充更新后的源文件，改为独立未保存副本并显示来源已改变。
- 另一活跃实例已经持有文档时，重新打开其保存源不会替换该实例的未保存接受态；GUI 使用独立 origin 副本。关闭后重新接受保存的源可推进持久版本，旧冻结快照进入可追溯历史。
- 仅选择文档、刷新条件列表或重建派生显示缓存不推进接受版本。GUI 读取/初始化填充原生网格边长、缺失色标或同步表面单元方向时，真实变化会记录为未保存；重载结果若保留了不同于输入文件的原时间网格，也保持未保存。savedArtifactHash 仍指实际保存源，冻结快照反映当前接受值。

## 目录与锁

`project.json` 原子替换，`.project.lock` 协作锁保护元数据与原生保存保留区；每份打开的文档还持有 `models/<id>/.instance.lock` 实例所有权。锁绑定规范工作区、文档及运行期实例。原生保存先用 `RModel::nativeWritePaths()` 固定全部实际输出，保留锁跨越后台写入与目录提交，拒绝覆盖其他文档或受管理工件目录。

冻结模型位于 `models/<id>/snapshots/<sha256>/model.rbm`。临时序列化也用二进制 `.rbm`，按哈希发布；变化版本归档旧指针，原文件保留。它是模型快照，不自动成为求解成功结果，也不提供求解器中断续算。

原生保存可能产生逻辑 LINK 和编号记录文件。`sourceArtifacts` 保存从逻辑路径到最终 MODEL 的有序路径/哈希链，`resolvedSourcePath` 指最终文件，`savedArtifactHash` 为最终 MODEL 字节哈希。既检查 LINK 又检查目标，链有深度/格式/大小限制；保存中查询显示 publishing/缓存状态，不将自己的写入识别为外部冲突。

## 当前入口

GUI：File → Project → Open project workspace；切换到另一工作区时须关闭当前文档，避免隐式重绑定。默认目录为应用 dataDir 下 `projects/default`。选中一个文档后可 Freeze selected document snapshot。界面输入的未接受数字保留为草稿；目录/Agent 仅看到字段和状态，不保存原始草稿文本或凭据。重开后的模态草稿若无法恢复原文，界面显示提示与 Keep accepted value；点击仅清除该字段的草稿描述，不产生数值修改或 Undo 记录，也可输入新值完成修改。

宿主只读操作（`range.host/v1` 封套）：

```json
{"apiVersion":"range.host/v1","requestId":"example","operation":"project.query","parameters":{}}
```

`document.project.query` 参数为当前 `target={documentId,sessionEpoch}`。`context.query` 的各文档新增 project 信息；草稿和未保存接受态分别描述，仍无法识别的旧编辑范围报告 unknown。项目不可用、不同实例版本、来源冲突或未提交草稿不能绕过写入检查。

工作流/SDK 只读操作：

```json
{"apiVersion":"range.workflow/v1","requestId":"example","operation":"project.catalog.query","parameters":{}}
```

可选 `persistentDocumentId` 筛选目录。`project.snapshot.query` 必须提供该 ID，并在返回路径前验证当前来源版本与文件 SHA-256。目录元数据查询明确 `snapshotBytesVerified=false`，不能据此声称已核验字节。工作流 `project.query` 附带 catalog 和 modelBinding；模板配置仍显示 `usesOpenDocument=false`。

Python `WorkflowClient.project_catalog(id=None)` 和 `project_snapshot(id)` 使用相同操作。GUI/CLI 创建的新目录与工作流共用 projectId；旧 `workflow-state.json` 保留任务/审计，新目录采用其既有项目 ID，互不相符时拒绝隐式替换。

命令行：

```text
range-project --workspace <目录> --create
range-project --workspace <目录> --query
range-project --workspace <目录> --register <原生模型路径>
range-project --workspace <目录> --document <持久ID>
range-project --workspace <目录> --snapshot <原生模型路径> --document <持久ID>
```

成功输出纯 JSON；失败非零退出、诊断到 stderr。查询要求已有目录，不创建文件。CLI 快照要求干净已保存来源并取得实例所有权，不能将保存源的旧字节覆盖到另一实例的未保存接受版本。

## 严格解析与边界

原生/Python 共用字段约定：拒绝未知版本/字段、重复 JSON 键、非法 UUID/rN、坏源索引和来源链、缺失 origin、非有限值、非法实例引用及越界快照路径。目录最多 8 MiB、4096 文档、JSON 深度 32/节点 200000；标签最多 256 UTF-8 bytes，路径最多 4096 bytes，原生 LINK 深度 16，每文档快照历史最多 256 条。实际限制由能力描述和实现共同核对。

当前支持单个 GUI Session 的活跃项目；实例所有权和协作锁不防止非协作外部程序改写文件，使用前仍验证哈希。草稿字段描述能恢复但原始文本不作为本接口的持久数据；需要明确输入确认或取消。跨文件发布核对、未知操作结果和中断计算恢复仍按 WF-05 验收，不由 project.json 的存在推断完成。
