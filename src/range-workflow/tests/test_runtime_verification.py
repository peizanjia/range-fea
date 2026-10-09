"""Bounded read-only runtime verification; injected hashing, no native processes."""
from __future__ import annotations

import copy
import io
import json
from pathlib import Path
import queue
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import scientific_runtime
from range_workflow.runtime import serve
from range_workflow.service import WorkflowError, WorkflowService, canonical


def request(operation, parameters=None, request_id="request"):
    return {"apiVersion": "range.workflow/v1", "requestId": request_id,
            "operation": operation, "parameters": parameters or {}}


class RuntimeVerificationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        self.examples = self.base / "examples"
        self.examples.mkdir()
        self.service = self.open_service()
        self.addCleanup(lambda: self.service.close())
        self.stamp = "a" * 64
        self.entered, self.release = threading.Event(), threading.Event()
        self.release.set()
        self.stamp_mock = mock.patch.object(scientific_runtime, "environment_stamp", side_effect=lambda _: self.stamp)
        self.full_mock = mock.patch.object(scientific_runtime, "runtime_identity", side_effect=self.verify)
        self.stamp_mock.start()
        self.verifier = self.full_mock.start()
        self.addCleanup(self.stamp_mock.stop)
        self.addCleanup(self.full_mock.stop)
        self.addCleanup(self.finish)

    def open_service(self):
        return WorkflowService(self.base / "workspace", Path(sys.executable), Path(sys.executable),
                               self.examples, Path(sys.executable))

    def finish(self):
        self.release.set()
        self.service.close()
        worker = self.service._runtime_verification_worker
        if worker:
            worker.join(3)
            self.assertFalse(worker.is_alive())

    def verify(self, captured, *, force_verify):
        self.assertTrue(force_verify)
        self.assertEqual(set(vars(captured)), {"python"})
        self.entered.set()
        self.assertTrue(self.release.wait(3))
        return {"profile": "packaged", "runtimeHash": "b" * 64, "identity": {"verifiedFiles": True}}

    def start(self, **parameters):
        return self.service.handle(request("extension.runtime.verify", {"sdkVersion": "range.extension/v2", **parameters}))

    def query(self, verification_id):
        return self.service.handle(request("extension.runtime.verification.query", {"verificationId": verification_id}))

    def wait_terminal(self, verification_id):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            result = self.query(verification_id)
            if result["state"] in {"ready", "failed", "cancelled"}:
                return result
            time.sleep(.005)
        self.fail("verification did not finish")

    def assert_code(self, code, action):
        with self.assertRaises(WorkflowError) as caught:
            action()
        self.assertEqual(caught.exception.code, code)

    def scientific_registry(self, sdk_version="range.extension/v2"):
        bundle = "f" * 64
        record = {"candidateId": "candidate", "bundleHash": bundle, "manifest": {"sdkVersion": sdk_version}}
        self.service._state["extensions"]["scientific"] = {
            "candidates": {"candidate": record}, "versions": {bundle: record}, "activeBundleHash": bundle}
        return {
            "extension.validate": {"candidateId": "candidate", "bundleHash": bundle},
            "extension.activate": {"candidateId": "candidate", "bundleHash": bundle},
            "extension.run": {"extensionId": "scientific", "bundleHash": bundle},
            "extension.rollback": {"extensionId": "scientific", "targetBundleHash": bundle}}

    def preview(self, command, parameters):
        return self.service.handle(request("command.preview", {"command": command, "parameters": parameters,
                                   "expectedRevision": self.service._state["revision"]}))

    def test_v2_cold_preview_and_rebinding_reject_before_any_normalizer_or_hash(self):
        commands = self.scientific_registry()
        normalizers = []
        for command, parameters in commands.items():
            normalizer = mock.Mock(side_effect=AssertionError("normalizer must not run"))
            normalizers.append(normalizer)
            with mock.patch.dict(self.service._commands[command], {"normalize": normalizer}):
                start = time.monotonic()
                self.assert_code("RUNTIME_VERIFICATION_REQUIRED", lambda: self.preview(command, parameters))
                self.assertLess(time.monotonic() - start, .5)
        self.assertFalse(any(normalizer.called for normalizer in normalizers))
        self.assertFalse(self.verifier.called)
        preview = {"sessionId": self.service._state["sessionId"], "revision": self.service._state["revision"],
                   "normalized": {"command": "extension.validate", "parameters": commands["extension.validate"]}}
        self.assert_code("RUNTIME_VERIFICATION_REQUIRED", lambda: self.service._validate_preview(preview))

    def test_v2_active_verification_rejects_fast_even_when_previous_receipt_ready(self):
        commands = self.scientific_registry()
        ack = self.start()
        self.assertEqual(self.wait_terminal(ack["verificationId"])["state"], "ready")
        # A witnessed normalizer verifies that a completed association clears the gate.
        normalizer = mock.Mock(side_effect=WorkflowError("NORMALIZER_REACHED", "witness"))
        with mock.patch.dict(self.service._commands["extension.validate"], {"normalize": normalizer}):
            self.assert_code("NORMALIZER_REACHED", lambda: self.preview("extension.validate", commands["extension.validate"]))
        self.release.clear()
        self.entered.clear()
        ack = self.start()
        self.assertTrue(self.entered.wait(2))
        start = time.monotonic()
        for command, parameters in commands.items():
            self.assert_code("RUNTIME_VERIFICATION_IN_PROGRESS", lambda: self.preview(command, parameters))
        self.service.handle(request("project.query"))
        self.assertLess(time.monotonic() - start, .5)
        self.release.set()
        self.assertEqual(self.wait_terminal(ack["verificationId"])["state"], "ready")

    def test_v1_registry_commands_do_not_require_scientific_association(self):
        for command, parameters in self.scientific_registry("range.extension/v1").items():
            self.service._require_scientific_runtime_verification(command, parameters)

    def test_immediate_ack_dedup_and_owner_queries_during_full_verification(self):
        self.release.clear()
        before = copy.deepcopy(self.service._state)
        start = time.monotonic()
        ack = self.start()
        self.assertLess(time.monotonic() - start, .5)
        self.assertLess(len(canonical(ack)), 1024)
        self.assertEqual(set(ack), {"verificationId", "state"})
        self.assertTrue(self.entered.wait(2))
        self.assertEqual(self.start()["verificationId"], ack["verificationId"])
        self.assertEqual(self.query(ack["verificationId"])["state"], "running")
        self.service.handle(request("project.query"))
        self.assertEqual(self.service._state, before)
        self.release.set()
        ready = self.wait_terminal(ack["verificationId"])
        self.assertEqual(ready["state"], "ready")
        self.assertTrue(ready["fullFileVerify"]["verifiedFiles"])
        self.assertEqual(ready["profile"], "packaged")
        self.assertTrue(ready["verifiedAt"])
        self.assertTrue(ready["isCurrent"])
        self.assertEqual(self.service._state, before)
        self.assertFalse(self.service.trusted_extensions)
        self.assertFalse(self.service._approval_tokens)

    def test_live_job_cancel_and_runtime_parameters_query_remain_responsive(self):
        cancel = threading.Event()
        worker = threading.Thread(target=lambda: cancel.wait(3), daemon=True)
        worker.start()
        self.service._state["jobs"]["live"] = {"jobId": "live", "state": "running", "command": "workflow.run"}
        self.service._workers["live"] = (worker, cancel)
        self.release.clear()
        ack = self.start()
        self.assertTrue(self.entered.wait(2))
        start = time.monotonic()
        result = self.service.handle(request("runtime.parameters.query", {"jobId": "live"}))
        self.assertEqual(result["jobState"], "running")
        self.assertEqual(self.service.handle(request("job.cancel", {"jobId": "live"}))["state"], "cancelling")
        self.service.handle(request("project.query"))
        self.assertLess(time.monotonic() - start, .5)
        self.assertTrue(cancel.is_set())
        self.release.set()
        self.assertEqual(self.wait_terminal(ack["verificationId"])["state"], "ready")

    def test_environment_changed_during_full_verification_cannot_be_ready(self):
        self.release.clear()
        ack = self.start()
        self.assertTrue(self.entered.wait(2))
        self.stamp = "c" * 64
        self.release.set()
        result = self.wait_terminal(ack["verificationId"])
        self.assertEqual(result["state"], "failed")
        self.assertEqual(result["error"]["code"], "RUNTIME_CHANGED")
        self.assertIsNone(result["fullFileVerify"])

    def test_ready_receipt_is_historical_and_changed_stamp_invalidates_currentity(self):
        ack = self.start()
        ready = self.wait_terminal(ack["verificationId"])
        self.stamp = "d" * 64
        changed = self.query(ack["verificationId"])
        self.assertEqual(changed["state"], "failed")
        self.assertFalse(changed["isCurrent"])
        self.assertEqual(changed["fullFileVerify"], ready["fullFileVerify"])
        self.assertEqual(self.verifier.call_count, 1)

    def test_cancel_does_not_interrupt_read_or_grant_ready_and_bounds_actual_work(self):
        self.release.clear()
        first = self.start()
        self.assertTrue(self.entered.wait(2))
        cancelled = self.service.handle(request("host.runtime.verification.cancel", {"verificationId": first["verificationId"]}))
        self.assertEqual(cancelled["state"], "cancelled")
        second = self.start()
        self.assertNotEqual(first["verificationId"], second["verificationId"])
        # Different configured source may queue, but the cancelled read still occupies one slot.
        self.service.python = Path(sys.executable).with_name("other-python.exe")
        self.assert_code("RUNTIME_VERIFICATION_BUSY", self.start)
        self.service.python = Path(sys.executable)
        self.release.set()
        self.wait_terminal(second["verificationId"])
        self.assertEqual(self.query(first["verificationId"])["state"], "cancelled")
        self.assertIsNone(self.query(first["verificationId"])["fullFileVerify"])

    def test_terminal_retention_fresh_force_and_no_project_revision_changes(self):
        before = copy.deepcopy(self.service._state)
        ids = []
        for _ in range(10):
            ack = self.start(force=True)
            ids.append(ack["verificationId"])
            self.assertEqual(self.wait_terminal(ids[-1])["state"], "ready")
        self.assertEqual(len(set(ids)), 10)
        self.assertEqual(len(self.service._runtime_verifications), 8)
        self.assert_code("RUNTIME_VERIFICATION_NOT_FOUND", lambda: self.query(ids[0]))
        self.assertEqual(self.service._state, before)
        self.assertEqual(self.verifier.call_count, 10)

    def test_closed_schemas_no_paths_and_descriptors_no_hidden_cancel(self):
        before = copy.deepcopy(self.service._state)
        for parameters in ({"sdkVersion": "range.extension/v1"}, {"sdkVersion": "range.extension/v2", "force": 1},
                           {"sdkVersion": "range.extension/v2", "path": str(sys.executable)},
                           {"sdkVersion": "range.extension/v2", "code": "print(1)"}):
            self.assert_code("INVALID_PARAMETERS", lambda held=parameters: self.service.handle(request("extension.runtime.verify", held)))
        self.assert_code("RUNTIME_VERIFICATION_NOT_FOUND", lambda: self.query("unknown"))
        descriptors = {item["id"]: item for item in self.service.capabilities()["operations"]}
        self.assertTrue(descriptors["extension.runtime.verify"]["readOnly"])
        self.assertTrue(descriptors["extension.runtime.verification.query"]["readOnly"])
        self.assertNotIn("host.runtime.verification.cancel", descriptors)
        self.assertEqual(self.service._state, before)

    def test_close_is_prompt_and_reopened_host_cannot_query_old_identity(self):
        self.release.clear()
        ack = self.start()
        self.assertTrue(self.entered.wait(2))
        old = self.service
        start = time.monotonic()
        old.close()
        self.assertLess(time.monotonic() - start, .5)
        self.assertEqual(old._runtime_verifications[ack["verificationId"]]["state"], "cancelled")
        self.service = self.open_service()
        self.assert_code("RUNTIME_VERIFICATION_NOT_FOUND", lambda: self.query(ack["verificationId"]))
        self.release.set()
        old._runtime_verification_worker.join(3)
        self.assertFalse(old._runtime_verification_worker.is_alive())

    def test_stdio_stamp_query_does_not_block_project_owner_loop(self):
        ack = self.start()
        self.wait_terminal(ack["verificationId"])
        entered, release = threading.Event(), threading.Event()
        class Input:
            def __init__(self):
                self.lines = queue.Queue()
            def readline(self, _maximum):
                return self.lines.get(timeout=3)
        class Output(io.BytesIO):
            def __init__(self):
                super().__init__()
                self.project = threading.Event()
            def write(self, data):
                result = super().write(data)
                if json.loads(data)["requestId"] == "project":
                    self.project.set()
                return result
        source, output = Input(), Output()
        def blocked(_):
            entered.set()
            self.assertTrue(release.wait(3))
            return self.stamp
        with mock.patch.object(scientific_runtime, "environment_stamp", side_effect=blocked):
            worker = threading.Thread(target=lambda: serve(self.service, source, output))
            worker.start()
            source.lines.put(canonical(request("extension.runtime.verification.query", {"verificationId": ack["verificationId"]}, "verify-query")) + b"\n")
            self.assertTrue(entered.wait(2))
            source.lines.put(canonical(request("project.query", request_id="project")) + b"\n")
            self.assertTrue(output.project.wait(1))
            release.set()
            source.lines.put(b"")
            worker.join(3)
            self.assertFalse(worker.is_alive())


if __name__ == "__main__":
    unittest.main()
