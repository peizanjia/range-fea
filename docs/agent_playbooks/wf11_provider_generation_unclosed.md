# WF11 真实提供商科学代码生成未闭合记录

2026-10-10。用户允许提高 effort 后仍反复无法闭合的问题完整记录后跳过；本项保留未通过，不改接口、数值、审批或超时门槛。

受信科学核心及实际原生来源 SDK/Windows GUI 故事已经通过；它们使用独立撰写并显式 staging 的候选，不能冒称本项真实 DeepSeek 自主生成完成。实际提供商用既有本地 `.env.local`（不输出/改写凭据），要求阅读 live capabilities/v2 三个 schema/knowledge，撰写自己的源码和小 N 解析案例，经 typed 输入检查取得 SHA，再人工 staging；后续独立宿主四案例、激活和实际原生20帧执行仍必须通过。

## 已定位并修复的产品/测试问题

- Windows 斜杠字符串不是执行文件身份：比较绝对 canonical 路径，保留真实文件身份约束。
- GUI 共享后台 verifier 的 ACK 可为 queued/running，最终 ready/current/full-file SHA 强校验不变。
- 新 extensionId 不存在时定向 list 正确拒绝；测试改 listall+精确ID筛选，不伪造注册。
- 输入 SHA 是宿主元数据工作，不要求语言模型口算。`scientific_input_inspect` 只绑定2..64×1..4的 caller 测试输入和SHA/contractHash，不提供数值答案或 native 证明、不改状态。原 candidate/output 必需 SHA 全部门槛不变。
- 真实脱敏诊断证明默认工具输出被硬编码2048 tokens截断：`finish_reason=length,prompt_tokens=28846,completion_tokens=2048,arguments_bytes=6021,reason=OUTPUT_TOKEN_LIMIT`，该次16.494 s、3个有效usage收据、0 staging/approval。GUI现在尊重配置的输出预算；本项使用8192，higher effort试次使用16384。仅调整请求预算，不增加解析/候选大小上限。
- 严格解析器保留重複键/非有限数/深度/字节数/原始工具对应检查；length更早失败关闭，并给固定 SANITIZED_LOCAL 元数据。新 Qt parser 测试及独立审查通过，本地子模块提交 a28bfec。
- 一次8192试次在 proposal 拒绝后 ask_user 无人应答，Qt300 s截止失败；后续加入即时 phase/code 日志和最多一次重申原固定要求的测试答复，不提供数值答案、不授予额外批准，不延长原截止。另一次7.411 s明确拒绝不符合 schema 的 extension_list 参数，提示因此限定所需六工具。

## 最后高 effort 结果

显式 `RANGE_TEST_DEEPSEEK_EFFORT=high`、`RANGE_TEST_DEEPSEEK_MAX_TOKENS=16384`，仍使用原30 s单请求设置和严格接口。最后试次71.714 s，2个有效usage收据，capabilities/三schema/knowledge读取完成，typed inspection未到达，stage/approval均为0；transport出现 QIODevice device not open，最终 failed。准确 transport失败代码未被该次诊断白名单记录，不能把推测的网络超时当已证实原因。

保留 `build/audit-20261009/WF-11/provider-high-final-unclosed.txt` 和 `.console.log`。早期多次试次使用同名实际输出，旧原始文件被覆盖；上述早期时长/错误/计数依据本次会话的实际工具返回摘要，**不是完整原始日志**，此证据缺口明确保留。后续试次须使用独立名称，不能继续覆盖。

## 下一步与交付边界

不再盲目重复该远程生成试次。后续应在单独有界问题中保存完整但不含凭据的阶段元数据，区分提供商传输、工具 schema 使用和候选错误，并通过真实模型重新生成修复；不能手工修补候选再冒称模型完成。无需修改已通过的核心数值/科学合同；普通生成执行仍关闭，WF12隔离亦未通过。

本条保持未通过。安装私有包故事可以继续验收受信候选，但不代替真实提供商科学生成，也不删除父级验收条款来制造 done。
