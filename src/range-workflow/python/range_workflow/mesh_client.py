"""File-owner mesh jobs through the fixed native worker; never accepts a model.

Hashes bind captured files, not proof of mesh correctness. Only the native query
re-reads and independently verifies actual MODEL artifacts and their reports.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import threading
import uuid

from .process import ProcessFailure, ProcessRunner


class MeshClientError(RuntimeError):
    def __init__(self, code: str, message: str, *, log_path: Path | None = None):
        self.code = code
        self.log_path = log_path
        super().__init__(f"{code}: {message}")


def _json(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def _regular(path: Path, limit: int) -> Path:
    if (not path.is_absolute() or path.is_symlink() or not path.is_file()
            or path.resolve() != path or path.stat().st_nlink != 1
            or not 0 < path.stat().st_size <= limit):
        raise ValueError("Require an absolute bounded regular file without links")
    return path


def _read(path: Path, limit: int) -> bytes:
    _regular(path, limit)
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if not 0 < len(data) <= limit or len(data) != path.stat().st_size:
        raise ValueError("Input read failed or exceeded its bound")
    return data


def _sha(path: Path, limit: int) -> str:
    _regular(path, limit)
    digest = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        while block := stream.read(65536):
            count += len(block)
            if count > limit:
                raise ValueError("Input exceeded its bound")
            digest.update(block)
    if count != path.stat().st_size:
        raise ValueError("Input changed while reading")
    return digest.hexdigest()


def _response(output: bytes) -> dict:
    # The fenced runner combines stderr diagnostics with stdout. Only one final
    # native JSON object is a protocol record; partial/multiple records fail.
    lines = output.splitlines()
    records = []
    for line in lines:
        try:
            value = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            continue
        if isinstance(value, dict):
            records.append(value)
    try:
        final = json.loads(lines[-1])
    except (IndexError, ValueError, UnicodeDecodeError):
        final = None
    if len(records) != 1 or final != records[0]:
        raise MeshClientError("invalid_protocol", "Require one final native JSON response")
    return records[0]


@dataclass(frozen=True)
class MeshJob:
    attempt_id: str
    request_path: Path
    request_sha256: str
    _descriptor_json: bytes

    @property
    def descriptor(self) -> dict:
        return json.loads(self._descriptor_json)

    @property
    def accepted(self) -> bool:
        return False

    @property
    def requires_rebinding(self) -> bool:
        return True


class MeshClient:
    """Explicit caller authority over named files and a private output workspace.

    No GUI document identity, approval token, solver binding or acceptance is
    provided by this client. Failed/partial jobs require a fresh prepare call.
    """
    def __init__(self, worker_absolute: str | Path, workspace_root: str | Path,
                 *, timeout: float = 180):
        self.worker_path = _regular(Path(worker_absolute), 256 * 1024 * 1024)
        self.workspace_root = Path(workspace_root)
        if (not self.workspace_root.is_absolute() or not self.workspace_root.is_dir()
                or self.workspace_root.is_symlink()
                or self.workspace_root.resolve() != self.workspace_root):
            raise ValueError("Require an existing canonical workspace directory")
        if isinstance(timeout, bool) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be finite and positive")
        self.timeout = timeout
        self._runner = ProcessRunner()

    def _call(self, action: str, path: Path, cancel: threading.Event | None) -> dict:
        _regular(path, 65536)
        # Every invocation has its own exclusive log directory, including queries.
        log_dir = self.workspace_root / ("call-" + str(uuid.uuid4()))
        log_dir.mkdir()
        try:
            output = self._runner.run([str(self.worker_path), action, "--request", str(path)],
                                      log_dir, "mesh-worker", cancel or threading.Event(), self.timeout)
        except ProcessFailure as error:
            log_path = log_dir / "mesh-worker.log"
            # Recover only a complete explicit native rejection. A nonzero
            # process exit can never be promoted to success by its output.
            if error.code == "PROCESS_FAILED":
                try:
                    rejected = _response(_read(log_path, self._runner.MAX_LOG_BYTES))
                except (OSError, ValueError, MeshClientError):
                    rejected = {}
                if (rejected.get("valid") is False and isinstance(rejected.get("code"), str)
                        and isinstance(rejected.get("message"), str)):
                    raise MeshClientError(rejected["code"], rejected["message"], log_path=log_path) from error
            raise MeshClientError(error.code, str(error), log_path=log_path) from error
        except OSError as error:
            raise MeshClientError("launch_failed", str(error)) from error
        result = _response(output)
        if result.get("valid") is not True:
            raise MeshClientError(str(result.get("code", "native_rejected")),
                                  str(result.get("message", "Native worker rejected job")))
        return result

    def prepare(self, source_file: str | Path, *, max_volume: float,
                radius_edge_ratio: float = 1.5, source_id: str | None = None,
                source_revision: str = "1", cancel: threading.Event | None = None) -> MeshJob:
        for value, low, high in ((max_volume, 1e-15, 1e18), (radius_edge_ratio, 1.2, 2.0)):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
                raise ValueError("Require bounded finite numeric mesh options")
        identity = source_id or str(uuid.uuid4())
        if str(uuid.UUID(identity)) != identity or uuid.UUID(identity).int == 0:
            raise ValueError("source_id must be a canonical nonzero UUID")
        if not isinstance(source_revision, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", source_revision):
            raise ValueError("Invalid source revision")
        source = Path(source_file)
        if source.suffix not in (".rbm", ".ram"):
            raise ValueError("Require a native MODEL file")
        source_bytes = _read(source, 32 * 1024 * 1024)
        source_sha = hashlib.sha256(source_bytes).hexdigest()
        metadata = self.workspace_root / ("input-" + str(uuid.uuid4()))
        metadata.mkdir()
        capture = metadata / ("source" + source.suffix)
        with capture.open("xb") as stream:
            stream.write(source_bytes)
        context_file = metadata / "runtime.json"
        with context_file.open("xb") as stream:
            stream.write(_json({"schemaVersion": 1, "authority": "file-owner",
                                "sourceId": identity, "sourceRevision": source_revision,
                                "modelSha256": source_sha}))
        attempt = str(uuid.uuid4())
        request = {"schemaVersion": 1, "mode": "closed_surface_tetrahedra",
                   "attemptId": attempt, "sourceSnapshotPath": capture.as_posix(),
                   "sourceSnapshotSha256": source_sha,
                   "options": {"unit": "m", "volumeUnit": "m^3", "maxVolume": max_volume,
                               "radiusEdgeRatio": radius_edge_ratio},
                   "runtimeContext": {"authority": "file-owner", "persistentModelPath": source.as_posix(),
                       "persistentModelSha256": source_sha, "runtimeSnapshotPath": context_file.as_posix(),
                       "runtimeSnapshotSha256": _sha(context_file, 1024 * 1024),
                       "sourceSnapshotRef": {"sourceId": identity, "sourceRevision": source_revision,
                                             "modelSha256": source_sha, "instanceRef": None},
                       "sourceRuntimeStamp": None},
                   "workerPath": self.worker_path.as_posix(), "workerSha256": _sha(self.worker_path, 256 * 1024 * 1024),
                   "jobDirectory": (self.workspace_root / attempt).as_posix()}
        raw_path = metadata / "prepare.json"
        encoded = _json(request)
        if len(encoded) > 65536:
            raise ValueError("Request exceeds its bound")
        with raw_path.open("xb") as stream:
            stream.write(encoded)
        result = self._call("prepare", raw_path, cancel)
        descriptor = result.get("descriptor", {})
        expected_path = self.workspace_root / attempt / "request.json"
        if (result.get("code") != "prepared" or descriptor.get("attemptId") != attempt
                or not isinstance(descriptor.get("requestPath"), str)
                or Path(descriptor["requestPath"]) != expected_path
                or descriptor.get("accepted") is not False or descriptor.get("requiresRebinding") is not True):
            raise MeshClientError("invalid_protocol", "Native preparation binding is invalid")
        frozen = json.loads(_read(expected_path, 65536))
        if frozen.get("runtimeContext") != request["runtimeContext"] or frozen.get("attemptId") != attempt:
            raise MeshClientError("invalid_protocol", "Frozen request context differs from caller capture")
        digest = descriptor.get("requestSha256")
        if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest) or frozen.get("requestSha256") != digest:
            raise MeshClientError("invalid_protocol", "Missing native frozen request digest")
        return MeshJob(attempt, expected_path, digest, _json(descriptor))

    def _validate_request_path(self, job: MeshJob) -> Path:
        if not isinstance(job, MeshJob) or str(uuid.UUID(job.attempt_id)) != job.attempt_id:
            raise ValueError("Require a mesh job descriptor")
        expected = self.workspace_root / job.attempt_id / "request.json"
        if job.request_path != expected:
            raise ValueError("Job request is outside this workspace")
        frozen = json.loads(_read(expected, 65536))
        if (frozen.get("attemptId") != job.attempt_id or frozen.get("requestSha256") != job.request_sha256
                or not isinstance(frozen.get("workerPath"), str)
                or Path(frozen["workerPath"]) != self.worker_path
                or frozen.get("runtimeContext", {}).get("authority") != "file-owner"):
            raise ValueError("Job request binding changed")
        return expected

    def _operation(self, action: str, job: MeshJob, cancel: threading.Event | None) -> MeshJob:
        result = self._call(action, self._validate_request_path(job), cancel)
        descriptor = result.get("descriptor", {})
        if (result.get("code") != "candidate_ready" or descriptor.get("attemptId") != job.attempt_id
                or descriptor.get("requestSha256") != job.request_sha256
                or descriptor.get("accepted") is not False or descriptor.get("requiresRebinding") is not True):
            raise MeshClientError("invalid_protocol", "Native candidate binding is invalid")
        return MeshJob(job.attempt_id, job.request_path, job.request_sha256, _json(descriptor))

    def run(self, job: MeshJob, *, cancel: threading.Event | None = None) -> MeshJob:
        self._operation("run", job, cancel)
        return self.query(job, cancel=cancel)

    def query(self, job: MeshJob, *, cancel: threading.Event | None = None) -> MeshJob:
        return self._operation("query", job, cancel)
