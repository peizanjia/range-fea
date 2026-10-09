"""Server-side WF10 adapter checks using a fake verified native broker."""
from __future__ import annotations

import copy
import math
from pathlib import Path
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import series_service  # noqa: E402
from range_workflow.service import WorkflowError  # noqa: E402


class FakeVerifiedBroker:
    def __init__(self, frame_count=8, *, mode="active", source_kind=None,
                 model_sha=None, unit="m", snapshot_ref=None):
        self.frame_count = frame_count
        self.mode = mode
        self.source_kind = source_kind or f"native-{mode}-fe"
        self.model_sha = model_sha or "a" * 64
        self.unit = unit
        self.snapshot_ref = snapshot_ref or {
            "kind": "gui-native-input-snapshot/v1",
            "sourceRuntimeStamp": {"sourceRevision": "live"},
            "inputDerivedFrom": "live_unlinked",
            "inputSnapshotSha256": self.model_sha,
            "inputPackageSha256": "b" * 64,
        }
        self.manifest_sha = "c" * 64
        self.catalog_calls = 0
        self.calls = []
        self.page_mutator = None
        self.cursor_mode = "normal"
        self.final_catalog_mutator = None

    def __call__(self, service, operation, parameters):
        self.calls.append((operation, copy.deepcopy(parameters)))
        if operation == "catalog":
            self.catalog_calls += 1
            catalog = {
                "jobId": parameters["jobId"], "mode": self.mode,
                "artifactId": parameters["artifactId"],
                "sourceKind": self.source_kind,
                "manifestSha256": self.manifest_sha,
                "sourceSnapshotRef": copy.deepcopy(self.snapshot_ref),
                "analysis": {"domain": "structural-fe", "type": "structural-dynamics"},
                "sampleScope": "all-persisted-numbered-model-records",
                "frameCount": self.frame_count,
                "validationScope": "trusted-job-manager-complete-journal-frames",
                "proofScope": "verified-native-result-manifest/v1",
                "sourceRuntimeStamp": {"runtime": "fake-verified-broker"},
                "inputSnapshotSha256": self.model_sha,
                "inputPackageSha256": "b" * 64,
                "runtime": {"fingerprintSha256": "d" * 64, "scope": "test"},
                "control": {"enabled": self.mode == "active"},
            }
            if self.final_catalog_mutator and self.catalog_calls > 1:
                catalog = self.final_catalog_mutator(catalog)
            return catalog

        self.assert_page_query(operation, parameters)
        offset = self.cursor_offset(parameters.get("cursor"))
        end = min(self.frame_count, offset + parameters["limit"])
        samples = []
        for index in range(offset, end):
            value = 2.0 * math.cos(2.0 * math.pi * 5 * index / self.frame_count)
            proof = {
                "recordNo": index + 1,
                "originalFrameSha256": "e" * 64,
                "originKind": "reused-history",
                "originManifestPath": "D:/private/job/native.manifest.json",
                "relativePath": f"frames/{index + 1}.rmod",
                "origin": {"jobId": parameters["jobId"], "artifactId": parameters["artifactId"]},
            }
            samples.append({
                "recordNo": index + 1,
                "timeS": index / self.frame_count,
                "value": value,
                "originKind": "reused-history",
                "origin": {"jobId": parameters["jobId"], "recordNo": index + 1},
                "historyProof": proof,
            })
        next_cursor = None
        if end < self.frame_count:
            next_cursor = "cursor-200" if self.cursor_mode == "repeat" else f"cursor-{end}"
        page = {
            "valid": True,
            "operation": "series",
            "artifactId": parameters["artifactId"],
            "channelId": parameters["channelId"],
            "nodePhysicalId": 51000,
            "variableId": ("var-force" if parameters["channelId"].endswith(":force:Y")
                           else "var-displacement"),
            "axis": "Y" if parameters["channelId"].endswith(":force:Y") else "X",
            "unit": "N" if parameters["channelId"].endswith(":force:Y") else self.unit,
            "samples": samples,
            "offset": offset,
            "limit": parameters["limit"],
            "jobId": parameters["jobId"],
            "mode": self.mode,
            "completed": True,
            "manifestSha256": self.manifest_sha,
            "sourceKind": self.source_kind,
            "sourceSnapshotRef": copy.deepcopy(self.snapshot_ref),
        }
        if next_cursor is not None:
            page["nextCursor"] = next_cursor
        if self.page_mutator:
            page = self.page_mutator(page, offset)
        return page

    @staticmethod
    def assert_page_query(operation, parameters):
        assert operation == "series"
        assert parameters["limit"] == 200
        assert set(parameters) <= {"jobId", "mode", "artifactId", "channelId", "limit", "cursor"}

    @staticmethod
    def cursor_offset(cursor):
        if cursor is None:
            return 0
        return int(cursor.removeprefix("cursor-"))


