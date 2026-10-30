# Range FEA phase checkpoint — 2026-10-30

This checkpoint records what is being published for the current phase. It separates source/CI verification from course-product acceptance. A build or passing CTest suite does not close the live Agent or installed-GUI gates.

## Repository and change scope

- Root repository: `peizanjia/range-fea`, branch `feature/structural-profile`. The source freeze commit for this checkpoint is `ad9dbda685fa1a6dec08b9aecd6ec48ef0ac2a1d`; this document follow-up records its exact cross-device handoff identity.
- The phase includes concurrent changes in six submodules. Publish each submodule commit first, then publish the root commit with the updated gitlinks.
- The root/submodule changes cover trusted mechanics and verification services, agent evidence capture, bounded tool/provider diagnostics, native axial/layered/solid piezo solver paths and persistence, external excitation/control integration, GUI and cloud lifecycle handling, and the corresponding tests and CI registration.
- `origin` is the personal `peizanjia` repository. `upstream` is `Range-Software`; this phase targets only `origin` and does not rewrite history or publish to upstream.
- Remote refs were refreshed on 2026-10-30. Before this closeout, local `feature/structural-profile` and `origin/feature/structural-profile` both pointed to `9ea1d515`. `origin/main` was `69948543`; `upstream/main` was `41d6c339`. The feature branch contains fetched `origin/main` history and is 143 commits ahead with none behind. It has diverged from `upstream/main` (143 commits only on the personal feature branch and 73 only on upstream main); do not merge upstream implicitly when resuming on another device.

Submodule commits published for this checkpoint:

| Submodule | Branch | Commit |
| --- | --- | --- |
| `range-ai-lib` | `codex/wf11-tool-diagnostics` | `66c79da` |
| `range-base-lib` | `codex/wf10-quiet-logs` | `25cca75` |
| `range-cloud-lib` | `codex/source-freeze-20261030` | `d34f2f5` |
| `range-gui-lib` | `codex/source-freeze-20261030` | `047c060` |
| `range-model-lib` | `feature/structural-profile` | `6a732d9` |
| `range-solver-lib` | `feature/structural-profile` | `880c5d1` |

All six published submodule commits were fetched from their personal `origin` remotes and match the root gitlinks. `range-build-tools` remains unchanged at `ec4cdb2` on the checked-out detached commit, which is present on its `origin/main`.

## Why the GitHub email was relevant

