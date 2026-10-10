#!/usr/bin/env python3
"""Validate delivery tracking and evidence bytes, not physical correctness."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path

EXPECTED_REQUIREMENTS = {f"CW-R{i:02d}" for i in range(1, 15)}
EXPECTED_TASKS = {f"CW-{i:02d}" for i in range(1, 17)}
EXPECTED_ACCEPTANCE = {f"CW-{i:02d}-A{j:02d}" for i in range(1, 17) for j in range(1, 4)}
EXPECTED_SCOPE_SHA256 = "07e488f3a58dd10dc24ec48467f87294540cbe9c1909f82802f8c5b786c0a405"
STATUSES = {"planned", "in_progress", "done"}
ACCEPTANCE_STATUSES = {"pending", "pass", "fail"}

def scope_signature(plan):
    """Freeze requirements, dependency graph, and criteria; allow evidence/status updates."""
    scope = {"schemaVersion": plan.get("schemaVersion"),
             "project": plan.get("project"), "objective": plan.get("objective"),
             "requirements": plan.get("requirements"), "tasks": []}
    for task in plan.get("tasks", []):
        scope["tasks"].append({key: task.get(key) for key in
                               ("id", "title", "dependsOn", "requirementIds")})
        scope["tasks"][-1]["acceptance"] = [
            {key: criterion.get(key) for key in ("id", "description")}
            for criterion in task.get("acceptance", [])]
    return hashlib.sha256(json.dumps(scope, ensure_ascii=False, sort_keys=True,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()

def inspect(plan, root: Path):
    errors = []
    if not isinstance(plan, dict) or type(plan.get("schemaVersion")) is not int or plan.get("schemaVersion") != 1:
        return ["Unsupported control-workbench plan schema"], []
    if type(plan.get("productComplete")) is not bool:
        errors.append("productComplete must be an explicit boolean")
    requirements = plan.get("requirements")
    tasks = plan.get("tasks")
    if not isinstance(requirements, list) or not isinstance(tasks, list):
        return errors + ["requirements and tasks must be lists"], []
    if any(not isinstance(item, dict) for item in requirements + tasks):
        return errors + ["Requirements and tasks must be objects"], []
    requirement_ids = [item.get("id") for item in requirements]
    task_ids = [item.get("id") for item in tasks]
    if (any(not isinstance(value, str) for value in requirement_ids + task_ids)
            or len(set(requirement_ids)) != len(requirement_ids)
            or set(requirement_ids) != EXPECTED_REQUIREMENTS):
        errors.append("The complete agreed requirement inventory must be preserved")
    if (any(not isinstance(value, str) for value in task_ids)
            or len(set(task_ids)) != len(task_ids)
            or set(task_ids) != EXPECTED_TASKS):
        errors.append("The complete staged task inventory must be preserved")
    ids = {value for value in task_ids if isinstance(value, str)}
    by_id = {item["id"]: item for item in tasks if isinstance(item.get("id"), str)}
    covered = set()
    acceptance_ids = set()
    root = root.resolve()
    for task in tasks:
        identifier = task.get("id", "<missing>")
        status = task.get("status")
        if status not in STATUSES:
            errors.append(f"{identifier}: invalid status")
        dependencies = task.get("dependsOn", [])
        if (not isinstance(dependencies, list)
                or any(not isinstance(dep, str) or dep not in ids or dep == identifier for dep in dependencies)):
            errors.append(f"{identifier}: invalid dependencies")
            dependencies = []
        mappings = task.get("requirementIds", [])
        if (not isinstance(mappings, list) or not mappings
                or any(not isinstance(value, str) or value not in EXPECTED_REQUIREMENTS for value in mappings)):
            errors.append(f"{identifier}: invalid requirement mapping")
        elif identifier != "CW-01":
            covered.update(mappings)
        acceptance = task.get("acceptance")
        if not isinstance(acceptance, list) or not acceptance:
            errors.append(f"{identifier}: missing acceptance")
            continue
        for criterion in acceptance:
            if not isinstance(criterion, dict):
                errors.append(f"{identifier}: invalid acceptance object")
                continue
            aid = criterion.get("id")
            if not isinstance(aid, str) or not re.fullmatch(r"CW-\d{2}-A\d{2}", aid):
                errors.append(f"{identifier}: invalid acceptance ID")
            elif aid in acceptance_ids or not aid.startswith(str(identifier) + "-"):
                errors.append(f"{identifier}: duplicate or incorrectly scoped acceptance ID")
            else:
                acceptance_ids.add(aid)
            cstatus = criterion.get("status")
            if cstatus not in ACCEPTANCE_STATUSES:
                errors.append(f"{aid}: invalid acceptance status")
            evidence = criterion.get("evidence")
            if not isinstance(evidence, list):
                errors.append(f"{aid}: evidence must be a list")
                continue
            if cstatus == "pass" and not evidence:
                errors.append(f"{aid}: passed acceptance needs inspectable evidence")
            for artifact in evidence:
                if not isinstance(artifact, dict):
                    errors.append(f"{aid}: invalid evidence")
                    continue
                relative = artifact.get("path")
                digest = artifact.get("sha256")
                if (not isinstance(relative, str) or not relative
                        or not isinstance(digest, str) or not re.fullmatch("[a-f0-9]{64}", digest)):
                    errors.append(f"{aid}: invalid evidence path/hash")
                    continue
                path = (root / relative).resolve()
                if Path(relative).is_absolute() or not path.is_relative_to(root):
                    errors.append(f"{aid}: evidence escapes project root")
                elif not path.is_file():
                    errors.append(f"{aid}: evidence is missing: {relative}")
                elif hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    errors.append(f"{aid}: evidence changed: {relative}")
            if status == "done" and cstatus != "pass":
                errors.append(f"{identifier}: done requires every acceptance to pass")
        if status == "done" and any(by_id.get(dep, {}).get("status") != "done" for dep in dependencies):
            errors.append(f"{identifier}: dependency not done")
    if covered != EXPECTED_REQUIREMENTS:
        errors.append("Implementation tasks must cover every agreed requirement")
    if acceptance_ids != EXPECTED_ACCEPTANCE:
        errors.append("The complete agreed acceptance inventory must be preserved")
    try:
        if scope_signature(plan) != EXPECTED_SCOPE_SHA256:
            errors.append("The frozen requirements, descriptions, mappings, or dependencies changed")
    except (TypeError, AttributeError):
        errors.append("Cannot read the frozen scope")
    visited, visiting = set(), set()
    def visit(identifier):
        if identifier in visiting:
            errors.append(f"{identifier}: dependency cycle")
            return
        if identifier in visited:
            return
        visiting.add(identifier)
        dependencies = by_id.get(identifier, {}).get("dependsOn", [])
        if isinstance(dependencies, list):
            for dep in dependencies:
                if isinstance(dep, str) and dep in by_id:
                    visit(dep)
        visiting.remove(identifier)
        visited.add(identifier)
    for identifier in by_id:
        visit(identifier)
    unfinished = [item["id"] for item in tasks
                  if isinstance(item.get("id"), str) and item.get("status") != "done"]
    if plan.get("productComplete") and unfinished:
        errors.append("productComplete cannot be true while required tasks remain")
    return errors, unfinished

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--complete", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    path = args.plan or root / "docs/control_workbench_plan.json"
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
        errors, unfinished = inspect(plan, root)
    except (OSError, ValueError, TypeError, RecursionError) as error:
        plan, unfinished, errors = {}, [], [f"Cannot validate plan: {error}"]
    if args.complete and (unfinished or not plan.get("productComplete", False)):
        errors.append("The full goal is not complete")
    result = {"valid": not errors, "verificationScope": "tracking-and-evidence-bytes",
              "productComplete": plan.get("productComplete", False),
              "unfinishedTasks": unfinished, "errors": errors}
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print("Tracking valid" if not errors else "Tracking rejected")
        for error in errors:
            print(error)
        if unfinished:
            print("Unfinished: " + ", ".join(unfinished))
    return 0 if not errors else 1

if __name__ == "__main__":
    raise SystemExit(main())

