"""Trust-boundary tests for the immutable native model source registry."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import model_sources
from range_workflow.project_catalog import resolve_project_snapshot
from range_workflow.service import WorkflowError


def valid_descriptor(source_sha: str) -> dict:
    node_ids = [7118, 7135, 7152, 7169, 7186]
    nodes = [{"position": index, "physicalId": physical_id, "coordinates": [float(index), 0.0, 0.0],
              "initialDisplacementM": [0.0, 0.0, 0.0], "initialVelocityMS": [0.0, 0.0, 0.0],
              "initialRotationRad": [0.0, 0.0, 0.0], "initialAngularVelocityRadS": [0.0, 0.0, 0.0]}
             for index, physical_id in enumerate(node_ids)]
    material = {"youngModulusPa": 2.0e5, "densityKgM3": 1000.0, "poissonRatio": 0.3}
    section = {"areaM2": 0.01, "iyM4": 0.0, "izM4": 0.0, "torsionM4": 0.0,
               "wyM3": None, "wzM3": None, "wtM3": None}
    elements = [{"type": "truss1", "physicalId": 8000 + index,
                 "nodePhysicalIds": [node_ids[index], node_ids[index + 1]],
                 "material": material, "section": section}
                for index in range(4)]
    point_elements = [{"physicalId": 9000 + index,
                       "nodePhysicalId": node_ids[0 if index < 3 else -1]}
                      for index in range(6)]
    free_dofs = [{"nodePhysicalId": node_ids[index], "physicalDof": "UZ",
                  "globalDirection": [1.0, 0.0, 0.0], "unit": "m"}
                 for index in range(1, 5)]
    supports = [{"nodePhysicalId": node_ids[0], "physicalDof": dof,
                 "globalDirection": direction, "unit": "m", "value": 0.0}
                for dof, direction in (("UX", [1.0, 0.0, 0.0]),
                                       ("UY", [0.0, 1.0, 0.0]),
                                       ("UZ", [0.0, 0.0, 1.0]))]
    for physical_id in node_ids[1:]:
        supports.extend([{"nodePhysicalId": physical_id, "physicalDof": "UY",
                          "globalDirection": [0.0, 1.0, 0.0], "unit": "m", "value": 0.0},
                         {"nodePhysicalId": physical_id, "physicalDof": "UZ",
                          "globalDirection": [0.0, 0.0, 1.0], "unit": "m", "value": 0.0}])
    return {"apiVersion": "range.native-import/v1", "status": "supported", "sourceSha256": source_sha,
            "modelKind": "four-rod", "nodes": nodes, "elements": elements,
            "pointElements": point_elements,
            "freeDofs": free_dofs, "supports": supports,
            "ports": {"force": {"id": "source-tip-force", "nodePhysicalId": node_ids[-1],
                                 "globalDirection": [1.0, 0.0, 0.0], "unit": "N", "kind": "nodal_force",
                                 "minimum": -1e6, "maximum": 1e6},
                      "velocity": {"id": "source-tip-velocity", "nodePhysicalId": node_ids[-1],
                                    "globalDirection": [1.0, 0.0, 0.0], "unit": "m/s", "kind": "velocity"}},
            "dynamics": {"enabled": True, "method": "newmark", "massMatrix": "consistent",
                         "timeStepSeconds": 0.005, "steps": 4, "outputFrequency": 1,
                         "newmarkGamma": 0.5, "newmarkBeta": 0.25, "rayleighAlpha": 0.0,
                         "rayleighBeta": 0.0, "modalDampingRatios": [], "rhoInfinity": 0.0,
                         "legacyTimeSolverEnabled": False, "restartAvailable": False},
            "originalControl": {"enabled": False, "controlProtocolVersion": 0,
                                "controllerId": "", "deterministicSeed": "0"},
            "diagnostics": []}


def valid_beam_descriptor(source_sha):
    value = valid_descriptor(source_sha)
    value["modelKind"] = "cantilever-beam"
    value["nodes"] = [value["nodes"][0], value["nodes"][-1]]
    value["nodes"][1]["position"] = 1
    root, tip = (node["physicalId"] for node in value["nodes"])
    element = copy.deepcopy(value["elements"][0])
    element.update(type="beam1", nodePhysicalIds=[root, tip])
    element["section"] = {"areaM2": 0.01, "iyM4": 0.001, "izM4": 0.001, "torsionM4": 0.002,
                           "wyM3": 0.01, "wzM3": 0.01, "wtM3": 0.02}
    value["elements"] = [element]
    value["pointElements"] = [{"physicalId": 9000, "nodePhysicalId": root},
                              {"physicalId": 9001, "nodePhysicalId": tip}]
    axes = {"X": [1.0, 0.0, 0.0], "Y": [0.0, 1.0, 0.0], "Z": [0.0, 0.0, 1.0]}
    def dof(node_id, name):
        return {"nodePhysicalId": node_id, "physicalDof": name, "globalDirection": axes[name[1]],
                "unit": "m" if name.startswith("U") else "rad"}
    value["freeDofs"] = [dof(tip, "UY"), dof(tip, "RZ")]
    value["supports"] = [{**dof(node_id, name), "value": 0.0}
                         for node_id, names in ((root, ("UX", "UY", "UZ", "RX", "RY", "RZ")),
                                               (tip, ("UX", "UZ", "RX", "RY"))) for name in names]
    for port in value["ports"].values():
        port.update(nodePhysicalId=tip, globalDirection=axes["Y"])
    return value


class ModelSourcesTest(unittest.TestCase):
    def test_beam_requires_actual_positive_recovery_moduli(self):
        descriptor = valid_beam_descriptor("b" * 64)
        model_sources._validate_descriptor(descriptor, "b" * 64)
        for field in ("wyM3", "wzM3", "wtM3"):
            for bad_value in (None, 0.0, -1.0):
                broken = copy.deepcopy(descriptor)
                broken["elements"][0]["section"][field] = bad_value
                with self.subTest(field=field, value=bad_value), self.assertRaises(WorkflowError):
                    model_sources._validate_descriptor(broken, "b" * 64)

    def test_descriptor_accepts_internal_position_permutation_and_rejects_bad_bounds(self):
        descriptor = valid_descriptor("a" * 64)
        for node, position in zip(descriptor["nodes"], (2, 4, 1, 3, 0)):
            node["position"] = position
        self.assertEqual(model_sources._validate_descriptor(descriptor, "a" * 64), descriptor)
        for element in descriptor["elements"]:
            element["section"].update(iyM4=None, izM4=None, torsionM4=None)
        self.assertEqual(model_sources._validate_descriptor(descriptor, "a" * 64), descriptor)
        for limits in ((0, 1e6), (-1e6, -1), (-1e6, True)):
            broken = copy.deepcopy(descriptor)
            broken["ports"]["force"].update(minimum=limits[0], maximum=limits[1])
            with self.assertRaises(WorkflowError):
                model_sources._validate_descriptor(broken, "a" * 64)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-model-sources-")
        self.root = Path(self.temporary.name).resolve()
        self.workspace = self.root / "workflow-workspace"
        self.workspace.mkdir()
        self.catalog_root = self.root / "gui-project"
        self.catalog_root.mkdir()
        self.project_id = str(uuid.uuid4())
        self.document_id = str(uuid.uuid4())
        self.instance_ref = {"documentId": str(uuid.uuid4()), "sessionEpoch": str(uuid.uuid4()),
                             "documentRevision": 17}
        self.payload = b"accepted current project model snapshot"
        self.model_sha = hashlib.sha256(self.payload).hexdigest()
        self.snapshot_path = (self.catalog_root / "models" / self.document_id / "snapshots"
                              / self.model_sha / "model.rbm")
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_bytes(self.payload)
        self.document = {"persistentDocumentId": self.document_id, "label": "Current document",
            "sourcePath": "", "resolvedSourcePath": "", "sourceArtifacts": [],
            "originDocumentId": "", "sourceRevision": "r3", "savedArtifactHash": "",
            "dirty": True, "draftMetadata": {}, "tombstone": False,
            "modelSha256": self.model_sha, "snapshotSourceRevision": "r3",
            "snapshotRelativePath": f"models/{self.document_id}/snapshots/{self.model_sha}/model.rbm",
            "snapshotInstanceRef": self.instance_ref, "orphanSnapshots": []}
        self.catalog = {"storageVersion": 1, "projectId": self.project_id,
                        "documents": {self.document_id: self.document}, "sourcePathIndex": {}}
        self.catalog_file = self.catalog_root / "project.json"
        self._write_catalog()
        self.native = self.root / "range-workflow-native.exe"
        self.native.write_bytes(b"trusted test executable placeholder")
        resolved = resolve_project_snapshot(self.catalog_root, self.document_id)
        self.snapshot_ref = {key: resolved[key] for key in
                             ("projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef")}

    def tearDown(self):
        self.temporary.cleanup()

    def _write_catalog(self):
        self.catalog_file.write_text(json.dumps(self.catalog), encoding="utf-8")

    def _register(self, descriptor=None):
        value = descriptor or valid_descriptor(self.model_sha)
        with mock.patch.object(model_sources, "_invoke_native_describe", return_value=value) as describe:
            entry = model_sources.register_snapshot_source(self.workspace, self.catalog_root,
                                                             self.snapshot_ref, self.native)
        args = describe.call_args.args
        self.assertEqual(args[0], self.native)
        self.assertTrue(Path(args[1]).is_relative_to(self.workspace / "model-sources"))
        self.assertEqual(args[2], self.model_sha)
        self.assertEqual(Path(args[1]).read_bytes(), self.payload)
        return entry

    def test_registers_only_verified_snapshot_into_separate_workspace(self):
        catalog_before = self.catalog_file.read_bytes()
        entry = self._register()

        self.assertEqual(entry["originRef"]["projectId"], self.project_id)
        self.assertEqual(entry["originRef"]["persistentDocumentId"], self.document_id)
        self.assertEqual(entry["originRef"]["sourceRevision"], "r3")
        self.assertEqual(entry["originRef"]["modelSha256"], self.model_sha)
        self.assertEqual(entry["originRef"]["instanceRef"], self.instance_ref)
        self.assertEqual(entry["registeredModelSha256"], self.model_sha)
        self.assertEqual(entry["descriptor"]["modelKind"], "four-rod")
        self.assertTrue(Path(entry["path"]).is_relative_to(self.workspace / "model-sources"))
        self.assertEqual(Path(entry["path"]).read_bytes(), self.payload)
        self.assertNotEqual(Path(entry["path"]), self.snapshot_path)
        self.assertEqual(self.catalog_file.read_bytes(), catalog_before)

        readback = model_sources.read_model_source(self.workspace, entry["sourceId"])
        self.assertEqual(readback, entry)
        page = model_sources.list_model_sources(self.workspace)
        self.assertEqual(page["total"], 1)
        self.assertEqual(page["sources"], [entry])

    def test_registered_snapshot_remains_bound_after_original_catalog_changes(self):
        entry = self._register()
        self.snapshot_path.write_bytes(b"GUI document was edited later")
        self.assertEqual(model_sources.read_model_source(self.workspace, entry["sourceId"]), entry)

    def test_stale_or_forged_snapshot_reference_is_rejected_before_preflight(self):
        stale = copy.deepcopy(self.snapshot_ref)
        stale["sourceRevision"] = "r2"
        with mock.patch.object(model_sources, "_invoke_native_describe") as describe:
            with self.assertRaises(WorkflowError) as failed:
                model_sources.register_snapshot_source(self.workspace, self.catalog_root, stale, self.native)
        self.assertEqual(failed.exception.code, "STALE_SOURCE")
        describe.assert_not_called()
        self.assertFalse((self.workspace / "model-sources" / "index.json").exists())

    def test_registered_bytes_and_manifest_are_checked_against_ledger(self):
        entry = self._register()
        Path(entry["path"]).write_bytes(b"tampered registered model")
        with self.assertRaises(WorkflowError) as failed:
            model_sources.read_model_source(self.workspace, entry["sourceId"])
        self.assertEqual(failed.exception.code, "MODEL_SOURCE_CHANGED")

        Path(entry["path"]).write_bytes(self.payload)
        Path(entry["manifestPath"]).write_bytes(b"{}")
        with self.assertRaises(WorkflowError) as failed:
            model_sources.read_model_source(self.workspace, entry["sourceId"])
        self.assertEqual(failed.exception.code, "MODEL_SOURCE_CHANGED")

    def test_model_identity_is_opaque_and_listing_is_bounded(self):
        first = self._register()
        second = self._register()
        page = model_sources.list_model_sources(self.workspace, offset=1, limit=1)
        self.assertEqual(page["total"], 2)
        self.assertEqual(len(page["sources"]), 1)
        self.assertNotIn("..", first["sourceId"])
        self.assertNotIn("..", second["sourceId"])
        with self.assertRaises(WorkflowError) as failed:
            model_sources.read_model_source(self.workspace, "../../project.json")
        self.assertEqual(failed.exception.code, "MODEL_SOURCE_INVALID")

    def test_native_describe_uses_bounded_argv_without_a_shell(self):
        response = {"apiVersion": "range.native-import/v1", "status": "supported"}

        class FakeProcess:
            def __init__(self):
                self.stdout = io.BytesIO(json.dumps(response).encode("utf-8"))
                self.stderr = io.BytesIO(b"bounded diagnostic")
                self.returncode = 0

            def wait(self, timeout=None):
                self.timeout = timeout
                return self.returncode

            def kill(self):
                self.returncode = -9

        process = FakeProcess()
        with mock.patch.object(model_sources.subprocess, "Popen", return_value=process) as popen:
            result = model_sources._invoke_native_describe(self.native, self.snapshot_path, self.model_sha)
        self.assertEqual(result, response)
        args, kwargs = popen.call_args
        self.assertEqual(args[0], [str(self.native), "describe", "--input", str(self.snapshot_path),
                                   "--expected-sha", self.model_sha])
        self.assertIs(kwargs["stdin"], model_sources.subprocess.DEVNULL)
        self.assertIs(kwargs["shell"], False)
        self.assertEqual(kwargs["stdout"], model_sources.subprocess.PIPE)
        self.assertEqual(process.timeout, 30)

    def test_native_describe_kills_output_over_512_kib(self):
        class FakeProcess:
            def __init__(self):
                self.stdout = io.BytesIO(b"x" * (model_sources.MAX_DESCRIPTOR_BYTES + 1))
                self.stderr = io.BytesIO()
                self.returncode = 0
                self.killed = False

            def wait(self, timeout=None):
                return self.returncode

            def kill(self):
                self.killed = True
                self.returncode = -9

        process = FakeProcess()
        with mock.patch.object(model_sources.subprocess, "Popen", return_value=process):
            with self.assertRaises(WorkflowError) as failed:
                model_sources._invoke_native_describe(self.native, self.snapshot_path, self.model_sha)
        self.assertEqual(failed.exception.code, "NATIVE_PREFLIGHT_OUTPUT_LIMIT")
        self.assertTrue(process.killed)

    def test_rejected_native_source_keeps_bounded_field_diagnostics(self):
        rejected = {"apiVersion": model_sources.PROTOCOL_VERSION, "status": "rejected",
            "diagnostics": [{"code": "INVALID_SECTION_MODULUS", "severity": "error",
                "path": "lines[0].section.wyM3", "message": "Wy must be positive\n" + "x" * 1000}] * 20}
        class FakeProcess:
            def __init__(self):
                self.stdout = io.BytesIO(json.dumps(rejected).encode("utf-8"))
                self.stderr = io.BytesIO()
                self.returncode = 3
            def wait(self, timeout=None): return self.returncode
            def kill(self): self.returncode = -9
        with mock.patch.object(model_sources.subprocess, "Popen", return_value=FakeProcess()):
            with self.assertRaises(WorkflowError) as failed:
                model_sources._invoke_native_describe(self.native, self.snapshot_path, self.model_sha)
        self.assertEqual(failed.exception.code, "NATIVE_PREFLIGHT_REJECTED")
        self.assertIn("lines[0].section.wyM3", str(failed.exception))
        self.assertIn("INVALID_SECTION_MODULUS", str(failed.exception))
        self.assertNotIn("\n", str(failed.exception))
        self.assertLess(len(str(failed.exception)), 1700)


if __name__ == "__main__":
    unittest.main()
