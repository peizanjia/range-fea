"""Bounded second-order control analysis for Range FEA linear-model JSON v1.

The input matrices describe perturbations about ``operating_point``. The
module intentionally implements dense systems only (at most 256 physical
coordinates) and reports numerical diagnostics rather than claiming a
certified controllability or observability proof.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Mapping, Sequence

import numpy as np
from scipy import linalg


MODEL_SCHEMA = "range-fea-linear-model"
MODEL_VERSION = 1
REPORT_SCHEMA = "range-fea-analysis-report"
REPORT_VERSION = 1
MAX_DOF = 256
MAX_CHANNELS = 64
MAX_FREQUENCIES = 2048
MAX_FRF_ENTRIES = 100_000
MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_REPORT_BYTES = 64 * 1024 * 1024
SYMMETRY_REL_TOL = 1.0e-10
PSD_REL_TOL = 1.0e-10
CLUSTER_REL_TOL = 1.0e-9
EIGEN_GROUP_REL_TOL = 64.0 * np.finfo(np.float64).eps
PBH_REL_TOL = 1.0e-10
SOLVE_RESIDUAL_TOL = 1.0e-8
_DOF_UNITS = {"UX": "m", "UY": "m", "UZ": "m",
              "RX": "rad", "RY": "rad", "RZ": "rad"}
_OUTPUT_UNITS = {"Displacement": "m", "Velocity": "m/s", "Acceleration": "m/s^2"}
_MATRIX_FIELDS = {"M", "C", "K", "B", "Sq", "Sv", "Sa", "D"}
_REQUIRED_FIELDS = _MATRIX_FIELDS | {
    "schema", "version", "operating_point", "dofs", "inputs", "outputs"
}
_TOP_LEVEL_ALLOWED = _REQUIRED_FIELDS | {"source", "metadata"}
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class AnalysisError(ValueError):
    """Invalid model data or an explicitly failed numerical operation."""


@dataclass(frozen=True)
class LinearModel:
    M: np.ndarray
    C: np.ndarray
    K: np.ndarray
    B: np.ndarray
    Sq: np.ndarray
    Sv: np.ndarray
    Sa: np.ndarray
    D: np.ndarray
    q: np.ndarray
    v: np.ndarray
    a: np.ndarray
    force: np.ndarray
    residual: float
    operating_outputs: np.ndarray | None
    dofs: tuple[dict[str, Any], ...]
    inputs: tuple[dict[str, Any], ...]
    outputs: tuple[dict[str, Any], ...]
    source: dict[str, Any]
    metadata: dict[str, Any]
    extensions: dict[str, Any]
    input_sha256: str | None = None

    @property
    def n(self) -> int:
        return int(self.M.shape[0])

    @property
    def m(self) -> int:
        return int(self.B.shape[1])

    @property
    def p(self) -> int:
        return int(self.Sq.shape[0])


@dataclass(frozen=True)
class DescriptorSystem:
    E: np.ndarray
    A: np.ndarray
    B: np.ndarray
    C: np.ndarray
    D: np.ndarray


@dataclass(frozen=True)
class ReductionResult:
    model: LinearModel
    basis: np.ndarray
    method: str
    metadata: dict[str, Any]


@dataclass(frozen=True)
class FrequencyResponse:
    omega: float
    displacement: np.ndarray  # q/u transfer matrix, shape n by m.
    output: np.ndarray         # y/u transfer matrix, shape p by m.
    residual_norm: float       # ||Z Hq-B||_F / max(||B||_F, tiny).
    residual_absolute_norm: float
    condition_number: float


def _reject_json_constant(value: str) -> None:
    raise AnalysisError(f"Non-finite JSON number {value!r} is not allowed")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AnalysisError(f"Duplicate JSON field {key!r}")
        result[key] = value
    return result


def _strict_json_loads(text: str, label: str) -> Any:
    try:
        return json.loads(text, parse_constant=_reject_json_constant,
                          object_pairs_hook=_unique_object)
    except AnalysisError:
        raise
    except (json.JSONDecodeError, UnicodeError) as error:
        raise AnalysisError(f"Invalid {label} JSON: {error}") from error


def _validate_json_tree(value: Any, path: str = "$") -> None:
    if value is None or isinstance(value, (str, bool)):
        return
    if isinstance(value, int) and not isinstance(value, bool):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise AnalysisError(f"{path} contains a non-finite number")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_json_tree(item, f"{path}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise AnalysisError(f"{path} contains a non-string object key")
            _validate_json_tree(item, f"{path}.{key}")
        return
    raise AnalysisError(f"{path} contains a non-JSON value")


def _number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise AnalysisError(f"{name} must be a JSON number")
    try:
        result = float(value)
    except OverflowError as error:
        raise AnalysisError(f"{name} must be a finite JSON number") from error
    if not math.isfinite(result):
        raise AnalysisError(f"{name} must be finite")
    return result


def _integer(value: Any, name: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise AnalysisError(f"{name} must be an integer >= {minimum}")
    return value


def _identifier(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value or value.strip() != value or "\x00" in value:
        raise AnalysisError(f"{name} must be a non-empty stable string identifier")
    return value


def _vector(value: Any, length: int, name: str) -> np.ndarray:
    if not isinstance(value, list) or len(value) != length:
        raise AnalysisError(f"{name} must be a vector of length {length}")
    result = np.empty(length, dtype=np.float64)
    for index, item in enumerate(value):
        result[index] = _number(item, f"{name}[{index}]")
    result.setflags(write=False)
    return result


def _matrix(value: Any, rows: int, columns: int, name: str) -> np.ndarray:
    if not isinstance(value, list) or len(value) != rows:
        raise AnalysisError(f"{name} must have {rows} rows")
    result = np.empty((rows, columns), dtype=np.float64)
    for row_index, row in enumerate(value):
        if not isinstance(row, list) or len(row) != columns:
            raise AnalysisError(f"{name}[{row_index}] must have {columns} columns")
        for column_index, item in enumerate(row):
            result[row_index, column_index] = _number(
                item, f"{name}[{row_index}][{column_index}]")
    result.setflags(write=False)
    return result


def _catalog(value: Any, name: str, length: int,
             required: tuple[str, ...]) -> tuple[dict[str, Any], ...]:
    if not isinstance(value, list) or len(value) != length:
        raise AnalysisError(f"{name} must contain exactly {length} entries")
    result: list[dict[str, Any]] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise AnalysisError(f"{name}[{index}] must be an object")
        for key in required:
            if key not in item:
                raise AnalysisError(f"{name}[{index}] is missing {key!r}")
        result.append(copy.deepcopy(item))
    return tuple(result)


def _validate_catalogs(dofs: tuple[dict[str, Any], ...],
                       inputs: tuple[dict[str, Any], ...],
                       outputs: tuple[dict[str, Any], ...]) -> None:
    generalized = [row.get("kind") == "generalized-coordinate" for row in dofs]
    if any(generalized):
        if not all(generalized):
            raise AnalysisError("dofs cannot mix physical and generalized-coordinate entries")
        seen_generalized: set[str] = set()
        for index, row in enumerate(dofs):
            identifier = _identifier(row.get("id"), f"dofs[{index}].id")
            if identifier in seen_generalized:
                raise AnalysisError(f"Duplicate generalized-coordinate ID {identifier!r}")
            if row.get("kind") != "generalized-coordinate":
                raise AnalysisError(f"dofs[{index}].kind must be 'generalized-coordinate'")
            seen_generalized.add(identifier)
    else:
        dof_keys: set[tuple[str, str]] = set()
        physical_nodes: dict[str, int] = {}
        node_positions: dict[int, str] = {}
        for index, row in enumerate(dofs):
            physical_id = _identifier(row["nodePhysicalId"], f"dofs[{index}].nodePhysicalId")
            position = _integer(row["nodePosition"], f"dofs[{index}].nodePosition")
            dof = row["dof"]
            if not isinstance(dof, str) or dof not in _DOF_UNITS:
                raise AnalysisError(f"dofs[{index}].dof must be one of {sorted(_DOF_UNITS)}")
            frame = row["frame"]
            if not isinstance(frame, str) or row["unit"] != _DOF_UNITS[dof] or \
                    frame not in {"global", "constraint-local"}:
                raise AnalysisError(f"dofs[{index}] has an unsupported unit or frame")
            if frame == "constraint-local" and "globalDirection" not in row:
                raise AnalysisError(f"dofs[{index}] constraint-local frame needs globalDirection")
            if "globalDirection" in row:
                direction = _vector(row["globalDirection"], 3, f"dofs[{index}].globalDirection")
                norm = float(linalg.norm(direction))
                if abs(norm - 1.0) > 1.0e-9:
                    raise AnalysisError(f"dofs[{index}].globalDirection must be a normalized 3-vector")
            key = (physical_id, dof)
            if key in dof_keys:
                raise AnalysisError(f"Duplicate physical degree of freedom {key!r}")
            dof_keys.add(key)
            if physical_id in physical_nodes and physical_nodes[physical_id] != position:
                raise AnalysisError(f"Physical node {physical_id!r} has inconsistent positions")
            if position in node_positions and node_positions[position] != physical_id:
                raise AnalysisError(f"Node position {position} maps to multiple physical IDs")
            physical_nodes[physical_id] = position
            node_positions[position] = physical_id

    for name, rows in (("inputs", inputs), ("outputs", outputs)):
        seen: set[str] = set()
        for index, row in enumerate(rows):
            identifier = _identifier(row["id"], f"{name}[{index}].id")
            if identifier in seen:
                raise AnalysisError(f"Duplicate {name} identifier {identifier!r}")
            seen.add(identifier)
            _identifier(row["unit"], f"{name}[{index}].unit")
            _identifier(row["nodePhysicalId"], f"{name}[{index}].nodePhysicalId")
            _integer(row["nodePosition"], f"{name}[{index}].nodePosition")
    for index, row in enumerate(outputs):
        quantity = row["quantity"]
        if not isinstance(quantity, str) or quantity not in _OUTPUT_UNITS or \
                row["unit"] != _OUTPUT_UNITS[quantity]:
            raise AnalysisError(f"outputs[{index}] has an unsupported quantity/unit pair")
    for index, row in enumerate(inputs):
        if row["unit"] != "N":
            raise AnalysisError(f"inputs[{index}].unit must be 'N'")


def _validate_source(source: Any) -> dict[str, Any]:
    if source is None:
        return {}
    if not isinstance(source, dict):
        raise AnalysisError("source must be an object when provided")
    copied = copy.deepcopy(source)
    if "kind" in copied and (not isinstance(copied["kind"], str) or copied["kind"] not in {
        "linear-elastic-FE", "taut-cable-tangent-FE"
    }):
        raise AnalysisError("source.kind is not a supported Range FEA linearization")
    if "units" in copied and copied["units"] != "SI":
        raise AnalysisError("source.units must be 'SI'")
    if "sha256" in copied and (not isinstance(copied["sha256"], str) or
                                 not _SHA256_RE.fullmatch(copied["sha256"])):
        raise AnalysisError("source.sha256 must be a lowercase SHA-256 hex digest")
    return copied


def _check_symmetric_psd(matrix: np.ndarray, name: str) -> None:
    scale = max(np.finfo(np.float64).tiny, float(np.max(np.abs(matrix))))
    with np.errstate(over="ignore", invalid="ignore"):
        asymmetry = float(np.max(np.abs(matrix - matrix.T)))
    if not math.isfinite(asymmetry):
        raise AnalysisError(f"{name} has non-finite symmetry error")
    diagonal_scale = np.sqrt(np.abs(np.diag(matrix)))
    pair_scale = np.maximum(np.maximum(np.abs(matrix), np.abs(matrix.T)),
                            diagonal_scale[:, None] * diagonal_scale[None, :])
    pair_scale = np.maximum(pair_scale, np.finfo(np.float64).tiny)
    relative_asymmetry = float(np.max(np.abs(matrix - matrix.T) / pair_scale))
    if not math.isfinite(relative_asymmetry) or relative_asymmetry > SYMMETRY_REL_TOL:
        raise AnalysisError(
            f"{name} must be symmetric (maximum pair-relative error {relative_asymmetry:.3g})")
    symmetric = 0.5 * (matrix + matrix.T)
    diagonal = np.diag(symmetric).copy()
    row_scales = np.max(np.abs(symmetric), axis=1)
    for index, value in enumerate(diagonal):
        if value < 0.0:
            local_scale = max(np.finfo(np.float64).tiny, float(row_scales[index]))
            if -float(value) > PSD_REL_TOL * local_scale:
                raise AnalysisError(f"{name} must be positive semidefinite; negative diagonal entry")
            diagonal[index] = 0.0
    positive = np.flatnonzero(diagonal > 0.0)
    zero = np.flatnonzero(diagonal == 0.0)
    normalized = np.zeros_like(symmetric)
    if positive.size:
        roots = np.sqrt(diagonal[positive])
        with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
            block = symmetric[np.ix_(positive, positive)] / roots[:, None] / roots[None, :]
        if not np.isfinite(block).all():
            raise AnalysisError(f"{name} cannot be equilibrated for a PSD check")
        normalized[np.ix_(positive, positive)] = block
    zero_tolerance = PSD_REL_TOL * scale
    for index in zero:
        off_diagonal = np.delete(symmetric[index, :], index)
        if off_diagonal.size and float(np.max(np.abs(off_diagonal))) > zero_tolerance:
            raise AnalysisError(f"{name} must be positive semidefinite; zero diagonal has coupling")
    try:
        eigenvalues = linalg.eigvalsh(normalized, check_finite=False, driver="evd")
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Could not validate {name}: {error}") from error
    minimum = float(eigenvalues[0])
    tolerance = PSD_REL_TOL * max(1.0, float(np.max(np.abs(eigenvalues))))
    if minimum < -tolerance:
        raise AnalysisError(f"{name} must be positive semidefinite; minimum eigenvalue {minimum:.6g}")


def parse_model(document: Mapping[str, Any]) -> LinearModel:
    """Validate and build a dense second-order model from a decoded JSON object.

    Required catalogue entries follow the C++ Stage 9 manifest: DOF rows carry
    nodePhysicalId/nodePosition/dof/unit/frame; input and output rows carry
    id/unit/nodePhysicalId/nodePosition, with output quantity as well.
    Unknown non-matrix metadata is preserved. Unknown matrix-like fields are
    rejected so misspelled matrix names cannot be silently ignored.
    """
    if not isinstance(document, Mapping):
        raise AnalysisError("Model root must be a JSON object")
    raw = dict(document)
    _validate_json_tree(raw)
    missing = sorted(_REQUIRED_FIELDS - raw.keys())
    if missing:
        raise AnalysisError(f"Model is missing required fields: {', '.join(missing)}")
    if raw["schema"] != MODEL_SCHEMA or raw["version"] != MODEL_VERSION or \
            isinstance(raw["version"], bool) or not isinstance(raw["version"], int):
        raise AnalysisError(f"Expected schema {MODEL_SCHEMA!r} version {MODEL_VERSION}")

    for key, value in raw.items():
        if key in _TOP_LEVEL_ALLOWED:
            continue
        matrix_like = isinstance(value, list) and bool(value) and all(
            isinstance(row, list) for row in value)
        if key[:1].isupper() or matrix_like:
            raise AnalysisError(f"Unknown matrix-like field {key!r}")

    dofs = raw["dofs"]
    if not isinstance(dofs, list) or not 1 <= len(dofs) <= MAX_DOF:
        raise AnalysisError(f"dofs must contain between 1 and {MAX_DOF} entries")
    n = len(dofs)
    inputs_raw = raw["inputs"]
    outputs_raw = raw["outputs"]
    if not isinstance(inputs_raw, list) or not 1 <= len(inputs_raw) <= MAX_CHANNELS:
        raise AnalysisError(f"inputs must contain between 1 and {MAX_CHANNELS} force inputs")
    if not isinstance(outputs_raw, list) or not 1 <= len(outputs_raw) <= MAX_CHANNELS:
        raise AnalysisError(f"outputs must contain between 1 and {MAX_CHANNELS} measured outputs")
    m, p = len(inputs_raw), len(outputs_raw)
    generalized_catalog = all(isinstance(row, dict) and
                              row.get("kind") == "generalized-coordinate" for row in dofs)
    dof_required = (("id", "kind") if generalized_catalog else
                    ("nodePhysicalId", "nodePosition", "dof", "unit", "frame"))
    dof_catalog = _catalog(dofs, "dofs", n, dof_required)
    input_catalog = _catalog(inputs_raw, "inputs", m,
                              ("id", "unit", "nodePhysicalId", "nodePosition"))
    output_catalog = _catalog(outputs_raw, "outputs", p,
                               ("id", "quantity", "unit", "nodePhysicalId", "nodePosition"))
    _validate_catalogs(dof_catalog, input_catalog, output_catalog)

    mass = _matrix(raw["M"], n, n, "M")
    damping = _matrix(raw["C"], n, n, "C")
    stiffness = _matrix(raw["K"], n, n, "K")
    force_map = _matrix(raw["B"], n, m, "B")
    sensor_q = _matrix(raw["Sq"], p, n, "Sq")
    sensor_v = _matrix(raw["Sv"], p, n, "Sv")
    sensor_a = _matrix(raw["Sa"], p, n, "Sa")
    feedthrough = _matrix(raw["D"], p, m, "D")
    _check_symmetric_psd(mass, "M")
    try:
        linalg.cholesky(mass, lower=True, check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"M must be positive definite: {error}") from error
    _check_symmetric_psd(damping, "C")
    _check_symmetric_psd(stiffness, "K")

    op = raw["operating_point"]
    if not isinstance(op, dict):
        raise AnalysisError("operating_point must be an object")
    for field in ("q", "v", "a", "force", "residual"):
        if field not in op:
            raise AnalysisError(f"operating_point is missing {field!r}")
    op_outputs = None
    if "outputs" in op:
        op_outputs = _vector(op["outputs"], p, "operating_point.outputs")
    metadata = raw.get("metadata", {})
    if not isinstance(metadata, dict):
        raise AnalysisError("metadata must be an object when provided")
    extensions = {key: copy.deepcopy(value) for key, value in raw.items()
                  if key not in _REQUIRED_FIELDS and key not in {"source", "metadata"}}
    return LinearModel(
        M=mass, C=damping, K=stiffness, B=force_map,
        Sq=sensor_q, Sv=sensor_v, Sa=sensor_a, D=feedthrough,
        q=_vector(op["q"], n, "operating_point.q"),
        v=_vector(op["v"], n, "operating_point.v"),
        a=_vector(op["a"], n, "operating_point.a"),
        force=_vector(op["force"], n, "operating_point.force"),
        residual=_number(op["residual"], "operating_point.residual"),
        operating_outputs=op_outputs,
        dofs=dof_catalog, inputs=input_catalog, outputs=output_catalog,
        source=_validate_source(raw.get("source")),
        metadata=copy.deepcopy(metadata), extensions=extensions,
    )


def load_model(path: str | os.PathLike[str]) -> LinearModel:
    """Read a bounded UTF-8 JSON model and bind its exact input-file digest."""
    file_path = Path(path)
    try:
        size = file_path.stat().st_size
        if size > MAX_INPUT_BYTES:
            raise AnalysisError(f"Model JSON exceeds {MAX_INPUT_BYTES} bytes")
        with file_path.open("rb") as source:
            data = source.read(MAX_INPUT_BYTES + 1)
        final_size = file_path.stat().st_size
    except OSError as error:
        raise AnalysisError(f"Could not read model file: {error}") from error
    if len(data) > MAX_INPUT_BYTES:
        raise AnalysisError(f"Model JSON exceeds {MAX_INPUT_BYTES} bytes")
    if len(data) != size or len(data) != final_size:
        raise AnalysisError("Model file changed while it was being read")
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise AnalysisError("Model JSON must be valid UTF-8") from error
    document = _strict_json_loads(text, "model")
    model = parse_model(document)
    return replace(model, input_sha256=hashlib.sha256(data).hexdigest())


def _solve_mass(model: LinearModel, rhs: np.ndarray, context: str) -> np.ndarray:
    try:
        result = linalg.solve(model.M, rhs, assume_a="pos", check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Mass solve failed while computing {context}: {error}") from error
    if not np.isfinite(result).all():
        raise AnalysisError(f"Mass solve produced non-finite values while computing {context}")
    return result


def descriptor(model: LinearModel) -> DescriptorSystem:
    """Return descriptor matrices for x=[q;v], E xdot=A x+B u, y=C x+D u."""
    n = model.n
    zero = np.zeros((n, n), dtype=np.float64)
    identity = np.eye(n, dtype=np.float64)
    E = linalg.block_diag(identity, model.M)
    A = np.block([[zero, identity], [-model.K, -model.C]])
    B = np.vstack((np.zeros((n, model.m), dtype=np.float64), model.B))
    C = np.hstack((model.Sq - model.Sa @ _solve_mass(model, model.K, "descriptor C"),
                   model.Sv - model.Sa @ _solve_mass(model, model.C, "descriptor C")))
    D = model.D + model.Sa @ _solve_mass(model, model.B, "descriptor D")
    if not all(np.isfinite(matrix).all() for matrix in (E, A, B, C, D)):
        raise AnalysisError("Descriptor construction produced non-finite values")
    return DescriptorSystem(E=E, A=A, B=B, C=C, D=D)


def _check_integer_k(k: int, maximum: int, allow_zero: bool) -> int:
    if isinstance(k, bool) or not isinstance(k, int):
        raise AnalysisError("Mode count must be an integer")
    lower = 0 if allow_zero else 1
    if k < lower or k > maximum:
        raise AnalysisError(f"Mode count must be in [{lower}, {maximum}]")
    return k


def _reject_partial_cluster(eigenvalues: np.ndarray, k: int, label: str) -> None:
    if k <= 0 or k >= len(eigenvalues):
        return
    left = float(eigenvalues[k - 1])
    right = float(eigenvalues[k])
    threshold = CLUSTER_REL_TOL * max(np.finfo(np.float64).tiny, abs(left), abs(right))
    if abs(right - left) <= threshold:
        raise AnalysisError(
            f"{label} cutoff k={k} splits a repeated eigenvalue cluster "
            f"({left:.12g}, {right:.12g})")


def _generalized_dofs(method: str, count: int, start: int = 0) -> tuple[dict[str, Any], ...]:
    return tuple({"id": f"{method}-{index + start}", "kind": "generalized-coordinate"}
                 for index in range(count))


def _project_model(model: LinearModel, basis: np.ndarray, method: str,
                   reduced_dofs: tuple[dict[str, Any], ...],
                   metadata: dict[str, Any]) -> LinearModel:
    Mr = basis.T @ model.M @ basis
    Cr = basis.T @ model.C @ basis
    Kr = basis.T @ model.K @ basis
    Br = basis.T @ model.B
    force = basis.T @ model.force
    Sqr = model.Sq @ basis
    Svr = model.Sv @ basis
    Sar = model.Sa @ basis
    try:
        q = linalg.solve(Mr, basis.T @ model.M @ model.q, assume_a="pos", check_finite=False)
        v = linalg.solve(Mr, basis.T @ model.M @ model.v, assume_a="pos", check_finite=False)
        a = linalg.solve(Mr, basis.T @ model.M @ model.a, assume_a="pos", check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Could not project operating point for {method}: {error}") from error
    arrays = (Mr, Cr, Kr, Br, Sqr, Svr, Sar, q, v, a, force)
    if not all(np.isfinite(array).all() for array in arrays):
        raise AnalysisError(f"{method} reduction produced non-finite matrices")
    for array in arrays[:7]:
        array.setflags(write=False)
    for array in arrays[7:]:
        array.setflags(write=False)
    feedthrough = np.array(model.D, copy=True)
    feedthrough.setflags(write=False)
    return LinearModel(
        M=Mr, C=Cr, K=Kr, B=Br, Sq=Sqr, Sv=Svr, Sa=Sar, D=feedthrough,
        q=q, v=v, a=a, force=force, residual=model.residual,
        operating_outputs=model.operating_outputs,
        dofs=reduced_dofs, inputs=model.inputs, outputs=model.outputs,
        source=copy.deepcopy(model.source),
        metadata={**copy.deepcopy(model.metadata), "reduction": {
            "method": method, **copy.deepcopy(metadata),
            "operatingPointCoordinates": "mass-projected source full-model coordinates",
            "operatingPointResidualScope": "source-full-operating-point"}},
        extensions=copy.deepcopy(model.extensions),
        input_sha256=model.input_sha256)


def modal_reduce(model: LinearModel, k: int) -> ReductionResult:
    """Mass-normalized lowest-frequency Ritz reduction with cluster protection."""
    k = _check_integer_k(k, model.n, allow_zero=False)
    try:
        eigenvalues, vectors = linalg.eigh(model.K, model.M, check_finite=False, driver="gvd")
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Modal generalized eigensolve failed: {error}") from error
    _reject_partial_cluster(eigenvalues, k, "Modal")
    basis = np.array(vectors[:, :k], dtype=np.float64, copy=True)
    retained = np.asarray(eigenvalues[:k], dtype=np.float64)
    mass_error = float(linalg.norm(basis.T @ model.M @ basis - np.eye(k), ord="fro"))
    stiffness_modes = model.K @ basis
    mass_modes = (model.M @ basis) * retained[None, :]
    eigen_residual = float(linalg.norm(stiffness_modes - mass_modes, ord="fro"))
    eigen_scale = max(float(linalg.norm(stiffness_modes, ord="fro")),
                      float(linalg.norm(mass_modes, ord="fro")),
                      np.finfo(np.float64).tiny)
    relative_eigen_residual = eigen_residual / eigen_scale
    if not math.isfinite(mass_error) or mass_error > SOLVE_RESIDUAL_TOL or \
            not math.isfinite(relative_eigen_residual) or \
            relative_eigen_residual > SOLVE_RESIDUAL_TOL:
        raise AnalysisError("Modal basis failed mass-normalization or generalized-eigen residual check")
    basis.setflags(write=False)
    metadata = {"requestedModes": k, "eigenvalues": retained.tolist(),
                "massOrthonormal": True, "massOrthonormalityErrorFro": mass_error,
                "generalizedEigenResidualRelative": relative_eigen_residual}
    reduced = _project_model(model, basis, "modal", _generalized_dofs("modal", k), metadata)
    return ReductionResult(reduced, basis, "modal", metadata)


def craig_bampton(model: LinearModel, boundary_indices: Sequence[int], k: int) -> ReductionResult:
    """Craig-Bampton reduction with static constraint modes and fixed-interface modes."""
    try:
        boundary = tuple(boundary_indices)
    except TypeError as error:
        raise AnalysisError("boundary_indices must be a sequence of integer DOF indices") from error
    if not boundary:
        raise AnalysisError("Craig-Bampton requires at least one boundary DOF")
    if any(isinstance(index, bool) or not isinstance(index, int) for index in boundary):
        raise AnalysisError("boundary_indices must contain only integers")
    if len(set(boundary)) != len(boundary):
        raise AnalysisError("boundary_indices contains duplicates")
    if any(index < 0 or index >= model.n for index in boundary):
        raise AnalysisError(f"boundary_indices must lie in [0, {model.n - 1}]")
    boundary_set = set(boundary)
    interior = tuple(index for index in range(model.n) if index not in boundary_set)
    if not interior:
        raise AnalysisError("Craig-Bampton requires at least one interior DOF")
    k = _check_integer_k(k, len(interior), allow_zero=True)
    Kii = model.K[np.ix_(interior, interior)]
    Mii = model.M[np.ix_(interior, interior)]
    Kib = model.K[np.ix_(interior, boundary)]
    try:
        linalg.cholesky(Kii, lower=True, check_finite=False)
        static_modes = -linalg.solve(Kii, Kib, assume_a="pos", check_finite=False)
        eigenvalues, fixed_vectors = linalg.eigh(Kii, Mii, check_finite=False, driver="gvd")
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Craig-Bampton fixed-interface solve failed: {error}") from error
    static_residual = Kii @ static_modes + Kib
    static_residual_abs = float(linalg.norm(static_residual, ord="fro"))
    static_residual_scale = max(float(linalg.norm(Kii @ static_modes, ord="fro")),
                                float(linalg.norm(Kib, ord="fro")),
                                np.finfo(np.float64).tiny)
    static_residual_relative = static_residual_abs / static_residual_scale
    if not math.isfinite(static_residual_relative) or \
            static_residual_relative > SOLVE_RESIDUAL_TOL:
        raise AnalysisError("Craig-Bampton static constraint modes failed their residual check")
    _reject_partial_cluster(eigenvalues, k, "Craig-Bampton fixed-interface")
    nb = len(boundary)
    basis = np.zeros((model.n, nb + k), dtype=np.float64)
    basis[np.ix_(boundary, range(nb))] = np.eye(nb)
    basis[np.ix_(interior, range(nb))] = static_modes
    if k:
        basis[np.ix_(interior, range(nb, nb + k))] = fixed_vectors[:, :k]
    basis.setflags(write=False)
    fixed_values = np.asarray(eigenvalues[:k], dtype=np.float64)
    metadata = {"boundaryIndices": list(boundary), "interiorIndices": list(interior),
                "coordinateKinds": ["boundary"] * nb + ["fixed-interface"] * k,
                "fixedInterfaceModes": k, "fixedInterfaceEigenvalues": fixed_values.tolist(),
                "staticConstraintResidualRelative": static_residual_relative}
    cb_dofs = _generalized_dofs("craig-bampton", nb + k)
    reduced = _project_model(model, basis, "craig-bampton", cb_dofs, metadata)
    return ReductionResult(reduced, basis, "craig-bampton", metadata)


def frequency_response(model: LinearModel, omega: float) -> FrequencyResponse:
    """Solve the complex harmonic transfer for q/u and y/u at angular frequency."""
    frequency = _number(omega, "omega")
    if frequency < 0.0:
        raise AnalysisError("omega must be non-negative")
    dynamic_stiffness = model.K.astype(np.complex128) - frequency * frequency * model.M + \
        (1j * frequency) * model.C
    if not np.isfinite(dynamic_stiffness).all():
        raise AnalysisError("Dynamic stiffness contains non-finite values")
    try:
        singular_values = linalg.svdvals(dynamic_stiffness, check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Could not assess dynamic stiffness at omega={frequency:g}: {error}") from error
    largest = float(singular_values[0]) if singular_values.size else 0.0
    smallest = float(singular_values[-1]) if singular_values.size else 0.0
    threshold = np.finfo(np.float64).eps * max(model.n, 1) * largest * 32.0
    if largest == 0.0 or smallest <= threshold:
        raise AnalysisError(f"Dynamic stiffness is singular at omega={frequency:.12g}")
    condition = largest / smallest
    try:
        displacement = linalg.solve(dynamic_stiffness, model.B, assume_a="gen", check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Frequency solve failed at omega={frequency:.12g}: {error}") from error
    residual = dynamic_stiffness @ displacement - model.B
    residual_absolute = float(linalg.norm(residual, ord="fro"))
    denominator = max(float(linalg.norm(model.B, ord="fro")), np.finfo(np.float64).tiny)
    residual_norm = residual_absolute / denominator
    output_matrix = model.Sq.astype(np.complex128) + 1j * frequency * model.Sv - \
        frequency * frequency * model.Sa
    output = output_matrix @ displacement + model.D
    if not np.isfinite(displacement).all() or not np.isfinite(output).all() or \
            not math.isfinite(residual_norm):
        raise AnalysisError(f"Frequency response is non-finite at omega={frequency:.12g}")
    if residual_norm > SOLVE_RESIDUAL_TOL:
        raise AnalysisError(
            f"Frequency solve residual {residual_norm:.3g} exceeds {SOLVE_RESIDUAL_TOL:g} "
            f"at omega={frequency:.12g}")
    return FrequencyResponse(frequency, displacement, output,
                             residual_norm, residual_absolute, condition)


def _equilibrated_rank(matrix: np.ndarray) -> tuple[int, float]:
    scaled = np.asarray(matrix, dtype=np.complex128).copy()
    row_scale = np.max(np.abs(scaled), axis=1)
    row_scale[row_scale == 0.0] = 1.0
    scaled /= row_scale[:, None]
    column_scale = np.max(np.abs(scaled), axis=0)
    column_scale[column_scale == 0.0] = 1.0
    scaled /= column_scale[None, :]
    singular_values = linalg.svdvals(scaled, check_finite=False)
    largest = float(singular_values[0]) if singular_values.size else 0.0
    smallest = float(singular_values[-1]) if singular_values.size else 0.0
    if largest == 0.0:
        return 0, 0.0
    threshold = PBH_REL_TOL * largest
    return int(np.count_nonzero(singular_values > threshold)), smallest / largest


def _eigenvalue_groups(eigenvalues: np.ndarray) -> list[list[complex]]:
    ordered = sorted((complex(value) for value in eigenvalues), key=lambda value: (value.real, value.imag))
    groups: list[list[complex]] = []
    for value in ordered:
        previous = groups[-1][-1] if groups else None
        threshold = EIGEN_GROUP_REL_TOL * max(
            np.finfo(np.float64).tiny, abs(value), abs(previous) if previous is not None else 0.0)
        if previous is not None and abs(value - previous) <= threshold:
            groups[-1].append(value)
        else:
            groups.append([value])
    return groups


def diagnostics(model: LinearModel) -> dict[str, Any]:
    """Return scaled PBH controllability/observability ranks for descriptor modes."""
    system = descriptor(model)
    try:
        eigenvalues = linalg.eigvals(system.A, system.E, check_finite=False)
    except (ValueError, linalg.LinAlgError) as error:
        raise AnalysisError(f"Descriptor eigenvalue solve failed: {error}") from error
    dimension = 2 * model.n
    control_modes: list[dict[str, Any]] = []
    observe_modes: list[dict[str, Any]] = []
    if not np.isfinite(eigenvalues).all():
        raise AnalysisError("Descriptor eigenvalue solve produced non-finite eigenvalues")
    for group in _eigenvalue_groups(eigenvalues):
        control_results: list[tuple[int, float]] = []
        observe_results: list[tuple[int, float]] = []
        for eigenvalue in group:
            # Rank is evaluated at each actual computed eigenvalue. The group
            # only compresses machine-identical roots in the report; averaging
            # close but distinct roots can hide an uncontrollable mode.
            pencil = eigenvalue * system.E - system.A
            control_results.append(_equilibrated_rank(np.hstack((pencil, system.B))))
            observe_results.append(_equilibrated_rank(np.vstack((pencil, system.C))))
        eigenvalue = group[0]
        multiplicity = len(group)
        controllability_rank = min(result[0] for result in control_results)
        control_ratio = min(result[1] for result in control_results)
        observability_rank = min(result[0] for result in observe_results)
        observe_ratio = min(result[1] for result in observe_results)
        eigen_record = {"real": float(eigenvalue.real), "imag": float(eigenvalue.imag)}
        control_modes.append({"eigenvalue": eigen_record, "multiplicity": multiplicity,
                              "rank": controllability_rank, "dimension": dimension,
                              "smallestToLargestSingularRatio": control_ratio})
        observe_modes.append({"eigenvalue": eigen_record, "multiplicity": multiplicity,
                              "rank": observability_rank, "dimension": dimension,
                              "smallestToLargestSingularRatio": observe_ratio})
    controllable = all(mode["rank"] == dimension for mode in control_modes)
    observable = all(mode["rank"] == dimension for mode in observe_modes)
    return {
        "stateDimension": dimension,
        "rankToleranceRelative": PBH_REL_TOL,
        "scaling": "row and column max-norm equilibration before SVD",
        "operatingPointResidual": model.residual,
        "operatingPointResidualScope": "source-full-operating-point",
        "controllability": {"fullRank": controllable,
                            "minimumPbhRank": min(mode["rank"] for mode in control_modes),
                            "modes": control_modes},
        "observability": {"fullRank": observable,
                          "minimumPbhRank": min(mode["rank"] for mode in observe_modes),
                          "modes": observe_modes},
    }


def compare_models(full: LinearModel, rom: LinearModel,
                   omegas: Sequence[float], relative_floor: float = 1.0e-12) -> dict[str, Any]:
    """Compare FRFs with relative denominator max(|H_full|, relative_floor)."""
    floor = _number(relative_floor, "relative_floor")
    if floor <= 0.0:
        raise AnalysisError("relative_floor must be positive")
    if [row["id"] for row in full.inputs] != [row["id"] for row in rom.inputs] or \
            [row["id"] for row in full.outputs] != [row["id"] for row in rom.outputs]:
        raise AnalysisError("Full and reduced models must use matching input/output IDs and order")
    if [row["unit"] for row in full.inputs] != [row["unit"] for row in rom.inputs] or \
            [(row["quantity"], row["unit"]) for row in full.outputs] != \
            [(row["quantity"], row["unit"]) for row in rom.outputs]:
        raise AnalysisError("Full and reduced models must use matching input/output quantities and units")
    transfer_units = [[f"{output['unit']}/{input_row['unit']}" for input_row in full.inputs]
                      for output in full.outputs]
    comparisons: list[dict[str, Any]] = []
    for index, omega in enumerate(omegas):
        if index >= MAX_FREQUENCIES:
            raise AnalysisError(f"frequencies may contain at most {MAX_FREQUENCIES} entries")
        if (index + 1) * full.p * full.m > MAX_FRF_ENTRIES:
            raise AnalysisError(f"FRF comparison exceeds {MAX_FRF_ENTRIES} transfer entries")
        full_response = frequency_response(full, omega)
        rom_response = frequency_response(rom, omega)
        difference = np.abs(rom_response.output - full_response.output)
        relative = difference / np.maximum(np.abs(full_response.output), floor)
        if not np.isfinite(difference).all() or not np.isfinite(relative).all():
            raise AnalysisError(f"Non-finite FRF comparison at omega={float(omega):g}")
        by_unit: dict[str, float] = {}
        for row in range(full.p):
            for column in range(full.m):
                unit = transfer_units[row][column]
                by_unit[unit] = max(by_unit.get(unit, 0.0), float(difference[row, column]))
        comparisons.append({
            "omega": float(omega),
            "absoluteError": difference.tolist(),
            "relativeError": relative.tolist(),
            # An absolute maximum across m/N and (m/s^2)/N has no physical unit.
            "absoluteErrorUnits": transfer_units,
            "maximumAbsoluteErrorByUnit": by_unit,
            "maximumAbsoluteError": float(np.max(difference)) if len(by_unit) == 1 else None,
            "maximumRelativeError": float(np.max(relative)),
            "fullResidualNorm": full_response.residual_norm,
            "reducedResidualNorm": rom_response.residual_norm,
        })
    return {"relativeDenominator": "max(abs(full transfer), relativeFloor)",
            "relativeFloor": floor, "frequencies": comparisons}


def select_modal_order(model: LinearModel, omegas: Sequence[float],
                       tolerance: float, maximum_modes: int | None = None) -> dict[str, Any]:
    """Choose the first whole modal cluster passing all supplied FRF samples.

    This is a sampled-frequency error gate, not a continuous-band or closed-loop
    stability certificate. Near channel zeros the existing denominator floor
    remains conservative; absolute errors are retained in each comparison.
    """
    limit = model.n if maximum_modes is None else _check_integer_k(maximum_modes, model.n, False)
    tolerance = _number(tolerance, "relative tolerance")
    if not 0 < tolerance < 1:
        raise AnalysisError("relative tolerance must be in (0,1)")
    if not omegas or len(omegas) > MAX_FREQUENCIES:
        raise AnalysisError("automatic modal selection requires a bounded non-empty frequency list")
    # Invalid/singular full-order samples must not be hidden as failed ROMs.
    for omega in omegas:
        frequency_response(model, omega)
    attempts: list[dict[str, Any]] = []
    selected = None
    for count in range(1, limit + 1):
        try:
            reduction = modal_reduce(model, count)
            comparison = compare_models(model, reduction.model, omegas)
            maximum = max(row["maximumRelativeError"] for row in comparison["frequencies"])
        except AnalysisError as error:
            attempts.append({"modes": count, "error": str(error)})
            continue
        selected = {"modes": count, "maximumRelativeError": maximum,
                    "metTolerance": maximum <= tolerance, "comparison": comparison}
        attempts.append({"modes": count, "maximumRelativeError": maximum,
                         "metTolerance": selected["metTolerance"]})
        if selected["metTolerance"]:
            break
    if selected is None:
        raise AnalysisError("no valid whole-cluster modal order within maximum_modes")
    return {**selected, "relativeTolerance": tolerance, "maximumModes": limit,
            "scope": "supplied angular-frequency samples only; no intersample or stability guarantee",
            "attempts": attempts}


def _array_json(array: np.ndarray) -> list[Any]:
    return np.asarray(array).tolist()


def model_to_json(model: LinearModel) -> dict[str, Any]:
    operating_point: dict[str, Any] = {
        "q": _array_json(model.q), "v": _array_json(model.v),
        "a": _array_json(model.a), "force": _array_json(model.force),
        "residual": model.residual,
    }
    if model.operating_outputs is not None:
        operating_point["outputs"] = _array_json(model.operating_outputs)
    result: dict[str, Any] = {
        "schema": MODEL_SCHEMA, "version": MODEL_VERSION,
        "M": _array_json(model.M), "C": _array_json(model.C), "K": _array_json(model.K),
        "B": _array_json(model.B), "Sq": _array_json(model.Sq), "Sv": _array_json(model.Sv),
        "Sa": _array_json(model.Sa), "D": _array_json(model.D),
        "operating_point": operating_point,
        "dofs": copy.deepcopy(list(model.dofs)),
        "inputs": copy.deepcopy(list(model.inputs)),
        "outputs": copy.deepcopy(list(model.outputs)),
    }
    if model.source:
        result["source"] = copy.deepcopy(model.source)
    if model.metadata:
        result["metadata"] = copy.deepcopy(model.metadata)
    result.update(copy.deepcopy(model.extensions))
    return result


def descriptor_to_json(system: DescriptorSystem) -> dict[str, Any]:
    return {"E": _array_json(system.E), "A": _array_json(system.A),
            "B": _array_json(system.B), "C": _array_json(system.C),
            "D": _array_json(system.D)}


def _complex_matrix_json(matrix: np.ndarray) -> dict[str, Any]:
    matrix = np.asarray(matrix, dtype=np.complex128)
    return {"real": matrix.real.tolist(), "imag": matrix.imag.tolist()}


def frequency_response_to_json(response: FrequencyResponse) -> dict[str, Any]:
    return {"omega": response.omega,
            "displacementTransfer": _complex_matrix_json(response.displacement),
            "outputTransfer": _complex_matrix_json(response.output),
            "residualNorm": response.residual_norm,
            "residualAbsoluteNorm": response.residual_absolute_norm,
            "conditionNumber": response.condition_number}


def analyze_model(model: LinearModel, modes: int = 0,
                  boundary_indices: Sequence[int] | None = None,
                  frequencies: Sequence[float] = ()) -> dict[str, Any]:
    """Run requested model, reduction, FRF, and diagnostic analyses.

    Per-analysis failures are included in ``errors`` so a valid bounded report
    can still be written when, for example, a requested frequency is singular.
    """
    if isinstance(modes, bool) or not isinstance(modes, int) or modes < 0:
        raise AnalysisError("modes must be a non-negative integer")
    frequency_values: list[float] = []
    for index, value in enumerate(frequencies):
        if index >= MAX_FREQUENCIES:
            raise AnalysisError(f"frequencies may contain at most {MAX_FREQUENCIES} entries")
        frequency_values.append(_number(value, f"frequencies[{index}]"))
    for value in frequency_values:
        if value < 0.0:
            raise AnalysisError("frequencies must be non-negative")
    report: dict[str, Any] = {
        "schema": REPORT_SCHEMA, "version": REPORT_VERSION,
        "provenance": {"modelSha256": model.input_sha256,
                       "source": copy.deepcopy(model.source)},
        "errors": [], "models": {"full": model_to_json(model)},
        "descriptor": {}, "diagnostics": {}, "frequencyResponse": {},
        "reductions": {},
    }
    variants: dict[str, LinearModel] = {"full": model}
    try:
        report["descriptor"]["full"] = descriptor_to_json(descriptor(model))
    except AnalysisError as error:
        report["errors"].append({"stage": "descriptor.full", "message": str(error)})
    try:
        report["diagnostics"]["full"] = diagnostics(model)
    except AnalysisError as error:
        report["errors"].append({"stage": "diagnostics.full", "message": str(error)})

    if modes:
        try:
            result = modal_reduce(model, modes)
            variants["modal"] = result.model
            report["models"]["modal"] = model_to_json(result.model)
            report["reductions"]["modal"] = {
                "method": result.method, "metadata": result.metadata,
                "basis": _array_json(result.basis)}
        except AnalysisError as error:
            report["errors"].append({"stage": "reduction.modal", "message": str(error)})
    if boundary_indices is not None:
        try:
            result = craig_bampton(model, boundary_indices, modes)
            variants["craigBampton"] = result.model
            report["models"]["craigBampton"] = model_to_json(result.model)
            report["reductions"]["craigBampton"] = {
                "method": result.method, "metadata": result.metadata,
                "basis": _array_json(result.basis)}
        except AnalysisError as error:
            report["errors"].append({"stage": "reduction.craigBampton", "message": str(error)})

    for name, variant in variants.items():
        if name != "full":
            try:
                report["descriptor"][name] = descriptor_to_json(descriptor(variant))
            except AnalysisError as error:
                report["errors"].append({"stage": f"descriptor.{name}", "message": str(error)})
            try:
                report["diagnostics"][name] = diagnostics(variant)
            except AnalysisError as error:
                report["errors"].append({"stage": f"diagnostics.{name}", "message": str(error)})

    if frequency_values:
        requested_entries = len(frequency_values) * len(variants) * model.p * model.m
        if requested_entries > MAX_FRF_ENTRIES:
            raise AnalysisError(
                f"FRF report exceeds {MAX_FRF_ENTRIES} transfer entries")
        full_rows: list[dict[str, Any]] = []
        for omega in frequency_values:
            try:
                full_rows.append(frequency_response_to_json(frequency_response(model, omega)))
            except AnalysisError as error:
                full_rows.append({"omega": omega, "error": str(error)})
                report["errors"].append({"stage": "frequencyResponse.full", "omega": omega,
                                         "message": str(error)})
        report["frequencyResponse"]["full"] = full_rows
        for name, variant in variants.items():
            if name == "full":
                continue
            rows: list[dict[str, Any]] = []
            comparisons: list[dict[str, Any]] = []
            for omega in frequency_values:
                try:
                    response = frequency_response(variant, omega)
                    rows.append(frequency_response_to_json(response))
                    comparisons.append(compare_models(model, variant, [omega])["frequencies"][0])
                except AnalysisError as error:
                    rows.append({"omega": omega, "error": str(error)})
                    report["errors"].append({"stage": f"frequencyResponse.{name}",
                                             "omega": omega, "message": str(error)})
            report["frequencyResponse"][name] = rows
            report["frequencyResponse"][f"comparison.{name}"] = {
                "relativeDenominator": "max(abs(full transfer), relativeFloor)",
                "relativeFloor": 1.0e-12, "frequencies": comparisons}
    return report


def atomic_write_json(path: str | os.PathLike[str], report: Mapping[str, Any]) -> None:
    """Atomically publish a finite JSON report bounded to 64 MiB."""
    try:
        encoded = (json.dumps(report, ensure_ascii=False, allow_nan=False,
                              separators=(",", ":")) + "\n").encode("utf-8")
    except (TypeError, ValueError) as error:
        raise AnalysisError(f"Report is not finite JSON: {error}") from error
    if len(encoded) > MAX_REPORT_BYTES:
        raise AnalysisError(f"Analysis report exceeds {MAX_REPORT_BYTES} bytes")
    destination = Path(path)
    parent = destination.parent if str(destination.parent) else Path(".")
    if not parent.is_dir():
        raise AnalysisError(f"Output directory does not exist: {parent}")
    temporary_path: str | None = None
    try:
        descriptor_fd, temporary_path = tempfile.mkstemp(
            prefix=f".{destination.name}.", suffix=".tmp", dir=str(parent))
        with os.fdopen(descriptor_fd, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, destination)
        temporary_path = None
    except OSError as error:
        raise AnalysisError(f"Could not atomically write report: {error}") from error
    finally:
        if temporary_path is not None:
            try:
                os.unlink(temporary_path)
            except OSError:
                pass


def _read_json_argument(value: str, name: str) -> Any:
    return _strict_json_loads(value, name)


def _bounded_file_sha256(path: Path) -> str | None:
    """Hash a file only when it fits the documented input bound."""
    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > MAX_INPUT_BYTES:
                    return None
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, help="Path to range-fea-linear-model JSON v1")
    parser.add_argument("--output", required=True, help="Atomic JSON analysis report destination")
    parser.add_argument("--modes", type=int, default=0,
                        help="Retained low-frequency modes; zero skips modal reduction")
    parser.add_argument("--boundary-indices-json", default=None,
                        help="JSON array of physical DOF indices for Craig-Bampton reduction")
    parser.add_argument("--frequencies-json", default="[]",
                        help="JSON array of angular frequencies in rad/s")
    parser.add_argument("--auto-modal-relative-tolerance", type=float,
                        help="Choose the first modal order meeting this relative FRF error at every supplied sample")
    parser.add_argument("--max-auto-modes", type=int,
                        help="Maximum retained modes for automatic selection; defaults to full order")
    arguments = parser.parse_args(argv)
    model_path = Path(arguments.model)
    output_path = Path(arguments.output)
    if model_path.resolve() == output_path.resolve():
        parser.error("--output must not overwrite --model")
    try:
        frequencies = _read_json_argument(arguments.frequencies_json, "frequencies")
        if not isinstance(frequencies, list):
            raise AnalysisError("--frequencies-json must be a JSON array")
        boundaries = None
        if arguments.boundary_indices_json is not None:
            boundaries = _read_json_argument(arguments.boundary_indices_json, "boundary indices")
            if not isinstance(boundaries, list):
                raise AnalysisError("--boundary-indices-json must be a JSON array")
        model = load_model(model_path)
        selection = None
        modes = arguments.modes
        if arguments.auto_modal_relative_tolerance is not None:
            if modes != 0 or boundaries is not None:
                raise AnalysisError("automatic modal selection cannot combine with --modes or Craig-Bampton boundaries")
            selection = select_modal_order(model, frequencies, arguments.auto_modal_relative_tolerance, arguments.max_auto_modes)
            modes = selection["modes"]
        elif arguments.max_auto_modes is not None:
            raise AnalysisError("--max-auto-modes requires --auto-modal-relative-tolerance")
        report = analyze_model(model, modes, boundaries, frequencies)
        if selection is not None:
            report["modalSelection"] = selection
            if not selection["metTolerance"]:
                report["errors"].append({"stage": "modalSelection", "message": "requested sampled FRF tolerance not met within maximum modes"})
        report["provenance"]["modelPath"] = str(model_path.resolve())
        atomic_write_json(output_path, report)
        return 1 if report["errors"] else 0
    except AnalysisError as error:
        digest = _bounded_file_sha256(model_path)
        failure_report = {
            "schema": REPORT_SCHEMA, "version": REPORT_VERSION,
            "provenance": {"modelPath": str(model_path), "modelSha256": digest},
            "errors": [{"stage": "input", "message": str(error)}],
            "models": {}, "descriptor": {}, "diagnostics": {},
            "frequencyResponse": {}, "reductions": {},
        }
        try:
            atomic_write_json(output_path, failure_report)
        except AnalysisError as output_error:
            print(str(output_error), file=sys.stderr)
            return 2
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
