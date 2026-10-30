# Documentation index

This index separates current workstream entry points from historical design notes and run evidence. A dated document is evidence for its recorded source and build identity; it is not a current acceptance statement by itself.

**Portable status authority:** use [the 2026-10-30 phase checkpoint](acceptance/phase-checkpoint-20261030.md) for the latest repository and course-stage status. `.planning/`, `audit/`, `newaudit/`, `Testing/`, and `artifacts/` are local working records and are intentionally excluded from a normal source handoff; do not rely on links into them after cloning.

## Start here

1. [Continuation index](CONTINUATION.md) for repository ownership, active workstreams, and current acceptance boundaries.
2. [Repository handoff](../HANDOFF.md) for shared-worktree and submodule safeguards.
3. [Development plan](DEVELOPMENT_PLAN.md) for the product roadmap.
4. [Phase checkpoint](acceptance/phase-checkpoint-20261030.md) for the current source, verification, and open acceptance gates.

## Document roles and status authority

| Document type | Use | Status rule |
| --- | --- | --- |
| Scope and requirements | Workstream plans and contracts | Define what must be delivered; a planned capability is not an accepted one. |
| Current acceptance | [Portable phase checkpoint](acceptance/phase-checkpoint-20261030.md), plus the plans and summaries included in this checkout | Use only evidence present in the repository when reviewing a fresh clone. Raw `.planning/` receipts remain local to the originating machine and are not remote acceptance evidence. Record conflicts rather than choosing a favorable count. |
| Continuation | [Continuation guide](CONTINUATION.md) and the phase checkpoint | Explain where to resume and which gates remain; old authorization is not approval for a new Host preview. |
| Interaction and engineering history | [Decision log](CONTINUATION_LOG.md), [development log](../DEVELOPMENT_LOG.md), dated handoffs | Preserve what was decided or observed at that time. These are summaries, not complete chat transcripts or current acceptance certificates. |
| Course delivery drafts | [Trusted Computation Agent plan](TRUSTED_COMPUTATION_AGENT_PLAN.md) | Preparation material only until trial, installed full-flow, attribution and final-freeze gates close. |

Keep the existing filenames and evidence paths stable. Update current summaries from receipts; do not rewrite historical test results to make summaries agree.

## Maintenance and archive classification

Treat an unfinished item as active when its owning plan still marks it planned, in progress, blocked, or pending and its scope remains useful. Do not call it obsolete because it is old or incomplete. Mark an item superseded only when a newer tracked plan or decision explicitly replaces it; retain the old record as history.

| Class | Files and rule |
| --- | --- |
| Active plans and contracts | `TRUSTED_COMPUTATION_AGENT_PLAN.md`, the Piezo/Workflow/Control Workbench plan documents and JSON ledgers, and their contracts. Their status fields and the phase checkpoint govern completion. |
| Maintained chronological records | `DEVELOPMENT_LOG.md`, `CONTINUATION_LOG.md`, `PIEZO_CONTROL_INTERACTION_LOG.md`, and `HANDOFF.md`. Append dated decisions and results; keep earlier entries intact and label them as snapshots. |
| Acceptance snapshots | Dated files under `acceptance/` and `evidence/`. Retain them to show what was measured and which source/build they bind; they do not become current merely because they are tracked. |
| Design and reference material | Architecture, interface, technical-route, and playbook documents. Keep them when they explain implemented contracts or a still-useful planned capability; link them from the relevant workstream plan. |
| Local execution data | Build trees, runtime profiles, raw logs, generated receipts, and temporary models stay under ignored `build/` or `artifacts/<run-id>/`. They are not required for a fresh source checkout unless a tracked acceptance record explicitly says otherwise. |

The 2026-10-30 phase checkpoint is the handoff authority for the current root/submodule source snapshot and its remaining gates. Workstream plans remain in force after this checkpoint; this is a source-and-documentation milestone, not a declaration that every planned capability is finished.

