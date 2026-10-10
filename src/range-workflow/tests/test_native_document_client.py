"""Mocked protocol tests for the native document SDK; no Qt binary is needed."""

import json
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.native_document_client import (
    NativeCommandPreview,
    NativeDocumentClient,
    NativeDocumentError,
)


CLI_PATH = None
SOURCE_FIXTURE = None
for flag, variable in (("--cli", "cli"), ("--source", "source")):
    if flag in sys.argv:
        position = sys.argv.index(flag)
        value = sys.argv[position + 1]
        if variable == "cli":
            CLI_PATH = Path(value).resolve()
        else:
            SOURCE_FIXTURE = Path(value).resolve()
        del sys.argv[position:position + 2]


SOURCE_SHA = "a" * 64


def completed(args, value, returncode=0, stderr=""):
    return mock.Mock(args=args, returncode=returncode, stdout=json.dumps(value), stderr=stderr)


class NativeDocumentClientTests(unittest.TestCase):
    def setUp(self):
        self.client = NativeDocumentClient("range-model-command", "model.ram")

    def test_query_uses_argument_vector_and_reads_one_json_object(self):
        with mock.patch("range_workflow.native_document_client.subprocess.run",
                        return_value=completed([], {"sourceSha256": SOURCE_SHA, "modelId": "ephemeral"})) as run:
            result = self.client.query()
        self.assertEqual(result["sourceSha256"], SOURCE_SHA)
        args, kwargs = run.call_args
        self.assertIsInstance(args[0], list)
        self.assertEqual(args[0][-1], "--query")
        self.assertFalse(kwargs["shell"])

    def test_preview_freezes_normalized_request_and_execute_reuses_it(self):
        arguments = {"method": "multiple", "iterations": 123, "modes": 4,
                     "tolerance": 1e-8, "unit": "1"}
        normalized = dict(arguments)
        calls = []

        def run(argv, **kwargs):
            calls.append(list(argv))
            if "--query" in argv:
                return completed(argv, {"sourceSha256": SOURCE_SHA, "modelId": "fresh-each-time"})
            parameter_path = Path(argv[argv.index("--parameters") + 1])
            envelope = json.loads(parameter_path.read_text(encoding="utf-8"))
            self.assertEqual(envelope, {"sourceSha256": SOURCE_SHA, "parameters": normalized})
            if "--dry-run" in argv:
                return completed(argv, {"status": "preview", "sourceSha256": SOURCE_SHA,
                                        "normalizedParameters": normalized, "fingerprint": "state-digest"})
            return completed(argv, {"status": "published", "sourceSha256": SOURCE_SHA,
                                    "normalizedParameters": normalized, "output": "result.ram"})

        with mock.patch("range_workflow.native_document_client.subprocess.run", side_effect=run):
            preview = self.client.preview("analysis.modal.configure", arguments)
            self.assertIsInstance(preview, NativeCommandPreview)
            self.assertEqual(preview.normalized_arguments, normalized)
            self.assertEqual(preview.fingerprint, "state-digest")
            self.assertEqual(len(preview.digest), 64)
            response = self.client.execute(preview, "result.ram")

        self.assertEqual(response["status"], "published")
        execute_call = calls[-1]
        self.assertEqual(execute_call[execute_call.index("--command") + 1], "analysis.modal.configure")
        self.assertNotIn("--dry-run", execute_call)
        self.assertFalse(hasattr(self.client, "approve"))

    def test_execute_rejects_foreign_preview_and_stale_source(self):
        arguments = {"method": "multiple", "iterations": 100, "modes": 3,
                     "tolerance": 1e-9, "unit": "1"}
        responses = [
            {"sourceSha256": SOURCE_SHA},
            {"status": "preview", "sourceSha256": SOURCE_SHA,
             "normalizedParameters": arguments, "fingerprint": "f"},
        ]
        with mock.patch("range_workflow.native_document_client.subprocess.run",
                        side_effect=lambda argv, **kwargs: completed(argv, responses.pop(0))):
            preview = self.client.preview("analysis.modal.configure", arguments)
        other = NativeDocumentClient("range-model-command", "model.ram")
        with self.assertRaises(ValueError):
            other.execute(preview, "out.ram")

        with mock.patch("range_workflow.native_document_client.subprocess.run",
                        return_value=completed([], {"sourceSha256": "b" * 64})):
            with self.assertRaises(NativeDocumentError) as caught:
                self.client.execute(preview, "out.ram")
        self.assertIn("source changed", str(caught.exception).lower())

    def test_named_modal_method_reads_advertised_schema(self):
        calls = []
        capabilities = {"commands": [{"id": "analysis.modal.configure", "supported": True,
            "required": ["method", "iterations", "modes", "tolerance", "unit"],
            "units": {"tolerance": "1"}, "methods": ["dominant", "multiple"]}]}

        def run(argv, **kwargs):
            calls.append(list(argv))
            if "--query" in argv:
                return completed(argv, {"sourceSha256": SOURCE_SHA})
            if "--capabilities" in argv:
                return completed(argv, {**capabilities, "sourceSha256": SOURCE_SHA})
            request_path = Path(argv[argv.index("--parameters") + 1])
            envelope = json.loads(request_path.read_text(encoding="utf-8"))
            self.assertEqual(envelope["parameters"]["unit"], "1")
            return completed(argv, {"status": "preview", "sourceSha256": SOURCE_SHA,
                                    "normalizedParameters": envelope["parameters"], "fingerprint": "f"})

        with mock.patch("range_workflow.native_document_client.subprocess.run", side_effect=run):
            preview = self.client.configure_modal(method="multiple", iterations=50, modes=2, tolerance=1e-7)
        self.assertEqual(preview.normalized_arguments["unit"], "1")
        self.assertTrue(any("--capabilities" in call for call in calls))

    def test_named_method_rejects_source_change_during_schema_read(self):
        changed_sha = "b" * 64
        capabilities = {"sourceSha256": changed_sha, "commands": [{
            "id": "analysis.modal.configure", "supported": True,
            "required": ["method", "iterations", "modes", "tolerance", "unit"],
            "units": {"tolerance": "1"}, "methods": ["multiple"]}]}
        with mock.patch("range_workflow.native_document_client.subprocess.run", side_effect=[
                completed([], {"sourceSha256": SOURCE_SHA}),
                completed([], {"sourceSha256": changed_sha}),
                completed([], capabilities)]):
            with self.assertRaises(NativeDocumentError) as caught:
                self.client.configure_modal(method="multiple", iterations=10, modes=2, tolerance=1e-6)
        self.assertIn("source changed", str(caught.exception).lower())

    def test_label_query_returns_candidates_without_selecting(self):
        result = {"sourceSha256": SOURCE_SHA, "status": "ok", "labelQuery": "Beam",
                  "ambiguous": True, "candidateIds": ["g:18", "g:19"], "targets": []}
        with mock.patch("range_workflow.native_document_client.subprocess.run") as run:
            run.side_effect = [completed([], {"sourceSha256": SOURCE_SHA}), completed([], result)]
            page = self.client.find_targets("Beam")
        self.assertTrue(page["ambiguous"])
        self.assertEqual(page["candidateIds"], ["g:18", "g:19"])
        self.assertEqual(page["targets"], [])
        self.assertFalse(hasattr(self.client, "approve"))

    def test_cursor_page_is_forwarded_as_json_without_a_session_identity(self):
        request = {"cursor": "opaque-native-cursor"}

        def run(argv, **kwargs):
            if "--query" in argv:
                return completed(argv, {"sourceSha256": SOURCE_SHA, "modelId": "ephemeral"})
            self.assertIn("--targets", argv)
            page_path = Path(argv[argv.index("--page") + 1])
            self.assertEqual(json.loads(page_path.read_text(encoding="utf-8")), request)
            return completed(argv, {"sourceSha256": SOURCE_SHA, "status": "ok", "targets": [],
                                    "hasMore": False})

        with mock.patch("range_workflow.native_document_client.subprocess.run", side_effect=run):
            result = self.client.targets(request)
        self.assertEqual(result["status"], "ok")

    def test_rejected_json_becomes_structured_exception(self):
        rejection = {"status": "rejected", "code": "invalid_units", "message": "Expected N"}
        with mock.patch("range_workflow.native_document_client.subprocess.run",
                        side_effect=[completed([], {"sourceSha256": SOURCE_SHA}),
                                     completed([], rejection, returncode=2)]):
            with self.assertRaises(NativeDocumentError) as caught:
                self.client.targets()
        self.assertEqual(caught.exception.code, "invalid_units")
        self.assertEqual(caught.exception.response, rejection)


