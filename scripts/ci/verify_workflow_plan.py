#!/usr/bin/env python3
"""Validate the workflow delivery backlog without changing it or running tasks."""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import date
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Sequence


ROOT_KEYS = {"schemaVersion", "project", "updatedAt", "baseline", "tasks"}
BASELINE_KEYS = {"rootCommit", "submodules"}
SUBMODULE_KEYS = {"path", "sha"}
TASK_KEYS = {
    "id",
    "title",
    "status",
    "statusReason",
    "priority",
    "effort",
    "dependsOn",
    "detail",
    "acceptanceIds",
    "existingCodePaths",
    "evidence",
}
EVIDENCE_KEYS = {"artifactPath", "outcome", "acceptanceId", "commit", "notes"}
REQUIRED_EVIDENCE_KEYS = EVIDENCE_KEYS - {"notes"}
VALID_STATUSES = {"planned", "in_progress", "blocked", "done"}
VALID_PRIORITIES = {"P0", "P1", "release"}
VALID_EFFORTS = {"S", "M", "L", "XL"}
VALID_EVIDENCE_OUTCOMES = {"pass", "fail", "skipped", "unverified"}
TASK_ID_PATTERN = re.compile(r"^WF-\d{2}$")
ACCEPTANCE_ID_PATTERN = re.compile(r"^(WF-\d{2})-A\d{2}$")
MARKDOWN_TASK_HEADING_PATTERN = re.compile(r"^##\s+(WF-\d{2})(?:\s|$)")
MARKDOWN_SECTION_HEADING_PATTERN = re.compile(r"^##\s+")
FULL_COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class PlanValidationError(ValueError):
    """A plan issue with a stable, non-sensitive machine-readable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def _fail(code: str, message: str) -> None:
    raise PlanValidationError(code, message)


def _expect_object(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail("TYPE_OBJECT", f"{where} must be an object.")
    return value


def _expect_exact_keys(value: dict[str, Any], expected: set[str], where: str) -> None:
    missing = expected - value.keys()
    unknown = value.keys() - expected
    if missing:
        _fail("KEY_MISSING", f"{where} is missing required key(s): {', '.join(sorted(missing))}.")
    if unknown:
        _fail("KEY_UNKNOWN", f"{where} has unknown key(s).")


def _nonempty_string(value: Any, where: str) -> str:
    if not isinstance(value, str) or not value.strip():
        _fail("TYPE_STRING", f"{where} must be a non-empty string.")
    if value != value.strip():
        _fail("STRING_WHITESPACE", f"{where} must not have leading or trailing whitespace.")
    return value


def _safe_relative_path(value: Any, where: str) -> tuple[str, ...]:
    """Return a safe repository-relative path, rejecting traversal and config paths."""
    path_text = _nonempty_string(value, where)
    if "\\" in path_text or "\x00" in path_text:
        _fail("PATH_UNSAFE", f"{where} must use safe slash-separated relative paths.")
    posix_path = PurePosixPath(path_text)
    windows_path = PureWindowsPath(path_text)
    parts = path_text.split("/")
    if posix_path.is_absolute() or windows_path.is_absolute() or windows_path.drive:
        _fail("PATH_UNSAFE", f"{where} must be repository-relative.")
    if any(part in {"", ".", ".."} or ":" in part for part in parts):
        _fail("PATH_UNSAFE", f"{where} must not contain empty, dot, or parent path segments.")
    if any(part.casefold() in {".git", ".codex", ".agents"} or part.casefold().startswith(".env") for part in parts):
        _fail("PATH_UNSAFE", f"{where} must not point into repository metadata or user configuration.")
    return tuple(PurePosixPath(path_text).parts)


def _repository_path(repo_root: Path, parts: tuple[str, ...], where: str, *, must_exist: bool) -> Path:
    root = repo_root.resolve()
    candidate = root.joinpath(*parts)
    try:
        resolved = candidate.resolve(strict=False)
        resolved_parts = resolved.relative_to(root).parts
    except (OSError, RuntimeError, ValueError):
        _fail("PATH_OUTSIDE_REPO", f"{where} must resolve inside the repository.")
    if any(
        part.casefold() in {".git", ".codex", ".agents"} or part.casefold().startswith(".env")
        for part in resolved_parts
    ):
        _fail("PATH_UNSAFE", f"{where} must not resolve into repository metadata or user configuration.")
    if must_exist and not resolved.exists():
        _fail("PATH_MISSING", f"{where} does not exist in the repository.")
    return resolved


def _read_plan(plan_file: Path) -> Any:
    try:
        content = plan_file.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        _fail("PLAN_READ_ERROR", "Could not read the plan file as UTF-8.")
    try:
        return json.loads(content)
    except json.JSONDecodeError as exc:
        _fail("PLAN_JSON_INVALID", f"Plan file is not valid JSON at line {exc.lineno}, column {exc.colno}.")


def _check_dependency_cycles(tasks_by_id: dict[str, dict[str, Any]]) -> None:
    visited: set[str] = set()
    visiting: set[str] = set()

    def visit(task_id: str) -> None:
        if task_id in visiting:
            _fail("DEPENDENCY_CYCLE", f"Dependency cycle includes {task_id}.")
        if task_id in visited:
            return
        visiting.add(task_id)
        for dependency in tasks_by_id[task_id]["dependsOn"]:
            visit(dependency)
        visiting.remove(task_id)
        visited.add(task_id)

    for task_id in tasks_by_id:
        visit(task_id)


def _extract_task_sections(markdown: str, detail_path: str) -> dict[str, set[str]]:
    lines = markdown.splitlines()
    headings: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = MARKDOWN_TASK_HEADING_PATTERN.match(line)
        if match:
            headings.append((index, match.group(1)))

    sections: dict[str, set[str]] = {}
    for start, task_id in headings:
        if task_id in sections:
            _fail("DETAIL_SECTION_DUPLICATE", f"{detail_path} repeats the {task_id} task section.")
        end = len(lines)
        for index in range(start + 1, len(lines)):
            if MARKDOWN_SECTION_HEADING_PATTERN.match(lines[index]):
                end = index
                break
        section_text = "\n".join(lines[start + 1 : end])
        sections[task_id] = set(re.findall(r"\bWF-\d{2}-A\d{2}\b", section_text))
    return sections


def _validate_detail_sections(
    tasks_by_id: dict[str, dict[str, Any]],
    detail_to_task_ids: dict[str, set[str]],
    repo_root: Path,
) -> None:
    for detail, expected_task_ids in detail_to_task_ids.items():
        path_parts = _safe_relative_path(detail, f"detail path {detail}")
        detail_file = _repository_path(repo_root, path_parts, f"detail path {detail}", must_exist=True)
        if not detail_file.is_file():
            _fail("DETAIL_NOT_FILE", f"{detail} must point to a file.")
        try:
            markdown = detail_file.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            _fail("DETAIL_READ_ERROR", f"Could not read the detail document {detail} as UTF-8.")

        sections = _extract_task_sections(markdown, detail)
        missing_sections = expected_task_ids - sections.keys()
        extra_sections = sections.keys() - expected_task_ids
        if missing_sections:
            _fail("DETAIL_SECTION_MISSING", f"{detail} omits task section(s): {', '.join(sorted(missing_sections))}.")
        if extra_sections:
            _fail("DETAIL_SECTION_EXTRA", f"{detail} has task section(s) absent from the JSON plan: {', '.join(sorted(extra_sections))}.")

        for task_id in expected_task_ids:
            expected_ids = set(tasks_by_id[task_id]["acceptanceIds"])
            documented_ids = sections[task_id]
            missing_ids = expected_ids - documented_ids
            extra_ids = documented_ids - expected_ids
            if missing_ids:
                _fail(
                    "ACCEPTANCE_REFERENCE_MISSING",
                    f"{task_id} detail section omits acceptance ID(s): {', '.join(sorted(missing_ids))}.",
                )
            if extra_ids:
                _fail(
                    "ACCEPTANCE_REFERENCE_EXTRA",
                    f"{task_id} detail section has unlisted acceptance ID(s): {', '.join(sorted(extra_ids))}.",
                )


def validate_plan_data(plan: Any, repo_root: Path) -> dict[str, Any]:
    """Validate structure and evidence; each acceptance's last evidence record is authoritative."""
    root = _expect_object(plan, "plan")
    _expect_exact_keys(root, ROOT_KEYS, "plan")
    if root["schemaVersion"] != 1 or isinstance(root["schemaVersion"], bool):
        _fail("SCHEMA_VERSION", "schemaVersion must be 1.")
    if root["project"] != "range-fea":
        _fail("PROJECT_ID", "project must be range-fea.")
    updated_at = _nonempty_string(root["updatedAt"], "updatedAt")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", updated_at):
        _fail("UPDATED_AT", "updatedAt must use YYYY-MM-DD format.")
    try:
        date.fromisoformat(updated_at)
    except ValueError:
        _fail("UPDATED_AT", "updatedAt must use YYYY-MM-DD format.")

    baseline = _expect_object(root["baseline"], "baseline")
    _expect_exact_keys(baseline, BASELINE_KEYS, "baseline")
    root_commit = _nonempty_string(baseline["rootCommit"], "baseline.rootCommit")
    if not FULL_COMMIT_PATTERN.fullmatch(root_commit):
        _fail("BASELINE_COMMIT", "baseline.rootCommit must be a full lowercase commit hash.")
    submodules = baseline["submodules"]
    if not isinstance(submodules, list):
        _fail("BASELINE_SUBMODULES", "baseline.submodules must be a list.")
    seen_submodule_paths: set[str] = set()
    for index, submodule_value in enumerate(submodules):
        submodule = _expect_object(submodule_value, f"baseline.submodules[{index}]")
        _expect_exact_keys(submodule, SUBMODULE_KEYS, f"baseline.submodules[{index}]")
        path_parts = _safe_relative_path(submodule["path"], f"baseline.submodules[{index}].path")
        submodule_path = "/".join(path_parts)
        if submodule_path in seen_submodule_paths:
            _fail("BASELINE_SUBMODULE_DUPLICATE", "baseline.submodules paths must be unique.")
        seen_submodule_paths.add(submodule_path)
        _repository_path(repo_root, path_parts, f"baseline.submodules[{index}].path", must_exist=True)
        submodule_sha = _nonempty_string(submodule["sha"], f"baseline.submodules[{index}].sha")
        if not FULL_COMMIT_PATTERN.fullmatch(submodule_sha):
            _fail("BASELINE_COMMIT", f"baseline.submodules[{index}].sha must be a full lowercase commit hash.")

    tasks = root["tasks"]
    if not isinstance(tasks, list) or not tasks:
        _fail("TASKS_EMPTY", "tasks must contain at least one workflow item.")

    tasks_by_id: dict[str, dict[str, Any]] = {}
    detail_to_task_ids: dict[str, set[str]] = {}
    acceptance_ids_seen: set[str] = set()
    total_acceptances = 0
    for index, task_value in enumerate(tasks):
        task = _expect_object(task_value, f"tasks[{index}]")
        _expect_exact_keys(task, TASK_KEYS, f"tasks[{index}]")
        task_id = _nonempty_string(task["id"], f"tasks[{index}].id")
        if not TASK_ID_PATTERN.fullmatch(task_id):
            _fail("TASK_ID_INVALID", f"tasks[{index}].id must use the WF-NN format.")
        if task_id in tasks_by_id:
            _fail("TASK_ID_DUPLICATE", f"Workflow task IDs must be unique.")
        tasks_by_id[task_id] = task
        _nonempty_string(task["title"], f"{task_id}.title")
        status = _nonempty_string(task["status"], f"{task_id}.status")
        if status not in VALID_STATUSES:
            _fail("STATUS_UNKNOWN", f"{task_id}.status is not a supported task status.")
        status_reason = task["statusReason"]
        if not isinstance(status_reason, str) or status_reason != status_reason.strip():
            _fail("STATUS_REASON", f"{task_id}.statusReason must be a trimmed string.")
        if len(status_reason) > 2000:
            _fail("STATUS_REASON_LENGTH", f"{task_id}.statusReason must not exceed 2000 characters.")
        if status == "blocked" and not status_reason:
            _fail("STATUS_REASON_REQUIRED", f"{task_id}.statusReason is required when status is blocked.")
        priority = _nonempty_string(task["priority"], f"{task_id}.priority")
        if priority not in VALID_PRIORITIES:
            _fail("PRIORITY_UNKNOWN", f"{task_id}.priority is not supported.")
        effort = _nonempty_string(task["effort"], f"{task_id}.effort")
        if effort not in VALID_EFFORTS:
            _fail("EFFORT_UNKNOWN", f"{task_id}.effort is not supported.")

        dependencies = task["dependsOn"]
        if not isinstance(dependencies, list) or any(not isinstance(item, str) for item in dependencies):
            _fail("DEPENDENCIES_TYPE", f"{task_id}.dependsOn must be a list of task IDs.")
        if len(dependencies) != len(set(dependencies)):
            _fail("DEPENDENCY_DUPLICATE", f"{task_id}.dependsOn contains duplicate IDs.")
        if any(not TASK_ID_PATTERN.fullmatch(dependency) for dependency in dependencies):
            _fail("DEPENDENCY_ID_INVALID", f"{task_id}.dependsOn contains an invalid task ID.")

        acceptance_ids = task["acceptanceIds"]
        if not isinstance(acceptance_ids, list) or not acceptance_ids:
            _fail("ACCEPTANCE_IDS_EMPTY", f"{task_id}.acceptanceIds must contain at least one ID.")
        for acceptance_id in acceptance_ids:
            if not isinstance(acceptance_id, str):
                _fail("ACCEPTANCE_ID_INVALID", f"{task_id}.acceptanceIds contains an invalid ID.")
            match = ACCEPTANCE_ID_PATTERN.fullmatch(acceptance_id)
            if not match or match.group(1) != task_id:
                _fail("ACCEPTANCE_ID_INVALID", f"{task_id}.acceptanceIds contains an invalid ID.")
            if acceptance_id in acceptance_ids_seen:
                _fail("ACCEPTANCE_ID_DUPLICATE", "Acceptance IDs must be unique across the plan.")
            acceptance_ids_seen.add(acceptance_id)
        total_acceptances += len(acceptance_ids)

        detail = "/".join(_safe_relative_path(task["detail"], f"{task_id}.detail"))
        detail_path = _repository_path(
            repo_root, tuple(PurePosixPath(detail).parts), f"{task_id}.detail", must_exist=True
        )
        if not detail_path.is_file():
            _fail("DETAIL_NOT_FILE", f"{task_id}.detail must point to a file.")
        detail_to_task_ids.setdefault(detail, set()).add(task_id)

        code_paths = task["existingCodePaths"]
        if not isinstance(code_paths, list) or not code_paths:
            _fail("CODE_PATHS_EMPTY", f"{task_id}.existingCodePaths must contain existing repository paths.")
        normalized_code_paths: set[str] = set()
        for path_index, value in enumerate(code_paths):
            parts = _safe_relative_path(value, f"{task_id}.existingCodePaths[{path_index}]")
            normalized = "/".join(parts)
            if normalized in normalized_code_paths:
                _fail("CODE_PATH_DUPLICATE", f"{task_id}.existingCodePaths contains a duplicate path.")
            normalized_code_paths.add(normalized)
            _repository_path(repo_root, parts, f"{task_id}.existingCodePaths[{path_index}]", must_exist=True)

        evidence = task["evidence"]
        if not isinstance(evidence, list):
            _fail("EVIDENCE_TYPE", f"{task_id}.evidence must be a list.")
        for evidence_index, entry_value in enumerate(evidence):
            where = f"{task_id}.evidence[{evidence_index}]"
            entry = _expect_object(entry_value, where)
            unknown = entry.keys() - EVIDENCE_KEYS
            missing = REQUIRED_EVIDENCE_KEYS - entry.keys()
            if unknown:
                _fail("KEY_UNKNOWN", f"{where} has unknown key(s).")
            if missing:
                _fail("KEY_MISSING", f"{where} is missing required key(s): {', '.join(sorted(missing))}.")
            acceptance_id = _nonempty_string(entry["acceptanceId"], f"{where}.acceptanceId")
            if acceptance_id not in acceptance_ids:
                _fail("EVIDENCE_ACCEPTANCE_UNKNOWN", f"{where}.acceptanceId must identify an acceptance in {task_id}.")
            outcome = _nonempty_string(entry["outcome"], f"{where}.outcome")
            if outcome not in VALID_EVIDENCE_OUTCOMES:
                _fail("EVIDENCE_OUTCOME", f"{where}.outcome must be pass, fail, skipped, or unverified.")
            artifact_parts = _safe_relative_path(entry["artifactPath"], f"{where}.artifactPath")
            artifact_path = _repository_path(
                repo_root,
                artifact_parts,
                f"{where}.artifactPath",
                must_exist=(status == "done"),
            )
            if status == "done" and not artifact_path.is_file():
                _fail("EVIDENCE_ARTIFACT_MISSING", f"{where}.artifactPath must identify an existing file for a done task.")
            commit = _nonempty_string(entry["commit"], f"{where}.commit")
            if not FULL_COMMIT_PATTERN.fullmatch(commit):
                _fail("EVIDENCE_COMMIT", f"{where}.commit must be a full lowercase Git commit hash.")
            if "notes" in entry and (not isinstance(entry["notes"], str) or entry["notes"] != entry["notes"].strip()):
                _fail("EVIDENCE_NOTES", f"{where}.notes must be a string without surrounding whitespace.")

    for task_id, task in tasks_by_id.items():
        missing_dependencies = [dependency for dependency in task["dependsOn"] if dependency not in tasks_by_id]
        if missing_dependencies:
            _fail(
                "DEPENDENCY_MISSING",
                f"{task_id}.dependsOn refers to unknown task ID(s): {', '.join(missing_dependencies)}.",
            )
    _check_dependency_cycles(tasks_by_id)
    for task_id, task in tasks_by_id.items():
        incomplete_dependencies = [
            dependency for dependency in task["dependsOn"] if tasks_by_id[dependency]["status"] != "done"
        ]
        if task["status"] == "in_progress" and incomplete_dependencies:
            _fail(
                "IN_PROGRESS_DEPENDENCY_INCOMPLETE",
                f"{task_id} cannot be in_progress while dependencies are incomplete: {', '.join(incomplete_dependencies)}.",
            )
        if task["status"] == "done":
            if incomplete_dependencies:
                _fail(
                    "DONE_DEPENDENCY_INCOMPLETE",
                    f"{task_id} cannot be done while dependencies are incomplete: {', '.join(incomplete_dependencies)}.",
                )
            latest_outcome: dict[str, str] = {}
            for entry in task["evidence"]:
                latest_outcome[entry["acceptanceId"]] = entry["outcome"]
            missing_passes = {
                acceptance_id
                for acceptance_id in task["acceptanceIds"]
                if latest_outcome.get(acceptance_id) != "pass"
            }
            if missing_passes:
                _fail(
                    "DONE_EVIDENCE_INCOMPLETE",
                    f"{task_id} lacks latest passing evidence for: {', '.join(sorted(missing_passes))}.",
                )

    _validate_detail_sections(tasks_by_id, detail_to_task_ids, repo_root)

    return {
        "taskCount": len(tasks_by_id),
        "acceptanceCount": total_acceptances,
        "statusCounts": {
            status: sum(1 for task in tasks if task["status"] == status)
            for status in ("planned", "in_progress", "blocked", "done")
        },
        "tasks": tasks,
    }


