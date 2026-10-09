"""Single-owner persistent workflow service. State and audit commit together."""
from __future__ import annotations

import copy
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import threading
from typing import Any
import uuid

from .process import ProcessFailure, ProcessRunner
from .templates import TEMPLATES

API_VERSION = "range.workflow/v1"
MAX_REQUEST_BYTES = 256 * 1024
MAX_STATE_BYTES = 64 * 1024 * 1024
MAX_STEPS = 5000
MAX_EXTENSION_JOBS = 4
TERMINAL_STATES = {"succeeded", "failed", "cancelled", "interrupted"}


class WorkflowError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code, self.message = code, message


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def digest(value: Any) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def exact(value: Any, required: set[str], optional: set[str] = frozenset()) -> dict:
    if not isinstance(value, dict) or not required <= value.keys() or value.keys() - required - optional:
        raise WorkflowError("INVALID_PARAMETERS", "Missing required fields or unknown fields")
    return value


def integer(value: Any, name: str, low: int, high: int) -> int:
    if type(value) is not int or not low <= value <= high:
        raise WorkflowError("INVALID_PARAMETERS", f"{name} must be an integer in {low}..{high}")
    return value


def number(value: Any, name: str, low: float, high: float) -> float:
    if type(value) not in (int, float):
        raise WorkflowError("INVALID_PARAMETERS", f"{name} must be a finite number")
    try:
        result = float(value)
    except (OverflowError, ValueError):
        raise WorkflowError("INVALID_PARAMETERS", f"{name} must be finite") from None
    if not math.isfinite(result) or not low < result <= high:
        raise WorkflowError("INVALID_PARAMETERS", f"{name} must be in ({low}, {high}]")
    return result


def identifier(value: Any, name: str = "ID") -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", value):
        raise WorkflowError("INVALID_PARAMETERS", f"{name} must be a bounded identifier")
    return value


def finite_tree(value: Any, depth: int = 0, max_collection_size: int = 1024):
    if depth > 16:
        raise WorkflowError("INVALID_PARAMETERS", "JSON nesting exceeds 16 levels")
    if value is None or isinstance(value, bool):
        return
    if type(value) in (int, float):
        try:
            good = math.isfinite(value)
        except OverflowError:
            good = False
        if not good:
            raise WorkflowError("INVALID_PARAMETERS", "Numbers must be finite")
    elif isinstance(value, str):
        if len(value) > 65536:
            raise WorkflowError("INVALID_PARAMETERS", "Text exceeds size limit")
    elif isinstance(value, (list, dict)):
        if len(value) > max_collection_size:
            raise WorkflowError("INVALID_PARAMETERS", "Collection exceeds size limit")
        if isinstance(value, dict):
            if any(not isinstance(k, str) or len(k) > 256 for k in value):
                raise WorkflowError("INVALID_PARAMETERS", "Invalid object key")
            value = value.values()
        for item in value:
            finite_tree(item, depth + 1, max_collection_size)
    else:
        raise WorkflowError("INVALID_PARAMETERS", "Value is not JSON")


def utc() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class WorkspaceLock:
    def __init__(self, directory: Path):
        self.file = (directory / ".workflow.lock").open("a+b")
        try:
            if os.name == "nt":
                import msvcrt
                self.file.seek(0, 2)
                if self.file.tell() == 0:
                    self.file.write(b"0")
                    self.file.flush()
                self.file.seek(0)
                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.file.close()
            raise WorkflowError("WORKSPACE_BUSY", "Another service owns this workspace") from None

    def close(self):
        if not self.file.closed:
            if os.name == "nt":
                import msvcrt
                self.file.seek(0)
                msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
            self.file.close()


