"""Workflow contract tests; pass native tool paths to include real FE acceptance."""
from __future__ import annotations

import argparse
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient, WorkflowError, WorkflowService
from range_workflow.process import ProcessFailure, ProcessRunner
from range_workflow.runtime import serve
from range_workflow.jobs import _native

ARGS = None


def request(service, operation, parameters=None):
    return service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                           "operation": operation, "parameters": parameters or {}})


def approved(service, command, parameters):
    preview = request(service, "command.preview", {"command": command, "parameters": parameters,
                      "expectedRevision": request(service, "project.query")["revision"]})
    approval = request(service, "host.approve", {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})
    return {"previewId": preview["previewId"], "approvalToken": approval["approvalToken"]}


def wait(service, jid, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        job = request(service, "jobs.query", {"jobId": jid})["job"]
        if job["state"] in {"succeeded", "failed", "cancelled", "interrupted"}:
            return job
        time.sleep(0.01)
    raise AssertionError("Job timed out")


class ContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.examples = self.base / "examples"
        self.examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (self.examples / name).write_text("fixture", encoding="utf-8")
        self.workspace = self.base / "workspace"
        self.service = self.open()

    def open(self):
        return WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), self.examples, Path(sys.executable))

    def tearDown(self):
        self.service.close()
        self.temporary.cleanup()

    def test_execution_receipt_lookup_survives_reopen_without_authority(self):
        preview = request(self.service, "command.preview", {"command": "workflow.configure",
            "parameters": {"dt": .005, "steps": 20, "targetDamping": .1},
            "expectedRevision": request(self.service, "project.query")["revision"]})
        lookup = {"requestSha256": preview["previewHash"]}
        self.assertFalse(request(self.service, "execution.query", lookup)["accepted"])
        approval = request(self.service, "host.approve",
            {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})
        with mock.patch.object(self.service, "_run_job", return_value=None):
            accepted = request(self.service, "command.execute",
                {"previewId": preview["previewId"], "approvalToken": approval["approvalToken"]})
        before = self.service.state_snapshot()
        receipt = request(self.service, "execution.query", lookup)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["jobId"], accepted["jobId"])
        self.assertEqual(self.service.state_snapshot(), before)
        self.assertNotIn("approvalToken", json.dumps(receipt))
        self.assertNotIn("previewId", json.dumps(receipt))
        self.assertNotIn(approval["approvalToken"], json.dumps(receipt))
        self.service.close()
        self.service = self.open()
        self.assertEqual(request(self.service, "execution.query", lookup)["jobId"], accepted["jobId"])
        self.assertFalse(request(self.service, "execution.query", {"requestSha256": "f" * 64})["accepted"])
        self.assertCode("INVALID_PARAMETERS", request, self.service, "execution.query",
                        {"requestSha256": preview["previewHash"], "approvalToken": approval["approvalToken"]})
    def assertCode(self, code, callable, *args):
        with self.assertRaises(WorkflowError) as captured:
            callable(*args)
        self.assertEqual(captured.exception.code, code)

    def test_protocol_exact_schema_duplicate_nonfinite_and_catalog(self):
        catalog = request(self.service, "capabilities.query")
        self.assertNotIn("host.approve", [x["id"] for x in catalog["operations"]])
        self.assertCode("INVALID_PARAMETERS", request, self.service, "project.query", {"unknown": 0})
        for value in (float("nan"), float("inf"), True, "0.005"):
            self.assertCode("INVALID_PARAMETERS", approved, self.service, "workflow.configure",
                            {"dt": value, "steps": 10, "targetDamping": .1})
        stream = io.BytesIO(b'{"apiVersion":"range.workflow/v1","apiVersion":"oops"}\n' +
                            b'{"apiVersion":"range.workflow/v1","requestId":"x","operation":"project.query","parameters":{"x":NaN}}\n')
        output = io.BytesIO()
        serve(self.service, stream, output)
        self.assertEqual([json.loads(x)["code"] for x in output.getvalue().splitlines()], ["INVALID_JSON", "INVALID_JSON"])

    def test_host_approval_idempotency_revision_and_reopen(self):
        params = {"dt": .005, "steps": 20, "targetDamping": .1}
        preview = request(self.service, "command.preview", {"command": "workflow.configure", "parameters": params,
                           "expectedRevision": self.service.project_snapshot()["revision"]})
        self.assertCode("APPROVAL_REQUIRED", request, self.service, "command.execute",
                        {"previewId": preview["previewId"], "approvalToken": "forged"})
        action = approved(self.service, "workflow.configure", params)
        launched = request(self.service, "command.execute", action)
        result = wait(self.service, launched["jobId"])
        self.assertEqual(result["state"], "succeeded")
        repeated = request(self.service, "command.execute", action)
        self.assertEqual(launched["jobId"], repeated["jobId"])
        self.assertEqual(len(request(self.service, "jobs.query")["jobs"]), 1)
        stale = approved(self.service, "workflow.configure", params)
        self.service.close()
        self.service = self.open()
        self.assertEqual(self.service.project_snapshot()["configuration"]["steps"], 20)
        self.assertCode("STALE_REVISION", request, self.service, "command.execute", stale)
        records = request(self.service, "audit.query", {"limit": 1000})["records"]
        self.assertTrue(records)
        required = {"schemaVersion", "recordId", "parentRecordId", "timestamp", "actorType", "actorId",
                    "sessionId", "projectId", "modelId", "modelRevision", "runId", "workflowStage",
                    "commandId", "commandVersion", "parameters", "selectionSnapshot", "agentProposal",
                    "approvalState", "preStateHash", "postStateHash", "resultStatus", "diagnostics",
                    "artifactRefs", "applicationVersion", "pluginVersions"}
        for item in records:
            self.assertEqual(set(item["record"]), required)
            self.assertRegex(item["record"]["modelRevision"], r"^r\d+$")
            self.assertNotIn("approvalToken", json.dumps(item["record"]))

    def test_lock_tamper_recovery_and_failed_atomic_commit(self):
        self.assertCode("WORKSPACE_BUSY", self.open)
        action = approved(self.service, "workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .1})
        (self.examples / "stage9_truss4.ram").write_text("changed", encoding="utf-8")
        self.assertCode("SOURCE_CHANGED", request, self.service, "command.execute", action)
        before = self.service.state_snapshot()
        with mock.patch("range_workflow.service.os.replace", side_effect=OSError("disk")):
            self.assertCode("PERSISTENCE_ERROR", request, self.service, "command.preview",
                            {"command": "workflow.configure", "parameters": {"dt": .005, "steps": 10, "targetDamping": .1},
                             "expectedRevision": before["revision"]})
        self.assertEqual(self.service.state_snapshot(), before)
        self.service.close()
        state_path = self.workspace / "workflow-state.json"
        disk = json.loads(state_path.read_bytes())
        jid = str(uuid.uuid4())
        disk["jobs"][jid] = {"jobId": jid, "state": "running", "command": "workflow.run"}
        state_path.write_text(json.dumps(disk), encoding="utf-8")
        self.service = self.open()
        self.assertEqual(request(self.service, "jobs.query", {"jobId": jid})["job"]["state"], "interrupted")
        self.assertCode("RESULT_UNAVAILABLE", request, self.service, "results.query", {"jobId": jid})

    def test_approval_is_ephemeral_and_v1_migration_expires_tokens(self):
        action = approved(self.service, "workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .1})
        state_path = self.workspace / "workflow-state.json"
        persisted = state_path.read_bytes()
        self.assertNotIn(action["approvalToken"].encode(), persisted)
        self.assertNotIn(b'"approvalToken"', persisted)
        preview = self.service.state_snapshot()["previews"][action["previewId"]]
        again = request(self.service, "host.approve", {"previewId": action["previewId"], "previewHash": preview["previewHash"]})
        self.assertEqual(again["approvalToken"], action["approvalToken"])
        self.service.close()
        legacy = json.loads(state_path.read_bytes())
        legacy["storageVersion"] = 1
        legacy.pop("plans")
        legacy["previews"][action["previewId"]]["approvalToken"] = action["approvalToken"]
        state_path.write_text(json.dumps(legacy), encoding="utf-8")
        self.service = self.open()
        self.assertEqual(json.loads(state_path.read_bytes())["storageVersion"], 3)
        self.assertEqual(json.loads(state_path.read_bytes())["plans"], {})
        self.assertNotIn(action["approvalToken"].encode(), state_path.read_bytes())
        self.assertCode("STALE_REVISION", request, self.service, "command.execute", action)
        self.assertCode("STALE_REVISION", request, self.service, "host.approve",
                        {"previewId": action["previewId"], "previewHash": preview["previewHash"]})
        fresh = approved(self.service, "workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .1})
        self.assertNotEqual(action["approvalToken"], fresh["approvalToken"])
        self.assertEqual(wait(self.service, request(self.service, "command.execute", fresh)["jobId"])["state"], "succeeded")

    def test_failed_approval_commit_does_not_grant_authorization(self):
        preview = request(self.service, "command.preview", {"command": "workflow.configure",
                          "parameters": {"dt": .005, "steps": 20, "targetDamping": .1},
                          "expectedRevision": self.service.project_snapshot()["revision"]})
        with mock.patch("range_workflow.service.os.replace", side_effect=OSError("disk")):
            self.assertCode("PERSISTENCE_ERROR", request, self.service, "host.approve",
                            {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})
        self.assertEqual(self.service._approval_tokens, {})
        self.assertNotIn("approvalState", self.service.state_snapshot()["previews"][preview["previewId"]])

    def test_async_cancel_failure_and_process_bounds(self):
        def execute(service, job, directory, cancel):
            service.runner.run([sys.executable, "-c", "import time; time.sleep(120)"], directory, "sleep", cancel)
            return {"unexpected": True}
        self.service.register_command("test.sleep", descriptor={}, normalize=lambda s, p: {}, execute=execute)
        launched = request(self.service, "command.execute", approved(self.service, "test.sleep", {}))
        self.assertTrue(request(self.service, "project.query")["busy"])
        request(self.service, "job.cancel", {"jobId": launched["jobId"]})
        self.assertEqual(wait(self.service, launched["jobId"])["state"], "cancelled")
        self.assertCode("RESULT_UNAVAILABLE", request, self.service, "results.query", {"jobId": launched["jobId"]})
        directory = self.base / "bounded"
        directory.mkdir()
        runner = ProcessRunner()
        with self.assertRaises(ProcessFailure) as caught:
            runner.run([sys.executable, "-c", "import time; time.sleep(120)"], directory, "timeout", threading.Event(), timeout=.1)
        self.assertEqual(caught.exception.code, "TIMEOUT")
        runner.MAX_LOG_BYTES = 1024
        with self.assertRaises(ProcessFailure) as caught:
            runner.run([sys.executable, "-c", "print('x'*100000)"], directory, "overflow", threading.Event())
        self.assertEqual(caught.exception.code, "OUTPUT_LIMIT")
        self.assertLessEqual((directory / "overflow.log").stat().st_size, 1024)

    def test_native_adapter_rejects_unversioned_or_malformed_success(self):
        responses = (
            {"status": "ok"},
            {"apiVersion": "range.workflow.native/v0", "status": "ok"},
            {"apiVersion": "range.workflow.native/v1", "status": "ok"},
        )
        for response in responses:
            with self.subTest(response=response), mock.patch.object(
                    self.service.runner, "run", return_value=json.dumps(response).encode("utf-8")):
                with self.assertRaises(WorkflowError) as caught:
                    _native(self.service, self.base, "fake-inspect", threading.Event(), ["inspect"])
                self.assertEqual(caught.exception.code, "INVALID_NATIVE_RESPONSE")

    def test_native_inspect_requires_consistent_persisted_controller_identity(self):
        source = self.base / "accepted.ram"
        base = {"apiVersion": "range.workflow.native/v1", "status": "ok",
                "sourceSha256": "a" * 64, "sourcePath": str(source),
                "sourceFiles": [{"path": str(source), "sha256": "a" * 64}],
                "checkpointAvailable": False, "step": 0, "plannedSteps": 1,
                "configuredDt": .005, "time": 0.0, "dt": .005,
                "q": [], "v": [], "a": [], "controlWork": [],
                "controlAvailable": False, "nodes": []}
        valid = ((False, ""), (True, "stage10-beam-velocity-feedback"))
        for enabled, controller_id in valid:
            response = {**base, "controlEnabled": enabled, "controllerId": controller_id}
            with self.subTest(enabled=enabled, controller_id=controller_id), mock.patch.object(
                    self.service.runner, "run", return_value=json.dumps(response).encode("utf-8")):
                self.assertEqual(_native(self.service, self.base, "inspect", threading.Event(), ["inspect"]),
                                 response)

        invalid = ({**base, "controlEnabled": True},
                   {**base, "controlEnabled": True, "controllerId": ""},
                   {**base, "controlEnabled": False, "controllerId": "stage9-velocity-feedback"},
                   {**base, "controlEnabled": True, "controllerId": "controller with space"})
        for response in invalid:
            with self.subTest(response=response), mock.patch.object(
                    self.service.runner, "run", return_value=json.dumps(response).encode("utf-8")):
                with self.assertRaises(WorkflowError) as caught:
                    _native(self.service, self.base, "inspect", threading.Event(), ["inspect"])
                self.assertEqual(caught.exception.code, "INVALID_NATIVE_RESPONSE")


class NativeIntegration(unittest.TestCase):
    def test_actual_fe_beam_configure_run_both_and_resume(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual FE acceptance")
        with tempfile.TemporaryDirectory(prefix="range-workflow-beam-") as temporary:
            workspace = Path(temporary)

            def client():
                return WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)

            def execute(sdk, command, params):
                preview = sdk.preview(command, params)
                approval = sdk.approve(preview)
                job = sdk.execute(preview, approval)
                completed = sdk.wait(job["jobId"], timeout=240)
                self.assertEqual(completed["state"], "succeeded", completed.get("error"))
                return completed

            with client() as sdk:
                execute(sdk, "workflow.configure", {"dt": .005, "steps": 200,
                        "targetDamping": .1, "model": "cantilever-beam"})
                original = execute(sdk, "workflow.run", {"mode": "both", "independentReference": True})
                result = original["result"]
                self.assertEqual(result["model"], "cantilever-beam")
                self.assertEqual([(d["nodePosition"], d["dof"][0], d["unit"], d["globalDirection"])
                                  for d in result["dofs"]],
                                 [(1, "U", "m", [0.0, 1.0, 0.0]),
                                  (1, "R", "rad", [0.0, 0.0, 1.0])])
                self.assertEqual(len(result["active"]["q"]), 2)
                self.assertEqual(len(result["passive"]["q"]), 2)
                self.assertLess(result["controlledToPassiveEnergyRatio"], 1.0)
                catalog = sdk.native_result(original["jobId"], "catalog", mode="active")
                self.assertTrue(catalog["valid"])
                self.assertEqual(catalog["control"]["controllerId"],
                                 "stage10-beam-velocity-feedback")
                original_id = original["jobId"]
                original_hashes = {item["path"]: item["sha256"] for item in original["artifacts"]}

            with client() as sdk:
                resumed = execute(sdk, "workflow.resume", {"jobId": original_id, "additionalSteps": 200})
                result = resumed["result"]
                self.assertEqual(result["totalSteps"], 400)
                self.assertEqual(result["configuration"]["model"], "cantilever-beam")
                self.assertLess(result["controlledToPassiveEnergyRatio"], 1.0)
                from range_workflow.service import file_hash
                for path, expected in original_hashes.items():
                    self.assertEqual(file_hash(Path(path)), expected, "Resume mutated an original artifact")
                for variant in ("active", "passive"):
                    self.assertLess(result[variant]["maxDisplacementErrorM"], 1e-8)
                    self.assertLess(result[variant]["maxVelocityErrorMPerSecond"], 1e-7)
                    self.assertLess(result[variant]["maxRotationErrorRad"], 1e-8)
                    self.assertLess(result[variant]["maxAngularVelocityErrorRadPerSecond"], 1e-7)

    def test_actual_fe_sdk_configure_both_resume_and_reopen(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual FE acceptance")
        with tempfile.TemporaryDirectory(prefix="range-workflow-native-") as temporary:
            workspace = Path(temporary)
            def client():
                return WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)
            def execute(sdk, command, params):
                preview = sdk.preview(command, params)
                approval = sdk.approve(preview)
                job = sdk.execute(preview, approval)
                self.assertEqual(sdk.execute(preview, approval)["jobId"], job["jobId"])
                completed = sdk.wait(job["jobId"], timeout=240)
                self.assertEqual(completed["state"], "succeeded", completed.get("error"))
                return completed
            with client() as sdk:
                execute(sdk, "workflow.configure", {"dt": .005, "steps": 200, "targetDamping": .1})
                original = execute(sdk, "workflow.run", {"mode": "both", "independentReference": True})
                result = original["result"]
                self.assertLess(result["controlledToPassiveEnergyRatio"], .2)
                for variant in ("active", "passive"):
                    self.assertLess(result[variant]["maxDisplacementErrorM"], 1e-8)
                    self.assertLess(result[variant]["maxVelocityErrorMPerSecond"], 1e-7)
                    self.assertEqual(len(result[variant]["q"]), 4)
                original_id = original["jobId"]
                hashes = {a["path"]: a["sha256"] for a in original["artifacts"]}
            with client() as sdk:
                resumed = execute(sdk, "workflow.resume", {"jobId": original_id, "additionalSteps": 200})
                result = resumed["result"]
                self.assertEqual(result["totalSteps"], 400)
                self.assertLess(result["controlledToPassiveEnergyRatio"], .05)
                from range_workflow.service import file_hash
                for path, expected in hashes.items():
                    self.assertEqual(file_hash(Path(path)), expected, "Resume mutated an original artifact")
                for variant in ("active", "passive"):
                    self.assertLess(result[variant]["maxDisplacementErrorM"], 1e-8)
                    self.assertLess(result[variant]["maxVelocityErrorMPerSecond"], 1e-7)
                for path in (Path(result["gainsPath"]), Path(result["controllerPath"]),
                             Path(result["controllerHostPath"]), Path(result["active"]["checkpointPath"])):
                    original_bytes = path.read_bytes()
                    try:
                        path.write_bytes(original_bytes + b"\nchanged")
                        with self.assertRaises(WorkflowError) as caught:
                            sdk.preview("workflow.resume", {"jobId": resumed["jobId"], "additionalSteps": 10})
                        self.assertEqual(caught.exception.code, "SOURCE_CHANGED")
                    finally:
                        path.write_bytes(original_bytes)
                with self.assertRaises(WorkflowError) as caught:
                    sdk.preview("workflow.resume", {"jobId": resumed["jobId"], "additionalSteps": 5000})
                self.assertEqual(caught.exception.code, "STEP_LIMIT")
                preview = sdk.preview("workflow.run", {"mode": "both", "independentReference": True})
                job = sdk.execute(preview, sdk.approve(preview))
                sdk.request("job.cancel", {"jobId": job["jobId"]})
                self.assertEqual(sdk.wait(job["jobId"])["state"], "cancelled")
                with self.assertRaises(WorkflowError):
                    sdk.request("results.query", {"jobId": job["jobId"]})

    def test_abrupt_service_exit_recovers_native_job_as_interrupted(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Native acceptance paths were not supplied")
        with tempfile.TemporaryDirectory(prefix="range-workflow-crash-") as temporary:
            workspace = Path(temporary)
            sdk = WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)
            try:
                preview = sdk.preview("workflow.configure", {"dt": .005, "steps": 5000, "targetDamping": .1})
                configured = sdk.wait(sdk.execute(preview, sdk.approve(preview))["jobId"])
                self.assertEqual(configured["state"], "succeeded")
                preview = sdk.preview("workflow.run", {"mode": "both", "independentReference": True})
                jid = sdk.execute(preview, sdk.approve(preview))["jobId"]
                deadline = time.monotonic() + 40
                while not (workspace / "jobs" / jid / "solve-active.log").exists():
                    self.assertLess(time.monotonic(), deadline, "Native solve never started")
                    state = sdk.request("jobs.query", {"jobId": jid})["job"]
                    self.assertNotIn(state["state"], ("failed", "succeeded"), state)
                    time.sleep(.025)
                sdk.process.kill()
                sdk.process.wait(timeout=5)
            finally:
                sdk.close()
            with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
                job = reopened.request("jobs.query", {"jobId": jid})["job"]
                self.assertEqual(job["state"], "interrupted")
                with self.assertRaises(WorkflowError):
                    reopened.request("results.query", {"jobId": jid})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    ARGS, remaining = parser.parse_known_args()
    if ARGS.solver and not all((ARGS.native, ARGS.examples)):
        parser.error("--solver requires --native and --examples")
    unittest.main(argv=[sys.argv[0], *remaining])