def get_next_work(tasks: list[dict[str, Any]]) -> dict[str, Any]:
    """Return read-only resume and eligible suggestions, preserving all statuses."""
    tasks_by_id = {task["id"]: task for task in tasks}
    priority_order = {"P0": 0, "P1": 1, "release": 2}
    task_sort_key = lambda task: (priority_order[task["priority"]], task["id"])
    in_progress = sorted((task for task in tasks if task["status"] == "in_progress"), key=task_sort_key)
    eligible = sorted(
        (
            task
            for task in tasks
            if task["status"] == "planned"
            and all(tasks_by_id[dependency]["status"] == "done" for dependency in task["dependsOn"])
        ),
        key=task_sort_key,
    )
    blocked = sorted((task for task in tasks if task["status"] == "blocked"), key=task_sort_key)
    recommended = in_progress if in_progress else eligible
    return {
        "resumeFirst": [task["id"] for task in in_progress],
        "eligible": [task["id"] for task in eligible],
        "blocked": [task["id"] for task in blocked],
        "blockedReasons": {task["id"]: task["statusReason"] for task in blocked},
        "recommended": [task["id"] for task in recommended],
        "note": "Blocked tasks remain blocked; this report never changes task status or evidence.",
        "commands": [
            "python scripts/ci/verify_workflow_plan.py --next",
            "python scripts/ci/verify_workflow_plan.py --json",
        ],
    }


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--plan",
        type=Path,
        default=Path("docs/workflow_delivery_plan.json"),
        help="plan JSON path, relative to the repository root (default: docs/workflow_delivery_plan.json)",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        help="repository root (default: the checkout containing this verifier)",
    )
    parser.add_argument("--next", action="store_true", help="include read-only next-work suggestions")
    parser.add_argument("--json", action="store_true", help="print a JSON report")
    return parser


