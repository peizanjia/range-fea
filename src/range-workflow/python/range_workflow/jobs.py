"""Actual native FE solve, accepted checkpoint resume and independent replay."""
from __future__ import annotations

import copy
import json
import hashlib
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


def _publish_native_results(service, job, result, variant, native_path, accepted, binding,
                            directory, cancel):
    """Freeze actual numbered MODEL frames with declared software-input provenance."""
    from .native_results import NativeResultsClient

    executable = getattr(service, "native_results", None)
    if executable is None:
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE", "Native results executable is required for result publication")
    result_directory = native_path.parent
    terminal = Path(result[variant]["checkpointPath"])
    terminal_sha = result[variant]["checkpointSha256"]
    if file_hash(terminal) != terminal_sha:
        raise WorkflowError("SOURCE_CHANGED", "Accepted terminal MODEL changed before native result publication")
    source_ref = result.get("sourceRef")
    if source_ref:
        snapshot_ref = {"usesOpenDocument": True, **source_ref["originRef"],
                        "sourceId": source_ref["sourceId"],
                        "registeredModelSha256": source_ref["registeredModelSha256"]}
        runtime_stamp = source_ref["originRef"]["instanceRef"]
    else:
        snapshot_ref = result["templateSourceRef"]
        runtime_stamp = {"kind": "workflow-template", "model": result["model"]}
    software = {key: binding[key] for key in ("solver", "native", "python", "nativeResults") if key in binding}
    runtime = {"scope": "declared-software-inputs/v1", "inputs": software,
               "fingerprintSha256": hashlib.sha256(canonical(software)).hexdigest()}
    control_enabled = variant == "active"
    if accepted.get("controlEnabled") is not control_enabled:
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL control mode differs from the workflow variant")
    controller_id = accepted.get("controllerId")
    if not _native_controller_id(controller_id, control_enabled):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL has invalid persisted controller identity")
    control_inputs = {key: {"path": result[key + "Path"],
                            "sha256": file_hash(Path(result[key + "Path"]))}
                      for key in ("controller", "controllerHost", "gains")}
    control = {"enabled": control_enabled, "controllerId": controller_id,
               "controllerVersionSha256": hashlib.sha256(canonical(control_inputs)).hexdigest() if control_enabled else ""}
    if control_enabled:
        control["inputs"] = control_inputs
    input_sha = result[variant]["inputSnapshotSha256"]
    package = {"inputSnapshotSha256": input_sha, "sourceSnapshotRef": snapshot_ref,
               "runtime": runtime, "control": control}
    provenance = {"jobId": job["jobId"], "proofScope": "solver-and-declared-controller-runtime/v1",
                  "sourceRuntimeStamp": runtime_stamp, "sourceSnapshotRef": snapshot_ref,
                  "inputSnapshotSha256": input_sha,
                  "inputPackageSha256": hashlib.sha256(canonical(package)).hexdigest(),
                  "runtime": runtime, "control": control}
    history = result.get("nativeHistoryPrefix", {}).get(variant, [])
    if history:
        provenance["reusedHistory"] = [_native_history_proof(item) for item in history]
    provenance_path = result_directory / "native-result.provenance.json"
    provenance_path.write_bytes(canonical(provenance))
    raw = service.runner.run([str(executable), "publish", "--result-directory", str(result_directory),
                              "--latest", str(native_path), "--provenance-file", str(provenance_path)],
                             directory, "publish-native-" + variant, cancel)
    try:
        responses = [json.loads(line, object_pairs_hook=_unique_object) for line in raw.splitlines()
                     if line.lstrip().startswith(b"{")]
        if len(responses) != 1:
            raise ValueError()
        descriptor = responses[0]
        canonical(descriptor)
    except (ValueError, TypeError):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native result publication returned invalid JSON") from None
    manifest_path = result_directory / "native-result.manifest.json"
    if (not isinstance(descriptor, dict) or descriptor.get("valid") is not True or
            not _same_path(descriptor.get("manifestPath"), manifest_path) or
            not _native_sha256(descriptor.get("manifestSha256")) or
            file_hash(manifest_path) != descriptor["manifestSha256"]):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native result publication did not bind its manifest")
    manifest = _read(manifest_path)
    terminal_record = manifest.get("terminalModel", {})
    if (manifest.get("jobId") != job["jobId"] or manifest.get("artifactId") != descriptor.get("artifactId") or
            terminal_record.get("sha256") != terminal_sha or
            not _same_path(result_directory / terminal_record.get("relativePath", ""), terminal) or
            file_hash(terminal) != terminal_sha):
        raise WorkflowError("SOURCE_CHANGED", "Native manifest differs from the accepted terminal MODEL")
    catalog = NativeResultsClient(executable, manifest_path, descriptor["manifestSha256"]).query("catalog", {"limit": 1})
    if catalog.get("artifactId") != descriptor["artifactId"] or catalog.get("jobId") != job["jobId"]:
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Native catalog differs from its accepted job")
    artifacts = {"manifest": {"path": str(manifest_path), "sha256": descriptor["manifestSha256"]}}
    for index, frame in enumerate(manifest["frames"]):
        path = result_directory / frame["relativePath"]
        if (path.is_symlink() or not path.resolve().is_relative_to(result_directory.resolve()) or
                file_hash(path) != frame["sha256"]):
            raise WorkflowError("SOURCE_CHANGED", "Native frame differs from its immutable manifest")
        artifacts["frame/" + str(index)] = {"path": str(path), "sha256": frame["sha256"]}
    result.setdefault("nativeResults", {})[variant] = descriptor
    result.setdefault("nativeResultArtifacts", {})[variant] = artifacts


def _native_history_proof(item: dict) -> dict:
    required = ("recordNo", "originalFrameSha256", "originKind", "origin",
                "ancestryValidationScope", "relativePath", "sha256")
    if any(key not in item for key in required):
        raise WorkflowError("RECOVERY_CONFLICT", "Native history proof is incomplete")
    proof = {key: item[key] for key in required}
    for key in ("sourceRestartSha256", "sourceAttemptId", "journalEvidence", "checkpointEvidence"):
        if key in item:
            proof[key] = item[key]
    if item.get("originKind") == "reused-history":
        for key in ("originManifestPath", "originManifestSha256"):
            if key not in item:
                raise WorkflowError("RECOVERY_CONFLICT", "Reused native history lacks its ancestor manifest")
            proof[key] = item[key]
    return proof


def _native_sha256(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64 and
            all(char in "0123456789abcdef" for char in value))


def _native_path(value: object) -> bool:
    return isinstance(value, str) and bool(value) and Path(value).is_absolute()


def _native_source_files(value: object) -> bool:
    return (isinstance(value, list) and bool(value) and all(
        isinstance(item, dict) and _native_path(item.get("path")) and
        _native_sha256(item.get("sha256")) for item in value))


