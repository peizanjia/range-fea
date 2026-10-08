"""Independent source-derived matrices, ports, and initial-state mapping."""
from __future__ import annotations

import importlib.util
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.source_reference import SourceReferenceError, build_source_reference


def node(position, physical_id, coordinates, *, displacement=None, velocity=None,
         rotation=None, angular_velocity=None):
    return {"position": position, "physicalId": physical_id, "coordinates": coordinates,
            "initialDisplacementM": displacement or [0.0, 0.0, 0.0],
            "initialVelocityMS": velocity or [0.0, 0.0, 0.0],
            "initialRotationRad": rotation or [0.0, 0.0, 0.0],
            "initialAngularVelocityRadS": angular_velocity or [0.0, 0.0, 0.0]}


def source_port(node_id, direction, *, force_id="source-tip-force", velocity_id="source-tip-velocity"):
    return {"force": {"id": force_id, "nodePhysicalId": node_id, "globalDirection": direction,
                       "unit": "N", "kind": "nodal_force"},
            "velocity": {"id": velocity_id, "nodePhysicalId": node_id, "globalDirection": direction,
                          "unit": "m/s", "kind": "velocity"}}


def rod_case():
    descriptor = {
        "status": "supported", "sourceSha256": "a" * 64, "modelKind": "four-rod",
        "nodes": [node(0, 101, [0.0, 0.0, 0.0]),
                  node(1, 205, [2.0, 0.0, 0.0], displacement=[0.012, 0.0, 0.0],
                       velocity=[0.034, 0.0, 0.0]),
                  node(2, 302, [5.0, 0.0, 0.0], displacement=[0.020, 0.0, 0.0],
                       velocity=[0.056, 0.0, 0.0])],
        "elements": [{"type": "truss1", "physicalId": 900, "nodePhysicalIds": [101, 205],
                      "material": {"youngModulusPa": 200.0, "densityKgM3": 3.0},
                      "section": {"areaM2": 0.5}},
                     {"type": "truss1", "physicalId": 901, "nodePhysicalIds": [205, 302],
                      "material": {"youngModulusPa": 100.0, "densityKgM3": 2.0},
                      "section": {"areaM2": 0.25}}],
        "freeDofs": [{"nodePhysicalId": 205, "physicalDof": "UZ",
                      "globalDirection": [1.0, 0.0, 0.0], "unit": "m"},
                     {"nodePhysicalId": 302, "physicalDof": "UZ",
                      "globalDirection": [1.0, 0.0, 0.0], "unit": "m"}],
        "ports": source_port(302, [1.0, 0.0, 0.0]),
    }
    linear = {"dofs": [{"nodePhysicalId": "302", "nodePosition": 2, "dof": "UZ",
                        "frame": "constraint-local", "globalDirection": [1.0, 0.0, 0.0], "unit": "m"},
                       {"nodePhysicalId": "205", "nodePosition": 1, "dof": "UZ",
                        "frame": "constraint-local", "globalDirection": [1.0, 0.0, 0.0], "unit": "m"}],
              "M": [[0.5, 0.25], [0.25, 1.5]],
              "K": [[25.0 / 3.0, -25.0 / 3.0], [-25.0 / 3.0, 175.0 / 3.0]],
              "B": [[1.0], [0.0]], "Sv": [[1.0, 0.0]],
              "inputs": [{"id": "source-tip-force", "nodePhysicalId": "302", "unit": "N"}],
              "outputs": [{"id": "source-tip-velocity", "nodePhysicalId": "302", "quantity": "Velocity",
                           "unit": "m/s"}]}
    return descriptor, linear


