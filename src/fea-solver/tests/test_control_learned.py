"""Focused tests for the dependency-free learned velocity controller."""

from __future__ import annotations

import copy
import importlib.util
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
RUNTIME_SCRIPT = EXAMPLES / "control_learned_python.py"
TRAINER_SCRIPT = EXAMPLES / "train_control_policy.py"


def import_script(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


runtime = import_script("range_fea_learned_runtime_test", RUNTIME_SCRIPT)


def policy_artifact() -> dict[str, Any]:
    return {
        "format": runtime.POLICY_FORMAT,
        "version": 1,
        "kind": "supervised-collocated-velocity-gain-distillation",
        "controlMode": "scalar-nodal-force",
        "sensorCatalog": [{"sensorId": "tip-velocity", "unit": "m/s"}],
        "actuatorCatalog": [{"actuatorId": "tip-force", "unit": "N"}],
        "normalization": {"inputOffset": 0.0, "inputScale": 0.5,
                          "inputDomain": [-0.5, 0.5], "outputOffset": 0.0,
                          "outputScale": 1.0},
        "network": {"activation": "tanh", "layerSizes": [1, 8, 1],
                    "layers": [{"weights": [[1.0] for _ in range(8)],
                                "bias": [0.0 for _ in range(8)]},
                               {"weights": [[-0.125 for _ in range(8)]], "bias": [0.0]}]},
        "distillation": {"law": "force=-gain*velocity", "gainNSecondsPerMeter": 2.0,
                         "trainingSamples": 257, "holdoutSamples": 1025,
                         "epochs": 10, "seed": 17},
    }


def handshake_request() -> dict[str, Any]:
    return {
        "protocolVersion": 1, "requestId": "handshake-1", "op": "handshake",
        "controllerId": "learned-test", "controllerRevision": "a" * 64,
        "modelSnapshotSha256": "b" * 64, "deterministicSeed": "17",
        "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                         "sensors": [{"sensorId": "tip-velocity", "unit": "m/s"}],
                         "actuatorIds": ["tip-force"]},
    }


def compute_request(*, step_id: str = "7", state: dict[str, Any] | None = None,
                    velocity: float = 0.125) -> dict[str, Any]:
    return {
        "protocolVersion": 1, "requestId": f"compute-{step_id}", "op": "compute",
        "stepId": step_id, "controllerId": "learned-test",
        "controllerRevision": "a" * 64, "modelSnapshotSha256": "b" * 64,
        "deterministicSeed": "17", "time": 0.01 * int(step_id), "dt": 0.01,
        "observations": [{"sensorId": "tip-velocity", "value": velocity, "unit": "m/s"}],
        "acceptedControllerState": {} if state is None else state,
    }


def run_child(policy_path: Path, requests: list[dict[str, Any]]) -> subprocess.CompletedProcess[str]:
    payload = "".join(json.dumps(request, allow_nan=False) + "\n" for request in requests)
    return subprocess.run([sys.executable, str(RUNTIME_SCRIPT), "--policy", str(policy_path)],
                          input=payload, capture_output=True, text=True, check=False)


