"""Independent time-load goldens; these do not qualify the native FE driver."""
import copy
from contextvars import copy_context
from dataclasses import FrozenInstanceError
import importlib.util
import math
from pathlib import Path
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import piezo_excitation as pe


def external_ramp(t):
    return {"tipX": {"value": 2 * t, "unit": "N"}}


class ExcitationTest(unittest.TestCase):
    def test_framework_batch_parity_bounds_and_scope_isolation(self):
        p = self.compile({"kind": "expression", "expression": "2*t+sin(t)"})
        other = self.compile({"kind": "constant", "value": 9})
        expected = [p.evaluate(t) for t in (0., .25, 1.)]
        with patch.object(pe, "_check_source", wraps=pe._check_source) as check:
            with p._framework_batch(3):
                actual = [p.evaluate(t) for t in (0., .25, 1.)]
                p.check_revision()
                self.assertEqual(check.call_count, 1)
                other.evaluate(0)
                self.assertEqual(check.call_count, 2)
                with self.assertRaisesRegex(pe.ExcitationError, "cannot nest"):
                    with p._framework_batch(): pass
                with self.assertRaisesRegex(pe.ExcitationError, "point bound"):
                    p.evaluate(2)
                inherited = copy_context()
                with ThreadPoolExecutor(max_workers=1) as pool:
                    self.assertEqual(pool.submit(inherited.run, p.evaluate, .25).result(), expected[1])
                self.assertEqual(check.call_count, 3)
            self.assertEqual(check.call_count, 4)
            self.assertEqual(inherited.run(p.evaluate, .25), expected[1])
            self.assertEqual(check.call_count, 5)
        self.assertEqual(actual, expected)
        self.assertIsNone(pe._FRAMEWORK_BATCH.get())
        for maximum in (True, -1, 130, .5):
            with self.assertRaises(pe.ExcitationError):
                with p._framework_batch(maximum): pass
        with p._framework_batch(0): p.check_revision()
        with p._framework_batch(129):
            for t in range(129): p.evaluate(t)

    def test_framework_batch_source_change_and_exception_reset(self):
        p = self.compile({"kind": "expression", "expression": "1/t"})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"framework.py"
            path.write_bytes(pe._SOURCE_BYTES)
            with patch.object(pe, "_SOURCE_PATH", path):
                with self.assertRaisesRegex(pe.ExcitationError, "source changed"):
                    with p._framework_batch(1):
                        p.evaluate(1)
                        path.write_bytes(pe._SOURCE_BYTES+b"\n# changed within batch\n")
                self.assertIsNone(pe._FRAMEWORK_BATCH.get())
                with self.assertRaises(pe.ExcitationError): p.evaluate(1)
                path.write_bytes(pe._SOURCE_BYTES)
                with self.assertRaises(pe.ExcitationError):
                    with p._framework_batch(1): p.evaluate(0)
                self.assertIsNone(pe._FRAMEWORK_BATCH.get())
                with patch.object(pe, "_check_source", wraps=pe._check_source) as check:
                    self.assertEqual(self.force(p, 1), 1)
                    self.assertEqual(check.call_count, 1)

    def test_framework_batch_retains_every_external_revision_and_evaluation(self):
        calls, revisions = [], []
        def function(t):
            calls.append(t)
            return {"tipX": {"value": 2*t, "unit": "N"}}
        e = self.external(function, cache_exact_time=True, revision_check=lambda: revisions.append(1))
        p = pe.compile_excitation(self.catalog, external_functions=(e,))
        revisions.clear()
        with p._framework_batch(3):
            actual = [p.evaluate(t) for t in (0., .5, .5)]
        self.assertEqual(calls, [0., .5, .5])
        self.assertEqual(len(revisions), 9)  # Proposal check plus external pre/post, at EVERY point.
        self.assertEqual(actual[1], actual[2])
        def replacement(t):
            calls.append(t)
            return {"tipX": {"value": 0., "unit": "N"}}
        with self.assertRaisesRegex(pe.ExcitationError, "revision changed"):
            with p._framework_batch(2):
                p.evaluate(.5)
                function.__code__ = replacement.__code__
                p.evaluate(.5)
        self.assertIsNone(pe._FRAMEWORK_BATCH.get())

    def test_local_catalog_units_capabilities_and_global_rotation_alias(self):
        records = [{"channelId": "globalRy", "physicalNodeId": 42, "dof": "RY", "unit": "N m"},
                   {"channelId": "localRy", "physicalNodeId": 42, "dof": "RY", "unit": "N m", "coordinateSystem": "local"},
                   {"channelId": "localEz", "physicalNodeId": 42, "dof": "EZ", "unit": "N", "coordinateSystem": "local"}]
        catalog = pe.build_force_catalog(records, supported_dofs={42: ("RY", "EZ")})
        self.assertEqual(tuple(c.coordinate_system for c in catalog), ("global", "local", "local"))
        p = pe.compile_excitation(catalog, {"channels": [{"channelId": "localEz", "unit": "N",
            "terms": [{"kind": "constant", "value": 3.}]}]})
        self.assertEqual(p.evaluate(.01)["forces"]["localEz"], {"value": 3., "unit": "N"})
        for mutation in ("unit", "axis", "coordinate", "unsupported"):
            invalid = copy.deepcopy(records)
            if mutation == "unit": invalid[2]["unit"] = "N m"
            elif mutation == "axis": invalid[2]["dof"] = "Z"
            elif mutation == "coordinate": invalid[2]["coordinateSystem"] = "user-vector"
            else: invalid[2]["dof"] = "EX"
            with self.assertRaises(pe.ExcitationError):
                pe.build_force_catalog(invalid, supported_dofs={42: ("RY", "EZ")})

    def test_local_global_coordinate_identity_and_duplicate_binding(self):
        global_channel = pe.ForceChannel("moment", 42, "RY", "N m")
        local_channel = pe.ForceChannel("moment", 42, "RY", "N m", "local")
        a, b = pe.compile_excitation((global_channel,)), pe.compile_excitation((local_channel,))
        self.assertNotEqual(a.source_hash, b.source_hash)
        duplicate = pe.ForceChannel("other", 42, "RY", "N m", "local")
        with self.assertRaises(pe.ExcitationError):
            pe.compile_excitation((local_channel, duplicate))

    def test_local_moment_builtin_external_addition_and_failed_frame_atomicity(self):
        channel = pe.ForceChannel("node:42:local:RY", 42, "RY", "N m", "local")
        def load(t):
            return {channel.channel_id: {"value": 2*t, "unit": "N m" if t < 1 else "N"}}
        external = pe.ExternalTimeFunction(load, code_identity="local-test", dependency_identity="none",
            environment_identity="current", channels=(channel,), missing_policy="error", cache_exact_time=True)
        p = pe.compile_excitation((channel,), {"channels": [{"channelId": channel.channel_id, "unit": "N m",
            "terms": [{"kind": "constant", "value": 3.}]}]}, external_functions=(external,))
        accepted = p.evaluate(.125)
        self.assertEqual(accepted["forces"][channel.channel_id], {"value": 3.25, "unit": "N m"})
        with self.assertRaises(pe.ExcitationError): p.evaluate(1.)
        self.assertEqual(accepted, p.evaluate(.125))

    def setUp(self):
        self.catalog = pe.build_force_catalog([
            {"channelId": "tipX", "physicalNodeId": 42, "dof": "X", "unit": "N"},
            {"channelId": "tipY", "physicalNodeId": 42, "dof": "Y", "unit": "N"},
            {"channelId": "moment", "physicalNodeId": 42, "dof": "RZ", "unit": "N m"},
        ], supported_dofs={42: ("X", "Y", "RZ")})

    def compile(self, *terms):
        return pe.compile_excitation(self.catalog, {"channels": [{"channelId": "tipX", "unit": "N", "terms": list(terms)}]})

    def force(self, proposal, t):
        return proposal.evaluate(t)["forces"]["tipX"]["value"]

    def external(self, function=external_ramp, **kwargs):
        options = dict(code_identity="test-v1", dependency_identity="stdlib-only", environment_identity="test-python", channels=(self.catalog[0],), missing_policy="error")
        options.update(kwargs)
        return pe.ExternalTimeFunction(function, **options)

    def test_free_default_all_named_units_and_actual_time(self):
        p = pe.compile_excitation(self.catalog)
        r = p.evaluate_frame({"time": 0.123})
        self.assertEqual(r["time"], 0.123)
        self.assertEqual(r["forces"], {"tipX": {"value": 0, "unit": "N"}, "tipY": {"value": 0, "unit": "N"}, "moment": {"value": 0, "unit": "N m"}})
        self.assertEqual(len(r["sourceHash"]), 64)
        self.assertEqual(len(r["loadedCodeHash"]), 64)

    def test_catalog_actual_dofs_and_units(self):
        record = {"channelId": "f", "physicalNodeId": 42, "dof": "RZ", "unit": "N m"}
        with self.assertRaises(pe.ExcitationError):
            pe.build_force_catalog([record], supported_dofs={42: ("X",)})
        with self.assertRaises(pe.ExcitationError):
            pe.build_force_catalog([record, record], supported_dofs={42: ("RZ",)})
        record["unit"] = "N"
        with self.assertRaises(pe.ExcitationError):
            pe.build_force_catalog([record], supported_dofs={42: ("RZ",)})
        with self.assertRaises(pe.ExcitationError):
            pe.compile_excitation((self.catalog[0], self.catalog[0]))

    def test_additive_constant_sine_multisine_goldens(self):
        p = self.compile({"kind": "constant", "value": 3}, {"kind": "sine", "amplitude": 2, "frequencyHz": 1, "phaseRad": 0}, {"kind": "multisine", "components": [{"amplitude": 4, "frequencyHz": 2, "phaseRad": 0}, {"amplitude": 1, "frequencyHz": 0, "phaseRad": math.pi / 2}]})
        self.assertAlmostEqual(self.force(p, 0), 4)
        self.assertAlmostEqual(self.force(p, 0.25), 6)
        self.assertAlmostEqual(self.force(p, 0.125), 8 + math.sqrt(2))

    def test_chirp_integrated_phase_frequency_derivative(self):
        p = self.compile({"kind": "chirp", "amplitude": 1, "startTime": 2, "duration": 4, "startFrequencyHz": 0.1, "endFrequencyHz": 0.5, "phaseRad": 0.2})
        tau = 0.7
        phase = 0.2 + 2 * math.pi * (0.1 * tau + 0.05 * tau ** 2)
        self.assertAlmostEqual(self.force(p, 2 + tau), math.sin(phase), places=14)
        h = 1e-6
        derivative = (self.force(p, 2 + tau + h) - self.force(p, 2 + tau - h)) / (2 * h)
        self.assertAlmostEqual(derivative, math.cos(phase) * 2 * math.pi * (0.1 + 0.1 * tau), places=8)
        self.assertEqual(self.force(p, 1.99), 0)
        self.assertEqual(self.force(p, 6.01), 0)

    def test_c2_smooth_pulse_endpoint_goldens(self):
        p = self.compile({"kind": "smoothPulse", "amplitude": 8, "startTime": 1, "riseTime": 2, "holdTime": 1, "fallTime": 2})
        for t, v in [(0, 0), (1, 0), (2, 4), (3, 8), (4, 8), (5, 4), (6, 0), (7, 0)]:
            self.assertEqual(self.force(p, t), v)
        h = 1e-4
        for t in (1, 3, 4, 6):
            d1 = (self.force(p, t + h) - self.force(p, t - h)) / (2 * h)
            d2 = (self.force(p, t + h) - 2 * self.force(p, t) + self.force(p, t - h)) / h ** 2
            self.assertLess(abs(d1), 1e-6)
            self.assertLess(abs(d2), 0.002)

    def test_table_boundaries_and_explicit_policies(self):
        term = {"kind": "table", "times": [1, 2, 4], "values": [2, 6, 0], "interpolation": "linear", "outOfRange": "error"}
        p = self.compile(term)
        for t, value in [(1, 2), (1.5, 4), (2, 6), (3, 3), (4, 0)]:
            self.assertEqual(self.force(p, t), value)
        for t in (0.99, 4.01):
            with self.assertRaises(pe.ExcitationError):
                self.force(p, t)
        term["outOfRange"] = "hold"
        self.assertEqual(self.force(self.compile(term), 0), 2)
        term["outOfRange"] = "zero"
        self.assertEqual(self.force(self.compile(term), 0), 0)
        term["times"] = [1, 1, 4]
        with self.assertRaises(pe.ExcitationError):
            self.compile(term)

    def test_expression_goldens_and_endpoint_ramp_not_held(self):
        p = self.compile({"kind": "expression", "expression": "2*t + sin(pi/2) + cos(0) + exp(0) + sqrt(4) + t**2 - t**-1"})
        self.assertEqual(self.force(p, 2), 12.5)
        ramp = self.compile({"kind": "expression", "expression": "3*t"})
        self.assertEqual(self.force(ramp, 1), 3)
        self.assertAlmostEqual(self.force(ramp, 1.2), 3.6)
        self.assertNotEqual(self.force(ramp, 1), self.force(ramp, 1.2))

    def test_expression_rejects_all_non_time_constructs(self):
        malformed = ["q", "__import__('os')", "t.real", "[t][0]", "[x for x in [t]]", "lambda: t", "sin(t, t)", "sin(x=t)", "t**t", "t**17", "True", "1e999", "t if t else 0", "t%2", "'x'", "{t: t}"]
        for source in malformed:
            with self.subTest(source=source), self.assertRaises(pe.ExcitationError):
                self.compile({"kind": "expression", "expression": source})
        for source, t in [("exp(t)", 1000), ("1/t", 0), ("sqrt(t)", -1), ("t**0.5", -1), ("1e308*t", 100)]:
            with self.subTest(source=source), self.assertRaises(pe.ExcitationError):
                self.force(self.compile({"kind": "expression", "expression": source}), t)

    def test_time_only_protocol_and_nonfinite(self):
        p = pe.compile_excitation(self.catalog)
        for frame in [{"time": 0, "q": []}, {"t": 0}, {}, {"time": False}, {"time": math.inf}]:
            with self.assertRaises(pe.ExcitationError):
                p.evaluate_frame(frame)

    def test_configuration_frozen_exact_identity_and_atomic_errors(self):
        config = {"channels": [{"channelId": "tipX", "unit": "N", "terms": [{"kind": "constant", "value": 0.0}]}]}
        original = copy.deepcopy(config)
        p = pe.compile_excitation(self.catalog, config)
        config["channels"][0]["terms"][0]["value"] = 20
        self.assertEqual(self.force(p, 0), 0)
        negative = copy.deepcopy(original)
        negative["channels"][0]["terms"][0]["value"] = -0.0
        self.assertNotEqual(p.source_hash, pe.compile_excitation(self.catalog, negative).source_hash)
        self.assertEqual(p.source_hash, pe.compile_excitation(self.catalog, original).source_hash)
        with self.assertRaises(FrozenInstanceError):
            p.source_hash = "wrong"
        for update in ({"unit": "N m"}, {"channelId": "unknown"}, {"terms": []}):
            bad = copy.deepcopy(original)
            bad["channels"][0].update(update)
            with self.assertRaises(pe.ExcitationError):
                pe.compile_excitation(self.catalog, bad)
            self.assertEqual(self.force(p, 0), 0)
        bad = copy.deepcopy(original)
        bad["channels"].append(copy.deepcopy(bad["channels"][0]))
        with self.assertRaises(pe.ExcitationError):
            pe.compile_excitation(self.catalog, bad)

    def test_external_trusted_t_only_declared_subset_and_units(self):
        e = self.external()
        p = pe.compile_excitation(self.catalog, external_functions=(e,))
        self.assertEqual(self.force(p, 2), 4)
        self.assertEqual(p.evaluate(2)["forces"]["tipY"]["value"], 0)
        self.assertTrue(e.source_bytes)
        self.assertEqual(len(e.loaded_code_hash), 64)
        for function in (lambda t, q: {}, lambda t=0: {}, lambda **kwargs: {}):
            with self.assertRaises(pe.ExcitationError):
                self.external(function)
        for function in (lambda t: {"tipX": {"value": 1, "unit": "N m"}}, lambda t: {"unknown": {"value": 1, "unit": "N"}}, lambda t: {"tipX": {"value": math.inf, "unit": "N"}}, lambda t: {}):
            with self.assertRaises(pe.ExcitationError):
                self.external(function).evaluate(0)
        self.assertEqual(self.external(lambda t: {}, missing_policy="zero").evaluate(0)["tipX"], 0)
        with self.assertRaises(pe.ExcitationError):
            self.external(code_identity="")

    def test_external_exact_retry_cache_and_stale_loaded_code(self):
        calls = []
        def function(t):
            calls.append(t)
            return {"tipX": {"value": t, "unit": "N"}}
        e = self.external(function, cache_exact_time=True)
        self.assertEqual(e.evaluate(2), e.evaluate(2))
        self.assertEqual(len(calls), 2)
        e.evaluate(math.nextafter(2, math.inf))
        self.assertEqual(len(calls), 3)
        with self.assertRaises(TypeError):
            e.evaluate(2)["tipX"] = 0
        def replacement(t):
            calls.append(t)
            return {"tipX": {"value": 0, "unit": "N"}}
        function.__code__ = replacement.__code__
        with self.assertRaises(pe.ExcitationError):
            e.evaluate(2)

    def test_external_actual_source_revision_rejected_even_cached(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trusted_force.py"
            path.write_text('def force(t):\n    return {"tipX": {"value": t, "unit": "N"}}\n', encoding="utf-8")
            spec = importlib.util.spec_from_file_location("trusted_force", path)
            module = importlib.util.module_from_spec(spec)
            sys.modules["trusted_force"] = module
            try:
                spec.loader.exec_module(module)
                e = self.external(module.force, cache_exact_time=True)
                self.assertEqual(e.source_bytes, path.read_bytes())
                e.evaluate(1)
                path.write_text('# revised\n' + path.read_text(encoding="utf-8"), encoding="utf-8")
                with self.assertRaises(pe.ExcitationError):
                    e.evaluate(1)
            finally:
                sys.modules.pop("trusted_force", None)

    def test_stateless_repeat_and_no_result_mutation(self):
        p = self.compile({"kind": "expression", "expression": "sin(2*pi*t)+t**2"})
        first = p.evaluate(0.4)
        p.evaluate(0.8)
        self.assertEqual(first, p.evaluate(0.4))
        first["forces"]["tipX"]["value"] = 100
        self.assertNotEqual(first, p.evaluate(0.4))

    def test_loaded_backend_source_revision_rejected(self):
        p = pe.compile_excitation(self.catalog)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.py"
            path.write_bytes(pe._SOURCE_BYTES)
            with patch.object(pe, "_SOURCE_PATH", path):
                p.evaluate(0)
                path.write_bytes(pe._SOURCE_BYTES + b"\n# revision\n")
                with self.assertRaises(pe.ExcitationError):
                    p.evaluate(0)
                with self.assertRaises(pe.ExcitationError):
                    pe.compile_excitation(self.catalog)

    def test_external_full_catalog_and_atomic_failed_output(self):
        def function(t):
            return {c.channel_id: {"value": t, "unit": c.unit if t == 0 else "bad"} for c in self.catalog}
        e = self.external(function, channels=self.catalog, cache_exact_time=True)
        p = pe.compile_excitation(self.catalog, external_functions=(e,))
        good = p.evaluate(0)
        with self.assertRaises(pe.ExcitationError):
            p.evaluate(1)
        self.assertEqual(good, p.evaluate(0))
        self.assertNotIn(b"?\xf0\x00\x00\x00\x00\x00\x00", e._cache)


if __name__ == "__main__":
    unittest.main()
