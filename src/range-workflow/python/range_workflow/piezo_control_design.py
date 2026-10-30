"""PC-07 numerical designs for supplied discrete FE/ROM increment operators.

All runtime calls are pure proposals. The native accepted transaction alone may
persist input history, MPC warm starts and observer estimates. Bias/q0 removal
belongs to the caller. No FE state is read by the sensor-only observer.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import marshal
from pathlib import Path
import re
import sys

import numpy as np
import scipy
from scipy import linalg

from .piezo_design import (DEPENDENCY_LOCK_SHA256, DesignError, SourceIdentity,
                           StateSpace, _array, _digest, _freeze, _positive,
                           _psd, _symmetric, provenance as _design_provenance)


_MODULE_SOURCE_PATH = Path(__file__).resolve()
_LOADED_MODULE_SHA256 = hashlib.sha256(_MODULE_SOURCE_PATH.read_bytes()).hexdigest()
_LOADED_EXECUTION_SHA256 = hashlib.sha256(marshal.dumps(sys._getframe().f_code)).hexdigest()


def _optimization_backend():
    backend = globals().get("optimize")
    if backend is None:
        from scipy import optimize as backend
        globals()["optimize"] = backend
    return backend


def __getattr__(name):
    # Preserve direct access to the actual SciPy module for callers and patches.
    if name == "optimize":
        return _optimization_backend()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def __dir__():
    return sorted(set(globals()) | {"optimize"})


def provenance():
    try:
        current = hashlib.sha256(_MODULE_SOURCE_PATH.read_bytes()).hexdigest()
    except OSError as exc:
        raise DesignError('control backend source unavailable; reload required') from exc
    if current != _LOADED_MODULE_SHA256:
        raise DesignError('control backend source changed after import; reload required')
    plant_provenance = dict(_design_provenance())
    return (("numpyVersion", np.__version__), ("scipyVersion", scipy.__version__),
            ("moduleCodeSha256", _LOADED_MODULE_SHA256),
            ("moduleExecutionCodeSha256", _LOADED_EXECUTION_SHA256),
            ("moduleIdentityScope", "this-module-source-and-executing-code-only"),
            ("dependencyLockSha256", DEPENDENCY_LOCK_SHA256),
            ('plantDesignModuleCodeSha256', plant_provenance['moduleCodeSha256']),
            ('plantDesignModuleExecutionCodeSha256', plant_provenance['moduleExecutionCodeSha256']))


def _system(system):
    if not isinstance(system, StateSpace) or system.sampling_period is None:
        raise DesignError("control design requires discrete StateSpace")
    _positive(system.sampling_period, 'sampling_period')
    a = _symmetric_shape(system.A, 'A')
    n = len(a)
    b, c = _array(system.B, 'B'), _array(system.Cy, 'Cy')
    if b.ndim != 2 or b.shape[0] != n or b.shape[1] == 0:
        raise DesignError("B must contain at least one mapped input")
    if c.ndim != 2 or c.shape[1] != n:
        raise DesignError("Cy dimensions do not match A")
    d = _array(system.Dy, 'Dy', (len(c), b.shape[1]))
    if (not isinstance(system.source, SourceIdentity) or not system.source.dofs or
            len(system.source.inputs) != b.shape[1] or len(system.source.outputs) != len(c) or
            not isinstance(system.matrix_sha256, str) or
            not re.fullmatch('[0-9a-f]{64}', system.matrix_sha256)):
        raise DesignError("state-space source mappings or matrix identity invalid")
    return a, b, c, d


def _symmetric_shape(value, name):
    a = _array(value, name)
    if a.ndim != 2 or not len(a) or a.shape[0] != a.shape[1]:
        raise DesignError(f"{name} must be nonempty square")
    return a


def _weight(value, name, n, positive=False):
    a = _symmetric(value, name, n)
    _psd(a, name)
    if positive:
        try:
            linalg.cholesky(a)
        except linalg.LinAlgError as exc:
            raise DesignError(f"{name} must be positive definite") from exc
    return a


def _pbh(a, b, name):
    # Includes unit-circle modes. Rank uses matrix-local singular-value scales.
    for pole in linalg.eigvals(a):
        if abs(pole) >= 1-32*np.finfo(float).eps:
            test = np.hstack((pole*np.eye(len(a))-a, b))
            if np.linalg.matrix_rank(test) < len(a):
                raise DesignError(f"{name} PBH qualification failed")


def _sqrt_psd(q):
    roots, vectors = linalg.eigh(q)
    return (vectors*np.sqrt(np.maximum(roots, 0)))@vectors.T


def _relative(residual, *terms):
    return float(np.linalg.norm(residual, 'fro') /
                 max(sum(np.linalg.norm(t, 'fro') for t in terms), np.finfo(float).tiny))


def _stable(a):
    poles = linalg.eigvals(a)
    if not np.all(np.isfinite(poles)) or np.any(np.abs(poles) >= 1):
        raise DesignError("no strictly stable closed-loop solution")
    return tuple(complex(p) for p in poles)


@dataclass(frozen=True)
class Identity:
    source: SourceIdentity
    matrix_sha256: str
    operators_sha256: str
    sampling_period: float
    provenance: tuple[tuple[str, str], ...]


def _identity(system, a, b, c, d):
    return Identity(system.source, system.matrix_sha256, _digest(a, b, c, d),
                    system.sampling_period, provenance())


def _match(identity, system):
    if identity.provenance != provenance():
        raise DesignError('control design backend provenance mismatch')
    a, b, c, d = _system(system)
    if (identity.source != system.source or identity.matrix_sha256 != system.matrix_sha256 or
            identity.operators_sha256 != _digest(a, b, c, d) or
            identity.sampling_period != system.sampling_period):
        raise DesignError("control design source or operator identity mismatch")
    return a, b, c, d


@dataclass(frozen=True)
class LQRDesign:
    K: np.ndarray
    P: np.ndarray
    riccati_relative_residual: float
    closed_poles: tuple[complex, ...]
    identity: Identity


def discrete_lqr(system, Q, R, *, verification_tol=1e-8):
    a, b, c, d = _system(system)
    q, r = _weight(Q, 'Q', len(a)), _weight(R, 'R', b.shape[1], True)
    tol = _positive(verification_tol, 'verification_tol')
    _pbh(a, b, 'stabilizability')
    _pbh(a.T, _sqrt_psd(q).T, 'cost detectability')
    try:
        p = _weight(linalg.solve_discrete_are(a, b, q, r), 'Riccati P', len(a))
        gain = linalg.solve(r+b.T@p@b, b.T@p@a, assume_a='pos')
    except linalg.LinAlgError as exc:
        raise DesignError("no stabilizing Riccati solution") from exc
    term = a.T@p@b@gain
    residual = _relative(a.T@p@a-p-term+q, a.T@p@a, p, term, q)
    poles = _stable(a-b@gain)
    if not np.isfinite(residual) or residual > tol:
        raise DesignError("Riccati relative residual qualification failed")
    return LQRDesign(_freeze(gain), _freeze(p), residual, poles, _identity(system, a, b, c, d))


def lqr_action(design, system, state):
    a, b, _, _ = _match(design.identity, system)
    k = _array(design.K, 'K', (b.shape[1], len(a)))
    return _freeze(-k@_array(state, 'increment state', (len(a),)))


@dataclass(frozen=True)
class MPCDesign:
    Q: np.ndarray
    R: np.ndarray
    Qf: np.ndarray
    horizon: int
    lower: np.ndarray
    upper: np.ndarray
    delta: np.ndarray
    identity: Identity


def constrained_mpc(system, Q, R, Qf, horizon, *, lower, upper, delta):
    """Freeze a finite-horizon cost sum(x[k]'Q x[k]+u[k]'R u[k])+x[N]'Qf x[N]."""
    a, b, c, d = _system(system)
    if isinstance(horizon, (bool, np.bool_)) or not isinstance(horizon, (int, np.integer)) or horizon <= 0:
        raise DesignError("horizon must be a positive integer")
    n, m = len(a), b.shape[1]
    q, r, qf = _weight(Q, 'Q', n), _weight(R, 'R', m, True), _weight(Qf, 'Qf', n)
    lo, hi = _array(lower, 'lower', (m,)), _array(upper, 'upper', (m,))
    if np.any(lo >= hi):
        raise DesignError("finite lower must be strictly below upper")
    if np.iscomplexobj(delta):
        raise DesignError("delta must be real")
    try:
        rate = np.array(delta, dtype=float, copy=True)
    except (TypeError, ValueError, OverflowError) as exc:
        raise DesignError("delta must be numeric") from exc
    if rate.shape != (m,) or np.any(np.isnan(rate)) or np.any(rate <= 0):
        raise DesignError("delta must be positive per-sample limits (infinity allowed)")
    # Rate can be infinite, unlike normal persisted numeric arrays.
    rate = np.frombuffer(rate.tobytes(), dtype=float)
    return MPCDesign(*map(_freeze, (q, r, qf)), int(horizon), _freeze(lo), _freeze(hi),
                     rate, _identity(system, a, b, c, d))


