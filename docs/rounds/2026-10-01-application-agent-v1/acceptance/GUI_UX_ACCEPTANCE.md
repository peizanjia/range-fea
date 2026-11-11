# Agent 对话与动力学监视交互验收

日期：2026-10-04。起点为根提交 `382b2fe`、AI 子模块 `e15b7fe`。本次是在 Stage 10C–12 受控 v1 上进行的交互增量；上一轮的 105 项结果不作为新增代码的证据。

## 交付范围

- 独立右侧 Assistant 停靠面板；对话记录与底部输入区，模型、effort、当轮自动操作选项，问题和批准卡片。供应商、端点及凭据迁入应用设置。
- 完成后追问、运行中 Send now、Stop、空闲 New chat；新一轮读取当前状态，旧问题/提案/迟到响应失效，已提交计算继续运行。未观察到执行结果时明确保留未知状态，通过工作流任务列表核对。
- DeepSeek Off/Low/High/Max 参数适配；内部保留有界工具轮次协议元数据。凭据不进入持久设置或聊天。
- 独立底部动力学面板，将实时监视、实验配置、结果历史分开。实际 FE 接受步经 `telemetry.query` 增量更新传感器和施加力曲线，支持通道、时间窗、鼠标读值、冻结显示与 CSV。
- 遥测只读、限定工作区路径、校验原生日志身份/序列/接受状态；有限读取与缓存，损坏不会被跳过后恢复成成功。

## 验证记录

全目标 Release 重建成功；必跑清单 **107/107**；完整 CTest **107/107、0 失败、248.95 s**。原始工件位于 `build/audit-20261004/`。

| 项目 | 证据 |
| --- | --- |
| 构建与必跑清单 | `gui-ux-build-verified.log`、`gui-ux-inventory.log/json` |
| 完整回归 | `gui-ux-full-ctest.log`；远程调用和 Windows 截图为显式单独验收，不将默认跳过当作通过 |
| 对话与授权 | 离线真实控件的问题回答、拒绝、追问、凭据不持久化；runner 的旧响应/旧批准/同步信号重入与已提交任务保留测试通过 |
| 原生曲线 | GUI 实际梁 20 步主动/被动求解及 20 步续算；断言在任务仍为 running 时收到原生接受步，切换到续算任务后身份与样本数量匹配 |
| 遥测解析 | 独立套件 11 项，10 通过、1 因 Windows 符号链接权限跳过；覆盖增量/分页/部分尾行、接受标记、损坏持续失败、身份/数值校验和缓存回收 |
| 最后显示修正 | 截图后补充安全 Markdown 排版；再次全目标 Release 成功，相关 GUI/runner/事件/对话 CTest **4/4、25.02 s**，见 `gui-ux-final-build.log`、`gui-ux-final-tests.log`。前述 107 项是在此局部显示修正之前执行，未重复全套数值测试 |
| DeepSeek 与 Windows 窗口 | 最终 Low effort 实际修改与追问通过，模态迭代次数改为 53，后续提问得到 53 且没有额外修改；5 次模型响应、20182 tokens。同期真实梁 100+100 步与 Windows/OpenGL 截图通过，QtTest **5/5、19.954 s**（含初始化/清理），见 `gui-ux-native-remote.txt` |
| 安装版 | `build/stage12-install` 使用安装 bin + Windows 系统目录 PATH、安装 Qt 插件及私有 Python，GUI 实际梁 20+20 步、接受步曲线和 Windows/OpenGL 渲染 **4/4、11.838 s**；GUI 与遥测/服务文件哈希和已验证构建/源码一致。临时验收 exe/DLL 已移除，见 `gui-ux-installed.txt`、`gui-ux-installed-live.png` |

实际画面为 `gui-ux-live.png`，同时显示真实远程对话和已完成续算的原生传感器/施加力曲线。没有用设计稿代替程序渲染。视觉复查先后修正原始 Markdown 标记和列表格式延续到下一消息的问题；本轮三次真实联调合计 13 次模型响应、50406 tokens，均未记录凭据或原始 HTTP 错误体。

本轮第一次构建修正了新增测试中的括号错误。第一次聚焦回归暴露测试夹具对响应封装、配置任务身份及 Qt 信号断开的假设错误；修正后对话两项重跑通过，并通过上述完整回归。没有删除断言来跳过功能检查。

## 复跑入口

在仓库根目录按顺序运行 `cmake --build build/vs2026 --config Release --parallel 4` 和 `ctest --test-dir build/vs2026 -C Release --output-on-failure`。必跑清单使用 `scripts/ci/verify_required_ctest.py`，清单为 `scripts/ci/required-ctest-tests.json`。

显式远程/原生窗口验收使用 `build/vs2026/Release/tst_gui_model_lifecycle.exe` 的 `deepSeekAppliesLiveGuiCommand workflowPanelRunsAndResumesRealBeam visibleViewportRenders` 三个测试函数。配置当前 Qt 的 bin 到 PATH、plugins 到 `QT_PLUGIN_PATH`，`QT_QPA_PLATFORM=windows`；设 `RANGE_TEST_DEEPSEEK_CREDENTIALS_FILE` 为本地凭据文件路径，`RANGE_TEST_DEEPSEEK_EFFORT=low`、`RANGE_TEST_DEEPSEEK_FOLLOWUP=1`、`RANGE_TEST_CONTROL_STEPS=100`。`RANGE_TEST_CONTROL_RENDER_OUTPUT` 和 `RANGE_TEST_GUI_RENDER_OUTPUT` 指定绝对 PNG 路径。使用 QtTest `-o 输出文件,txt`，不要使用会打印对象信号参数的 `-vs`。远程调用会消耗模型用量。

## 已修复的审查问题

- 控制日志必须明确标记 `accepted`，不得将同名但未接受的事件画入曲线。
- 冻结显示时导出冻结的数据；打开保存对话框前固定任务、运行与通道身份，避免后台刷新改变导出来源。
- 追问切换任务后，信号回调必须再次核对任务代次，避免继续旧工具或旧自动批准。
- 设置端点不得携带凭据；检查发生在持久化之前。

## 保留边界与后续顺序

详见 `GUI_INTERACTION_DESIGN.md`。当前没有逐 token SSE、持久会话/分支、跨任务并行只读问答；Send now 会替换 Agent 当前任务，不终止已提交计算。实时图目前是受限杆/梁工作流的主动控制通道，不是任意打开模型、任意求解器或硬件实时接口。

下一增量依次为：流式回答及活动卡片；长时曲线/多任务对照与工作区布局；声明式扩展界面；accepted-step 调参事务。各项分别验证可访问性、高 DPI、长任务、断网恢复及干净机器安装，不能以本次截图宣称全面达到 Codex 或 Simulink 的产品成熟度。
