"""WF-03 imported snapshot execution and resume acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient, compare_native_series
from range_workflow.model_sources import register_snapshot_source
from range_workflow.project_catalog import resolve_project_snapshot
from range_workflow.service import file_hash
from range_workflow.service import WorkflowError

ARGS = None


def _make_catalog(catalog_root: Path, snapshot: Path, model_sha: str) -> tuple[str, dict]:
    project_id, document_id = str(uuid.uuid4()), str(uuid.uuid4())
    instance_ref = {"documentId": str(uuid.uuid4()), "sessionEpoch": str(uuid.uuid4()),
                    "documentRevision": 7}
    relative = f"models/{document_id}/snapshots/{model_sha}/model.rbm"
    document = {"persistentDocumentId": document_id, "label": "Imported source snapshot",
                "sourcePath": "", "resolvedSourcePath": "", "sourceArtifacts": [],
                "originDocumentId": "", "sourceRevision": "r7", "savedArtifactHash": "",
                "dirty": True, "draftMetadata": {}, "tombstone": False,
                "modelSha256": model_sha, "snapshotSourceRevision": "r7",
                "snapshotRelativePath": relative, "snapshotInstanceRef": instance_ref,
                "orphanSnapshots": []}
    catalog = {"storageVersion": 1, "projectId": project_id,
               "documents": {document_id: document}, "sourcePathIndex": {}}
    catalog_root.mkdir(parents=True, exist_ok=True)
    (catalog_root / "project.json").write_text(json.dumps(catalog), encoding="utf-8")
    expected_snapshot = catalog_root / relative
    expected_snapshot.parent.mkdir(parents=True, exist_ok=True)
    if snapshot.resolve() != expected_snapshot.resolve():
        expected_snapshot.write_bytes(snapshot.read_bytes())
    resolved = resolve_project_snapshot(catalog_root, document_id)
    snapshot_ref = {key: resolved[key] for key in
                    ("projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef")}
    return document_id, snapshot_ref


class ImportedWorkflowIntegration(unittest.TestCase):
    def assert_native_history(self, sdk, job, source):
        result = job["result"]
        descriptor = result["nativeResults"]["active"]
        catalog = sdk.native_result(job["jobId"], "catalog", limit=2)
        self.assertEqual(catalog["artifactId"], descriptor["artifactId"])
        self.assertEqual(catalog["sourceSnapshotRef"]["modelSha256"], source["registeredModelSha256"])
        port = source["descriptor"]["ports"]["velocity"]
        axis = "XYZ"[next(i for i, value in enumerate(port["globalDirection"]) if value)]
        channel = f'node:{port["nodePhysicalId"]}:velocity:{axis}'
        channels = sdk.native_result(job["jobId"], "channels", limit=200)
        self.assertIn(channel, [item["channelId"] for item in channels["channels"]])
        samples, cursor = [], None
        while True:
            page = sdk.native_result(job["jobId"], "series", channelId=channel, limit=7,
                                     **({"cursor": cursor} if cursor else {}))
            self.assertEqual(page["nodePhysicalId"], port["nodePhysicalId"])
            samples.extend(page["samples"])
            next_cursor = page.get("nextCursor")
            if next_cursor is None:
                break
            self.assertNotEqual(next_cursor, cursor)
            cursor = next_cursor
        self.assertEqual(len(samples), catalog["frameCount"])
        replay = json.loads(Path(result["referencePath"]).read_bytes())["timeOutputs"]
        for sample in samples:
            index = round(sample["timeS"] / result["dtSeconds"])
            self.assertAlmostEqual(sample["timeS"], replay["timeSeconds"][index], places=8)
            self.assertLessEqual(abs(sample["value"] - replay["fullVelocityMPerSecond"][index]), 1e-7)
        summary = sdk.native_result(job["jobId"], "summary", channelId=channel)
        self.assertEqual(summary["sampleCount"], len(samples))
        expected_rms = math.sqrt(math.fsum(sample["value"] ** 2 for sample in samples) / len(samples))
        self.assertAlmostEqual(summary["rms"], expected_rms, places=12)
        passive = sdk.native_result(job["jobId"], "catalog", mode="passive", limit=1)
        self.assertEqual(passive["sourceKind"], "native-passive-fe")
        active_history = sdk.native_series(job["jobId"], channel)
        passive_history = sdk.native_series(job["jobId"], channel, mode="passive")
        comparison = compare_native_series(active_history, passive_history)
        self.assertEqual(len(comparison["samples"]), len(samples))
        self.assertEqual(comparison["sourceSnapshotSha256"], source["registeredModelSha256"])
        for record, active, passive in zip(comparison["samples"], active_history["samples"], passive_history["samples"]):
            self.assertEqual(record["value"], active["value"] - passive["value"])
        return channel, samples

    def test_actual_imported_snapshot_run_resume_and_frozen_source_lineage(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual imported-source FE acceptance")
        with tempfile.TemporaryDirectory(prefix="range-workflow-imported-") as temporary:
            base = Path(temporary)
            workspace = base / "workflow-workspace"
            catalog_root = base / "gui-project"
            binary_snapshot = base / "catalog-snapshot.rbm"
            converted = subprocess.run(
                [str(ARGS.native), "prepare", "--input", str(ARGS.examples / "stage9_truss4.ram"),
                 "--output", str(binary_snapshot), "--steps", "80", "--output-frequency", "1", "--dt", "0.005"],
                capture_output=True, check=False, timeout=60)
            self.assertEqual(converted.returncode, 0, converted.stderr.decode("utf-8", errors="replace"))
            payload = binary_snapshot.read_bytes()
            model_sha = hashlib.sha256(payload).hexdigest()
            document_id, snapshot_ref = _make_catalog(catalog_root, binary_snapshot, model_sha)
            source = register_snapshot_source(workspace, catalog_root, snapshot_ref, ARGS.native)
            self.assertEqual(source["descriptor"]["modelKind"], "four-rod")
            original_origin = source["originRef"]
            original_snapshot_hash = source["registeredModelSha256"]

            # The caller may edit its project after import; execution stays on the registered copy.
            gui_file = catalog_root / f"models/{document_id}/snapshots/{model_sha}/model.rbm"
            gui_file.write_bytes(b"edited after source registration")

            def client():
                return WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python,
                                      trusted_extensions=True)

            def execute(sdk, command, parameters):
                preview = sdk.preview(command, parameters)
                launched = sdk.execute(preview, sdk.approve(preview))
                completed = sdk.wait(launched["jobId"], timeout=240)
                self.assertEqual(completed["state"], "succeeded", completed.get("error"))
                return completed

            with client() as sdk:
                execute(sdk, "workflow.configure", {"dt": 0.005, "steps": 40,
                        "targetDamping": 0.1, "model": "four-rod", "sourceId": source["sourceId"]})
                first = execute(sdk, "workflow.run", {"mode": "both", "independentReference": True})
                channel, samples = self.assert_native_history(sdk, first, source)
                telemetry = sdk.request("telemetry.query", {"jobId": first["jobId"], "mode": "active", "limit": 10})
                self.assertEqual(telemetry["availability"], "available")
                self.assertGreater(len(telemetry["samples"]), 0)
                result = first["result"]
                self.assertEqual(result["modelBinding"], {
                    "kind": "native-snapshot", "usesOpenDocument": True,
                    "sourceId": source["sourceId"], "originRef": original_origin,
                    "registeredModelSha256": original_snapshot_hash,
                    "sourceManifestSha256": source["manifestSha256"]})
                self.assertEqual(result["sourceRef"]["originRef"], original_origin)
                self.assertEqual(result["sourceRef"]["registeredModelSha256"], original_snapshot_hash)
                self.assertEqual(result["sourceReference"]["sourceSha256"], original_snapshot_hash)
                self.assertEqual(result["sourceReference"]["controller"], {
                    "sensorId": "source-tip-velocity", "actuatorId": "source-tip-force"})
                for variant in ("active", "passive"):
                    self.assertLess(result[variant]["maxDisplacementErrorM"], 1e-8)
                    self.assertLess(result[variant]["maxVelocityErrorMPerSecond"], 1e-7)
                    self.assertEqual(result[variant]["preparedInputSha256"],
                                     result["preparedInputs"][variant]["sha256"])
                    self.assertEqual(file_hash(Path(result["preparedInputs"][variant]["path"])),
                                     result["preparedInputs"][variant]["sha256"])
                self.assertTrue(Path(result["sourceRef"]["jobSnapshotPath"]).is_file())
                self.assertEqual(file_hash(Path(result["sourceRef"]["jobSnapshotPath"])), original_snapshot_hash)
                original_job_id = first["jobId"]
                original_binding = result["modelBinding"]
                original_reference = result["sourceReference"]

            with client() as sdk:
                resumed = execute(sdk, "workflow.resume", {"jobId": original_job_id, "additionalSteps": 40})
                self.assert_native_history(sdk, resumed, source)
                with self.assertRaises(WorkflowError) as mismatch:
                    sdk.native_result(resumed["jobId"], artifactId=first["result"]["nativeResults"]["active"]["artifactId"])
                self.assertEqual(mismatch.exception.code, "ARTIFACT_IDENTITY_MISMATCH")
                result = resumed["result"]
                self.assertEqual(result["totalSteps"], 80)
                self.assertEqual(result["modelBinding"], original_binding)
                self.assertEqual(result["sourceReference"], original_reference)
                self.assertEqual(result["sourceRef"]["originRef"], original_origin)
                for variant in ("active", "passive"):
                    self.assertLess(result[variant]["maxDisplacementErrorM"], 1e-8)
                    self.assertLess(result[variant]["maxVelocityErrorMPerSecond"], 1e-7)
                    self.assertEqual(result[variant]["preparedInputSha256"],
                                     result["preparedInputs"][variant]["sha256"])
                    self.assertEqual(file_hash(Path(result["preparedInputs"][variant]["path"])),
                                     result["preparedInputs"][variant]["sha256"])
                execute(sdk, "workflow.configure", {"dt": 0.005, "steps": 80,
                        "targetDamping": 0.1, "sourceId": source["sourceId"]})
                continuous = execute(sdk, "workflow.run", {"mode": "both", "independentReference": True})["result"]
                for variant in ("active", "passive"):
                    for key, tolerance in (("q", 1e-8), ("v", 1e-7)):
                        self.assertEqual(len(result[variant][key]), len(continuous[variant][key]))
                        self.assertLessEqual(max(abs(a - b) for a, b in
                            zip(result[variant][key], continuous[variant][key])), tolerance)
                    self.assertEqual(continuous["sourceRef"]["originRef"], original_origin)
                    self.assertTrue(Path(result["nativeResults"][variant]["manifestPath"]).is_file())
                # Consume the same immutable native channel through a trusted extension.
                from test_extensions import manifest, tests, _stats_source
                staged = execute(sdk, "extension.stage", {"manifest": manifest("native.stats"),
                    "source": _stats_source(), "tests": tests()})["result"]
                validated = execute(sdk, "extension.validate", {"candidateId": staged["candidateId"],
                    "bundleHash": staged["bundleHash"]})["result"]
                activated = execute(sdk, "extension.activate", {"candidateId": validated["candidateId"],
                    "bundleHash": validated["bundleHash"]})["result"]
                statistics = execute(sdk, "extension.run", {"extensionId": "native.stats",
                    "bundleHash": activated["bundleHash"], "provenance": {"jobId": original_job_id,
                    "mode": "active", "artifactId": first["result"]["nativeResults"]["active"]["artifactId"],
                    "channelId": channel}})["result"]
                self.assertEqual(statistics["sourceKind"], "native-active-fe")
                self.assertAlmostEqual(statistics["metrics"]["rms"],
                    math.sqrt(math.fsum(sample["value"] ** 2 for sample in samples) / len(samples)), places=12)
                # Mutate a frame outside page zero: every immutable query must fail closed.
                native_manifest = first["result"]["nativeResults"]["active"]
                manifest_path = Path(native_manifest["manifestPath"])
                manifest_body = json.loads(manifest_path.read_bytes())
                frame_path = manifest_path.parent / manifest_body["frames"][-2]["relativePath"]
                frame_bytes = frame_path.read_bytes()
                try:
                    frame_path.write_bytes(frame_bytes + b"tampered")
                    for operation, parameters in (("catalog", {"limit": 1}), ("channels", {"limit": 1}),
                                                  ("series", {"channelId": channel, "limit": 1})):
                        with self.assertRaises(WorkflowError):
                            sdk.native_result(original_job_id, operation, **parameters)
                finally:
                    frame_path.write_bytes(frame_bytes)

    def test_permuted_zero_and_saturated_sources_run_and_resume(self):
        if not ARGS or not ARGS.solver or not ARGS.fixture:
            self.skipTest("Pass --fixture with native runtime arguments for imported fixture acceptance")
        cases = ARGS.source_case or ("rod-permuted", "beam-permuted", "rod-zero", "beam-zero",
                                     "rod-saturated", "beam-saturated")
        for kind in cases:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(prefix="range-import-case-") as temporary:
                base = Path(temporary)
                binary = base / "source.rbm"
                generated = subprocess.run([str(ARGS.fixture), kind, str(binary)],
                                           capture_output=True, timeout=30)
                self.assertEqual(generated.returncode, 0, generated.stderr)
                workspace, catalog = base / "workspace", base / "catalog"
                _, snapshot_ref = _make_catalog(catalog, binary, file_hash(binary))
                source = register_snapshot_source(workspace, catalog, snapshot_ref, ARGS.native)
                with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                    def execute(command, parameters):
                        preview = sdk.preview(command, parameters)
                        launched = sdk.execute(preview, sdk.approve(preview))
                        completed = sdk.wait(launched["jobId"], timeout=240)
                        diagnostics = completed.get("error")
                        if completed["state"] != "succeeded":
                            logs = workspace / "jobs" / launched["jobId"]
                            diagnostics = {"error": diagnostics, "logTails": {
                                path.name: path.read_text(encoding="utf-8", errors="replace")[-8000:]
                                for path in logs.glob("solve-*.log")}}
                        self.assertEqual(completed["state"], "succeeded", diagnostics)
                        return completed
                    execute("workflow.configure", {"dt": 0.0001, "steps": 4, "targetDamping": 0.1,
                        "model": source["descriptor"]["modelKind"], "sourceId": source["sourceId"]})
                    first = execute("workflow.run", {"mode": "both", "independentReference": True})
                    resumed = execute("workflow.resume", {"jobId": first["jobId"], "additionalSteps": 4})
                    execute("workflow.configure", {"dt": 0.0001, "steps": 8, "targetDamping": 0.1,
                        "sourceId": source["sourceId"]})
                    continuous = execute("workflow.run", {"mode": "both", "independentReference": True})
                    for variant in ("active", "passive"):
                        a, b = resumed["result"][variant], continuous["result"][variant]
                        self.assertLessEqual(max(abs(x - y) for x, y in zip(a["q"], b["q"])), 1e-8)
                        self.assertLessEqual(max(abs(x - y) for x, y in zip(a["v"], b["v"])), 1e-7)
                        self.assertEqual(resumed["result"]["sourceRef"], first["result"]["sourceRef"])
                    if kind.endswith("zero"):
                        self.assertEqual(first["result"]["passive"]["energyJ"], 0.0)
                        self.assertIsNone(first["result"]["controlledToPassiveEnergyRatio"])
                    if kind.endswith("saturated"):
                        replay = json.loads(Path(first["result"]["referencePath"]).read_bytes())
                        self.assertEqual(max(abs(value) for value in replay["timeOutputs"]["requestedForceN"]), 1e6)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--source-case", action="append", help="Limit fixture acceptance to named cases")
    ARGS, remaining = parser.parse_known_args()
    if ARGS.solver and not all((ARGS.native, ARGS.examples)):
        parser.error("--solver requires --native and --examples")
    unittest.main(argv=[sys.argv[0], *remaining])
