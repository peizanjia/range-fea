"""Native bridge protocol and accepted-history checks; no GUI delivery claim."""
import copy
import json
import hashlib
import tempfile
import platform
import os
from pathlib import Path
import subprocess
import sys
import struct
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"python"))

import numpy as np
import scipy
from numpy.testing import assert_allclose
from range_workflow.piezo_design import SourceIdentity, compute_modes
from range_workflow.piezo_native_backend import NativeBackend
from range_workflow import piezo_native_backend as backend_module
from range_workflow import piezo_excitation as pe


def configuration(kind="PI", N=1, **controller):
    defaults = {"PI": {"kp": 2., "ki": 3.},
                "PD": {"kp": 2., "kd": .4, "derivativeFilterAlpha": .25},
                "PID": {"kp": 2., "ki": 3., "kd": .4, "derivativeFilterAlpha": .25},
                "velocity_feedback": {"gain": 2.},
                "LQR": {"Q": [[2., 0.], [0., 1.]], "R": [[.5]]},
                "MPC": {"Q": [[2., 0.], [0., 1.]], "R": [[.5]],
                    "Qf": [[2., 0.], [0., 1.]], "horizon": 3,
                    "voltageLower": [4.], "voltageUpper": [6.], "deltaVoltageLimits": [.3]},
                "discrete_state_space": {"Ac": [[.8]], "Bc": [[.1]], "Cc": [[2.]],
                    "Dc": [[3.]], "initialState": [4.], "samplingPeriod": .1*N,
                    "inputUnits": ["m"], "outputUnits": ["V"]}, "none": {}}
    c = dict(kind=kind, **defaults[kind])
    c.update(controller)
    unit = "m/s" if kind == "velocity_feedback" else "m"
    return {"op": "initialize", "sourceFingerprint": "a"*64,
            "M": [[2.]], "K": [[8.]], "C": [[.4]], "B": [[3.]],
            "Lq": [[0. if kind == "velocity_feedback" else 1.]],
            "Lv": [[1. if kind == "velocity_feedback" else 0.]], "La": [[0.]], "D": [[.2]],
            "dofs": [["node:7:X", "m"]], "inputs": [["actuator:a", "V"]],
            "outputs": [["sensor:s", unit]], "actuatorVoltages0": [5.], "dt": .1,
            "controlEveryFESteps": N, "nodeIds": [7, 12],
            "forceCatalog": [{"channelId": "force:7:X", "physicalNodeId": 7,
                              "dof": "X", "unit": "N", "physicalIndex": 0}],
            "configuration": {"design": {}, "controller": c, "excitation": {"channels": []}}}


def readouts(value, unit="m"):
    return [{"portId": "sensor:s", "deltaValue": value, "unit": unit, "actionRecord": False}]


def forced_configuration(kind="LQR", N=3, initial_time=1.7, **controller):
    message = configuration(kind, N=N, **controller)
    message.update(Bforce=[[4.]], Dforce=[[.7]], disturbanceInputs=[["force:7:X", "N"]],
                   initialTime=initial_time, biasForce=[.6])
    message["forceCatalog"][0]["disturbanceColumn"] = 0
    message["configuration"]["excitation"] = {"channels": [{"channelId": "force:7:X", "unit": "N",
        "terms": [{"kind": "expression", "expression": "2+3*t"}]}]}
    return message


def framed_configuration(family="planarBeam", *, rotated=True):
    message = configuration("none")
    axis = np.array([1., 2., 3.])/np.sqrt(14.)
    skew = np.array([[0., -axis[2], axis[1]], [axis[2], 0., -axis[0]], [-axis[1], axis[0], 0.]])
    c, s = np.cos(.71), np.sin(.71)
    basis = c*np.eye(3)+(1-c)*np.outer(axis, axis)+s*skew if rotated else np.eye(3)
    message["sourceFrame"] = {"family": family, "frame": {"origin": [.3, -.2, .4],
        **{name: basis[:, index].tolist() for index, name in enumerate(("ex", "ey", "ez"))}}}
    message["forceCatalog"] = []
    message["configuration"]["excitation"] = {"channels": []}
    return message, basis