def _plan_file_path(plan_argument: Path, repo_root: Path) -> tuple[Path, str]:
    if plan_argument.is_absolute():
        try:
            relative = plan_argument.resolve().relative_to(repo_root.resolve())
        except (OSError, RuntimeError, ValueError):
            _fail("PLAN_PATH_OUTSIDE_REPO", "--plan must identify a file inside the repository.")
        plan_text = relative.as_posix()
    else:
        plan_text = plan_argument.as_posix()
    parts = _safe_relative_path(plan_text, "--plan")
    return _repository_path(repo_root, parts, "--plan", must_exist=True), "/".join(parts)


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    repo_root = (args.repo_root if args.repo_root is not None else Path(__file__).resolve().parents[2]).resolve()
    report: dict[str, Any]
    plan_label = args.plan.as_posix() if not args.plan.is_absolute() else "<repository plan>"
    try:
        # A valid checkout may live under .codex/worktrees. Only reject a root
        # that itself names metadata; referenced paths remain confined below it.
        if repo_root.name.casefold() in {".git", ".codex", ".agents"} or repo_root.name.casefold().startswith(".env"):
            _fail("REPO_ROOT_UNSAFE", "Repository root must not point into repository metadata or user configuration.")
        if not repo_root.is_dir():
            _fail("REPO_ROOT_INVALID", "Repository root must be an existing directory.")
        plan_file, plan_label = _plan_file_path(args.plan, repo_root)
        plan = _read_plan(plan_file)
        validated = validate_plan_data(plan, repo_root)
        report = {
            "ok": True,
            "plan": plan_label,
            "taskCount": validated["taskCount"],
            "acceptanceCount": validated["acceptanceCount"],
            "statusCounts": validated["statusCounts"],
        }
        if args.next:
            suggestions = get_next_work(validated["tasks"])
            if suggestions["recommended"]:
                first_id = suggestions["recommended"][0]
                first_task = next(task for task in validated["tasks"] if task["id"] == first_id)
                suggestions["recommendedDetail"] = f"{first_task['detail']}#{first_id.lower()}"
            report["next"] = suggestions
    except PlanValidationError as exc:
        report = {"ok": False, "errors": [{"code": exc.code, "message": exc.message}]}
        if args.json:
            # Keep machine-readable output ASCII-safe on Windows runners whose
            # redirected stdout may use a legacy code page such as cp1252.
            print(json.dumps(report, ensure_ascii=True, indent=2))
        else:
            print(f"error[{exc.code}]: {exc.message}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, ensure_ascii=True, indent=2))
    elif args.next:
        suggestions = report["next"]
        if suggestions["resumeFirst"]:
            print("Resume in-progress task(s) first: " + ", ".join(suggestions["resumeFirst"]))
        elif suggestions["eligible"]:
            print("Eligible planned task(s), ordered by priority: " + ", ".join(suggestions["eligible"]))
            print(f"Start with {suggestions['recommended'][0]} in {suggestions['recommendedDetail']}.")
        else:
            print("No planned task is currently eligible.")
        if suggestions["blocked"]:
            for task_id in suggestions["blocked"]:
                reason = suggestions["blockedReasons"][task_id]
                suffix = f" — {reason}" if reason else ""
                print(f"Blocked task remains blocked: {task_id}{suffix}")
        for command in suggestions["commands"]:
            print(f"Check command: {command}")
    else:
        print(
            f"Verified {report['taskCount']} workflow tasks and {report['acceptanceCount']} acceptance IDs; "
            f"statuses: {report['statusCounts']}."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
