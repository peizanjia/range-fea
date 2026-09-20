# 开发执行与交互记录

本文件记录项目阶段执行中的关键决策、用户反馈、异常、纠正和验证结果。记录按时间追加；详细命令和代码差异以 Git 提交为准。

## 2026-09-20 Stage 0 交付闭环

- 用户目标：执行并纠错完成 Stage 0、1、2；Stage 3 网格生成优先采用成熟工具；保留原有非结构功能，但不把它们作为本轮开发任务。
- 打包问题：Release 安装树最初未携带 `libomp140.x86_64.dll`。CMake 现会定位 MSVC LLVM OpenMP runtime，缺失时配置失败，并将其安装到 `bin`。
- 测试事故：首次直接运行 Release CTest 时，测试执行文件旁缺少 Qt runtime，Windows 连续显示“找不到 Qt6Core.dll”。用户报告后立即终止 CTest；随后将 `windeployqt` 输出和 `Qt6Test.dll` 部署到测试目录，并为 CTest 增加逐测试超时。
- 纠正结果：`tst_structural_product_profile` 单测通过；Release 全量 CTest 为 28/28 通过，总耗时约 9.74 秒，无错误弹窗。
- 安装树验证：`Qt6Core.dll`、MSVC runtime 和 `libomp140.x86_64.dll` 均存在；从独立安装目录执行 `fea-solver --version` 成功，版本为 1.3.0。
- 交付物：生成 `range-fea-1.3.0-winnt-amd64.zip` 和 Inno Setup `range-fea-1.3.0-winnt-amd64.exe`。
- 环境补充：通过 winget 安装当前用户范围的 Inno Setup 6.7.3，并让 CMake 同时发现系统级与当前用户级安装位置。
- 安装器验证：正式安装器静默安装、CLI 启动和静默卸载均成功。
- GUI 验证：首次启动出现 `Crash report - Range FEA`，确认原因是此前强制终止 GUI 留下的 `range-fea.lck`，锁内 PID 已不存在。该锁已移入忽略的构建验证目录备份。随后从独立 install-tree 启动，主窗口标题为 `Range FEA :: Finite Element Analysis - [Beam] - Range FEA`，界面响应正常，并通过窗口关闭消息正常退出，退出码为 0，锁文件被正常清除。
- 未伪造结论：当前主机没有可直接使用的 Windows Sandbox，干净 Windows 虚拟机终验仍是发布前检查项；本轮以完整 runtime 清单和独立安装树启动作为可执行替代验证。
