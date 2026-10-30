# Project continuation

**Snapshot:** 2026-10-30 (Asia/Singapore). This file is the portable entry point for a fresh checkout. The source freeze is `ad9dbda685fa1a6dec08b9aecd6ec48ef0ac2a1d` on `feature/structural-profile`; the dated [phase checkpoint](acceptance/phase-checkpoint-20261030.md) records the source, build, CI, and course-stage status. Clone the branch tip after publication to include this follow-up handoff.

## Resume order

1. Read the [phase checkpoint](acceptance/phase-checkpoint-20261030.md) and root [handoff rules](../HANDOFF.md).
2. For this handoff, use the personal `origin/feature/structural-profile` branch recorded in the phase checkpoint, then run `git submodule update --init --recursive`. Check out the root commit and each pinned gitlink before building. Changes in `src/range-*-lib/` are separate Git repositories and must be committed and pushed in their own repositories before changing the root gitlinks.
3. Follow the [Linux build guide](../README.md) and [CI validation notes](CI_VALIDATION.md). The Windows Release result recorded in the phase checkpoint does not claim a Linux build.
4. Use the workstream documents below for requirements; treat old dated reports as evidence only for their exact source/build/run identity.

## Workstream entry points

- Trusted Computation Agent: [requirements and scope](TRUSTED_COMPUTATION_AGENT_PLAN.md), [bounded experiment authorization](agent-bounded-experiment-authorization.md), and the current acceptance status in the phase checkpoint.
- Piezo control: [delivery requirements](PIEZO_CONTROL_DELIVERY.md), [machine-readable plan](piezo_control_plan.json), and [external interface design](PIEZO_EXTERNAL_INTERFACES_DESIGN.md).
- Workflow: [delivery plan](WORKFLOW_DELIVERY_PLAN.md), [machine-readable plan](workflow_delivery_plan.json), and [contract](WORKFLOW_CONTRACT.md).
- Control Workbench: [delivery plan](CONTROL_WORKBENCH_DELIVERY.md) and [machine-readable plan](control_workbench_plan.json); it remains a separate product workstream with incomplete items.
- Product architecture: root [README](../README.md), [project roadmap](../PROJECT_ROADMAP.md), and the API/design documents indexed in [docs/README](README.md).

## Evidence and generated files

Portable summaries belong under `docs/acceptance/` and `docs/evidence/`. Build trees belong under `build/`; test logs, runtime profiles, temporary files, and screenshots belong under `artifacts/<run-id>/`. `.planning/`, `audit/`, `newaudit/`, `Testing/`, `.codex/`, and `.agents/` are local-only records and are ignored by Git. A fresh checkout may not contain them. Never cite a missing local log as if it were part of the repository.

## Course acceptance boundary

The user's course-scope decision waived execution of the proposed 180-slot three-arm comparison because there is no prepared case set. The comparison remains useful planned research work; it is unperformed and is not accepted evidence. Clean Windows-account first use was also waived. These waivers do not close the corresponding acceptance criteria. Case B's latest real DeepSeek Pro inspect/diagnose request returned `Request failed`; Case C's report was accepted only on the targeted patched INSTALL-07 candidate, not as qualification of a complete current-source installation. The installed end-to-end UI flow, Linux build, final source/dependency/contribution freeze, and delivery review remain open. Two Windows model-operation failures and the private Python runtime cache issue are also recorded in the checkpoint as open engineering/verification work.
