"""Durable attempt facts and qualification of complete native checkpoints."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import threading

from .service import WorkflowError, canonical, digest, file_hash, utc

SCHEMA = "range.workflow-recovery/v1"
POLICY = "accepted-steps-or-sim-time-v1"
CONTEXT_SCHEMA = "range.native-checkpoint-context/v1"
CHECKPOINT_SCHEMA = "range.native-checkpoint/v1"
POINTER_SCHEMA = "range.native-checkpoint-pointer/v1"
RESULT_FRAME_SCHEMA = "range.native-result-frame/v1"
HISTORY_PREFIX_SCHEMA = "range.workflow-history-prefix/v1"
NO_CONTROL_SHA256 = hashlib.sha256(b"range.native-checkpoint.no-control.v1").hexdigest()
_VARIANTS = ("active", "passive")
_PHASES = {"pending", "prepared", "running", "checkpointed", "completed"}


def _sha256(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64 and
            all(character in "0123456789abcdef" for character in value))


def _finite_number(value: object, *, positive: bool = False) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value) and (not positive or value > 0.0)
    except OverflowError:
        return False


def _field(hasher, value: bytes) -> None:
    hasher.update(str(len(value)).encode("ascii"))
    hasher.update(b":")
    hasher.update(value)


def _canonical_file(path: Path) -> tuple[bytes, bytes]:
    if not path.is_absolute() or not path.is_file():
        raise WorkflowError("SOURCE_CHANGED", "Controller runtime file is unavailable")
    try:
        canonical_path = path.resolve(strict=True).as_posix().encode("utf-8")
        sha = file_hash(path).encode("ascii")
    except (OSError, UnicodeError):
        raise WorkflowError("SOURCE_CHANGED", "Controller runtime file is unavailable") from None
    return canonical_path, sha


def controller_revision(program: Path, arguments: list[str]) -> str:
    """Mirror solver_task.cpp's length-delimited controllerRevision hash."""
    hasher = hashlib.sha256()
    _field(hasher, b"range-control-transport-v1")
    program_path, program_sha = _canonical_file(Path(program))
    _field(hasher, program_path)
    _field(hasher, program_sha)
    if not isinstance(arguments, list) or any(not isinstance(value, str) for value in arguments):
        raise WorkflowError("INVALID_PARAMETERS", "Controller arguments must be strings")
    for argument in arguments:
        _field(hasher, argument.encode("utf-8"))
        argument_path = argument
        if argument.startswith("--"):
            separator = argument.find("=")
            if separator > 2:
                argument_path = argument[separator + 1:]
        candidate = Path(argument_path)
        if candidate.is_file():
            canonical_path, sha = _canonical_file(candidate)
            _field(hasher, canonical_path)
            _field(hasher, sha)
    return hasher.hexdigest()


def control_arguments(result: dict) -> list[str]:
    """Return the exact --control-args-json vector used by workflow jobs."""
    flags = result.get("controllerInterpreterFlags", [])
    if flags not in ([], ["-B"]):
        raise WorkflowError("RECOVERY_CHANGED", "Controller interpreter policy is invalid")
    arguments = [*flags, str(result["controllerHostPath"]), "--implementation", str(result["controllerPath"]),
                 "--gains", str(result["gainsPath"])]
    if "runtimeParametersCodecPath" in result:
        arguments += ["--runtime-parameters-codec", str(result["runtimeParametersCodecPath"])]
    return arguments


def _package_sha256(binding: dict, result: dict, source_sha: str,
                    model_sha: str, control_sha: str) -> str:
    runtime = {key: binding[key] for key in ("solver", "native", "python", "nativeResults") if key in binding}
    control_keys = ("controller", "controllerHost", "gains") + (
        ("runtimeParametersCodec",) if "runtimeParametersCodecPath" in result else ())
    control_inputs = {key: result.get("sourceArtifacts", {}).get(key, {}).get("sha256")
                      for key in control_keys}
    return digest({"sourceSha256": source_sha, "modelSha256": model_sha,
                   "controlSha256": control_sha, "runtime": runtime,
                   "controlInputs": control_inputs,
                   "configuration": result.get("configuration"),
                   "targetSteps": result.get("totalSteps"), "dtS": result.get("dtSeconds")})


def checkpoint_context(binding: dict, result: dict, variant: str, native_path: Path,
                       *, revision: str | None = None) -> dict:
    if variant not in _VARIANTS:
        raise WorkflowError("INVALID_PARAMETERS", "Unknown native workflow variant")
    source_ref = result.get("sourceRef") or {}
    template_ref = result.get("templateSourceRef") or {}
    source_sha = source_ref.get("registeredModelSha256") or template_ref.get("sourceSha256")
    model_sha = file_hash(Path(native_path))
    if not _sha256(source_sha) or not _sha256(model_sha):
        raise WorkflowError("SOURCE_CHANGED", "Checkpoint source or prepared MODEL digest is unavailable")
    if variant == "active":
        if revision is None:
            revision = controller_revision(Path(binding["python"]["path"]), control_arguments(result))
        if not _sha256(revision):
            raise WorkflowError("INVALID_NATIVE_RESPONSE", "Native controller revision is invalid")
        control_sha = revision
    else:
        control_sha = NO_CONTROL_SHA256
    runtime_sha = binding.get("solver", {}).get("sha256")
    if not _sha256(runtime_sha):
        raise WorkflowError("SOURCE_CHANGED", "Solver runtime digest is unavailable")
    return {"schema": CONTEXT_SCHEMA, "sourceSha256": source_sha,
            "modelSha256": model_sha, "controlSha256": control_sha,
            "runtimeSha256": runtime_sha,
            "packageSha256": _package_sha256(binding, result, source_sha, model_sha, control_sha),
            "policy": POLICY, "maxAcceptedSteps": 10,
            "maxSimulationSeconds": 1.0, "dtS": result["dtSeconds"]}


def _valid_context(context: object) -> bool:
    if not isinstance(context, dict) or set(context) != {
            "schema", "sourceSha256", "modelSha256", "controlSha256", "runtimeSha256",
            "packageSha256", "policy", "maxAcceptedSteps", "maxSimulationSeconds", "dtS"}:
        return False
    return (context.get("schema") == CONTEXT_SCHEMA and
            all(_sha256(context.get(key)) for key in
                ("sourceSha256", "modelSha256", "controlSha256", "runtimeSha256", "packageSha256")) and
            context.get("policy") == POLICY and type(context.get("maxAcceptedSteps")) is int and
            context["maxAcceptedSteps"] == 10 and
            type(context.get("maxSimulationSeconds")) in (int, float) and
            context["maxSimulationSeconds"] == 1.0 and _finite_number(context.get("dtS"), positive=True))


