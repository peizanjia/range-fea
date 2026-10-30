"""Native FE control bridge. Runtime history is exclusively caller-owned JSON.

Run ``python -B -m range_workflow.piezo_native_backend --stdio``. Each line is
one request, followed by exactly one {ok, result|error} JSON response. Initialize
freezes actual supplied FE operators; propose and postFE never modify their input
opaque state. The native transaction alone accepts candidateOpaqueState.

feStep counts completed physical steps. A control sample occurs at feStep % N ==
0; propose advances controlTick, postFE advances feStep and predicts the observer
using the *applied absolute* voltage, less the operating-point voltage. q is an
FE increment, never an absolute displacement. sensor_only forbids q/v fields.
"""
from __future__ import annotations

import argparse
import copy
from dataclasses import replace
from contextlib import nullcontext
import hashlib
import json
import marshal
import math
from pathlib import Path
import sys
import struct
import uuid
import time

import numpy as np
from scipy import linalg

from . import piezo_control_design as control
from . import piezo_design as design
from .piezo_excitation import ForceChannel, compile_excitation


_SOURCE_PATH = Path(__file__).resolve()
_SOURCE_SHA = hashlib.sha256(_SOURCE_PATH.read_bytes()).hexdigest()
_EXECUTION_SHA = hashlib.sha256(marshal.dumps(sys._getframe().f_code)).hexdigest()
_EXTERNAL_STATE_BYTES = 16*1024
_EXTERNAL_LEDGER_BYTES = 4*1024*1024


def _keys(value, required=(), optional=(), label="object"):
    if (not isinstance(value, dict) or not set(required) <= set(value)
            or set(value)-set(required)-set(optional)):
        raise design.DesignError(f"{label}: unexpected or missing fields")