@unittest.skipUnless(CLI_PATH and SOURCE_FIXTURE,
                     "pass --cli range-model-command --source stress-model.ram for native integration")
class NativeDocumentCliIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-native-document-integration-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.ram"
        shutil.copyfile(SOURCE_FIXTURE, self.source)
        self.original = self.source.read_bytes()

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, source, *args, expected=0):
        result = subprocess.run([str(CLI_PATH), "--input", str(source), *map(str, args)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30,
                                check=False, shell=False)
        self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
        return json.loads(result.stdout)

    @staticmethod
    def parameter_file(directory, source_sha, parameters, name):
        path = Path(directory) / name
        path.write_text(json.dumps({"sourceSha256": source_sha, "parameters": parameters}),
                        encoding="utf-8")
        return path

    def test_v18_three_command_round_trip_and_stale_guards(self):
        source_hash = hashlib.sha256(self.original).hexdigest()
        client = NativeDocumentClient(CLI_PATH, self.source)
        capabilities = client.capabilities()
        advertised = {item["id"]: item for item in capabilities["commands"]}
        for command in ("analysis.structural_dynamics.configure", "entity.material.set",
                        "entity.boundary.set"):
            self.assertTrue(advertised[command]["supported"], command)

        first_page = client.targets({"limit": 1})
        self.assertTrue(first_page["hasMore"])
        stale_cursor = first_page["nextCursor"]
        point = next(item for item in client.targets({"limit": 200})["targets"]
                     if item["type"] == "point" and item["nodeCount"] == 1)

        dynamics_preview = client.configure_structural_dynamics(
            enabled=True, method="newmark", time_step=0.01, steps=4,
            output_frequency=2, rayleigh_alpha=0.0, rayleigh_beta=0.0,
            rho_infinity=1.0, newmark_gamma=0.5, newmark_beta=0.25,
            modal_damping_ratios=[])
        dynamics_output = self.root / "dynamics.ram"
        self.assertEqual(client.execute(dynamics_preview, dynamics_output)["status"], "published")
        self.assertEqual(dynamics_output.read_bytes().splitlines()[0], b"1.3.19")

        material_client = NativeDocumentClient(CLI_PATH, dynamics_output)
        material_preview = material_client.set_material(
            target_id=point["id"], name="SDK steel", young_modulus_pa=2.0e11,
            density_kg_m3=7850.0, poisson_ratio=0.3, thermal_expansion_per_k=1.2e-5)
        material_output = self.root / "material.ram"
        self.assertEqual(material_client.execute(material_preview, material_output)["status"], "published")
        self.assertEqual(material_output.read_bytes().splitlines()[0], b"1.3.19")

        boundary_client = NativeDocumentClient(CLI_PATH, material_output)
        boundary_preview = boundary_client.set_boundary(
            target_id=point["id"], kind="force", values=(1.0, None, -2.0))
        boundary_output = self.root / "boundary.ram"
        self.assertEqual(boundary_client.execute(boundary_preview, boundary_output)["status"], "published")
        self.assertEqual(boundary_output.read_bytes().splitlines()[0], b"1.3.19")
        self.assertEqual(self.source.read_bytes(), self.original)

        # A label query remains a candidate list; it never chooses a target.
        matches = boundary_client.find_targets("point")
        self.assertIsInstance(matches.get("candidateIds"), list)

        material_before_rejections = material_output.read_bytes()
        current_hash = hashlib.sha256(material_before_rejections).hexdigest()
        invalid_output = self.root / "must-not-exist.ram"
        wrong_hash = self.parameter_file(self.root, "0" * 64,
            {"targetId": point["id"], "kind": "force", "values": [1.0, None, 0.0], "unit": "N"},
            "stale.json")
        stale = self.run_cli(material_output, "--command", "entity.boundary.set",
                             "--parameters", wrong_hash, "--output", invalid_output, expected=2)
        self.assertEqual(stale["status"], "rejected")
        self.assertFalse(invalid_output.exists())

        wrong_unit = self.parameter_file(self.root, current_hash,
            {"targetId": point["id"], "kind": "force", "values": [1.0, None, 0.0], "unit": "kN"},
            "wrong-unit.json")
        rejection = self.run_cli(material_output, "--command", "entity.boundary.set",
                                 "--parameters", wrong_unit, "--output", invalid_output, expected=2)
        self.assertEqual(rejection["code"], "invalid_units")
        self.assertFalse(invalid_output.exists())
        self.assertEqual(material_output.read_bytes(), material_before_rejections)

        with self.assertRaises(NativeDocumentError) as caught:
            boundary_client.targets({"cursor": stale_cursor})
        self.assertEqual(caught.exception.code, "stale_target_cursor")


if __name__ == "__main__":
    unittest.main()