class LearnedRuntimeTests(unittest.TestCase):
    def test_pure_replay_restart_and_accepted_state_are_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "policy.json"
            path.write_text(json.dumps(policy_artifact()), encoding="utf-8")
            handshake = handshake_request()
            compute = compute_request()

            first_process = run_child(path, [handshake, compute, compute])
            self.assertEqual(first_process.returncode, 0, first_process.stderr)
            first_replies = [json.loads(line) for line in first_process.stdout.splitlines()]
            self.assertEqual(first_replies[1], first_replies[2])
            self.assertEqual(first_replies[1]["commands"], [
                {"actuatorId": "tip-force", "force": -math.tanh(0.25), "unit": "N"}])
            self.assertEqual(first_replies[1]["nextControllerState"],
                             {"sampleCount": 1, "lastStepId": "7"})

            restarted = run_child(path, [handshake, compute])
            self.assertEqual(restarted.returncode, 0, restarted.stderr)
            restarted_reply = json.loads(restarted.stdout.splitlines()[1])
            self.assertEqual(first_replies[1], restarted_reply)

            resumed_compute = compute_request(step_id="8", state=first_replies[1]["nextControllerState"])
            resumed = run_child(path, [handshake, resumed_compute])
            self.assertEqual(resumed.returncode, 0, resumed.stderr)
            resumed_reply = json.loads(resumed.stdout.splitlines()[1])
            self.assertEqual(resumed_reply["nextControllerState"],
                             {"sampleCount": 2, "lastStepId": "8"})

    def test_extra_catalog_is_checked_by_id_and_observation_order_is_free(self) -> None:
        handler = runtime.LearnedVelocityRuntime(policy_artifact())
        request = handshake_request()
        request["capabilities"]["sensors"] = [
            {"sensorId": "tip-displacement", "unit": "m"},
            {"sensorId": "tip-velocity", "unit": "m/s"},
            {"sensorId": "tip-acceleration", "unit": "m/s^2"},
        ]
        handler.handle(request)
        reordered_catalog = copy.deepcopy(request)
        reordered_catalog["capabilities"]["sensors"].reverse()
        with self.assertRaisesRegex(runtime.ControllerError, "catalog cannot change"):
            handler.handle(reordered_catalog)
        compute = compute_request()
        compute["observations"] = [
            {"sensorId": "tip-acceleration", "value": 0.0, "unit": "m/s^2"},
            {"sensorId": "tip-velocity", "value": 0.125, "unit": "m/s"},
            {"sensorId": "tip-displacement", "value": 0.01, "unit": "m"},
        ]
        response = handler.handle(compute)
        self.assertEqual(response["commands"][0]["force"], -math.tanh(0.25))

        for observations in (
                compute["observations"][:-1],
                [*compute["observations"], compute["observations"][0]],
                [{**item, "sensorId": "not-offered"} if item["sensorId"] == "tip-acceleration" else item
                 for item in compute["observations"]],
                [{**item, "unit": "m"} if item["sensorId"] == "tip-velocity" else item
                 for item in compute["observations"]],
        ):
            with self.subTest(observations=observations):
                invalid = copy.deepcopy(compute)
                invalid["observations"] = observations
                with self.assertRaises(runtime.ControllerError):
                    handler.handle(invalid)

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "policy.json"
            path.write_text(json.dumps(policy_artifact()), encoding="utf-8")
            process = run_child(path, [request, compute])
            self.assertEqual(process.returncode, 0, process.stderr)
            replies = [json.loads(line) for line in process.stdout.splitlines()]
            self.assertEqual(replies[1]["commands"][0]["force"], -math.tanh(0.25))

    def test_handshake_identity_order_domain_and_nonfinite_values_fail_closed(self) -> None:
        wrong_order = handshake_request()
        wrong_order["capabilities"]["actuatorIds"] = ["other-force"]
        with self.assertRaisesRegex(runtime.ControllerError, "actuator catalog"):
            runtime.LearnedVelocityRuntime(policy_artifact()).handle(wrong_order)

        handler = runtime.LearnedVelocityRuntime(policy_artifact())
        handler.handle(handshake_request())
        wrong_identity = compute_request()
        wrong_identity["modelSnapshotSha256"] = "c" * 64
        with self.assertRaisesRegex(runtime.ControllerError, "identity mismatch"):
            handler.handle(wrong_identity)
        near_numeric_limit = compute_request(
            step_id="2", state={"sampleCount": 0x20000000000000, "lastStepId": "1"})
        with self.assertRaisesRegex(runtime.ControllerError, "bounded integer"):
            handler.handle(near_numeric_limit)
        for value in (0.5000001, -0.5000001, math.inf, math.nan):
            with self.subTest(value=value):
                invalid = compute_request(velocity=value)
                with self.assertRaises(runtime.ControllerError):
                    handler.handle(invalid)
        positive = handler.handle(compute_request(step_id="1", velocity=0.25))
        negative = handler.handle(compute_request(step_id="2", velocity=-0.25))
        zero = handler.handle(compute_request(step_id="3", velocity=0.0))
        self.assertLess(positive["commands"][0]["force"], 0.0)
        self.assertGreater(negative["commands"][0]["force"], 0.0)
        self.assertEqual(zero["commands"][0]["force"], 0.0)

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "policy.json"
            path.write_text(json.dumps(policy_artifact()), encoding="utf-8")
            nonfinite_request = compute_request(velocity=math.nan)
            payload = (json.dumps(handshake_request()) + "\n" +
                       json.dumps(nonfinite_request, allow_nan=True) + "\n")
            process = subprocess.run([sys.executable, str(RUNTIME_SCRIPT), "--policy", str(path)],
                                     input=payload, capture_output=True, text=True, check=False)
            self.assertEqual(process.returncode, 2)
            self.assertIn("non-finite JSON number", process.stderr)

    def test_weights_are_immutable_and_malformed_policy_files_are_rejected(self) -> None:
        source = policy_artifact()
        policy = runtime.LearnedVelocityPolicy.from_json(source)
        original = policy.hidden_weights[0][0]
        source["network"]["layers"][0]["weights"][0][0] = 99.0
        self.assertEqual(policy.hidden_weights[0][0], original)
        with self.assertRaises(TypeError):
            policy.hidden_weights[0][0] = 5.0

        bad_policies = []
        missing = policy_artifact()
        del missing["network"]["layers"][0]["weights"]
        bad_policies.append(missing)
        wrong_shape = policy_artifact()
        wrong_shape["network"]["layers"][0]["weights"] = [[0.0]]
        bad_policies.append(wrong_shape)
        oversized = policy_artifact()
        oversized["network"]["layers"][0]["weights"][0][0] = 1.0e100
        bad_policies.append(oversized)

        with tempfile.TemporaryDirectory() as temporary:
            for index, artifact in enumerate(bad_policies):
                path = Path(temporary) / f"bad-{index}.json"
                path.write_text(json.dumps(artifact), encoding="utf-8")
                process = subprocess.run([sys.executable, str(RUNTIME_SCRIPT), "--weights", str(path)],
                                         input="", capture_output=True, text=True, check=False)
                self.assertEqual(process.returncode, 2)
                self.assertEqual(process.stdout, "")
            nan_artifact = policy_artifact()
            nan_artifact["network"]["layers"][0]["bias"][0] = math.nan
            nan_path = Path(temporary) / "nan.json"
            nan_path.write_text(json.dumps(nan_artifact), encoding="utf-8")
            process = subprocess.run([sys.executable, str(RUNTIME_SCRIPT), "--weights", str(nan_path)],
                                     input="", capture_output=True, text=True, check=False)
            self.assertEqual(process.returncode, 2)
            self.assertIn("non-finite JSON number", process.stderr)


class LearnedTrainerTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("torch") is not None,
                         "PyTorch is optional for offline policy training")
    def test_training_cli_reports_holdout_and_export_parity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "trained-policy.json"
            process = subprocess.run(
                [sys.executable, str(TRAINER_SCRIPT), "--gain", "2.0",
                 "--sensor-id", "tip-velocity", "--actuator-id", "tip-force",
                 "--velocity-scale", "0.5", "--output", str(output),
                 "--epochs", "700", "--seed", "123"],
                capture_output=True, text=True, check=False)
            self.assertEqual(process.returncode, 0, process.stderr)
            report = json.loads(process.stdout)
            self.assertTrue(output.is_file())
            self.assertEqual(report["holdout"]["samples"], 1025)
            self.assertGreaterEqual(report["holdout"]["maxAbsErrorN"], 0.0)
            self.assertGreaterEqual(report["holdout"]["rmsErrorN"], 0.0)
            self.assertLess(report["torchVsExported"]["maxAbsDifferenceN"], 1.0e-12)
            exported = runtime.load_policy(str(output))
            self.assertEqual(exported.sensor_id, "tip-velocity")
            self.assertEqual(exported.actuator_id, "tip-force")


if __name__ == "__main__":
    unittest.main()