@dataclass(frozen=True)
class MPCCertificate:
    primal: float
    stationarity: float
    dual: float
    complementarity: float
    multipliers: np.ndarray
    action_stationarity: float


@dataclass(frozen=True)
class MPCProposal:
    first_action: np.ndarray
    trajectory: np.ndarray
    predicted_states: np.ndarray
    next_warm_start: np.ndarray
    objective: float
    certificate: MPCCertificate
    identity: Identity


def mpc_proposal(design, system, state, previous_applied, *, warm_start=None,
                 state_offsets=None, state_transitions=None, input_transitions=None,
                 verification_tol=1e-6, max_iterations=500):
    """Solve the real convex QP; any failure raises DesignError without an action.

    Rate limits are increments per sample, not per second. previous_applied must
    be the last accepted plant command. Warm starts are optional persisted N*m
    proposals and are sequentially clamped into the current feasible set.
    state_offsets[k] is a supplied known affine increment in
    x[k+1] = A*x[k] + B*u[k] + state_offsets[k]. It is never persisted here.
    Paired state_transitions/input_transitions optionally supply known per-step
    transitions (e.g. the native canonical clock). The frozen cost/input limits
    stay the design's; feasibility and KKT certify this supplied prediction.
    """
    a, b, _, _ = _match(design.identity, system)
    n, m, N = len(a), b.shape[1], design.horizon
    x = _array(state, 'increment state', (n,))
    for supplied_name, supplied in (("state_offsets", state_offsets),
            ("state_transitions", state_transitions), ("input_transitions", input_transitions)):
        if supplied is None:
            continue
        try:
            tagged_offsets = np.asarray(supplied, dtype=object)
        except (TypeError, ValueError) as exc:
            raise DesignError(f'{supplied_name} must be a real numeric array') from exc
        if any(isinstance(v, (bool, np.bool_, str, bytes)) for v in tagged_offsets.flat):
            raise DesignError(f'{supplied_name} must contain real numbers, not booleans or strings')
    offsets = (np.zeros((N, n)) if state_offsets is None else
               _array(state_offsets, 'state_offsets', (N, n)))
    if (state_transitions is None) != (input_transitions is None):
        raise DesignError('state_transitions and input_transitions must be supplied together')
    transitions_a = (np.tile(a, (N, 1, 1)) if state_transitions is None else
                     _array(state_transitions, 'state_transitions', (N, n, n)))
    transitions_b = (np.tile(b, (N, 1, 1)) if input_transitions is None else
                     _array(input_transitions, 'input_transitions', (N, n, m)))
    previous = _array(previous_applied, 'previous_applied', (m,))
    if np.any(previous < design.lower) or np.any(previous > design.upper):
        raise DesignError("previous_applied is outside amplitude limits")
    tol = _positive(verification_tol, 'verification_tol')
    if (isinstance(max_iterations, (bool, np.bool_)) or
            not isinstance(max_iterations, (int, np.integer)) or max_iterations <= 0):
        raise DesignError("max_iterations must be a positive integer")
    start = (np.tile(previous, (N, 1)) if warm_start is None else
             _array(warm_start, 'warm_start', (N, m)))
    last = previous.copy()
    for i in range(N):
        start[i] = np.clip(start[i], np.maximum(design.lower, last-design.delta),
                           np.minimum(design.upper, last+design.delta))
        last = start[i].copy()
    # Condense states x[0]..x[N] = F*x[0]+T*vec(u).
    F, T = np.zeros(((N+1)*n, n)), np.zeros(((N+1)*n, N*m))
    F[:n] = np.eye(n)
    affine = np.zeros((N+1, n))
    for i in range(N):
        ai, bi = transitions_a[i], transitions_b[i]
        F[(i+1)*n:(i+2)*n] = ai@F[i*n:(i+1)*n]
        T[(i+1)*n:(i+2)*n] = ai@T[i*n:(i+1)*n]
        T[(i+1)*n:(i+2)*n, i*m:(i+1)*m] += bi
        affine[i+1] = ai@affine[i]+offsets[i]
    W = linalg.block_diag(*([design.Q]*N+[design.Qf]))
    U = linalg.block_diag(*([design.R]*N))
    base = _array(F@x+affine.ravel(), 'affine free prediction')
    hessian, linear = 2*(T.T@W@T+U), 2*T.T@W@base
    hessian = _array(hessian, 'condensed Hessian')
    linear = _array(linear, 'condensed linear term')
    constant = float(base@W@base)
    if not np.isfinite(constant):
        raise DesignError('MPC condensed objective overflow')
    eye = np.eye(N*m)
    rows, bounds = [eye, -eye], [np.tile(design.upper, N), -np.tile(design.lower, N)]
    diff = eye.copy()
    for i in range(1, N):
        diff[i*m:(i+1)*m, (i-1)*m:i*m] = -np.eye(m)
    offset = np.concatenate((previous, np.zeros((N-1)*m)))
    rates = np.tile(design.delta, N)
    finite = np.isfinite(rates)
    rows.extend((diff[finite], -diff[finite]))
    bounds.extend(((rates+offset)[finite], (rates-offset)[finite]))
    g, h = np.vstack(rows), np.concatenate(bounds)
    # u=D*z balances individual curvature directions, rather than dividing
    # everything by the largest physical weight. Constant cost is excluded from
    # optimization to avoid loss of objective differences through cancellation.
    diagonal = np.diag(hessian)
    if np.any(diagonal <= 0):
        raise DesignError('MPC Hessian is not positive definite')
    variable_scale = 1/np.sqrt(diagonal)
    balanced_hessian = _array(variable_scale[:, None]*hessian*variable_scale, 'balanced Hessian')
    balanced_linear = _array(variable_scale*linear, 'balanced linear term')
    balanced_g = g*variable_scale
    row_scale = np.linalg.norm(balanced_g, axis=1)
    gs, hs = balanced_g/row_scale[:, None], h/row_scale
    def jac(z):
        return balanced_hessian@z+balanced_linear
    try:
        solver_start = start.ravel()/variable_scale
        # A tiny objective can satisfy SLSQP's absolute stopping criterion at
        # the warm start while missing the physical action certificate. When
        # the unconstrained minimizer is feasible, start at that exact convex
        # optimum. The solver and all physical KKT checks still qualify it.
        unconstrained = linalg.solve(balanced_hessian, -balanced_linear, assume_a='pos')
        unconstrained_action = variable_scale*unconstrained
        unconstrained_slack = h-g@unconstrained_action
        if (np.all(np.isfinite(unconstrained_action)) and
                np.all(np.isfinite(unconstrained_slack)) and np.all(unconstrained_slack >= 0)):
            solver_start = unconstrained
        # z=s*y and f(s*y)/s**2 resolve tiny constrained actions without
        # changing curvature conditioning or any physical constraint/tolerance.
        solver_scale = 1.
        magnitude = float(max(np.max(np.abs(unconstrained)), np.max(np.abs(solver_start))))
        if 0 < magnitude < 1:
            with np.errstate(over='ignore', invalid='ignore', divide='ignore'):
                scaled_linear = balanced_linear/magnitude
                scaled_bounds = hs/magnitude
                scaled_start = solver_start/magnitude
            if all(np.all(np.isfinite(value)) for value in (scaled_linear, scaled_bounds, scaled_start)):
                solver_scale = magnitude
        solver_linear = balanced_linear/solver_scale
        solver_bounds = hs/solver_scale
        solver_start = solver_start/solver_scale
        def solver_fun(y):
            return float(.5*y@balanced_hessian@y+solver_linear@y)
        def solver_jac(y):
            return balanced_hessian@y+solver_linear
        optimize = _optimization_backend()
        result = optimize.minimize(solver_fun, solver_start, jac=solver_jac, method='SLSQP',
            constraints={'type': 'ineq', 'fun': lambda y: solver_bounds-gs@y, 'jac': lambda y: -gs},
            options={'ftol': 1e-12, 'maxiter': int(max_iterations)})
    except (ValueError, RuntimeError, linalg.LinAlgError) as exc:
        raise DesignError("MPC QP solver failed") from exc
    if not result.success:
        raise DesignError(f"MPC QP solver failed: {result.message}")
    z = _array(solver_scale*_array(result.x, 'scaled QP solution', (N*m,)),
               'balanced QP solution', (N*m,))
    u = _array(variable_scale*z, 'QP solution', (N*m,))
    slack = _array(h-g@u, 'physical QP constraint slack')
    gradient = _array(hessian@u+linear, 'physical QP objective gradient')
    constraint_scale = np.maximum(1., np.maximum(np.abs(h), np.abs(g)@np.abs(u)))
    active = slack/constraint_scale <= tol
    multipliers = np.zeros(len(h))
    try:
        if np.any(active):
            balanced_multipliers, _ = optimize.nnls(gs[active].T, -jac(z),
                                                    maxiter=max(100, 10*int(active.sum())))
            multipliers[active] = balanced_multipliers/row_scale[active]
        physical_stationarity = _array(gradient+g.T@multipliers, 'physical KKT stationarity')
        # Convert residual to the physical unconstrained Newton displacement.
        # This certificate catches a low-weight action missed despite a tiny
        # gradient relative to another component's very large weight.
        action_error = variable_scale*linalg.solve(balanced_hessian,
            variable_scale*physical_stationarity, assume_a='pos')
    except (ValueError, RuntimeError, linalg.LinAlgError) as exc:
        raise DesignError("MPC KKT multiplier qualification failed") from exc
    term_scale = np.maximum(1., np.abs(hessian)@np.abs(u)+np.abs(linear)+np.abs(g).T@multipliers)
    primal = float(max(0., np.max(-slack/constraint_scale)))
    stationarity = float(np.max(np.abs(physical_stationarity)/term_scale))
    action_stationarity = float(np.max(np.abs(action_error)/
        np.maximum(1., np.tile(design.upper-design.lower, N))))
    dual = float(np.max(np.maximum(-multipliers, 0)/np.maximum(1., np.abs(multipliers))))
    complementarity = float(np.max(np.abs(multipliers*slack)/
        np.maximum(1., np.abs(multipliers)*constraint_scale)))
    residuals = [primal, stationarity, dual, complementarity, action_stationarity]
    if not np.all(np.isfinite(residuals)) or max(residuals) > tol:
        raise DesignError("MPC feasibility or KKT qualification failed")
    trajectory = u.reshape(N, m)
    states = (base+T@u).reshape(N+1, n)
    # Verify uncondensed dynamics independently of the cost/constraint solver.
    next_states = (np.einsum('kij,kj->ki', transitions_a, states[:-1])+
                   np.einsum('kij,kj->ki', transitions_b, trajectory)+offsets)
    if not np.allclose(states[1:], next_states, rtol=tol, atol=tol):
        raise DesignError("MPC predicted-state qualification failed")
    next_start = np.vstack((trajectory[1:], trajectory[-1:]))
    objective = float(states.ravel()@W@states.ravel()+u@U@u)
    if not np.isfinite(objective):
        raise DesignError('MPC objective qualification failed')
    certificate = MPCCertificate(primal, stationarity, dual, complementarity,
                                 _freeze(multipliers), action_stationarity)
    return MPCProposal(_freeze(trajectory[0]), _freeze(trajectory), _freeze(states),
                       _freeze(next_start), objective, certificate, design.identity)