def _native_controller_id(value: object, enabled: bool) -> bool:
    if not isinstance(value, str):
        return False
    if not enabled:
        return value == ""
    return (bool(value) and len(value) <= 128 and value.strip() == value and
            not any(character.isspace() or character in ('\x00', '"') for character in value))


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
            _native_controller_id(result.get("controllerId"), result["controlEnabled"]) and
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


def _validate_wf05_inspection(accepted: dict, variant: str) -> None:
    """Require persisted control and FE fingerprints without inventing a revision field."""
    enabled = variant == "active"
    if accepted.get("controlAvailable") is not enabled:
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL control-state availability differs from its variant")
    for key in ("meshFingerprint", "dofFingerprint", "systemFingerprint"):
        if not _native_sha256(accepted.get(key)):
            raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL is missing a valid FE restart fingerprint")
    fingerprint = accepted.get("controlFingerprint")
    state_sha = accepted.get("controlControllerStateSha256")
    if enabled:
        if not _native_sha256(fingerprint) or not _native_sha256(state_sha):
            raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted active MODEL is missing its control-state fingerprints")
    elif fingerprint != "" or state_sha != "":
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted passive MODEL unexpectedly contains control state")
    for key in ("controlLastCommands", "controlPowers", "controlWork"):
        values = accepted.get(key)
        if (not isinstance(values, list) or
                not all(_native_finite_number(item) for item in values) or
                (enabled and len(values) != 1) or (not enabled and values)):
            raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL control ledger is incomplete")
    if not (len(accepted["controlLastCommands"]) == len(accepted["controlPowers"]) == len(accepted["controlWork"])):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Accepted MODEL control ledger lengths differ")


def _same_accepted_restart(left: dict, right: dict) -> bool:
    fields = ("step", "time", "dt", "q", "v", "a", "controlEnabled", "controllerId",
              "controlAvailable", "controlFingerprint",
              "controlControllerStateSha256", "controlLastCommands", "controlPowers", "controlWork",
              "systemFingerprint", "meshFingerprint", "dofFingerprint", "externalWorkJ",
              "dampingDissipationJ", "initialMechanicalEnergyJ", "supportReactions", "supportPowers",
              "supportWork", "nodes")
    return all(left.get(key) == right.get(key) for key in fields)


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


def _seed_recovery_checkpoint(selection: dict, destination: Path) -> None:
    """Copy the qualified checkpoint bundle so a new attempt remains recoverable."""
    try:
        source_directory = Path(selection.get("checkpointDirectory", "")).resolve(strict=True)
        model_source = Path(selection.get("checkpointPath", "")).resolve(strict=True)
        envelope_source = Path(selection.get("envelopePath", "")).resolve(strict=True)
        pointer_source = Path(selection.get("pointerPath", "")).resolve(strict=True)
    except (OSError, RuntimeError):
        raise WorkflowError("RECOVERY_CONFLICT", "Qualified checkpoint bundle is unavailable") from None
    envelope_name = selection.get("envelopeFile")
    if (not source_directory.is_absolute() or not model_source.is_absolute() or
            not envelope_source.is_absolute() or not pointer_source.is_absolute() or
            not isinstance(envelope_name, str) or Path(envelope_name).name != envelope_name or
            model_source.parent != source_directory or envelope_source.parent != source_directory or
            pointer_source != source_directory / "latest.json" or envelope_source.name != envelope_name):
        raise WorkflowError("RECOVERY_CONFLICT", "Qualified checkpoint bundle has inconsistent paths")
    if destination.is_symlink():
        raise WorkflowError("RECOVERY_CONFLICT", "New checkpoint directory cannot be a symlink")
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise WorkflowError("RECOVERY_CONFLICT", "New checkpoint directory is not empty")
    _copy_bound(model_source, destination / model_source.name, selection["checkpointSha256"])
    _copy_bound(envelope_source, destination / envelope_name, selection["envelopeSha256"])
    _copy_bound(pointer_source, destination / "latest.json", selection["pointerSha256"])


def _seed_recovery_history(selection: dict, variant_directory: Path, variant: str,
                           extension: str) -> tuple[list[dict], dict | None]:
    prefix = selection.get("historyPrefix", [])
    if not isinstance(prefix, list):
        raise WorkflowError("RECOVERY_CONFLICT", "Qualified accepted frame prefix is malformed")
    copied = []
    expected_record = 1
    for frame in prefix:
        if (not isinstance(frame, dict) or type(frame.get("recordNo")) is not int or
                frame["recordNo"] != expected_record or
                frame.get("relativePath") != f"{variant}-{expected_record:05d}.{extension}" or
                not _native_path(frame.get("sourcePath")) or
                not _native_sha256(frame.get("sha256"))):
            raise WorkflowError("RECOVERY_CONFLICT", "Qualified accepted frame prefix has invalid identity")
        source = Path(frame["sourcePath"])
        destination = variant_directory / frame["relativePath"]
        if destination.exists():
            if destination.is_symlink() or not destination.is_file() or file_hash(destination) != frame["sha256"]:
                raise WorkflowError("RECOVERY_CONFLICT", "A staged accepted frame differs from its qualified hash")
        else:
            _copy_bound(source, destination, frame["sha256"])
        copied.append({**copy.deepcopy(frame), "sourcePath": str(destination)})
        expected_record += 1

    selected_seq = selection.get("acceptedSeq")
    if type(selected_seq) is not int or selected_seq < 0 or len(copied) != selected_seq:
        raise WorkflowError("RECOVERY_CONFLICT", "Accepted frame prefix does not reach its qualified checkpoint")
    terminal = selection.get("terminalLink")
    if terminal is None:
        return copied, None
    if (not isinstance(terminal, dict) or terminal.get("recordNo") != selected_seq or
            not _native_path(terminal.get("linkPath")) or not _native_sha256(terminal.get("linkSha256")) or
            not _native_path(terminal.get("terminalPath")) or not _native_sha256(terminal.get("terminalSha256"))):
        raise WorkflowError("RECOVERY_CONFLICT", "Qualified terminal LINK proof is malformed")
    terminal_frame = variant_directory / f"{variant}-{selected_seq:05d}.{extension}"
    if not terminal_frame.is_file() or terminal_frame.is_symlink() or file_hash(terminal_frame) != terminal["terminalSha256"]:
        raise WorkflowError("RECOVERY_CONFLICT", "Terminal frame differs from its accepted prefix proof")
    logical_link = variant_directory / f"{variant}.{extension}"
    _copy_bound(Path(terminal["linkPath"]), logical_link, terminal["linkSha256"])
    return copied, {"linkPath": str(logical_link), "linkSha256": terminal["linkSha256"],
                    "terminalPath": str(terminal_frame), "terminalSha256": terminal["terminalSha256"],
                    "recordNo": selected_seq}


