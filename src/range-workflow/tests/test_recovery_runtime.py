"""Cross-process recovery checks against the real workflow service and FE solver."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient, WorkflowError
from range_workflow.service import canonical, file_hash


ARGS = None
_ALLOWED_ENVIRONMENT = {
    "PATH", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "COMSPEC", "PATHEXT",
    "NUMBER_OF_PROCESSORS", "OMP_NUM_THREADS", "QT_PLUGIN_PATH",
}
_RUNTIME_BOOTSTRAP = r'''
import json, os, sys, threading, time
from pathlib import Path

fault = __FAULT_JSON__
arguments = sys.argv[1:]
workspace = Path(arguments[arguments.index("--workspace") + 1])

def mark_and_exit(exit_code, payload):
    temporary = workspace / ".recovery-test-crash.tmp"
    final = workspace / ".recovery-test-crash.json"
    temporary.write_bytes(json.dumps(payload, sort_keys=True).encode("utf-8"))
    os.replace(temporary, final)
    os._exit(exit_code)

if fault:
    if fault["kind"] == "after-progress":
        from range_workflow import recovery
        original = recovery.record_progress
        def record_progress(*args, **kwargs):
            result = original(*args, **kwargs)
            if kwargs.get("variant") == fault["variant"] and kwargs.get("phase") == fault["phase"]:
                mark_and_exit(71, {"kind": fault["kind"], "variant": fault["variant"],
                                   "phase": fault["phase"]})
            return result
        recovery.record_progress = record_progress
    elif fault["kind"] == "checkpoint-watch":
        variant = fault["variant"]
        target = fault["acceptedSeq"]
        def watch_checkpoint():
            jobs = workspace / "jobs"
            while True:
                for pointer_path in jobs.glob("*/" + variant + "/checkpoints/latest.json"):
                    try:
                        pointer = json.loads(pointer_path.read_bytes())
                        envelope_path = pointer_path.parent / pointer["envelopeFile"]
                        envelope = json.loads(envelope_path.read_bytes())
                        accepted = envelope.get("acceptedSeq")
                    except (OSError, ValueError, KeyError, TypeError):
                        continue
                    if type(accepted) is int and accepted >= target:
                        mark_and_exit(72, {"kind": fault["kind"], "variant": variant,
                                           "acceptedSeq": accepted})
                time.sleep(0.0005)
        threading.Thread(target=watch_checkpoint, daemon=True).start()
    elif fault["kind"] == "after-solve":
        from range_workflow.process import ProcessRunner
        original = ProcessRunner.run
        def run(self, argv, directory, label, cancel, timeout=180, env=None):
            result = original(self, argv, directory, label, cancel, timeout, env)
            if label == fault["label"]:
                marker = None
                for pointer_path in (workspace / "jobs").glob("*/" + fault["variant"] + "/checkpoints/latest.json"):
                    try:
                        pointer = json.loads(pointer_path.read_bytes())
                        envelope = json.loads((pointer_path.parent / pointer["envelopeFile"]).read_bytes())
                        marker = {"acceptedSeq": envelope.get("acceptedSeq"),
                                  "checkpointDirectory": str(pointer_path.parent)}
                    except (OSError, ValueError, KeyError, TypeError):
                        pass
                mark_and_exit(73, {"kind": fault["kind"], "label": label, "checkpoint": marker})
            return result
        ProcessRunner.run = run

from range_workflow.runtime import main
raise SystemExit(main(arguments))
'''


def _bootstrap(fault):
    return _RUNTIME_BOOTSTRAP.replace("__FAULT_JSON__", repr(fault))


def _build_runtime_environment():
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() in _ALLOWED_ENVIRONMENT}
    environment["PYTHONPATH"] = str(Path(__file__).resolve().parents[1] / "python")
    environment["PYTHONNOUSERSITE"] = "1"
    return environment


_RUNTIME_ENVIRONMENT_SNAPSHOT = _build_runtime_environment()


def _runtime_environment():
    return dict(_RUNTIME_ENVIRONMENT_SNAPSHOT)


class InjectedRuntimeClient:
    """Run the production stdio runtime in a fresh process with test-only crash hooks."""

    def __init__(self, workspace, fault=None):
        self.workspace = Path(workspace).resolve()
        self._mutex = threading.Lock()
        self._closed = False
        python = Path(ARGS.python or sys.executable).resolve()
        arguments = [str(python), "-c", _bootstrap(fault), "--stdio",
                     "--workspace", str(self.workspace), "--solver", str(ARGS.solver),
                     "--native", str(ARGS.native), "--examples", str(ARGS.examples),
                     "--python", str(python)]
        self.process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=_runtime_environment())
        self._stderr = bytearray()

        def drain_stderr():
            for block in iter(lambda: self.process.stderr.read(4096), b""):
                if len(self._stderr) < 65536:
                    self._stderr.extend(block[:65536 - len(self._stderr)])

        self._error_reader = threading.Thread(target=drain_stderr, daemon=True)
        self._error_reader.start()
        try:
            capabilities = self.request("capabilities.query")
            if capabilities.get("apiVersion") != "range.workflow/v1":
                raise WorkflowError("UNSUPPORTED_VERSION", "Runtime API version differs")
        except BaseException:
            self.close()
            raise

    def _stderr_diagnostic(self):
        self._error_reader.join(timeout=0.5)
        return _sanitize_diagnostic_text(bytes(self._stderr).decode("utf-8", "replace"), limit=2048)

    def request(self, operation, parameters=None):
        with self._mutex:
            if self._closed:
                raise WorkflowError("SERVICE_CLOSED", "Runtime client is closed")
            request_id = str(uuid.uuid4())
            request = {"apiVersion": "range.workflow/v1", "requestId": request_id,
                       "operation": operation, "parameters": parameters or {}}
            try:
                self.process.stdin.write(canonical(request) + b"\n")
                self.process.stdin.flush()
                raw = self.process.stdout.readline(16 * 1024 * 1024 + 1)
            except (OSError, ValueError):
                diagnostic = self._stderr_diagnostic()
                detail = "; stderr: " + diagnostic if diagnostic else ""
                raise WorkflowError("SERVICE_EXITED", "Runtime process exited" + detail) from None
            if not raw:
                diagnostic = self._stderr_diagnostic()
                detail = "; stderr: " + diagnostic if diagnostic else ""
                raise WorkflowError("SERVICE_EXITED", "Runtime process closed its output" + detail)
            try:
                envelope = json.loads(raw)
                if (set(envelope) != {"apiVersion", "requestId", "status", "code", "result"} or
                        envelope["apiVersion"] != "range.workflow/v1" or
                        envelope["requestId"] != request_id or
                        envelope["status"] not in ("ok", "error") or
                        not isinstance(envelope["result"], dict)):
                    raise ValueError("invalid response envelope")
            except (ValueError, TypeError):
                raise WorkflowError("INVALID_RESPONSE", "Runtime returned invalid JSON") from None
            if envelope["status"] == "error":
                raise WorkflowError(envelope["code"], envelope["result"].get("message", "Request failed"))
            return envelope["result"]

    def preview(self, command, parameters):
        revision = self.request("project.query")["revision"]
        return self.request("command.preview", {"command": command, "parameters": parameters,
                                                  "expectedRevision": revision})

    def approve(self, preview):
        return self.request("host.approve", {"previewId": preview["previewId"],
                                              "previewHash": preview["previewHash"]})

    def execute(self, preview, approval):
        return self.request("command.execute", {"previewId": preview["previewId"],
                                                  "approvalToken": approval["approvalToken"]})

    def wait(self, job_id, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            job = self.request("jobs.query", {"jobId": job_id})["job"]
            if job["state"] in {"succeeded", "failed", "cancelled", "interrupted"}:
                return job
            time.sleep(0.025)
        raise AssertionError("Workflow job did not finish before the test timeout")

    def _job_failure_diagnostic(self, job_id, job):
        return _job_failure_diagnostic(self.workspace, job_id, job)

    def wait_for_exit(self, job_id, timeout=180):
        deadline = time.monotonic() + timeout
        while self.process.poll() is None:
            if job_id is not None:
                try:
                    job = self.request("jobs.query", {"jobId": job_id})["job"]
                except WorkflowError as error:
                    if error.code == "SERVICE_EXITED":
                        break
                    raise
                if job.get("state") in {"failed", "cancelled", "succeeded", "interrupted"}:
                    details = self._job_failure_diagnostic(job_id, job)
                    raise AssertionError("Runtime job became terminal before the injected process exit: " + details)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                details = ""
                if job_id is not None:
                    try:
                        job = self.request("jobs.query", {"jobId": job_id})["job"]
                        details = "; job=" + self._job_failure_diagnostic(job_id, job)
                    except WorkflowError:
                        pass
                raise AssertionError("Injected runtime did not exit before timeout" + details)
            time.sleep(min(0.025, remaining))
        return self.process.wait(timeout=max(1, deadline - time.monotonic()))

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.process.stdin.close()
        except OSError:
            pass
        if self.process.poll() is None:
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self._error_reader.join(2)
        for stream in (self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def _job_failure_diagnostic(workspace, job_id, job):
    job_root = (Path(workspace) / "jobs" / job_id).resolve()
    candidates = [job_root]
    attempts = job.get("recovery", {}).get("attempts", [])
    if attempts and isinstance(attempts[-1], dict):
        directory = attempts[-1].get("directory")
        if isinstance(directory, str):
            candidates.insert(0, Path(directory))
    attempt_id = job.get("attemptId")
    if isinstance(attempt_id, str):
        candidates.insert(0, job_root / "attempts" / attempt_id)
    directories = []
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(job_root)
            if resolved.is_dir() and resolved not in directories:
                directories.append(resolved)
        except (OSError, ValueError, RuntimeError):
            continue

    summary = {"jobId": job_id, "state": job.get("state")}
    error = job.get("error")
    if isinstance(error, dict):
        summary["jobError"] = {
            "code": _sanitize_diagnostic_text(str(error.get("code", "")), limit=128),
            "message": _sanitize_diagnostic_text(str(error.get("message", "")), limit=512),
        }
    for directory in directories:
        diagnostic_path = directory / "execution-diagnostics.json"
        try:
            if diagnostic_path.is_symlink() or diagnostic_path.stat().st_size > 65536:
                continue
            diagnostic = json.loads(diagnostic_path.read_bytes())
            frames = diagnostic.get("frames") if isinstance(diagnostic, dict) else None
            if isinstance(frames, list):
                safe_frames = []
                for frame in frames[:32]:
                    if not isinstance(frame, dict):
                        continue
                    filename = frame.get("filename", frame.get("module", ""))
                    function = frame.get("function", "")
                    line = frame.get("line")
                    if not isinstance(filename, str) or not isinstance(function, str) or type(line) is not int:
                        continue
                    safe_frames.append({"filename": Path(filename).name[:128],
                                        "function": function[:128], "line": line})
                summary["internalDiagnostic"] = {
                    "exceptionType": _sanitize_diagnostic_text(
                        str(diagnostic.get("exceptionType", "")), limit=64),
                    "frames": safe_frames,
                }
                break
        except (OSError, ValueError, TypeError, AttributeError):
            continue

    log_tails = []
    log_paths = []
    seen_logs = set()
    for directory in directories:
        try:
            for path in directory.rglob("*.log"):
                if path.is_symlink():
                    continue
                resolved = path.resolve(strict=True)
                resolved.relative_to(job_root)
                if resolved in seen_logs:
                    continue
                seen_logs.add(resolved)
                log_paths.append((path.stat().st_mtime_ns, path, resolved))
        except (OSError, ValueError, RuntimeError):
            continue
    for _, path, resolved in sorted(log_paths, key=lambda item: item[0], reverse=True)[:6]:
        try:
            with resolved.open("rb") as stream:
                stream.seek(max(0, resolved.stat().st_size - 2048))
                tail = stream.read(2048).decode("utf-8", "replace")
            lines = [line for line in tail.splitlines()
                     if not re.search(r"(?i)(apiVersion|requestId|approvalToken|approval[_ -]?token)", line)]
            log_tails.append({"path": path.relative_to(job_root).as_posix(),
                              "tail": _sanitize_diagnostic_text(" | ".join(lines), limit=1024)})
        except (OSError, ValueError):
            continue
    if log_tails:
        summary["logTails"] = log_tails
    return json.dumps(summary, sort_keys=True, separators=(",", ":"))[:8192]


def _sanitize_diagnostic_text(value, *, limit):
    diagnostic = re.sub(
            r"(?i)\b(approval[_ -]?token|(?:openai|api)[_ -]?key|authorization|bearer|password|secret)\b"
            r"(\s*[:=]\s*)[^\s,;]+",
            r"\1\2[redacted]", value)
    diagnostic = " | ".join(line.strip() for line in diagnostic.splitlines() if line.strip())
    return diagnostic[-limit:]


def _checkpoint(directory):
    pointer_path = Path(directory) / "latest.json"
    pointer = json.loads(pointer_path.read_bytes())
    envelope_path = pointer_path.parent / pointer["envelopeFile"]
    envelope = json.loads(envelope_path.read_bytes())
    model = pointer_path.parent / envelope["modelFile"]
    return {"pointerPath": pointer_path, "pointerBytes": pointer_path.read_bytes(),
            "envelopePath": envelope_path, "envelopeBytes": envelope_path.read_bytes(),
            "modelPath": model, "modelBytes": model.read_bytes(), "envelope": envelope}


def _checkpoint_tree_hashes(directory):
    directory = Path(directory)
    return {path.relative_to(directory).as_posix(): file_hash(path)
            for path in directory.rglob("*") if path.is_file()}


def _json_from_runtime_output(output):
    for line in output.splitlines():
        if not line.lstrip().startswith(b"{"):
            continue
        try:
            value = json.loads(line)
        except (ValueError, TypeError):
            continue
        if isinstance(value, dict) and value.get("apiVersion") == "range.workflow.native/v1":
            return value
    raise AssertionError("Native inspector returned no versioned JSON response")


def _inspect(native, model_path, working_directory):
    completed = subprocess.run([str(native), "inspect", "--input", str(model_path)],
                               cwd=working_directory, env=_runtime_environment(),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=60, check=False)
    if completed.returncode != 0:
        raise AssertionError("Native inspection failed: " + completed.stdout[-4000:].decode("utf-8", "replace"))
    return _json_from_runtime_output(completed.stdout)


def _continuous_native(args, workspace, attempt, variant):
    """Run uninterrupted solver from the exact frozen input and controller argv."""
    partial = attempt["partialResult"]
    prepared = Path(partial["preparedInputs"][variant]["path"])
    context = attempt["variants"][variant]["context"]
    attempt_directory = Path(attempt["directory"]).resolve()
    directory = Path(workspace) / "continuous-native" / variant
    directory.mkdir(parents=True, exist_ok=False)
    native_input = directory / (variant + prepared.suffix)
    shutil.copyfile(prepared, native_input)
    checkpoint_directory = directory / "checkpoints"
    checkpoint_directory.mkdir()
    argv = [str(args.solver), "--file=" + str(native_input),
            "--native-checkpoint-dir=" + str(checkpoint_directory),
            "--native-checkpoint-context-json=" + canonical(context).decode("utf-8")]
    if variant == "active":
        from range_workflow.recovery import control_arguments
        controller_arguments = control_arguments(partial)
        argv.extend(["--control-program=" + str(args.python), "--control-timeout-ms=10000",
                     "--control-args-json=" + canonical(controller_arguments).decode("utf-8")])
    # Production ProcessRunner copies the service child's startup environment.
    # Use the exact same snapshot as that child for the uninterrupted oracle.
    environment = _runtime_environment()
    environment["RANGE_WORKFLOW_INPUT_SHA256"] = file_hash(prepared)
    completed = subprocess.run(argv, cwd=attempt_directory, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=120, check=False)
    if completed.returncode != 0:
        raise AssertionError("Continuous native solver failed: " +
                             completed.stdout[-6000:].decode("utf-8", "replace"))
    return _inspect(args.native, native_input, directory)


def _dot(left, matrix, right):
    if len(matrix) != len(left) or any(len(row) != len(right) for row in matrix):
        raise AssertionError("Energy reference matrix dimensions do not match physical DOF vectors")
    return sum(left[row] * matrix[row][column] * right[column]
               for row in range(len(left)) for column in range(len(right)))


def _physical_scales(partial):
    linear = json.loads(Path(partial["linearPath"]).read_bytes())
    reference = json.loads(Path(partial["referencePath"]).read_bytes())
    q0, v0 = reference["initialQ"], reference["initialV"]
    energy_floor = max(abs(_dot(q0, linear["K"], q0) + _dot(v0, linear["M"], v0)), 1e-30)
    dt = partial["dtSeconds"]
    acceleration_floor = max(max((abs(value) / (dt * dt) for value in q0), default=0.0),
                             max((abs(value) / dt for value in v0), default=0.0), 1e-12)
    return linear, energy_floor, acceleration_floor


def _physical_dof_state(partial, inspection):
    """Project accepted nodal vectors into the source-reference DOF ordering."""
    nodes = {node["id"]: node for node in inspection.get("nodes", []) if isinstance(node, dict)}
    q, v = [], []
    for dof in partial["dofs"]:
        node = nodes[dof["nodePosition"]]
        if dof["unit"] == "m":
            q_values, v_values = node["displacement"], node["velocity"]
        elif dof["unit"] == "rad":
            q_values, v_values = node["rotation"], node["angularVelocity"]
        else:
            raise AssertionError("Source reference DOF has an unsupported physical unit")
        direction = dof["globalDirection"]
        q.append(sum(q_values[axis] * direction[axis] for axis in range(3)))
        v.append(sum(v_values[axis] * direction[axis] for axis in range(3)))
    return q, v


def _assert_close_vector(test, actual, expected, *, absolute_tolerance=None, relative_floor=None,
                         relative_tolerance=1e-8):
    test.assertEqual(len(actual), len(expected))
    for index, (left, right) in enumerate(zip(actual, expected)):
        limit = (absolute_tolerance if absolute_tolerance is not None else
                 relative_tolerance * max(abs(right), relative_floor or 0.0))
        test.assertLessEqual(abs(left - right), limit,
                             f"component {index}: {left!r} != {right!r}, tolerance {limit:g}")


def _assert_solver_state_matches(test, actual, expected, partial, variant):
    linear, energy_floor, acceleration_floor = _physical_scales(partial)
    dofs = partial["dofs"]
    for key, tolerance in (("q", 1e-8), ("v", 1e-7)):
        test.assertEqual(len(actual[key]), len(expected[key]))
        for index, (left, right) in enumerate(zip(actual[key], expected[key])):
            test.assertLessEqual(abs(left - right), tolerance,
                                 f"{variant} {key}[{index}] exceeds frozen unit tolerance")
    _assert_close_vector(test, actual["a"], expected["a"], relative_floor=acceleration_floor)
    force_floor = max((abs(sum(linear["K"][row][column] * reference_q
                               for column, reference_q in enumerate(
                                   json.loads(Path(partial["referencePath"]).read_bytes())["initialQ"])))
                       for row in range(len(linear["K"]))), default=0.0)
    force_floor = max(force_floor, 1e-12)
    _assert_close_vector(test, actual.get("controlWork", []), expected.get("controlWork", []),
                         relative_floor=energy_floor)
    _assert_close_vector(test, actual.get("supportReactions", []), expected.get("supportReactions", []),
                         relative_floor=force_floor)
    _assert_close_vector(test, actual.get("supportWork", []), expected.get("supportWork", []),
                         relative_floor=energy_floor)
    test.assertEqual(actual["controlEnabled"], expected["controlEnabled"])
    test.assertEqual(actual["controlFingerprint"], expected["controlFingerprint"])
    if actual["controlEnabled"]:
        a_control, b_control = actual["controlStateComponents"], expected["controlStateComponents"]
        for identity in ("controllerLogicStateSha256", "conditionerConfigurationSha256", "conditionerCompletedSteps"):
            test.assertEqual(a_control[identity], b_control[identity])
        # Independent default-policy conditioner fixture proves identity:
        # no delay/no noise/hold=1. No numerical tolerance is introduced here.
        for state in (actual, expected):
            components = state["controlStateComponents"]
            test.assertEqual(components["conditionerCompletedSteps"], state["step"])
            test.assertEqual(components["conditionerHeldCommands"], state["controlLastCommands"])
            test.assertTrue(all(queue == [] for queue in components["conditionerDelayedCommands"]))
        _assert_close_vector(test, a_control["conditionerHeldCommands"], b_control["conditionerHeldCommands"],
                             relative_floor=force_floor, relative_tolerance=2e-10)
    else:
        test.assertEqual(actual["controlControllerStateSha256"], expected["controlControllerStateSha256"])
    test.assertEqual(actual["meshFingerprint"], expected["meshFingerprint"])
    test.assertEqual(actual["dofFingerprint"], expected["dofFingerprint"])
    test.assertEqual(actual["systemFingerprint"], expected["systemFingerprint"])
    power_floor = max(energy_floor / partial["dtSeconds"], 1e-12)
    test.assertEqual(len(actual["controlPowers"]), len(expected["controlPowers"]))
    _assert_close_vector(test, actual["controlPowers"], expected["controlPowers"], relative_floor=power_floor,
                         relative_tolerance=2e-10)
    _assert_close_vector(test, actual["controlLastCommands"], expected["controlLastCommands"],
                         relative_floor=force_floor, relative_tolerance=2e-10)
    energy_actual = 0.5 * (_dot(actual["q"], linear["K"], actual["q"]) +
                           _dot(actual["v"], linear["M"], actual["v"]))
    energy_expected = 0.5 * (_dot(expected["q"], linear["K"], expected["q"]) +
                             _dot(expected["v"], linear["M"], expected["v"]))
    test.assertLessEqual(abs(energy_actual - energy_expected), 1e-8 * energy_floor)


def _assert_result_history(test, client, job_id, result, variant, target_steps, *,
                           qualification=None, require_legacy=False):
    catalog = client.request("result.catalog.query", {"jobId": job_id, "mode": variant, "limit": 200})
    test.assertTrue(catalog["completed"])
    test.assertEqual(catalog["jobId"], job_id)
    test.assertEqual(catalog["artifactId"], result["nativeResults"][variant]["artifactId"])
    test.assertEqual(catalog["proofScope"], "solver-and-declared-controller-runtime/v1")
    test.assertEqual(catalog["frameCount"], target_steps)
    frames = catalog["frames"]
    test.assertEqual(len(frames), target_steps)
    test.assertEqual([frame["recordNo"] for frame in frames], list(range(1, target_steps + 1)))

    selected = (qualification or {}).get("variants", {}).get(variant, {})
    prefix = selected.get("historyPrefix", [])
    prefix_by_record = {item["recordNo"]: item for item in prefix if isinstance(item, dict)}
    test.assertEqual(len(prefix_by_record), len(prefix))
    legacy_records = set()
    frames_by_record = {frame["recordNo"]: frame for frame in frames}
    for frame in frames:
        kind = frame["originKind"]
        test.assertIn(kind, {"produced", "reused-history", "legacy-history-unattributed"})
        if kind == "produced":
            test.assertEqual(frame["origin"]["jobId"], job_id)
            test.assertTrue(frame["origin"]["inputPackageSha256"])
            test.assertIsNone(frame.get("historyProof"))
        elif kind == "reused-history":
            origin = frame.get("origin")
            if origin is not None:
                test.assertEqual(origin["jobId"], job_id)
                test.assertTrue(origin["inputPackageSha256"])
            proof = frame.get("historyProof")
            test.assertIsInstance(proof, dict)
            for key in ("recordNo", "relativePath", "sha256", "originalFrameSha256", "originKind", "origin"):
                test.assertIn(key, proof)
            test.assertEqual(proof["recordNo"], frame["recordNo"])
            test.assertEqual(proof["sha256"], frame["sha256"])
            test.assertEqual(proof["relativePath"], frame["relativePath"])
            test.assertEqual(proof["origin"], frame["origin"])
        else:
            legacy_records.add(frame["recordNo"])
            expected = prefix_by_record.get(frame["recordNo"])
            test.assertIsNotNone(expected, "Legacy frame has no qualified attempt-ledger proof")
            test.assertIsNone(frame.get("origin"))
            proof = frame.get("historyProof")
            test.assertIsInstance(proof, dict)
            test.assertIsNone(proof.get("origin"))
            test.assertEqual(proof.get("originKind"), "legacy-history-unattributed")
            test.assertEqual(proof.get("ancestryValidationScope"), "copied-restart-input-bytes")
            for key in ("recordNo", "sha256", "originalFrameSha256", "sourceRestartSha256",
                        "sourceAttemptId", "checkpointEvidence"):
                test.assertEqual(proof.get(key), expected.get(key),
                                 f"Legacy frame proof field {key} differs from the qualified ledger")
            test.assertEqual(frame["sha256"], expected["sha256"])
            test.assertEqual(frame["sha256"], expected["originalFrameSha256"],
                             "Legacy history bytes differ from the captured original bytes")
            test.assertEqual(proof.get("relativePath"), expected["relativePath"])
            expected_journal = expected.get("journalEvidence")
            proof_journal = proof.get("journalEvidence")
            if expected_journal is None:
                test.assertIsNone(proof_journal)
            else:
                test.assertIsInstance(proof_journal, dict)
                for key, value in expected_journal.items():
                    test.assertEqual(proof_journal.get(key), value,
                                     f"Legacy journal proof field {key} differs from the qualified ledger")
                test.assertTrue(Path(proof_journal.get("path", "")).is_absolute())
            test.assertEqual(proof["sourceAttemptId"], qualification["attemptId"])
            checkpoint = proof["checkpointEvidence"]
            test.assertEqual(checkpoint["attemptId"], qualification["attemptId"])
            test.assertGreaterEqual(checkpoint["acceptedSeq"], frame["recordNo"])
            for key in ("checkpointSha256", "envelopeSha256"):
                test.assertRegex(checkpoint[key], r"^[0-9a-f]{64}$")
            test.assertNotIn("originManifestPath", proof)
            test.assertNotIn("originManifestSha256", proof)
    if require_legacy:
        test.assertEqual(legacy_records, set(prefix_by_record),
                         "Recovered output omitted frames from the qualified legacy prefix")
        test.assertGreater(len(legacy_records), 0,
                           "Recovery did not classify captured pre-crash frames as legacy history")

    channels = client.request("result.channels.query", {"jobId": job_id, "mode": variant,
                                                         "artifactId": catalog["artifactId"], "limit": 200})
    displacement = next((entry for entry in channels["channels"]
                         if entry.get("quantity") == "displacement"), None)
    test.assertIsNotNone(displacement, "Native broker omitted displacement channels")
    page = client.request("result.series.query", {"jobId": job_id, "mode": variant,
        "artifactId": catalog["artifactId"], "channelId": displacement["channelId"], "limit": 200})
    samples = page["samples"]
    test.assertEqual(len(samples), catalog["frameCount"])
    records = [sample["recordNo"] for sample in samples]
    test.assertEqual(records, sorted(set(records)), "Recovered broker history contains duplicate or unordered samples")
    test.assertEqual(records[-1], target_steps)
    times = [sample["timeS"] for sample in samples]
    test.assertEqual(times, sorted(set(times)), "Recovered broker history contains duplicate or unordered times")
    for sample in samples:
        frame = frames_by_record[sample["recordNo"]]
        test.assertEqual(sample["originKind"], frame["originKind"])
        test.assertEqual(sample.get("origin"), frame.get("origin"))
        if sample["originKind"] == "produced":
            test.assertEqual(sample["origin"]["jobId"], job_id)
            test.assertTrue(sample["origin"]["inputPackageSha256"])
            test.assertIsNone(sample.get("historyProof"))
        else:
            test.assertEqual(sample.get("historyProof"), frame.get("historyProof"))


class RecoveryRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="range-workflow-recovery-runtime-")
        self.addCleanup(self.temporary.cleanup)

    def _workspace(self, name):
        path = Path(self.temporary.name) / name
        path.mkdir()
        return path

    def _start_crashed_run(self, workspace, fault, *, mode="both", steps=24):
        client = InjectedRuntimeClient(workspace, fault)
        self.addCleanup(client.close)
        configure = client.preview("workflow.configure", {"dt": 0.005, "steps": steps,
                    "targetDamping": 0.1, "model": "four-rod"})
        configured = client.wait(client.execute(configure, client.approve(configure))["jobId"])
        self.assertEqual(configured["state"], "succeeded", configured.get("error"))
        original = client.preview("workflow.run", {"mode": mode, "independentReference": True})
        approval = client.approve(original)
        job_id = client.execute(original, approval)["jobId"]
        return client, job_id, original, approval

    @staticmethod
    def _attempt(job):
        attempts = job.get("recovery", {}).get("attempts", [])
        if not attempts:
            raise AssertionError("Interrupted job has no durable attempt record")
        return attempts[-1]

    def _recover(self, client, workspace, job_id, old_preview, old_approval, *, expected_variant_phases=None):
        with self.assertRaises(WorkflowError) as stale:
            client.execute(old_preview, old_approval)
        self.assertEqual(stale.exception.code, "STALE_REVISION")
        qualification = client.request("job.recovery.query", {"jobId": job_id})
        if qualification["eligibility"] != "recoverable":
            job = client.request("jobs.query", {"jobId": job_id})["job"]
            qualification_summary = _sanitize_diagnostic_text(
                json.dumps(qualification, sort_keys=True, separators=(",", ":")), limit=4096)
            attempt_diagnostic = _job_failure_diagnostic(workspace, job_id, job)
            raise AssertionError("Recovery qualification was not recoverable: " + qualification_summary +
                                 "; attempt diagnostics: " + attempt_diagnostic)
        for variant, phase in (expected_variant_phases or {}).items():
            self.assertEqual(qualification["variants"][variant]["phase"], phase)
        parameters = {"jobId": job_id, "qualificationSha256": qualification["qualificationSha256"]}
        preview = client.preview("workflow.recover", parameters)
        approval = client.approve(preview)
        accepted = client.execute(preview, approval)
        self.assertEqual(accepted["jobId"], job_id, "Recovery must retain the logical job identity")
        completed = client.wait(job_id)
        if completed["state"] != "succeeded":
            job = client.request("jobs.query", {"jobId": job_id})["job"]
            raise AssertionError("Recovered job did not succeed: " + _job_failure_diagnostic(workspace, job_id, job))
        job = client.request("jobs.query", {"jobId": job_id})["job"]
        attempts = job["recovery"]["attempts"]
        self.assertEqual(len(attempts), 2)
        self.assertEqual(attempts[0]["attemptId"], job_id)
        self.assertNotEqual(attempts[1]["attemptId"], attempts[0]["attemptId"])
        self.assertEqual(job["attemptId"], attempts[1]["attemptId"])
        return completed, qualification

    def _continuous(self, workspace, attempt, variants):
        return {variant: _continuous_native(ARGS, workspace, attempt, variant) for variant in variants}

    def _assert_recovered_states(self, client, recovered, baseline, partial, variants, qualification,
                                 *, require_legacy=()):
        for variant in variants:
            actual = _inspect(ARGS.native, Path(recovered["result"][variant]["checkpointPath"]),
                              Path(recovered["result"][variant]["checkpointPath"]).parent)
            _assert_solver_state_matches(self, actual, baseline[variant], partial, variant)
            linear, energy_floor, _ = _physical_scales(partial)
            baseline_q, baseline_v = _physical_dof_state(partial, baseline[variant])
            recovered_q, recovered_v = recovered["result"][variant]["q"], recovered["result"][variant]["v"]
            for index, (actual_value, expected_value) in enumerate(zip(recovered_q, baseline_q)):
                self.assertLessEqual(abs(actual_value - expected_value), 1e-8,
                                     f"{variant} physical q[{index}] exceeds the frozen tolerance")
            for index, (actual_value, expected_value) in enumerate(zip(recovered_v, baseline_v)):
                self.assertLessEqual(abs(actual_value - expected_value), 1e-7,
                                     f"{variant} physical v[{index}] exceeds the frozen tolerance")
            expected_energy = 0.5 * (_dot(baseline_q, linear["K"], baseline_q) +
                                     _dot(baseline_v, linear["M"], baseline_v))
            self.assertLessEqual(abs(recovered["result"][variant]["energyJ"] - expected_energy),
                                 1e-8 * energy_floor)
            _assert_result_history(self, client, recovered["jobId"], recovered["result"], variant,
                                   recovered["result"]["totalSteps"], qualification=qualification,
                                   require_legacy=variant in require_legacy)

    def test_workspace_lock_rejects_second_live_service(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual native recovery tests")
        workspace = self._workspace("lock")
        first = InjectedRuntimeClient(workspace)
        try:
            with self.assertRaises(WorkflowError):
                WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)
        finally:
            first.close()

    def test_active_complete_passive_prepared_survives_runtime_exit_and_recovery(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual native recovery tests")
        workspace = self._workspace("active-complete")
        client, job_id, original, approval = self._start_crashed_run(workspace,
            {"kind": "after-progress", "variant": "active", "phase": "completed"}, steps=14)
        self.assertEqual(client.wait_for_exit(job_id), 71)
        marker = json.loads((workspace / ".recovery-test-crash.json").read_bytes())
        self.assertEqual(marker["phase"], "completed")
        with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
            interrupted = reopened.request("jobs.query", {"jobId": job_id})["job"]
            self.assertEqual(interrupted["state"], "interrupted")
            original_attempt = self._attempt(interrupted)
            self.assertEqual(original_attempt["variants"]["active"]["phase"], "completed")
            self.assertEqual(original_attempt["variants"]["passive"]["phase"], "prepared")
            self.assertFalse((Path(original_attempt["directory"]) / "passive" / "solve-passive.log").exists())
            saved_checkpoints = {name: _checkpoint_tree_hashes(value["checkpointDirectory"])
                for name, value in original_attempt["variants"].items()
                if value.get("checkpointDirectory") and Path(value["checkpointDirectory"]).exists()}
            baseline = self._continuous(workspace, original_attempt, ("active", "passive"))
            recovered, qualification = self._recover(reopened, workspace, job_id, original, approval,
                expected_variant_phases={"active": "completed", "passive": "prepared"})
            self._assert_recovered_states(reopened, recovered, baseline,
                original_attempt["partialResult"], ("active", "passive"), qualification)
            current = reopened.request("jobs.query", {"jobId": job_id})["job"]
            attempts = current["recovery"]["attempts"]
            self.assertEqual(len(attempts), 2)
            self.assertEqual(attempts[0]["attemptId"], job_id)
            self.assertNotEqual(attempts[1]["attemptId"], attempts[0]["attemptId"])
            self.assertEqual(current["attemptId"], attempts[1]["attemptId"])
            self.assertEqual(current["state"], "succeeded")
            for name, expected_hashes in saved_checkpoints.items():
                self.assertEqual(_checkpoint_tree_hashes(original_attempt["variants"][name]["checkpointDirectory"]),
                                 expected_hashes, "Recovery rewrote an original accepted checkpoint bundle")
            self.assertEqual(qualification["attemptId"], attempts[0]["attemptId"])

    def test_both_mode_passive_recovery_uses_seq10_checkpoint_and_preserves_frame_proof(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual native recovery tests")
        workspace = self._workspace("passive-partial")
        client, job_id, original, approval = self._start_crashed_run(workspace,
            {"kind": "checkpoint-watch", "variant": "passive", "acceptedSeq": 10}, steps=24)
        self.assertEqual(client.wait_for_exit(job_id), 72)
        marker = json.loads((workspace / ".recovery-test-crash.json").read_bytes())
        self.assertEqual(marker["acceptedSeq"], 10, "Crash hook missed the first durable accepted checkpoint")
        with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
            interrupted = reopened.request("jobs.query", {"jobId": job_id})["job"]
            self.assertEqual(interrupted["state"], "interrupted")
            original_attempt = self._attempt(interrupted)
            bundle = _checkpoint(original_attempt["variants"]["passive"]["checkpointDirectory"])
            before_hashes = _checkpoint_tree_hashes(original_attempt["variants"]["passive"]["checkpointDirectory"])
            baseline = self._continuous(workspace, original_attempt, ("active", "passive"))

            # The public query exposes hashes and policy only; workspace files are fixture evidence.
            first = reopened.request("job.recovery.query", {"jobId": job_id})
            self.assertEqual(first["eligibility"], "recoverable", first)
            selected = first["variants"]["passive"]
            self.assertEqual(selected["acceptedSeq"], 10)
            self.assertEqual(selected["remainingSteps"], 14)
            self.assertEqual(selected["replayRange"], {"afterAcceptedSeq": 10,
                             "maxAcceptedSteps": 10, "maxSimulationSeconds": 1.0})

            # Corrupt and restore each piece independently; qualification must fail closed.
            bundle["pointerPath"].write_bytes(b"{")
            self.assertEqual(reopened.request("job.recovery.query", {"jobId": job_id})["eligibility"], "conflict")
            bundle["pointerPath"].write_bytes(bundle["pointerBytes"])
            bundle["envelopePath"].write_bytes(b"{")
            pointer = json.loads(bundle["pointerBytes"])
            pointer["envelopeSha256"] = hashlib.sha256(b"{").hexdigest()
            bundle["pointerPath"].write_bytes(canonical(pointer))
            self.assertEqual(reopened.request("job.recovery.query", {"jobId": job_id})["eligibility"], "conflict")
            bundle["envelopePath"].write_bytes(bundle["envelopeBytes"])
            bundle["pointerPath"].write_bytes(bundle["pointerBytes"])
            bundle["modelPath"].write_bytes(bundle["modelBytes"] + b"\x00")
            self.assertEqual(reopened.request("job.recovery.query", {"jobId": job_id})["eligibility"], "conflict")
            bundle["modelPath"].write_bytes(bundle["modelBytes"])

            source_artifact = original_attempt["partialResult"]["sourceArtifacts"]["controller"]
            source_path = Path(source_artifact["path"])
            source_bytes = source_path.read_bytes()
            source_path.write_bytes(source_bytes + b"\nchanged during recovery test")
            self.assertEqual(reopened.request("job.recovery.query", {"jobId": job_id})["eligibility"], "conflict")
            source_path.write_bytes(source_bytes)
            qualification = reopened.request("job.recovery.query", {"jobId": job_id})
            self.assertEqual(qualification["eligibility"], "recoverable", qualification)

            # A second process cannot take the live workspace lock.
            with self.assertRaises(WorkflowError):
                WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python)

            recovered, accepted_qualification = self._recover(reopened, workspace, job_id, original, approval,
                expected_variant_phases={"active": "completed", "passive": "checkpointed"})
            self.assertEqual(accepted_qualification["variants"]["passive"]["acceptedSeq"], 10)
            self._assert_recovered_states(reopened, recovered, baseline,
                original_attempt["partialResult"], ("active", "passive"), accepted_qualification,
                require_legacy=("passive",))
            self.assertEqual(_checkpoint_tree_hashes(original_attempt["variants"]["passive"]["checkpointDirectory"]),
                             before_hashes, "Recovery overwrote the only original accepted checkpoint")
            current = reopened.request("jobs.query", {"jobId": job_id})["job"]
            self.assertEqual(current["recovery"]["attempts"][0]["variants"]["passive"]["phase"], "running")
            self.assertEqual(len(current["recovery"]["attempts"]), 2)

    def test_single_active_recovery_uses_seq10_checkpoint(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual native recovery tests")
        workspace = self._workspace("single-active")
        client, job_id, original, approval = self._start_crashed_run(workspace,
            {"kind": "checkpoint-watch", "variant": "active", "acceptedSeq": 10},
            mode="active", steps=24)
        self.assertEqual(client.wait_for_exit(job_id), 72)
        marker = json.loads((workspace / ".recovery-test-crash.json").read_bytes())
        self.assertEqual(marker["acceptedSeq"], 10, "Crash hook missed the first durable accepted checkpoint")
        with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
            interrupted = reopened.request("jobs.query", {"jobId": job_id})["job"]
            original_attempt = self._attempt(interrupted)
            baseline = self._continuous(workspace, original_attempt, ("active",))
            recovered, qualification = self._recover(reopened, workspace, job_id, original, approval,
                expected_variant_phases={"active": "checkpointed"})
            self.assertEqual(qualification["variants"]["active"]["acceptedSeq"], 10)
            self.assertEqual(qualification["variants"]["active"]["remainingSteps"], 14)
            self._assert_recovered_states(reopened, recovered, baseline,
                original_attempt["partialResult"], ("active",), qualification, require_legacy=("active",))
            self.assertEqual(len(reopened.request("jobs.query", {"jobId": job_id})["job"]["recovery"]["attempts"]), 2)

    def test_target_checkpoint_crash_is_reconciled_without_recompute(self):
        if not ARGS or not ARGS.solver:
            self.skipTest("Pass --solver --native --examples --python for actual native recovery tests")
        workspace = self._workspace("target-checkpoint")
        client, job_id, original, approval = self._start_crashed_run(workspace,
            {"kind": "after-solve", "variant": "active", "label": "solve-active"},
            mode="active", steps=10)
        self.assertEqual(client.wait_for_exit(job_id), 73)
        marker = json.loads((workspace / ".recovery-test-crash.json").read_bytes())
        self.assertEqual(marker["checkpoint"]["acceptedSeq"], 10,
                         "Target MODEL checkpoint was not durable before terminal accounting")
        with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
            interrupted = reopened.request("jobs.query", {"jobId": job_id})["job"]
            original_attempt = self._attempt(interrupted)
            checkpoint_hashes = _checkpoint_tree_hashes(original_attempt["variants"]["active"]["checkpointDirectory"])
            baseline = self._continuous(workspace, original_attempt, ("active",))
            recovered, qualification = self._recover(reopened, workspace, job_id, original, approval,
                expected_variant_phases={"active": "checkpointed"})
            self.assertEqual(qualification["variants"]["active"]["acceptedSeq"], 10)
            self.assertEqual(qualification["variants"]["active"]["remainingSteps"], 0)
            self._assert_recovered_states(reopened, recovered, baseline,
                original_attempt["partialResult"], ("active",), qualification, require_legacy=("active",))
            self.assertEqual(_checkpoint_tree_hashes(original_attempt["variants"]["active"]["checkpointDirectory"]),
                             checkpoint_hashes)
            completed_attempt = reopened.request("jobs.query", {"jobId": job_id})["job"]["recovery"]["attempts"][-1]
            solve_logs = list(Path(completed_attempt["directory"]).glob("solve-active.log"))
            self.assertEqual(solve_logs, [], "Zero-step target-checkpoint recovery reran the native solver")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    ARGS, remaining = parser.parse_known_args()
    if ARGS.solver and not all((ARGS.native, ARGS.examples)):
        parser.error("--solver requires --native and --examples")
    unittest.main(argv=[sys.argv[0], *remaining])
