"""Protocol tests: mocks check transport bindings, never prove native mesh quality."""
import hashlib
from contextlib import nullcontext
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.mesh_client import MeshClient, MeshClientError
from range_workflow.process import ProcessFailure

WORKER = None
SOURCE = None
OUTPUT = None
for flag in ("--worker", "--source", "--output"):
    if flag in sys.argv:
        position = sys.argv.index(flag)
        value = Path(sys.argv[position + 1]).absolute()
        if flag == "--worker":
            WORKER = value
        elif flag == "--source":
            SOURCE = value
        else:
            OUTPUT = value
        del sys.argv[position:position + 2]


class MeshClientTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.worker = self.root / "fixed worker.exe"
        self.worker.write_bytes(b"mock worker bytes")
        self.source = self.root / "source ; literal.rbm"
        self.source.write_bytes(b"mock native source - not a MODEL quality fixture")
        self.client = MeshClient(self.worker, self.root)
        self.calls = []

    def protocol(self, argv, directory, label, cancel, timeout):
        self.calls.append(argv)
        self.assertEqual(argv[0], str(self.worker))
        self.assertEqual(argv[2], "--request")
        request = json.loads(Path(argv[3]).read_bytes())
        if argv[1] == "prepare":
            for field in ("sourceSnapshotPath", "workerPath", "jobDirectory"):
                self.assertNotIn("\\", request[field])
            for field in ("persistentModelPath", "runtimeSnapshotPath"):
                self.assertNotIn("\\", request["runtimeContext"][field])
            self.assertEqual(request["runtimeContext"]["authority"], "file-owner")
            self.assertIsNone(request["runtimeContext"]["sourceRuntimeStamp"])
            self.assertEqual(Path(request["sourceSnapshotPath"]).read_bytes(), self.source.read_bytes())
            frozen = dict(request, requestSha256="a" * 64)
            path = Path(request["jobDirectory"]) / "request.json"
            path.parent.mkdir()
            path.write_text(json.dumps(frozen), encoding="utf-8")
            descriptor = {"attemptId": request["attemptId"], "requestSha256": "a" * 64,
                          "requestPath": str(path), "accepted": False, "requiresRebinding": True}
            code = "prepared"
        else:
            source = Path(request["runtimeContext"]["persistentModelPath"])
            if hashlib.sha256(source.read_bytes()).hexdigest() != request["runtimeContext"]["persistentModelSha256"]:
                return json.dumps({"valid": False, "code": "context_changed", "message": "Source changed"}).encode()
            descriptor = {"attemptId": request["attemptId"], "requestSha256": request["requestSha256"],
                          "accepted": False, "requiresRebinding": True}
            code = "candidate_ready"
        return ("native diagnostics\n" + json.dumps({"valid": True, "code": code,
                                                    "descriptor": descriptor}) + "\n").encode()

    def test_fresh_frozen_attempts_fixed_argv_and_query(self):
        with patch.object(self.client._runner, "run", side_effect=self.protocol):
            first = self.client.prepare(self.source, max_volume=.1)
            second = self.client.prepare(self.source, max_volume=.1)
            self.assertNotEqual(first.attempt_id, second.attempt_id)
            completed = self.client.run(first)
            self.assertEqual([call[1] for call in self.calls], ["prepare", "prepare", "run", "query"])
            self.assertFalse(completed.accepted)
            self.assertTrue(completed.requires_rebinding)
            descriptor = completed.descriptor
            descriptor["accepted"] = True
            self.assertFalse(completed.descriptor["accepted"])

    def test_changed_original_is_native_rejection_and_no_replay(self):
        with patch.object(self.client._runner, "run", side_effect=self.protocol):
            job = self.client.prepare(self.source, max_volume=.1)
            self.source.write_bytes(b"changed")
            with self.assertRaises(MeshClientError) as rejected:
                self.client.query(job)
            self.assertEqual(rejected.exception.code, "context_changed")
            self.assertEqual([call[1] for call in self.calls], ["prepare", "query"])

    def test_failure_never_returns_candidate(self):
        for failure in (ProcessFailure("TIMEOUT", "timeout"), ProcessFailure("CANCELLED", "cancelled"),
                        ProcessFailure("PROCESS_FAILED", "failed")):
            with patch.object(self.client._runner, "run", side_effect=failure):
                with self.assertRaises(MeshClientError):
                    self.client.prepare(self.source, max_volume=.1)

    def test_nonzero_native_rejection_preserves_code_and_log(self):
        def reject(argv, directory, label, cancel, timeout):
            (directory / "mesh-worker.log").write_bytes(
                b'diagnostics\n{"valid":false,"code":"invalid_runtime_context","message":"Bound context rejected"}\n')
            raise ProcessFailure("PROCESS_FAILED", "exit 3")
        with patch.object(self.client._runner, "run", side_effect=reject):
            with self.assertRaises(MeshClientError) as failure:
                self.client.prepare(self.source, max_volume=.1)
            self.assertEqual(failure.exception.code, "invalid_runtime_context")
            self.assertTrue(failure.exception.log_path.is_file())

    def test_nonzero_success_record_never_promoted(self):
        def reject(argv, directory, label, cancel, timeout):
            (directory / "mesh-worker.log").write_bytes(b'{"valid":true,"code":"candidate_ready"}\n')
            raise ProcessFailure("PROCESS_FAILED", "exit 3")
        with patch.object(self.client._runner, "run", side_effect=reject):
            with self.assertRaises(MeshClientError) as failure:
                self.client.prepare(self.source, max_volume=.1)
            self.assertEqual(failure.exception.code, "PROCESS_FAILED")

    def test_invalid_output_and_multiple_records_rejected(self):
        for output in (b"garbage", b"{}\n{}\n", b'{"valid":true}\ntrailing diagnostics'):
            with patch.object(self.client._runner, "run", return_value=output):
                with self.assertRaises(MeshClientError):
                    self.client.prepare(self.source, max_volume=.1)

    def test_bounds_and_no_arbitrary_options(self):
        for volume in (True, float("nan"), float("inf"), 0, 1e19, "1"):
            with self.assertRaises(ValueError):
                self.client.prepare(self.source, max_volume=volume)
        with self.assertRaises(TypeError):
            self.client.prepare(self.source, max_volume=.1, tetgen="; arbitrary")
        with self.assertRaises(ValueError):
            MeshClient(Path("worker.exe"), self.root)

    def test_frozen_context_tampering_stops_before_launch(self):
        with patch.object(self.client._runner, "run", side_effect=self.protocol):
            job = self.client.prepare(self.source, max_volume=.1)
            frozen = json.loads(job.request_path.read_bytes())
            frozen["runtimeContext"]["authority"] = "gui-captured-document"
            job.request_path.write_text(json.dumps(frozen), encoding="utf-8")
            with self.assertRaises(ValueError):
                self.client.run(job)
            self.assertEqual(len(self.calls), 1)


