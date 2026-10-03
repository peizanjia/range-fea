"""Focused contract tests for native accepted-control telemetry."""
from __future__ import annotations

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

    def write_events(self, events):
        self.journal.write_bytes(b"".join(json.dumps(item, separators=(",", ":")).encode() + b"\n"
                                       for item in events))

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


if __name__ == "__main__":
    unittest.main()
