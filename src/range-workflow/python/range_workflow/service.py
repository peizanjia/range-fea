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
from types import SimpleNamespace
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
READ_OPERATIONS = frozenset({"result.plot.sources.query", "result.plot.query", "result.raw.query",
                             "result.raw.export", "result.raw.export.chunk", "extension.result.query"})
MAX_READ_QUERIES = 4
MAX_READ_CONTEXT_BYTES = 8 * 1024 * 1024
RUNTIME_VERIFICATION_QUERY = "extension.runtime.verification.query"
RUNTIME_VERIFICATION_OPERATIONS = frozenset({"extension.runtime.verify", RUNTIME_VERIFICATION_QUERY,
                                           "host.runtime.verification.cancel"})
MAX_RUNTIME_VERIFICATIONS = 2
MAX_RETAINED_RUNTIME_VERIFICATIONS = 8


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


def _read_parameters(service, operation, p):
    """Validate the closed read schemas before allocating any worker context."""
    from . import series_workbench as workbench
    from . import series_sources as sources
    export = None
    if operation == "result.plot.sources.query":
        exact(p, {"jobId"}, {"mode"})
        identifier(p["jobId"], "jobId")
        if p.get("mode", "active") not in ("active", "passive"):
            raise WorkflowError("INVALID_PARAMETERS", "Native mode must be active or passive")
        selected = [{"jobId": p["jobId"], "mode": p.get("mode", "active")}]
    elif operation == "result.plot.query":
        exact(p, {"view", "sources"}, {"options"})
        if type(p["view"]) is not str or p["view"] not in workbench._VIEWS:
            raise WorkflowError("INVALID_PARAMETERS", "Plot view is unsupported")
        selected = workbench._source_list(p["sources"])
        workbench._options(p.get("options", {}))
        if p["view"] in ("error", "phase") and len(selected) != 2:
            raise WorkflowError("INVALID_PARAMETERS", "Error and phase views require exactly two sources")
    elif operation == "result.raw.query":
        exact(p, {"source"}, {"offset", "limit"})
        selected = [sources.selector(p["source"])]
        integer(p.get("offset", 0), "offset", 0, sources.MAX_SAMPLES)
        integer(p.get("limit", 200), "limit", 1, 200)
    elif operation == "result.raw.export":
        exact(p, {"sources", "range"})
        if p["range"] != "complete":
            raise WorkflowError("INVALID_PARAMETERS", "Raw exports require the complete source range")
        selected = workbench._source_list(p["sources"])
        if any("sourceHash" not in s for s in selected):
            raise WorkflowError("INVALID_PARAMETERS", "Export requires held source hashes")
    elif operation == "result.raw.export.chunk":
        exact(p, {"exportId", "offset", "length"})
        export_id = identifier(p["exportId"], "exportId")
        integer(p["offset"], "offset", 0, workbench._MAX_EXPORT_BYTES)
        integer(p["length"], "length", 1, 65536)
        with workbench._EXPORT_LOCK:
            export = copy.deepcopy(getattr(service, "_series_exports", {}).get(export_id))
        if export is None:
            raise WorkflowError("EXPORT_UNAVAILABLE", "Managed export identity is unavailable")
        selected = workbench._source_list([s["selector"] for s in export["sources"]])
    else:
        exact(p, {"jobId"}, {"channel", "offset", "limit", "view"})
        identifier(p["jobId"], "jobId")
        integer(p.get("channel", 0), "channel", 0, 1023)
        integer(p.get("offset", 0), "offset", 0, 100_000)
        integer(p.get("limit", 256), "limit", 1, 1000)
        if p.get("view", "amplitude") not in ("filtered-time", "amplitude", "psd", "real", "imag", "phase"):
            raise WorkflowError("INVALID_PARAMETERS", "Unknown scientific plot view")
        selected = [{"jobId": p["jobId"]}]
    return selected, export


def _read_native_binding(workspace, job, source):
    descriptor = job.get("result", {}).get("nativeResults", {}).get(source["mode"])
    if type(descriptor) is not dict:
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE", "Source lacks a registered native manifest")
    path = Path(descriptor.get("manifestPath", ""))
    root = workspace / "jobs" / job["jobId"]
    if (not path.is_absolute() or not path.resolve().is_relative_to(root.resolve())
            or any(p.is_symlink() for p in (path, *path.parents) if p.is_relative_to(root))):
        raise WorkflowError("ARTIFACT_PATH", "Native manifest must remain inside its registered job")
    entries = [a for a in job.get("artifacts", []) if Path(a.get("path", "")) == path]
    if (len(entries) != 1 or entries[0].get("sha256") != descriptor.get("manifestSha256")
            or not re.fullmatch(r"[0-9a-f]{64}", str(descriptor.get("manifestSha256", "")))
            or source.get("artifactId", descriptor.get("artifactId")) != descriptor.get("artifactId")):
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Native manifest is not bound to this source ledger")


