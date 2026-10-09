"""Pure read-lane isolation and real stdio scheduling tests; no FE claims."""
from __future__ import annotations

import copy
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import series_sources, series_workbench
from range_workflow import native_results
from range_workflow.service import WorkflowError, WorkflowService, canonical, file_hash
from test_series_workbench import VerifiedSnapshots


def request(operation, parameters=None, request_id="request"):
    return {"apiVersion": "range.workflow/v1", "requestId": request_id,
            "operation": operation, "parameters": parameters or {}}


def source(job="job-1"):
    return {"kind": "native", "jobId": job, "mode": "active", "artifactId": "artifact-1",
            "channelId": "node:51000:velocity:X"}


def fixture(base):
    examples = base / "examples"
    examples.mkdir(exist_ok=True)
    for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
        (examples / name).write_text("fixture", encoding="utf-8")
    service = WorkflowService(base / "workspace", Path(sys.executable), Path(sys.executable),
                              examples, Path(sys.executable), native_results=Path(sys.executable))
    for job_id in ("job-1", "job-2"):
        directory = service.workspace / "jobs" / job_id
        directory.mkdir(parents=True)
        manifest = directory / "manifest.json"
        manifest.write_bytes(b"{}")
        sha = file_hash(manifest)
        service._state["jobs"][job_id] = {"jobId": job_id, "state": "succeeded",
            "result": {"nativeResults": {"active": {"manifestPath": str(manifest),
                       "manifestSha256": sha, "artifactId": "artifact-1"}}},
            "artifacts": [{"path": str(manifest), "sha256": sha}]}
    event = threading.Event()
    sleeper = threading.Thread(target=lambda: event.wait(10), daemon=True)
    sleeper.start()
    service._state["jobs"]["live"] = {"jobId": "live", "state": "running", "command": "workflow.run"}
    service._workers["live"] = (sleeper, event)
    return service


class ReadQueryLaneTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.service = fixture(Path(self.directory.name))
        self.addCleanup(self.service.close)
        self.broker = VerifiedSnapshots()
        self.patch = mock.patch.object(series_sources, "query_registered", side_effect=self.broker)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def plot(self, job="job-1"):
        return request("result.plot.query", {"view": "time", "sources": [source(job)]})

    def assertCode(self, code, function):
        with self.assertRaises(WorkflowError) as caught:
            function()
        self.assertEqual(caught.exception.code, code)

    def test_expensive_snapshot_does_not_hold_owner_lock_or_block_cancel(self):
        entered, release = threading.Event(), threading.Event()
        broker = self.broker
        def blocked(view, operation, parameters):
            if operation == "snapshot":
                entered.set()
                self.assertTrue(release.wait(5))
            return broker(view, operation, parameters)
        result = []
        with mock.patch.object(series_sources, "query_registered", side_effect=blocked):
            worker = threading.Thread(target=lambda: result.append(self.service.handle(self.plot())))
            worker.start()
            self.assertTrue(entered.wait(3))
            start = time.monotonic()
            self.assertEqual(self.service.handle(request("job.cancel", {"jobId": "live"}))["state"], "cancelling")
            self.assertTrue(self.service._workers["live"][1].is_set())
            self.service.handle(request("project.query"))
            self.assertLess(time.monotonic() - start, 1)
            release.set()
            worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["view"], "time")

    def test_copy_is_minimal_deep_and_unrelated_revision_is_accepted(self):
        view = self.service.prepare_read(self.plot())
        self.assertEqual(set(view._state), {"jobs"})
        self.assertEqual(set(view._state["jobs"]), {"job-1"})
        exposed = view._job("job-1")
        exposed["result"]["nativeResults"].clear()
        self.assertTrue(view._job("job-1")["result"]["nativeResults"])
        self.service._state["revision"] += 1
        self.service._state["jobs"]["job-2"]["result"]["unrelated"] = True
        self.assertEqual(view.run()["view"], "time")
        self.assertFalse(self.service._read_queries)

    def test_owner_closure_is_captured_and_missing_owner_is_rejected(self):
        path = self.service.workspace / "jobs" / "job-2" / "reference.json"
        path.write_bytes(b"{}")
        artifact = {"path": str(path), "sha256": file_hash(path)}
        self.service._state["jobs"]["job-2"]["artifacts"].append(artifact)
        job = self.service._state["jobs"]["job-1"]
        job["result"].update(referencePath=str(path), sourceArtifacts={"reference": artifact})
        view = self.service.prepare_read(self.plot())
        self.assertEqual(set(view._state["jobs"]), {"job-1", "job-2"})
        view.cancel.set()
        self.assertCode("SERVICE_CLOSED", view.run)
        self.service._state["jobs"]["job-2"]["state"] = "running"
        self.assertCode("RESULT_UNAVAILABLE", lambda: self.service.prepare_read(self.plot()))

    def test_ledger_file_and_workspace_changes_fail_closed(self):
        view = self.service.prepare_read(self.plot())
        self.service._state["jobs"]["job-1"]["result"]["changed"] = 1
        self.assertCode("READ_SOURCE_CHANGED", view.run)
        view = self.service.prepare_read(self.plot())
        self.service.workspace = self.service.workspace.parent / "other"
        self.assertCode("READ_SOURCE_CHANGED", view.run)
        self.service.workspace = Path(self.directory.name) / "workspace"
        view = self.service.prepare_read(self.plot())
        descriptor = view._job("job-1")["result"]["nativeResults"]["active"]
        Path(descriptor["manifestPath"]).write_bytes(b"tampered")
        self.assertCode("READ_SOURCE_CHANGED", view.run)

    def test_strict_schema_and_registered_identity_before_worker_allocation(self):
        self.assertCode("INVALID_PARAMETERS", lambda: self.service.handle(request(["result.plot.query"])))
        bad = self.plot()
        bad["apiVersion"] = "wrong"
        self.assertCode("UNSUPPORTED_VERSION", lambda: self.service.prepare_read(bad))
        bad = self.plot()
        bad["parameters"]["path"] = "untrusted"
        self.assertCode("INVALID_PARAMETERS", lambda: self.service.prepare_read(bad))
        bad = self.plot()
        bad["parameters"]["sources"][0]["artifactId"] = "other"
        self.assertCode("ARTIFACT_IDENTITY_MISMATCH", lambda: self.service.prepare_read(bad))
        self.assertCode("UNKNOWN_OPERATION", lambda: self.service.prepare_read(request("host.approve", {})))
        self.assertFalse(self.service._read_queries)
        self.assertEqual(self.service._state["audit"][-1]["record"]["commandId"], "request.reject")

    def test_four_context_bound_and_pending_cancellation_release(self):
        views = [self.service.prepare_read(self.plot()) for _ in range(4)]
        self.assertCode("READ_QUERY_BUSY", lambda: self.service.prepare_read(self.plot()))
        self.service.cancel_reads()
        for view in views:
            self.assertCode("SERVICE_CLOSED", view.run)
        self.assertFalse(self.service._read_queries)

    def test_hidden_cancel_is_targeted_and_never_cancels_mutation_jobs(self):
        first = self.service.prepare_read(self.plot())
        other_request = self.plot("job-2")
        other_request["requestId"] = "other"
        other = self.service.prepare_read(other_request)
        response = self.service.handle(request("host.read.cancel", {"requestId": "request"}, "cancel-read"))
        self.assertEqual(response, {"requestId": "request", "readCancelled": True})
        self.assertFalse(self.service._workers["live"][1].is_set())
        self.assertFalse(other.cancel.is_set())
        self.assertCode("READ_CANCELLED", first.run)
        self.assertEqual(other.run()["view"], "time")
        self.assertFalse(self.service.handle(request("host.read.cancel", {"requestId": "other"}))["readCancelled"])
        self.assertNotIn("host.read.cancel", [o["id"] for o in self.service.capabilities()["operations"]])

    def test_cancel_active_read_preserves_other_read_and_source_state(self):
        entered = threading.Event()
        outcome = []
        broker = self.broker
        def blocked(view, operation, parameters):
            if operation == "snapshot" and parameters["jobId"] == "job-1":
                entered.set()
                self.assertTrue(view.cancel.wait(3))
                raise WorkflowError("READ_CANCELLED", "cancelled")
            return broker(view, operation, parameters)
        def read():
            try:
                self.service.handle(self.plot())
            except WorkflowError as error:
                outcome.append(error.code)
        with mock.patch.object(series_sources, "query_registered", side_effect=blocked):
            worker = threading.Thread(target=read)
            worker.start()
            self.assertTrue(entered.wait(3))
            self.assertTrue(self.service.handle(request("host.read.cancel", {"requestId": "request"}))["readCancelled"])
            self.assertEqual(self.service.handle(self.plot("job-2"))["view"], "time")
            worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(outcome, ["READ_CANCELLED"])
        self.assertEqual(self.service._state["jobs"]["job-1"]["state"], "succeeded")
        self.assertFalse(self.service._read_queries)

    def test_closed_service_rejects_pending_views_without_waiting_for_unstarted_work(self):
        view = self.service.prepare_read(self.plot())
        start = time.monotonic()
        self.service.close()
        self.assertLess(time.monotonic() - start, 1)
        self.assertCode("SERVICE_CLOSED", view.run)
        self.assertTrue(view.done.is_set())

    def test_cancel_native_python_child_reaps_process_tree(self):
        marker = Path(self.directory.name) / "child-pid"
        cancel = threading.Event()
        errors = []
        child_code = "import time; time.sleep(60)"
        parent_code = ("import subprocess,sys,time,pathlib; "
                       f"p=subprocess.Popen([sys.executable,'-c',{child_code!r}]); "
                       f"pathlib.Path({str(marker)!r}).write_text(str(p.pid)); time.sleep(60)")
        def run():
            try:
                with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as stderr:
                    with native_results.query_cancellation(cancel):
                        native_results._run_query_process([sys.executable, "-c", parent_code], output, stderr, 30)
            except WorkflowError as error:
                errors.append(error.code)
        worker = threading.Thread(target=run)
        worker.start()
        until = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < until:
            time.sleep(.01)
        self.assertTrue(marker.exists())
        pid = int(marker.read_text())
        process_handle = None
        if os.name == "nt":
            import ctypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            process_handle = kernel.OpenProcess(0x00100000, False, pid)
            self.assertTrue(process_handle)
        try:
            start = time.monotonic()
            cancel.set()
            worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertLess(time.monotonic() - start, 3)
            self.assertEqual(errors, ["READ_CANCELLED"])
            if process_handle:
                self.assertEqual(kernel.WaitForSingleObject(process_handle, 1000), 0)
        finally:
            cancel.set()
            worker.join(3)
            if process_handle:
                kernel.CloseHandle(process_handle)

    def test_managed_export_registry_and_chunk_use_captured_source(self):
        raw = self.service.handle(request("result.raw.query", {"source": source()}))
        held = {**source(), "sourceHash": raw["sourceHash"]}
        manifest = self.service.handle(request("result.raw.export", {"sources": [held], "range": "complete"}))
        self.assertIn(manifest["exportId"], self.service._series_exports)
        chunk = self.service.handle(request("result.raw.export.chunk", {
            "exportId": manifest["exportId"], "offset": 0, "length": 65536}))
        self.assertEqual(chunk["sha256"], manifest["sha256"])
        self.assertTrue(chunk["complete"])
        self.assertFalse(self.service._read_queries)

    def test_blocked_chunk_hash_does_not_block_admission_or_control_cancel(self):
        raw = self.service.handle(request("result.raw.query", {"source": source()}))
        held = {**source(), "sourceHash": raw["sourceHash"]}
        manifest = self.service.handle(request("result.raw.export", {"sources": [held], "range": "complete"}))
        parameters = {"exportId": manifest["exportId"], "offset": 0, "length": 65536}
        entered, release, admitted = threading.Event(), threading.Event(), threading.Event()
        results, failures, prepared = [], [], []
        real_hash = series_workbench.file_hash
        def blocked_hash(path):
            if threading.current_thread().name == "blocked-export-hash":
                entered.set()
                if not release.wait(5):
                    raise AssertionError("Chunk hash barrier was not released")
            return real_hash(path)
        def read():
            try:
                results.append(self.service.handle(request("result.raw.export.chunk", parameters, "first-chunk")))
            except BaseException as error:
                failures.append(error)
        def admit_and_control():
            try:
                prepared.append(self.service.prepare_read(request("result.raw.export.chunk", parameters, "second-chunk")))
                self.service.handle(request("project.query"))
                results.append(self.service.handle(request("job.cancel", {"jobId": "live"})))
            except BaseException as error:
                failures.append(error)
            finally:
                admitted.set()
        reader = threading.Thread(target=read, name="blocked-export-hash")
        admission = threading.Thread(target=admit_and_control, name="read-admission")
        with mock.patch.object(series_workbench, "file_hash", side_effect=blocked_hash):
            reader.start()
            try:
                self.assertTrue(entered.wait(3))
                start = time.monotonic()
                admission.start()
                self.assertTrue(admitted.wait(1), "Chunk hash held the export quota lock and blocked control admission")
                self.assertLess(time.monotonic() - start, 1)
                self.assertTrue(self.service._workers["live"][1].is_set())
            finally:
                release.set()
                reader.join(5)
                if admission.ident is not None:
                    admission.join(5)
        self.assertFalse(reader.is_alive())
        self.assertFalse(admission.is_alive())
        self.assertFalse(failures, failures)
        self.assertEqual(len(prepared), 1)
        second_chunk = prepared[0].run()
        first_chunk = next(r for r in results if "data" in r)
        for chunk in (first_chunk, second_chunk):
            self.assertEqual(chunk["sha256"], manifest["sha256"])
            self.assertEqual(hashlib.sha256(base64.b64decode(chunk["data"], validate=True)).hexdigest(), manifest["sha256"])
        self.assertEqual(self.service._series_exports[manifest["exportId"]]["sources"][0]["sourceHash"], raw["sourceHash"])
        self.assertFalse(self.service._read_queries)

    def test_export_source_change_prevents_publication(self):
        raw = self.service.handle(request("result.raw.query", {"source": source()}))
        held = {**source(), "sourceHash": raw["sourceHash"]}
        view = self.service.prepare_read(request("result.raw.export", {"sources": [held], "range": "complete"}))
        real_gate = view._read_source_gate
        calls = []
        def gate():
            calls.append(True)
            if len(calls) == 2:
                self.service._state["jobs"]["job-1"]["state"] = "failed"
            real_gate()
        view._read_source_gate = gate
        self.assertCode("READ_SOURCE_CHANGED", view.run)
        self.assertGreaterEqual(len(calls), 2)
        self.assertFalse(list(self.service.workspace.glob("derived/series-exports/*.csv")))


