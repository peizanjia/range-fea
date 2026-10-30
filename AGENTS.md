# Codex project instructions

For complex coding tasks, use the `astra-orchestrator` skill when its trigger conditions match.

The root agent owns architecture, decomposition, integration, and final verification.
Prefer specialized subagents for bounded exploration, implementation, testing, review, and technical research.

Do not delegate trivial work merely for parallelism.
Do not let multiple implementation agents edit the same files without explicit ownership boundaries.
User instructions always take precedence over this orchestration policy.

## Artifact location policy

- Do not create project-specific files or folders outside this checkout, including ad hoc directories at drive roots, in LOCALAPPDATA, or in TEMP.
- Put compiler output under build/ and project run artifacts such as installed candidates, isolated profiles, screenshots, logs, and runtime state under artifacts/<task-or-run-id>/.
- Before launching a test or application that writes user-profile or temporary data, redirect its profile and temporary paths into the checkout and verify the resolved output path. Do not launch it if the output cannot be kept inside the checkout.
