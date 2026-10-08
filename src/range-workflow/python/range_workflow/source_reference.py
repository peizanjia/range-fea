"""Independent WF-03 matrices and initial-state projection for imported sources."""
from __future__ import annotations

import math

import numpy as np


class SourceReferenceError(ValueError):
    """The imported descriptor or native linear export is not referenceable."""


def _fail(message: str) -> None:
    raise SourceReferenceError(message)


def _number(value, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail(f"{label} must be a finite number")
    try:
        result = float(value)
    except (OverflowError, ValueError):
        _fail(f"{label} must be a finite number")
    if not math.isfinite(result):
        _fail(f"{label} must be a finite number")
    return result


def _vector(value, label: str) -> np.ndarray:
    try:
        result = np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError, OverflowError):
        _fail(f"{label} must be a finite three-component vector")
    if result.shape != (3,) or not np.isfinite(result).all():
        _fail(f"{label} must be a finite three-component vector")
    return result


def _matrix(value, label: str, size: int) -> np.ndarray:
    try:
        result = np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError, OverflowError):
        _fail(f"{label} must be a finite square matrix")
    if result.shape != (size, size) or not np.isfinite(result).all():
        _fail(f"{label} must be a finite {size} by {size} matrix")
    return result


def _global_axis(value, label: str) -> np.ndarray:
    direction = _vector(value, label)
    if not math.isclose(float(np.linalg.norm(direction)), 1.0, rel_tol=1.0e-9, abs_tol=1.0e-12):
        _fail(f"{label} must be a normalized global axis")
    axis = int(np.argmax(np.abs(direction)))
    expected = np.zeros(3, dtype=np.float64)
    expected[axis] = math.copysign(1.0, direction[axis])
    if not np.allclose(direction, expected, rtol=0.0, atol=1.0e-9):
        _fail(f"{label} must be parallel to one global axis")
    return expected


def _physical_key(node_id, direction, unit: str) -> tuple[str, tuple[float, float, float], str]:
    axis = _global_axis(direction, "globalDirection")
    return str(node_id), tuple(float(value) for value in axis), unit


def _add_projected(matrix: np.ndarray, indices: list[int | None], coefficients: list[float],
                   local: np.ndarray) -> None:
    for row, row_index in enumerate(indices):
        if row_index is None:
            continue
        for column, column_index in enumerate(indices):
            if column_index is not None:
                matrix[row_index, column_index] += (coefficients[row] * local[row, column] *
                                                    coefficients[column])


def _assemble_rod(descriptor: dict, dofs: list[dict], key_to_index: dict) -> tuple[np.ndarray, np.ndarray]:
    size = len(dofs)
    mass = np.zeros((size, size), dtype=np.float64)
    stiffness = np.zeros((size, size), dtype=np.float64)
    nodes = {str(node["physicalId"]): node for node in descriptor["nodes"]}
    for element_index, element in enumerate(descriptor["elements"]):
        node_ids = element.get("nodePhysicalIds")
        if not isinstance(node_ids, list) or len(node_ids) != 2:
            _fail(f"elements[{element_index}] must connect two nodes")
        endpoint_ids = [str(value) for value in node_ids]
        if any(identifier not in nodes for identifier in endpoint_ids):
            _fail(f"elements[{element_index}] references an unknown node")
        p0 = _vector(nodes[endpoint_ids[0]]["coordinates"], "node coordinates")
        p1 = _vector(nodes[endpoint_ids[1]]["coordinates"], "node coordinates")
        delta = p1 - p0
        length = float(np.linalg.norm(delta))
        if not math.isfinite(length) or length <= 0.0:
            _fail(f"elements[{element_index}] has a nonpositive length")
        axis = delta / length
        if not np.allclose(np.abs(axis), [1.0, 0.0, 0.0], rtol=0.0, atol=1.0e-9):
            _fail("the rod reference requires each element to lie on global X")
        material = element.get("material", {})
        section = element.get("section", {})
        young = _number(material.get("youngModulusPa"), "element Young's modulus")
        density = _number(material.get("densityKgM3"), "element density")
        area = _number(section.get("areaM2"), "element area")
        if young <= 0.0 or density <= 0.0 or area <= 0.0:
            _fail("rod material and area values must be positive")
        local_k = young * area / length * np.asarray([[1.0, -1.0], [-1.0, 1.0]])
        local_m = density * area * length / 6.0 * np.asarray([[2.0, 1.0], [1.0, 2.0]])
        indices: list[int | None] = []
        coefficients: list[float] = []
        for node_id in endpoint_ids:
            key = _physical_key(node_id, [1.0, 0.0, 0.0], "m")
            indices.append(key_to_index.get(key))
            coefficients.append(float(axis[0]))
        _add_projected(stiffness, indices, coefficients, local_k)
        _add_projected(mass, indices, coefficients, local_m)
    return mass, stiffness