class WorkflowService:
    def __init__(self, workspace: Path, solver: Path, native: Path,
                 examples: Path, python: Path, *, trusted_extensions: bool = False,
                 native_results: Path | None = None):
        self.workspace = Path(workspace).resolve()
        self.solver, self.native, self.examples, self.python = map(Path, (solver, native, examples, python))
        result_tool = Path(native_results) if native_results is not None else self.native.with_name(
            "range-native-results" + self.native.suffix)
        self.native_results = result_tool if result_tool.is_absolute() and result_tool.is_file() else None
        if native_results is not None and self.native_results is None:
            raise WorkflowError("INVALID_PATH", "Configured native results executable is unavailable")
        if type(trusted_extensions) is not bool:
            raise WorkflowError("INVALID_PARAMETERS", "trusted_extensions must be an explicit boolean")
        self.trusted_extensions = trusted_extensions
        self.extension_timeout_seconds = 15
        for path in (self.solver, self.native, self.python):
            if not path.is_absolute() or not path.is_file():
                raise WorkflowError("INVALID_PATH", "Tool paths must be existing absolute files")
        if not self.examples.is_absolute() or not self.examples.is_dir():
            raise WorkflowError("INVALID_PATH", "Examples must be an absolute directory")
        self.workspace.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._os_lock = WorkspaceLock(self.workspace)
        self._state_path = self.workspace / "workflow-state.json"
        self.runner = ProcessRunner()
        self._workers: dict[str, tuple[threading.Thread, threading.Event]] = {}
        self._extension_writers: dict[str, str] = {}
        self._commands: dict[str, dict] = {}
        # Authorization belongs to this live host instance, never its disk state.
        self._approval_tokens: dict[str, str] = {}
        from .telemetry import TelemetryReader
        self._telemetry = TelemetryReader(self.workspace)
        self._closed = False
        self._fault = False
        try:
            self._state = self._load()
            from .extensions import install as install_extensions
            install_extensions(self)
            candidate = copy.deepcopy(self._state)
            candidate["sessionId"] = str(uuid.uuid4())
            candidate["revision"] += 1
            for job in candidate["jobs"].values():
                if job["state"] not in TERMINAL_STATES:
                    job.update(state="interrupted", completedAt=utc(),
                               error={"code": "INTERRUPTED", "message": "Service closed before job completion"})
            self._commit(candidate, "session.open", "opened")
        except BaseException:
            self._os_lock.close()
            raise

    def _load(self) -> dict:
        from .project_catalog import load_project_catalog
        catalog = load_project_catalog(self.workspace)
        if not self._state_path.exists():
            return {"storageVersion": 2, "projectId": catalog["projectId"] if catalog else str(uuid.uuid4()), "modelId": str(uuid.uuid4()),
                    "sessionId": str(uuid.uuid4()), "revision": 0, "auditRevision": 0,
                    "configuration": {"dt": 0.005, "steps": 400, "targetDamping": 0.1, "model": "four-rod"},
                    "jobs": {}, "previews": {}, "audit": [], "extensions": {}}
        try:
            if self._state_path.stat().st_size > MAX_STATE_BYTES:
                raise ValueError("state too large")
            value = json.loads(self._state_path.read_bytes())
            exact(value, {"storageVersion", "projectId", "modelId", "sessionId", "revision",
                          "auditRevision", "configuration", "jobs", "previews", "audit", "extensions"})
            if value["storageVersion"] not in (1, 2):
                raise ValueError("unsupported storage version")
            for key in ("projectId", "modelId", "sessionId"):
                if uuid.UUID(value[key]).int == 0:
                    raise ValueError("null identity")
            integer(value["revision"], "revision", 0, 2**53 - 1)
            integer(value["auditRevision"], "auditRevision", 0, 2**53 - 1)
            self._normalize_builtin("workflow.configure", value["configuration"], state=value)
            if not all(isinstance(value[k], dict) for k in ("jobs", "previews", "extensions")):
                raise ValueError("invalid state collections")
            from .extensions import validate_registry_state
            validate_registry_state(value["extensions"])
            if not isinstance(value["audit"], list) or value["auditRevision"] != len(value["audit"]):
                raise ValueError("invalid audit sequence")
            # Strict finite serialization also rejects NaN accepted by json.loads.
            canonical(value)
            for jid, job in value["jobs"].items():
                if str(uuid.UUID(jid)) != jid or job["jobId"] != jid or job["state"] not in TERMINAL_STATES | {"queued", "running", "cancelling"}:
                    raise ValueError("invalid job")
            # v1 saved raw approval tokens. Migration discards them before the
            # new session is committed; accepted jobs remain historical facts.
            for preview in value["previews"].values():
                if not isinstance(preview, dict):
                    raise ValueError("invalid preview")
                preview.pop("approvalToken", None)
            value["storageVersion"] = 2
        except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
            raise WorkflowError("CORRUPT_STATE", "Workspace state is invalid; it was preserved") from error
        if catalog is not None and catalog["projectId"] != value["projectId"]:
            raise WorkflowError("PROJECT_ID_CONFLICT", "Host catalog and workflow state belong to different projects")
        return value

    def _state_digest(self, state: dict) -> str:
        return digest({k: v for k, v in state.items() if k not in {"audit", "previews"}})

    def _commit(self, candidate: dict, command: str, status: str, *, job_id: str | None = None,
                approval: str = "not_required", parameters: dict | None = None,
                artifacts: list[dict] | None = None, actor: str = "system"):
        before = self._state
        candidate["auditRevision"] = before["auditRevision"] + 1
        # Audit parameters contain fixed metadata and hashes, never approval tokens or generated source.
        record = {"schemaVersion": 1, "recordId": str(uuid.uuid4()),
                  "parentRecordId": before["audit"][-1]["record"]["recordId"] if before["audit"] else None,
                  "timestamp": utc(), "actorType": actor, "actorId": "range-workflow-host" if actor == "user" else "range-workflow",
                  "sessionId": candidate["sessionId"], "projectId": candidate["projectId"],
                  "modelId": candidate["modelId"], "modelRevision": f"r{candidate['revision']}",
                  "runId": job_id, "workflowStage": "workflow", "commandId": command,
                  "commandVersion": "1.0.0", "parameters": parameters or {},
                  "selectionSnapshot": {"beforeRevision": before["revision"], "afterRevision": candidate["revision"]},
                  "agentProposal": None, "approvalState": approval,
                  "preStateHash": self._state_digest(before), "postStateHash": self._state_digest(candidate),
                  "resultStatus": status, "diagnostics": [],
                  "artifactRefs": [{"artifactId": item["artifactId"], "sha256": item["sha256"]} for item in artifacts or []],
                  "applicationVersion": "1.0.0", "pluginVersions": {}}
        candidate["audit"].append({"revision": candidate["auditRevision"], "record": record})
        raw = canonical(candidate)
        if len(raw) > MAX_STATE_BYTES:
            raise WorkflowError("STATE_LIMIT", "Workspace metadata reached its storage limit")
        temporary = self.workspace / (".state-" + str(uuid.uuid4()) + ".tmp")
        try:
            with temporary.open("xb") as stream:
                stream.write(raw)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self._state_path)
            if os.name != "nt":
                descriptor = os.open(self.workspace, os.O_RDONLY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        except OSError as error:
            self._fault = True
            for _, cancel in self._workers.values():
                cancel.set()
            raise WorkflowError("PERSISTENCE_ERROR", "Could not commit workflow state and audit together") from error
        finally:
            temporary.unlink(missing_ok=True)
        self._state = candidate

    def register_command(self, name: str, *, descriptor: dict, normalize, execute, bind=None,
                         lane: str = "project", registry_write: bool = False):
        if not re.fullmatch(r"[a-z][a-z0-9_.-]{1,100}", name) or name.startswith("workflow.") or name in self._commands:
            raise ValueError("Invalid or duplicate extension command")
        if lane not in ("project", "extension") or type(registry_write) is not bool:
            raise ValueError("Invalid command conflict scope")
        self._commands[name] = dict(descriptor=descriptor, normalize=normalize, execute=execute,
                                    bind=bind, lane=lane, registry_write=registry_write)

    def state_snapshot(self) -> dict:
        with self._lock:
            return copy.deepcopy(self._state)

    def extension_state(self) -> dict:
        with self._lock:
            return copy.deepcopy(self._state["extensions"])

    def _capability_revision(self) -> int:
        return sum(entry.get("capabilityRevision", 0) for entry in self._state["extensions"].values())

    def project_snapshot(self) -> dict:
        with self._lock:
            from .project_catalog import query_project_catalog
            catalog = query_project_catalog(self.workspace)
            if catalog.get("projectId", self._state["projectId"]) != self._state["projectId"]:
                raise WorkflowError("PROJECT_ID_CONFLICT", "Host catalog and workflow state belong to different projects")
            return {k: copy.deepcopy(self._state[k]) for k in
                    ("projectId", "modelId", "sessionId", "revision", "configuration")} | {
                    "busy": self._busy("project"), "template": (None if "sourceId" in self._state["configuration"]
                        else self._state["configuration"].get("model", "four-rod")),
                    "catalog": catalog, "modelBinding": self._model_binding(self._state["configuration"])}

    def _model_binding(self, configuration: dict) -> dict:
        if "sourceId" not in configuration:
            return {"kind": "template", "usesOpenDocument": False}
        from .model_sources import read_model_source
        source = read_model_source(self.workspace, configuration["sourceId"])
        return {"kind": "native-snapshot", "usesOpenDocument": True,
                "sourceId": source["sourceId"], "originRef": source["originRef"],
                "registeredModelSha256": source["registeredModelSha256"],
                "sourceManifestSha256": source["manifestSha256"]}

    def result_snapshot(self, job_id: str) -> dict:
        with self._lock:
            job = self._job(job_id)
            if job["state"] != "succeeded" or "result" not in job:
                raise WorkflowError("RESULT_UNAVAILABLE", "Only completed successful jobs have accepted results")
            return copy.deepcopy(job["result"])

    def _busy(self, lane: str = "all") -> bool:
        return self._active_jobs(lane) > 0

    def _active_jobs(self, lane: str = "all") -> int:
        return sum(1 for x in self._state["jobs"].values()
                   if x["state"] not in TERMINAL_STATES and
                   (lane == "all" or x.get("lane", "project") == lane))

    def _command_lane(self, command: str) -> str:
        return self._commands.get(command, {}).get("lane", "project")

    def _command_extension_id(self, command: str, parameters: dict) -> str | None:
        if not command.startswith("extension."):
            return None
        if command == "extension.stage":
            manifest = parameters.get("manifest")
            if isinstance(manifest, dict):
                return manifest.get("extensionId")
            return parameters.get("extensionId")
        if command in ("extension.validate", "extension.activate"):
            candidate_id = parameters.get("candidateId")
            if isinstance(candidate_id, str):
                for extension_id, entry in self._state["extensions"].items():
                    if candidate_id in entry.get("candidates", {}):
                        return extension_id
        return parameters.get("extensionId")

    def _compact_stale_extension_previews(self, previews: dict, revision: int, session_id: str):
        from .extensions import compact_job_parameters
        for preview in previews.values():
            normalized = preview.get("normalized", {})
            command = normalized.get("command")
            parameters = normalized.get("parameters", {})
            if (command in ("extension.stage", "extension.run") and "jobId" not in preview and
                    (preview.get("sessionId") != session_id or preview.get("revision") != revision)):
                if ((command == "extension.stage" and "manifest" in parameters) or
                        (command == "extension.run" and "series" in parameters)):
                    normalized["parameters"] = compact_job_parameters(command, parameters)

    def _job(self, job_id: str) -> dict:
        identifier(job_id, "jobId")
        if job_id not in self._state["jobs"]:
            raise WorkflowError("JOB_NOT_FOUND", "Unknown jobId")
        return self._state["jobs"][job_id]

    def _normalize_builtin(self, command: str, parameters: dict, state=None) -> dict:
        state = state if state is not None else self._state
        if command == "workflow.configure":
            exact(parameters, {"dt", "steps", "targetDamping"}, {"model", "sourceId"})
            source_id = identifier(parameters["sourceId"], "sourceId") if "sourceId" in parameters else None
            if "sourceId" not in parameters and "model" not in parameters:
                source_id = state["configuration"].get("sourceId")
            model = parameters.get("model", state["configuration"].get("model", "four-rod"))
            if source_id is not None:
                from .model_sources import read_model_source
                source = read_model_source(self.workspace, identifier(source_id, "sourceId"))
                source_kind = source["descriptor"]["modelKind"]
                if "model" in parameters and model != source_kind:
                    raise WorkflowError("SOURCE_MODEL_MISMATCH", "The requested family differs from the registered source")
                model = source_kind
            if not isinstance(model, str) or model not in TEMPLATES:
                raise WorkflowError("INVALID_PARAMETERS", "model must be four-rod or cantilever-beam")
            normalized = {"dt": number(parameters["dt"], "dt", 0, 0.02),
                          "steps": integer(parameters["steps"], "steps", 1, MAX_STEPS),
                          "targetDamping": number(parameters["targetDamping"], "targetDamping", 0, 0.5), "model": model}
            if source_id is not None:
                normalized["sourceId"] = source_id
            return normalized
        if command == "workflow.run":
            exact(parameters, {"mode", "independentReference"})
            if parameters["mode"] not in ("active", "passive", "both") or parameters["independentReference"] is not True:
                raise WorkflowError("INVALID_PARAMETERS", "mode must be active/passive/both and independentReference must be true")
            return dict(parameters)
        if command == "workflow.resume":
            exact(parameters, {"jobId", "additionalSteps"})
            source = self._job(parameters["jobId"])
            extra = integer(parameters["additionalSteps"], "additionalSteps", 1, MAX_STEPS)
            if source["state"] != "succeeded" or source["command"] not in ("workflow.run", "workflow.resume"):
                raise WorkflowError("RESUME_UNAVAILABLE", "Resume requires a successful native workflow result")
            if source["result"]["totalSteps"] + extra > MAX_STEPS:
                raise WorkflowError("STEP_LIMIT", "Resumed trajectory exceeds 5000 total reference steps")
            return {"jobId": source["jobId"], "additionalSteps": extra}
        if command == "workflow.recover":
            exact(parameters, {"jobId", "qualificationSha256"})
            from .recovery import qualify
            qualification, source = qualify(self, identifier(parameters["jobId"], "jobId"))
            if source is None or qualification["eligibility"] != "recoverable":
                raise WorkflowError("RECOVERY_UNAVAILABLE", qualification["diagnostic"])
            if parameters["qualificationSha256"] != qualification["qualificationSha256"]:
                raise WorkflowError("RECOVERY_CHANGED", "Recovery selection changed; query and preview it again")
            return dict(parameters)
        raise WorkflowError("UNKNOWN_COMMAND", "Command is not in the capability catalog")

    def _binding(self, command: str, parameters: dict) -> dict:
        if command == "workflow.recover":
            from .recovery import qualify
            qualification, source = qualify(self, parameters["jobId"])
            if source is None or qualification.get("qualificationSha256") != parameters["qualificationSha256"]:
                raise WorkflowError("RECOVERY_CHANGED", "Original recovery evidence changed")
            job = self._job(parameters["jobId"])
            binding = copy.deepcopy(self._state["previews"][job["previewId"]]["binding"])
            # Read-only qualification validates all original sources and results;
            # these selected checkpoint bytes also enter the approval binding.
            binding["recoverySelection"] = {"qualificationSha256": parameters["qualificationSha256"]}
            for variant, selected in qualification["variants"].items():
                if "checkpointPath" in selected:
                    binding["recovery/" + variant] = {"path": selected["checkpointPath"],
                                                     "sha256": selected["checkpointSha256"]}
            return binding
        if command.startswith("extension."):
            from .extensions import bind_command
            return bind_command(self, command, parameters)
        configuration = (parameters if command == "workflow.configure" else self._job(parameters["jobId"])["result"]["configuration"]
                         if command == "workflow.resume" else self._state["configuration"])
        template = TEMPLATES[configuration.get("model", "four-rod")]
        paths = {"template": self.examples / template["file"], "solver": self.solver,
                 "native": self.native, "python": self.python,
                 "analysis": self.examples / "stage9_analysis.py",
                 "controller": self.examples / "stage9_velocity_controller.py",
                 "controllerHost": Path(__file__).with_name("controller_host.py")}
        if self.native_results is not None:
            paths["nativeResults"] = self.native_results
        if "sourceId" in configuration:
            from .model_sources import read_model_source
            source = read_model_source(self.workspace, configuration["sourceId"])
            paths.pop("template")
            paths["modelInput"] = Path(source["path"])
            paths["sourceManifest"] = Path(source["manifestPath"])
        if command == "workflow.resume":
            for artifact in self._job(parameters["jobId"])["artifacts"]:
                # Bind every original accepted artifact, including original script/gain paths.
                path = Path(artifact["path"])
                if not path.is_file() or path.is_symlink() or file_hash(path) != artifact["sha256"]:
                    raise WorkflowError("SOURCE_CHANGED", "An accepted trajectory artifact changed")
                paths[artifact["artifactId"]] = path
            source = self._job(parameters["jobId"])["result"]
            for key, artifact in source["sourceArtifacts"].items():
                path = Path(artifact["path"])
                if not path.is_file() or path.is_symlink() or file_hash(path) != artifact["sha256"]:
                    raise WorkflowError("SOURCE_CHANGED", "Original controller or design provenance changed")
                paths["original/" + key] = path
        try:
            result = {name: {"path": str(path), "sha256": file_hash(path)} for name, path in paths.items()}
        except OSError:
            raise WorkflowError("SOURCE_CHANGED", "A required workflow source is unavailable") from None
        if command in self._commands and self._commands[command]["bind"]:
            result["extension"] = self._commands[command]["bind"](self, parameters)
        return result

    def handle(self, request: dict) -> dict:
        with self._lock:
            operation = "request.reject"
            try:
                if self._closed:
                    raise WorkflowError("SERVICE_CLOSED", "Service is closed")
                exact(request, {"apiVersion", "requestId", "operation", "parameters"})
                parameters = request.get("parameters", {}) if isinstance(request, dict) else {}
                command = parameters.get("command") if isinstance(parameters, dict) else None
                is_extension_preview = (request.get("operation") == "command.preview" and
                                        isinstance(command, str) and command.startswith("extension."))
                finite_tree(request, max_collection_size=100_000 if is_extension_preview else 1024)
                if len(canonical(request)) > MAX_REQUEST_BYTES:
                    raise WorkflowError("REQUEST_TOO_LARGE", "Request exceeds byte limit")
                if request["apiVersion"] != API_VERSION:
                    raise WorkflowError("UNSUPPORTED_VERSION", "Expected range.workflow/v1")
                identifier(request["requestId"], "requestId")
                operation = identifier(request["operation"], "operation")
                if not isinstance(request["parameters"], dict):
                    raise WorkflowError("INVALID_PARAMETERS", "parameters must be an object")
                return copy.deepcopy(self._dispatch(operation, request["parameters"]))
            except WorkflowError as error:
                if not self._fault and not self._closed:
                    self._commit(copy.deepcopy(self._state), "request.reject", "rejected", approval="rejected",
                                 parameters={"operation": operation, "code": error.code})
                raise

    def _dispatch(self, operation: str, p: dict) -> dict:
        if operation == "capabilities.query":
            exact(p, set())
            return self.capabilities()
        if operation == "project.query":
            exact(p, set())
            return self.project_snapshot()
        if operation == "execution.query":
            exact(p, {"requestSha256"})
            request_sha = p["requestSha256"]
            if type(request_sha) is not str or not re.fullmatch(r"[0-9a-f]{64}", request_sha):
                raise WorkflowError("INVALID_PARAMETERS", "Execution lookup requires a SHA-256 reference")
            # This digest is a receipt lookup, never an approval or retry token.
            matches = [v for v in self._state["previews"].values()
                       if v.get("previewHash") == request_sha and "jobId" in v]
            result = {"projectId": self._state["projectId"], "requestSha256": request_sha,
                      "state": "unobserved", "accepted": False}
            if len(matches) > 1:
                raise WorkflowError("AMBIGUOUS_EXECUTION", "Execution receipt identity is ambiguous")
            if matches:
                preview = matches[0]
                job_id = identifier(preview["jobId"], "jobId")
                job = self._job(job_id)
                result.update(state="accepted", accepted=True, jobId=job_id,
                              command=preview["normalized"]["command"],
                              currentJobState=job["state"])
            return result
        if operation == "project.catalog.query":
            exact(p, set(), {"persistentDocumentId"})
            from .project_catalog import query_project_catalog
            selected = identifier(p["persistentDocumentId"], "persistentDocumentId") if "persistentDocumentId" in p else None
            return query_project_catalog(self.workspace, selected)
        if operation == "project.snapshot.query":
            exact(p, {"persistentDocumentId"})
            from .project_catalog import resolve_project_snapshot
            return resolve_project_snapshot(self.workspace, p["persistentDocumentId"])
        if operation == "model.source.register":
            exact(p, {"catalogRoot", "snapshotRef"})
            if not isinstance(p["catalogRoot"], str) or not isinstance(p["snapshotRef"], dict):
                raise WorkflowError("INVALID_PARAMETERS", "A trusted catalog root and snapshot reference are required")
            from .model_sources import register_snapshot_source
            source = register_snapshot_source(self.workspace, Path(p["catalogRoot"]), p["snapshotRef"], self.native)
            try:
                candidate = copy.deepcopy(self._state)
                candidate["revision"] += 1
                self._commit(candidate, "model.source.register", "registered",
                             parameters={"sourceId": source["sourceId"], "originRef": source["originRef"]})
            except WorkflowError:
                raise WorkflowError("SOURCE_REGISTERED_AUDIT_FAILED",
                    "Immutable source " + source["sourceId"] + " was registered; its workflow audit failed. "
                    "Reopen and query model.sources.query before registering again.") from None
            return {key: value for key, value in source.items() if key not in {"path", "manifestPath"}}
        if operation == "model.sources.query":
            exact(p, set(), {"offset", "limit"})
            from .model_sources import list_model_sources
            result = list_model_sources(self.workspace, p.get("offset", 0), p.get("limit", 50))
            result["sources"] = [{key: value for key, value in source.items()
                                  if key not in {"path", "manifestPath"}} for source in result["sources"]]
            return result
        if operation == "model.source.query":
            exact(p, {"sourceId"})
            from .model_sources import read_model_source
            source = read_model_source(self.workspace, identifier(p["sourceId"], "sourceId"))
            return {key: value for key, value in source.items() if key not in {"path", "manifestPath"}}
        if operation in ("jobs.query", "results.query"):
            exact(p, set(), {"jobId"})
            if "jobId" in p:
                return {"job": self._job(p["jobId"])} if operation == "jobs.query" else {"result": self.result_snapshot(p["jobId"])}
            jobs = list(self._state["jobs"].values())
            return {"jobs": jobs} if operation == "jobs.query" else {"results": [x["result"] for x in jobs if x["state"] == "succeeded" and "result" in x]}
        if operation == "job.recovery.query":
            exact(p, {"jobId"})
            from .recovery import qualify
            return self._public_recovery_qualification(qualify(self, identifier(p["jobId"], "jobId"))[0])
        if operation in {"result.catalog.query", "result.channels.query", "result.series.query", "result.summary.query"}:
            from .native_results import query_registered
            return query_registered(self, operation.split(".")[1], p)
        if operation == "telemetry.query":
            exact(p, {"jobId"}, {"afterSeq", "limit", "mode"})
            job_id = identifier(p["jobId"], "jobId")
            after_seq = integer(p.get("afterSeq", 0), "afterSeq", 0, 2**53 - 1)
            limit = integer(p.get("limit", 256), "limit", 1, 512)
            mode = p.get("mode", "active")
            if mode not in ("active", "passive"):
                raise WorkflowError("INVALID_PARAMETERS", "mode must be active or passive")
            job = self._job(job_id)
            scheduled_mode = None
            if job.get("command") == "workflow.run":
                scheduled_mode = job.get("parameters", {}).get("mode")
            elif job.get("command") == "workflow.resume":
                source_id = job.get("parameters", {}).get("jobId")
                source = self._state["jobs"].get(source_id, {})
                source_result = source.get("result", {})
                scheduled_mode = source_result.get("mode") or source.get("parameters", {}).get("mode")
            elif job.get("command") == "workflow.recover":
                scheduled_mode = job.get("result", {}).get("mode")
                attempts = job.get("recovery", {}).get("attempts", [])
                if not scheduled_mode and isinstance(attempts, list) and attempts and isinstance(attempts[-1], dict):
                    partial = attempts[-1].get("partialResult", {})
                    if isinstance(partial, dict):
                        scheduled_mode = partial.get("mode")
            return self._telemetry.query(job_id, job, mode, after_seq, limit, scheduled_mode)
        if operation == "audit.query":
            exact(p, set(), {"afterRevision", "limit"})
            after = integer(p.get("afterRevision", 0), "afterRevision", 0, 2**53 - 1)
            limit = integer(p.get("limit", 100), "limit", 1, 1000)
            items = [x for x in self._state["audit"] if x["revision"] > after][:limit]
            return {"records": items, "auditRevision": self._state["auditRevision"],
                    "nextRevision": items[-1]["revision"] if items else after}
        from .extensions import read_operation
        extension_result = read_operation(self, operation, p)
        if extension_result is not None:
            return extension_result
        if self._fault:
            raise WorkflowError("PERSISTENCE_ERROR", "Service requires reopening after a persistence failure")
        if operation == "command.preview":
            exact(p, {"command", "parameters", "expectedRevision"})
            command = identifier(p["command"], "command")
            expected = integer(p["expectedRevision"], "expectedRevision", 0, 2**53 - 1)
            if expected != self._state["revision"]:
                raise WorkflowError("STALE_REVISION", "Project revision changed")
            if command not in self._commands and not command.startswith("workflow."):
                raise WorkflowError("UNKNOWN_COMMAND", "Command is not in the capability catalog")
            lane = self._command_lane(command)
            if lane == "project" and self._busy("project"):
                raise WorkflowError("PROJECT_BUSY", "A project job already owns the model workflow lane")
            if lane == "extension" and self._active_jobs("extension") >= MAX_EXTENSION_JOBS:
                raise WorkflowError("EXTENSION_BUSY", "Extension job concurrency limit reached")
            extension_id = self._command_extension_id(command, p["parameters"])
            if extension_id and extension_id in self._extension_writers:
                raise WorkflowError("EXTENSION_BUSY", "A registry write for this extension is already running")
            normalized = self._commands[command]["normalize"](self, p["parameters"]) if command in self._commands else self._normalize_builtin(command, p["parameters"])
            finite_tree(normalized, max_collection_size=100_000 if lane == "extension" else 1024)
            candidate = copy.deepcopy(self._state)
            self._compact_stale_extension_previews(candidate["previews"], self._state["revision"],
                                                   self._state["sessionId"])
            if command in ("extension.stage", "extension.run"):
                from .extensions import MAX_PENDING_PREVIEW_BYTES
                retained = sum(len(canonical(preview["normalized"])) for preview in candidate["previews"].values()
                    if preview.get("sessionId") == self._state["sessionId"] and
                       preview.get("revision") == self._state["revision"] and "jobId" not in preview and
                       preview.get("normalized", {}).get("command") in ("extension.stage", "extension.run") and
                       ((preview["normalized"]["command"] == "extension.stage" and
                         "source" in preview["normalized"]["parameters"]) or
                        (preview["normalized"]["command"] == "extension.run" and
                         "series" in preview["normalized"]["parameters"])) )
                if retained + len(canonical({"command": command, "parameters": normalized})) > MAX_PENDING_PREVIEW_BYTES:
                    raise WorkflowError("PENDING_PREVIEW_LIMIT",
                        "Pending extension preview data exceeds 8 MiB; execute an approved preview or reopen the service to expire stale previews")
            value = {"previewId": str(uuid.uuid4()), "projectId": self._state["projectId"],
                     "modelId": self._state["modelId"], "sessionId": self._state["sessionId"],
                     "revision": expected, "normalized": {"command": command, "parameters": normalized},
                     "binding": self._binding(command, normalized)}
            value["effectiveConfiguration"] = (copy.deepcopy(self._job(normalized["jobId"])["result"]["configuration"])
                if command == "workflow.resume" else copy.deepcopy(self._job(normalized["jobId"])["configuration"])
                if command == "workflow.recover" else normalized if command == "workflow.configure" else copy.deepcopy(self._state["configuration"]))
            if command in ("workflow.configure", "workflow.run", "workflow.resume", "workflow.recover"):
                value["modelBinding"] = self._model_binding(value["effectiveConfiguration"])
            if command == "workflow.recover":
                from .recovery import qualify
                value["recoverySelection"] = self._public_recovery_qualification(qualify(self, normalized["jobId"])[0])
            value["previewHash"] = digest(value)
            if len(candidate["previews"]) >= 1024:
                raise WorkflowError("PREVIEW_LIMIT", "Workspace preview limit reached")
            candidate["previews"][value["previewId"]] = value
            self._commit(candidate, "command.preview", "previewed", approval="pending",
                         parameters={"command": command, "previewHash": value["previewHash"]})
            return value
        if operation == "host.approve":
            exact(p, {"previewId", "previewHash"})
            preview = self._preview(p["previewId"])
            if p["previewHash"] != preview["previewHash"]:
                raise WorkflowError("PREVIEW_MISMATCH", "Approval hash does not match preview")
            if preview["sessionId"] != self._state["sessionId"]:
                raise WorkflowError("STALE_REVISION", "Approval belongs to an earlier session")
            if "jobId" in preview:
                return {"approvalToken": self._approval_tokens.get(preview["previewId"], ""), "previewId": preview["previewId"]}
            self._validate_preview(preview)
            candidate = copy.deepcopy(self._state)
            approved = candidate["previews"][preview["previewId"]]
            token = self._approval_tokens.get(preview["previewId"]) or secrets.token_urlsafe(32)
            approved["approvalState"] = "approved"
            self._commit(candidate, "host.approve", "approved", approval="approved", actor="user",
                         parameters={"previewHash": preview["previewHash"]})
            self._approval_tokens[preview["previewId"]] = token
            return {"approvalToken": token, "previewId": preview["previewId"]}
        if operation == "command.execute":
            exact(p, {"previewId", "approvalToken"})
            preview = self._preview(p["previewId"])
            token = p["approvalToken"]
            if preview["sessionId"] != self._state["sessionId"]:
                raise WorkflowError("STALE_REVISION", "Approval belongs to an earlier session")
            if not isinstance(token, str) or not secrets.compare_digest(token, self._approval_tokens.get(preview["previewId"], "")) or not token:
                raise WorkflowError("APPROVAL_REQUIRED", "A matching host approval is required")
            if "jobId" in preview:
                job = self._job(preview["jobId"])
                return {"jobId": job["jobId"], "state": job["state"], "revision": self._state["revision"]}
            self._validate_preview(preview)
            command = preview["normalized"]["command"]
            command_entry = self._commands.get(command)
            lane = command_entry.get("lane", "project") if command_entry else "project"
            if lane == "project" and self._busy("project"):
                raise WorkflowError("PROJECT_BUSY", "A project job already owns the model workflow lane")
            if lane == "extension" and self._active_jobs("extension") >= MAX_EXTENSION_JOBS:
                raise WorkflowError("EXTENSION_BUSY", "Extension job concurrency limit reached")
            if command_entry and command_entry["descriptor"].get("trustedRuntimeRequired") and not self.trusted_extensions:
                raise WorkflowError("TRUSTED_RUNTIME_REQUIRED", "Enable trusted developer extension mode before executing Python")
            extension_id = self._command_extension_id(command, preview["normalized"]["parameters"])
            registry_write = bool(command_entry and command_entry.get("registry_write"))
            if registry_write and extension_id in self._extension_writers:
                raise WorkflowError("EXTENSION_BUSY", "A registry write for this extension is already running")
            jid = str(uuid.uuid4())
            execution_parameters = copy.deepcopy(preview["normalized"]["parameters"])
            stored_parameters = execution_parameters
            recovering_job = None
            if command == "workflow.recover":
                from .recovery import qualify
                qualification, source = qualify(self, execution_parameters["jobId"])
                if source is None or qualification.get("qualificationSha256") != execution_parameters["qualificationSha256"]:
                    raise WorkflowError("RECOVERY_CHANGED", "Recovery evidence changed before acceptance")
                jid = execution_parameters["jobId"]
                recovering_job = copy.deepcopy(self._job(jid))
                execution_parameters = {**execution_parameters, "_recoverySource": source}
            if command.startswith("extension."):
                from .extensions import compact_job_parameters
                stored_parameters = compact_job_parameters(command, execution_parameters)
            candidate = copy.deepcopy(self._state)
            candidate["revision"] += 1
            candidate["previews"][preview["previewId"]]["jobId"] = jid
            candidate["previews"][preview["previewId"]]["normalized"]["parameters"] = copy.deepcopy(stored_parameters)
            job = {"jobId": jid, "state": "queued", "command": command,
                   "parameters": copy.deepcopy(stored_parameters),
                   "configuration": copy.deepcopy(preview["effectiveConfiguration"]), "createdAt": utc(),
                   "previewId": preview["previewId"], "previewHash": preview["previewHash"],
                   "projectId": candidate["projectId"], "modelId": candidate["modelId"], "lane": lane,
                   "revision": candidate["revision"]}
            if recovering_job is not None:
                job["recovery"] = recovering_job["recovery"]
                job["attemptId"] = str(uuid.uuid4())
                job["priorAttempts"] = recovering_job.get("priorAttempts", []) + [{
                    "previewId": recovering_job["previewId"], "command": recovering_job["command"],
                    "state": recovering_job["state"], "error": recovering_job.get("error"),
                    "attemptId": recovering_job.get("attemptId", jid)}]
            candidate["jobs"][jid] = job
            self._compact_stale_extension_previews(candidate["previews"], candidate["revision"],
                                                   candidate["sessionId"])
            self._commit(candidate, "command.execute", "accepted", job_id=jid, approval="approved",
                         parameters={"command": job["command"], "previewHash": job["previewHash"]})
            cancel = threading.Event()
            worker = threading.Thread(target=self._run_job, args=(jid, cancel, execution_parameters), daemon=True)
            self._workers[jid] = (worker, cancel)
            if registry_write and extension_id:
                self._extension_writers[extension_id] = jid
            worker.start()
            return {"jobId": jid, "state": "queued", "revision": self._state["revision"]}
        if operation == "job.cancel":
            exact(p, {"jobId"})
            job = self._job(p["jobId"])
            if job["state"] in TERMINAL_STATES:
                return {"jobId": job["jobId"], "state": job["state"]}
            candidate = copy.deepcopy(self._state)
            candidate["revision"] += 1
            candidate["jobs"][job["jobId"]]["state"] = "cancelling"
            self._commit(candidate, "job.cancel", "cancelling", job_id=job["jobId"])
            self._workers[job["jobId"]][1].set()
            return {"jobId": job["jobId"], "state": "cancelling"}
        raise WorkflowError("UNKNOWN_OPERATION", "Operation is not supported")

    def _preview(self, preview_id: str) -> dict:
        identifier(preview_id, "previewId")
        if preview_id not in self._state["previews"]:
            raise WorkflowError("PREVIEW_NOT_FOUND", "Unknown previewId")
        return self._state["previews"][preview_id]

    @staticmethod
    def _public_recovery_qualification(qualification: dict) -> dict:
        # Selection is opaque outside the file-owner service. Hashes, policy,
        # sequence and original attempt identity remain inspectable.
        def redact(value):
            if isinstance(value, dict):
                return {key: redact(item) for key, item in value.items()
                        if key == "relativePath" or (key != "path" and not key.endswith("Path") and not key.endswith("Directory"))}
            if isinstance(value, list):
                return [redact(item) for item in value]
            return value
        return redact(qualification)

    def _validate_preview(self, preview: dict):
        if preview["sessionId"] != self._state["sessionId"] or preview["revision"] != self._state["revision"]:
            raise WorkflowError("STALE_REVISION", "Preview belongs to an earlier project revision or session")
        normalized = preview["normalized"]
        try:
            current = self._binding(normalized["command"], normalized["parameters"])
        except OSError:
            raise WorkflowError("SOURCE_CHANGED", "A preview source is missing") from None
        if current != preview["binding"]:
            raise WorkflowError("SOURCE_CHANGED", "Template, source artifacts, or execution tools changed")

    def _artifacts(self, directory: Path) -> list[dict]:
        files = sorted(x for x in directory.rglob("*") if x.is_file())
        if len(files) > 12288:
            raise WorkflowError("ARTIFACT_LIMIT", "Job produced too many files")
        artifacts = []
        total_bytes = 0
        for index, path in enumerate(files):
            if path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
                raise WorkflowError("ARTIFACT_PATH", "Artifact escaped its job directory")
            size = path.stat().st_size
            total_bytes += size
            if size > 128 * 1024 * 1024 or total_bytes > 2 * 1024 * 1024 * 1024:
                raise WorkflowError("ARTIFACT_LIMIT", "Job artifacts exceed the declared byte budget")
            artifacts.append({"artifactId": f"artifact/{directory.name}/{index}", "path": str(path),
                              "sha256": file_hash(path), "sizeBytes": size})
        return artifacts

    def _run_job(self, job_id: str, cancel: threading.Event, execution_parameters: dict | None = None):
        directory = None
        try:
            with self._lock:
                candidate = copy.deepcopy(self._state)
                job = candidate["jobs"][job_id]
                if cancel.is_set():
                    raise ProcessFailure("CANCELLED", "Job was cancelled")
                job.update(state="running", startedAt=utc())
                self._commit(candidate, "job.start", "running", job_id=job_id)
                job = copy.deepcopy(job)
                if execution_parameters is not None:
                    job["parameters"] = copy.deepcopy(execution_parameters)
                binding = copy.deepcopy(self._state["previews"][job["previewId"]]["binding"])
                if job.get("lane") == "extension":
                    job["_extensionBinding"] = binding
            directory = self.workspace / "jobs" / job_id
            if "attemptId" in job:
                directory = directory / "attempts" / job["attemptId"]
            directory.mkdir(parents=True, exist_ok=False)
            if job["command"] == "workflow.configure":
                result = {"jobId": job_id, "configuration": job["parameters"]}
            elif job["command"] in self._commands:
                result = self._commands[job["command"]]["execute"](self, job, directory, cancel)
            else:
                from .jobs import run_native_workflow
                result = run_native_workflow(self, job, directory, cancel, binding)
            # Native manifests may bind all 5,000 actual frames per variant.
            # Request limits remain smaller; this is a bounded trusted result.
            finite_tree(result, max_collection_size=12288 if job["command"] in
                        ("workflow.run", "workflow.resume", "workflow.recover") else 1024)
            result.setdefault("jobId", job_id)
            artifacts = self._artifacts(self.workspace / "jobs" / job_id)
            with self._lock:
                if self._fault:
                    raise WorkflowError("PERSISTENCE_ERROR", "Cannot publish after a persistence failure")
                if cancel.is_set():
                    raise ProcessFailure("CANCELLED", "Job was cancelled")
                candidate = copy.deepcopy(self._state)
                candidate["revision"] += 1
                if job["command"] == "workflow.configure":
                    candidate["configuration"] = job["parameters"]
                update = result.pop("extensionUpdate", None)
                if update is not None:
                    if job["command"].startswith("extension."):
                        from .extensions import merge_update
                        capability_revision = merge_update(candidate["extensions"], update)
                        if update["operation"] in ("activate", "rollback"):
                            result["capabilityRevision"] = capability_revision
                    else:
                        exact(update, {"extensionId", "entry"})
                        identifier(update["extensionId"], "extensionId")
                        candidate["extensions"][update["extensionId"]] = update["entry"]
                candidate["jobs"][job_id].update(state="succeeded", completedAt=utc(), result=result, artifacts=artifacts)
                self._commit(candidate, "job.publish", "succeeded", job_id=job_id, artifacts=artifacts,
                             parameters={"command": job["command"], "artifactCount": len(artifacts)})
        except Exception as error:
            code = getattr(error, "code", "EXECUTION_FAILED")
            message = str(error) if isinstance(error, (WorkflowError, ProcessFailure)) else "Job execution failed; inspect job diagnostics"
            if not isinstance(error, (WorkflowError, ProcessFailure)) and directory is not None:
                # Diagnostics contain locations only, never exception values,
                # source lines, locals, requests, approval tokens or credentials.
                import traceback
                frames = traceback.extract_tb(error.__traceback__, limit=32)
                diagnostics = {"exceptionType": type(error).__name__[:64], "frames": [
                    {"module": Path(frame.filename).name[:128], "function": frame.name[:128], "line": frame.lineno}
                    for frame in frames]}
                try:
                    (directory / "execution-diagnostics.json").write_bytes(canonical(diagnostics))
                except OSError:
                    pass
            with self._lock:
                if not self._fault:
                    candidate = copy.deepcopy(self._state)
                    candidate["revision"] += 1
                    cancelled = cancel.is_set() or code == "CANCELLED"
                    state = "cancelled" if cancelled else "failed"
                    candidate["jobs"][job_id].update(state=state, completedAt=utc(),
                        error={"code": "CANCELLED" if cancelled else code, "message": message[:4096]})
                    candidate["jobs"][job_id].pop("result", None)
                    try:
                        self._commit(candidate, "job.finish", state, job_id=job_id,
                                     parameters={"code": code})
                    except WorkflowError:
                        self._fault = True
        finally:
            with self._lock:
                stored_job = self._state.get("jobs", {}).get(job_id, {})
                command = stored_job.get("command", "")
                command_entry = self._commands.get(command)
                if command_entry and command_entry.get("registry_write"):
                    extension_id = self._command_extension_id(command, stored_job.get("parameters", {}))
                    if extension_id and self._extension_writers.get(extension_id) == job_id:
                        self._extension_writers.pop(extension_id, None)

    def capabilities(self) -> dict:
        from .extensions import MAX_PENDING_PREVIEW_BYTES
        def schema(properties, required=None):
            return {"type": "object", "properties": properties, "required": list(properties) if required is None else required,
                    "additionalProperties": False}
        id_schema = {"type": "string", "pattern": "^[A-Za-z0-9._:-]{1,128}$"}
        revision_schema = {"type": "integer", "minimum": 0, "maximum": 2**53 - 1}
        operation_parameters = {
            "capabilities.query": schema({}), "project.query": schema({}),
            "execution.query": schema({"requestSha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"}}),
            "project.catalog.query": schema({"persistentDocumentId": id_schema}, []),
            "project.snapshot.query": schema({"persistentDocumentId": id_schema}),
            "jobs.query": schema({"jobId": id_schema}, []), "results.query": schema({"jobId": id_schema}, []),
            "job.recovery.query": schema({"jobId": id_schema}),
            "telemetry.query": schema({"jobId": id_schema,
                                        "afterSeq": {"type": "integer", "minimum": 0, "maximum": 2**53 - 1},
                                        "limit": {"type": "integer", "minimum": 1, "maximum": 512},
                                        "mode": {"type": "string", "enum": ["active", "passive"]}}, ["jobId"]),
            "audit.query": schema({"afterRevision": revision_schema, "limit": {"type": "integer", "minimum": 1, "maximum": 1000}}, []),
            "command.preview": schema({"command": id_schema, "parameters": {"type": "object"}, "expectedRevision": revision_schema}),
            "command.execute": schema({"previewId": id_schema, "approvalToken": {"type": "string", "minLength": 1, "maxLength": 128}}),
            "job.cancel": schema({"jobId": id_schema}),
            "extension.list": schema({"extensionId": id_schema,
                                       "candidateOffset": {"type": "integer", "minimum": 0, "maximum": 64},
                                       "candidateLimit": {"type": "integer", "minimum": 1, "maximum": 32}}, []),
            "extension.candidate.get": schema({"candidateId": id_schema}),
            "extension.version.get": schema({"extensionId": id_schema, "bundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"}}),
            "schema.get": schema({"schemaId": {"type": "string", "enum": [
                "range.extension/v1/manifest", "range.extension/v1/numeric-series", "range.extension/v1/statistics"]}}),
            "knowledge.query": schema({"query": {"type": "string", "maxLength": 256},
                                        "extensionId": id_schema}, ["query"])}
        operation_parameters["model.source.register"] = schema({"catalogRoot": {"type": "string"},
                                                               "snapshotRef": {"type": "object"}})
        operation_parameters["model.sources.query"] = schema({"offset": {"type": "integer", "minimum": 0},
                                                             "limit": {"type": "integer", "minimum": 1, "maximum": 100}}, [])
        operation_parameters["model.source.query"] = schema({"sourceId": id_schema})
        for quantity in ("catalog", "channels", "series", "summary"):
            operation_parameters["result." + quantity + ".query"] = schema({
                "jobId": id_schema, "mode": {"enum": ["active", "passive"]}, "artifactId": id_schema,
                "channelId": id_schema, "nodePhysicalId": {"type": "integer", "minimum": 1},
                "offset": {"type": "integer", "minimum": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": 200},
                "cursor": {"type": "string", "maxLength": 16384}}, ["jobId"])
        read_operations = {"capabilities.query", "project.query", "project.catalog.query", "project.snapshot.query", "jobs.query", "results.query", "telemetry.query", "audit.query",
                           "model.sources.query", "model.source.query", "job.recovery.query",
                           "extension.list", "extension.candidate.get", "extension.version.get", "schema.get", "knowledge.query"}
        operations = [{"id": name, "readOnly": name in read_operations, "parameters": parameters}
                      for name, parameters in operation_parameters.items()]
        for item in operations:
            if item["id"].startswith("result."):
                item["readOnly"] = True
            if item["id"] == "model.source.register":
                item["trustedOwnerOnly"] = True
        commands = [{"id": "workflow.configure", "readOnly": False, "requiresApproval": True,
                     "parameters": schema({"dt": {"type": "number", "exclusiveMinimum": 0, "maximum": .02, "description": "Time step in seconds"},
                                           "steps": {"type": "integer", "minimum": 1, "maximum": MAX_STEPS},
                                           "targetDamping": {"type": "number", "exclusiveMinimum": 0, "maximum": .5, "description": "Dimensionless modal damping ratio"},
                                           "model": {"type": "string", "enum": list(TEMPLATES), "description": "Explicit family chooses its legacy template unless sourceId is provided; omission preserves the current source"},
                                           "sourceId": id_schema},
                                          ["dt", "steps", "targetDamping"])},
                    {"id": "workflow.run", "readOnly": False, "requiresApproval": True,
                     "parameters": schema({"mode": {"type": "string", "enum": ["active", "passive", "both"]},
                                           "independentReference": {"type": "boolean", "const": True}})},
                    {"id": "workflow.resume", "readOnly": False, "requiresApproval": True,
                     "precondition": "Successful native job, unchanged source artifacts, total steps <=5000",
                     "parameters": schema({"jobId": id_schema, "additionalSteps": {"type": "integer", "minimum": 1, "maximum": MAX_STEPS}})},
                    {"id": "workflow.recover", "readOnly": False, "requiresApproval": True,
                     "precondition": "Interrupted logical job with a freshly qualified original attempt",
                     "parameters": schema({"jobId": id_schema,
                         "qualificationSha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"}})}]
        commands += [{"id": name, **entry["descriptor"]} for name, entry in self._commands.items()]
        return {"apiVersion": API_VERSION, "operations": operations, "commands": commands,
                "capabilityRevision": self._capability_revision(),
                "extensionRuntime": {"trustedExtensions": self.trusted_extensions,
                    "trustMode": "trusted-developer" if self.trusted_extensions else "disabled",
                    "osSandbox": False, "protocolVersion": "range.extension/v1",
                    "maxRuntimeSeconds": 15, "maxOutputBytes": 64 * 1024,
                    "maxConcurrentJobs": MAX_EXTENSION_JOBS},
                "models": [{"id": name, "templateFile": data["file"], "supportedCoordinates": [dof for _, dof in data["dofs"]]} for name, data in TEMPLATES.items()],
                "limits": {"maxRequestBytes": MAX_REQUEST_BYTES, "maxProjectJobs": 1,
                           "maxExtensionJobs": MAX_EXTENSION_JOBS,
                           "maxPendingExtensionPreviewBytes": MAX_PENDING_PREVIEW_BYTES,
                           "maxTotalSteps": MAX_STEPS, "processTimeoutSeconds": 180,
                           "maxProcessLogBytes": self.runner.MAX_LOG_BYTES},
                "approval": "Host approval is separate from the Agent tool catalog"}

    def close(self):
        with self._lock:
            if self._closed:
                return
            self._closed = True
            workers = list(self._workers.values())
            for _, cancel in workers:
                cancel.set()
        for worker, _ in workers:
            worker.join(15)
        self._os_lock.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
