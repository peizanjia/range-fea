"""Independent contract checks for control profiles, without native solver claims."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.control_profiles import (
    catalog_digest, load_catalog, validate_device, validate_material,
)


def diagonal(size, value):
    return [[value if i == j else 0 for j in range(size)] for i in range(size)]


class ControlProfilesTest(unittest.TestCase):
    def setUp(self):
        self.catalog = load_catalog()
        self.materials = self.catalog["materials"]
        self.piezo = copy.deepcopy(self.materials[1])
        self.rod = copy.deepcopy(self.catalog["devices"][0])

    def load_value(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            return load_catalog(path)

    def test_default_examples_are_unbound_and_synthetic(self):
        self.assertEqual(len(self.materials), 2)
        self.assertEqual(len(self.catalog["devices"]), 10)
        for profile in self.materials + self.catalog["devices"]:
            self.assertEqual(profile["provenance"]["kind"], "example")
            self.assertNotIn("solverSupported", profile)
        for device in self.catalog["devices"]:
            self.assertEqual(device["state"], "ready-for-binding")
        self.assertIn("no hardware calibration", self.catalog["metadata"]["qualification"])

    def test_strain_charge_conversion_matches_hand_computation(self):
        # cE=4I, d row=[0.5,0.25,0,...] => e=[2,1,0,...],
        # d*cE*dT[0,0]=1.25; off diagonal with row [0,0.5,...]=0.5.
        strain = copy.deepcopy(self.piezo)
        strain["properties"] = {"formulation": "strain-charge", "rho_kg_m3": 2,
            "cE_Pa": diagonal(6, 4),
            "d_C_N": [[0.5, 0.25, 0, 0, 0, 0], [0, 0.5, 0, 0, 0, 0], [0, 0, 0.25, 0, 0, 0]],
            "epsilonT_F_m": [[3.25, 0.5, 0], [0.5, 3, 0], [0, 0, 2.25]]}
        stress = copy.deepcopy(strain)
        stress["properties"] = {"formulation": "stress-charge", "rho_kg_m3": 2,
            "cE_Pa": diagonal(6, 4), "e_C_m2": [[2, 1, 0, 0, 0, 0], [0, 2, 0, 0, 0, 0], [0, 0, 1, 0, 0, 0]],
            "epsilonS_F_m": diagonal(3, 2)}
        self.assertEqual(validate_material(strain), validate_material(stress))

    def test_conversion_rejects_nonpositive_epsilon_s(self):
        p = self.piezo["properties"]
        p.pop("e_C_m2")
        p.pop("epsilonS_F_m")
        p.update(formulation="strain-charge", cE_Pa=diagonal(6, 4),
                 d_C_N=[[1, 0, 0, 0, 0, 0], [0]*6, [0]*6], epsilonT_F_m=diagonal(3, 1))
        with self.assertRaisesRegex(ValueError, "positive definite"):
            validate_material(self.piezo)

    def test_conversion_preserves_symmetry_near_cancellation(self):
        self.piezo["properties"] = {"formulation": "strain-charge", "rho_kg_m3": 7500,
            "cE_Pa": diagonal(6, 1e11),
            "d_C_N": [[1e-10, 1e-10, 0, 0, 0, 0],
                      [1e-10, 1.1e-10, 0, 0, 0, 0], [0, 0, 1e-10, 0, 0, 0]],
            "epsilonT_F_m": [[1.2e-8, 2.1e-9, 0], [2.1e-9, 1.221e-8, 0], [0, 0, 1.1e-8]]}
        epsilon = validate_material(self.piezo)["properties"]["epsilonS_F_m"]
        for i in range(3):
            for j in range(3):
                self.assertAlmostEqual(epsilon[i][j], 1e-8 if i == j else 0, delta=1e-23)
                self.assertEqual(epsilon[i][j], epsilon[j][i])

    def test_conversion_overflow_is_rejected(self):
        self.piezo["properties"] = {"formulation": "strain-charge", "rho_kg_m3": 7500,
            "cE_Pa": diagonal(6, 1e308), "d_C_N": [[1e308]*6 for _ in range(3)],
            "epsilonT_F_m": diagonal(3, 1e-8)}
        with self.assertRaises(ValueError):
            validate_material(self.piezo)

    def test_all_profile_and_catalog_text_rejects_isolated_surrogates(self):
        for template, validator in ((self.piezo, validate_material),
                                     (self.rod, lambda value: validate_device(value, self.materials))):
            for path in (("id",), ("name",), ("version",),
                         ("provenance", "source"), ("provenance", "note")):
                value = copy.deepcopy(template)
                target = value
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = "\ud800"
                with self.subTest(path=path, profile=template["type"]):
                    with self.assertRaisesRegex(ValueError, "UTF-8"):
                        validator(value)
                    catalog = copy.deepcopy(self.catalog)
                    array = "materials" if template["schema"] == "range.control-material/v1" else "devices"
                    catalog[array][1 if array == "materials" else 0] = value
                    with self.assertRaisesRegex(ValueError, "UTF-8"):
                        self.load_value(catalog)
        for key in ("purpose", "assumptions", "qualification"):
            catalog = copy.deepcopy(self.catalog)
            catalog["metadata"][key] = "\udfff"
            with self.assertRaisesRegex(ValueError, "UTF-8"):
                self.load_value(catalog)
        catalog = copy.deepcopy(self.catalog)
        catalog["version"] = "\ud800"
        with self.assertRaisesRegex(ValueError, "UTF-8"):
            self.load_value(catalog)

    def test_all_matrix_fields_reject_bool_nan_and_bad_dimensions(self):
        strain = copy.deepcopy(self.piezo)
        strain["properties"].pop("e_C_m2")
        strain["properties"].pop("epsilonS_F_m")
        strain["properties"].update(formulation="strain-charge", d_C_N=[[0]*6 for _ in range(3)],
                                    epsilonT_F_m=diagonal(3, 1e-8))
        for template in (self.piezo, strain):
            for field in set(template["properties"]) - {"formulation", "rho_kg_m3"}:
                for bad in (True, float("nan"), float("inf")):
                    with self.subTest(field=field, bad=bad):
                        value = copy.deepcopy(template)
                        value["properties"][field][0][0] = bad
                        with self.assertRaises(ValueError):
                            validate_material(value)
                for matrix in ([], [[0]], [[0]*7 for _ in range(6)]):
                    with self.subTest(field=field, matrix=matrix):
                        value = copy.deepcopy(template)
                        value["properties"][field] = matrix
                        with self.assertRaises(ValueError):
                            validate_material(value)

    def test_symmetric_and_positive_definite_requirements(self):
        for field in ("cE_Pa", "epsilonS_F_m"):
            for defect in ("asymmetry", "negative", "indefinite"):
                with self.subTest(field=field, defect=defect):
                    value = copy.deepcopy(self.piezo)
                    matrix = value["properties"][field]
                    if defect == "asymmetry":
                        matrix[0][1] = 1
                    elif defect == "negative":
                        matrix[0][0] = -1
                    else:
                        matrix[0][1] = matrix[1][0] = 2 * matrix[0][0]
                    with self.assertRaises(ValueError):
                        validate_material(value)

    def test_strain_epsilon_t_must_be_symmetric(self):
        p = self.piezo["properties"]
        p.pop("e_C_m2")
        p.pop("epsilonS_F_m")
        p.update(formulation="strain-charge", d_C_N=[[0]*6 for _ in range(3)],
                 epsilonT_F_m=[[1, 0.1, 0], [0, 1, 0], [0, 0, 1]])
        with self.assertRaises(ValueError):
            validate_material(self.piezo)

    def test_unknown_material_fields_and_schemas(self):
        for location in ((), ("properties",), ("provenance",), ("tensorConvention",)):
            value = copy.deepcopy(self.piezo)
            target = value
            for key in location:
                target = target[key]
            target["unexpected"] = 1
            with self.assertRaises(ValueError):
                validate_material(value)
        for key, bad in (("schema", "range.control-material/v2"), ("units", "mm"), ("version", "")):
            value = copy.deepcopy(self.piezo)
            value[key] = bad
            with self.assertRaises(ValueError):
                validate_material(value)

    def test_relative_permittivity_and_wrong_tensor_convention_rejected(self):
        self.piezo["properties"]["epsilonRelative"] = 1000
        with self.assertRaises(ValueError):
            validate_material(self.piezo)
        self.piezo["properties"].pop("epsilonRelative")
        self.piezo["tensorConvention"]["shear"] = "tensor"
        with self.assertRaises(ValueError):
            validate_material(self.piezo)

    def test_elastic_stability_and_positive_scalars(self):
        for key, bad in (("E_Pa", 0), ("rho_kg_m3", -1), ("nu", 0.5), ("nu", -1), ("nu", True)):
            value = copy.deepcopy(self.materials[0])
            value["properties"][key] = bad
            with self.assertRaises(ValueError):
                validate_material(value)

    def test_global_ids_and_material_references(self):
        for mutation in ("materials", "devices", "cross", "missing", "elastic"):
            value = copy.deepcopy(self.catalog)
            if mutation in ("materials", "devices"):
                value[mutation].append(copy.deepcopy(value[mutation][0]))
            elif mutation == "cross":
                value["devices"][0]["id"] = value["materials"][0]["id"]
            else:
                value["devices"][0]["materialId"] = "missing" if mutation == "missing" else self.materials[0]["id"]
            with self.assertRaises(ValueError):
                self.load_value(value)

    def test_effective_rod_and_forbidden_mixed_modes(self):
        self.rod.pop("materialId")
        self.rod["parameters"] = {"mode": "calibrated-effective", "k_N_m": 100,
                                  "freeStroke_m_V": -0.001, "capacitanceClamped_F": 1e-8, "mass_kg": 0.1}
        effective = validate_device(self.rod, self.materials)
        self.assertEqual(effective["parameters"]["freeStroke_m_V"], -0.001)
        for mixed in ("materialId", "length_m", "capacitance_F", "capacitanceFree_F"):
            value = copy.deepcopy(self.rod)
            if mixed == "materialId":
                value[mixed] = self.piezo["id"]
            else:
                value["parameters"][mixed] = 0.1
            with self.assertRaises(ValueError):
                validate_device(value, self.materials)
        for key in ("k_N_m", "capacitanceClamped_F", "mass_kg"):
            value = copy.deepcopy(self.rod)
            value["parameters"][key] = 0
            with self.assertRaises(ValueError):
                validate_device(value, self.materials)

    def test_geometry_positive_and_directions_unit_length(self):
        for key in ("length_m", "area_m2", "electrodeSeparation_m"):
            for bad in (0, -1, True):
                value = copy.deepcopy(self.rod)
                value["parameters"][key] = bad
                with self.assertRaises(ValueError):
                    validate_device(value, self.materials)
        for direction in ([0, 0, 0], [0, 0, 2], [0, True, 0], [0, 1]):
            self.rod["parameters"]["axis"] = direction
            with self.assertRaises(ValueError):
                validate_device(self.rod, self.materials)

    def test_patch_requires_explicit_ideal_bond(self):
        value = copy.deepcopy(self.catalog["devices"][1])
        value["parameters"]["bond"] = "adhesive"
        with self.assertRaises(ValueError):
            validate_device(value, self.materials)
        value["parameters"]["bond"] = "ideal"
        value["parameters"]["thickness_m"] = 0
        with self.assertRaises(ValueError):
            validate_device(value, self.materials)

    def test_cable_reference_and_travel_limits(self):
        for field, bad in (("minLength_m", 2), ("maxLength_m", 0.5), ("fixedMass_kg", 0),
                           ("massModel", "spool")):
            value = copy.deepcopy(self.catalog["devices"][2])
            value["parameters"][field] = bad
            with self.assertRaises(ValueError):
                validate_device(value, self.materials)

    def test_virtual_sensors_have_no_mass_or_hardware_claim(self):
        for sensor in self.catalog["devices"][3:]:
            canonical = validate_device(sensor, self.materials)
            self.assertEqual(set(canonical["parameters"]), {"quantity", "unit", "location"})
            for key, bad in (("mass_kg", 1), ("unit", "mm"), ("location", "node:1"), ("quantity", "DAQ")):
                value = copy.deepcopy(sensor)
                value["parameters"][key] = bad
                with self.assertRaises(ValueError):
                    validate_device(value, self.materials)

    def test_acceleration_requires_native_unit_spelling(self):
        sensor = copy.deepcopy(next(device for device in self.catalog["devices"]
                                    if device["parameters"].get("quantity") == "acceleration"))
        self.assertEqual(sensor["parameters"]["unit"], "m/s^2")
        self.assertEqual(validate_device(sensor, self.materials)["parameters"]["unit"], "m/s^2")
        sensor["parameters"]["unit"] = "m/s2"
        with self.assertRaises(ValueError):
            validate_device(sensor, self.materials)

    def test_device_unknown_fields_schema_units_and_bound_state(self):
        for key, bad in (("solverSupported", True), ("schema", "range.control-device/v2"),
                          ("units", "mixed"), ("state", "bound")):
            value = copy.deepcopy(self.rod)
            value[key] = bad
            with self.assertRaises(ValueError):
                validate_device(value, self.materials)

    def test_duplicate_json_keys_and_nonfinite_constants(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            for raw in ('{"schema":"a","schema":"b"}', '{"nested":{"x":1,"x":2}}',
                        '{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}'):
                path.write_text(raw, encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_catalog(path)

    def test_catalog_unknown_fields_schema_and_metadata(self):
        for key, bad in (("extra", 0), ("schema", "range.control-catalog/v2"), ("materials", {})):
            value = copy.deepcopy(self.catalog)
            value[key] = bad
            with self.assertRaises(ValueError):
                self.load_value(value)
        self.catalog["metadata"]["solverSupported"] = True
        with self.assertRaises(ValueError):
            self.load_value(self.catalog)

    def test_digest_stability_and_all_metadata_sensitivity(self):
        original = catalog_digest(self.catalog)
        reordered = {key: self.catalog[key] for key in reversed(self.catalog)}
        self.assertEqual(original, catalog_digest(reordered))
        for key, bad in (("units", "mm"), ("state", "bound"), ("version", "2")):
            value = copy.deepcopy(self.catalog)
            value["devices"][0][key] = bad
            self.assertNotEqual(original, catalog_digest(value))
        with self.assertRaises(ValueError):
            catalog_digest({"x": float("nan")})

    def test_canonical_output_is_independently_owned_and_idempotent(self):
        canonical = validate_material(self.piezo)
        self.assertEqual(canonical, validate_material(canonical))
        canonical["properties"]["cE_Pa"][0][0] = 1
        canonical["tensorConvention"]["voigtOrder"][0] = "changed"
        self.assertEqual(self.piezo["tensorConvention"]["voigtOrder"][0], "xx")
        self.assertEqual(self.piezo["properties"]["cE_Pa"][0][0], 1e11)
        self.assertEqual(self.catalog, self.load_value(self.catalog))
        self.catalog["metadata"]["purpose"] = "caller modified"
        self.assertNotEqual(self.catalog["metadata"]["purpose"], load_catalog()["metadata"]["purpose"])


if __name__ == "__main__":
    unittest.main()
