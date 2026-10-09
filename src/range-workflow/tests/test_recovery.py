"""WF-05 checkpoint qualification and durable per-variant progress tests."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.service import WorkflowError, canonical, file_hash
from range_workflow.recovery import (
    CHECKPOINT_SCHEMA, CONTEXT_SCHEMA, NO_CONTROL_SHA256, POINTER_SCHEMA, RESULT_FRAME_SCHEMA, SCHEMA,
    checkpoint_context, controller_revision, control_arguments, qualify, record_progress,
)
from range_workflow.jobs import _native_history_proof, _seed_recovery_checkpoint


class RecoveryService:
    def __init__(self, workspace, job, binding, *, native_results=None):
        self.workspace = workspace
        self.native_results = native_results or (Path(binding["nativeResults"]["path"])
                                                 if "nativeResults" in binding else None)
        self.solver, self.native, self.python = (Path(binding[key]["path"])
                                               for key in ("solver", "native", "python"))
        self._state = {"jobs": {job["jobId"]: job},
                       "previews": {job["previewId"]: {"binding": binding}}}
        self._lock = __import__("threading").RLock()
        self.records = []

    def _job(self, job_id):
        return self._state["jobs"][job_id]

    def _commit(self, candidate, command, status, *, job_id=None, parameters=None, **kwargs):
        self.records.append((command, status, copy.deepcopy(parameters)))
        self._state = candidate


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name).resolve()
        self.workspace = self.base / "workspace"
        self.job_id = str(uuid.uuid4())
        self.attempt_id = self.job_id
        self.job_dir = self.workspace / "jobs" / self.job_id
        self.attempt_dir = self.job_dir
        self.attempt_dir.mkdir(parents=True)
        self.preview_id = str(uuid.uuid4())

        def write(name, data):
            path = self.attempt_dir / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return path

        self.template = write("template.ram", b"immutable-template")
        self.controller = write("controller.py", b"controller source")
        self.controller_host = write("controller_host.py", b"controller host")
        self.gains = write("gains.json", b"{\"gain\":4}")
        self.prepared = {
            "active": write("active-prepared.rbm", b"active prepared model"),
            "passive": write("passive-prepared.rbm", b"passive prepared model"),
        }
        for variant in ("active", "passive"):
            variant_directory = self.attempt_dir / variant
            variant_directory.mkdir(parents=True, exist_ok=True)
            for record_no in range(1, 5):
                (variant_directory / f"{variant}-{record_no:05d}.ram").write_bytes(
                    f"{variant}-accepted-frame-{record_no}".encode())
        self.runtime = {}
        for key, data in (("solver", b"solver"), ("native", b"native helper"),
                          ("python", Path(sys.executable).read_bytes()), ("nativeResults", b"broker")):
            path = self.base / (key + ".bin")
            path.write_bytes(data)
            self.runtime[key] = {"path": str(path), "sha256": file_hash(path)}
        self.binding = {
            **copy.deepcopy(self.runtime),
            "template": {"path": str(self.template), "sha256": file_hash(self.template)},
            "controller": {"path": str(self.controller), "sha256": file_hash(self.controller)},
            "controllerHost": {"path": str(self.controller_host), "sha256": file_hash(self.controller_host)},
            "gains": {"path": str(self.gains), "sha256": file_hash(self.gains)},
        }
        self.source = {
            "jobId": self.job_id, "totalSteps": 4, "dtSeconds": 0.25, "mode": "both",
            "configuration": {"dt": 0.25, "steps": 4, "targetDamping": 0.1, "model": "four-rod"},
            "templateSourceRef": {"sourceSha256": file_hash(self.template)},
            "controllerPath": str(self.controller), "controllerHostPath": str(self.controller_host),
            "gainsPath": str(self.gains),
            "preparedInputs": {key: {"path": str(path), "sha256": file_hash(path)}
                               for key, path in self.prepared.items()},
            "sourceArtifacts": {
                "template": {"path": str(self.template), "sha256": file_hash(self.template)},
                "controller": {"path": str(self.controller), "sha256": file_hash(self.controller)},
                "controllerHost": {"path": str(self.controller_host), "sha256": file_hash(self.controller_host)},
                "gains": {"path": str(self.gains), "sha256": file_hash(self.gains)},
            },
        }
        active_context = checkpoint_context(self.binding, self.source, "active", self.prepared["active"])
        passive_context = checkpoint_context(self.binding, self.source, "passive", self.prepared["passive"])
        self.contexts = {"active": active_context, "passive": passive_context}
        self.active_checkpoint_directory = self.attempt_dir / "active" / "checkpoints"
        self.active_checkpoint_directory.mkdir(parents=True)
        self._write_checkpoint(self.active_checkpoint_directory, active_context, accepted_seq=2)
        self.job = {"jobId": self.job_id, "state": "interrupted", "command": "workflow.run",
                    "previewId": self.preview_id, "recovery": {"schema": SCHEMA, "attempts": [{
                        "attemptId": self.attempt_id, "directory": str(self.attempt_dir),
                        "partialResult": copy.deepcopy(self.source),
                        "variants": {
                            "active": {"phase": "running", "context": active_context,
                                        "checkpointDirectory": str(self.active_checkpoint_directory)},
                            "passive": {"phase": "prepared", "context": passive_context,
                                        "checkpointDirectory": str(self.attempt_dir / "passive" / "checkpoints")},
                        },
                    }]}}
        self.service = RecoveryService(self.workspace, self.job, self.binding)

    def tearDown(self):
        self.temporary.cleanup()

    def test_controller_bytecode_policy_binds_revision_without_changing_legacy(self):
        legacy = control_arguments(self.source)
        self.assertEqual(legacy[0], str(self.controller_host))
        self.assertEqual(checkpoint_context(self.binding, self.source, "active", self.prepared["active"]),
                         self.contexts["active"])
        clean = copy.deepcopy(self.source)
        clean["controllerInterpreterFlags"] = ["-B"]
        argv = control_arguments(clean)
        self.assertEqual(argv, ["-B", *legacy])
        context = checkpoint_context(self.binding, clean, "active", self.prepared["active"])
        self.assertEqual(context["controlSha256"], controller_revision(Path(self.runtime["python"]["path"]), argv))
        self.assertNotEqual(context["controlSha256"], self.contexts["active"]["controlSha256"])
        self.assertEqual(control_arguments(copy.deepcopy(clean)), argv)
        for flags in (["-c", "pass"], ["-B", "-B"], "-B", None):
            clean["controllerInterpreterFlags"] = flags
            with self.assertRaises(WorkflowError):
                control_arguments(clean)

    @staticmethod
    def _write_checkpoint(directory, context, *, accepted_seq):
        model_name = "accepted-" + str(accepted_seq) + ".rbm"
        model = directory / model_name
        model.write_bytes(("native-state-" + str(accepted_seq)).encode())
        envelope = {
            "schema": CHECKPOINT_SCHEMA,
            "sourceSha256": context["sourceSha256"],
            "inputModelSha256": context["modelSha256"],
            "controlSha256": context["controlSha256"],
            "runtimeSha256": context["runtimeSha256"],
            "packageSha256": context["packageSha256"],
            "policy": context["policy"],
            "maxAcceptedSteps": context["maxAcceptedSteps"],
            "maxSimulationSeconds": context["maxSimulationSeconds"],
            "acceptedSeq": accepted_seq,
            "timeS": accepted_seq * context["dtS"],
            "dtS": context["dtS"],
            "modelFile": model_name,
            "checkpointModelSha256": file_hash(model),
        }
        envelope_path = directory / ("checkpoint-" + str(accepted_seq) + ".json")
        envelope_path.write_bytes(canonical(envelope))
        pointer = {"schema": POINTER_SCHEMA, "envelopeFile": envelope_path.name,
                   "envelopeSha256": file_hash(envelope_path)}
        (directory / "latest.json").write_bytes(canonical(pointer))
        return envelope_path

    def test_controller_revision_matches_length_delimited_solver_identity(self):
        args = [str(self.controller_host), "--implementation", str(self.controller),
                "--gains", str(self.gains)]
        expected = hashlib.sha256()

        def add(value):
            expected.update(str(len(value)).encode("ascii") + b":" + value)

        add(b"range-control-transport-v1")
        add(Path(self.runtime["python"]["path"]).resolve().as_posix().encode("utf-8"))
        add(file_hash(Path(self.runtime["python"]["path"])).encode("ascii"))
        for argument in args:
            add(argument.encode("utf-8"))
            candidate = Path(argument)
            if candidate.is_file():
                add(candidate.resolve().as_posix().encode("utf-8"))
                add(file_hash(candidate).encode("ascii"))
        self.assertEqual(controller_revision(Path(self.runtime["python"]["path"]), args), expected.hexdigest())
        self.assertEqual(self.contexts["active"]["schema"], CONTEXT_SCHEMA)
        self.assertEqual(self.contexts["active"]["controlSha256"], expected.hexdigest())
        self.assertEqual(self.contexts["passive"]["controlSha256"], NO_CONTROL_SHA256)

    def test_checkpoint_qualification_keeps_prepared_passive_and_uses_total_minus_accepted(self):
        answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recoverable")
        self.assertEqual(answer["variants"]["active"]["acceptedSeq"], 2)
        self.assertEqual(answer["variants"]["active"]["remainingSteps"], 2)
        self.assertEqual(answer["variants"]["passive"]["phase"], "prepared")
        self.assertEqual(answer["variants"]["passive"]["remainingSteps"], 4)
        self.assertEqual(source["active"]["acceptedSeq"], 2)
        self.assertNotIn("passive", source)

    def test_current_tool_reconfiguration_conflicts_even_when_original_files_remain(self):
        for attribute in ("solver", "native", "python", "native_results"):
            with self.subTest(tool=attribute):
                original = getattr(self.service, attribute)
                alternate = self.base / (attribute + "-alternate.bin")
                alternate.write_bytes(original.read_bytes())
                setattr(self.service, attribute, alternate)
                answer, source = qualify(self.service, self.job_id)
                self.assertEqual(answer["eligibility"], "conflict")
                self.assertIsNone(source)
                self.assertTrue(original.is_file())
                setattr(self.service, attribute, original)

    def test_frame_journal_binds_actual_prefix_and_ignores_only_incomplete_tail(self):
        variant_directory = self.attempt_dir / "active"
        journal = variant_directory / "native-result.journal.jsonl"
        records = []
        for record_no in (1, 2):
            frame_name = f"active-{record_no:05d}.ram"
            frame = variant_directory / frame_name
            records.append({"protocolVersion": RESULT_FRAME_SCHEMA, "jobId": self.attempt_id,
                            "seq": record_no, "recordNo": record_no, "timeS": record_no * .25,
                            "relativePath": frame_name, "sha256": file_hash(frame)})
        journal.write_bytes(b"".join(canonical(record) + b"\n" for record in records) + b'{"partial":')
        answer, _ = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recoverable")
        evidence = answer["variants"]["active"]["historyPrefix"]
        self.assertEqual([item["recordNo"] for item in evidence], [1, 2])
        self.assertTrue(all(item["journalEvidence"]["recorded"] for item in evidence))
        self.assertTrue(all("originManifestPath" not in item and "originManifestSha256" not in item
                            for item in evidence))
        self.assertEqual(evidence[0]["journalEvidence"]["sha256"], file_hash(journal))
        self.assertEqual(evidence[0]["sourceAttemptId"], self.attempt_id)
        pointer = json.loads((self.active_checkpoint_directory / "latest.json").read_bytes())
        self.assertEqual(evidence[0]["checkpointEvidence"]["envelopeSha256"], pointer["envelopeSha256"])

    def test_malformed_completed_frame_journal_record_is_conflict(self):
        journal = self.attempt_dir / "active" / "native-result.journal.jsonl"
        journal.write_bytes(b'{"protocolVersion":"range.native-result-frame/v1"}\n')
        answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "conflict")
        self.assertIsNone(source)

    def test_legacy_history_proof_keeps_explicit_null_origin_without_manifest_fields(self):
        item = {"recordNo": 3, "originalFrameSha256": "a" * 64,
                "originKind": "legacy-history-unattributed", "origin": None,
                "sourceRestartSha256": "b" * 64,
                "ancestryValidationScope": "copied-restart-input-bytes",
                "relativePath": "active-00003.ram", "sha256": "a" * 64}
        proof = _native_history_proof(item)
        self.assertIn("origin", proof)
        self.assertIsNone(proof["origin"])
        self.assertIn("sourceRestartSha256", proof)
        self.assertNotIn("originManifestPath", proof)
        self.assertNotIn("originManifestSha256", proof)

    def test_recovery_binding_metadata_does_not_change_frozen_package_context(self):
        before, _ = qualify(self.service, self.job_id)
        selected = before["variants"]["active"]
        self.binding["recoverySelection"] = {"qualificationSha256": before["qualificationSha256"]}
        self.binding["recovery/active"] = {"path": selected["checkpointPath"],
                                           "sha256": selected["checkpointSha256"]}
        after, source = qualify(self.service, self.job_id)
        self.assertEqual(after["eligibility"], "recoverable")
        self.assertEqual(after["qualificationSha256"], before["qualificationSha256"])
        self.assertIsNotNone(source)

    def test_target_checkpoint_requires_reconciliation_without_zero_step_solve(self):
        (self.active_checkpoint_directory / "latest.json").unlink()
        self._write_checkpoint(self.active_checkpoint_directory, self.contexts["active"], accepted_seq=4)
        logical = self.attempt_dir / "active" / "active.ram"
        logical.write_bytes(b"native logical link")
        with mock.patch("range_workflow.recovery._capture_terminal_link",
                        return_value={"linkPath": str(logical), "linkSha256": file_hash(logical),
                                      "terminalPath": str(self.attempt_dir / "active" / "active-00004.ram"),
                                      "terminalSha256": file_hash(self.attempt_dir / "active" / "active-00004.ram"),
                                      "recordNo": 4}):
            answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recoverable")
        self.assertEqual(answer["variants"]["active"]["remainingSteps"], 0)
        self.assertEqual(source["active"]["acceptedSeq"], 4)
        self.assertEqual(answer["variants"]["active"]["terminalLink"]["recordNo"], 4)

    def test_started_variant_without_checkpoint_requires_recompute(self):
        (self.active_checkpoint_directory / "latest.json").unlink()
        answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recompute_required")
        self.assertIsNone(source)
        self.assertEqual(answer["variants"]["active"]["phase"], "recompute_required")

    def test_checkpoint_context_and_model_are_independently_hash_bound(self):
        directory = self.active_checkpoint_directory
        pointer = json.loads((directory / "latest.json").read_bytes())
        envelope_path = directory / pointer["envelopeFile"]
        envelope = json.loads(envelope_path.read_bytes())
        envelope["inputModelSha256"] = "f" * 64
        envelope_path.write_bytes(canonical(envelope))
        (directory / "latest.json").write_bytes(canonical({
            "schema": POINTER_SCHEMA, "envelopeFile": envelope_path.name,
            "envelopeSha256": file_hash(envelope_path)}))
        answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "conflict")
        self.assertIsNone(source)

    def test_qualified_checkpoint_seed_is_copied_without_rewriting_original_attempt(self):
        answer, _ = qualify(self.service, self.job_id)
        selection = answer["variants"]["active"]
        pointer_before = (self.active_checkpoint_directory / "latest.json").read_bytes()
        destination = self.job_dir / "attempts" / str(uuid.uuid4()) / "active" / "checkpoints"
        _seed_recovery_checkpoint(selection, destination)
        self.assertEqual((destination / "latest.json").read_bytes(), pointer_before)
        self.assertEqual(file_hash(destination / Path(selection["checkpointPath"]).name),
                         selection["checkpointSha256"])
        self.assertEqual(file_hash(Path(selection["checkpointPath"])), selection["checkpointSha256"])

    def test_progress_records_new_attempt_and_preserves_historical_attempt(self):
        new_attempt = str(uuid.uuid4())
        self.job["attemptId"] = new_attempt
        second_dir = self.job_dir / "attempts" / new_attempt
        second_dir.mkdir(parents=True)
        self.service._state["jobs"][self.job_id]["attemptId"] = new_attempt
        record_progress(self.service, self.job_id, second_dir, self.source)
        record_progress(self.service, self.job_id, second_dir, self.source, variant="active",
                        phase="prepared", context=self.contexts["active"])
        attempts = self.service._job(self.job_id)["recovery"]["attempts"]
        self.assertEqual([attempt["attemptId"] for attempt in attempts], [self.attempt_id, new_attempt])
        self.assertEqual(attempts[-1]["variants"]["active"]["phase"], "prepared")
        self.assertEqual(attempts[-1]["previewId"], self.preview_id)
        self.assertEqual(len(self.service.records), 2)

    def test_first_recovery_progress_atomically_records_ancestor_and_all_variant_baselines(self):
        new_attempt = str(uuid.uuid4())
        attempt_dir = self.job_dir / "attempts" / new_attempt
        attempt_dir.mkdir(parents=True)
        self.service._state["jobs"][self.job_id]["attemptId"] = new_attempt
        self.service._state["jobs"][self.job_id]["parameters"] = {}
        inherited = {"attemptId": self.attempt_id, "targetSteps": 4,
                     "qualificationSha256": "a" * 64,
                     "variants": {"active": {"phase": "checkpointed", "acceptedSeq": 2,
                                                "sourceAttemptId": self.attempt_id},
                                  "passive": {"phase": "prepared", "acceptedSeq": 0,
                                                 "sourceAttemptId": self.attempt_id}}}
        states = {
            "active": {"phase": "checkpointed", "context": self.contexts["active"],
                       "checkpointDirectory": str(attempt_dir / "active" / "checkpoints"),
                       "historyPrefix": [], "historySourceRestartSha256": self.contexts["active"]["modelSha256"],
                       "checkpointSourceAttemptId": self.attempt_id},
            "passive": {"phase": "prepared", "context": self.contexts["passive"],
                        "checkpointDirectory": str(attempt_dir / "passive" / "checkpoints"),
                        "historyPrefix": [], "historySourceRestartSha256": self.contexts["passive"]["modelSha256"],
                        "checkpointSourceAttemptId": self.attempt_id},
        }
        record_progress(self.service, self.job_id, attempt_dir, self.source,
                        variant_states=states, inherited_selection=inherited)
        attempt = self.service._job(self.job_id)["recovery"]["attempts"][-1]
        self.assertEqual(attempt["inheritedSelection"], inherited)
        self.assertEqual(attempt["variants"]["active"]["phase"], "checkpointed")
        self.assertEqual(attempt["variants"]["passive"]["phase"], "prepared")
        self.assertEqual(len(self.service.records), 1)

    def test_completed_active_manifest_is_reused_when_passive_is_pending(self):
        binary = Path(self.runtime["nativeResults"]["path"])
        binary.write_bytes(b"broker")
        self.service.native_results = binary
        manifest = self.attempt_dir / "active" / "native-result.manifest.json"
        manifest.parent.mkdir(exist_ok=True)
        terminal = self.active_checkpoint_directory / "accepted-4.rbm"
        terminal.write_bytes(b"terminal accepted MODEL")
        manifest.write_bytes(canonical({"jobId": self.job_id, "artifactId": "native-artifact-active",
                                        "terminalModel": {"relativePath": os.path.relpath(terminal, manifest.parent),
                                                          "sha256": file_hash(terminal)},
                                        "frames": []}))
        descriptor = {"artifactId": "native-artifact-active", "manifestPath": str(manifest),
                      "manifestSha256": file_hash(manifest)}
        self.source["active"] = {"checkpointPath": str(terminal), "checkpointSha256": file_hash(terminal)}
        self.source["nativeResults"] = {"active": descriptor}
        self.source["nativeResultArtifacts"] = {"active": {
            "manifest": {"path": str(manifest), "sha256": file_hash(manifest)}}}
        self.job["recovery"]["attempts"][0]["partialResult"] = copy.deepcopy(self.source)
        self.job["recovery"]["attempts"][0]["variants"]["active"] = {"phase": "completed"}
        with mock.patch("range_workflow.native_results.NativeResultsClient.query",
                        return_value={"jobId": self.job_id, "artifactId": "native-artifact-active"}):
            answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recoverable")
        self.assertEqual(answer["variants"]["active"]["phase"], "completed")
        self.assertEqual(answer["variants"]["passive"]["phase"], "prepared")
        self.assertIn("active", source)

    def test_completed_active_manifest_is_reused_with_partial_passive_checkpoint(self):
        binary = Path(self.runtime["nativeResults"]["path"])
        binary.write_bytes(b"broker")
        self.service.native_results = binary
        manifest = self.attempt_dir / "active" / "native-result.manifest.json"
        manifest.parent.mkdir(exist_ok=True)
        terminal = self.attempt_dir / "active" / "accepted-final.rbm"
        terminal.write_bytes(b"terminal accepted MODEL")
        manifest.write_bytes(canonical({"jobId": self.job_id, "artifactId": "native-artifact-active",
                                        "terminalModel": {"relativePath": terminal.name,
                                                          "sha256": file_hash(terminal)},
                                        "frames": []}))
        descriptor = {"artifactId": "native-artifact-active", "manifestPath": str(manifest),
                      "manifestSha256": file_hash(manifest)}
        self.source["active"] = {"checkpointPath": str(terminal), "checkpointSha256": file_hash(terminal)}
        self.source["nativeResults"] = {"active": descriptor}
        self.source["nativeResultArtifacts"] = {"active": {
            "manifest": {"path": str(manifest), "sha256": file_hash(manifest)}}}
        passive_checkpoint_directory = self.attempt_dir / "passive" / "checkpoints"
        passive_checkpoint_directory.mkdir(parents=True)
        self._write_checkpoint(passive_checkpoint_directory, self.contexts["passive"], accepted_seq=3)
        attempt = self.job["recovery"]["attempts"][0]
        attempt["partialResult"] = copy.deepcopy(self.source)
        attempt["variants"]["active"] = {"phase": "completed"}
        attempt["variants"]["passive"] = {
            "phase": "running", "context": self.contexts["passive"],
            "checkpointDirectory": str(passive_checkpoint_directory)}
        with mock.patch("range_workflow.native_results.NativeResultsClient.query",
                        return_value={"jobId": self.job_id, "artifactId": "native-artifact-active"}):
            answer, source = qualify(self.service, self.job_id)
        self.assertEqual(answer["eligibility"], "recoverable")
        self.assertEqual(answer["variants"]["active"]["phase"], "completed")
        self.assertEqual(answer["variants"]["passive"]["phase"], "checkpointed")
        self.assertEqual(answer["variants"]["passive"]["remainingSteps"], 1)
        self.assertNotIn("active", answer["variants"]["passive"])
        self.assertEqual(source["active"]["checkpointPath"], str(terminal))


if __name__ == "__main__":
    unittest.main()
