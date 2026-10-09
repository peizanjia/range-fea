"""WF-11 real native-history, scientific result, and immutable-version acceptance.

Run with explicit --solver, --native, --examples, and frozen --python paths.
An optional --output directory retains the workspace and audit artifacts.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

TESTS = Path(__file__).resolve().parent
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from range_workflow import WorkflowClient
from range_workflow.scientific_reference import compare_results, reference_result
from range_workflow.scientific_contract import canonical_hash, verification_profile
from range_workflow.scientific_extensions import GUI
from range_workflow.service import WorkflowError, file_hash
from range_workflow import scientific_contract as contract
from range_workflow import scientific_extensions as scientific
from test_imported_workflow import _make_catalog
from test_scientific_extensions import manifest as scientific_manifest
from range_workflow.model_sources import register_snapshot_source


EXTENSION_ID = "spectral-test"
ARGS = None


def _candidate_tests() -> dict:
    data = scientific.golden_cases()[0]["input"]
    if len(data["times"]) != 8:
        raise AssertionError("WF-11 candidate fixture must stay at the independent small-N boundary")
    return {"cases": [{"id": "candidate-dc", "input": data,
                       "expected": reference_result(data, small_n=True)}]}


def _example_source() -> str:
    return Path(scientific.__file__).with_name("scientific_example.py").read_text(encoding="utf-8")


def _bad_normalization_source(source: str) -> str:
    return source + '''

# Deliberately corrupt one spectral amplitude so the host oracle rejects v3.
_trusted_process = process
def process(data):
    output = _trusted_process(data)
    if len(output["channels"][0]["amplitude"]) > 1:
        output["channels"][0]["amplitude"][1] *= 1.25
    return output
'''


class ScientificWorkflowIntegration(unittest.TestCase):
    def require_explicit_paths(self):
        if ARGS is None:
            self.fail("Pass --solver --native --examples --python; this integration must not silently skip")
        for name in ("solver", "native", "examples", "python"):
            path = getattr(ARGS, name)
            if path is None or not path.is_absolute():
                self.fail(f"--{name} must be an explicit absolute path")
            if name == "examples":
                if not path.is_dir():
                    self.fail(f"--examples is not a directory: {path}")
            elif not path.is_file():
                self.fail(f"--{name} does not name an existing file: {path}")

    def client(self, workspace: Path, *, trusted: bool) -> WorkflowClient:
        return WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python,
                              timeout=30, trusted_extensions=trusted)

    def preverify_scientific_runtime(self, sdk: WorkflowClient, timeout: int = 180) -> dict:
        ack = sdk.request("extension.runtime.verify", {
            "sdkVersion": scientific.PROTOCOL_VERSION, "force": True})
        self.assertIn(ack["state"], {"queued", "running"})
        self.assertTrue(ack["verificationId"])
        deadline = time.monotonic() + timeout
        while True:
            receipt = sdk.request("extension.runtime.verification.query", {
                "verificationId": ack["verificationId"]})
            if receipt["state"] == "ready":
                self.assertEqual(receipt["sdkVersion"], scientific.PROTOCOL_VERSION)
                self.assertEqual(receipt["profile"], "packaged")
                self.assertEqual(receipt["currentity"], "current")
                self.assertTrue(receipt["isCurrent"])
                self.assertEqual(receipt["currentityScope"], "environment-stamp")
                self.assertTrue(receipt["fullFileVerify"]["verifiedFiles"])
                self.assertEqual(receipt["fullFileVerify"]["runtimeHash"], receipt["runtimeHash"])
                self.assertEqual(receipt["fullFileVerify"]["environmentStamp"],
                                 receipt["expectedEnvironmentStamp"])
                self.assertRegex(receipt["runtimeHash"], r"^[0-9a-f]{64}$")
                return receipt
            if receipt["state"] in {"failed", "cancelled"}:
                self.fail(f"Scientific runtime preverification failed: {receipt.get('error')}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.fail(f"Scientific runtime preverification did not become ready within {timeout} seconds")
            time.sleep(min(0.1, remaining))

    def approved_job(self, sdk, command: str, parameters: dict, *, timeout: int = 240,
                     expected_state: str = "succeeded"):
        preview = sdk.preview(command, parameters)
        approval = sdk.approve(preview)
        accepted = sdk.execute(preview, approval)
        job = sdk.wait(accepted["jobId"], timeout=timeout)
        self.assertEqual(job["state"], expected_state, job.get("error"))
        if expected_state == "succeeded":
            self.assertIsNone(job.get("error"))
        return preview, approval, job

    def _stage(self, sdk, version: str, source: str, *, base_hash: str | None = None) -> dict:
        parameters = {"manifest": scientific_manifest(version), "source": source,
                      "tests": _candidate_tests()}
        if base_hash is not None:
            parameters["baseBundleHash"] = base_hash
        _preview, _approval, job = self.approved_job(sdk, "extension.stage", parameters)
        return job["result"]

    def _validate(self, sdk, staged: dict, *, expected_state: str = "succeeded"):
        preview, approval, job = self.approved_job(sdk, "extension.validate", {
            "candidateId": staged["candidateId"], "bundleHash": staged["bundleHash"]},
            timeout=180, expected_state=expected_state)
        return preview, approval, job

    def _activate(self, sdk, validated: dict) -> dict:
        _preview, _approval, job = self.approved_job(sdk, "extension.activate", {
            "candidateId": validated["candidateId"], "bundleHash": validated["bundleHash"]})
        return job["result"]

    def _assert_workspace_file(self, workspace: Path, path_value: str,
                                expected_sha256: str | None = None) -> Path:
        path = Path(path_value)
        if not path.is_absolute():
            path = workspace / path
        resolved = path.resolve(strict=True)
        self.assertTrue(resolved.is_relative_to(workspace.resolve()),
                        f"artifact escaped its workflow workspace: {path_value}")
        self.assertFalse(path.is_symlink(), f"artifact path is a symbolic link: {path_value}")
        self.assertTrue(path.is_file(), f"artifact is not a file: {path_value}")
        if expected_sha256 is not None:
            self.assertEqual(file_hash(path), expected_sha256, f"artifact hash differs: {path_value}")
        return path

    def _assert_job_artifacts(self, sdk, workspace: Path, job: dict) -> None:
        refreshed = sdk.request("jobs.query", {"jobId": job["jobId"]})["job"]
        artifacts = refreshed.get("artifacts")
        self.assertIsInstance(artifacts, list)
        self.assertGreater(len(artifacts), 0, f"job has no artifact ledger: {job['jobId']}")
        for artifact in artifacts:
            self._assert_workspace_file(workspace, artifact["path"], artifact["sha256"])

    def _read_native_series(self, sdk, job_id: str, selector: dict, *, limit: int = 7) -> list[dict]:
        catalog = sdk.native_result(job_id, "catalog", mode=selector["mode"], limit=1)
        self.assertEqual(catalog["artifactId"], selector["artifactId"])
        samples, cursor = [], None
        pages = 0
        while True:
            page = sdk.native_result(job_id, "series", mode=selector["mode"],
                artifactId=selector["artifactId"], channelId=selector["channelId"], limit=limit,
                **({"cursor": cursor} if cursor else {}))
            pages += 1
            self.assertEqual(page["channelId"], selector["channelId"])
            samples.extend(page["samples"])
            next_cursor = page.get("nextCursor")
            if not next_cursor:
                break
            self.assertNotEqual(next_cursor, cursor, "native history cursor did not advance")
            cursor = next_cursor
        self.assertGreater(pages, 1, "raw native history was not exercised through pagination")
        self.assertEqual(len(samples), catalog["frameCount"])
        return samples

    def _query_scientific_view(self, sdk, job_id: str, channel: int, view: str,
                               expected_count: int, *, limit: int = 7):
        points, first, offset = [], None, 0
        while True:
            page = sdk.scientific_result(job_id, channel=channel, offset=offset,
                                         limit=limit, view=view)
            if first is None:
                first = page
            self.assertEqual(page["view"], view)
            self.assertEqual(page["offset"], offset)
            self.assertEqual(page["pointCount"], expected_count)
            self.assertEqual(len(page["points"]), min(limit, expected_count - offset))
            points.extend(page["points"])
            next_offset = page["nextOffset"]
            if next_offset is None:
                break
            self.assertGreater(next_offset, offset, "scientific result offset did not advance")
            offset = next_offset
        self.assertEqual(len(points), expected_count)
        self.assertEqual([point["rawOrdinal"] for point in points], list(range(expected_count)))
        return first, points

    def _assert_version_source(self, sdk, workspace: Path, bundle_hash: str, source: str,
                               version_name: str, expected_active: str) -> dict:
        record = sdk.request("extension.version.get", {"extensionId": EXTENSION_ID,
                                                        "bundleHash": bundle_hash})["version"]
        self.assertEqual(record["version"], version_name)
        self.assertEqual(record["state"], expected_active)
        self.assertEqual(record["source"], source)
        self.assertEqual(record["manifest"]["gui"], GUI)
        self.assertEqual(record["manifest"]["inputSchema"], contract.INPUT_SCHEMA)
        self.assertEqual(record["manifest"]["outputSchema"], contract.OUTPUT_SCHEMA)
        self.assertEqual(record["hashes"]["source"], hashlib.sha256(source.encode("utf-8")).hexdigest())
        self.assertTrue(record["validation"]["passed"])
        self.assertEqual(record["validation"]["independentCaseCount"], 4)
        self.assertEqual(record["report"]["contractHash"], contract.CONTRACT_HASH)
        self.assertEqual(record["report"]["independentCaseCount"], 4)
        self.assertEqual(record["report"]["runtimeHash"], record["runtimeHash"])
        reported_profile = record["report"]["cases"][0]["verificationProfile"]
        expected_profile = verification_profile()
        self.assertEqual({key: reported_profile[key] for key in expected_profile}, expected_profile)
        self.assertEqual(set(reported_profile), set(expected_profile) | {"maximumAbsoluteError"})
        self.assertTrue(math.isfinite(reported_profile["maximumAbsoluteError"]))
        self.assertGreaterEqual(reported_profile["maximumAbsoluteError"], 0.)
        self._assert_workspace_file(workspace, record["validation"]["reportPath"],
                                    record["validation"]["reportSha256"])
        return record

    def test_real_native_scientific_lifecycle_result_queries_and_rollback(self):
        self.require_explicit_paths()
        if ARGS.output is not None:
            output_root = ARGS.output.resolve()
            output_root.mkdir(parents=True, exist_ok=True)
            base = Path(tempfile.mkdtemp(prefix="range-wf11-", dir=output_root))
            cleanup = None
        else:
            cleanup = tempfile.TemporaryDirectory(prefix="range-wf11-scientific-")
            base = Path(cleanup.name)

        try:
            workspace = base / "workflow-workspace"
            catalog_root = base / "gui-project"
            snapshot = base / "stage9-truss4-20.rbm"
            prepared = subprocess.run([
                str(ARGS.native), "prepare", "--input", str(ARGS.examples / "stage9_truss4.ram"),
                "--output", str(snapshot), "--steps", "20", "--output-frequency", "1", "--dt", ".005"],
                capture_output=True, check=False, timeout=60)
            self.assertEqual(prepared.returncode, 0, prepared.stderr.decode("utf-8", errors="replace"))
            self.assertTrue(snapshot.is_file())

            snapshot_sha = file_hash(snapshot)
            _document_id, snapshot_ref = _make_catalog(catalog_root, snapshot, snapshot_sha)
            registered_source = register_snapshot_source(workspace, catalog_root, snapshot_ref, ARGS.native)
            self.assertEqual(registered_source["registeredModelSha256"], snapshot_sha)

            correct_source = _example_source()
            self.assertEqual(contract.CONTRACT_HASH,
                             "0708308690808434e9552c2f96e718af3c28fa15ce63fae2f6b04daf19f0565f")
            self.assertIn(contract.CONTRACT_HASH, correct_source)
            manifest_v1 = scientific_manifest("1.0.0")
            staged_v1 = None
            disabled_validation_preview = None
            disabled_validation_approval = None

            # Ordinary mode can retain source text, but its approved validation
            # command is rejected before any candidate Python is executed.
            with self.client(workspace, trusted=False) as disabled:
                self.preverify_scientific_runtime(disabled)
                capabilities = disabled.request("capabilities.query")
                self.assertFalse(capabilities["extensionRuntime"]["trustedExtensions"])
                self.assertEqual(capabilities["extensionRuntime"]["trustMode"], "disabled")
                self.assertFalse(capabilities["extensionRuntime"]["scientificV2"]["ordinaryCodeExecution"])
                self.assertTrue(capabilities["extensionRuntime"]["scientificV2"]["trustedHostOracleRequired"])
                self.assertIn("range.extension/v2", capabilities["extensionRuntime"]["supportedProtocols"])

                _stage_preview, _stage_approval, stage_job = self.approved_job(disabled, "extension.stage", {
                    "manifest": manifest_v1, "source": correct_source, "tests": _candidate_tests()})
                staged_v1 = stage_job["result"]
                self.assertEqual(staged_v1["state"], "staged")
                self.assertEqual(staged_v1["hashes"]["source"],
                                 hashlib.sha256(correct_source.encode("utf-8")).hexdigest())

                disabled_validation_preview = disabled.preview("extension.validate", {
                    "candidateId": staged_v1["candidateId"], "bundleHash": staged_v1["bundleHash"]})
                disabled_validation_approval = disabled.approve(disabled_validation_preview)
                with self.assertRaises(WorkflowError) as denied:
                    disabled.execute(disabled_validation_preview, disabled_validation_approval)
                self.assertEqual(denied.exception.code, "TRUSTED_RUNTIME_REQUIRED")

            # Reopen the same workspace explicitly in trusted developer mode.
            # The old approved preview is session-bound and cannot be replayed.
            with self.client(workspace, trusted=True) as sdk:
                runtime_receipt = self.preverify_scientific_runtime(sdk)
                capabilities = sdk.request("capabilities.query")
                self.assertTrue(capabilities["extensionRuntime"]["trustedExtensions"])
                self.assertEqual(capabilities["extensionRuntime"]["trustMode"], "trusted-developer")
                with self.assertRaises(WorkflowError) as stale:
                    sdk.execute(disabled_validation_preview, disabled_validation_approval)
                self.assertEqual(stale.exception.code, "STALE_REVISION")

                candidate = sdk.request("extension.candidate.get", {
                    "candidateId": staged_v1["candidateId"]})["candidate"]
                self.assertEqual(candidate["state"], "staged")
                self.assertEqual(candidate["source"], correct_source)
                self.assertEqual(candidate["manifest"], manifest_v1)
                self.assertEqual(candidate["tests"]["cases"][0]["expected"],
                                 reference_result(scientific.golden_cases()[0]["input"], small_n=True))

                validation_preview, _validation_approval, validation_job = self._validate(sdk, staged_v1)
                runtime = validation_preview["normalized"]["parameters"]["runtime"]
                self.assertEqual(runtime["runtimeHash"], runtime_receipt["runtimeHash"])
                self.assertEqual(runtime["profile"], "packaged")
                self.assertEqual(runtime["identity"]["runtimeId"],
                                 "cpython-3.13.5-numpy-2.2.6-scipy-1.15.3-win-amd64")
                self.assertEqual(runtime["identity"]["dependencies"], [
                    {"name": "numpy", "version": "2.2.6"},
                    {"name": "scipy", "version": "1.15.3"}])
                self.assertEqual(validation_job["result"]["validation"], {
                    "passed": True, "caseCount": 1, "independentCaseCount": 4})
                activated_v1 = self._activate(sdk, validation_job["result"])
                bundle_v1 = activated_v1["bundleHash"]
                self.assertEqual(bundle_v1, staged_v1["bundleHash"])

                source_registry = sdk.request("model.source.query", {
                    "sourceId": registered_source["sourceId"]})
                self.assertEqual(source_registry["registeredModelSha256"], snapshot_sha)
                self.assertEqual(source_registry["manifestSha256"], registered_source["manifestSha256"])

                _configure_preview, _configure_approval, configured = self.approved_job(sdk,
                    "workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .05,
                                            "sourceId": registered_source["sourceId"]})
                self.assertEqual(configured["state"], "succeeded")
                native_preview, native_approval, native_job = self.approved_job(sdk, "workflow.run", {
                    "mode": "both", "independentReference": True})
                native_job_id = native_job["jobId"]
                self.assertEqual(native_job["result"]["configuration"]["dt"], .005)
                self.assertEqual(native_job["result"]["configuration"]["steps"], 20)
                self.assertEqual(native_job["result"]["configuration"]["targetDamping"], .05)
                self.assertTrue(native_job["result"]["independentReference"])
                self._assert_job_artifacts(sdk, workspace, native_job)

                native_catalog = sdk.native_result(native_job_id, "catalog", mode="active", limit=1)
                self.assertEqual(native_catalog["sourceKind"], "native-active-fe")
                self.assertEqual(native_catalog["sourceSnapshotRef"]["modelSha256"], snapshot_sha)
                self.assertEqual(native_catalog["frameCount"], 20)
                self.assertEqual(native_catalog["proofScope"], "solver-and-declared-controller-runtime/v1")
                self._assert_workspace_file(workspace, native_job["result"]["referencePath"])
                self._assert_workspace_file(workspace, native_job["result"]["sourceReferencePath"])
                for artifact in native_job["result"]["sourceArtifacts"].values():
                    self._assert_workspace_file(workspace, artifact["path"], artifact["sha256"])
                for descriptor in native_job["result"]["nativeResults"].values():
                    self._assert_workspace_file(workspace, descriptor["manifestPath"],
                                                descriptor["manifestSha256"])

                port = registered_source["descriptor"]["ports"]["velocity"]
                axis = "XYZ"[next(i for i, value in enumerate(port["globalDirection"]) if value)]
                source_catalog = sdk.series_sources(native_job_id, mode="active")
                self.assertEqual(source_catalog["artifactId"], native_catalog["artifactId"])
                selections = []
                for quantity, unit in (("displacement", "m"), ("velocity", "m/s")):
                    channel_id = f"node:{port['nodePhysicalId']}:{quantity}:{axis}"
                    entry = next(item for item in source_catalog["channels"]
                                 if item["selector"]["channelId"] == channel_id)
                    self.assertEqual(entry["unit"], unit)
                    self.assertEqual(entry["selector"]["mode"], "active")
                    selections.append(entry)
                self.assertEqual([item["unit"] for item in selections], ["m", "m/s"])

                raw_by_channel = [self._read_native_series(sdk, native_job_id, item["selector"])
                                  for item in selections]
                original_times = [sample["timeS"] for sample in raw_by_channel[0]]
                self.assertTrue(original_times)
                for samples in raw_by_channel:
                    self.assertEqual([sample["timeS"] for sample in samples], original_times)
                    self.assertGreater(max(abs(sample["value"]) for sample in samples), 0.0)

                filter_options = {"window": "hann", "removeMean": True,
                    "filter": {"kind": "butterworth-lowpass", "order": 1, "cutoffHz": 10.0,
                               "phase": "causal", "initialCondition": "zero"}}
                scientific_preview, scientific_approval, scientific_job = self.approved_job(sdk,
                    "extension.run", {"extensionId": EXTENSION_ID, "bundleHash": bundle_v1,
                                      "sources": [item["selector"] for item in selections],
                                      "options": filter_options})
                scientific_job_id = scientific_job["jobId"]
                run_parameters = scientific_preview["normalized"]["parameters"]
                reference_input = copy.deepcopy(run_parameters["series"])
                self.assertEqual(reference_input["times"], original_times)
                self.assertEqual(reference_input["window"], "hann")
                self.assertTrue(reference_input["removeMean"])
                self.assertEqual(reference_input["filter"], filter_options["filter"])
                self.assertEqual(len(reference_input["channels"]), 2)
                for index, channel in enumerate(reference_input["channels"]):
                    self.assertEqual(channel["values"], [sample["value"] for sample in raw_by_channel[index]])
                    self.assertEqual(channel["source"]["channelId"], selections[index]["selector"]["channelId"])
                    self.assertEqual(channel["source"]["artifactId"], native_catalog["artifactId"])

                # N <= 128 uses the host-owned stdlib DFT and filter recurrence.
                expected_output = reference_result(reference_input, small_n=True)
                result_descriptor = scientific_job["result"]["scientificResult"]
                result_path = self._assert_workspace_file(workspace, result_descriptor["path"],
                                                          result_descriptor["sha256"])
                actual_output = json.loads(result_path.read_text(encoding="utf-8"))
                compare_results(actual_output, expected_output, reference_input)
                self.assertEqual(actual_output["times"], original_times)
                self.assertEqual(actual_output["inputHash"], canonical_hash(reference_input))
                self.assertEqual(scientific_job["result"]["seriesSha256"], canonical_hash(reference_input))
                self.assertEqual(scientific_job["result"]["bundleHash"], bundle_v1)
                self.assertEqual(scientific_job["result"]["version"], "1.0.0")
                self.assertEqual(scientific_job["result"]["runtimeHash"], runtime["runtimeHash"])
                self._assert_job_artifacts(sdk, workspace, scientific_job)
                provenance_path = workspace / "jobs" / scientific_job_id / "input-provenance.json"
                self._assert_workspace_file(workspace, str(provenance_path),
                                            scientific_job["result"]["inputProvenanceSha256"])

                for channel_index, (expected_channel, actual_channel) in enumerate(
                        zip(expected_output["channels"], actual_output["channels"])):
                    for view in ("filtered-time", "amplitude", "psd", "real", "imag", "phase"):
                        count = len(expected_output["times"] if view == "filtered-time"
                                    else expected_output["frequenciesHz"])
                        header, points = self._query_scientific_view(sdk, scientific_job_id,
                            channel_index, view, count)
                        self.assertEqual(header["sourceSha256"], canonical_hash(reference_input))
                        self.assertEqual(header["resultSha256"], result_descriptor["sha256"])
                        self.assertEqual(header["sourceHash"], result_descriptor["sha256"])
                        self.assertEqual(header["runtimeHash"], runtime["runtimeHash"])
                        self.assertEqual(header["channel"]["unit"], expected_channel["unit"])
                        self.assertEqual(header["sampleCount"], len(expected_output["times"]))
                        self.assertEqual(header["binCount"], len(expected_output["frequenciesHz"]))
                        self.assertEqual(header["verificationProfile"]["id"],
                                         verification_profile()["id"])
                        self.assertEqual(header["verificationProfile"]["absoluteTolerance"],
                                         verification_profile()["absoluteTolerance"])
                        if view == "filtered-time":
                            self.assertEqual((header["domain"], header["xUnit"], header["yUnit"]),
                                             ("time", "s", expected_channel["unit"]))
                            self.assertEqual([point["x"] for point in points], actual_output["times"])
                            self.assertEqual([point["y"] for point in points], actual_channel["filteredValues"])
                        else:
                            self.assertEqual((header["domain"], header["xUnit"]), ("frequency", "Hz"))
                            self.assertEqual([point["x"] for point in points], actual_output["frequenciesHz"])
                        if view == "amplitude":
                            self.assertEqual(header["yUnit"], expected_channel["unit"])
                            self.assertEqual([point["y"] for point in points], actual_channel["amplitude"])
                        elif view == "psd":
                            self.assertEqual(header["yUnit"], expected_channel["unit"] + "^2/Hz")
                            self.assertEqual(header["psdUnit"], expected_channel["psdUnit"])
                            self.assertEqual([point["y"] for point in points], actual_channel["psd"])
                        elif view == "real":
                            self.assertEqual(header["yUnit"], expected_channel["unit"])
                            self.assertEqual([point["y"] for point in points],
                                             [pair[0] for pair in actual_channel["fft"]])
                        elif view == "imag":
                            self.assertEqual(header["yUnit"], expected_channel["unit"])
                            self.assertEqual([point["y"] for point in points],
                                             [pair[1] for pair in actual_channel["fft"]])
                        elif view == "phase":
                            self.assertEqual(header["yUnit"], "rad")
                            self.assertEqual([point["y"] for point in points], [
                                math.atan2(pair[1], pair[0]) for pair in actual_channel["fft"]])
                        self.assertEqual(header["inputOptions"], filter_options)
                        self.assertEqual(len(header["inputSources"]), 2)
                        for source_index, (source_metadata, selection) in enumerate(
                                zip(header["inputSources"], selections)):
                            self.assertEqual(source_metadata["selector"]["channelId"],
                                             selection["selector"]["channelId"])
                            self.assertEqual(source_metadata["selector"]["sourceHash"],
                                             reference_input["channels"][source_index]["source"]["sourceHash"])
                            self.assertEqual(source_metadata["channel"]["unit"], selection["unit"])
                            self.assertTrue(source_metadata["proofScope"])

                # Reopen the workspace before version changes. Persisted job and
                # source records must still be queryable, and old approvals stay stale.
            with self.client(workspace, trusted=True) as sdk:
                reopened_runtime_receipt = self.preverify_scientific_runtime(sdk)
                self.assertEqual(reopened_runtime_receipt["runtimeHash"], runtime["runtimeHash"])
                sources = sdk.request("model.sources.query", {"offset": 0, "limit": 20})["sources"]
                self.assertIn(registered_source["sourceId"], [item["sourceId"] for item in sources])
                reopened_source = sdk.request("model.source.query", {
                    "sourceId": registered_source["sourceId"]})
                self.assertEqual(reopened_source["registeredModelSha256"], snapshot_sha)
                registry = sdk.request("extension.list", {"extensionId": EXTENSION_ID})["extensions"][0]
                self.assertEqual(registry["activeBundleHash"], bundle_v1)
                self.assertEqual(registry["activeVersion"], "1.0.0")
                v1_record = self._assert_version_source(sdk, workspace, bundle_v1,
                                                        correct_source, "1.0.0", "active")
                old_scientific_job = sdk.request("jobs.query", {"jobId": scientific_job_id})["job"]
                self.assertEqual(old_scientific_job["result"]["bundleHash"], bundle_v1)
                self.assertEqual(old_scientific_job["result"]["runtimeHash"], runtime["runtimeHash"])
                self.assertEqual(file_hash(result_path), result_descriptor["sha256"])
                with self.assertRaises(WorkflowError) as stale:
                    sdk.execute(native_preview, native_approval)
                self.assertEqual(stale.exception.code, "STALE_REVISION")

                version2_source = correct_source + "\n# retained immutable source for version 2.0.0\n"
                staged_v2 = self._stage(sdk, "2.0.0", version2_source, base_hash=bundle_v1)
                self.assertNotEqual(staged_v2["bundleHash"], bundle_v1)
                validated_v2_preview, _approval_v2, validated_v2_job = self._validate(sdk, staged_v2)
                self.assertEqual(validated_v2_job["result"]["validation"]["independentCaseCount"], 4)
                self.assertEqual(validated_v2_preview["normalized"]["parameters"]["runtime"]["runtimeHash"],
                                 runtime["runtimeHash"])
                self._activate(sdk, validated_v2_job["result"])
                registry = sdk.request("extension.list", {"extensionId": EXTENSION_ID})["extensions"][0]
                self.assertEqual(registry["activeBundleHash"], staged_v2["bundleHash"])
                self.assertEqual(registry["activeVersion"], "2.0.0")
                v2_record = self._assert_version_source(sdk, workspace, staged_v2["bundleHash"],
                                                        version2_source, "2.0.0", "active")

                # The old successful job remains byte-for-byte pinned to v1.
                old_after_v2 = sdk.request("jobs.query", {"jobId": scientific_job_id})["job"]
                self.assertEqual(old_after_v2["result"]["bundleHash"], bundle_v1)
                self.assertEqual(old_after_v2["result"]["version"], "1.0.0")
                self.assertEqual(old_after_v2["result"]["scientificResult"], result_descriptor)
                self.assertEqual(file_hash(result_path), result_descriptor["sha256"])
                self.assertEqual(sdk.scientific_result(scientific_job_id, channel=0, view="amplitude",
                                                       limit=100)["resultSha256"], result_descriptor["sha256"])
                self._assert_version_source(sdk, workspace, bundle_v1, correct_source, "1.0.0", "retained")

                staged_v3 = self._stage(sdk, "3.0.0", _bad_normalization_source(correct_source),
                                        base_hash=staged_v2["bundleHash"])
                _bad_preview, _bad_approval, failed_v3 = self._validate(sdk, staged_v3,
                                                                        expected_state="failed")
                self.assertTrue(failed_v3.get("error"), "wrong normalization was not rejected by validation")
                bad_candidate = sdk.request("extension.candidate.get", {
                    "candidateId": staged_v3["candidateId"]})["candidate"]
                self.assertEqual(bad_candidate["state"], "staged")
                registry = sdk.request("extension.list", {"extensionId": EXTENSION_ID})["extensions"][0]
                self.assertEqual(registry["activeBundleHash"], staged_v2["bundleHash"])
                self.assertEqual(registry["activeVersion"], "2.0.0")

                _rollback_preview, _rollback_approval, rolled_back = self.approved_job(sdk,
                    "extension.rollback", {"extensionId": EXTENSION_ID,
                        "expectedActiveBundleHash": staged_v2["bundleHash"],
                        "targetBundleHash": bundle_v1})
                self.assertEqual(rolled_back["result"]["activeBundleHash"], bundle_v1)
                registry = sdk.request("extension.list", {"extensionId": EXTENSION_ID})["extensions"][0]
                self.assertEqual(registry["activeBundleHash"], bundle_v1)
                self.assertEqual(registry["activeVersion"], "1.0.0")
                self._assert_version_source(sdk, workspace, bundle_v1, correct_source, "1.0.0", "active")
                self._assert_version_source(sdk, workspace, staged_v2["bundleHash"],
                                            version2_source, "2.0.0", "retained")

                _final_preview, _final_approval, final_job = self.approved_job(sdk, "extension.run", {
                    "extensionId": EXTENSION_ID, "bundleHash": bundle_v1,
                    "sources": [item["selector"] for item in selections], "options": filter_options})
                self.assertEqual(final_job["result"]["bundleHash"], bundle_v1)
                self.assertEqual(final_job["result"]["version"], "1.0.0")
                self.assertEqual(final_job["result"]["runtimeHash"], runtime["runtimeHash"])
                self._assert_job_artifacts(sdk, workspace, final_job)
                final_result = final_job["result"]["scientificResult"]
                self._assert_workspace_file(workspace, final_result["path"], final_result["sha256"])
                self.assertEqual(sdk.scientific_result(final_job["jobId"], view="filtered-time",
                                                       limit=100)["resultSha256"], final_result["sha256"])
                self.assertEqual(file_hash(result_path), result_descriptor["sha256"])
        finally:
            if cleanup is not None:
                cleanup.cleanup()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--examples", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True,
                        help="Frozen private Python runtime with verified NumPy/SciPy archives")
    parser.add_argument("--output", type=Path,
                        help="Optional directory that retains the generated workspace and audit files")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
