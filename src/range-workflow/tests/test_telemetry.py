"""Focused contract tests for native accepted-control telemetry."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowError, WorkflowService
from range_workflow import telemetry


def request(service, operation, parameters):
    return service.handle({"apiVersion": "range.workflow/v1", "requestId": str(uuid.uuid4()),
                           "operation": operation, "parameters": parameters})


def event(run_id, sequence, kind, payload=None, *, event_id=None):
    return {"schemaVersion": 1, "eventId": event_id or str(uuid.uuid4()), "runId": run_id,
            "sequence": sequence, "utc": "2026-10-04T12:00:00.000Z", "type": kind,
            "status": "accepted", "payload": payload or {}}


def accepted(time_s=0.005, *, sensor_value=0.25, unit="m", force=2.0):
    return {"acceptedTimeS": time_s, "nextTimeS": time_s + 0.005,
            "sensors": [{"sensorId": "tip-u", "value": sensor_value, "unit": unit}],
            "appliedChannels": [{"actuatorId": "root-force", "appliedForceN": force,
                                 "endpointPowerW": 0.5, "cumulativeWorkJ": 0.01}],
            "resultArtifactPersisted": False}


class TelemetryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.examples = self.base / "examples"
        self.examples.mkdir()
        for name in ("stage9_truss4.ram", "stage9_analysis.py", "stage9_velocity_controller.py"):
            (self.examples / name).write_text("fixture", encoding="utf-8")
        self.workspace = self.base / "workspace"
        self.service = WorkflowService(self.workspace, Path(sys.executable), Path(sys.executable),
                                       self.examples, Path(sys.executable))
        self.job_id = str(uuid.uuid4())
        self.job = {"jobId": self.job_id, "command": "workflow.run", "state": "running",
                    "parameters": {"mode": "both"}}
        self.service._state["jobs"][self.job_id] = self.job
        self.directory = self.workspace / "jobs" / self.job_id
        self.directory.mkdir(parents=True)
        self.run_id = str(uuid.uuid4())
        self.journal = self.directory / f"active.ram.{self.run_id}.events.jsonl"

    def tearDown(self):
        self.service.close()
        self.temporary.cleanup()

    def query(self, **parameters):
        return request(self.service, "telemetry.query", {"jobId": self.job_id, **parameters})

    def query_recovered_reader(self, mode="active", *, after_seq=0, limit=256):
        return self.query(mode=mode, afterSeq=after_seq, limit=limit)

    def write_events(self, events):
        self.journal.write_bytes(b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                                       for item in events))

    def prepare_recovered_history(self, *, selected_seq=2):
        dt, total_steps = 0.005, 4
        run_result = {"jobId": self.job_id, "mode": "both", "totalSteps": total_steps,
                      "dtSeconds": dt, "configuration": {"dt": dt}}
        original_run = str(uuid.uuid4())
        original_events = [event(original_run, 1, "SolverRunStarted")]
        for accepted_seq in range(1, total_steps + 1):
            original_events.append(event(
                original_run, len(original_events) + 1, "ControlStepAccepted",
                accepted((accepted_seq - 1) * dt)))
        original_path = self.directory / f"active.ram.{original_run}.events.jsonl"
        original_path.write_bytes(b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                                                   for item in original_events))

        attempt_id = str(uuid.uuid4())
        attempt_directory = self.directory / "attempts" / attempt_id
        variant_directory = attempt_directory / "active"
        variant_directory.mkdir(parents=True)
        attempt_run = str(uuid.uuid4())
        self.job.update(command="workflow.recover", state="running", attemptId=attempt_id,
                        parameters={"jobId": self.job_id, "qualificationSha256": "a" * 64},
                        recovery={"schema": "range.workflow-recovery/v1", "attempts": [
                            {"attemptId": self.job_id, "directory": str(self.directory),
                             "partialResult": run_result},
                            {"attemptId": attempt_id, "directory": str(attempt_directory),
                             "partialResult": run_result,
                             "inheritedSelection": {
                                 "attemptId": self.job_id, "targetSteps": total_steps,
                                 "qualificationSha256": "b" * 64,
                                 "variants": {
                                     "active": {"phase": "checkpointed", "acceptedSeq": selected_seq},
                                     "passive": {"phase": "prepared", "acceptedSeq": 0}}}}]})
        attempt_events = [event(attempt_run, 1, "SolverRunStarted")]
        for accepted_seq in range(selected_seq + 1, total_steps + 1):
            attempt_events.append(event(
                attempt_run, len(attempt_events) + 1, "ControlStepAccepted",
                accepted((accepted_seq - 1) * dt)))
        attempt_path = variant_directory / f"active.rbm.{attempt_run}.events.jsonl"
        attempt_path.write_bytes(b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                                                  for item in attempt_events))
        self.service._telemetry = telemetry.TelemetryReader(self.workspace)
        return attempt_run, attempt_path, original_path

    def assertCode(self, code, callable, *args, **kwargs):
        with self.assertRaises(WorkflowError) as captured:
            callable(*args, **kwargs)
        self.assertEqual(captured.exception.code, code)

    def test_query_is_read_only_and_no_journal_waits(self):
        catalog = request(self.service, "capabilities.query", {})
        operation = next(item for item in catalog["operations"] if item["id"] == "telemetry.query")
        self.assertTrue(operation["readOnly"])
        result = self.query()
        self.assertEqual(result["source"], "native-accepted-control")
        self.assertEqual(result["state"], "running")
        self.assertEqual(result["availability"], "waiting")
        self.assertEqual(result["samples"], [])

    def test_variant_directory_journal_is_available_and_legacy_duplicate_is_refused(self):
        legacy_path = self.journal
        variant = self.directory / "active"
        variant.mkdir()
        self.journal = variant / f"active.rbm.{self.run_id}.events.jsonl"
        self.write_events([event(self.run_id, 1, "SolverRunStarted"),
                           event(self.run_id, 2, "ControlStepAccepted", accepted())])
        result = self.query()
        self.assertEqual(result["availability"], "available")
        self.assertEqual(result["samples"][0]["sensors"][0]["value"], 0.25)
        legacy_path.write_bytes(self.journal.read_bytes())
        self.assertCode("TELEMETRY_AMBIGUOUS", self.query)

    def test_variant_directory_reparse_point_is_refused(self):
        variant = self.directory / "active"
        variant.mkdir()
        original_lstat = Path.lstat
        def replaced_lstat(path):
            info = original_lstat(path)
            if path != variant:
                return info
            from types import SimpleNamespace
            return SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
        with mock.patch.object(Path, "lstat", replaced_lstat):
            self.assertCode("TELEMETRY_PATH", self.query)

    def test_partial_tail_is_retained_and_completed_on_next_query(self):
        metadata = json.dumps(event(self.run_id, 1, "SolverRunStarted"), separators=(",", ":")).encode() + b"\n"
        encoded = json.dumps(event(self.run_id, 2, "ControlStepAccepted", accepted()),
                             separators=(",", ":")).encode()
        split = len(encoded) // 2
        self.journal.write_bytes(metadata + encoded[:split])

        partial = self.query()
        self.assertEqual(partial["availability"], "waiting")
        self.assertEqual(partial["samples"], [])
        with self.journal.open("ab") as stream:
            stream.write(encoded[split:] + b"\n")

        result = self.query()
        self.assertEqual(result["availability"], "available")
        self.assertEqual(result["runId"], self.run_id)
        self.assertEqual(result["samples"], [{"seq": 2, "timeS": 0.005, "nextTimeS": 0.01,
            "sensors": [{"id": "tip-u", "value": 0.25, "unit": "m"}],
            "actuators": [{"id": "root-force", "forceN": 2.0, "powerW": 0.5, "workJ": 0.01}]}])
        self.assertEqual(self.query(afterSeq=2)["samples"], [])

    def test_incremental_read_and_native_sequence_pagination(self):
        self.write_events([event(self.run_id, 1, "SolverRunStarted"),
                           event(self.run_id, 2, "ControlStepAccepted", accepted()),
                           event(self.run_id, 3, "ControlStepAccepted", accepted(0.01)),
                           event(self.run_id, 4, "ControlStepAccepted", accepted(0.015))])
        with mock.patch.object(telemetry, "MAX_READ_BYTES_PER_QUERY", 96):
            pages = []
            cursor = 0
            for _ in range(20):
                result = self.query(afterSeq=cursor, limit=1)
                pages.extend(result["samples"])
                if result["samples"]:
                    cursor = result["nextSeq"]
                if len(pages) == 3:
                    break
            self.assertEqual([item["seq"] for item in pages], [2, 3, 4])
            self.assertEqual([item["timeS"] for item in pages], [0.005, 0.01, 0.015])

    def test_cache_budget_evicts_other_jobs_and_allows_returning_to_them(self):
        self.write_events([event(self.run_id, 1, "ControlStepAccepted", accepted())])
        other_id, other_run = str(uuid.uuid4()), str(uuid.uuid4())
        other_dir = self.workspace / "jobs" / other_id
        other_dir.mkdir()
        other_path = other_dir / f"active.ram.{other_run}.events.jsonl"
        other_path.write_bytes(json.dumps(event(other_run, 1, "ControlStepAccepted", accepted()),
                                         separators=(",", ":")).encode() + b"\n")
        self.service._state["jobs"][other_id] = {**self.job, "jobId": other_id}
        with mock.patch.object(telemetry, "MAX_CACHED_SAMPLE_BYTES", self.journal.stat().st_size + 20):
            self.assertEqual(len(self.query()["samples"]), 1)
            self.assertEqual(len(request(self.service, "telemetry.query", {"jobId": other_id})["samples"]), 1)
            self.assertEqual(len(self.query()["samples"]), 1)

    def test_control_step_requires_accepted_status(self):
        record = event(self.run_id, 1, "ControlStepAccepted", accepted())
        record["status"] = "rejected"
        self.write_events([record])
        with self.assertRaises(WorkflowError) as error:
            self.query()
        self.assertEqual(error.exception.code, "TELEMETRY_CORRUPT")

    def test_corruption_remains_failed_after_first_error(self):
        self.write_events([event(self.run_id, 1, "SolverRunStarted"),
                           event(self.run_id, 3, "ControlStepAccepted", accepted())])
        for _ in range(2):
            self.assertCode("TELEMETRY_CORRUPT", self.query)

    def test_invalid_run_id_unit_magnitude_and_nonincreasing_time_are_rejected(self):
        cases = [
            [event(str(uuid.uuid4()), 1, "SolverRunStarted")],
            [event(self.run_id, 1, "ControlStepAccepted", accepted(unit="bad\nunit"))],
            [event(self.run_id, 1, "ControlStepAccepted", accepted(sensor_value=1e101))],
            [event(self.run_id, 1, "ControlStepAccepted", accepted()),
             event(self.run_id, 2, "ControlStepAccepted", accepted(0.005))],
        ]
        for items in cases:
            with self.subTest(events=items):
                self.service._telemetry = telemetry.TelemetryReader(self.workspace)
                self.write_events(items)
                self.assertCode("TELEMETRY_CORRUPT", self.query)

    def test_oversized_line_and_total_journal_are_rejected(self):
        self.journal.write_bytes(b"x" * (telemetry.MAX_EVENT_BYTES + 1) + b"\n")
        self.assertCode("TELEMETRY_LIMIT", self.query)
        self.service._telemetry = telemetry.TelemetryReader(self.workspace)
        self.write_events([event(self.run_id, 1, "SolverRunStarted")])
        with mock.patch.object(telemetry, "MAX_JOURNAL_BYTES", 8):
            self.assertCode("TELEMETRY_LIMIT", self.query)

    def test_multiple_journals_and_symlink_escape_are_refused(self):
        self.write_events([event(self.run_id, 1, "SolverRunStarted")])
        (self.directory / f"active.ram.{uuid.uuid4()}.events.jsonl").write_text("", encoding="utf-8")
        self.assertCode("TELEMETRY_AMBIGUOUS", self.query)

        external = self.base / "external"
        external.mkdir()
        for path in self.directory.glob("active.ram.*.events.jsonl"):
            path.unlink()
        linked = self.directory / f"active.ram.{self.run_id}.events.jsonl"
        try:
            linked.symlink_to(external / "outside.events.jsonl")
        except OSError as error:
            self.skipTest(f"file symlinks are unavailable: {error}")
        (external / "outside.events.jsonl").write_text("", encoding="utf-8")
        self.assertCode("TELEMETRY_PATH", self.query)

    def test_passive_run_without_control_samples_is_explicitly_unavailable(self):
        self.journal = self.directory / f"passive.ram.{self.run_id}.events.jsonl"
        self.write_events([event(self.run_id, 1, "SolverRunStarted"),
                           event(self.run_id, 2, "RunFinished")])
        result = self.query(mode="passive")
        self.assertEqual(result["availability"], "unavailable")
        self.assertIn("Passive", result["message"])

    def test_terminal_job_without_journal_is_unavailable(self):
        self.job["state"] = "cancelled"
        result = self.query()
        self.assertEqual(result["availability"], "unavailable")

    def test_recovered_history_uses_checkpoint_cutoff_and_accepted_sequence(self):
        attempt_run, _, _ = self.prepare_recovered_history(selected_seq=2)
        result = self.query_recovered_reader()
        samples = result["samples"]
        self.assertEqual([sample["seq"] for sample in samples], [1, 2, 3, 4])
        self.assertEqual([sample["timeS"] for sample in samples], [0.0, 0.005, 0.01, 0.015])
        self.assertEqual([sample["replay"] for sample in samples], [True, True, False, False])
        self.assertEqual([sample["originAttemptId"] for sample in samples[:2]], [self.job_id] * 2)
        self.assertEqual([sample["originAttemptId"] for sample in samples[2:]],
                         [self.job["attemptId"]] * 2)
        self.assertEqual([sample["runId"] for sample in samples[2:]], [attempt_run] * 2)
        self.assertEqual([sample["nativeEventSeq"] for sample in samples], [2, 3, 2, 3])
        self.assertEqual(len({sample["seq"] for sample in samples}), len(samples))
        self.assertEqual(result["nextSeq"], 4)
        self.assertEqual(result["availability"], "available")

    def test_recovered_history_paginates_by_logical_accepted_sequence(self):
        self.prepare_recovered_history(selected_seq=2)
        first = self.query_recovered_reader(limit=2)
        second = self.query_recovered_reader(after_seq=first["nextSeq"], limit=2)
        self.assertEqual([sample["seq"] for sample in first["samples"]], [1, 2])
        self.assertEqual([sample["seq"] for sample in second["samples"]], [3, 4])

    def test_recovery_on_recovery_preserves_each_selected_origin_prefix(self):
        _, _, _ = self.prepare_recovered_history(selected_seq=2)
        attempts = self.job["recovery"]["attempts"]
        first_recovery_id = self.job["attemptId"]
        second_id = str(uuid.uuid4())
        second_directory = self.directory / "attempts" / second_id
        second_variant = second_directory / "active"
        second_variant.mkdir(parents=True)
        second_run = str(uuid.uuid4())
        second_events = [event(second_run, 1, "SolverRunStarted"),
                         event(second_run, 2, "ControlStepAccepted", accepted(0.015))]
        (second_variant / f"active.rbm.{second_run}.events.jsonl").write_bytes(
            b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                    for item in second_events))
        run_result = attempts[-1]["partialResult"]
        attempts.append({"attemptId": second_id, "directory": str(second_directory),
                         "partialResult": run_result,
                         "inheritedSelection": {
                             "attemptId": first_recovery_id, "targetSteps": 4,
                             "qualificationSha256": "c" * 64,
                             "variants": {
                                 "active": {"phase": "checkpointed", "acceptedSeq": 3,
                                            "sourceAttemptId": first_recovery_id},
                                 "passive": {"phase": "prepared", "acceptedSeq": 0,
                                             "sourceAttemptId": first_recovery_id}}}})
        self.job["attemptId"] = second_id
        result = self.query_recovered_reader()
        samples = result["samples"]
        self.assertEqual([sample["seq"] for sample in samples], [1, 2, 3, 4])
        self.assertEqual([sample["originAttemptId"] for sample in samples],
                         [self.job_id, self.job_id, first_recovery_id, second_id])
        self.assertEqual([sample["replay"] for sample in samples], [True, True, True, False])

    def test_recovered_read_budget_covers_all_attempt_journals(self):
        self.prepare_recovered_history(selected_seq=2)
        with mock.patch.object(telemetry, "MAX_READ_BYTES_PER_QUERY", 256):
            result = self.query_recovered_reader()
        read_bytes = sum(journal.offset for journal in self.service._telemetry._journals.values())
        self.assertLessEqual(read_bytes, 256)
        self.assertTrue(result["hasMore"])

    def test_recovered_history_accepts_compact_source_attempt_identity(self):
        self.prepare_recovered_history(selected_seq=2)
        selection = self.job["recovery"]["attempts"][1]["inheritedSelection"]
        selection.pop("attemptId")
        selection["variants"]["active"]["sourceAttemptId"] = self.job_id
        result = self.query_recovered_reader()
        self.assertEqual([sample["seq"] for sample in result["samples"]], [1, 2, 3, 4])

    def test_recovered_query_does_not_mutate_job_facts(self):
        self.prepare_recovered_history(selected_seq=2)
        before = copy.deepcopy(self.job)
        self.query_recovered_reader()
        self.assertEqual(self.job, before)

    def test_recovered_history_rejects_unaligned_control_times(self):
        _, attempt_path, _ = self.prepare_recovered_history(selected_seq=2)
        run_id = str(uuid.uuid4())
        malformed = [event(run_id, 1, "SolverRunStarted"),
                     event(run_id, 2, "ControlStepAccepted", accepted(0.011))]
        attempt_path.unlink()
        attempt_path = attempt_path.with_name(f"active.rbm.{run_id}.events.jsonl")
        attempt_path.write_bytes(b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                                       for item in malformed))
        self.assertCode("TELEMETRY_CORRUPT", self.query_recovered_reader)

    def test_recovered_partial_tail_completes_without_reparsing_old_attempt(self):
        _, attempt_path, _ = self.prepare_recovered_history(selected_seq=2)
        # Replace the prepared attempt journal with an incomplete event tail.
        run_id = str(uuid.uuid4())
        events = [event(run_id, 1, "SolverRunStarted"),
                  event(run_id, 2, "ControlStepAccepted", accepted(0.01))]
        encoded = json.dumps(events[1], separators=(",", ":")).encode()
        split = len(encoded) // 2
        attempt_path.unlink()
        attempt_path = attempt_path.with_name(f"active.rbm.{run_id}.events.jsonl")
        attempt_path.write_bytes(json.dumps(events[0], separators=(",", ":")).encode() + b"\n" + encoded[:split])

        first = self.query_recovered_reader(after_seq=2)
        self.assertEqual(first["samples"], [])
        reader = self.service._telemetry
        original_journal = reader._journals[(self.job_id, "active", self.job_id)]
        original_offset = original_journal.offset
        with attempt_path.open("ab") as stream:
            stream.write(encoded[split:] + b"\n")
        second = self.query_recovered_reader(after_seq=2)
        self.assertEqual([sample["seq"] for sample in second["samples"]], [3])
        self.assertEqual(second["samples"][0]["runId"], run_id)
        self.assertEqual(original_journal.offset, original_offset)

    def test_recovered_history_refuses_ambiguous_origin_journals(self):
        _, _, original_path = self.prepare_recovered_history(selected_seq=2)
        variant_directory = self.directory / "active"
        variant_directory.mkdir()
        duplicate = variant_directory / f"active.rbm.{uuid.uuid4()}.events.jsonl"
        duplicate.write_bytes(original_path.read_bytes())
        self.assertCode("TELEMETRY_AMBIGUOUS", self.query_recovered_reader)

    def test_recovered_history_does_not_fall_back_after_origin_journal_disappears(self):
        _, _, original_path = self.prepare_recovered_history(selected_seq=2)
        self.query_recovered_reader()
        original_path.unlink()
        self.assertCode("TELEMETRY_CHANGED", self.query_recovered_reader)

    def test_recovered_history_selection_cannot_change_after_projection(self):
        self.prepare_recovered_history(selected_seq=2)
        self.query_recovered_reader()
        self.job["recovery"]["attempts"][1]["inheritedSelection"]["variants"]["active"]["acceptedSeq"] = 1
        self.assertCode("TELEMETRY_CHANGED", self.query_recovered_reader)

    def test_recovered_history_refuses_symlink_attempt_directory(self):
        attempt_id = str(uuid.uuid4())
        attempt_directory = self.directory / "attempts" / attempt_id
        external = self.base / "external"
        external.mkdir()
        (self.directory / "attempts").mkdir()
        try:
            attempt_directory.symlink_to(external, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks are unavailable: {error}")
        result = {"jobId": self.job_id, "mode": "both", "totalSteps": 4,
                  "dtSeconds": 0.005, "configuration": {"dt": 0.005}}
        self.job.update(command="workflow.recover", state="running", attemptId=attempt_id,
                        recovery={"schema": "range.workflow-recovery/v1", "attempts": [
                            {"attemptId": self.job_id, "directory": str(self.directory),
                             "partialResult": result},
                            {"attemptId": attempt_id, "directory": str(attempt_directory),
                             "partialResult": result,
                             "inheritedSelection": {"attemptId": self.job_id, "variants": {
                                 "active": {"phase": "checkpointed", "acceptedSeq": 2}}}}]})
        self.assertCode("TELEMETRY_PATH", self.query_recovered_reader)

    def test_recovered_history_refuses_unknown_selection_origin(self):
        self.prepare_recovered_history(selected_seq=2)
        inherited = self.job["recovery"]["attempts"][1]["inheritedSelection"]
        inherited["variants"]["active"]["sourceAttemptId"] = str(uuid.uuid4())
        self.assertCode("TELEMETRY_AMBIGUOUS", self.query_recovered_reader)

    def test_recovered_passive_telemetry_is_explicitly_unavailable(self):
        self.prepare_recovered_history(selected_seq=2)
        result = self.query_recovered_reader(mode="passive")
        self.assertEqual(result["availability"], "unavailable")
        self.assertIn("Passive", result["message"])


if __name__ == "__main__":
    unittest.main()
