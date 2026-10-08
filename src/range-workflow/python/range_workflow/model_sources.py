"""Immutable registry for explicitly imported native model snapshots.

The host supplies a project snapshot reference and the configured native CLI.
Only the host-owned project catalog supplies source paths. Registry entries are
published through an append-only index so a later checksum read cannot silently
become the new trusted checksum.
"""
from __future__ import annotations

from contextlib import contextmanager
import copy
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import threading
import uuid

from .project_catalog import MAX_SNAPSHOT_BYTES, PROTOCOL_VERSION as PROJECT_PROTOCOL_VERSION
from .project_catalog import load_project_catalog, resolve_project_snapshot
from .service import WorkflowError, canonical, file_hash

PROTOCOL_VERSION = "range.native-import/v1"
STORAGE_VERSION = 1
MAX_DESCRIPTOR_BYTES = 512 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_INDEX_BYTES = 8 * 1024 * 1024
MAX_SOURCES = 4096
MAX_PAGE_SIZE = 100
MAX_PAGE_OFFSET = 2**31 - 1
NATIVE_TIMEOUT_SECONDS = 30
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
_SEED = re.compile(r"^(0|[1-9][0-9]*)$")
_DOFS = {"UX", "UY", "UZ", "RX", "RY", "RZ"}
_VECTOR_FIELDS = ("initialDisplacementM", "initialVelocityMS",
                  "initialRotationRad", "initialAngularVelocityRadS")

_LOCKS_GUARD = threading.Lock()
_LOCKS: dict[str, threading.RLock] = {}


def _fail(code: str, message: str):
    raise WorkflowError(code, message)


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def _parse_json(raw: bytes, message: str) -> object:
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite JSON")))
        _bounded_tree(value)
        canonical(value)
        return value
    except (UnicodeError, ValueError, TypeError, RecursionError, OverflowError):
        _fail("MODEL_SOURCE_INVALID", message)


def _bounded_tree(value):
    count = 0

    def visit(item, depth):
        nonlocal count
        count += 1
        if depth > 40 or count > 150_000:
            raise ValueError("JSON structure limit")
        if isinstance(item, dict):
            for key, entry in item.items():
                if not isinstance(key, str):
                    raise ValueError("JSON object key")
                visit(entry, depth + 1)
        elif isinstance(item, list):
            for entry in item:
                visit(entry, depth + 1)
        elif type(item) is float and not math.isfinite(item):
            raise ValueError("non-finite JSON number")

    visit(value, 0)


def _is_link(path: Path) -> bool:
    try:
        if path.is_symlink():
            return True
        is_junction = getattr(path, "is_junction", None)
        return bool(is_junction and is_junction())
    except OSError:
        return True


def _contains_link(path: Path) -> bool:
    """Inspect every existing component without following a link."""
    path = Path(os.path.abspath(path))
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current = current / component
        if _is_link(current):
            return True
    return False


def _root_path(value, *, create: bool) -> Path:
    try:
        raw = Path(value)
    except (TypeError, ValueError, OSError):
        _fail("INVALID_PATH", "Workspace and catalog roots must be absolute paths")
    if not raw.is_absolute() or ".." in raw.parts or _contains_link(raw):
        _fail("INVALID_PATH", "Workspace and catalog roots must be canonical nonsymlink paths")
    if create:
        try:
            raw.mkdir(parents=True, exist_ok=True)
        except OSError:
            _fail("INVALID_PATH", "Workflow workspace is unavailable")
    try:
        resolved = raw.resolve(strict=True)
        if _contains_link(raw) or not resolved.is_dir():
            _fail("INVALID_PATH", "Workspace and catalog roots must be nonsymlink directories")
        return resolved
    except OSError:
        _fail("INVALID_PATH", "Workspace and catalog roots must be available directories")


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(os.path.normpath(str(left))) == os.path.normcase(os.path.normpath(str(right)))


def _assert_managed_path(root: Path, path: Path, *, must_exist: bool) -> None:
    """Require a path composed below root, with no link in any component."""
    try:
        relative = path.relative_to(root)
    except ValueError:
        _fail("MODEL_SOURCE_INVALID", "Registered model path escaped its managed root")
    current = root
    for component in relative.parts:
        if component in ("", ".", ".."):
            _fail("MODEL_SOURCE_INVALID", "Registered model path is not canonical")
        current = current / component
        if _is_link(current):
            _fail("MODEL_SOURCE_INVALID", "Registered model paths must not traverse symbolic links")
    if must_exist:
        try:
            resolved = path.resolve(strict=True)
        except OSError:
            _fail("MODEL_SOURCE_INVALID", "Registered model file is unavailable")
        if not _same_path(path, resolved):
            _fail("MODEL_SOURCE_INVALID", "Registered model path is not canonical")


def _sha256(value, name: str) -> str:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        _fail("MODEL_SOURCE_INVALID", f"{name} must be a lowercase SHA-256 digest")
    return value


