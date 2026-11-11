# COMSOL 全流程 Agent 调研交接

日期：2026-11-05。本页记录当时的调研交接范围：仅调研和报告，不继续产品修改、重配、编译、算例或 Provider 调用。它是历史范围说明，不表示仍有并行进程，也不授权新的运行。

## 先读

1. [COMSOL 全流程 Agent 主报告](COMSOL_FULL_WORKFLOW_AGENT_REPORT.md)：25 项官方来源，区分 Chatbot、现有 Java API Agent 实作和 2027 MCP；涵盖共享模型、上下文、选择集、求解、结果、验证、并发、部署及开发蓝图。
2. [浏览版 HTML](COMSOL_FULL_WORKFLOW_AGENT_REPORT.html)：同一内容，带目录与文献链接。
3. [机器可读交接](developer_handoff.json)：注意 `execution_mode` 与最新限制；开发任务都是未来建议。

## 证据和附录

- `fullflow_sources.jsonl`、`fullflow_evidence.jsonl`、`fullflow_claims.jsonl`：主报告的稳定来源身份、释义证据和判断映射；引用编号只适用于主报告。
- `comsol_fullflow_addendum.json` / `.md`：版本状态与官方/第三方区别。
- `research_agent.json` / `.md`：6.4 手册、补丁、Chatbot 和 API 的逐项证据。
- `range_agent_gap.json` / `.md`：13 项当前静态差距；不是当前运行通过证据。
- `report.md`：Range 综合附录，含旧计划和建议验收；以主报告的完整 API Agent 路线为准。
- `piezo_validation.json` / `.md`：在用户收窄范围前的有限构建/运行及中断记录；含此前 12 个源码/脚本改动清单。改动未自动回退；完整 PZT 未取得全例 PASS。

## 已确认的主线

COMSOL 全流程 Agent 并非只靠内置 Chatbot：已有 Java API + 外部 Agent + 共享服务器模型的官方实作；官方 MCP 是新增的标准化接入路线。API 全流程覆盖、特定任务示范、普适可靠性验收必须分开。

未取得官方 MCP 完整 schema、鉴权与恢复合同；登录保护的 webinar 录像未观看。本轮未在本机运行 COMSOL，未取得独立对照性能结论。