def record_progress(service, job_id, directory, result, variant=None, phase=None, context=None,
                    *, variant_states=None, inherited_selection=None):
    """Persist immutable preparation and variant progress before side effects."""
    root = service.workspace / "jobs" / job_id
    attempt_directory = Path(directory)
    try:
        root_absolute = Path(os.path.abspath(root))
        relative = Path(os.path.abspath(attempt_directory)).relative_to(root_absolute)
        cursor = root_absolute
        if cursor.is_symlink():
            raise ValueError()
        for component in relative.parts:
            cursor = cursor / component
            if cursor.is_symlink():
                raise ValueError()
        if (not attempt_directory.is_absolute() or
                not attempt_directory.resolve(strict=True).is_relative_to(root.resolve(strict=True))):
            raise ValueError()
    except (OSError, ValueError):
        raise WorkflowError("ARTIFACT_PATH", "Recovery attempt directory escaped its logical job") from None
    if not isinstance(result, dict):
        raise WorkflowError("INVALID_STATE", "Recovery progress requires a workflow result object")
    if variant is not None:
        if variant not in _VARIANTS or phase not in _PHASES:
            raise WorkflowError("INVALID_STATE", "Recovery variant phase is invalid")
        if context is not None and not _valid_context(context):
            raise WorkflowError("INVALID_STATE", "Native checkpoint context is invalid")
    elif phase is not None or context is not None:
        raise WorkflowError("INVALID_STATE", "Base preparation cannot declare a variant phase")
    if variant_states is not None:
        if variant is not None or not isinstance(variant_states, dict) or any(
                name not in _VARIANTS or not isinstance(state, dict) or state.get("phase") not in _PHASES or
                (state.get("context") is not None and not _valid_context(state["context"]))
                for name, state in variant_states.items()):
            raise WorkflowError("INVALID_STATE", "Initial recovery variant baseline is invalid")
    if inherited_selection is not None and not isinstance(inherited_selection, dict):
        raise WorkflowError("INVALID_STATE", "Inherited recovery selection is malformed")

    with service._lock:
        candidate = copy.deepcopy(service._state)
        job = candidate["jobs"].get(job_id)
        if not isinstance(job, dict):
            raise WorkflowError("INVALID_STATE", "Recovery job is unavailable")
        recovery = job.setdefault("recovery", {"schema": SCHEMA, "attempts": []})
        if recovery.get("schema") != SCHEMA or not isinstance(recovery.get("attempts"), list):
            raise WorkflowError("INVALID_STATE", "Recovery attempt ledger is malformed")
        attempt_id = job.get("attemptId", job_id)
        attempts = recovery["attempts"]
        matching = [entry for entry in attempts if isinstance(entry, dict) and entry.get("attemptId") == attempt_id]
        if matching:
            if attempts[-1] is not matching[0]:
                raise WorkflowError("INVALID_STATE", "Progress cannot rewrite a historical attempt")
            attempt = matching[0]
        else:
            if len(attempts) >= 64:
                raise WorkflowError("ATTEMPT_LIMIT", "Recovery attempt history reached its limit")
            attempt = {"attemptId": attempt_id, "directory": str(attempt_directory),
                       "createdAt": utc(), "variants": {}}
            attempts.append(attempt)
            if inherited_selection is not None:
                attempt["inheritedSelection"] = copy.deepcopy(inherited_selection)
        attempt["previewId"] = job.get("previewId")
        if attempt.get("directory") != str(attempt_directory):
            raise WorkflowError("INVALID_STATE", "Recovery attempt directory changed")
        attempt["partialResult"] = copy.deepcopy(result)
        if variant is not None:
            variants = attempt.setdefault("variants", {})
            entry = variants.setdefault(variant, {})
            entry["phase"] = phase
            if phase == "completed":
                entry.pop("historyPrefix", None)
            if context is not None:
                entry["context"] = copy.deepcopy(context)
                entry["checkpointDirectory"] = str(attempt_directory / variant / "checkpoints")
        if variant_states is not None:
            variants = attempt.setdefault("variants", {})
            for name, state in variant_states.items():
                entry = variants.setdefault(name, {})
                for key in ("phase", "context", "historySourceRestartSha256", "checkpointSourceAttemptId",
                            "terminalLink"):
                    if key in state:
                        entry[key] = copy.deepcopy(state[key])
                entry["checkpointDirectory"] = str(state.get("checkpointDirectory",
                                                            attempt_directory / name / "checkpoints"))
                if "historyPrefix" in state:
                    entry["historyPrefix"] = _compact_history_prefix(state["historyPrefix"])
        service._commit(candidate, "job.recovery.record", phase or "prepared", job_id=job_id,
                        parameters={"attemptId": attempt_id, "variant": variant, "phase": phase,
                                    "variantCount": len(variant_states) if variant_states is not None else None,
                                    "inheritedAttemptId": inherited_selection.get("attemptId")
                                    if inherited_selection is not None else None})


def _regular_bound(path, root, expected=None, limit=128 * 1024 * 1024):
    candidate = Path(path)
    root = Path(root)
    try:
        root_absolute = Path(os.path.abspath(root))
        lexical = Path(os.path.abspath(candidate))
        relative = lexical.relative_to(root_absolute)
        cursor = root_absolute
        if cursor.is_symlink():
            raise ValueError()
        for component in relative.parts:
            cursor = cursor / component
            if cursor.is_symlink():
                raise ValueError()
        resolved_root = root.resolve(strict=True)
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(resolved_root)
        if (not candidate.is_absolute() or not candidate.is_file() or
                candidate.stat().st_size > limit):
            raise ValueError()
    except (OSError, ValueError, RuntimeError):
        raise WorkflowError("RECOVERY_CONFLICT", "Recovery artifact is unavailable or outside its original job") from None
    if expected is not None and (not _sha256(expected) or file_hash(resolved) != expected):
        raise WorkflowError("RECOVERY_CONFLICT", "Recovery artifact bytes changed")
    return resolved


