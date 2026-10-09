"""File-owner SDK and registered workspace adapter for real native MODEL frames."""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
from contextlib import contextmanager

from .process import _WindowsJob
from .service import WorkflowError, canonical, digest, exact, file_hash, identifier

_query_context = threading.local()


@contextmanager
def query_cancellation(event):
    previous = getattr(_query_context, "cancel", None)
    _query_context.cancel = event
    try:
        yield
    finally:
        _query_context.cancel = previous


def _run_query_process(argv, output, errors, timeout):
    """Own the reader's process tree even if the hosting service is closed."""
    flags = {"creationflags": 0x4 | 0x08000000} if os.name == "nt" else {"start_new_session": True}
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=output,
                               stderr=errors, shell=False, **flags)
    job = None
    try:
        if os.name == "nt":
            # Assignment and resume happen in _WindowsJob, before any child
            # code runs. Its non-inheritable handle dies with this host.
            job = _WindowsJob(process)
        cancel = getattr(_query_context, "cancel", None)
        if cancel is None:
            return subprocess.CompletedProcess(argv, process.wait(timeout=timeout))
        deadline = time.monotonic() + timeout
        while True:
            if cancel.is_set():
                raise WorkflowError("READ_CANCELLED", "Native result query was cancelled")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(argv, timeout)
            try:
                return subprocess.CompletedProcess(argv, process.wait(timeout=min(.05, remaining)))
            except subprocess.TimeoutExpired:
                pass
    finally:
        if job:
            job.close()
        elif os.name != "nt":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if process.poll() is None:
            process.kill()
        process.wait()


