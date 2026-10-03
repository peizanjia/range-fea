"""Actual native FE solve, accepted checkpoint resume and independent replay."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import threading

from .service import WorkflowError, canonical, file_hash
from .templates import TEMPLATES

_NATIVE_API_VERSION = "range.workflow.native/v1"


def _read(path: Path) -> dict:
    if path.stat().st_size > 32 * 1024 * 1024:
        raise WorkflowError("ARTIFACT_LIMIT", "JSON artifact exceeds 32 MiB")
    result = json.loads(path.read_bytes())
    canonical(result)
    if not isinstance(result, dict):
        raise WorkflowError("INVALID_ARTIFACT", "Expected a JSON artifact object")
    return result


def _native(service, directory: Path, label: str, cancel, args: list[str]) -> dict:
    raw = service.runner.run([str(service.native), *args], directory, label, cancel)
    operation = args[0] if args else ""
    responses = []
    # Native diagnostics are on stderr and the runner captures both into one log.
    for line in raw.splitlines():
        if not line.lstrip().startswith(b"{"):
            continue
        try:
            result = json.loads(line, object_pairs_hook=_unique_object)
            canonical(result)
        except (ValueError, TypeError, RecursionError):
            continue
        if isinstance(result, dict) and "apiVersion" in result:
            responses.append(result)
    if len(responses) != 1 or not _valid_native_response(responses[0], operation):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native adapter did not return a valid range.workflow.native/v1 response")
    return responses[0]


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _native_sha256(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64 and
            all(char in "0123456789abcdef" for char in value))


def _native_path(value: object) -> bool:
    return isinstance(value, str) and bool(value) and Path(value).is_absolute()


def _native_source_files(value: object) -> bool:
    return (isinstance(value, list) and bool(value) and all(
        isinstance(item, dict) and _native_path(item.get("path")) and
        _native_sha256(item.get("sha256")) for item in value))


def _native_finite_number(value: object, *, minimum: float | None = None) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value) and (minimum is None or value >= minimum)
    except OverflowError:
        return False


def _native_number_array(value: object, *, nullable: bool = False) -> bool:
    return (nullable and value is None) or (
        isinstance(value, list) and len(value) == 3 and
        all(_native_finite_number(item) for item in value))


def _valid_native_response(result: dict, operation: str) -> bool:
    if result.get("apiVersion") != _NATIVE_API_VERSION:
        return False
    if operation == "prepare":
        return (result.get("status") == "published" and
                _native_path(result.get("output")) and
                _native_sha256(result.get("sourceSha256")) and
                _native_path(result.get("sourcePath")) and
                _native_source_files(result.get("sourceFiles")) and
                _native_sha256(result.get("sha256")) and
                type(result.get("steps")) is int and 1 <= result["steps"] <= 5000 and
                _native_finite_number(result.get("dt"), minimum=0.0) and result["dt"] > 0.0 and
                type(result.get("resume")) is bool)
    if operation != "inspect" or result.get("status") != "ok":
        return False
    if not (_native_sha256(result.get("sourceSha256")) and
            _native_path(result.get("sourcePath")) and
            _native_source_files(result.get("sourceFiles")) and
            type(result.get("checkpointAvailable")) is bool and
            type(result.get("step")) is int and 0 <= result["step"] <= 5000 and
            type(result.get("controlEnabled")) is bool and
            type(result.get("plannedSteps")) is int and 1 <= result["plannedSteps"] <= 5000 and
            _native_finite_number(result.get("configuredDt"), minimum=0.0) and result["configuredDt"] > 0.0 and
            _native_finite_number(result.get("time"), minimum=0.0) and
            _native_finite_number(result.get("dt"), minimum=0.0) and
            all(isinstance(result.get(key), list) and
                all(_native_finite_number(value) for value in result[key])
                for key in ("q", "v", "a", "controlWork")) and
            type(result.get("controlAvailable")) is bool and
            isinstance(result.get("nodes"), list)):
        return False
    node_ids = set()
    for node in result["nodes"]:
        if not isinstance(node, dict) or type(node.get("id")) is not int or node["id"] in node_ids:
            return False
        node_ids.add(node["id"])
        for key in ("displacement", "velocity", "acceleration", "rotation",
                    "angularVelocity", "angularAcceleration"):
            if key not in node or not _native_number_array(node[key], nullable=True):
                return False
    return True


def _copy_bound(source: Path, destination: Path, expected: str):
    if source.is_symlink() or not source.is_file() or file_hash(source) != expected:
        raise WorkflowError("SOURCE_CHANGED", "Approved source changed before snapshot")
    with source.open("rb") as src, destination.open("xb") as dst:
        while True:
            block = src.read(1024 * 1024)
            if not block:
                break
            dst.write(block)
    if file_hash(destination) != expected or file_hash(source) != expected:
        raise WorkflowError("SOURCE_CHANGED", "Source changed while snapshot was being created")


def _verified_path(result: dict, key: str) -> Path:
    value = Path(result[key])
    if not value.is_absolute() or value.is_symlink() or not value.is_file():
        raise WorkflowError("SOURCE_CHANGED", "Original trajectory artifact is unavailable")
    return value


def _matches_template_dof(record: dict, node: int, physical: str) -> bool:
    import numpy as np
    try:
        direction = np.asarray(record["globalDirection"], dtype=float)
        unit = "m" if physical.startswith("U") else "rad"
        axis = "XYZ".index(physical[1])
        label = record.get("dof")
    except (KeyError, TypeError, ValueError, OverflowError):
        return False
    expected = [0.0, 0.0, 0.0]
    expected[axis] = 1.0
    return (record.get("nodePosition") == node and
            label in ("UX", "UY", "UZ", "RX", "RY", "RZ") and label.startswith(physical[0]) and
            record.get("unit") == unit and direction.shape == (3,) and
            bool(np.isfinite(direction).all()) and
            bool(np.allclose(np.abs(direction), expected, rtol=0.0, atol=1e-12)))


def run_native_workflow(service, job: dict, directory: Path, cancel: threading.Event,
                        binding: dict) -> dict:
    import numpy as np
    # Only explicitly catalogued native FE templates enter this workflow.
    for key, artifact in binding.items():
        if key != "extension" and file_hash(Path(artifact["path"])) != artifact["sha256"]:
            raise WorkflowError("SOURCE_CHANGED", "Approved tool/template changed before execution")
    resume = job["command"] == "workflow.resume"
    if resume:
        source = service.result_snapshot(job["parameters"]["jobId"])
        configuration = source["configuration"]
        additional = job["parameters"]["additionalSteps"]
        total = source["totalSteps"] + additional
        mode = source["mode"]
        linear_path = _verified_path(source, "linearPath")
        gains_path = _verified_path(source, "gainsPath")
        controller_path = _verified_path(source, "controllerPath")
        controller_host_path = _verified_path(source, "controllerHostPath")
        analysis_path = _verified_path(source, "analysisPath")
    else:
        source = None
        configuration = job["configuration"]
        additional = total = configuration["steps"]
        mode = job["parameters"]["mode"]
        definition = TEMPLATES[configuration.get("model", "four-rod")]
        template = directory / "template.ram"
        controller_path = directory / "stage9_velocity_controller.py"
        controller_host_path = directory / "controller_host.py"
        analysis_script = directory / "stage9_analysis.py"
        for key, target in (("template", template), ("controller", controller_path), ("analysis", analysis_script),
                            ("controllerHost", controller_host_path)):
            _copy_bound(Path(binding[key]["path"]), target, binding[key]["sha256"])
        linear_path, gains_path, analysis_path = (directory / n for n in ("linear.json", "gains.json", "analysis.json"))
        # Gain source digest must identify exactly the pre-solve active native input.
        _native(service, directory, "prepare-active", cancel,
                ["prepare", "--input", str(template), "--output", str(directory / "active.ram"),
                 "--steps", str(total), "--dt", str(configuration["dt"])])
        service.runner.run([str(service.solver), "--file=" + str(directory / "active.ram"),
                            "--linear-model-output=" + str(linear_path)], directory, "linear-export", cancel)
        service.runner.run([str(service.python), str(analysis_script), "--model", str(linear_path),
                            "--output", str(analysis_path), "--modes", str(definition["modes"]),
                            "--boundary-indices-json", canonical([definition["boundary"]]).decode(),
                            "--frequencies-json", "[0,5,15,30]"], directory, "analysis", cancel)
        if _read(analysis_path).get("errors"):
            raise WorkflowError("ANALYSIS_FAILED", "Independent linear analysis reported errors")
        service.runner.run([str(service.python), str(controller_path), "--model", str(linear_path),
                            "--target-damping", str(configuration["targetDamping"]), "--sensor-id", definition["sensor"],
                            "--actuator-id", definition["actuator"], "--output", str(gains_path)], directory, "gain-design", cancel)
    linear = _read(linear_path)
    dofs = linear["dofs"]
    model = configuration.get("model", "four-rod")
    definition = TEMPLATES[model]
    expected_dofs = definition["dofs"]
    if len(dofs) != len(expected_dofs) or any(
            not _matches_template_dof(d, node, physical)
            for d, (node, physical) in zip(dofs, expected_dofs)):
        raise WorkflowError("UNSUPPORTED_MODEL", "Exported physical coordinates differ from the selected template")
    q0 = []
    for d, (_, physical) in zip(dofs, expected_dofs):
        axis = "XYZ".index(physical[1])
        amplitude = (0.001 * math.sin(math.pi * d["nodePosition"] / 8.0)
                     if model == "four-rod" else 0.001)
        q0.append(float(d["globalDirection"][axis]) * amplitude if physical.startswith("U") else 0.0)
    reference_path = directory / "reference.json"
    service.runner.run([str(service.python), str(controller_path), "--replay-model", str(linear_path),
                        "--gains", str(gains_path), "--dt", str(configuration["dt"]), "--steps", str(total),
                        "--initial-q-json", canonical(q0).decode(), "--output", str(reference_path)],
                       directory, "independent-reference", cancel)
    reference = _read(reference_path)
    result = {"jobId": job["jobId"], "totalSteps": total, "dtSeconds": configuration["dt"],
              "mode": mode, "configuration": configuration, "independentReference": True,
              "model": model, "dofs": dofs,
              "referencePath": str(reference_path), "linearPath": str(linear_path),
              "gainsPath": str(gains_path), "controllerPath": str(controller_path),
              "controllerHostPath": str(controller_host_path),
              "analysisPath": str(analysis_path)}
    result["sourceArtifacts"] = {name: {"path": str(path), "sha256": file_hash(path)} for name, path in
                                 (("linear", linear_path), ("gains", gains_path), ("controller", controller_path),
                                  ("controllerHost", controller_host_path), ("analysis", analysis_path))}
    mass, stiffness = np.asarray(linear["M"], dtype=float), np.asarray(linear["K"], dtype=float)
    for variant in ("active", "passive"):
        if mode not in (variant, "both"):
            continue
        native_path = directory / (variant + ".ram")
        if resume:
            checkpoint = Path(source[variant]["checkpointPath"])
            if file_hash(checkpoint) != source[variant]["checkpointSha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Accepted checkpoint changed")
            _native(service, directory, "prepare-" + variant, cancel,
                    ["prepare", "--input", str(checkpoint), "--output", str(native_path),
                     "--steps", str(additional), "--dt", str(configuration["dt"]), "--resume"])
        elif variant == "passive":
            _native(service, directory, "prepare-passive", cancel,
                    ["prepare", "--input", str(directory / "template.ram"), "--output", str(native_path),
                     "--steps", str(total), "--dt", str(configuration["dt"]), "--passive"])
        args = [str(service.solver), "--file=" + str(native_path)]
        if resume:
            args.append("--restart")
        if variant == "active":
            args += ["--control-program=" + str(service.python), "--control-timeout-ms=10000",
                     "--control-args-json=" + canonical([str(controller_host_path), "--implementation", str(controller_path),
                                                         "--gains", str(gains_path)]).decode()]
        environment = os.environ.copy()
        environment["RANGE_WORKFLOW_INPUT_SHA256"] = file_hash(native_path)
        service.runner.run(args, directory, "solve-" + variant, cancel, env=environment)
        accepted = _native(service, directory, "inspect-" + variant, cancel, ["inspect", "--input", str(native_path)])
        if accepted.get("checkpointAvailable") is not True or accepted["step"] != total:
            raise WorkflowError("INCOMPLETE_RESULT", "Solver did not publish the requested accepted checkpoint")
        if abs(accepted["time"] - total * configuration["dt"]) > 1e-8:
            raise WorkflowError("INCOMPLETE_RESULT", "Accepted checkpoint time differs from requested trajectory")
        checkpoint_path = Path(accepted.get("sourcePath", str(native_path)))
        if not checkpoint_path.resolve().is_relative_to(directory.resolve()):
            raise WorkflowError("ARTIFACT_PATH", "Accepted checkpoint escaped its job directory")
        nodes = {node["id"]: node for node in accepted["nodes"]}
        q = np.asarray([np.dot(nodes[d["nodePosition"]]["displacement" if physical.startswith("U") else "rotation"], d["globalDirection"])
                        for d, (_, physical) in zip(dofs, expected_dofs)])
        v = np.asarray([np.dot(nodes[d["nodePosition"]]["velocity" if physical.startswith("U") else "angularVelocity"], d["globalDirection"])
                        for d, (_, physical) in zip(dofs, expected_dofs)])
        if not np.isfinite(q).all() or not np.isfinite(v).all():
            raise WorkflowError("NONFINITE_RESULT", "Native FE result contains non-finite values")
        predicted = reference["fullOrder" if variant == "active" else "passiveFullOrder"]
        q_difference, v_difference = np.abs(q - np.asarray(predicted["q"])), np.abs(v - np.asarray(predicted["v"]))
        translational = np.asarray([physical.startswith("U") for _, physical in expected_dofs])
        q_error, v_error = float(np.max(q_difference[translational])), float(np.max(v_difference[translational]))
        rotation_error = float(np.max(q_difference[~translational])) if np.any(~translational) else 0.0
        angular_velocity_error = float(np.max(v_difference[~translational])) if np.any(~translational) else 0.0
        if q_error > 1e-8 or v_error > 1e-7 or rotation_error > 1e-8 or angular_velocity_error > 1e-7:
            raise WorkflowError("REFERENCE_MISMATCH", "Native FE result disagrees with the independent Newmark reference")
        result[variant] = {"q": q.tolist(), "v": v.tolist(),
                           "energyJ": float(0.5 * (q @ stiffness @ q + v @ mass @ v)),
                           "maxDisplacementErrorM": q_error, "maxVelocityErrorMPerSecond": v_error,
                           "maxRotationErrorRad": rotation_error, "maxAngularVelocityErrorRadPerSecond": angular_velocity_error,
                           "inputSnapshotSha256": environment["RANGE_WORKFLOW_INPUT_SHA256"],
                           "checkpointPath": str(checkpoint_path), "checkpointSha256": file_hash(checkpoint_path)}
    if mode == "both":
        passive_energy = result["passive"]["energyJ"]
        if passive_energy <= 0:
            raise WorkflowError("INVALID_ENERGY", "Passive reference energy must be positive")
        result["controlledToPassiveEnergyRatio"] = result["active"]["energyJ"] / passive_energy
    (directory / "verification.json").write_bytes(canonical(result))
    return result