def _seed_manifest_history(service, source, variant_directory, variant, extension):
    from .recovery import _regular_bound
    from .native_results import NativeResultsClient

    descriptor = source["nativeResults"][variant]
    if service.native_results is None:
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE", "Completed source history requires the native results broker")
    source_root = service.workspace / "jobs" / source["jobId"]
    manifest_path = _regular_bound(descriptor["manifestPath"], source_root, descriptor["manifestSha256"])
    catalog = NativeResultsClient(service.native_results, manifest_path, descriptor["manifestSha256"]).query(
        "catalog", {"limit": 1})
    if catalog.get("jobId") != source["jobId"] or catalog.get("artifactId") != descriptor.get("artifactId"):
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Completed source history manifest identity changed")
    manifest = _read(manifest_path)
    frames = manifest.get("frames")
    if (not isinstance(frames, list) or not frames or not isinstance(frames[-1], dict) or
            manifest.get("jobId") != source["jobId"] or
            manifest.get("artifactId") != descriptor.get("artifactId") or
            frames[-1].get("recordNo") != source["totalSteps"]):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed source manifest does not end at its accepted target")
    terminal = manifest.get("terminalModel", {})
    if (terminal.get("sha256") != source.get(variant, {}).get("checkpointSha256") or
            _regular_bound(manifest_path.parent / terminal.get("relativePath", ""), source_root,
                           terminal.get("sha256")) != Path(source[variant]["checkpointPath"]).resolve(strict=True)):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed source manifest does not bind its accepted terminal MODEL")
    artifacts = source["nativeResultArtifacts"][variant]
    reused = []
    previous_record = 0
    for frame_index, frame in enumerate(frames):
        if (not isinstance(frame, dict) or type(frame.get("recordNo")) is not int or
                frame["recordNo"] <= previous_record or frame["recordNo"] > source["totalSteps"] or
                not isinstance(frame.get("relativePath"), str)):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed source frame sequence is invalid")
        record_no = frame["recordNo"]
        original = _regular_bound(manifest_path.parent / frame["relativePath"], source_root, frame.get("sha256"))
        proof_ref = artifacts.get("frame/" + str(frame_index))
        if (not isinstance(proof_ref, dict) or proof_ref.get("sha256") != frame["sha256"] or
                not Path(proof_ref.get("path", "")).resolve(strict=True) == original):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed source frame is absent from its artifact ledger")
        name = f"{variant}-{record_no:05d}.{extension}"
        destination = variant_directory / name
        _copy_bound(original, destination, frame["sha256"])
        reused.append({"recordNo": record_no, "sourcePath": str(destination), "relativePath": name,
                       "sha256": file_hash(destination), "originalFrameSha256": frame["sha256"],
                       "originKind": "reused-history", "origin": copy.deepcopy(frame.get("origin")),
                       "originManifestPath": str(manifest_path),
                       "originManifestSha256": descriptor["manifestSha256"],
                       "ancestryValidationScope": "captured-source-input",
                       "sourceAttemptId": source["jobId"]})
        previous_record = record_no
    return reused


def _prepare_restart_input(service, directory: Path, cancel, label: str,
                           checkpoint: Path, output: Path, args: list[str]) -> dict:
    checkpoint_sha = file_hash(checkpoint)
    prepared = _native(service, directory, label, cancel, args)
    expected = prepared.get("sha256")
    if (prepared.get("resume") is not True or prepared.get("sourceSha256") != checkpoint_sha or
            not _same_path(prepared.get("sourcePath"), checkpoint) or
            not _same_path(prepared.get("output"), output) or not _native_sha256(expected) or
            file_hash(output) != expected or
            not any(_same_path(item.get("path"), checkpoint) and item.get("sha256") == checkpoint_sha
                    for item in prepared.get("sourceFiles", []))):
        raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native restart preparation did not preserve its checkpoint")
    if file_hash(checkpoint) != checkpoint_sha:
        raise WorkflowError("SOURCE_CHANGED", "Accepted recovery checkpoint changed during preparation")
    return prepared


def _native_result_journal_environment(job, variant_directory):
    journal = variant_directory / "native-result.journal.jsonl"
    if journal.exists() or journal.is_symlink():
        raise WorkflowError("ARTIFACT_PATH", "Native result journal path already exists")
    environment = os.environ.copy()
    environment["RANGE_NATIVE_JOB_ID"] = job.get("attemptId", job["jobId"])
    environment["RANGE_NATIVE_RESULT_JOURNAL_PATH"] = str(journal.resolve())
    environment["RANGE_NATIVE_RESULT_DIRECTORY"] = str(variant_directory.resolve(strict=True))
    return environment, journal


def _verify_native_result_journal(service, job, journal_path, variant_directory, variant,
                                  extension, first_new_record, target, dt):
    from .recovery import _frame_journal

    root = service.workspace / "jobs" / job["jobId"]
    parsed = _frame_journal(journal_path, root, job.get("attemptId", job["jobId"]))
    if parsed is None:
        raise WorkflowError("NATIVE_RESULT_JOURNAL_UNAVAILABLE", "Native solver did not publish its bounded frame journal")
    records, journal_sha = parsed
    expected_range = range(first_new_record, target + 1)
    if any(number > target for number in records):
        raise WorkflowError("NATIVE_RESULT_JOURNAL_CONFLICT", "Native frame journal contains records beyond the approved target")
    for record_no in expected_range:
        record = records.get(record_no)
        expected_name = f"{variant}-{record_no:05d}.{extension}"
        if (not isinstance(record, dict) or record.get("seq") != record_no - first_new_record + 1 or
                record.get("relativePath") != expected_name or
                abs(record.get("timeS", float("nan")) - record_no * dt) >
                1e-12 * max(1.0, abs(record_no * dt))):
            raise WorkflowError("NATIVE_RESULT_JOURNAL_INCOMPLETE", "Native result journal does not cover every newly accepted frame")
        frame = variant_directory / expected_name
        if file_hash(frame) != record["sha256"]:
            raise WorkflowError("SOURCE_CHANGED", "Native accepted frame changed after journaling")
    return {"path": str(journal_path), "sha256": journal_sha,
            "firstRecordNo": first_new_record, "lastRecordNo": target,
            "frameCount": max(0, target - first_new_record + 1)}


def _verified_path(result: dict, key: str) -> Path:
    value = Path(result[key])
    if not value.is_absolute() or value.is_symlink() or not value.is_file():
        raise WorkflowError("SOURCE_CHANGED", "Original trajectory artifact is unavailable")
    return value