## Current workstreams

| Workstream | Entry point | Evidence and status authority |
| --- | --- | --- |
| Trusted Computation Agent | [Stages 0–8 plan](TRUSTED_COMPUTATION_AGENT_PLAN.md) | Use the portable [phase checkpoint](acceptance/phase-checkpoint-20261030.md) for current status. Historical raw attempts under local `.planning/` are not included in a fresh clone. |
| Piezo control | [Delivery plan](PIEZO_CONTROL_DELIVERY.md), [machine-readable plan](piezo_control_plan.json) | The verifier records PC-00 done and PC-01–PC-11 planned (`productComplete=false`). The older 33/36 handoff count is a historical snapshot from a different acceptance baseline, not the current plan status. |
| Workflow delivery | [Workflow plan](WORKFLOW_DELIVERY_PLAN.md), [machine-readable plan](workflow_delivery_plan.json) | The verifier currently reports 10 done, WF-10–WF-12 blocked, and WF-13 planned; see the [workflow contract](WORKFLOW_CONTRACT.md) and task-specific `WF-*.md` checkpoints. |
| Control Workbench | [Workbench delivery](CONTROL_WORKBENCH_DELIVERY.md), [machine-readable plan](control_workbench_plan.json) | The verifier currently reports 3 done, 5 in progress, and 8 planned (`productComplete=false`). This is a separate product workstream from the course stages. |

## Numbering systems and current trusted-agent status

The repository has several independent numbering systems. Do not combine them into a single Stage 0–12 plan.

| Numbering | Scope | Current authority |
| --- | --- | --- |
| Trusted Computation Agent Stages 0–8 | The course project in this thread: rod/beam analysis, diagnosis, bounded repair, convergence, Agent orchestration, comparison, and course delivery. “Execute stages 0, 1, 2” was the request to start the first three stages; it did not add Stages 9–12. | [Plan](TRUSTED_COMPUTATION_AGENT_PLAN.md) and [current phase checkpoint](acceptance/phase-checkpoint-20261030.md). |
| Legacy product Stages 9–12 | Older Range product roadmap and acceptance documents; a separate numbering scheme. | [Development plan](DEVELOPMENT_PLAN.md), marked historical where applicable. |
| Piezo PC-00–PC-11 | Twelve piezo-control delivery increments, not Trusted Agent stages. | [Piezo plan](PIEZO_CONTROL_DELIVERY.md) and [machine-readable status](piezo_control_plan.json). |
| Workflow WF-01–WF-14 | Workflow subsystem delivery increments. | [Workflow plan](WORKFLOW_DELIVERY_PLAN.md) and [machine-readable status](workflow_delivery_plan.json). |
| Control Workbench CW-01–CW-16 | Earlier structural-control workbench delivery increments. | [Workbench delivery](CONTROL_WORKBENCH_DELIVERY.md) and [machine-readable status](control_workbench_plan.json); not the current course-stage ledger. |

The detailed stage status is maintained in the dated [phase checkpoint](acceptance/phase-checkpoint-20261030.md). Older acceptance and attempt records remain local-only; they are not included in a clean clone and do not override that portable summary.

## Repository structure for continuation

