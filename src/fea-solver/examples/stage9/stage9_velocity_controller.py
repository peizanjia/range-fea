#!/usr/bin/env python3
"""Stage 9 collocated velocity feedback design and ControlNdjsonTransport v1 child.

Design an offline scalar nodal-force gain with::

    python stage9_velocity_controller.py --model linear_model.json \
        --target-damping 0.05 --sensor-id tip-v --actuator-id tip-force \
        --output velocity-gain.json

Run the full-FE controller as a v1 child with ``--gains velocity-gain.json``.
The gain is designed from the linear model; the child only applies the
requested physical force to an accepted velocity observation. Conditioning,
limits, and FE step acceptance remain owned by the solver.

Replay sampled-and-held Newmark feedback independently of the native solver
with::

    python stage9_velocity_controller.py --replay-model linear_model.json \
        --gains velocity-gain.json --dt 0.005 --steps 400 \
        --initial-q-json '[0.001,0,0]' --output replay.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, TextIO

import numpy as np
from scipy import linalg


PROTOCOL_VERSION = 1
GAIN_FORMAT = "range-fea-stage9-velocity-gain-v1"
MAX_MODEL_BYTES = 32 * 1024 * 1024
MAX_GAIN_BYTES = 1024 * 1024
MAX_REPLAY_BYTES = 8 * 1024 * 1024
MAX_PROTOCOL_LINE_BYTES = 1024 * 1024
MAX_DOF_COUNT = 256
MAX_REPLAY_STEPS = 5000
ZERO_TOL = 1.0e-12
COLLOCATION_RTOL = 1.0e-9


class ControllerError(ValueError):
    """Invalid model, gain artifact, or protocol request."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ControllerError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ControllerError(f"non-finite JSON number: {value}")


def _load_json_bytes(path: str | os.PathLike[str], maximum: int) -> tuple[dict[str, Any], bytes]:
    file_path = Path(path)
    try:
        size = file_path.stat().st_size
        if size <= 0 or size > maximum:
            raise ControllerError(f"JSON file size must be between 1 and {maximum} bytes")
        raw = file_path.read_bytes()
    except OSError as error:
        raise ControllerError(f"cannot read {file_path}: {error}") from error
    if len(raw) != size or len(raw) > maximum:
        raise ControllerError("JSON file changed while being read or exceeded its size limit")
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ControllerError(f"invalid UTF-8 JSON: {error}") from error
    if not isinstance(value, dict):
        raise ControllerError("JSON root must be an object")
    return value, raw


def _finite_number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ControllerError(f"{name} must be a finite number")
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        raise ControllerError(f"{name} must be a finite number") from error
    if not math.isfinite(result):
        raise ControllerError(f"{name} must be a finite number")
    return result


def _numeric_tree(value: Any) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, np.ndarray):
        return value.dtype.kind in "iuf" and bool(np.isfinite(value).all())
    if isinstance(value, (int, float)):
        try:
            return math.isfinite(float(value))
        except (OverflowError, ValueError):
            return False
    if isinstance(value, list):
        return all(_numeric_tree(item) for item in value)
    return False


def _matrix(model: dict[str, Any], name: str, shape: tuple[int, int]) -> np.ndarray:
    source = model.get(name)
    if not _numeric_tree(source):
        raise ControllerError(f"{name} must contain only finite JSON numbers")
    try:
        result = np.asarray(source, dtype=np.float64)
    except (KeyError, TypeError, ValueError, OverflowError) as error:
        raise ControllerError(f"{name} must be a numeric matrix") from error
    if result.shape != shape or not np.isfinite(result).all():
        raise ControllerError(f"{name} must have shape {shape} and finite entries")
    return result


def _zero_vector(vector: np.ndarray) -> bool:
    return float(np.linalg.norm(vector, ord=np.inf)) <= ZERO_TOL


def _check_symmetric(matrix: np.ndarray, name: str) -> None:
    if not np.allclose(matrix, matrix.T, rtol=1.0e-9, atol=1.0e-12):
        raise ControllerError(f"{name} must be symmetric")


def _check_positive_definite(matrix: np.ndarray, name: str) -> None:
    diagonal = np.diag(matrix)
    if not np.isfinite(diagonal).all() or np.any(diagonal <= 0.0):
        raise ControllerError(f"{name} must be positive definite")
    scale = np.sqrt(diagonal)
    equilibrated = matrix / np.outer(scale, scale)
    try:
        linalg.cholesky(equilibrated, lower=True, check_finite=False)
    except linalg.LinAlgError as error:
        raise ControllerError(f"{name} must be positive definite") from error


def _check_positive_semidefinite(matrix: np.ndarray, name: str) -> None:
    diagonal = np.diag(matrix)
    magnitude = max(float(np.max(np.abs(matrix))), np.finfo(np.float64).tiny)
    tolerance = 1.0e-12 * magnitude
    if np.any(diagonal < -tolerance):
        raise ControllerError(f"{name} must be positive semidefinite")
    active = diagonal > tolerance
    if np.any(~active):
        inactive_rows = matrix[~active, :]
        if np.any(np.abs(inactive_rows) > 1.0e-10 * magnitude):
            raise ControllerError(f"{name} must be positive semidefinite")
    if np.any(active):
        scale = np.sqrt(diagonal[active])
        equilibrated = matrix[np.ix_(active, active)] / np.outer(scale, scale)
        smallest = float(np.linalg.eigvalsh(equilibrated)[0])
        if smallest < -1.0e-10:
            raise ControllerError(f"{name} must be positive semidefinite")


def _valid_id(value: Any, field: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > 256 or
            "\n" in value or "\r" in value):
        raise ControllerError(f"{field} must be a non-empty ID of at most 256 characters")
    return value


def _valid_sha256(value: Any, field: str, optional: bool = False) -> str | None:
    if optional and value is None:
        return None
    if (not isinstance(value, str) or len(value) != 64 or
            any(char not in "0123456789abcdef" for char in value)):
        raise ControllerError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _catalog(model: dict[str, Any], key: str) -> list[dict[str, Any]]:
    value = model.get(key)
    if not isinstance(value, list) or not value or len(value) > 4096:
        raise ControllerError(f"{key} must be a non-empty array")
    result: list[dict[str, Any]] = []
    ids: set[str] = set()
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise ControllerError(f"{key}[{index}] must be an object")
        identifier = _valid_id(item.get("id"), f"{key}[{index}].id")
        if identifier in ids:
            raise ControllerError(f"duplicate {key} ID: {identifier}")
        ids.add(identifier)
        result.append(item)
    return result