def _number(value, name, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise design.DesignError(f"{name} must be a real number")
    try:
        value = float(value)
    except OverflowError as exc:
        raise design.DesignError(f"{name} must be finite") from exc
    if not math.isfinite(value) or (positive and value <= 0):
        raise design.DesignError(f"{name} must be finite{' and positive' if positive else ''}")
    return value


def _hex_bytes(value, length, name):
    if (not isinstance(value, str) or len(value) != 2*length
            or any(c not in "0123456789abcdef" for c in value)):
        raise design.DesignError(f"{name} requires {2*length} lowercase hexadecimal characters")
    return bytes.fromhex(value)


def _binary64(value, name):
    return _number(struct.unpack("<d", _hex_bytes(value, 8, name))[0], name)


def _integer(value, name, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise design.DesignError(f"{name} must be an integer >= {minimum}")
    return value


def _finite_tree(value):
    if isinstance(value, dict):
        if any(not isinstance(k, str) for k in value):
            raise design.DesignError("JSON object keys must be strings")
        for item in value.values():
            _finite_tree(item)
    elif isinstance(value, (list, tuple)):
        for item in value:
            _finite_tree(item)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        _number(value, "JSON numeric value")
    elif value is not None and not isinstance(value, (str, bool)):
        raise design.DesignError("unsupported JSON value")


def _digest(value):
    _finite_tree(value)
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True,
        separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _transport_digest(value):
    """Digest JSON values using Qt's binary64 representation for numbers."""
    def normalize(item):
        if isinstance(item, bool) or item is None or isinstance(item, str):
            return item
        if isinstance(item, (int, float)):
            return float(item)
        if isinstance(item, list):
            return [normalize(child) for child in item]
        if isinstance(item, dict):
            return {key: normalize(child) for key, child in item.items()}
        raise design.DesignError("unsupported JSON value in transport checkpoint")

    return _digest(normalize(value))


def _array(value, name, shape):
    # The common opaque-state vectors are exact flat JSON lists. Convert each
    # accepted binary64 scalar once and allocate an independent owned array.
    # Other types, dimensions and every invalid value retain the original path.
    if (type(value) is list and type(shape) is tuple and len(shape) == 1
            and type(shape[0]) is int and len(value) == shape[0]):
        converted = []
        for item in value:
            if type(item) not in (int, float):
                break
            try:
                number = float(item)
            except OverflowError:
                break
            if not math.isfinite(number):
                break
            converted.append(number)
        else:
            return np.array(converted, dtype=float, copy=True)
    # Strict JSON numbers: NumPy alone would silently accept booleans/strings.
    def numbers(v):
        if isinstance(v, (list, tuple)):
            for item in v:
                numbers(item)
        else:
            _number(v, name)
    numbers(value)
    return design._array(value, name, shape)


def _gain(value, name, m, p):
    if m == p == 1 and isinstance(value, (int, float)):
        value = [[value]]
    return _array(value, name, (m, p))


def _poles(values):
    return [[float(v.real), float(v.imag)] for v in values]


def _actuator_prediction_scope():
    """Scope of the linear QP certificate, separate from native conditioning."""
    return {"prediction": "ideal-target-voltage-held-for-each-control-period",
            "nativeRealization": "absolute-amplitude-and-slew-limits-at-every-FE-step",
            "observerPrediction": "actual-accepted-voltage-at-every-FE-step",
            "qpConstraints": "target-amplitude-and-target-change-per-control-period",
            "includesNativeSlewTrajectory": False,
            "guaranteesActualFEStateConstraints": False}


def provenance():
    if hashlib.sha256(_SOURCE_PATH.read_bytes()).hexdigest() != _SOURCE_SHA:
        raise design.DesignError("native backend source changed; reload required")
    return {"moduleCodeSha256": _SOURCE_SHA,
            "moduleExecutionCodeSha256": _EXECUTION_SHA,
            "identityScope": "module-source-and-executing-code-only",
            "plantDesign": dict(design.provenance()),
            "controlDesign": dict(control.provenance())}


class NativeBackend:
    """Immutable initialized design plus pure runtime proposals, no history."""

    def __init__(self):
        self._initialized = False
        self._external_clients = {}
        self._external_frozen = {}

    def close(self):
        for client in self._external_clients.values():
            client.close()
        self._external_clients.clear()

    def handle(self, message):
        """Service boundary: errors have no action and never consume history."""
        try:
            self._external_deadline = time.monotonic()+50.
            _finite_tree(message)
            if not isinstance(message, dict) or message.get("op") not in (
                    "initialize", "force", "propose", "postFE", "validate", "certificationBatch"):
                raise design.DesignError("unknown operation")
            if message["op"] == "initialize":
                # Build off to the side: failed reinitialization retains old design.
                candidate = NativeBackend()
                try:
                    result = candidate._initialize(message)
                except Exception:
                    candidate.close()
                    raise
                self.close()
                self.__dict__ = candidate.__dict__.copy()
            else:
                if not self._initialized:
                    raise design.DesignError("backend is not initialized")
                scope = (self.excitation._framework_batch(129)
                         if message["op"] == "certificationBatch" else nullcontext())
                with scope:
                    if provenance() != self.backend_identity:
                        raise design.DesignError("backend identity changed")
                    self.excitation.check_revision()
                    if self.external_specs:
                        from .piezo_external_io import fingerprint
                        if any(fingerprint(spec["connection"]) != key for key, spec in self.external_specs.items()):
                            raise design.DesignError("external algorithm/dependency identity changed; reload required")
                    result = getattr(self, "_"+message["op"])(message)
            _finite_tree(result)
            return {"ok": True, "result": result}
        except Exception as exc:
            return {"ok": False, "error": str(exc) or type(exc).__name__}

    def _initialize(self, msg):
        required = ("op", "sourceFingerprint", "M", "K", "C", "B", "Lq",
                    "Lv", "La", "D", "dofs", "inputs", "outputs",
                    "actuatorVoltages0", "dt", "controlEveryFESteps", "nodeIds",
                    "forceCatalog", "configuration")
        _keys(msg, required, ("Bforce", "Dforce", "disturbanceInputs", "initialTime", "biasForce", "physicalComponentsPerNode", "sourceFrame", "physicalForceDirections", "targetSteps", "initialTimeBits"), "initialize")
        self.physical_components = _integer(msg.get("physicalComponentsPerNode", 6), "physicalComponentsPerNode")
        if self.physical_components not in (3, 6):
            raise design.DesignError("physicalComponentsPerNode must be 3 XYZ or 6 XYZ/rotations")
        self.certification_target = None
        if "targetSteps" in msg:
            self.certification_target = _integer(msg["targetSteps"], "targetSteps", 1)
            if self.certification_target > 1000000 or "initialTime" not in msg:
                raise design.DesignError("targetSteps requires an explicit original clock and at most 1000000 steps")
        self._configure_frame(msg.get("sourceFrame"), "sourceFrame" in msg)
        self.source = design.SourceIdentity(msg["sourceFingerprint"], msg["dofs"],
                                           msg["inputs"], msg["outputs"])
        self.n, self.m, self.ny = len(self.source.dofs), len(self.source.inputs), len(self.source.outputs)
        if not self.n or any(unit != "V" for _, unit in self.source.inputs):
            raise design.DesignError("plant requires physical DOFs and voltage inputs in V")
        if any(unit not in ("m", "rad") for _, unit in self.source.dofs):
            raise design.DesignError("physical FE DOF unit must be m or rad")
        self.M, self.K, supplied_c = (_array(msg[key], key, (self.n, self.n))
                                     for key in ("M", "K", "C"))
        self.B = _array(msg["B"], "B", (self.n, self.m))
        self.Lq, self.Lv, self.La = (_array(msg[key], key, (self.ny, self.n))
                                    for key in ("Lq", "Lv", "La"))
        self.D = _array(msg["D"], "D", (self.ny, self.m))
        self.va0 = _array(msg["actuatorVoltages0"], "actuatorVoltages0", (self.m,))
        self.dt = _number(msg["dt"], "dt", True)
        self.N = _integer(msg["controlEveryFESteps"], "controlEveryFESteps", 1)
        self.ts = _number(self.dt*self.N, "sampling period", True)
        cfg = msg["configuration"]
        _keys(cfg, ("design", "controller", "excitation"), label="configuration")
        dc = cfg["design"]
        _keys(dc, optional=("modeIndices", "damping"), label="design")
        modes = design.compute_modes(self.M, self.K, source=self.source)
        if "modeIndices" in dc:
            modes = design.select_modes(modes, dc["modeIndices"])
        self.modes = modes
        authoritative_c = None
        self.C = supplied_c
        if "damping" in dc:
            damping = dc["damping"]
            _keys(damping, ("kind",), ("zeta", "alpha", "beta", "imported_C", "high_mode_policy"), "damping")
            args = dict(damping)
            for key in ("alpha", "beta"):
                if key in args:
                    _number(args[key], key)
            if "zeta" in args:
                values = args["zeta"]
                for value in (values if isinstance(values, list) else [values]):
                    _number(value, "zeta")
            if "imported_C" in args:
                _array(args["imported_C"], "imported_C", (self.n, self.n))
            if args["kind"] in ("uniform", "per_mode", "per_cluster"):
                args["modes"] = modes
            self.C = design.physical_damping(self.M, self.K, source=self.source, **args).C
            authoritative_c = self.C.tolist()
        rom = design.reduce_model(self.M, self.K, self.C, self.B, modes,
                                  source=self.source, Lq=self.Lq, Lv=self.Lv,
                                  La=self.La, Dinput=self.D)
        self.Phi = rom.Phi
        full_actuator = design.continuous_state_space(self.M, self.K, self.C, self.B,
            source=self.source, Lq=self.Lq, Lv=self.Lv, La=self.La, Dinput=self.D)
        self.continuous = self._physical_readout_rom(rom.state_space, full_actuator)
        self.discrete = design.exact_zoh(self.continuous, self.ts)
        self.substep = design.exact_zoh(self.continuous, self.dt)
        self.nx = len(self.discrete.A)
        self._configure_controller(cfg["controller"])
        self._configure_force(msg["nodeIds"], msg["forceCatalog"], cfg["excitation"])
        self._configure_disturbance(msg, cfg["excitation"], rom)
        self._configure_external()
        self.backend_identity = provenance()
        self.identity = _digest({"initialize": msg, "backend": self.backend_identity,
                                 "excitationIdentity": self.excitation.source_hash,
                                 "excitationCode": self.excitation.loaded_code_hash,
                                 "externalConnections": self.external_identities})
        self._external_run_id = str(uuid.uuid4())
        self._initialized = True
        state = {"schemaVersion": 1, "configurationIdentity": self.identity,
                 "sourceFingerprint": self.source.sha256, "feStep": 0,
                 "controlTick": 0, "phase": "ready", "heldDelta": [0.]*self.m,
                 "integral": [0.]*self.p, "previousError": [0.]*self.p,
                 "filteredDerivative": [0.]*self.p,
                 "controllerState": self.initial_controller.tolist(),
                 "observerPrior": self.initial_prior.tolist(),
                 "warmStart": self.initial_warm.tolist(),
                 "previousApplied": self.va0.tolist()}
        if self.external_specs:
            state.update(externalControllerState=copy.deepcopy(self.external_controller_initial),
                excitationStates=[copy.deepcopy(s["initialState"]) for s in self.excitation._sampled],
                excitationLedger=[[] for _ in self.excitation._sampled],
                excitationLedgerDigest=_transport_digest([[] for _ in self.excitation._sampled]),
                pendingExcitation=[], externalHeldAbsolute=self.va0.tolist())
        self._initial_opaque_state = copy.deepcopy(state)
        ss = lambda s: {"A": s.A.tolist(), "B": s.B.tolist(),
                        "Cy": s.Cy.tolist(), "D": s.Dy.tolist()}
        certificate = {"selectedModes": list(modes.indices), "Phi": modes.Phi.tolist(),
                       "clusters": [list(cluster) for cluster in modes.clusters],
                       "omega": modes.omega.tolist(), "modalResiduals": modes.residuals.tolist(),
                       "massOrthogonalityError": modes.mass_orthogonality_error,
                       "continuous": ss(self.continuous), "discrete": ss(self.discrete),
                       "substep": ss(self.substep), "samplingPeriod": self.ts,
                       "samplingContract": "nominal-period-LQR-Kalman-gain-design;actual-canonical-clock-observer-and-MPC-prediction",
                       "samplingQualification": {"relativePeriodTolerance": 1e-10,
                            "runtimeBoundaryRecheck": True, "maxMPCForecastFESteps": 20000},
                       "controller": self.design_certificate,
                       "actuatorPredictionScope": _actuator_prediction_scope(),
                       "forceAware": self.force_package}
        return {"identity": {"configurationIdentity": self.identity,
                             "sourceFingerprint": self.source.sha256,
                             "backend": self.backend_identity,
                             "excitationSourceHash": self.excitation.source_hash,
                             "excitationLoadedCodeHash": self.excitation.loaded_code_hash,
                             "externalConnections": self.external_identities},
                "initialOpaqueState": state, "design": certificate,
                "authoritativeC": authoritative_c,
                "envelopes": {"lower": None if self.lower is None else self.lower.tolist(),
                              "upper": None if self.upper is None else self.upper.tolist(),
                              "deltaPerSample": None if self.delta is None else self.delta.tolist()}}

    def _configure_controller(self, cfg):
        common = ("stateSource", "selectedOutputs", "voltageLower", "voltageUpper", "deltaVoltageLimits")
        kinds = {
            "none": (), "external": ("external",), "velocity_feedback": ("gain", "reference", "bias"),
            "PI": ("kp", "ki", "reference", "bias"),
            "PD": ("kp", "kd", "reference", "bias", "derivativeFilterAlpha"),
            "PID": ("kp", "ki", "kd", "reference", "bias", "derivativeFilterAlpha"),
            "discrete_state_space": ("Ac", "Bc", "Cc", "Dc", "initialState", "reference", "bias", "samplingPeriod", "inputUnits", "outputUnits"),
            "LQR": ("Q", "R", "stateReference", "observer"),
            "MPC": ("Q", "R", "Qf", "horizon", "stateReference", "observer", "verificationTolerance", "maxIterations")}
        if not isinstance(cfg, dict) or cfg.get("kind") not in kinds:
            raise design.DesignError("unknown controller kind")
        self.kind = cfg["kind"]
        if not self.m and self.kind != "none":
            raise design.DesignError("active controller requires at least one voltage actuator")
        _keys(cfg, ("kind",), common+kinds[self.kind], "controller")
        self.controller = copy.deepcopy(cfg)
        self.state_source = cfg.get("stateSource", "sensor_only" if self.kind == "external" else "full_fe")
        if self.state_source not in ("full_fe", "sensor_only"):
            raise design.DesignError("unknown stateSource")
        ids = [item[0] for item in self.source.outputs]
        selected = cfg.get("selectedOutputs", ids)
        if (not isinstance(selected, list) or any(not isinstance(v, str) or v not in ids for v in selected)
                or len(set(selected)) != len(selected)):
            raise design.DesignError("selectedOutputs requires unique actual output IDs")
        self.selected = tuple(ids.index(v) for v in selected)
        self.p = len(self.selected)
        if self.kind not in ("none", "LQR", "MPC") and not self.p:
            raise design.DesignError("feedback controller needs selected output measurements")
        self.lower = self.upper = self.delta = None
        if ("voltageLower" in cfg) != ("voltageUpper" in cfg):
            raise design.DesignError("voltageLower and voltageUpper must be supplied together")
        if "voltageLower" in cfg:
            self.lower = _array(cfg["voltageLower"], "voltageLower", (self.m,))
            self.upper = _array(cfg["voltageUpper"], "voltageUpper", (self.m,))
            if np.any(self.lower >= self.upper) or np.any(self.va0 < self.lower) or np.any(self.va0 > self.upper):
                raise design.DesignError("absolute voltage envelope or initial voltage invalid")
        if "deltaVoltageLimits" in cfg:
            self.delta = _array(cfg["deltaVoltageLimits"], "deltaVoltageLimits", (self.m,))
            if np.any(self.delta <= 0):
                raise design.DesignError("deltaVoltageLimits must be positive per-sample limits")
        self.initial_controller, self.initial_prior = np.zeros(0), np.zeros(0)
        self.initial_warm = np.zeros((0, self.m))
        self.design_certificate, self.observer = {}, None
        self.reference = _array(cfg.get("reference", [0.]*self.p), "reference", (self.p,))
        self.bias = _array(cfg.get("bias", [0.]*self.m), "bias", (self.m,))
        if self.kind == "velocity_feedback":
            if any(self.source.outputs[i][1] not in ("m/s", "rad/s") for i in self.selected):
                raise design.DesignError("velocity feedback requires velocity readout units")
            self.gain = _gain(cfg["gain"], "gain", self.m, self.p)
        if self.kind in ("PI", "PD", "PID"):
            self.kp = _gain(cfg["kp"], "kp", self.m, self.p)
            if self.kind in ("PI", "PID"):
                self.ki = _gain(cfg["ki"], "ki", self.m, self.p)
            if self.kind in ("PD", "PID"):
                self.kd = _gain(cfg["kd"], "kd", self.m, self.p)
                self.alpha = _number(cfg["derivativeFilterAlpha"], "derivativeFilterAlpha")
                if not 0 <= self.alpha < 1:
                    raise design.DesignError("derivativeFilterAlpha must satisfy 0 <= alpha < 1")
        if self.kind == "discrete_state_space":
            ac = design._array(cfg["Ac"], "Ac")
            if ac.ndim != 2 or len(ac) == 0 or ac.shape != (len(ac), len(ac)):
                raise design.DesignError("Ac must be nonempty square")
            self.Ac = _array(cfg["Ac"], "Ac", ac.shape)
            nz = len(ac)
            self.Bc, self.Cc, self.Dc = (_array(cfg[key], key, shape) for key, shape in (
                ("Bc", (nz, self.p)), ("Cc", (self.m, nz)), ("Dc", (self.m, self.p))))
            self.initial_controller = _array(cfg.get("initialState", [0.]*nz), "initialState", (nz,))
            if _number(cfg["samplingPeriod"], "controller samplingPeriod", True) != self.ts:
                raise design.DesignError("controller samplingPeriod differs from native control period")
            if cfg["inputUnits"] != [self.source.outputs[i][1] for i in self.selected] or cfg["outputUnits"] != ["V"]*self.m:
                raise design.DesignError("discrete controller port units mismatch")
        if self.kind in ("LQR", "MPC"):
            self.state_reference = _array(cfg.get("stateReference", [0.]*self.nx), "stateReference", (self.nx,))
            if np.any(self.state_reference != 0):
                raise design.DesignError("nonzero stateReference requires a separately certified affine equilibrium and reference input")
            Q, R = _array(cfg["Q"], "Q", (self.nx, self.nx)), _array(cfg["R"], "R", (self.m, self.m))
            if self.kind == "LQR":
                self.control_design = control.discrete_lqr(self.discrete, Q, R)
                d = self.control_design
                self.design_certificate = {"K": d.K.tolist(), "P": d.P.tolist(),
                    "riccatiRelativeResidual": d.riccati_relative_residual, "closedPoles": _poles(d.closed_poles)}
            else:
                if self.lower is None or self.delta is None:
                    raise design.DesignError("MPC requires absolute amplitude and per-sample rate limits")
                Qf = _array(cfg["Qf"], "Qf", (self.nx, self.nx))
                self.control_design = control.constrained_mpc(self.discrete, Q, R, Qf,
                    _integer(cfg["horizon"], "horizon", 1), lower=self.lower-self.va0,
                    upper=self.upper-self.va0, delta=self.delta)
                self.initial_warm = np.zeros((self.control_design.horizon, self.m))
                self.verification_tol = _number(cfg.get("verificationTolerance", 1e-6), "verificationTolerance", True)
                self.max_iterations = _integer(cfg.get("maxIterations", 500), "maxIterations", 1)
                self.design_certificate = {"horizon": self.control_design.horizon}
            if self.state_source == "sensor_only":
                observer = cfg.get("observer")
                _keys(observer, ("processCovariance", "measurementCovariance"), ("initialPrior",), "observer")
                w = _array(observer["processCovariance"], "processCovariance", (self.nx, self.nx))
                v = _array(observer["measurementCovariance"], "measurementCovariance", (self.p, self.p))
                self.observer = control.steady_state_kalman(self.discrete, w, v, selected_outputs=self.selected)
                self.initial_prior = _array(observer.get("initialPrior", [0.]*self.nx), "initialPrior", (self.nx,))
                d = self.observer
                self.design_certificate["observer"] = {"correctionGain": d.correction_gain.tolist(),
                    "Pprior": d.Pprior.tolist(), "riccatiRelativeResidual": d.riccati_relative_residual,
                    "closedPoles": _poles(d.closed_poles)}
            elif "observer" in cfg:
                raise design.DesignError("observer requires sensor_only stateSource")

    def _configure_frame(self, source_frame, provided):
        self.family, self.frame_basis = None, None
        if not provided:
            return
        _keys(source_frame, ("family", "frame"), label="sourceFrame")
        self.family = source_frame["family"]
        if self.family not in ("planarBeam", "flatPlate", "axialRod", "solid3D"):
            raise design.DesignError("unsupported force source family")
        if self.physical_components != (3 if self.family == "axialRod" else 6):
            raise design.DesignError("source family differs from physical node layout")
        frame = source_frame["frame"]
        _keys(frame, ("origin", "ex", "ey", "ez"), label="source frame")
        origin = _array(frame["origin"], "frame origin", (3,))
        basis = np.column_stack([_array(frame[key], "frame "+key, (3,)) for key in ("ex", "ey", "ez")])
        if np.max(np.abs(basis.T@basis-np.eye(3))) > 1e-10 or abs(linalg.det(basis)-1.) > 1e-10:
            raise design.DesignError("source frame must be proper orthonormal")
        if self.family == "axialRod" and (not np.array_equal(basis, np.eye(3)) or np.any(origin != 0)):
            raise design.DesignError("axial source frame must be global identity")
        self.frame_basis = design._freeze(basis)

    def _configure_external(self):
        """Freeze connection identities offline. Opening an endpoint is a runtime action."""
        self.external_specs, self.external_identities = {}, {}
        self.external_controller_initial = {}
        if not self.excitation._sampled and self.kind != "external":
            return
        from .piezo_external_io import normalize_config, fingerprint, connection_identity

        def register(raw, ports, seed):
            connection = normalize_config(raw)
            if max(connection["handshakeTimeout"], connection["requestTimeout"]) > 50:
                raise design.DesignError("external timeout exceeds the native 50-second operation budget")
            key = fingerprint(connection)
            if key in self.external_specs:
                spec = self.external_specs[key]
                if spec["seed"] != seed:
                    raise design.DesignError("shared external connection requires the same seed")
                spec["ports"].extend(ports)
            else:
                self.external_specs[key] = dict(connection=connection, ports=ports, seed=seed)
                self.external_identities[key] = connection_identity(connection)
            return key

        def port(pid, unit, role, direction, reference, binding, initial=0.):
            quantities = {"N": "force", "N m": "moment", "V": "voltage", "m": "displacement",
                "m/s": "velocity", "m/s^2": "acceleration", "rad": "rotation",
                "rad/s": "angularVelocity", "rad/s^2": "angularAcceleration", "C": "charge"}
            return dict(id=pid, unit=unit, role=role, direction=direction, reference=reference,
                quantity=quantities.get(unit, unit), binding=binding, initialValue=float(initial),
                coordinateSystem="global", positiveDirection="+")

        self._external_controller_key = None
        if self.kind == "external":
            cfg = self.controller.get("external")
            _keys(cfg, ("connection",), ("seed", "initialState"), "external controller")
            seed = _integer(cfg.get("seed", 0), "external controller seed")
            self.external_controller_initial = copy.deepcopy(cfg.get("initialState", {}))
            self._external_state_bound(self.external_controller_initial)
            ports = [port("readout:"+self.source.outputs[i][0], self.source.outputs[i][1], "controller", "input", "increment",
                          {"outputId": self.source.outputs[i][0]}) for i in self.selected]
            ports += [port("actuator:"+pid, unit, "controller", "output", "absolute", {"actuatorId": pid}, self.va0[i])
                      for i, (pid, unit) in enumerate(self.source.inputs)]
            if self.state_source == "full_fe":
                for pid, unit in self.source.dofs:
                    ports += [port("state:"+pid+suffix, u, "controller", "input", "increment", {"dofId": pid})
                              for suffix, u in ((":q", unit), (":v", "m/s" if unit == "m" else "rad/s"))]
            self._external_controller_key = register(cfg["connection"], ports, seed)
            self.design_certificate = {"kind": "external", "stateSource": self.state_source,
                "commandReference": "absolute", "explicitState": True, "operationBudgetSeconds": 50.,
                "maximumExplicitStateBytes": _EXTERNAL_STATE_BYTES}
        catalog = {c.channel_id: c for c in self.excitation.catalog}
        self._external_excitation_keys = []
        for source in self.excitation._sampled:
            self._external_state_bound(source["initialState"])
            ports = []
            for cid in source["channelIds"]:
                channel = catalog[cid]
                p = port("force:"+cid, channel.unit, "excitation", "output", "absolute",
                    {"nodeId": str(channel.physical_node_id), "dof": channel.dof})
                p["coordinateSystem"] = channel.coordinate_system
                ports.append(p)
            key = register(source["connection"], ports, source["seed"])
            if key in self._external_excitation_keys:
                raise design.DesignError("one connection must declare its excitation ports in one externalSources entry")
            self._external_excitation_keys.append(key)
        if self.excitation._sampled:
            if self.certification_target is None:
                raise design.DesignError("sampled external excitation requires targetSteps for offline ledger capacity admission")
            # A finite binary64 JSON number needs at most 25 bytes. Each compact
            # sample stores its ordered values and one 64-character state hash;
            # canonical clocks, units and physical bindings are already frozen.
            maximum = 2+len(self.excitation._sampled)*3
            for source in self.excitation._sampled:
                count = (self.certification_target+source["sampleEveryFESteps"]-1)//source["sampleEveryFESteps"]
                maximum += count*(75+26*len(source["channelIds"]))
            if maximum > _EXTERNAL_LEDGER_BYTES:
                raise design.DesignError("sampled external excitation targetSteps exceeds 4 MiB frozen ledger capacity; shorten the run or increase sampleEveryFESteps")
            self.force_package["externalLedgerCapacity"] = {
                "format": "per-source-ordered-values-and-state-sha256;clock-from-frozen-stride",
                "maximumBytes": _EXTERNAL_LEDGER_BYTES, "admittedWorstCaseBytes": maximum,
                "maximumExplicitStateBytes": _EXTERNAL_STATE_BYTES,
                "targetSteps": self.certification_target,
                "storage": "accepted-checkpoint-only;no-sidecar",
                "scaling": "cumulative-checkpoints-copy-prefix;not-a-streaming-log"}

    @staticmethod
    def _external_state_bound(state):
        _finite_tree(state)
        if len(json.dumps(state, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode()) > _EXTERNAL_STATE_BYTES:
            raise design.DesignError("external explicit state exceeds the declared 16 KiB per-role state contract")

    def _external_client(self, key, state):
        from .piezo_external_io import ExternalIOClient, fingerprint
        spec = self.external_specs[key]
        if fingerprint(spec["connection"]) != key:
            raise design.DesignError("external algorithm/dependency identity changed; reload required")
        if key not in self._external_clients:
            if state["feStep"] and spec["connection"]["transport"] == "tcp":
                raise design.DesignError("cannot resume self-reported TCP algorithm identity")
            client = ExternalIOClient(spec["connection"], run_id=self._external_run_id,
                model_digest=self.source.sha256, config_digest=self.identity,
                mapping_digest=_digest(spec["ports"]), ports=spec["ports"], seed=spec["seed"])
            try:
                handshake = client.handshake(deadline=self._external_deadline)
                initial = {}
                if key == self._external_controller_key:
                    initial["controller"] = self.external_controller_initial
                for index, source_key in enumerate(self._external_excitation_keys):
                    if key == source_key:
                        initial["excitation"] = self.excitation._sampled[index]["initialState"]
                if handshake["initialState"] != initial:
                    raise design.DesignError("external handshake initialState differs from frozen configuration")
            except Exception:
                client.close()
                raise
            self._external_clients[key] = client
        return self._external_clients[key]

    def _external_evaluate(self, key, role, accepted_context, **kwargs):
        # Frozen retry results are proposals, never an accepted-state authority.
        cache_key = _digest(dict(key=key, role=role, **kwargs))
        if cache_key not in self._external_frozen:
            result = self._external_client(key, accepted_context).evaluate(role,
                deadline=self._external_deadline, **kwargs)
            _finite_tree(result)
            self._external_state_bound(result.get("candidateState"))
            if len(self._external_frozen) >= 256:
                self._external_frozen.pop(next(iter(self._external_frozen)))
            self._external_frozen[cache_key] = copy.deepcopy(result)
        return copy.deepcopy(self._external_frozen[cache_key])

    @staticmethod
    def _external_outputs(response, expected):
        outputs = response.get("outputs")
        if not isinstance(outputs, list):
            raise design.DesignError("external outputs must be records")
        values = {}
        for record in outputs:
            _keys(record, ("id", "value", "unit", "reference"), label="external output")
            pid = record["id"]
            if not isinstance(pid, str) or pid not in expected or pid in values:
                raise design.DesignError("unknown or duplicate external output port")
            if (record["unit"], record["reference"]) != expected[pid]:
                raise design.DesignError("external output unit/reference mismatch")
            values[pid] = _number(record["value"], "external output")
        if set(values) != set(expected) or "candidateState" not in response:
            raise design.DesignError("external output missing port or candidateState")
        return values

    def _prepare_excitation(self, state):
        if not self.excitation._sampled:
            return state
        step = state["feStep"]
        if step >= self.certification_target:
            raise design.DesignError("external excitation preparation exceeds admitted targetSteps")
        if state["pendingExcitation"]:
            return state
        catalog = {c.channel_id: c for c in self.excitation.catalog}
        for index, source in enumerate(self.excitation._sampled):
            every = source["sampleEveryFESteps"]
            if step % every:
                continue
            response = self._external_evaluate(self._external_excitation_keys[index], "excitation", state,
                step=str(step), tick=str(step//every), time=self._time(step),
                sample_period=self.dt*every, state=state["excitationStates"][index],
                inputs=[], previous_applied=[], valid_until=self._time(step+every))
            values = self._external_outputs(response, {"force:"+cid: (catalog[cid].unit, "absolute") for cid in source["channelIds"]})
            values = {cid: values["force:"+cid] for cid in source["channelIds"]}
            entry = dict(source=index, step=step, time=self._time(step), validUntil=self._time(step+every),
                values=values, acceptedStateDigest=_transport_digest(state["excitationStates"][index]),
                candidateState=copy.deepcopy(response["candidateState"]))
            entry["digest"] = _transport_digest(entry)
            state["pendingExcitation"].append(entry)
        return state

    def _sampled_values(self, state, step, *, include_pending=True):
        values = {}
        for index, source in enumerate(self.excitation._sampled):
            sample_step = (step//source["sampleEveryFESteps"])*source["sampleEveryFESteps"]
            offset = sample_step//source["sampleEveryFESteps"]
            samples = state["excitationLedger"][index]
            found = [e for e in state["pendingExcitation"] if include_pending and e["source"] == index and e["step"] == sample_step]
            if offset < len(samples) and not found:
                values.update(zip(source["channelIds"], samples[offset][0]))
            elif offset == len(samples) and len(found) == 1:
                values.update(found[0]["values"])
            else:
                raise design.DesignError("missing or duplicate frozen external excitation sample")
        return values

    def _configure_force(self, node_ids, records, configuration):
        if not isinstance(node_ids, list) or not node_ids:
            raise design.DesignError("nodeIds requires physical node IDs")
        for value in node_ids:
            _integer(value, "nodeId")
        if len(set(node_ids)) != len(node_ids) or not isinstance(records, list):
            raise design.DesignError("duplicate nodeIds or invalid forceCatalog")
        self.node_ids, self.force_positions, self.force_declarations = tuple(node_ids), {}, {}
        channels, used = [], set()
        global_axes = ("X", "Y", "Z", "RX", "RY", "RZ")
        local_axes = ("EX", "EY", "EZ", "RX", "RY", "RZ")
        for record in records:
            _keys(record, ("channelId", "physicalNodeId", "dof", "unit"),
                  ("physicalIndex", "coordinateSystem", "physicalDirection", "disturbanceColumn"), "forceCatalog")
            coordinate = record.get("coordinateSystem", "global")
            channel = ForceChannel(record["channelId"], record["physicalNodeId"], record["dof"], record["unit"], coordinate)
            local = coordinate == "local"
            component = (local_axes if local else global_axes).index(channel.dof)
            if self.family == "solid3D" and component >= 3:
                raise design.DesignError("solid3D excitation only supports translational force ports")
            if component >= self.physical_components:
                raise design.DesignError("forceCatalog DOF is absent from the declared physical node layout")
            if channel.physical_node_id not in self.node_ids:
                raise design.DesignError("force node is absent from native nodeIds")
            offset = self.physical_components*self.node_ids.index(channel.physical_node_id)
            direction = np.zeros(6)
            if local:
                if self.frame_basis is None or self.family == "axialRod":
                    raise design.DesignError("local forces require a persisted layered source frame")
                allowed = (0, 2, 4) if self.family == "planarBeam" else (0, 1, 2) if self.family == "solid3D" else (0, 1, 2, 3, 4)
                if component not in allowed:
                    raise design.DesignError("unsupported source-family local force/moment axis")
                if channel.channel_id != f"node:{channel.physical_node_id}:local:{channel.dof}" or "physicalIndex" in record:
                    raise design.DesignError("local force binding requires a named source-frame axis")
                block = 3*(component//3)
                direction[block:block+3] = self.frame_basis[:, component % 3]
                if "physicalDirection" not in record:
                    raise design.DesignError("local force requires physicalDirection")
            else:
                index = _integer(record.get("physicalIndex"), "physicalIndex")
                if index != offset+component:
                    raise design.DesignError("forceCatalog physicalIndex or binding mismatch")
                direction[component] = 1.
                if self.frame_basis is not None and self.family != "axialRod":
                    admitted = (0, 2) if self.family == "planarBeam" and component < 3 else (1,) if self.family == "planarBeam" else (0, 1, 2) if component < 3 else (0, 1)
                    basis = self.frame_basis[:, admitted]
                    block = direction[3*(component//3):3*(component//3)+3]
                    if linalg.norm(block-basis@(basis.T@block)) > 1e-10:
                        raise design.DesignError("global force axis lies outside source-family supported subspace")
            if ("coordinateSystem" in record) != ("physicalDirection" in record):
                raise design.DesignError("force coordinateSystem and physicalDirection must be supplied together")
            if "physicalDirection" in record and not np.array_equal(
                    _array(record["physicalDirection"], "physicalDirection", (6,)), direction):
                raise design.DesignError("physicalDirection differs from the named source-frame axis")
            binding = (channel.physical_node_id, coordinate, channel.dof)
            if channel.channel_id in self.force_positions or binding in used:
                raise design.DesignError("forceCatalog duplicate physicalIndex or binding mismatch")
            used.add(binding)
            self.force_positions[channel.channel_id] = (offset, tuple(direction[:self.physical_components]))
            if "disturbanceColumn" in record:
                self.force_declarations[channel.channel_id] = _integer(record["disturbanceColumn"], "disturbanceColumn")
            channels.append(channel)
        self.excitation = compile_excitation(channels, configuration)

    def _physical_readout_rom(self, reduced, full):
        """Project actual FE output rows; retain physical input direct terms.

        Acceleration output is a native measurement. Its instantaneous force
        response must not disappear when a mode is truncated from the dynamics.
        """
        cy = np.hstack((full.Cy[:, :self.n]@self.Phi, full.Cy[:, self.n:]@self.Phi))
        return design.StateSpace(reduced.A, reduced.B, design._freeze(cy), full.Dy,
            full.source, design._digest(reduced.A, reduced.B, cy, full.Dy, self.Phi),
            reduced.provenance)

    def _configure_disturbance(self, msg, excitation, actuator_rom):
        fields = ("Bforce", "Dforce", "disturbanceInputs", "initialTime")
        provided = [key in msg for key in fields]
        if any(provided) and not all(provided):
            raise design.DesignError("Bforce/Dforce/disturbanceInputs/initialTime must be supplied together")
        self.force_group = all(provided)
        configured = {entry["channelId"] for entry in excitation["channels"]}
        configured.update(c.channel_id for external in self.excitation._external for c in external.channels)
        configured.update(cid for external in self.excitation._sampled for cid in external["channelIds"])
        if self.excitation._sampled and (self.kind == "MPC" or self.observer is not None):
            raise design.DesignError("sampled external excitation does not support internal MPC future-preview or observer force prediction")
        catalog = {channel.channel_id: channel for channel in self.excitation.catalog}
        self.initial_time = _number(msg.get("initialTime", 0.), "initialTime")
        if "initialTimeBits" in msg:
            if "initialTime" not in msg:
                raise design.DesignError("initialTimeBits requires numeric initialTime")
            exact = _binary64(msg["initialTimeBits"], "initialTimeBits")
            if exact != self.initial_time:
                raise design.DesignError("initialTimeBits differs from numeric initialTime")
            self.initial_time = exact
        self.force_columns = []
        if self.force_group:
            entries = msg["disturbanceInputs"]
            if not isinstance(entries, list):
                raise design.DesignError("disturbanceInputs must be an array")
            combined_source = design.SourceIdentity(self.source.sha256, self.source.dofs,
                self.source.inputs+tuple(tuple(v) for v in entries), self.source.outputs)
            self.nf = len(entries)
            self.Bforce = _array(msg["Bforce"], "Bforce", (self.n, self.nf))
            self.Dforce = _array(msg["Dforce"], "Dforce", (self.ny, self.nf))
            if set(self.force_declarations) != configured or self.nf != len(configured):
                raise design.DesignError("disturbance columns must map exactly the configured force channels")
            if sorted(self.force_declarations.values()) != list(range(self.nf)):
                raise design.DesignError("disturbanceColumn must be unique and contiguous")
            for column, (cid, unit) in enumerate(entries):
                if cid not in configured or cid not in catalog or self.force_declarations.get(cid) != column:
                    raise design.DesignError("disturbance input ID/column mapping mismatch")
                if unit != catalog[cid].unit:
                    raise design.DesignError("disturbance input force/moment unit mismatch")
                self.force_columns.append(cid)
        else:
            if self.force_declarations:
                raise design.DesignError("disturbanceColumn requires supplied force operators")
            self.nf = 0
            self.Bforce, self.Dforce = np.zeros((self.n, 0)), np.zeros((self.ny, 0))
            combined_source = self.source
            if configured and (self.kind == "MPC" or (self.kind == "LQR" and self.state_source == "sensor_only")):
                raise design.DesignError("configured forcing requires native force operators for MPC/sensor-only model control")
        if "physicalForceDirections" in msg:
            supplied = _array(msg["physicalForceDirections"], "physicalForceDirections",
                              (self.physical_components*len(self.node_ids), self.nf))
            expected = np.zeros(supplied.shape)
            for column, cid in enumerate(self.force_columns):
                offset, direction = self.force_positions[cid]
                expected[offset:offset+self.physical_components, column] = direction
            if not np.array_equal(supplied, expected):
                raise design.DesignError("physicalForceDirections differs from ordered named channel directions")
        self.bias_force = _array(msg.get("biasForce", [0.]*self.n), "biasForce", (self.n,))
        combined_b = np.hstack((self.B, self.Bforce))
        combined_d = np.hstack((self.D, self.Dforce))
        full = design.continuous_state_space(self.M, self.K, self.C, combined_b,
            source=combined_source, Lq=self.Lq, Lv=self.Lv, La=self.La, Dinput=combined_d)
        if self.nf:
            # Same verified mechanical basis, now bound to the additional actual
            # input names; source digest and M/K matrix identity stay unchanged.
            combined_modes = replace(self.modes, source=combined_source)
            reduced = design.reduce_model(self.M, self.K, self.C, combined_b, combined_modes,
                source=combined_source, Lq=self.Lq, Lv=self.Lv, La=self.La, Dinput=combined_d).state_space
        else:
            reduced = actuator_rom.state_space
        self.combined_rom = self._physical_readout_rom(reduced, full)
        self.force_direct = full.Dy[:, self.m:]
        physical_acceleration = linalg.solve(self.M, self.bias_force, assume_a="pos")
        self.output_bias = self.La@physical_acceleration
        full_bias = np.concatenate((np.zeros(self.n), physical_acceleration))
        r = self.Phi.shape[1]
        self.bias_derivative = np.concatenate((np.zeros(r), linalg.solve(
            actuator_rom.M, self.Phi.T@self.bias_force, assume_a="pos")))
        self.rom_foh_dt = self._foh(self.combined_rom, self.bias_derivative, self.dt)
        self.rom_foh_ts = self._foh(self.combined_rom, self.bias_derivative, self.ts)
        self._interval_cache = {self.dt: (self.substep, self.rom_foh_dt)}
        self._clock_interval(0)  # Reject a collapsed/nonfinite first clock interval.
        self._qualify_sampling_clock(0)
        fe_foh_dt = self._foh(full, full_bias, self.dt)
        def package(system, bias):
            def ss(s):
                return {"A": s.A.tolist(), "B": s.B.tolist(), "Cy": s.Cy.tolist(),
                        "D": s.Dy.tolist(), "matrixIdentity": s.matrix_sha256}
            return {"continuous": ss(system), "discrete": ss(design.exact_zoh(system, self.ts)),
                    "substep": ss(design.exact_zoh(system, self.dt)),
                    "affineDerivative": bias.tolist(), "outputBias": self.output_bias.tolist()}
        def foh_result(result):
            return {"leftForce": result[0].tolist(), "rightForce": result[1].tolist(),
                    "affineIntegral": result[2].tolist()}
        qualification = ("complete-known-forcing" if self.force_group else
                         "actuator-only-unmapped-forcing" if configured else
                         "complete-known-affine" if np.any(self.bias_force) else "actuator-only")
        self.force_package = {"qualification": qualification,
            "outputContract": "projected-full-FE-readout-with-retained-direct-terms",
            "allConfiguredForcesMapped": not configured or self.force_group,
            "interpolation": ("external-FE-interval-ZOH-plus-deterministic-time-sources" if self.excitation._sampled else "FOH-endpoint-linear-approximation-to-source-waveform"),
            "runtimeTimeContract": "canonical-initialTime-plus-step-times-dt;actual-endpoint-difference",
            "exportPeriodContract": "nominal-dt-and-control-period;runtime-MPC-composes-actual-FE-intervals",
            "initialTime": self.initial_time,
            "source": {"sourceFingerprint": combined_source.sha256,
                       "dofs": [list(v) for v in combined_source.dofs],
                       "inputs": [list(v) for v in combined_source.inputs],
                       "outputs": [list(v) for v in combined_source.outputs]},
            "basisIdentity": design._digest(self.Phi),
            "FE": package(full, full_bias), "ROM": package(self.combined_rom, self.bias_derivative),
            "foh": {"FEsubstep": foh_result(fe_foh_dt), "ROMsubstep": foh_result(self.rom_foh_dt),
                    "ROMcontrol": foh_result(self.rom_foh_ts)}}

    def _foh(self, system, bias, period):
        n = len(system.A)
        # Augmented state [x, f, df/dt, constant]. The native held voltage is
        # handled independently by the existing exact-ZOH control operators.
        generator = np.zeros((n+2*self.nf+1, n+2*self.nf+1))
        generator[:n, :n] = system.A
        generator[:n, n:n+self.nf] = system.B[:, self.m:]
        generator[n:n+self.nf, n+self.nf:n+2*self.nf] = np.eye(self.nf)
        generator[:n, -1] = bias
        transition = design._array(linalg.expm(generator*period), "FOH transition")
        right = transition[:n, n+self.nf:n+2*self.nf]/period
        left = transition[:n, n:n+self.nf]-right
        return (design._freeze(left), design._freeze(right), design._freeze(transition[:n, -1]))

    def _time(self, step):
        return _number(self.initial_time+step*self.dt, "native clock grid")

    def _clock_interval(self, step):
        left_time, right_time = self._time(step), self._time(step+1)
        h = _number(right_time-left_time, "actual native clock interval", True)
        return left_time, right_time, h

    def _qualify_sampling_clock(self, step):
        if self.kind not in ("LQR", "MPC", "discrete_state_space"):
            return
        horizon = self.control_design.horizon if self.kind == "MPC" else 1
        if self.kind == "MPC" and horizon*self.N > 20000:
            raise design.DesignError("MPC forecast exceeds 20000 actual FE intervals")
        # Constant-time admission independent of arbitrary N. Recheck on every
        # proposal/boundary; the native caller additionally gates its full grid.
        for k in range(min(32, max(8, horizon))):
            start = step+k*self.N
            actual = self._time(start+self.N)-self._time(start)
            if not np.isfinite(actual) or actual <= 0 or abs(actual-self.ts)/self.ts > 1e-10:
                raise design.DesignError("sampling period quantization exceeds relative 1e-10 nominal design qualification")

    def _interval(self, step):
        left_time, right_time, h = self._clock_interval(step)
        # This cache contains only source-bound, reproducible numerical designs,
        # never controller/observer history or an accepted-step counter.
        if h not in self._interval_cache:
            self._interval_cache[h] = (design.exact_zoh(self.continuous, h),
                self._foh(self.combined_rom, self.bias_derivative, h))
        system, foh = self._interval_cache[h]
        return left_time, right_time, system, foh

    def _forecast(self, step, horizon):
        matrices_a, matrices_b, offsets = [], [], []
        for k in range(horizon):
            a, b, offset = np.eye(self.nx), np.zeros((self.nx, self.m)), np.zeros(self.nx)
            for j in range(self.N):
                t0, t1, system, foh = self._interval(step+k*self.N+j)
                a = system.A@a
                b = system.A@b+system.B
                offset = system.A@offset+self._known_offset(t0, t1, foh)
            matrices_a.append(a)
            matrices_b.append(b)
            offsets.append(offset)
        return np.array(matrices_a), np.array(matrices_b), np.array(offsets)

    def _forces_at(self, time):
        if not self.nf:
            return np.zeros(0)
        values = self.excitation.evaluate(time)["forces"]
        return np.array([values[cid]["value"] for cid in self.force_columns])

    def _known_offset(self, left_time, right_time, foh):
        result = foh[0]@self._forces_at(left_time)+foh[1]@self._forces_at(right_time)+foh[2]
        return design._array(result, "known force/affine state offset", (self.nx,))

    def _state(self, value):
        external_keys = ("externalControllerState", "excitationStates", "excitationLedger", "excitationLedgerDigest", "pendingExcitation", "externalHeldAbsolute") if self.external_specs else ()
        _keys(value, ("schemaVersion", "configurationIdentity", "sourceFingerprint", "feStep", "controlTick", "phase",
                      "heldDelta", "integral", "previousError", "filteredDerivative", "controllerState",
                      "observerPrior", "warmStart", "previousApplied")+external_keys, label="opaqueState")
        if (type(value["schemaVersion"]) is not int or value["schemaVersion"] != 1
                or value["configurationIdentity"] != self.identity or value["sourceFingerprint"] != self.source.sha256):
            raise design.DesignError("opaque state source or configuration identity mismatch")
        step = _integer(value["feStep"], "feStep")
        tick = _integer(value["controlTick"], "controlTick")
        phase = value["phase"]
        if phase not in ("ready", "proposed"):
            raise design.DesignError("opaque state phase invalid")
        expected = (step+self.N-1)//self.N
        if phase == "proposed":
            if step % self.N:
                raise design.DesignError("proposal phase must be on a control sample")
            expected += 1
        if tick != expected:
            raise design.DesignError("opaque state counter/tick/phase mismatch")
        for key, shape in (("heldDelta", (self.m,)), ("integral", (self.p,)),
                ("previousError", (self.p,)), ("filteredDerivative", (self.p,)),
                ("controllerState", self.initial_controller.shape),
                ("observerPrior", self.initial_prior.shape), ("warmStart", self.initial_warm.shape),
                ("previousApplied", (self.m,))):
            # JSON [] cannot encode the trailing dimension of an empty matrix.
            if key == "warmStart" and self.initial_warm.size == 0 and value[key] == []:
                continue
            _array(value[key], key, shape)
        if self.lower is not None:
            previous = np.asarray(value["previousApplied"])
            if np.any(previous < self.lower) or np.any(previous > self.upper):
                raise design.DesignError("previousApplied exceeds absolute amplitude envelope")
        if self.external_specs:
            self._validate_external_state(value)
        return copy.deepcopy(value)

    def _validate_external_state(self, state):
        self._external_state_bound(state["externalControllerState"])
        if state["feStep"] == 0 and state["phase"] == "ready":
            actual = {k: v for k, v in state.items() if k != "pendingExcitation"}
            initial = {k: v for k, v in self._initial_opaque_state.items() if k != "pendingExcitation"}
            if _transport_digest(actual) != _transport_digest(initial):
                changed = sorted(key for key in set(actual) | set(initial)
                    if key not in actual or key not in initial
                    or _transport_digest(actual[key]) != _transport_digest(initial[key]))
                raise design.DesignError("initial external checkpoint differs from frozen initial state: " + ",".join(changed))
        held = _array(state["externalHeldAbsolute"], "externalHeldAbsolute", (self.m,))
        if self.kind == "external" and not np.array_equal(held-self.va0, np.array(state["heldDelta"])):
            raise design.DesignError("external absolute command differs from explicit operating-point increment")
        states, ledger, pending = (state[k] for k in ("excitationStates", "excitationLedger", "pendingExcitation"))
        if (not isinstance(states, list) or len(states) != len(self.excitation._sampled)
                or not isinstance(ledger, list) or len(ledger) != len(states) or not isinstance(pending, list)):
            raise design.DesignError("invalid external excitation checkpoint")
        if self.excitation._sampled and state["feStep"] > self.certification_target:
            raise design.DesignError("external excitation checkpoint exceeds admitted targetSteps")
        if state["excitationLedgerDigest"] != _transport_digest(ledger):
            raise design.DesignError("excitation ledger digest mismatch")
        if len(json.dumps(ledger, separators=(",", ":"), allow_nan=False).encode()) > _EXTERNAL_LEDGER_BYTES:
            raise design.DesignError("external input ledger exceeds admitted 4 MiB capacity")
        for index, source in enumerate(self.excitation._sampled):
            self._external_state_bound(states[index])
            samples = ledger[index]
            expected = (state["feStep"]+source["sampleEveryFESteps"]-1)//source["sampleEveryFESteps"]
            if not isinstance(samples, list) or len(samples) != expected:
                raise design.DesignError("excitation checkpoint sample sequence mismatch")
            for record in samples:
                if not isinstance(record, list) or len(record) != 2:
                    raise design.DesignError("invalid compact excitation ledger record")
                _array(record[0], "excitation ledger forces", (len(source["channelIds"]),))
                _hex_bytes(record[1], 32, "excitation candidate state digest")
            expected_digest = samples[-1][1] if samples else _transport_digest(source["initialState"])
            if _transport_digest(states[index]) != expected_digest:
                raise design.DesignError("excitation checkpoint state differs from accepted ledger")
        pending_sources = set()
        for entry in pending:
            _keys(entry, ("source", "step", "time", "validUntil", "values", "acceptedStateDigest", "candidateState", "digest"), label="pending excitation entry")
            index = _integer(entry["source"], "excitation source")
            if index >= len(states) or index in pending_sources:
                raise design.DesignError("unknown or duplicate pending excitation source")
            source = self.excitation._sampled[index]
            sample_step = _integer(entry["step"], "excitation sample step")
            if sample_step != state["feStep"] or sample_step % source["sampleEveryFESteps"] or sample_step >= self.certification_target:
                raise design.DesignError("pending excitation sample sequence mismatch")
            if entry["time"] != self._time(sample_step) or entry["validUntil"] != self._time(sample_step+source["sampleEveryFESteps"]):
                raise design.DesignError("pending excitation clock mismatch")
            if entry["acceptedStateDigest"] != _transport_digest(states[index]):
                raise design.DesignError("pending excitation accepted state mismatch")
            if entry["digest"] != _transport_digest({k: v for k, v in entry.items() if k != "digest"}):
                raise design.DesignError("pending excitation digest mismatch")
            if not isinstance(entry["values"], dict) or set(entry["values"]) != set(source["channelIds"]):
                raise design.DesignError("pending excitation output mapping mismatch")
            for force in entry["values"].values():
                _number(force, "pending excitation force")
            self._external_state_bound(entry["candidateState"])
            pending_sources.add(index)

    def _readouts(self, raw):
        if not isinstance(raw, list):
            raise design.DesignError("readouts requires records")
        required_ids = {self.source.outputs[i][0] for i in self.selected}
        known = dict(self.source.outputs)
        values = {}
        for record in raw:
            _keys(record, ("portId", "deltaValue", "actionRecord", "unit"), label="readout")
            port = record["portId"]
            if not isinstance(port, str) or port not in known or port in values or type(record["actionRecord"]) is not bool:
                raise design.DesignError("unknown or duplicate readout or invalid actionRecord")
            if record["unit"] != known[port]:
                raise design.DesignError("readout unit mismatch")
            if port in required_ids and record["actionRecord"]:
                raise design.DesignError("an action record is not an independent measurement")
            values[port] = _number(record["deltaValue"], "readout deltaValue")
        if not required_ids <= set(values) or (self.state_source == "sensor_only" and set(values) != required_ids):
            raise design.DesignError("readouts do not match selected measurements")
        return np.array([values[self.source.outputs[i][0]] for i in self.selected])

    def _force(self, msg):
        _keys(msg, ("op", "time"), ("opaqueState", "feStep", "replayOnly"), label="force")
        candidate = None
        if self.excitation._sampled:
            if "opaqueState" not in msg:
                raise design.DesignError("sampled force requires explicit opaqueState")
            candidate = self._state(msg["opaqueState"])
            step = _integer(msg.get("feStep", candidate["feStep"]), "force feStep")
            replay = msg.get("replayOnly", False)
            if type(replay) is not bool or step > candidate["feStep"]:
                raise design.DesignError("invalid force replay/step")
            t = _number(msg["time"], "force time")
            if not self._time(step) <= t <= self._time(step+1):
                raise design.DesignError("sampled force query is outside its explicit FE interval")
            if not replay:
                if step != candidate["feStep"] or candidate["phase"] != "ready":
                    raise design.DesignError("force preparation requires the current ready FE boundary")
                candidate = self._prepare_excitation(candidate)
                self._state(candidate)
            frame = self.excitation.evaluate(t, sampled_values=self._sampled_values(candidate, step))
        else:
            frame = self.excitation.evaluate_frame({"time": msg["time"]})
        contributions = [[] for _ in range(self.physical_components*len(self.node_ids))]
        for channel, value in frame["forces"].items():
            offset, direction = self.force_positions[channel]
            for component, weight in enumerate(direction):
                contributions[offset+component].append(weight*value["value"])
        forces = [math.fsum(values) for values in contributions]
        result = {"forces": forces, "sourceHash": frame["sourceHash"], "loadedCodeHash": frame["loadedCodeHash"]}
        if candidate is not None:
            result["candidateOpaqueState"] = candidate
        return result

    def _propose(self, msg):
        _keys(msg, ("op", "opaqueState"), ("q", "v", "readouts"), "propose")
        state = self._state(msg["opaqueState"])
        if state["phase"] != "ready" or state["feStep"] % self.N:
            raise design.DesignError("propose requires ready state on a control sample")
        self._qualify_sampling_clock(state["feStep"])
        if self.state_source == "sensor_only" and ("q" in msg or "v" in msg):
            raise design.DesignError("sensor_only cannot receive FE q/v")
        delta, diagnostics = np.zeros(self.m), {}
        requires_measurements = self.kind not in ("none", "LQR", "MPC") or self.observer is not None
        y = self._readouts(msg.get("readouts", [])) if requires_measurements else None
        if self.excitation._sampled:
            self._sampled_values(state, state["feStep"])
        if self.kind == "external":
            inputs = [dict(id="readout:"+self.source.outputs[i][0], value=float(y[j]), unit=self.source.outputs[i][1], reference="increment")
                      for j, i in enumerate(self.selected)]
            if self.state_source == "full_fe":
                for name, suffix in (("q", ":q"), ("v", ":v")):
                    physical = _array(msg.get(name), name, (self.n,))
                    for (pid, unit), value in zip(self.source.dofs, physical):
                        inputs.append(dict(id="state:"+pid+suffix, value=float(value),
                            unit=unit if name == "q" else "m/s" if unit == "m" else "rad/s", reference="increment"))
            previous = [dict(id="actuator:"+pid, value=state["previousApplied"][i], unit="V", reference="absolute")
                        for i, (pid, _) in enumerate(self.source.inputs)]
            response = self._external_evaluate(self._external_controller_key, "controller", state,
                step=str(state["feStep"]), tick=str(state["controlTick"]), time=self._time(state["feStep"]),
                sample_period=self.ts, state=state["externalControllerState"], inputs=inputs, previous_applied=previous)
            values = self._external_outputs(response, {"actuator:"+pid: ("V", "absolute") for pid, _ in self.source.inputs})
            delta = np.array([values["actuator:"+pid] for pid, _ in self.source.inputs])-self.va0
            state["externalHeldAbsolute"] = [values["actuator:"+pid] for pid, _ in self.source.inputs]
            state["externalControllerState"] = copy.deepcopy(response["candidateState"])
            diagnostics["external"] = {"requestedAbsoluteVoltages": [values["actuator:"+pid] for pid, _ in self.source.inputs],
                "previousApplied": copy.deepcopy(state["previousApplied"])}
        elif self.kind in ("velocity_feedback", "PI", "PD", "PID", "discrete_state_space"):
            error = self.reference-y
            delta = self.bias.copy()
            if self.kind == "velocity_feedback":
                delta += self.gain@error
            elif self.kind == "discrete_state_space":
                z = np.array(state["controllerState"])
                delta += self.Cc@z+self.Dc@error
                state["controllerState"] = (self.Ac@z+self.Bc@error).tolist()
            else:
                delta += self.kp@error
                if self.kind in ("PI", "PID"):
                    delta += self.ki@np.array(state["integral"])
                    state["integral"] = (np.array(state["integral"])+self.ts*error).tolist()
                if self.kind in ("PD", "PID"):
                    raw_derivative = (error-np.array(state["previousError"]))/self.ts
                    filtered = self.alpha*np.array(state["filteredDerivative"])+(1-self.alpha)*raw_derivative
                    delta += self.kd@filtered
                    state["filteredDerivative"] = filtered.tolist()
            state["previousError"] = error.tolist()
        elif self.kind in ("LQR", "MPC"):
            if self.state_source == "full_fe":
                q, v = (_array(msg[key], key, (self.n,)) for key in ("q", "v"))
                x = np.concatenate((self.Phi.T@self.M@q, self.Phi.T@self.M@v))
            else:
                known_output = self.force_direct@self._forces_at(self._time(state["feStep"]))+self.output_bias
                correction = control.observer_correction(self.observer, self.discrete,
                    state["observerPrior"], y-known_output[list(self.selected)],
                    np.array(state["previousApplied"])-self.va0)
                x = correction.corrected_state
                state["observerPrior"] = x.tolist()
                diagnostics["innovation"] = correction.innovation.tolist()
            if self.kind == "LQR":
                delta = control.lqr_action(self.control_design, self.discrete, x-self.state_reference)
            else:
                matrices_a, matrices_b, offsets = self._forecast(state["feStep"], self.control_design.horizon)
                proposal = control.mpc_proposal(self.control_design, self.discrete,
                    x-self.state_reference, np.array(state["previousApplied"])-self.va0,
                    warm_start=state["warmStart"], state_offsets=offsets, verification_tol=self.verification_tol,
                    state_transitions=matrices_a, input_transitions=matrices_b,
                    max_iterations=self.max_iterations)
                delta = proposal.first_action
                state["warmStart"] = proposal.next_warm_start.tolist()
                c = proposal.certificate
                diagnostics["mpc"] = {"objective": proposal.objective, "primal": c.primal,
                    "stationarity": c.stationarity, "dual": c.dual,
                    "complementarity": c.complementarity, "actionStationarity": c.action_stationarity,
                    "stateOffsets": offsets.tolist(),
                    "actuatorPredictionScope": _actuator_prediction_scope()}
        state["heldDelta"] = _array(delta.tolist(), "controller delta", (self.m,)).tolist()
        state["controlTick"] += 1
        state["phase"] = "proposed"
        self._state(state)
        return {"absoluteVoltages": (state["externalHeldAbsolute"] if self.kind == "external" else (self.va0+delta).tolist()), "candidateOpaqueState": state,
                "controlSample": True, "diagnostics": diagnostics}

    def _postFE(self, msg):
        _keys(msg, ("op", "opaqueState", "appliedVoltages"), ("feStep",), "postFE")
        state = self._state(msg["opaqueState"])
        if state["phase"] == "ready" and state["feStep"] % self.N == 0:
            raise design.DesignError("postFE on a control sample requires a controller proposal")
        self._clock_interval(state["feStep"])
        self._qualify_sampling_clock((state["feStep"]//self.N)*self.N)
        applied = _array(msg["appliedVoltages"], "appliedVoltages", (self.m,))
        if self.excitation._sampled:
            self._sampled_values(state, state["feStep"])
        if self.lower is not None and (np.any(applied < self.lower) or np.any(applied > self.upper)):
            raise design.DesignError("applied voltage exceeds absolute amplitude envelope")
        # Native applies its FE-step slew limits while held requests stay fixed.
        # QP delta limits describe desired control samples, not actual FE ramps.
        # Every accepted actual command must enter prediction, including holds.
        if self.observer is not None:
            t0, t1, system, foh = self._interval(state["feStep"])
            offset = self._known_offset(t0, t1, foh)
            prior = system.A@np.array(state["observerPrior"])+system.B@(applied-self.va0)+offset
            state["observerPrior"] = _array(prior.tolist(), "observer prediction", (self.nx,)).tolist()
        state["previousApplied"] = applied.tolist()
        if self.external_specs:
            for entry in state["pendingExcitation"]:
                index = entry["source"]
                state["excitationStates"][index] = copy.deepcopy(entry["candidateState"])
                state["excitationLedger"][index].append([
                    [entry["values"][cid] for cid in self.excitation._sampled[index]["channelIds"]],
                    _transport_digest(entry["candidateState"])])
            state["excitationLedgerDigest"] = _transport_digest(state["excitationLedger"])
            state["pendingExcitation"] = []
        state["feStep"] += 1
        state["phase"] = "ready"
        if "feStep" in msg and _integer(msg["feStep"], "feStep") != state["feStep"]:
            raise design.DesignError("postFE completed step mismatch")
        self._state(state)
        return {"candidateOpaqueState": state}

    def _certificationBatch(self, msg):
        """Preflight complete native boundary fields and actual canonical forces.

        Caller record IDs bind its raw codec bytes; they are not authentication
        or a substitute for physical raw-runtime certification by native FE.
        This operation never proposes/accepts controller or observer history.
        """
        _keys(msg, ("op", "records", "times"), ("opaqueState",), label="certificationBatch")
        if self.certification_target is None:
            raise design.DesignError("certificationBatch requires frozen original targetSteps")
        records, times = msg["records"], msg["times"]
        if not isinstance(records, list) or len(records) > 128:
            raise design.DesignError("certificationBatch records must be an array of at most 128")
        if not isinstance(times, list) or len(times) > 129:
            raise design.DesignError("certificationBatch times must be an array of at most 129")

        def check_source():
            if provenance() != self.backend_identity:
                raise design.DesignError("backend identity changed during certificationBatch")
            self.excitation.check_revision()

        def step(value):
            value = _integer(value, "feStep")
            if value > self.certification_target:
                raise design.DesignError("certificationBatch FE step exceeds frozen original targetSteps")
            return value

        check_source()
        ids, record_steps, force_steps = set(), set(), set()
        boundaries, force_requests = [], []
        for record in records:
            _keys(record, ("recordId", "opaqueState", "feStep", "controlTick", "phase",
                           "appliedVoltages", "heldVoltages"), label="certificationBatch record")
            _hex_bytes(record["recordId"], 32, "recordId")
            actual_step = step(record["feStep"])
            if record["recordId"] in ids or actual_step in record_steps:
                raise design.DesignError("duplicate certificationBatch recordId or FE step")
            ids.add(record["recordId"])
            record_steps.add(actual_step)
            if record["phase"] != "ready":
                raise design.DesignError("certificationBatch requires ready accepted boundaries")
            boundaries.append(record)
        for entry in times:
            _keys(entry, ("feStep", "timeBits"), label="certificationBatch time")
            actual_step = step(entry["feStep"])
            if actual_step in force_steps:
                raise design.DesignError("duplicate certificationBatch force FE step")
            force_steps.add(actual_step)
            actual_time = _binary64(entry["timeBits"], "timeBits")
            expected_time = self.initial_time if actual_step == 0 else self._time(actual_step)
            if struct.pack("<d", actual_time) != struct.pack("<d", expected_time):
                raise design.DesignError("certificationBatch timeBits differs from original canonical clock")
            if actual_step:
                previous = self.initial_time if actual_step == 1 else self._time(actual_step-1)
                if actual_time <= previous:
                    raise design.DesignError("certificationBatch original clock interval collapsed")
            force_requests.append((entry, actual_time))

        validations = []
        for record in boundaries:
            boundary = {"op": "validate", **{key: value for key, value in record.items() if key != "recordId"}}
            validation = self._validate(boundary)
            validations.append({"recordId": record["recordId"], **validation})
        forces = []
        replay_state = msg.get("opaqueState")
        if self.excitation._sampled and replay_state is None and boundaries:
            replay_state = max(boundaries, key=lambda r: r["feStep"])["opaqueState"]
        for entry, actual_time in force_requests:
            request = {"op": "force", "time": actual_time}
            if self.excitation._sampled:
                if replay_state is None:
                    raise design.DesignError("sampled certification requires a frozen accepted input ledger")
                request.update(opaqueState=replay_state, replayOnly=True, feStep=max(0, entry["feStep"]-1))
            force = self._force(request)
            force.pop("candidateOpaqueState", None)
            forces.append({"feStep": entry["feStep"], "timeBits": entry["timeBits"],
                           "time": actual_time, **force})
        # Also runs for an empty final batch. Nothing is publishable on failure.
        check_source()
        return {"sourceFingerprint": self.source.sha256, "configurationIdentity": self.identity,
                "backend": copy.deepcopy(self.backend_identity), "records": validations, "forces": forces}

    def _validate(self, msg):
        _keys(msg, ("op", "opaqueState"), ("feStep", "controlTick", "phase", "appliedVoltages", "heldVoltages"), "validate")
        state = self._state(msg["opaqueState"])
        if "appliedVoltages" in msg:
            actual = _array(msg["appliedVoltages"], "appliedVoltages", (self.m,))
            if actual.tobytes() != np.asarray(state["previousApplied"], dtype=float).tobytes():
                raise design.DesignError("opaque previousApplied differs from accepted native appliedVoltages")
        if "heldVoltages" in msg:
            held = _array(msg["heldVoltages"], "heldVoltages", (self.m,))
            # Before the first proposal, native initializes the held absolute
            # command directly to Va0 (preserve its binary64 signed zero).
            expected_held = (self.va0 if state["controlTick"] == 0 else
                             np.array(state["externalHeldAbsolute"], dtype=float) if self.kind == "external" else
                             self.va0+np.array(state["heldDelta"], dtype=float))
            if held.tobytes() != expected_held.tobytes():
                raise design.DesignError("opaque heldDelta differs from accepted native heldVoltages")
        for key in ("feStep", "controlTick"):
            if key in msg and _integer(msg[key], key) != state[key]:
                raise design.DesignError(f"validate {key} mismatch")
        expected_phase = msg.get("phase", "ready")
        if expected_phase != state["phase"]:
            raise design.DesignError("validate phase mismatch")
        return {"valid": True, "configurationIdentity": self.identity,
                "feStep": state["feStep"], "controlTick": state["controlTick"], "phase": state["phase"]}


def _reject_constant(value):
    raise ValueError(f"nonfinite JSON constant {value}")


def _unique_object(items):
    value = {}
    for key, item in items:
        if key in value:
            raise ValueError(f"duplicate JSON key {key}")
        value[key] = item
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdio", action="store_true", required=True)
    parser.parse_args(argv)
    backend = NativeBackend()
    try:
        for line in sys.stdin:
            try:
                message = json.loads(line, parse_constant=_reject_constant, object_pairs_hook=_unique_object)
                response = backend.handle(message)
            except Exception as exc:
                response = {"ok": False, "error": str(exc) or type(exc).__name__}
            sys.stdout.write(json.dumps(response, ensure_ascii=True, allow_nan=False, separators=(",", ":"))+"\n")
            sys.stdout.flush()
    finally:
        backend.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
