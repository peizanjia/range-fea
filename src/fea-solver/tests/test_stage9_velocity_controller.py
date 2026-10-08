"""Focused Python tests for Stage 9 velocity-gain design and v1 runtime."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from scipy import linalg

SCRIPT = (Path(__file__).resolve().parents[1] / "examples" / "stage9" /
          "stage9_velocity_controller.py")
SPEC = importlib.util.spec_from_file_location("stage9_velocity_controller", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
controller = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(controller)


def one_dof_model() -> dict:
    return {
        "schema": "range-fea-linear-model", "version": 1,
        "source": {"kind": "linear-elastic-FE", "units": "SI",
                   "coordinateMeaning": "perturbations about static equilibrium"},
        "dofs": [{"nodePhysicalId": "node-tip", "nodePosition": 0,
                  "dof": "UX", "unit": "m", "frame": "global",
                  "globalDirection": [1.0, 0.0, 0.0]}],
        "inputs": [{"id": "tip-force", "unit": "N", "nodePhysicalId": "node-tip",
                    "nodePosition": 0}],
        "outputs": [{"id": "tip-velocity", "quantity": "Velocity", "unit": "m/s",
                     "nodePhysicalId": "node-tip", "nodePosition": 0}],
        "operating_point": {"q": [0.0], "v": [0.0], "a": [0.0], "force": [0.0],
                            "residual": 0.0},
        "M": [[2.0]], "C": [[0.8]], "K": [[8.0]], "B": [[1.0]],
        "Sq": [[0.0]], "Sv": [[1.0]], "Sa": [[0.0]], "D": [[0.0]],
    }


def coupled_two_dof_model() -> dict:
    model = one_dof_model()
    model["dofs"] = [
        {"nodePhysicalId": "node-1", "nodePosition": 0, "dof": "UX", "unit": "m",
         "frame": "global", "globalDirection": [1.0, 0.0, 0.0]},
        {"nodePhysicalId": "node-2", "nodePosition": 1, "dof": "UX", "unit": "m",
         "frame": "global", "globalDirection": [1.0, 0.0, 0.0]},
    ]
    model["inputs"][0].update({"nodePhysicalId": "node-1", "nodePosition": 0})
    model["outputs"][0].update({"nodePhysicalId": "node-1", "nodePosition": 0})
    model["operating_point"] = {"q": [0.0, 0.0], "v": [0.0, 0.0],
                                "a": [0.0, 0.0], "force": [0.0, 0.0], "residual": 0.0}
    model["M"] = [[1.0, 0.0], [0.0, 1.0]]
    model["C"] = [[0.08, 0.0], [0.0, 0.12]]
    model["K"] = [[4.0, 0.25], [0.25, 9.0]]
    model["B"] = [[1.0], [0.0]]
    model["Sq"] = [[0.0, 0.0]]
    model["Sv"] = [[1.0, 0.0]]
    model["Sa"] = [[0.0, 0.0]]
    model["D"] = [[0.0]]
    return model


def write_model(folder: Path, model: dict) -> Path:
    path = folder / "linear_model.json"
    path.write_text(json.dumps(model, separators=(",", ":")), encoding="utf-8")
    return path


def handshake(runtime: object, sensors: list[dict] | None = None,
              model_hash: str = "b" * 64) -> dict:
    return runtime.handle({
        "protocolVersion": 1, "requestId": "handshake-1", "op": "handshake",
        "controllerId": "stage9-velocity", "controllerRevision": "a" * 64,
        "modelSnapshotSha256": model_hash, "deterministicSeed": "17",
        "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                         "sensors": sensors or [{"sensorId": "tip-velocity", "unit": "m/s"}],
                         "actuatorIds": ["tip-force"]},
    })


class VelocityGainDesignTests(unittest.TestCase):
    def test_analytic_one_dof_gain_and_complex_full_order_poles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model_path = write_model(Path(temporary), one_dof_model())
            artifact = controller.design_gain(
                model_path, mode=0, target_damping=0.3,
                sensor_id="tip-velocity", actuator_id="tip-force")

        # M=2, K=8 gives omega=2; the initial modal damping ratio is 0.1.
        # The mass-normalized participation is 1/sqrt(2), giving g=1.6 N*s/m.
        self.assertAlmostEqual(artifact["mode"]["omegaRadPerSecond"], 2.0, places=12)
        self.assertAlmostEqual(artifact["mode"]["existingDampingRatio"], 0.1, places=12)
        self.assertAlmostEqual(artifact["gain"], 1.6, places=12)
        self.assertAlmostEqual(artifact["mode"]["predictedSingleModeDampingRatio"], 0.3, places=12)
        poles = artifact["fullOrderPoleCheck"]["polesRealImaginary"]
        self.assertEqual(len(poles), 2)
        self.assertAlmostEqual(poles[0][0], -0.6, places=11)
        self.assertAlmostEqual(poles[1][0], -0.6, places=11)
        self.assertAlmostEqual(abs(poles[0][1]), math.sqrt(3.64), places=11)
        self.assertAlmostEqual(abs(poles[1][1]), math.sqrt(3.64), places=11)
        self.assertAlmostEqual(artifact["fullOrderPoleCheck"]["spectralAbscissaPerSecond"],
                               -0.6, places=11)
        self.assertTrue(artifact["fullOrderPoleCheck"]["noPositiveGrowthVerified"])

    def test_negative_gain_and_noncollocated_sensor_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            with self.assertRaisesRegex(controller.ControllerError, "zero or negative"):
                controller.design_gain(model_path, mode=0, target_damping=0.05,
                                       sensor_id="tip-velocity", actuator_id="tip-force")

            noncollocated = one_dof_model()
            noncollocated["Sv"] = [[-1.0]]
            model_path = write_model(folder, noncollocated)
            with self.assertRaisesRegex(controller.ControllerError, "not collocated"):
                controller.design_gain(model_path, mode=0, target_damping=0.3,
                                       sensor_id="tip-velocity", actuator_id="tip-force")

            non_psd = one_dof_model()
            non_psd["C"] = [[-0.1]]
            model_path = write_model(folder, non_psd)
            with self.assertRaisesRegex(controller.ControllerError, "C must be positive semidefinite"):
                controller.design_gain(model_path, mode=0, target_damping=0.3,
                                       sensor_id="tip-velocity", actuator_id="tip-force")

    def test_mixed_output_quantity_and_missing_ids_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            bad_quantity = one_dof_model()
            bad_quantity["outputs"][0]["quantity"] = "Displacement"
            bad_quantity["outputs"][0]["unit"] = "m"
            model_path = write_model(folder, bad_quantity)
            with self.assertRaisesRegex(controller.ControllerError, "must be a physical velocity"):
                controller.design_gain(model_path, mode=0, target_damping=0.3,
                                       sensor_id="tip-velocity", actuator_id="tip-force")

            model_path = write_model(folder, one_dof_model())
            with self.assertRaisesRegex(controller.ControllerError, "sensor ID is missing"):
                controller.design_gain(model_path, mode=0, target_damping=0.3,
                                       sensor_id="missing", actuator_id="tip-force")

    def test_bad_dof_axis_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for direction, message in (([2.0, 0.0, 0.0], "normalized 3-vector"),
                                       ([0.0, 1.0, 0.0], "does not match its global DOF label")):
                model = one_dof_model()
                model["dofs"][0]["globalDirection"] = direction
                model_path = write_model(Path(temporary), model)
                with self.assertRaisesRegex(controller.ControllerError, message):
                    controller.design_gain(model_path, mode=0, target_damping=0.3,
                                           sensor_id="tip-velocity", actuator_id="tip-force")

    def test_constraint_local_unit_axis_is_supported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = one_dof_model()
            model["dofs"][0]["frame"] = "constraint-local"
            model["dofs"][0]["globalDirection"] = [0.0, 1.0, 0.0]
            model_path = write_model(Path(temporary), model)
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
        self.assertAlmostEqual(artifact["gain"], 1.6)

    def test_boolean_matrix_entries_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = one_dof_model()
            model["M"] = [[True]]
            model_path = write_model(Path(temporary), model)
            with self.assertRaisesRegex(controller.ControllerError, "finite JSON numbers"):
                controller.design_gain(model_path, mode=0, target_damping=0.3,
                                       sensor_id="tip-velocity", actuator_id="tip-force")

    def test_model_hashes_and_atomic_gain_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
            output_path = folder / "gains.json"
            controller.write_json_atomic(output_path, artifact)
            raw = model_path.read_bytes()
            decoded = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(decoded["modelSha256"], hashlib.sha256(raw).hexdigest())
            self.assertEqual(decoded["format"], controller.GAIN_FORMAT)
            self.assertFalse(list(folder.glob(".gains.json.*.tmp")))

    def test_newmark_full_and_one_mode_replay_energy_and_projection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
            gains_path = folder / "gains.json"
            controller.write_json_atomic(gains_path, artifact)
            report = controller.replay(model_path, gains_path, dt=0.005, steps=400,
                                       initial_q=[0.01])

        self.assertEqual(len(report["timeOutputs"]["timeSeconds"]), 401)
        self.assertEqual(len(report["timeOutputs"]["requestedForceN"]), 400)
        self.assertLess(report["energiesJ"]["fullControlled"], report["energiesJ"]["fullPassive"])
        self.assertLess(report["controllerWorkJ"], 0.0)
        self.assertLess(max(report["projectionError"]["qAbsoluteNorm"]), 1.0e-12)
        self.assertLess(max(report["projectionError"]["vAbsoluteNorm"]), 1.0e-12)
        self.assertAlmostEqual(report["fullOrder"]["q"][0], report["reducedOrder"]["q"][0], places=12)
        self.assertAlmostEqual(report["fullOrder"]["v"][0], report["reducedOrder"]["v"][0], places=12)

    def test_saturated_initial_velocity_uses_held_force_for_initial_acceleration(self) -> None:
        for velocity, expected_force in ((10.0, -3.0), (-10.0, 2.0)):
            trajectory = controller._trajectory(
                one_dof_model(), np.array([0.0]), np.array([velocity]),
                force_column=np.array([1.0]), velocity_row=np.array([1.0]), gain=4.0,
                dt=0.01, steps=1, feedback=True, force_minimum=-3.0, force_maximum=2.0)
            self.assertEqual(trajectory["heldForce"], [expected_force])
            initial_acceleration = (expected_force - 0.8 * velocity) / 2.0
            self.assertAlmostEqual(trajectory["a"][0][0], initial_acceleration)
            # Independent scalar Newmark calculation, with the saturated force held throughout.
            predicted_q = 0.01 * velocity + 0.000025 * initial_acceleration
            predicted_v = velocity + 0.005 * initial_acceleration
            expected_q = (expected_force + 80000.0 * predicted_q +
                          0.8 * (200.0 * predicted_q - predicted_v)) / 80168.0
            expected_v = predicted_v + 0.005 * (expected_q - predicted_q) / 0.000025
            self.assertAlmostEqual(trajectory["q"][1][0], expected_q)
            self.assertAlmostEqual(trajectory["v"][1][0], expected_v)

    def test_sampled_newmark_solution_converges_to_continuous_feedback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
            gains_path = folder / "gains.json"
            controller.write_json_atomic(gains_path, artifact)
            coarse = controller.replay(model_path, gains_path, dt=0.04, steps=25,
                                       initial_q=[0.01])
            fine = controller.replay(model_path, gains_path, dt=0.02, steps=50,
                                     initial_q=[0.01])

        continuous_a = np.array([[0.0, 1.0], [-4.0, -(0.8 + artifact["gain"]) / 2.0]])
        exact = linalg.expm(continuous_a * 1.0) @ np.array([0.01, 0.0])
        coarse_state = np.array([coarse["fullOrder"]["q"][0], coarse["fullOrder"]["v"][0]])
        fine_state = np.array([fine["fullOrder"]["q"][0], fine["fullOrder"]["v"][0]])
        self.assertLess(np.linalg.norm(fine_state - exact), np.linalg.norm(coarse_state - exact))

    def test_coupled_full_order_time_error_is_distinct_from_modal_projection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model = coupled_two_dof_model()
            model_path = write_model(folder, model)
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.4,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
            gains_path = folder / "gains.json"
            controller.write_json_atomic(gains_path, artifact)
            eigenvalues, modes = linalg.eigh(np.asarray(model["K"]), np.asarray(model["M"]))
            self.assertGreater(eigenvalues[0], 0.0)
            initial_q = (0.01 * modes[:, 0]).tolist()
            report = controller.replay(model_path, gains_path, dt=0.005, steps=400,
                                       initial_q=initial_q)

        projection_error = max(report["projectionError"]["qAbsoluteNorm"])
        trajectory_error = max(report["trajectoryError"]["qAbsoluteNorm"])
        self.assertGreater(trajectory_error, 1.0e-7)
        self.assertGreaterEqual(trajectory_error, projection_error)
        self.assertGreater(max(report["trajectoryError"]["sensorVelocityAbsolute"]), 1.0e-8)

    def test_public_design_and_replay_cli(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            gains_path = folder / "gains.json"
            design = subprocess.run(
                [sys.executable, str(SCRIPT), "--model", str(model_path),
                 "--mode", "0", "--target-damping", "0.3",
                 "--sensor-id", "tip-velocity", "--actuator-id", "tip-force",
                 "--output", str(gains_path)],
                capture_output=True, text=True, check=False)
            self.assertEqual(design.returncode, 0, design.stderr)
            self.assertAlmostEqual(json.loads(gains_path.read_text(encoding="utf-8"))["gain"], 1.6)

            report_path = folder / "replay.json"
            replay_process = subprocess.run(
                [sys.executable, str(SCRIPT), "--replay-model", str(model_path),
                 "--gains", str(gains_path), "--dt", "0.005", "--steps", "4",
                 "--initial-q-json", "[0.01]", "--output", str(report_path)],
                capture_output=True, text=True, check=False)
            self.assertEqual(replay_process.returncode, 0, replay_process.stderr)
            self.assertEqual(json.loads(report_path.read_text(encoding="utf-8"))["steps"], 4)

    def test_cli_refuses_output_alias_and_preserves_input_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            before = hashlib.sha256(model_path.read_bytes()).hexdigest()
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--model", str(model_path),
                 "--target-damping", "0.3", "--sensor-id", "tip-velocity",
                 "--actuator-id", "tip-force", "--output", str(model_path)],
                capture_output=True, text=True, check=False)
            after = hashlib.sha256(model_path.read_bytes()).hexdigest()
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("aliases an input file", process.stderr)
        self.assertEqual(after, before)

    def test_cli_refuses_replay_gain_output_alias_and_preserves_gain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            gains_path = folder / "gains.json"
            controller.write_json_atomic(
                gains_path, controller.design_gain(model_path, mode=0, target_damping=0.3,
                                                   sensor_id="tip-velocity", actuator_id="tip-force"))
            before = hashlib.sha256(gains_path.read_bytes()).hexdigest()
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--replay-model", str(model_path),
                 "--gains", str(gains_path), "--dt", "0.01", "--steps", "1",
                 "--initial-q-json", "[0.01]", "--output", str(gains_path)],
                capture_output=True, text=True, check=False)
            after = hashlib.sha256(gains_path.read_bytes()).hexdigest()
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("aliases an input file", process.stderr)
        self.assertEqual(after, before)

    def test_cli_refuses_hardlink_output_alias(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            alias = folder / "model-hardlink.json"
            try:
                os.link(model_path, alias)
            except OSError as error:
                self.skipTest(f"hard links unavailable in temporary directory: {error}")
            before = hashlib.sha256(model_path.read_bytes()).hexdigest()
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--model", str(model_path),
                 "--target-damping", "0.3", "--sensor-id", "tip-velocity",
                 "--actuator-id", "tip-force", "--output", str(alias)],
                capture_output=True, text=True, check=False)
            after = hashlib.sha256(model_path.read_bytes()).hexdigest()
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("aliases an input file", process.stderr)
        self.assertEqual(after, before)


class VelocityControllerProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model_path = write_model(Path(temporary), one_dof_model())
            self.gains = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                                sensor_id="tip-velocity", actuator_id="tip-force")

    def compute_request(self, *, step_id: str, state: dict, velocity: float = 0.25,
                        observations: list[dict] | None = None) -> dict:
        return {
            "protocolVersion": 1, "requestId": f"compute-{step_id}", "op": "compute",
            "stepId": step_id, "controllerId": "stage9-velocity",
            "controllerRevision": "a" * 64, "modelSnapshotSha256": "b" * 64,
            "deterministicSeed": "17", "time": 0.01 * int(step_id), "dt": 0.01,
            "observations": observations or
            [{"sensorId": "tip-velocity", "value": velocity, "unit": "m/s"}],
            "acceptedControllerState": state,
        }

    def test_v1_handshake_force_and_accepted_state_resume(self) -> None:
        runtime = controller.VelocityControllerRuntime(self.gains)
        response = handshake(runtime)
        self.assertTrue(response["ok"])
        self.assertEqual(response["modelSnapshotSha256"], "b" * 64)

        first = runtime.handle(self.compute_request(step_id="4", state={}))
        self.assertEqual(first["requestId"], "compute-4")
        self.assertEqual(first["stepId"], "4")
        self.assertEqual(first["commands"], [{"actuatorId": "tip-force", "force": -0.4,
                                                "unit": "N"}])
        self.assertEqual(first["nextControllerState"], {"sampleCount": 1, "lastStepId": "4"})

        # A new process has no hidden counter; the accepted JSON state resumes it.
        resumed = controller.VelocityControllerRuntime(self.gains)
        handshake(resumed)
        second = resumed.handle(self.compute_request(step_id="5", state=first["nextControllerState"]))
        self.assertEqual(second["nextControllerState"], {"sampleCount": 2, "lastStepId": "5"})

    def test_protocol_fails_closed_for_mixed_unit_identity_and_step_replay(self) -> None:
        runtime = controller.VelocityControllerRuntime(self.gains)
        handshake(runtime)
        wrong_unit = self.compute_request(step_id="1", state={})
        wrong_unit["observations"][0]["unit"] = "m"
        with self.assertRaisesRegex(controller.ControllerError, "observation ID/unit"):
            runtime.handle(wrong_unit)

        wrong_identity = self.compute_request(step_id="1", state={})
        wrong_identity["modelSnapshotSha256"] = "c" * 64
        with self.assertRaisesRegex(controller.ControllerError, "identity mismatch"):
            runtime.handle(wrong_identity)

        first = runtime.handle(self.compute_request(step_id="2", state={}))
        replay = self.compute_request(step_id="2", state=first["nextControllerState"])
        with self.assertRaisesRegex(controller.ControllerError, "must advance"):
            runtime.handle(replay)

    def test_handshake_refuses_wrong_selected_sensor_unit(self) -> None:
        runtime = controller.VelocityControllerRuntime(self.gains)
        request = {
            "protocolVersion": 1, "requestId": "handshake-1", "op": "handshake",
            "controllerId": "stage9-velocity", "controllerRevision": "a" * 64,
            "modelSnapshotSha256": "b" * 64, "deterministicSeed": "17",
            "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                         "sensors": [{"sensorId": "tip-velocity", "unit": "m"}],
                             "actuatorIds": ["tip-force"]},
        }
        with self.assertRaisesRegex(controller.ControllerError, "configured velocity sensor in m/s"):
            runtime.handle(request)

    def test_handshake_and_compute_accept_complete_native_multisensor_catalog(self) -> None:
        runtime = controller.VelocityControllerRuntime(self.gains)
        catalog = [{"sensorId": "tip-displacement", "unit": "m"},
                   {"sensorId": "tip-velocity", "unit": "m/s"},
                   {"sensorId": "tip-acceleration", "unit": "m/s^2"}]
        handshake(runtime, catalog)
        request = self.compute_request(step_id="1", state={}, velocity=0.125, observations=[
            {"sensorId": "tip-acceleration", "value": 0.0, "unit": "m/s^2"},
            {"sensorId": "tip-displacement", "value": 0.02, "unit": "m"},
            {"sensorId": "tip-velocity", "value": 0.125, "unit": "m/s"},
        ])
        response = runtime.handle(request)
        self.assertAlmostEqual(response["commands"][0]["force"], -0.2)

        request["observations"][0]["sensorId"] = "unoffered"
        with self.assertRaisesRegex(controller.ControllerError, "observation ID/unit"):
            runtime.handle(request)

    def test_handshake_binds_native_model_hash_when_gain_artifact_has_one(self) -> None:
        bound = dict(self.gains)
        bound["sourceSha256"] = "c" * 64
        bound["sourceHashScope"] = "native-fe-model-file-bytes"
        runtime = controller.VelocityControllerRuntime(bound)
        with self.assertRaisesRegex(controller.ControllerError, "does not match the gain artifact"):
            handshake(runtime, model_hash="b" * 64)

    def test_tampered_gain_artifact_is_rejected(self) -> None:
        tampered = dict(self.gains)
        tampered["gain"] = self.gains["gain"] * 2.0
        with self.assertRaisesRegex(controller.ControllerError, "do not match its design formula"):
            controller.VelocityControllerRuntime(tampered)

    def test_public_runtime_cli_serves_v1_json_lines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = write_model(folder, one_dof_model())
            artifact = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                              sensor_id="tip-velocity", actuator_id="tip-force")
            gains_path = folder / "gains.json"
            controller.write_json_atomic(gains_path, artifact)
            # Reuse the wire-complete handshake fixture and a matching v1 compute request.
            handshake_request = {
                "protocolVersion": 1, "requestId": "handshake-cli", "op": "handshake",
                "controllerId": "stage9-velocity", "controllerRevision": "a" * 64,
                "modelSnapshotSha256": "b" * 64, "deterministicSeed": "17",
                "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                                 "sensors": [{"sensorId": "tip-velocity", "unit": "m/s"}],
                                 "actuatorIds": ["tip-force"]},
            }
            compute = self.compute_request(step_id="1", state={}, velocity=0.125)
            input_text = json.dumps(handshake_request) + "\n" + json.dumps(compute) + "\n"
            process = subprocess.run([sys.executable, str(SCRIPT), "--gains", str(gains_path)],
                                     input=input_text, capture_output=True, text=True, check=False)

        self.assertEqual(process.returncode, 0, process.stderr)
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual(len(replies), 2)
        self.assertTrue(replies[0]["ok"])
        self.assertEqual(replies[1]["stepId"], "1")
        self.assertAlmostEqual(replies[1]["commands"][0]["force"], -0.2)


if __name__ == "__main__":
    unittest.main()
