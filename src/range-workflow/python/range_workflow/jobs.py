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
    if operation in ("prepare", "prepare-source"):
        prepared_hash = result.get("preparedSha256") if operation == "prepare-source" else result.get("sha256")
        common = (result.get("status") == "published" and
                  _native_path(result.get("output")) and
                  _native_sha256(result.get("sourceSha256")) and
                  _native_path(result.get("sourcePath")) and
                  _native_source_files(result.get("sourceFiles")) and
                  _native_sha256(prepared_hash) and
                  type(result.get("steps")) is int and 1 <= result["steps"] <= 5000 and
                  _native_finite_number(result.get("dt"), minimum=0.0) and result["dt"] > 0.0 and
                  type(result.get("resume")) is bool)
        if operation == "prepare-source":
            return common and result.get("resume") is False
        return common
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


def _same_path(left: object, right: object) -> bool:
    try:
        return Path(left).resolve(strict=True) == Path(right).resolve(strict=True)
    except (OSError, TypeError, ValueError):
        return False


def _read_registered_source(service, source_id: str, binding: dict,
                            lineage: dict | None = None) -> tuple[dict, dict]:
    from .model_sources import read_model_source

    registered = read_model_source(service.workspace, source_id)
    if not isinstance(registered, dict):
        raise WorkflowError("MODEL_SOURCE_INVALID", "Registered source lookup returned an invalid record")
    expected_binding = {
        "kind": "native-snapshot", "usesOpenDocument": True,
        "sourceId": source_id, "originRef": registered["originRef"],
        "registeredModelSha256": registered["registeredModelSha256"],
        "sourceManifestSha256": registered["manifestSha256"],
    }
    if lineage is not None and lineage != expected_binding:
        raise WorkflowError("SOURCE_CHANGED", "Registered source lineage differs from the accepted workflow source")
    model_input = binding.get("modelInput")
    manifest = binding.get("sourceManifest")
    if (not isinstance(model_input, dict) or not isinstance(manifest, dict) or
            not _same_path(model_input.get("path"), registered["path"]) or
            not _same_path(manifest.get("path"), registered["manifestPath"]) or
            model_input.get("sha256") != registered["registeredModelSha256"] or
            manifest.get("sha256") != registered["manifestSha256"]):
        raise WorkflowError("SOURCE_CHANGED", "Approved source binding differs from the immutable registry")
    if file_hash(Path(model_input["path"])) != model_input["sha256"]:
        raise WorkflowError("SOURCE_CHANGED", "Registered model bytes changed before execution")
    if file_hash(Path(manifest["path"])) != manifest["sha256"]:
        raise WorkflowError("SOURCE_CHANGED", "Registered source manifest changed before execution")
    return registered, expected_binding


def _source_dof_state(linear: dict, accepted: dict) -> tuple[object, object]:
    import numpy as np

    nodes = {item["id"]: item for item in accepted.get("nodes", []) if isinstance(item, dict)}
    q, v = [], []
    dofs = linear.get("dofs")
    if not isinstance(dofs, list):
        raise WorkflowError("INVALID_LINEAR_MODEL", "Native linear export has no DOF catalog")
    for item in dofs:
        if not isinstance(item, dict):
            raise WorkflowError("INVALID_LINEAR_MODEL", "Native linear export contains an invalid DOF")
        position = item.get("nodePosition")
        try:
            direction = np.asarray(item.get("globalDirection"), dtype=float)
        except (TypeError, ValueError, OverflowError):
            raise WorkflowError("INVALID_LINEAR_MODEL", "Native DOF direction is invalid") from None
        if (type(position) is not int or direction.shape != (3,) or not np.isfinite(direction).all() or
                position not in nodes):
            raise WorkflowError("INVALID_LINEAR_MODEL", "Native DOF node or direction is invalid")
        node = nodes[position]
        if item.get("unit") == "m":
            q_values, v_values = node.get("displacement"), node.get("velocity")
        elif item.get("unit") == "rad":
            q_values, v_values = node.get("rotation"), node.get("angularVelocity")
        else:
            raise WorkflowError("INVALID_LINEAR_MODEL", "Native DOF unit is unsupported")
        q_values = np.asarray(q_values, dtype=float)
        v_values = np.asarray(v_values, dtype=float)
        if (q_values.shape != (3,) or v_values.shape != (3,) or
                not np.isfinite(q_values).all() or not np.isfinite(v_values).all()):
            raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native accepted state is missing a DOF component")
        q.append(float(np.dot(q_values, direction)))
        v.append(float(np.dot(v_values, direction)))
    return np.asarray(q), np.asarray(v)


