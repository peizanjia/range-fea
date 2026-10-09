"""WF10 adapter tests with a fake *verified* snapshot broker, not FE evidence."""
from __future__ import annotations

import base64
import copy
import csv
import hashlib
import io
import math
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import runtime_parameters, series_sources, series_workbench
from range_workflow.service import WorkflowError, canonical, file_hash


class VerifiedSnapshots:
    def __init__(self, count=8, dt=.01):
        self.count, self.dt = count, dt
        self.calls, self.catalog_counts = [], {}
        self.mutate = None
        self.final_pin_mutation = False
        self.work_available = False
        self.value = lambda channel, index: float(index + 1)
        self.model = lambda job: "a" * 64
        self.time = lambda index: (index + 1) * self.dt

    def __call__(self, service, operation, parameters):
        self.calls.append((operation, copy.deepcopy(parameters)))
        key = tuple(parameters[k] for k in ("jobId", "mode", "artifactId"))
        pin = {"jobId": key[0], "mode": key[1], "artifactId": key[2],
               "sourceKind": f"native-{key[1]}-fe", "manifestSha256": "c" * 64,
               "sourceSnapshotRef": {"kind": "template", "usesOpenDocument": False,
                                     "model": "four-rod", "sourceSha256": self.model(key[0])},
               "proofScope": "verified-native-manifest/v1", "frameCount": self.count}
        if operation == "catalog":
            self.catalog_counts[key] = self.catalog_counts.get(key, 0) + 1
            if self.final_pin_mutation and self.catalog_counts[key] > 1:
                pin["manifestSha256"] = "d" * 64
            return {**pin, "analysis": {"domain": "structural-fe", "type": "structural-dynamics"},
                    "sampleScope": "all-persisted-numbered-model-records"}
        assert operation == "snapshot"
        channels = []
        for channel in parameters["channelIds"]:
            if channel == "control:work":
                if not self.work_available:
                    raise WorkflowError("CHANNEL_UNAVAILABLE", "No actual accepted control work ledger")
                channels.append({"channelId": channel, "variableId": "control-cumulative-work",
                    "quantity": "controlWork", "axis": "scalar", "unit": "J", "nodeTarget": "accepted-control-ledger"})
                continue
            _, node, quantity, axis = channel.split(":")
            variable, unit = series_sources._PHYSICAL[quantity]
            channels.append({"channelId": channel, "variableId": variable, "quantity": quantity,
                             "axis": axis, "unit": unit, "nodeTarget": "persistent-node-physical-id"})
        result = {**pin, "valid": True, "operation": "snapshot",
                  "apiVersion": "range.native-series-snapshot/v1", "schemaId": "range.native-series-snapshot/v1",
                  "completed": True, "live": False, "channels": channels,
                  "frames": [{"recordNo": i + 1, "timeS": self.time(i), "modelSha256": "e" * 64,
                              "originKind": "reused-history" if i < 2 else "native-solver",
                              "originSha256": "f" * 64, "historyProofSha256": "1" * 64,
                              "values": [self.value(channel, i) for channel in parameters["channelIds"]]}
                             for i in range(self.count)]}
        return self.mutate(result) if self.mutate else result


class WorkbenchTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.workspace = Path(self.temporary.name)
        self.jobs = {}
        self.service = SimpleNamespace(workspace=self.workspace, _job=lambda job_id: copy.deepcopy(self.jobs[job_id]))
        self.source = {"kind": "native", "jobId": "job-1", "mode": "active", "artifactId": "artifact-1",
                       "channelId": "node:51000:velocity:X"}
        self.broker = VerifiedSnapshots()
        self.patch = mock.patch.object(series_sources, "query_registered", side_effect=self.broker)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def call(self, operation, parameters):
        return series_workbench.handle(self.service, operation, parameters)

    def plot(self, view="time", sources=None, options=None):
        return self.call("result.plot.query", {"view": view, "sources": sources or [self.source],
                                               "options": options or {}})

    def expect_error(self, code, function):
        with self.assertRaises(WorkflowError) as caught:
            function()
        self.assertEqual(caught.exception.code, code)

    def register_reference(self):
        directory = self.workspace / "jobs" / "job-1"
        directory.mkdir(parents=True)
        linear = {"dofs": [{"nodePhysicalId": "51000", "unit": "m", "globalDirection": [1, 0, 0]}],
                  "outputs": [{"id": "tip-velocity", "nodePhysicalId": "51000", "unit": "m/s", "quantity": "Velocity"}],
                  "Sv": [[1]], "M": [[2]], "K": [[3]]}
        reference = {"sensorId": "tip-velocity", "initialQ": [0], "initialV": [0],
                     "timeOutputs": {"timeSeconds": [0] + [self.broker.time(i) for i in range(self.broker.count)],
                                     "fullVelocityMPerSecond": [0] + [float(i + 1) for i in range(self.broker.count)]}}
        gains = {"sensor": {"id": "tip-velocity"}}
        result, artifacts = {"configuration": {"dt": self.broker.dt}}, []
        for name, data in (("reference", reference), ("linear", linear), ("gains", gains)):
            path = directory / (name + ".json")
            path.write_bytes(canonical(data))
            result[name + "Path"] = str(path)
            artifacts.append({"path": str(path), "sha256": file_hash(path)})
        self.jobs["job-1"] = {"jobId": "job-1", "state": "succeeded", "result": result, "artifacts": artifacts}
        return {**self.source, "kind": "reference", "referenceSha256": artifacts[0]["sha256"]}

    def test_four_traces_bounded_independent_models_truthful_passive(self):
        self.broker.count = 5000
        self.broker.model = lambda job: hashlib.sha256(job.encode()).hexdigest()
        selected = [{**self.source, "jobId": f"job-{i + 1}", "mode": "active" if i % 2 == 0 else "passive"} for i in range(4)]
        result = self.plot(sources=selected)
        self.assertEqual(len(result["traces"]), 4)
        for trace in result["traces"]:
            self.assertEqual(trace["rawSampleCount"], 5000)
            self.assertLessEqual(trace["displaySampleCount"], 1000)
            self.assertEqual(trace["points"][0]["ordinal"], 0)
            self.assertEqual(trace["points"][-1]["ordinal"], 4999)
            self.assertTrue(trace["displayOnly"])
        self.assertEqual(result["traces"][1]["sources"][0]["sourceKind"], "native-passive-fe")
        self.assertEqual(sum(op == "snapshot" for op, _ in self.broker.calls), 4)

    def test_shared_snapshot_for_two_channels_compact_proof_budget(self):
        self.broker.count = 5000
        resolved = series_sources.resolve_many(self.service, [self.source, {**self.source, "channelId": "node:51001:velocity:X"}])
        self.assertEqual(sum(op == "snapshot" for op, _ in self.broker.calls), 1)
        self.assertLess(len(canonical(resolved[0]["dto"]["origin"])), 512)
        self.assertNotIn("origin", resolved[0]["dto"]["samples"][0])
        self.assertEqual(len(resolved[0]["records"]), 5000)

    def test_spike_envelope_preserves_peak_and_ordinals(self):
        self.broker.count = 5000
        self.broker.value = lambda c, i: 9999. if i in (201, 4990) else 0.
        result = self.plot(options={"limit": 40})
        ordinals = [p["ordinal"] for p in result["traces"][0]["points"]]
        self.assertIn(201, ordinals)
        self.assertIn(4990, ordinals)
        self.assertEqual(ordinals, sorted(set(ordinals)))

    def test_fft_full_raw_before_bin_reduction(self):
        self.broker.count = 2048
        self.broker.value = lambda c, i: 3 * math.cos(2 * math.pi * 713 * i / 2048)
        result = self.plot("amplitude", options={"limit": 8, "window": "rectangular", "removeMean": False})
        trace = result["traces"][0]
        peak = max(trace["points"], key=lambda p: p["y"])
        self.assertEqual(peak["ordinal"], 713)
        self.assertAlmostEqual(peak["y"], 3, places=10)
        self.assertEqual(trace["rawSampleCount"], 2048)
        self.assertEqual(trace["spectralBinCount"], 1025)
        self.assertEqual(result["axes"]["x"]["unit"], "Hz")

    def test_psd_units_and_nonuniform_grid_rejection(self):
        self.assertEqual(self.plot("psd")["axes"]["y"]["unit"], "(m/s)^2/Hz")
        self.broker.time = lambda i: (i + 1) * .01 + (.001 if i > 3 else 0)
        self.expect_error("INCOMPARABLE_RESULTS", lambda: self.plot("amplitude"))

    def test_error_same_model_exact_grid_and_channel_required(self):
        other = {**self.source, "jobId": "job-2"}
        result = self.plot("error", [self.source, other])
        self.assertTrue(all(p["y"] == 0 for p in result["traces"][0]["points"]))
        self.broker.model = lambda job: ("a" if job == "job-1" else "b") * 64
        self.expect_error("INCOMPARABLE_RESULTS", lambda: self.plot("error", [self.source, other]))
        self.broker.model = lambda job: "a" * 64
        self.expect_error("INCOMPARABLE_RESULTS", lambda: self.plot("error", [self.source, {**other, "channelId": "node:51001:velocity:X"}]))

    def test_time_overlay_rejects_units_phase_allows_distinct_units(self):
        displacement = {**self.source, "channelId": "node:51000:displacement:X"}
        self.expect_error("INCOMPARABLE_RESULTS", lambda: self.plot("time", [self.source, displacement]))
        result = self.plot("phase", [displacement, self.source])
        self.assertEqual(result["axes"]["x"]["unit"], "m")
        self.assertEqual(result["axes"]["y"]["unit"], "m/s")

    def test_reference_initial_record_only_removal_and_physical_mapping(self):
        reference = self.register_reference()
        result = self.plot("error", [self.source, reference])
        self.assertTrue(all(p["y"] == 0 for p in result["traces"][0]["points"]))
        self.assertEqual(result["traces"][0]["sources"][1]["sourceKind"], "independent-reference")
        identity = series_sources.reference_identity(self.service, self.source)
        self.assertEqual(identity["selector"], reference)
        self.expect_error("REFERENCE_UNAVAILABLE", lambda: self.plot(sources=[{**reference, "channelId": "node:51001:velocity:X"}]))

    def test_reference_rejects_time_tolerance_and_forged_hash(self):
        reference = self.register_reference()
        self.expect_error("SOURCE_CHANGED", lambda: self.plot(sources=[{**reference, "referenceSha256": "b" * 64}]))
        path = Path(self.jobs["job-1"]["result"]["referencePath"])
        import json
        data = json.loads(path.read_bytes())
        data["timeOutputs"]["timeSeconds"][4] += 1e-15
        path.write_bytes(canonical(data))
        self.jobs["job-1"]["artifacts"][0]["sha256"] = file_hash(path)
        reference["referenceSha256"] = file_hash(path)
        self.expect_error("INCOMPARABLE_RESULTS", lambda: self.plot(sources=[reference]))

    def test_runtime_reference_actual_sv_row_accepted_schedule(self):
        reference = self.register_reference()
        parameters = runtime_parameters.make_parameters("a" * 64, 0, 1.,
                [{"actuatorId": "force", "minN": -5., "maxN": 5., "rateNPerS": 0.}])
        result = self.jobs["job-1"]["result"]
        result["runtimeParameters"] = {"schema": runtime_parameters.SCHEMA, "parameters": parameters, "history": []}
        result["runtimeProposalRecords"] = {}
        result["runtimeReference"] = {"trajectory": {"v": [[float(i)] for i in range(9)]}}
        reference = series_sources.reference_identity(self.service, self.source)["selector"]
        plotted = self.plot("error", [self.source, reference])
        self.assertTrue(all(p["y"] == 0 for p in plotted["traces"][0]["points"]))
        result["runtimeParameters"]["parameters"]["parameterSha256"] = "b" * 64
        self.expect_error("SOURCE_CHANGED", lambda: self.plot(sources=[reference]))

    def test_energy_actual_histories_checked_matrices_complete_export(self):
        self.register_reference()
        result = self.jobs["job-1"]["result"]
        result["sourceReference"] = {"sourceSha256": "a" * 64, "analyticalM": [[2]], "analyticalK": [[3]],
                "dofs": [{"nodePhysicalId": "51000", "unit": "m", "globalDirection": [1, 0, 0]}],
                "matrixComparison": {"mass": {"maxAbsoluteError": 0}, "stiffness": {"maxAbsoluteError": 0}}}
        evidence_path = self.workspace / "jobs" / "job-1" / "source-reference.json"
        evidence_path.write_bytes(canonical(result["sourceReference"]))
        result["sourceReferencePath"] = str(evidence_path)
        self.jobs["job-1"]["artifacts"].append({"path": str(evidence_path), "sha256": file_hash(evidence_path)})
        self.broker.value = lambda channel, i: 2. if ":displacement:" in channel else 3.
        energy = {"kind": "energy", "jobId": "job-1", "mode": "active", "artifactId": "artifact-1"}
        plot = self.plot("energy", [energy])
        self.assertTrue(plot["export"]["available"])
        self.assertEqual(plot["traces"][0]["points"][0]["y"], 15.)
        metadata = plot["traces"][0]["sources"][0]
        self.assertTrue(metadata["derived"])
        self.assertEqual(metadata["sourceKind"], "derived-mechanical-energy")
        self.assertEqual(len(metadata["physicalChannelIds"]), 2)
        exported = self.call("result.raw.export", {"sources": plot["export"]["sources"], "range": "complete"})
        self.assertEqual(exported["count"], 8)

    def test_missing_energy_and_work_return_unavailable_without_fake_values(self):
        self.register_reference()
        energy = {"kind": "energy", "jobId": "job-1", "mode": "active", "artifactId": "artifact-1"}
        for view, chosen in (("energy", energy), ("work", self.source)):
            result = self.plot(view, [chosen])
            self.assertEqual(result["traces"], [])
            self.assertTrue(result["unavailable"])
            self.assertFalse(result["export"]["available"])

    def test_actual_ledger_work_values_and_passive_unavailability_are_distinct(self):
        self.broker.work_available = True
        self.broker.value = lambda channel, index: -2.0 * (index + 1) if channel == "control:work" else float(index + 1)
        passive = {**self.source, "mode": "passive"}
        result = self.plot("work", [self.source, passive])
        self.assertEqual(result["axes"]["y"]["unit"], "J")
        self.assertEqual(len(result["traces"]), 1)
        self.assertEqual([point["y"] for point in result["traces"][0]["points"]], [-2.0 * (i + 1) for i in range(8)])
        self.assertTrue(result["export"]["available"])
        self.assertEqual(len(result["unavailable"]), 1)
        self.assertEqual(result["export"]["sources"][0]["channelId"], "control:work")

    def test_raw_pages_original_frame_identity_complete_export_chunks(self):
        self.broker.count = 5000
        plot = self.plot(options={"limit": 40})
        pinned = plot["export"]["sources"][0]
        raw = self.call("result.raw.query", {"source": pinned, "offset": 201, "limit": 2})
        self.assertEqual(raw["points"][0]["ordinal"], 201)
        self.assertEqual(raw["points"][0]["recordNo"], 202)
        self.assertEqual(raw["points"][0]["modelSha256"], "e" * 64)
        exported = self.call("result.raw.export", {"sources": [pinned], "range": "complete"})
        chunks, offset = [], 0
        while offset < exported["bytes"]:
            chunk = self.call("result.raw.export.chunk", {"exportId": exported["exportId"], "offset": offset, "length": 65536})
            decoded = base64.b64decode(chunk["data"])
            self.assertLessEqual(len(decoded), 65536)
            chunks.append(decoded)
            offset += len(decoded)
        data = b"".join(chunks)
        self.assertEqual(hashlib.sha256(data).hexdigest(), exported["sha256"])
        rows = list(csv.DictReader(io.StringIO(data.decode())))
        self.assertEqual(len(rows), 5000)
        self.assertEqual(rows[-1]["ordinal"], "4999")
        self.assertEqual(rows[-1]["sourceHash"], pinned["sourceHash"])
        self.assertNotIn("path", canonical(exported).decode().lower())

    def test_hold_identity_refuses_new_trace_but_existing_export_stays_frozen(self):
        held = self.plot()["export"]["sources"]
        exported = self.call("result.raw.export", {"sources": held, "range": "complete"})
        self.broker.value = lambda c, i: 99.
        self.expect_error("SOURCE_CHANGED", lambda: self.call("result.raw.export", {"sources": held, "range": "complete"}))
        chunk = self.call("result.raw.export.chunk", {"exportId": exported["exportId"], "offset": 0, "length": 65536})
        self.assertEqual(hashlib.sha256(base64.b64decode(chunk["data"])).hexdigest(), exported["sha256"])

    def test_snapshot_invalid_shapes_types_hash_order_units_and_path(self):
        mutations = [lambda r: r.update(live=True),
                     lambda r: r["frames"][0].update(recordNo=True),
                     lambda r: r["frames"][0].update(values=[True]),
                     lambda r: r["frames"][0].update(values=[math.inf]),
                     lambda r: r["frames"][1].update(timeS=r["frames"][0]["timeS"]),
                     lambda r: r["frames"][0].update(originSha256="bad"),
                     lambda r: r["channels"][0].update(unit="N"),
                     lambda r: r["sourceSnapshotRef"].update(privatePath="secret")]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                def changed(result):
                    mutate(result)
                    return result
                self.broker.mutate = changed
                with self.assertRaises(WorkflowError):
                    self.plot()

    def test_final_catalog_pin_change_rejected(self):
        self.broker.final_pin_mutation = True
        self.expect_error("SOURCE_CHANGED", self.plot)

    def test_no_arbitrary_paths_dtos_code_unpinned_exports_or_boolean_options(self):
        for extra in ({"path": "C:/secret"}, {"samples": []}, {"expression": "eval()"}):
            self.expect_error("INVALID_PARAMETERS", lambda: self.plot(sources=[{**self.source, **extra}]))
        self.expect_error("INVALID_PARAMETERS", lambda: self.plot(options={"removeMean": 1}))
        self.expect_error("INVALID_PARAMETERS", lambda: self.call("result.raw.query", {"source": self.source, "offset": True}))
        self.expect_error("INVALID_PARAMETERS", lambda: self.call("result.raw.export", {"sources": [self.source], "range": "complete"}))
        self.expect_error("INVALID_PARAMETERS", lambda: self.call("result.raw.export", {"sources": self.plot()["export"]["sources"], "range": "display"}))

    def test_reference_artifact_tampering_and_failed_job(self):
        reference = self.register_reference()
        Path(self.jobs["job-1"]["result"]["referencePath"]).write_bytes(b"{}")
        self.expect_error("SOURCE_CHANGED", lambda: self.plot(sources=[reference]))
        self.jobs["job-1"]["state"] = "failed"
        self.expect_error("RESULT_UNAVAILABLE", lambda: self.plot(sources=[reference]))

    def test_export_tamper_same_size_missing_unknown_and_chunk_bounds(self):
        exported = self.call("result.raw.export", {"sources": self.plot()["export"]["sources"], "range": "complete"})
        path = self.workspace / "derived" / "series-exports" / (exported["exportId"] + ".csv")
        original = path.read_bytes()
        path.write_bytes(b"x" + original[1:])
        request = {"exportId": exported["exportId"], "offset": 0, "length": 65536}
        self.expect_error("SOURCE_CHANGED", lambda: self.call("result.raw.export.chunk", request))
        path.write_bytes(original)
        self.expect_error("INVALID_PARAMETERS", lambda: self.call("result.raw.export.chunk", {**request, "length": 65537}))
        self.expect_error("INVALID_PARAMETERS", lambda: self.call("result.raw.export.chunk", {**request, "offset": exported["bytes"] + 1}))
        self.expect_error("EXPORT_UNAVAILABLE", lambda: self.call("result.raw.export.chunk", {**request, "exportId": "unknown"}))
        path.unlink()
        self.expect_error("SOURCE_CHANGED", lambda: self.call("result.raw.export.chunk", request))

    def test_export_publication_io_failures_remove_owned_csv_and_registry_entry(self):
        selected = self.plot()["export"]["sources"]
        parameters = {"sources": selected, "range": "complete"}
        real_open = Path.open
        class FailedWrite:
            def __init__(self, stream):
                self.stream = stream
            def fileno(self):
                return self.stream.fileno()
            def __enter__(self):
                return self
            def __exit__(self, *args):
                return self.stream.__exit__(*args)
            def write(self, value):
                raise OSError("Injected CSV write failure")
        def failed_write(path, mode="r", *args, **kwargs):
            stream = real_open(path, mode, *args, **kwargs)
            return FailedWrite(stream) if mode == "xb" else stream
        failures = {
            "write": lambda: mock.patch.object(Path, "open", failed_write),
            "fsync": lambda: mock.patch.object(series_workbench.os, "fsync", side_effect=OSError("Injected fsync failure")),
            "verification-hash": lambda: mock.patch.object(series_workbench, "file_hash", return_value="0" * 64),
        }
        for stage, patch in failures.items():
            with self.subTest(stage=stage), patch():
                self.expect_error("EXPORT_FAILED", lambda: self.call("result.raw.export", parameters))
            self.assertEqual(getattr(self.service, "_series_exports", {}), {})
            self.assertEqual(list(self.workspace.glob("derived/series-exports/*.csv")), [])
        # Failed attempts did not consume a quota slot; authentic publication still works.
        exported = self.call("result.raw.export", parameters)
        self.assertEqual(len(self.service._series_exports), 1)
        self.assertEqual(len(list(self.workspace.glob("derived/series-exports/*.csv"))), 1)
        self.assertEqual(exported["sources"][0]["sourceHash"], selected[0]["sourceHash"])

    def test_export_source_gate_failure_after_writing_removes_only_owned_csv(self):
        selected = self.plot()["export"]["sources"]
        gate = mock.Mock(side_effect=[None, WorkflowError("READ_SOURCE_CHANGED", "Injected stale source")])
        self.service._read_source_gate = gate
        self.expect_error("READ_SOURCE_CHANGED", lambda: self.call("result.raw.export", {
            "sources": selected, "range": "complete"}))
        self.assertEqual(gate.call_count, 2)
        self.assertEqual(self.service._series_exports, {})
        self.assertEqual(list(self.workspace.glob("derived/series-exports/*.csv")), [])

    def test_export_uuid_collision_preserves_existing_bytes_and_empty_registry(self):
        parameters = {"sources": self.plot()["export"]["sources"], "range": "complete"}
        export_id = "11111111-1111-4111-8111-111111111111"
        root = series_workbench._export_root(self.service, create=True)
        path = root / (export_id + ".csv")
        original = b"preexisting owned-by-another-attempt CSV bytes\n"
        path.write_bytes(original)
        with mock.patch.object(series_workbench.uuid, "uuid4", return_value=export_id):
            self.expect_error("EXPORT_FAILED", lambda: self.call("result.raw.export", parameters))
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(self.service._series_exports, {})
        self.assertEqual(len(list(path.parent.glob("*.csv"))), 1)

    def test_export_symlink_directory_rejected(self):
        outside = self.workspace / "outside"
        outside.mkdir()
        derived = self.workspace / "derived"
        try:
            os.symlink(outside, derived, target_is_directory=True)
        except OSError:
            self.skipTest("Host does not permit symbolic link creation")
        self.expect_error("ARTIFACT_PATH", lambda: self.call("result.raw.export", {"sources": self.plot()["export"]["sources"], "range": "complete"}))

    def test_export_symlink_file_rejected_even_with_identical_bytes(self):
        exported = self.call("result.raw.export", {"sources": self.plot()["export"]["sources"], "range": "complete"})
        path = self.workspace / "derived" / "series-exports" / (exported["exportId"] + ".csv")
        target = self.workspace / "copy.csv"
        target.write_bytes(path.read_bytes())
        path.unlink()
        try:
            os.symlink(target, path)
        except OSError:
            self.skipTest("Host does not permit symbolic link creation")
        self.expect_error("SOURCE_CHANGED", lambda: self.call("result.raw.export.chunk", {"exportId": exported["exportId"], "offset": 0, "length": 1}))


if __name__ == "__main__":
    unittest.main()
