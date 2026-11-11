# PC-00-A02/A03 独立审查记录

审查日期：2026-10-20（Asia/Singapore）
审查方式：独立只读代码与证据审查；未修改源代码、计划或验收证据，未执行测试。
审查来源：`acceptance_gap_map` 子代理对 attempt-03 构建收据、测试实现、序列化分支、CTest 日志和 JUnit 的复核。下方记录的是可攜审查摘要；原始结构化收据和 JUnit 按仓库目录规范保留在本机 `artifacts/pc00-attempt-03/`，完整 CTest 原始日志保留在本机 `audit/`，这些运行产物均不随仓库发布。

## 来源绑定

- attempt-03 收据（本机限定）：`artifacts/pc00-attempt-03/PC-00-A02-attempt-03-receipt.json`，SHA-256 `78b983c7865ed487fd5fcb46546b9818356f80fbf6eebbeb33e06fe99fa33667`。
- attempt-03 JUnit（本机限定）：`artifacts/pc00-attempt-03/PC-00-A02.junit.xml`，SHA-256 `2a2bca894c7c6697b30c0d9ea1d41b78eac101f232b2260f32521b8342acdcb2`。
- attempt-03 CTest 日志（本机限定，未发布）：`audit/pc00-registry-fixture-20261020-attempt-03/ctest.log`，SHA-256 `fd0bb41196c94d8303fe08532bc854ef64b41d3a5b6e6f32ea27293038eead6c`。
- 格式序列化实现 `src/range-model-lib/src/rml_file_io.cpp`，SHA-256 `8e1e0e443e8e2d6dc89caa1fcf480da5127a42e860d8c4691b931676bdb20215`。该文件未列入 attempt-03 的 `sourceSha256`，因此在此明确补充绑定；文件时间为 2026-10-12，当前 RelWithDebInfo `range-model-lib.lib` 于 2026-10-20 18:29 构建，早于 attempt-03 测试。
- 模型格式测试 `src/range-model-lib/tests/tst_structural_native_control_model.cpp` SHA-256 `03308ce838f1a369798ad8e4877f65870a34408dbbc8443887f1c30ac9f36e99`；原生控制运行测试 `src/fea-solver/tests/tst_native_control_run.cpp` SHA-256 `72a62315ed65a5196a879544af70b2ae13139b873f9e9dc6cddcbe41f73445a9`。两者亦已列于 attempt-03 收据。

## 审查结论

**PC-00-A02：通过。**构建和 CTest 均 exit 0，三项测试 3/3 通过。模型测试以格式 1.3.21 覆盖 PI spec/state 的 ASCII/BINARY 持久化；原生控制测试执行 PI 核心；真实 CLI 测试使用收据绑定的当前 `fea-solver.exe`，对 RAM 与 RBM 做 4 步中断、续算至 12 步，并与连续运行的完整物理及控制状态比较。进程观察记录确认测试期间确实启动该 RelWithDebInfo solver。覆盖范围为小型合成模型的资格测试，不代表大模型或产品整体验收。

**PC-00-A03：通过，范围受限。**序列化实现仅在版本大于 1.3.20 时读写 native control spec/state；1.3.20 读入默认状态，写出非默认新字段时拒绝静默降版。通过测试分别比较 ASCII/BINARY 的 1.3.20 输出与手工冻结旧外层字段清单生成的 golden bytes，并验证旧格式读回默认值；hidden-downgrade 用例确认拒写不会部分写出。真实 CLI 测试的 `publicationRollbackSameSolverRetry` 覆盖发布失败后的 accepted model/result/checkpoint 回滚，以及同一 solver 重试后与连续结果一致。独立审查未发现与通过测试冲突的序列化或回滚实现风险。

兼容证据的边界：1.3.20 golden bytes 来自测试内手工冻结的旧字段清单，而非某个已发布旧版本保存的外部模型文件；本记录据此确认当前字段布局兼容性，不声称已完成历史发行版文件的跨版本互操作资格。attempt-03 仅补齐 PC-00-A02/A03 的范围，不完成 PC-00 以外的阶段，也不改变其余验收要求。
