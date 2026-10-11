#!/usr/bin/env python3
"""Verify frozen delivery scope and evidence bytes, not physical qualification."""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED_SCOPE_SHA256 = "d1d5fd76a14fba2b68e4b06fbeb2428da796e98c512e0b8b7940fb1545207dda"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify(plan, root):
    errors = []
    tasks = plan.get("tasks", [])
    requirements = plan.get("requirements", [])
    if plan.get("schemaVersion") != 1 or type(plan.get("productComplete")) is not bool:
        errors.append("Invalid schema or productComplete flag")
    if [r.get("id") for r in requirements] != [f"PC-R{i:02d}" for i in range(1, 13)]:
        errors.append("Required complete requirement inventory changed")
    if [t.get("id") for t in tasks] != [f"PC-{i:02d}" for i in range(12)]:
        errors.append("Required complete stage inventory changed")
    scope = {k: plan.get(k) for k in ("schemaVersion", "project", "objective", "requirements")}
    scope["tasks"] = [{k: t.get(k) for k in ("id", "title", "dependsOn", "requirementIds")}
                      | {"acceptance": [{k: a.get(k) for k in ("id", "description")}
                                        for a in t.get("acceptance", [])]} for t in tasks]
    actual_scope = hashlib.sha256(json.dumps(scope, ensure_ascii=False, sort_keys=True,
                                            separators=(",", ":")).encode()).hexdigest()
    if actual_scope != EXPECTED_SCOPE_SHA256 or plan.get("frozenScopeSha256") != actual_scope:
        errors.append("Frozen scope, dependencies or criteria changed")
    by_id = {t.get("id"): t for t in tasks}
    artifacts = [plan.get("contractDocument", {})]
    coverage = set()
    for task in tasks:
        identifier = task.get("id")
        if task.get("status") not in {"planned", "in_progress", "done"}:
            errors.append(f"{identifier}: invalid status")
        coverage.update(task.get("requirementIds", []))
        criteria = task.get("acceptance", [])
        if [a.get("id") for a in criteria] != [f"{identifier}-A{i:02d}" for i in range(1, 4)]:
            errors.append(f"{identifier}: missing or changed criteria")
        for item in criteria:
            if item.get("status") not in {"pending", "pass", "fail"}:
                errors.append(f"{item.get('id')}: invalid status")
            evidence = item.get("evidence", [])
            if not isinstance(evidence, list) or (item.get("status") == "pass" and not evidence):
                errors.append(f"{item.get('id')}: passed criterion requires evidence")
            elif isinstance(evidence, list):
                artifacts.extend(evidence)
        if task.get("status") == "done":
            if any(a.get("status") != "pass" for a in criteria):
                errors.append(f"{identifier}: done requires all criteria pass")
            if any(by_id.get(dep, {}).get("status") != "done" for dep in task.get("dependsOn", [])):
                errors.append(f"{identifier}: done requires accepted prerequisites")
    if coverage != {f"PC-R{i:02d}" for i in range(1, 13)}:
        errors.append("Requirements not completely mapped")
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not isinstance(artifact.get("path"), str):
            errors.append("Invalid evidence object")
            continue
        relative = Path(artifact["path"])
        path = (root / relative).resolve()
        if relative.is_absolute() or not path.is_relative_to(root) or not path.is_file():
            errors.append(f"Invalid evidence path: {relative}")
        elif artifact.get("sha256") != digest(path):
            errors.append(f"Evidence bytes changed: {relative}")
    unfinished = [t.get("id") for t in tasks if t.get("status") != "done"]
    if plan.get("productComplete") and unfinished:
        errors.append("Unfinished stages cannot establish productComplete")
    return {"valid": not errors, "verificationScope": "tracking-and-evidence-bytes-only",
            "productComplete": plan.get("productComplete"), "unfinishedStages": unfinished,
            "requirements": len(requirements), "stages": len(tasks), "acceptance": 36,
            "errors": errors}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--plan", type=Path, default=Path("docs/piezo_control_plan.json"))
    args = parser.parse_args()
    root = args.root.resolve()
    result = verify(json.loads((root / args.plan).read_text(encoding="utf-8")), root)
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["valid"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
