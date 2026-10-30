"""PC-04/05 design algebra for matrices supplied by the compiled native FE plant.

No equilibrium is inferred or certified here. K is the supplied effective tangent
at the caller's accepted operating point. All coordinates and outputs are increments;
absolute electrical/operating-point offsets remain the caller's responsibility.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import marshal
from pathlib import Path
import re
import sys

import numpy as np
import scipy
from scipy import linalg


DEPENDENCY_LOCK_SHA256 = "2e71d9c57a8bb986f65fe25ef328d3011a3c6be9ef791629fe2daf424dca04e8"
_MODULE_SOURCE_PATH = Path(__file__).resolve()
_LOADED_SOURCE_SHA256 = hashlib.sha256(_MODULE_SOURCE_PATH.read_bytes()).hexdigest()
# Bind the actual executing module code as well as its import-time source bytes.
# This digest is interpreter-specific and covers this module, not NumPy/SciPy.
_LOADED_EXECUTION_SHA256 = hashlib.sha256(marshal.dumps(sys._getframe().f_code)).hexdigest()


class DesignError(ValueError):
    """Invalid physical matrices, source identity, or numerical qualification."""


def _check_loaded_module():
    try:
        current = hashlib.sha256(_MODULE_SOURCE_PATH.read_bytes()).hexdigest()
    except OSError as exc:
        raise DesignError("loaded module source is unavailable; reload required") from exc
    if current != _LOADED_SOURCE_SHA256:
        raise DesignError("module source changed since import; reload required")


def _array(value, name, shape=None):
    if np.iscomplexobj(value):
        raise DesignError(f"{name} must be real")
    try:
        a = np.array(value, dtype=float, copy=True)
    except (TypeError, ValueError, OverflowError) as exc:
        raise DesignError(f"{name} must be a real numeric array") from exc
    if not np.all(np.isfinite(a)) or (shape is not None and a.shape != shape):
        raise DesignError(f"{name} has invalid shape or nonfinite values")
    return a


def _freeze(a):
    a = _array(a, "result")
    # Immutable bytes backing also prevents re-enabling the write flag.
    return np.frombuffer(a.tobytes(), dtype=a.dtype).reshape(a.shape)


def _positive(value, name, *, zero=False):
    if isinstance(value, (bool, np.bool_)):
        raise DesignError(f"{name} must be numeric")
    try:
        v = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise DesignError(f"{name} must be numeric") from exc
    if not np.isfinite(v) or (v < 0 if zero else v <= 0):
        raise DesignError(f"{name} must be finite and {'nonnegative' if zero else 'positive'}")
    return v


def _symmetric(value, name, n=None):
    a = _array(value, name)
    if a.ndim != 2 or not a.shape[0] or a.shape[0] != a.shape[1] or (n is not None and a.shape != (n, n)):
        raise DesignError(f"{name} must be a nonempty square matrix")
    # Each pair has its own physical scale. An unrelated large diagonal must
    # not hide opposite low-scale couplings in an imported operator.
    diagonal_scale = np.sqrt(np.abs(np.diag(a)))
    scale = np.maximum(np.maximum(np.abs(a), np.abs(a.T)),
                       diagonal_scale[:, None]*diagonal_scale[None, :])
    active = scale > 0
    normalized = np.zeros_like(a)
    normalized[active] = a[active]/scale[active]-a.T[active]/scale[active]
    asymmetry = np.abs(normalized)
    if np.any(asymmetry > 1e-10):
        row, column = np.unravel_index(int(np.argmax(asymmetry)), asymmetry.shape)
        raise DesignError(f"{name} must be symmetric (relative defect {asymmetry[row, column]:.6g} at [{row},{column}], values {a[row, column]:.17g}/{a[column, row]:.17g}, scale {scale[row, column]:.17g})")
    # Preserve exact entries (including subnormals) and avoid overflowing the
    # sum of two finite equal large entries before dividing by two.
    return np.where(a == a.T, a, 0.5*a+0.5*a.T)


def _psd(a, name):
    """Qualify PSD after diagonal congruence, without changing physical entries.

    A PSD matrix cannot have a negative diagonal or a nonzero row attached to
    an exactly zero diagonal. Positive diagonal coordinates are equilibrated
    to one, preventing an unrelated large stiffness/damping from masking a
    negative direction. The remaining eigensolver allowance is only floating
    roundoff: 16 n eps ||equilibrated||inf, not a physical relative tolerance.
    """
    diagonal = np.diag(a)
    if np.any(diagonal < 0):
        raise DesignError(f"{name} must be positive semidefinite: negative diagonal")
    zero = diagonal == 0
    if np.any(a[zero, :] != 0):
        raise DesignError(f"{name} must be positive semidefinite: zero diagonal with nonzero coupling")
    active = ~zero
    if not np.any(active):
        return
    scaling = np.sqrt(diagonal[active])
    with np.errstate(over='ignore', invalid='ignore', divide='ignore'):
        equilibrated = (a[np.ix_(active, active)]/scaling[:, None])/scaling[None, :]
    if not np.all(np.isfinite(equilibrated)):
        raise DesignError(f"{name} has invalid equilibrated PSD entries")
    equilibrated = (equilibrated+equilibrated.T)*0.5
    roots = linalg.eigvalsh(equilibrated)
    bound = 16*len(equilibrated)*np.finfo(float).eps*np.linalg.norm(equilibrated, ord=np.inf)
    if roots[0] < -bound:
        raise DesignError(f"{name} must be positive semidefinite")


def _mechanical(M, K):
    _check_loaded_module()
    m = _symmetric(M, "M")
    k = _symmetric(K, "K", len(m))
    try:
        linalg.cholesky(m, lower=True)
    except linalg.LinAlgError as exc:
        raise DesignError("M must be positive definite; constrained massless DOFs must be eliminated") from exc
    _psd(k, "K")
    return m, k


def _symmetric_congruence(operator, basis, name):
    """Compute a symmetric Galerkin congruence with a componentwise error check.

    Each matrix product uses length-n dot products.  The standard floating-point
    dot-product bound gamma_(2n) covers rounded products and additions; composing
    the two products is bounded by gamma_(4n).  The nonnegative scale matrix
    abs(Phi).T @ abs(A) @ abs(Phi) bounds the exact absolute term sum, with its
    own two-product underestimation corrected by (1-gamma_(2n))**-2.  Additive
    subnormal allowances make the same bound conservative when tiny operations
    underflow.  An output is mirrored only after every antisymmetric component
    passes this bound.
    """
    p = _array(basis, 'Phi')
    if p.ndim != 2 or not p.shape[0] or not p.shape[1]:
        raise DesignError("Phi must be a nonempty matrix for congruence")
    a = _symmetric(operator, name, p.shape[0])
    n = len(a)
    eps = np.finfo(float).eps
    eta = np.nextafter(0.0, 1.0)

    def _gamma(operations):
        x = operations*eps
        if x >= 1:
            raise DesignError("congruence dimensions exceed floating-point error bound")
        return np.nextafter(x/(1-x), np.inf)

    # A dot product has n rounded multiplies and at most n rounded additions.
    gamma_stage = _gamma(2*n)
    gamma_total = _gamma(4*n)
    underflow = 2*n*eta
    p_abs, a_abs = np.abs(p), np.abs(a)
    with np.errstate(over='ignore', invalid='ignore', under='ignore'):
        left = np.matmul(p.T, a)
        projected = np.matmul(left, p)
        scale_left = np.matmul(p_abs.T, a_abs)
        scale = np.matmul(scale_left, p_abs)
        column_sum = np.sum(p_abs, axis=0)
    if not all(np.all(np.isfinite(value)) for value in
               (left, projected, scale_left, scale, column_sum)):
        raise DesignError(f"{name} congruence has nonfinite intermediate values")

    # Bound column sums used by the additive underflow term, including rounding
    # in their computation.  All quantities here are nonnegative.
    with np.errstate(over='ignore', invalid='ignore', under='ignore', divide='ignore'):
        column_sum_upper = np.nextafter(
            (column_sum+n*eta)/(1-gamma_stage), np.inf)
        scale_upper = np.nextafter(
            (scale+(1-gamma_stage)*underflow*column_sum_upper[None, :]+underflow)
            /((1-gamma_stage)**2), np.inf)
        one_sided_error = (
            gamma_total*scale_upper
            +(1+gamma_stage)*underflow*column_sum_upper[None, :]
            +underflow)
        one_sided_error = np.nextafter(one_sided_error*(1+8*eps)+8*eta, np.inf)
        residual = np.abs(projected-projected.T)
        residual_bound = np.nextafter(
            (one_sided_error+one_sided_error.T)*(1+eps)+eta, np.inf)
    if not all(np.all(np.isfinite(value)) for value in
               (column_sum_upper, scale_upper, one_sided_error, residual, residual_bound)):
        raise DesignError(f"{name} congruence error bound is nonfinite")
    if np.any(residual > residual_bound):
        raise DesignError(f"{name} congruence antisymmetry exceeds its floating-point bound")

    symmetric = np.array(projected, copy=True)
    lower = np.tril_indices_from(symmetric, k=-1)
    symmetric[lower] = symmetric.T[lower]
    return symmetric


def _digest(*arrays):
    h = hashlib.sha256()
    for a in arrays:
        a = np.asarray(a, dtype='<f8')
        h.update(json.dumps(a.shape).encode('ascii'))
        h.update(a.tobytes(order='C'))
    return h.hexdigest()


@dataclass(frozen=True)
class SourceIdentity:
    """Caller-supplied native source digest and ordered physical names/units.

    Each mapping entry is (physical identifier, unit). The digest must identify the
    compiled source, constraints, electrical conditions and operating point. This
    class validates format; it cannot certify that the caller's digest is truthful.
    """
    sha256: str
    dofs: tuple[tuple[str, str], ...]
    inputs: tuple[tuple[str, str], ...] = ()
    outputs: tuple[tuple[str, str], ...] = ()

    def __post_init__(self):
        if not isinstance(self.sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", self.sha256):
            raise DesignError("source sha256 must contain 64 lowercase hexadecimal characters")
        for name in ('dofs', 'inputs', 'outputs'):
            try:
                entries = tuple(tuple(e) for e in getattr(self, name))
            except TypeError as exc:
                raise DesignError(f"{name} requires named entries and units") from exc
            if any(len(e) != 2 or any(not isinstance(s, str) or not s.strip() for s in e) for e in entries):
                raise DesignError(f"{name} requires named entries and units")
            if len({e[0] for e in entries}) != len(entries):
                raise DesignError(f"{name} identifiers must be unique")
            object.__setattr__(self, name, entries)


def _source(source, n, nu=None, ny=None):
    if not isinstance(source, SourceIdentity) or len(source.dofs) != n:
        raise DesignError("source DOF mapping does not match matrices")
    if nu is not None and len(source.inputs) != nu:
        raise DesignError("source input mapping does not match B")
    if ny is not None and len(source.outputs) != ny:
        raise DesignError("source output mapping does not match output operators")


def provenance():
    """Import-time module identity, actual libraries, and frozen dependency lock.

    Refuse to label old loaded code with changed disk-source bytes. The source
    and execution-code hashes cover this module only; the lock and library
    versions separately describe dependencies, without hashing their binaries.
    """
    _check_loaded_module()
    return (("numpyVersion", np.__version__), ("scipyVersion", scipy.__version__),
            ("moduleCodeSha256", _LOADED_SOURCE_SHA256),
            ("moduleExecutionCodeSha256", _LOADED_EXECUTION_SHA256),
            ("moduleIdentityScope", "this-module-source-and-executing-code-only"),
            ("dependencyLockSha256", DEPENDENCY_LOCK_SHA256))


@dataclass(frozen=True)
class Modes:
    eigenvalues: np.ndarray
    omega: np.ndarray
    Phi: np.ndarray
    residuals: np.ndarray
    mass_orthogonality_error: float
    clusters: tuple[tuple[int, ...], ...]
    indices: tuple[int, ...]
    full_size: int
    matrix_sha256: str
    source: SourceIdentity
    provenance: tuple[tuple[str, str], ...]
    # (original modal index, raw negative root, zero-stiffness residual, roundoff bound)
    numerical_zero_corrections: tuple[tuple[int, float, float, float], ...] = ()


def compute_modes(M, K, *, source, cluster_rtol=1e-7, cluster_atol=0.0,
                  verification_tol=1e-8):
    """Complete generalized symmetric eigensystem, including rigid zero modes.

    Clusters compare adjacent squared frequencies using pair-local scales and
    directional matrix-vector roundoff floors. An unrelated large eigenvalue
    does not merge physically separate low-frequency roots into one cluster. The
    largest component fixes each vector's sign; vectors inside a cluster are not
    unique, so selection and cluster damping operate on the entire subspace.
    """
    m, k = _mechanical(M, K)
    _source(source, len(m))
    rt = _positive(cluster_rtol, 'cluster_rtol', zero=True)
    at = _positive(cluster_atol, 'cluster_atol', zero=True)
    tol = _positive(verification_tol, 'verification_tol')
    roots, phi = linalg.eigh(k, m)
    # PSD qualification above is coordinate equilibrated. A negative numerical
    # root is treated as rigid zero only if its *directional* zero-stiffness
    # residual is explainable by matrix-vector cancellation roundoff. An
    # unrelated high-frequency mode supplies no allowance for this decision.
    zero_corrections = []
    for j in np.flatnonzero(roots < 0):
        directional_bound = 16*len(m)*np.finfo(float).eps*linalg.norm(np.abs(k)@np.abs(phi[:, j]))
        zero_residual = float(linalg.norm(k@phi[:, j]))
        if zero_residual > directional_bound:
            raise DesignError("negative modal root is not a qualified numerical rigid zero")
        zero_corrections.append((int(j), float(roots[j]), zero_residual, float(directional_bound)))
        roots[j] = 0.0
    for j in range(len(roots)):
        if phi[np.argmax(np.abs(phi[:, j])), j] < 0:
            phi[:, j] *= -1
    def column_norms(values):
        return np.array([linalg.norm(values[:, j]) for j in range(values.shape[1])])
    errors = column_norms(k@phi - (m@phi)*roots)
    denominators = ((np.linalg.norm(k, ord=2) + roots*np.linalg.norm(m, ord=2))
                    * column_norms(phi))
    residuals = errors/np.maximum(denominators, np.finfo(float).tiny)
    orth = float(np.linalg.norm(phi.T@m@phi - np.eye(len(m)), ord=np.inf))
    if np.max(residuals) > tol or orth > tol:
        raise DesignError("modal residual or mass orthogonality qualification failed")
    groups = [[0]]
    numerical_floors = (16*len(m)*np.finfo(float).eps
                        * column_norms(np.abs(k)@np.abs(phi))
                        / np.maximum(column_norms(m@phi), np.finfo(float).tiny))
    if (not np.all(np.isfinite(roots)) or not np.all(np.isfinite(errors))
            or not np.all(np.isfinite(denominators)) or not np.all(np.isfinite(residuals))
            or not np.isfinite(orth) or not np.all(np.isfinite(numerical_floors))):
        raise DesignError("modal certificate has nonfinite intermediate values")
    for i in range(1, len(roots)):
        if roots[i]-roots[i-1] <= max(numerical_floors[i], numerical_floors[i-1],
                                    at+rt*max(abs(roots[i]), abs(roots[i-1]))):
            groups[-1].append(i)
        else:
            groups.append([i])
    return Modes(_freeze(roots), _freeze(np.sqrt(roots)), _freeze(phi),
                 _freeze(residuals), orth, tuple(map(tuple, groups)), tuple(range(len(m))),
                 len(m), _digest(m, k), source, provenance(), tuple(zero_corrections))


def select_modes(modes, indices):
    """Zero-based indices into this result; reject selection splitting any cluster."""
    _check_loaded_module()
    picked = tuple(indices)
    if (not picked or any(isinstance(i, (bool, np.bool_)) or not isinstance(i, (int, np.integer))
                          or i < 0 or i >= len(modes.indices) for i in picked)
            or len(set(picked)) != len(picked)):
        raise DesignError("modal selection must contain unique valid integer indices")
    picked = tuple(sorted(picked))
    chosen = {modes.indices[i] for i in picked}
    for cluster in modes.clusters:
        if chosen.intersection(cluster) and not set(cluster).issubset(chosen):
            raise DesignError("selection splits a near-root cluster")
    return Modes(_freeze(modes.eigenvalues[list(picked)]), _freeze(modes.omega[list(picked)]),
                 _freeze(modes.Phi[:, picked]), _freeze(modes.residuals[list(picked)]),
                 modes.mass_orthogonality_error,
                 tuple(c for c in modes.clusters if set(c).issubset(chosen)),
                 tuple(modes.indices[i] for i in picked), modes.full_size,
                 modes.matrix_sha256, modes.source, modes.provenance,
                 tuple(c for c in modes.numerical_zero_corrections if c[0] in chosen))


def _basis(modes, m, k, source):
    if (not isinstance(modes, Modes) or modes.source != source or
            modes.matrix_sha256 != _digest(m, k) or modes.full_size != len(m)):
        raise DesignError("modal source or matrix identity mismatch")
    phi = _array(modes.Phi, 'Phi')
    orth = linalg.norm(phi.T@m@phi-np.eye(phi.shape[1]), ord=np.inf)
    if phi.shape != (len(m), len(modes.indices)) or not np.isfinite(orth) or orth > 1e-8:
        raise DesignError("basis is not mass orthonormal")
    roots = _array(modes.eigenvalues, 'modal eigenvalues', (phi.shape[1],))
    omega = _array(modes.omega, 'modal omega', roots.shape)
    if np.any(roots < 0) or np.any(omega < 0) or not np.allclose(omega**2, roots, rtol=1e-12, atol=0):
        raise DesignError("inconsistent modal eigenvalues or frequencies")
    norms=lambda value:np.array([linalg.norm(value[:,j]) for j in range(value.shape[1])])
    denominator = ((np.linalg.norm(k, ord=2)+roots*np.linalg.norm(m, ord=2))*norms(phi))
    errors=norms(k@phi-(m@phi)*roots)
    residual = errors/np.maximum(denominator, np.finfo(float).tiny)
    if (not np.all(np.isfinite(errors)) or not np.all(np.isfinite(denominator))
            or not np.all(np.isfinite(residual)) or np.any(residual > 1e-8)):
        raise DesignError("modal basis eigen residual qualification failed")
    return phi


@dataclass(frozen=True)
class Damping:
    C: np.ndarray
    strategy: str
    source: SourceIdentity
    matrix_sha256: str
    provenance: tuple[tuple[str, str], ...]


def physical_damping(M, K, *, source, kind='none', modes=None, zeta=None,
                     alpha=None, beta=None, imported_C=None, high_mode_policy=None):
    """One authoritative full FE C. Modal kinds: uniform, per_mode, per_cluster.

    A complete basis constructs M Phi diag(2 zeta omega) Phi.T M. A partial
    basis requires high_mode_policy='zero_damping', explicitly assigning zero
    damping to its mass-orthogonal complement. No other completion is inferred.
    Cluster members must have equal damping ratios, including per_mode input.
    """
    m, k = _mechanical(M, K)
    _source(source, len(m))
    modal_kind = kind in ('uniform', 'per_mode', 'per_cluster')
    if ((not modal_kind and (modes is not None or zeta is not None or high_mode_policy is not None))
            or (kind != 'rayleigh' and (alpha is not None or beta is not None))
            or (kind != 'imported' and imported_C is not None)):
        raise DesignError("parameters from different damping models cannot be combined")
    if kind == 'none':
        c, strategy = np.zeros_like(m), 'none'
    elif kind == 'rayleigh':
        c = _positive(alpha, 'alpha', zero=True)*m + _positive(beta, 'beta', zero=True)*k
        strategy = 'rayleigh-full-FE'
    elif kind == 'imported':
        c, strategy = _symmetric(imported_C, 'C', len(m)), 'imported-full-FE'
    elif kind in ('uniform', 'per_mode', 'per_cluster'):
        phi = _basis(modes, m, k, source)
        partial = phi.shape[1] < len(m)
        if partial and high_mode_policy != 'zero_damping':
            raise DesignError("partial modal damping requires explicit high_mode_policy='zero_damping'")
        if not partial and high_mode_policy is not None:
            raise DesignError("complete basis does not require a high-mode policy")
        if kind == 'uniform':
            ratios = np.full(phi.shape[1], _positive(zeta, 'zeta', zero=True))
        else:
            values = _array(zeta, 'zeta', (phi.shape[1] if kind == 'per_mode' else len(modes.clusters),))
            if np.any(values < 0):
                raise DesignError("zeta must be nonnegative")
            if kind == 'per_mode':
                ratios = values
            else:
                lookup = {index: values[j] for j, cluster in enumerate(modes.clusters) for index in cluster}
                ratios = np.array([lookup[i] for i in modes.indices])
        for cluster in modes.clusters:
            positions = [modes.indices.index(i) for i in cluster]
            if not np.all(ratios[positions] == ratios[positions[0]]):
                raise DesignError("near-root cluster requires a single damping ratio")
        mp = m@phi
        c = (mp*(2*ratios*modes.omega))@mp.T
        strategy = 'partial-basis-zero-damping-complement' if partial else 'complete-mass-normalized-basis'
    else:
        raise DesignError("unknown physical damping kind")
    c = _symmetric(c, 'C', len(m))
    _psd(c, 'C')
    return Damping(_freeze(c), strategy, source, _digest(m, k), provenance())


@dataclass(frozen=True)
class StateSpace:
    A: np.ndarray
    B: np.ndarray
    Cy: np.ndarray
    Dy: np.ndarray
    source: SourceIdentity
    matrix_sha256: str
    provenance: tuple[tuple[str, str], ...]
    sampling_period: float | None = None


def _inputs_outputs(n, B, Lq, Lv, La, Dinput):
    b = _array(B, 'Bplant')
    if b.ndim != 2 or b.shape[0] != n:
        raise DesignError("Bplant must have one row per FE DOF")
    lq = _array(Lq, 'Lq')
    if lq.ndim != 2 or lq.shape[1] != n:
        raise DesignError("Lq must have one column per FE DOF")
    ny, nu = lq.shape[0], b.shape[1]
    lv = np.zeros_like(lq) if Lv is None else _array(Lv, 'Lv', lq.shape)
    la = np.zeros_like(lq) if La is None else _array(La, 'La', lq.shape)
    d = np.zeros((ny, nu)) if Dinput is None else _array(Dinput, 'Dinput', (ny, nu))
    return b, lq, lv, la, d


def continuous_state_space(M, K, C, Bplant, *, source, Lq, Lv=None, La=None, Dinput=None):
    """x=[deltaq, qdot], y=Lq deltaq+Lv qdot+La qddot+Dinput u.

    Bplant contains *all* force/voltage/disturbance columns with their supplied
    signs. Electrical feedthrough belongs in Dinput and is preserved exactly.
    """
    m, k = _mechanical(M, K)
    if isinstance(C, Damping):
        if C.source != source or C.matrix_sha256 != _digest(m, k):
            raise DesignError("damping source or matrix identity mismatch")
        C = C.C
    c = _symmetric(C, 'C', len(m))
    _psd(c, 'C')
    b, lq, lv, la, d = _inputs_outputs(len(m), Bplant, Lq, Lv, La, Dinput)
    _source(source, len(m), b.shape[1], lq.shape[0])
    mk, mc, mb = (linalg.solve(m, v, assume_a='pos') for v in (k, c, b))
    a = np.block([[np.zeros_like(m), np.eye(len(m))], [-mk, -mc]])
    bs = np.vstack((np.zeros_like(b), mb))
    cy, dy = np.hstack((lq-la@mk, lv-la@mc)), d+la@mb
    return StateSpace(_freeze(a), _freeze(bs), _freeze(cy), _freeze(dy), source,
                      _digest(m, k, c, b, lq, lv, la, d), provenance())


@dataclass(frozen=True)
class ReducedModel:
    M: np.ndarray
    K: np.ndarray
    C: np.ndarray
    Bplant: np.ndarray
    Phi: np.ndarray
    state_space: StateSpace
    source: SourceIdentity


def reduce_model(M, K, C, Bplant, modes, *, source, Lq, Lv=None, La=None, Dinput=None):
    """Galerkin projection of the same FE matrices and output operators."""
    m, k = _mechanical(M, K)
    phi = _basis(modes, m, k, source)
    # Validate all full operators and source before constructing any result.
    full = continuous_state_space(m, k, C, Bplant, source=source, Lq=Lq, Lv=Lv, La=La, Dinput=Dinput)
    c = C.C if isinstance(C, Damping) else _symmetric(C, 'C', len(m))
    b, lq, lv, la, d = _inputs_outputs(len(m), Bplant, Lq, Lv, La, Dinput)
    mr = _symmetric_congruence(m, phi, 'M')
    kr = _symmetric_congruence(k, phi, 'K')
    cr = _symmetric_congruence(c, phi, 'C')
    br = phi.T@b
    modal_source = SourceIdentity(source.sha256,
        tuple((f'mode:{i}', 'mass-normalized-coordinate') for i in modes.indices), source.inputs, source.outputs)
    ss = continuous_state_space(mr, kr, cr, br, source=modal_source,
                                Lq=lq@phi, Lv=lv@phi, La=la@phi, Dinput=d)
    # Retain actual FE identity; synthetic modal labels are internal dimensions only.
    ss = StateSpace(ss.A, ss.B, ss.Cy, ss.Dy, source,
                    _digest(full.A, full.B, full.Cy, full.Dy, phi), ss.provenance)
    return ReducedModel(*map(_freeze, (mr, kr, cr, br, phi)), ss, source)


def project_state(M, K, modes, q, velocity, q0, *, source):
    """Project an accepted FE state using the persisted physical equilibrium q0."""
    m, k = _mechanical(M, K)
    phi = _basis(modes, m, k, source)
    n = len(m)
    q, v, q0 = (_array(value, name, (n,)) for value, name in ((q, 'q'), (velocity, 'velocity'), (q0, 'q0')))
    return _freeze(np.concatenate((phi.T@m@(q-q0), phi.T@m@v)))


def exact_zoh(system, sampling_period):
    """Exact held-input discretization via augmented expm, including singular A."""
    _check_loaded_module()
    if not isinstance(system, StateSpace) or system.sampling_period is not None:
        raise DesignError("ZOH requires a continuous StateSpace")
    ts = _positive(sampling_period, 'sampling_period')
    a = _array(system.A, 'A')
    if a.ndim != 2 or a.shape[0] != a.shape[1] or not len(a):
        raise DesignError("A must be nonempty square")
    b = _array(system.B, 'B')
    if b.ndim != 2 or b.shape[0] != len(a):
        raise DesignError("B dimensions do not match A")
    cy = _array(system.Cy, 'Cy')
    if cy.ndim != 2 or cy.shape[1] != len(a):
        raise DesignError("Cy dimensions do not match A")
    dy = _array(system.Dy, 'Dy', (len(cy), b.shape[1]))
    if (not isinstance(system.source, SourceIdentity) or
            len(system.source.inputs) != b.shape[1] or len(system.source.outputs) != len(cy)):
        raise DesignError("state-space source port mappings do not match matrices")
    augmented = np.zeros((len(a)+b.shape[1], len(a)+b.shape[1]))
    augmented[:len(a), :len(a)], augmented[:len(a), len(a):] = a, b
    transition = linalg.expm(augmented*ts)
    return StateSpace(_freeze(transition[:len(a), :len(a)]), _freeze(transition[:len(a), len(a):]),
                      _freeze(cy), _freeze(dy), system.source,
                      _digest(a, b, cy, dy, np.array([ts])), provenance(), ts)
