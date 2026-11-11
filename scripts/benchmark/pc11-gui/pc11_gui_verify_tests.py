"""Verifier unit tests only; crafted metadata is never GUI qualification evidence."""
import base64
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pc11_gui_verify as verify


class VerifierGuards(unittest.TestCase):
    def event(self, **updates):
        result = {"schema": "range.piezo-gui-performance-event/v1", "guiPid": "123", "guiExecutable": __file__,
                  "measurementValid": True, "requestRoute": "product-native-path", "event": "request_begin"}
        result.update(updates)
        return result

    def metadata(self, schema, family, **updates):
        result = {"schema": schema, "family": family, "recordCount": 20001, "targetSteps": 20000,
                  "baseSeq": 0, "continuousBetweenSelectedRecords": True}
        result.update(updates)
        return result

    def test_fixture_route_rejected(self):
        with self.assertRaisesRegex(ValueError, "Fixture"):
            verify.check_event(self.event(requestRoute="trusted-fixture-authority"), 123, __file__)

    def test_invalid_measurement_rejected(self):
        with self.assertRaisesRegex(ValueError, "invalid"):
            verify.check_event(self.event(measurementValid=False), 123, __file__)

    def test_pid_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "process mismatch"):
            verify.check_event(self.event(guiPid="124"), 123, __file__)

    def test_executable_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "process mismatch"):
            verify.check_event(self.event(guiExecutable=__file__ + ".other"), 123, __file__)

    def test_superseded_plot_rejected(self):
        with self.assertRaisesRegex(ValueError, "superseded"):
            verify.check_event(self.event(event="request_superseded"), 123, __file__)

    def test_schema_rejected(self):
        with self.assertRaisesRegex(ValueError, "schema"):
            verify.check_event(self.event(schema="other"), 123, __file__)

    def test_exactly_one_event_required(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            verify.one([], lambda x: True, "commit")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            verify.one([{}, {}], lambda x: True, "commit")

    def test_full_metadata_needs_initial_record(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "axial", recordCount=20000)
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            verify.check_complete_metadata(metadata, "axialRod")

    def test_sparse_and_restart_prefix_rejected(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "axial", baseSeq=1)
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            verify.check_complete_metadata(metadata, "axialRod")
        metadata.update(baseSeq=0, continuousBetweenSelectedRecords=False)
        with self.assertRaisesRegex(ValueError, "nonconsecutive"):
            verify.check_complete_metadata(metadata, "axialRod")

    def test_wrong_family_rejected(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "axial")
        with self.assertRaisesRegex(ValueError, "Wrong family"):
            verify.check_complete_metadata(metadata, "flatPlate")

    def test_verified_compact_axial_family_maps_to_manifest_family(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "axial")
        verify.check_complete_metadata(metadata, "axialRod")
        event = self.event(event="canvas_painted", family="axial")
        verify.check_performance_family(event, "axialRod")

    def test_typed_compact_kernel_model_families_accepted(self):
        pairs = (("axial", "axialRod"), ("layered", "planarBeam"), ("layered", "flatPlate"))
        for kernel_family, model_family in pairs:
            with self.subTest(kernel_family=kernel_family, model_family=model_family):
                metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", kernel_family, modelFamily=model_family)
                verify.check_complete_metadata(metadata, model_family)
                event = self.event(event="canvas_painted", family=kernel_family, modelFamily=model_family)
                verify.check_performance_family(event, model_family)

    def test_compact_axial_cross_family_pairs_rejected(self):
        for manifest in ("planarBeam", "flatPlate"):
            with self.subTest(manifest=manifest):
                metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "axial")
                with self.assertRaisesRegex(ValueError, "Wrong family"):
                    verify.check_complete_metadata(metadata, manifest)
                event = self.event(event="canvas_painted", family="axial")
                with self.assertRaisesRegex(ValueError, "Wrong family"):
                    verify.check_performance_family(event, manifest)

    def test_unknown_compact_family_rejected(self):
        for kernel_family in (None, 3, "beam", "plate", "axialRod", "mixed", "unknown"):
            with self.subTest(kernel_family=kernel_family):
                metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", kernel_family, modelFamily="planarBeam")
                with self.assertRaisesRegex(ValueError, "Unknown compact kernel family"):
                    verify.check_complete_metadata(metadata, "planarBeam")
                event = self.event(event="canvas_painted", family=kernel_family, modelFamily="planarBeam")
                with self.assertRaisesRegex(ValueError, "Unknown compact kernel family"):
                    verify.check_performance_family(event, "planarBeam")

    def test_typed_compact_cross_family_pairs_rejected(self):
        pairs = (("axial", "planarBeam"), ("axial", "flatPlate"), ("layered", "axialRod"))
        for kernel_family, model_family in pairs:
            with self.subTest(kernel_family=kernel_family, model_family=model_family):
                metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", kernel_family, modelFamily=model_family)
                with self.assertRaisesRegex(ValueError, "Kernel/model family mismatch"):
                    verify.check_complete_metadata(metadata, model_family)
                event = self.event(event="canvas_painted", family=kernel_family, modelFamily=model_family)
                with self.assertRaisesRegex(ValueError, "Kernel/model family mismatch"):
                    verify.check_performance_family(event, model_family)

    def test_typed_compact_family_must_match_manifest(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "layered", modelFamily="planarBeam")
        with self.assertRaisesRegex(ValueError, "Wrong family"):
            verify.check_complete_metadata(metadata, "flatPlate")
        event = self.event(event="canvas_painted", family="layered", modelFamily="planarBeam")
        with self.assertRaisesRegex(ValueError, "Wrong family"):
            verify.check_performance_family(event, "flatPlate")

    def test_missing_typed_layered_family_rejected(self):
        metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "layered")
        with self.assertRaisesRegex(ValueError, "Typed modelFamily required"):
            verify.check_complete_metadata(metadata, "planarBeam")
        event = self.event(event="canvas_painted", family="layered")
        with self.assertRaisesRegex(ValueError, "Typed modelFamily required"):
            verify.check_performance_family(event, "planarBeam")

    def test_malformed_typed_model_family_rejected(self):
        for model_family in (None, "beam", "unknown", 3):
            with self.subTest(model_family=model_family):
                metadata = self.metadata("range.piezo-accepted-trace-timeseries/v1", "layered", modelFamily=model_family)
                with self.assertRaisesRegex(ValueError, "Unknown or malformed modelFamily"):
                    verify.check_complete_metadata(metadata, "planarBeam")
                event = self.event(event="canvas_painted", family="layered", modelFamily=model_family)
                with self.assertRaisesRegex(ValueError, "Unknown or malformed modelFamily"):
                    verify.check_performance_family(event, "planarBeam")

    def test_compact_schema_mismatch_rejected(self):
        metadata = self.metadata("range.piezo-accepted-timeseries/v2", "axial")
        with self.assertRaisesRegex(ValueError, "schema mismatch"):
            verify.check_complete_metadata(metadata, "axialRod")
        with self.assertRaisesRegex(ValueError, "schema mismatch"):
            verify.check_performance_family(self.event(schema="other", family="axial"), "axialRod")

    def test_legacy_metadata_keeps_canonical_family_names(self):
        pairs = (("axialRod", "axialRod"), ("planarBeam", "planarBeam"), ("flatPlate", "flatPlate"))
        for metadata_family, manifest in pairs:
            with self.subTest(metadata_family=metadata_family, manifest=manifest):
                metadata = self.metadata("range.piezo-accepted-timeseries/v1", metadata_family)
                verify.check_complete_metadata(metadata, manifest)
                typed_metadata = self.metadata("range.piezo-accepted-timeseries/v1", metadata_family, modelFamily=metadata_family)
                verify.check_complete_metadata(typed_metadata, manifest)
        compact_name = self.metadata("range.piezo-accepted-timeseries/v1", "axial")
        with self.assertRaisesRegex(ValueError, "Unknown legacy CSV family"):
            verify.check_complete_metadata(compact_name, "axialRod")

    def test_legacy_typed_family_must_agree_with_canonical_family(self):
        metadata = self.metadata("range.piezo-accepted-timeseries/v1", "axialRod", modelFamily="flatPlate")
        with self.assertRaisesRegex(ValueError, "Legacy family/modelFamily mismatch"):
            verify.check_complete_metadata(metadata, "axialRod")
        malformed = self.metadata("range.piezo-accepted-timeseries/v1", "axialRod", modelFamily=3)
        with self.assertRaisesRegex(ValueError, "Unknown or malformed legacy modelFamily"):
            verify.check_complete_metadata(malformed, "axialRod")
        missing_canonical = self.metadata("range.piezo-accepted-timeseries/v1", "axialRod", modelFamily="axialRod")
        missing_canonical.pop("family")
        with self.assertRaisesRegex(ValueError, "Unknown legacy CSV family"):
            verify.check_complete_metadata(missing_canonical, "axialRod")

    def test_hash_streamed_bytes_and_json_bom(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bytes"
            content = b"x" * (1024 * 1024 + 3)
            path.write_bytes(content)
            self.assertEqual(verify.sha(path), hashlib.sha256(content).hexdigest())
            path.write_text('{"schema":"unit-metadata-only"}\n', encoding="utf-8-sig")
            self.assertEqual(verify.read_lines(path), [{"schema": "unit-metadata-only"}])

    def test_utc_offset_matches_millisecond_event(self):
        self.assertEqual(verify.stamp("2026-10-12T09:00:00.000Z"), verify.stamp("2026-10-12T17:00:00+08:00"))


class WorkerQualificationContract(unittest.TestCase):
    def _write_json_lines(self, path, rows):
        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    def _qualified_fixture(self, root):
        installed = root / "installed"
        binary_dir = installed / "bin"
        binary_dir.mkdir(parents=True)
        executable = binary_dir / "fea.exe"
        executable.write_bytes(b"synthetic executable identity; no GUI process is launched")
        executable_sha = verify.sha(executable)
        profile = root / "profile"
        profile.mkdir()
        source = root / "source.rbm"
        source.write_bytes(b"synthetic source")
        workspace = root / "workspace"
        workspace.mkdir()
        result_model = root / "registered-result.rbm"
        result_model.write_bytes(b"synthetic registered result")
        csv_path = root / "accepted.csv"
        spec = {
            "executable": str(executable), "executableSha256": executable_sha,
            "installedRoot": str(installed), "family": "axialRod",
            "sourceModel": str(source), "workspace": str(workspace),
            "resultModel": str(result_model), "csv": str(csv_path), "sourceFingerprint": "1" * 64,
        }
        (root / "manifest.json").write_text(json.dumps(spec), encoding="utf-8")

        now = datetime.now(timezone.utc)
        utc = lambda value: value.isoformat(timespec="milliseconds").replace("+00:00", "Z")
        pid = 27123
        actions = [
            {"action": "verified-gui", "guiPid": pid, "utc": utc(now - timedelta(milliseconds=1010)), "detail": {"discoveryOnly": False,
             "image": str(executable), "sha256": executable_sha}},
            {"action": "baseline-sampler-ready", "utc": utc(now - timedelta(seconds=1))},
            {"action": "isolated-application-profile", "detail": {"launch": True,
             "environment": "RANGE_APPLICATION_PROFILE_DIR", "path": str(profile)}},
        ]
        for path in (source, workspace, result_model, csv_path):
            actions.append({"action": "picker-commit", "detail": {"path": str(path)}})
        terminal = {"status": "TERMINAL_NATIVE_20000", "jobId": "job-20000", "projectId": "project-1", "guiPid": pid,
                    "resultModel": str(result_model), "resultArtifactSha256": verify.sha(result_model),
                    "acceptedSeq": 20000, "stateFingerprintSha256": "6" * 64}
        (root / "terminal-native-job.json").write_text(json.dumps(terminal), encoding="utf-8")
        actions.append({"action": "terminal-native-20000", "guiPid": pid, "detail": terminal})
        actions.append({"action": "native-result-auto-bound", "detail": {
            **terminal, "binding": "product-automatic-native-job-result", "sourceFingerprint": spec["sourceFingerprint"]}})
        self._write_json_lines(root / "uia-actions.jsonl", actions)

        source_binding = json.dumps({"synthetic": True, "case": "qualification-worker-contract",
                                     "project": {"projectId": terminal["projectId"]}}, sort_keys=True).encode()
        source_binding_sha = hashlib.sha256(source_binding).hexdigest()
        source_fingerprint = "1" * 64
        base_model_sha = "2" * 64
        header_sha = "3" * 64
        context_sha = "4" * 64
        terminal_record_sha = "5" * 64
        terminal_state_sha = "6" * 64
        common = {
            "schema": verify.GUI_EVENT_SCHEMA, "guiPid": str(pid), "guiExecutable": str(executable),
            "measurementValid": True, "requestRoute": "product-native-path", "family": "axial",
            "totalRecords": 20001, "baseSeq": 0, "targetSteps": 20000, "acceptedSeq": 20000,
            "sourceFingerprint": source_fingerprint, "sourceBindingBytesBase64": base64.b64encode(source_binding).decode(),
            "sourceBindingSha256": source_binding_sha, "committedByteEnd": 987654,
            "configurationIdentity": "7" * 64, "backendIdentity": "8" * 64,
            "baseModelSha256": base_model_sha, "headerSha256": header_sha,
            "contextSha256": context_sha, "terminalRecordSha256": terminal_record_sha,
            "terminalStateSha256": terminal_state_sha,
        }
        plot_begin = {**common, "event": "request_begin", "operation": "plot", "requestId": "plot-1",
                      "arguments": {"nativePaths": [str(result_model)]},
                      "elapsedNanoseconds": 1_000_000_000, "utc": utc(now)}
        plot_end = {**common, "event": "canvas_painted", "operation": "plot", "requestId": "plot-1",
                    "elapsedNanoseconds": 2_000_000_000, "utc": utc(now + timedelta(milliseconds=100)),
                    "installedWindowQualified": True, "platform": "windows", "windowVisible": True,
                    "canvasVisible": True, "windowExposed": True, "canvasWidth": 800, "canvasHeight": 600,
                    "view": "time", "firstFE": 0, "lastFE": 20000, "firstDisplayedOrdinal": 0,
                    "lastDisplayedOrdinal": 20000, "displayRecords": 20001, "terminalStateSha256": terminal["stateFingerprintSha256"]}
        csv_begin = {**common, "event": "request_begin", "operation": "csv", "requestId": "csv-1",
                     "elapsedNanoseconds": 2_500_000_000, "utc": utc(now + timedelta(milliseconds=200))}

        payload_stream = __import__("io").StringIO(newline="")
        writer = csv.writer(payload_stream, lineterminator="\n")
        writer.writerow(["completedFE [1]", "time [s]", "controlTick [1]", "controlPhase [1]", "stateSha256", "value [1]"])
        for index in range(20001):
            writer.writerow([index, f"{index * 0.01:.2f}", index, "accepted", "a" * 64, f"{index * 0.001:.3f}"])
        payload = payload_stream.getvalue().encode("utf-8")
        metadata = {
            "schema": verify.COMPACT_TRACE_METADATA_SCHEMA, "family": "axial", "recordCount": 20001,
            "targetSteps": 20000, "baseSeq": 0, "continuousBetweenSelectedRecords": True,
            "csvPayloadSha256": hashlib.sha256(payload).hexdigest(), "sourceFingerprint": source_fingerprint,
            "configurationIdentity": "7" * 64, "backendIdentity": "8" * 64,
            "baseModelSha256": base_model_sha, "headerSha256": header_sha, "contextSha256": context_sha,
        }
        csv_path.write_bytes(b"# " + json.dumps(metadata, separators=(",", ":")).encode("utf-8") + b"\n" + payload)
        csv_end = {**common, "event": "csv_committed", "operation": "csv", "requestId": "csv-1",
                   "elapsedNanoseconds": 3_500_000_000, "utc": utc(now + timedelta(milliseconds=300)),
                   "continuousFE": True, "destination": str(csv_path)}
        self._write_json_lines(root / "gui-performance.jsonl", [plot_begin, plot_end, csv_begin, csv_end])

        monotonic_offset = __import__("time").monotonic() - __import__("time").time()
        (root / "working-set").mkdir()
        summary = {
            "status": "MEASURED", "rootPid": pid, "expectedRootImage": str(executable),
            "samplingIntervalMilliseconds": 50, "includeBaseline": True, "subtractSharedLibraries": False,
            "sampleCount": 20, "maximumWorkingSetBytes": 400000000, "maximumSampleGapMilliseconds": 50.0,
            "firstSampleMonotonicNs": int((now.timestamp() + monotonic_offset - 1.0) * 1e9),
            "lastSampleMonotonicNs": int((now.timestamp() + monotonic_offset + 1.0) * 1e9),
        }
        (root / "working-set" / "summary.json").write_text(json.dumps(summary), encoding="utf-8")

        physical_dir = root / "physical-ram"
        physical_dir.mkdir()
        first_ns = int((now.timestamp() + monotonic_offset - 1.0) * 1e9)
        last_ns = int((now.timestamp() + monotonic_offset + 1.0) * 1e9)
        total_bytes = 1_000_000_000
        available_bytes = 300_000_000
        samples = []
        for index in range(20):
            monotonic_ns = first_ns + (last_ns - first_ns) * index // 19
            samples.append({
                "monotonic_ns": monotonic_ns,
                "elapsed_seconds": (monotonic_ns - first_ns) / 1e9,
                "total_physical_bytes": total_bytes,
                "available_physical_bytes": available_bytes,
                "used_percent": 100.0 * (1.0 - available_bytes / total_bytes),
            })
        self._write_physical_ram_series(physical_dir, samples)
        return executable

    def _write_physical_ram_series(self, physical_dir, samples):
        columns = verify.PHYSICAL_RAM_SAMPLE_COLUMNS
        normalized = [{
            "monotonic_ns": int(sample["monotonic_ns"]),
            "elapsed_seconds": float(sample["elapsed_seconds"]),
            "total_physical_bytes": int(sample["total_physical_bytes"]),
            "available_physical_bytes": int(sample["available_physical_bytes"]),
            "used_percent": float(sample["used_percent"]),
        } for sample in samples]
        with (physical_dir / "physical-ram.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns, lineterminator="\n")
            writer.writeheader()
            writer.writerows(normalized)
        samples = normalized
        times = [sample["monotonic_ns"] for sample in samples]
        gaps = [(right - left) / 1e6 for left, right in zip(times, times[1:])]
        summary = {
            "schema": "range.pc11-physical-ram/v1", "status": "MEASURED",
            "source": "GlobalMemoryStatusEx", "samplingIntervalMilliseconds": 50,
            "sampleCount": len(samples), "totalPhysicalBytes": samples[0]["total_physical_bytes"],
            "maximumUsedPercent": max(sample["used_percent"] for sample in samples),
            "maximumSampleGapMilliseconds": max(gaps, default=0.0),
            "firstSampleMonotonicNs": times[0], "lastSampleMonotonicNs": times[-1],
            "coveredDurationMilliseconds": (times[-1] - times[0]) / 1e6,
        }
        (physical_dir / "summary.json").write_text(json.dumps(summary), encoding="utf-8")

    def _rewrite_physical_ram_usage(self, physical_dir, available_bytes):
        samples_path = physical_dir / "physical-ram.csv"
        with samples_path.open(newline="", encoding="utf-8") as stream:
            samples = list(csv.DictReader(stream))
        for sample in samples:
            total = int(sample["total_physical_bytes"])
            sample["available_physical_bytes"] = available_bytes
            sample["used_percent"] = 100.0 * (1.0 - available_bytes / total)
            sample["monotonic_ns"] = int(sample["monotonic_ns"])
            sample["elapsed_seconds"] = float(sample["elapsed_seconds"])
            sample["total_physical_bytes"] = total
        self._write_physical_ram_series(physical_dir, samples)

    def _mutate_jsonl(self, path, mutate):
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]
        mutate(rows)
        self._write_json_lines(path, rows)

    def test_automatic_result_binding_is_bound_to_terminal_admission(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            actions_path = root / "uia-actions.jsonl"
            self._mutate_jsonl(actions_path, lambda rows: next(a for a in rows if a["action"] == "native-result-auto-bound")["detail"].update(jobId="other-job"))
            with self.assertRaisesRegex(ValueError, "differs from terminal jobId"):
                verify.qualify(root)

    def test_terminal_action_must_be_unique_and_gui_owned(self):
        for mutation, message in (
            (lambda rows: next(a for a in rows if a["action"] == "terminal-native-20000").update(guiPid=1), "GUI process identity"),
            (lambda rows: rows.append(next(a for a in rows if a["action"] == "terminal-native-20000")), "exactly one terminal native job admission action"),
        ):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self._qualified_fixture(root)
                self._mutate_jsonl(root / "uia-actions.jsonl", mutation)
                with self.assertRaisesRegex(ValueError, message):
                    verify.qualify(root)

    def test_automatic_plot_must_use_terminal_result_and_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            def mutate(rows):
                next(e for e in rows if e.get("event") == "request_begin" and e.get("operation") == "plot")["arguments"].update(nativePaths=["elsewhere.rbm"])
            self._mutate_jsonl(root / "gui-performance.jsonl", mutate)
            with self.assertRaisesRegex(ValueError, "Plot did not use"):
                verify.qualify(root)

    def test_plot_terminal_state_and_project_must_match_admission(self):
        for change, message in (
            (lambda event: event.update(terminalStateSha256="9" * 64), "Plot terminal state"),
            (lambda event: event.update(sourceFingerprint="9" * 64), "Plot source fingerprint"),
            (lambda event: event.update(sourceBindingBytesBase64=base64.b64encode(json.dumps({"project": {"projectId": "other"}}).encode()).decode()), "Plot source binding project"),
        ):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self._qualified_fixture(root)
                def mutate(rows):
                    change(next(e for e in rows if e.get("event") == "canvas_painted"))
                self._mutate_jsonl(root / "gui-performance.jsonl", mutate)
                with self.assertRaisesRegex(ValueError, message):
                    verify.qualify(root)

    def test_result_picker_is_not_required_but_csv_picker_is(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            actions_path = root / "uia-actions.jsonl"
            self._mutate_jsonl(actions_path, lambda rows: rows.__setitem__(slice(None), [a for a in rows if not (a["action"] == "picker-commit" and a["detail"]["path"].endswith("registered-result.rbm"))]))
            self.assertEqual(verify.qualify(root)["status"], "QUALIFIED")
            self._mutate_jsonl(actions_path, lambda rows: rows.__setitem__(slice(None), [a for a in rows if not (a["action"] == "picker-commit" and a["detail"]["path"].endswith("accepted.csv"))]))
            with self.assertRaisesRegex(ValueError, "CSV destination"):
                verify.qualify(root)

    def test_physical_ram_limit_is_strictly_below_90_percent(self):
        for available_bytes, expected_status in ((100_010_000, "QUALIFIED"), (100_000_000, "FAILED_LIMITS")):
            with self.subTest(available_bytes=available_bytes), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self._qualified_fixture(root)
                self._rewrite_physical_ram_usage(root / "physical-ram", available_bytes)
                result = verify.qualify(root)
                self.assertEqual(result["status"], expected_status)
                self.assertEqual(result["maximumPhysicalRamUsedPercentLimit"], 90.0)
                self.assertEqual(result["guiAcceptanceVerified"], expected_status == "QUALIFIED")

    def test_missing_physical_ram_series_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            (root / "physical-ram" / "physical-ram.csv").unlink()
            with self.assertRaisesRegex(ValueError, "Physical RAM series missing"):
                verify.qualify(root)

    def test_incomplete_physical_ram_series_must_cover_plot_and_csv_requests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            physical_dir = root / "physical-ram"
            with (physical_dir / "physical-ram.csv").open(newline="", encoding="utf-8") as stream:
                samples = list(csv.DictReader(stream))
            self._write_physical_ram_series(physical_dir, samples[:-8])
            with self.assertRaisesRegex(ValueError, "did not cover the complete GUI requests"):
                verify.qualify(root)

    def test_valid_physical_ram_summary_is_joined_and_working_set_stays_informational(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            ws_path = root / "working-set" / "summary.json"
            ws = json.loads(ws_path.read_text(encoding="utf-8"))
            ws["maximumWorkingSetBytes"] = 700_000_000
            ws_path.write_text(json.dumps(ws), encoding="utf-8")
            result = verify.qualify(root)
            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(result["maximumPhysicalRamUsedPercent"], 70.0)
            self.assertEqual(result["physicalRamSampleCount"], 20)
            self.assertGreater(result["physicalRamCoverageMilliseconds"], 1900.0)
            self.assertTrue(result["workingSetInformational"])
            self.assertEqual(result["maximumWorkingSetBytes"], 700_000_000)

    def test_missing_or_invalid_working_set_does_not_override_physical_ram_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            summary_path = root / "working-set" / "summary.json"
            summary_path.unlink()
            result = verify.qualify(root)
            self.assertEqual(result["status"], "QUALIFIED")
            self.assertTrue(result["workingSetInformational"])
            self.assertEqual(result["workingSetStatus"], "UNAVAILABLE")
            self.assertIsNone(result["maximumWorkingSetBytes"])

            summary_path.write_text(json.dumps({"status": "INVALID", "error": "mock identity failure"}))
            result = verify.qualify(root)
            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(result["workingSetStatus"], "INVALID")
            self.assertIsNone(result["maximumWorkingSetBytes"])

    def _run_production_worker_predicate(self, result, worker_path, root, label):
        powershell = Path(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")
        if not powershell.is_file():
            self.skipTest("Windows PowerShell 5.1 is required for the worker StrictMode contract")
        worker_text = Path(worker_path).read_text(encoding="utf-8-sig")
        predicates = [line.strip() for line in worker_text.splitlines()
                      if "$qualification.acceptedSeq -ne 20000" in line]
        self.assertEqual(len(predicates), 1, "worker qualification StrictMode predicate must be unique")
        predicate = predicates[0]
        for field in ("status", "guiAcceptanceVerified", "records", "acceptedSeq", "maximumWorkingSetBytes"):
            self.assertIn("$qualification." + field, predicate)
        qualification_path = root / (label + ".json")
        script_path = root / (label + ".ps1")
        qualification_path.write_text(json.dumps(result), encoding="utf-8")
        ps_quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        script_path.write_text(
            "$ErrorActionPreference='Stop'\nSet-StrictMode -Version Latest\n"
            + "$qualification=[IO.File]::ReadAllText(" + ps_quote(qualification_path) + ",[Text.Encoding]::UTF8)|ConvertFrom-Json -ErrorAction Stop\n"
            + predicate + "\nexit 0\n", encoding="utf-8")
        return subprocess.run([str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive",
                               "-ExecutionPolicy", "Bypass", "-File", str(script_path)],
                              capture_output=True, text=True, timeout=30)

    def test_qualified_output_satisfies_worker_strictmode_and_missing_key_is_rejected(self):
        worker_path = os.environ.get("RANGE_PC11_WORKER_SCRIPT")
        if not worker_path:
            self.skipTest("package harness must set RANGE_PC11_WORKER_SCRIPT to the frozen worker")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._qualified_fixture(root)
            result = verify.qualify(root)
            worker_fields = ("status", "guiAcceptanceVerified", "records", "acceptedSeq", "maximumWorkingSetBytes")
            self.assertTrue(all(field in result for field in worker_fields))
            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(result["records"], 20001)
            self.assertEqual(result["acceptedSeq"], 20000)
            self.assertTrue(result["guiAcceptanceVerified"])
            self.assertTrue(result["workingSetInformational"])
            positive = self._run_production_worker_predicate(result, worker_path, root, "worker-positive")
            self.assertEqual(positive.returncode, 0, positive.stdout + positive.stderr)

            missing_key = dict(result)
            del missing_key["acceptedSeq"]
            negative = self._run_production_worker_predicate(missing_key, worker_path, root, "worker-missing-accepted-seq")
            self.assertNotEqual(negative.returncode, 0, "StrictMode unexpectedly accepted a missing acceptedSeq field")
            self.assertIn("acceptedSeq", negative.stdout + negative.stderr)


if __name__ == "__main__":
    unittest.main()