def _validate_metadata(model: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], int]:
    version = model.get("version")
    if (model.get("schema") != "range-fea-linear-model" or
            isinstance(version, bool) or not isinstance(version, int) or version != 1):
        raise ControllerError("model must use the range-fea-linear-model version 1 schema")
    source = model.get("source")
    if (not isinstance(source, dict) or
            source.get("kind") not in ("linear-elastic-FE", "taut-cable-tangent-FE") or
            source.get("units") != "SI" or
            source.get("coordinateMeaning") != "perturbations about static equilibrium"):
        raise ControllerError("source must identify a supported SI linear FE model")

    dofs = model.get("dofs")
    if not isinstance(dofs, list) or not dofs or len(dofs) > MAX_DOF_COUNT:
        raise ControllerError(f"dofs must contain between 1 and {MAX_DOF_COUNT} entries")
    seen_dofs: set[tuple[str, str]] = set()
    for index, item in enumerate(dofs):
        if not isinstance(item, dict):
            raise ControllerError(f"dofs[{index}] must be an object")
        node = _valid_id(item.get("nodePhysicalId"), f"dofs[{index}].nodePhysicalId")
        dof = item.get("dof")
        if dof not in ("UX", "UY", "UZ", "RX", "RY", "RZ"):
            raise ControllerError(f"dofs[{index}].dof is unsupported")
        expected_unit = "m" if dof.startswith("U") else "rad"
        frame = item.get("frame")
        if item.get("unit") != expected_unit or frame not in ("global", "constraint-local"):
            raise ControllerError(f"dofs[{index}] must use its SI unit and a supported coordinate frame")
        try:
            raw_direction = item["globalDirection"]
            if not _numeric_tree(raw_direction):
                raise ControllerError(f"dofs[{index}].globalDirection must contain finite numbers")
            direction = np.asarray(raw_direction, dtype=np.float64)
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            raise ControllerError(f"dofs[{index}].globalDirection must be a finite unit vector") from error
        if (direction.shape != (3,) or not np.isfinite(direction).all() or
                not math.isclose(float(np.linalg.norm(direction)), 1.0, rel_tol=1.0e-9, abs_tol=1.0e-12)):
            raise ControllerError(f"dofs[{index}].globalDirection must be a finite normalized 3-vector")
        if frame == "global":
            expected_direction = np.zeros(3, dtype=np.float64)
            expected_direction[("UX", "UY", "UZ", "RX", "RY", "RZ").index(dof) % 3] = 1.0
            if not np.allclose(direction, expected_direction, rtol=0.0, atol=1.0e-9):
                raise ControllerError(f"dofs[{index}] globalDirection does not match its global DOF label")
        if frame == "global":
            expected_direction = np.zeros(3, dtype=np.float64)
            expected_direction[("UX", "UY", "UZ", "RX", "RY", "RZ").index(dof) % 3] = 1.0
            if not np.allclose(direction, expected_direction, rtol=0.0, atol=1.0e-9):
                raise ControllerError(f"dofs[{index}] globalDirection does not match its global DOF label")
        position = item.get("nodePosition")
        if isinstance(position, bool) or not isinstance(position, int) or position < 0:
            raise ControllerError(f"dofs[{index}].nodePosition must be a nonnegative integer")
        key = (node, dof)
        if key in seen_dofs:
            raise ControllerError(f"duplicate physical degree of freedom: {key}")
        seen_dofs.add(key)

    inputs = _catalog(model, "inputs")
    for index, item in enumerate(inputs):
        if item.get("unit") != "N":
            raise ControllerError(f"inputs[{index}] must be a translational force in N")
        _valid_id(item.get("nodePhysicalId"), f"inputs[{index}].nodePhysicalId")
        position = item.get("nodePosition")
        if isinstance(position, bool) or not isinstance(position, int) or position < 0:
            raise ControllerError(f"inputs[{index}].nodePosition must be a nonnegative integer")

    outputs = _catalog(model, "outputs")
    expected_units = {"Displacement": "m", "Velocity": "m/s", "Acceleration": "m/s^2"}
    for index, item in enumerate(outputs):
        quantity = item.get("quantity")
        if (not isinstance(quantity, str) or quantity not in expected_units or
                item.get("unit") != expected_units[quantity]):
            raise ControllerError(f"outputs[{index}] has an unsupported quantity/unit pair")
        _valid_id(item.get("nodePhysicalId"), f"outputs[{index}].nodePhysicalId")
        position = item.get("nodePosition")
        if isinstance(position, bool) or not isinstance(position, int) or position < 0:
            raise ControllerError(f"outputs[{index}].nodePosition must be a nonnegative integer")

    operating = model.get("operating_point")
    if not isinstance(operating, dict):
        raise ControllerError("operating_point is required")
    n = len(dofs)
    for vector_name in ("q", "v", "a", "force"):
        try:
            raw_vector = operating[vector_name]
            if not _numeric_tree(raw_vector):
                raise ControllerError(f"operating_point.{vector_name} must contain finite numbers")
            vector = np.asarray(raw_vector, dtype=np.float64)
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            raise ControllerError(f"operating_point.{vector_name} must be a finite vector") from error
        if vector.shape != (n,) or not np.isfinite(vector).all():
            raise ControllerError(f"operating_point.{vector_name} must have {n} finite entries")
    _finite_number(operating.get("residual"), "operating_point.residual")
    return inputs, outputs, n


def _json_bytes(value: Any) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=False, allow_nan=False).encode("utf-8")
    except (TypeError, ValueError, OverflowError) as error:
        raise ControllerError(f"value is not finite JSON: {error}") from error


def _complex_pairs(values: np.ndarray) -> list[list[float]]:
    ordered = sorted((complex(value) for value in values), key=lambda item: (item.real, item.imag))
    pairs = [[float(value.real), float(value.imag)] for value in ordered]
    if not all(math.isfinite(part) for pair in pairs for part in pair):
        raise ControllerError("pole calculation produced a non-finite value")
    return pairs