@dataclass(frozen=True)
class ObserverDesign:
    Pprior: np.ndarray
    correction_gain: np.ndarray
    selected_outputs: tuple[int, ...]
    riccati_relative_residual: float
    closed_poles: tuple[complex, ...]
    identity: Identity


def steady_state_kalman(system, process_covariance, measurement_covariance, *,
                        selected_outputs, verification_tol=1e-8):
    a, b, c, d = _system(system)
    picked = tuple(selected_outputs)
    if (not picked or any(isinstance(i, (bool, np.bool_)) or
            not isinstance(i, (int, np.integer)) or i < 0 or i >= len(c) for i in picked) or
            len(set(picked)) != len(picked)):
        raise DesignError("selected_outputs requires unique valid integer indices")
    cs = c[list(picked)]
    w = _weight(process_covariance, 'process_covariance', len(a))
    v = _weight(measurement_covariance, 'measurement_covariance', len(picked), True)
    tol = _positive(verification_tol, 'verification_tol')
    _pbh(a.T, cs.T, 'sensor detectability')
    _pbh(a, _sqrt_psd(w), 'process-noise stabilizability')
    try:
        p = _weight(linalg.solve_discrete_are(a.T, cs.T, w, v), 'Pprior', len(a))
        gain = linalg.solve(v+cs@p@cs.T, cs@p, assume_a='pos').T
    except linalg.LinAlgError as exc:
        raise DesignError("no stabilizing observer Riccati solution") from exc
    term = a@gain@cs@p@a.T
    residual = _relative(a@p@a.T-p-term+w, a@p@a.T, p, term, w)
    poles = _stable(a@(np.eye(len(a))-gain@cs))
    if not np.isfinite(residual) or residual > tol:
        raise DesignError("observer Riccati relative residual qualification failed")
    return ObserverDesign(_freeze(p), _freeze(gain), picked, residual, poles,
                          _identity(system, a, b, c, d))


