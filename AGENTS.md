# Codex project instructions

For complex coding tasks, use the `astra-orchestrator` skill when its trigger conditions match.

The root agent owns architecture, decomposition, integration, and final verification.
Prefer specialized subagents for bounded exploration, implementation, testing, review, and technical research.

Do not delegate trivial work merely for parallelism.
Do not let multiple implementation agents edit the same files without explicit ownership boundaries.
User instructions always take precedence over this orchestration policy.

## Repository work and delivery

- Read [the repository maintenance rules](docs/development/REPOSITORY_MAINTENANCE.md) before creating, moving, or deleting files or publishing changes. Read the nearest nested `AGENTS.md` too.
- Keep source in `src/`, reusable tools in the owning group under `scripts/`, long-lived documentation in `docs/reference/` or `docs/development/`, and goal-specific records in `docs/rounds/<date>-<goal>/`. Use `docs/history/` for shared history and `.planning/<task-id>/` for local working notes.
- Put builds in `build/` and run output in `artifacts/<task-or-run-id>/`. Keep credentials, profiles, caches, raw run output, and local planning out of Git.
- Clean up task-owned, reproducible temporary output when a run ends and again before handoff. Check references and active processes first. Preserve evidence, current dependencies, and files owned by the user or another task.
- At each coherent, reviewed checkpoint, commit the task-owned changes and push them to the confirmed work branch by default. Respect any user pause, read-only request, or restriction on committing or pushing. Review the staged scope first and report the commit, remote branch, and verification status. Keep pre-existing changes and submodule work outside the checkpoint unless specifically included.
- Use the current branch for a coordinated continuation. For a new independent task, prefer `codex/<task>` from the agreed base. Publish `main` only through the agreed integration process. Do not force-push or change remote, branch, or submodule history as part of routine closeout.

## Artifact location policy

- Do not create project-specific files or folders outside this checkout, including ad hoc directories at drive roots, in LOCALAPPDATA, or in TEMP.
- Put compiler output under build/ and project run artifacts such as installed candidates, isolated profiles, screenshots, logs, and runtime state under artifacts/<task-or-run-id>/.
- Before launching a test or application that writes user-profile or temporary data, redirect its profile and temporary paths into the checkout and verify the resolved output path. Do not launch it if the output cannot be kept inside the checkout.
