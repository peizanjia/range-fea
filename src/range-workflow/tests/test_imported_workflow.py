"""WF-03 imported snapshot execution and resume acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source
from range_workflow.project_catalog import resolve_project_snapshot
from range_workflow.service import file_hash

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
                return WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)

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