def _run_imported_native_workflow(service, job: dict, directory: Path, cancel: threading.Event,
                                  binding: dict, accepted_source: dict | None) -> dict:
    import numpy as np
    from .source_reference import SourceReferenceError, build_source_reference

    resume = accepted_source is not None
    if resume:
        source = accepted_source
        configuration = source["configuration"]
        model_binding = source.get("modelBinding")
        source_ref = source.get("sourceRef")
        if (not isinstance(model_binding, dict) or model_binding.get("kind") != "native-snapshot" or
                not isinstance(source_ref, dict)):
            raise WorkflowError("RESUME_UNAVAILABLE", "Accepted workflow has no immutable model source lineage")
        source_id = model_binding.get("sourceId")
        additional = job["parameters"]["additionalSteps"]
        total = source["totalSteps"] + additional
        mode = source["mode"]
        linear_path = _verified_path(source, "linearPath")
        linear_input_path = _verified_path(source, "linearInputPath")
        if (not _native_sha256(source.get("linearInputSha256")) or
                file_hash(linear_input_path) != source["linearInputSha256"]):
            raise WorkflowError("SOURCE_CHANGED", "Original native linear-export input changed")
        gains_path = _verified_path(source, "gainsPath")
        controller_path = _verified_path(source, "controllerPath")
        controller_host_path = _verified_path(source, "controllerHostPath")
        analysis_path = _verified_path(source, "analysisPath")
        reference_path = _verified_path(source, "sourceReferencePath")
        source_snapshot_copy = Path(source_ref.get("jobSnapshotPath", ""))
        source_manifest_copy = Path(source_ref.get("jobManifestPath", ""))
        if (not source_snapshot_copy.is_file() or source_snapshot_copy.is_symlink() or
                file_hash(source_snapshot_copy) != source_ref.get("jobSnapshotSha256") or
                not source_manifest_copy.is_file() or source_manifest_copy.is_symlink() or
                file_hash(source_manifest_copy) != source_ref.get("jobManifestSha256")):
            raise WorkflowError("SOURCE_CHANGED", "Original imported source snapshot or manifest changed")
        prepared_inputs = source.get("preparedInputs")
        if not isinstance(prepared_inputs, dict) or set(prepared_inputs) != {"active", "passive"} or any(
                not isinstance(prepared_inputs.get(name), dict) or
                not _native_path(prepared_inputs[name].get("path")) or
                not _native_sha256(prepared_inputs[name].get("sha256"))
                for name in ("active", "passive")):
            raise WorkflowError("SOURCE_CHANGED", "Original prepared active/passive inputs are unavailable")
        for prepared in prepared_inputs.values():
            path = Path(prepared["path"])
            if path.is_symlink() or not path.is_file() or file_hash(path) != prepared["sha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Original prepared source input changed")
    else:
        source = None
        configuration = job["configuration"]
        source_id = configuration.get("sourceId")
        if not isinstance(source_id, str) or not source_id:
            raise WorkflowError("INVALID_PARAMETERS", "Imported workflow requires a registered sourceId")
        additional = total = configuration["steps"]
        mode = job["parameters"]["mode"]
        source_snapshot_copy = directory / "source.rbm"
        source_manifest_copy = directory / "source-manifest.json"
        prepared_inputs = {}
        controller_path = directory / "stage9_velocity_controller.py"
        controller_host_path = directory / "controller_host.py"
        analysis_script = directory / "stage9_analysis.py"
        for key, target in (("controller", controller_path), ("analysis", analysis_script),
                            ("controllerHost", controller_host_path)):
            _copy_bound(Path(binding[key]["path"]), target, binding[key]["sha256"])
        linear_path, gains_path, analysis_path = (directory / name for name in
                                                   ("linear.json", "gains.json", "analysis.json"))
        linear_input_path = directory / "active-linear-input.ram"
        reference_path = directory / "source-reference.json"

    registered, model_binding = _read_registered_source(
        service, source_id, binding, source.get("modelBinding") if resume else None)
    descriptor = registered.get("descriptor")
    model_kind = registered.get("descriptor", {}).get("modelKind")
    if model_kind not in ("four-rod", "cantilever-beam") or configuration.get("model") != model_kind:
        raise WorkflowError("UNSUPPORTED_MODEL", "Registered source kind differs from workflow configuration")

    if resume:
        if (source_ref.get("sourceId") != source_id or source_ref.get("originRef") != registered["originRef"] or
                source_ref.get("registeredModelSha256") != registered["registeredModelSha256"] or
                source_ref.get("sourceManifestSha256") != registered["manifestSha256"] or
                not _same_path(source_ref.get("registeredPath"), registered["path"]) or
                not _same_path(source_ref.get("registeredManifestPath"), registered["manifestPath"])):
            raise WorkflowError("SOURCE_CHANGED", "Original source reference no longer matches the registry")
        reference = _read(reference_path)
        linear = _read(linear_path)
        try:
            recomputed = build_source_reference(descriptor, linear)
        except SourceReferenceError as error:
            raise WorkflowError("SOURCE_REFERENCE_FAILED", str(error)) from error
        if reference != recomputed or source.get("sourceReference") != recomputed:
            raise WorkflowError("SOURCE_CHANGED", "Stored independent source reference differs from its inputs")
        original_reference_path = reference_path
    else:
        if registered["registeredModelSha256"] != binding["modelInput"]["sha256"]:
            raise WorkflowError("SOURCE_CHANGED", "Bound model input hash differs from its registered snapshot")
        _copy_bound(Path(binding["modelInput"]["path"]), source_snapshot_copy,
                    registered["registeredModelSha256"])
        _copy_bound(Path(binding["sourceManifest"]["path"]), source_manifest_copy,
                    registered["manifestSha256"])
        source_ref = {
            "sourceId": source_id, "originRef": registered["originRef"],
            "registeredModelSha256": registered["registeredModelSha256"],
            "sourceManifestSha256": registered["manifestSha256"],
            "registeredPath": registered["path"], "registeredManifestPath": registered["manifestPath"],
            "jobSnapshotPath": str(source_snapshot_copy),
            "jobSnapshotSha256": file_hash(source_snapshot_copy),
            "jobManifestPath": str(source_manifest_copy),
            "jobManifestSha256": file_hash(source_manifest_copy),
        }
        source_sha = registered["registeredModelSha256"]
        for variant in ("active", "passive"):
            native_path = directory / (variant + "-prepared.ram")
            args = ["prepare-source", "--input", str(source_snapshot_copy), "--expected-sha", source_sha,
                    "--output", str(native_path), "--steps", str(total), "--dt", str(configuration["dt"])]
            if variant == "passive":
                args.append("--passive")
            prepared = _native(service, directory, "prepare-source-" + variant, cancel, args)
            expected_prepared = prepared.get("preparedSha256")
            if (prepared.get("sourceSha256") != source_sha or prepared.get("output") != str(native_path) or
                    not _same_path(prepared.get("sourcePath"), source_snapshot_copy) or
                    prepared.get("resume") is not False or file_hash(native_path) != expected_prepared or
                    not any(_same_path(item.get("path"), source_snapshot_copy) and item.get("sha256") == source_sha
                            for item in prepared.get("sourceFiles", []))):
                raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native source preparation did not preserve its hash binding")
            prepared_inputs[variant] = {"path": str(native_path), "sha256": expected_prepared}

        _copy_bound(Path(prepared_inputs["active"]["path"]), linear_input_path,
                    prepared_inputs["active"]["sha256"])
        service.runner.run([str(service.solver), "--file=" + str(linear_input_path),
                            "--linear-model-output=" + str(linear_path)], directory, "linear-export", cancel)
        if file_hash(linear_input_path) != prepared_inputs["active"]["sha256"]:
            raise WorkflowError("SOURCE_CHANGED", "Native linear export modified its prepared source input")
        linear = _read(linear_path)
        try:
            reference = build_source_reference(descriptor, linear)
        except SourceReferenceError as error:
            raise WorkflowError("SOURCE_REFERENCE_FAILED", str(error)) from error
        if reference.get("sourceSha256") != source_sha:
            raise WorkflowError("SOURCE_REFERENCE_FAILED", "Descriptor source hash differs from registered snapshot")
        reference_path.write_bytes(canonical(reference))
        original_reference_path = reference_path
        analysis_definition = reference["analysis"]
        service.runner.run([str(service.python), str(analysis_script), "--model", str(linear_path),
                            "--output", str(analysis_path), "--modes", str(analysis_definition["modes"]),
                            "--boundary-indices-json", canonical(analysis_definition["boundaryIndices"]).decode(),
                            "--frequencies-json", "[0,5,15,30]"],
                           directory, "analysis", cancel)
        if _read(analysis_path).get("errors"):
            raise WorkflowError("ANALYSIS_FAILED", "Independent linear analysis reported errors")
        controller = reference["controller"]
        service.runner.run([str(service.python), str(controller_path), "--model", str(linear_path),
                            "--target-damping", str(configuration["targetDamping"]),
                            "--sensor-id", controller["sensorId"], "--actuator-id", controller["actuatorId"],
                            "--output", str(gains_path)], directory, "gain-design", cancel)

    linear = _read(linear_path)
    dofs = linear["dofs"]
    initial_q = reference["initialQ"]
    initial_v = reference["initialV"]
    if len(dofs) != len(initial_q) or len(dofs) != len(initial_v):
        raise WorkflowError("SOURCE_REFERENCE_FAILED", "Source initial state does not match exported coordinates")
    replay_path = directory / "reference.json"
    replay_args = [str(service.python), str(controller_path), "--replay-model", str(linear_path),
                   "--gains", str(gains_path), "--dt", str(configuration["dt"]), "--steps", str(total),
                   "--initial-q-json", canonical(initial_q).decode(), "--output", str(replay_path)]
    replay_args += ["--initial-v-json", canonical(initial_v).decode()]
    service.runner.run(replay_args, directory, "independent-reference", cancel)
    replay = _read(replay_path)
    result = {"jobId": job["jobId"], "totalSteps": total, "dtSeconds": configuration["dt"],
              "mode": mode, "configuration": configuration, "independentReference": True,
              "model": model_kind, "modelKind": model_kind, "dofs": dofs,
              "modelBinding": model_binding, "sourceRef": source_ref,
              "sourceReference": reference, "sourceReferencePath": str(original_reference_path),
              "preparedInputs": prepared_inputs,
              "referencePath": str(replay_path), "linearPath": str(linear_path),
              "linearInputPath": str(linear_input_path), "linearInputSha256": file_hash(linear_input_path),
              "gainsPath": str(gains_path), "controllerPath": str(controller_path),
              "controllerHostPath": str(controller_host_path), "analysisPath": str(analysis_path)}
    if not resume:
        result["sourceArtifacts"] = {name: {"path": str(path), "sha256": file_hash(path)} for name, path in
                                     (("sourceModel", source_snapshot_copy), ("sourceManifest", source_manifest_copy),
                                      ("activePreparedInput", Path(prepared_inputs["active"]["path"])),
                                      ("passivePreparedInput", Path(prepared_inputs["passive"]["path"])),
                                      ("linearInput", linear_input_path),
                                      ("linear", linear_path), ("gains", gains_path),
                                      ("controller", controller_path), ("controllerHost", controller_host_path),
                                      ("analysis", analysis_path), ("sourceReference", original_reference_path))}
    else:
        result["sourceArtifacts"] = source["sourceArtifacts"]
        result["modelBinding"] = source["modelBinding"]
        result["sourceRef"] = source["sourceRef"]
        result["sourceReference"] = source["sourceReference"]
        result["sourceReferencePath"] = source["sourceReferencePath"]
        result["preparedInputs"] = source["preparedInputs"]

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
                     "--steps", str(additional), "--dt", str(configuration["dt"]),
                     "--output-frequency", "1", "--resume"])
        else:
            _copy_bound(Path(prepared_inputs[variant]["path"]), native_path,
                        prepared_inputs[variant]["sha256"])
        args = [str(service.solver), "--file=" + str(native_path)]
        if resume:
            args.append("--restart")
        if variant == "active":
            args += ["--control-program=" + str(service.python), "--control-timeout-ms=10000",
                     "--control-args-json=" + canonical([str(controller_host_path), "--implementation", str(controller_path),
                                                         "--gains", str(gains_path)]).decode()]
        environment = os.environ.copy()
        prepared_sha = (prepared_inputs[variant]["sha256"] if not resume else
                        source["preparedInputs"][variant]["sha256"])
        environment["RANGE_WORKFLOW_INPUT_SHA256"] = file_hash(native_path)
        service.runner.run(args, directory, "solve-" + variant, cancel, env=environment)
        accepted = _native(service, directory, "inspect-" + variant, cancel,
                           ["inspect", "--input", str(native_path)])
        expected_step = total
        if accepted.get("checkpointAvailable") is not True or accepted["step"] != expected_step:
            raise WorkflowError("INCOMPLETE_RESULT", "Solver did not publish the requested accepted checkpoint")
        if abs(accepted["time"] - total * configuration["dt"]) > 1e-8:
            raise WorkflowError("INCOMPLETE_RESULT", "Accepted checkpoint time differs from requested trajectory")
        checkpoint_path = Path(accepted.get("sourcePath", str(native_path)))
        if not checkpoint_path.resolve().is_relative_to(directory.resolve()):
            raise WorkflowError("ARTIFACT_PATH", "Accepted checkpoint escaped its job directory")
        q, v = _source_dof_state(linear, accepted)
        if not np.isfinite(q).all() or not np.isfinite(v).all():
            raise WorkflowError("NONFINITE_RESULT", "Native FE result contains non-finite values")
        predicted = replay["fullOrder" if variant == "active" else "passiveFullOrder"]
        q_difference = np.abs(q - np.asarray(predicted["q"], dtype=float))
        v_difference = np.abs(v - np.asarray(predicted["v"], dtype=float))
        translations = np.asarray([item["unit"] == "m" for item in dofs])
        rotations = np.asarray([item["unit"] == "rad" for item in dofs])
        q_error = float(np.max(q_difference[translations])) if np.any(translations) else 0.0
        v_error = float(np.max(v_difference[translations])) if np.any(translations) else 0.0
        rotation_error = float(np.max(q_difference[rotations])) if np.any(rotations) else 0.0
        angular_velocity_error = float(np.max(v_difference[rotations])) if np.any(rotations) else 0.0
        if q_error > 1e-8 or v_error > 1e-7 or rotation_error > 1e-8 or angular_velocity_error > 1e-7:
            raise WorkflowError("REFERENCE_MISMATCH", "Native FE result disagrees with the independent Newmark reference")
        result[variant] = {"q": q.tolist(), "v": v.tolist(),
                           "energyJ": float(0.5 * (q @ stiffness @ q + v @ mass @ v)),
                           "maxDisplacementErrorM": q_error, "maxVelocityErrorMPerSecond": v_error,
                           "maxRotationErrorRad": rotation_error,
                           "maxAngularVelocityErrorRadPerSecond": angular_velocity_error,
                           "inputSnapshotSha256": environment["RANGE_WORKFLOW_INPUT_SHA256"],
                           "preparedInputPath": str(prepared_inputs[variant]["path"]),
                           "preparedInputSha256": prepared_sha,
                           "nativeInputPath": str(native_path),
                           "checkpointPath": str(checkpoint_path), "checkpointSha256": file_hash(checkpoint_path)}
    if mode == "both":
        passive_energy = result["passive"]["energyJ"]
        if passive_energy <= 0:
            raise WorkflowError("INVALID_ENERGY", "Passive reference energy must be positive")
        result["controlledToPassiveEnergyRatio"] = result["active"]["energyJ"] / passive_energy
    (directory / "verification.json").write_bytes(canonical(result))
    return result


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
        if source.get("modelBinding", {}).get("kind") == "native-snapshot":
            return _run_imported_native_workflow(service, job, directory, cancel, binding, source)
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
        if configuration.get("sourceId") is not None:
            return _run_imported_native_workflow(service, job, directory, cancel, binding, None)
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
