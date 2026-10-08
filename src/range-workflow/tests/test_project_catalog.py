"""Project identity interoperability, strict metadata and immutable snapshot checks."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.project_catalog import load_project_catalog, query_project_catalog, resolve_project_snapshot
from range_workflow.client import WorkflowClient
from range_workflow.service import WorkflowError, WorkflowService

CLI = None
if "--catalog-cli" in sys.argv:
    position = sys.argv.index("--catalog-cli")
    CLI = Path(sys.argv[position + 1]).resolve()
    del sys.argv[position:position + 2]


class ProjectCatalogTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-project-contract-")
        self.workspace = Path(self.temporary.name).resolve()
        self.project_id, self.document_id = str(uuid.uuid4()), str(uuid.uuid4())
        self.document = {"persistentDocumentId": self.document_id, "label": "same label",
            "sourcePath": "", "resolvedSourcePath": "", "sourceArtifacts": [],
            "originDocumentId": "", "sourceRevision": "r0", "savedArtifactHash": "",
            "dirty": True, "draftMetadata": {}, "tombstone": False, "modelSha256": "",
            "snapshotSourceRevision": "", "snapshotRelativePath": "", "snapshotInstanceRef": {},
            "orphanSnapshots": []}
        self.catalog = {"storageVersion": 1, "projectId": self.project_id,
            "documents": {self.document_id: self.document}, "sourcePathIndex": {}}
        self.write_catalog()

    def tearDown(self):
        self.temporary.cleanup()

    def write_catalog(self, value=None):
        (self.workspace / "project.json").write_text(json.dumps(value or self.catalog), encoding="utf-8")

    def cli_accepts(self):
        if CLI is None:
            return None
        run = subprocess.run([str(CLI), "--workspace", str(self.workspace), "--query"],
            capture_output=True, timeout=20)
        # Failed CLI requests diagnose on stderr; successful protocol output
        # must be a complete JSON object with no legacy diagnostics mixed in.
        if run.returncode == 0:
            json.loads(run.stdout.decode("utf-8"))
        return run.returncode == 0

    def test_same_metadata_native_cli_and_sdk_query(self):
        before = (self.workspace / "project.json").read_bytes()
        data = query_project_catalog(self.workspace)
        self.assertEqual(data["projectId"], self.project_id)
        self.assertEqual(data["documents"], [self.document])
        self.assertFalse(data["snapshotBytesVerified"])
        if CLI is not None:
            native = subprocess.run([str(CLI), "--workspace", str(self.workspace), "--query"],
                capture_output=True, timeout=20)
            self.assertEqual(native.returncode, 0, native.stderr.decode("utf-8", errors="replace"))
            response = json.loads(native.stdout.decode("utf-8"))
            payload = response.get("result", response)
            self.assertEqual(payload["projectId"], data["projectId"])
            self.assertEqual(payload["documents"], data["documents"])
        self.assertEqual((self.workspace / "project.json").read_bytes(), before)

    def test_duplicate_keys_and_unicode_label_limits_match_native(self):
        base = (self.workspace / "project.json").read_text(encoding="utf-8")
        (self.workspace / "project.json").write_text(base.replace(
            '"projectId":', '"projectId":"' + str(uuid.uuid4()) + '","projectId":', 1), encoding="utf-8")
        with self.assertRaises(WorkflowError):
            load_project_catalog(self.workspace)
        if CLI is not None:
            self.assertFalse(self.cli_accepts())
        for label in ("a" * 256, "中" * 85, "a" * 257, "中" * 86, "bad\nlabel", " "):
            with self.subTest(label_length=len(label)):
                self.document["label"] = label
                self.write_catalog()
                accepted = True
                try:
                    load_project_catalog(self.workspace)
                except WorkflowError:
                    accepted = False
                expected = len(label.encode("utf-8")) <= 256 and bool(label.strip()) and "\n" not in label
                self.assertEqual(accepted, expected)
                if CLI is not None:
                    self.assertEqual(self.cli_accepts(), accepted)

    def test_source_alias_binding_and_origin_membership(self):
        source = self.workspace / "source.rbm"
        target = self.workspace / "source-00001.rbm"
        source.write_bytes(b"link bytes")
        target.write_bytes(b"native bytes")
        artifacts = [{"path": str(path).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in (source, target)]
        self.document.update(sourcePath=artifacts[0]["path"], resolvedSourcePath=artifacts[1]["path"],
            sourceArtifacts=artifacts, savedArtifactHash=artifacts[1]["sha256"], dirty=False)
        self.catalog["sourcePathIndex"] = {(item["path"].casefold() if os.name == "nt" else item["path"]): self.document_id
            for item in artifacts}
        self.write_catalog()
        self.assertEqual(load_project_catalog(self.workspace)["documents"][self.document_id], self.document)
        self.document["originDocumentId"] = str(uuid.uuid4())
        self.write_catalog()
        with self.assertRaises(WorkflowError):
            load_project_catalog(self.workspace)
        if CLI is not None:
            self.assertFalse(self.cli_accepts())

    def test_snapshot_bytes_and_revision_are_verified_before_use(self):
        payload = b"immutable native model"
        sha = hashlib.sha256(payload).hexdigest()
        relative = f"models/{self.document_id}/snapshots/{sha}/model.rbm"
        path = self.workspace / relative
        path.parent.mkdir(parents=True)
        path.write_bytes(payload)
        self.document.update(modelSha256=sha, snapshotSourceRevision="r0", snapshotRelativePath=relative,
            snapshotInstanceRef={"documentId": str(uuid.uuid4()), "sessionEpoch": str(uuid.uuid4()), "documentRevision": 7})
        self.write_catalog()
        result = resolve_project_snapshot(self.workspace, self.document_id)
        self.assertTrue(result["snapshotBytesVerified"])
        self.assertEqual(result["modelSha256"], sha)
        path.write_bytes(b"tampered model")
        with self.assertRaisesRegex(WorkflowError, "hash changed"):
            resolve_project_snapshot(self.workspace, self.document_id)
        path.write_bytes(payload)
        self.document["sourceRevision"] = "r1"
        self.write_catalog()
        with self.assertRaises(WorkflowError) as failed:
            resolve_project_snapshot(self.workspace, self.document_id)
        self.assertEqual(failed.exception.code, "STALE_SOURCE")

    def test_workflow_adopts_same_project_and_rejects_foreign_catalog(self):
        examples = self.workspace / "examples"
        examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (examples / name).write_text("fixture", encoding="utf-8")
        service = WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), examples, Path(sys.executable))
        try:
            value = service.project_snapshot()
            self.assertEqual(value["projectId"], self.project_id)
            self.assertEqual(value["catalog"]["projectId"], self.project_id)
            self.assertFalse(value["modelBinding"]["usesOpenDocument"])
        finally:
            service.close()
        self.catalog["projectId"] = str(uuid.uuid4())
        self.write_catalog()
        state_before = (self.workspace / "workflow-state.json").read_bytes()
        with self.assertRaises(WorkflowError) as failed:
            WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), examples, Path(sys.executable))
        self.assertEqual(failed.exception.code, "PROJECT_ID_CONFLICT")
        self.assertEqual((self.workspace / "workflow-state.json").read_bytes(), state_before)

    def test_workflow_client_process_reads_and_verifies_project_snapshot(self):
        payload = b"opaque project snapshot fixture"
        sha = hashlib.sha256(payload).hexdigest()
        relative = f"models/{self.document_id}/snapshots/{sha}/model.rbm"
        snapshot_path = self.workspace / relative
        snapshot_path.parent.mkdir(parents=True)
        snapshot_path.write_bytes(payload)
        self.document.update(modelSha256=sha, snapshotSourceRevision="r3", sourceRevision="r3",
            snapshotRelativePath=relative,
            snapshotInstanceRef={"documentId": str(uuid.uuid4()), "sessionEpoch": str(uuid.uuid4()),
                                 "documentRevision": 23})
        self.write_catalog()
        manifest_before = (self.workspace / "project.json").read_bytes()

        examples = self.workspace / "examples"
        examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (examples / name).write_text("fixture", encoding="utf-8")

        def service_request(service, operation, parameters=None):
            return service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                "operation": operation, "parameters": parameters or {}})

        native_catalog = native_document = None
        if CLI is not None:
            native = subprocess.run([str(CLI), "--workspace", str(self.workspace), "--query"],
                capture_output=True, timeout=20)
            self.assertEqual(native.returncode, 0, native.stderr.decode("utf-8", errors="replace"))
            native_catalog = json.loads(native.stdout.decode("utf-8"))
            native_doc = subprocess.run([str(CLI), "--workspace", str(self.workspace),
                "--document", self.document_id], capture_output=True, timeout=20)
            self.assertEqual(native_doc.returncode, 0, native_doc.stderr.decode("utf-8", errors="replace"))
            native_document = json.loads(native_doc.stdout.decode("utf-8"))

        service = WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), examples,
                                  Path(sys.executable))
        try:
            service_catalog = service_request(service, "project.catalog.query")
            service_selected = service_request(service, "project.catalog.query",
                {"persistentDocumentId": self.document_id})
            service_snapshot = service_request(service, "project.snapshot.query",
                {"persistentDocumentId": self.document_id})
            service_capabilities = service_request(service, "capabilities.query")
        finally:
            service.close()

        # WorkflowClient sets PYTHONPATH to the package root derived from its own module.
        with WorkflowClient(self.workspace, Path(sys.executable), Path(sys.executable), examples,
                            python=sys.executable, timeout=20) as client:
            client_catalog = client.project_catalog()
            client_selected = client.project_catalog(self.document_id)
            client_snapshot = client.project_snapshot(self.document_id)
            client_capabilities = client.request("capabilities.query")

            self.assertEqual(client_catalog, service_catalog)
            self.assertEqual(client_selected, service_selected)
            self.assertEqual(client_capabilities, service_capabilities)
            self.assertEqual(client_selected["documents"], [client_catalog["documents"][0]])
            self.assertEqual(client_snapshot, service_snapshot)
            self.assertEqual(client_snapshot["projectId"], self.project_id)
            self.assertEqual(client_snapshot["persistentDocumentId"], self.document_id)
            self.assertEqual(client_snapshot["sourceRevision"], "r3")
            self.assertEqual(client_snapshot["modelSha256"], sha)
            self.assertTrue(client_snapshot["snapshotBytesVerified"])

            operations = {item["id"]: item for item in client_capabilities["operations"]}
            self.assertTrue(operations["project.catalog.query"]["readOnly"])
            self.assertTrue(operations["project.snapshot.query"]["readOnly"])

            metadata_views = [service_catalog["documents"][0], client_catalog["documents"][0],
                              service_selected["documents"][0], client_selected["documents"][0]]
            if native_document is not None:
                metadata_views.append(native_document["document"])
                for field in ("protocolVersion", "storageVersion", "projectId", "documents", "sourcePathIndex"):
                    self.assertEqual(native_catalog[field], service_catalog[field])
                native_capability = native_catalog["capability"]
                self.assertEqual(native_capability["protocolVersion"], "range.project/v1")
                self.assertEqual(native_capability["unitConvention"], client_catalog["unitConvention"])
                self.assertIn("project.query", native_capability["readOnlyOperations"])
                self.assertIn("document.query", native_capability["readOnlyOperations"])
                for field in ("persistentDocumentId", "sourceRevision", "modelSha256",
                              "snapshotSourceRevision", "snapshotRelativePath", "snapshotInstanceRef"):
                    self.assertEqual(native_document["document"][field], service_selected["documents"][0][field])

            # Optional physical-unit or model-capability fields must agree when supplied;
            # the project catalog does not invent them when this fixture has none.
            for field in ("units", "unitSystem", "modelCapabilities", "analysisCapabilities"):
                if any(field in view for view in metadata_views):
                    self.assertTrue(all(field in view for view in metadata_views), field)
                    for view in metadata_views[1:]:
                        self.assertEqual(view[field], metadata_views[0][field])

            snapshot_path.write_bytes(b"tampered snapshot fixture")
            with self.assertRaises(WorkflowError) as failed:
                client.project_snapshot(self.document_id)
            self.assertEqual(failed.exception.code, "SOURCE_CHANGED")

        self.assertEqual((self.workspace / "project.json").read_bytes(), manifest_before)

    def test_malformed_rpc_selectors_are_rejected_and_audited(self):
        examples = self.workspace / "examples"
        examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (examples / name).write_text("fixture", encoding="utf-8")
        service = WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable), examples, Path(sys.executable))
        try:
            before = len(service.state_snapshot()["audit"])
            count = 0
            for operation in ("project.catalog.query", "project.snapshot.query"):
                for invalid in (None, [], {}, 4, True, "not-a-uuid"):
                    with self.subTest(operation=operation, invalid_type=type(invalid).__name__):
                        with self.assertRaises(WorkflowError) as failed:
                            service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                                "operation": operation, "parameters": {"persistentDocumentId": invalid}})
                        self.assertEqual(failed.exception.code, "INVALID_PARAMETERS")
                        count += 1
            records = service.state_snapshot()["audit"]
            self.assertEqual(len(records), before + count)
            self.assertTrue(all(record["record"]["commandId"] == "request.reject" for record in records[before:]))
        finally:
            service.close()


if __name__ == "__main__":
    unittest.main()