def _fsync_directory(path: Path):
    if os.name == "nt":
        return
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _uuid(value, name: str) -> str:
    if not isinstance(value, str) or not _UUID.fullmatch(value):
        _fail("MODEL_SOURCE_INVALID", f"{name} must be a canonical UUID")
    try:
        parsed = uuid.UUID(value)
    except ValueError:
        _fail("MODEL_SOURCE_INVALID", f"{name} must be a canonical UUID")
    if parsed.int == 0 or str(parsed) != value:
        _fail("MODEL_SOURCE_INVALID", f"{name} must be a canonical UUID")
    return value


def _source_reference(snapshot_ref: object) -> dict:
    required = {"projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"}
    optional = {"protocolVersion", "path", "snapshotBytesVerified"}
    if not isinstance(snapshot_ref, dict) or not required <= snapshot_ref.keys() or snapshot_ref.keys() - required - optional:
        _fail("INVALID_PARAMETERS", "snapshot_ref must contain the accepted project and runtime identities")
    if "protocolVersion" in snapshot_ref and snapshot_ref["protocolVersion"] != PROJECT_PROTOCOL_VERSION:
        _fail("INVALID_PARAMETERS", "snapshot_ref uses an unsupported project protocol")
    if "snapshotBytesVerified" in snapshot_ref and snapshot_ref["snapshotBytesVerified"] is not True:
        _fail("INVALID_PARAMETERS", "snapshot_ref must identify a verified accepted snapshot")
    instance = snapshot_ref["instanceRef"]
    if (not isinstance(instance, dict) or set(instance) != {"documentId", "sessionEpoch", "documentRevision"}
            or type(instance.get("documentRevision")) is not int
            or not 0 <= instance["documentRevision"] <= 2**53 - 1):
        _fail("INVALID_PARAMETERS", "snapshot_ref has an invalid runtime instance reference")
    for key in ("projectId", "persistentDocumentId", "instanceRef"):
        if key == "instanceRef":
            for identity in ("documentId", "sessionEpoch"):
                try:
                    parsed = uuid.UUID(instance[identity])
                except (ValueError, TypeError, AttributeError):
                    _fail("INVALID_PARAMETERS", "snapshot_ref has an invalid runtime instance reference")
                if parsed.int == 0 or str(parsed) != instance[identity]:
                    _fail("INVALID_PARAMETERS", "snapshot_ref has an invalid runtime instance reference")
        else:
            try:
                parsed = uuid.UUID(snapshot_ref[key])
            except (ValueError, TypeError, AttributeError):
                _fail("INVALID_PARAMETERS", "snapshot_ref has invalid project identities")
            if parsed.int == 0 or str(parsed) != snapshot_ref[key]:
                _fail("INVALID_PARAMETERS", "snapshot_ref has invalid project identities")
    if (not isinstance(snapshot_ref["sourceRevision"], str)
            or not re.fullmatch(r"r(0|[1-9][0-9]*)", snapshot_ref["sourceRevision"])):
        _fail("INVALID_PARAMETERS", "snapshot_ref has an invalid source revision")
    if not isinstance(snapshot_ref["modelSha256"], str) or not _SHA256.fullmatch(snapshot_ref["modelSha256"]):
        _fail("INVALID_PARAMETERS", "snapshot_ref has an invalid model SHA-256")
    return {key: copy.deepcopy(snapshot_ref[key]) for key in required}


def _origin_ref(resolved: dict) -> dict:
    return {key: copy.deepcopy(resolved[key]) for key in
            ("projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef")}


def _resolve_expected(catalog_root: Path, expected: dict) -> dict:
    resolved = resolve_project_snapshot(catalog_root, expected["persistentDocumentId"])
    actual = _origin_ref(resolved)
    if actual != expected:
        _fail("STALE_SOURCE", "The project catalog snapshot no longer matches the supplied source reference")
    path = Path(resolved["path"])
    expected_path = (catalog_root / "models" / expected["persistentDocumentId"] / "snapshots"
                     / expected["modelSha256"] / "model.rbm")
    _assert_managed_path(catalog_root, path, must_exist=True)
    if not _same_path(path, expected_path) or not path.is_file():
        _fail("SNAPSHOT_UNAVAILABLE", "The accepted project snapshot is not at its managed catalog path")
    try:
        if path.stat().st_size > MAX_SNAPSHOT_BYTES:
            _fail("SNAPSHOT_UNAVAILABLE", "The accepted project snapshot exceeds the size limit")
        if file_hash(path) != expected["modelSha256"]:
            _fail("SOURCE_CHANGED", "The accepted project snapshot hash changed")
    except OSError:
        _fail("SNAPSHOT_UNAVAILABLE", "The accepted project snapshot is unreadable")
    return {"originRef": actual, "path": path}