| Location | Use |
| --- | --- |
| Root `README.md`, `HANDOFF.md`, `DEVELOPMENT_LOG.md`, `PROJECT_ROADMAP.md` | Project orientation, shared-worktree safeguards, chronological engineering log, and product roadmap. |
| Earlier root-level design and technical routes | [Agent interface](../AGENT_INTERFACE_DESIGN.md), [structural baseline](../STRUCTURAL_BASELINE.md), route [0–3](../TECHNICAL_ROUTE_STAGE0_3.md), [4](../TECHNICAL_ROUTE_STAGE4.md), [5](../TECHNICAL_ROUTE_STAGE5.md), [6](../TECHNICAL_ROUTE_STAGE6.md), [7](../TECHNICAL_ROUTE_STAGE7.md), [8](../TECHNICAL_ROUTE_STAGE8.md), [9](../TECHNICAL_ROUTE_STAGE9.md). These are historical product-stage references, not the Trusted Computation Agent course plan. |
| `docs/` | Stable cross-workstream plans, interfaces, contracts, and this navigation index. Keep linked filenames stable. |
| `docs/acceptance/` and `docs/evidence/` | Portable, curated summaries and evidence contracts. Keep generated logs and binaries under ignored `artifacts/<run-id>/`. |
| `.planning/`, `audit/`, `newaudit/`, `Testing/` | Local-only plans, raw attempts, test output, and investigation artifacts. They are ignored and may be absent on another device. |

For platform handoff, begin at [Continuation](CONTINUATION.md), then read the [portable phase checkpoint](acceptance/phase-checkpoint-20261030.md). Never copy a status from an older snapshot without checking its source/build/package identity.

## Engineering references by topic

| Topic | Documents |
| --- | --- |
| Agent and application interfaces | [Agent operations](AGENT_OPERATIONS.md), [application services](APPLICATION_SERVICE.md), [conversation records](CONVERSATIONS.md), [extension SDK](EXTENSION_SDK.md), [native document SDK](NATIVE_DOCUMENT_SDK.md), [native model operations](NATIVE_MODEL_OPERATIONS.md), [native results](NATIVE_RESULTS.md) |
| Model and workflow lifecycle | [current model workflow](CURRENT_MODEL_WORKFLOW.md), [project catalog](PROJECT_CATALOG.md), [workflow contract](WORKFLOW_CONTRACT.md), [workflow delivery plan](WORKFLOW_DELIVERY_PLAN.md), [workflow integrated scope](WORKFLOW_INTEGRATED_SCOPE.md), [workflow SDK](WORKFLOW_SDK.md), [WF-10 series contract](WF10_SERIES_CONTRACT.md) |
| Control and structural dynamics | [control profiles](CONTROL_PROFILES.md), [control workbench delivery](CONTROL_WORKBENCH_DELIVERY.md), [control workbench plan](control_workbench_plan.json), [control workbench resume](CONTROL_WORKBENCH_RESUME.md), [control workflow](CONTROL_WORKFLOW.md), [controller adapters](CONTROLLER_ADAPTERS.md), [native control v3 increment](NATIVE_CONTROL_V3_NEXT_INCREMENT.md), [native truss dynamics](NATIVE_TRUSS_DYNAMICS_DESIGN.md), [native truss next increment](NATIVE_TRUSS_NEXT_INCREMENT.md), [typed FE control](TYPED_FE_CONTROL.md) |
| Piezo and external interfaces | [piezo delivery plan](PIEZO_CONTROL_DELIVERY.md), [piezo continuation handoff](PIEZO_CONTROL_CONTINUATION_HANDOFF.md), [piezo interaction log](PIEZO_CONTROL_INTERACTION_LOG.md), [piezo dependency provenance](PIEZO_DEPENDENCY_PROVENANCE.md), [external interface design](PIEZO_EXTERNAL_INTERFACES_DESIGN.md), [piezo machine-readable plan](piezo_control_plan.json) |
| Reliability, recovery, and validation | [CI validation](CI_VALIDATION.md), [GUI UX acceptance](GUI_UX_ACCEPTANCE.md), [LiveLink comparison acceptance](LIVELINK_COMPARISON_ACCEPTANCE.md), [recovery contract](RECOVERY_CONTRACT.md), [recovery operations](RECOVERY_OPERATIONS.md), [reliability audit](RELIABILITY_AUDIT.md) |
| Roadmap and plan navigation | [development plan](DEVELOPMENT_PLAN.md), [task plans index](TASK_PLANS.md), [workflow machine-readable plan](workflow_delivery_plan.json) |

## Historical plans and records