class NativeResultsClient:
    """Read a frozen manifest through the same broker used by the GUI.

    This file-scoped SDK does not grant access to a GUI Session or certify a
    caller's job as successful. The workspace adapter below checks that ledger.
    """
    def __init__(self, executable, manifest, expected_sha, *, timeout=30):
        self.executable, self.manifest = Path(executable), Path(manifest)
        self.expected_sha, self.timeout = expected_sha, timeout
        if not self.executable.is_absolute() or not self.executable.is_file():
            raise WorkflowError("INVALID_PATH", "An explicit native results executable is required")
        if (not self.manifest.is_absolute() or self.manifest.is_symlink()
                or not self.manifest.is_file()):
            raise WorkflowError("ARTIFACT_PATH", "A regular absolute manifest file is required")

    def query(self, operation, arguments=None):
        if operation not in ("catalog", "channels", "series", "summary", "snapshot"):
            raise WorkflowError("UNKNOWN_OPERATION", "Unsupported native result query")
        arguments = {} if arguments is None else arguments
        if not isinstance(arguments, dict) or len(canonical(arguments)) > 65536:
            raise WorkflowError("INVALID_PARAMETERS", "Native result arguments must be a bounded object")
        try:
            if file_hash(self.manifest) != self.expected_sha:
                raise WorkflowError("SOURCE_CHANGED", "Registered native manifest changed")
            # Broker pages have a fixed point limit. Spool rather than buffering
            # unbounded child output in memory, then enforce the response limit.
            import tempfile
            with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
                result = _run_query_process([str(self.executable), "--manifest", str(self.manifest),
                    "--expected-sha", self.expected_sha, "--operation", operation,
                    "--parameters", canonical(arguments).decode("utf-8")],
                    output, errors, self.timeout)
                output.seek(0)
                response_limit = 32 * 1024 * 1024 if operation == "snapshot" else 2 * 1024 * 1024
                raw = output.read(response_limit + 1)
            if len(raw) > response_limit:
                raise WorkflowError("RESULT_TOO_LARGE", "Native broker response exceeds its byte limit")
            def unique_object(pairs):
                result = {}
                for key, item in pairs:
                    if key in result:
                        raise ValueError("duplicate JSON key")
                    result[key] = item
                return result
            value = json.loads(raw, object_pairs_hook=unique_object,
                               parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
            if not isinstance(value, dict):
                raise ValueError()
            canonical(value)
            if result.returncode != 0 or value.get("valid") is not True:
                raise WorkflowError(value.get("code", "NATIVE_RESULT_FAILED"),
                                    value.get("message", "Native result query failed"))
            if file_hash(self.manifest) != self.expected_sha:
                raise WorkflowError("SOURCE_CHANGED", "Native manifest changed during query")
            return value
        except WorkflowError:
            raise
        except (OSError, subprocess.TimeoutExpired):
            raise WorkflowError("NATIVE_RESULT_FAILED", "Native results executable is unavailable or timed out") from None
        except (ValueError, TypeError):
            raise WorkflowError("INVALID_RESPONSE", "Native broker returned an invalid response") from None

    def series(self, channel_id, **page):
        return self.query("series", {"channelId": channel_id, **page})


def query_registered(service, operation, parameters):
    optional = {"mode", "artifactId", "channelId", "nodePhysicalId", "offset", "limit", "cursor"}
    if operation == "snapshot":
        optional.add("channelIds")
    exact(parameters, {"jobId"}, optional)
    job_id = identifier(parameters["jobId"], "jobId")
    mode = parameters.get("mode", "active")
    if mode not in ("active", "passive"):
        raise WorkflowError("INVALID_PARAMETERS", "Native result mode must be active or passive")
    job = service._job(job_id)
    if job["state"] != "succeeded":
        raise WorkflowError("RESULT_UNAVAILABLE", "Only successful registered workspace jobs have immutable native results")
    descriptor = job.get("result", {}).get("nativeResults", {}).get(mode)
    if not isinstance(descriptor, dict):
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE", "This job has no registered native result manifest for the requested mode")
    path = Path(descriptor.get("manifestPath", ""))
    root = service.workspace / "jobs" / job_id
    if (not path.is_absolute() or not path.resolve().is_relative_to(root.resolve())
            or any(parent.is_symlink() for parent in [path, *path.parents] if parent.is_relative_to(root))):
        raise WorkflowError("ARTIFACT_PATH", "Native manifest must remain inside its registered job")
    artifacts = [a for a in job.get("artifacts", []) if Path(a.get("path", "")) == path]
    if len(artifacts) != 1 or artifacts[0]["sha256"] != descriptor.get("manifestSha256"):
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Native manifest is not bound to the job artifact ledger")
    if parameters.get("artifactId", descriptor["artifactId"]) != descriptor["artifactId"]:
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Requested native artifact does not belong to this job/mode")
    if service.native_results is None:
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE", "Native results executable is not configured")
    arguments = {k: v for k, v in parameters.items() if k not in {"jobId", "mode", "artifactId"}}
    client = NativeResultsClient(service.native_results, path, descriptor["manifestSha256"])
    # Snapshot already returns the hash-validated artifact/job/source identity.
    # Reading a separate catalog repeats every MODEL hash and ancestry check.
    # Verify that same registered identity directly on the complete snapshot.
    if operation == "snapshot":
        result = catalog = client.query(operation, arguments)
    else:
        catalog = client.query("catalog", {"limit": 1})
        result = catalog if operation == "catalog" and arguments == {"limit": 1} else None
    if catalog.get("artifactId") != descriptor["artifactId"] or catalog.get("jobId") != job_id:
        raise WorkflowError("ARTIFACT_IDENTITY_MISMATCH", "Native catalog identity does not match the registered job")
    if result is None:
        result = client.query(operation, arguments)
    result.update(jobId=job_id, mode=mode, completed=True, manifestSha256=descriptor["manifestSha256"],
                  sourceKind=catalog.get("sourceKind"), sourceSnapshotRef=catalog.get("sourceSnapshotRef"))
    return result


def resolve_series(service, provenance):
    exact(provenance, {"jobId", "mode", "artifactId", "channelId"})
    samples, cursor, unit = [], None, None
    while True:
        page = query_registered(service, "series", {**provenance, "limit": 200,
                                                    **({"cursor": cursor} if cursor else {})})
        if unit is not None and unit != page.get("unit"):
            raise WorkflowError("SOURCE_CHANGED", "Native channel unit changed between pages")
        unit = page.get("unit")
        samples.extend(page["samples"])
        if len(samples) > 100000:
            raise WorkflowError("EXTENSION_INPUT_LIMIT", "Native series exceeds the extension point limit")
        next_cursor = page.get("nextCursor")
        if not next_cursor:
            break
        if next_cursor == cursor:
            raise WorkflowError("INVALID_RESPONSE", "Native series cursor failed to advance")
        cursor = next_cursor
    origin_counts = {}
    for sample in samples:
        kind = sample.get("originKind", "unavailable")
        origin_counts[kind] = origin_counts.get(kind, 0) + 1
    origins = [{"recordNo": s["recordNo"], "originKind": s.get("originKind"),
                "origin": s.get("origin"), "historyProof": s.get("historyProof")} for s in samples]
    return {"times": [s["timeS"] for s in samples], "values": [s["value"] for s in samples],
            "timeUnit": "s", "valueUnit": unit}, {**provenance, "sourceKind": page["sourceKind"],
                "artifactSha256": page["manifestSha256"], "sourceSnapshotRef": page["sourceSnapshotRef"],
                "frameOriginSha256": digest(origins), "frameOriginCounts": origin_counts,
                "sampleCount": len(samples), "sampleScope": "actual-published-native-frames"}


def compare_native_series(left, right):
    """Compare two complete SDK histories only after checking physical identity.

    No interpolation, coordinate conversion, or reference-to-native relabeling
    takes place. File-owner callers remain responsible for where DTOs came from.
    """
    import math
    for history in (left, right):
        if (not isinstance(history, dict) or history.get("coverageComplete") is not True
                or history.get("completed") is not True
                or history.get("sourceKind") not in ("native-active-fe", "native-passive-fe", "native-fe")
                or not isinstance(history.get("samples"), list) or not history["samples"]):
            raise WorkflowError("INCOMPARABLE_RESULTS", "Two complete native histories are required")
    def source_hash(history):
        ref = history.get("sourceSnapshotRef", {})
        return ref.get("modelSha256") or ref.get("sourceSha256")
    sha = source_hash(left)
    if not sha or sha != source_hash(right):
        raise WorkflowError("INCOMPARABLE_RESULTS", "Native histories use different model snapshots")
    for field in ("channelId", "unit", "axis", "nodePhysicalId", "variableId"):
        if field not in left or left[field] != right.get(field):
            raise WorkflowError("INCOMPARABLE_RESULTS", "Native channel identities, units, or global directions differ")
    if len(left["samples"]) != len(right["samples"]):
        raise WorkflowError("INCOMPARABLE_RESULTS", "Native histories have different actual time coverage")
    differences = []
    previous = -math.inf
    for a, b in zip(left["samples"], right["samples"]):
        if a.get("timeS") != b.get("timeS") or a.get("recordNo") != b.get("recordNo"):
            raise WorkflowError("INCOMPARABLE_RESULTS", "Native histories have different actual time grids")
        try:
            time, av, bv = (float(a["timeS"]), float(a["value"]), float(b["value"]))
            if (not all(math.isfinite(v) for v in (time, av, bv)) or time <= previous
                    or any(isinstance(v, bool) for v in (a["timeS"], a["value"], b["value"]))):
                raise ValueError()
            delta = av - bv
            if not math.isfinite(delta):
                raise ValueError()
        except (KeyError, ValueError, TypeError, OverflowError):
            raise WorkflowError("INCOMPARABLE_RESULTS", "Native histories contain invalid numerical samples") from None
        differences.append({"recordNo": a["recordNo"], "timeS": time, "value": delta})
        previous = time
    return {"sourceKind": "native-comparison", "sourceSnapshotSha256": sha, "channelId": left["channelId"],
            "unit": left["unit"], "axis": left["axis"], "samples": differences,
            "maxAbsoluteDifference": max(abs(p["value"]) for p in differences),
            "leftArtifactId": left["artifactId"], "rightArtifactId": right["artifactId"],
            "comparisonScope": "same-model-channel-unit-and-actual-time-grid-no-interpolation"}
