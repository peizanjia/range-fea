"""Real process tests of physical ports and caller-owned FE transactions."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import numpy as np
from numpy.testing import assert_allclose

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow.piezo_native_backend import NativeBackend, _transport_digest
from test_piezo_native_backend import configuration, readouts


PROVIDER = '''
import copy, json, sys
sys.path.insert(0, PYTHON_PATH)
from range_workflow.piezo_external_io import serve
ports = []
def initialize(request):
    ports[:] = request['ports']
    return {role: {'n': 0} for role in {p['role'] for p in ports}}
def evaluate(request):
    with open(LOG_PATH, 'a', encoding='utf-8') as stream:
        stream.write(json.dumps(request)+'\\n')
    state = copy.deepcopy(request['state'])
    state['n'] += 1
    if request['role'] == 'controller':
        state['previous'] = request['previousApplied'][0]['value']
        value = 5. - 2.*request['inputs'][0]['value']
    else:
        value = state['n'] + request['time']
    outputs = [dict(id=p['id'], unit=p['unit'], reference=p['reference'], value=value)
               for p in ports if p['role'] == request['role'] and p['direction'] == 'output']
    return outputs, state
serve(sys.stdin.buffer, sys.stdout.buffer, algorithm={'id':'integration','version':'1'},
      initialize=initialize, evaluate=evaluate)
'''


class ExternalIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)/"provider.py"
        self.log = Path(self.directory.name)/"requests.jsonl"
        python = str(Path(__file__).resolve().parents[1]/"python")
        self.path.write_text(PROVIDER.replace("PYTHON_PATH", repr(python)).replace("LOG_PATH", repr(str(self.log))), encoding="utf-8")
        self.connection = dict(transport="stdio", executable=sys.executable,
            args=["-B", "-u", str(self.path)], cwd=self.directory.name, dependencies=[str(self.path)],
            algorithm=dict(id="integration", version="1"), requestTimeout=2., handshakeTimeout=2.)

    def backend(self, msg):
        backend = NativeBackend()
        self.addCleanup(backend.close)
        result = self.ok(backend, msg)
        return backend, result["initialOpaqueState"], result

    def ok(self, backend, msg):
        response = backend.handle(msg)
        self.assertTrue(response["ok"], response)
        return response["result"]

    def records(self):
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()] if self.log.exists() else []

    @staticmethod
    def qt_json_numbers(value):
        """Model Qt's JSON round-trip of exactly integral binary64 values."""
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, list):
            return [ExternalIntegrationTest.qt_json_numbers(item) for item in value]
        if isinstance(value, dict):
            return {key: ExternalIntegrationTest.qt_json_numbers(item) for key, item in value.items()}
        return value

    def external(self, N=1, excitation=False):
        msg = configuration("velocity_feedback", N=N)
        msg["configuration"]["controller"] = dict(kind="external", external=dict(
            connection=copy.deepcopy(self.connection), initialState={"n": 0}))
        if excitation:
            msg["configuration"]["excitation"]["externalSources"] = [dict(
                connection=copy.deepcopy(self.connection), channelIds=["force:7:X"],
                sampleEveryFESteps=2, initialState={"n": 0})]
        msg.update(targetSteps=12, initialTime=0., Bforce=[[4.]] if excitation else [[]],
            Dforce=[[0.]] if excitation else [[]], disturbanceInputs=[["force:7:X", "N"]] if excitation else [])
        if excitation:
            msg["forceCatalog"][0]["disturbanceColumn"] = 0
        return msg

    def prepare(self, backend, state, replay=False, step=None, time=None):
        step = state["feStep"] if step is None else step
        result = self.ok(backend, dict(op="force", time=backend._time(step) if time is None else time,
            opaqueState=state, feStep=step, replayOnly=replay))
        return result.get("candidateOpaqueState", state), result["forces"]

    def advance(self, backend, state, measurement=.3, applied=4.):
        if state["feStep"] % backend.N == 0:
            state = self.ok(backend, dict(op="propose", opaqueState=state,
                readouts=readouts(measurement, "m/s")))["candidateOpaqueState"]
        return self.ok(backend, dict(op="postFE", opaqueState=state,
            appliedVoltages=[applied]))["candidateOpaqueState"]

    def test_controller_matches_builtin_physical_response_and_actual_applied_feedback(self):
        external, state, _ = self.backend(self.external(N=3))
        builtin, reference, _ = self.backend(configuration("velocity_feedback", N=3))
        x = np.array([.2, -.1])
        previous = 5.
        for step in range(9):
            measurement = float(x[1]+.2*(previous-5.))
            args = dict(op="propose", readouts=readouts(measurement, "m/s"))
            if step % 3 == 0:
                a = self.ok(external, dict(args, opaqueState=state))
                b = self.ok(builtin, dict(args, opaqueState=reference))
                assert_allclose(a["absoluteVoltages"], b["absoluteVoltages"], atol=1e-14)
                self.assertEqual(a["candidateOpaqueState"]["externalControllerState"]["previous"], previous)
                state, reference = a["candidateOpaqueState"], b["candidateOpaqueState"]
            target = float(external.va0[0]+state["heldDelta"][0])
            applied = float(np.clip(target, previous-.05, previous+.05))
            x = external.substep.A@x+external.substep.B[:, 0]*(applied-5.)
            for backend, current in ((external, state), (builtin, reference)):
                result = self.ok(backend, dict(op="postFE", opaqueState=current, appliedVoltages=[applied]))
                if backend is external:
                    state = result["candidateOpaqueState"]
                else:
                    reference = result["candidateOpaqueState"]
            previous = applied
        self.assertEqual(len(self.records()), 3)
        self.assertEqual(state["externalControllerState"]["n"], 3)
        bad = external.handle(dict(op="propose", opaqueState=state, q=[0.], v=[0.], readouts=readouts(0., "m/s")))
        self.assertFalse(bad["ok"])
        self.assertIn("sensor_only", bad["error"])

    def test_default_zero_start_first_external_sample_uses_signed_boundary_time(self):
        backend, state, _ = self.backend(self.external(N=1, excitation=True))
        backend.initial_time = -.1
        state, force = self.prepare(backend, state)
        self.assertAlmostEqual(force[0], .9)
        proposal = self.ok(backend, dict(op="propose", opaqueState=state,
            readouts=readouts(.2, "m/s")))
        self.assertTrue(np.isfinite(proposal["absoluteVoltages"][0]))
        records = self.records()
        self.assertEqual([record["time"] for record in records], [-.1, -.1])
        self.assertEqual([record["role"] for record in records], ["excitation", "controller"])

    def test_shared_roles_retry_zoh_atomic_commit_and_offline_certification(self):
        backend, initial, _ = self.backend(self.external(N=3, excitation=True))
        self.assertFalse(self.log.exists())  # initialization and preview are offline
        prepared, forces = self.prepare(backend, initial)
        self.assertEqual(forces[0], 1.)
        self.assertEqual(initial["excitationStates"], [{"n": 0}])
        self.assertEqual(initial["excitationLedger"], [[]])
        tampered = copy.deepcopy(prepared)
        tampered["externalControllerState"] = {"n": 90}
        failed_initial = backend.handle(dict(op="validate", opaqueState=tampered))
        self.assertFalse(failed_initial["ok"])
        self.assertIn("frozen initial state", failed_initial["error"])
        again, again_force = self.prepare(backend, initial)
        self.assertEqual(prepared, again)
        self.assertEqual(forces, again_force)
        self.assertEqual(len(self.records()), 1)
        _, endpoint = self.prepare(backend, prepared, replay=True, time=.1)
        self.assertEqual(endpoint, forces)
        proposal = self.ok(backend, dict(op="propose", opaqueState=prepared, readouts=readouts(.2, "m/s")))
        proposed = proposal["candidateOpaqueState"]
        self.assertEqual(proposed["excitationStates"], [{"n": 0}])
        self.assertEqual(proposed["externalControllerState"]["n"], 1)
        failed = backend.handle(dict(op="postFE", opaqueState=proposed, appliedVoltages=[float("nan")]))
        self.assertFalse(failed["ok"])
        repeated = self.ok(backend, dict(op="propose", opaqueState=prepared, readouts=readouts(.2, "m/s")))
        self.assertEqual(proposal, repeated)
        self.assertEqual(len(self.records()), 2)
        state = self.ok(backend, dict(op="postFE", opaqueState=proposed, appliedVoltages=[4.6]))["candidateOpaqueState"]
        for _ in range(3):
            state, _ = self.prepare(backend, state)
            state = self.advance(backend, state)
        self.assertEqual(state["excitationStates"], [{"n": 2}])
        self.assertEqual(state["externalControllerState"]["n"], 2)
        self.assertEqual([r["role"] for r in self.records()], ["excitation", "controller", "excitation", "controller"])
        backend.close()
        self.path.unlink()  # identity recheck should reject mutated/missing software
        failed = backend.handle(dict(op="certificationBatch", records=[], times=[], opaqueState=state))
        self.assertFalse(failed["ok"])

    def test_offline_replay_and_resume_match_continuous_without_old_endpoint(self):
        message = self.external(excitation=True)
        backend, state, _ = self.backend(message)
        for _ in range(4):
            state, _ = self.prepare(backend, state)
            state = self.advance(backend, state)
        saved = copy.deepcopy(state)
        backend.close()
        replay, _, _ = self.backend(message)
        times = [dict(feStep=i, timeBits=struct.pack("<d", .1*i).hex()) for i in range(5)]
        before = len(self.records())
        cert = self.ok(replay, dict(op="certificationBatch", records=[], times=times, opaqueState=saved))
        self.assertEqual(len(self.records()), before)
        self.assertEqual([r["forces"][0] for r in cert["forces"]], [1., 1., 1., 2.2, 2.2])
        continuous, _ = self.prepare(backend, saved, replay=True, step=3, time=.4)
        self.assertEqual(continuous, saved)
        resumed, forces = self.prepare(replay, saved)
        self.assertEqual(forces[0], 3.4)
        self.assertEqual(saved["excitationStates"], [{"n": 2}])
        resumed = self.advance(replay, resumed)
        self.assertEqual(resumed["excitationStates"], [{"n": 3}])
        self.assertEqual([r for r in self.records() if r["role"] == "excitation"][-1]["state"], {"n": 2})

    def test_no_future_queries_and_internal_predictor_admission(self):
        backend, state, _ = self.backend(self.external(excitation=True))
        failed = backend.handle(dict(op="force", opaqueState=state, feStep=1, time=.1))
        self.assertFalse(failed["ok"])
        self.assertFalse(self.log.exists())
        for kind in ("MPC", "LQR"):
            message = configuration(kind, stateSource="sensor_only", observer={
                "processCovariance": [[1., 0.], [0., 1.]], "measurementCovariance": [[1.]]})
            message["configuration"]["excitation"]["externalSources"] = self.external(excitation=True)["configuration"]["excitation"]["externalSources"]
            failed = NativeBackend().handle(message)
            self.assertFalse(failed["ok"])
            self.assertIn("future-preview or observer", failed["error"])

    def test_separate_connections_multirate_and_ledger_corruption_rejection(self):
        message = self.external(N=2, excitation=True)
        second = self.path.with_name("excitation.py")
        second.write_bytes(self.path.read_bytes())
        source = message["configuration"]["excitation"]["externalSources"][0]
        source["connection"]["args"][-1] = str(second)
        source["connection"]["dependencies"] = [str(second)]
        source["sampleEveryFESteps"] = 1
        backend, state, _ = self.backend(message)
        for step in range(4):
            state, force = self.prepare(backend, state)
            self.assertAlmostEqual(force[0], step+1+.1*step)
            state = self.advance(backend, state)
        self.assertEqual(len(backend._external_clients), 2)
        self.assertEqual(state["externalControllerState"]["n"], 2)
        self.assertEqual(state["excitationStates"], [{"n": 4}])
        corrupted = copy.deepcopy(state)
        corrupted["excitationLedger"][0][0][0][0] = 900.
        failed = backend.handle(dict(op="validate", opaqueState=corrupted))
        self.assertFalse(failed["ok"])
        self.assertIn("digest mismatch", failed["error"])
        bounded = copy.deepcopy(state)
        bounded["excitationStates"][0]["large"] = "x"*262144
        failed = backend.handle(dict(op="validate", opaqueState=bounded))
        self.assertFalse(failed["ok"])
        self.assertIn("16 KiB", failed["error"])

    def test_external_excitation_checkpoint_digest_survives_qt_numeric_roundtrip(self):
        backend, initial, _ = self.backend(self.external(N=1, excitation=True))
        pending, force = self.prepare(backend, initial)
        self.assertEqual(force[0], 1.)
        self.assertTrue(all(value == 0. for value in force[1:]))
        normalized = self.qt_json_numbers(pending)
        self.ok(backend, dict(op="validate", opaqueState=normalized))
        accepted = self.advance(backend, normalized, measurement=.2, applied=4.9)
        accepted = self.qt_json_numbers(accepted)
        self.ok(backend, dict(op="validate", opaqueState=accepted))
        self.assertEqual(accepted["excitationStates"], [{"n": 1}])
        self.assertEqual(accepted["excitationLedger"][0][0][0], [1.])

    def test_controller_full_fe_access_requires_explicit_configuration(self):
        message = self.external()
        message["configuration"]["controller"]["stateSource"] = "full_fe"
        backend, state, _ = self.backend(message)
        self.ok(backend, dict(op="propose", opaqueState=state, readouts=readouts(.2, "m/s"), q=[.7], v=[.9]))
        records = self.records()
        self.assertEqual([p["id"] for p in records[0]["inputs"]], ["readout:sensor:s", "state:node:7:X:q", "state:node:7:X:v"])
        self.assertEqual([p["value"] for p in records[0]["inputs"]], [.2, .7, .9])

    def test_capacity_preflight_admits_twenty_thousand_samples_and_rejects_unsupported_run_offline(self):
        message = self.external(excitation=True)
        message["targetSteps"] = 20000
        source = message["configuration"]["excitation"]["externalSources"][0]
        source["sampleEveryFESteps"] = 1
        backend, state, result = self.backend(message)
        capacity = result["design"]["forceAware"]["externalLedgerCapacity"]
        self.assertLess(capacity["admittedWorstCaseBytes"], capacity["maximumBytes"])
        self.assertFalse(self.log.exists())
        # Exercise the complete admitted checkpoint size without launching an
        # endpoint 20,000 times. Runtime transaction tests above cover creation.
        state["feStep"] = state["controlTick"] = 20000
        state["excitationStates"] = [{"n": 20000}]
        state["excitationLedger"] = [[[[float(i+1)], _transport_digest({"n": i+1})] for i in range(20000)]]
        state["excitationLedgerDigest"] = _transport_digest(state["excitationLedger"])
        self.ok(backend, dict(op="validate", opaqueState=state))
        _, force = self.prepare(backend, state, replay=True, step=19999, time=2000.)
        self.assertEqual(force[0], 20000.)
        self.assertLess(len(json.dumps(state, separators=(",", ":")).encode()), 2*1024*1024)
        self.assertFalse(self.log.exists())
        message["targetSteps"] = 1000000
        failed = NativeBackend().handle(message)
        self.assertFalse(failed["ok"])
        self.assertIn("targetSteps exceeds 4 MiB", failed["error"])
        self.assertFalse(self.log.exists())
        # Controller-only use has no cumulative excitation log allocation.
        controller_only = self.external()
        controller_only["targetSteps"] = 1000000
        _, initial, _ = self.backend(controller_only)
        self.assertEqual(initial["excitationLedger"], [])


if __name__ == "__main__":
    unittest.main()