def design_gain(model_path: str | os.PathLike[str], *, mode: int, target_damping: float,
                sensor_id: str, actuator_id: str) -> dict[str, Any]:
    """Design a collocated velocity gain and verify the exact linear full-order poles."""
    model, raw = _load_json_bytes(model_path, MAX_MODEL_BYTES)
    inputs, outputs, n = _validate_metadata(model)
    if isinstance(mode, bool) or not isinstance(mode, int) or mode < 0:
        raise ControllerError("mode must be a nonnegative zero-based integer")
    target_damping = _finite_number(target_damping, "target damping ratio")
    if target_damping <= 0.0:
        raise ControllerError("target damping ratio must be finite and positive")
    try:
        actuator_index = next(i for i, item in enumerate(inputs) if item["id"] == actuator_id)
    except StopIteration as error:
        raise ControllerError(f"actuator ID is missing: {actuator_id}") from error
    try:
        sensor_index = next(i for i, item in enumerate(outputs) if item["id"] == sensor_id)
    except StopIteration as error:
        raise ControllerError(f"sensor ID is missing: {sensor_id}") from error

    actuator = inputs[actuator_index]
    sensor = outputs[sensor_index]
    if actuator["unit"] != "N":
        raise ControllerError("selected actuator must be a physical nodal force in N")
    if sensor.get("quantity") != "Velocity" or sensor.get("unit") != "m/s":
        raise ControllerError("selected sensor must be a physical velocity output in m/s")
    if (actuator["nodePhysicalId"] != sensor["nodePhysicalId"] or
            actuator["nodePosition"] != sensor["nodePosition"]):
        raise ControllerError("selected sensor and actuator are not at the same physical node")

    m_matrix = _matrix(model, "M", (n, n))
    c_matrix = _matrix(model, "C", (n, n))
    k_matrix = _matrix(model, "K", (n, n))
    b_matrix = _matrix(model, "B", (n, len(inputs)))
    sq = _matrix(model, "Sq", (len(outputs), n))
    sv = _matrix(model, "Sv", (len(outputs), n))
    sa = _matrix(model, "Sa", (len(outputs), n))
    d_matrix = _matrix(model, "D", (len(outputs), len(inputs)))
    _check_symmetric(m_matrix, "M")
    _check_symmetric(c_matrix, "C")
    _check_symmetric(k_matrix, "K")
    _check_positive_definite(m_matrix, "M")
    _check_positive_semidefinite(c_matrix, "C")
    _check_positive_definite(k_matrix, "K")

    velocity_row = sv[sensor_index, :]
    force_column = b_matrix[:, actuator_index]
    if (_zero_vector(sq[sensor_index, :]) is False or
            _zero_vector(sa[sensor_index, :]) is False or
            abs(float(d_matrix[sensor_index, actuator_index])) > ZERO_TOL):
        raise ControllerError("selected output must be pure velocity with no q/a/direct-force feedthrough")
    if (np.linalg.norm(velocity_row) <= ZERO_TOL or
            np.linalg.norm(force_column) <= ZERO_TOL or
            not np.allclose(velocity_row, force_column, rtol=COLLOCATION_RTOL, atol=ZERO_TOL)):
        raise ControllerError("selected velocity sensor and force actuator are not collocated with matching sign/scale")

    try:
        eigenvalues, modes = linalg.eigh(k_matrix, m_matrix, check_finite=False)
    except linalg.LinAlgError as error:
        raise ControllerError(f"generalized modal eigensolve failed: {error}") from error
    if mode >= n:
        raise ControllerError(f"mode {mode} is outside the available range 0..{n - 1}")
    eigenvalue = float(eigenvalues[mode])
    if not math.isfinite(eigenvalue) or eigenvalue <= 0.0:
        raise ControllerError("selected mode has zero or negative squared frequency")
    omega = math.sqrt(eigenvalue)
    phi = modes[:, mode]
    modal_b = float(phi @ force_column)
    if (not math.isfinite(modal_b) or
            abs(modal_b) <= ZERO_TOL * max(1.0, float(np.linalg.norm(phi) * np.linalg.norm(force_column)))):
        raise ControllerError("selected mode has zero actuator participation")
    modal_c = float(phi @ c_matrix @ phi)
    existing_zeta = modal_c / (2.0 * omega)
    if not math.isfinite(existing_zeta):
        raise ControllerError("selected mode has a non-finite damping ratio")
    gain = 2.0 * omega * (target_damping - existing_zeta) / (modal_b * modal_b)
    if not math.isfinite(gain) or gain <= 0.0:
        raise ControllerError("target requires a zero or negative velocity-feedback gain")
    predicted_zeta = existing_zeta + gain * modal_b * modal_b / (2.0 * omega)

    zero = np.zeros((n, n), dtype=np.float64)
    identity = np.eye(n, dtype=np.float64)
    try:
        m_inverse_k = linalg.solve(m_matrix, k_matrix, assume_a="pos", check_finite=False)
        m_inverse_c = linalg.solve(m_matrix, c_matrix, assume_a="pos", check_finite=False)
        m_inverse_b = linalg.solve(m_matrix, force_column, assume_a="pos", check_finite=False)
    except linalg.LinAlgError as error:
        raise ControllerError(f"full-order state matrix could not be formed: {error}") from error
    open_loop_a = np.block([[zero, identity], [-m_inverse_k, -m_inverse_c]])
    input_vector = np.concatenate((np.zeros(n, dtype=np.float64), m_inverse_b))
    output_row = np.concatenate((np.zeros(n, dtype=np.float64), velocity_row))
    closed_loop_a = open_loop_a - gain * np.outer(input_vector, output_row)
    try:
        full_poles = linalg.eigvals(closed_loop_a, check_finite=False)
    except linalg.LinAlgError as error:
        raise ControllerError(f"full-order closed-loop pole calculation failed: {error}") from error
    full_pairs = _complex_pairs(full_poles)
    spectral_abscissa = max(pair[0] for pair in full_pairs)
    if spectral_abscissa > 0.0:
        raise ControllerError(f"designed full-order feedback is unstable (spectral abscissa {spectral_abscissa:.9g})")

    rom_a = np.array([[0.0, 1.0], [-omega * omega, -2.0 * predicted_zeta * omega]], dtype=np.float64)
    rom_poles = linalg.eigvals(rom_a, check_finite=False)
    canonical_hash = hashlib.sha256(_json_bytes(model)).hexdigest()
    source = model.get("source", {})
    source_hash = source.get("sha256") if isinstance(source, dict) else None
    _valid_sha256(source_hash, "source.sha256", optional=True)

    return {
        "format": GAIN_FORMAT,
        "controllerLaw": "u=-g*y",
        "modelHashScope": "serialized-stage9-linear-model-file-bytes",
        "modelSha256": hashlib.sha256(raw).hexdigest(),
        "modelCanonicalSha256": canonical_hash,
        "sourceSha256": source_hash,
        "sourceHashScope": "native-fe-model-file-bytes" if source_hash is not None else None,
        "coordinateOrdering": "dofs-array-order",
        "coordinateOrdering": "dofs-array-order",
        "sensor": {"id": sensor_id, "quantity": "Velocity", "unit": "m/s"},
        "actuator": {"id": actuator_id, "quantity": "NodalForce", "unit": "N"},
        "gain": float(gain),
        "gainUnit": "N*s/m",
        "mode": {
            "index": mode,
            "eigenvalueRadPerSecondSquared": eigenvalue,
            "omegaRadPerSecond": omega,
            "modalForceParticipation": modal_b,
            "existingDampingRatio": existing_zeta,
            "requestedTargetDampingRatio": float(target_damping),
            "predictedSingleModeDampingRatio": float(predicted_zeta),
        },
        "reducedOrder": {
            "order": 2,
            "polesRealImaginary": _complex_pairs(rom_poles),
        },
        "fullOrderPoleCheck": {
            "order": 2 * n,
            "polesRealImaginary": full_pairs,
            "spectralAbscissaPerSecond": float(spectral_abscissa),
            "noPositiveGrowthVerified": bool(spectral_abscissa <= 0.0),
        },
        "claim": "The selected-mode damping target is a modal-decoupling estimate; the full-order rank-one poles are checked for positive growth.",
    }