def _verify_recovery_base(service, job, source):
    from .recovery import verify_artifact_reference
    artifacts = source.get("sourceArtifacts")
    prepared = source.get("preparedInputs")
    if not isinstance(artifacts, dict) or not isinstance(prepared, dict):
        raise WorkflowError("RECOVERY_CHANGED", "Immutable recovery preparation is unavailable")
    for artifact in artifacts.values():
        if not isinstance(artifact, dict):
            raise WorkflowError("RECOVERY_CHANGED", "Recovery source artifact is malformed")
        verify_artifact_reference(service, job, artifact.get("path", ""), artifact.get("sha256"))
    if set(prepared) != {"active", "passive"}:
        raise WorkflowError("RECOVERY_CHANGED", "Frozen active/passive inputs are unavailable")
    for artifact in prepared.values():
        if not isinstance(artifact, dict):
            raise WorkflowError("RECOVERY_CHANGED", "Frozen workflow input is malformed")
        verify_artifact_reference(service, job, artifact.get("path", ""), artifact.get("sha256"))
    selection = source.get("recoveryQualification", {})
    for variant, chosen in selection.get("variants", {}).items():
        if not isinstance(chosen, dict):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified variant source is malformed")
        for frame in chosen.get("historyPrefix", []):
            if not isinstance(frame, dict):
                raise WorkflowError("RECOVERY_CHANGED", "Qualified history frame proof is malformed")
            verify_artifact_reference(service, job, frame.get("sourcePath", ""), frame.get("sha256"))
            journal = frame.get("journalEvidence")
            if journal is not None:
                if not isinstance(journal, dict):
                    raise WorkflowError("RECOVERY_CHANGED", "Qualified native journal proof is malformed")
                verify_artifact_reference(service, job, journal.get("path", ""), journal.get("sha256"))
        terminal = chosen.get("terminalLink")
        if terminal is not None:
            if not isinstance(terminal, dict):
                raise WorkflowError("RECOVERY_CHANGED", "Qualified terminal LINK proof is malformed")
            verify_artifact_reference(service, job, terminal.get("linkPath", ""), terminal.get("linkSha256"))
            verify_artifact_reference(service, job, terminal.get("terminalPath", ""), terminal.get("terminalSha256"))


