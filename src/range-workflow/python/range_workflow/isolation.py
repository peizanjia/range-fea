"""Host-owned disposable LPAC staging; capability stays off until denial validation."""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import threading
import uuid

from .service import WorkflowError, canonical, file_hash
from .process import ProcessRunner, ProcessFailure


def tree_hash(root):
    root = Path(root).resolve(strict=True)
    entries = []
    for path in root.rglob("*"):
        if path.is_symlink() or path.is_junction():
            raise WorkflowError("ISOLATION_STAGE", "Runtime reparse points are forbidden")
        if path.is_file():
            entries.append((path.relative_to(root).as_posix().encode("utf-8"), path))
    if not 1 <= len(entries) <= 8192 or sum(p.stat().st_size for _, p in entries) > 2*1024**3:
        raise WorkflowError("ISOLATION_STAGE", "Runtime exceeds its frozen staging bounds")
    digest = hashlib.sha256()
    for name, path in sorted(entries):
        digest.update(name + b"\0" + file_hash(path).encode("ascii") + b"\n")
    return digest.hexdigest()


def stage(workspace, python, source, input_value):
    """Only the host calls this after binding the exact approved source/input."""
    from .scientific_runtime import runtime_identity
    from types import SimpleNamespace
    before = runtime_identity(SimpleNamespace(python=Path(python)), force_verify=True)
    if before["profile"] != "packaged":
        raise WorkflowError("PACKAGED_RUNTIME_REQUIRED", "LPAC staging requires the frozen private runtime")
    workspace = Path(workspace).resolve(strict=True)
    base = workspace / "isolated-workers"
    if base.is_symlink() or (base.exists() and base.is_junction()):
        raise WorkflowError("ISOLATION_STAGE", "Managed staging root is a reparse point")
    base.mkdir(exist_ok=True)
    managed = base / str(uuid.uuid4())
    managed.mkdir()
    runtime = managed / "runtime" / "python"
    source_runtime = Path(python).resolve(strict=True).parent
    # A disposable copy receives package-SID ACLs. The installed immutable
    # environment and user directories never receive worker permissions.
    shutil.copytree(source_runtime, runtime, symlinks=False,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    copied_identity = runtime_identity(SimpleNamespace(python=runtime/"python.exe"), force_verify=True)
    if copied_identity["identity"]["runtimeLockSha256"] != before["identity"]["runtimeLockSha256"]:
        raise WorkflowError("RUNTIME_CHANGED", "Staged runtime lock differs")
    after = runtime_identity(SimpleNamespace(python=Path(python)), force_verify=True)
    if after != before:
        raise WorkflowError("RUNTIME_CHANGED", "Original runtime changed while staging")
    job = managed / "job"
    job.mkdir()
    package = Path(__file__).parent
    for name in ("extension_child_v2.py", "scientific_contract.py"):
        path = package/name
        if path.is_symlink() or not path.is_file():
            raise WorkflowError("ISOLATION_STAGE", "Verified harness is unavailable")
        raw = path.read_bytes()
        (job/name).write_bytes(raw)
        if file_hash(path) != hashlib.sha256(raw).hexdigest():
            raise WorkflowError("RUNTIME_CHANGED", "Harness changed while staging")
    source = Path(source)
    if source.is_symlink() or not source.is_file() or source.stat().st_size > 64*1024:
        raise WorkflowError("EXTENSION_TAMPERED", "Bounded approved source file required")
    raw_source = source.read_bytes()
    (job/"extension.py").write_bytes(raw_source)
    raw_input = canonical(input_value)
    if len(raw_input) > 32*1024*1024:
        raise WorkflowError("EXTENSION_INPUT_LIMIT", "Isolated input exceeds 32 MiB")
    (job/"input.json").write_bytes(raw_input)
    request = {"schema": "range.workflow.lpac.request/v1", "managedRoot": str(managed),
        "jobRoot": str(job), "runtimeRoot": str(runtime), "harnessPath": str(job/"extension_child_v2.py"),
        "sourcePath": str(job/"extension.py"), "inputPath": str(job/"input.json"),
        "outputRoot": str(job/"output"), "mode": "run", "timeoutMs": 30000,
        "pythonSha256": file_hash(runtime/"python.exe"), "runtimeSha256": tree_hash(runtime),
        "harnessSha256": file_hash(job/"extension_child_v2.py"), "contractSha256": file_hash(job/"scientific_contract.py"),
        "sourceSha256": hashlib.sha256(raw_source).hexdigest(), "inputSha256": hashlib.sha256(raw_input).hexdigest()}
    request_path = job/"request.json"
    request_path.write_bytes(canonical(request))
    return request_path, request


def launch(request_path, launcher, *, cancel=None):
    request_path, launcher = Path(request_path), Path(launcher)
    cancel = cancel or threading.Event()
    try:
        raw = ProcessRunner().run([str(launcher), "run", "--request", str(request_path)],
            request_path.parent, "lpac-launcher", cancel, timeout=60)
    except ProcessFailure:
        log = request_path.parent / "lpac-launcher.log"
        raw = log.read_bytes() if log.is_file() else b""
        if not raw:
            raise
    from .extensions import _strict_json
    receipt = _strict_json(raw, max_bytes=8*1024*1024)
    if receipt.get("schema") != "range.workflow.lpac.receipt/v1":
        raise WorkflowError("ISOLATION_FAILED", "Launcher returned an invalid receipt")
    if (receipt.get("status") != "succeeded" or receipt.get("cleanupComplete") is not True):
        raise WorkflowError(receipt.get("code", "ISOLATION_FAILED"), receipt.get("message", "LPAC launch failed"))
    return receipt
