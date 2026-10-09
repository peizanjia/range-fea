"""Independent scalar hand calculations for accepted runtime switching."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import runtime_parameters as rp
from range_workflow.runtime_reference import accepted_schedule, replay


class RuntimeReferenceTest(unittest.TestCase):
    def fixture(self, *, maximum=10, rate=5):
        limits = [{"actuatorId": "f", "minN": -maximum, "maxN": maximum, "rateNPerS": rate}]
        initial = rp.make_parameters("b" * 64, 0, 1, limits)
        changed = rp.make_parameters("b" * 64, 1, 2, limits)
        proposal = "00000000-0000-4000-8000-000000000001"
        change = {"proposalId": proposal, "previousRevision": "0", "revision": "1",
                  "appliedSeq": "2", "acceptedTimeS": 2, "parameterSha256": changed["parameterSha256"]}
        state = {"schema": rp.SCHEMA, "parameters": changed, "history": [change]}
        records = {proposal: {"submittedAt": "durable-intent-before-effect", "parameters": changed}}
        return initial, state, records

    def run_reference(self, initial, schedule):
        return replay([[1]], [[0]], [[1]], [1], [1], [1], [0],
                      dt=1, steps=2, initial_parameters=initial, schedule=schedule)

    def test_hand_gain_switch_at_second_interval(self):
        initial, state, records = self.fixture()
        result = self.run_reference(initial, accepted_schedule(state, records))
        # First step q=.6,v=-.8. Second held force=1.6, refreshed a=1.
        self.assertAlmostEqual(result["q"][0], .36)
        self.assertAlmostEqual(result["v"][0], .32)
        self.assertAlmostEqual(result["a"][0], 1.24)
        self.assertAlmostEqual(result["controllerWork"], -.384)
        self.assertEqual(result["trajectory"]["parameterRevision"], ["0", "1"])

    def test_hand_amplitude_and_rate_are_applied_to_same_version(self):
        for maximum, rate, q, v, work in ((1, 5, .12, -.16, -.48), (10, .5, -.08, -.56, -.34)):
            with self.subTest(maximum=maximum, rate=rate):
                initial, state, records = self.fixture(maximum=maximum, rate=rate)
                result = self.run_reference(initial, accepted_schedule(state, records))
                self.assertAlmostEqual(result["q"][0], q)
                self.assertAlmostEqual(result["v"][0], v)
                self.assertAlmostEqual(result["controllerWork"], work)

    def test_missing_or_forged_history_values_cannot_be_constant_gain(self):
        _, state, records = self.fixture()
        with self.assertRaises(ValueError):
            accepted_schedule(state, {})
        broken = copy.deepcopy(records)
        next(iter(broken.values()))["parameters"]["velocityGainNsm"] = 3
        with self.assertRaises(ValueError):
            accepted_schedule(state, broken)

    def test_accepted_time_must_match_actual_clock(self):
        initial, state, records = self.fixture()
        schedule = accepted_schedule(state, records)
        schedule[0]["acceptedTimeS"] = 1.5
        with self.assertRaises(ValueError):
            self.run_reference(initial, schedule)


if __name__ == "__main__":
    unittest.main()