@unittest.skipUnless(WORKER and SOURCE, "Explicit native worker and closed tetrahedron MODEL fixture required")
class NativeMeshClientTests(unittest.TestCase):
    def test_actual_cli_prepare_run_query_and_tamper(self):
        if OUTPUT:
            retained = OUTPUT / ("mesh-sdk-" + str(uuid.uuid4()))
            retained.mkdir(parents=True)
            workspace = nullcontext(str(retained))
        else:
            workspace = tempfile.TemporaryDirectory(prefix="range-mesh-sdk-")
        with workspace as temporary:
            root = Path(temporary).resolve()
            source = root / SOURCE.name
            shutil.copyfile(SOURCE, source)
            client = MeshClient(WORKER, root, timeout=60)
            prepared = client.prepare(source, max_volume=.1)
            completed = client.run(prepared)
            self.assertEqual(completed.descriptor["status"], "candidate_ready")
            self.assertFalse(completed.accepted)
            self.assertTrue(completed.requires_rebinding)
            report = Path(completed.descriptor["reportPath"])
            report_bytes = report.read_bytes()
            report.write_bytes(report_bytes + b" ")
            with self.assertRaises(MeshClientError):
                client.query(prepared)
            report.write_bytes(report_bytes)
            source.write_bytes(source.read_bytes() + b" ")
            with self.assertRaises(MeshClientError):
                client.query(prepared)


if __name__ == "__main__":
    unittest.main()
