"""Numerical and input-boundary tests for the Stage 9 analysis toolkit."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


SCRIPT = (Path(__file__).resolve().parents[1] / "examples" / "stage9" /
          "stage9_analysis.py")
SPEC = importlib.util.spec_from_file_location("stage9_analysis", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
analysis = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analysis
SPEC.loader.exec_module(analysis)


def model_document(M, C, K, B, Sq, Sv=None, Sa=None, D=None, *,
                   q=None, v=None, a=None, force=None, residual=0.0,
                   frame="global", metadata=None, extension=None):
    M = np.asarray(M, dtype=float)
    C = np.asarray(C, dtype=float)
    K = np.asarray(K, dtype=float)
    B = np.asarray(B, dtype=float)
    Sq = np.asarray(Sq, dtype=float)
    n, m = B.shape
    p = Sq.shape[0]
    Sv = np.zeros((p, n)) if Sv is None else np.asarray(Sv, dtype=float)
    Sa = np.zeros((p, n)) if Sa is None else np.asarray(Sa, dtype=float)
    D = np.zeros((p, m)) if D is None else np.asarray(D, dtype=float)
    directions = {"global": {}, "constraint-local": {"globalDirection": [1.0, 0.0, 0.0]}}
    return {
        "schema": analysis.MODEL_SCHEMA,
        "version": analysis.MODEL_VERSION,
        "M": M.tolist(), "C": C.tolist(), "K": K.tolist(), "B": B.tolist(),
        "Sq": Sq.tolist(), "Sv": Sv.tolist(), "Sa": Sa.tolist(), "D": D.tolist(),
        "operating_point": {
            "q": (np.zeros(n) if q is None else np.asarray(q, dtype=float)).tolist(),
            "v": (np.zeros(n) if v is None else np.asarray(v, dtype=float)).tolist(),
            "a": (np.zeros(n) if a is None else np.asarray(a, dtype=float)).tolist(),
            "force": (np.zeros(n) if force is None else np.asarray(force, dtype=float)).tolist(),
            "residual": residual,
        },
        "dofs": [
            {"nodePhysicalId": f"node-{i}", "nodePosition": i, "dof": "UX",
             "unit": "m", "frame": frame, **directions[frame]}
            for i in range(n)
        ],
        "inputs": [
            {"id": f"force-{j}", "unit": "N", "nodePhysicalId": f"node-{j % n}",
             "nodePosition": j % n}
            for j in range(m)
        ],
        "outputs": [
            {"id": f"output-{j}", "quantity": "Displacement", "unit": "m",
             "nodePhysicalId": f"node-{j % n}", "nodePosition": j % n}
            for j in range(p)
        ],
        **({"metadata": metadata} if metadata is not None else {}),
        **({"vendorExtension": extension} if extension is not None else {}),
    }


class DescriptorAndFrequencyTests(unittest.TestCase):
    def test_descriptor_and_frequency_response_include_acceleration_feedthrough(self) -> None:
        document = model_document(
            [[2.0]], [[0.8]], [[8.0]], [[1.0]], [[3.0]],
            Sv=[[4.0]], Sa=[[0.5]], D=[[0.7]])
        model = analysis.parse_model(document)
        system = analysis.descriptor(model)

        np.testing.assert_allclose(system.E, [[1.0, 0.0], [0.0, 2.0]])
        np.testing.assert_allclose(system.A, [[0.0, 1.0], [-8.0, -0.8]])
        np.testing.assert_allclose(system.B, [[0.0], [1.0]])
        np.testing.assert_allclose(system.C, [[1.0, 3.8]])
        np.testing.assert_allclose(system.D, [[0.95]])

        response = analysis.frequency_response(model, 2.0)
        expected_q = 1.0 / (8.0 - 2.0**2 * 2.0 + 1j * 2.0 * 0.8)
        expected_y = (3.0 + 1j * 2.0 * 4.0 - 2.0**2 * 0.5) * expected_q + 0.7
        self.assertAlmostEqual(response.displacement[0, 0].real, expected_q.real, places=12)
        self.assertAlmostEqual(response.displacement[0, 0].imag, expected_q.imag, places=12)
        self.assertAlmostEqual(response.output[0, 0].real, expected_y.real, places=12)
        self.assertAlmostEqual(response.output[0, 0].imag, expected_y.imag, places=12)

        state = np.linalg.solve(1j * 2.0 * system.E - system.A, system.B)
        descriptor_output = system.C @ state + system.D
        np.testing.assert_allclose(response.output, descriptor_output, rtol=1e-12, atol=1e-12)

    def test_singular_frequency_is_explicit(self) -> None:
        model = analysis.parse_model(model_document(
            [[1.0]], [[0.0]], [[1.0]], [[1.0]], [[1.0]]))
        with self.assertRaisesRegex(analysis.AnalysisError, "singular"):
            analysis.frequency_response(model, 1.0)


class ReductionTests(unittest.TestCase):
    def test_automatic_modal_order_rejects_inaccurate_truncation(self) -> None:
        model = analysis.parse_model(model_document(
            np.eye(2), np.eye(2)*0.1, np.diag([4.0,9.0]),
            [[1.0],[1.0]], [[1.0,1.0]]))
        insufficient = analysis.select_modal_order(model, [0.0,1.0,4.0], 0.01, 1)
        self.assertFalse(insufficient["metTolerance"])
        self.assertGreater(insufficient["maximumRelativeError"], .01)
        selected = analysis.select_modal_order(model, [0.0,1.0,4.0], .01)
        self.assertEqual(selected["modes"], 2)
        self.assertTrue(selected["metTolerance"])
        self.assertLess(selected["maximumRelativeError"], 1e-12)
        with self.assertRaises(analysis.AnalysisError):
            analysis.select_modal_order(model, [], .01)
        clustered = analysis.parse_model(model_document(
            np.eye(2), np.eye(2)*.1, np.eye(2)*4,
            [[1.0],[1.0]], [[1.0,1.0]]))
        selected = analysis.select_modal_order(clustered, [0.0], .01)
        self.assertEqual(selected["modes"], 2)
        self.assertIn("error", selected["attempts"][0])

    def test_modal_reduction_projects_force_and_round_trips_rom(self) -> None:
        document = model_document(
            [[2.0, 0.0], [0.0, 1.0]], [[0.2, 0.0], [0.0, 0.3]],
            [[8.0, 0.0], [0.0, 9.0]], [[1.0], [2.0]], [[1.0, 2.0]],
            Sv=[[0.5, -1.0]], Sa=[[0.1, 0.2]], D=[[0.3]],
            q=[1.0, 2.0], v=[0.2, -0.3], a=[0.4, 0.5], force=[10.0, 20.0],
            residual=2.5, metadata={"producer": "test"}, extension={"tag": "preserved"})
        full = analysis.parse_model(document)
        reduced = analysis.modal_reduce(full, 1)

        self.assertAlmostEqual(float(reduced.basis[:, 0] @ full.M @ reduced.basis[:, 0]),
                               1.0, places=12)
        self.assertLess(reduced.metadata["massOrthonormalityErrorFro"], 1.0e-12)
        self.assertLess(reduced.metadata["generalizedEigenResidualRelative"], 1.0e-12)
        np.testing.assert_allclose(reduced.model.force, reduced.basis.T @ full.force)
        np.testing.assert_allclose(reduced.model.B, reduced.basis.T @ full.B)
        np.testing.assert_allclose(reduced.model.Sa, full.Sa @ reduced.basis)
        self.assertEqual(reduced.model.residual, 2.5)
        self.assertEqual(reduced.model.metadata["reduction"]["operatingPointResidualScope"],
                         "source-full-operating-point")

        restored = analysis.parse_model(analysis.model_to_json(reduced.model))
        self.assertEqual(restored.n, 1)
        np.testing.assert_allclose(restored.force, reduced.basis.T @ full.force)
        self.assertEqual(restored.metadata["producer"], "test")
        self.assertEqual(restored.extensions["vendorExtension"], {"tag": "preserved"})

    def test_craig_bampton_static_compliance_sensor_and_rom_round_trip(self) -> None:
        stiffness = [[2.0, -1.0, 0.0], [-1.0, 2.0, -1.0], [0.0, -1.0, 2.0]]
        document = model_document(
            np.eye(3), np.zeros((3, 3)), stiffness,
            [[1.0], [0.0], [0.0]], [[0.0, 0.0, 1.0]],
            Sa=[[0.0, 1.0, 0.0]], D=[[0.2]], force=[1.0, 2.0, 3.0],
            q=[0.2, 0.1, 0.3], v=[0.4, 0.5, 0.6], a=[0.0, 0.2, 0.4],
            residual=3.75)
        full = analysis.parse_model(document)
        cb_static = analysis.craig_bampton(full, [0], 0)

        # The static constraint mode gives the exact Schur-complement stiffness
        # and tip displacement for a static load on the retained boundary.
        self.assertAlmostEqual(cb_static.model.K[0, 0], 4.0 / 3.0, places=12)
        self.assertAlmostEqual(cb_static.basis[2, 0], 1.0 / 3.0, places=12)
        self.assertAlmostEqual(cb_static.model.Sq[0, 0], 1.0 / 3.0, places=12)
        self.assertLess(cb_static.metadata["staticConstraintResidualRelative"], 1.0e-12)
        self.assertEqual(cb_static.model.residual, 3.75)
        self.assertEqual(cb_static.model.metadata["reduction"]["operatingPointResidualScope"],
                         "source-full-operating-point")
        np.testing.assert_allclose(
            cb_static.model.q,
            np.linalg.solve(cb_static.model.M,
                            cb_static.basis.T @ full.M @ full.q))
        full_static = analysis.frequency_response(full, 0.0)
        cb_response = analysis.frequency_response(cb_static.model, 0.0)
        np.testing.assert_allclose(cb_response.output, full_static.output, rtol=1e-12, atol=1e-12)
        comparison = analysis.compare_models(full, cb_static.model, [0.0])
        for error in comparison["frequencies"][0]["maximumAbsoluteErrorByUnit"].values():
            self.assertLess(error, 1.0e-12)

        cb_dynamic = analysis.craig_bampton(full, [0], 1)
        self.assertEqual(cb_dynamic.model.n, 2)
        restored = analysis.parse_model(analysis.model_to_json(cb_dynamic.model))
        self.assertEqual(restored.n, 2)
        self.assertEqual(restored.dofs[0]["kind"], "generalized-coordinate")
        np.testing.assert_allclose(restored.force, cb_dynamic.basis.T @ full.force)

    def test_modal_and_fixed_interface_cutoffs_do_not_split_repeated_modes(self) -> None:
        model = analysis.parse_model(model_document(
            np.eye(3), np.zeros((3, 3)), np.eye(3),
            [[1.0], [0.0], [0.0]], [[1.0, 0.0, 0.0]]))
        with self.assertRaisesRegex(analysis.AnalysisError, "splits a repeated"):
            analysis.modal_reduce(model, 1)
        with self.assertRaisesRegex(analysis.AnalysisError, "splits a repeated"):
            analysis.craig_bampton(model, [0], 1)


class ValidationAndDiagnosticsTests(unittest.TestCase):
    def test_schema_units_catalog_frames_and_matrix_typos_fail_closed(self) -> None:
        base = model_document([[1.0]], [[0.0]], [[1.0]], [[1.0]], [[1.0]])
        bad = dict(base)
        bad["Sqq"] = [[0.0]]
        with self.assertRaisesRegex(analysis.AnalysisError, "Unknown matrix-like"):
            analysis.parse_model(bad)
        bad = dict(base)
        del bad["Sa"]
        with self.assertRaisesRegex(analysis.AnalysisError, "missing required fields"):
            analysis.parse_model(bad)
        bad = json.loads(json.dumps(base))
        bad["inputs"][0]["unit"] = "N*s"
        with self.assertRaisesRegex(analysis.AnalysisError, "unit must be 'N'"):
            analysis.parse_model(bad)
        bad = json.loads(json.dumps(base))
        bad["outputs"][0]["unit"] = "m/s"
        with self.assertRaisesRegex(analysis.AnalysisError, "quantity/unit"):
            analysis.parse_model(bad)
        bad = json.loads(json.dumps(base))
        bad["operating_point"]["force"] = [1.0, 2.0]
        with self.assertRaisesRegex(analysis.AnalysisError, "vector of length 1"):
            analysis.parse_model(bad)

        native_local = model_document(
            [[1.0]], [[0.0]], [[1.0]], [[1.0]], [[1.0]], frame="constraint-local")
        analysis.parse_model(native_local)
        native_local["dofs"][0]["globalDirection"] = [0.0, 2.0, 0.0]
        with self.assertRaisesRegex(analysis.AnalysisError, "normalized 3-vector"):
            analysis.parse_model(native_local)

    def test_spd_symmetry_and_psd_tolerances_are_scale_aware(self) -> None:
        document = model_document([[1.0, 0.01], [0.0, 1.0]], np.eye(2), np.eye(2),
                                  [[1.0], [0.0]], [[1.0, 0.0]])
        with self.assertRaisesRegex(analysis.AnalysisError, "M must be symmetric"):
            analysis.parse_model(document)
        mixed_scale_mass = [[1.0e20, 0.0, 0.0],
                            [0.0, 1.0e-20, 1.0e-20],
                            [0.0, 0.0, 1.0e-20]]
        document = model_document(mixed_scale_mass, np.eye(3), np.eye(3),
                                  [[1.0], [0.0], [0.0]], [[1.0, 0.0, 0.0]])
        with self.assertRaisesRegex(analysis.AnalysisError, "M must be symmetric"):
            analysis.parse_model(document)
        document = model_document([[1.0]], [[0.0]], [[-1.0e-30]], [[1.0]], [[1.0]])
        with self.assertRaisesRegex(analysis.AnalysisError, "K must be positive semidefinite"):
            analysis.parse_model(document)
        document = model_document([[1.0]], [[0.0]], [[1.0]], [[1.0]], [[math.nan]])
        with self.assertRaisesRegex(analysis.AnalysisError, "non-finite"):
            analysis.parse_model(document)

    def test_pbh_diagnostic_detects_uncontrollable_close_and_repeated_modes(self) -> None:
        for second_stiffness in (1.0, 1.0 + 1.0e-9):
            model = analysis.parse_model(model_document(
                np.eye(2), np.zeros((2, 2)), np.diag([1.0, second_stiffness]),
                [[1.0], [0.0]], [[1.0, 0.0]]))
            result = analysis.diagnostics(model)
            control = result["controllability"]
            self.assertFalse(control["fullRank"])
            self.assertLess(control["minimumPbhRank"], result["stateDimension"])
            self.assertEqual(result["operatingPointResidualScope"], "source-full-operating-point")
            if second_stiffness == 1.0:
                self.assertTrue(any(mode["multiplicity"] >= 2 for mode in control["modes"]))


class CliReportTests(unittest.TestCase):
    def test_auto_selection_unmet_tolerance_has_failure_status_and_evidence(self) -> None:
        document=model_document(np.eye(2),np.eye(2)*.1,np.diag([4.,9.]),[[1.],[1.]],[[1.,1.]])
        with tempfile.TemporaryDirectory() as temporary:
            model_path=Path(temporary)/"model.json"
            output_path=Path(temporary)/"report.json"
            model_path.write_text(json.dumps(document),encoding="utf-8")
            status=analysis.main(["--model",str(model_path),"--output",str(output_path),
                "--frequencies-json","[0,1,4]","--auto-modal-relative-tolerance",".01","--max-auto-modes","1"])
            self.assertEqual(status,1)
            report=json.loads(output_path.read_text(encoding="utf-8"))
            self.assertFalse(report["modalSelection"]["metTolerance"])
            self.assertEqual(report["errors"][-1]["stage"],"modalSelection")

    def test_cli_writes_bounded_atomic_report_with_model_hash(self) -> None:
        document = model_document([[2.0]], [[0.2]], [[8.0]], [[1.0]], [[1.0]])
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            model_path = folder / "model.json"
            output_path = folder / "analysis.json"
            model_path.write_text(json.dumps(document), encoding="utf-8")
            status = analysis.main([
                "--model", str(model_path), "--output", str(output_path),
                "--modes", "1", "--frequencies-json", "[0, 1]"])
            self.assertEqual(status, 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], analysis.REPORT_SCHEMA)
            self.assertEqual(report["provenance"]["modelSha256"],
                             hashlib.sha256(model_path.read_bytes()).hexdigest())
            self.assertEqual(len(report["frequencyResponse"]["full"]), 2)
            self.assertIn("modal", report["reductions"])
            self.assertFalse(list(folder.glob(".analysis.json.*.tmp")))

    def test_analysis_report_caps_frequency_channel_expansion(self) -> None:
        model = analysis.parse_model(model_document(
            [[1.0]], [[0.1]], [[2.0]], np.ones((1, 64)), np.ones((64, 1)),
            D=np.zeros((64, 64))))
        with self.assertRaisesRegex(analysis.AnalysisError, "FRF report exceeds"):
            analysis.analyze_model(model, frequencies=[0.0] * 157)


if __name__ == "__main__":
    unittest.main()
