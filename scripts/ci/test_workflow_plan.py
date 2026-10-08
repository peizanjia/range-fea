"""Focused tests for the workflow plan's generic validation rules."""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from verify_workflow_plan import PlanValidationError, get_next_work, main, validate_plan_data


def make_plan() -> dict:
    tasks = [
        {
            "id": "WF-01",
            "title": "Identity",
            "status": "planned",
            "statusReason": "",
            "priority": "P0",
            "effort": "S",
            "dependsOn": [],
            "detail": "docs/WORKFLOW_DELIVERY_PLAN.md",
            "acceptanceIds": ["WF-01-A01", "WF-01-A02"],
            "existingCodePaths": ["src/range-workflow"],
            "evidence": [],
        },
        {
            "id": "WF-02",
            "title": "Commands",
            "status": "planned",
            "statusReason": "",
            "priority": "P1",
            "effort": "L",
            "dependsOn": ["WF-01"],
            "detail": "docs/WORKFLOW_DELIVERY_PLAN.md",
            "acceptanceIds": ["WF-02-A01"],
            "existingCodePaths": ["src/range-workflow"],
            "evidence": [],
        },
        {
            "id": "WF-03",
            "title": "Read only queries",
            "status": "planned",
            "statusReason": "",
            "priority": "release",
            "effort": "M",
            "dependsOn": ["WF-01"],
            "detail": "docs/WORKFLOW_DELIVERY_PLAN.md",
            "acceptanceIds": ["WF-03-A01"],
            "existingCodePaths": ["src/range-workflow"],
            "evidence": [],
        },
    ]
    return {
        "schemaVersion": 1,
        "project": "range-fea",
        "updatedAt": "2026-10-08",
        "baseline": {
            "rootCommit": "a" * 40,
            "submodules": [{"path": "src/range-ai-lib", "sha": "b" * 40}],
        },
        "tasks": tasks,
    }


class WorkflowPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo_root = Path(self.temporary_directory.name)
        (self.repo_root / "src" / "range-workflow").mkdir(parents=True)
        (self.repo_root / "src" / "range-ai-lib").mkdir(parents=True)
        docs = self.repo_root / "docs"
        docs.mkdir()
        plan = make_plan()
        sections = []
        for task in plan["tasks"]:
            ids = "\n".join(f"- {acceptance_id}" for acceptance_id in task["acceptanceIds"])
            sections.append(f"## {task['id']} Test task\n\n{ids}")
        self.detail_file = docs / "WORKFLOW_DELIVERY_PLAN.md"
        self.detail_file.write_text("\n\n".join(sections), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_cli_accepts_checkout_under_codex_worktrees(self) -> None:
        worktree = self.repo_root / ".codex" / "worktrees" / "planning"
        (worktree / "src" / "range-workflow").mkdir(parents=True)
        (worktree / "src" / "range-ai-lib").mkdir()
        (worktree / "docs").mkdir()
        (worktree / "docs" / "WORKFLOW_DELIVERY_PLAN.md").write_text(
            self.detail_file.read_text(encoding="utf-8"), encoding="utf-8")
        (worktree / "docs" / "workflow_delivery_plan.json").write_text(
            json.dumps(make_plan()), encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = main(["--repo-root", str(worktree), "--next", "--json"])
        self.assertEqual(result, 0)
        report = json.loads(output.getvalue())
        self.assertTrue(report["ok"])
        self.assertEqual(report["next"]["recommended"], ["WF-01"])

    def test_current_plan_matches_document_and_cli_recommends_wf01(self) -> None:
        checkout = Path(__file__).resolve().parents[2]
        plan_file = checkout / "docs" / "workflow_delivery_plan.json"
        plan = json.loads(plan_file.read_text(encoding="utf-8"))
        result = validate_plan_data(plan, checkout)
        self.assertEqual(result["taskCount"], 14)
        self.assertEqual(result["acceptanceCount"], 119)
        self.assertEqual(result["statusCounts"]["planned"], 14)

        original = plan_file.read_text(encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_code = main(["--repo-root", str(checkout), "--next", "--json"])
        report = json.loads(output.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["next"]["recommended"], ["WF-01"])
        self.assertEqual(report["next"]["recommendedDetail"], "docs/WORKFLOW_DELIVERY_PLAN.md#wf-01")
        self.assertEqual(plan_file.read_text(encoding="utf-8"), original)

    def test_replan_uses_json_dependencies_and_priorities(self) -> None:
        plan = make_plan()
        plan["tasks"][0]["priority"] = "P1"
        plan["tasks"][1]["dependsOn"] = []
        plan["tasks"][1]["priority"] = "P1"
        plan["tasks"][2]["dependsOn"] = []
        plan["tasks"][2]["priority"] = "P0"

        result = validate_plan_data(plan, self.repo_root)
        self.assertEqual(result["taskCount"], 3)
        self.assertEqual(get_next_work(result["tasks"])["eligible"], ["WF-03", "WF-01", "WF-02"])

        plan["tasks"][1]["status"] = "in_progress"
        plan["tasks"][2]["status"] = "blocked"
        plan["tasks"][2]["statusReason"] = "Waiting for environment evidence."
        validate_plan_data(plan, self.repo_root)
        next_work = get_next_work(plan["tasks"])
        self.assertEqual(next_work["resumeFirst"], ["WF-02"])
        self.assertEqual(next_work["blockedReasons"], {"WF-03": "Waiting for environment evidence."})

        plan["tasks"][2]["statusReason"] = ""
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "STATUS_REASON_REQUIRED")

        plan["tasks"][1]["dependsOn"] = ["WF-01"]
        plan["tasks"][2]["statusReason"] = "Waiting for environment evidence."
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "IN_PROGRESS_DEPENDENCY_INCOMPLETE")

    def test_done_uses_latest_evidence_and_requires_completed_dependencies(self) -> None:
        plan = make_plan()
        artifact = self.repo_root / "build" / "evidence.txt"
        artifact.parent.mkdir()
        artifact.write_text("reviewed evidence", encoding="utf-8")

        def evidence(acceptance_id: str, outcome: str) -> dict:
            return {
                "artifactPath": "build/evidence.txt",
                "outcome": outcome,
                "acceptanceId": acceptance_id,
                "commit": "a" * 40,
            }

        first, second = plan["tasks"][:2]
        first["status"] = "done"
        first["evidence"] = [evidence(acceptance_id, "pass") for acceptance_id in first["acceptanceIds"]]
        second["status"] = "done"
        second["evidence"] = [evidence("WF-02-A01", "pass")]
        validate_plan_data(plan, self.repo_root)

        second["evidence"][0]["commit"] = "a" * 7
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "EVIDENCE_COMMIT")
        second["evidence"][0]["commit"] = "a" * 40

        second["evidence"].append(evidence("WF-02-A01", "fail"))
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DONE_EVIDENCE_INCOMPLETE")

        second["evidence"].append(evidence("WF-02-A01", "unverified"))
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DONE_EVIDENCE_INCOMPLETE")

        second["evidence"].append(evidence("WF-02-A01", "pass"))
        validate_plan_data(plan, self.repo_root)
        first["status"] = "planned"
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DONE_DEPENDENCY_INCOMPLETE")

    def test_dependency_cycles_and_unknown_dependencies_are_rejected(self) -> None:
        plan = make_plan()
        plan["tasks"][0]["dependsOn"] = ["WF-02"]

        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DEPENDENCY_CYCLE")

        plan["tasks"][0]["dependsOn"] = ["WF-99"]
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DEPENDENCY_MISSING")

    def test_json_acceptance_deletion_and_detail_task_drift_are_rejected(self) -> None:
        plan = make_plan()
        plan["tasks"][0]["acceptanceIds"].pop()
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "ACCEPTANCE_REFERENCE_EXTRA")

        plan = make_plan()
        original_detail = self.detail_file.read_text(encoding="utf-8")
        self.detail_file.write_text(original_detail.replace("- WF-01-A02\n", ""), encoding="utf-8")
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "ACCEPTANCE_REFERENCE_MISSING")

        self.detail_file.write_text(original_detail + "\n\n## WF-04 Extra task\n\n- WF-04-A01\n", encoding="utf-8")
        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "DETAIL_SECTION_EXTRA")

    def test_repository_traversal_is_rejected(self) -> None:
        plan = make_plan()
        plan["tasks"][0]["existingCodePaths"] = ["../../.env.local"]

        with self.assertRaises(PlanValidationError) as raised:
            validate_plan_data(plan, self.repo_root)
        self.assertEqual(raised.exception.code, "PATH_UNSAFE")


if __name__ == "__main__":
    unittest.main()