def _assemble_beam(descriptor: dict, dofs: list[dict], key_to_index: dict) -> tuple[np.ndarray, np.ndarray]:
    size = len(dofs)
    mass = np.zeros((size, size), dtype=np.float64)
    stiffness = np.zeros((size, size), dtype=np.float64)
    nodes = {str(node["physicalId"]): node for node in descriptor["nodes"]}
    element = descriptor["elements"][0]
    node_ids = [str(value) for value in element["nodePhysicalIds"]]
    p0 = _vector(nodes[node_ids[0]]["coordinates"], "node coordinates")
    p1 = _vector(nodes[node_ids[1]]["coordinates"], "node coordinates")
    delta = p1 - p0
    length = float(np.linalg.norm(delta))
    if not math.isfinite(length) or length <= 0.0:
        _fail("beam element has a nonpositive length")
    ex = delta / length
    if not np.allclose(ex, [1.0, 0.0, 0.0], rtol=0.0, atol=1.0e-9):
        _fail("the planar beam reference requires its local X axis to be global +X")
    ey = np.asarray([0.0, 1.0, 0.0])
    ez = np.cross(ex, ey)
    material = element.get("material", {})
    section = element.get("section", {})
    young = _number(material.get("youngModulusPa"), "element Young's modulus")
    density = _number(material.get("densityKgM3"), "element density")
    area = _number(section.get("areaM2"), "element area")
    iz = _number(section.get("izM4"), "element Iz")
    if young <= 0.0 or density <= 0.0 or area <= 0.0 or iz <= 0.0:
        _fail("beam material and section values must be positive")

    length2 = length * length
    length3 = length2 * length
    scale_k = young * iz / length3
    local_k = scale_k * np.asarray([
        [12.0, 6.0 * length, -12.0, 6.0 * length],
        [6.0 * length, 4.0 * length2, -6.0 * length, 2.0 * length2],
        [-12.0, -6.0 * length, 12.0, -6.0 * length],
        [6.0 * length, 2.0 * length2, -6.0 * length, 4.0 * length2],
    ])
    scale_m = density * area * length / 420.0
    local_m = scale_m * np.asarray([
        [156.0, 22.0 * length, 54.0, -13.0 * length],
        [22.0 * length, 4.0 * length2, 13.0 * length, -3.0 * length2],
        [54.0, 13.0 * length, 156.0, -22.0 * length],
        [-13.0 * length, -3.0 * length2, -22.0 * length, 4.0 * length2],
    ])

    # The accepted plane is local [v1, rz1, v2, rz2] = global [UY, RZ].
    indices: list[int | None] = []
    coefficients: list[float] = []
    for node_id in node_ids:
        for direction, unit, local_axis in ((ey, "m", ey), (ez, "rad", ez)):
            key = _physical_key(node_id, direction, unit)
            indices.append(key_to_index.get(key))
            coefficients.append(float(np.dot(local_axis, direction)))
    _add_projected(stiffness, indices, coefficients, local_k)
    _add_projected(mass, indices, coefficients, local_m)
    return mass, stiffness


