# Portable handoff before deleting the old checkout

Date: 2026-10-30. Repository: `https://github.com/peizanjia/range-fea.git`, branch `feature/structural-profile`.

Implementation checkpoint: `ad9dbda685fa1a6dec08b9aecd6ec48ef0ac2a1d`. Windows JSON-output repair: `446b62a474eb9d998bf420f8df3c2b00942a9593`. Later documentation-only handoff commits do not change the tested solver sources. Use the branch tip to include these records.

## Continue on the next device

```sh
git clone --branch feature/structural-profile --recurse-submodules https://github.com/peizanjia/range-fea.git
cd range-fea
git submodule update --init --recursive
git status --short --branch
git submodule status
```

Relative submodule URLs resolve to the personal `peizanjia` repositories. Ordinary recursive update uses the pinned gitlinks; do not use `git submodule update --remote` to restore this checkpoint, because `.gitmodules` branch hints name `main` rather than the feature commits.

An actual HTTPS recursive clone at source head `446b62a` completed successfully inside `artifacts/handoff-clone-20261030/checkout`. All seven submodules checked out their pinned commits and both the clone and its submodules were clean. This verifies source retrieval, not a fresh Linux build.

The final local-branch audit also found `codex/control-materials-cw04` at `681c8cfe102713c83013af41e78e6cc2704969dc`. It is now published to personal origin. `git cherry` marks its material/device catalog patch equivalent to a patch already in `feature/structural-profile`; retain the branch as historical provenance, without merging it again. Its linked local worktree was clean. The other local branch tips were already reachable from remote history.

Read [continuation](../CONTINUATION.md), [phase status](phase-checkpoint-20261030.md), and the relevant workstream plan. Linux build/validation remains open. Put fresh compiler output in `build/` and profiles, logs, screenshots and temporary data in `artifacts/<run-id>/`. Optional local `.agents/` skills are not build dependencies and do not travel in this source handoff.

## Completed hosted regression for the source checkpoint

[Windows Regression 41016043003](https://github.com/peizanjia/range-fea/actions/runs/41016043003), head `446b62a474eb9d998bf420f8df3c2b00942a9593`, completed with **failure**. Checkout/submodules, Qt installation, metadata verifiers, workflow-plan verification, frozen scientific runtime assembly, configure, build, required-test inventory, and regression-log upload succeeded. CTest completed with **239/246 passed**, seven failed, 12,659.93 seconds. Failed targets:

- `tst_agent_task_runner`
- `tst_agent_conversation_history_widget`
- `tst_runtime_control_widget` (timeout)
- `tst_project_session`
- `tst_workflow_scientific_runtime`
- `tst_workflow_scientific_input_inspection`
- `tst_workflow_telemetry`

These are unresolved follow-up items, not waived or accepted tests. They differ from the seven failures in the earlier local run. Retrieve the uploaded logs from that run for root-cause work; hosted artifact retention is finite. The cp1252 JSON-output issue is resolved: the hosted workflow-plan step passed. No green full-suite or Linux result is claimed.

## Computation provenance and retained local verification summary

Range already provided modal eigenanalysis. The fork modifies `range-solver-lib/src/reigenvaluesolver.cpp`, including generalized-residual and exhausted-iteration acceptance checks. The standard stress-modal route selects inverse power iteration for a single mode and subspace iteration with Rayleigh-Ritz for multiple modes, then converts lambda to Hz by `sqrt(lambda)/(2*pi)`. The existing Lanczos implementation is not selected by that standard route.

`RStructuralDynamicsIntegrator` and its stress-solver route were added in the fork. Fixed-operator linear dynamics uses Newmark average acceleration (beta=1/4, gamma=1/2), generalized-alpha, or central difference with a stability guard. Specialized nonlinear truss/cable dynamics uses Newmark with nonlinear equilibrium iterations and native linear solvers. These are implementations/integrations of established algorithms, not claims of new integration methods.

The 2026-10-30 **local** CTest log records passes for `tst_eigen_value_solver`, `tst_modal_damping`, `tst_structural_thermal_prestress_modal`, `tst_structural_dynamics_integrator`, `tst_structural_dynamics_fe`, the beam/load/support/restart/failure-atomicity/rotational-initial/parallel-repro dynamics targets, and the truss/cable dynamics targets. References include analytic eigenpairs/oscillators, generalized residuals, step refinement, beam-column frequency refinement, energy/work, and restart checks. That local suite also had 239/246 passed with a different failure set; see the phase checkpoint. These results apply to covered models and assumptions, not arbitrary geometries or a complete installed-product qualification.

## What survives deletion

Tracked source, tests, build scripts, requirements/designs, plan ledgers, acceptance summaries, and maintained interaction/decision logs are published in Git. Credentials, local provider sessions, old Host previews, user profiles, build outputs, installed candidates, screenshots and raw local trial files are not transferred by cloning.

In particular `build/`, `artifacts/`, `.planning/`, `audit/`, `newaudit/`, `Testing/`, `.vs/`, and optional local agent directories are not the portable source handoff. Deleting the old checkout removes these local-only files, including raw historical evidence. Their maintained conclusions and remaining limitations are retained in `docs/`; reproducing a run on the next device requires a new build and fresh credentials/session configuration. No API key is committed.

Useful incomplete work remains active: Stage 6/8 acceptance, the deferred three-arm comparison, Piezo, Workflow and Control Workbench backlog items. Final contribution/use/license review remains deferred to final delivery. The source can be resumed while these product gates remain open.
