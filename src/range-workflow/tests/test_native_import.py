"""WF-03 raw native model describe protocol and rejection coverage."""
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

NATIVE, FIXTURE = map(Path, sys.argv[1:3])
sys.argv[1:3] = []


class NativeImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-native-import-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self, kind, binary=False):
        path = self.directory / f"{kind}.{'rbm' if binary else 'ram'}"
        result = subprocess.run([str(FIXTURE), kind, str(path)], capture_output=True,
                                text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue(path.is_file())
        return path

    def describe(self, path, expected_sha=None, success=True):
        actual_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        result = subprocess.run(
            [str(NATIVE), "describe", "--input", str(path), "--expected-sha",
             expected_sha if expected_sha is not None else actual_sha],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stderr + result.stdout)
        response = json.loads(result.stdout)
        self.assertEqual(response["apiVersion"], "range.native-import/v1")
        return response

    def prepare_source(self, source, output, steps=16, dt=0.004, output_frequency=None, passive=False,
                       expected_sha=None, success=True):
        actual_sha = hashlib.sha256(source.read_bytes()).hexdigest()
        command = [str(NATIVE), "prepare-source", "--input", str(source),
                   "--expected-sha", expected_sha if expected_sha is not None else actual_sha,
                   "--output", str(output), "--steps", str(steps), "--dt", str(dt)]
        if output_frequency is not None:
            command.extend(["--output-frequency", str(output_frequency)])
        if passive:
            command.append("--passive")
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stderr + result.stdout)
        response = json.loads(result.stdout)
        self.assertEqual(response["apiVersion"], "range.workflow.native/v1")
        self.assertEqual(response["operation"], "prepare-source")
        return response

    def prepare_legacy(self, source, output, steps=12, dt=0.005, output_frequency=None):
        command = [str(NATIVE), "prepare", "--input", str(source), "--output", str(output),
                   "--steps", str(steps), "--dt", str(dt), "--passive"]
        if output_frequency is not None:
            command.extend(["--output-frequency", str(output_frequency)])
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def test_four_rod_reports_raw_nondefault_values_and_physical_ids(self):
        source = self.fixture("rod")
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        response = self.describe(source)
        self.assertEqual(response["status"], "supported")
        self.assertEqual(response["sourceSha256"], before)
        self.assertEqual(response["modelKind"], "four-rod")
        self.assertEqual([node["physicalId"] for node in response["nodes"]],
                         [4001, 87, 625, 1009, 51000])
        self.assertEqual(response["nodes"][2]["coordinates"], [1.15, 2.5, -0.75])
        self.assertEqual(response["nodes"][1]["initialDisplacementM"], [0.0017, 0.0, 0.0])
        self.assertEqual(response["nodes"][2]["initialVelocityMS"], [-0.023, 0.0, 0.0])
        self.assertEqual(len(response["elements"]), 4)
        self.assertEqual(response["elements"][0]["material"], {
            "youngModulusPa": 3.2e7, "densityKgM3": 840.0, "poissonRatio": 0.29})
        self.assertEqual(response["elements"][0]["section"]["areaM2"], 0.0065)
        self.assertEqual(len(response["pointElements"]), 6)
        self.assertEqual([dof["nodePhysicalId"] for dof in response["freeDofs"]
                          if dof["physicalDof"] == "UX"], [87, 625, 1009, 51000])
        self.assertTrue(all(item["value"] == 0.0 for item in response["supports"]))
        self.assertEqual(response["ports"]["force"]["nodePhysicalId"], 51000)
        self.assertEqual(response["ports"]["force"]["globalDirection"], [1, 0, 0])
        self.assertEqual(response["dynamics"]["timeStepSeconds"], 0.0125)
        self.assertEqual(response["originalControl"]["deterministicSeed"], "0")
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)
        self.assertEqual(sorted(path.name for path in self.directory.iterdir()), ["rod.ram"])

    def test_beam_reports_all_four_initial_condition_fields_and_free_dofs(self):
        # Verify the independently derived tube geometry without ASCII MODEL
        # serialization rounding the declared inertia and recovery moduli.
        source = self.fixture("beam", binary=True)
        response = self.describe(source)
        self.assertEqual(response["status"], "supported")
        self.assertEqual(response["modelKind"], "cantilever-beam")
        self.assertEqual([node["physicalId"] for node in response["nodes"]], [4093, 117])
        self.assertEqual([node["coordinates"] for node in response["nodes"]],
                         [[0.0, 1.25, -0.5], [3.3, 1.25, -0.5]])
        tip = response["nodes"][1]
        self.assertEqual(tip["initialDisplacementM"], [0.0, 0.0042, 0.0])
        self.assertEqual(tip["initialVelocityMS"], [0.0, -0.031, 0.0])
        self.assertEqual(tip["initialRotationRad"], [0.0, 0.0, 0.014])
        self.assertEqual(tip["initialAngularVelocityRadS"], [0.0, 0.0, 0.63])
        self.assertEqual(len(response["pointElements"]), 2)
        self.assertEqual({(item["nodePhysicalId"], item["physicalDof"])
                          for item in response["freeDofs"]}, {(117, "UY"), (117, "RZ")})
        area, outer_radius = 0.007, 0.08
        inner_squared = outer_radius**2 - area / math.pi
        inertia = area * (outer_radius**2 + inner_squared) / 4
        expected_section = {"areaM2": area, "iyM4": inertia, "izM4": inertia,
                            "torsionM4": 2 * inertia, "wyM3": inertia / outer_radius,
                            "wzM3": inertia / outer_radius, "wtM3": 2 * inertia / outer_radius}
        section = response["elements"][0]["section"]
        self.assertEqual(section.keys(), expected_section.keys())
        for field, expected in expected_section.items():
            self.assertTrue(math.isclose(section[field], expected, rel_tol=1e-14, abs_tol=0.0), field)
        self.assertEqual(response["ports"]["velocity"]["globalDirection"], [0, 1, 0])

    def test_hash_mismatch_is_a_structured_rejection(self):
        source = self.fixture("rod")
        response = self.describe(source, "0" * 64, success=False)
        self.assertEqual(response["status"], "rejected")
        self.assertIn({"code": "SOURCE_SHA256_MISMATCH", "severity": "error",
                       "message": "Input bytes do not match the caller's expected SHA-256.",
                       "path": "/expectedSha256"}, response["diagnostics"])

    def test_permuted_storage_preserves_physical_identity_and_actual_native_position(self):
        for kind, positions, tip_id in (("rod", [2, 4, 1, 3, 0], 51000),
                                        ("beam", [1, 0], 117)):
            with self.subTest(kind=kind):
                baseline = self.describe(self.fixture(kind))
                source = self.fixture(f"{kind}-permuted")
                original = source.read_bytes()
                description = self.describe(source)
                self.assertEqual([node["position"] for node in description["nodes"]], positions)
                # Geometry, ICs, elements and supports must keep the same physical identity.
                self.assertEqual([{key: value for key, value in node.items() if key != "position"}
                                  for node in description["nodes"]],
                                 [{key: value for key, value in node.items() if key != "position"}
                                  for node in baseline["nodes"]])
                for field in ("elements", "pointElements", "supports", "freeDofs", "ports"):
                    self.assertEqual(description[field], baseline[field])
                output = self.directory / f"{kind}-permuted-prepared.ram"
                prepared = self.prepare_source(source, output)
                actuator = prepared["mapping"]["actuators"][0]
                self.assertEqual(actuator["targetNodePosition"], positions[-1])
                self.assertEqual(actuator["targetNodePhysicalId"], tip_id)
                self.assertTrue(all(sensor["targetNodePosition"] == positions[-1] and
                                    sensor["targetNodePhysicalId"] == tip_id
                                    for sensor in prepared["mapping"]["sensors"]))
                self.assertEqual(self.describe(output)["nodes"], description["nodes"])
                self.assertEqual(source.read_bytes(), original)

    def test_saved_kinematic_results_cannot_be_registered_or_prepared_as_fresh_state(self):
        for quantity in ("displacement", "rotation", "velocity", "angular-velocity"):
            with self.subTest(quantity=quantity):
                source = self.fixture(f"beam-saved-{quantity}")
                original = source.read_bytes()
                rejected = self.describe(source, success=False)
                self.assertIn("SAVED_INITIAL_STATE_PRESENT",
                              {item["code"] for item in rejected["diagnostics"]})
                output = self.directory / f"saved-{quantity}-prepared.ram"
                prepared = self.prepare_source(source, output, success=False)
                self.assertIn("SAVED_INITIAL_STATE_PRESENT",
                              {item["code"] for item in prepared["diagnostics"]})
                self.assertFalse(output.exists())
                self.assertEqual(source.read_bytes(), original)

    def test_unsupported_geometry_supports_and_loads_have_stable_diagnostics(self):
        cases = {
            "rod-rotated": "UNSUPPORTED_GEOMETRY",
            "rod-support-conflict": "NONZERO_SUPPORT",
            "rod-force": "NONZERO_EXTERNAL_FORCE",
            "rod-gravity-x": "NONZERO_ENVIRONMENT_LOAD",
            "beam-support-conflict": "UNSUPPORTED_SUPPORT_PATTERN",
            "beam-gravity-y": "NONZERO_ENVIRONMENT_LOAD",
        }
        for fixture_kind, expected_code in cases.items():
            with self.subTest(fixture=fixture_kind):
                response = self.describe(self.fixture(fixture_kind), success=False)
                self.assertEqual(response["status"], "rejected")
                self.assertIn(expected_code, {item["code"] for item in response["diagnostics"]})
                self.assertTrue(all(item["path"].startswith("/")
                                    for item in response["diagnostics"]))

    def test_missing_or_zero_beam_recovery_moduli_are_rejected_before_preparation(self):
        for kind, fields in (("beam-modulus-missing", {"wyM3", "wzM3", "wtM3"}),
                             ("beam-modulus-wy-zero", {"wyM3"}),
                             ("beam-modulus-wz-zero", {"wzM3"}),
                             ("beam-modulus-wt-zero", {"wtM3"})):
            with self.subTest(kind=kind):
                source = self.fixture(kind)
                before = source.read_bytes()
                rejected = self.describe(source, success=False)
                diagnostics = [item for item in rejected["diagnostics"]
                               if item["code"] == "INVALID_SECTION_PROPERTY"]
                self.assertEqual({item["path"].rsplit("/", 1)[-1] for item in diagnostics}, fields)
                output = self.directory / f"{kind}-prepared.ram"
                response = self.prepare_source(source, output, success=False)
                self.assertIn("INVALID_SECTION_PROPERTY", {item["code"] for item in response["diagnostics"]})
                self.assertFalse(output.exists())
                self.assertEqual(source.read_bytes(), before)

    def test_prepare_source_rebuilds_mappings_on_a_hash_bound_private_copy(self):
        source = self.fixture("rod-controlled")
        original = source.read_bytes()
        source_sha = hashlib.sha256(original).hexdigest()
        output = self.directory / "prepared.ram"
        response = self.prepare_source(source, output, steps=19, dt=0.006)
        self.assertEqual(response["status"], "published")
        self.assertEqual(response["sourceSha256"], source_sha)
        self.assertEqual(response["preparedSha256"], response["sha256"])
        self.assertEqual(response["preparedSha256"], hashlib.sha256(output.read_bytes()).hexdigest())
        self.assertEqual(Path(response["sourcePath"]).resolve(), source.resolve())
        self.assertEqual(len(response["sourceFiles"]), 1)
        self.assertEqual(Path(response["sourceFiles"][0]["path"]).resolve(), source.resolve())
        self.assertEqual(response["sourceFiles"][0]["sha256"], source_sha)
        self.assertFalse(response["resume"])
        self.assertEqual(response["outputFrequency"], 1)
        self.assertEqual(response["modelKind"], "four-rod")
        mapping = response["mapping"]
        self.assertEqual(len(mapping["actuators"]), 1)
        self.assertEqual(mapping["actuators"][0]["id"], "source-tip-force")
        self.assertEqual(mapping["actuators"][0]["targetNodePhysicalId"], 51000)
        self.assertEqual(mapping["actuators"][0]["targetNodePosition"], 4)
        self.assertEqual(mapping["actuators"][0]["globalDirection"], [1, 0, 0])
        self.assertEqual(mapping["actuators"][0]["unit"], "N")
        self.assertEqual(mapping["actuators"][0]["minimum"], -1.0e6)
        self.assertEqual(mapping["actuators"][0]["maximum"], 1.0e6)
        approved_port = self.describe(source)["ports"]["force"]
        self.assertEqual(approved_port["minimum"], mapping["actuators"][0]["minimum"])
        self.assertEqual(approved_port["maximum"], mapping["actuators"][0]["maximum"])
        self.assertEqual([sensor["id"] for sensor in mapping["sensors"]], [
            "source-tip-displacement", "source-tip-velocity", "source-tip-acceleration"])
        self.assertTrue(all(sensor["targetNodePhysicalId"] == 51000 for sensor in mapping["sensors"]))
        self.assertEqual(mapping["controller"], {
            "enabled": True, "controlProtocolVersion": 1,
            "controllerId": "stage9-velocity-feedback", "deterministicSeed": "41",
            "conditioningCount": 0})
        prepared = self.describe(output, response["preparedSha256"])
        self.assertEqual(prepared["status"], "supported")
        self.assertEqual(prepared["originalControl"]["controllerId"], "stage9-velocity-feedback")
        self.assertEqual(prepared["originalControl"]["deterministicSeed"], "41")
        self.assertEqual(prepared["dynamics"]["steps"], 19)
        self.assertEqual(prepared["dynamics"]["timeStepSeconds"], 0.006)
        self.assertEqual(prepared["dynamics"]["outputFrequency"], 1)
        self.assertEqual(source.read_bytes(), original)
        source_description = self.describe(source, source_sha)
        self.assertEqual(source_description["originalControl"]["controllerId"], "source-controller")
        self.assertEqual(source_description["originalControl"]["deterministicSeed"], "41")

    def test_prepare_source_passive_clears_source_controller_and_stale_hash_cannot_publish(self):
        source = self.fixture("rod-controlled")
        original = source.read_bytes()
        source_sha = hashlib.sha256(original).hexdigest()
        passive_output = self.directory / "passive.ram"
        response = self.prepare_source(source, passive_output, steps=11, dt=0.003, passive=True)
        self.assertEqual(response["status"], "published")
        self.assertTrue(response["passive"])
        self.assertEqual(response["mapping"]["actuators"], [])
        self.assertEqual(response["mapping"]["sensors"], [])
        self.assertFalse(response["mapping"]["controller"]["enabled"])
        passive = self.describe(passive_output, response["preparedSha256"])
        self.assertEqual(passive["dynamics"]["outputFrequency"], 1)
        self.assertFalse(passive["originalControl"]["enabled"])
        self.assertEqual(passive["originalControl"]["controllerId"], "")
        self.assertEqual(source.read_bytes(), original)

        stale_output = self.directory / "stale.ram"
        source.write_bytes(original + b"stale")
        stale = self.prepare_source(source, stale_output, expected_sha=source_sha, success=False)
        self.assertEqual(stale["status"], "rejected")
        self.assertIn("SOURCE_SHA256_MISMATCH", {item["code"] for item in stale["diagnostics"]})
        self.assertFalse(stale_output.exists())

    def test_prepare_source_beam_uses_actual_nondefault_tip_id_and_global_y(self):
        source = self.fixture("beam")
        source_bytes = source.read_bytes()
        source_sha = hashlib.sha256(source_bytes).hexdigest()
        output = self.directory / "beam-prepared.ram"
        response = self.prepare_source(source, output, steps=7, dt=0.0025, output_frequency=2)
        self.assertEqual(response["status"], "published")
        self.assertEqual(response["modelKind"], "cantilever-beam")
        self.assertEqual(response["sourceSha256"], source_sha)
        self.assertEqual(response["outputFrequency"], 2)
        self.assertEqual(response["mapping"]["actuators"][0]["id"], "source-tip-force")
        self.assertEqual(response["mapping"]["actuators"][0]["targetNodePhysicalId"], 117)
        self.assertEqual(response["mapping"]["actuators"][0]["globalDirection"], [0, 1, 0])
        self.assertEqual(response["mapping"]["actuators"][0]["variable"], "var-force_y")
        self.assertTrue(all(sensor["targetNodePhysicalId"] == 117 and
                            sensor["globalDirection"] == [0, 1, 0]
                            for sensor in response["mapping"]["sensors"]))
        prepared = self.describe(output, response["preparedSha256"])
        self.assertEqual(prepared["dynamics"]["outputFrequency"], 2)
        self.assertEqual(source.read_bytes(), source_bytes)

    def test_legacy_prepare_frequency_flag_and_default_remain_compatible(self):
        source = self.fixture("rod")
        explicit_output = self.directory / "legacy-explicit.ram"
        explicit = self.prepare_legacy(source, explicit_output, steps=12, output_frequency=1)
        self.assertEqual(explicit["outputFrequency"], 1)
        self.assertEqual(self.describe(explicit_output)["dynamics"]["outputFrequency"], 1)

        default_output = self.directory / "legacy-default.ram"
        default = self.prepare_legacy(source, default_output, steps=12)
        self.assertEqual(default["outputFrequency"], 12)
        self.assertEqual(self.describe(default_output)["dynamics"]["outputFrequency"], 12)


if __name__ == "__main__":
    unittest.main()