class StdioReadLaneTest(unittest.TestCase):
    def test_actual_subprocess_out_of_order_bounded_and_controls_respond(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            harness = base / "harness.py"
            harness.write_text('''
import sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from test_read_query_lane import fixture
from test_series_workbench import VerifiedSnapshots
from range_workflow import runtime, series_sources
from range_workflow.service import WorkflowError
base = Path(sys.argv[2]); service = fixture(base)
broker = VerifiedSnapshots()
def read(view, operation, parameters):
    if operation == "snapshot" and parameters["jobId"] == "job-1":
        (base / "entered").touch()
        while not (base / "release").exists():
            if view.cancel.is_set():
                raise WorkflowError("SERVICE_CLOSED", "cancelled")
            time.sleep(.005)
    return broker(view, operation, parameters)
series_sources.query_registered = read
try:
    runtime.serve(service)
finally:
    service.close()
''', encoding="utf-8")
            process = subprocess.Popen([sys.executable, str(harness), str(Path(__file__).parent), str(base)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            responses = queue.Queue()
            reader = threading.Thread(target=lambda: [responses.put(json.loads(line)) for line in process.stdout], daemon=True)
            reader.start()
            def send(operation, parameters, rid):
                process.stdin.write(canonical(request(operation, parameters, rid)) + b"\n")
                process.stdin.flush()
            try:
                send("result.plot.query", {"view": "time", "sources": [source()]}, "slow")
                until = time.monotonic() + 5
                while not (base / "entered").exists() and time.monotonic() < until:
                    time.sleep(.005)
                self.assertTrue((base / "entered").exists())
                send("result.plot.query", {"view": "time", "sources": [source("job-2")]}, "fast")
                first = responses.get(timeout=2)
                self.assertEqual((first["requestId"], first["status"]), ("fast", "ok"))
                for i in range(20):
                    send("result.plot.query", {"view": "time", "sources": [source()]}, f"flood-{i}")
                start = time.monotonic()
                send("project.query", {}, "control")
                send("runtime.execution.query", {"jobId": "live"}, "runtime-control")
                send("job.cancel", {"jobId": "live"}, "cancel")
                seen = []
                while not {"control", "runtime-control", "cancel"} <= {r["requestId"] for r in seen}:
                    seen.append(responses.get(timeout=2))
                self.assertLess(time.monotonic() - start, 2)
                self.assertGreaterEqual(sum(r["code"] == "READ_QUERY_BUSY" for r in seen), 16)
                self.assertEqual(next(r for r in seen if r["requestId"] == "cancel")["result"]["state"], "cancelling")
                self.assertFalse(any(r["requestId"] == "slow" for r in seen))
                # EOF cancels both active and pending reads; no unbounded queue survives.
                process.stdin.close()
                process.wait(timeout=5)
                reader.join(2)
                self.assertEqual(process.returncode, 0, process.stderr.read().decode())
                remaining = []
                while not responses.empty():
                    remaining.append(responses.get_nowait())
                self.assertEqual(next(r for r in remaining if r["requestId"] == "slow")["code"], "SERVICE_CLOSED")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                process.stdout.close()
                process.stderr.close()
                if not process.stdin.closed:
                    process.stdin.close()


if __name__ == "__main__":
    unittest.main()
