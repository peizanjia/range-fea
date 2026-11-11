# Windows私有Python运行时构建

来源：[共用应用服务、GUI/Agent与受控扩展v1](../../../docs/rounds/2026-10-01-application-agent-v1/README.md)。这里保留同目标主工具、辅助实现和测试，不是一次性运行产物。

- [build_private_python.py](build_private_python.py)
- [runtime-win-amd64.lock.json](runtime-win-amd64.lock.json)
- [test_private_python.py](test_private_python.py)

执行前读参数/前置条件并将profile/temp/cache/output指向仓内；按平台分别验收，不在目录整理中启动产品或远程调用。