def beam_case():
    descriptor = {
        "status": "supported", "sourceSha256": "b" * 64, "modelKind": "cantilever-beam",
        "nodes": [node(0, 12, [0.0, 0.0, 0.0]),
                  node(1, 97, [2.0, 0.0, 0.0], displacement=[0.0, 0.003, 0.0],
                       velocity=[0.0, 0.005, 0.0], rotation=[0.0, 0.0, 0.004],
                       angular_velocity=[0.0, 0.0, 0.006])],
        "elements": [{"type": "beam1", "physicalId": 311, "nodePhysicalIds": [12, 97],
                      "material": {"youngModulusPa": 200.0, "densityKgM3": 2.5},
                      "section": {"areaM2": 0.4, "iyM4": 0.3, "izM4": 0.5, "torsionM4": 0.2}}],
        "freeDofs": [{"nodePhysicalId": 97, "physicalDof": "UY",
                      "globalDirection": [0.0, 1.0, 0.0], "unit": "m"},
                     {"nodePhysicalId": 97, "physicalDof": "RZ",
                      "globalDirection": [0.0, 0.0, 1.0], "unit": "rad"}],
        "ports": source_port(97, [0.0, 1.0, 0.0]),
    }
    linear = {"dofs": [{"nodePhysicalId": "97", "nodePosition": 1, "dof": "RZ", "frame": "global",
                        "globalDirection": [0.0, 0.0, 1.0], "unit": "rad"},
                       {"nodePhysicalId": "97", "nodePosition": 1, "dof": "UY", "frame": "global",
                        "globalDirection": [0.0, 1.0, 0.0], "unit": "m"}],
              "M": [[16.0 / 210.0, -44.0 / 210.0], [-44.0 / 210.0, 156.0 / 210.0]],
              "K": [[200.0, -150.0], [-150.0, 150.0]],
              "B": [[0.0], [1.0]], "Sv": [[0.0, 1.0]],
              "inputs": [{"id": "source-tip-force", "nodePhysicalId": "97", "unit": "N"}],
              "outputs": [{"id": "source-tip-velocity", "nodePhysicalId": "97", "quantity": "Velocity",
                           "unit": "m/s"}]}
    return descriptor, linear


