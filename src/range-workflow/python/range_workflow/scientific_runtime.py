"""Immutable private scientific runtime identity; v1 identity stays unchanged."""
from __future__ import annotations

import hashlib
from pathlib import Path

from .service import WorkflowError, digest, file_hash

EXPECTED_RUNTIME_ID = "cpython-3.13.5-numpy-2.2.6-scipy-1.15.3-win-amd64"
EXPECTED_PACKAGES = {"cpython": "3.13.5", "numpy": "2.2.6", "scipy": "1.15.3"}
EXPECTED_ARCHIVES = {
    "cpython": "7d2650fd9d1b9d002d4a315d5f354247fd6a44f30517c7ef577b08f57a0fb6d9",
    "numpy": "b0544343a702fa80c95ad5d3d608ea3599dd54d4632df855e4c8d24eb6ecfa1c",
    "scipy": "b90ab29d0c37ec9bf55424c064312930ca5f4bde15ee8619ee44e69319aab163",
}


def _metadata_bytes(path, maximum):
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
            raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime metadata is unavailable or oversized")
        with path.open("rb") as stream:
            raw = stream.read(maximum + 1)
        if len(raw) > maximum:
            raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime metadata exceeds its byte limit")
        return raw
    except OSError:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime metadata is unavailable") from None


def _frozen_lock(raw):
    from .extensions import _strict_json
    try:
        lock = _strict_json(raw, max_bytes=1024 * 1024)
    except WorkflowError:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime lock is malformed") from None
    if type(lock) is not dict:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime lock is malformed")
    files = lock.get("files")
    if (lock.get("runtimeId") != EXPECTED_RUNTIME_ID or type(files) is not list or len(files) != 3
            or {f.get("name"): f.get("version") for f in files if type(f) is dict} != EXPECTED_PACKAGES
            or {f.get("name"): f.get("sha256") for f in files if type(f) is dict} != EXPECTED_ARCHIVES):
        raise WorkflowError("RUNTIME_CHANGED", "Scientific package versions or frozen archives differ")


def _harness_hashes():
    harness = {}
    for name in ("extension_child_v2.py", "scientific_contract.py", "scientific_reference.py", "scientific_oracle_child.py"):
        path = Path(__file__).with_name(name)
        if path.is_symlink() or not path.is_file():
            raise WorkflowError("RUNTIME_CHANGED", "Scientific harness is unavailable")
        try:
            harness[name] = file_hash(path)
        except OSError:
            raise WorkflowError("RUNTIME_CHANGED", "Scientific harness is unavailable") from None
    return harness


def environment_stamp(service):
    """Pin cheap identity inputs for background verification, without verifying the tree.

    The stamp deliberately makes no full-byte or file-set verification claim.
    Only runtime_identity can establish that claim and must still be called at
    preview and with force_verify at the execution boundaries.
    """
    from .extensions import MAX_RUNTIME_MANIFEST_BYTES
    configured = Path(service.python)
    try:
        if configured.is_symlink() or not configured.is_file():
            raise WorkflowError("RUNTIME_CHANGED", "Configured scientific interpreter is unavailable")
        executable = configured.resolve(strict=True)
        executable_sha = file_hash(executable)
    except OSError:
        raise WorkflowError("RUNTIME_CHANGED", "Configured scientific interpreter is unavailable") from None
    root = executable.parent
    lock_raw = _metadata_bytes(root / "runtime-lock.json", 1024 * 1024)
    files_raw = _metadata_bytes(root / "runtime-files.json", MAX_RUNTIME_MANIFEST_BYTES)
    _frozen_lock(lock_raw)
    return digest({"sdkVersion": "range.extension/v2", "flags": ["-I", "-B"],
        "configuredPythonPath": str(configured.absolute()),
        "pythonPath": str(executable), "runtimeRoot": str(root), "pythonSha256": executable_sha,
        "runtimeLockSha256": hashlib.sha256(lock_raw).hexdigest(),
        "runtimeFilesSha256": hashlib.sha256(files_raw).hexdigest(),
        "scientificHarnessSha256": _harness_hashes()})


def runtime_identity(service, *, force_verify=False):
    from .extensions import _runtime_environment_identity
    environment = _runtime_environment_identity(service, force_verify=force_verify,
        require_cached=not force_verify, reject_bytecode=True)
    if environment["profile"] != "packaged":
        raise WorkflowError("PACKAGED_RUNTIME_REQUIRED", "Scientific v2 requires the frozen private runtime")
    root = Path(environment["pythonPath"]).parent
    _frozen_lock(_metadata_bytes(root / "runtime-lock.json", 1024 * 1024))
    harness = _harness_hashes()
    identity = {**environment, "protocolVersion": "range.extension/v2",
                "pythonVersion": EXPECTED_PACKAGES["cpython"],
                "extensionChildSha256": harness["extension_child_v2.py"],
                "scientificHarnessSha256": harness,
                "runtimeId": EXPECTED_RUNTIME_ID,
                "dependencies": [{"name": name, "version": EXPECTED_PACKAGES[name]}
                                 for name in ("numpy", "scipy")],
                "versionEvidence": "verified-frozen-runtime-lock",
                "flags": ["-I", "-B"]}
    return {"identity": identity, "runtimeHash": digest(identity), "profile": "packaged"}