The root `TECHNICAL_ROUTE_STAGE0_3.md` and `TECHNICAL_ROUTE_STAGE4.md`–`TECHNICAL_ROUTE_STAGE9.md` files, plus `AGENT_INTERFACE_DESIGN.md` and `STRUCTURAL_BASELINE.md`, are earlier product/interface references retained at stable paths. `STAGE0_8_AUDIT_20260926.md`, `STAGE9_CONTROL_ANALYSIS.md`, `STAGE12_ACCEPTANCE.md`, dated `*_checkpoint*` files, and dated interaction records are also retained as history. None of these files extends the Trusted Computation Agent course plan beyond Stages 0–8. The product's Stage 9–12 documents, Piezo PC-00–PC-11, Workflow WF-01–WF-14, and Trusted Agent Stages 0–8 are separate numbering systems.

Do not copy a status from an old snapshot into a current plan without matching its source, build, package, and run receipt. Keep failed attempts when they explain a skipped or open gate.

## File placement and cleanup

- Put durable requirements and user-facing entry points in `docs/` only when they are shared across workstreams. Keep raw task logs, hashes, and attempt receipts in local `.planning/<plan>/`; promote only reviewed summaries needed by a fresh checkout into `docs/acceptance/` or `docs/evidence/`.
- Keep stable filenames in place when other documents link to them. Use this index instead of moving legacy files solely to make the directory look flatter.
- Generated build trees, IDE caches, test outputs, and installed candidates are not source documentation. Before removing one, verify that it is reproducible and not referenced by an active or historical acceptance receipt; record the exact paths and hashes.
- A read-only 2026-10-27 inventory measured `build/` at 52,125 MiB (50.9 GiB; 55 independent trees), `.planning/` at 7,267 MiB (7.1 GiB), `.vs/` at 446.7 MiB, `src/` at 1,839.7 MiB, and `.git/` at 387.2 MiB. Build outputs are not safe to remove by age alone: many are explicitly cited by receipts, and the 1.98-GiB `build/audit-20261019/` contains CTest interruption records and install inventories even though no external text path points to it. The active plan's package/install candidates also retain distinct identities. No build or acceptance directory was deleted.
- After that baseline scan, the current-source Stage 8 install/runtime candidate added 3,252 files and 419,341,587 bytes (399.92 MiB) under `.planning/`, including its 3,185-file, 408,251,047-byte install prefix. A follow-up `.planning/` scan measured 53,392 files and 7,666.4 MiB (7.49 GiB). This package is kept because its manifest, executable, and runtime receipts bind current-source Stage 8 evidence.
- The cleanup audit identified 446.64 MiB of ignored Visual Studio cache files. The deletion request was blocked by automatic approval review, so those files remain and the space has not been reclaimed. Loose root-level QtTest outputs were moved byte-for-byte into the local-only `audit/recovered-root-outputs-20261027/` archive; its mapping and hashes, and the detailed cleanup audit, are not included in this checkout.
- A 2026-10-27 follow-up scan measured `docs/` at 100 files / 1.15 MiB, `.planning/` at 53,398 files / 7,666.52 MiB, and the active Trusted Agent plan subtree at 46,868 files / 6,892.23 MiB. The active plan's bulk is install/package candidates and retained evidence, not prose; no candidate was removed from this scan. These values are dated inventory results, not fixed limits.
- A later read-only follow-up measured `.planning/` at 53,444 files / about 7.50 GiB, the active Trusted Agent plan at about 6.74 GiB, and Stage 8 at about 6.61 GiB. Two CPack ZIP pairs had matching internal/output archive hashes, but no receipt or acceptance audit proved the internal copies disposable, so they were retained. No new deletion candidate met the evidence-retention rules. The 50.9-GiB `build/` and 446.7-MiB `.vs/` figures remain from the earlier 2026-10-27 scans; the `.vs/` deletion request was blocked by automatic approval review and was not retried by another path.