def _initialize_attempt_baseline(service, job, directory, result, binding, mode, recovering, source, extension):
    from .recovery import checkpoint_context, record_progress, selection_summary

    contexts, states = {}, {}
    selections = source.get("recoveryQualification", {}).get("variants", {}) if recovering else {}
    result.setdefault("nativeHistoryPrefix", {})
    for variant in ("active", "passive"):
        if mode not in (variant, "both"):
            continue
        variant_directory = directory / variant
        variant_directory.mkdir(exist_ok=True)
        checkpoint_directory = variant_directory / "checkpoints"
        checkpoint_directory.mkdir(exist_ok=True)
        selected = selections.get(variant, {}) if recovering else {}
        if recovering and selected.get("phase") not in ("completed", "prepared", "checkpointed"):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified recovery variant phase is unavailable")
        state = {"phase": "completed" if recovering and selected.get("phase") == "completed" else "prepared",
                 "checkpointDirectory": str(checkpoint_directory)}
        if recovering and selected.get("phase") == "completed":
            states[variant] = state
            continue
        context = (selected.get("context") if recovering else
                   checkpoint_context(binding, result, variant, Path(result["preparedInputs"][variant]["path"])))
        if not isinstance(context, dict):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified checkpoint context is unavailable")
        contexts[variant] = context
        prefix = []
        source_attempt_id = job.get("attemptId", job["jobId"])
        source_restart_sha = context["modelSha256"]
        if recovering and selected.get("phase") == "checkpointed":
            checkpoint = selected
            _seed_recovery_checkpoint(checkpoint, checkpoint_directory)
            prefix, terminal = _seed_recovery_history(selected, variant_directory, variant, extension)
            state["phase"] = "checkpointed"
            source_restart_sha = source[variant]["checkpointSha256"]
            source_attempt_id = selected.get("sourceAttemptId")
            if terminal is not None:
                state["terminalLink"] = terminal
                selected["terminalLink"] = copy.deepcopy(terminal)
        elif (job.get("command") == "workflow.resume" and isinstance(source, dict) and
              isinstance(source.get("nativeResults", {}).get(variant), dict) and
              isinstance(source.get("nativeResultArtifacts", {}).get(variant), dict)):
            prefix = _seed_manifest_history(service, source, variant_directory, variant, extension)
            source_attempt_id = source.get("jobId")
        state.update(context=context, historyPrefix=prefix,
                     historySourceRestartSha256=source_restart_sha,
                     checkpointSourceAttemptId=source_attempt_id)
        result["nativeHistoryPrefix"][variant] = copy.deepcopy(prefix)
        states[variant] = state
    inherited = None
    if recovering:
        inherited = selection_summary(source["recoveryQualification"])
        result["recoveryEvidence"] = copy.deepcopy(inherited)
    record_progress(service, job["jobId"], directory, result,
                    variant_states=states, inherited_selection=inherited)
    return contexts, selections


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
    recovering = job["command"] == "workflow.recover"
    if recovering and accepted_source is None:
        accepted_source = job.get("parameters", {}).get("_recoverySource")
    if recovering:
        if not isinstance(accepted_source, dict):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified recovery source is unavailable")
        source = accepted_source
        _verify_recovery_base(service, job, source)
        configuration = source["configuration"]
        model_binding = source.get("modelBinding")
        source_ref = source.get("sourceRef")
        if (not isinstance(model_binding, dict) or model_binding.get("kind") != "native-snapshot" or
                not isinstance(source_ref, dict)):
            raise WorkflowError("RESUME_UNAVAILABLE", "Accepted workflow has no immutable model source lineage")
        source_id = model_binding.get("sourceId")
        additional = 0
        total = source["totalSteps"]
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
        if not isinstance(prepared_inputs, dict) or set(prepared_inputs) != set(("active", "passive")):
            raise WorkflowError("SOURCE_CHANGED", "Original prepared active/passive inputs are unavailable")
        for prepared in prepared_inputs.values():
            path = Path(prepared["path"])
            if path.is_symlink() or not path.is_file() or file_hash(path) != prepared["sha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Original prepared source input changed")
    elif resume:
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
        linear_input_path = directory / "active-linear-input.rbm"
        reference_path = directory / "source-reference.json"

    registered, model_binding = _read_registered_source(
        service, source_id, binding, source.get("modelBinding") if (resume or recovering) else None)
    descriptor = registered.get("descriptor")
    model_kind = registered.get("descriptor", {}).get("modelKind")
    if model_kind not in ("four-rod", "cantilever-beam") or configuration.get("model") != model_kind:
        raise WorkflowError("UNSUPPORTED_MODEL", "Registered source kind differs from workflow configuration")

    if resume or recovering:
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
            native_path = directory / (variant + "-prepared.rbm")
            args = ["prepare-source", "--input", str(source_snapshot_copy), "--expected-sha", source_sha,
                    "--output", str(native_path), "--steps", str(total), "--dt", str(configuration["dt"])]
            if variant == "passive":
                args.append("--passive")
            prepared = _native(service, directory, "prepare-source-" + variant, cancel, args)
            expected_prepared = prepared.get("preparedSha256")
            if (prepared.get("sourceSha256") != source_sha or not _same_path(prepared.get("output"), native_path) or
                    not _same_path(prepared.get("sourcePath"), source_snapshot_copy) or
                    prepared.get("resume") is not False or file_hash(native_path) != expected_prepared or
                    not any(_same_path(item.get("path"), source_snapshot_copy) and item.get("sha256") == source_sha
                            for item in prepared.get("sourceFiles", []))):
                raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native source preparation did not preserve its hash binding")
            prepared_inputs[variant] = {"path": str(native_path), "sha256": expected_prepared}
            if variant == "active":
                port = descriptor["ports"]["force"]
                actuators = prepared.get("mapping", {}).get("actuators")
                if (not isinstance(actuators, list) or len(actuators) != 1 or
                        any(actuators[0].get(key) != port.get(key) for key in
                            ("id", "globalDirection", "unit", "minimum", "maximum")) or
                        actuators[0].get("targetNodePhysicalId") != port["nodePhysicalId"]):
                    raise WorkflowError("INVALID_NATIVE_RESPONSE", "Prepared actuator differs from the verified source force port")

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
    force_port = descriptor["ports"]["force"]
    replay_args += ["--force-minimum", str(force_port["minimum"]),
                    "--force-maximum", str(force_port["maximum"])]
    service.runner.run(replay_args, directory, "independent-reference", cancel)
    replay = _read(replay_path)
    if resume:
        resume_prepared = copy.deepcopy(prepared_inputs)
        for variant in ("active", "passive"):
            if mode not in (variant, "both"):
                continue
            checkpoint = Path(source[variant]["checkpointPath"])
            if file_hash(checkpoint) != source[variant]["checkpointSha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Accepted checkpoint changed")
            output = directory / (variant + "-resume-prepared.rbm")
            prepared = _prepare_restart_input(
                service, directory, cancel, "prepare-" + variant, checkpoint, output,
                ["prepare", "--input", str(checkpoint), "--output", str(output),
                 "--steps", str(additional), "--dt", str(configuration["dt"]),
                 "--output-frequency", "1", "--resume"])
            expected = prepared.get("sha256")
            resume_prepared[variant] = {"path": str(output), "sha256": expected}
        prepared_inputs = resume_prepared
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
    if not resume and not recovering:
        result["sourceArtifacts"] = {name: {"path": str(path), "sha256": file_hash(path)} for name, path in
                                     (("sourceModel", source_snapshot_copy), ("sourceManifest", source_manifest_copy),
                                      ("activePreparedInput", Path(prepared_inputs["active"]["path"])),
                                      ("passivePreparedInput", Path(prepared_inputs["passive"]["path"])),
                                      ("linearInput", linear_input_path),
                                      ("linear", linear_path), ("gains", gains_path),
                                      ("controller", controller_path), ("controllerHost", controller_host_path),
                                       ("analysis", analysis_path), ("sourceReference", original_reference_path))}
    elif resume:
        result["sourceArtifacts"] = copy.deepcopy(source["sourceArtifacts"])
        for variant, artifact in prepared_inputs.items():
            if artifact["path"].endswith("-resume-prepared.rbm"):
                result["sourceArtifacts"][variant + "ResumePreparedInput"] = {
                    "path": artifact["path"], "sha256": artifact["sha256"]}
    else:
        result["sourceArtifacts"] = source["sourceArtifacts"]
        result["modelBinding"] = source["modelBinding"]
        result["sourceRef"] = source["sourceRef"]
        result["sourceReference"] = source["sourceReference"]
        result["sourceReferencePath"] = source["sourceReferencePath"]
        result["preparedInputs"] = prepared_inputs

    if recovering:
        result["nativeResults"] = copy.deepcopy(source.get("nativeResults", {}))
        result["nativeResultArtifacts"] = copy.deepcopy(source.get("nativeResultArtifacts", {}))
        for variant in ("active", "passive"):
            if variant in source.get("recoveryQualification", {}).get("variants", {}) and \
                    source["recoveryQualification"]["variants"][variant].get("phase") == "completed":
                result[variant] = copy.deepcopy(source[variant])

    mass, stiffness = np.asarray(linear["M"], dtype=float), np.asarray(linear["K"], dtype=float)
    extension = "rbm"
    variant_contexts, selections = _initialize_attempt_baseline(
        service, job, directory, result, binding, mode, recovering, source, extension)

    for variant in ("active", "passive"):
        if mode not in (variant, "both"):
            continue
        variant_directory = directory / variant
        variant_directory.mkdir(exist_ok=True)
        native_path = variant_directory / (variant + ".rbm")
        recovery_selection = (source.get("recoveryQualification", {}).get("variants", {}).get(variant, {})
                              if recovering else {})
        if recovering and recovery_selection.get("phase") not in ("completed", "prepared", "checkpointed"):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified recovery variant phase is unavailable")
        if recovering and recovery_selection.get("phase") == "completed":
            continue
        recovery_remaining = total
        if recovering and recovery_selection.get("phase") == "checkpointed":
            checkpoint = Path(source[variant]["checkpointPath"])
            if file_hash(checkpoint) != source[variant]["checkpointSha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Qualified recovery checkpoint changed")
            recovery_remaining = recovery_selection["remainingSteps"]
            if recovery_remaining:
                _prepare_restart_input(
                    service, directory, cancel, "prepare-recovery-" + variant, checkpoint, native_path,
                    ["prepare", "--input", str(checkpoint), "--output", str(native_path),
                     "--steps", str(recovery_remaining), "--dt", str(configuration["dt"]),
                     "--output-frequency", "1", "--resume"])
            elif not native_path.is_file() or native_path.is_symlink():
                raise WorkflowError("RECOVERY_CONFLICT", "Qualified terminal LINK was not staged before progress commit")
        elif recovering and recovery_selection.get("phase") == "prepared":
            frozen = recovery_selection["preparedInput"]
            _copy_bound(Path(frozen["path"]), native_path, frozen["sha256"])
        elif resume:
            _copy_bound(Path(prepared_inputs[variant]["path"]), native_path,
                        prepared_inputs[variant]["sha256"])
        else:
            _copy_bound(Path(prepared_inputs[variant]["path"]), native_path,
                        prepared_inputs[variant]["sha256"])
        context = variant_contexts.get(variant)
        if context is not None:
            from .recovery import record_progress
            record_progress(service, job["jobId"], directory, result,
                            variant=variant, phase="running", context=context)
        args = [str(service.solver), "--file=" + str(native_path)]
        if resume or (recovering and recovery_selection.get("phase") == "checkpointed" and recovery_remaining > 0):
            args.append("--restart")
        if context is not None:
            args += ["--native-checkpoint-dir=" + str(variant_directory / "checkpoints"),
                     "--native-checkpoint-context-json=" + canonical(context).decode("utf-8")]
        if variant == "active":
            args += ["--control-program=" + str(service.python), "--control-timeout-ms=10000",
                     "--control-args-json=" + canonical([str(controller_host_path), "--implementation", str(controller_path),
                                                          "--gains", str(gains_path)]).decode()]
        environment = os.environ.copy()
        prepared_sha = prepared_inputs[variant]["sha256"]
        input_snapshot_sha = (variant_contexts[variant]["modelSha256"]
                              if recovering and recovery_remaining == 0 else file_hash(native_path))
        environment["RANGE_WORKFLOW_INPUT_SHA256"] = variant_contexts[variant]["modelSha256"]
        if not recovering or recovery_remaining > 0:
            journal_environment, journal_path = _native_result_journal_environment(job, variant_directory)
            environment.update(journal_environment)
            service.runner.run(args, directory, "solve-" + variant, cancel, env=environment)
            journal_evidence = _verify_native_result_journal(
                service, job, journal_path, variant_directory, variant, extension,
                (recovery_selection.get("acceptedSeq", 0) + 1 if recovering and
                 recovery_selection.get("phase") == "checkpointed" else
                 source[variant].get("acceptedSeq", source.get("totalSteps", 0)) + 1 if resume else 1),
                total, configuration["dt"])
            result.setdefault("nativeResultJournals", {})[variant] = journal_evidence
        else:
            checkpoint_path = Path(source[variant]["checkpointPath"])
            checkpoint_inspection = _native(service, directory, "inspect-recovery-checkpoint-" + variant,
                                             cancel, ["inspect", "--input", str(checkpoint_path)])
            accepted_link_inspection = _native(service, directory, "inspect-recovery-terminal-" + variant,
                                                cancel, ["inspect", "--input", str(native_path)])
            terminal_link = recovery_selection["terminalLink"]
            if (not _same_path(accepted_link_inspection.get("sourcePath"), terminal_link["terminalPath"]) or
                    file_hash(Path(terminal_link["terminalPath"])) != terminal_link["terminalSha256"] or
                    file_hash(native_path) != terminal_link["linkSha256"] or
                    checkpoint_inspection.get("step") != total or
                    not _same_accepted_restart(checkpoint_inspection, accepted_link_inspection)):
                raise WorkflowError("RECOVERY_CONFLICT", "Captured terminal LINK differs from the selected accepted checkpoint")
            accepted = accepted_link_inspection
        if not recovering or recovery_remaining > 0:
            accepted = _native(service, directory, "inspect-" + variant, cancel,
                               ["inspect", "--input", str(native_path)])
        expected_step = total
        if accepted.get("checkpointAvailable") is not True or accepted["step"] != expected_step:
            raise WorkflowError("INCOMPLETE_RESULT", "Solver did not publish the requested accepted checkpoint")
        if abs(accepted["time"] - total * configuration["dt"]) > 1e-8:
            raise WorkflowError("INCOMPLETE_RESULT", "Accepted checkpoint time differs from requested trajectory")
        if context is not None:
            _validate_wf05_inspection(accepted, variant)
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
        result[variant] = {"q": q.tolist(), "v": v.tolist(), "acceptedSeq": total,
                           "energyJ": float(0.5 * (q @ stiffness @ q + v @ mass @ v)),
                           "maxDisplacementErrorM": q_error, "maxVelocityErrorMPerSecond": v_error,
                           "maxRotationErrorRad": rotation_error,
                           "maxAngularVelocityErrorRadPerSecond": angular_velocity_error,
                           "inputSnapshotSha256": input_snapshot_sha,
                           "preparedInputPath": str(prepared_inputs[variant]["path"]),
                           "preparedInputSha256": prepared_sha,
                           "nativeInputPath": str(native_path),
                           "checkpointPath": str(checkpoint_path), "checkpointSha256": file_hash(checkpoint_path)}
        _publish_native_results(service, job, result, variant, native_path, accepted, binding, directory, cancel)
        if context is not None:
            result.get("nativeHistoryPrefix", {}).pop(variant, None)
            from .recovery import record_progress
            record_progress(service, job["jobId"], directory, result,
                            variant=variant, phase="completed", context=context)
    if mode == "both":
        passive_energy = result["passive"]["energyJ"]
        if passive_energy < 0:
            raise WorkflowError("INVALID_ENERGY", "Passive reference energy must be nonnegative")
        result["controlledToPassiveEnergyRatio"] = (
            result["active"]["energyJ"] / passive_energy if passive_energy > 0 else None)
    result.pop("nativeHistoryPrefix", None)
    (directory / "verification.json").write_bytes(canonical(result))
    record_progress(service, job["jobId"], directory, result)
    return result