def add_direction(message, dof, value, *, local=True):
    node = 7
    axes = ("EX", "EY", "EZ", "RX", "RY", "RZ") if local else ("X", "Y", "Z", "RX", "RY", "RZ")
    component = axes.index(dof)
    unit = "N m" if component >= 3 else "N"
    cid = f"node:{node}:"+("local:" if local else "")+dof
    direction = np.zeros(6)
    if local:
        frame = message["sourceFrame"]["frame"]
        direction[3*(component//3):3*(component//3)+3] = frame[("ex", "ey", "ez")[component % 3]]
    else:
        direction[component] = 1.
    record = {"channelId": cid, "physicalNodeId": node, "dof": dof, "unit": unit,
              "coordinateSystem": "local" if local else "global", "physicalDirection": direction.tolist()}
    if not local:
        record["physicalIndex"] = component
    message["forceCatalog"].append(record)
    message["configuration"]["excitation"]["channels"].append({"channelId": cid, "unit": unit,
        "terms": [{"kind": "constant", "value": value}]})
    return record


def bind_directions(message):
    records = message["forceCatalog"]
    for column, record in enumerate(records):
        record["disturbanceColumn"] = column
    count = len(records)
    directions = np.zeros((12, count))
    for column, record in enumerate(records):
        directions[:6, column] = record["physicalDirection"]
    message.update(Bforce=[[2.]*count], Dforce=[[0.]*count], initialTime=0.,
        disturbanceInputs=[[record["channelId"], record["unit"]] for record in records],
        physicalForceDirections=directions.tolist())


def batch_configuration(target=4, initial=0.):
    message = forced_configuration("none", N=1, initial_time=initial)
    message["targetSteps"] = target
    message["initialTimeBits"] = struct.pack("<d", initial).hex()
    return message


def boundary_record(backend, initial_state, step=0):
    state = copy.deepcopy(initial_state)
    state["feStep"], state["controlTick"] = step, (step+backend.N-1)//backend.N
    return {"recordId": hashlib.sha256(f"caller-raw-codec-proof:{step}".encode()).hexdigest(),
        "opaqueState": state, "feStep": step, "controlTick": state["controlTick"], "phase": "ready",
        "appliedVoltages": state["previousApplied"].copy(),
        "heldVoltages": (backend.va0 if state["controlTick"] == 0 else
                         backend.va0+np.array(state["heldDelta"])).tolist()}


def batch_time(backend, step):
    time = backend.initial_time if step == 0 else backend.initial_time+step*backend.dt
    return {"feStep": step, "timeBits": struct.pack("<d", time).hex()}


class NativeBackendTest(unittest.TestCase):
    def test_certification_batch_checks_framework_twice_and_single_force_per_point(self):
        b, state, _ = self.initialize(batch_configuration(target=129))
        request = {"op": "certificationBatch", "records": [boundary_record(b, state)],
                   "times": [batch_time(b, step) for step in range(129)]}
        with patch.object(pe, "_check_source", wraps=pe._check_source) as check:
            result = self.ok(b, request)
            self.assertEqual(check.call_count, 2)
            self.ok(b, {"op": "certificationBatch", "records": [], "times": []})
            self.assertEqual(check.call_count, 4)
            first = self.ok(b, {"op": "force", "time": 0.})
            self.assertEqual(check.call_count, 6)  # Service and actual point checks remain.
        self.assertEqual(first, {key: result["forces"][0][key]
            for key in ("forces", "sourceHash", "loadedCodeHash")})
        self.assertIsNone(pe._FRAMEWORK_BATCH.get())

    def test_certification_batch_framework_mutation_is_atomic_and_resets_guard(self):
        b, state, _ = self.initialize(batch_configuration())
        request = {"op": "certificationBatch", "records": [boundary_record(b, state)],
                   "times": [batch_time(b, step) for step in (0, 1)]}
        before, original_force = copy.deepcopy(state), b._force
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"framework.py"
            path.write_bytes(pe._SOURCE_BYTES)
            def modify_after_force(msg):
                result = original_force(msg)
                path.write_bytes(pe._SOURCE_BYTES+b"\n# changed during batch\n")
                return result
            with patch.object(pe, "_SOURCE_PATH", path):
                with patch.object(b, "_force", side_effect=modify_after_force) as force:
                    response = b.handle(request)
                    self.assertFalse(response["ok"])
                    self.assertNotIn("result", response)
                    self.assertEqual(force.call_count, 2)
                self.assertIsNone(pe._FRAMEWORK_BATCH.get())
                self.assertFalse(b.handle({"op": "force", "time": 0.})["ok"])
                path.write_bytes(pe._SOURCE_BYTES)
                self.ok(b, request)
                with patch.object(b, "_force", side_effect=RuntimeError("force failed")):
                    self.bad(b, request, "force failed")
                self.assertIsNone(pe._FRAMEWORK_BATCH.get())
                path.write_bytes(pe._SOURCE_BYTES+b"\n# changed after failed batch\n")
                self.assertFalse(b.handle({"op": "force", "time": 0.})["ok"])
                path.write_bytes(pe._SOURCE_BYTES)
                self.ok(b, {"op": "force", "time": 0.})
        self.assertEqual(state, before)
        self.ok(b, {"op": "validate", "opaqueState": state})

    def test_mpc_certificate_explicitly_excludes_native_slew_trajectory(self):
        backend, state, initial = self.initialize(configuration("MPC", N=3))
        scope = initial["design"]["actuatorPredictionScope"]
        self.assertFalse(scope["includesNativeSlewTrajectory"])
        self.assertFalse(scope["guaranteesActualFEStateConstraints"])
        self.assertEqual(scope["observerPrediction"], "actual-accepted-voltage-at-every-FE-step")
        q, v = np.array([-1.]), np.array([-1.])
        x = np.concatenate((backend.Phi.T@backend.M@q, backend.Phi.T@backend.M@v))
        proposal = self.ok(backend, {"op": "propose", "opaqueState": state,
                                   "readouts": readouts(-1.), "q": q.tolist(), "v": v.tolist()})
        diagnostics = proposal["diagnostics"]["mpc"]
        self.assertEqual(diagnostics["actuatorPredictionScope"], scope)
        self.assertLess(diagnostics["stationarity"], 1e-6)
        target = proposal["absoluteVoltages"][0]
        self.assertGreater(abs(target-5.), .1)
        aa, bb, offsets = backend._forecast(0, 1)
        held_prediction = aa[0]@x + bb[0][:, 0]*(target-5.) + offsets[0]
        native_prediction, applied, accepted = x.copy(), 5., proposal["candidateOpaqueState"]
        for step in range(3):
            applied += float(np.clip(target-applied, -.1, .1))
            _, _, system, foh = backend._interval(step)
            native_prediction = system.A@native_prediction + system.B[:, 0]*(applied-5.)
            accepted = self.ok(backend, {"op": "postFE", "opaqueState": accepted,
                                        "appliedVoltages": [applied], "feStep": step+1})["candidateOpaqueState"]
        self.assertGreater(np.linalg.norm(native_prediction-held_prediction), 1e-4)
        self.assertEqual(accepted["previousApplied"], [applied])

    def test_certification_batch_matches_every_existing_validator_and_force(self):
        b, state, initialized = self.initialize(batch_configuration())
        rows = [boundary_record(b, state, step) for step in (0, 1, 2)]
        times = [batch_time(b, step) for step in (0, 1, 2, 3)]
        before = copy.deepcopy(rows)
        expected_rows = [{"recordId": row["recordId"], **self.ok(b,
            {"op": "validate", **{key: value for key, value in row.items() if key != "recordId"}})} for row in rows]
        expected_forces = [{**entry, "time": struct.unpack("<d", bytes.fromhex(entry["timeBits"]))[0],
            **self.ok(b, {"op": "force", "time": struct.unpack("<d", bytes.fromhex(entry["timeBits"]))[0]})} for entry in times]
        with patch.object(b, "_validate", wraps=b._validate) as validate, patch.object(b, "_force", wraps=b._force) as force:
            result = self.ok(b, {"op": "certificationBatch", "records": rows, "times": times})
            self.assertEqual(validate.call_count, len(rows))
            self.assertEqual(force.call_count, len(times))
        self.assertEqual(result["records"], expected_rows)
        self.assertEqual(result["forces"], expected_forces)
        self.assertEqual(result["sourceFingerprint"], initialized["identity"]["sourceFingerprint"])
        self.assertEqual(result["configurationIdentity"], initialized["identity"]["configurationIdentity"])
        self.assertEqual(result["backend"], initialized["identity"]["backend"])
        self.assertEqual(rows, before)
        self.ok(b, {"op": "validate", "opaqueState": state})
        self.assertEqual(result, self.ok(b, {"op": "certificationBatch", "records": rows, "times": times}))

    def test_certification_batch_maximum_bounds_and_collision_rejections(self):
        b, state, _ = self.initialize(batch_configuration(target=129))
        rows = [boundary_record(b, state, step) for step in range(128)]
        times = [batch_time(b, step) for step in range(129)]
        result = self.ok(b, {"op": "certificationBatch", "records": rows, "times": times})
        self.assertEqual(len(result["records"]), 128)
        self.assertEqual(len(result["forces"]), 129)
        for request in (
                {"op": "certificationBatch", "records": rows+[boundary_record(b, state, 128)], "times": []},
                {"op": "certificationBatch", "records": [], "times": times+[batch_time(b, 129)]},
                {"op": "certificationBatch", "records": [rows[0], rows[0]], "times": []},
                {"op": "certificationBatch", "records": [], "times": [times[0], times[0]]}):
            response = b.handle(request)
            self.assertFalse(response["ok"])
            self.assertNotIn("result", response)
        duplicate_id = copy.deepcopy(rows[:2]); duplicate_id[1]["recordId"] = duplicate_id[0]["recordId"]
        self.bad(b, {"op": "certificationBatch", "records": duplicate_id, "times": []}, "duplicate")

    def test_certification_batch_closed_shapes_and_ready_boundary_only(self):
        b, state, _ = self.initialize(batch_configuration())
        row, time = boundary_record(b, state), batch_time(b, 0)
        request = {"op": "certificationBatch", "records": [row], "times": [time]}
        for mutation in ("top-field", "missing-array", "records-object", "times-object", "record-field",
                         "missing-applied", "missing-held", "time-field", "missing-timeBits", "proposed",
                         "bad-id", "upper-id", "boolean-step", "noninteger-tick"):
            invalid = copy.deepcopy(request)
            if mutation == "top-field": invalid["sourceFingerprint"] = "a"*64
            elif mutation == "missing-array": invalid.pop("times")
            elif mutation == "records-object": invalid["records"] = {}
            elif mutation == "times-object": invalid["times"] = {}
            elif mutation == "record-field": invalid["records"][0]["rawPayload"] = "ignored"
            elif mutation == "missing-applied": invalid["records"][0].pop("appliedVoltages")
            elif mutation == "missing-held": invalid["records"][0].pop("heldVoltages")
            elif mutation == "time-field": invalid["times"][0]["time"] = 0.
            elif mutation == "missing-timeBits": invalid["times"][0].pop("timeBits")
            elif mutation == "proposed": invalid["records"][0]["phase"] = "proposed"
            elif mutation == "bad-id": invalid["records"][0]["recordId"] = "raw-payload-name"
            elif mutation == "upper-id": invalid["records"][0]["recordId"] = "A"*64
            elif mutation == "boolean-step": invalid["records"][0]["feStep"] = True
            else: invalid["records"][0]["controlTick"] = 0.5
            response = b.handle(invalid)
            self.assertFalse(response["ok"], mutation)
            self.assertNotIn("result", response)

    def test_certification_batch_exact_time_bits_target_bounds_and_signed_zero(self):
        message = batch_configuration(initial=-0.)
        message["initialTime"] = +0.  # Numeric JSON may normalize zero; bits own its exact sign.
        b, state, _ = self.initialize(message)
        self.assertEqual(struct.pack("<d", b.initial_time).hex(), "0000000000000080")
        valid = self.ok(b, {"op": "certificationBatch", "records": [boundary_record(b, state)],
                           "times": [batch_time(b, 0), batch_time(b, 4)]})
        self.assertEqual(valid["forces"][0]["timeBits"], "0000000000000080")
        self.assertEqual(struct.pack("<d", valid["forces"][0]["time"]).hex(), "0000000000000080")
        invalid_times = [dict(feStep=0, timeBits=struct.pack("<d", +0.).hex()),
            dict(feStep=1, timeBits=struct.pack("<d", np.nextafter(.1, float("inf"))).hex()),
            dict(feStep=5, timeBits=struct.pack("<d", .5).hex()),
            dict(feStep=-1, timeBits=struct.pack("<d", -.1).hex()),
            dict(feStep=True, timeBits=struct.pack("<d", .1).hex()),
            dict(feStep=0, timeBits=struct.pack("<d", float("nan")).hex()),
            dict(feStep=0, timeBits=struct.pack("<d", float("inf")).hex()),
            dict(feStep=0, timeBits="00000000000000AF"), dict(feStep=0, timeBits="00")]
        for entry in invalid_times:
            self.assertFalse(b.handle({"op": "certificationBatch", "records": [], "times": [entry]})["ok"], entry)
        out_of_range = boundary_record(b, state, 5)
        self.bad(b, {"op": "certificationBatch", "records": [out_of_range], "times": []}, "targetSteps")

    def test_original_clock_fields_are_optional_bound_and_failure_atomic(self):
        b, state, _ = self.initialize(configuration("none"))
        self.ok(b, {"op": "force", "time": .2})
        self.bad(b, {"op": "certificationBatch", "records": [], "times": []}, "targetSteps")
        original = self.ok(b, {"op": "validate", "opaqueState": state})
        for target in (True, 0, -1, 1.5, 1000001):
            invalid = batch_configuration(); invalid["targetSteps"] = target
            self.assertFalse(b.handle(invalid)["ok"])
            self.assertEqual(self.ok(b, {"op": "validate", "opaqueState": state}), original)
        for time_bits in ("short", "FFFFFFFFFFFFFFFF", struct.pack("<d", float("nan")).hex(),
                          struct.pack("<d", float("inf")).hex(), struct.pack("<d", 1.).hex()):
            invalid = batch_configuration(); invalid["initialTimeBits"] = time_bits
            self.assertFalse(b.handle(invalid)["ok"])
        missing = batch_configuration(); missing.pop("initialTime")
        self.assertFalse(b.handle(missing)["ok"])
        first = batch_configuration(target=4); other = batch_configuration(target=5)
        x, xstate, _ = self.initialize(first); y, _, _ = self.initialize(other)
        self.bad(y, {"op": "validate", "opaqueState": xstate}, "identity mismatch")
        changed_bits = batch_configuration(initial=-0.); changed_bits["initialTime"] = 0.
        z, _, _ = self.initialize(changed_bits)
        self.bad(z, {"op": "validate", "opaqueState": xstate}, "identity mismatch")

    def test_certification_batch_held_applied_signed_bytes_and_late_row_atomicity(self):
        message = batch_configuration(); message["actuatorVoltages0"] = [-0.]
        b, state, _ = self.initialize(message)
        first, late = boundary_record(b, state, 0), boundary_record(b, state, 1)
        accepted = self.ok(b, {"op": "certificationBatch", "records": [first, late], "times": []})
        for key in ("heldVoltages", "appliedVoltages"):
            wrong = copy.deepcopy(first); wrong[key] = [+0.]
            response = b.handle({"op": "certificationBatch", "records": [wrong], "times": []})
            self.assertFalse(response["ok"])
            self.assertNotIn("result", response)
        altered = copy.deepcopy(late); altered["opaqueState"]["previousApplied"] = [.1]
        before = copy.deepcopy([first, altered])
        with patch.object(b, "_validate", wraps=b._validate) as validate:
            response = b.handle({"op": "certificationBatch", "records": [first, altered], "times": []})
            self.assertFalse(response["ok"])
            self.assertEqual(validate.call_count, 2)
            self.assertNotIn("result", response)
        self.assertEqual([first, altered], before)
        self.assertEqual(accepted, self.ok(b, {"op": "certificationBatch", "records": [first, late], "times": []}))

    def test_certification_batch_late_force_failure_does_not_publish_partial_rows(self):
        b, state, _ = self.initialize(batch_configuration())
        rows = [boundary_record(b, state, 0), boundary_record(b, state, 1)]
        times = [batch_time(b, step) for step in (0, 1)]
        original_force = b._force
        def fail_late(msg):
            if msg["time"] == .1: raise RuntimeError("late force failure")
            return original_force(msg)
        with patch.object(b, "_force", side_effect=fail_late) as force:
            response = b.handle({"op": "certificationBatch", "records": rows, "times": times})
            self.assertFalse(response["ok"])
            self.assertEqual(force.call_count, 2)
            self.assertNotIn("result", response)
        self.ok(b, {"op": "certificationBatch", "records": rows, "times": times})
        self.ok(b, {"op": "validate", "opaqueState": state})

    def test_certification_batch_final_empty_rechecks_before_and_after_sources(self):
        b, state, _ = self.initialize(batch_configuration())
        request = {"op": "certificationBatch", "records": [], "times": []}
        before = copy.deepcopy(state)
        original_revision = type(b.excitation).check_revision
        revisions = []
        def track_revision(exc):
            revisions.append(exc)
            return original_revision(exc)
        with patch.object(backend_module, "provenance", wraps=backend_module.provenance) as provenance, \
             patch.object(type(b.excitation), "check_revision", track_revision):
            result = self.ok(b, request)
            self.assertEqual(provenance.call_count, 3)  # service + batch start/end
            self.assertEqual(len(revisions), 3)
        self.assertEqual(result["records"], [])
        self.assertEqual(result["forces"], [])
        changed = copy.deepcopy(b.backend_identity); changed["moduleCodeSha256"] = "b"*64
        with patch.object(backend_module, "provenance", side_effect=[b.backend_identity, b.backend_identity, changed]):
            response = b.handle(request)
            self.assertFalse(response["ok"])
            self.assertNotIn("result", response)
        self.assertEqual(state, before)
        self.ok(b, {"op": "validate", "opaqueState": state})
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory)/"backend.py"
            source_path.write_bytes(backend_module._SOURCE_PATH.read_bytes()+b"\n# stale backend\n")
            with patch.object(backend_module, "_SOURCE_PATH", source_path):
                self.bad(b, request, "source changed")

    def test_certification_batch_actual_module_change_after_forces_is_atomic(self):
        b, state, _ = self.initialize(batch_configuration())
        request = {"op": "certificationBatch", "records": [boundary_record(b, state)], "times": [batch_time(b, 0)]}
        before = copy.deepcopy(state)
        original_force = b._force
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory)/"backend.py"
            frozen = backend_module._SOURCE_PATH.read_bytes()
            source_path.write_bytes(frozen)
            def modify_after_force(msg):
                result = original_force(msg)
                source_path.write_bytes(frozen+b"\n# changed after complete force evaluation\n")
                return result
            with patch.object(backend_module, "_SOURCE_PATH", source_path), patch.object(b, "_force", side_effect=modify_after_force):
                response = b.handle(request)
                self.assertFalse(response["ok"])
                self.assertIn("source changed", response["error"])
                self.assertNotIn("result", response)
            self.assertEqual(state, before)
            self.ok(b, {"op": "validate", "opaqueState": state})
            self.ok(b, request)

    def test_certification_batch_actual_external_force_revision_and_retry(self):
        message = batch_configuration()
        cid = message["forceCatalog"][0]["channelId"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"load.py"
            source = f"def load(t):\n    return {{{cid!r}: {{'value': 4+t, 'unit': 'N'}}}}\n".encode()
            path.write_bytes(source)
            external = {"sourcePath": str(path), "functionName": "load", "sourceSha256": hashlib.sha256(source).hexdigest(),
                "dependencyFiles": [], "environment": {"pythonVersion": platform.python_version(), "numpyVersion": np.__version__, "scipyVersion": scipy.__version__},
                "channels": [{"channelId": cid, "unit": "N"}], "missingPolicy": "error", "cacheExactTime": True}
            message["configuration"]["excitation"]["externalFunctions"] = [external]
            b, state, _ = self.initialize(message)
            request = {"op": "certificationBatch", "records": [boundary_record(b, state)],
                       "times": [batch_time(b, 0), batch_time(b, 1)]}
            response = self.ok(b, request)
            assert_allclose([v["forces"][0] for v in response["forces"]], [6., 6.4])
            self.assertEqual(response, self.ok(b, request))
            original_force = b._force
            def change_between_points(msg):
                value = original_force(msg)
                path.write_bytes(source+b"# changed after first point\n")
                return value
            with patch.object(b, "_force", side_effect=change_between_points) as force:
                rejected = b.handle(request)
                self.assertFalse(rejected["ok"])
                self.assertNotIn("result", rejected)
                self.assertIn("SHA256 mismatch", rejected["error"])
                self.assertEqual(force.call_count, 2)  # The second point rechecks the actual file.
            self.assertIsNone(pe._FRAMEWORK_BATCH.get())
            path.write_bytes(source)
            self.assertEqual(response, self.ok(b, request))
            path.write_bytes(source+b"# changed\n")
            self.bad(b, {"op": "certificationBatch", "records": [], "times": []}, "SHA256 mismatch")
            path.write_bytes(source)
            self.assertEqual(response, self.ok(b, request))

    def test_certification_batch_actual_declared_dependency_change_between_points(self):
        message = batch_configuration()
        cid = message["forceCatalog"][0]["channelId"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"load.py"
            dependency = Path(directory)/"range_test_force_helper.py"
            dependency_source = b"def force(t):\n    return 4+t\n"
            dependency.write_bytes(dependency_source)
            source = ("from range_test_force_helper import force\n"
                      f"def load(t):\n    return {{{cid!r}: {{'value': force(t), 'unit': 'N'}}}}\n").encode()
            path.write_bytes(source)
            external = {"sourcePath": str(path), "functionName": "load",
                "sourceSha256": hashlib.sha256(source).hexdigest(),
                "dependencyFiles": [{"path": str(dependency),
                                     "sha256": hashlib.sha256(dependency_source).hexdigest()}],
                "environment": {"pythonVersion": platform.python_version(),
                    "numpyVersion": np.__version__, "scipyVersion": scipy.__version__},
                "channels": [{"channelId": cid, "unit": "N"}],
                "missingPolicy": "error", "cacheExactTime": True}
            message["configuration"]["excitation"]["externalFunctions"] = [external]
            b, state, _ = self.initialize(message)
            request = {"op": "certificationBatch", "records": [boundary_record(b, state)],
                       "times": [batch_time(b, 0), batch_time(b, 1)]}
            response = self.ok(b, request)
            assert_allclose([force["forces"][0] for force in response["forces"]], [6., 6.4])
            original_force = b._force
            force_times = []
            def change_declared_dependency_after_first_point(msg):
                force_times.append(msg["time"])
                value = original_force(msg)
                if len(force_times) == 1:
                    dependency.write_bytes(dependency_source+b"# changed after first point\n")
                return value
            with patch.object(b, "_force", side_effect=change_declared_dependency_after_first_point) as force:
                rejected = b.handle(request)
                self.assertFalse(rejected["ok"])
                self.assertNotIn("result", rejected)
                self.assertIn("SHA256 mismatch", rejected["error"])
                self.assertEqual(force.call_count, 2)
                self.assertEqual(force_times, [0., .1])
            self.assertIsNone(pe._FRAMEWORK_BATCH.get())
            dependency.write_bytes(dependency_source)
            self.assertEqual(response, self.ok(b, request))

    def test_certification_target_does_not_bound_mpc_virtual_forecast(self):
        message = forced_configuration("MPC", N=1, initial_time=0.)
        message.update(targetSteps=1, initialTimeBits=struct.pack("<d", 0.).hex())
        b, state, _ = self.initialize(message)
        proposal = self.ok(b, {"op": "propose", "opaqueState": state, "q": [0.], "v": [0.], "readouts": readouts(0.)})
        self.assertEqual(proposal["candidateOpaqueState"]["controlTick"], 1)
        self.assertGreater(b.control_design.horizon, message["targetSteps"])
        self.bad(b, {"op": "certificationBatch", "records": [], "times": [batch_time(b, 2)]}, "targetSteps")

    def test_generic_source_frame_force_and_moment_scatter(self):
        for family in ("planarBeam", "flatPlate"):
            with self.subTest(family=family):
                message, basis = framed_configuration(family)
                add_direction(message, "EZ", 2.)
                add_direction(message, "RY", -.3)
                bind_directions(message)
                b, _, _ = self.initialize(message)
                expected = np.concatenate((2*basis[:, 2], -.3*basis[:, 1], np.zeros(6)))
                assert_allclose(self.ok(b, {"op": "force", "time": .013})["forces"], expected, rtol=0, atol=0)
                assert_allclose(b._forces_at(.013), [2., -.3], rtol=0, atol=0)

    def test_global_local_alias_force_moment_sum_and_ordered_columns(self):
        message, _ = framed_configuration(rotated=False)
        add_direction(message, "Z", 2., local=False)
        add_direction(message, "EZ", 3.)
        add_direction(message, "RY", -.2, local=False)
        add_direction(message, "RY", -.3)
        bind_directions(message)
        b, _, _ = self.initialize(message)
        assert_allclose(self.ok(b, {"op": "force", "time": .1})["forces"], [0, 0, 5, 0, -.5, 0]+[0]*6)
        assert_allclose(b._forces_at(.1), [2., 3., -.2, -.3])
        assert_allclose(b.Bforce@b._forces_at(.1), [9.])

    def test_all_plate_local_axes_and_partial_global_force_rejection(self):
        message, basis = framed_configuration("flatPlate")
        values = (1., 2., 3., -.2, -.3)
        for dof, value in zip(("EX", "EY", "EZ", "RX", "RY"), values):
            add_direction(message, dof, value)
        for dof in ("X", "Y", "Z"):
            add_direction(message, dof, 1., local=False)
        bind_directions(message)
        b, _, _ = self.initialize(message)
        expected = np.concatenate((basis@np.array(values[:3])+1, basis[:, :2]@np.array(values[3:]), np.zeros(6)))
        assert_allclose(self.ok(b, {"op": "force", "time": 0.})["forces"], expected, atol=1e-15)
        for family, dofs in (("planarBeam", ("X", "Y", "Z", "RX", "RY", "RZ")),
                              ("flatPlate", ("RX", "RY", "RZ"))):
            for dof in dofs:
                with self.subTest(family=family, dof=dof):
                    invalid, _ = framed_configuration(family)
                    add_direction(invalid, dof, 0., local=False)
                    self.bad(NativeBackend(), invalid, "outside source-family supported subspace")

    def test_zero_amplitude_declared_local_family_subspace_rejected(self):
        for family, dofs in (("planarBeam", ("EY", "RX", "RZ")), ("flatPlate", ("RZ",))):
            for dof in dofs:
                invalid, _ = framed_configuration(family)
                add_direction(invalid, dof, 0.)
                self.bad(NativeBackend(), invalid, "unsupported source-family local")
        axial, _ = framed_configuration("axialRod", rotated=False)
        axial["sourceFrame"]["frame"]["origin"] = [0., 0., 0.]
        axial["physicalComponentsPerNode"] = 3
        add_direction(axial, "EX", 0.)
        self.bad(NativeBackend(), axial, "local forces require a persisted layered")

    def test_named_direction_cannot_be_overridden_by_arbitrary_vector(self):
        message, _ = framed_configuration()
        add_direction(message, "EZ", 0.)
        for mutation in ("vector", "global-label", "alias", "index", "missing-vector", "unknown-field"):
            invalid = copy.deepcopy(message)
            record = invalid["forceCatalog"][0]
            if mutation == "vector": record["physicalDirection"][0] += .01
            elif mutation == "global-label": record["coordinateSystem"] = "global"
            elif mutation == "alias": record["channelId"] = "arbitrary-vector"
            elif mutation == "index": record["physicalIndex"] = 2
            elif mutation == "missing-vector": record.pop("physicalDirection")
            else: record["direction"] = [1., 0., 0.]
            self.assertFalse(NativeBackend().handle(invalid)["ok"], mutation)

    def test_frame_must_be_proper_and_matches_native_layout(self):
        message, _ = framed_configuration()
        add_direction(message, "EZ", 1.)
        for mutation in ("reflection", "skew", "nonfinite", "family", "layout", "field", "null"):
            invalid = copy.deepcopy(message)
            frame = invalid["sourceFrame"]["frame"]
            if mutation == "reflection": frame["ey"] = [-v for v in frame["ey"]]
            elif mutation == "skew": frame["ex"][0] += .01
            elif mutation == "nonfinite": frame["origin"][0] = float("nan")
            elif mutation == "family": invalid["sourceFrame"]["family"] = "3D-solid"
            elif mutation == "layout": invalid["physicalComponentsPerNode"] = 3
            elif mutation == "field": frame["vector"] = [1., 2., 3.]
            else: invalid["sourceFrame"] = None
            self.assertFalse(NativeBackend().handle(invalid)["ok"], mutation)

    def test_ordered_direction_matrix_is_certified_and_failure_atomic(self):
        message, _ = framed_configuration()
        add_direction(message, "EZ", 2.)
        add_direction(message, "RY", -.3)
        bind_directions(message)
        b, state, _ = self.initialize(message)
        original = self.ok(b, {"op": "force", "time": .12})
        for mutation in ("column-swap", "vector", "shape"):
            invalid = copy.deepcopy(message)
            if mutation == "column-swap": invalid["physicalForceDirections"] = np.asarray(invalid["physicalForceDirections"])[:, ::-1].tolist()
            elif mutation == "vector": invalid["physicalForceDirections"][0][0] += .01
            else: invalid["physicalForceDirections"].pop()
            self.bad(b, invalid, "physicalForceDirections")
            self.assertEqual(original, self.ok(b, {"op": "force", "time": .12}))
            self.ok(b, {"op": "validate", "opaqueState": state})

    def test_restart_identity_freezes_frame_origin_maps_and_source_configuration(self):
        message, _ = framed_configuration()
        add_direction(message, "EZ", 2.)
        bind_directions(message)
        b, state, _ = self.initialize(message)
        fresh, _, _ = self.initialize(copy.deepcopy(message))
        self.ok(fresh, {"op": "validate", "opaqueState": state})
        for mutation in ("origin", "channels", "source"):
            changed = copy.deepcopy(message)
            if mutation == "origin": changed["sourceFrame"]["frame"]["origin"][0] += .1
            elif mutation == "channels": changed["configuration"]["excitation"]["channels"][0]["terms"][0]["value"] += .1
            else: changed["sourceFingerprint"] = "b"*64
            other, _, _ = self.initialize(changed)
            self.bad(other, {"op": "validate", "opaqueState": state}, "identity mismatch")

    def test_local_builtin_external_same_channel_and_global_alias_sum(self):
        message, _ = framed_configuration(rotated=False)
        add_direction(message, "Z", 2., local=False)
        record = add_direction(message, "EZ", 3.)
        cid = record["channelId"]
        bind_directions(message)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"load.py"
            source = f"def load(t):\n    return {{{cid!r}: {{'value': 4+t, 'unit': 'N'}}}}\n".encode()
            path.write_bytes(source)
            external = {"sourcePath": str(path), "functionName": "load", "sourceSha256": hashlib.sha256(source).hexdigest(),
                "dependencyFiles": [], "environment": {"pythonVersion": platform.python_version(), "numpyVersion": np.__version__, "scipyVersion": scipy.__version__},
                "channels": [{"channelId": cid, "unit": "N"}], "missingPolicy": "error", "cacheExactTime": True}
            message["configuration"]["excitation"]["externalFunctions"] = [external]
            b, state, _ = self.initialize(message)
            endpoint = self.ok(b, {"op": "force", "time": .013})
            assert_allclose(endpoint["forces"], [0., 0., 9.013]+[0.]*9)
            assert_allclose(b._forces_at(.013), [2., 7.013])
            self.assertEqual(endpoint, self.ok(b, {"op": "force", "time": .013}))
            fresh, _, _ = self.initialize(copy.deepcopy(message))
            self.ok(fresh, {"op": "validate", "opaqueState": state})
            path.write_bytes(source+b"# stale\n")
            self.bad(b, {"op": "force", "time": .013}, "SHA256 mismatch")
            self.assertEqual(state["feStep"], 0)

    def sensor_configuration(self, forced=False):
        message = forced_configuration("none", N=1) if forced else configuration("none")
        message.update(inputs=[], B=[[]], D=[[]], actuatorVoltages0=[])
        return message

    def test_pure_sensor_none_modal_force_feedthrough_retry_and_restart(self):
        for forced in (False, True):
            message = self.sensor_configuration(forced)
            if forced:
                message.update(Lq=[[0.]], La=[[1.]])
                message["outputs"] = [["sensor:s", "m/s^2"]]
            b, state, init = self.initialize(message)
            self.assertEqual(init["design"]["continuous"]["B"], [[], []])
            self.assertEqual(init["design"]["selectedModes"], [0])
            request = {"op": "propose", "opaqueState": state}
            proposal = self.ok(b, request)
            self.assertEqual(proposal["absoluteVoltages"], [])
            self.assertEqual(proposal, self.ok(b, request))
            candidate = proposal["candidateOpaqueState"]
            accepted = self.ok(b, {"op": "postFE", "opaqueState": candidate,
                                   "appliedVoltages": [], "feStep": 1})["candidateOpaqueState"]
            self.assertEqual(state["feStep"], 0)
            self.assertEqual(accepted["feStep"], 1)
            self.assertEqual(accepted["previousApplied"], [])
            fresh, _, _ = self.initialize(message)
            self.ok(fresh, {"op": "validate", "opaqueState": accepted})
            self.assertEqual(self.ok(b, {"op": "propose", "opaqueState": accepted}),
                             self.ok(fresh, {"op": "propose", "opaqueState": accepted}))
            if forced:
                # Physical qddot: La M^-1 Bforce + independently supplied Dforce.
                assert_allclose(init["design"]["forceAware"]["FE"]["continuous"]["D"], [[2.7]])
                self.assertEqual(init["design"]["forceAware"]["source"]["inputs"], [["force:7:X", "N"]])
                self.assertEqual(self.ok(b, {"op": "force", "time": 1.7})["forces"][0], 7.1)
            changed = copy.deepcopy(message)
            changed["sourceFingerprint"] = "b"*64
            other, _, _ = self.initialize(changed)
            self.bad(other, {"op": "validate", "opaqueState": accepted}, "identity mismatch")

    def test_pure_sensor_rejects_every_active_controller(self):
        for kind in ("velocity_feedback", "PI", "PD", "PID", "discrete_state_space", "LQR", "MPC"):
            message = configuration(kind)
            message.update(inputs=[], B=[[]], D=[[]], actuatorVoltages0=[])
            self.bad(NativeBackend(), message, "requires at least one voltage actuator")

    def test_accepted_held_request_binary64_all_phases_and_slew_masked_tamper(self):
        message = configuration("PI", N=3)
        b, state, _ = self.initialize(message)
        self.ok(b, {"op": "validate", "opaqueState": state, "heldVoltages": [5.]})
        proposal = self.proposal(b, state, value=1.)
        held = proposal["absoluteVoltages"]
        state = proposal["candidateOpaqueState"]
        self.ok(b, {"op": "validate", "opaqueState": state, "phase": "proposed", "heldVoltages": held})
        for step in range(3):
            # Rate limit masks different future held requests at this boundary.
            applied = [5.-.01*(step+1)]
            state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": applied})["candidateOpaqueState"]
            self.ok(b, {"op": "validate", "opaqueState": state,
                        "appliedVoltages": applied, "heldVoltages": held})
            tampered = copy.deepcopy(state)
            tampered["heldDelta"][0] -= .2
            self.bad(b, {"op": "validate", "opaqueState": tampered,
                         "appliedVoltages": applied, "heldVoltages": held}, "heldDelta differs")
        for invalid in ([True], [], [float("inf")]):
            self.bad(b, {"op": "validate", "opaqueState": state, "heldVoltages": invalid})
        message = configuration("none")
        message["actuatorVoltages0"] = [-0.]
        b, state, _ = self.initialize(message)
        self.ok(b, {"op": "validate", "opaqueState": state, "heldVoltages": [-0.]})
        self.bad(b, {"op": "validate", "opaqueState": state, "heldVoltages": [0.]}, "heldDelta differs")
        proposal = self.ok(b, {"op": "propose", "opaqueState": state})
        state = proposal["candidateOpaqueState"]
        self.ok(b, {"op": "validate", "opaqueState": state, "phase": "proposed",
                    "heldVoltages": proposal["absoluteVoltages"]})
        self.bad(b, {"op": "validate", "opaqueState": state, "phase": "proposed",
                     "heldVoltages": [-0.]}, "heldDelta differs")

    def clock_configuration(self, kind="LQR", initial_time=1000., dt=.1, N=3, **controller):
        if kind == "LQR":
            controller = dict(stateSource="sensor_only", observer={"processCovariance": [[.1, 0.], [0., .1]],
                "measurementCovariance": [[.01]]}, **controller)
        message = forced_configuration(kind, N=N, initial_time=initial_time, **controller)
        message.update(M=[[1.]], K=[[0.]], C=[[0.]], B=[[1.]], Bforce=[[1.]],
                       D=[[0.]], Dforce=[[0.]], dt=dt, biasForce=[0.])
        message["configuration"]["excitation"]["channels"][0]["terms"] = [{"kind": "constant", "value": 2.}]
        return message

    def test_large_canonical_interval_observer_and_mpc_closed_form(self):
        b, state, init = self.initialize(self.clock_configuration("none", initial_time=1e16, dt=3., N=1))
        t0, t1, system, foh = b._interval(0)
        # Native double clock: (1e16+3)-1e16 = 4, not the nominal 3.
        assert_allclose(b._known_offset(t0, t1, foh), [16., 8.], atol=1e-13)
        assert_allclose(system.B[:, 0], [8., 4.], atol=1e-13)
        self.assertIn("nominal-period-LQR-Kalman-gain-design", init["design"]["samplingContract"])
        message = self.clock_configuration("MPC", initial_time=1e16, dt=3., N=1,
            horizon=1, Q=[[0., 0.], [0., 0.]], Qf=[[1., 0.], [0., 1.]], R=[[1.]],
            voltageLower=[-100.], voltageUpper=[100.], deltaVoltageLimits=[100.])
        self.bad(NativeBackend(), message, "sampling period quantization")
        for kind in ("LQR", "MPC"):
            for N in (3, 4):
                self.bad(NativeBackend(), self.clock_configuration(kind, initial_time=1e12, dt=.1, N=N),
                         "sampling period quantization")
        unstable = self.clock_configuration(initial_time=1e16, dt=7., N=1, Q=[[1., 0.], [0., 1.]], R=[[.01]])
        unstable["K"] = [[1.]]
        self.bad(NativeBackend(), unstable, "sampling period quantization")
        self.bad(NativeBackend(), self.clock_configuration("discrete_state_space", initial_time=1e12, N=3),
                 "sampling period quantization")
        self.bad(NativeBackend(), self.clock_configuration("MPC", initial_time=0., N=20001, horizon=1),
                 "20000 actual FE intervals")

    def test_clock_origins_actual_slew_all_phase_split_restart(self):
        for origin, N in ((1000., 3), (1000., 4)):
            message = self.clock_configuration(initial_time=origin, N=N)
            b, state, _ = self.initialize(message)
            states, actions, intervals = [state], [], []
            for step in range(2*N):
                if step % N == 0:
                    p = self.proposal(b, state, value=.02*(step+1))
                    state = p["candidateOpaqueState"]
                    held = np.clip(p["absoluteVoltages"], [4.], [6.])
                applied = np.clip(held, np.array(state["previousApplied"])-.025,
                                  np.array(state["previousApplied"])+.025).tolist()
                h = (origin+(step+1)*.1)-(origin+step*.1)
                intervals.append(h)
                q, v = state["observerPrior"]
                acceleration = applied[0]-5+2
                exact = [q+h*v+.5*h*h*acceleration, v+h*acceleration]
                state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": applied})["candidateOpaqueState"]
                assert_allclose(state["observerPrior"], exact, atol=1e-13)
                states.append(state)
                actions.append(applied)
            self.assertTrue(any(h != .1 for h in intervals))
            for split in range(2*N):
                fresh, _, _ = self.initialize(message)
                state = json.loads(json.dumps(states[split]))
                for step in range(split, 2*N):
                    if step % N == 0:
                        state = self.proposal(fresh, state, value=.02*(step+1))["candidateOpaqueState"]
                    state = self.ok(fresh, {"op": "postFE", "opaqueState": state,
                                          "appliedVoltages": actions[step]})["candidateOpaqueState"]
                    self.assertEqual(state, states[step+1])

    def test_mpc_forecast_keeps_pulse_inside_control_interval(self):
        message = self.clock_configuration("MPC", initial_time=0., dt=.1, N=10, horizon=1,
            Q=[[0., 0.], [0., 0.]], Qf=[[1., 0.], [0., 1.]], R=[[1.]],
            voltageLower=[-100.], voltageUpper=[100.], deltaVoltageLimits=[100.])
        message["configuration"]["excitation"]["channels"][0]["terms"] = [{"kind": "smoothPulse",
            "amplitude": 1., "startTime": 0., "riseTime": .2, "holdTime": .6, "fallTime": .2}]
        b, state, _ = self.initialize(message)
        p = self.ok(b, {"op": "propose", "opaqueState": state, "q": [0.], "v": [0.]})
        # FE endpoint samples 0,.5,1,...,1,.5,0; symmetric FOH pulse has
        # impulse .8 and first moment .4 although both control endpoints are 0.
        assert_allclose(p["diagnostics"]["mpc"]["stateOffsets"], [[.4, .8]], atol=1e-14)
        assert_allclose(p["absoluteVoltages"], [5.-4/9], atol=1e-10)

    def test_collapsed_clock_interval_fails_closed_even_without_observer(self):
        message = self.clock_configuration("none", initial_time=1e16, dt=.1, N=2)
        self.bad(NativeBackend(), message, "actual native clock interval")
        message["dt"] = 1.1  # First interval rounds to 2; second collapses to 0.
        b, state, _ = self.initialize(message)
        p = self.proposal(b, state)
        state = self.accept(b, p, [5.])
        before = copy.deepcopy(state)
        self.bad(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": [5.]}, "actual native clock interval")
        self.assertEqual(state, before)

    def test_double_integrator_foh_and_affine_closed_form(self):
        message = forced_configuration("none", N=3)
        message.update(M=[[1.]], K=[[0.]], C=[[0.]], B=[[1.]], Bforce=[[1.]], biasForce=[3.])
        _, _, init = self.initialize(message)
        package = init["design"]["forceAware"]
        dt = message["dt"]
        foh = package["foh"]["ROMsubstep"]
        assert_allclose(foh["leftForce"], [[dt*dt/3], [dt/2]], atol=1e-16)
        assert_allclose(foh["rightForce"], [[dt*dt/6], [dt/2]], atol=1e-16)
        assert_allclose(foh["affineIntegral"], [1.5*dt*dt, 3*dt], atol=1e-16)
        f0, f1 = 2+3*1.7, 2+3*1.8
        predicted = np.array(foh["leftForce"])[:, 0]*f0+np.array(foh["rightForce"])[:, 0]*f1+foh["affineIntegral"]
        assert_allclose(predicted, [dt*dt*(f0/3+f1/6+1.5), dt*((f0+f1)/2+3)], atol=1e-15)
        self.assertEqual(package["source"]["inputs"], [["actuator:a", "V"], ["force:7:X", "N"]])

    def test_physical_acceleration_direct_terms_bias_and_sample_correction(self):
        message = forced_configuration("LQR", N=1, stateSource="sensor_only",
            observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]})
        message["La"] = [[.1]]
        b, state, init = self.initialize(message)
        package = init["design"]["forceAware"]
        # Native M=2,BVa=3,Bf=4: DVa=.2+.1*3/2, Df=.7+.1*4/2.
        assert_allclose(package["FE"]["continuous"]["D"], [[.35, .9]], atol=1e-15)
        assert_allclose(package["ROM"]["continuous"]["D"], [[.35, .9]], atol=1e-15)
        assert_allclose(package["ROM"]["outputBias"], [.03], atol=1e-15)
        f0 = 2+3*message["initialTime"]
        p = self.proposal(b, state, value=.9*f0+.03)
        assert_allclose(p["diagnostics"]["innovation"], [0.], atol=1e-15)
        state = self.accept(b, p, [5.])
        f1 = 2+3*(message["initialTime"]+.1)
        # Expected native output from the restricted physical state plus actual
        # voltage/force feedthrough and constant affine acceleration.
        cy = np.array(package["ROM"]["continuous"]["Cy"])
        sample = (cy@np.array(state["observerPrior"]))[0]+.9*f1+.03
        p = self.proposal(b, state, value=sample)
        assert_allclose(p["diagnostics"]["innovation"], [0.], atol=1e-14)

    def test_partial_basis_retains_actual_native_high_mode_direct_terms(self):
        message = forced_configuration("LQR", N=1, stateSource="sensor_only",
            observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]})
        message.update(M=[[1., 0.], [0., 1.]], K=[[4., 0.], [0., 9.]], C=[[.2, 0.], [0., .3]],
            B=[[1.], [2.]], Bforce=[[3.], [4.]], biasForce=[5., 6.],
            Lq=[[1., 0.]], Lv=[[0., 0.]], La=[[0., 2.]], dofs=[["node:7:X", "m"], ["node:12:X", "m"]])
        message["configuration"]["design"] = {"modeIndices": [0]}
        b, state, init = self.initialize(message)
        package = init["design"]["forceAware"]
        assert_allclose(package["ROM"]["continuous"]["D"], [[4.2, 8.7]], atol=1e-14)
        assert_allclose(package["ROM"]["continuous"]["Cy"], [[1., 0.]], atol=1e-14)
        assert_allclose(package["ROM"]["outputBias"], [12.])
        assert_allclose(package["ROM"]["affineDerivative"], [0., 5.])
        self.assertEqual(package["outputContract"], "projected-full-FE-readout-with-retained-direct-terms")
        p = self.proposal(b, state, value=8.7*(2+3*1.7)+12.)
        assert_allclose(p["diagnostics"]["innovation"], [0.], atol=1e-14)
        # La*Phi is zero here: a projected derivative interpretation would lose
        # all 8 N-to-output direct response through the discarded second mode.
        assert_allclose(np.array(message["La"])@np.array(init["design"]["Phi"]), [[0.]])

    def test_full_basis_force_package_exact_coordinate_equivalence(self):
        message = forced_configuration("none")
        message.update(M=[[2., .2], [.2, 3.]], K=[[8., -1.], [-1., 5.]], C=[[.4, .1], [.1, .5]],
            B=[[3.], [1.]], Bforce=[[4.], [-2.]], biasForce=[.6, -.2],
            Lq=[[1., 2.]], Lv=[[.3, -.2]], La=[[.1, .4]], dofs=[["node:7:X", "m"], ["node:12:X", "m"]])
        _, _, init = self.initialize(message)
        package = init["design"]["forceAware"]
        phi = np.array(init["design"]["Phi"])
        z = np.zeros_like(phi)
        transform = np.block([[phi, z], [z, phi]])
        for form in ("continuous", "discrete", "substep"):
            fe, rom = package["FE"][form], package["ROM"][form]
            assert_allclose(transform@rom["A"], np.array(fe["A"])@transform, atol=2e-14)
            assert_allclose(transform@rom["B"], fe["B"], atol=2e-14)
            assert_allclose(rom["Cy"], np.array(fe["Cy"])@transform, atol=2e-14)
            assert_allclose(rom["D"], fe["D"], atol=0)
        assert_allclose(transform@package["ROM"]["affineDerivative"], package["FE"]["affineDerivative"], atol=2e-14)

    def test_forced_mpc_forecast_native_double_integrator_analytic(self):
        message = forced_configuration("MPC", N=1, horizon=1, Q=[[0., 0.], [0., 0.]],
            R=[[1.]], Qf=[[1., 0.], [0., 1.]], voltageLower=[-100.], voltageUpper=[100.], deltaVoltageLimits=[100.])
        message.update(M=[[1.]], K=[[0.]], C=[[0.]], B=[[1.]], Bforce=[[1.]],
                       dt=1., biasForce=[0.])
        message["configuration"]["excitation"]["channels"][0]["terms"] = [{"kind": "constant", "value": 2.}]
        b, state, _ = self.initialize(message)
        p = self.ok(b, {"op": "propose", "opaqueState": state, "q": [0.], "v": [0.]})
        # x1=[.5*u+1,u+2]; J=u^2+(.5*u+1)^2+(u+2)^2.
        assert_allclose(p["absoluteVoltages"], [5.-2.5/2.25], atol=1e-10)
        assert_allclose(p["diagnostics"]["mpc"]["stateOffsets"], [[1., 2.]], atol=1e-15)

    def test_forced_observer_actual_slew_every_split_restart_nonzero_time(self):
        for kind, N in (("LQR", 3), ("LQR", 4), ("MPC", 3), ("MPC", 4)):
            message = forced_configuration(kind, N=N, initial_time=2.3, stateSource="sensor_only",
                observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]},
                voltageLower=[4.], voltageUpper=[6.], deltaVoltageLimits=[.2])
            message["configuration"]["excitation"]["channels"][0]["terms"] = [
                {"kind": "expression", "expression": "0.2+0.3*t"}]
            b, state, init = self.initialize(message)
            package = init["design"]["forceAware"]
            a, inputs = (np.array(init["design"]["substep"][key]) for key in ("A", "B"))
            left, right, bias = (np.array(package["foh"]["ROMsubstep"][key])
                                  for key in ("leftForce", "rightForce", "affineIntegral"))
            states, actions = [state], []
            for step in range(2*N):
                if step % N == 0:
                    p = self.proposal(b, state, value=.01*(step+1))
                    state = p["candidateOpaqueState"]
                    held = np.clip(p["absoluteVoltages"], [4.], [6.])
                applied = np.clip(held, np.array(state["previousApplied"])-.025,
                                  np.array(state["previousApplied"])+.025).tolist()
                t0, t1 = 2.3+step*.1, 2.3+(step+1)*.1
                expected = a@state["observerPrior"]+inputs@(np.array(applied)-5)+left@[.2+.3*t0]+right@[.2+.3*t1]+bias
                state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": applied})["candidateOpaqueState"]
                assert_allclose(state["observerPrior"], expected, atol=1e-14)
                states.append(state)
                actions.append(applied)
            self.assertNotEqual(actions[0], actions[1])
            for split in range(2*N):
                fresh, _, _ = self.initialize(message)
                state = json.loads(json.dumps(states[split]))
                for step in range(split, 2*N):
                    if step % N == 0:
                        state = self.proposal(fresh, state, value=.01*(step+1))["candidateOpaqueState"]
                    state = self.ok(fresh, {"op": "postFE", "opaqueState": state,
                                          "appliedVoltages": actions[step]})["candidateOpaqueState"]
                    self.assertEqual(state, states[step+1])

    def test_sinusoidal_source_foh_time_convergence_not_exact_waveform(self):
        errors = []
        for dt in (.1, .05):
            message = forced_configuration("LQR", N=100, initial_time=0., stateSource="sensor_only",
                observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]})
            message.update(M=[[1.]], K=[[0.]], C=[[0.]], B=[[1.]], Bforce=[[1.]],
                           dt=dt, biasForce=[0.])
            message["configuration"]["excitation"]["channels"][0]["terms"] = [
                {"kind": "sine", "amplitude": 1., "frequencyHz": 1/np.pi, "phaseRad": 0.}]
            b, state, _ = self.initialize(message)
            state = self.proposal(b, state, value=0.)["candidateOpaqueState"]
            for _ in range(round(1/dt)):
                state = self.ok(b, {"op": "postFE", "opaqueState": state,
                                    "appliedVoltages": [5.]})["candidateOpaqueState"]
            # Independent continuous double-integrator response to sin(2*t).
            exact = np.array([.5-np.sin(2)/4, (1-np.cos(2))/2])
            errors.append(np.linalg.norm(np.array(state["observerPrior"])-exact))
        self.assertGreater(errors[0], 0.)
        self.assertGreater(errors[0]/errors[1], 3.8)
        self.assertLess(errors[0]/errors[1], 4.2)

    def test_force_operators_missing_strict_mapping_units_and_identity(self):
        for kind, state_source in (("MPC", "full_fe"), ("MPC", "sensor_only"), ("LQR", "sensor_only")):
            message = forced_configuration(kind, stateSource=state_source,
                **({"observer": {"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]}}
                   if state_source == "sensor_only" else {}))
            for key in ("Bforce", "Dforce", "disturbanceInputs", "initialTime"):
                del message[key]
            del message["forceCatalog"][0]["disturbanceColumn"]
            self.bad(NativeBackend(), message, "requires native force operators")
        # Legacy full-FE LQR may regulate a physical unmodeled disturbance,
        # while its package explicitly records the incomplete force mapping.
        legacy = forced_configuration("LQR")
        for key in ("Bforce", "Dforce", "disturbanceInputs", "initialTime"):
            del legacy[key]
        del legacy["forceCatalog"][0]["disturbanceColumn"]
        _, _, result = self.initialize(legacy)
        self.assertEqual(result["design"]["forceAware"]["qualification"], "actuator-only-unmapped-forcing")
        for mutate in (lambda m: m.pop("Dforce"),
                       lambda m: m.__setitem__("Bforce", [[1., 2.]]),
                       lambda m: m.__setitem__("biasForce", [0., 0.]),
                       lambda m: m["disturbanceInputs"][0].__setitem__(1, "N m"),
                       lambda m: m["disturbanceInputs"][0].__setitem__(0, "unknown"),
                       lambda m: m["forceCatalog"][0].__setitem__("disturbanceColumn", 1),
                       lambda m: m.__setitem__("initialTime", True)):
            message = forced_configuration("none")
            mutate(message)
            self.bad(NativeBackend(), message)
        message = forced_configuration("none")
        message["forceCatalog"].append({"channelId": "moment:12:RZ", "physicalNodeId": 12,
            "dof": "RZ", "unit": "N m", "physicalIndex": 11, "disturbanceColumn": 0})
        message["configuration"]["excitation"]["channels"].append({"channelId": "moment:12:RZ", "unit": "N m",
            "terms": [{"kind": "constant", "value": 1.}]})
        message.update(Bforce=[[4., 2.]], Dforce=[[.7, .3]],
                       disturbanceInputs=[["force:7:X", "N"], ["moment:12:RZ", "N m"]])
        self.bad(NativeBackend(), message, "unique and contiguous")
        message["forceCatalog"][1]["disturbanceColumn"] = 1
        b, state, _ = self.initialize(message)
        changed = copy.deepcopy(message)
        changed["Bforce"][0][1] = 3.
        other, _, _ = self.initialize(changed)
        self.bad(other, {"op": "validate", "opaqueState": state}, "identity")

    def test_known_force_failure_keeps_observer_mpc_state_unchanged(self):
        for kind in ("LQR", "MPC"):
            message = forced_configuration(kind, N=1, initial_time=.9,
                **({"stateSource": "sensor_only", "observer": {"processCovariance": [[.1, 0.], [0., .1]],
                    "measurementCovariance": [[.01]]}} if kind == "LQR" else {}))
            message["configuration"]["excitation"]["channels"][0]["terms"] = [{"kind": "expression", "expression": "sqrt(1-t)"}]
            if kind == "LQR":
                message["dt"] = .2
            b, state, _ = self.initialize(message)
            before = copy.deepcopy(state)
            if kind == "MPC":
                request = {"op": "propose", "opaqueState": state, "q": [0.], "v": [0.]}
                self.bad(b, request, "excitation evaluation")
                self.bad(b, request, "excitation evaluation")
            else:
                p = self.proposal(b, state, value=0.)
                pending_before = copy.deepcopy(p["candidateOpaqueState"])
                request = {"op": "postFE", "opaqueState": p["candidateOpaqueState"], "appliedVoltages": [5.]}
                self.bad(b, request, "excitation evaluation")
                self.assertEqual(p["candidateOpaqueState"], pending_before)
            self.assertEqual(state, before)

    def initialize(self, message=None):
        backend = NativeBackend()
        result = self.ok(backend, configuration() if message is None else message)
        return backend, result["initialOpaqueState"], result

    def ok(self, backend, message):
        result = backend.handle(message)
        self.assertTrue(result["ok"], result.get("error"))
        return result["result"]

    def bad(self, backend, message, fragment=None):
        result = backend.handle(message)
        self.assertFalse(result["ok"], result)
        self.assertNotIn("result", result)
        if fragment:
            self.assertIn(fragment, result["error"])
        return result

    def proposal(self, backend, state, value=1., **extra):
        return self.ok(backend, dict(op="propose", opaqueState=state, readouts=readouts(value), **extra))

    def accept(self, backend, proposal, applied=None):
        return self.ok(backend, {"op": "postFE", "opaqueState": proposal["candidateOpaqueState"],
            "appliedVoltages": proposal["absoluteVoltages"] if applied is None else applied})["candidateOpaqueState"]

    def test_pi_old_integral_deterministic_rollback(self):
        b, state, _ = self.initialize()
        before = copy.deepcopy(state)
        p = self.proposal(b, state)
        self.assertEqual(state, before)
        self.assertEqual(p, self.proposal(b, state))
        assert_allclose(p["absoluteVoltages"], [3.])
        assert_allclose(p["candidateOpaqueState"]["integral"], [-.1])
        # Failed physical publish means the native caller retains the old state.
        accepted = self.accept(b, p)
        assert_allclose(self.proposal(b, accepted)["absoluteVoltages"], [2.7])
        self.assertEqual(self.proposal(b, state), p)
        self.assertEqual(state, before)

    def test_multirate_split_replay_at_every_phase(self):
        message = configuration(N=3)
        b, initial, _ = self.initialize(message)
        states = [initial]
        actions = []
        for step in range(9):
            state = states[-1]
            if step % 3 == 0:
                proposal = self.proposal(b, state, value=1+step*.1)
                action = proposal["absoluteVoltages"]
                state = proposal["candidateOpaqueState"]
            actions.append(action)
            state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": action,
                                "feStep": step+1})["candidateOpaqueState"]
            self.ok(b, {"op": "validate", "opaqueState": state, "feStep": step+1,
                        "controlTick": (step+3)//3})
            states.append(state)
        for split in range(9):
            fresh, _, _ = self.initialize(message)
            state = json.loads(json.dumps(states[split]))
            self.ok(fresh, {"op": "validate", "opaqueState": state, "feStep": split})
            for step in range(split, 9):
                if step % 3 == 0:
                    proposal = self.proposal(fresh, state, value=1+step*.1)
                    state = proposal["candidateOpaqueState"]
                    self.assertEqual(proposal["absoluteVoltages"], actions[step])
                state = self.ok(fresh, {"op": "postFE", "opaqueState": state,
                                       "appliedVoltages": actions[step]})["candidateOpaqueState"]
                self.assertEqual(state, states[step+1])

    def test_pd_pid_explicit_backward_filter(self):
        for kind in ("PD", "PID"):
            b, state, _ = self.initialize(configuration(kind))
            first = self.proposal(b, state)
            assert_allclose(first["absoluteVoltages"], [5-2-.4*7.5])
            assert_allclose(first["candidateOpaqueState"]["filteredDerivative"], [-7.5])
            state = self.accept(b, first)
            second = self.proposal(b, state)
            expected = 5-2-.4*1.875-(.3 if kind == "PID" else 0.)
            assert_allclose(second["absoluteVoltages"], [expected])
            message = configuration(kind)
            del message["configuration"]["controller"]["derivativeFilterAlpha"]
            self.bad(b, message)

    def test_velocity_feedback_and_unit_contract(self):
        b, state, _ = self.initialize(configuration("velocity_feedback"))
        p = self.ok(b, {"op": "propose", "opaqueState": state, "readouts": readouts(2., "m/s")})
        assert_allclose(p["absoluteVoltages"], [1.])
        message = configuration("velocity_feedback")
        message["outputs"][0][1] = "m"
        self.bad(b, message, "velocity readout")

    def test_discrete_state_space_old_state_output(self):
        b, state, _ = self.initialize(configuration("discrete_state_space"))
        p = self.proposal(b, state)
        assert_allclose(p["absoluteVoltages"], [10.])  # 5+2*4-3
        assert_allclose(p["candidateOpaqueState"]["controllerState"], [3.1])
        self.assertEqual(state["controllerState"], [4.])
        message = configuration("discrete_state_space")
        message["configuration"]["controller"]["samplingPeriod"] = .2
        self.bad(b, message, "samplingPeriod")

    def test_actual_fe_modes_lqr_and_projection(self):
        b, state, init = self.initialize(configuration("LQR"))
        assert_allclose(init["design"]["omega"], [2.])
        phi = np.array(init["design"]["Phi"])
        assert_allclose(phi.T@np.array([[2.]])@phi, [[1.]])
        a = np.array(init["design"]["discrete"]["A"])
        inputs = np.array(init["design"]["discrete"]["B"])
        d = init["design"]["controller"]
        message = configuration("LQR")
        library_modes = compute_modes(message["M"], message["K"], source=SourceIdentity(
            message["sourceFingerprint"], message["dofs"], message["inputs"], message["outputs"]))
        self.assertEqual(init["design"]["clusters"], [list(c) for c in library_modes.clusters])
        gain, P = np.array(d["K"]), np.array(d["P"])
        Q, R = np.diag([2., 1.]), np.array([[.5]])
        assert_allclose(a.T@P@a-P-a.T@P@inputs@gain+Q, np.zeros((2, 2)), atol=1e-10)
        self.assertLess(d["riccatiRelativeResidual"], 1e-10)
        self.assertTrue(all(np.linalg.norm(pole) < 1 for pole in d["closedPoles"]))
        p = self.ok(b, {"op": "propose", "opaqueState": state, "q": [.3], "v": [-.2]})
        x = np.concatenate((phi.T@np.array([[2.]])@[.3], phi.T@np.array([[2.]])@[-.2]))
        assert_allclose(p["absoluteVoltages"], 5-gain@x)

    def test_actual_mpc_kkt_constraints_and_warm_history(self):
        b, state, _ = self.initialize(configuration("MPC"))
        original = copy.deepcopy(state)
        p = self.ok(b, {"op": "propose", "opaqueState": state, "q": [4.], "v": [1.]})
        self.assertEqual(state, original)
        self.assertLessEqual(abs(p["absoluteVoltages"][0]-5), .3+1e-10)
        self.assertGreaterEqual(p["absoluteVoltages"][0], 4.)
        c = p["diagnostics"]["mpc"]
        self.assertLess(max(c[key] for key in ("primal", "stationarity", "dual", "complementarity", "actionStationarity")), 1e-6)
        self.assertNotEqual(p["candidateOpaqueState"]["warmStart"], state["warmStart"])
        self.assertEqual(p, self.ok(b, {"op": "propose", "opaqueState": state, "q": [4.], "v": [1.]}))
        # A QP failure returns no candidate or fallback and consumes no history.
        with patch("range_workflow.piezo_native_backend.control.mpc_proposal", side_effect=ValueError("QP failure")):
            self.bad(b, {"op": "propose", "opaqueState": state, "q": [4.], "v": [1.]}, "QP failure")
        self.assertEqual(state, original)

    def observer_configuration(self, N=3):
        return configuration("LQR", N=N, stateSource="sensor_only", selectedOutputs=["sensor:s"],
            observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]},
            voltageLower=[4.], voltageUpper=[6.], deltaVoltageLimits=[.2])

    def test_sensor_only_clipped_actual_increment_each_substep(self):
        b, state, init = self.initialize(self.observer_configuration())
        p = self.proposal(b, state, value=3.)
        self.assertLess(p["absoluteVoltages"][0], 4.8)
        corrected = np.array(p["candidateOpaqueState"]["observerPrior"])
        applied = [4.8]
        state = self.accept(b, p, applied)
        a, inputs = (np.array(init["design"]["substep"][key]) for key in ("A", "B"))
        assert_allclose(state["observerPrior"], a@corrected+inputs@[-.2], atol=1e-14)
        # Held substeps predict again using actual -0.2 V increment, no correction.
        for step in (2, 3):
            old = np.array(state["observerPrior"])
            state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": applied,
                                "feStep": step})["candidateOpaqueState"]
            assert_allclose(state["observerPrior"], a@old+inputs@[-.2], atol=1e-14)
        # Feedthrough uses last actually applied increment, not desired command.
        next_p = self.proposal(b, state, value=.5)
        cy = np.array(init["design"]["discrete"]["Cy"])
        assert_allclose(next_p["diagnostics"]["innovation"], [.5]-cy@np.array(state["observerPrior"])-np.array([[.2]])@[-.2])

    def test_sensor_only_no_fe_access_and_selected_action_echo(self):
        b, state, _ = self.initialize(self.observer_configuration())
        self.bad(b, {"op": "propose", "opaqueState": state, "q": [123.], "v": [456.],
                     "readouts": readouts(1.)}, "cannot receive FE")
        record = readouts(1.)
        record[0]["actionRecord"] = True
        self.bad(b, {"op": "propose", "opaqueState": state, "readouts": record}, "independent measurement")
        message = self.observer_configuration()
        message["Lq"] = [[0.]]
        message["C"] = [[0.]]  # Unobserved undamped modes are on the unit circle.
        self.bad(b, message, "detectability")

    def test_sensor_observer_split_replay_all_phases(self):
        message = self.observer_configuration()
        b, state, _ = self.initialize(message)
        states, actions = [state], []
        for step in range(6):
            if step % 3 == 0:
                p = self.proposal(b, state, value=.01*(step+1))
                state = p["candidateOpaqueState"]
                action = np.clip(p["absoluteVoltages"], np.asarray(states[-1]["previousApplied"])-.2,
                                 np.asarray(states[-1]["previousApplied"])+.2).tolist()
            state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": action})["candidateOpaqueState"]
            states.append(state)
            actions.append(action)
        for split in range(6):
            fresh, _, _ = self.initialize(message)
            state = copy.deepcopy(states[split])
            for step in range(split, 6):
                if step % 3 == 0:
                    state = self.proposal(fresh, state, value=.01*(step+1))["candidateOpaqueState"]
                state = self.ok(fresh, {"op": "postFE", "opaqueState": state, "appliedVoltages": actions[step]})["candidateOpaqueState"]
                self.assertEqual(state, states[step+1])

    def test_force_mapping_time_only_and_default_zero(self):
        b, _, _ = self.initialize()
        self.assertEqual(self.ok(b, {"op": "force", "time": .4})["forces"], [0.]*12)
        message = configuration("none")
        message["configuration"]["excitation"] = {"channels": [{"channelId": "force:7:X", "unit": "N",
            "terms": [{"kind": "constant", "value": 2.}, {"kind": "expression", "expression": "3*t"}]}]}
        self.ok(b, message)
        a = self.ok(b, {"op": "force", "time": .4})
        self.assertEqual(a, self.ok(b, {"op": "force", "time": .4}))
        assert_allclose(a["forces"], [3.2]+[0.]*11)
        self.bad(b, {"op": "force", "time": .4, "q": [1.]}, "unexpected")
        message["configuration"]["excitation"] = {"externalFunctions": ["load.py"]}
        self.bad(b, message, "unexpected or missing")

    def test_external_controller_namespaces_same_electrode_readout_and_command(self):
        message = configuration("none")
        message["outputs"] = [["actuator:a", "C"]]
        example = Path(__file__).resolve().parents[2] / "fea-solver" / "examples" / "external_io"
        message["configuration"]["controller"] = {
            "kind": "external", "stateSource": "sensor_only", "selectedOutputs": ["actuator:a"],
            "external": {"connection": {"transport": "stdio", "executable": sys.executable,
                "args": ["-u", str(example / "pure_provider.py")],
                "dependencies": [str(example / "range_external_sdk.py")],
                "algorithm": {"id": "range-example-pure-provider", "version": "1"}},
                "initialState": {"integral": 0.}}}
        instance = NativeBackend()
        self.ok(instance, message)
        ports = next(iter(instance.external_specs.values()))["ports"]
        self.assertEqual([p["id"] for p in ports], ["readout:actuator:a", "actuator:actuator:a"])
        self.assertEqual(len({p["id"] for p in ports}), 2)
        self.assertEqual(ports[0]["binding"], {"outputId": "actuator:a"})
        self.assertEqual(ports[1]["binding"], {"actuatorId": "actuator:a"})

    def test_authoritative_physical_damping_and_mode_selection(self):
        message = configuration("none")
        message["configuration"]["design"] = {"modeIndices": [0], "damping": {"kind": "uniform", "zeta": .1}}
        _, _, init = self.initialize(message)
        assert_allclose(init["authoritativeC"], [[.8]])
        assert_allclose(init["design"]["continuous"]["A"][1], [-4., -.4])
        message["configuration"]["design"]["modeIndices"] = [1]
        self.bad(NativeBackend(), message, "modal selection")

    def test_strict_units_shapes_unknown_fields_and_failed_reinit(self):
        b, state, _ = self.initialize()
        for mutation in (lambda m: m.update(unknown=1),
                         lambda m: m["configuration"]["controller"].update(kd=1),
                         lambda m: m["inputs"][0].__setitem__(1, "N"),
                         lambda m: m.__setitem__("M", [[True]]),
                         lambda m: m.__setitem__("B", [[1., 2.]]),
                         lambda m: m["forceCatalog"][0].__setitem__("physicalIndex", 1),
                         lambda m: m.__setitem__("sourceFingerprint", "stale"),
                         lambda m: m.__setitem__("dt", float("nan"))):
            message = configuration()
            mutation(message)
            self.bad(b, message)
        self.ok(b, {"op": "validate", "opaqueState": state})
        bad = readouts(1.)
        bad[0]["unit"] = "V"
        self.bad(b, {"op": "propose", "opaqueState": state, "readouts": bad}, "unit")

    def test_validate_all_persisted_dimensions_finite_identity_counters(self):
        b, state, _ = self.initialize()
        cases = {"integral": [float("inf")], "previousError": [True], "filteredDerivative": ["0"],
                 "controllerState": [0.], "observerPrior": [0.], "warmStart": [[0.]],
                 "previousApplied": [0., 0.], "heldDelta": [], "controlTick": 1,
                 "feStep": 1, "phase": "bad", "sourceFingerprint": "b"*64,
                 "configurationIdentity": "b"*64, "schemaVersion": True}
        for key, invalid in cases.items():
            wrong = copy.deepcopy(state)
            wrong[key] = invalid
            self.bad(b, {"op": "validate", "opaqueState": wrong})
        p = self.proposal(b, state)
        self.bad(b, {"op": "validate", "opaqueState": p["candidateOpaqueState"]}, "phase")
        self.ok(b, {"op": "validate", "opaqueState": p["candidateOpaqueState"], "phase": "proposed"})
        changed = configuration()
        changed["K"] = [[9.]]
        other, _, _ = self.initialize(changed)
        self.bad(other, {"op": "validate", "opaqueState": state}, "identity")

    def test_hold_action_and_phase_failures_are_pure(self):
        b, state, _ = self.initialize(configuration(N=2))
        self.bad(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": [5.]}, "requires a controller proposal")
        p = self.proposal(b, state)
        self.bad(b, {"op": "propose", "opaqueState": p["candidateOpaqueState"], "readouts": readouts(1.)}, "ready")
        state = self.accept(b, p)
        before = copy.deepcopy(state)
        self.bad(b, {"op": "propose", "opaqueState": state, "readouts": readouts(1.)}, "control sample")
        self.bad(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": [3.], "feStep": 9}, "step mismatch")
        self.assertEqual(state, before)

    def test_multirate_authoritative_fe_slew_ramp_split_replay(self):
        for N in (3, 4):
            message = self.observer_configuration(N=N)
            b, state, init = self.initialize(message)
            a, inputs = (np.array(init["design"]["substep"][key]) for key in ("A", "B"))
            states, actions, requested = [state], [], []
            max_native_increment = .025
            for step in range(2*N):
                if step % N == 0:
                    proposal = self.proposal(b, state, value=3.)
                    state = proposal["candidateOpaqueState"]
                    held = np.clip(proposal["absoluteVoltages"], [4.], [6.])
                applied = np.clip(held, np.array(state["previousApplied"])-max_native_increment,
                                  np.array(state["previousApplied"])+max_native_increment).tolist()
                old_prior = np.array(state["observerPrior"])
                state = self.ok(b, {"op": "postFE", "opaqueState": state, "appliedVoltages": applied})["candidateOpaqueState"]
                assert_allclose(state["observerPrior"], a@old_prior+inputs@(np.array(applied)-5), atol=1e-14)
                states.append(state)
                actions.append(applied)
                requested.append(held.tolist())
            self.assertNotEqual(actions[0], actions[1])
            self.assertEqual(requested[0], requested[1])
            for split in range(2*N):
                fresh, _, _ = self.initialize(message)
                state = json.loads(json.dumps(states[split]))
                for step in range(split, 2*N):
                    if step % N == 0:
                        state = self.proposal(fresh, state, value=3.)["candidateOpaqueState"]
                    state = self.ok(fresh, {"op": "postFE", "opaqueState": state,
                                          "appliedVoltages": actions[step]})["candidateOpaqueState"]
                    self.assertEqual(state, states[step+1])

    def test_reject_uncertified_nonzero_model_references(self):
        for kind in ("LQR", "MPC"):
            self.bad(NativeBackend(), configuration(kind, stateReference=[.1, 0.]), "affine equilibrium")
            self.initialize(configuration(kind, stateReference=[0., 0.]))

    def test_native_applied_boundary_rejects_self_consistent_tampered_history(self):
        for kind in ("LQR", "MPC"):
            message = configuration(kind, stateSource="sensor_only", selectedOutputs=["sensor:s"],
                observer={"processCovariance": [[.1, 0.], [0., .1]], "measurementCovariance": [[.01]]},
                voltageLower=[4.], voltageUpper=[6.], deltaVoltageLimits=[.3])
            b, state, _ = self.initialize(message)
            p = self.proposal(b, state, value=.01)
            applied = p["absoluteVoltages"]
            state = self.accept(b, p, applied)
            self.ok(b, {"op": "validate", "opaqueState": state, "feStep": 1,
                        "controlTick": 1, "appliedVoltages": applied})
            tampered = json.loads(json.dumps(state))
            # Finite, correctly shaped and inside amplitude/rate envelopes: an
            # internally coherent/checksummed JSON history still contradicts FE.
            tampered["previousApplied"] = [5.1]
            self.ok(b, {"op": "validate", "opaqueState": tampered})
            self.bad(b, {"op": "validate", "opaqueState": tampered,
                         "appliedVoltages": applied}, "differs from accepted native")
            for invalid in ([applied[0], 0.], [True], [float("inf")]):
                self.bad(b, {"op": "validate", "opaqueState": state, "appliedVoltages": invalid})

    def test_stdio_one_json_per_line_malformed_recovery(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1]/"python")
        requests = ["{invalid}", '{"op":"force","op":"validate"}', '{"op":"force","time":NaN}',
                    json.dumps(configuration("none")), json.dumps({"op": "force", "time": .5})]
        command = [sys.executable, "-B", "-m", "range_workflow.piezo_native_backend", "--stdio"]
        if sys.flags.ignore_environment:
            # Embedded private Python ignores PYTHONPATH via its immutable _pth.
            # Bootstrap the checkout explicitly; installed -m is a packaging gate.
            bootstrap = ("import sys,runpy;sys.path.insert(0,"+repr(env["PYTHONPATH"])+
                         ");sys.argv=['range_workflow.piezo_native_backend','--stdio'];"
                         "runpy.run_module('range_workflow.piezo_native_backend',run_name='__main__')")
            command = [sys.executable, "-B", "-c", bootstrap]
        result = subprocess.run(command,
            input="\n".join(requests)+"\n", text=True, capture_output=True, env=env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), len(requests))
        responses = [json.loads(line) for line in lines]
        self.assertEqual([r["ok"] for r in responses], [False, False, False, True, True])
        self.assertEqual(responses[-1]["result"]["forces"], [0.]*12)
        self.assertEqual(result.stderr, "")


    def test_explicit_axial_xyz_force_layout_and_identity(self):
        message = configuration("none")
        message["physicalComponentsPerNode"] = 3
        message["forceCatalog"] = [{"channelId": "force:12:Z", "physicalNodeId": 12,
            "dof": "Z", "unit": "N", "physicalIndex": 5}]
        message["configuration"]["excitation"] = {"channels": [{"channelId": "force:12:Z", "unit": "N",
            "terms": [{"kind": "constant", "value": 2.}]}]}
        backend = NativeBackend(); initialized = backend.handle(message)
        self.assertTrue(initialized["ok"], initialized)
        self.assertEqual(backend.handle({"op": "force", "time": .1})["result"]["forces"], [0.,0.,0.,0.,0.,2.])
        legacy = copy.deepcopy(message); legacy.pop("physicalComponentsPerNode")
        legacy["forceCatalog"][0]["physicalIndex"] = 8
        other = NativeBackend().handle(legacy)
        self.assertTrue(other["ok"], other)
        self.assertNotEqual(initialized["result"]["identity"]["configurationIdentity"], other["result"]["identity"]["configurationIdentity"])

    def test_physical_force_layout_rejects_rotations_and_mismatched_stride(self):
        message = configuration("none"); message["physicalComponentsPerNode"] = 3
        for dof, index, unit in (("RX",3,"N m"),("X",6,"N")):
            bad = copy.deepcopy(message)
            bad["forceCatalog"] = [{"channelId":"bad", "physicalNodeId":12,"dof":dof,"unit":unit,"physicalIndex":index}]
            self.assertFalse(NativeBackend().handle(bad)["ok"])
        for count in (True,0,2,4,7):
            bad = copy.deepcopy(message); bad["physicalComponentsPerNode"] = count
            self.assertFalse(NativeBackend().handle(bad)["ok"])

if __name__ == "__main__":
    unittest.main()
