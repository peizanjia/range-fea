# 脚本入口：按目标整组维护

目录不再混放PC11、WF长图、隔离和打包工具。所有新运行输出放仓内artifacts，编译用build。[目标历史](../docs/history/WORK_HISTORY.md)说明这些工具为何存在；本轮不替代各工具平台资格。

| 职能／目标 | 实体目录 | 来源轮次 |
|---|---|---|
| 持续CI/台账门禁 | [scripts/ci/](ci) | [全工作流WF-01至WF-14](../docs/rounds/2026-10-08-workflow-delivery/README.md) |
| PC11安装GUI资格 | [scripts/benchmark/pc11-gui/](benchmark/pc11-gui) | [完整压电PC交付与20k产品链](../docs/rounds/2026-10-12-piezo-delivery/README.md) |
| WF10长序列 | [scripts/benchmark/wf10-series/](benchmark/wf10-series) | [全工作流WF-01至WF-14](../docs/rounds/2026-10-08-workflow-delivery/README.md) |
| WF12平台隔离 | [scripts/benchmark/wf12-isolation/](benchmark/wf12-isolation) | [全工作流WF-01至WF-14](../docs/rounds/2026-10-08-workflow-delivery/README.md) |
| 私有Python构建 | [scripts/packaging/python-runtime/](packaging/python-runtime) | [共用应用服务、GUI/Agent与受控扩展v1](../docs/rounds/2026-10-01-application-agent-v1/README.md) |
| PC安装实物清点 | [scripts/packaging/piezo-off-inventory/](packaging/piezo-off-inventory) | [完整压电PC交付与20k产品链](../docs/rounds/2026-10-12-piezo-delivery/README.md) |
| OFF链接图 | [scripts/packaging/piezo-off-linkmap-v44/](packaging/piezo-off-linkmap-v44) | [完整压电PC交付与20k产品链](../docs/rounds/2026-10-12-piezo-delivery/README.md) |
| v43静态候选链 | [scripts/packaging/piezo_gui_static_v43/](packaging/piezo_gui_static_v43) | [完整压电PC交付与20k产品链](../docs/rounds/2026-10-12-piezo-delivery/README.md) |

## 保留与删除

ci内是持续工具；benchmark各组含主入口、同目录辅助和回归，整体保留。v43已是独立目标包，保持内部相对层级；v43/v44未闭合验收不是废案。现有ci/benchmark/packaging根被source scope纳入，不仅为分类新增另一根。

11月5日固定调查runner已明确退役，底层PZT/delivery/bridge测试和数值环境不删。[删除理由/原SHA/替代入口](../artifacts/repository-rounds-organization-20261111/retired-scripts.json)；历史日志中的旧命令保留原样，不能当新入口执行。没有调用Provider或长跑作为文件整理。
## Windows CI 兼容入口

`scripts/packaging/build_private_python.py` 保留现有 Windows 工作流调用的路径，实际打包逻辑由 `packaging/python-runtime/` 维护。
