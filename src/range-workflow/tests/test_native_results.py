"""Registered native result boundaries before a broker process can run."""
from pathlib import Path
import copy
import os
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.native_results import query_registered, resolve_series, _run_query_process
from range_workflow.service import WorkflowError, file_hash
from range_workflow import WorkflowClient, compare_native_series


class NativeResultBoundaryTest(unittest.TestCase):
    def test_compare_requires_same_complete_native_physical_history(self):
        left = {"coverageComplete": True, "completed": True, "sourceKind": "native-active-fe",
                "sourceSnapshotRef": {"modelSha256": "a" * 64}, "channelId": "node:51000:velocity:X",
                "unit": "m/s", "axis": "X", "nodePhysicalId": 51000, "variableId": "var-velocity",
                "artifactId": "active", "samples": [{"recordNo": 1, "timeS": 0.1, "value": 2.0},
                                                        {"recordNo": 2, "timeS": 0.2, "value": -3.0}]}
        right = copy.deepcopy(left)
        right.update(sourceKind="native-passive-fe", artifactId="passive")
        right["samples"][1]["value"] = -1.0
        result = compare_native_series(left, right)
        self.assertEqual(result["maxAbsoluteDifference"], 2.0)
        self.assertEqual([sample["value"] for sample in result["samples"]], [0.0, -2.0])
        invalid = (("coverageComplete", False), ("completed", False), ("sourceKind", "reference-newmark"),
                   ("sourceSnapshotRef", {"modelSha256": "b" * 64}), ("unit", "rad/s"), ("axis", "Y"),
                   ("nodePhysicalId", 9000), ("channelId", "node:9000:velocity:X"),
                   ("samples", [{"recordNo": 1, "timeS": 0.1, "value": 1.0}]),
                   ("samples", [{"recordNo": 1, "timeS": 0.11, "value": 1.0}, right["samples"][1]]))
        for field, value in invalid:
            with self.subTest(field=field):
                broken = copy.deepcopy(right)
                broken[field] = value
                with self.assertRaises(WorkflowError) as caught:
                    compare_native_series(left, broken)
                self.assertEqual(caught.exception.code, "INCOMPARABLE_RESULTS")

    def test_sdk_native_series_pins_artifact_across_pages_and_reports_actual_coverage(self):
        sdk = object.__new__(WorkflowClient)
        catalog = {"artifactId": "native-artifact", "frameCount": 2,
                   "sourceSnapshotRef": {"modelSha256": "a" * 64}, "proofScope": "declared"}
        shared = {"artifactId": "native-artifact", "completed": True, "sourceKind": "native-active-fe"}
        pages = [{**shared, "samples": [{"recordNo": 1, "timeS": 0.1, "value": 2.0}], "nextCursor": "page-two"},
                 {**shared, "samples": [{"recordNo": 2, "timeS": 0.2, "value": -3.0}]}]
        sdk.native_result = mock.Mock(side_effect=copy.deepcopy([catalog, *pages]))
        history = sdk.native_series(self.job_id, "node:51000:velocity:X")
        self.assertTrue(history["coverageComplete"])
        self.assertTrue(history["completed"])
        self.assertEqual(len(history["samples"]), 2)
        self.assertEqual(history["sourceSnapshotRef"], catalog["sourceSnapshotRef"])
        self.assertEqual(sdk.native_result.call_args_list[2].kwargs["cursor"], "page-two")
        self.assertEqual(sdk.native_result.call_args_list[2].kwargs["artifactId"], "native-artifact")
        sdk.native_result = mock.Mock(side_effect=copy.deepcopy([{**catalog, "frameCount": 3}, *pages]))
        self.assertFalse(sdk.native_series(self.job_id, "node:51000:velocity:X")["coverageComplete"])

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-native-result-boundary-")
        self.workspace = Path(self.temporary.name)
        self.job_id = "native-job"
        self.manifest = self.workspace / "jobs" / self.job_id / "active" / "native-result.manifest.json"
        self.manifest.parent.mkdir(parents=True)
        self.manifest.write_bytes(b"{}")
        self.descriptor = {"manifestPath": str(self.manifest), "manifestSha256": file_hash(self.manifest),
                           "artifactId": "native-artifact"}
        self.job = {"state": "succeeded", "result": {"nativeResults": {"active": self.descriptor}},
                    "artifacts": [{"path": str(self.manifest), "sha256": self.descriptor["manifestSha256"]}]}
        self.service = SimpleNamespace(workspace=self.workspace, native_results=Path(sys.executable),
                                       _job=lambda _: self.job)

    def tearDown(self):
        self.temporary.cleanup()

    def reject(self, expected, **parameters):
        with mock.patch("range_workflow.native_results.subprocess.Popen") as run:
            with self.assertRaises(WorkflowError) as caught:
                query_registered(self.service, "catalog", {"jobId": self.job_id, **parameters})
            self.assertEqual(caught.exception.code, expected)
            run.assert_not_called()

    def test_unfinished_jobs_cannot_claim_immutable_native_results(self):
        for state in ("queued", "running", "failed", "cancelled", "interrupted"):
            with self.subTest(state=state):
                self.job["state"] = state
                self.reject("RESULT_UNAVAILABLE")

    def test_other_artifact_identity_and_missing_manifest_ledger_are_rejected(self):
        self.reject("ARTIFACT_IDENTITY_MISMATCH", artifactId="another-job-artifact")
        self.job["artifacts"] = []
        self.reject("ARTIFACT_IDENTITY_MISMATCH")

    def test_manifest_tampering_and_path_escape_are_rejected_before_execution(self):
        self.manifest.write_bytes(b'{"tampered":true}')
        self.reject("SOURCE_CHANGED")
        self.descriptor["manifestPath"] = str(self.workspace / "foreign.manifest.json")
        self.reject("ARTIFACT_PATH")

    def test_snapshot_checks_registered_identity_without_second_catalog_process(self):
        snapshot = {"valid": True, "artifactId": self.descriptor["artifactId"], "jobId": self.job_id,
                    "sourceKind": "native-active-fe", "sourceSnapshotRef": {"modelSha256": "a" * 64}}
        arguments = {"channelIds": ["node:51000:velocity:X"]}
        with mock.patch("range_workflow.native_results.NativeResultsClient.query", return_value=snapshot) as query:
            result = query_registered(self.service, "snapshot", {"jobId": self.job_id, **arguments})
            query.assert_called_once_with("snapshot", arguments)
            self.assertTrue(result["completed"])
        for field in ("artifactId", "jobId"):
            with self.subTest(field=field):
                changed = {**snapshot, field: "foreign"}
                with mock.patch("range_workflow.native_results.NativeResultsClient.query", return_value=changed):
                    with self.assertRaises(WorkflowError) as caught:
                        query_registered(self.service, "snapshot", {"jobId": self.job_id, **arguments})
                    self.assertEqual(caught.exception.code, "ARTIFACT_IDENTITY_MISMATCH")

    def test_query_reader_is_assigned_suspended_and_closed_after_timeout(self):
        child = mock.Mock()
        child.wait.side_effect = [subprocess.TimeoutExpired("reader", 1), -1]
        child.poll.return_value = None
        job = mock.Mock()
        with mock.patch("range_workflow.native_results.os.name", "nt"), \
                mock.patch("range_workflow.native_results.subprocess.Popen", return_value=child) as start, \
                mock.patch("range_workflow.native_results._WindowsJob", return_value=job) as assign:
            with self.assertRaises(subprocess.TimeoutExpired):
                _run_query_process(["reader"], mock.sentinel.output, mock.sentinel.errors, 1)
            self.assertEqual(start.call_args.kwargs["creationflags"], 0x4 | 0x08000000)
            assign.assert_called_once_with(child)
            job.close.assert_called_once()
            child.kill.assert_called_once()
            self.assertEqual(child.wait.call_count, 2)

    def test_reader_assignment_failure_kills_unresumed_process(self):
        child = mock.Mock()
        child.poll.return_value = None
        with mock.patch("range_workflow.native_results.os.name", "nt"), \
                mock.patch("range_workflow.native_results.subprocess.Popen", return_value=child), \
                mock.patch("range_workflow.native_results._WindowsJob", side_effect=OSError("assignment failed")):
            with self.assertRaises(OSError):
                _run_query_process(["reader"], mock.sentinel.output, mock.sentinel.errors, 1)
            child.kill.assert_called_once()
            child.wait.assert_called_once_with()

    @unittest.skipUnless(os.name == "nt", "Windows kill-on-job-close contract")
    def test_reader_dies_when_query_host_is_terminated(self):
        import ctypes
        from ctypes import wintypes
        marker = self.workspace / "reader.pid"
        reader = f"import os,time; from pathlib import Path; Path({str(marker)!r}).write_text(str(os.getpid())); time.sleep(30)"
        helper = (f"import sys,tempfile; sys.path.insert(0,{str(Path(__file__).resolve().parents[1] / 'python')!r}); "
                  "from range_workflow.native_results import _run_query_process; "
                  f"_run_query_process([{sys.executable!r},'-c',{reader!r}],tempfile.TemporaryFile(),tempfile.TemporaryFile(),30)")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        host = subprocess.Popen([sys.executable, "-c", helper], creationflags=0x08000000,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        handle = None
        try:
            deadline = time.monotonic() + 5
            reader_pid = None
            while host.poll() is None and time.monotonic() < deadline:
                try:
                    reader_pid = int(marker.read_text())
                    break
                except (OSError, ValueError):
                    pass
                time.sleep(0.01)
            self.assertIsNotNone(reader_pid, "Query reader did not start in its assigned job")
            handle = kernel.OpenProcess(0x100000 | 0x1, False, reader_pid)
            self.assertTrue(handle)
            host.kill()
            host.wait(timeout=5)
            self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0,
                             "Reader survived host termination")
        finally:
            if host.poll() is None:
                host.kill()
            host.wait(timeout=5)
            if handle:
                kernel.TerminateProcess(handle, 1)
                kernel.CloseHandle(handle)

    def test_extension_adapter_retains_physical_channel_and_native_origin_across_pages(self):
        origin = {"jobId": self.job_id, "mode": "passive", "artifactId": "native-artifact",
                  "channelId": "node:51000:velocity:X"}
        shared = {"unit": "m/s", "sourceKind": "native-passive-fe", "manifestSha256": "a" * 64,
                  "sourceSnapshotRef": {"modelSha256": "b" * 64}}
        pages = [{**shared, "samples": [{"recordNo": 1, "timeS": 0.1, "value": 2.0,
                    "originKind": "current-job", "origin": {"jobId": self.job_id}}], "nextCursor": "page-two"},
                 {**shared, "samples": [{"recordNo": 2, "timeS": 0.2, "value": -3.0,
                    "originKind": "current-job", "origin": {"jobId": self.job_id}}]}]
        with mock.patch("range_workflow.native_results.query_registered", side_effect=pages) as query:
            series, provenance = resolve_series(self.service, origin)
        self.assertEqual(series, {"times": [0.1, 0.2], "values": [2.0, -3.0],
                                  "timeUnit": "s", "valueUnit": "m/s"})
        for key, value in {**origin, "sourceKind": "native-passive-fe", "artifactSha256": "a" * 64,
                           "sourceSnapshotRef": shared["sourceSnapshotRef"], "sampleCount": 2,
                           "frameOriginCounts": {"current-job": 2}}.items():
            self.assertEqual(provenance[key], value)
        self.assertEqual(query.call_args_list[1].args[2]["cursor"], "page-two")


if __name__ == "__main__":
    unittest.main()
