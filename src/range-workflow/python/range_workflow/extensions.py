"""Versioned, process-isolated workflow extensions.

This module deliberately does not claim an operating-system sandbox.  Generated
Python is executable only when the host started the service in trusted developer
mode, and every mutation still goes through the workflow preview/approval path.
"""
from __future__ import annotations

import copy
import difflib
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import sys
import threading
import uuid
from typing import Any

from .service import (MAX_REQUEST_BYTES, WorkflowError, canonical, digest,
                      exact, file_hash, finite_tree, identifier, integer, utc)

PROTOCOL_VERSION = "range.extension/v1"
MANIFEST_SCHEMA = "range.extension/v1/manifest"
SERIES_SCHEMA = "range.extension/v1/numeric-series"
STATISTICS_SCHEMA = "range.extension/v1/statistics"
MAX_SOURCE_BYTES = 64 * 1024
MAX_TEST_BYTES = 96 * 1024
MAX_REPORT_BYTES = 64 * 1024
MAX_OUTPUT_BYTES = 64 * 1024
MAX_SERIES_POINTS = 100_000
MAX_TEST_CASES = 16
MAX_CANDIDATES_PER_EXTENSION = 64
MAX_VERSIONS_PER_EXTENSION = 32
MAX_EXTENSIONS = 128
MAX_EXTENSION_RUNTIME_SECONDS = 15
MAX_RUNTIME_MANIFEST_BYTES = 16 * 1024 * 1024
MAX_RUNTIME_FILES = 100_000
MAX_RUNTIME_FILE_BYTES = 2 * 1024 * 1024 * 1024
MAX_PENDING_PREVIEW_BYTES = 8 * 1024 * 1024