def write_json_atomic(path: str | os.PathLike[str], value: dict[str, Any],
                      maximum: int = MAX_GAIN_BYTES) -> None:
    """Write bounded finite JSON using a same-directory atomic replace."""
    raw = _json_bytes(value) + b"\n"
    if len(raw) > maximum:
        raise ControllerError(f"JSON output exceeds {maximum} bytes")
    destination = Path(path)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile("wb", dir=destination.parent or Path("."),
                                         prefix=f".{destination.name}.", suffix=".tmp",
                                         delete=False) as stream:
            temporary_name = stream.name
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, destination)
        temporary_name = None
    except OSError as error:
        raise ControllerError(f"cannot atomically write {destination}: {error}") from error
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except OSError:
                pass


def _ensure_distinct_output(output: str | os.PathLike[str],
                            inputs: list[str | os.PathLike[str]]) -> None:
    destination = Path(output)
    try:
        resolved_destination = destination.resolve(strict=False)
    except OSError as error:
        raise ControllerError(f"cannot resolve output path: {error}") from error
    destination_key = os.path.normcase(os.path.abspath(str(resolved_destination)))
    for source in inputs:
        source_path = Path(source)
        try:
            resolved_source = source_path.resolve(strict=True)
        except OSError as error:
            raise ControllerError(f"cannot resolve input path {source_path}: {error}") from error
        source_key = os.path.normcase(os.path.abspath(str(resolved_source)))
        if destination_key == source_key:
            raise ControllerError("output path aliases an input file")
        try:
            if destination.exists() and os.path.samefile(destination, source_path):
                raise ControllerError("output path aliases an input file")
        except OSError as error:
            raise ControllerError(f"cannot compare output/input paths: {error}") from error


def _trajectory(model: dict[str, Any], q_initial: np.ndarray, v_initial: np.ndarray,
                *, force_column: np.ndarray, velocity_row: np.ndarray, gain: float,
                dt: float, steps: int, feedback: bool) -> dict[str, Any]:
    """Average-acceleration Newmark with accepted-state sampled, held force."""
    n = len(q_initial)
    m_matrix = np.asarray(model["M"], dtype=np.float64)
    c_matrix = np.asarray(model["C"], dtype=np.float64)
    k_matrix = np.asarray(model["K"], dtype=np.float64)
    beta, gamma = 0.25, 0.5
    try:
        mass_factor = linalg.cho_factor(m_matrix, lower=True, check_finite=False)
        effective = (k_matrix + gamma / (beta * dt) * c_matrix +
                     1.0 / (beta * dt * dt) * m_matrix)
        effective_factor = linalg.lu_factor(effective, check_finite=False)
    except linalg.LinAlgError as error:
        raise ControllerError(f"Newmark effective matrix factorization failed: {error}") from error

    q = q_initial.copy()
    v = v_initial.copy()
    q_history = [q.copy()]
    v_history = [v.copy()]
    a_history: list[np.ndarray] = []
    sensor_history = [float(velocity_row @ v)]
    force_history: list[float] = []
    energy_history: list[float] = []
    controller_work = 0.0
    for _ in range(steps):
        u = -gain * float(velocity_row @ v) if feedback else 0.0
        if not math.isfinite(u):
            raise ControllerError("Newmark controller produced a non-finite force")
        force = force_column * u
        acceleration = linalg.cho_solve(
            mass_factor, force - c_matrix @ v - k_matrix @ q, check_finite=False)
        q_predict = q + dt * v + dt * dt * (0.5 - beta) * acceleration
        v_predict = v + dt * (1.0 - gamma) * acceleration
        rhs = (force + m_matrix @ (q_predict / (beta * dt * dt)) +
               c_matrix @ (gamma / (beta * dt) * q_predict - v_predict))
        q_next = linalg.lu_solve(effective_factor, rhs, check_finite=False)
        a_next = (q_next - q_predict) / (beta * dt * dt)
        v_next = v_predict + gamma * dt * a_next
        if not (np.isfinite(q_next).all() and np.isfinite(v_next).all() and
                np.isfinite(a_next).all()):
            raise ControllerError("Newmark replay produced a non-finite state")
        controller_work += u * float(force_column @ (q_next - q))
        if not math.isfinite(controller_work):
            raise ControllerError("Newmark replay accumulated non-finite controller work")
        q, v = q_next, v_next
        q_history.append(q.copy())
        v_history.append(v.copy())
        a_history.append(a_next.copy())
        sensor_history.append(float(velocity_row @ v))
        force_history.append(float(u))
        energy_history.append(float(0.5 * (v @ m_matrix @ v + q @ k_matrix @ q)))
    # The t=0 acceleration is evaluated with the same force sample used on
    # the first interval, matching the accepted-state refresh contract.
    initial_u = -gain * float(velocity_row @ v_initial) if feedback else 0.0
    a_initial = linalg.cho_solve(
        mass_factor, force_column * initial_u - c_matrix @ v_initial - k_matrix @ q_initial,
        check_finite=False)
    if not np.isfinite(a_initial).all():
        raise ControllerError("Newmark initial acceleration is non-finite")
    a_history.insert(0, a_initial)
    energy_history.insert(0, float(0.5 * (v_initial @ m_matrix @ v_initial + q_initial @ k_matrix @ q_initial)))
    return {"q": q_history, "v": v_history, "a": a_history,
            "sensorVelocity": sensor_history, "heldForce": force_history,
            "energy": energy_history, "controllerWork": float(controller_work)}


def _energy(model: dict[str, Any], q: np.ndarray, v: np.ndarray) -> float:
    m_matrix = np.asarray(model["M"], dtype=np.float64)
    k_matrix = np.asarray(model["K"], dtype=np.float64)
    value = 0.5 * float(v @ m_matrix @ v + q @ k_matrix @ q)
    if not math.isfinite(value):
        raise ControllerError("replay energy is non-finite")
    return value