@dataclass(frozen=True)
class ObserverProposal:
    corrected_state: np.ndarray
    innovation: np.ndarray
    identity: Identity


def observer_correction(design, system, prior_state, selected_measurement, previous_applied):
    """Correct only the caller's persisted prior from selected increment signals.

    Samples obey y[k]=C*x[k]+D*u[k-1]. Subtract the last accepted command's
    direct term explicitly; the caller supplies already bias-removed readings.
    """
    a, b, c, d = _match(design.identity, system)
    picked = list(design.selected_outputs)
    prior = _array(prior_state, 'prior_state', (len(a),))
    y = _array(selected_measurement, 'selected_measurement', (len(picked),))
    previous = _array(previous_applied, 'previous_applied', (b.shape[1],))
    innovation = y-d[picked]@previous-c[picked]@prior
    corrected = prior+design.correction_gain@innovation
    return ObserverProposal(_freeze(corrected), _freeze(innovation), design.identity)


def observer_prediction(design, system, corrected_state, applied_input):
    """Pure next prior; call/persist only with the transaction's accepted input."""
    a, b, _, _ = _match(design.identity, system)
    return _freeze(a@_array(corrected_state, 'corrected_state', (len(a),))+
                   b@_array(applied_input, 'applied_input', (b.shape[1],)))
