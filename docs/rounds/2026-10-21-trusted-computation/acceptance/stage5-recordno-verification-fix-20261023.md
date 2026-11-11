# Stage 5 verification record-number correction

Date: 2026-10-23

## Finding

The three-case GUI convergence run produced a valid static cantilever beam result at mesh level 1, but the experiment terminated with `VERIFICATION_REPORT_TOO_LARGE`. The native result broker uses record number `0` for a terminal static result (`rnative_result_broker.cpp`, static MODEL inspection and its broker regression). The convergence-report bound incorrectly required all result record numbers to be at least `1`.

## Change and evidence

- `rtrusted_experiment_service.cpp` now bounds static, modal, and transient report record numbers by their analysis contract: static `0`, modal `1..modes`, and transient `1..steps`. The result-report size bounds remain in force.
- The follow-up focused GUI regression passed: `build/agent-cloud-off/fea/stage5-spatial-recordno-fix.txt` — 3 passed, 0 failed, 0 skipped; spatial levels 1, 2, and 4 were verified and converged, and the source model fingerprint/revision stayed unchanged.
- The preceding three-case GUI run is preserved at `build/agent-cloud-off/fea/stage5-gui.txt`. Temporal convergence and budget-stop cases passed. Its spatial case failed before the fix with the static record-number mismatch described above.
- The new service boundary test `timeStepRangesContainPlanEndpointsAndAcceptPlannedRepair` compiled but did not yield a test result. Direct and CTest launches of `tst_trusted_experiment_service` exited with code 1 and no QtTest output; CTest's `LastTest.log` also contains an empty output section. After checking the configured Qt runtime and working directory, further attempts through the same runner were stopped. This test remains unverified; no pass is inferred.

## Static measurement report preflight

- A read-only review found that a valid static ProblemSpec could fit the 64 KiB input cap while its duplicated convergence measurement selectors and values exceeded the 64 KiB report cap. That would reject the report after the solve and leave the experiment in verification.
- `RTrustedConvergenceService::validateStudy()` now estimates the serialized static convergence measurement shape before authorization/solve, using each normalized selector/component and maximum finite JSON numbers for coordinates, displacement values, and strain energy. It returns `STUDY_MEASUREMENT_SIZE_LIMIT` when the existing cap would be exceeded. The experiment report validator uses the same published cap constant.
- Added `rejectsOversizedStaticMeasurementsBeforeSolve`, which builds a valid sub-48-KiB ProblemSpec with enough distinct position QOIs to exceed the static measurement report bound. Both convergence and experiment-service Release targets compile successfully.
- The focused `tst_trusted_convergence_service` CTest and direct QtTest entry both exited with code 1 and no output. CTest's `LastTest.log` has an empty output block. The CTest command, working directory, and Qt bin PATH were inspected; an incorrectly composed first direct path was corrected, after which the exact executable still exited 1 without output. The runner fault is recorded and the regression remains unverified; it is not counted as a pass.
- After the service change, the focused GUI spatial regression was rebuilt and rerun through its registered `--model-operations` dispatcher: `build/agent-cloud-off/fea/stage5-static-preflight-spatial-regression-20261023.txt`, 3 passed, 0 failed, 0 skipped; 1→2→4 beam levels completed and the source physics remained fixed.
- The separate `tst_trusted_experiment_service` boundary test `timeStepRangesContainPlanEndpointsAndAcceptPlannedRepair` remains unverified for the previously recorded identical exit-1/empty-output runner failure.

## Stage status

The static record-number correction closes the focused spatial regression, and the new static measurement-size gap is closed in code with a compiled preflight regression. Stage 5 remains unaccepted: the modal repeat/solver failure remains a recorded skip, and both newly added/changed service boundary tests lack executable results because their runner exits without output. No convergence tolerance or durable-record cap was loosened.