def verify_artifact_reference(service, job, path, expected):
    """Verify a current-attempt file or a prior file explicitly bound by its preview."""
    if not _sha256(expected):
        raise WorkflowError("RECOVERY_CONFLICT", "Recovery artifact has no valid SHA-256 binding")
    root = service.workspace / "jobs" / job["jobId"]
    try:
        return _regular_bound(path, root, expected)
    except WorkflowError:
        candidate = Path(path)
        preview = service._state.get("previews", {}).get(job.get("previewId"), {})
        binding = preview.get("binding", {})
        bound = any(isinstance(item, dict) and item.get("path") == str(candidate) and
                    item.get("sha256") == expected for item in binding.values())
        if (not bound or not candidate.is_absolute() or candidate.is_symlink() or
                not candidate.is_file() or candidate.stat().st_size > 128 * 1024 * 1024 or
                not _sha256(expected) or file_hash(candidate) != expected):
            raise WorkflowError("RECOVERY_CONFLICT", "Recovery artifact is unavailable or not bound by the original attempt")
        return candidate


def _json(path, root, expected=None, *, limit=2 * 1024 * 1024):
    path = _regular_bound(path, root, expected, limit)
    try:
        def unique(pairs):
            value = {}
            for key, item in pairs:
                if key in value:
                    raise ValueError("duplicate key")
                value[key] = item
            return value
        value = json.loads(path.read_bytes(), object_pairs_hook=unique,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite")))
        canonical(value)
        if not isinstance(value, dict):
            raise ValueError("not an object")
        return value
    except (OSError, ValueError, TypeError, RecursionError):
        raise WorkflowError("RECOVERY_CONFLICT", "Recovery record is malformed; original files were preserved") from None


def _variant_extension(source):
    return "rbm" if source.get("modelBinding", {}).get("kind") == "native-snapshot" else "ram"


def _frame_name(variant, record_no, extension):
    return f"{variant}-{record_no:05d}.{extension}"


def _frame_journal(path, root, expected_job_id):
    path = Path(path)
    if not path.exists() and not path.is_symlink():
        return None
    journal = _regular_bound(path, root, limit=16 * 1024 * 1024)
    raw = journal.read_bytes()
    journal_sha = hashlib.sha256(raw).hexdigest()
    complete_end = raw.rfind(b"\n") + 1
    complete, partial = raw[:complete_end], raw[complete_end:]
    if len(partial) > 64 * 1024:
        raise WorkflowError("RECOVERY_CONFLICT", "Native result journal has an oversized incomplete tail")
    records = {}

    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("duplicate key")
            value[key] = item
        return value

    for sequence, line in enumerate(complete.splitlines(), start=1):
        try:
            record = json.loads(line, object_pairs_hook=unique,
                                parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite")))
            canonical(record)
            required = {"protocolVersion", "jobId", "seq", "recordNo", "timeS", "relativePath", "sha256"}
            if (not isinstance(record, dict) or set(record) != required or
                    record.get("protocolVersion") != RESULT_FRAME_SCHEMA or
                    record.get("jobId") != expected_job_id or
                    type(record.get("seq")) is not int or record["seq"] != sequence or
                    type(record.get("recordNo")) is not int or not 1 <= record["recordNo"] <= 5000 or
                    not _finite_number(record.get("timeS"), positive=False) or
                    not isinstance(record.get("relativePath"), str) or
                    Path(record["relativePath"]).is_absolute() or
                    Path(record["relativePath"]).name != record["relativePath"] or
                    "\\" in record["relativePath"] or not _sha256(record.get("sha256")) or
                    record["recordNo"] in records):
                raise ValueError("invalid record")
            frame_path = journal.parent / record["relativePath"]
            _regular_bound(frame_path, root, record["sha256"])
            records[record["recordNo"]] = record
        except WorkflowError:
            raise
        except (UnicodeDecodeError, ValueError, TypeError, RecursionError):
            raise WorkflowError("RECOVERY_CONFLICT", "Native result journal has a malformed completed record") from None
    if file_hash(journal) != journal_sha:
        raise WorkflowError("RECOVERY_CONFLICT", "Native result journal changed during qualification")
    return records, journal_sha


def _verify_journal_evidence(evidence, root, record_no, frame_sha):
    if evidence is None:
        return
    if (not isinstance(evidence, dict) or not isinstance(evidence.get("path"), str) or
            not _sha256(evidence.get("sha256")) or evidence.get("recordNo") != record_no or
            evidence.get("frameSha256") != frame_sha or not isinstance(evidence.get("jobId"), str) or
            type(evidence.get("recorded")) is not bool):
        raise WorkflowError("RECOVERY_CONFLICT", "Accepted frame journal proof is malformed")
    journal_path = _regular_bound(evidence["path"], root, evidence["sha256"], 16 * 1024 * 1024)
    parsed = _frame_journal(journal_path, root, evidence["jobId"])
    if parsed is None:
        raise WorkflowError("RECOVERY_CONFLICT", "Accepted frame journal proof disappeared")
    records, journal_sha = parsed
    if journal_sha != evidence["sha256"]:
        raise WorkflowError("RECOVERY_CONFLICT", "Accepted frame journal proof changed")
    record = records.get(record_no)
    if evidence["recorded"]:
        if (not isinstance(record, dict) or record.get("seq") != evidence.get("seq") or
                record.get("relativePath") != evidence.get("relativePath") or
                record.get("sha256") != frame_sha or record.get("timeS") != evidence.get("timeS")):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed native result journal entry differs from its proof")
    elif record is not None:
        raise WorkflowError("RECOVERY_CONFLICT", "Unattributed frame now has a completed native journal entry")


def _history_proof_shape(item):
    if not isinstance(item, dict):
        return False
    checkpoint_evidence = item.get("checkpointEvidence")
    if checkpoint_evidence is not None:
        if (not isinstance(checkpoint_evidence, dict) or
                not isinstance(checkpoint_evidence.get("attemptId"), str) or
                type(checkpoint_evidence.get("acceptedSeq")) is not int or
                checkpoint_evidence["acceptedSeq"] < 1 or
                (type(item.get("recordNo")) is int and
                 checkpoint_evidence["acceptedSeq"] < item["recordNo"]) or
                not _sha256(checkpoint_evidence.get("checkpointSha256")) or
                not _sha256(checkpoint_evidence.get("envelopeSha256"))):
            return False
    elif item.get("originKind") == "legacy-history-unattributed":
        return False
    if item.get("originKind") == "legacy-history-unattributed":
        return (item.get("origin") is None and _sha256(item.get("sourceRestartSha256")) and
                "originManifestPath" not in item and "originManifestSha256" not in item and
                item.get("ancestryValidationScope") == "copied-restart-input-bytes")
    if item.get("originKind") == "reused-history":
        return ((item.get("origin") is None or isinstance(item.get("origin"), dict)) and
                _sha256(item.get("originManifestSha256")) and
                isinstance(item.get("originManifestPath"), str) and
                item.get("ancestryValidationScope") in ("captured-source-input", "registered-native-job-manifest"))
    return False


def _compact_history_prefix(items):
    """Store frame proof in ranges so repeated recovery does not inflate state."""
    compacted = []
    current = None
    for item in items:
        if (not isinstance(item, dict) or type(item.get("recordNo")) is not int or
                not _sha256(item.get("sha256")) or not _sha256(item.get("originalFrameSha256")) or
                not _history_proof_shape(item) or not isinstance(item.get("sourcePath"), str)):
            raise WorkflowError("INVALID_STATE", "Recovery frame proof is malformed")
        identity = {key: copy.deepcopy(item.get(key)) for key in
                    ("originKind", "origin", "sourceRestartSha256", "ancestryValidationScope",
                     "sourceAttemptId", "checkpointEvidence")}
        evidence = item.get("journalEvidence")
        journal_source = None if evidence is None else {
            key: copy.deepcopy(evidence.get(key)) for key in ("path", "sha256", "jobId")}
        identity["journalSource"] = journal_source
        if item.get("originKind") == "reused-history":
            identity.update(originManifestPath=item.get("originManifestPath"),
                            originManifestSha256=item.get("originManifestSha256"))
        if (current is not None and item["recordNo"] == current["recordEnd"] + 1 and
                all(current.get(key) == value for key, value in identity.items())):
            current["frameSha256"].append(item["sha256"])
            current["originalFrameSha256"].append(item["originalFrameSha256"])
            current["journalRecorded"].append(bool(evidence and evidence.get("recorded")))
            current["journalSeq"].append(evidence.get("seq") if evidence and evidence.get("recorded") else None)
            current["journalTimeS"].append(evidence.get("timeS") if evidence and evidence.get("recorded") else None)
            current["recordEnd"] = item["recordNo"]
        else:
            current = {"schema": HISTORY_PREFIX_SCHEMA, "recordStart": item["recordNo"],
                       "recordEnd": item["recordNo"], "frameSha256": [item["sha256"]],
                       "originalFrameSha256": [item["originalFrameSha256"]],
                       "journalRecorded": [bool(evidence and evidence.get("recorded"))],
                       "journalSeq": [evidence.get("seq") if evidence and evidence.get("recorded") else None],
                       "journalTimeS": [evidence.get("timeS") if evidence and evidence.get("recorded") else None],
                       **identity}
            compacted.append(current)
    return compacted


def _expand_history_prefix(compacted, variant_directory, variant, extension, root):
    if not isinstance(compacted, list):
        raise WorkflowError("RECOVERY_CONFLICT", "Stored native history proof is malformed")
    expanded = []
    previous_record = 0
    for entry in compacted:
        if (not isinstance(entry, dict) or entry.get("schema") != HISTORY_PREFIX_SCHEMA or
                type(entry.get("recordStart")) is not int or type(entry.get("recordEnd")) is not int or
                entry["recordStart"] <= previous_record or entry["recordEnd"] < entry["recordStart"] or
                not isinstance(entry.get("frameSha256"), list) or
                not isinstance(entry.get("originalFrameSha256"), list) or
                len(entry["frameSha256"]) != entry["recordEnd"] - entry["recordStart"] + 1 or
                len(entry["originalFrameSha256"]) != len(entry["frameSha256"]) or
                not isinstance(entry.get("journalRecorded"), list) or
                not isinstance(entry.get("journalSeq"), list) or
                not isinstance(entry.get("journalTimeS"), list) or
                any(len(entry[key]) != len(entry["frameSha256"])
                    for key in ("journalRecorded", "journalSeq", "journalTimeS")) or
                not _history_proof_shape(entry)):
            raise WorkflowError("RECOVERY_CONFLICT", "Stored native history proof ranges are invalid")
        for index, record_no in enumerate(range(entry["recordStart"], entry["recordEnd"] + 1)):
            sha, original_sha = entry["frameSha256"][index], entry["originalFrameSha256"][index]
            if not _sha256(sha) or not _sha256(original_sha):
                raise WorkflowError("RECOVERY_CONFLICT", "Stored native history frame hash is invalid")
            name = _frame_name(variant, record_no, extension)
            path = _regular_bound(variant_directory / name, root, sha)
            journal_source = entry.get("journalSource")
            journal_evidence = None
            if journal_source is not None:
                if (not isinstance(journal_source, dict) or
                        set(journal_source) != {"path", "sha256", "jobId"} or
                        not isinstance(journal_source.get("path"), str) or
                        not _sha256(journal_source.get("sha256")) or
                        not isinstance(journal_source.get("jobId"), str) or
                        type(entry["journalRecorded"][index]) is not bool):
                    raise WorkflowError("RECOVERY_CONFLICT", "Stored native journal range is invalid")
                journal_evidence = {**copy.deepcopy(journal_source), "recordNo": record_no,
                                    "frameSha256": sha, "recorded": entry["journalRecorded"][index]}
                if entry["journalRecorded"][index]:
                    if (type(entry["journalSeq"][index]) is not int or
                            not _finite_number(entry["journalTimeS"][index])):
                        raise WorkflowError("RECOVERY_CONFLICT", "Stored native journal sequence/time is invalid")
                    journal_evidence.update(seq=entry["journalSeq"][index],
                                            timeS=entry["journalTimeS"][index], relativePath=name)
                elif entry["journalSeq"][index] is not None or entry["journalTimeS"][index] is not None:
                    raise WorkflowError("RECOVERY_CONFLICT", "Unrecorded native journal frame has a sequence")
            elif (type(entry["journalRecorded"][index]) is not bool or entry["journalRecorded"][index] or
                  entry["journalSeq"][index] is not None or entry["journalTimeS"][index] is not None):
                raise WorkflowError("RECOVERY_CONFLICT", "Native journal range is missing its file identity")
            item = {"recordNo": record_no, "sourcePath": str(path), "sha256": sha,
                    "relativePath": name, "originalFrameSha256": original_sha,
                    "journalEvidence": journal_evidence,
                    **{key: copy.deepcopy(entry.get(key)) for key in
                       ("originKind", "origin", "sourceRestartSha256", "ancestryValidationScope",
                        "sourceAttemptId", "checkpointEvidence")}}
            if entry.get("originKind") == "reused-history":
                item["originManifestPath"] = copy.deepcopy(entry.get("originManifestPath"))
                item["originManifestSha256"] = copy.deepcopy(entry.get("originManifestSha256"))
            expanded.append(item)
        previous_record = entry["recordEnd"]
    return expanded


def _capture_history_prefix(attempt, attempt_directory, root, source, variant, dt, context,
                            entry, checkpoint):
    variant_directory = attempt_directory / variant
    extension = _variant_extension(source)
    old_prefix = _expand_history_prefix(entry.get("historyPrefix", []), variant_directory,
                                        variant, extension, root)
    old_by_record = {item["recordNo"]: item for item in old_prefix}
    journal_path = variant_directory / "native-result.journal.jsonl"
    journal_result = _frame_journal(journal_path, root, attempt["attemptId"])
    journal, journal_sha = ({}, None) if journal_result is None else journal_result
    source_restart_sha = entry.get("historySourceRestartSha256") or context["modelSha256"]
    if not _sha256(source_restart_sha):
        raise WorkflowError("RECOVERY_CONFLICT", "Recovery frame source restart hash is invalid")
    prefix = []
    selected_sequence = checkpoint["acceptedSeq"]
    for record_no in range(1, selected_sequence + 1):
        name = _frame_name(variant, record_no, extension)
        frame_path = variant_directory / name
        if not frame_path.exists() and not frame_path.is_symlink():
            raise WorkflowError("RECOVERY_INCOMPLETE_HISTORY", "An accepted numbered frame is missing")
        path = _regular_bound(frame_path, root)
        sha = file_hash(path)
        previous = old_by_record.get(record_no)
        journal_record = journal.get(record_no)
        if previous is not None:
            if previous["sha256"] != sha:
                raise WorkflowError("RECOVERY_CONFLICT", "A previously captured frame changed")
            _verify_journal_evidence(previous.get("journalEvidence"), root, record_no, sha)
            proof = {key: copy.deepcopy(previous.get(key)) for key in
                     ("originKind", "origin", "sourceRestartSha256", "ancestryValidationScope",
                      "sourceAttemptId", "checkpointEvidence")}
            if previous.get("originKind") == "reused-history":
                proof["originManifestPath"] = copy.deepcopy(previous["originManifestPath"])
                proof["originManifestSha256"] = copy.deepcopy(previous["originManifestSha256"])
            original_sha = previous["originalFrameSha256"]
            journal_evidence = copy.deepcopy(previous.get("journalEvidence"))
        else:
            journal_evidence = ({"path": str(journal_path), "sha256": journal_sha,
                                 "jobId": attempt["attemptId"], "recordNo": record_no,
                                 "frameSha256": sha, "recorded": False}
                                if journal_sha is not None else None)
            if journal_record is not None:
                if (journal_record["relativePath"] != name or journal_record["sha256"] != sha or
                        abs(journal_record["timeS"] - record_no * dt) >
                        1e-12 * max(1.0, abs(journal_record["timeS"]), abs(record_no * dt))):
                    raise WorkflowError("RECOVERY_CONFLICT", "Accepted frame journal differs from its MODEL file")
                journal_evidence = {"path": str(journal_path), "sha256": journal_sha,
                                    "jobId": attempt["attemptId"], "seq": journal_record["seq"],
                                    "recordNo": record_no, "timeS": journal_record["timeS"],
                                    "relativePath": journal_record["relativePath"],
                                    "frameSha256": journal_record["sha256"], "recorded": True}
            proof = {"originKind": "legacy-history-unattributed", "origin": None,
                     "sourceRestartSha256": source_restart_sha,
                     "ancestryValidationScope": "copied-restart-input-bytes",
                     "sourceAttemptId": attempt["attemptId"],
                     "checkpointEvidence": {"attemptId": attempt["attemptId"],
                                             "acceptedSeq": checkpoint["acceptedSeq"],
                                             "checkpointSha256": checkpoint["checkpointSha256"],
                                             "envelopeSha256": checkpoint["envelopeSha256"]}}
            original_sha = sha
        prefix.append({"recordNo": record_no, "sourcePath": str(path), "sha256": sha,
                       "relativePath": name, "originalFrameSha256": original_sha,
                       "journalEvidence": journal_evidence, **proof})
    if len(prefix) != selected_sequence:
        raise WorkflowError("RECOVERY_INCOMPLETE_HISTORY", "The accepted checkpoint does not have a complete numbered frame prefix")
    return prefix


def _inspect_native(service, path):
    from .jobs import _native

    with tempfile.TemporaryDirectory(prefix="range-recovery-inspect-") as temporary:
        return _native(service, Path(temporary), "inspect-recovery", threading.Event(),
                       ["inspect", "--input", str(path)])


def _same_accepted_model(left, right):
    fields = ("step", "time", "dt", "q", "v", "a", "controlEnabled", "controllerId",
              "controlAvailable", "controlFingerprint",
              "controlControllerStateSha256", "controlLastCommands", "controlPowers", "controlWork",
              "systemFingerprint", "meshFingerprint", "dofFingerprint", "externalWorkJ",
              "dampingDissipationJ", "initialMechanicalEnergyJ", "supportReactions", "supportPowers",
              "supportWork", "nodes")
    return all(left.get(key) == right.get(key) for key in fields)


def _capture_terminal_link(service, variant_directory, root, variant, extension, checkpoint,
                           target, dt):
    link_path = variant_directory / (variant + "." + extension)
    terminal_path = variant_directory / _frame_name(variant, target, extension)
    if (not link_path.exists() or link_path.is_symlink() or not terminal_path.exists() or
            terminal_path.is_symlink()):
        raise WorkflowError("RECOVERY_INCOMPLETE_HISTORY", "A captured terminal LINK or numbered frame is missing")
    link_path = _regular_bound(link_path, root)
    terminal_path = _regular_bound(terminal_path, root)
    link_sha, terminal_sha, checkpoint_sha = file_hash(link_path), file_hash(terminal_path), file_hash(Path(checkpoint["checkpointPath"]))
    checkpoint_state = _inspect_native(service, Path(checkpoint["checkpointPath"]))
    linked_state = _inspect_native(service, link_path)
    if (checkpoint_state.get("checkpointAvailable") is not True or linked_state.get("checkpointAvailable") is not True or
            checkpoint_state.get("step") != target or linked_state.get("step") != target or
            checkpoint_state.get("dt") != dt or linked_state.get("dt") != dt or
            abs(checkpoint_state.get("time", float("nan")) - target * dt) >
            1e-12 * max(1.0, abs(target * dt)) or
            not _same_accepted_model(checkpoint_state, linked_state)):
        raise WorkflowError("RECOVERY_INCOMPLETE_HISTORY", "Terminal LINK and selected checkpoint are different accepted states")
    if not _same_path_for_recovery(linked_state.get("sourcePath"), terminal_path):
        raise WorkflowError("RECOVERY_INCOMPLETE_HISTORY", "Captured terminal LINK does not resolve to the selected highest record")
    source_files = linked_state.get("sourceFiles", [])
    def source_sha(path):
        return next((item.get("sha256") for item in source_files if isinstance(item, dict) and
                     _same_path_for_recovery(item.get("path"), path)), None)
    if (not isinstance(source_files, list) or len(source_files) != 2 or
            source_sha(link_path) != link_sha or source_sha(terminal_path) != terminal_sha or
            file_hash(link_path) != link_sha or file_hash(terminal_path) != terminal_sha or
            file_hash(Path(checkpoint["checkpointPath"])) != checkpoint_sha):
        raise WorkflowError("RECOVERY_CONFLICT", "Captured terminal MODEL/LINK changed during inspection")
    return {"linkPath": str(link_path), "linkSha256": link_sha,
            "terminalPath": str(terminal_path), "terminalSha256": terminal_sha,
            "recordNo": target, "checkpointSha256": checkpoint_sha}


def _same_path_for_recovery(left, right):
    try:
        return Path(left).resolve(strict=True) == Path(right).resolve(strict=True)
    except (OSError, TypeError, ValueError, RuntimeError):
        return False


def _checkpoint(entry, root, target, dt):
    context = entry.get("context")
    if not _valid_context(context):
        return None
    directory = Path(entry.get("checkpointDirectory", ""))
    pointer_path = directory / "latest.json"
    if not pointer_path.exists() and not pointer_path.is_symlink():
        return None
    latest = _json(pointer_path, root, limit=64 * 1024)
    if (set(latest) != {"schema", "envelopeFile", "envelopeSha256"} or
            latest.get("schema") != POINTER_SCHEMA):
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint pointer has an invalid schema")
    name = latest.get("envelopeFile")
    if not isinstance(name, str) or Path(name).name != name or not _sha256(latest.get("envelopeSha256")):
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint pointer has an invalid envelope reference")
    envelope_path = directory / name
    envelope = _json(envelope_path, root, latest["envelopeSha256"], limit=64 * 1024)
    required = {"schema", "sourceSha256", "inputModelSha256", "controlSha256", "runtimeSha256",
                "packageSha256", "policy", "maxAcceptedSteps", "maxSimulationSeconds", "acceptedSeq",
                "timeS", "dtS", "modelFile", "checkpointModelSha256"}
    if set(envelope) != required:
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint envelope fields differ from the accepted schema")
    expected = {key: context[key] for key in ("sourceSha256", "controlSha256", "runtimeSha256", "packageSha256",
                                               "policy", "maxAcceptedSteps", "maxSimulationSeconds", "dtS")}
    expected.update(schema=CHECKPOINT_SCHEMA, inputModelSha256=context["modelSha256"])
    if any(envelope.get(key) != value for key, value in expected.items()):
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint provenance or policy differs from the accepted attempt")
    step = envelope.get("acceptedSeq")
    dt_s, time_s = envelope.get("dtS"), envelope.get("timeS")
    expected_time = step * dt if type(step) is int else math.nan
    tolerance = 1e-12 * max(1.0, abs(time_s) if _finite_number(time_s) else 0.0,
                            abs(expected_time) if math.isfinite(expected_time) else 0.0)
    if (type(step) is not int or not 1 <= step <= target or dt_s != dt or
            not _finite_number(time_s, positive=False) or abs(time_s - expected_time) > tolerance):
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint accepted sequence/time is invalid")
    model_name = envelope.get("modelFile")
    if not isinstance(model_name, str) or Path(model_name).name != model_name:
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint MODEL reference is invalid")
    model = _regular_bound(directory / model_name, root, envelope.get("checkpointModelSha256"))
    return {"checkpointPath": str(model), "checkpointSha256": file_hash(model), "acceptedSeq": step,
            "envelopePath": str(envelope_path), "envelopeFile": name,
            "envelopeSha256": file_hash(envelope_path), "checkpointDirectory": str(directory),
            "pointerPath": str(pointer_path), "pointerSha256": file_hash(pointer_path),
            "context": copy.deepcopy(context),
            "replayRange": {"afterAcceptedSeq": step, "maxAcceptedSteps": 10,
                            "maxSimulationSeconds": 1.0}}


def _verify_external_binding(binding):
    if not isinstance(binding, dict):
        raise WorkflowError("RECOVERY_CONFLICT", "Original preview binding is unavailable")
    for item in binding.values():
        if not isinstance(item, dict) or "path" not in item:
            # Typed metadata such as recoverySelection is not a file binding.
            continue
        path = Path(item.get("path", ""))
        if (not path.is_absolute() or path.is_symlink() or not path.is_file() or
                not _sha256(item.get("sha256")) or file_hash(path) != item["sha256"]):
            raise WorkflowError("RECOVERY_CONFLICT", "Original software or source changed")


def _verify_current_runtime(service, binding):
    for key, attribute in (("solver", "solver"), ("native", "native"),
                           ("python", "python"), ("nativeResults", "native_results")):
        current = getattr(service, attribute, None)
        frozen = binding.get(key)
        if current is None and frozen is None:
            continue
        if current is None or not isinstance(frozen, dict):
            raise WorkflowError("RECOVERY_CONFLICT", "Configured recovery runtime differs from original attempt")
        path = Path(current)
        if (not path.is_absolute() or path.is_symlink() or not path.is_file() or
                str(path.resolve(strict=True)) != str(Path(frozen["path"]).resolve(strict=True)) or
                file_hash(path) != frozen["sha256"]):
            raise WorkflowError("RECOVERY_CONFLICT", "Configured recovery runtime differs from original attempt")


def _verify_context(context, result, variant, binding):
    if not _valid_context(context):
        raise WorkflowError("RECOVERY_CONFLICT", "Native checkpoint context is malformed")
    prepared = result.get("preparedInputs", {}).get(variant, {})
    source_ref = result.get("sourceRef") or {}
    template_ref = result.get("templateSourceRef") or {}
    source_sha = source_ref.get("registeredModelSha256") or template_ref.get("sourceSha256")
    if (context["sourceSha256"] != source_sha or context["modelSha256"] != prepared.get("sha256") or
            context["dtS"] != result.get("dtSeconds") or
            context["runtimeSha256"] != binding.get("solver", {}).get("sha256")):
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint context differs from its immutable workflow package")
    if variant == "active":
        revision = controller_revision(Path(binding["python"]["path"]), control_arguments(result))
        if context["controlSha256"] != revision:
            raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint controller revision differs from the source files")
    elif context["controlSha256"] != NO_CONTROL_SHA256:
        raise WorkflowError("RECOVERY_CONFLICT", "Passive checkpoint declares an enabled controller")
    package_sha = _package_sha256(binding, result, source_sha, context["modelSha256"],
                                  context["controlSha256"])
    if context["packageSha256"] != package_sha:
        raise WorkflowError("RECOVERY_CONFLICT", "Checkpoint package digest differs from the immutable workflow inputs")


def _verify_completed_variant(service, source, variant, root):
    from .native_results import NativeResultsClient

    value = source.get(variant)
    descriptor = source.get("nativeResults", {}).get(variant)
    artifacts = source.get("nativeResultArtifacts", {}).get(variant)
    if not isinstance(value, dict) or not isinstance(descriptor, dict) or not isinstance(artifacts, dict):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant has no verified result manifest")
    manifest = _regular_bound(descriptor.get("manifestPath", ""), root, descriptor.get("manifestSha256"))
    manifest_artifact = artifacts.get("manifest")
    if (not isinstance(manifest_artifact, dict) or manifest_artifact.get("path") != str(manifest) or
            manifest_artifact.get("sha256") != descriptor.get("manifestSha256")):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant manifest is absent from its artifact proof")
    terminal = _regular_bound(value.get("checkpointPath", ""), root, value.get("checkpointSha256"))
    if service.native_results is None:
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant cannot be verified without the native results broker")
    # Native canonical/semantic validation precedes use of the storage-only
    # Python decoder; the final check below still closes the extraction race.
    canonical_catalog = NativeResultsClient(service.native_results, manifest, descriptor["manifestSha256"]).query(
        "catalog", {"limit": 1})
    if canonical_catalog.get("jobId") != source.get("jobId") or canonical_catalog.get("artifactId") != descriptor.get("artifactId"):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant native identity differs")
    from .native_manifest import load_native_manifest
    manifest_data = load_native_manifest(manifest, descriptor["manifestSha256"])
    terminal_record = manifest_data.get("terminalModel")
    if (manifest_data.get("jobId") != source.get("jobId") or
            manifest_data.get("artifactId") != descriptor.get("artifactId") or
            not isinstance(terminal_record, dict) or
            terminal_record.get("sha256") != value.get("checkpointSha256") or
            _regular_bound(manifest.parent / terminal_record.get("relativePath", ""), root,
                           terminal_record.get("sha256")) != terminal):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant manifest does not bind its terminal MODEL")
    frame_records = manifest_data.get("frames")
    if not isinstance(frame_records, list):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant frame list is malformed")
    frame_artifacts = {key: item for key, item in artifacts.items() if key.startswith("frame/")}
    if len(frame_artifacts) != len(frame_records):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant frame proof is incomplete")
    for index, frame in enumerate(frame_records):
        if not isinstance(frame, dict):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed variant frame record is malformed")
        bound = _regular_bound(manifest.parent / frame.get("relativePath", ""), root, frame.get("sha256"))
        proof = frame_artifacts.get("frame/" + str(index))
        if (not isinstance(proof, dict) or proof.get("sha256") != frame.get("sha256") or
                Path(proof.get("path", "")).resolve(strict=True) != bound):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed variant frame differs from its artifact proof")
    for key, item in artifacts.items():
        if key != "manifest" and not key.startswith("frame/"):
            raise WorkflowError("RECOVERY_CONFLICT", "Completed variant contains an unknown artifact proof")
    if service.native_results is None:
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant cannot be verified without the native results broker")
    catalog = NativeResultsClient(service.native_results, manifest, descriptor["manifestSha256"]).query(
        "catalog", {"limit": 1})
    if catalog.get("jobId") != source.get("jobId") or catalog.get("artifactId") != descriptor.get("artifactId"):
        raise WorkflowError("RECOVERY_CONFLICT", "Completed variant manifest has a different identity")
    return {"phase": "completed", "acceptedSeq": source["totalSteps"], "remainingSteps": 0}


def selection_summary(qualification):
    """Compact source-attempt baseline for telemetry and audit continuity."""
    variants = {}
    for name, selected in qualification.get("variants", {}).items():
        variants[name] = {key: copy.deepcopy(selected[key]) for key in
                          ("phase", "acceptedSeq", "remainingSteps", "checkpointSha256",
                           "envelopeSha256", "replayRange") if key in selected}
        variants[name]["sourceAttemptId"] = qualification.get("attemptId")
        if "historyPrefix" in selected:
            variants[name]["historyPrefixCount"] = len(selected["historyPrefix"])
    return {"attemptId": qualification.get("attemptId"),
            "qualificationSha256": qualification.get("qualificationSha256"),
            "targetSteps": qualification.get("targetSteps"), "variants": variants}


def qualify(service, job_id):
    job = service._job(job_id)
    answer = {"jobId": job_id, "eligibility": "recompute_required", "variants": {},
              "diagnostic": "No durable prepared attempt is available"}
    if job["state"] != "interrupted":
        answer.update(eligibility="reconcile_required", diagnostic="Recovery is available only for interrupted jobs")
        return answer, None
    root = service.workspace / "jobs" / job_id
    try:
        recovery = job.get("recovery", {})
        if recovery.get("schema") != SCHEMA or not isinstance(recovery.get("attempts"), list) or not recovery["attempts"]:
            return answer, None
        attempt = recovery["attempts"][-1]
        if not isinstance(attempt, dict) or not isinstance(attempt.get("variants"), dict):
            raise WorkflowError("RECOVERY_CONFLICT", "Recovery attempt ledger is malformed")
        attempt_directory = Path(attempt.get("directory", ""))
        if (not attempt_directory.is_absolute() or root.is_symlink() or attempt_directory.is_symlink() or
                not attempt_directory.resolve(strict=True).is_relative_to(root.resolve(strict=True))):
            raise WorkflowError("RECOVERY_CONFLICT", "Recovery attempt directory is outside its logical job")
        source = copy.deepcopy(attempt["partialResult"])
        if source.get("runtimeControlEnabled") or job.get("runtimeControl"):
            # Values saved before submission remain evidence for an accepted
            # checkpoint digest, never authority to replay a pending proposal.
            source["runtimeControlEnabled"] = True
            source["runtimeControl"] = copy.deepcopy(job.get("runtimeControl", source.get("runtimeControl")))
            if type((job.get("result") or {}).get("runtimeProposalRecords")) is dict:
                source["runtimeProposalRecords"] = copy.deepcopy(job["result"]["runtimeProposalRecords"])
        target, dt, mode = source["totalSteps"], source["dtSeconds"], source["mode"]
        if (type(target) is not int or not 1 <= target <= 5000 or
                not _finite_number(dt, positive=True) or mode not in ("active", "passive", "both")):
            raise WorkflowError("RECOVERY_CONFLICT", "Prepared recovery range is invalid")
        preview = service._state["previews"][job["previewId"]]
        binding = preview.get("binding")
        _verify_external_binding(binding)
        _verify_current_runtime(service, binding)
        artifacts = source.get("sourceArtifacts")
        prepared = source.get("preparedInputs")
        if not isinstance(artifacts, dict) or not isinstance(prepared, dict):
            raise WorkflowError("RECOVERY_CONFLICT", "Immutable source artifacts or prepared inputs are unavailable")
        for artifact in artifacts.values():
            if not isinstance(artifact, dict):
                raise WorkflowError("RECOVERY_CONFLICT", "Source artifact reference is malformed")
            verify_artifact_reference(service, job, artifact.get("path", ""), artifact.get("sha256"))
        for variant in _VARIANTS:
            item = prepared.get(variant)
            if not isinstance(item, dict):
                raise WorkflowError("RECOVERY_CONFLICT", "A frozen active/passive prepared input is missing")
            verify_artifact_reference(service, job, item.get("path", ""), item.get("sha256"))
        source_ref = source.get("sourceRef") or {}
        template_ref = source.get("templateSourceRef") or {}
        source_sha = source_ref.get("registeredModelSha256") or template_ref.get("sourceSha256")
        source_artifact = artifacts.get("sourceModel") if source_ref else artifacts.get("template")
        if (not _sha256(source_sha) or not isinstance(source_artifact, dict) or
                source_artifact.get("sha256") != source_sha):
            raise WorkflowError("RECOVERY_CONFLICT", "Workflow source identity differs from its immutable copy")
        if source_ref:
            manifest_artifact = artifacts.get("sourceManifest")
            if (not isinstance(manifest_artifact, dict) or
                    manifest_artifact.get("path") != source_ref.get("jobManifestPath") or
                    manifest_artifact.get("sha256") != source_ref.get("sourceManifestSha256") or
                    source_ref.get("jobManifestSha256") != source_ref.get("sourceManifestSha256") or
                    source_artifact.get("path") != source_ref.get("jobSnapshotPath") or
                    source_artifact.get("sha256") != source_ref.get("jobSnapshotSha256")):
                raise WorkflowError("RECOVERY_CONFLICT", "Imported source manifest and snapshot lineage differ")

        usable = True
        for variant in _VARIANTS:
            if mode not in (variant, "both"):
                continue
            entry = attempt["variants"].get(variant, {})
            phase = entry.get("phase")
            if phase == "completed":
                answer["variants"][variant] = _verify_completed_variant(service, source, variant, root)
            else:
                context = entry.get("context")
                if not _valid_context(context):
                    usable = False
                    answer["variants"][variant] = {"phase": "recompute_required"}
                    continue
                _verify_context(context, source, variant, binding)
                checkpoint = _checkpoint(entry, root, target, dt) if phase in ("running", "checkpointed") else None
                if checkpoint is not None:
                    try:
                        history_prefix = _capture_history_prefix(attempt, attempt_directory, root, source,
                                                                 variant, dt, context, entry, checkpoint)
                    except WorkflowError as history_error:
                        if getattr(history_error, "code", "") != "RECOVERY_INCOMPLETE_HISTORY":
                            raise
                        usable = False
                        answer["variants"][variant] = {"phase": "recompute_required",
                                                        "diagnostic": history_error.message}
                        continue
                    checkpoint["sourceAttemptId"] = attempt["attemptId"]
                    checkpoint["historyPrefix"] = history_prefix
                    if checkpoint["acceptedSeq"] == target:
                        try:
                            checkpoint["terminalLink"] = _capture_terminal_link(
                                service, attempt_directory / variant, root, variant,
                                _variant_extension(source), checkpoint, target, dt)
                        except WorkflowError as terminal_error:
                            if getattr(terminal_error, "code", "") != "RECOVERY_INCOMPLETE_HISTORY":
                                raise
                            usable = False
                            answer["variants"][variant] = {"phase": "recompute_required",
                                                            "diagnostic": terminal_error.message}
                            continue
                    source[variant] = checkpoint
                    answer["variants"][variant] = {"phase": "checkpointed", **checkpoint,
                                                    "remainingSteps": target - checkpoint["acceptedSeq"]}
                elif phase in (None, "pending", "prepared"):
                    answer["variants"][variant] = {"phase": "prepared", "acceptedSeq": 0,
                                                    "remainingSteps": target,
                                                    "preparedInput": copy.deepcopy(prepared[variant]),
                                                    "context": copy.deepcopy(context)}
                    source.pop(variant, None)
                else:
                    usable = False
                    answer["variants"][variant] = {"phase": "recompute_required"}
        answer.update(eligibility="recoverable" if usable else "recompute_required",
                      diagnostic="Validated original attempt" if usable else
                      "A started variant has no valid complete checkpoint",
                      attemptId=attempt["attemptId"], targetSteps=target,
                      qualificationSha256=digest({"attemptId": attempt["attemptId"],
                                                  "variants": answer["variants"]}))
        source["recoveryQualification"] = copy.deepcopy(answer)
        return answer, source if usable else None
    except (WorkflowError, OSError, ValueError, KeyError, TypeError, AttributeError, RuntimeError) as error:
        answer.update(eligibility="conflict", diagnostic=getattr(error, "message",
                      "Recovery evidence is unavailable or inconsistent"))
        if not isinstance(error, WorkflowError):
            import traceback
            answer["inspectionDiagnostic"] = {"exceptionType": type(error).__name__[:64], "frames": [
                {"module": Path(frame.filename).name[:128], "function": frame.name[:128], "line": frame.lineno}
                for frame in traceback.extract_tb(error.__traceback__, limit=8)]}
        return answer, None
