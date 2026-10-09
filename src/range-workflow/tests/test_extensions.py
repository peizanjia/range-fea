"""Contract tests for immutable workflow extensions and trusted child execution."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.service import WorkflowError, WorkflowService
from range_workflow.extensions import runtime_identity, _strict_json


def request(service, operation, parameters=None):
    return service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                          "operation": operation, "parameters": parameters or {}})


def wait(service, job_id, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        job = request(service, "jobs.query", {"jobId": job_id})["job"]
        if job["state"] in {"succeeded", "failed", "cancelled", "interrupted"}:
            return job
        time.sleep(0.01)
    raise AssertionError("extension job did not finish")


def run_command(service, command, parameters):
    revision = request(service, "project.query")["revision"]
    preview = request(service, "command.preview", {"command": command,
        "parameters": parameters, "expectedRevision": revision})
    approval = request(service, "host.approve", {"previewId": preview["previewId"],
        "previewHash": preview["previewHash"]})
    return request(service, "command.execute", {"previewId": preview["previewId"],
        "approvalToken": approval["approvalToken"]})


def _stats_source(delay=0):
    return f'''import math
import time
import os

def process(series):
    if "DEEPSEEK_API_KEY" in os.environ:
        raise RuntimeError("credential leaked to extension environment")
    time.sleep({delay!r})
    values = series["values"]
    times = series["times"]
    peak = max(abs(value) for value in values)
    index = next(i for i, value in enumerate(values) if abs(value) == peak)
    rms = 0.0 if peak == 0 else peak * math.sqrt(math.fsum((value / peak) ** 2 for value in values) / len(values))
    return {{"rms": rms, "peakAbs": peak, "peakTime": times[index],
            "timeUnit": series["timeUnit"], "valueUnit": series["valueUnit"]}}
'''


def manifest(extension_id="series.stats", version="1.0.0"):
    return {"manifestVersion": 1, "extensionId": extension_id, "version": version,
        "sdkVersion": "range.extension/v1", "kind": "numeric-series-statistics",
        "entryPoint": "extension.py:process", "dependencies": [],
        "inputSchema": "range.extension/v1/numeric-series",
        "outputSchema": "range.extension/v1/statistics",
        "gui": {"form": [{"id": "series", "type": "numeric-series", "required": True}],
                "table": {"columns": [
                    {"id": "rms", "label": "RMS", "unit": "valueUnit"},
                    {"id": "peakAbs", "label": "Absolute peak", "unit": "valueUnit"},
                    {"id": "peakTime", "label": "Peak time", "unit": "timeUnit"}]}}}


def tests():
    series = {"times": [0, 1], "values": [3, 4], "timeUnit": "s", "valueUnit": "m"}
    return {"cases": [{"id": "known-pair", "input": series,
        "expected": {"rms": math.sqrt(12.5), "peakAbs": 4, "peakTime": 1,
                      "timeUnit": "s", "valueUnit": "m"}}]}


class ExtensionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-extension-")
        self.base = Path(self.temporary.name)
        self.examples = self.base / "examples"
        self.examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (self.examples / name).write_text("fixture", encoding="utf-8")
        self.workspace = self.base / "workspace"
        self.service = self.open(trusted=True)

    def open(self, *, trusted=False):
        return WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable),
            self.examples, Path(sys.executable), trusted_extensions=trusted)

    def tearDown(self):
        self.service.close()
        self.temporary.cleanup()

    def stage(self, *, extension_manifest=None, source=None, test_cases=None, base_hash=None):
        parameters = {"manifest": extension_manifest or manifest(),
                     "source": source if source is not None else _stats_source(),
                     "tests": test_cases or tests()}
        if base_hash is not None:
            parameters["baseBundleHash"] = base_hash
        launched = run_command(self.service, "extension.stage", parameters)
        job = wait(self.service, launched["jobId"])
        self.assertEqual(job["state"], "succeeded", job.get("error"))
        return job["result"]

    def validate(self, staged):
        launched = run_command(self.service, "extension.validate",
            {"candidateId": staged["candidateId"], "bundleHash": staged["bundleHash"]})
        job = wait(self.service, launched["jobId"])
        self.assertEqual(job["state"], "succeeded", job.get("error"))
        return job["result"]

    def activate(self, validated):
        launched = run_command(self.service, "extension.activate",
            {"candidateId": validated["candidateId"], "bundleHash": validated["bundleHash"]})
        job = wait(self.service, launched["jobId"])
        self.assertEqual(job["state"], "succeeded", job.get("error"))
        return job["result"]

    def test_capabilities_schemas_and_trusted_mode_are_explicit(self):
        catalog = request(self.service, "capabilities.query")
        self.assertNotIn("host.approve", [item["id"] for item in catalog["operations"]])
        self.assertTrue(catalog["extensionRuntime"]["trustedExtensions"])
        self.assertEqual(catalog["extensionRuntime"]["trustMode"], "trusted-developer")
        self.assertFalse(catalog["extensionRuntime"]["osSandbox"])
        self.assertIn("extension.stage", [item["id"] for item in catalog["commands"]])
        self.assertEqual(request(self.service, "schema.get", {"schemaId": "range.extension/v1/numeric-series"})["schema"]["properties"]["times"]["maxItems"], 100000)
        self.assertEqual(request(self.service, "schema.get", {"schemaId": "range.extension/v1/numeric-series"})["schema"]["x-hostMaxJsonBytes"], 96 * 1024)
        knowledge = request(self.service, "knowledge.query", {"query": "command.preview"})
        self.assertTrue(any(item["id"] == "command.preview" for item in knowledge["items"]))
        for item in knowledge["items"]:
            self.assertTrue(item["sourcePath"].startswith("package:range_workflow/knowledge/core-v1.json#"))
            self.assertEqual(item["version"], "1.0.0")
            self.assertEqual(len(item["schemaHash"]), 64)
        extension_knowledge = request(self.service, "knowledge.query", {"query": "numeric-series RMS extension"})
        self.assertTrue(any(item["id"].startswith("extension.") for item in extension_knowledge["items"]))

    def test_stage_validate_activate_run_query_and_rollback(self):
        with mock.patch.dict(os.environ, {"DEEPSEEK_API_KEY": "must-not-enter-child"}):
            first = self.stage()
            self.assertEqual(first["state"], "staged")
            persisted = json.loads((self.workspace / "workflow-state.json").read_bytes())
            for persisted_job in persisted["jobs"].values():
                if persisted_job["command"] == "extension.stage":
                    self.assertNotIn("source", persisted_job["parameters"])
                    self.assertNotIn("tests", persisted_job["parameters"])
            for persisted_preview in persisted["previews"].values():
                if persisted_preview["normalized"]["command"] == "extension.stage":
                    self.assertNotIn("source", persisted_preview["normalized"]["parameters"])
                    self.assertNotIn("tests", persisted_preview["normalized"]["parameters"])
            candidate = request(self.service, "extension.candidate.get", {"candidateId": first["candidateId"]})["candidate"]
            self.assertEqual(candidate["source"], _stats_source())
            self.assertEqual(candidate["sourceDiff"]["format"], "unified-diff")
            self.assertEqual(candidate["baseSource"], "")
            validated = self.validate(first)
            self.assertEqual(validated["validation"], {"passed": True, "caseCount": 1, "independentCaseCount": 3})
            activated = self.activate(validated)
            self.assertEqual(activated["state"], "active")
            self.assertEqual(activated["capabilityRevision"], 1)

            series = {"times": [0.0, 1.0, 2.0], "values": [-3.0, 4.0, -4.0],
                      "timeUnit": "s", "valueUnit": "m/s"}
            launched = run_command(self.service, "extension.run", {"extensionId": "series.stats",
                "bundleHash": activated["bundleHash"], "series": series})
            result = wait(self.service, launched["jobId"])
            self.assertEqual(result["state"], "succeeded", result.get("error"))
            metrics = result["result"]["metrics"]
            self.assertAlmostEqual(metrics["rms"], math.sqrt(41 / 3))
            self.assertEqual(metrics["peakAbs"], 4)
            self.assertEqual(metrics["peakTime"], 1)
            self.assertEqual(result["result"]["sourceKind"], "caller-supplied")

            active_version = request(self.service, "extension.version.get", {
                "extensionId": "series.stats", "bundleHash": activated["bundleHash"]})["version"]
            self.assertEqual(active_version["manifest"]["gui"]["table"]["columns"][0]["id"], "rms")
            self.assertEqual(active_version["state"], "active")
            listed = request(self.service, "extension.list")["extensions"][0]
            self.assertEqual(listed["activeBundleHash"], activated["bundleHash"])
            detailed = request(self.service, "extension.list", {"extensionId": "series.stats",
                "candidateLimit": 1})["extensions"][0]
            self.assertEqual(len(detailed["candidates"]), 1)
            self.assertEqual(detailed["candidateTotal"], 1)
            self.assertIsNone(detailed["nextCandidateOffset"])

            # A 1,500-point caller series passes the extension-specific collection
            # bound while the overall JSON request remains below 256 KiB.
            long_series = {"times": [index * 0.01 for index in range(1500)],
                "values": [math.sin(index * 0.01) for index in range(1500)],
                "timeUnit": "s", "valueUnit": "m/s"}
            long_run = run_command(self.service, "extension.run", {"extensionId": "series.stats",
                "bundleHash": activated["bundleHash"], "series": long_series})
            long_result = wait(self.service, long_run["jobId"])
            self.assertEqual(long_result["state"], "succeeded", long_result.get("error"))
            persisted = json.loads((self.workspace / "workflow-state.json").read_bytes())
            for persisted_job in persisted["jobs"].values():
                if persisted_job["command"] == "extension.run":
                    self.assertNotIn("series", persisted_job["parameters"])
            for persisted_preview in persisted["previews"].values():
                if persisted_preview["normalized"]["command"] == "extension.run":
                    self.assertNotIn("series", persisted_preview["normalized"]["parameters"])

            second = self.stage(extension_manifest=manifest(version="1.1.0"),
                source=_stats_source() + "\n# immutable second version\n", base_hash=activated["bundleHash"])
            pages = request(self.service, "extension.list", {"extensionId": "series.stats",
                "candidateLimit": 1})["extensions"][0]
            self.assertEqual(len(pages["candidates"]), 1)
            self.assertEqual(pages["candidateTotal"], 2)
            self.assertEqual(pages["nextCandidateOffset"], 1)
            second_page = request(self.service, "extension.list", {"extensionId": "series.stats",
                "candidateOffset": pages["nextCandidateOffset"], "candidateLimit": 1})["extensions"][0]
            self.assertEqual(len(second_page["candidates"]), 1)
            diff = request(self.service, "extension.candidate.get", {"candidateId": second["candidateId"]})["candidate"]
            self.assertEqual(diff["baseBundleHash"], activated["bundleHash"])
            self.assertIn("immutable second version", diff["sourceDiff"]["text"])
            self.assertEqual(diff["baseSource"], _stats_source())
            second_validated = self.validate(second)
            self.activate(second_validated)
            now_active = request(self.service, "extension.list")["extensions"][0]["activeBundleHash"]
            rollback_preview = request(self.service, "command.preview", {"command": "extension.rollback",
                "parameters": {"extensionId": "series.stats", "expectedActiveBundleHash": now_active,
                               "targetBundleHash": activated["bundleHash"]},
                "expectedRevision": request(self.service, "project.query")["revision"]})
            rollback_approval = request(self.service, "host.approve", {
                "previewId": rollback_preview["previewId"], "previewHash": rollback_preview["previewHash"]})
            with mock.patch("range_workflow.extensions.runtime_identity",
                            return_value={"runtimeHash": "f" * 64}):
                with self.assertRaises(WorkflowError) as caught:
                    request(self.service, "command.execute", {"previewId": rollback_preview["previewId"],
                        "approvalToken": rollback_approval["approvalToken"]})
            self.assertEqual(caught.exception.code, "RUNTIME_CHANGED")
            rolled = run_command(self.service, "extension.rollback", {"extensionId": "series.stats",
                "expectedActiveBundleHash": now_active, "targetBundleHash": activated["bundleHash"]})
            rollback = wait(self.service, rolled["jobId"])
            self.assertEqual(rollback["state"], "succeeded", rollback.get("error"))
            self.assertEqual(rollback["result"]["activeBundleHash"], activated["bundleHash"])
            self.assertEqual(request(self.service, "extension.list")["extensions"][0]["activeVersion"], "1.0.0")

    def test_trust_gate_tamper_malformed_cases_and_stale_preview(self):
        self.service.close()
        self.workspace = self.base / "untrusted-workspace"
        untrusted = self.open(trusted=False)
        try:
            staged = run_command(untrusted, "extension.stage", {"manifest": manifest(),
                "source": _stats_source(), "tests": tests()})
            staged_job = wait(untrusted, staged["jobId"])
            self.assertEqual(staged_job["state"], "succeeded", staged_job.get("error"))
            revision = request(untrusted, "project.query")["revision"]
            preview = request(untrusted, "command.preview", {"command": "extension.validate",
                "parameters": {"candidateId": staged_job["result"]["candidateId"],
                               "bundleHash": staged_job["result"]["bundleHash"]},
                "expectedRevision": revision})
            approval = request(untrusted, "host.approve", {"previewId": preview["previewId"],
                "previewHash": preview["previewHash"]})
            with self.assertRaises(WorkflowError) as caught:
                request(untrusted, "command.execute", {"previewId": preview["previewId"],
                    "approvalToken": approval["approvalToken"]})
            self.assertEqual(caught.exception.code, "TRUSTED_RUNTIME_REQUIRED")
            self.assertEqual(request(untrusted, "jobs.query")["jobs"], [staged_job])
        finally:
            untrusted.close()
        self.workspace = self.base / "workspace"
        self.service = self.open(trusted=True)

        staged = self.stage()
        record = self.service.extension_state()["series.stats"]["candidates"][staged["candidateId"]]
        source_path = self.workspace / record["sourcePath"]
        source_path.write_text("# tampered\n", encoding="utf-8")
        with self.assertRaises(WorkflowError) as caught:
            request(self.service, "extension.candidate.get", {"candidateId": staged["candidateId"]})
        self.assertEqual(caught.exception.code, "EXTENSION_TAMPERED")

        bad = tests()
        bad["cases"][0]["expected"]["rms"] = 1.0
        bad_candidate = self.stage(extension_manifest=manifest("series.bad"), test_cases=bad)
        launched = run_command(self.service, "extension.validate", {
            "candidateId": bad_candidate["candidateId"], "bundleHash": bad_candidate["bundleHash"]})
        failed = wait(self.service, launched["jobId"])
        self.assertEqual(failed["state"], "failed")
        self.assertEqual(failed["error"]["code"], "EXTENSION_REFERENCE_MISMATCH")

    def test_project_revision_change_stales_an_extension_preview(self):
        staged = self.stage(extension_manifest=manifest("series.stale"))
        revision = request(self.service, "project.query")["revision"]
        preview = request(self.service, "command.preview", {"command": "extension.validate",
            "parameters": {"candidateId": staged["candidateId"], "bundleHash": staged["bundleHash"]},
            "expectedRevision": revision})
        approval = request(self.service, "host.approve", {"previewId": preview["previewId"],
            "previewHash": preview["previewHash"]})
        project = run_command(self.service, "workflow.configure",
            {"dt": .005, "steps": 10, "targetDamping": .1})
        self.assertEqual(wait(self.service, project["jobId"])["state"], "succeeded")
        with self.assertRaises(WorkflowError) as caught:
            request(self.service, "command.execute", {"previewId": preview["previewId"],
                "approvalToken": approval["approvalToken"]})
        self.assertEqual(caught.exception.code, "STALE_REVISION")

    def test_timeout_and_invalid_child_output_fail_without_activation(self):
        self.service.extension_timeout_seconds = 0.25
        slow = self.stage(source=_stats_source(2.0))
        launched = run_command(self.service, "extension.validate", {
            "candidateId": slow["candidateId"], "bundleHash": slow["bundleHash"]})
        job = wait(self.service, launched["jobId"], timeout=5)
        self.assertEqual(job["state"], "failed")
        self.assertEqual(job["error"]["code"], "TIMEOUT")
        self.assertFalse(request(self.service, "extension.list")["extensions"][0].get("activeBundleHash"))

        self.service.extension_timeout_seconds = 15
        malformed_source = '''def process(series):\n    return {"rms": float("nan"), "peakAbs": 0, "peakTime": 0, "timeUnit": series["timeUnit"], "valueUnit": series["valueUnit"]}\n'''
        malformed = self.stage(extension_manifest=manifest("series.malformed"), source=malformed_source)
        launched = run_command(self.service, "extension.validate", {
            "candidateId": malformed["candidateId"], "bundleHash": malformed["bundleHash"]})
        failed = wait(self.service, launched["jobId"])
        self.assertEqual(failed["state"], "failed")
        malformed_entry = next(item for item in request(self.service, "extension.list")["extensions"]
                               if item["extensionId"] == "series.malformed")
        self.assertIsNone(malformed_entry["activeBundleHash"])

    def test_reopen_rejects_tampered_extension_registry_metadata(self):
        staged = self.stage()
        activated = self.activate(self.validate(staged))
        self.assertTrue(activated["bundleHash"])
        self.service.close()
        state_path = self.workspace / "workflow-state.json"
        persisted = json.loads(state_path.read_bytes())
        persisted["extensions"]["series.stats"]["activeBundleHash"] = "0" * 64
        state_path.write_text(json.dumps(persisted), encoding="utf-8")
        with self.assertRaises(WorkflowError) as caught:
            self.open(trusted=True)
        self.assertEqual(caught.exception.code, "CORRUPT_STATE")

    def test_packaged_runtime_hashes_every_manifest_file_and_detects_tamper(self):
        runtime = self.base / "fake-runtime"
        runtime.mkdir()
        executable = runtime / "python.exe"
        library = runtime / "python313.dll"
        lock = runtime / "runtime-lock.json"
        files = runtime / "runtime-files.json"
        executable.write_bytes(b"test interpreter")
        library.write_bytes(b"trusted dll")
        lock.write_text(json.dumps({"schemaVersion": 1, "runtimeId": "test-runtime",
            "platform": "win_amd64", "files": [{"name": "cpython"}]}), encoding="utf-8")
        manifest_data = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (executable, library, lock)}
        files.write_text(json.dumps(manifest_data), encoding="utf-8")
        self.service.python = executable

        original = runtime_identity(self.service)
        self.assertEqual(original["profile"], "packaged")
        self.assertTrue(original["identity"]["verifiedFiles"])
        self.assertRegex(original["identity"]["runtimeFilesSha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(original["identity"]["extensionChildSha256"],
            hashlib.sha256((Path(__file__).resolve().parents[1] / "python" / "range_workflow" /
                            "extension_child.py").read_bytes()).hexdigest())

        before = library.stat()
        library.write_bytes(b"UNTRUSTED!!")
        os.utime(library, ns=(before.st_atime_ns, before.st_mtime_ns))
        after = library.stat()
        self.assertEqual(after.st_size, before.st_size)
        self.assertEqual(after.st_mtime_ns, before.st_mtime_ns)
        with self.assertRaises(WorkflowError) as caught:
            runtime_identity(self.service, force_verify=True)
        self.assertEqual(caught.exception.code, "RUNTIME_CHANGED")

    def test_unapproved_extension_previews_are_bounded_and_stale_payloads_compact(self):
        parameters = {"manifest": manifest("series.pending"), "source": _stats_source(), "tests": tests()}
        revision = request(self.service, "project.query")["revision"]
        first = request(self.service, "command.preview", {"command": "extension.stage",
            "parameters": parameters, "expectedRevision": revision})
        stored = json.loads((self.workspace / "workflow-state.json").read_bytes())
        first_record = stored["previews"][first["previewId"]]
        self.assertIn("source", first["normalized"]["parameters"])
        first_size = len(json.dumps(first_record["normalized"], separators=(",", ":")).encode("utf-8"))

        with mock.patch("range_workflow.extensions.MAX_PENDING_PREVIEW_BYTES", first_size):
            with self.assertRaises(WorkflowError) as caught:
                request(self.service, "command.preview", {"command": "extension.stage",
                    "parameters": parameters, "expectedRevision": revision})
        self.assertEqual(caught.exception.code, "PENDING_PREVIEW_LIMIT")

        configured = run_command(self.service, "workflow.configure",
            {"dt": .005, "steps": 10, "targetDamping": .1})
        self.assertEqual(wait(self.service, configured["jobId"])["state"], "succeeded")
        compacted = json.loads((self.workspace / "workflow-state.json").read_bytes())
        stale = compacted["previews"][first["previewId"]]
        self.assertNotIn("source", stale["normalized"]["parameters"])
        self.assertNotIn("tests", stale["normalized"]["parameters"])
        with self.assertRaises(WorkflowError) as caught:
            request(self.service, "host.approve", {"previewId": first["previewId"],
                "previewHash": first["previewHash"]})
        self.assertEqual(caught.exception.code, "STALE_REVISION")

    def test_stored_candidate_cases_accept_over_1024_points_under_byte_limit(self):
        series = {"times": [index * .001 for index in range(1500)],
                  "values": [float(index % 5 - 2) for index in range(1500)],
                  "timeUnit": "s", "valueUnit": "m/s"}
        peak = max(abs(value) for value in series["values"])
        peak_index = next(index for index, value in enumerate(series["values"]) if abs(value) == peak)
        expected = {"rms": math.sqrt(math.fsum(value * value for value in series["values"]) / len(series["values"])),
                    "peakAbs": peak, "peakTime": series["times"][peak_index],
                    "timeUnit": "s", "valueUnit": "m/s"}
        case_data = {"cases": [{"id": "large-series", "input": series, "expected": expected}]}
        staged = self.stage(extension_manifest=manifest("series.large"), test_cases=case_data)
        validated = self.validate(staged)
        self.assertEqual(validated["validation"]["caseCount"], 1)

        parsed = _strict_json(json.dumps({"times": series["times"]}).encode("utf-8"),
                              max_bytes=96 * 1024, max_collection_size=100000)
        self.assertEqual(len(parsed["times"]), 1500)

    def test_extension_lane_hot_activation_and_pinned_run_survive(self):
        first = self.stage(source=_stats_source(1.5))
        self.activate(self.validate(first))
        project_entered = threading.Event()
        project_release = threading.Event()
        def fixture_project_run(*_args):
            project_entered.set()
            if not project_release.wait(5):
                raise AssertionError("project fixture was not released")
            return {"testFixtureOnly": True}

        with mock.patch("range_workflow.jobs.run_native_workflow", side_effect=fixture_project_run):
            project_job = run_command(self.service, "workflow.run",
                {"mode": "active", "independentReference": True})
            self.assertTrue(project_entered.wait(2))

            pinned_series = {"times": [0, 1], "values": [3, 4], "timeUnit": "s", "valueUnit": "m"}
            old_run = run_command(self.service, "extension.run", {"extensionId": "series.stats",
                "bundleHash": first["bundleHash"], "series": pinned_series})
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                state = request(self.service, "jobs.query", {"jobId": old_run["jobId"]})["job"]["state"]
                if state == "running":
                    break
                time.sleep(0.01)
            self.assertEqual(state, "running")

            second = self.stage(extension_manifest=manifest(version="1.1.0"),
                source=_stats_source() + "\n# new active version\n", base_hash=first["bundleHash"])
            second_validated = self.validate(second)
            second_active = self.activate(second_validated)
            self.assertNotEqual(second_active["bundleHash"], first["bundleHash"])
            project_release.set()

        self.assertEqual(wait(self.service, project_job["jobId"])["state"], "succeeded")
        old_result = wait(self.service, old_run["jobId"], timeout=5)
        self.assertEqual(old_result["state"], "succeeded", old_result.get("error"))
        self.assertEqual(old_result["result"]["bundleHash"], first["bundleHash"])
        self.assertEqual(old_result["result"]["version"], "1.0.0")
        old_version = request(self.service, "extension.version.get", {"extensionId": "series.stats",
            "bundleHash": first["bundleHash"]})["version"]
        self.assertEqual(old_version["state"], "retained")


if __name__ == "__main__":
    unittest.main()