def replay(model_path: str | os.PathLike[str], gains_path: str | os.PathLike[str], *,
           dt: float, steps: int, initial_q: list[float] | np.ndarray) -> dict[str, Any]:
    """Compare sampled/held full-order Newmark feedback with a single-mode ROM."""
    dt = _finite_number(dt, "dt")
    if dt <= 0.0:
        raise ControllerError("dt must be positive")
    if isinstance(steps, bool) or not isinstance(steps, int) or not 1 <= steps <= MAX_REPLAY_STEPS:
        raise ControllerError(f"steps must be between 1 and {MAX_REPLAY_STEPS}")
    model, raw = _load_json_bytes(model_path, MAX_MODEL_BYTES)
    gains, _ = _load_json_bytes(gains_path, MAX_GAIN_BYTES)
    runtime = VelocityControllerRuntime(gains)
    mode_data = gains.get("mode")
    if not isinstance(mode_data, dict):
        raise ControllerError("gain artifact has no selected-mode design data")
    mode_index = mode_data.get("index")
    target_damping = _finite_number(mode_data.get("requestedTargetDampingRatio"),
                                    "gain artifact target damping ratio")
    expected = design_gain(model_path, mode=mode_index,
                           target_damping=target_damping,
                           sensor_id=runtime.sensor_id, actuator_id=runtime.actuator_id)
    if (gains.get("modelSha256") != hashlib.sha256(raw).hexdigest() or
            gains.get("sourceSha256") != expected["sourceSha256"] or
            not math.isclose(runtime.gain, expected["gain"], rel_tol=1.0e-12, abs_tol=1.0e-15)):
        raise ControllerError("gain artifact does not match the replay model and design")
    inputs, outputs, n = _validate_metadata(model)
    actuator_index = next(i for i, item in enumerate(inputs) if item["id"] == runtime.actuator_id)
    sensor_index = next(i for i, item in enumerate(outputs) if item["id"] == runtime.sensor_id)
    m_matrix = _matrix(model, "M", (n, n))
    c_matrix = _matrix(model, "C", (n, n))
    k_matrix = _matrix(model, "K", (n, n))
    b_matrix = _matrix(model, "B", (n, len(inputs)))
    sv = _matrix(model, "Sv", (len(outputs), n))
    if not _numeric_tree(initial_q):
        raise ControllerError("initial_q must contain only finite JSON numbers")
    try:
        q0 = np.asarray(initial_q, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as error:
        raise ControllerError("initial_q must be a finite vector") from error
    if q0.shape != (n,) or not np.isfinite(q0).all():
        raise ControllerError(f"initial_q must contain {n} finite entries")
    v0 = np.zeros(n, dtype=np.float64)
    phi_values, phi_vectors = linalg.eigh(k_matrix, m_matrix, check_finite=False)
    if isinstance(mode_index, bool) or not isinstance(mode_index, int) or not 0 <= mode_index < n:
        raise ControllerError("gain artifact contains an invalid mode index")
    phi = phi_vectors[:, mode_index]
    omega = math.sqrt(float(phi_values[mode_index]))
    modal_mass = float(phi @ m_matrix @ phi)
    if not math.isclose(modal_mass, 1.0, rel_tol=1.0e-9, abs_tol=1.0e-11):
        raise ControllerError("selected replay mode is not mass normalized")
    force_column = b_matrix[:, actuator_index]
    velocity_row = sv[sensor_index, :]
    full_controlled = _trajectory(model, q0, v0, force_column=force_column,
                                  velocity_row=velocity_row, gain=runtime.gain,
                                  dt=dt, steps=steps, feedback=True)
    full_passive = _trajectory(model, q0, v0, force_column=force_column,
                               velocity_row=velocity_row, gain=0.0,
                               dt=dt, steps=steps, feedback=False)

    modal_c = float(phi @ c_matrix @ phi)
    modal_b = float(phi @ force_column)
    modal_sensor = float(velocity_row @ phi)
    q_modal0 = float(phi @ m_matrix @ q0)
    v_modal0 = float(phi @ m_matrix @ v0)
    rom = {"M": [[1.0]], "C": [[modal_c]], "K": [[omega * omega]]}
    reduced = _trajectory(rom, np.array([q_modal0]), np.array([v_modal0]),
                          force_column=np.array([modal_b]),
                          velocity_row=np.array([modal_sensor]), gain=runtime.gain,
                          dt=dt, steps=steps, feedback=True)

    q_abs: list[float] = []
    q_rel: list[float] = []
    v_abs: list[float] = []
    v_rel: list[float] = []
    a_abs: list[float] = []
    a_rel: list[float] = []
    q_traj_abs: list[float] = []
    q_traj_rel: list[float] = []
    v_traj_abs: list[float] = []
    v_traj_rel: list[float] = []
    a_traj_abs: list[float] = []
    a_traj_rel: list[float] = []
    sensor_error_abs: list[float] = []
    sensor_error_rel: list[float] = []
    reduced_sensor: list[float] = []
    for index, (q_full, v_full, a_full) in enumerate(
            zip(full_controlled["q"], full_controlled["v"], full_controlled["a"])):
        q_projection = phi * float(phi @ m_matrix @ q_full)
        v_projection = phi * float(phi @ m_matrix @ v_full)
        a_projection = phi * float(phi @ m_matrix @ a_full)
        q_error = float(np.linalg.norm(q_full - q_projection))
        v_error = float(np.linalg.norm(v_full - v_projection))
        a_error = float(np.linalg.norm(a_full - a_projection))
        q_abs.append(q_error)
        q_rel.append(q_error / max(float(np.linalg.norm(q_full)), 1.0e-12))
        v_abs.append(v_error)
        v_rel.append(v_error / max(float(np.linalg.norm(v_full)), 1.0e-12))
        a_abs.append(a_error)
        a_rel.append(a_error / max(float(np.linalg.norm(a_full)), 1.0e-12))
        q_rom = phi * float(reduced["q"][index][0])
        v_rom = phi * float(reduced["v"][index][0])
        a_rom = phi * float(reduced["a"][index][0])
        q_error = float(np.linalg.norm(q_full - q_rom))
        v_error = float(np.linalg.norm(v_full - v_rom))
        a_error = float(np.linalg.norm(a_full - a_rom))
        q_traj_abs.append(q_error)
        q_traj_rel.append(q_error / max(float(np.linalg.norm(q_full)), 1.0e-12))
        v_traj_abs.append(v_error)
        v_traj_rel.append(v_error / max(float(np.linalg.norm(v_full)), 1.0e-12))
        a_traj_abs.append(a_error)
        a_traj_rel.append(a_error / max(float(np.linalg.norm(a_full)), 1.0e-12))
        rom_sensor = float(modal_sensor * reduced["v"][index][0])
        reduced_sensor.append(rom_sensor)
        measurement_error = float(velocity_row @ v_full) - rom_sensor
        sensor_error_abs.append(abs(measurement_error))
        sensor_error_rel.append(abs(measurement_error) /
                                max(abs(float(velocity_row @ v_full)), 1.0e-12))

    times = [float(index * dt) for index in range(steps + 1)]
    result = {
        "format": "range-fea-stage9-newmark-replay-v1",
        "modelSha256": hashlib.sha256(raw).hexdigest(),
        "sensorId": runtime.sensor_id, "actuatorId": runtime.actuator_id,
        "gain": runtime.gain, "dt": dt, "steps": steps, "duration": float(steps * dt),
        "initialQ": q0.tolist(), "initialV": v0.tolist(),
        "fullOrder": {"q": full_controlled["q"][-1].tolist(),
                      "v": full_controlled["v"][-1].tolist(),
                      "a": full_controlled["a"][-1].tolist(),
                      "energy": _energy(model, full_controlled["q"][-1], full_controlled["v"][-1]),
                      "controllerWork": full_controlled["controllerWork"]},
        "passive": {"energy": _energy(model, full_passive["q"][-1], full_passive["v"][-1])},
        "passiveFullOrder": {"q": full_passive["q"][-1].tolist(),
                             "v": full_passive["v"][-1].tolist(),
                             "a": full_passive["a"][-1].tolist()},
        "reducedOrder": {"modeIndex": mode_index,
                         "q": (phi * reduced["q"][-1][0]).tolist(),
                         "v": (phi * reduced["v"][-1][0]).tolist(),
                         "a": (phi * reduced["a"][-1][0]).tolist(),
                         "modalQ": float(reduced["q"][-1][0]),
                         "modalV": float(reduced["v"][-1][0]),
                         "modalA": float(reduced["a"][-1][0])},
        "projectionError": {"qAbsoluteNorm": q_abs, "qRelativeNorm": q_rel,
                            "vAbsoluteNorm": v_abs, "vRelativeNorm": v_rel,
                            "aAbsoluteNorm": a_abs, "aRelativeNorm": a_rel,
                            "relativeDenominatorFloor": 1.0e-12},
        "trajectoryError": {"qAbsoluteNorm": q_traj_abs, "qRelativeNorm": q_traj_rel,
                            "vAbsoluteNorm": v_traj_abs, "vRelativeNorm": v_traj_rel,
                            "aAbsoluteNorm": a_traj_abs, "aRelativeNorm": a_traj_rel,
                            "sensorVelocityAbsolute": sensor_error_abs,
                            "sensorVelocityRelative": sensor_error_rel,
                            "relativeDenominatorFloor": 1.0e-12},
        "timeOutputs": {"timeSeconds": times,
                        "fullVelocityMPerSecond": full_controlled["sensorVelocity"],
                        "romVelocityMPerSecond": reduced_sensor,
                        "requestedForceN": full_controlled["heldForce"]},
        "energiesJ": {"fullControlled": _energy(model, full_controlled["q"][-1], full_controlled["v"][-1]),
                      "fullPassive": _energy(model, full_passive["q"][-1], full_passive["v"][-1]),
                      "romControlled": float(0.5 * (reduced["v"][-1][0] ** 2 +
                                                    omega * omega * reduced["q"][-1][0] ** 2)),
                      "fullControlledHistory": full_controlled["energy"],
                      "fullPassiveHistory": full_passive["energy"]},
        "controllerWorkJ": full_controlled["controllerWork"],
        "integrator": "Newmark average acceleration (beta=1/4,gamma=1/2), force sampled at accepted velocity and held over each interval",
    }
    # Keep the report serializable and bounded before any filesystem publication.
    encoded = _json_bytes(result)
    if len(encoded) > MAX_REPLAY_BYTES:
        raise ControllerError(f"replay report exceeds {MAX_REPLAY_BYTES} bytes")
    return result


def _step_number(value: Any) -> int:
    if (not isinstance(value, str) or len(value) > 20 or
            not value.isascii() or not value.isdecimal()):
        raise ControllerError("stepId must be an unsigned decimal string")
    result = int(value)
    if result > 0xFFFFFFFFFFFFFFFF:
        raise ControllerError("stepId exceeds the v1 unsigned 64-bit range")
    return result


class VelocityControllerRuntime:
    """Pure v1 compute function; only handshake identity is process-local."""

    def __init__(self, gains: dict[str, Any]):
        if gains.get("format") != GAIN_FORMAT or gains.get("controllerLaw") != "u=-g*y":
            raise ControllerError("unsupported Stage 9 gain artifact")
        _valid_sha256(gains.get("modelSha256"), "gain.modelSha256")
        _valid_sha256(gains.get("modelCanonicalSha256"), "gain.modelCanonicalSha256")
        sensor = gains.get("sensor")
        actuator = gains.get("actuator")
        if not isinstance(sensor, dict) or not isinstance(actuator, dict):
            raise ControllerError("gain artifact has no sensor/actuator metadata")
        self.sensor_id = _valid_id(sensor.get("id"), "gain.sensor.id")
        self.actuator_id = _valid_id(actuator.get("id"), "gain.actuator.id")
        if (sensor.get("quantity") != "Velocity" or sensor.get("unit") != "m/s" or
                actuator.get("quantity") != "NodalForce" or actuator.get("unit") != "N"):
            raise ControllerError("gain artifact must bind one m/s velocity output to one N actuator")
        self.gain = _finite_number(gains.get("gain"), "gain")
        if self.gain <= 0.0 or gains.get("gainUnit") != "N*s/m":
            raise ControllerError("gain artifact must contain a positive N*s/m gain")
        mode = gains.get("mode")
        if not isinstance(mode, dict):
            raise ControllerError("gain artifact has no modal design metadata")
        mode_index = mode.get("index")
        if isinstance(mode_index, bool) or not isinstance(mode_index, int) or mode_index < 0:
            raise ControllerError("gain artifact has an invalid mode index")
        omega = _finite_number(mode.get("omegaRadPerSecond"), "mode omega")
        modal_b = _finite_number(mode.get("modalForceParticipation"), "modal force participation")
        existing_zeta = _finite_number(mode.get("existingDampingRatio"), "existing damping ratio")
        target_zeta = _finite_number(mode.get("requestedTargetDampingRatio"), "target damping ratio")
        predicted_zeta = _finite_number(mode.get("predictedSingleModeDampingRatio"),
                                        "predicted damping ratio")
        if omega <= 0.0 or abs(modal_b) <= ZERO_TOL or target_zeta <= 0.0:
            raise ControllerError("gain artifact has invalid modal design values")
        formula_gain = 2.0 * omega * (target_zeta - existing_zeta) / (modal_b * modal_b)
        formula_zeta = existing_zeta + self.gain * modal_b * modal_b / (2.0 * omega)
        if (not math.isclose(self.gain, formula_gain, rel_tol=1.0e-10, abs_tol=1.0e-14) or
                not math.isclose(predicted_zeta, formula_zeta, rel_tol=1.0e-10, abs_tol=1.0e-12)):
            raise ControllerError("gain artifact modal values do not match its design formula")
        reduced = gains.get("reducedOrder")
        if not isinstance(reduced, dict) or reduced.get("order") != 2:
            raise ControllerError("gain artifact is missing its second-order ROM pole report")
        reduced_poles = reduced.get("polesRealImaginary")
        if not isinstance(reduced_poles, list) or len(reduced_poles) != 2:
            raise ControllerError("gain artifact has an invalid ROM pole report")
        for pole in reduced_poles:
            if not isinstance(pole, list) or len(pole) != 2:
                raise ControllerError("gain artifact has an invalid ROM pole report")
            _finite_number(pole[0], "ROM pole real part")
            _finite_number(pole[1], "ROM pole imaginary part")
        full = gains.get("fullOrderPoleCheck")
        if not isinstance(full, dict):
            raise ControllerError("gain artifact has no full-order pole check")
        full_order = full.get("order")
        full_poles = full.get("polesRealImaginary")
        if (isinstance(full_order, bool) or not isinstance(full_order, int) or
                not 2 <= full_order <= 2 * MAX_DOF_COUNT or not isinstance(full_poles, list) or
                len(full_poles) != full_order):
            raise ControllerError("gain artifact has an invalid full-order pole report")
        for pole in full_poles:
            if not isinstance(pole, list) or len(pole) != 2:
                raise ControllerError("gain artifact has an invalid full-order pole report")
            _finite_number(pole[0], "full-order pole real part")
            _finite_number(pole[1], "full-order pole imaginary part")
        pole_abscissa = max(float(pole[0]) for pole in full_poles)
        reported_abscissa = _finite_number(full.get("spectralAbscissaPerSecond"),
                                           "full-order spectral abscissa")
        if (reported_abscissa > 0.0 or
                not math.isclose(reported_abscissa, pole_abscissa, rel_tol=1.0e-10, abs_tol=1.0e-12) or
                full.get("noPositiveGrowthVerified") is not True):
            raise ControllerError("gain artifact does not verify stable full-order poles")
        self.source_sha256 = gains.get("sourceSha256")
        _valid_sha256(self.source_sha256, "gain.sourceSha256", optional=True)
        if (self.source_sha256 is not None and
                gains.get("sourceHashScope") != "native-fe-model-file-bytes"):
            raise ControllerError("gain artifact does not identify the native FE source hash scope")
        self.identity: dict[str, str] | None = None
        self.sensor_units: dict[str, str] = {}

    def handle(self, request: dict[str, Any]) -> dict[str, Any]:
        version = request.get("protocolVersion")
        request_id = request.get("requestId")
        op = request.get("op")
        if (isinstance(version, bool) or not isinstance(version, int) or
                version != PROTOCOL_VERSION or not isinstance(request_id, str) or not request_id):
            raise ControllerError("unsupported protocol version or missing requestId")
        if len(request_id) > 256 or "\n" in request_id or "\r" in request_id:
            raise ControllerError("invalid requestId")
        base = {"protocolVersion": PROTOCOL_VERSION, "requestId": request_id, "op": op}
        if op == "handshake":
            return self._handshake(request, base)
        if op == "compute":
            return self._compute(request, base)
        raise ControllerError("unknown operation")

    def _handshake(self, request: dict[str, Any], base: dict[str, Any]) -> dict[str, Any]:
        capabilities = request.get("capabilities")
        if not isinstance(capabilities, dict) or capabilities.get("pureStep") is not True or \
                capabilities.get("controlMode") != "scalar-nodal-force":
            raise ControllerError("unsupported v1 control mode")
        sensors = capabilities.get("sensors")
        actuators = capabilities.get("actuatorIds")
        if not isinstance(sensors, list) or not 1 <= len(sensors) <= 64:
            raise ControllerError("handshake sensor catalog must contain 1..64 entries")
        sensor_units: dict[str, str] = {}
        valid_units = {"m", "m/s", "m/s^2"}
        for item in sensors:
            if not isinstance(item, dict):
                raise ControllerError("handshake sensor entries must be objects")
            sensor_id = _valid_id(item.get("sensorId"), "sensorId")
            unit = item.get("unit")
            if not isinstance(unit, str) or unit not in valid_units or sensor_id in sensor_units:
                raise ControllerError("handshake has a duplicate sensor ID or unsupported sensor unit")
            sensor_units[sensor_id] = unit
        if sensor_units.get(self.sensor_id) != "m/s":
            raise ControllerError("handshake must offer the configured velocity sensor in m/s")
        if not isinstance(actuators, list) or len(actuators) != 1 or actuators[0] != self.actuator_id:
            raise ControllerError("handshake must offer exactly the configured NodalForce actuator")
        controller_id = _valid_id(request.get("controllerId"), "controllerId")
        revision = request.get("controllerRevision")
        model_hash = request.get("modelSnapshotSha256")
        seed = request.get("deterministicSeed")
        for field, value in (("controllerRevision", revision), ("modelSnapshotSha256", model_hash)):
            if (not isinstance(value, str) or len(value) != 64 or
                    any(char not in "0123456789abcdef" for char in value)):
                raise ControllerError(f"{field} must be a lowercase SHA-256 digest")
        if (not isinstance(seed, str) or len(seed) > 20 or not seed.isascii() or
                not seed.isdecimal() or int(seed) > 0xFFFFFFFFFFFFFFFF):
            raise ControllerError("deterministicSeed must be an unsigned decimal string")
        if self.source_sha256 is not None and model_hash != self.source_sha256:
            raise ControllerError("native FE model digest does not match the gain artifact source hash")
        self.identity = {"controllerId": controller_id, "controllerRevision": revision,
                         "modelSnapshotSha256": model_hash, "deterministicSeed": seed}
        self.sensor_units = sensor_units
        return {**base, "ok": True,
                "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                                 "actuatorIds": [self.actuator_id]},
                **self.identity}

    def _compute(self, request: dict[str, Any], base: dict[str, Any]) -> dict[str, Any]:
        if self.identity is None:
            raise ControllerError("handshake is required before compute")
        for key, value in self.identity.items():
            if request.get(key) != value:
                raise ControllerError(f"compute identity mismatch: {key}")
        step_text = request.get("stepId")
        step = _step_number(step_text)
        time = _finite_number(request.get("time"), "time")
        dt = _finite_number(request.get("dt"), "dt")
        if time < 0.0 or dt <= 0.0:
            raise ControllerError("time must be nonnegative and dt positive")
        observations = request.get("observations")
        if (not isinstance(observations, list) or len(observations) != len(self.sensor_units) or
                len(observations) > 64):
            raise ControllerError("compute observation catalog is incomplete or oversized")
        observed: dict[str, float] = {}
        for observation in observations:
            if not isinstance(observation, dict):
                raise ControllerError("compute observations must be objects")
            sensor_id = observation.get("sensorId")
            if (not isinstance(sensor_id, str) or sensor_id not in self.sensor_units or
                    sensor_id in observed or observation.get("unit") != self.sensor_units[sensor_id]):
                raise ControllerError("observation ID/unit does not match the handshaken sensor catalog")
            observed[sensor_id] = _finite_number(observation.get("value"),
                                                 f"observation {sensor_id}")
        if set(observed) != set(self.sensor_units):
            raise ControllerError("compute observation IDs do not match the handshaken sensor catalog")
        velocity = observed[self.sensor_id]
        state = request.get("acceptedControllerState")
        if not isinstance(state, dict) or set(state) - {"sampleCount", "lastStepId"}:
            raise ControllerError("acceptedControllerState has an unsupported shape")
        count = state.get("sampleCount", 0)
        if isinstance(count, bool) or not isinstance(count, int) or count < 0 or count >= 0x7FFFFFFFFFFFFFFF:
            raise ControllerError("acceptedControllerState.sampleCount must be a nonnegative bounded integer")
        previous_step = state.get("lastStepId")
        if count == 0:
            if previous_step is not None:
                raise ControllerError("fresh controller state cannot contain lastStepId")
        else:
            previous = _step_number(previous_step)
            if step <= previous:
                raise ControllerError("stepId must advance beyond the last accepted controller step")
        force = -self.gain * velocity
        if not math.isfinite(force):
            raise ControllerError("requested force is non-finite")
        next_state = {"sampleCount": count + 1, "lastStepId": step_text}
        return {**base, "stepId": step_text, **self.identity,
                "commands": [{"actuatorId": self.actuator_id, "force": force, "unit": "N"}],
                "nextControllerState": next_state}


def _serve(gains_path: str, stdin: TextIO = sys.stdin, stdout: TextIO = sys.stdout,
           stderr: TextIO = sys.stderr) -> int:
    try:
        gains, _ = _load_json_bytes(gains_path, MAX_GAIN_BYTES)
        controller = VelocityControllerRuntime(gains)
    except ControllerError as error:
        print(f"Stage 9 velocity controller: {error}", file=stderr, flush=True)
        return 2
    while True:
        line = stdin.readline(MAX_PROTOCOL_LINE_BYTES + 1)
        if line == "":
            break
        if len(line.encode("utf-8")) > MAX_PROTOCOL_LINE_BYTES:
            print("Stage 9 velocity controller: request exceeds the line limit", file=stderr, flush=True)
            return 2
        try:
            request = json.loads(line, object_pairs_hook=_unique_object, parse_constant=_reject_constant)
            if not isinstance(request, dict):
                raise ControllerError("request must be a JSON object")
            response = controller.handle(request)
            encoded = _json_bytes(response) + b"\n"
            if len(encoded) > MAX_PROTOCOL_LINE_BYTES:
                raise ControllerError("response exceeds the line limit")
            stdout.write(encoded.decode("utf-8"))
            stdout.flush()
        except (ControllerError, json.JSONDecodeError, UnicodeError) as error:
            print(f"Stage 9 velocity controller rejected request: {error}", file=stderr, flush=True)
            return 2
    return 0


def _nonnegative_int(value: str) -> int:
    try:
        result = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a nonnegative integer") from error
    if result < 0:
        raise argparse.ArgumentTypeError("must be a nonnegative integer")
    return result


def _initial_q_json(value: str) -> list[float]:
    if len(value.encode("utf-8")) > 65536:
        raise argparse.ArgumentTypeError("initial q JSON exceeds 64 KiB")
    try:
        decoded = json.loads(value, parse_constant=_reject_constant)
        if not _numeric_tree(decoded):
            raise ControllerError("vector entries must be finite JSON numbers")
        vector = np.asarray(decoded, dtype=np.float64)
    except (json.JSONDecodeError, ControllerError, TypeError, ValueError, OverflowError) as error:
        raise argparse.ArgumentTypeError(f"must be a finite JSON vector: {error}") from error
    if vector.ndim != 1 or not np.isfinite(vector).all():
        raise argparse.ArgumentTypeError("must be a finite JSON vector")
    return vector.tolist()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", help="offline Stage 9 linear FE model JSON")
    parser.add_argument("--replay-model", help="linear FE model JSON for held-force Newmark replay")
    parser.add_argument("--gains", help="gain artifact for v1 runtime or replay")
    parser.add_argument("--mode", type=_nonnegative_int, default=0, help="zero-based mode index (default: 0)")
    parser.add_argument("--target-damping", type=float, help="requested selected-mode damping ratio")
    parser.add_argument("--sensor-id", help="selected physical velocity output ID")
    parser.add_argument("--actuator-id", help="selected physical nodal-force input ID")
    parser.add_argument("--dt", type=float, help="Newmark replay time step in seconds")
    parser.add_argument("--steps", type=_nonnegative_int, help="Newmark replay interval count")
    parser.add_argument("--initial-q-json", type=_initial_q_json,
                        help='initial perturbation displacement JSON vector, e.g. "[0.001,0,0]"')
    parser.add_argument("--output", help="atomic output path for the gain artifact")
    args = parser.parse_args(argv)
    try:
        if args.model:
            if (args.replay_model is not None or args.gains is not None or args.dt is not None or
                    args.steps is not None or args.initial_q_json is not None):
                parser.error("--model design mode cannot be combined with replay/runtime options")
            if (args.target_damping is None or args.sensor_id is None or
                    args.actuator_id is None or args.output is None):
                parser.error("--model requires --target-damping, --sensor-id, --actuator-id, and --output")
            _ensure_distinct_output(args.output, [args.model])
            gains = design_gain(args.model, mode=args.mode, target_damping=args.target_damping,
                                sensor_id=args.sensor_id, actuator_id=args.actuator_id)
            write_json_atomic(args.output, gains)
            return 0
        if args.replay_model is not None:
            if args.gains is None or args.dt is None or args.steps is None or \
                    args.initial_q_json is None or args.output is None:
                parser.error("--replay-model requires --gains, --dt, --steps, --initial-q-json, and --output")
            if any(value is not None for value in (args.target_damping, args.sensor_id, args.actuator_id)):
                parser.error("design options cannot be combined with replay")
            _ensure_distinct_output(args.output, [args.replay_model, args.gains])
            report = replay(args.replay_model, args.gains, dt=args.dt, steps=args.steps,
                            initial_q=args.initial_q_json)
            write_json_atomic(args.output, report, maximum=MAX_REPLAY_BYTES)
            return 0
        if args.gains is None:
            parser.error("select --model, --replay-model, or --gains")
        if any(value is not None for value in
               (args.target_damping, args.sensor_id, args.actuator_id, args.output,
                args.dt, args.steps, args.initial_q_json)) or args.mode != 0:
            parser.error("design/replay options are only valid with --model or --replay-model")
        return _serve(args.gains)
    except ControllerError as error:
        print(f"Stage 9 velocity controller: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