def _revalidate_catalog_identity(catalog_root: Path, expected: dict, source_path: Path):
    """Recheck catalog version identity without rereading large snapshot bytes."""
    catalog = load_project_catalog(catalog_root)
    if catalog is None or catalog["projectId"] != expected["projectId"]:
        _fail("STALE_SOURCE", "The host project identity changed during source registration")
    document = catalog["documents"].get(expected["persistentDocumentId"])
    if (document is None or document["tombstone"]
            or document["sourceRevision"] != expected["sourceRevision"]
            or document["snapshotSourceRevision"] != expected["sourceRevision"]
            or document["modelSha256"] != expected["modelSha256"]
            or document["snapshotInstanceRef"] != expected["instanceRef"]):
        _fail("STALE_SOURCE", "The project catalog snapshot identity changed during source registration")
    expected_path = (catalog_root / "models" / expected["persistentDocumentId"] / "snapshots"
                     / expected["modelSha256"] / "model.rbm")
    _assert_managed_path(catalog_root, source_path, must_exist=True)
    if not _same_path(source_path, expected_path) or not source_path.is_file():
        _fail("SNAPSHOT_UNAVAILABLE", "The accepted project snapshot is not at its managed catalog path")


def _finite_number(value, name: str, *, minimum: float | None = None,
                   maximum: float | None = None) -> float:
    if type(value) not in (int, float):
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor field {name} must be finite numeric data")
    try:
        number = float(value)
    except (ValueError, OverflowError):
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor field {name} must be finite numeric data")
    if (not math.isfinite(number) or (minimum is not None and number < minimum)
            or (maximum is not None and number > maximum)):
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor field {name} is outside its supported range")
    return number


def _require_object(value, fields: set[str], name: str):
    if not isinstance(value, dict) or value.keys() != fields:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {name} has an invalid shape")


def _positive_id(value, name: str) -> int:
    if type(value) is not int or not 1 <= value <= 2**53 - 1:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {name} must be a positive physical ID")
    return value


def _vector(value, name: str) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor field {name} must have three components")
    return [_finite_number(component, name) for component in value]


def _direction(value, name: str):
    direction = _vector(value, name)
    norm = math.sqrt(sum(component * component for component in direction))
    nonzero = [component for component in direction if abs(component) > 1e-8]
    if abs(norm - 1.0) > 1e-8 or len(nonzero) != 1 or abs(abs(nonzero[0]) - 1.0) > 1e-8:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {name} must be a global Cartesian axis")
    return direction


def _validate_port(value: object, *, kind: str, unit: str):
    _require_object(value, {"id", "nodePhysicalId", "globalDirection", "unit", "kind"}, f"{kind} port")
    if (not isinstance(value["id"], str) or not value["id"] or len(value["id"].encode("utf-8")) > 128
            or value["kind"] != kind or value["unit"] != unit):
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {kind} port is invalid")
    _positive_id(value["nodePhysicalId"], f"{kind} port nodePhysicalId")
    _direction(value["globalDirection"], f"{kind} port globalDirection")


def _validate_dof(value: object, *, supported: bool, name: str):
    fields = {"nodePhysicalId", "physicalDof", "globalDirection", "unit"}
    if supported:
        fields.add("value")
    _require_object(value, fields, name)
    node_id = _positive_id(value["nodePhysicalId"], f"{name} nodePhysicalId")
    physical_dof = value["physicalDof"]
    if not isinstance(physical_dof, str) or physical_dof not in _DOFS:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {name} has an unsupported physical DOF")
    expected_unit = "m" if physical_dof.startswith("U") else "rad"
    if value["unit"] != expected_unit:
        _fail("NATIVE_PREFLIGHT_INVALID", f"Native descriptor {name} has an incompatible unit")
    _direction(value["globalDirection"], f"{name} globalDirection")
    if supported:
        _finite_number(value["value"], f"{name} value")
    return node_id, physical_dof, value["unit"], tuple(value["globalDirection"])