def run_native_workflow(service, job: dict, directory: Path, cancel: threading.Event,
                        binding: dict) -> dict:
    import numpy as np
    from .recovery import record_progress
    # Only explicitly catalogued native FE templates enter this workflow.
    for key, artifact in binding.items():
        if key != "extension" and isinstance(artifact, dict) and "path" in artifact and \
                file_hash(Path(artifact["path"])) != artifact.get("sha256"):
            raise WorkflowError("SOURCE_CHANGED", "Approved tool/template changed before execution")
    resume = job["command"] == "workflow.resume"
    recovering = job["command"] == "workflow.recover"
    if recovering:
        source = job.get("parameters", {}).get("_recoverySource")
        if not isinstance(source, dict) or not isinstance(source.get("recoveryQualification"), dict):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified recovery source is unavailable")
        _verify_recovery_base(service, job, source)
        if source.get("modelBinding", {}).get("kind") == "native-snapshot":
            return _run_imported_native_workflow(service, job, directory, cancel, binding, None)
        configuration = source["configuration"]
        total = source["totalSteps"]
        additional = 0
        mode = source["mode"]
        model = configuration.get("model", "four-rod")
        if model not in TEMPLATES:
            raise WorkflowError("UNSUPPORTED_MODEL", "Recovered workflow template is unavailable")
        artifact_map = source.get("sourceArtifacts", {})
        template = Path(artifact_map["template"]["path"])
        template_source_ref = source["templateSourceRef"]
        definition = TEMPLATES[model]
        controller_path = _verified_path(source, "controllerPath")
        controller_host_path = _verified_path(source, "controllerHostPath")
        analysis_path = _verified_path(source, "analysisPath")
        linear_path = _verified_path(source, "linearPath")
        gains_path = _verified_path(source, "gainsPath")
        reference_path = _verified_path(source, "referencePath")
        prepared_inputs = source.get("preparedInputs")
        if not isinstance(prepared_inputs, dict) or set(prepared_inputs) != set(("active", "passive")):
            raise WorkflowError("SOURCE_CHANGED", "Original prepared active/passive inputs are unavailable")
        for prepared in prepared_inputs.values():
            path = Path(prepared["path"])
            if path.is_symlink() or not path.is_file() or file_hash(path) != prepared["sha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Original prepared source input changed")
    elif resume:
        source = service.result_snapshot(job["parameters"]["jobId"])
        if source.get("modelBinding", {}).get("kind") == "native-snapshot":
            return _run_imported_native_workflow(service, job, directory, cancel, binding, source)
        if not isinstance(source.get("templateSourceRef"), dict):
            raise WorkflowError("SOURCE_REFERENCE_UNAVAILABLE",
                                "This legacy result lacks native catalog source provenance; run the workflow again before resuming")
        configuration = source["configuration"]
        additional = job["parameters"]["additionalSteps"]
        total = source["totalSteps"] + additional
        mode = source["mode"]
        linear_path = _verified_path(source, "linearPath")
        gains_path = _verified_path(source, "gainsPath")
        controller_path = _verified_path(source, "controllerPath")
        controller_host_path = _verified_path(source, "controllerHostPath")
        analysis_path = _verified_path(source, "analysisPath")
        template_source = Path(source.get("templatePath", Path(source["linearPath"]).parent / "template.ram"))
        template = directory / "template.ram"
        _copy_bound(template_source, template, source["templateSourceRef"]["sourceSha256"])
        prepared_inputs = copy.deepcopy(source.get("preparedInputs", {}))
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
                 "--steps", str(total), "--dt", str(configuration["dt"]), "--output-frequency", "1"])
        active_prepared_path = directory / "active-prepared.ram"
        _copy_bound(directory / "active.ram", active_prepared_path, file_hash(directory / "active.ram"))
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
        passive_prepared_path = directory / "passive-prepared.ram"
        _native(service, directory, "prepare-passive", cancel,
                ["prepare", "--input", str(template), "--output", str(passive_prepared_path),
                 "--steps", str(total), "--dt", str(configuration["dt"]),
                 "--output-frequency", "1", "--passive"])
        prepared_inputs = {"active": {"path": str(active_prepared_path), "sha256": file_hash(active_prepared_path)},
                           "passive": {"path": str(passive_prepared_path), "sha256": file_hash(passive_prepared_path)}}
    linear = _read(linear_path)
    dofs = linear["dofs"]
    model = configuration.get("model", "four-rod")
    definition = TEMPLATES[model]
    expected_dofs = definition["dofs"]
    if len(dofs) != len(expected_dofs) or any(
            not _matches_template_dof(d, node, physical)
            for d, (node, physical) in zip(dofs, expected_dofs)):
        raise WorkflowError("UNSUPPORTED_MODEL", "Exported physical coordinates differ from the selected template")
    if recovering:
        reference = _read(reference_path)
    else:
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
    if resume:
        resume_prepared = copy.deepcopy(prepared_inputs)
        for variant in ("active", "passive"):
            if mode not in (variant, "both"):
                continue
            checkpoint = Path(source[variant]["checkpointPath"])
            if file_hash(checkpoint) != source[variant]["checkpointSha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Accepted checkpoint changed")
            output = directory / (variant + "-resume-prepared.ram")
            prepared = _prepare_restart_input(
                service, directory, cancel, "prepare-" + variant, checkpoint, output,
                ["prepare", "--input", str(checkpoint), "--output", str(output),
                 "--steps", str(additional), "--dt", str(configuration["dt"]),
                 "--output-frequency", "1", "--resume"])
            expected = prepared.get("sha256")
            resume_prepared[variant] = {"path": str(output), "sha256": expected}
        for variant in ("active", "passive"):
            if variant in resume_prepared:
                continue
            output = directory / (variant + "-prepared.ram")
            args = ["prepare", "--input", str(template), "--output", str(output),
                    "--steps", str(total), "--dt", str(configuration["dt"])]
            if variant == "passive":
                args.append("--passive")
            prepared = _native(service, directory, "prepare-" + variant, cancel, args)
            expected = prepared.get("sha256")
            if not _same_path(prepared.get("output"), output) or not _native_sha256(expected) or file_hash(output) != expected:
                raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native resume base preparation did not bind its input")
            resume_prepared[variant] = {"path": str(output), "sha256": expected}
        prepared_inputs = resume_prepared
    result = {"jobId": job["jobId"], "totalSteps": total, "dtSeconds": configuration["dt"],
              "mode": mode, "configuration": configuration, "independentReference": True,
              "model": model, "dofs": dofs,
              "templateSourceRef": source["templateSourceRef"] if (resume or recovering) else {
                   "usesOpenDocument": False, "kind": "template", "model": model,
                   "sourceSha256": binding["template"]["sha256"]},
              "templatePath": str(template), "preparedInputs": prepared_inputs,
              "referencePath": str(reference_path), "linearPath": str(linear_path),
              "gainsPath": str(gains_path), "controllerPath": str(controller_path),
              "controllerHostPath": str(controller_host_path),
              "analysisPath": str(analysis_path)}
    if recovering:
        result["sourceArtifacts"] = copy.deepcopy(source["sourceArtifacts"])
        result["templateSourceRef"] = source["templateSourceRef"]
        result["nativeResults"] = copy.deepcopy(source.get("nativeResults", {}))
        result["nativeResultArtifacts"] = copy.deepcopy(source.get("nativeResultArtifacts", {}))
        for variant in ("active", "passive"):
            if source.get("recoveryQualification", {}).get("variants", {}).get(variant, {}).get("phase") == "completed":
                result[variant] = copy.deepcopy(source[variant])
    elif resume:
        result["sourceArtifacts"] = copy.deepcopy(source.get("sourceArtifacts", {}))
        for name, path in (("template", template), ("linear", linear_path), ("gains", gains_path), ("controller", controller_path),
                           ("controllerHost", controller_host_path), ("analysis", analysis_path),
                           ("reference", reference_path)):
            result["sourceArtifacts"][name] = {"path": str(path), "sha256": file_hash(path)}
        for variant, artifact in prepared_inputs.items():
            if artifact["path"].endswith("-resume-prepared.ram"):
                result["sourceArtifacts"][variant + "ResumePreparedInput"] = {
                    "path": artifact["path"], "sha256": artifact["sha256"]}
    else:
        result["sourceArtifacts"] = {name: {"path": str(path), "sha256": file_hash(path)} for name, path in
                                     (("template", template), ("activePreparedInput", Path(prepared_inputs["active"]["path"])),
                                      ("passivePreparedInput", Path(prepared_inputs["passive"]["path"])),
                                      ("linear", linear_path), ("gains", gains_path), ("controller", controller_path),
                                      ("controllerHost", controller_host_path), ("analysis", analysis_path),
                                      ("reference", reference_path))}
    mass, stiffness = np.asarray(linear["M"], dtype=float), np.asarray(linear["K"], dtype=float)
    extension = "ram"
    variant_contexts, selections = _initialize_attempt_baseline(
        service, job, directory, result, binding, mode, recovering, source, extension)

    for variant in ("active", "passive"):
        if mode not in (variant, "both"):
            continue
        variant_directory = directory / variant
        variant_directory.mkdir(exist_ok=True)
        native_path = variant_directory / (variant + ".ram")
        recovery_selection = selections.get(variant, {}) if recovering else {}
        if recovering and recovery_selection.get("phase") not in ("completed", "prepared", "checkpointed"):
            raise WorkflowError("RECOVERY_CHANGED", "Qualified recovery variant phase is unavailable")
        if recovering and recovery_selection.get("phase") == "completed":
            continue
        recovery_remaining = total
        if recovering and recovery_selection.get("phase") == "checkpointed":
            checkpoint = Path(source[variant]["checkpointPath"])
            if file_hash(checkpoint) != source[variant]["checkpointSha256"]:
                raise WorkflowError("SOURCE_CHANGED", "Qualified recovery checkpoint changed")
            recovery_remaining = recovery_selection["remainingSteps"]
            if recovery_remaining:
                _prepare_restart_input(
                    service, directory, cancel, "prepare-recovery-" + variant, checkpoint, native_path,
                    ["prepare", "--input", str(checkpoint), "--output", str(native_path),
                     "--steps", str(recovery_remaining), "--dt", str(configuration["dt"]),
                     "--output-frequency", "1", "--resume"])
            elif not native_path.is_file() or native_path.is_symlink():
                raise WorkflowError("RECOVERY_CONFLICT", "Qualified terminal LINK was not staged before progress commit")
        elif recovering and recovery_selection.get("phase") == "prepared":
            frozen = recovery_selection["preparedInput"]
            _copy_bound(Path(frozen["path"]), native_path, frozen["sha256"])
        elif resume:
            _copy_bound(Path(prepared_inputs[variant]["path"]), native_path, prepared_inputs[variant]["sha256"])
        else:
            _copy_bound(Path(prepared_inputs[variant]["path"]), native_path, prepared_inputs[variant]["sha256"])
        context = variant_contexts.get(variant)
        if context is not None:
            record_progress(service, job["jobId"], directory, result,
                            variant=variant, phase="running", context=context)
        args = [str(service.solver), "--file=" + str(native_path)]
        restart = resume or (recovering and recovery_selection.get("phase") == "checkpointed" and recovery_remaining > 0)
        if restart:
            args.append("--restart")
        if context is not None:
            args += ["--native-checkpoint-dir=" + str(variant_directory / "checkpoints"),
                     "--native-checkpoint-context-json=" + canonical(context).decode("utf-8")]
        if variant == "active":
            args += ["--control-program=" + str(service.python), "--control-timeout-ms=10000",
                     "--control-args-json=" + canonical([str(controller_host_path), "--implementation", str(controller_path),
                                                          "--gains", str(gains_path)]).decode()]
        environment = os.environ.copy()
        input_snapshot_sha = (variant_contexts[variant]["modelSha256"]
                              if recovering and recovery_remaining == 0 else file_hash(native_path))
        environment["RANGE_WORKFLOW_INPUT_SHA256"] = variant_contexts[variant]["modelSha256"]
        if not recovering or recovery_remaining > 0:
            journal_environment, journal_path = _native_result_journal_environment(job, variant_directory)
            environment.update(journal_environment)
            service.runner.run(args, directory, "solve-" + variant, cancel, env=environment)
            journal_evidence = _verify_native_result_journal(
                service, job, journal_path, variant_directory, variant, extension,
                (recovery_selection.get("acceptedSeq", 0) + 1 if recovering and
                 recovery_selection.get("phase") == "checkpointed" else
                 source[variant].get("acceptedSeq", source.get("totalSteps", 0)) + 1 if resume else 1),
                total, configuration["dt"])
            result.setdefault("nativeResultJournals", {})[variant] = journal_evidence
        else:
            checkpoint_path = Path(source[variant]["checkpointPath"])
            checkpoint_inspection = _native(service, directory, "inspect-recovery-checkpoint-" + variant,
                                             cancel, ["inspect", "--input", str(checkpoint_path)])
            accepted = _native(service, directory, "inspect-recovery-terminal-" + variant,
                               cancel, ["inspect", "--input", str(native_path)])
            terminal_link = recovery_selection["terminalLink"]
            if (not _same_path(accepted.get("sourcePath"), terminal_link["terminalPath"]) or
                    file_hash(Path(terminal_link["terminalPath"])) != terminal_link["terminalSha256"] or
                    file_hash(native_path) != terminal_link["linkSha256"] or
                    checkpoint_inspection.get("step") != total or
                    not _same_accepted_restart(checkpoint_inspection, accepted)):
                raise WorkflowError("RECOVERY_CONFLICT", "Captured terminal LINK differs from the selected accepted checkpoint")
        if not recovering or recovery_remaining > 0:
            accepted = _native(service, directory, "inspect-" + variant, cancel, ["inspect", "--input", str(native_path)])
        if accepted.get("checkpointAvailable") is not True or accepted["step"] != total:
            raise WorkflowError("INCOMPLETE_RESULT", "Solver did not publish the requested accepted checkpoint")
        if abs(accepted["time"] - total * configuration["dt"]) > 1e-8:
            raise WorkflowError("INCOMPLETE_RESULT", "Accepted checkpoint time differs from requested trajectory")
        if context is not None:
            _validate_wf05_inspection(accepted, variant)
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
        result[variant] = {"q": q.tolist(), "v": v.tolist(), "acceptedSeq": total,
                           "energyJ": float(0.5 * (q @ stiffness @ q + v @ mass @ v)),
                           "maxDisplacementErrorM": q_error, "maxVelocityErrorMPerSecond": v_error,
                           "maxRotationErrorRad": rotation_error, "maxAngularVelocityErrorRadPerSecond": angular_velocity_error,
                           "inputSnapshotSha256": input_snapshot_sha,
                           "checkpointPath": str(checkpoint_path), "checkpointSha256": file_hash(checkpoint_path)}
        _publish_native_results(service, job, result, variant, native_path, accepted, binding, directory, cancel)
        if context is not None:
            result.get("nativeHistoryPrefix", {}).pop(variant, None)
            record_progress(service, job["jobId"], directory, result,
                            variant=variant, phase="completed", context=context)
    if mode == "both":
        passive_energy = result["passive"]["energyJ"]
        if passive_energy <= 0:
            raise WorkflowError("INVALID_ENERGY", "Passive reference energy must be positive")
        result["controlledToPassiveEnergyRatio"] = result["active"]["energyJ"] / passive_energy
    result.pop("nativeHistoryPrefix", None)
    (directory / "verification.json").write_bytes(canonical(result))
    record_progress(service, job["jobId"], directory, result)
    return result