class SeriesServiceTest(unittest.TestCase):
    def setUp(self):
        self.service = object()
        self.source = {"jobId": "job-1", "mode": "active", "artifactId": "artifact-1",
                       "channelId": "node:51000:displacement:X"}

    def invoke(self, broker, operation, sources=None, options=None):
        parameters = {"operation": operation,
                      "sources": [self.source] if sources is None else sources,
                      "options": {} if options is None else options}
        with mock.patch.object(series_service, "query_registered", side_effect=broker):
            return series_service.handle(self.service, parameters)

    def test_envelope_preserves_sample_identity_and_proof_but_removes_paths(self):
        broker = FakeVerifiedBroker()
        result = self.invoke(broker, "envelope", options={"limit": 8})
        sample = result["samples"][0]
        self.assertEqual(sample["recordNo"], 1)
        self.assertEqual(sample["time"], 0.0)
        self.assertEqual(sample["origin"]["originKind"], "reused-history")
        self.assertEqual(sample["origin"]["historyProof"]["recordNo"], 1)
        self.assertEqual(sample["origin"]["historyProof"]["originalFrameSha256"], "e" * 64)
        self.assertEqual(sample["origin"]["historyProof"]["origin"]["artifactId"], "artifact-1")

        def assert_no_path_fields(value):
            if isinstance(value, dict):
                for key, item in value.items():
                    self.assertNotRegex(key, re.compile("path", re.IGNORECASE))
                    assert_no_path_fields(item)
            elif isinstance(value, list):
                for item in value:
                    assert_no_path_fields(item)

        assert_no_path_fields(result)
        self.assertNotIn("D:/private", repr(result))
        self.assertNotIn("frames/1.rmod", repr(result))
        self.assertEqual(sum(operation == "catalog" for operation, _ in broker.calls), 2)

    def test_spectrum_is_deterministic_and_uses_registered_model_provenance(self):
        broker = FakeVerifiedBroker(frame_count=64)
        first = self.invoke(broker, "spectrum", options={"window": "rectangular", "removeMean": False})
        repeated = self.invoke(FakeVerifiedBroker(frame_count=64), "spectrum",
                               options={"window": "rectangular", "removeMean": False})
        self.assertEqual(first, repeated)
        self.assertEqual(first["provenance"]["source"]["modelSha256"], "a" * 64)
        self.assertAlmostEqual(first["amplitude"][5], 2.0)
        self.assertEqual(first["provenance"]["source"]["sourceKind"], "native-active-fe")

    def test_compare_and_phase_use_native_sources_and_keep_distinct_units(self):
        left = self.source
        right = {"jobId": "job-2", "mode": "passive", "artifactId": "artifact-2",
                 "channelId": "node:51000:displacement:X"}
        active = FakeVerifiedBroker(mode="active")
        passive = FakeVerifiedBroker(mode="passive", unit="N")
        # Give both source modes the same frozen model identity for comparison.
        passive.model_sha = active.model_sha
        passive.snapshot_ref["inputSnapshotSha256"] = active.model_sha
        passive.unit = "m"

        with mock.patch.object(series_service, "query_registered", side_effect=lambda _svc, op, p:
                               active(_svc, op, p) if p["mode"] == "active" else passive(_svc, op, p)):
            compared = series_service.handle(self.service, {"operation": "compare", "sources": [left, right],
                                                            "options": {}})
        self.assertEqual([sample["value"] for sample in compared["samples"]], [0.0] * 8)

        phase_source = {"jobId": "job-2", "mode": "passive", "artifactId": "artifact-2",
                        "channelId": "node:51000:force:Y"}
        phase_active = FakeVerifiedBroker(mode="active")
        phase_passive = FakeVerifiedBroker(mode="passive")
        phase_passive.model_sha = phase_active.model_sha
        phase_passive.snapshot_ref["inputSnapshotSha256"] = phase_active.model_sha
        with mock.patch.object(series_service, "query_registered", side_effect=lambda _svc, op, p:
                               phase_active(_svc, op, p) if p["mode"] == "active" else phase_passive(_svc, op, p)):
            result = series_service.handle(self.service, {"operation": "phase",
                                                           "sources": [left, phase_source],
                                                           "options": {}})
        self.assertEqual(result["units"], {"time": "s", "x": "m", "y": "N"})
        self.assertEqual(result["samples"][0]["xRecordNo"], 1)

    def test_tampering_between_pages_is_rejected(self):
        broker = FakeVerifiedBroker(frame_count=201)

        def tamper(page, offset):
            if offset:
                page["manifestSha256"] = "f" * 64
            return page

        broker.page_mutator = tamper
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(broker, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "SOURCE_CHANGED")

    def test_final_catalog_revalidation_detects_source_change(self):
        broker = FakeVerifiedBroker()

        def tamper(catalog):
            catalog["sourceSnapshotRef"]["inputSnapshotSha256"] = "f" * 64
            return catalog

        broker.final_catalog_mutator = tamper
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(broker, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "SOURCE_CHANGED")

    def test_repeated_cursor_changed_unit_and_unsupported_source_fail_closed(self):
        repeated = FakeVerifiedBroker(frame_count=402)
        repeated.cursor_mode = "repeat"
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(repeated, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "INVALID_RESPONSE")

        changed_unit = FakeVerifiedBroker(frame_count=201)
        changed_unit.page_mutator = lambda page, offset: ({**page, "unit": "cm"}
                                                          if offset else page)
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(changed_unit, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "SOURCE_CHANGED")

        unsupported = FakeVerifiedBroker(source_kind="native-fe")
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(unsupported, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "UNSUPPORTED_SOURCE")

    def test_strict_selectors_and_linked_model_hash_are_enforced(self):
        broker = FakeVerifiedBroker()
        bad = {**self.source, "samples": []}
        with mock.patch.object(series_service, "query_registered", side_effect=broker) as query:
            with self.assertRaises(WorkflowError) as caught:
                series_service.handle(self.service, {"operation": "envelope", "sources": [bad],
                                                     "options": {"limit": 8}})
        self.assertEqual(caught.exception.code, "INVALID_PARAMETERS")
        query.assert_not_called()

        linked_ref = {
            "projectId": "00000000-0000-4000-8000-000000000001",
            "persistentDocumentId": "00000000-0000-4000-8000-000000000002",
            "sourceRevision": "r7",
            "modelSha256": "9" * 64,
            "instanceRef": {
                "documentId": "00000000-0000-4000-8000-000000000003",
                "sessionEpoch": "00000000-0000-4000-8000-000000000004",
                "documentRevision": 3,
            },
        }
        linked = FakeVerifiedBroker(snapshot_ref=linked_ref)
        linked_result = self.invoke(linked, "envelope", options={"limit": 8})
        self.assertEqual(linked_result["provenance"]["modelSha256"], "9" * 64)

        missing_hash_ref = {"sourceRevision": "r7", "inputDerivedFrom": "restart"}
        missing_hash = FakeVerifiedBroker(snapshot_ref=missing_hash_ref)
        with self.assertRaises(WorkflowError) as caught:
            self.invoke(missing_hash, "envelope", options={"limit": 8})
        self.assertEqual(caught.exception.code, "SOURCE_CHANGED")


if __name__ == "__main__":
    unittest.main()