def _validate_descriptor(value: object, expected_sha: str) -> dict:
    fields = {"apiVersion", "status", "sourceSha256", "modelKind", "nodes", "elements",
              "pointElements", "freeDofs", "supports", "ports", "dynamics", "originalControl", "diagnostics"}
    _require_object(value, fields, "root")
    if value["apiVersion"] != PROTOCOL_VERSION:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor protocol version is unsupported")
    if value["status"] != "supported":
        _fail("NATIVE_PREFLIGHT_REJECTED", "The source model is outside the supported import matrix")
    if _sha256(value["sourceSha256"], "Native source hash") != expected_sha:
        _fail("SOURCE_CHANGED", "Native preflight described bytes different from the registered model copy")
    model_kind = value["modelKind"]
    if model_kind not in ("four-rod", "cantilever-beam"):
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor model kind is unsupported")

    nodes, elements = value["nodes"], value["elements"]
    expected_node_count, expected_element_count, expected_element_type, expected_point_count = (
        (5, 4, "truss1", 6) if model_kind == "four-rod" else (2, 1, "beam1", 2))
    if not isinstance(nodes, list) or len(nodes) != expected_node_count:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor node topology does not match its model kind")
    node_ids, positions = set(), set()
    for node in nodes:
        _require_object(node, {"position", "physicalId", "coordinates", *_VECTOR_FIELDS}, "node")
        if type(node["position"]) is not int or not 0 <= node["position"] < len(nodes):
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor node position is invalid")
        if node["position"] in positions:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor node positions must be unique")
        positions.add(node["position"])
        physical_id = _positive_id(node["physicalId"], "node physicalId")
        if physical_id in node_ids:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor node physical IDs must be unique")
        node_ids.add(physical_id)
        _vector(node["coordinates"], "node coordinates")
        for field in _VECTOR_FIELDS:
            _vector(node[field], f"node {field}")
    if positions != set(range(len(nodes))):
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor node positions must be contiguous")
    if not isinstance(elements, list) or len(elements) != expected_element_count:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor element topology does not match its model kind")
    element_ids = set()
    for element in elements:
        _require_object(element, {"type", "physicalId", "nodePhysicalIds", "material", "section"}, "element")
        if element["type"] != expected_element_type:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor contains an unsupported element type")
        physical_id = _positive_id(element["physicalId"], "element physicalId")
        if physical_id in element_ids:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor element physical IDs must be unique")
        element_ids.add(physical_id)
        connectivity = element["nodePhysicalIds"]
        if (not isinstance(connectivity, list) or len(connectivity) != 2
                or any(type(node_id) is not int or node_id not in node_ids for node_id in connectivity)
                or connectivity[0] == connectivity[1]):
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor element connectivity is invalid")
        _require_object(element["material"], {"youngModulusPa", "densityKgM3", "poissonRatio"}, "material")
        _finite_number(element["material"]["youngModulusPa"], "material youngModulusPa", minimum=0.0)
        _finite_number(element["material"]["densityKgM3"], "material densityKgM3", minimum=0.0)
        if element["material"]["youngModulusPa"] == 0 or element["material"]["densityKgM3"] == 0:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor material stiffness and density must be positive")
        poisson = _finite_number(element["material"]["poissonRatio"], "material poissonRatio")
        if not -1.0 < poisson < 0.5:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor Poisson ratio is outside its stable range")
        section_fields = {"areaM2", "iyM4", "izM4", "torsionM4"}
        _require_object(element["section"], section_fields, "section")
        for field in section_fields:
            _finite_number(element["section"][field], f"section {field}", minimum=0.0)
        required_sections = ("areaM2",) if model_kind == "four-rod" else tuple(section_fields)
        if any(element["section"][field] <= 0 for field in required_sections):
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor active section properties must be positive")

    point_elements = value["pointElements"]
    if not isinstance(point_elements, list) or len(point_elements) != expected_point_count:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor point-element topology does not match its model kind")
    point_ids = set()
    for element in point_elements:
        _require_object(element, {"physicalId", "nodePhysicalId"}, "point element")
        physical_id = _positive_id(element["physicalId"], "point element physicalId")
        node_id = _positive_id(element["nodePhysicalId"], "point element nodePhysicalId")
        if physical_id in element_ids or physical_id in point_ids or node_id not in node_ids:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor point-element identity or node reference is invalid")
        point_ids.add(physical_id)

    free_dofs, supports = value["freeDofs"], value["supports"]
    expected_free_count = 4 if model_kind == "four-rod" else 2
    expected_support_count = 11 if model_kind == "four-rod" else 10
    if not isinstance(free_dofs, list) or len(free_dofs) != expected_free_count:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor free DOF catalog does not match its model kind")
    if not isinstance(supports, list) or len(supports) != expected_support_count:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor support catalog does not match its model kind")
    free_keys, support_keys = set(), set()
    for dof in free_dofs:
        key = _validate_dof(dof, supported=False, name="free DOF")
        if key[0] not in node_ids or key in free_keys:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor free DOFs must be unique model coordinates")
        free_keys.add(key)
    for support in supports:
        key = _validate_dof(support, supported=True, name="support")
        if key[0] not in node_ids or key in support_keys or support["value"] != 0:
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor supports must be unique model coordinates")
        support_keys.add(key)
    if free_keys & support_keys:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor DOF cannot be both free and supported")

    ports = value["ports"]
    _require_object(ports, {"force", "velocity"}, "ports")
    _validate_port(ports["force"], kind="nodal_force", unit="N")
    _validate_port(ports["velocity"], kind="velocity", unit="m/s")
    if ports["force"]["nodePhysicalId"] not in node_ids or ports["velocity"]["nodePhysicalId"] not in node_ids:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor ports must reference source nodes")

    dynamics = value["dynamics"]
    dynamics_fields = {"enabled", "method", "massMatrix", "timeStepSeconds", "steps", "outputFrequency",
                       "newmarkGamma", "newmarkBeta", "rayleighAlpha", "rayleighBeta",
                       "modalDampingRatios", "rhoInfinity", "legacyTimeSolverEnabled", "restartAvailable"}
    _require_object(dynamics, dynamics_fields, "dynamics")
    if (dynamics["enabled"] is not True or dynamics["method"] != "newmark"
            or dynamics["massMatrix"] != "consistent" or dynamics["legacyTimeSolverEnabled"] is not False
            or dynamics["restartAvailable"] is not False):
        _fail("NATIVE_PREFLIGHT_REJECTED", "Source dynamics settings are outside the supported import matrix")
    _finite_number(dynamics["timeStepSeconds"], "dynamics timeStepSeconds", minimum=0.0)
    if dynamics["timeStepSeconds"] <= 0:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor time step must be positive")
    if (type(dynamics["steps"]) is not int or not 1 <= dynamics["steps"] <= 5000
            or type(dynamics["outputFrequency"]) is not int
            or not 1 <= dynamics["outputFrequency"] <= dynamics["steps"]):
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor step counts are invalid")
    if (_finite_number(dynamics["newmarkGamma"], "dynamics newmarkGamma") != 0.5
            or _finite_number(dynamics["newmarkBeta"], "dynamics newmarkBeta") != 0.25
            or _finite_number(dynamics["rayleighAlpha"], "dynamics rayleighAlpha") != 0.0
            or _finite_number(dynamics["rayleighBeta"], "dynamics rayleighBeta") != 0.0):
        _fail("NATIVE_PREFLIGHT_REJECTED", "Source integration or damping settings are outside the supported import matrix")
    _finite_number(dynamics["rhoInfinity"], "dynamics rhoInfinity")
    if not isinstance(dynamics["modalDampingRatios"], list) or dynamics["modalDampingRatios"]:
        _fail("NATIVE_PREFLIGHT_REJECTED", "Modal damping is outside the supported import matrix")

    original_control = value["originalControl"]
    _require_object(original_control, {"enabled", "controlProtocolVersion", "controllerId", "deterministicSeed"},
                    "originalControl")
    if (type(original_control["enabled"]) is not bool
            or type(original_control["controlProtocolVersion"]) is not int
            or original_control["controlProtocolVersion"] < 0
            or not isinstance(original_control["controllerId"], str)
            or len(original_control["controllerId"].encode("utf-8")) > 256
            or not isinstance(original_control["deterministicSeed"], str)
            or not _SEED.fullmatch(original_control["deterministicSeed"])
            or int(original_control["deterministicSeed"]) > 2**64 - 1):
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor original control metadata is invalid")

    diagnostics = value["diagnostics"]
    if not isinstance(diagnostics, list) or len(diagnostics) > 256:
        _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor diagnostics are invalid")
    for diagnostic in diagnostics:
        _require_object(diagnostic, {"code", "severity", "message", "path"}, "diagnostic")
        if (not isinstance(diagnostic["code"], str) or not diagnostic["code"]
                or diagnostic["severity"] != "error"
                or not isinstance(diagnostic["message"], str)
                or not isinstance(diagnostic["path"], str)
                or len(diagnostic["code"].encode("utf-8")) > 128
                or len(diagnostic["message"].encode("utf-8")) > 4096
                or len(diagnostic["path"].encode("utf-8")) > 4096):
            _fail("NATIVE_PREFLIGHT_INVALID", "Native descriptor diagnostic fields are invalid")
        _fail("NATIVE_PREFLIGHT_REJECTED", "Native descriptor reports a source-model rejection")

    return copy.deepcopy(value)


