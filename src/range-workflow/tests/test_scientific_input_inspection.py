"""Caller-authored scientific test metadata binding; no numerical runtime execution."""
from __future__ import annotations

import copy
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import scientific_contract as contract
from range_workflow.service import WorkflowError, WorkflowService


def analytic_input():
    return {"schema": contract.INPUT_SCHEMA, "algorithm": contract.ALGORITHM,
            "domain": "time", "scalarType": "real64", "times": [0, 1], "timeUnit": "s",
            "samplePeriodS": 1, "samplingRateHz": 1,
            "channels": [{"id": "own-dc", "quantity": "velocity", "axis": "x", "unit": "m/s", "values": [3, 3]}],
            "window": "rectangular", "removeMean": False, "filter": {"kind": "none"}}


def request(data, **extra):
    return {"apiVersion": "range.workflow/v1", "requestId": "inspect-own-case",
            "operation": "extension.scientific.input.inspect", "parameters": {"input": data, **extra}}


class ScientificInputInspectionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        examples = self.base / "examples"
        examples.mkdir()
        self.service = WorkflowService(self.base / "workspace", Path(sys.executable),
            Path(sys.executable), examples, Path(sys.executable), trusted_extensions=False)
        self.addCleanup(self.service.close)

    def files(self):
        result = {}
        for path in self.base.rglob("*"):
            if not path.is_file():
                continue
            if path == self.service.workspace / ".workflow.lock":
                # Windows denies a second handle access to the locked byte.
                # Read every byte through the actual owner's existing handle.
                handle = self.service._os_lock.file
                position = handle.tell()
                try:
                    handle.seek(0)
                    data = handle.read()
                finally:
                    handle.seek(position)
            else:
                data = path.read_bytes()
            result[str(path.relative_to(self.base))] = data
        return result

    def test_readonly_normalization_without_runtime_or_side_effects(self):
        data = analytic_input()
        state, files = copy.deepcopy(self.service._state), self.files()
        workers = dict(self.service._workers)
        threads = {t.ident for t in threading.enumerate()}
        with mock.patch.object(self.service, "_commit", side_effect=AssertionError("no commit")), \
             mock.patch.object(threading.Thread, "start", side_effect=AssertionError("no worker")), \
             mock.patch.object(self.service.runner, "run", side_effect=AssertionError("no process")):
            result = self.service.handle(request(data))
        normalized = contract.validate_input(data)
        self.assertEqual(result, {"normalizedInput": normalized, "inputHash": contract.canonical_hash(normalized),
            "contractHash": contract.CONTRACT_HASH, "scope": "caller-provided-test-input-binding",
            "nativeProvenanceVerified": False})
        self.assertIs(type(result["normalizedInput"]["times"][0]), float)
        self.assertEqual(data, analytic_input())
        result["normalizedInput"]["channels"][0]["values"][0] = 99
        self.assertEqual(data, analytic_input())
        self.assertEqual(self.service._state, state)
        self.assertEqual(self.files(), files)
        self.assertEqual(self.service._workers, workers)
        self.assertEqual({t.ident for t in threading.enumerate()}, threads)
        self.assertIsNone(self.service._runtime_verification_worker)
        self.assertFalse(self.service.trusted_extensions)
        capability = next(op for op in self.service.capabilities()["operations"]
                          if op["id"] == "extension.scientific.input.inspect")
        self.assertTrue(capability["readOnly"])
        self.assertEqual(capability["parameters"]["required"], ["input"])
        self.assertFalse(capability["parameters"]["additionalProperties"])

    def test_invalid_inputs_never_mutate_even_rejection_audit(self):
        mutations = [
            lambda d: d.update(path="C:/private/result.json"),
            lambda d: d.update(sourceActual=True),
            lambda d: d.update(times="not-an-array"),
            lambda d: d.update(times=[0]),
            lambda d: d.update(times=list(range(65))),
            lambda d: d.update(channels=[]),
            lambda d: d.update(channels=d["channels"] * 5),
            lambda d: d["channels"][0].update(values=[3]),
            lambda d: d["channels"][0].update(values=list(range(65))),
            lambda d: d["channels"][0].update(values=[float("nan"), 3]),
            lambda d: d["channels"][0].update(values=[float("inf"), 3]),
            lambda d: d["channels"][0].update(unit="N"),
            lambda d: d["channels"][0].update(axis="q"),
            lambda d: d["channels"][0].update(path="file.json"),
            lambda d: d.update(times=[0, 2]),
            lambda d: d.update(schema="arbitrary-input"),
            lambda d: d.update(removeMean=0),
            lambda d: d.update(filter={"kind": "none", "code": "pass"}),
            lambda d: d.update(huge=["x" * 65000] * 3),
        ]
        state, files = copy.deepcopy(self.service._state), self.files()
        with mock.patch.object(self.service, "_commit", side_effect=AssertionError("no rejection audit")):
            for mutate in mutations:
                data = analytic_input()
                mutate(data)
                with self.subTest(mutation=mutate), self.assertRaises(WorkflowError):
                    self.service.handle(request(data))
            for bad in (None, [], "arbitrary text"):
                with self.subTest(input=bad), self.assertRaises(WorkflowError):
                    self.service.handle(request(bad))
            with self.assertRaises(WorkflowError):
                self.service.handle(request(analytic_input(), code="pass"))
        self.assertEqual(self.service._state, state)
        self.assertEqual(self.files(), files)

    def test_dimension_limits_precede_full_validation(self):
        data = analytic_input()
        data["times"] = list(range(65))
        with mock.patch.object(contract, "validate_input", side_effect=AssertionError("dimensions first")):
            with self.assertRaises(WorkflowError):
                self.service.handle(request(data))

    def test_boundaries_and_tamper_change_binding_with_ordinary_mode_off(self):
        data = analytic_input()
        first = self.service.handle(request(data))
        data["channels"][0]["values"][1] = 4
        self.assertNotEqual(first["inputHash"], self.service.handle(request(data))["inputHash"])
        data = analytic_input()
        data["times"] = list(range(64))
        data["channels"][0]["values"] = [3] * 64
        data["channels"] += [dict(data["channels"][0], id=f"own-{i}") for i in range(3)]
        self.assertEqual(len(self.service.handle(request(data))["normalizedInput"]["channels"]), 4)
        self.assertFalse(self.service.trusted_extensions)

    def test_wrong_expected_hash_still_rejected_by_existing_candidate_contract(self):
        # Independently derived two-sample constant velocity: DFT=(6,0), PSD=(18,0).
        from range_workflow.scientific_extensions import validate_cases
        binding = self.service.handle(request(analytic_input()))
        data = binding["normalizedInput"]
        expected = {**{k: copy.deepcopy(data[k]) for k in ("algorithm", "times", "timeUnit",
            "samplePeriodS", "samplingRateHz", "window", "removeMean", "filter")},
            "schema": contract.OUTPUT_SCHEMA, "algorithm": contract.ALGORITHM,
            "contractHash": binding["contractHash"], "inputHash": binding["inputHash"],
            "domain": "frequency", "scalarType": "complex128-pairs", "frequenciesHz": [0., .5],
            "frequencyUnit": "Hz", "binSpacingHz": .5, "binCount": 2, "sampleCount": 2,
            "windowSum": 2., "windowEnergy": 2., "channels": [{
                "id": "own-dc", "quantity": "velocity", "axis": "x", "unit": "m/s", "amplitudeUnit": "m/s",
                "psdUnit": {"numeratorUnit": "m/s", "power": 2, "denominatorUnit": "Hz"},
                "filteredValues": [3., 3.], "fft": [[6., 0.], [0., 0.]], "amplitude": [3., 0.], "psd": [18., 0.]}]}
        tests = {"cases": [{"id": "own-analytic-dc", "input": data, "expected": expected}]}
        self.assertEqual(validate_cases(tests), tests)
        expected["inputHash"] = "0" * 64
        with self.assertRaises(WorkflowError):
            validate_cases(tests)
        self.assertFalse(self.service.trusted_extensions)


if __name__ == "__main__":
    unittest.main()
