"""Storage-format and recovery-origin checks without invoking a native solver."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.native_manifest import (decode_native_manifest, load_native_manifest,
                                           origin_table_sha256, ORIGIN_ENCODING)
from range_workflow.service import WorkflowError, canonical


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def compact_manifest(origins, frames, history=()):
    table = {sha(raw): raw.decode("utf-8") for raw in origins}
    return {"apiVersion": "range.native-results/v1", "frames": frames,
            "reusedHistory": list(history), "originEncoding": ORIGIN_ENCODING,
            "originTable": table, "originTableSha256": origin_table_sha256(table)}


def read(value):
    raw = canonical(value)
    return decode_native_manifest(raw, sha(raw))


class NativeManifestTest(unittest.TestCase):
    def test_nonfinite_overflow_and_legacy_nested_reference_rejected(self):
        raw = b'{"frames":[{"origin":{"value":1e309}}],"reusedHistory":[]}'
        with self.assertRaises(WorkflowError):
            decode_native_manifest(raw, sha(raw))
        with self.assertRaises(WorkflowError):
            read({"frames": [{"origin": {"nested": {"originRef": "a"*64}}}], "reusedHistory": []})
    def test_legacy_expanded_origins_and_proofs_are_preserved(self):
        origin = {"jobId": "old", "runtime": {"scope": "solver/v1"}}
        proof = {"origin": origin, "originManifestPath": "D:/private/native-result.manifest.json",
                 "originManifestSha256": sha(b"ancestor"), "recordNo": 1}
        legacy = {"frames": [{"recordNo": 1, "origin": origin, "historyProof": proof},
                             {"recordNo": 2, "origin": None, "historyProof": None}], "reusedHistory": [proof]}
        self.assertEqual(read(legacy), legacy)

    def test_resolves_original_qt_bytes_before_recovery_proof_capture(self):
        raw_origin = b'{"dt":0.000001,"jobId":"old","revision":1}'
        key = sha(raw_origin)
        proof = {"recordNo": 1, "originRef": key, "originalFrameSha256": sha(b"frame"),
                 "originManifestPath": "D:/private/native-result.manifest.json",
                 "originManifestSha256": sha(b"exact ancestor bytes")}
        legacy_proof = {**proof, "origin": json.loads(raw_origin)}
        legacy_proof.pop("originRef")
        value = compact_manifest([raw_origin, b"null"],
                                 [{"recordNo": 1, "originRef": key, "historyProof": proof},
                                  {"recordNo": 2, "originRef": sha(b"null")}], [proof])
        expanded = read(value)
        self.assertEqual(expanded["frames"][0]["historyProof"], legacy_proof)
        self.assertEqual(expanded["reusedHistory"][0], legacy_proof)
        self.assertIsNone(expanded["frames"][1]["origin"])
        self.assertEqual(sha(value["originTable"][key].encode()), key)
        self.assertNotEqual(sha(canonical(expanded["frames"][0]["origin"])), key)
        self.assertNotIn("originTable", expanded)

    def test_raw_expected_hash_is_mandatory_even_for_valid_table(self):
        value = compact_manifest([b'{"jobId":"old"}'], [{"originRef": sha(b'{"jobId":"old"}')}])
        raw = canonical(value)
        with self.assertRaises(WorkflowError):
            decode_native_manifest(raw, sha(b"different manifest"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "native-result.manifest.json"
            path.write_bytes(raw)
            self.assertEqual(load_native_manifest(path, sha(raw))["frames"][0]["origin"], {"jobId": "old"})
            path.write_bytes(raw + b" ")
            with self.assertRaises(WorkflowError):
                load_native_manifest(path, sha(raw))

    def test_rejects_origin_tampering_missing_references_and_ambiguous_records(self):
        raw_origin = b'{"jobId":"old"}'
        key = sha(raw_origin)
        base = compact_manifest([raw_origin], [{"originRef": key}])
        cases = []
        changed = copy.deepcopy(base)
        changed["originTable"][key] = '{"jobId":"forged"}'
        changed["originTableSha256"] = origin_table_sha256(changed["originTable"])
        cases.append(changed)
        changed = copy.deepcopy(base)
        changed["originTableSha256"] = sha(b"bad table hash")
        cases.append(changed)
        for alteration in ({"originRef": sha(b"missing")}, {"origin": {}},
                           {"originRef": None}, {"originRef": key, "origin": {}},
                           {"originRef": key, "metadata": {"originRef": key}},
                           {"originRef": key, "historyProof": {"originRef": key,
                                                               "nested": {"originRef": key}}}):
            changed = copy.deepcopy(base)
            changed["frames"] = [alteration]
            cases.append(changed)
        for field in ("originEncoding", "originTable", "originTableSha256"):
            changed = copy.deepcopy(base)
            del changed[field]
            cases.append(changed)
        changed = copy.deepcopy(base)
        changed["originTable"][sha(b"null")] = "null"
        changed["originTableSha256"] = origin_table_sha256(changed["originTable"])
        cases.append(changed)
        for value in cases:
            with self.subTest(value=value), self.assertRaises(WorkflowError):
                read(value)

    def test_rejects_duplicate_keys_nonfinite_and_undeclared_legacy_references(self):
        for raw in (b'{"frames":[],"frames":[]}', b'{"frames":[],"a":NaN}',
                    b'{"frames":[{"originRef":"missing"}]}'):
            with self.subTest(raw=raw), self.assertRaises(WorkflowError):
                decode_native_manifest(raw, sha(raw))
        encoded = b'{"a":1,"a":2}'
        with self.assertRaises(WorkflowError):
            read(compact_manifest([encoded], [{"originRef": sha(encoded)}]))

    def test_enforces_independent_raw_frame_and_expansion_budgets(self):
        encoded = b'{"metadata":"' + b"x" * 256 + b'"}'
        value = compact_manifest([encoded], [{"originRef": sha(encoded)}] * 3)
        with patch("range_workflow.native_manifest.MAX_EXPANDED_ORIGIN_BYTES", len(encoded) * 2):
            with self.assertRaises(WorkflowError):
                read(value)
        with patch("range_workflow.native_manifest.MAX_FRAMES", 2):
            with self.assertRaises(WorkflowError):
                read(value)
        with patch("range_workflow.native_manifest.MAX_MANIFEST_BYTES", 32):
            with self.assertRaises(WorkflowError):
                read(value)

    def test_full_5000_frame_index_with_representative_runtime_and_control_metadata(self):
        private_root = "D:/YOGA/Documents/Projects/range-fea/build/audit-20261009/WF-10/performance-dev-quiet/workspace/jobs/" + "a" * 36
        origin = {"jobId": "a" * 36, "proofScope": "solver-and-declared-controller-runtime/v1",
                  "sourceRuntimeStamp": {"documentId": "b" * 36, "documentRevision": 7},
                  "sourceSnapshotRef": {"projectId": "c" * 36, "sourceRevision": "r7",
                                        "registeredModelSha256": sha(b"source")},
                  "inputSnapshotSha256": sha(b"input"), "inputPackageSha256": sha(b"package"),
                  "runtime": {"fingerprintSha256": sha(b"runtime"), "scope": "declared-software-inputs/v1",
                              "inputs": {name: {"path": private_root + "/runtime/" + name + ".exe",
                                                "sha256": sha(name.encode())}
                                         for name in ("solver", "native", "nativeResults", "python")}},
                  "control": {"enabled": True, "controllerId": "stage9-velocity-feedback",
                              "controllerVersionSha256": sha(b"controller"),
                              "inputs": {name: {"path": private_root + "/" + name + ".py",
                                                "sha256": sha(name.encode())}
                                         for name in ("controller", "controllerHost", "gains", "runtimeParametersCodec")}}}
        encoded = canonical(origin)
        # Include realistic frozen runtime metadata at the observed 4 KiB scale.
        origin["runtime"]["buildIdentity"] = "x" * max(0, 4096 - len(encoded))
        encoded = canonical(origin)
        key = sha(encoded)
        frames = [{"recordNo": index, "timeS": index * 0.01,
                   "relativePath": f"active-{index:05d}.ram", "sha256": sha(str(index).encode()),
                   "nodePhysicalIdMapSha256": sha(b"nodes"), "originKind": "produced", "originRef": key}
                  for index in range(1, 5001)]
        value = compact_manifest([encoded], frames)
        raw = canonical(value)
        self.assertLess(len(raw), 16 * 1024 * 1024)
        expanded = decode_native_manifest(raw, sha(raw))
        self.assertEqual(len(expanded["frames"]), 5000)
        self.assertEqual(expanded["frames"][-1]["origin"], origin)
        self.assertGreater(len(canonical(expanded)), 16 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