class SourceReferenceTest(unittest.TestCase):
    def test_legacy_template_resume_reports_missing_provenance_without_rewriting_result(self):
        from range_workflow.jobs import run_native_workflow
        from range_workflow.service import WorkflowError
        legacy = {"configuration": {"model": "four-rod"}, "referencePath": "legacy-reference.json"}
        service = mock.Mock()
        service.result_snapshot.return_value = legacy
        with self.assertRaises(WorkflowError) as caught:
            run_native_workflow(service, {"command": "workflow.resume", "parameters": {"jobId": "old"}},
                                Path("unused"), mock.Mock(), {})
        self.assertEqual(caught.exception.code, "SOURCE_REFERENCE_UNAVAILABLE")
        self.assertEqual(legacy, {"configuration": {"model": "four-rod"}, "referencePath": "legacy-reference.json"})

    def test_internal_positions_can_be_permuted_independently_of_geometry_order(self):
        for factory, permutation in ((rod_case, [2, 0, 1]), (beam_case, [1, 0])):
            descriptor, linear = factory()
            expected = build_source_reference(descriptor, linear)
            positions = {}
            for record, position in zip(descriptor["nodes"], permutation):
                record["position"] = position
                positions[str(record["physicalId"])] = position
            for record in linear["dofs"]:
                record["nodePosition"] = positions[record["nodePhysicalId"]]
            actual = build_source_reference(descriptor, linear)
            self.assertEqual(actual["initialQ"], expected["initialQ"])
            self.assertEqual(actual["initialV"], expected["initialV"])
            self.assertEqual(actual["analyticalM"], expected["analyticalM"])
            self.assertEqual(actual["analyticalK"], expected["analyticalK"])
            linear["dofs"][0]["nodePosition"] = len(permutation)
            with self.assertRaisesRegex(SourceReferenceError, "physical ID"):
                build_source_reference(descriptor, linear)

    def test_rod_uses_actual_local_axis_mapping_geometry_and_initial_state(self):
        descriptor, linear = rod_case()
        result = build_source_reference(descriptor, linear)

        self.assertEqual(result["initialQ"], [0.020, 0.012])
        self.assertEqual(result["initialV"], [0.056, 0.034])
        np.testing.assert_allclose(result["analyticalM"], linear["M"], rtol=0.0, atol=1.0e-14)
        np.testing.assert_allclose(result["analyticalK"], linear["K"], rtol=0.0, atol=1.0e-14)
        self.assertEqual(result["controller"], {"sensorId": "source-tip-velocity",
                                                 "actuatorId": "source-tip-force"})

    def test_planar_beam_uses_consistent_euler_bernoulli_bending_block(self):
        descriptor, linear = beam_case()
        result = build_source_reference(descriptor, linear)

        self.assertEqual(result["initialQ"], [0.004, 0.003])
        self.assertEqual(result["initialV"], [0.006, 0.005])
        np.testing.assert_allclose(result["analyticalM"], linear["M"], rtol=0.0, atol=1.0e-14)
        np.testing.assert_allclose(result["analyticalK"], linear["K"], rtol=0.0, atol=1.0e-14)
        self.assertEqual(result["analysis"]["boundaryIndices"], [1])
        self.assertEqual(result["analysis"]["modes"], 1)

    def test_native_matrix_mismatch_fails_before_reference_replay(self):
        descriptor, linear = rod_case()
        linear["M"][0][0] = 1.25
        with self.assertRaisesRegex(SourceReferenceError, "mass matrix"):
            build_source_reference(descriptor, linear)

    def test_port_coefficients_are_independently_signed_unit_projections_even_for_zero_initial_state(self):
        for zero_initial in (False, True):
            descriptor, linear = rod_case()
            if zero_initial:
                for item in descriptor["nodes"]:
                    item["initialDisplacementM"] = item["initialVelocityMS"] = [0.0, 0.0, 0.0]
            for matrix, value in (("B", -1.0), ("B", -2.0), ("B", 2.0),
                                  ("Sv", -1.0), ("Sv", -3.0), ("Sv", 3.0)):
                broken = copy.deepcopy(linear)
                broken[matrix][0][0] = value
                with self.subTest(matrix=matrix, value=value, zero_initial=zero_initial):
                    with self.assertRaisesRegex(SourceReferenceError, "coefficients"):
                        build_source_reference(descriptor, broken)
            # A physically reversed force/sensor axis legitimately requires -1.
            for port in descriptor["ports"].values():
                port["globalDirection"] = [-1.0, 0.0, 0.0]
            linear["B"][0][0] = linear["Sv"][0][0] = -1.0
            build_source_reference(descriptor, linear)

    def test_replay_accepts_initial_velocity_and_keeps_zero_default(self):
        script = (Path(__file__).resolve().parents[2] / "fea-solver" / "examples" / "stage9" /
                  "stage9_velocity_controller.py")
        spec = importlib.util.spec_from_file_location("stage9_velocity_controller_source_reference", script)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        controller = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(controller)
        model = {"schema": "range-fea-linear-model", "version": 1,
                 "source": {"kind": "linear-elastic-FE", "units": "SI",
                            "coordinateMeaning": "perturbations about static equilibrium"},
                 "dofs": [{"nodePhysicalId": "tip", "nodePosition": 0, "dof": "UX", "unit": "m",
                           "frame": "global", "globalDirection": [1.0, 0.0, 0.0]}],
                 "inputs": [{"id": "force", "unit": "N", "nodePhysicalId": "tip", "nodePosition": 0}],
                 "outputs": [{"id": "velocity", "quantity": "Velocity", "unit": "m/s",
                              "nodePhysicalId": "tip", "nodePosition": 0}],
                 "operating_point": {"q": [0.0], "v": [0.0], "a": [0.0], "force": [0.0], "residual": 0.0},
                 "M": [[2.0]], "C": [[0.8]], "K": [[8.0]], "B": [[1.0]],
                 "Sq": [[0.0]], "Sv": [[1.0]], "Sa": [[0.0]], "D": [[0.0]]}
        with tempfile.TemporaryDirectory(prefix="range-initial-velocity-") as temporary:
            folder = Path(temporary)
            model_path = folder / "linear.json"
            gains_path = folder / "gains.json"
            model_path.write_text(json.dumps(model), encoding="utf-8")
            gains = controller.design_gain(model_path, mode=0, target_damping=0.3,
                                           sensor_id="velocity", actuator_id="force")
            gains_path.write_text(json.dumps(gains), encoding="utf-8")
            replay_path = folder / "replay.json"
            subprocess.run([sys.executable, str(script), "--replay-model", str(model_path),
                            "--gains", str(gains_path), "--dt", "0.01", "--steps", "2",
                            "--initial-q-json", "[0.001]", "--initial-v-json", "[0.05]",
                            "--output", str(replay_path)], check=True, capture_output=True)
            self.assertEqual(json.loads(replay_path.read_text(encoding="utf-8"))["initialV"], [0.05])

            default_path = folder / "default-replay.json"
            subprocess.run([sys.executable, str(script), "--replay-model", str(model_path),
                            "--gains", str(gains_path), "--dt", "0.01", "--steps", "2",
                            "--initial-q-json", "[0.001]", "--output", str(default_path)],
                           check=True, capture_output=True)
            self.assertEqual(json.loads(default_path.read_text(encoding="utf-8"))["initialV"], [0.0])


if __name__ == "__main__":
    unittest.main()
