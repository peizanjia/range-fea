"""WF08 host contract tests; mocked numerical data is NEVER FE evidence.

Real two-point acceptance is opt-in with all native tool arguments. Ordinary
execution runs pure service contracts only, with no subprocess solver launch.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "examples"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow import WorkflowClient, WorkflowError, WorkflowService
from range_workflow.task_runtime import TaskManager
from parameter_scan import run_scan, scan_plan

ARGS = None
REAL_MANAGER_START = TaskManager.start


def request(service, operation, parameters=None):
    return service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                           "operation": operation, "parameters": parameters or {}})


class TaskRuntimeContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-task-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.examples = self.base / "examples"
        self.examples.mkdir()
        self.source = {"sourceId": "contract-source", "descriptor": {"modelKind": "four-rod"},
                       "originRef": {}, "registeredModelSha256": "a" * 64, "manifestSha256": "b" * 64}
        self.files = {"modelInput": {"sha256": "a" * 64}, "sourceManifest": {"sha256": "b" * 64},
                      "solver": {"sha256": "c" * 64}}
        for patcher in (
            mock.patch("range_workflow.model_sources.read_model_source", return_value=self.source),
            mock.patch.object(WorkflowService, "_binding", side_effect=lambda *args: copy.deepcopy(self.files)),
            mock.patch.object(WorkflowService, "_run_job", return_value=None),
            mock.patch.object(TaskManager, "start", return_value=None),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.service = self.open()
        self.addCleanup(lambda: self.service.close())
        with self.service._lock:
            candidate = self.service.state_snapshot()
            candidate["configuration"].update(sourceId=self.source["sourceId"], steps=20)
            self.service._commit(candidate, "contract.fixture", "configured")

    def open(self):
        return WorkflowService(self.base / "workspace", Path(sys.executable), Path(sys.executable),
                               self.examples, Path(sys.executable))

    def stage(self, plan=None):
        self.plan = request(self.service, "task.plan.propose", {"plan": plan or scan_plan(points=(.05,))})
        self.controls = {"planId": self.plan["planId"], "expectedPlanSha256": self.plan["planSha256"]}
        return self.plan

    def query(self):
        return request(self.service, "task.plan.query", {"planId": self.plan["planId"]})

    def pump(self, count=1):
        for _ in range(count):
            self.service._plans.pump()
        return self.query()

    def start_plan(self):
        self.stage()
        request(self.service, "task.plan.start", self.controls)
        return self.pump()

    def approve(self, node):
        return request(self.service, "host.task.node.approve", {**self.controls,
            "nodeId": node["id"], "previewHash": node["previewHash"]})

    def submitted(self):
        observed = self.start_plan()
        self.approve(observed["nodes"][0])
        observed = self.pump()
        return observed["nodes"][0]["jobId"]

    def complete(self, job_id, *, error=0.0):
        # Explicit host-result fixture, not native execution or numeric proof.
        with self.service._lock:
            candidate = self.service.state_snapshot()
            job = candidate["jobs"][job_id]
            job["state"] = "succeeded"
            if job["command"] == "workflow.configure":
                configuration = candidate["previews"][job["previewId"]]["effectiveConfiguration"]
                candidate["configuration"] = copy.deepcopy(configuration)
                job["result"] = {"configuration": copy.deepcopy(configuration)}
            else:
                values = {"acceptedSeq": job["configuration"]["steps"],
                          "maxDisplacementErrorM": error, "maxVelocityErrorMPerSecond": 0.0,
                          "maxRotationErrorRad": 0.0, "maxAngularVelocityErrorRadPerSecond": 0.0}
                job["result"] = {"active": copy.deepcopy(values), "passive": copy.deepcopy(values)}
            self.service._commit(candidate, "contract.fixture", "succeeded")

    def run_submitted(self):
        configure = self.submitted()
        self.complete(configure)
        observed = self.pump(2)
        self.approve(observed["nodes"][1])
        observed = self.pump()
        return observed["nodes"][1]["jobId"]

    def assert_code(self, code, operation, parameters):
        with self.assertRaises(WorkflowError) as caught:
            request(self.service, operation, parameters)
        self.assertEqual(caught.exception.code, code)

    def assert_corrupt_persisted_plan(self, baseline, plan_id, mutate):
        path = self.base / "workspace" / "workflow-state.json"
        persisted = json.loads(baseline)
        mutate(persisted["plans"][plan_id], persisted)
        corrupted = json.dumps(persisted, separators=(",", ":")).encode("utf-8")
        path.write_bytes(corrupted)
        with self.assertRaises(WorkflowError) as caught:
            self.open()
        self.assertEqual(caught.exception.code, "CORRUPT_PLAN")
        self.assertEqual(path.read_bytes(), corrupted)

    def test_nonobject_control_node_input_refs_are_invalid_plan(self):
        for kind, malformed in (("condition", []), ("condition", None),
                                ("clarification", []), ("clarification", None)):
            plan = scan_plan(points=(.05,))
            if kind == "condition":
                plan["nodes"][2]["inputRefs"] = malformed
            else:
                plan["nodes"].append({"id": "clarify0", "kind": "clarification",
                    "dependsOn": ["check0"], "question": "Which case?", "inputRefs": malformed})
            with self.subTest(kind=kind, inputRefs=malformed):
                self.assert_code("INVALID_PLAN", "task.plan.propose", {"plan": plan})
                self.assertEqual(self.service._state["plans"], {})
                self.assertEqual(self.service._state["jobs"], {})

    def test_corrupt_persisted_kernel_state_is_rejected_without_rewriting_bytes(self):
        self.stage()
        self.service.close()
        path = self.base / "workspace" / "workflow-state.json"
        baseline = path.read_bytes()
        plan_id = self.plan["planId"]

        def completed_with_pending(record, persisted):
            record["kernel"]["state"] = "completed"

        def boolean_count(record, persisted):
            record["kernel"]["counts"]["toolCalls"] = True

        def list_count(record, persisted):
            record["kernel"]["counts"] = []

        def invalid_node_map(record, persisted):
            record["kernel"]["nodes"]["configure0"] = []

        def invalid_outbox(record, persisted):
            record["kernel"]["intents"] = [{"nodeId": "missing"}]

        def invalid_reference(record, persisted):
            record["kernel"]["nodes"]["configure0"]["previewHash"] = "not-a-hash"

        def invalid_configuration(record, persisted):
            record["configuration"]["steps"] = True

        cases = (("completed plan with pending nodes", completed_with_pending),
                 ("boolean plan count", boolean_count), ("list plan counts", list_count),
                 ("invalid node map", invalid_node_map), ("invalid outbox", invalid_outbox),
                 ("invalid node reference", invalid_reference),
                 ("invalid source configuration", invalid_configuration))
        for label, mutate in cases:
            with self.subTest(case=label):
                self.assert_corrupt_persisted_plan(baseline, plan_id, mutate)

        path.write_bytes(baseline)
        self.service = self.open()

    def test_corrupt_persisted_job_associations_are_rejected_without_rewriting_bytes(self):
        job_id = self.submitted()
        self.service.close()
        path = self.base / "workspace" / "workflow-state.json"
        baseline = path.read_bytes()
        plan_id = self.plan["planId"]

        def job_and_record(persisted):
            record = persisted["plans"][plan_id]
            return record, persisted["jobs"][job_id], record["kernel"]["nodes"]["configure0"]

        cases = (
            ("list plan reference", lambda persisted: job_and_record(persisted)[1].update(taskPlanRef=[])),
            ("null plan reference", lambda persisted: job_and_record(persisted)[1].update(taskPlanRef=None)),
            ("string plan reference", lambda persisted: job_and_record(persisted)[1].update(taskPlanRef="not-an-object")),
            ("plan reference", lambda persisted: job_and_record(persisted)[1]["taskPlanRef"].update(planId="other-plan")),
            ("node reference", lambda persisted: job_and_record(persisted)[1]["taskPlanRef"].update(nodeId="run0")),
            ("project association", lambda persisted: job_and_record(persisted)[1].update(projectId="other-project")),
            ("command association", lambda persisted: job_and_record(persisted)[1].update(command="workflow.run")),
            ("missing accepted job", lambda persisted: job_and_record(persisted)[2].update(jobId="missing-job")),
            ("completed node without succeeded job", lambda persisted: (
                job_and_record(persisted)[2].update(state="completed"),
                job_and_record(persisted)[1].update(state="failed"))),
        )
        for label, mutate in cases:
            with self.subTest(case=label):
                self.assert_corrupt_persisted_plan(baseline, plan_id,
                    lambda record, persisted, mutate=mutate: mutate(persisted))

        path.write_bytes(baseline)
        self.service = self.open()

    def test_expired_persisted_outbox_reopens_as_failed_without_dispatch(self):
        from range_workflow.task_scheduler import reduce_event
        plan = scan_plan(points=(.05,))
        plan["budgets"] = {"deadlineSeconds": 1}
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        with self.service._lock:
            candidate = self.service.state_snapshot()
            record = candidate["plans"][self.plan["planId"]]
            state = record["kernel"]
            state = reduce_event(state, {"type": "tick"}, state["lastAt"])
            self.assertTrue(state["intents"])
            record["kernel"] = reduce_event(state, {"type": "tick"}, state["lastAt"] + 2)
            reopened_at = record["kernel"]["lastAt"] + 1
            self.assertEqual(record["kernel"]["state"], "failed")
            self.service._commit(candidate, "contract.expired", "failed")
        self.service.close()
        with mock.patch("range_workflow.task_runtime.time.time", return_value=reopened_at):
            self.service = self.open()
            self.assertEqual(self.query()["state"], "failed")
            self.pump()
        self.assertEqual(self.service._state["jobs"], {})

    def test_requires_registered_source_and_limits_fixed_points(self):
        del self.service._state["configuration"]["sourceId"]
        self.assert_code("PLAN_SOURCE_REQUIRED", "task.plan.propose", {"plan": scan_plan()})
        self.service._state["configuration"]["sourceId"] = "contract-source"
        plan = scan_plan(points=(.05,) * 4)
        extra = copy.deepcopy(plan["nodes"][:3])
        for node in extra:
            node["id"] = node["id"].replace("0", "4")
            node["dependsOn"] = [dep.replace("0", "4") for dep in node["dependsOn"]]
        extra[0]["dependsOn"] = ["check3"]
        extra[2]["condition"]["ref"]["node"] = "run4"
        plan["nodes"].extend(extra)
        self.assert_code("SCAN_LIMIT", "task.plan.propose", {"plan": plan})

    def test_identical_source_alias_is_rejected_before_plan_or_job_creation(self):
        alias = "contract-source-alias"
        plan = scan_plan(points=(.05,))
        plan["nodes"][0]["parameters"]["sourceId"] = alias

        def read_alias(workspace, source_id):
            source = copy.deepcopy(self.source)
            source["sourceId"] = source_id
            return source

        with mock.patch("range_workflow.model_sources.read_model_source", side_effect=read_alias):
            self.assert_code("SOURCE_CHANGED", "task.plan.propose", {"plan": plan})

        self.assertEqual(self.service._state["configuration"]["sourceId"], self.source["sourceId"])
        self.assertEqual(self.service._state["plans"], {})
        self.assertEqual(self.service._state["jobs"], {})

    def test_user_approval_required_and_public_output_is_bounded(self):
        observed = self.start_plan()
        self.pump(10)
        self.assertEqual(len(self.service._state["jobs"]), 0)
        self.assertEqual(self.query()["counts"], {"modelCalls": 0, "toolCalls": 1})
        self.assertNotIn("approvalToken", json.dumps(observed))
        self.assertNotIn("host.task.node.approve", [op["id"] for op in self.service.capabilities()["operations"]])
        self.assert_code("STALE_PLAN", "task.plan.pause", {**self.controls, "expectedPlanSha256": "f" * 64})

    def test_atomic_accepted_job_and_node_receipt(self):
        job_id = self.submitted()
        disk = json.loads((self.base / "workspace" / "workflow-state.json").read_bytes())
        self.assertEqual(disk["storageVersion"], 3)
        self.assertEqual(disk["plans"][self.plan["planId"]]["kernel"]["nodes"]["configure0"]["jobId"], job_id)
        self.assertEqual(disk["jobs"][job_id]["taskPlanRef"]["nodeId"], "configure0")
        self.assertEqual(disk["jobs"][job_id]["taskPlanRef"]["planId"], self.plan["planId"])

    def test_reply_failure_after_acceptance_never_duplicates(self):
        observed = self.start_plan()
        self.approve(observed["nodes"][0])
        original = self.service._dispatch
        def lose_reply(operation, parameters):
            result = original(operation, parameters)
            if operation == "command.execute":
                raise WorkflowError("REPLY_LOST", "Contract fixture after accepted commit")
            return result
        with mock.patch.object(self.service, "_dispatch", side_effect=lose_reply):
            self.pump()
        job_id = self.query()["nodes"][0]["jobId"]
        self.pump(20)
        self.assertEqual(list(self.service._state["jobs"]), [job_id])

    def test_unknown_unobserved_is_not_resubmitted(self):
        observed = self.start_plan()
        self.approve(observed["nodes"][0])
        original = self.service._dispatch
        def unknown(operation, parameters):
            if operation == "command.execute":
                raise WorkflowError("REPLY_LOST", "No acceptance fixture")
            return original(operation, parameters)
        with mock.patch.object(self.service, "_dispatch", side_effect=unknown):
            self.pump(10)
        self.assertEqual(self.query()["nodes"][0]["state"], "outcome_unknown")
        self.assertEqual(self.service._state["jobs"], {})

    def test_environment_drift_and_old_approval_fail_closed(self):
        observed = self.start_plan()
        self.files["solver"]["sha256"] = "d" * 64
        self.assert_code("SOURCE_CHANGED", "host.task.node.approve", {**self.controls,
            "nodeId": "configure0", "previewHash": observed["nodes"][0]["previewHash"]})
        # Resume forces fresh preview rather than reusing the old authorization.
        request(self.service, "task.plan.pause", self.controls)
        request(self.service, "task.plan.resume", self.controls)
        self.assertEqual(self.pump()["state"], "conflict")

    def test_input_conflict_prevents_run_preview(self):
        job_id = self.submitted()
        self.complete(job_id)
        self.pump()
        self.service._state["configuration"]["steps"] = 21
        self.assertEqual(self.pump()["state"], "conflict")
        self.assertEqual(len(self.service._state["jobs"]), 1)

    def test_reopen_discards_unsubmitted_approval(self):
        observed = self.start_plan()
        self.approve(observed["nodes"][0])
        self.service.close()
        self.service = self.open()
        self.assertEqual(self.service._approval_tokens, {})
        self.assertEqual(self.query()["nodes"][0]["state"], "pending")
        self.pump()
        self.assertEqual(len(self.service._state["jobs"]), 0)

    def test_old_preview_cannot_approve_after_resume(self):
        observed = self.start_plan()
        old = observed["nodes"][0]["previewHash"]
        request(self.service, "task.plan.pause", self.controls)
        request(self.service, "task.plan.resume", self.controls)
        fresh = self.pump()["nodes"][0]
        self.assertNotEqual(fresh["previewHash"], old)
        self.assert_code("STALE_PLAN", "host.task.node.approve", {**self.controls,
            "nodeId": "configure0", "previewHash": old})

    def test_reopen_accepted_job_preserves_receipt_without_resubmission(self):
        job_id = self.submitted()
        self.service.close()
        self.service = self.open()
        self.pump(5)
        self.assertEqual(list(self.service._state["jobs"]), [job_id])
        self.assertEqual(self.query()["nodes"][0]["jobId"], job_id)
        self.assertIn(self.query()["state"], {"failed", "conflict"})

    def test_pause_reopen_and_cancel_late_job_do_not_dispatch(self):
        job_id = self.submitted()
        request(self.service, "task.plan.pause", self.controls)
        self.service.close()
        self.service = self.open()
        self.assertEqual(self.query()["state"], "paused")
        self.complete(job_id)
        self.pump(5)
        self.assertEqual(len(self.service._state["jobs"]), 1)
        request(self.service, "task.plan.cancel", self.controls)
        self.pump(5)
        self.assertEqual(self.query()["state"], "cancelled")
        self.assertEqual(self.query()["nodes"][0]["jobId"], job_id)

    def test_waiting_consumes_no_calls_until_job_state_changes(self):
        job_id = self.submitted()
        self.pump(2)
        before = self.query()
        self.pump(50)
        self.assertEqual(self.query()["counts"], before["counts"])
        self.assertEqual(self.query()["counts"]["modelCalls"], 0)
        self.complete(job_id)
        self.assertEqual(self.pump()["nodes"][0]["state"], "completed")

    def test_host_numeric_predicate_and_native_kind_are_required(self):
        job_id = self.run_submitted()
        record = self.service._state["plans"][self.plan["planId"]]
        node = record["kernel"]["plan"]["nodes"][1]
        for value in (True, float("nan"), float("inf"), -1, 1e-6):
            job = copy.deepcopy(self.service._job(job_id))
            job["state"] = "succeeded"
            job["result"] = {variant: {"acceptedSeq": 20, "maxDisplacementErrorM": value}
                             for variant in ("active", "passive")}
            with self.subTest(value=value), self.assertRaises(WorkflowError):
                self.service._plans._verified_outputs(record, node, job)
        self.complete(job_id)
        with mock.patch("range_workflow.native_results.query_registered", return_value={"sourceKind": "reference"}):
            self.assertEqual(self.pump()["state"], "failed")

    def test_wrong_job_identity_configuration_and_incomplete_trajectory_fail(self):
        job_id = self.run_submitted()
        self.complete(job_id)
        record = self.service._state["plans"][self.plan["planId"]]
        node = record["kernel"]["plan"]["nodes"][1]
        for field, value in (("command", "workflow.configure"), ("projectId", str(uuid.uuid4())),
                             ("state", "failed")):
            job = copy.deepcopy(self.service._job(job_id))
            job[field] = value
            with self.subTest(field=field), self.assertRaises(WorkflowError):
                self.service._plans._verified_outputs(record, node, job)
        for modify in (lambda job: job["configuration"].update(targetDamping=.2),
                       lambda job: job["result"]["active"].update(acceptedSeq=19)):
            job = copy.deepcopy(self.service._job(job_id))
            modify(job)
            with self.assertRaises(WorkflowError):
                self.service._plans._verified_outputs(record, node, job)

    def test_plan_queries_return_only_bounded_job_identity(self):
        plan = scan_plan(points=(.05,))
        for tool, name in (("jobs.query", "status"), ("results.query", "results")):
            plan["nodes"].append({"id": name, "kind": "query", "tool": tool,
                "dependsOn": ["check0"], "inputRefs": {"jobId": {"node": "run0", "output": "jobId"}},
                "successPredicate": "query.identity"})
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        self.approve(self.pump()["nodes"][0])
        configured = self.pump()["nodes"][0]["jobId"]
        self.complete(configured)
        self.approve(self.pump(2)["nodes"][1])
        job_id = self.pump()["nodes"][1]["jobId"]
        self.complete(job_id)
        with mock.patch("range_workflow.native_results.query_registered", side_effect=lambda service, kind, p:
                        {"sourceKind": "native-" + p["mode"] + "-fe"}):
            observed = self.pump(5)
        self.assertEqual(observed["state"], "completed")
        for node in observed["nodes"][-2:]:
            self.assertEqual(node["outputs"], {"jobId": job_id})

    def test_failed_independent_query_does_not_block_valid_registered_job_query(self):
        missing_job_id = str(uuid.uuid4())
        valid_job_id = str(uuid.uuid4())
        with self.service._lock:
            candidate = self.service.state_snapshot()
            candidate["jobs"][valid_job_id] = {"jobId": valid_job_id, "state": "succeeded",
                                                  "projectId": candidate["projectId"]}
            self.service._commit(candidate, "contract.fixture", "registered-job")

        plan = {"schema": "range.task-plan/v1", "title": "Independent job identity checks", "nodes": [
            {"id": "missing", "kind": "query", "tool": "jobs.query",
             "parameters": {"jobId": missing_job_id}, "successPredicate": "query.identity",
             "failurePolicy": "continue-independent"},
            {"id": "present", "kind": "query", "tool": "jobs.query",
             "parameters": {"jobId": valid_job_id}, "successPredicate": "query.identity"}]}
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        observed = self.pump()
        nodes = {node["id"]: node for node in observed["nodes"]}

        self.assertEqual(nodes["missing"]["state"], "failed")
        self.assertEqual(nodes["missing"]["failure"], "JOB_NOT_FOUND")
        self.assertEqual(nodes["present"]["state"], "completed")
        self.assertEqual(nodes["present"]["outputs"], {"jobId": valid_job_id})
        self.assertEqual(observed["state"], "failed")

    def test_stale_query_outbox_is_skipped_after_sibling_budget_expiry(self):
        job_ids = {name: str(uuid.uuid4()) for name in ("q1", "q2", "q3")}
        with self.service._lock:
            candidate = self.service.state_snapshot()
            for job_id in job_ids.values():
                candidate["jobs"][job_id] = {"jobId": job_id, "state": "succeeded",
                                             "projectId": candidate["projectId"]}
            self.service._commit(candidate, "contract.fixture", "registered-query-jobs")

        plan = {"schema": "range.task-plan/v1", "title": "Expired sibling query intent",
                "budgets": {"toolCalls": 3}, "nodes": [
            {"id": "q1", "kind": "query", "tool": "jobs.query",
             "parameters": {"jobId": job_ids["q1"]}, "successPredicate": "query.identity"},
            {"id": "q2", "kind": "query", "tool": "jobs.query",
             "parameters": {"jobId": job_ids["q2"]}, "successPredicate": "query.identity",
             "failurePolicy": "continue-independent", "budgets": {"activeSeconds": 1}},
            {"id": "q3", "kind": "query", "tool": "jobs.query",
             "parameters": {"jobId": job_ids["q3"]}, "successPredicate": "query.identity"}]}

        class FakeClock:
            def __init__(self, now):
                self.now = now

            def time(self):
                return self.now

            def advance(self, seconds):
                self.now += seconds

        clock = FakeClock(time.time())
        dispatched_job_ids = []
        original_dispatch = self.service._dispatch

        def query_dispatch(operation, parameters):
            result = original_dispatch(operation, parameters)
            if operation == "jobs.query":
                dispatched_job_ids.append(parameters["jobId"])
                if parameters["jobId"] == job_ids["q1"]:
                    clock.advance(2)
            return result

        with mock.patch("range_workflow.task_runtime.time.time", side_effect=clock.time), \
                mock.patch.object(self.service, "_dispatch", side_effect=query_dispatch):
            self.stage(plan)
            request(self.service, "task.plan.start", self.controls)
            first = self.pump()
            first_nodes = {node["id"]: node for node in first["nodes"]}

            self.assertEqual(first["state"], "active")
            self.assertEqual(first_nodes["q1"]["state"], "completed")
            self.assertEqual(first_nodes["q2"]["state"], "failed")
            self.assertEqual(first_nodes["q2"]["failure"], "active time budget exhausted")
            self.assertEqual(first_nodes["q3"]["state"], "pending")
            self.assertNotIn(job_ids["q2"], dispatched_job_ids)
            self.assertEqual(dispatched_job_ids, [job_ids["q1"]])

            final = self.pump()
            final_nodes = {node["id"]: node for node in final["nodes"]}

        self.assertEqual(final["state"], "failed")
        self.assertNotEqual(final["state"], "conflict")
        self.assertEqual(final_nodes["q1"]["state"], "completed")
        self.assertEqual(final_nodes["q2"]["state"], "failed")
        self.assertEqual(final_nodes["q3"]["state"], "completed")
        self.assertEqual(dispatched_job_ids, [job_ids["q1"], job_ids["q3"]])
        self.assertEqual(final["counts"]["modelCalls"], 0)
        self.assertEqual(final["counts"]["toolCalls"], 3)
        self.assertLessEqual(final["counts"]["toolCalls"], plan["budgets"]["toolCalls"])

    def test_mocked_success(self):
        job_id = self.run_submitted()
        self.complete(job_id, error=1e-9)
        def native_fixture(service, kind, parameters):
            return {"sourceKind": "native-" + parameters["mode"] + "-fe"}
        with mock.patch("range_workflow.native_results.query_registered", side_effect=native_fixture):
            observed = self.pump(3)
        self.assertEqual(observed["state"], "completed")
        self.assertEqual(observed["nodes"][1]["outputs"]["maxDisplacementErrorM"], 1e-9)
        self.assertEqual(observed["counts"]["modelCalls"], 0)
        self.assertLessEqual(observed["counts"]["toolCalls"], 16)

    def test_false_host_condition_prevents_next_point(self):
        plan = scan_plan()
        plan["nodes"][2]["condition"]["value"] = 1e-12
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        self.approve(self.pump()["nodes"][0])
        configured = self.pump()["nodes"][0]["jobId"]
        self.complete(configured)
        self.approve(self.pump(2)["nodes"][1])
        run_job = self.pump()["nodes"][1]["jobId"]
        self.complete(run_job, error=1e-9)
        with mock.patch("range_workflow.native_results.query_registered", side_effect=lambda service, kind, p:
                        {"sourceKind": "native-" + p["mode"] + "-fe"}):
            observed = self.pump(5)
        self.assertEqual(observed["state"], "failed")
        self.assertEqual(observed["nodes"][2]["state"], "failed")
        self.assertEqual(len(self.service._state["jobs"]), 2)

    def test_false_stop_condition_does_not_turn_sibling_success_into_conflict(self):
        plan = scan_plan(points=(.05,))
        plan["nodes"][2]["condition"]["value"] = 1e-12
        plan["nodes"].append({"id": "check1", "kind": "condition", "dependsOn": ["run0"],
            "condition": {"ref": {"node": "run0", "output": "maxDisplacementErrorM"},
                          "operator": "le", "value": 1e-8, "unit": "m"}})
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        self.approve(self.pump()["nodes"][0])
        configure_job = self.pump()["nodes"][0]["jobId"]
        self.complete(configure_job)
        self.approve(self.pump(2)["nodes"][1])
        run_job = self.pump()["nodes"][1]["jobId"]
        self.complete(run_job, error=1e-9)

        with mock.patch("range_workflow.native_results.query_registered",
                        side_effect=lambda service, kind, parameters:
                            {"sourceKind": "native-" + parameters["mode"] + "-fe"}):
            self.pump()  # Observe the successful mocked run.
            observed = self.pump()  # Resolve both sibling conditions in order.

        nodes = {node["id"]: node for node in observed["nodes"]}
        self.assertEqual(observed["state"], "failed")
        self.assertEqual(nodes["check0"]["state"], "failed")
        self.assertEqual(nodes["check1"]["state"], "waiting_condition")

    def test_background_driver_observes_without_explicit_pump(self):
        self.stage()
        REAL_MANAGER_START(self.service._plans)
        request(self.service, "task.plan.start", self.controls)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and self.query()["nodes"][0]["state"] != "awaiting_approval":
            time.sleep(.01)
        self.approve(self.query()["nodes"][0])
        while time.monotonic() < deadline and "jobId" not in self.query()["nodes"][0]:
            time.sleep(.01)
        self.assertIn("jobId", self.query()["nodes"][0])
        self.assertEqual(self.query()["counts"]["modelCalls"], 0)

    def test_example_requires_user_callback_and_rejection_pauses(self):
        class SDK:
            def request(inner, operation, parameters):
                return request(self.service, operation, parameters)
        with self.assertRaises(TypeError):
            run_scan(SDK(), None)
        REAL_MANAGER_START(self.service._plans)
        observed = run_scan(SDK(), lambda plan, node: False, timeout=3)
        self.assertEqual(observed["state"], "paused")
        self.assertEqual(self.service._state["jobs"], {})

    def test_budget_stops_submission(self):
        plan = scan_plan(points=(.05,))
        plan["budgets"]["toolCalls"] = 1
        self.stage(plan)
        request(self.service, "task.plan.start", self.controls)
        self.approve(self.pump()["nodes"][0])
        self.assertEqual(self.pump()["state"], "failed")
        self.assertEqual(self.service._state["jobs"], {})

    def test_v1_v2_storage_migration(self):
        self.service.close()
        path = self.base / "workspace" / "workflow-state.json"
        for version in (1, 2):
            disk = json.loads(path.read_bytes())
            disk["storageVersion"] = version
            disk.pop("plans")
            path.write_text(json.dumps(disk), encoding="utf-8")
            self.service = self.open()
            self.assertEqual(self.service.state_snapshot()["storageVersion"], 3)
            self.assertEqual(request(self.service, "task.plan.query"), {"plans": []})
            self.service.close()


class RealTaskScanAcceptance(unittest.TestCase):
    def test_registered_native_two_point_scan(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Real FE acceptance requires --solver --native --examples --python")
        from test_imported_workflow import _make_catalog
        from range_workflow.model_sources import register_snapshot_source
        with tempfile.TemporaryDirectory(prefix="range-task-real-") as temporary:
            base = Path(temporary)
            snapshot = base / "source.rbm"
            converted = subprocess.run([str(ARGS.native), "prepare", "--input",
                str(ARGS.examples / "stage9_truss4.ram"), "--output", str(snapshot),
                "--steps", "20", "--output-frequency", "1", "--dt", "0.005"],
                capture_output=True, timeout=60, check=False)
            self.assertEqual(converted.returncode, 0, converted.stderr.decode(errors="replace"))
            _, snapshot_ref = _make_catalog(base / "catalog", snapshot, hashlib.sha256(snapshot.read_bytes()).hexdigest())
            source = register_snapshot_source(base / "workspace", base / "catalog", snapshot_ref, ARGS.native)
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                baseline = sdk.preview("workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .05,
                    "sourceId": source["sourceId"]})
                accepted = sdk.execute(baseline, sdk.approve(baseline))
                self.assertEqual(sdk.wait(accepted["jobId"], timeout=60)["state"], "succeeded")
                approvals = []
                def explicit_test_host(plan, node):
                    approvals.append(node["id"])
                    self.assertIn(node["tool"], {"workflow.configure", "workflow.run"})
                    return True  # Explicit opt-in test-host authority only.
                observed = run_scan(sdk, explicit_test_host)
                self.assertEqual(observed["state"], "completed", observed)
                self.assertEqual(approvals, ["configure0", "run0", "configure1", "run1"])
                self.assertEqual(observed["counts"]["modelCalls"], 0)
                self.assertLessEqual(observed["counts"]["toolCalls"], 32)
                for index, damping in enumerate((.05, .10)):
                    node = next(n for n in observed["nodes"] if n["id"] == f"run{index}")
                    job = sdk.request("jobs.query", {"jobId": node["jobId"]})["job"]
                    self.assertEqual(job["configuration"]["targetDamping"], damping)
                    self.assertEqual(job["configuration"]["sourceId"], source["sourceId"])
                    self.assertEqual(job["taskPlanRef"]["planId"], observed["planId"])
                    self.assertEqual(job["taskPlanRef"]["nodeId"], node["id"])
                    for variant in ("active", "passive"):
                        catalog = sdk.native_result(job["jobId"], "catalog", mode=variant, limit=1)
                        self.assertEqual(catalog["sourceKind"], f"native-{variant}-fe")
                        self.assertEqual(catalog["sourceSnapshotRef"]["modelSha256"], source["registeredModelSha256"])
                        for field, bound in (("maxDisplacementErrorM", 1e-8), ("maxVelocityErrorMPerSecond", 1e-7),
                            ("maxRotationErrorRad", 1e-8), ("maxAngularVelocityErrorRadPerSecond", 1e-7)):
                            value = job["result"][variant][field]
                            self.assertTrue(type(value) in (int, float) and math.isfinite(value))
                            self.assertGreaterEqual(value, 0)
                            self.assertLessEqual(value, bound)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for argument in ("solver", "native", "examples", "python"):
        parser.add_argument("--" + argument, type=Path)
    ARGS, remaining = parser.parse_known_args()
    if ARGS.solver and not all((ARGS.native, ARGS.examples, ARGS.python)):
        parser.error("--solver requires --native --examples --python")
    unittest.main(argv=[sys.argv[0], *remaining])
