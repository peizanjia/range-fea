"""WF11 lifecycle: real frozen worker, immutable registry and v1 compatibility."""
import copy
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.service import WorkflowError, WorkflowService
from range_workflow import scientific_extensions as scientific
from range_workflow import scientific_contract as contract
from range_workflow.scientific_reference import reference_result
from test_extensions import request, run_command, wait


def manifest(version="1.0.0"):
    return {"manifestVersion": 2, "extensionId": "spectral-test", "version": version,
        "sdkVersion": scientific.PROTOCOL_VERSION, "kind": scientific.KIND, "entryPoint": "extension.py:process",
        "compatibility": {"workflowApi": "range.workflow/v1", "inputVersion": 2, "outputVersion": 2},
        "dependencies": [{"name": n, "version": contract.DEPENDENCIES[n]} for n in ("numpy", "scipy")],
        "capabilities": ["numeric.ndarray.real64", "numeric.fft.rfft", "signal.lowpass1"],
        "inputSchema": contract.INPUT_SCHEMA, "outputSchema": contract.OUTPUT_SCHEMA, "gui": scientific.GUI}


class ScientificLifecycle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.examples = self.root / "examples"
        self.examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (self.examples / name).write_text("fixture", encoding="utf-8")
        project = Path(__file__).resolve().parents[3]
        self.python = Path(os.environ.get("RANGE_TEST_PRIVATE_PYTHON",
                                         str(project / "build/private-python-v2/python.exe")))
        if not self.python.is_file():
            self.fail("Mandatory frozen private scientific runtime is unavailable")
        self.workspace = self.root / "workspace"
        self.service = self.open()

    def open(self):
        return WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), self.examples,
                               self.python, trusted_extensions=True)

    def preverify_scientific_runtime(self, service, timeout=180):
        ack = request(service, "extension.runtime.verify", {
            "sdkVersion": scientific.PROTOCOL_VERSION, "force": True})
        self.assertEqual(ack["state"], "queued")
        self.assertTrue(ack["verificationId"])
        deadline = time.monotonic() + timeout
        while True:
            receipt = request(service, "extension.runtime.verification.query", {
                "verificationId": ack["verificationId"]})
            if receipt["state"] == "ready":
                self.assertEqual(receipt["sdkVersion"], scientific.PROTOCOL_VERSION)
                self.assertEqual(receipt["profile"], "packaged")
                self.assertEqual(receipt["currentity"], "current")
                self.assertTrue(receipt["isCurrent"])
                self.assertEqual(receipt["currentityScope"], "environment-stamp")
                self.assertTrue(receipt["fullFileVerify"]["verifiedFiles"])
                self.assertEqual(receipt["fullFileVerify"]["runtimeHash"], receipt["runtimeHash"])
                self.assertEqual(receipt["fullFileVerify"]["environmentStamp"],
                                 receipt["expectedEnvironmentStamp"])
                self.assertRegex(receipt["runtimeHash"], r"^[0-9a-f]{64}$")
                return receipt
            if receipt["state"] in {"failed", "cancelled"}:
                self.fail(f"Scientific runtime preverification failed: {receipt.get('error')}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.fail(f"Scientific runtime preverification did not become ready within {timeout} seconds")
            time.sleep(min(0.1, remaining))

    def tearDown(self):
        self.service.close()
        self.temporary.cleanup()

    def command(self, command, parameters, success=True):
        launched = run_command(self.service, command, parameters)
        job = wait(self.service, launched["jobId"], timeout=90)
        self.assertEqual(job["state"], "succeeded" if success else "failed", job.get("error"))
        return job

    def stage(self, version, source=None):
        data = scientific.golden_cases()[0]["input"]
        # The small-N fixture expected values use the independently authored
        # host DFT, while the actual candidate computes NumPy/SciPy results.
        tests = {"cases": [{"id": "candidate-dc", "input": data,
                            "expected": reference_result(data, small_n=True)}]}
        candidate = source or Path(scientific.__file__).with_name("scientific_example.py").read_text(encoding="utf-8")
        return self.command("extension.stage", {"manifest": manifest(version), "source": candidate,
                                                "tests": tests})["result"]

    def test_actual_private_validation_activation_reopen_failure_preserves_active(self):
        self.preverify_scientific_runtime(self.service)
        staged = self.stage("1.0.0")
        checked = self.command("extension.validate", {k: staged[k] for k in ("candidateId", "bundleHash")})["result"]
        self.assertEqual(checked["validation"]["independentCaseCount"], 4)
        self.command("extension.activate", {k: staged[k] for k in ("candidateId", "bundleHash")})
        self.service.close()
        self.service = self.open()
        self.preverify_scientific_runtime(self.service)
        active = request(self.service, "extension.list", {"extensionId": "spectral-test"})["extensions"][0]
        self.assertEqual(active["activeBundleHash"], staged["bundleHash"])
        bad = self.stage("2.0.0", "def process(data):\n    raise RuntimeError('candidate failure')\n")
        failed = self.command("extension.validate", {k: bad[k] for k in ("candidateId", "bundleHash")}, success=False)
        self.assertIsNotNone(failed["error"])
        active = request(self.service, "extension.list", {"extensionId": "spectral-test"})["extensions"][0]
        self.assertEqual(active["activeBundleHash"], staged["bundleHash"])

    def test_unknown_dependencies_gui_and_protocol_rejected(self):
        for mutation in (lambda m: m["dependencies"][0].update(version="latest"),
                         lambda m: m.update(gui={"script": "run()"}),
                         lambda m: m.update(kind="unknown-science")):
            m = copy.deepcopy(manifest())
            mutation(m)
            with self.assertRaises(WorkflowError):
                scientific.validate_manifest(m)


class ScientificArtifactPublication(unittest.TestCase):
    def test_result_replaced_during_oracle_is_rejected(self):
        data = scientific.golden_cases()[0]["input"]
        output = reference_result(data, small_n=True)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "scientific-result.json"
            from range_workflow.service import canonical, file_hash
            path.write_bytes(canonical(output))
            original_sha = file_hash(path)
            binding = {"sourcePath": "extensions/candidate.py", "sourceSha256": "a"*64,
                "manifestSha256": "b"*64, "testsSha256": "c"*64, "runtimeHash": "d"*64,
                "seriesSha256": contract.canonical_hash(data), "version": "1.0.0"}
            p = {"extensionId": "spectral-test", "bundleHash": "e"*64, "runtimeHash": "d"*64,
                 "series": data, "provenance": {"sourceKind": "verified-multichannel"}}
            def alter_artifact(*args, **kwargs):
                path.write_bytes(b'{"unverifiedReplacement":true}')
                return contract.verification_profile()
            with mock.patch("range_workflow.extensions._version", return_value=({}, binding)), \
                 mock.patch("range_workflow.extensions._source_record"), \
                 mock.patch.object(scientific, "revalidate_input", return_value=data), \
                 mock.patch.object(scientific, "execute_source", return_value=(output, original_sha)), \
                 mock.patch.object(scientific, "verify_private", side_effect=alter_artifact), \
                 mock.patch.object(scientific, "runtime_identity", return_value={"runtimeHash": "d"*64}):
                with self.assertRaises(WorkflowError) as rejected:
                    scientific.execute_run(SimpleNamespace(workspace=root),
                        {"parameters": p, "_extensionBinding": binding}, root, None)
                self.assertEqual(rejected.exception.code, "EXTENSION_TAMPERED")
            self.assertFalse((root / "input-provenance.json").exists())


if __name__ == "__main__":
    unittest.main()
