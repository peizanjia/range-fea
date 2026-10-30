"""Exercise the real Stage 9 child without installed scientific dependencies."""

from __future__ import annotations

import hashlib
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = (Path(__file__).resolve().parents[1] / "examples" / "stage9" /
          "stage9_velocity_controller.py")

# -I -S removes user/site setup; the blocker also rejects dependencies even if
# an interpreter makes them available elsewhere. Execute the production CLI.
BLOCKER = """
import builtins
import runpy
import sys
original_import = builtins.__import__
def blocked_import(name, *args, **kwargs):
    if name.split('.')[0] in ('numpy', 'scipy'):
        raise ModuleNotFoundError('scientific dependency blocked: ' + name)
    return original_import(name, *args, **kwargs)
builtins.__import__ = blocked_import
script = sys.argv[1]
sys.argv = sys.argv[1:]
"""


def analytic_gain() -> dict:
    """Independent one-DOF fixture: M=2, C=0.8, K=8, target zeta=0.3."""
    poles = [[-0.6, -math.sqrt(3.64)], [-0.6, math.sqrt(3.64)]]
    return {
        "format": "range-fea-stage9-velocity-gain-v1", "controllerLaw": "u=-g*y",
        "modelSha256": "a" * 64, "modelCanonicalSha256": "b" * 64,
        "sourceSha256": "c" * 64, "sourceHashScope": "native-fe-model-file-bytes",
        "sensor": {"id": "tip-velocity", "quantity": "Velocity", "unit": "m/s"},
        "actuator": {"id": "tip-force", "quantity": "NodalForce", "unit": "N"},
        "gain": 1.6, "gainUnit": "N*s/m",
        "mode": {"index": 0, "omegaRadPerSecond": 2.0,
                 "modalForceParticipation": 1.0 / math.sqrt(2.0),
                 "existingDampingRatio": 0.1, "requestedTargetDampingRatio": 0.3,
                 "predictedSingleModeDampingRatio": 0.3},
        "reducedOrder": {"order": 2, "polesRealImaginary": poles},
        "fullOrderPoleCheck": {"order": 2, "polesRealImaginary": poles,
                               "spectralAbscissaPerSecond": -0.6,
                               "noPositiveGrowthVerified": True},
    }


class RuntimeStartupTests(unittest.TestCase):
    def run_child(self, gains: dict, requests: list[dict]) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as temporary:
            gain_path = Path(temporary) / "gain.json"
            gain_path.write_text(json.dumps(gains), encoding="utf-8")
            return subprocess.run(
                [sys.executable, "-I", "-S", "-c", BLOCKER +
                 "runpy.run_path(script, run_name='__main__')\n",
                 str(SCRIPT), "--gains", str(gain_path)],
                input="".join(json.dumps(item) + "\n" for item in requests),
                text=True, capture_output=True, timeout=15, check=False)

    def requests(self) -> list[dict]:
        identity = {"controllerId": "stage9-velocity",
                    "controllerRevision": hashlib.sha256(SCRIPT.read_bytes()).hexdigest(),
                    "modelSnapshotSha256": "c" * 64, "deterministicSeed": "17"}
        handshake = {"protocolVersion": 1, "requestId": "h1", "op": "handshake",
                     **identity,
                     "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                                      "sensors": [{"sensorId": "tip-velocity", "unit": "m/s"}],
                                      "actuatorIds": ["tip-force"]}}
        compute = {"protocolVersion": 1, "requestId": "c1", "op": "compute",
                   **identity, "stepId": "1", "time": 0.0, "dt": 0.005,
                   "observations": [{"sensorId": "tip-velocity", "unit": "m/s", "value": 0.25}],
                   "acceptedControllerState": {}}
        return [handshake, compute]

    def test_real_cli_initializes_handshakes_and_computes_without_scientific_imports(self) -> None:
        requests = self.requests()
        result = self.run_child(analytic_gain(), requests)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(replies), 2)
        self.assertTrue(replies[0]["ok"])
        for key in ("controllerId", "controllerRevision", "modelSnapshotSha256", "deterministicSeed"):
            self.assertEqual(replies[0][key], requests[0][key])
            self.assertEqual(replies[1][key], requests[1][key])
        self.assertEqual(replies[1]["commands"],
                         [{"actuatorId": "tip-force", "force": -0.4, "unit": "N"}])
        self.assertEqual(replies[1]["nextControllerState"], {"sampleCount": 1, "lastStepId": "1"})

    def test_source_digest_mismatch_is_still_rejected_without_scientific_imports(self) -> None:
        requests = self.requests()
        requests[0]["modelSnapshotSha256"] = "d" * 64
        result = self.run_child(analytic_gain(), requests[:1])
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("native FE model digest does not match", result.stderr)

    def test_offline_design_replay_and_cli_vectors_import_dependencies_on_use(self) -> None:
        calls = (
            "module['design_gain']('unused.json', mode=0, target_damping=0.3, "
            "sensor_id='tip-velocity', actuator_id='tip-force')",
            "module['replay']('unused.json', 'unused-gain.json', dt=0.005, steps=1, initial_q=[0.0])",
            "module['_initial_q_json']('[0.0]')",
            "module['_initial_v_json']('[0.0]')",
        )
        for call in calls:
            with self.subTest(call=call):
                code = (BLOCKER + "module = runpy.run_path(script)\n" +
                        "assert 'numpy' not in sys.modules and 'scipy' not in sys.modules\n" +
                        "try:\n    " + call + "\n" +
                        "except ModuleNotFoundError as error:\n" +
                        "    assert 'scientific dependency blocked:' in str(error), str(error)\n" +
                        "else:\n    raise AssertionError('offline path did not import its dependency')\n")
                result = subprocess.run([sys.executable, "-I", "-S", "-c", code, str(SCRIPT)],
                                        text=True, capture_output=True, timeout=15, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
