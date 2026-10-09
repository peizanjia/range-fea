# WF-12 LPAC 读回验证未闭合

2026-10-09，本机 Windows 10.0.26300、SDK 10.0.26100、v145。普通生成代码执行保持 OFF，不以 Job Object、启动属性或私有 Python 的 `-I` 替代隔离验证。

已完成源码、编译、独立审查和受控启动诊断。此前 `CreateProcessW` 返回 203；仅加入 API 派生的新 AppContainer 自有存储 `LOCALAPPDATA` 后，本机实际创建成功，仍使用 LPAC opt-out、零 capability、子进程限制、精确继承句柄和创建时 Job 绑定。Job 限制读回成功；这只证明本机启动越过原错误，不证明全部隔离能力。

随后在恢复 worker 前失败：`GetTokenInformation(TokenIsLessPrivilegedAppContainer)`，class 46，空缓冲尺寸查询返回 FALSE、size=0、Win32 87。最后工件为 `build/audit-20261009/WF-12/lpac-token-class/isolated-workers/a3e89d4d-ac88-42fb-9ece-b68f5576d1a1/job/`。worker 未 Resume，清理 manifest 已保存；需以对应 receipt 的 ACL/profile 清理结果核对，不能把前期回执误报当作遗留 profile 证据。

Microsoft 的 [Win32 枚举说明](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class)描述 LPAC，但没有给 class 46 的完整 Win32 输出、版本与错误合同。[GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation)给出通用查询合同。本机实际失败独立于文档；第三方或问答中的观察不能替代本产品能力验证。

本轮已经执行 Sol high 聚焦诊断和一次 Sol xhigh 只读核查，保留每次失败工件，未降低 token/Job/ACL/环境要求。当前处理为完整记录后跳过此硬项：后端不可验，普通模式不可执行，没有静默 AppContainer/未隔离回退。`--probe` 仍报告 available=false、requiresLaunchVerification=true。

待闭合：有支持依据的 LPAC 实际核验方法、私有 NumPy/SciPy 真正运行、专用假私有文件/凭据/越界写/网络/宿主注册表/子进程反例、资源/取消/崩溃恢复、外部干净稳定 Windows。无论后续采用何种核验方式，都必须与实际拒绝反例共同验证，不能删除失败查询便宣称通过。

已声明的保护边界继续保留：512 MiB 是 committed-memory 限额；没有线程数量硬限额；普通文件输出是监测与验后上限，`outputHardQuota=false`；AppContainer 自有 profile 存储和部分共有系统资源不是全盘禁止。原始安装私有 runtime 未被加 SID 权限，ACL 只修改任务副本。

恢复必须只处理严格验证、属于此次 UUID staging 的清理记录及唯一 profile，逐项核对原 ACL 恢复和 profile 删除结果。不要重用含已完成/未知 cleanup manifest 的任务目录；不要删除失败工件或自动重做未知接受动作。