def build_source_reference(descriptor: dict, linear: dict) -> dict:
    """Map source initial values, independently assemble M/K, and check ports."""
    if not isinstance(descriptor, dict) or descriptor.get("status") != "supported":
        _fail("native import descriptor is not supported")
    model_kind = descriptor.get("modelKind")
    if model_kind not in ("four-rod", "cantilever-beam"):
        _fail("source model kind is unsupported")
    nodes = descriptor.get("nodes")
    free_dofs = descriptor.get("freeDofs")
    linear_dofs = linear.get("dofs") if isinstance(linear, dict) else None
    if not isinstance(nodes, list) or not isinstance(free_dofs, list) or not isinstance(linear_dofs, list):
        _fail("source descriptor and native export must contain node and DOF catalogs")
    if not linear_dofs or len(linear_dofs) != len(free_dofs):
        _fail("native exported DOF count differs from the source descriptor")

    nodes_by_id: dict[str, dict] = {}
    positions: dict[str, int] = {}
    for index, node in enumerate(nodes):
        if not isinstance(node, dict):
            _fail("source node catalog contains an invalid record")
        node_id = str(node.get("physicalId"))
        if node_id in nodes_by_id or type(node.get("position")) is not int:
            _fail("source node identities or positions are invalid")
        nodes_by_id[node_id] = node
        positions[node_id] = node["position"]
        if positions[node_id] != index:
            _fail("source node positions are not a contiguous ordered catalog")

    source_by_key: dict[tuple, dict] = {}
    for item in free_dofs:
        if not isinstance(item, dict):
            _fail("source free DOF catalog contains an invalid record")
        unit = item.get("unit")
        if unit not in ("m", "rad") or str(item.get("nodePhysicalId")) not in nodes_by_id:
            _fail("source free DOF has an invalid node or unit")
        key = _physical_key(item["nodePhysicalId"], item.get("globalDirection"), unit)
        if key in source_by_key:
            _fail("source descriptor contains duplicate physical free DOFs")
        source_by_key[key] = item

    ordered_keys: list[tuple] = []
    initial_q: list[float] = []
    initial_v: list[float] = []
    for index, item in enumerate(linear_dofs):
        if not isinstance(item, dict):
            _fail(f"native dofs[{index}] is invalid")
        node_id = str(item.get("nodePhysicalId"))
        if node_id not in nodes_by_id or type(item.get("nodePosition")) is not int:
            _fail(f"native dofs[{index}] has no source node identity")
        if item["nodePosition"] != positions[node_id]:
            _fail(f"native dofs[{index}] node position does not match its physical ID")
        unit = item.get("unit")
        if unit not in ("m", "rad"):
            _fail(f"native dofs[{index}] has an unsupported unit")
        key = _physical_key(node_id, item.get("globalDirection"), unit)
        source_item = source_by_key.get(key)
        if source_item is None or key in ordered_keys:
            _fail("native exported DOFs do not match source physical IDs, directions, and units")
        direction = _vector(item["globalDirection"], "native globalDirection")
        node = nodes_by_id[node_id]
        if unit == "m":
            q_values = _vector(node.get("initialDisplacementM"), "initial displacement")
            v_values = _vector(node.get("initialVelocityMS"), "initial velocity")
        else:
            q_values = _vector(node.get("initialRotationRad"), "initial rotation")
            v_values = _vector(node.get("initialAngularVelocityRadS"), "initial angular velocity")
        ordered_keys.append(key)
        initial_q.append(float(np.dot(direction, q_values)))
        initial_v.append(float(np.dot(direction, v_values)))
    if len(ordered_keys) != len(source_by_key):
        _fail("native exported DOFs omit one or more source free DOFs")

    key_to_index = {key: index for index, key in enumerate(ordered_keys)}
    if model_kind == "four-rod":
        analytical_m, analytical_k = _assemble_rod(descriptor, linear_dofs, key_to_index)
    else:
        analytical_m, analytical_k = _assemble_beam(descriptor, linear_dofs, key_to_index)
    native_m = _matrix(linear.get("M"), "native mass matrix", len(linear_dofs))
    native_k = _matrix(linear.get("K"), "native stiffness matrix", len(linear_dofs))

    def compare(expected: np.ndarray, actual: np.ndarray, label: str) -> dict:
        scale = max(float(np.max(np.abs(expected))), np.finfo(np.float64).tiny)
        absolute = float(np.max(np.abs(expected - actual)))
        relative = absolute / scale
        if not np.allclose(expected, actual, rtol=1.0e-9, atol=max(1.0e-13, scale * 1.0e-11)):
            _fail(f"native {label} matrix differs from the independent source assembly")
        return {"maxAbsoluteError": absolute, "relativeToMaxEntry": relative}

    mass_check = compare(analytical_m, native_m, "mass")
    stiffness_check = compare(analytical_k, native_k, "stiffness")
    ports = descriptor.get("ports")
    if not isinstance(ports, dict) or not isinstance(ports.get("force"), dict) or not isinstance(ports.get("velocity"), dict):
        _fail("source descriptor is missing force and velocity ports")
    force_port, velocity_port = ports["force"], ports["velocity"]
    if (force_port.get("kind") != "nodal_force" or force_port.get("unit") != "N" or
            velocity_port.get("kind") != "velocity" or velocity_port.get("unit") != "m/s" or
            str(force_port.get("nodePhysicalId")) != str(velocity_port.get("nodePhysicalId")) or
            not np.allclose(_vector(force_port.get("globalDirection"), "force port direction"),
                            _vector(velocity_port.get("globalDirection"), "velocity port direction"),
                            rtol=0.0, atol=1.0e-12)):
        _fail("source ports must be a collocated nodal force and translational velocity")
    force_id, velocity_id = force_port.get("id"), velocity_port.get("id")
    inputs, outputs = linear.get("inputs"), linear.get("outputs")
    if not isinstance(inputs, list) or not isinstance(outputs, list):
        _fail("native export is missing input/output catalogs")
    input_index = next((i for i, item in enumerate(inputs)
                        if isinstance(item, dict) and item.get("id") == force_id), None)
    output_index = next((i for i, item in enumerate(outputs)
                         if isinstance(item, dict) and item.get("id") == velocity_id), None)
    if input_index is None or output_index is None:
        _fail("native exported force and velocity port IDs differ from the source descriptor")
    if (str(inputs[input_index].get("nodePhysicalId")) != str(force_port.get("nodePhysicalId")) or
            inputs[input_index].get("unit") != "N" or
            str(outputs[output_index].get("nodePhysicalId")) != str(velocity_port.get("nodePhysicalId")) or
            outputs[output_index].get("unit") != "m/s" or outputs[output_index].get("quantity") != "Velocity"):
        _fail("native exported port nodes or units differ from the source descriptor")
    try:
        b_matrix = np.asarray(linear["B"], dtype=np.float64)
        sv_matrix = np.asarray(linear["Sv"], dtype=np.float64)
    except (KeyError, TypeError, ValueError, OverflowError):
        _fail("native input/output matrices are invalid")
    if (b_matrix.shape != (len(linear_dofs), len(inputs)) or
            sv_matrix.shape != (len(outputs), len(linear_dofs)) or
            not np.isfinite(b_matrix).all() or not np.isfinite(sv_matrix).all()):
        _fail("native input/output matrix dimensions are invalid")
    b_column = b_matrix[:, input_index]
    nonzero = np.flatnonzero(np.abs(b_column) > max(float(np.max(np.abs(b_column))) * 1.0e-12, 1.0e-15))
    port_key = _physical_key(force_port.get("nodePhysicalId"), force_port.get("globalDirection"), "m")
    force_coordinate = key_to_index.get(port_key)
    if force_coordinate is None or len(nonzero) != 1 or int(nonzero[0]) != force_coordinate:
        _fail("source nodal force must map to its matching free generalized coordinate")
    sensor_row = sv_matrix[output_index, :]
    sensor_nonzero = np.flatnonzero(np.abs(sensor_row) > max(float(np.max(np.abs(sensor_row))) * 1.0e-12,
                                                              1.0e-15))
    if (not np.isfinite(sensor_row).all() or len(sensor_nonzero) != 1 or
            int(sensor_nonzero[0]) != force_coordinate):
        _fail("source velocity sensor must map to the collocated free generalized coordinate")

    boundary_indices = [int(nonzero[0])]
    modes = min(2, len(linear_dofs) - len(boundary_indices))
    if modes < 1:
        _fail("native source has no interior coordinate for bounded modal analysis")
    source_sha = descriptor.get("sourceSha256")
    return {
        "modelKind": model_kind,
        "sourceSha256": source_sha,
        "initialQ": initial_q,
        "initialV": initial_v,
        "controller": {"sensorId": velocity_id, "actuatorId": force_id},
        "analysis": {"boundaryIndices": boundary_indices, "modes": modes},
        "analyticalM": analytical_m.tolist(),
        "analyticalK": analytical_k.tolist(),
        "matrixComparison": {"mass": mass_check, "stiffness": stiffness_check},
        "dofs": [dict(item) for item in linear_dofs],
    }