def _read_stream(stream, process, state, buffer_name: str):
    while True:
        chunk = stream.read(16 * 1024)
        if not chunk:
            return
        with state["lock"]:
            remaining = MAX_DESCRIPTOR_BYTES - state["size"]
            if remaining > 0:
                state[buffer_name].append(chunk[:remaining])
            state["size"] += len(chunk)
            if state["size"] > MAX_DESCRIPTOR_BYTES:
                state["overflow"].set()
                try:
                    process.kill()
                except OSError:
                    pass


def _invoke_native_describe(native_path: Path, snapshot_path: Path, expected_sha: str) -> dict:
    executable = Path(native_path)
    if not executable.is_absolute() or _is_link(executable) or not executable.is_file():
        _fail("INVALID_PATH", "The configured native workflow executable is unavailable")
    executable = executable.resolve()
    argv = [str(executable), "describe", "--input", str(snapshot_path), "--expected-sha", expected_sha]
    try:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, shell=False, close_fds=True)
    except OSError:
        _fail("NATIVE_PREFLIGHT_FAILED", "Could not start the configured native import preflight")
    state = {"lock": threading.Lock(), "size": 0, "stdout": [], "stderr": [],
             "overflow": threading.Event()}
    readers = [threading.Thread(target=_read_stream, args=(stream, process, state, buffer_name), daemon=True)
               for stream, buffer_name in ((process.stdout, "stdout"), (process.stderr, "stderr"))]
    for reader in readers:
        reader.start()
    timed_out = False
    try:
        process.wait(timeout=NATIVE_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        process.wait()
    for reader in readers:
        reader.join(timeout=5)
    if any(reader.is_alive() for reader in readers):
        for stream in (process.stdout, process.stderr):
            try:
                stream.close()
            except OSError:
                pass
        for reader in readers:
            reader.join(timeout=1)
    for stream in (process.stdout, process.stderr):
        try:
            stream.close()
        except OSError:
            pass
    if timed_out:
        _fail("NATIVE_PREFLIGHT_TIMEOUT", "Native import preflight exceeded its 30 second limit")
    if state["overflow"].is_set() or any(reader.is_alive() for reader in readers):
        _fail("NATIVE_PREFLIGHT_OUTPUT_LIMIT", "Native import preflight exceeded its 512 KiB output limit")
    stdout = b"".join(state["stdout"])
    try:
        response = _parse_json(stdout, "Native preflight did not return one valid JSON descriptor")
    except WorkflowError:
        if process.returncode != 0:
            _fail("NATIVE_PREFLIGHT_FAILED", "Native import preflight rejected the source model")
        raise
    if not isinstance(response, dict):
        _fail("NATIVE_PREFLIGHT_INVALID", "Native preflight response must be a JSON object")
    if process.returncode != 0:
        if response.get("apiVersion") == PROTOCOL_VERSION and response.get("status") == "rejected":
            _fail("NATIVE_PREFLIGHT_REJECTED", "The source model is outside the supported import matrix")
        _fail("NATIVE_PREFLIGHT_FAILED", "Native import preflight failed")
    return response


def _sources_root(workspace: Path, *, create: bool) -> Path:
    path = workspace / "model-sources"
    if create:
        try:
            path.mkdir(exist_ok=True)
        except OSError:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registry directory is unavailable")
    if _is_link(path):
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registry must not be a symbolic link")
    if not path.exists():
        return path
    _assert_managed_path(workspace, path, must_exist=True)
    if not path.is_dir():
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registry must be a directory")
    return path


@contextmanager
def _registry_write_lock(directory: Path):
    key = os.path.normcase(str(directory))
    with _LOCKS_GUARD:
        local = _LOCKS.setdefault(key, threading.RLock())
    local.acquire()
    lock_file = None
    try:
        lock_path = directory / ".registry.lock"
        _assert_managed_path(directory, lock_path, must_exist=False)
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(lock_path, flags, 0o600)
        lock_file = os.fdopen(descriptor, "r+b", buffering=0)
        if _is_link(lock_path) or not lock_path.is_file():
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registry lock is not a regular file")
        if os.name == "nt":
            import msvcrt
            lock_file.seek(0, os.SEEK_END)
            if lock_file.tell() == 0:
                lock_file.write(b"0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        yield
    except WorkflowError:
        raise
    except OSError:
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Could not lock the model source registry")
    finally:
        if lock_file is not None:
            try:
                if os.name == "nt":
                    import msvcrt
                    lock_file.seek(0)
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
            lock_file.close()
        local.release()


def _load_index(directory: Path, *, missing_ok: bool) -> dict:
    path = directory / "index.json"
    _assert_managed_path(directory, path, must_exist=False)
    if not path.exists():
        if missing_ok:
            return {"storageVersion": STORAGE_VERSION, "sources": {}}
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger is missing")
    if _is_link(path) or not path.is_file():
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger must be a regular file")
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_INDEX_BYTES + 1)
    except OSError:
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger is unreadable")
    if len(raw) > MAX_INDEX_BYTES:
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger exceeds its size limit")
    value = _parse_json(raw, "Model source registration ledger is invalid")
    if (not isinstance(value, dict) or value.keys() != {"storageVersion", "sources"}
            or type(value["storageVersion"]) is not int or value["storageVersion"] != STORAGE_VERSION
            or not isinstance(value["sources"], dict) or len(value["sources"]) > MAX_SOURCES):
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger has an unsupported shape")
    for source_id, record in value["sources"].items():
        _uuid(source_id, "sourceId")
        if not isinstance(record, dict) or record.keys() != {"manifestSha256", "registeredModelSha256"}:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger entry is invalid")
        _sha256(record["manifestSha256"], "manifest hash")
        _sha256(record["registeredModelSha256"], "registered model hash")
    return value


def _atomic_index(directory: Path, value: dict):
    target = directory / "index.json"
    _assert_managed_path(directory, target, must_exist=False)
    if target.exists() and (_is_link(target) or not target.is_file()):
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger is not a regular file")
    raw = canonical(value)
    if len(raw) > MAX_INDEX_BYTES:
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registration ledger exceeds its size limit")
    temporary = directory / (".index-" + str(uuid.uuid4()) + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        _assert_managed_path(directory, temporary, must_exist=True)
        os.replace(temporary, target)
        if os.name != "nt":
            fd = os.open(directory, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        _fail("MODEL_SOURCE_REGISTRY_INVALID", "Could not atomically publish model source registrations")


def _read_registered(directory: Path, workspace: Path, source_id: str,
                     record: dict, *, revalidate: bool) -> dict:
    source_directory = directory / source_id
    _assert_managed_path(directory, source_directory, must_exist=True)
    if not source_directory.is_dir():
        _fail("MODEL_SOURCE_INVALID", "Registered model source directory is unavailable")
    manifest_path = source_directory / "source.json"
    model_path = source_directory / "model.rbm"
    _assert_managed_path(workspace, manifest_path, must_exist=True)
    _assert_managed_path(workspace, model_path, must_exist=True)
    if (_is_link(manifest_path) or not manifest_path.is_file()
            or _is_link(model_path) or not model_path.is_file()):
        _fail("MODEL_SOURCE_INVALID", "Registered model source files must be regular nonsymlink files")
    try:
        if manifest_path.stat().st_size > MAX_MANIFEST_BYTES or model_path.stat().st_size > MAX_SNAPSHOT_BYTES:
            _fail("MODEL_SOURCE_INVALID", "Registered model source exceeds its storage limit")
        manifest_raw = manifest_path.read_bytes()
    except OSError:
        _fail("MODEL_SOURCE_INVALID", "Registered model source files are unreadable")
    manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
    if manifest_sha != record["manifestSha256"]:
        _fail("MODEL_SOURCE_CHANGED", "Registered model source manifest changed after publication")
    manifest = _parse_json(manifest_raw, "Registered model source manifest is invalid")
    fields = {"manifestVersion", "sourceId", "originRef", "registeredModelSha256", "descriptor"}
    if (not isinstance(manifest, dict) or manifest.keys() != fields
            or type(manifest["manifestVersion"]) is not int or manifest["manifestVersion"] != STORAGE_VERSION
            or manifest["sourceId"] != source_id):
        _fail("MODEL_SOURCE_INVALID", "Registered model source manifest has an unsupported shape")
    registered_sha = _sha256(manifest["registeredModelSha256"], "registered model hash")
    if registered_sha != record["registeredModelSha256"]:
        _fail("MODEL_SOURCE_INVALID", "Registered model hash does not match its registration ledger")
    origin = _validate_origin_manifest(manifest["originRef"])
    descriptor = _validate_descriptor(manifest["descriptor"], registered_sha)
    if origin["modelSha256"] != registered_sha:
        _fail("MODEL_SOURCE_INVALID", "Registered model hash does not match its project snapshot reference")
    if revalidate:
        try:
            if file_hash(model_path) != registered_sha:
                _fail("MODEL_SOURCE_CHANGED", "Registered model bytes changed after publication")
        except OSError:
            _fail("MODEL_SOURCE_INVALID", "Registered model bytes are unreadable")
    return {"sourceId": source_id, "originRef": origin, "descriptor": descriptor,
            "registeredModelSha256": registered_sha, "manifestSha256": manifest_sha,
            "path": str(model_path), "manifestPath": str(manifest_path)}


def _validate_origin_manifest(value: object) -> dict:
    fields = {"projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"}
    if not isinstance(value, dict) or value.keys() != fields:
        _fail("MODEL_SOURCE_INVALID", "Registered model origin reference is invalid")
    expected = _source_reference(value)
    return expected


def register_snapshot_source(workspace, catalog_root, snapshot_ref, native_path) -> dict:
    """Verify, copy and preflight one accepted host project snapshot.

    ``snapshot_ref`` may contain only project/runtime identities. If it is the
    full project snapshot DTO, its path is checked for consistency but never
    used to locate or copy the source file.
    """
    expected = _source_reference(snapshot_ref)
    workspace_root = _root_path(workspace, create=True)
    catalog = _root_path(catalog_root, create=False)
    native = Path(native_path)
    if not native.is_absolute() or _is_link(native) or not native.is_file():
        _fail("INVALID_PATH", "The configured native workflow executable is unavailable")
    sources = _sources_root(workspace_root, create=True)
    with _registry_write_lock(sources):
        # Resolve the current catalog state immediately before copying. The
        # snapshot hash is checked by project_catalog, then both source/copy
        # hashes are checked after the copy before metadata identity is read again.
        current = _resolve_expected(catalog, expected)
        source = current["path"]
        if "path" in snapshot_ref and snapshot_ref["path"] != str(source):
            _fail("INVALID_PARAMETERS", "snapshot_ref path does not match the managed project catalog entry")
        source_id = str(uuid.uuid4())
        source_directory = sources / source_id
        try:
            source_directory.mkdir(exist_ok=False)
        except OSError:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Could not allocate an immutable model source directory")
        _assert_managed_path(workspace_root, source_directory, must_exist=True)
        _fsync_directory(sources)
        copy_path = source_directory / "model.rbm"
        try:
            with source.open("rb") as reader, copy_path.open("xb") as writer:
                copied_bytes = 0
                while True:
                    block = reader.read(1024 * 1024)
                    if not block:
                        break
                    copied_bytes += len(block)
                    if copied_bytes > MAX_SNAPSHOT_BYTES:
                        _fail("SNAPSHOT_UNAVAILABLE", "The accepted project snapshot exceeds the size limit")
                    writer.write(block)
                writer.flush()
                os.fsync(writer.fileno())
            _fsync_directory(source_directory)
        except OSError:
            _fail("SNAPSHOT_UNAVAILABLE", "Could not copy the accepted project snapshot")
        _assert_managed_path(workspace_root, copy_path, must_exist=True)
        try:
            copied_sha = file_hash(copy_path)
            source_sha_after_copy = file_hash(source)
        except OSError:
            _fail("SNAPSHOT_UNAVAILABLE", "Could not verify the copied project snapshot")
        expected_sha = expected["modelSha256"]
        if copied_sha != expected_sha or source_sha_after_copy != expected_sha:
            _fail("SOURCE_CHANGED", "Project snapshot bytes changed while registering the source")
        _revalidate_catalog_identity(catalog, expected, source)

        raw_descriptor = _invoke_native_describe(native, copy_path, expected_sha)
        if len(canonical(raw_descriptor)) > MAX_DESCRIPTOR_BYTES:
            _fail("NATIVE_PREFLIGHT_OUTPUT_LIMIT", "Native import descriptor exceeds its 512 KiB limit")
        descriptor = _validate_descriptor(raw_descriptor, expected_sha)
        try:
            if file_hash(copy_path) != expected_sha:
                _fail("MODEL_SOURCE_CHANGED", "Copied project snapshot changed during native preflight")
        except OSError:
            _fail("MODEL_SOURCE_INVALID", "Copied project snapshot is unreadable after native preflight")
        manifest = {"manifestVersion": STORAGE_VERSION, "sourceId": source_id,
                    "originRef": expected, "registeredModelSha256": copied_sha,
                    "descriptor": descriptor}
        manifest_raw = canonical(manifest)
        if len(manifest_raw) > MAX_MANIFEST_BYTES:
            _fail("MODEL_SOURCE_INVALID", "Model source manifest exceeds its size limit")
        manifest_path = source_directory / "source.json"
        try:
            with manifest_path.open("xb") as stream:
                stream.write(manifest_raw)
                stream.flush()
                os.fsync(stream.fileno())
            _fsync_directory(source_directory)
        except OSError:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Could not publish model source manifest")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()

        index = _load_index(sources, missing_ok=True)
        if source_id in index["sources"]:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Generated model source identity already exists")
        if len(index["sources"]) >= MAX_SOURCES:
            _fail("MODEL_SOURCE_REGISTRY_INVALID", "Model source registry reached its entry limit")
        index["sources"][source_id] = {"manifestSha256": manifest_sha,
                                       "registeredModelSha256": copied_sha}
        _atomic_index(sources, index)
        record = index["sources"][source_id]
        return _read_registered(sources, workspace_root, source_id, record, revalidate=True)


def read_model_source(workspace, source_id, revalidate=True) -> dict:
    """Read one registry entry by opaque source ID and verify its ledger binding."""
    if type(revalidate) is not bool:
        _fail("INVALID_PARAMETERS", "revalidate must be a boolean")
    workspace_root = _root_path(workspace, create=False)
    source_id = _uuid(source_id, "sourceId")
    sources = _sources_root(workspace_root, create=False)
    if not sources.exists():
        _fail("MODEL_SOURCE_NOT_FOUND", "Unknown registered model source")
    index = _load_index(sources, missing_ok=False)
    record = index["sources"].get(source_id)
    if record is None:
        _fail("MODEL_SOURCE_NOT_FOUND", "Unknown registered model source")
    return _read_registered(sources, workspace_root, source_id, record, revalidate=revalidate)


def list_model_sources(workspace, offset=0, limit=50) -> dict:
    """Return a bounded page of registered source entries in stable ID order."""
    if type(offset) is not int or not 0 <= offset <= MAX_PAGE_OFFSET:
        _fail("INVALID_PARAMETERS", "offset must be an integer in its supported range")
    if type(limit) is not int or not 1 <= limit <= MAX_PAGE_SIZE:
        _fail("INVALID_PARAMETERS", f"limit must be an integer in 1..{MAX_PAGE_SIZE}")
    workspace_root = _root_path(workspace, create=False)
    sources = _sources_root(workspace_root, create=False)
    if not sources.exists():
        return {"sources": [], "offset": offset, "limit": limit, "total": 0}
    index = _load_index(sources, missing_ok=False)
    source_ids = sorted(index["sources"])
    page = source_ids[offset:offset + limit]
    entries = [_read_registered(sources, workspace_root, source_id, index["sources"][source_id],
                                revalidate=True) for source_id in page]
    return {"sources": entries, "offset": offset, "limit": limit, "total": len(source_ids)}