class _ReadQuery:
    """Private completed-source view. It exposes no live dispatch or approval methods."""
    def __init__(self, owner, operation, parameters, jobs, export, request_id):
        self.owner, self.operation, self.parameters = owner, operation, parameters
        self.request_id = request_id
        self.workspace, self.native_results = owner.workspace, owner.native_results
        self._state = {"jobs": jobs}
        self._project = owner._state["projectId"]
        self._session = owner._state["sessionId"]
        self._series_exports = {} if export is None else {export["exportId"]: export}
        self._original_export = export
        self.cancel, self.done = threading.Event(), threading.Event()
        self.cancel_code = "SERVICE_CLOSED"
        self._started = False

    def _job(self, job_id):
        if self.cancel.is_set():
            raise WorkflowError(self.cancel_code, "Read query was cancelled")
        identifier(job_id, "jobId")
        if job_id not in self._state["jobs"]:
            raise WorkflowError("SOURCE_CHANGED", "Source owner is outside the captured closure")
        # Helpers cannot mutate even their private ledger through this accessor.
        return copy.deepcopy(self._state["jobs"][job_id])

    def _read_source_gate(self):
        owner = self.owner
        with owner._lock:
            if owner._closed or self.cancel.is_set():
                raise WorkflowError(self.cancel_code, "Read query was cancelled")
            if (owner.workspace != self.workspace or owner.native_results != self.native_results
                    or owner._state["projectId"] != self._project or owner._state["sessionId"] != self._session):
                raise WorkflowError("READ_SOURCE_CHANGED", "Read workspace identity changed")
            for job_id, captured in self._state["jobs"].items():
                current = owner._state["jobs"].get(job_id, {})
                if {k: current[k] for k in captured if k in current} != captured:
                    raise WorkflowError("READ_SOURCE_CHANGED", "Completed source ledger changed")

    def _verify_files(self):
        # Hash only manifests and source JSON used by these helpers, outside the owner lock.
        for job in self._state["jobs"].values():
            result = job.get("result", {})
            selected = {result[k] for k in ("referencePath", "linearPath", "gainsPath", "sourceReferencePath")
                        if isinstance(result.get(k), str)}
            selected.update(d["manifestPath"] for d in result.get("nativeResults", {}).values()
                            if isinstance(d, dict) and isinstance(d.get("manifestPath"), str))
            if self.operation == "extension.result.query":
                artifact = result.get("scientificResult", {})
                if isinstance(artifact.get("path"), str):
                    selected.add(str(self.workspace / artifact["path"]))
                selected.add(str(self.workspace / "jobs" / job["jobId"] / "input-provenance.json"))
            for raw in selected:
                if self.cancel.is_set():
                    raise WorkflowError(self.cancel_code, "Read query was cancelled")
                path = Path(raw)
                try:
                    relative = path.relative_to(self.workspace / "jobs")
                    owning = self._state["jobs"][relative.parts[0]]
                    entries = [a for a in owning.get("artifacts", []) if a.get("path") == str(path)]
                    if len(entries) != 1:
                        raise ValueError()
                    cursor = self.workspace
                    for part in path.relative_to(self.workspace).parts:
                        cursor /= part
                        if cursor.is_symlink():
                            raise ValueError()
                    if (not path.is_file() or not path.resolve(strict=True).is_relative_to(self.workspace.resolve())
                            or file_hash(path) != entries[0]["sha256"]):
                        raise ValueError()
                except (OSError, ValueError, KeyError, IndexError):
                    raise WorkflowError("READ_SOURCE_CHANGED", "Completed source artifact changed") from None

    def _discard_exports(self):
        if self.operation != "result.raw.export":
            return
        for export_id in self._series_exports:
            path = self.workspace / "derived" / "series-exports" / (export_id + ".csv")
            try:
                if not path.is_symlink() and path.is_file():
                    path.unlink()
            except OSError:
                pass

    def run(self):
        from . import native_results
        from . import series_workbench as workbench
        with self.owner._lock:
            if self._started:
                raise WorkflowError("INVALID_PARAMETERS", "Read query can only run once")
            self._started = True
        try:
            self._read_source_gate()
            self._verify_files()
            with native_results.query_cancellation(self.cancel):
                if self.operation == "result.plot.sources.query":
                    from .series_sources import catalog
                    result = catalog(self, self.parameters)
                elif self.operation == "extension.result.query":
                    from .scientific_extensions import result_query
                    result = result_query(self, self.parameters)
                else:
                    result = workbench.handle(self, self.operation, self.parameters)
            self._verify_files()
            self._read_source_gate()
            if self.operation == "result.raw.export":
                with self.owner._lock, workbench._EXPORT_LOCK:
                    self._read_source_gate()
                    if not hasattr(self.owner, "_series_exports"):
                        self.owner._series_exports = {}
                    if len(self.owner._series_exports) >= 16:
                        raise WorkflowError("SOURCE_LIMIT", "Managed export quota reached")
                    self.owner._series_exports.update(copy.deepcopy(self._series_exports))
            return copy.deepcopy(result)
        except WorkflowError as error:
            if self.cancel.is_set():
                error = WorkflowError(self.cancel_code, "Read query was cancelled")
            self._discard_exports()
            with self.owner._lock:
                # A stale view must not write an audit into a switched workspace.
                if (self.owner.workspace == self.workspace and self.owner._state["projectId"] == self._project
                        and self.owner._state["sessionId"] == self._session):
                    self.owner._read_reject(self.operation, error)
            raise error
        except BaseException:
            self._discard_exports()
            raise
        finally:
            with self.owner._lock:
                self.owner._read_queries.discard(self)
            self.done.set()

    def reject_busy(self):
        """Release a prepared view if stdio still owns all four reply slots."""
        error = WorkflowError("READ_QUERY_BUSY", "The bounded read lane is full")
        with self.owner._lock:
            self.owner._read_queries.discard(self)
            self.owner._read_reject(self.operation, error)
        self.done.set()
        raise error


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
        self._read_queries: set[_ReadQuery] = set()
        self._runtime_verifications: dict[str, dict] = {}
        self._runtime_verification_sources: dict[str, Any] = {}
        self._runtime_verification_active: set[str] = set()
        self._runtime_verification_queue: list[str] = []
        self._runtime_verification_condition = threading.Condition(self._lock)
        self._runtime_verification_worker: threading.Thread | None = None
        self._fault = False
        from .task_runtime import TaskManager
        self._plans = TaskManager(self)
        from .runtime_manager import RuntimeManager
        self._runtime = RuntimeManager(self)
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
            self._plans.restore(candidate)
            self._runtime.restore(candidate)
            self._commit(candidate, "session.open", "opened")
            self._plans.start()
        except BaseException:
            self._os_lock.close()
            raise

    def _load(self) -> dict:
        from .project_catalog import load_project_catalog
        catalog = load_project_catalog(self.workspace)
        if not self._state_path.exists():
            return {"storageVersion": 3, "projectId": catalog["projectId"] if catalog else str(uuid.uuid4()), "modelId": str(uuid.uuid4()),
                    "sessionId": str(uuid.uuid4()), "revision": 0, "auditRevision": 0,
                    "configuration": {"dt": 0.005, "steps": 400, "targetDamping": 0.1, "model": "four-rod"},
                    "jobs": {}, "previews": {}, "audit": [], "extensions": {}, "plans": {}}
        try:
            if self._state_path.stat().st_size > MAX_STATE_BYTES:
                raise ValueError("state too large")
            value = json.loads(self._state_path.read_bytes())
            if type(value.get("storageVersion")) is not int:
                raise ValueError("invalid storage version type")
            if value.get("storageVersion") in (1, 2):
                if "plans" in value:
                    raise ValueError("unexpected plans in legacy state")
                value["plans"] = {}
            exact(value, {"storageVersion", "projectId", "modelId", "sessionId", "revision",
                          "auditRevision", "configuration", "jobs", "previews", "audit", "extensions", "plans"})
            if value["storageVersion"] not in (1, 2, 3):
                raise ValueError("unsupported storage version")
            for key in ("projectId", "modelId", "sessionId"):
                if uuid.UUID(value[key]).int == 0:
                    raise ValueError("null identity")
            integer(value["revision"], "revision", 0, 2**53 - 1)
            integer(value["auditRevision"], "auditRevision", 0, 2**53 - 1)
            self._normalize_builtin("workflow.configure", value["configuration"], state=value)
            if not all(isinstance(value[k], dict) for k in ("jobs", "previews", "extensions", "plans")) or len(value["plans"]) > 16:
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
            value["storageVersion"] = 3
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

    def _require_scientific_runtime_verification(self, command: str, parameters: dict):
        """Gate v2 runtime consumers before normalization/binding can enter hashing.

        Called under the owner lock. Only inspect the registry and ephemeral
        receipts here; warm runtime identity still checks file-set/stat currency.
        """
        if command not in {"extension.validate", "extension.activate", "extension.run", "extension.rollback"}:
            return
        if not isinstance(parameters, dict):
            raise WorkflowError("INVALID_PARAMETERS", "parameters must be an object")
        record = None
        if command in {"extension.validate", "extension.activate"}:
            candidate_id = parameters.get("candidateId")
            if isinstance(candidate_id, str):
                record = next((entry.get("candidates", {}).get(candidate_id)
                               for entry in self._state["extensions"].values()
                               if candidate_id in entry.get("candidates", {})), None)
        else:
            extension_id = parameters.get("extensionId")
            bundle = parameters.get("targetBundleHash" if command == "extension.rollback" else "bundleHash")
            if isinstance(extension_id, str) and isinstance(bundle, str):
                record = self._state["extensions"].get(extension_id, {}).get("versions", {}).get(bundle)
        # Leave missing/malformed identifiers to the existing closed normalizers.
        if record is None or record.get("manifest", {}).get("sdkVersion") != "range.extension/v2":
            return
        if any(self._runtime_verification_sources[key].python == self.python
               for key in self._runtime_verification_active):
            raise WorkflowError("RUNTIME_VERIFICATION_IN_PROGRESS", "Scientific runtime verification is still in progress")
        if not any(record["state"] == "ready" and record["isCurrent"] is True
                   and record["currentity"] == "current" and self._runtime_verification_sources[key].python == self.python
                   for key, record in self._runtime_verifications.items()):
            raise WorkflowError("RUNTIME_VERIFICATION_REQUIRED", "Verify the configured scientific runtime before previewing this command")

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
            exact(parameters, {"mode", "independentReference"}, {"runtimeControl", "pauseAtAcceptedSeq"})
            if parameters["mode"] not in ("active", "passive", "both") or parameters["independentReference"] is not True:
                raise WorkflowError("INVALID_PARAMETERS", "mode must be active/passive/both and independentReference must be true")
            if "runtimeControl" in parameters and type(parameters["runtimeControl"]) is not bool:
                raise WorkflowError("INVALID_PARAMETERS", "runtimeControl must be an explicit boolean")
            if parameters.get("runtimeControl"):
                if "sourceId" not in state["configuration"] or parameters["mode"] == "passive":
                    raise WorkflowError("RUNTIME_UNSUPPORTED", "Runtime control requires an active registered native source")
            if "pauseAtAcceptedSeq" in parameters:
                if not parameters.get("runtimeControl"):
                    raise WorkflowError("INVALID_PARAMETERS", "An initial runtime pause requires runtimeControl")
                integer(parameters["pauseAtAcceptedSeq"], "pauseAtAcceptedSeq", 1, state["configuration"]["steps"])
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
                 "controllerHost": Path(__file__).with_name("controller_host.py"),
                 "runtimeParametersCodec": Path(__file__).with_name("runtime_parameters.py")}
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

    def _inspect_scientific_input(self, request: dict) -> dict:
        """Bind caller-authored test data only; no runtime, provenance or lifecycle IO."""
        from . import scientific_contract as contract
        with self._lock:
            if self._closed:
                raise WorkflowError("SERVICE_CLOSED", "Service is closed")
            exact(request, {"apiVersion", "requestId", "operation", "parameters"})
            if request["apiVersion"] != API_VERSION:
                raise WorkflowError("UNSUPPORTED_VERSION", "Expected range.workflow/v1")
            identifier(request["requestId"], "requestId")
            p = exact(request["parameters"], {"input"})
            data = p["input"]
            # Check dimensions before walking/serialising any caller arrays.
            if type(data) is not dict:
                raise WorkflowError("INVALID_PARAMETERS", "input must be an object")
            times, channels = data.get("times"), data.get("channels")
            if type(times) is not list or not 2 <= len(times) <= 64:
                raise WorkflowError("INVALID_PARAMETERS", "Inspection requires 2..64 samples")
            if type(channels) is not list or not 1 <= len(channels) <= 4:
                raise WorkflowError("INVALID_PARAMETERS", "Inspection requires 1..4 channels")
            for channel in channels:
                if (type(channel) is not dict or type(channel.get("values")) is not list
                        or len(channel["values"]) != len(times)):
                    raise WorkflowError("INVALID_PARAMETERS", "Inspection channel dimensions differ")
            finite_tree(request)
            # Stop canonical encoding at the bound rather than allocating an oversized request.
            encoded_size = 0
            try:
                encoder = json.JSONEncoder(sort_keys=True, separators=(",", ":"),
                                           ensure_ascii=False, allow_nan=False)
                for chunk in encoder.iterencode(request):
                    encoded_size += len(chunk.encode("utf-8"))
                    if encoded_size > 128 * 1024:
                        raise WorkflowError("REQUEST_TOO_LARGE", "Inspection request exceeds 128 KiB")
            except UnicodeError:
                raise WorkflowError("INVALID_PARAMETERS", "Inspection requires valid UTF-8 text") from None
            try:
                normalized = contract.validate_input(data)
            except (contract.ScientificContractError, TypeError, ValueError) as exc:
                raise WorkflowError("INVALID_PARAMETERS", str(exc)) from None
            return {"normalizedInput": normalized, "inputHash": contract.canonical_hash(normalized),
                    "contractHash": contract.CONTRACT_HASH,
                    "scope": "caller-provided-test-input-binding", "nativeProvenanceVerified": False}

    def handle(self, request: dict) -> dict:
        if (isinstance(request, dict) and
                request.get("operation") == "extension.scientific.input.inspect"):
            return self._inspect_scientific_input(request)
        if (isinstance(request, dict) and isinstance(request.get("operation"), str)
                and request["operation"] in RUNTIME_VERIFICATION_OPERATIONS):
            return self._handle_runtime_verification(request)
        if (isinstance(request, dict) and isinstance(request.get("operation"), str)
                and request["operation"] in READ_OPERATIONS):
            return self.prepare_read(request).run()
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

    def _handle_runtime_verification(self, request):
        # This read-only, host-local lane never writes project state or audit.
        with self._lock:
            if self._closed:
                raise WorkflowError("SERVICE_CLOSED", "Service is closed")
            exact(request, {"apiVersion", "requestId", "operation", "parameters"})
            finite_tree(request)
            if len(canonical(request)) > MAX_REQUEST_BYTES:
                raise WorkflowError("REQUEST_TOO_LARGE", "Request exceeds byte limit")
            if request["apiVersion"] != API_VERSION:
                raise WorkflowError("UNSUPPORTED_VERSION", "Expected range.workflow/v1")
            identifier(request["requestId"], "requestId")
            operation, p = request["operation"], request["parameters"]
            if operation == "extension.runtime.verify":
                exact(p, {"sdkVersion"}, {"force"})
                if p["sdkVersion"] != "range.extension/v2" or type(p.get("force", True)) is not bool:
                    raise WorkflowError("INVALID_PARAMETERS", "Verification requires range.extension/v2 and a boolean force")
                # Capture configured paths only. No hashing, stat, subprocess or source IO in ACK.
                for verification_id in self._runtime_verification_active:
                    record = self._runtime_verifications[verification_id]
                    if (record["state"] in {"queued", "running"}
                            and self._runtime_verification_sources[verification_id].python == self.python):
                        return {"verificationId": verification_id, "state": record["state"]}
                if len(self._runtime_verification_active) >= MAX_RUNTIME_VERIFICATIONS:
                    raise WorkflowError("RUNTIME_VERIFICATION_BUSY", "The bounded runtime verification lane is full")
                verification_id = str(uuid.uuid4())
                self._runtime_verifications[verification_id] = {
                    "verificationId": verification_id, "state": "queued", "sdkVersion": p["sdkVersion"],
                    "expectedEnvironmentStamp": None, "runtimeHash": None, "profile": None,
                    "verifiedAt": None, "fullFileVerify": None, "currentity": "unknown", "isCurrent": None,
                    "currentityScope": "environment-stamp"}
                self._runtime_verification_sources[verification_id] = SimpleNamespace(python=Path(self.python))
                self._runtime_verification_active.add(verification_id)
                self._runtime_verification_queue.append(verification_id)
                self._trim_runtime_verifications()
                if self._runtime_verification_worker is None:
                    self._runtime_verification_worker = threading.Thread(
                        target=self._runtime_verification_loop, name="workflow-runtime-verify", daemon=True)
                    self._runtime_verification_worker.start()
                self._runtime_verification_condition.notify()
                return {"verificationId": verification_id, "state": "queued"}
            exact(p, {"verificationId"})
            verification_id = identifier(p["verificationId"], "verificationId")
            record = self._runtime_verifications.get(verification_id)
            if record is None:
                raise WorkflowError("RUNTIME_VERIFICATION_NOT_FOUND", "Unknown host runtime verification")
            if operation == "host.runtime.verification.cancel":
                self._cancel_runtime_verification(verification_id)
                return copy.deepcopy(self._runtime_verifications[verification_id])
            snapshot = copy.deepcopy(record)
            captured = self._runtime_verification_sources[verification_id]
        if snapshot["state"] == "ready":
            # Cheap bounded identity check outside the owner lock; the receipt is historical
            # byte verification, never a promise that bytes cannot change afterwards.
            from .scientific_runtime import environment_stamp
            try:
                current = environment_stamp(captured)
                changed = current != snapshot["expectedEnvironmentStamp"]
            except (WorkflowError, OSError):
                changed = True
            with self._lock:
                if self._closed:
                    raise WorkflowError("SERVICE_CLOSED", "Service is closed")
                record = self._runtime_verifications.get(verification_id)
                if record is None:
                    raise WorkflowError("RUNTIME_VERIFICATION_NOT_FOUND", "Unknown host runtime verification")
                changed = changed or self.python != captured.python
                if record["state"] == "ready":
                    record.update(currentity="changed" if changed else "current", isCurrent=not changed)
                    if changed:
                        record.update(state="failed", error={"code": "RUNTIME_CHANGED",
                                      "message": "Runtime environment differs from the verified snapshot"})
                snapshot = copy.deepcopy(record)
        return snapshot

    def _trim_runtime_verifications(self):
        retained = [key for key in self._runtime_verifications if key not in self._runtime_verification_active]
        for key in retained[:-MAX_RETAINED_RUNTIME_VERIFICATIONS]:
            self._runtime_verifications.pop(key, None)
            self._runtime_verification_sources.pop(key, None)

    def _cancel_runtime_verification(self, verification_id):
        record = self._runtime_verifications[verification_id]
        if record["state"] in {"queued", "running"}:
            record.update(state="cancelled", currentity="unknown", isCurrent=None)
            if verification_id in self._runtime_verification_queue:
                self._runtime_verification_queue.remove(verification_id)
                self._runtime_verification_active.discard(verification_id)
        self._trim_runtime_verifications()

    def cancel_runtime_verifications(self):
        with self._lock:
            for verification_id in list(self._runtime_verification_active):
                self._cancel_runtime_verification(verification_id)
            self._runtime_verification_condition.notify_all()

    def _runtime_verification_loop(self):
        from .scientific_runtime import environment_stamp, runtime_identity
        while True:
            with self._runtime_verification_condition:
                while not self._runtime_verification_queue and not self._closed:
                    self._runtime_verification_condition.wait()
                if self._closed:
                    return
                verification_id = self._runtime_verification_queue.pop(0)
                captured = self._runtime_verification_sources[verification_id]
                self._runtime_verifications[verification_id]["state"] = "running"
            result, error = None, None
            try:
                expected = environment_stamp(captured)
                with self._lock:
                    record = self._runtime_verifications[verification_id]
                    record["expectedEnvironmentStamp"] = expected
                    cancelled = record["state"] == "cancelled" or self._closed
                if not cancelled:
                    # Always verify bytes, even when force=False; no permanent trust shortcut.
                    result = runtime_identity(captured, force_verify=True)
                    if (environment_stamp(captured) != expected or result.get("profile") != "packaged"
                            or result.get("identity", {}).get("verifiedFiles") is not True
                            or not re.fullmatch(r"[0-9a-f]{64}", str(result.get("runtimeHash", "")))):
                        raise WorkflowError("RUNTIME_CHANGED", "Runtime changed during full file verification")
            except WorkflowError as failure:
                error = {"code": failure.code, "message": failure.message}
            except Exception:
                error = {"code": "RUNTIME_CHANGED", "message": "Runtime verification could not complete"}
            with self._lock:
                record = self._runtime_verifications[verification_id]
                if record["state"] != "cancelled" and not self._closed:
                    if self.python != captured.python:
                        error = {"code": "RUNTIME_CHANGED", "message": "Configured runtime changed during verification"}
                    if error:
                        record.update(state="failed", error=error, currentity="unknown", isCurrent=None)
                    elif result is not None:
                        verified_at = utc()
                        record.update(state="ready", runtimeHash=result["runtimeHash"], profile=result["profile"],
                                      verifiedAt=verified_at, currentity="current", isCurrent=True,
                                      fullFileVerify={"verifiedFiles": True, "verifiedAt": verified_at,
                                          "environmentStamp": expected, "runtimeHash": result["runtimeHash"]})
                self._runtime_verification_active.discard(verification_id)
                self._trim_runtime_verifications()

    def prepare_read(self, request: dict):
        """Capture a bounded completed-source view; never execute a read under _lock."""
        operation = "request.reject"
        with self._lock:
            try:
                if self._closed:
                    raise WorkflowError("SERVICE_CLOSED", "Service is closed")
                exact(request, {"apiVersion", "requestId", "operation", "parameters"})
                finite_tree(request)
                if len(canonical(request)) > MAX_REQUEST_BYTES:
                    raise WorkflowError("REQUEST_TOO_LARGE", "Request exceeds byte limit")
                if request["apiVersion"] != API_VERSION:
                    raise WorkflowError("UNSUPPORTED_VERSION", "Expected range.workflow/v1")
                identifier(request["requestId"], "requestId")
                operation = identifier(request["operation"], "operation")
                if operation not in READ_OPERATIONS:
                    raise WorkflowError("UNKNOWN_OPERATION", "Operation is outside the read lane")
                if not isinstance(request["parameters"], dict):
                    raise WorkflowError("INVALID_PARAMETERS", "parameters must be an object")
                selectors, export = _read_parameters(self, operation, request["parameters"])
                if len(self._read_queries) >= MAX_READ_QUERIES:
                    raise WorkflowError("READ_QUERY_BUSY", "The bounded read lane is full")
                jobs = {}
                pending = [s["jobId"] for s in selectors]
                while pending:
                    job_id = identifier(pending.pop(), "jobId")
                    if job_id in jobs:
                        continue
                    if len(jobs) >= 16:
                        raise WorkflowError("SOURCE_LIMIT", "Source owner closure exceeds its bound")
                    job = self._job(job_id)
                    if job.get("state") != "succeeded":
                        raise WorkflowError("RESULT_UNAVAILABLE", "Only successful completed sources may enter the read lane")
                    # No previews, approvals, live controllers, extensions or unrelated jobs.
                    jobs[job_id] = {k: job[k] for k in ("jobId", "state", "result", "artifacts") if k in job}
                    result = job.get("result", {})
                    paths = [result[k] for k in ("referencePath", "linearPath", "gainsPath", "sourceReferencePath")
                             if k in result]
                    paths += [a.get("path") for a in result.get("sourceArtifacts", {}).values() if isinstance(a, dict)]
                    for raw in paths:
                        if not isinstance(raw, str):
                            raise WorkflowError("SOURCE_CHANGED", "Source owner path is invalid")
                        try:
                            relative = Path(raw).relative_to(self.workspace / "jobs")
                            pending.append(relative.parts[0])
                        except (ValueError, IndexError):
                            raise WorkflowError("SOURCE_CHANGED", "Source owner is outside the workspace") from None
                if len(canonical(jobs)) > MAX_READ_CONTEXT_BYTES:
                    raise WorkflowError("SOURCE_LIMIT", "Completed source context exceeds the read lane byte bound")
                for source in selectors:
                    if operation != "extension.result.query":
                        _read_native_binding(self.workspace, jobs[source["jobId"]], source)
                view = _ReadQuery(self, operation, copy.deepcopy(request["parameters"]),
                                  copy.deepcopy(jobs), copy.deepcopy(export), request["requestId"])
                self._read_queries.add(view)
                return view
            except WorkflowError as error:
                self._read_reject(operation, error)
                raise

    def _read_reject(self, operation, error):
        # Called only with the short owner lock, including worker failures.
        if not self._fault and not self._closed:
            self._commit(copy.deepcopy(self._state), "request.reject", "rejected", approval="rejected",
                         parameters={"operation": operation, "code": error.code})

    def cancel_reads(self):
        with self._lock:
            for query in self._read_queries:
                query.cancel.set()

    def _dispatch(self, operation: str, p: dict) -> dict:
        if operation == "host.read.cancel":
            exact(p, {"requestId"})
            target = identifier(p["requestId"], "requestId")
            selected = [query for query in self._read_queries if query.request_id == target]
            for query in selected:
                query.cancel_code = "READ_CANCELLED"
                query.cancel.set()
            return {"requestId": target, "readCancelled": bool(selected)}
        if operation == "result.plot.sources.query":
            from .series_sources import catalog
            return catalog(self, p)
        if operation in {"result.plot.query", "result.raw.query", "result.raw.export", "result.raw.export.chunk"}:
            from .series_workbench import handle
            return handle(self, operation, p)
        if operation.startswith(("runtime.parameters.", "runtime.execution.")) or operation == "host.runtime.parameters.approve":
            return self._runtime.dispatch(operation, p)
        if operation.startswith("task.plan.") or operation == "host.task.node.approve":
            from .task_plan import PlanError
            try:
                return self._plans.dispatch(operation, p)
            except PlanError:
                raise WorkflowError("INVALID_PLAN", "The plan violates its closed schema, dependency, capability or budget rules") from None
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
            self._require_scientific_runtime_verification(command, p["parameters"])
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
                if "runtimeControl" in recovering_job:
                    job["runtimeControl"] = copy.deepcopy(recovering_job["runtimeControl"])
                job["attemptId"] = str(uuid.uuid4())
                job["priorAttempts"] = recovering_job.get("priorAttempts", []) + [{
                    "previewId": recovering_job["previewId"], "command": recovering_job["command"],
                    "state": recovering_job["state"], "error": recovering_job.get("error"),
                    "attemptId": recovering_job.get("attemptId", jid)}]
            candidate["jobs"][jid] = job
            self._plans.associate_accepted_job(candidate, job)
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
            self._plans.wake.set()
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
        self._require_scientific_runtime_verification(normalized["command"], normalized["parameters"])
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
        sha_schema = {"type": "string", "pattern": "^[0-9a-f]{64}$"}
        selector_common = {"jobId": id_schema, "mode": {"type": "string", "enum": ["active", "passive"]},
                           "artifactId": id_schema, "sourceHash": sha_schema}
        source_schema = {"oneOf": [
            schema({**selector_common, "kind": {"const": "native"}, "channelId": id_schema},
                   ["kind", "jobId", "mode", "artifactId", "channelId"]),
            schema({**selector_common, "kind": {"const": "reference"}, "channelId": id_schema,
                    "referenceSha256": sha_schema}, ["kind", "jobId", "mode", "artifactId", "channelId", "referenceSha256"]),
            schema({**selector_common, "kind": {"const": "energy"}}, ["kind", "jobId", "mode", "artifactId"])]}
        source_array = {"type": "array", "minItems": 1, "maxItems": 4, "items": source_schema}
        operation_parameters = {
            "extension.scientific.input.inspect": schema({"input": {"type": "object",
                "description": "Caller-authored scientific v2 input, 2..64 samples and 1..4 channels; strict contract validation. Binds metadata only, without verifying native provenance."}}),
            "extension.runtime.verify": schema({"sdkVersion": {"const": "range.extension/v2"},
                "force": {"type": "boolean", "default": True}}, ["sdkVersion"]),
            RUNTIME_VERIFICATION_QUERY: schema({"verificationId": id_schema}),
            "result.plot.sources.query": schema({"jobId": id_schema,
                "mode": {"type": "string", "enum": ["active", "passive"]}}, ["jobId"]),
            "result.plot.query": schema({"view": {"type": "string", "enum": ["time", "error", "phase", "amplitude", "psd", "energy", "work"]},
                "sources": source_array, "options": schema({"limit": {"type": "integer", "minimum": 8, "maximum": 1000},
                    "window": {"type": "string", "enum": ["hann", "rectangular"]}, "removeMean": {"type": "boolean"}}, [])}),
            "result.raw.query": schema({"source": source_schema, "offset": {"type": "integer", "minimum": 0, "maximum": 10000},
                "limit": {"type": "integer", "minimum": 1, "maximum": 200}}),
            "result.raw.export": schema({"sources": source_array, "range": {"const": "complete"}}),
            "result.raw.export.chunk": schema({"exportId": id_schema, "offset": {"type": "integer", "minimum": 0, "maximum": 32 * 1024 * 1024},
                "length": {"type": "integer", "minimum": 1, "maximum": 65536}}),
            "runtime.parameters.query": schema({"jobId": id_schema, "proposalId": id_schema}, ["jobId"]),
            "runtime.parameters.preview": schema({"jobId": id_schema,
                "velocityGainNsm": {"type": "number", "minimum": 1e-9, "maximum": 1e12,
                                    "description": "Scalar velocity feedback gain in N*s/m"},
                "actuatorLimits": {"type": "array", "minItems": 1, "maxItems": 1,
                    "items": schema({"actuatorId": id_schema,
                        "minN": {"type": "number", "description": "Lower physical force in N"},
                        "maxN": {"type": "number", "description": "Upper physical force in N"},
                        "rateNPerS": {"type": "number", "minimum": 0,
                                       "description": "Physical force rate in N/s; zero retains unlimited rate only when the immutable source also has unlimited rate"}})}}),
            "runtime.parameters.submit": schema({"jobId": id_schema, "proposalId": id_schema,
                "approvalToken": {"type": "string", "minLength": 1, "maxLength": 128}}),
            "runtime.parameters.cancel": schema({"jobId": id_schema, "proposalId": id_schema}),
            "runtime.execution.pause": schema({"jobId": id_schema}),
            "runtime.execution.resume": schema({"jobId": id_schema}),
            "runtime.execution.cancel": schema({"jobId": id_schema}),
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
            "extension.result.query": schema({"jobId": id_schema,
                "channel": {"type": "integer", "minimum": 0, "maximum": 3},
                "offset": {"type": "integer", "minimum": 0, "maximum": 10000},
                "view": {"type": "string", "enum": ["filtered-time", "amplitude", "psd", "real", "imag", "phase"]},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000}}, ["jobId"]),
            "schema.get": schema({"schemaId": {"type": "string", "enum": [
                "range.extension/v1/manifest", "range.extension/v1/numeric-series", "range.extension/v1/statistics", "range.task-plan/v1",
                "range.extension/v2/manifest", "range.extension/v2/multichannel-series", "range.extension/v2/spectral-result"]}}),
            "knowledge.query": schema({"query": {"type": "string", "maxLength": 256},
                                          "extensionId": id_schema}, ["query"])}
        operation_parameters["task.plan.propose"] = schema({"plan": {"type": "object"}})
        operation_parameters["task.plan.query"] = schema({"planId": id_schema}, [])
        plan_control_schema = schema({"planId": id_schema,
            "expectedPlanSha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"}})
        for action in ("start", "pause", "resume", "cancel", "supersede"):
            operation_parameters["task.plan." + action] = plan_control_schema
        operation_parameters["task.plan.clarify"] = schema({**plan_control_schema["properties"],
            "nodeId": id_schema, "answer": {"type": "string", "minLength": 1, "maxLength": 4096}})
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
                           "extension.list", "extension.candidate.get", "extension.version.get", "schema.get", "knowledge.query",
                           "task.plan.query", "runtime.parameters.query", "extension.runtime.verify", RUNTIME_VERIFICATION_QUERY,
                           "extension.scientific.input.inspect"}
        operations = [{"id": name, "readOnly": name in read_operations, "parameters": parameters}
                      for name, parameters in operation_parameters.items()]
        for item in operations:
            if item["id"] == "runtime.parameters.submit":
                item["requiresApproval"] = True
            if item["id"].startswith("runtime.execution."):
                item["trustedOwnerOnly"] = True
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
                                           "independentReference": {"type": "boolean", "const": True},
                                           "runtimeControl": {"type": "boolean", "description": "Enable finite gain and force limit updates on an active registered native source"},
                                           "pauseAtAcceptedSeq": {"type": "integer", "minimum": 1, "maximum": MAX_STEPS,
                                               "description": "Initial accepted boundary pause; requires runtimeControl"}},
                                          ["mode", "independentReference"])},
                    {"id": "workflow.resume", "readOnly": False, "requiresApproval": True,
                     "precondition": "Successful native job, unchanged source artifacts, total steps <=5000",
                     "parameters": schema({"jobId": id_schema, "additionalSteps": {"type": "integer", "minimum": 1, "maximum": MAX_STEPS}})},
                    {"id": "workflow.recover", "readOnly": False, "requiresApproval": True,
                     "precondition": "Interrupted logical job with a freshly qualified original attempt",
                     "parameters": schema({"jobId": id_schema,
                         "qualificationSha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"}})}]
        commands += [{"id": name, **entry["descriptor"]} for name, entry in self._commands.items()]
        return {"apiVersion": API_VERSION, "operations": operations, "commands": commands,
                "taskPlans": {"schemaVersion": "range.task-plan/v1", "storageVersion": 3,
                    "supportedSource": "registered-native-source", "nodeApproval": "separate-trusted-host-per-node",
                    "ordinaryCodeExecution": False, "runtimeAvailable": not self._plans.stop.is_set(),
                    "maxNodes": 64, "maxScanPoints": 4, "maxRetainedPlans": 16,
                    "maxModelCalls": 24, "maxToolCalls": 128, "maxReadOnlyParallel": 2},
                "capabilityRevision": self._capability_revision(),
                "extensionRuntime": {"trustedExtensions": self.trusted_extensions,
                    "trustMode": "trusted-developer" if self.trusted_extensions else "disabled",
                    "osSandbox": False, "protocolVersion": "range.extension/v1",
                    "supportedProtocols": ["range.extension/v1", "range.extension/v2"],
                    "scientificV2": {"kind": "multichannel-spectral-filter", "packagedRuntimeRequired": True,
                        "maximumChannels": 4, "maximumSamples": 10000, "maximumArtifactBytes": 32*1024*1024,
                        "ordinaryCodeExecution": False, "trustedHostOracleRequired": True},
                    "maxRuntimeSeconds": 15, "maxOutputBytes": 64 * 1024,
                    "maxConcurrentJobs": MAX_EXTENSION_JOBS},
                "models": [{"id": name, "templateFile": data["file"], "supportedCoordinates": [dof for _, dof in data["dofs"]]} for name, data in TEMPLATES.items()],
                "limits": {"maxRequestBytes": MAX_REQUEST_BYTES, "maxProjectJobs": 1,
                           "maxExtensionJobs": MAX_EXTENSION_JOBS,
                           "maxPendingExtensionPreviewBytes": MAX_PENDING_PREVIEW_BYTES,
                           "maxTotalSteps": MAX_STEPS, "processTimeoutSeconds": 180,
                           "runtimeControlMaxWallSeconds": 1800, "runtimePauseIncludedInWallLimit": True,
                           "maxProcessLogBytes": self.runner.MAX_LOG_BYTES},
                "approval": "Host approval is separate from the Agent tool catalog"}

    def close(self):
        with self._lock:
            if self._closed:
                return
            self._closed = True
            self.cancel_runtime_verifications()
            reads = list(self._read_queries)
            for query in reads:
                query.cancel.set()
                if not query._started:
                    self._read_queries.discard(query)
                    query.done.set()
            workers = list(self._workers.values())
            for _, cancel in workers:
                cancel.set()
        for worker, _ in workers:
            worker.join(15)
        for query in reads:
            query.done.wait(15)
        self._plans.close()
        self._runtime.close()
        self._os_lock.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
