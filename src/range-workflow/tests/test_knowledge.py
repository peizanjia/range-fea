"""Pure unit tests for the closed, versioned operation knowledge registry."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import knowledge


def descriptor(ident: str, *, read_only: bool = True, owner: bool = False,
               approval: bool | None = None) -> dict:
    value = {"id": ident, "readOnly": read_only,
             "parameters": {"type": "object", "properties": {"sample": {"type": "number"}}}}
    if owner:
        value["trustedOwnerOnly"] = True
    if approval is not None:
        value["requiresApproval"] = approval
    return value


def capabilities() -> dict:
    operation_ids = [
        "capabilities.query", "command.preview", "command.execute",
        "workflow.status", "project.catalog.query", "model.source.query",
        "model.sources.query", "result.summary.query", "native.document.query",
        "extension.list", "task.plan.query", "task.node.query", "schema.get",
        "knowledge.query", "telemetry.query", "audit.query", "jobs.query",
        "job.cancel", "execution.query",
    ]
    operations = [descriptor(ident, read_only=ident != "command.execute",
                            owner=ident == "model.source.register",
                            approval=True if ident == "command.execute" else None)
                  for ident in operation_ids]
    next(item for item in operations if item["id"] == "capabilities.query")["parameters"] = {
        "type": "object", "properties": {}, "required": [], "additionalProperties": False}
    operations.append(descriptor("model.source.register", read_only=False, owner=True))
    commands = [descriptor("workflow.configure", read_only=False, approval=True),
                descriptor("task.plan.submit", read_only=False, approval=True),
                {"id": "extension.run", "parameters": {"type": "object", "properties": {}},
                 "requiresApproval": True, "trustedRuntimeRequired": True}]
    return {"apiVersion": "range.workflow/v1", "operations": operations,
            "commands": commands,
            "extensionRuntime": {"trustedExtensions": False, "trustMode": "disabled",
                                 "osSandbox": False}}


class KnowledgeTest(unittest.TestCase):
    def setUp(self):
        self.original = knowledge.load_core_pack()
        self.catalog = capabilities()

    def _patched_pack(self, content: bytes | None = None):
        temporary = tempfile.TemporaryDirectory()
        package = Path(temporary.name) / "range_workflow"
        directory = package / "knowledge"
        directory.mkdir(parents=True)
        if content is None:
            content = (Path(knowledge.__file__).parent / "knowledge" / "core-v1.json").read_bytes()
        (directory / "core-v1.json").write_bytes(content)
        patcher = mock.patch.object(knowledge, "__file__", str(package / "knowledge.py"))
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(temporary.cleanup)
        return directory / "core-v1.json"

    def _write_pack(self, value: dict) -> bytes:
        return json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8")

    def test_coverage_covers_every_published_operation_and_command_family(self):
        proof = knowledge.coverage(self.catalog)
        self.assertTrue(proof["complete"])
        self.assertEqual(proof["coveredCount"], len(self.catalog["operations"]) + len(self.catalog["commands"]))
        self.assertIn("model.sources.query", proof["operationIds"])
        self.assertIn("task.plan.submit", proof["commandIds"])
        self.assertEqual(proof["protocolVersion"], "range.workflow/v1")

    def test_query_items_are_live_catalog_bound_and_carry_trust_metadata(self):
        result = knowledge.query_knowledge(self.catalog, "workflow.configure", max_items=5)
        self.assertLessEqual(len(result["items"]), 5)
        item = next(row for row in result["items"] if row["id"] == "workflow.configure")
        self.assertEqual(item["kind"], "command")
        self.assertEqual(item["command"], "workflow.configure")
        self.assertIsNone(item["operation"])
        self.assertFalse(item["readOnly"])
        self.assertTrue(item["requiresApproval"])
        self.assertFalse(item["trustedOwner"])
        self.assertEqual(item["trustedRuntime"], {
            "enabled": False, "trustMode": "disabled", "osSandbox": False, "required": False})
        self.assertEqual(item["protocolVersion"], "range.workflow/v1")
        self.assertEqual(item["sha256"], self.original["rawSha256"])
        self.assertEqual(item["packSha256"], self.original["packSha256"])
        self.assertEqual(item["schemaHash"], hashlib.sha256(
            json.dumps(descriptor("workflow.configure")["parameters"], sort_keys=True,
                       separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest())
        self.assertTrue(item["requiresPreparedParameters"])
        self.assertEqual(item["runnableExample"], (
            "def example(client, parameters, approve):\n"
            "    preview = client.preview('workflow.configure', parameters)\n"
            "    approval = approve(preview)  # trusted user callback; never auto-approve\n"
            "    receipt = client.execute(preview, approval)\n"
            "    return client.wait(receipt['jobId'])\n"))
        self.assertNotIn("authority", item)
        self.assertNotIn("approvalToken", json.dumps(item))

        owner = next(row for row in knowledge.query_knowledge(
            self.catalog, "model.source.register", max_items=5)["items"]
                     if row["id"] == "model.source.register")
        self.assertTrue(owner["trustedOwner"])
        self.assertIn("trusted owner workflow", " ".join(owner["prerequisites"]))

    def test_runnable_examples_are_syntax_valid_and_preserve_host_approval_sequence(self):
        operation = knowledge.query_knowledge(self.catalog, "capabilities.query")["items"][0]
        operation_namespace = {}
        exec(operation["runnableExample"], operation_namespace)

        class QueryClient:
            def __init__(self):
                self.calls = []

            def request(self, operation_id, parameters):
                self.calls.append(("request", operation_id, parameters))
                return {"apiVersion": "range.workflow/v1"}

        query_client = QueryClient()
        parameters = {"caller": "supplied"}
        operation_namespace["example"](query_client, parameters)
        self.assertEqual(query_client.calls, [("request", "capabilities.query", parameters)])

        command = knowledge.query_knowledge(self.catalog, "workflow.configure")["items"][0]
        command_namespace = {}
        exec(command["runnableExample"], command_namespace)

        class CommandClient:
            def __init__(self):
                self.calls = []

            def preview(self, command_id, supplied):
                self.calls.append(("preview", command_id, supplied))
                return {"previewHash": "preview"}

            def execute(self, preview, approval):
                self.calls.append(("execute", preview, approval))
                return {"jobId": "job"}

            def wait(self, job_id):
                self.calls.append(("wait", job_id))
                return {"state": "succeeded"}

        client = CommandClient()
        approval = {"hostApproval": True}

        def approve(preview):
            client.calls.append(("approve", preview))
            return approval

        self.assertEqual(command_namespace["example"](client, parameters, approve), {"state": "succeeded"})
        self.assertEqual([call[0] for call in client.calls], ["preview", "approve", "execute", "wait"])
        self.assertEqual(client.calls[0], ("preview", "workflow.configure", parameters))
        self.assertEqual(client.calls[2], ("execute", {"previewHash": "preview"}, approval))

        extension = next(row for row in knowledge.query_knowledge(
            self.catalog, "extension.run")["items"] if row["id"] == "extension.run")
        self.assertTrue(extension["trustedRuntime"]["required"])
        self.assertIn("trusted developer mode", " ".join(extension["prerequisites"]))

    def test_schema_change_changes_schema_and_catalog_hashes(self):
        before = knowledge.query_knowledge(self.catalog, "capabilities.query")["items"][0]
        changed = copy.deepcopy(self.catalog)
        operation = next(item for item in changed["operations"] if item["id"] == "capabilities.query")
        operation["parameters"]["properties"]["newField"] = {"type": "string"}
        after = knowledge.query_knowledge(changed, "capabilities.query")["items"][0]
        self.assertNotEqual(before["schemaHash"], after["schemaHash"])
        self.assertNotEqual(before["catalogSha256"], after["catalogSha256"])
        self.assertEqual(before["text"], after["text"])

    def test_unknown_query_does_not_invent_a_catalog_entry(self):
        self.assertEqual(knowledge.query_knowledge(self.catalog, "future.operation.id")["items"], [])
        changed = copy.deepcopy(self.catalog)
        changed["operations"].append(descriptor("future.operation.id"))
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.coverage(changed)
        self.assertEqual(caught.exception.code, "KNOWLEDGE_COVERAGE_INCOMPLETE")
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.query_knowledge(changed, "future.operation.id")

    def test_valid_pack_tampering_is_visible_in_both_content_hashes(self):
        value = copy.deepcopy(self.original["pack"])
        value["groups"]["workflow"]["steps"][0] = "Tampered packaged guidance."
        raw = self._write_pack(value)
        path = self._patched_pack(raw)
        loaded = knowledge.load_core_pack()
        self.assertEqual(loaded["rawSha256"], hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(loaded["rawSha256"], self.original["rawSha256"])
        self.assertNotEqual(loaded["packSha256"], self.original["packSha256"])
        result = knowledge.query_knowledge(self.catalog, "Tampered packaged guidance.")
        self.assertTrue(any("Tampered packaged guidance." in item["text"] for item in result["items"]))
        self.assertTrue(path.is_file())

    def test_missing_pack_fails_closed(self):
        temporary = tempfile.TemporaryDirectory()
        package = Path(temporary.name) / "range_workflow"
        (package / "knowledge").mkdir(parents=True)
        patcher = mock.patch.object(knowledge, "__file__", str(package / "knowledge.py"))
        with patcher:
            with self.assertRaises(knowledge.KnowledgeError) as caught:
                knowledge.load_core_pack()
        self.assertEqual(caught.exception.code, "KNOWLEDGE_UNAVAILABLE")
        temporary.cleanup()

    def test_bad_json_duplicate_keys_unknown_fields_and_oversize_fail_closed(self):
        self._patched_pack(b"{not-json")
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.load_core_pack()

        self._patched_pack(b'{"schema":"range.operation-knowledge/v1","schema":"duplicate"}')
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.load_core_pack()

        value = copy.deepcopy(self.original["pack"])
        value["unexpected"] = True
        self._patched_pack(self._write_pack(value))
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.load_core_pack()

        self._patched_pack(b"{" + b" " * knowledge.MAX_PACK_BYTES)
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.load_core_pack()
        self.assertEqual(caught.exception.code, "KNOWLEDGE_INVALID")

    def test_unknown_schema_and_pack_versions_fail_closed(self):
        value = copy.deepcopy(self.original["pack"])
        value["schema"] = "range.operation-knowledge/v9"
        self._patched_pack(self._write_pack(value))
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.load_core_pack()
        self.assertEqual(caught.exception.code, "KNOWLEDGE_VERSION_UNSUPPORTED")

        value = copy.deepcopy(self.original["pack"])
        value["version"] = "2.0.0"
        self._patched_pack(self._write_pack(value))
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.load_core_pack()
        self.assertEqual(caught.exception.code, "KNOWLEDGE_VERSION_UNSUPPORTED")

    def test_incompatible_pack_and_runtime_protocol_fail_closed(self):
        value = copy.deepcopy(self.original["pack"])
        value["compatibility"]["protocolVersions"] = ["range.workflow/v2"]
        self._patched_pack(self._write_pack(value))
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.load_core_pack()
        self.assertEqual(caught.exception.code, "KNOWLEDGE_VERSION_UNSUPPORTED")

        self._patched_pack(self._write_pack(self.original["pack"]))
        changed = copy.deepcopy(self.catalog)
        changed["apiVersion"] = "range.workflow/v2"
        with self.assertRaises(knowledge.KnowledgeError) as caught:
            knowledge.coverage(changed)
        self.assertEqual(caught.exception.code, "KNOWLEDGE_INCOMPATIBLE")

    def test_query_bounds_and_trust_flags_are_fail_closed(self):
        for count in (0, 6, True, -1):
            with self.subTest(max_items=count), self.assertRaises(knowledge.KnowledgeError):
                knowledge.query_knowledge(self.catalog, "workflow", count)
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.query_knowledge(self.catalog, "x" * 257)

        changed = copy.deepcopy(self.catalog)
        changed["extensionRuntime"] = {"trustedExtensions": True,
                                       "trustMode": "trusted-developer", "osSandbox": False}
        item = next(row for row in knowledge.query_knowledge(
            changed, "extensions", max_items=5)["items"] if row["id"] == "extension.list")
        self.assertEqual(item["trustedRuntime"], {
            "enabled": True, "trustMode": "trusted-developer", "osSandbox": False, "required": False})
        extension_command = next(row for row in knowledge.query_knowledge(
            changed, "extension.run", max_items=5)["items"] if row["id"] == "extension.run")
        self.assertFalse(extension_command["readOnly"])
        self.assertEqual(extension_command["trustedRuntime"]["required"], True)
        changed["extensionRuntime"]["trustedExtensions"] = False
        with self.assertRaises(knowledge.KnowledgeError):
            knowledge.coverage(changed)


if __name__ == "__main__":
    unittest.main()