_ID = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
_SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[A-Za-z0-9.-]{1,32})?$")
_UNIT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9./*^_-]{0,31}$")
_SOURCE_SIGNALS = {
    "reference.active.sensorVelocity": ("timeOutputs", "fullVelocityMPerSecond", "m/s"),
    "reference.active.energyJ": ("energiesJ", "fullControlledHistory", "J"),
    "reference.passive.energyJ": ("energiesJ", "fullPassiveHistory", "J"),
}
_RUNTIME_CACHE_LOCK = threading.RLock()
_RUNTIME_VERIFICATION_CACHE: dict[str, tuple[str, tuple]] = {}


def _fail(code: str, message: str):
    raise WorkflowError(code, message)


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def _strict_json(raw: bytes, *, max_bytes: int, max_collection_size: int = 1024) -> Any:
    if len(raw) > max_bytes:
        _fail("EXTENSION_OUTPUT_LIMIT", "Extension JSON exceeds its byte limit")
    try:
        value = json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=_pairs,
                           parse_constant=lambda _x: (_ for _ in ()).throw(ValueError("non-finite")))
        finite_tree(value, max_collection_size=max_collection_size)
        canonical(value)
        return value
    except WorkflowError:
        raise
    except (UnicodeError, ValueError, TypeError, RecursionError):
        _fail("EXTENSION_INVALID_JSON", "Extension JSON is malformed or contains duplicate/non-finite values")


def _unit(value: Any, name: str) -> str:
    if not isinstance(value, str) or not _UNIT.fullmatch(value):
        _fail("INVALID_PARAMETERS", f"{name} must be a bounded unit label")
    return value


def _finite_measure(value: Any, name: str) -> float:
    if type(value) not in (int, float):
        _fail("INVALID_PARAMETERS", f"{name} must be a finite number")
    try:
        result = float(value)
    except (ValueError, OverflowError):
        _fail("INVALID_PARAMETERS", f"{name} must be finite")
    if not math.isfinite(result) or abs(result) > 1e100:
        _fail("INVALID_PARAMETERS", f"{name} is outside the finite supported range")
    return result


def validate_series(value: Any) -> dict:
    exact(value, {"times", "values", "timeUnit", "valueUnit"})
    times, values = value["times"], value["values"]
    if (not isinstance(times, list) or not isinstance(values, list) or
            not 1 <= len(times) <= MAX_SERIES_POINTS or len(values) != len(times)):
        _fail("INVALID_PARAMETERS", f"times and values must contain the same 1..{MAX_SERIES_POINTS} points")
    normalized_times = [_finite_measure(item, f"times[{index}]") for index, item in enumerate(times)]
    normalized_values = [_finite_measure(item, f"values[{index}]") for index, item in enumerate(values)]
    if any(right <= left for left, right in zip(normalized_times, normalized_times[1:])):
        _fail("INVALID_PARAMETERS", "times must be strictly increasing")
    return {"times": normalized_times, "values": normalized_values,
            "timeUnit": _unit(value["timeUnit"], "timeUnit"),
            "valueUnit": _unit(value["valueUnit"], "valueUnit")}


def statistics(series: dict) -> dict:
    """Independent host implementation of the supported sample statistics."""
    values = series["values"]
    peak = max(abs(value) for value in values)
    peak_index = next(index for index, value in enumerate(values) if abs(value) == peak)
    if peak == 0:
        rms = 0.0
    else:
        rms = peak * math.sqrt(math.fsum((value / peak) ** 2 for value in values) / len(values))
    return {"rms": rms, "peakAbs": peak, "peakTime": series["times"][peak_index],
            "timeUnit": series["timeUnit"], "valueUnit": series["valueUnit"]}


def _result(value: Any, series: dict) -> dict:
    exact(value, {"rms", "peakAbs", "peakTime", "timeUnit", "valueUnit"})
    actual = {name: _finite_measure(value[name], name) for name in ("rms", "peakAbs", "peakTime")}
    actual["timeUnit"] = _unit(value["timeUnit"], "timeUnit")
    actual["valueUnit"] = _unit(value["valueUnit"], "valueUnit")
    if actual["rms"] < 0 or actual["peakAbs"] < 0:
        _fail("EXTENSION_INVALID_RESULT", "RMS and absolute peak must be nonnegative")
    if actual["timeUnit"] != series["timeUnit"] or actual["valueUnit"] != series["valueUnit"]:
        _fail("EXTENSION_INVALID_RESULT", "Extension output units do not match its input")
    return actual


def _matches(actual: dict, expected: dict) -> bool:
    for key in ("rms", "peakAbs", "peakTime"):
        if abs(actual[key] - expected[key]) > 1e-10 + 1e-10 * abs(expected[key]):
            return False
    return actual["timeUnit"] == expected["timeUnit"] and actual["valueUnit"] == expected["valueUnit"]


def _validate_gui(value: Any) -> dict:
    exact(value, {"form", "table"})
    form = value["form"]
    if form != [{"id": "series", "type": "numeric-series", "required": True}]:
        _fail("INVALID_PARAMETERS", "gui.form must declare the v1 numeric-series input control")
    table = exact(value["table"], {"columns"})
    columns = table["columns"]
    expected = [
        {"id": "rms", "label": "RMS", "unit": "valueUnit"},
        {"id": "peakAbs", "label": "Absolute peak", "unit": "valueUnit"},
        {"id": "peakTime", "label": "Peak time", "unit": "timeUnit"},
    ]
    if columns != expected:
        _fail("INVALID_PARAMETERS", "gui.table.columns must match the numeric-series statistics contract")
    return {"form": copy.deepcopy(form), "table": {"columns": copy.deepcopy(columns)}}


def validate_manifest(value: Any) -> dict:
    exact(value, {"manifestVersion", "extensionId", "version", "sdkVersion", "kind",
                  "entryPoint", "dependencies", "inputSchema", "outputSchema", "gui"})
    if type(value["manifestVersion"]) is not int or value["manifestVersion"] != 1:
        _fail("UNSUPPORTED_VERSION", "Only manifestVersion 1 is supported")
    extension_id = identifier(value["extensionId"], "extensionId")
    version = value["version"]
    if not isinstance(version, str) or len(version) > 64 or not _SEMVER.fullmatch(version):
        _fail("INVALID_PARAMETERS", "version must be a bounded semantic version")
    if (value["sdkVersion"] != PROTOCOL_VERSION or value["kind"] != "numeric-series-statistics" or
            value["entryPoint"] != "extension.py:process" or
            value["inputSchema"] != SERIES_SCHEMA or value["outputSchema"] != STATISTICS_SCHEMA):
        _fail("UNSUPPORTED_VERSION", "Manifest requests an unsupported extension protocol or type")
    dependencies = value["dependencies"]
    if dependencies != []:
        _fail("UNSUPPORTED_DEPENDENCY", "The v1 extension runtime supports only the Python standard library")
    return {"manifestVersion": 1, "extensionId": extension_id, "version": version,
            "sdkVersion": PROTOCOL_VERSION, "kind": "numeric-series-statistics",
            "entryPoint": "extension.py:process", "dependencies": [],
            "inputSchema": SERIES_SCHEMA, "outputSchema": STATISTICS_SCHEMA,
            "gui": _validate_gui(value["gui"])}


def _validate_cases(value: Any) -> dict:
    exact(value, {"cases"})
    cases = value["cases"]
    if not isinstance(cases, list) or not 1 <= len(cases) <= MAX_TEST_CASES:
        _fail("INVALID_PARAMETERS", f"tests.cases must contain 1..{MAX_TEST_CASES} cases")
    normalized = []
    ids = set()
    for case in cases:
        exact(case, {"id", "input", "expected"})
        case_id = identifier(case["id"], "test id")
        if case_id in ids:
            _fail("INVALID_PARAMETERS", "Test case IDs must be unique")
        ids.add(case_id)
        series = validate_series(case["input"])
        expected = _result(case["expected"], series)
        normalized.append({"id": case_id, "input": series, "expected": expected})
    result = {"cases": normalized}
    if len(canonical(result)) > MAX_TEST_BYTES:
        _fail("EXTENSION_INPUT_LIMIT", "Extension tests exceed their byte limit")
    return result


def _hash_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", "strict")).hexdigest()


def _hash_bundle(manifest: dict, source: str, tests: dict) -> dict:
    values = {"manifest": hashlib.sha256(canonical(manifest)).hexdigest(),
              "source": _hash_text(source), "tests": hashlib.sha256(canonical(tests)).hexdigest()}
    bundle_hash = digest({"sdkVersion": PROTOCOL_VERSION, **values})
    return {**values, "bundle": bundle_hash}


def _runtime_stat_token(path: Path) -> tuple:
    info = path.stat()
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _runtime_manifest_snapshot(root: Path, file_hashes: dict[str, str]) -> tuple:
    listed = set(file_hashes)
    actual = set()
    try:
        for path in root.rglob("*"):
            if path.is_symlink():
                _fail("RUNTIME_CHANGED", "Packaged Python runtime contains a symbolic link")
            if path.is_dir():
                continue
            if not path.is_file():
                _fail("RUNTIME_CHANGED", "Packaged Python runtime contains a non-file entry")
            relative = path.relative_to(root).as_posix()
            if relative == "runtime-files.json" or "__pycache__" in path.parts:
                continue
            actual.add(relative)
        if actual != listed:
            _fail("RUNTIME_CHANGED", "Packaged Python runtime file set differs from its manifest")
        tokens = []
        for relative in sorted(listed):
            path = root
            for part in PurePosixPath(relative).parts:
                path = path / part
                if path.is_symlink():
                    _fail("RUNTIME_CHANGED", "Packaged Python runtime contains a symbolic link")
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(root) or not resolved.is_file():
                _fail("RUNTIME_CHANGED", "Packaged Python runtime path escaped its root")
            size = resolved.stat().st_size
            if size > MAX_RUNTIME_FILE_BYTES:
                _fail("RUNTIME_CHANGED", "Packaged Python runtime file exceeds its size limit")
            tokens.append((relative, *_runtime_stat_token(resolved)))
        return tuple(tokens)
    except WorkflowError:
        raise
    except (OSError, ValueError, RuntimeError):
        _fail("RUNTIME_CHANGED", "Packaged Python runtime is incomplete or unavailable")


def _verify_packaged_runtime(executable: Path, executable_sha: str,
                             lock_path: Path, files_path: Path, *, force_verify: bool = False) -> tuple[str, str]:
    if (not lock_path.is_file() or lock_path.is_symlink() or
            not files_path.is_file() or files_path.is_symlink()):
        _fail("RUNTIME_CHANGED", "Packaged Python runtime is missing its lock or file manifest")
    root = executable.parent.resolve()
    try:
        if not executable.resolve(strict=True).is_relative_to(root):
            _fail("RUNTIME_CHANGED", "Configured Python interpreter is outside its runtime root")
        lock_raw = lock_path.read_bytes()
        files_raw = files_path.read_bytes()
        if len(lock_raw) > 1024 * 1024 or len(files_raw) > MAX_RUNTIME_MANIFEST_BYTES:
            _fail("RUNTIME_CHANGED", "Packaged Python runtime metadata exceeds its size limit")
        lock = json.loads(lock_raw.decode("utf-8", "strict"), object_pairs_hook=_pairs,
                          parse_constant=lambda _value: (_ for _ in ()).throw(ValueError("non-finite")))
        file_hashes = json.loads(files_raw.decode("utf-8", "strict"), object_pairs_hook=_pairs,
                                 parse_constant=lambda _value: (_ for _ in ()).throw(ValueError("non-finite")))
    except WorkflowError:
        raise
    except (OSError, UnicodeError, ValueError, TypeError, RecursionError):
        _fail("RUNTIME_CHANGED", "Packaged Python runtime metadata is malformed")
    if (not isinstance(lock, dict) or lock.get("schemaVersion") != 1 or
            not isinstance(lock.get("runtimeId"), str) or not lock["runtimeId"] or len(lock["runtimeId"]) > 128 or
            lock.get("platform") != "win_amd64" or not isinstance(lock.get("files"), list) or
            not 1 <= len(lock["files"]) <= 16 or
            not isinstance(file_hashes, dict) or not 1 <= len(file_hashes) <= MAX_RUNTIME_FILES):
        _fail("RUNTIME_CHANGED", "Packaged Python runtime metadata has an unsupported schema")
    for relative, expected in file_hashes.items():
        parsed = PurePosixPath(relative) if isinstance(relative, str) else None
        if (parsed is None or not parsed.parts or parsed.is_absolute() or parsed.as_posix() != relative or
                "\\" in relative or ":" in relative or
                any(part in ("", ".", "..", "__pycache__") for part in parsed.parts) or
                not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)):
            _fail("RUNTIME_CHANGED", "Packaged Python runtime manifest contains an unsafe entry")
    executable_relative = executable.relative_to(root).as_posix()
    lock_relative = lock_path.relative_to(root).as_posix()
    lock_sha = hashlib.sha256(lock_raw).hexdigest()
    if file_hashes.get(executable_relative) != executable_sha or file_hashes.get(lock_relative) != lock_sha:
        _fail("RUNTIME_CHANGED", "Packaged Python executable or lock differs from its manifest")
    fingerprint = _runtime_manifest_snapshot(root, file_hashes)
    files_sha = hashlib.sha256(files_raw).hexdigest()
    cache_key = str(root)
    with _RUNTIME_CACHE_LOCK:
        cached = _RUNTIME_VERIFICATION_CACHE.get(cache_key)
        cache_id = digest({"runtimeLockSha256": lock_sha, "runtimeFilesSha256": files_sha})
        if force_verify or cached != (cache_id, fingerprint):
            try:
                for relative, expected in file_hashes.items():
                    path = root.joinpath(*PurePosixPath(relative).parts)
                    before = _runtime_stat_token(path)
                    if file_hash(path) != expected or _runtime_stat_token(path) != before:
                        _fail("RUNTIME_CHANGED", "Packaged Python file differs from its runtime manifest")
                if _runtime_manifest_snapshot(root, file_hashes) != fingerprint:
                    _fail("RUNTIME_CHANGED", "Packaged Python runtime changed during verification")
            except WorkflowError:
                raise
            except OSError:
                _fail("RUNTIME_CHANGED", "Packaged Python runtime file is unavailable")
            _RUNTIME_VERIFICATION_CACHE[cache_key] = (cache_id, fingerprint)
            if len(_RUNTIME_VERIFICATION_CACHE) > 16:
                oldest = next(iter(_RUNTIME_VERIFICATION_CACHE))
                if oldest != cache_key:
                    _RUNTIME_VERIFICATION_CACHE.pop(oldest, None)
    return lock_sha, files_sha


def runtime_identity(service, *, force_verify: bool = False) -> dict:
    configured = Path(service.python)
    if configured.is_symlink() or not configured.is_file():
        _fail("RUNTIME_CHANGED", "Configured extension interpreter is unavailable")
    try:
        executable = configured.resolve(strict=True)
        executable_sha = file_hash(executable)
    except OSError:
        _fail("RUNTIME_CHANGED", "Configured extension interpreter is unavailable")
    lock_path = executable.with_name("runtime-lock.json")
    files_path = executable.with_name("runtime-files.json")
    has_lock = lock_path.exists() or lock_path.is_symlink()
    has_files = files_path.exists() or files_path.is_symlink()
    if has_lock or has_files:
        lock_sha, files_sha = _verify_packaged_runtime(executable, executable_sha, lock_path, files_path,
                                                       force_verify=force_verify)
        profile = "packaged"
    else:
        lock_sha = files_sha = None
        profile = "development"
    child = Path(__file__).with_name("extension_child.py")
    if child.is_symlink() or not child.is_file():
        _fail("RUNTIME_CHANGED", "Extension worker harness is unavailable")
    try:
        child_sha = file_hash(child)
    except OSError:
        _fail("RUNTIME_CHANGED", "Extension worker harness is unavailable")
    identity = {"protocolVersion": PROTOCOL_VERSION, "pythonPath": str(executable),
                "pythonSha256": executable_sha, "pythonVersion": sys.version.split()[0],
                "runtimeLockSha256": lock_sha, "runtimeFilesSha256": files_sha,
                "extensionChildSha256": child_sha,
                "profile": profile, "verifiedFiles": profile == "packaged",
                "dependencies": [], "flags": ["-I", "-S"]}
    return {"identity": identity, "runtimeHash": digest(identity), "profile": profile}


def _extension_root(service) -> Path:
    root = service.workspace / "extensions"
    if root.exists() and root.is_symlink():
        _fail("EXTENSION_PATH", "Extension store may not be a symbolic link")
    if not root.exists():
        root.mkdir()
    resolved = root.resolve()
    if not resolved.is_relative_to(service.workspace.resolve()):
        _fail("EXTENSION_PATH", "Extension store must remain inside its workflow workspace")
    return root


def _safe_relative(service, relative: str, expected_sha: str, max_bytes: int) -> Path:
    if not isinstance(relative, str) or len(relative) > 512 or "\\" in relative:
        _fail("EXTENSION_PATH", "Stored extension path is invalid")
    pure = PurePosixPath(relative)
    if pure.is_absolute() or not pure.parts or any(part in ("", ".", "..") for part in pure.parts):
        _fail("EXTENSION_PATH", "Stored extension path is invalid")
    root = service.workspace.resolve()
    path = service.workspace
    for part in pure.parts:
        path = path / part
        if path.is_symlink():
            _fail("EXTENSION_PATH", "Extension files may not contain symbolic links")
    resolved = path.resolve()
    if not resolved.is_relative_to(root) or not resolved.is_file() or resolved.stat().st_size > max_bytes:
        _fail("EXTENSION_PATH", "Stored extension file is missing or exceeds its byte limit")
    if file_hash(resolved) != expected_sha:
        _fail("EXTENSION_TAMPERED", "Stored extension content no longer matches its approved hash")
    return resolved


def _registry_entry(service, extension_id: str, *, required: bool = True) -> dict | None:
    entry = service._state["extensions"].get(extension_id)
    if entry is None and required:
        _fail("EXTENSION_NOT_FOUND", "Unknown extensionId")
    return entry


def _candidate(service, candidate_id: str, bundle_hash: str | None = None) -> tuple[dict, dict]:
    identifier(candidate_id, "candidateId")
    for extension_id, entry in service._state["extensions"].items():
        item = entry.get("candidates", {}).get(candidate_id)
        if item is not None:
            if bundle_hash is not None and item["bundleHash"] != bundle_hash:
                _fail("EXTENSION_HASH_MISMATCH", "Candidate bundle hash differs from the requested hash")
            return entry, item
    _fail("EXTENSION_NOT_FOUND", "Unknown extension candidate")


def _version(service, extension_id: str, bundle_hash: str) -> tuple[dict, dict]:
    identifier(extension_id, "extensionId")
    if not isinstance(bundle_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", bundle_hash):
        _fail("INVALID_PARAMETERS", "bundleHash must be a lowercase SHA-256 digest")
    entry = _registry_entry(service, extension_id)
    item = entry.get("versions", {}).get(bundle_hash)
    if item is None:
        _fail("EXTENSION_NOT_FOUND", "Unknown immutable extension version")
    return entry, item


def _source_record(service, record: dict) -> tuple[dict, str, dict]:
    manifest_path = _safe_relative(service, record["manifestPath"], record["manifestSha256"], 32 * 1024)
    source_path = _safe_relative(service, record["sourcePath"], record["sourceSha256"], MAX_SOURCE_BYTES)
    tests_path = _safe_relative(service, record["testsPath"], record["testsSha256"], MAX_TEST_BYTES)
    manifest = _strict_json(manifest_path.read_bytes(), max_bytes=32 * 1024)
    tests = _strict_json(tests_path.read_bytes(), max_bytes=MAX_TEST_BYTES,
                         max_collection_size=MAX_SERIES_POINTS)
    source = source_path.read_text(encoding="utf-8", errors="strict")
    manifest = validate_manifest(manifest)
    tests = _validate_cases(tests)
    if (record.get("manifest") != manifest or record.get("extensionId") != manifest["extensionId"] or
            record.get("version") != manifest["version"]):
        _fail("EXTENSION_TAMPERED", "Stored manifest metadata differs from its immutable file")
    actual = _hash_bundle(manifest, source, tests)
    if actual["bundle"] != record["bundleHash"]:
        _fail("EXTENSION_TAMPERED", "Extension bundle does not match its immutable identity")
    return manifest, source, tests


def _safe_write(path: Path, data: bytes):
    try:
        with path.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError:
        _fail("EXTENSION_CONFLICT", "Immutable extension target already exists")
    except OSError:
        _fail("EXTENSION_STORAGE", "Could not store immutable extension content")


def _write_candidate(service, candidate_id: str, manifest: dict, source: str,
                     tests: dict, hashes: dict) -> dict:
    root = _extension_root(service) / "candidates"
    if root.exists() and root.is_symlink():
        _fail("EXTENSION_PATH", "Candidate store may not be a symbolic link")
    root.mkdir(exist_ok=True)
    directory = root / candidate_id
    directory.mkdir(exist_ok=False)
    manifest_bytes = canonical(manifest)
    source_bytes = source.encode("utf-8", "strict")
    tests_bytes = canonical(tests)
    _safe_write(directory / "manifest.json", manifest_bytes)
    _safe_write(directory / "extension.py", source_bytes)
    _safe_write(directory / "tests.json", tests_bytes)
    relative = directory.relative_to(service.workspace).as_posix()
    return {"candidateId": candidate_id, "extensionId": manifest["extensionId"],
            "version": manifest["version"], "bundleHash": hashes["bundle"],
            "manifest": copy.deepcopy(manifest),
            "manifestSha256": hashes["manifest"], "sourceSha256": hashes["source"],
            "testsSha256": hashes["tests"], "manifestPath": f"{relative}/manifest.json",
            "sourcePath": f"{relative}/extension.py", "testsPath": f"{relative}/tests.json",
            "baseBundleHash": None, "baseSourcePath": None, "baseSourceSha256": None,
            "state": "staged", "stagedAt": utc(), "validation": None}


def _copy_verified(source: Path, destination: Path, expected: str, maximum: int):
    if source.is_symlink() or not source.is_file() or source.stat().st_size > maximum or file_hash(source) != expected:
        _fail("EXTENSION_TAMPERED", "Approved extension content changed before execution")
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected:
        _fail("EXTENSION_TAMPERED", "Approved extension content changed while being copied")
    _safe_write(destination, raw)


def _clean_environment(work_directory: Path) -> dict[str, str]:
    # No credential variables or arbitrary user environment are inherited.
    environment: dict[str, str] = {"TEMP": str(work_directory), "TMP": str(work_directory)}
    for name in ("SYSTEMROOT", "WINDIR"):
        value = os.environ.get(name)
        if value:
            environment[name] = value
    if os.name != "nt":
        environment["PATH"] = os.defpath
    return environment


def _execute_source(service, directory: Path, source_record: dict, series: dict,
                    cancel: threading.Event, *, tests: dict | None = None) -> dict:
    if not service.trusted_extensions:
        _fail("TRUSTED_RUNTIME_REQUIRED", "Enable trusted developer extension mode before executing Python")
    runtime = runtime_identity(service, force_verify=True)
    if source_record.get("runtimeHash") and runtime["runtimeHash"] != source_record["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "The active extension was validated for a different Python runtime")
    source_path = _safe_relative(service, source_record["sourcePath"], source_record["sourceSha256"], MAX_SOURCE_BYTES)
    child_source = directory / "extension.py"
    _copy_verified(source_path, child_source, source_record["sourceSha256"], MAX_SOURCE_BYTES)
    child_input = directory / "input.json"
    if tests is None:
        input_value = validate_series(series)
    else:
        input_value = tests
    input_bytes = canonical(input_value)
    if len(input_bytes) > MAX_TEST_BYTES:
        _fail("EXTENSION_INPUT_LIMIT", "Extension input exceeds its byte limit")
    _safe_write(child_input, input_bytes)
    output = directory / ("validation.json" if tests is not None else "result.json")
    mode = "--test" if tests is not None else "--run"
    child = Path(__file__).with_name("extension_child.py").resolve()
    service.runner.run([str(service.python), "-I", "-S", str(child), "--source", str(child_source),
                        "--input", str(child_input), "--output", str(output), mode],
                       directory, "extension-child", cancel,
                       timeout=service.extension_timeout_seconds, env=_clean_environment(directory))
    if runtime_identity(service, force_verify=True)["runtimeHash"] != runtime["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Extension runtime changed during child execution")
    if (not output.is_file() or output.is_symlink() or output.stat().st_size > MAX_OUTPUT_BYTES or
            file_hash(child_source) != source_record["sourceSha256"] or file_hash(child_input) != hashlib.sha256(input_bytes).hexdigest()):
        _fail("EXTENSION_OUTPUT_LIMIT", "Extension modified inputs or exceeded its output limit")
    return _strict_json(output.read_bytes(), max_bytes=MAX_OUTPUT_BYTES)


def _golden_cases() -> list[dict]:
    series = [
        {"times": [0.0, 0.5, 1.0], "values": [3.0, 3.0, 3.0], "timeUnit": "s", "valueUnit": "m"},
        {"times": [0.0, 1.5, 2.0], "values": [-3.0, 4.0, -4.0], "timeUnit": "s", "valueUnit": "m/s"},
        {"times": [2.5], "values": [-5.0], "timeUnit": "s", "valueUnit": "J"},
    ]
    return [{"id": f"host-reference-{index + 1}", "input": item,
             "expected": statistics(item)} for index, item in enumerate(series)]


def _manifest_schema() -> dict:
    return {"type": "object", "additionalProperties": False,
            "required": ["manifestVersion", "extensionId", "version", "sdkVersion", "kind",
                         "entryPoint", "dependencies", "inputSchema", "outputSchema", "gui"],
            "properties": {"manifestVersion": {"type": "integer", "const": 1},
                           "extensionId": {"type": "string", "pattern": _ID.pattern},
                           "version": {"type": "string", "pattern": _SEMVER.pattern},
                           "sdkVersion": {"type": "string", "const": PROTOCOL_VERSION},
                           "kind": {"type": "string", "const": "numeric-series-statistics"},
                           "entryPoint": {"type": "string", "const": "extension.py:process"},
                           "dependencies": {"type": "array", "maxItems": 0},
                           "inputSchema": {"type": "string", "const": SERIES_SCHEMA},
                           "outputSchema": {"type": "string", "const": STATISTICS_SCHEMA},
                           "gui": {"type": "object"}}}


def _series_schema() -> dict:
    measure = {"type": "number", "minimum": -1e100, "maximum": 1e100}
    unit = {"type": "string", "pattern": _UNIT.pattern}
    return {"type": "object", "additionalProperties": False,
            "required": ["times", "values", "timeUnit", "valueUnit"],
            "properties": {"times": {"type": "array", "minItems": 1, "maxItems": MAX_SERIES_POINTS, "items": measure},
                           "values": {"type": "array", "minItems": 1, "maxItems": MAX_SERIES_POINTS, "items": measure},
                           "timeUnit": unit, "valueUnit": unit},
            "x-hostConstraint": "times and values have equal length; times are strictly increasing",
            "x-hostMaxJsonBytes": MAX_TEST_BYTES}


def _statistics_schema() -> dict:
    return {"type": "object", "additionalProperties": False,
            "required": ["rms", "peakAbs", "peakTime", "timeUnit", "valueUnit"],
            "properties": {"rms": {"type": "number", "minimum": 0},
                           "peakAbs": {"type": "number", "minimum": 0},
                           "peakTime": {"type": "number"},
                           "timeUnit": {"type": "string", "pattern": _UNIT.pattern},
                           "valueUnit": {"type": "string", "pattern": _UNIT.pattern}}}


def _candidate_record(service, candidate_id: str, bundle_hash: str | None = None) -> dict:
    _entry, record = _candidate(service, candidate_id, bundle_hash)
    _source_record(service, record)
    return record


def _normalize_stage(service, parameters: dict) -> dict:
    exact(parameters, {"manifest", "source", "tests"}, {"baseBundleHash"})
    manifest = validate_manifest(parameters["manifest"])
    source = parameters["source"]
    if not isinstance(source, str) or not source or "\x00" in source:
        _fail("INVALID_PARAMETERS", "source must be nonempty UTF-8 Python text")
    try:
        source_bytes = source.encode("utf-8", "strict")
    except UnicodeError:
        _fail("INVALID_PARAMETERS", "source must be valid UTF-8")
    if len(source_bytes) > MAX_SOURCE_BYTES or source.startswith("\ufeff"):
        _fail("EXTENSION_INPUT_LIMIT", "Extension source exceeds its byte limit or contains a BOM")
    tests = _validate_cases(parameters["tests"])
    if len(canonical(tests)) > MAX_TEST_BYTES:
        _fail("EXTENSION_INPUT_LIMIT", "Extension tests exceed their byte limit")
    hashes = _hash_bundle(manifest, source, tests)
    entry = _registry_entry(service, manifest["extensionId"], required=False)
    base = parameters.get("baseBundleHash", entry.get("activeBundleHash") if entry else None)
    if base is not None:
        if not isinstance(base, str) or not re.fullmatch(r"[0-9a-f]{64}", base):
            _fail("INVALID_PARAMETERS", "baseBundleHash must be a SHA-256 digest or null")
        if entry is None or base not in entry.get("versions", {}):
            _fail("EXTENSION_NOT_FOUND", "baseBundleHash must identify a retained version of this extension")
    candidate_count = len(entry.get("candidates", {})) if entry else 0
    if candidate_count >= MAX_CANDIDATES_PER_EXTENSION:
        _fail("EXTENSION_LIMIT", "Too many retained candidates for this extensionId")
    if entry is None and len(service._state["extensions"]) >= MAX_EXTENSIONS:
        _fail("EXTENSION_LIMIT", "Workspace extension registry is full")
    return {"manifest": manifest, "source": source, "tests": tests,
            "candidateId": str(uuid.uuid4()), "bundleHash": hashes["bundle"],
            "hashes": {k: hashes[k] for k in ("manifest", "source", "tests")},
            "registryRevision": entry["registryRevision"] if entry else 0,
            "baseBundleHash": base}


def _bind_stage(service, parameters: dict) -> dict:
    extension_id = parameters["manifest"]["extensionId"]
    entry = _registry_entry(service, extension_id, required=False)
    current_revision = entry["registryRevision"] if entry else 0
    if current_revision != parameters["registryRevision"]:
        _fail("EXTENSION_CONFLICT", "Extension registry changed; create a fresh preview")
    base = parameters["baseBundleHash"]
    base_record = entry.get("versions", {}).get(base) if entry and base else None
    if base and base_record is None:
        _fail("SOURCE_CHANGED", "Base extension version is no longer available")
    base_hash = base_record["sourceSha256"] if base_record else None
    if base_record:
        _safe_relative(service, base_record["sourcePath"], base_hash, MAX_SOURCE_BYTES)
    return {"extensionId": extension_id, "candidateId": parameters["candidateId"],
            "bundleHash": parameters["bundleHash"], "hashes": parameters["hashes"],
            "registryRevision": current_revision, "baseBundleHash": base,
            "baseSourceSha256": base_hash}


def _execute_stage(service, job: dict, directory: Path, cancel: threading.Event) -> dict:
    p = job["parameters"]
    if cancel.is_set():
        _fail("CANCELLED", "Extension staging was cancelled")
    hashes = {**p["hashes"], "bundle": p["bundleHash"]}
    record = _write_candidate(service, p["candidateId"], p["manifest"], p["source"], p["tests"], hashes)
    base = p["baseBundleHash"]
    record["baseBundleHash"] = base
    if base:
        entry = service._state["extensions"][p["manifest"]["extensionId"]]
        base_record = entry["versions"][base]
        record["baseSourcePath"] = base_record["sourcePath"]
        record["baseSourceSha256"] = base_record["sourceSha256"]
    receipt = {"candidateId": p["candidateId"], "bundleHash": p["bundleHash"], "hashes": p["hashes"]}
    (directory / "staged.json").write_bytes(canonical(receipt))
    return {"extensionUpdate": {"operation": "candidate.add", "extensionId": p["manifest"]["extensionId"],
            "expectedRegistryRevision": p["registryRevision"], "record": record},
            "extensionId": p["manifest"]["extensionId"], "version": p["manifest"]["version"],
            "candidateId": p["candidateId"], "bundleHash": p["bundleHash"], "state": "staged",
            "hashes": p["hashes"]}


def _normalize_validate(service, parameters: dict) -> dict:
    exact(parameters, {"candidateId", "bundleHash"})
    record = _candidate_record(service, parameters["candidateId"], parameters["bundleHash"])
    if record["state"] != "staged":
        _fail("EXTENSION_STATE", "Only a staged candidate can be validated")
    if record["validation"] is not None:
        _fail("EXTENSION_STATE", "Candidate validation has already been recorded")
    runtime = runtime_identity(service)
    return {"candidateId": record["candidateId"], "extensionId": record["extensionId"],
            "bundleHash": record["bundleHash"], "runtime": runtime,
            "registryRevision": _registry_entry(service, record["extensionId"])["registryRevision"]}


def _bind_validate(service, parameters: dict) -> dict:
    record = _candidate_record(service, parameters["candidateId"], parameters["bundleHash"])
    if record["state"] != "staged":
        _fail("EXTENSION_STATE", "Only a staged candidate can be validated")
    current = runtime_identity(service)
    if current != parameters["runtime"]:
        _fail("RUNTIME_CHANGED", "Extension runtime changed since the validation preview")
    return {"candidateId": record["candidateId"], "extensionId": record["extensionId"],
            "bundleHash": record["bundleHash"], "sourceSha256": record["sourceSha256"],
            "testsSha256": record["testsSha256"], "manifestSha256": record["manifestSha256"],
            "runtimeHash": current["runtimeHash"],
            "registryRevision": _registry_entry(service, record["extensionId"])["registryRevision"]}


def _execute_validate(service, job: dict, directory: Path, cancel: threading.Event) -> dict:
    p = job["parameters"]
    record = _candidate_record(service, p["candidateId"], p["bundleHash"])
    if not service.trusted_extensions:
        _fail("TRUSTED_RUNTIME_REQUIRED", "Enable trusted developer extension mode before executing Python")
    if p["runtime"] != runtime_identity(service):
        _fail("RUNTIME_CHANGED", "Extension runtime changed after validation preview")
    manifest, _source, candidate_tests = _source_record(service, record)
    all_cases = list(candidate_tests["cases"]) + _golden_cases()
    input_tests = {"cases": [{"id": item["id"], "input": item["input"]} for item in all_cases]}
    validation_record = {**record, "runtimeHash": p["runtime"]["runtimeHash"]}
    results = _execute_source(service, directory, validation_record, {}, cancel, tests=input_tests)
    if not isinstance(results, dict) or set(results) != {"results"} or not isinstance(results["results"], list) or len(results["results"]) != len(all_cases):
        _fail("EXTENSION_INVALID_RESULT", "Validation worker returned an incomplete case report")
    checked = []
    for index, (case, actual_item) in enumerate(zip(all_cases, results["results"])):
        if not isinstance(actual_item, dict) or set(actual_item) != {"id", "output"} or actual_item["id"] != case["id"]:
            _fail("EXTENSION_INVALID_RESULT", "Validation worker returned an invalid case record")
        expected = statistics(case["input"])
        if case["expected"] is not None and not _matches(case["expected"], expected):
            _fail("EXTENSION_REFERENCE_MISMATCH", f"Candidate expected value differs from independent host reference in case {case['id']}")
        actual = _result(actual_item["output"], case["input"])
        if not _matches(actual, expected):
            _fail("EXTENSION_TEST_FAILED", f"Candidate output differs from independent host reference in case {case['id']}")
        checked.append({"id": case["id"], "passed": True, "expected": expected, "actual": actual})
    report = {"protocolVersion": PROTOCOL_VERSION, "candidateId": record["candidateId"],
              "bundleHash": record["bundleHash"], "runtimeHash": p["runtime"]["runtimeHash"],
              "caseCount": len(candidate_tests["cases"]), "independentCaseCount": len(_golden_cases()),
              "passed": True, "cases": checked}
    report_raw = canonical(report)
    if len(report_raw) > MAX_REPORT_BYTES:
        _fail("EXTENSION_OUTPUT_LIMIT", "Validation report exceeds its byte limit")
    report_path = directory / "validation-report.json"
    _safe_write(report_path, report_raw)
    report_hash = hashlib.sha256(report_raw).hexdigest()
    validation = {"passed": True, "caseCount": len(candidate_tests["cases"]),
                  "independentCaseCount": len(_golden_cases()), "reportPath": str(report_path.relative_to(service.workspace).as_posix()),
                  "reportSha256": report_hash, "runtimeHash": p["runtime"]["runtimeHash"], "validatedAt": utc()}
    return {"extensionUpdate": {"operation": "candidate.validate", "extensionId": record["extensionId"],
            "expectedRegistryRevision": p["registryRevision"], "candidateId": record["candidateId"],
            "bundleHash": record["bundleHash"], "validation": validation},
            "extensionId": record["extensionId"], "version": manifest["version"],
            "candidateId": record["candidateId"], "bundleHash": record["bundleHash"],
            "state": "validated", "runtimeHash": p["runtime"]["runtimeHash"],
            "reportHash": report_hash,
            "validation": {"passed": True, "caseCount": validation["caseCount"],
                           "independentCaseCount": validation["independentCaseCount"]}}


def _normalize_activate(service, parameters: dict) -> dict:
    exact(parameters, {"candidateId", "bundleHash"})
    entry, candidate = _candidate(service, parameters["candidateId"], parameters["bundleHash"])
    if candidate["state"] != "validated" or not candidate.get("validation", {}).get("passed"):
        _fail("EXTENSION_STATE", "Only a fully validated candidate may be activated")
    if candidate["bundleHash"] in entry.get("versions", {}):
        _fail("EXTENSION_CONFLICT", "This immutable bundle is already retained")
    if any(item["manifest"]["version"] == candidate["version"] for item in entry.get("versions", {}).values()):
        _fail("EXTENSION_VERSION_CONFLICT", "An extension version number cannot be reused")
    if len(entry.get("versions", {})) >= MAX_VERSIONS_PER_EXTENSION:
        _fail("EXTENSION_LIMIT", "Too many retained immutable extension versions")
    return {"extensionId": candidate["extensionId"], "candidateId": candidate["candidateId"],
            "bundleHash": candidate["bundleHash"],
            "registryRevision": entry["registryRevision"],
            "expectedActiveBundleHash": entry.get("activeBundleHash"),
            "record": copy.deepcopy(candidate)}


def _bind_activate(service, parameters: dict) -> dict:
    entry, candidate = _candidate(service, parameters["candidateId"], parameters["bundleHash"])
    if candidate["state"] != "validated" or not candidate.get("validation", {}).get("passed"):
        _fail("EXTENSION_STATE", "Candidate validation is unavailable")
    _source_record(service, candidate)
    validation = candidate["validation"]
    _safe_relative(service, validation["reportPath"], validation["reportSha256"], MAX_REPORT_BYTES)
    runtime = runtime_identity(service)
    if runtime["runtimeHash"] != validation["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Candidate must be revalidated for the current runtime")
    if entry["registryRevision"] != parameters["registryRevision"] or entry.get("activeBundleHash") != parameters["expectedActiveBundleHash"]:
        _fail("EXTENSION_CONFLICT", "Extension registry changed since activation was previewed")
    return {"extensionId": candidate["extensionId"], "candidateId": candidate["candidateId"],
            "bundleHash": candidate["bundleHash"], "registryRevision": entry["registryRevision"],
            "expectedActiveBundleHash": entry.get("activeBundleHash"),
            "manifestSha256": candidate["manifestSha256"], "sourceSha256": candidate["sourceSha256"],
            "testsSha256": candidate["testsSha256"], "reportSha256": validation["reportSha256"],
            "runtimeHash": runtime["runtimeHash"]}


def _execute_activate(service, job: dict, directory: Path, cancel: threading.Event) -> dict:
    p = job["parameters"]
    if cancel.is_set():
        _fail("CANCELLED", "Extension activation was cancelled")
    _bind_activate(service, p)
    previous = p["expectedActiveBundleHash"]
    return {"extensionUpdate": {"operation": "activate", "extensionId": p["extensionId"],
            "expectedRegistryRevision": p["registryRevision"],
            "expectedActiveBundleHash": previous, "candidateId": p["candidateId"],
            "bundleHash": p["bundleHash"]},
            "extensionId": p["extensionId"], "version": p["record"]["version"],
            "bundleHash": p["bundleHash"], "state": "active",
            "previousBundleHash": previous,
            "capabilityRevision": service._capability_revision() + 1}


def _normalize_rollback(service, parameters: dict) -> dict:
    exact(parameters, {"extensionId", "expectedActiveBundleHash", "targetBundleHash"})
    extension_id = identifier(parameters["extensionId"], "extensionId")
    entry = _registry_entry(service, extension_id)
    current = entry.get("activeBundleHash")
    if not isinstance(parameters["expectedActiveBundleHash"], str) or parameters["expectedActiveBundleHash"] != current:
        _fail("EXTENSION_CONFLICT", "expectedActiveBundleHash must match the currently active version")
    target = parameters["targetBundleHash"]
    if not isinstance(target, str) or target not in entry.get("versions", {}):
        _fail("EXTENSION_NOT_FOUND", "targetBundleHash must identify a retained immutable version")
    if target == current:
        _fail("EXTENSION_STATE", "Requested rollback target is already active")
    version = entry["versions"][target]
    _source_record(service, version)
    runtime_hash = runtime_identity(service)["runtimeHash"]
    if runtime_hash != version["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Rollback target was validated for a different runtime")
    return {"extensionId": extension_id, "expectedActiveBundleHash": current,
            "targetBundleHash": target, "targetVersion": version["manifest"]["version"],
            "registryRevision": entry["registryRevision"], "runtimeHash": runtime_hash}


def _bind_rollback(service, parameters: dict) -> dict:
    entry, target = _version(service, parameters["extensionId"], parameters["targetBundleHash"])
    _source_record(service, target)
    runtime = runtime_identity(service)
    if runtime["runtimeHash"] != parameters["runtimeHash"] or runtime["runtimeHash"] != target["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Rollback target was validated for a different runtime")
    if entry.get("activeBundleHash") != parameters["expectedActiveBundleHash"] or entry["registryRevision"] != parameters["registryRevision"]:
        _fail("EXTENSION_CONFLICT", "Extension registry changed since rollback was previewed")
    return {"extensionId": parameters["extensionId"], "targetBundleHash": parameters["targetBundleHash"],
            "targetVersion": parameters["targetVersion"], "expectedActiveBundleHash": parameters["expectedActiveBundleHash"],
            "registryRevision": entry["registryRevision"], "sourceSha256": target["sourceSha256"],
            "runtimeHash": runtime["runtimeHash"]}


def _execute_rollback(service, job: dict, directory: Path, cancel: threading.Event) -> dict:
    p = job["parameters"]
    if cancel.is_set():
        _fail("CANCELLED", "Extension rollback was cancelled")
    _bind_rollback(service, p)
    return {"extensionUpdate": {"operation": "rollback", "extensionId": p["extensionId"],
            "expectedRegistryRevision": p["registryRevision"],
            "expectedActiveBundleHash": p["expectedActiveBundleHash"],
            "targetBundleHash": p["targetBundleHash"]},
            "extensionId": p["extensionId"], "activeBundleHash": p["targetBundleHash"],
            "targetBundleHash": p["targetBundleHash"], "state": "active",
            "capabilityRevision": service._capability_revision() + 1}


def _resolve_provenance(service, provenance: dict) -> tuple[dict, dict]:
    if isinstance(provenance, dict) and "channelId" in provenance:
        from .native_results import resolve_series
        series, origin = resolve_series(service, provenance)
        return validate_series(series), origin
    exact(provenance, {"jobId", "signal"})
    job_id = identifier(provenance["jobId"], "jobId")
    signal = provenance["signal"]
    if signal not in _SOURCE_SIGNALS:
        _fail("INVALID_PARAMETERS", "signal is not an allowed accepted workflow series")
    source_job = service._job(job_id)
    if source_job["state"] != "succeeded" or source_job.get("command") not in ("workflow.run", "workflow.resume"):
        _fail("RESULT_UNAVAILABLE", "Accepted-series provenance requires a successful native workflow job")
    result = source_job.get("result", {})
    reference_path = result.get("referencePath")
    if not isinstance(reference_path, str) or not Path(reference_path).is_absolute():
        _fail("RESULT_UNAVAILABLE", "Workflow result does not contain an independent reference artifact")
    reference = Path(reference_path)
    job_dir = service.workspace / "jobs" / job_id
    if reference.is_symlink() or not reference.resolve().is_relative_to(job_dir.resolve()):
        _fail("ARTIFACT_PATH", "Reference history must remain inside the accepted workflow job")
    matches = [item for item in source_job.get("artifacts", []) if Path(item.get("path", "")) == reference]
    if len(matches) != 1 or file_hash(reference) != matches[0]["sha256"]:
        _fail("SOURCE_CHANGED", "Accepted independent-reference artifact hash changed")
    artifact_hash = matches[0]["sha256"]
    if reference.stat().st_size > 32 * 1024 * 1024:
        _fail("ARTIFACT_LIMIT", "Reference history exceeds the extension input limit")
    data = _strict_json(reference.read_bytes(), max_bytes=32 * 1024 * 1024,
                        max_collection_size=MAX_SERIES_POINTS)
    path_parts = _SOURCE_SIGNALS[signal]
    time_outputs = data.get("timeOutputs")
    if not isinstance(time_outputs, dict) or not isinstance(time_outputs.get("timeSeconds"), list):
        _fail("RESULT_UNAVAILABLE", "Reference artifact has no accepted time grid")
    times = time_outputs["timeSeconds"]
    if signal.endswith("sensorVelocity"):
        values, time_unit, value_unit = time_outputs.get(path_parts[1]), "s", path_parts[2]
    else:
        values = data.get(path_parts[0], {}).get(path_parts[1])
        time_unit, value_unit = "s", path_parts[2]
    series = validate_series({"times": times, "values": values,
                              "timeUnit": time_unit, "valueUnit": value_unit})
    return series, {"sourceKind": "independent-reference-history", "jobId": job_id,
                    "signal": signal, "artifactSha256": artifact_hash}


def _normalize_run(service, parameters: dict) -> dict:
    exact(parameters, {"extensionId", "bundleHash"}, {"series", "provenance"})
    if ("series" in parameters) == ("provenance" in parameters):
        _fail("INVALID_PARAMETERS", "Provide exactly one of series or provenance")
    extension_id = identifier(parameters["extensionId"], "extensionId")
    entry, version = _version(service, extension_id, parameters["bundleHash"])
    if entry.get("activeBundleHash") != parameters["bundleHash"]:
        _fail("EXTENSION_NOT_ACTIVE", "Run requires the exact currently active bundleHash")
    if "series" in parameters:
        series = validate_series(parameters["series"])
        if len(canonical(series)) > MAX_TEST_BYTES:
            _fail("EXTENSION_INPUT_LIMIT", "Numeric-series input exceeds its byte limit")
        provenance = {"sourceKind": "caller-supplied"}
    else:
        series, provenance = _resolve_provenance(service, parameters["provenance"])
        if len(canonical(series)) > MAX_TEST_BYTES:
            _fail("EXTENSION_INPUT_LIMIT", "Verified reference series exceeds its byte limit")
    return {"extensionId": extension_id, "bundleHash": parameters["bundleHash"],
            "series": series, "provenance": provenance,
            "version": version["manifest"]["version"],
            "runtimeHash": runtime_identity(service)["runtimeHash"]}


def _bind_run(service, parameters: dict) -> dict:
    entry, version = _version(service, parameters["extensionId"], parameters["bundleHash"])
    if entry.get("activeBundleHash") != parameters["bundleHash"]:
        _fail("EXTENSION_NOT_ACTIVE", "Run preview refers to a version that is no longer active")
    manifest, _source, _tests = _source_record(service, version)
    validation = version.get("validation", {})
    _safe_relative(service, validation["reportPath"], validation["reportSha256"], MAX_REPORT_BYTES)
    runtime = runtime_identity(service)
    if runtime["runtimeHash"] != parameters["runtimeHash"] or runtime["runtimeHash"] != version["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Active version was validated for a different extension runtime")
    provenance = parameters["provenance"]
    if provenance["sourceKind"] == "independent-reference-history":
        source_job = service._job(provenance["jobId"])
        series, current = _resolve_provenance(service, {"jobId": provenance["jobId"], "signal": provenance["signal"]})
        if current["artifactSha256"] != provenance["artifactSha256"] or series != parameters["series"]:
            _fail("SOURCE_CHANGED", "Accepted reference history changed since preview")
    elif provenance["sourceKind"] in ("native-active-fe", "native-passive-fe", "native-fe"):
        series, current = _resolve_provenance(service, {key: provenance[key]
            for key in ("jobId", "mode", "artifactId", "channelId")})
        if current != provenance or series != parameters["series"]:
            _fail("SOURCE_CHANGED", "Native history changed since preview")
    return {"extensionId": parameters["extensionId"], "bundleHash": parameters["bundleHash"],
            "manifestSha256": version["manifestSha256"], "sourceSha256": version["sourceSha256"],
            "testsSha256": version["testsSha256"], "reportSha256": validation["reportSha256"],
            "runtimeHash": runtime["runtimeHash"], "seriesSha256": hashlib.sha256(canonical(parameters["series"])).hexdigest(),
            "provenance": provenance, "sourcePath": version["sourcePath"],
            "version": manifest["version"]}


def _execute_run(service, job: dict, directory: Path, cancel: threading.Event) -> dict:
    p = job["parameters"]
    if not service.trusted_extensions:
        _fail("TRUSTED_RUNTIME_REQUIRED", "Enable trusted developer extension mode before executing Python")
    binding = job["_extensionBinding"]
    if p["runtimeHash"] != runtime_identity(service)["runtimeHash"]:
        _fail("RUNTIME_CHANGED", "Extension runtime changed after run preview")
    _version_entry, version = _version(service, p["extensionId"], p["bundleHash"])
    if version["sourceSha256"] != binding["sourceSha256"] or version["sourcePath"] != binding["sourcePath"]:
        _fail("EXTENSION_TAMPERED", "Pinned extension version changed after submission")
    # A queued invocation owns this immutable bundle and input snapshot even if
    # activation or rollback changes the current pointer while it is running.
    series = validate_series(p["series"])
    if hashlib.sha256(canonical(series)).hexdigest() != binding["seriesSha256"]:
        _fail("SOURCE_CHANGED", "Numeric-series input changed after preview")
    native_origin = p["provenance"]["sourceKind"] in ("native-active-fe", "native-passive-fe", "native-fe")
    if native_origin:
        verified, origin = _resolve_provenance(service, {key: p["provenance"][key]
            for key in ("jobId", "mode", "artifactId", "channelId")})
        if verified != series or origin != p["provenance"]:
            _fail("SOURCE_CHANGED", "Native source changed before extension execution")
    output = _execute_source(service, directory, {**version, "runtimeHash": binding["runtimeHash"]},
                             series, cancel)
    if native_origin:
        verified, origin = _resolve_provenance(service, {key: p["provenance"][key]
            for key in ("jobId", "mode", "artifactId", "channelId")})
        if verified != series or origin != p["provenance"]:
            _fail("SOURCE_CHANGED", "Native source changed during extension execution")
    actual = _result(output, series)
    expected = statistics(series)
    if not _matches(actual, expected):
        _fail("EXTENSION_REFERENCE_MISMATCH", "Extension output differs from the host statistics reference")
    (directory / "input-provenance.json").write_bytes(canonical({"provenance": p["provenance"],
        "seriesSha256": binding["seriesSha256"], "bundleHash": p["bundleHash"],
        "runtimeHash": binding["runtimeHash"]}))
    result = {"extensionId": p["extensionId"], "version": binding["version"],
              "bundleHash": p["bundleHash"], "sourceKind": p["provenance"]["sourceKind"],
              "sourceJobId": p["provenance"].get("jobId"),
              "sourceSignal": p["provenance"].get("signal"),
              "sourceArtifactSha256": p["provenance"].get("artifactSha256"),
              "seriesSha256": binding["seriesSha256"], "metrics": actual}
    return result


def _update_entry(entry: dict | None) -> dict:
    if entry is None:
        return {"registryRevision": 0, "capabilityRevision": 0, "activeBundleHash": None,
                "candidates": {}, "versions": {}}
    return entry


def validate_registry_state(extensions: dict):
    """Validate persisted registry metadata before it becomes live service state."""
    def require(condition: bool, message: str):
        if not condition:
            raise ValueError(message)

    def hash_value(value: Any) -> bool:
        return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None

    def relative_path(value: Any, expected: str):
        require(isinstance(value, str) and len(value) <= 512 and "\\" not in value,
                "invalid extension registry path")
        parsed = PurePosixPath(value)
        require(not parsed.is_absolute() and all(part not in ("", ".", "..") for part in parsed.parts),
                "invalid extension registry path")
        require(value == expected, "extension registry path does not match its immutable record")

    require(isinstance(extensions, dict) and len(extensions) <= MAX_EXTENSIONS,
            "invalid extension registry")
    for extension_id, entry in extensions.items():
        require(isinstance(extension_id, str) and _ID.fullmatch(extension_id) is not None,
                "invalid extension ID")
        require(isinstance(entry, dict) and set(entry) == {
            "registryRevision", "capabilityRevision", "activeBundleHash", "candidates", "versions"},
            "invalid extension registry entry")
        registry_revision = entry["registryRevision"]
        capability_revision = entry["capabilityRevision"]
        require(type(registry_revision) is int and 1 <= registry_revision <= 2**53 - 1,
                "invalid extension registry revision")
        require(type(capability_revision) is int and 0 <= capability_revision <= registry_revision,
                "invalid extension capability revision")
        candidates, versions = entry["candidates"], entry["versions"]
        require(isinstance(candidates, dict) and len(candidates) <= MAX_CANDIDATES_PER_EXTENSION and
                isinstance(versions, dict) and len(versions) <= MAX_VERSIONS_PER_EXTENSION,
                "invalid extension registry collections")
        active = entry["activeBundleHash"]
        require(active is None or (hash_value(active) and active in versions),
                "active extension version is not retained")

        records = [(candidate_id, record, False) for candidate_id, record in candidates.items()]
        records.extend((record.get("candidateId"), record, True) for record in versions.values()
                       if isinstance(record, dict))
        require(len(records) == len(candidates) + len(versions), "invalid extension version record")
        for candidate_id, record, is_version in records:
            try:
                require(str(uuid.UUID(candidate_id)) == candidate_id, "invalid extension candidate identity")
            except (ValueError, TypeError, AttributeError):
                raise ValueError("invalid extension candidate identity") from None
            require(isinstance(record, dict), "invalid extension candidate record")
            expected_keys = {"candidateId", "extensionId", "version", "bundleHash", "manifest",
                "manifestSha256", "sourceSha256", "testsSha256", "manifestPath", "sourcePath",
                "testsPath", "baseBundleHash", "baseSourcePath", "baseSourceSha256", "state",
                "stagedAt", "validation"}
            if record.get("state") == "validated" or is_version:
                expected_keys.add("runtimeHash")
            require(set(record) == expected_keys and record["candidateId"] == candidate_id and
                    record["extensionId"] == extension_id, "invalid extension candidate record")
            require(record["state"] in ({"validated"} if is_version else {"staged", "validated"}),
                    "invalid extension candidate state")
            try:
                manifest = validate_manifest(record["manifest"])
            except WorkflowError as error:
                raise ValueError("invalid extension manifest metadata") from error
            require(manifest["extensionId"] == extension_id and manifest["version"] == record["version"],
                    "invalid extension manifest identity")
            require(hash_value(record["bundleHash"]) and
                    all(hash_value(record[key]) for key in ("manifestSha256", "sourceSha256", "testsSha256")),
                    "invalid extension content hashes")
            require(hashlib.sha256(canonical(manifest)).hexdigest() == record["manifestSha256"] and
                    digest({"sdkVersion": PROTOCOL_VERSION, "manifest": record["manifestSha256"],
                            "source": record["sourceSha256"], "tests": record["testsSha256"]}) == record["bundleHash"],
                    "extension bundle identity differs from its content hashes")
            require(isinstance(record["stagedAt"], str) and len(record["stagedAt"]) <= 64,
                    "invalid extension staging timestamp")
            directory = f"extensions/candidates/{candidate_id}"
            relative_path(record["manifestPath"], f"{directory}/manifest.json")
            relative_path(record["sourcePath"], f"{directory}/extension.py")
            relative_path(record["testsPath"], f"{directory}/tests.json")
            base_hash, base_path, base_source_hash = (record["baseBundleHash"], record["baseSourcePath"],
                                                       record["baseSourceSha256"])
            require((base_hash is None and base_path is None and base_source_hash is None) or
                    (hash_value(base_hash) and hash_value(base_source_hash) and
                     isinstance(base_path, str) and base_path.startswith("extensions/candidates/")),
                    "invalid extension base source identity")
            if base_path is not None:
                relative_path(base_path, base_path)
            if is_version:
                require(record["bundleHash"] in versions and
                        versions[record["bundleHash"]] is record,
                        "version key differs from its immutable bundle hash")
            if record["state"] == "staged":
                require(record["validation"] is None and "runtimeHash" not in record,
                        "staged candidate contains validation data")
            else:
                validation = record["validation"]
                require(isinstance(validation, dict) and set(validation) == {
                    "passed", "caseCount", "independentCaseCount", "reportPath", "reportSha256",
                    "runtimeHash", "validatedAt"} and validation["passed"] is True,
                    "invalid extension validation record")
                require(type(validation["caseCount"]) is int and 1 <= validation["caseCount"] <= MAX_TEST_CASES and
                        type(validation["independentCaseCount"]) is int and validation["independentCaseCount"] == 3 and
                        hash_value(validation["reportSha256"]) and hash_value(validation["runtimeHash"]) and
                        record["runtimeHash"] == validation["runtimeHash"], "invalid extension validation identity")
                report = validation["reportPath"]
                require(isinstance(report, str) and report.endswith("/validation-report.json") and
                        report.startswith("jobs/"), "invalid extension validation report path")
                relative_path(report, report)
                require(isinstance(validation["validatedAt"], str) and len(validation["validatedAt"]) <= 64,
                        "invalid extension validation timestamp")
        for bundle_hash, record in versions.items():
            require(hash_value(bundle_hash) and record["bundleHash"] == bundle_hash,
                    "extension version key differs from bundle hash")
        for _candidate_id, record, _is_version in records:
            base_hash = record["baseBundleHash"]
            if base_hash is not None:
                base = versions.get(base_hash)
                require(base is not None and record["baseSourcePath"] == base["sourcePath"] and
                        record["baseSourceSha256"] == base["sourceSha256"],
                        "extension base source is not a retained version")


def merge_update(extensions: dict, update: dict):
    """Apply a registry compare-and-swap patch under the service state lock."""
    exact(update, {"operation", "extensionId", "expectedRegistryRevision"},
          {"record", "candidateId", "bundleHash", "validation", "expectedActiveBundleHash", "targetBundleHash"})
    extension_id = identifier(update["extensionId"], "extensionId")
    entry = _update_entry(extensions.get(extension_id))
    expected_revision = integer(update["expectedRegistryRevision"], "expectedRegistryRevision", 0, 2**53 - 1)
    if entry["registryRevision"] != expected_revision:
        _fail("EXTENSION_CONFLICT", "Extension registry changed before the approved update completed")
    operation = update["operation"]
    if operation == "candidate.add":
        record = update["record"]
        candidate_id = identifier(record["candidateId"], "candidateId")
        if record["extensionId"] != extension_id or candidate_id in entry["candidates"]:
            _fail("EXTENSION_CONFLICT", "Candidate identity conflicts with the extension registry")
        entry["candidates"][candidate_id] = record
    elif operation == "candidate.validate":
        candidate_id = identifier(update["candidateId"], "candidateId")
        candidate = entry["candidates"].get(candidate_id)
        if candidate is None or candidate["bundleHash"] != update["bundleHash"] or candidate["state"] != "staged":
            _fail("EXTENSION_CONFLICT", "Candidate changed before validation could be published")
        candidate["state"] = "validated"
        candidate["validation"] = update["validation"]
        candidate["runtimeHash"] = update["validation"]["runtimeHash"]
    elif operation == "activate":
        if entry.get("activeBundleHash") != update["expectedActiveBundleHash"]:
            _fail("EXTENSION_CONFLICT", "Active version changed before activation could be published")
        candidate = entry["candidates"].get(update["candidateId"])
        if candidate is None or candidate["bundleHash"] != update["bundleHash"] or candidate["state"] != "validated":
            _fail("EXTENSION_CONFLICT", "Validated activation candidate changed")
        if update["bundleHash"] in entry["versions"]:
            _fail("EXTENSION_CONFLICT", "Immutable extension version already exists")
        if len(entry["versions"]) >= MAX_VERSIONS_PER_EXTENSION:
            _fail("EXTENSION_LIMIT", "Too many retained immutable extension versions")
        if any(value["manifest"]["version"] == candidate["version"] for value in entry["versions"].values()):
            _fail("EXTENSION_VERSION_CONFLICT", "An extension version number cannot be reused")
        entry["versions"][update["bundleHash"]] = copy.deepcopy(candidate)
        entry["activeBundleHash"] = update["bundleHash"]
        entry["capabilityRevision"] += 1
    elif operation == "rollback":
        if entry.get("activeBundleHash") != update["expectedActiveBundleHash"]:
            _fail("EXTENSION_CONFLICT", "Active version changed before rollback could be published")
        if update["targetBundleHash"] not in entry["versions"]:
            _fail("EXTENSION_NOT_FOUND", "Rollback target is no longer retained")
        entry["activeBundleHash"] = update["targetBundleHash"]
        entry["capabilityRevision"] += 1
    else:
        _fail("EXTENSION_UPDATE", "Unknown extension registry update")
    if entry["registryRevision"] >= 2**53 - 1 or entry["capabilityRevision"] >= 2**53 - 1:
        _fail("EXTENSION_LIMIT", "Extension registry revision limit reached")
    entry["registryRevision"] += 1
    extensions[extension_id] = entry
    return sum(value.get("capabilityRevision", 0) for value in extensions.values())


def compact_job_parameters(command: str, parameters: dict) -> dict:
    """Keep generated source and large input arrays out of persistent state."""
    if command == "extension.stage":
        return {"extensionId": parameters["manifest"]["extensionId"],
                "candidateId": parameters["candidateId"], "bundleHash": parameters["bundleHash"],
                "hashes": copy.deepcopy(parameters["hashes"])}
    if command == "extension.run":
        return {"extensionId": parameters["extensionId"], "bundleHash": parameters["bundleHash"],
                "version": parameters["version"], "runtimeHash": parameters["runtimeHash"],
                "provenance": copy.deepcopy(parameters["provenance"]),
                "seriesSha256": hashlib.sha256(canonical(parameters["series"])).hexdigest()}
    return copy.deepcopy(parameters)


def _candidate_view(service, record: dict, *, include_diff: bool) -> dict:
    manifest, source, tests = _source_record(service, record)
    validation = copy.deepcopy(record.get("validation"))
    if validation:
        report_path = _safe_relative(service, validation["reportPath"], validation["reportSha256"], MAX_REPORT_BYTES)
        report = _strict_json(report_path.read_bytes(), max_bytes=MAX_REPORT_BYTES)
    else:
        report = None
    value = {"candidateId": record["candidateId"], "extensionId": record["extensionId"],
             "version": record["version"], "bundleHash": record["bundleHash"],
             "state": record["state"], "manifest": manifest, "source": source, "tests": tests,
             "hashes": {"manifest": record["manifestSha256"], "source": record["sourceSha256"],
                        "tests": record["testsSha256"]}, "validation": validation,
             "report": report, "baseBundleHash": record.get("baseBundleHash")}
    if include_diff:
        base_source = ""
        base_hash = record.get("baseSourceSha256")
        if base_hash:
            base_path = _safe_relative(service, record["baseSourcePath"], base_hash, MAX_SOURCE_BYTES)
            base_source = base_path.read_text(encoding="utf-8", errors="strict")
        diff = "".join(difflib.unified_diff(base_source.splitlines(keepends=True), source.splitlines(keepends=True),
                                            fromfile=("base:" + str(record.get("baseBundleHash") or "empty")),
                                            tofile=("candidate:" + record["bundleHash"])))
        if len(diff.encode("utf-8")) > 2 * MAX_SOURCE_BYTES:
            _fail("EXTENSION_OUTPUT_LIMIT", "Extension source diff exceeds its byte limit")
        value["baseSource"] = base_source
        value["sourceDiff"] = {"format": "unified-diff", "text": diff}
    return value


def read_operation(service, operation: str, parameters: dict) -> dict | None:
    if operation == "extension.list":
        exact(parameters, set(), {"extensionId", "candidateOffset", "candidateLimit"})
        requested_id = parameters.get("extensionId")
        if requested_id is not None:
            requested_id = identifier(requested_id, "extensionId")
            if requested_id not in service._state["extensions"]:
                _fail("EXTENSION_NOT_FOUND", "Unknown extensionId")
        if requested_id is None and ("candidateOffset" in parameters or "candidateLimit" in parameters):
            _fail("INVALID_PARAMETERS", "Candidate paging requires extensionId")
        candidate_offset = integer(parameters.get("candidateOffset", 0), "candidateOffset", 0, 64)
        candidate_limit = integer(parameters.get("candidateLimit", 16), "candidateLimit", 1, 32)
        extensions = []
        selected = ({requested_id: service._state["extensions"][requested_id]} if requested_id is not None
                    else service._state["extensions"])
        for extension_id, entry in sorted(selected.items()):
            active_hash = entry.get("activeBundleHash")
            versions = []
            show_detail = requested_id is not None
            for bundle_hash, record in sorted(entry.get("versions", {}).items()) if show_detail else ():
                validation = record.get("validation") or {}
                versions.append({"version": record["version"], "bundleHash": bundle_hash,
                    "state": "active" if bundle_hash == active_hash else "retained",
                    "manifestSha256": record["manifestSha256"], "reportSha256": validation.get("reportSha256"),
                    "runtimeHash": record.get("runtimeHash") or validation.get("runtimeHash")})
            candidate_records = sorted(entry.get("candidates", {}).items(),
                key=lambda pair: (pair[1].get("stagedAt", ""), pair[0]), reverse=True)
            candidates = []
            page = candidate_records[candidate_offset:candidate_offset + candidate_limit] if show_detail else ()
            for candidate_id, record in page:
                validation = record.get("validation") or {}
                candidates.append({"candidateId": candidate_id, "version": record["version"],
                    "bundleHash": record["bundleHash"], "state": record["state"],
                    "manifestSha256": record["manifestSha256"], "sourceSha256": record["sourceSha256"],
                    "testsSha256": record["testsSha256"], "reportSha256": validation.get("reportSha256"),
                    "baseBundleHash": record.get("baseBundleHash")})
            active = entry.get("versions", {}).get(active_hash, {}) if active_hash else {}
            summary = {"extensionId": extension_id, "registryRevision": entry["registryRevision"],
                "activeBundleHash": active_hash, "activeVersion": active.get("version"),
                "versionCount": len(entry.get("versions", {})),
                "candidateCount": len(candidate_records)}
            if show_detail:
                next_offset = candidate_offset + len(candidates)
                summary.update({"versions": versions, "candidates": candidates,
                                "candidateTotal": len(candidate_records),
                                "nextCandidateOffset": next_offset if next_offset < len(candidate_records) else None})
            extensions.append(summary)
        return {"apiVersion": "range.workflow/v1", "capabilityRevision": service._capability_revision(),
                "extensions": extensions}
    if operation == "extension.candidate.get":
        exact(parameters, {"candidateId"})
        _entry, record = _candidate(service, parameters["candidateId"])
        return {"candidate": _candidate_view(service, record, include_diff=True)}
    if operation == "extension.version.get":
        exact(parameters, {"extensionId", "bundleHash"})
        _entry, record = _version(service, parameters["extensionId"], parameters["bundleHash"])
        manifest, source, tests = _source_record(service, record)
        validation = record.get("validation") or {}
        report_path = _safe_relative(service, validation["reportPath"], validation["reportSha256"], MAX_REPORT_BYTES)
        report = _strict_json(report_path.read_bytes(), max_bytes=MAX_REPORT_BYTES)
        return {"version": {"extensionId": record["extensionId"], "version": record["version"],
            "bundleHash": record["bundleHash"],
            "state": "active" if _entry["activeBundleHash"] == record["bundleHash"] else "retained",
            "manifest": manifest, "source": source, "tests": tests,
            "validation": validation, "report": report,
            "hashes": {"manifest": record["manifestSha256"], "source": record["sourceSha256"],
                       "tests": record["testsSha256"]}, "reportSha256": validation["reportSha256"],
            "runtimeHash": record.get("runtimeHash") or validation["runtimeHash"]}}
    if operation == "schema.get":
        exact(parameters, {"schemaId"})
        schemas = {MANIFEST_SCHEMA: _manifest_schema(), SERIES_SCHEMA: _series_schema(),
                   STATISTICS_SCHEMA: _statistics_schema()}
        schema_id = parameters["schemaId"]
        if schema_id not in schemas:
            _fail("SCHEMA_NOT_FOUND", "Unknown versioned extension schema")
        return {"schemaId": schema_id, "apiVersion": PROTOCOL_VERSION, "schema": schemas[schema_id]}
    if operation == "knowledge.query":
        exact(parameters, {"query"}, {"extensionId"})
        query = parameters["query"]
        if not isinstance(query, str) or len(query) > 256:
            _fail("INVALID_PARAMETERS", "query must be at most 256 characters")
        extension_id = parameters.get("extensionId")
        if extension_id is not None:
            extension_id = identifier(extension_id, "extensionId")
        return _knowledge(query, extension_id, service)
    return None


def _knowledge(query: str, extension_id: str | None, service) -> dict:
    terms = [x.casefold() for x in re.findall(r"[A-Za-z0-9._/-]+|[^\s]+", query) if len(x) > 1]
    documents = []
    package_path = Path(__file__).resolve()
    roots = (package_path.parents[3] / "doc", package_path.parents[4] / "docs")
    for filename in ("AGENT_OPERATIONS.md", "WORKFLOW_CONTRACT.md", "EXTENSION_SDK.md",
                     "NATIVE_MODEL_OPERATIONS.md", "NATIVE_DOCUMENT_SDK.md", "NATIVE_RESULTS.md", "PROJECT_CATALOG.md"):
        found = next((root / filename for root in roots if (root / filename).is_file() and not (root / filename).is_symlink()), None)
        if found is None or found.stat().st_size > 256 * 1024:
            continue
        try:
            raw = found.read_bytes()
            text = raw.decode("utf-8", "strict")
        except (OSError, UnicodeError):
            continue
        sections = []
        current_title = filename
        current = []
        for line in text.splitlines():
            if line.startswith("#"):
                if current:
                    sections.append((current_title, "\n".join(current).strip()))
                current_title, current = line.lstrip("# ").strip(), [line]
            else:
                current.append(line)
        if current:
            sections.append((current_title, "\n".join(current).strip()))
        for title, section in sections:
            haystack = (title + "\n" + section).casefold()
            score = sum(1 for term in terms if term in haystack) if terms else 1
            if score:
                documents.append((score, len(terms), filename, title, section, hashlib.sha256(raw).hexdigest()))
    documents.sort(key=lambda row: (-row[0], row[2], row[3]))
    results = [{"title": title, "sourcePath": "doc/" + filename,
                "protocolVersion": "range.workflow/v1", "text": section[:4000], "sha256": sha}
               for _, _, filename, title, section, sha in documents[:5]]
    if extension_id is not None:
        entry = _registry_entry(service, extension_id)
        active_hash = entry.get("activeBundleHash")
        if active_hash:
            version = entry["versions"][active_hash]
            manifest = version["manifest"]
            text = canonical({"kind": manifest["kind"], "sdkVersion": manifest["sdkVersion"],
                              "inputSchema": manifest["inputSchema"], "outputSchema": manifest["outputSchema"],
                              "gui": manifest["gui"], "bundleHash": active_hash}).decode("utf-8")
            results.insert(0, {"title": "Active extension " + extension_id,
                "sourcePath": "workspace/extensions/candidates/" + version["candidateId"] + "/manifest.json",
                "protocolVersion": PROTOCOL_VERSION, "text": text[:4000],
                "sha256": version["manifestSha256"]})
    return {"items": results}


def install(service):
    commands = [
        ("extension.stage", _normalize_stage, _execute_stage, _bind_stage,
         "Stage an immutable numeric-series extension candidate without executing it", {
             "manifest": {"type": "object"}, "source": {"type": "string", "maxLength": MAX_SOURCE_BYTES},
             "tests": {"type": "object"}, "baseBundleHash": {"type": ["string", "null"]}},
         ["manifest", "source", "tests"], True),
        ("extension.validate", _normalize_validate, _execute_validate, _bind_validate,
         "Run candidate tests and independent host statistics checks in trusted developer mode", {
             "candidateId": {"type": "string"}, "bundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"}},
         ["candidateId", "bundleHash"], True),
        ("extension.activate", _normalize_activate, _execute_activate, _bind_activate,
         "Activate the exact validated immutable extension bundle", {
             "candidateId": {"type": "string"}, "bundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"}},
         ["candidateId", "bundleHash"], True),
        ("extension.rollback", _normalize_rollback, _execute_rollback, _bind_rollback,
         "Point future extension calls at an exact retained immutable version", {
             "extensionId": {"type": "string"}, "expectedActiveBundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
             "targetBundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"}},
         ["extensionId", "expectedActiveBundleHash", "targetBundleHash"], True),
        ("extension.run", _normalize_run, _execute_run, _bind_run,
         "Run the pinned active extension on explicit input, verified reference history, or registered native FE history", {
             "extensionId": {"type": "string"}, "bundleHash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
             "series": {"$ref": SERIES_SCHEMA}, "provenance": {"type": "object"}},
         ["extensionId", "bundleHash"], False),
    ]
    for name, normalize, execute, bind, description, properties, required, write in commands:
        service.register_command(name,
            descriptor={"description": description, "apiVersion": PROTOCOL_VERSION,
                        "kind": "numeric-series-statistics", "requiresApproval": True,
                        "trustedRuntimeRequired": name in ("extension.validate", "extension.run"),
                        "parameters": {"type": "object", "properties": properties,
                                       "required": required, "additionalProperties": False}},
            normalize=normalize, execute=execute, bind=bind, lane="extension",
            registry_write=write)


def bind_command(service, command: str, parameters: dict) -> dict:
    binders = {"extension.stage": _bind_stage, "extension.validate": _bind_validate,
               "extension.activate": _bind_activate, "extension.rollback": _bind_rollback,
               "extension.run": _bind_run}
    if command not in binders:
        _fail("UNKNOWN_COMMAND", "Command is not in the extension capability catalog")
    return binders[command](service, parameters)
