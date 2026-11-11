# 持续CI与计划门禁

来源：[全工作流WF-01至WF-14](../../docs/rounds/2026-10-08-workflow-delivery/README.md)。这里保留同目标主工具、辅助实现和测试，不是一次性运行产物。

- [required-ctest-tests.json](required-ctest-tests.json)
- [test_verify_required_ctest.py](test_verify_required_ctest.py)
- [test_workflow_plan.py](test_workflow_plan.py)
- [verify_control_workbench_plan.py](verify_control_workbench_plan.py)
- [verify_piezo_control_plan.py](verify_piezo_control_plan.py)
- [verify_required_ctest.py](verify_required_ctest.py)
- [verify_workflow_plan.py](verify_workflow_plan.py)

执行前读参数/前置条件并将profile/temp/cache/output指向仓内；按平台分别验收，不在目录整理中启动产品或远程调用。