The notification concerned Windows Regression run [40938750163](https://github.com/peizanjia/range-fea/actions/runs/40938750163) for commit `9ea1d51545c52a0adc48c62bc1a65b68775c9ff3`. It failed in the Qt installation step before configure, compilation, or tests. The failing outer composite action checked for a relative bundled JavaScript file from the wrong action root. The workflow now invokes the upstream `action/` subdirectory directly at pinned `v4.5.0` commit `b344308e4483fc892e0d27b3cd1b173ebdd95f06`; the follow-up GitHub run must confirm that this passes in the hosted Windows environment.

## Verification

- CI workflow metadata unit tests: 14 passed.
- Required CTest manifest: 246 tests across 9 groups; the configured CTest inventory registered exactly 246.
- The Piezo plan byte verifier returned `valid=true`; it confirms the frozen plan structure and the referenced portable summary bytes only. The current plan records PC-00 done and PC-01–PC-11 planned, with `productComplete=false`. The older 33/36 prose is a prior snapshot and is not the current machine-plan status. The PC-00 review summary preserves raw receipt/JUnit hashes, but those run files remain local under ignored `artifacts/pc00-attempt-03/`; the full CTest log remains local under ignored `audit/`.
- A clean Windows Release configure and full build completed successfully with Qt 6.11.1 and the already installed OpenSSL 3.0.16 development files. An incremental Release build after the final source/test edits also completed with exit code 0. OpenSSL was found at `D:/ProgramData/anaconda3/Library`; it was not copied or installed by this task.
- Full CTest completed with exit code 8: 239/246 passed and 7 failed. `tst_piezo_delivery_examples` passed after 20,789.56 seconds (about 5 h 46 min); total CTest time was 21,108.03 seconds. Local logs are under ignored `artifacts/closeout-20261030/logs/` and are not committed. This suite started before the final GUI/Agent source fixes, so its failures are baseline evidence and must be separated from the final-source targeted reruns.
- Final-source targeted evidence is partial. `tst_agent_task_runner`, `tst_extension_widget`, and `tst_piezo_external_integration` passed; the saved-video test now skips when its optional fixture is absent. The focused `dockLayoutsShrinkWithoutLosingControls` case passed after changing its smallest test window from an impossible 900×450 to 900×480. The full lifecycle target was not rerun after that adjustment.
- Two `tst_gui_model_operations` cases still fail in fresh isolated profiles. `trustedExperimentFailurePersistsJobBoundDiagnosticPackage` reports Win32 error 3 while opening the hash-bound input snapshot during failure capture; the preserved runtime contains that file after the test exits, so the brief disappearance or path race remains unexplained. `verifiedNativeResultOpensAsIndependentReusableDocument` reproduces a Windows `.job-record.lock` release warning; durable state stays `queued`, and result query returns `JOB_REGISTRY_CONFLICT`. In the current code `updateNativeJob()` still changes its in-memory record after durable persistence fails, so this requires a source fix before native terminal-record behavior is accepted.
- `tst_workflow_scientific_extensions` and `tst_workflow_scientific_integration` remain unverified because the existing private Python runtime contains unregistered `.pyc` files. The local cache was not deleted; no fresh private runtime was built for a rerun. Thus the seven-test targeted CTest invocation was not fully green, and the two model-operation failures remain open engineering defects rather than waived course-plan items.
- `verify_workflow_plan.py --next --json` reports 10 workflow items done, WF-10–WF-12 blocked, and WF-13 still planned. The Control Workbench verifier returned `valid=true` with 3 done, 5 in progress, and 8 planned; its `productComplete` remains false. These separate product plans are retained as active work, not folded into the course stages or declared obsolete.
- Local checks were run with profiles and temporary paths redirected into `artifacts/closeout-20261030/` in accordance with the repository artifact policy. This is Windows evidence; no Linux build result is claimed.

## Trusted Computation Agent acceptance boundary

| Stage | Phase status |
| --- | --- |
| 0–3 | Accepted only for the captured current-source Windows/Release evidence and the documented rod/beam scope. |
| 4 | Accepted for the bounded local repair service and the positive authorized rod repair. The intermittent historical solver exit 1 and broader repair reliability remain unexplained/unverified. |
| 5 | Accepted for selected displacement, static strain-energy comparison, and first modal-frequency studies with retained levels, applicable reference errors, costs, and curves. Stress/reaction convergence and an analytic true-error claim for the four-element transient case are unsupported. |
| 6 | Not accepted. Case B attempt 006's real DeepSeek Pro inspect/diagnose request returned `Request failed`; no result/report acceptance follows from that failure. Case C attempt 007 was reopened and accepted on the targeted patched INSTALL-07 candidate with matching request/job/artifact/manifest/source/authorization identities and all eight report metrics. That candidate-specific receipt does not qualify a complete current-source installation. |
| 7 | Not executed for this course submission because no prepared solver-result case set exists. The three-arm/180-slot comparison and independent holdout remain useful planned research work; they are unperformed, not accepted, and not discarded as obsolete. |
| 8 | Not accepted. Whole-current-source installed GUI model-to-report flow, final source/dependency/contribution freeze, and delivery review remain open. Clean Windows-account first use was waived and remains unperformed. |

The initial source/license inventory is recorded in [source provenance and license inventory](source-provenance-license-inventory-20261030.md). It lists license files found at repository/component roots, but it is not a complete transitive dependency review and does not establish individual authorship. The user's personal-contribution confirmation and a release-package notice review remain open; commit metadata alone is not proof of authorship.

## Local-only versus portable files

Portable requirements, designs, plans, and status summaries belong under `docs/`. Build output belongs under `build/`. Test logs, application profiles, temporary files, screenshots, and raw attempt data belong under ignored `artifacts/<run-id>/`. The local `.planning/`, `audit/`, `newaudit/`, and `Testing/` trees are not part of this source handoff. Do not cite a file in those trees as if it were present in a fresh clone.

## Required follow-up after publication

1. Hosted Windows Regression run 41016043003 completed: build and inventory passed, CTest finished 239/246 with seven failures. See the [final migration handoff](deletion-handoff-20261030.md) for the exact hosted failure list and verified recursive-clone evidence; resolve those failures in the next phase.
2. Build and test the fresh clone on Linux. This checkpoint contains no Linux verification.
3. Close the Stage 6 live-provider identity/report and Stage 8 installed-UI/source-provenance gates before calling the course project complete.
4. Complete the deferred source-contribution and license/use inventory before a final delivery freeze.
