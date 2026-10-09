"""Host/native golden identity and malformed accepted-state checks."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import runtime_parameters as rp


def base_limits():
    return [{"actuatorId": "f", "minN": -1.0, "maxN": 1.0, "rateNPerS": 0.0}]


def fresh():
    return {"schema": rp.SCHEMA,
            "parameters": rp.make_parameters("0" * 64, 0, 1, base_limits()), "history": []}


class RuntimeParametersTest(unittest.TestCase):
    def test_cpp_golden_vector(self):
        p = fresh()["parameters"]
        self.assertEqual(p["parameterSha256"],
                         "7f99575c73b6790a5748e6275a62127385d496df16aa7bb1336a425aa1ee54cc")
        self.assertEqual(p["runtimeControllerRevision"],
                         "a6546d489a5f102a1de13955556b011d89dbbef9e97be40a61048ea54e817669")

    def test_signed_zero_and_candidate_binding(self):
        original = fresh()
        zero = base_limits()
        zero[0]["rateNPerS"] = -0.0
        self.assertEqual(rp.make_parameters("0" * 64, 0, 1, zero), original["parameters"])
        p = rp.candidate(original, 2, zero, base_limits())
        self.assertEqual(p["revision"], "1")
        self.assertNotEqual(p["parameterSha256"], original["parameters"]["parameterSha256"])
        self.assertEqual(original, fresh())

    def test_forged_values_hashes_and_boolean_numbers(self):
        for field, replacement in (("velocityGainNsm", 2), ("revision", "01"),
                                   ("velocityGainNsm", True), ("parameterSha256", "f" * 64),
                                   ("runtimeControllerRevision", "f" * 64)):
            with self.subTest(field=field, value=replacement):
                value = fresh()["parameters"]
                value[field] = replacement
                with self.assertRaises(ValueError):
                    rp.validate_parameters(value)
        for bad in (float("inf"), float("nan"), 0, 1e13):
            with self.assertRaises(ValueError):
                rp.make_parameters("0" * 64, 0, bad, base_limits())

    def test_limits_must_stay_in_immutable_envelope(self):
        initial = fresh()
        limited = base_limits()
        limited[0]["rateNPerS"] = 1.0
        for changed in (base_limits(), [{**limited[0], "minN": -2.0}],
                        [{**limited[0], "actuatorId": "unknown"}]):
            with self.assertRaises(ValueError):
                rp.candidate(initial, 2, changed, limited)

    def test_actual_history_requires_sequence_time_and_digest(self):
        first = fresh()
        p = rp.candidate(first, 2, base_limits(), base_limits())
        accepted = {"schema": rp.SCHEMA, "parameters": p, "history": [{
            "proposalId": "00000000-0000-4000-8000-000000000001", "previousRevision": "0",
            "revision": "1", "appliedSeq": "17", "acceptedTimeS": 0.25,
            "parameterSha256": p["parameterSha256"]}]}
        self.assertEqual(rp.validate_state(accepted), accepted)
        for field, bad in (("appliedSeq", "0"), ("acceptedTimeS", -1),
                           ("revision", "2"), ("parameterSha256", "f" * 64)):
            corrupted = copy.deepcopy(accepted)
            corrupted["history"][0][field] = bad
            with self.assertRaises(ValueError):
                rp.validate_state(corrupted)


if __name__ == "__main__":
    unittest.main()
