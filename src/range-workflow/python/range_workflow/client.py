"""Versioned process SDK. approve() belongs to an explicitly authorized host."""
from __future__ import annotations

import json
from collections import OrderedDict
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import uuid

from .service import API_VERSION, MAX_REQUEST_BYTES, WorkflowError, canonical

_READ_OPERATIONS = frozenset({"result.plot.sources.query", "result.plot.query", "result.raw.query",
    "result.raw.export", "result.raw.export.chunk", "extension.result.query"})
_MAX_INFLIGHT = 16
_MAX_RETIRED = 128
_MAX_RESPONSE_BYTES = 16 * 1024 * 1024


class _Pending:
    def __init__(self, operation, cancel_target=None):
        self.operation = operation
        self.cancel_target = cancel_target
        self.responses = queue.Queue(maxsize=1)
        self.timer = None


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate response key")
        result[key] = value
    return result


class WorkflowClient:
    def __init__(self, workspace, solver, native, examples, python=sys.executable, *, timeout=30,
                 trusted_extensions=False, native_results=None):
        self.timeout = timeout
        self._mutex = threading.Lock()
        self._send_mutex = threading.Lock()
        self._close_mutex = threading.Lock()
        self._pending = {}
        self._retired = OrderedDict()
        self._cancel_outcomes = OrderedDict()
        self._closed = False
        self._disposed = False
        allowed_environment = {"PATH", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "COMSPEC",
                               "PATHEXT", "NUMBER_OF_PROCESSORS", "OMP_NUM_THREADS", "QT_PLUGIN_PATH"}
        environment = {key: value for key, value in os.environ.items() if key.upper() in allowed_environment}
        package_root = str(Path(__file__).resolve().parent.parent)
        environment["PYTHONPATH"] = package_root
        environment["PYTHONNOUSERSITE"] = "1"
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        arguments = [str(python), "-m", "range_workflow", "--stdio",
                    "--workspace", str(Path(workspace).absolute()), "--solver", str(solver),
                    "--native", str(native), "--examples", str(examples), "--python", str(python)]
        if Path(python).with_name("python313._pth").is_file():
            bootstrap = "import sys,runpy; sys.path.insert(0,sys.argv.pop(1)); runpy.run_module('range_workflow',run_name='__main__')"
            arguments = [str(python), "-I", "-B", "-c", bootstrap, package_root, *arguments[3:]]
        if trusted_extensions:
            arguments.append("--trusted-extensions")
        if native_results is not None:
            arguments.extend(["--native-results", str(native_results)])
        self.process = subprocess.Popen(arguments,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=environment, creationflags=0x08000000 if os.name == "nt" else 0)
        self._stderr = bytearray()
        def read_response():
            try:
                while True:
                    line = self.process.stdout.readline(_MAX_RESPONSE_BYTES + 1)
                    if not line:
                        break
                    if len(line) > _MAX_RESPONSE_BYTES:
                        self._fail_transport("RESPONSE_TOO_LARGE", "Runtime response exceeds SDK limit")
                        return
                    if not self._route_response(line):
                        return
            except (OSError, ValueError):
                self._fail_transport("SERVICE_EXITED", "Cannot read from the workflow service")
            finally:
                self._fail_transport("SERVICE_EXITED", "Workflow service closed its output stream")
        def read_error():
            for block in iter(lambda: self.process.stderr.read(4096), b""):
                room = max(0, 65536 - len(self._stderr))
                self._stderr.extend(block[:room])
        self._reader = threading.Thread(target=read_response, daemon=True)
        self._error_reader = threading.Thread(target=read_error, daemon=True)
        self._reader.start()
        self._error_reader.start()
        try:
            capabilities = self.request("capabilities.query")
            if capabilities.get("apiVersion") != API_VERSION:
                raise WorkflowError("UNSUPPORTED_VERSION", "Runtime uses an incompatible protocol")
        except BaseException:
            self.close()
            raise

    def request(self, operation, parameters=None):
        with self._mutex:
            if self._closed:
                raise WorkflowError("SERVICE_CLOSED", "SDK client is closed")
            if len(self._pending) >= _MAX_INFLIGHT:
                raise WorkflowError("SERVICE_BUSY", "SDK in-flight request limit was reached")
            request_id = str(uuid.uuid4())
            raw = canonical({"apiVersion": API_VERSION, "requestId": request_id,
                             "operation": operation, "parameters": parameters if parameters is not None else {}}) + b"\n"
            if len(raw) > MAX_REQUEST_BYTES:
                raise WorkflowError("REQUEST_TOO_LARGE", "Request exceeds protocol size limit")
            pending = _Pending(operation)
            request_timeout = self.timeout
            self._pending[request_id] = pending
        self._send(request_id, pending, raw)
        try:
            response = pending.responses.get(timeout=request_timeout)
        except queue.Empty:
            transport_aborted = False
            cancellation = None
            with self._mutex:
                # A reply can arrive between Queue.get's deadline and this lock.
                if request_id not in self._pending:
                    response = pending.responses.get_nowait()
                elif operation in _READ_OPERATIONS:
                    self._pending.pop(request_id)
                    self._remember(self._retired, request_id, pending)
                    cancellation = self._cancel_read_locked(request_id)
                    response = WorkflowError("SDK_TIMEOUT", "Workflow read request timed out; cancellation requested")
                else:
                    self._abort_locked("SDK_TIMEOUT", "Workflow request timed out")
                    transport_aborted = True
                    response = pending.responses.get_nowait()
            if cancellation is not None:
                self._send_cancel(*cancellation)
            if transport_aborted:
                self.close()
        if isinstance(response, Exception):
            raise response
        if response["status"] == "error":
            raise WorkflowError(response["code"], response["result"].get("message", "Workflow operation failed"))
        return response["result"]

    @staticmethod
    def _remember(ledger, request_id, value):
        ledger[request_id] = value
        while len(ledger) > _MAX_RETIRED:
            ledger.popitem(last=False)

    def _cancel_read_locked(self, target):
        request_id = str(uuid.uuid4())
        pending = _Pending("host.read.cancel", target)
        self._pending[request_id] = pending
        raw = canonical({"apiVersion": API_VERSION, "requestId": request_id,
            "operation": pending.operation, "parameters": {"requestId": target}}) + b"\n"
        return request_id, pending, raw, self.timeout

    def _send(self, request_id, pending, raw):
        # Pipe writes can wait for the peer to drain its input. The reader must
        # still be able to route output while this separate send lock is held.
        with self._send_mutex:
            with self._mutex:
                if self._closed or self._pending.get(request_id) is not pending:
                    return False
            try:
                self.process.stdin.write(raw)
                self.process.stdin.flush()
            except (OSError, ValueError):
                self._fail_transport("SERVICE_EXITED", "Cannot write to the workflow service")
                return False
        return True

    def _send_cancel(self, request_id, pending, raw, timeout):
        if not self._send(request_id, pending, raw):
            return
        def expire():
            with self._mutex:
                if self._pending.get(request_id) is pending:
                    self._pending.pop(request_id)
                    self._remember(self._retired, request_id, pending)
                    self._remember(self._cancel_outcomes, pending.cancel_target, "SDK_TIMEOUT")
        with self._mutex:
            # A fast ACK or a concurrent close can consume the context during
            # the write. Only the still-current context may acquire a timer.
            if self._pending.get(request_id) is pending:
                pending.timer = threading.Timer(timeout, expire)
                pending.timer.daemon = True
                pending.timer.start()

    def _route_response(self, line):
        try:
            envelope = json.loads(line.decode("utf-8", "strict"), object_pairs_hook=_unique_object)
            if (not isinstance(envelope, dict) or
                    set(envelope) != {"apiVersion", "requestId", "status", "code", "result"} or
                    envelope["apiVersion"] != API_VERSION or
                    not isinstance(envelope["requestId"], str) or
                    str(uuid.UUID(envelope["requestId"])) != envelope["requestId"] or
                    envelope["status"] not in ("ok", "error") or
                    not isinstance(envelope["code"], str) or not envelope["code"] or
                    (envelope["status"] == "ok" and envelope["code"] != "OK") or
                    (envelope["status"] == "error" and envelope["code"] == "OK") or
                    not isinstance(envelope["result"], dict)):
                raise ValueError("invalid response envelope")
            if envelope["status"] == "error" and (set(envelope["result"]) != {"message"} or
                    not isinstance(envelope["result"]["message"], str)):
                raise ValueError("invalid error result")
            canonical(envelope)
        except (UnicodeError, ValueError, TypeError, AttributeError, RecursionError):
            self._fail_transport("INVALID_RESPONSE", "Runtime returned an invalid response envelope")
            return False
        request_id = envelope["requestId"]
        with self._mutex:
            if self._closed:
                return False
            pending = self._pending.get(request_id)
            late = False
            if pending is None:
                # Only a timed-out fixed read or its sealed cancellation may reply late.
                pending = self._retired.get(request_id)
                late = True
                if pending is None or (pending.operation not in _READ_OPERATIONS and pending.cancel_target is None):
                    self._abort_locked("INVALID_RESPONSE", "Runtime returned an uncorrelated response")
                    return False
            if pending.cancel_target is not None and envelope["status"] == "ok":
                result = envelope["result"]
                if (set(result) != {"requestId", "readCancelled"} or
                        result["requestId"] != pending.cancel_target or type(result["readCancelled"]) is not bool):
                    self._abort_locked("INVALID_RESPONSE", "Runtime returned an invalid cancellation receipt")
                    return False
            if late:
                self._retired.pop(request_id)
                return True
            self._pending.pop(request_id)
            if pending.timer is not None:
                pending.timer.cancel()
            if pending.cancel_target is not None:
                outcome = (envelope["result"]["readCancelled"] if envelope["status"] == "ok" else envelope["code"])
                self._remember(self._cancel_outcomes, pending.cancel_target, outcome)
            else:
                pending.responses.put_nowait(envelope)
        return True

    def _abort_locked(self, code, message, *, kill=True):
        if self._closed:
            return
        self._closed = True
        for pending in self._pending.values():
            if pending.timer is not None:
                pending.timer.cancel()
            if pending.cancel_target is not None:
                self._remember(self._cancel_outcomes, pending.cancel_target, code)
            else:
                pending.responses.put_nowait(WorkflowError(code, message))
        self._pending.clear()
        if kill:
            try:
                self.process.kill()
            except OSError:
                pass

    def _fail_transport(self, code, message):
        with self._mutex:
            self._abort_locked(code, message)

    def preview(self, command, parameters, expected_revision=None):
        if expected_revision is None:
            expected_revision = self.request("project.query")["revision"]
        return self.request("command.preview", {"command": command, "parameters": parameters,
                                                "expectedRevision": expected_revision})

    def approve(self, preview):
        """Host API: call only after the user authorizes the displayed fixed preview."""
        return self.request("host.approve", {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})

    def propose_plan(self, plan):
        """Validate and retain a native-source plan without starting any node."""
        return self.request("task.plan.propose", {"plan": plan})

    def plans(self, plan_id=None):
        return self.request("task.plan.query", {} if plan_id is None else {"planId": plan_id})

    def control_plan(self, plan, action):
        """Pause/cancel/take over scheduling; submitted jobs stay independent."""
        if action not in {"start", "pause", "resume", "cancel", "supersede"}:
            raise WorkflowError("INVALID_PARAMETERS", "Unknown plan control action")
        return self.request("task.plan." + action,
            {"planId": plan["planId"], "expectedPlanSha256": plan["planSha256"]})

    def approve_plan_node(self, plan, node):
        """Trusted host API, only after the user reviews this exact node preview."""
        return self.request("host.task.node.approve", {"planId": plan["planId"],
            "expectedPlanSha256": plan["planSha256"], "nodeId": node["id"], "previewHash": node["previewHash"]})

    def answer_plan(self, plan, node_id, answer):
        return self.request("task.plan.clarify", {"planId": plan["planId"],
            "expectedPlanSha256": plan["planSha256"], "nodeId": node_id, "answer": answer})

    def execute(self, preview, approval):
        return self.request("command.execute", {"previewId": preview["previewId"], "approvalToken": approval["approvalToken"]})

    def wait(self, job_id, timeout=600):
        deadline = time.monotonic() + timeout
        while True:
            job = self.request("jobs.query", {"jobId": job_id})["job"]
            if job["state"] in ("succeeded", "failed", "cancelled", "interrupted"):
                return job
            if time.monotonic() >= deadline:
                raise WorkflowError("SDK_TIMEOUT", "Job did not finish within the host wait budget")
            time.sleep(0.05)

    def telemetry(self, job_id, *, after_seq=0, limit=256, mode="active"):
        """Read accepted native control samples; never blocks until job completion.

        Advance with response['nextSeq']; request again immediately when hasMore
        is true. An empty page may mean the running solver has not accepted a new
        step yet. Availability distinguishes that from missing control channels.
        """
        return self.request("telemetry.query", {"jobId": job_id, "afterSeq": after_seq,
                                               "limit": limit, "mode": mode})

    def project_catalog(self, persistent_document_id=None):
        """Read the linked host project; this does not select a workflow model."""
        parameters = {} if persistent_document_id is None else {"persistentDocumentId": persistent_document_id}
        return self.request("project.catalog.query", parameters)

    def project_snapshot(self, persistent_document_id):
        """Resolve and verify immutable native snapshot bytes before use."""
        return self.request("project.snapshot.query", {"persistentDocumentId": persistent_document_id})

    def register_model_source(self, catalog_root, snapshot_ref):
        """Trusted file-owner operation, outside the product Agent tool catalog."""
        return self.request("model.source.register", {"catalogRoot": str(catalog_root), "snapshotRef": snapshot_ref})

    def recovery(self, job_id):
        """Read verified interrupted-job eligibility; never starts a job."""
        return self.request("job.recovery.query", {"jobId": job_id})

    def runtime_parameters(self, job_id, proposal_id=None):
        return self.request("runtime.parameters.query", {"jobId": job_id,
            **({"proposalId": proposal_id} if proposal_id is not None else {})})

    def preview_runtime_parameters(self, job_id, velocity_gain_nsm, actuator_limits):
        return self.request("runtime.parameters.preview", {"jobId": job_id,
            "velocityGainNsm": velocity_gain_nsm, "actuatorLimits": actuator_limits})

    def submit_runtime_parameters(self, job_id, proposal_id, approval_token):
        return self.request("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal_id, "approvalToken": approval_token})

    def runtime_execution(self, job_id, operation):
        if operation not in ("pause", "resume", "cancel"):
            raise WorkflowError("INVALID_PARAMETERS", "Unsupported runtime execution operation")
        return self.request("runtime.execution." + operation, {"jobId": job_id})

    def preview_recovery(self, job_id):
        """Pin the original attempt/checkpoint in a fresh preview for approval."""
        selected = self.recovery(job_id)
        if selected.get("eligibility") != "recoverable":
            raise WorkflowError("RECOVERY_UNAVAILABLE", selected.get("diagnostic", "Recovery is unavailable"))
        return self.preview("workflow.recover", {"jobId": job_id,
                            "qualificationSha256": selected["qualificationSha256"]})

    def native_result(self, job_id, operation="catalog", *, mode="active", **arguments):
        if operation not in ("catalog", "channels", "series", "summary"):
            raise WorkflowError("UNKNOWN_OPERATION", "Unsupported native result query")
        return self.request("result." + operation + ".query", {"jobId": job_id, "mode": mode, **arguments})

    def series_sources(self, job_id, mode="active"):
        return self.request("result.plot.sources.query", {"jobId": job_id, "mode": mode})

    def series_plot(self, sources, view="time", **options):
        return self.request("result.plot.query", {"view": view, "sources": sources, "options": options})

    def series_raw(self, source, offset=0, limit=200):
        return self.request("result.raw.query", {"source": source, "offset": offset, "limit": limit})

    def series_export(self, sources):
        return self.request("result.raw.export", {"sources": sources, "range": "complete"})

    def series_export_chunk(self, export_id, offset, length=65536):
        return self.request("result.raw.export.chunk", {"exportId": export_id, "offset": offset, "length": length})

    def scientific_result(self, job_id, *, channel=0, offset=0, limit=256, view="amplitude"):
        return self.request("extension.result.query", {"jobId": job_id, "channel": channel,
                                                       "offset": offset, "limit": limit, "view": view})

    def native_series(self, job_id, channel_id, *, mode="active"):
        catalog = self.native_result(job_id, mode=mode, limit=1)
        artifact_id = catalog["artifactId"]
        samples, cursor, result = [], None, None
        while True:
            result = self.native_result(job_id, "series", mode=mode, artifactId=artifact_id,
                channelId=channel_id, limit=200, **({"cursor": cursor} if cursor else {}))
            samples.extend(result["samples"])
            if len(samples) > 100000:
                raise WorkflowError("RESULT_TOO_LARGE", "Native history exceeds the SDK point limit")
            next_cursor = result.get("nextCursor")
            if not next_cursor: break
            if next_cursor == cursor:
                raise WorkflowError("INVALID_RESPONSE", "Native history cursor failed to advance")
            cursor = next_cursor
        result.pop("nextCursor", None)
        result.update(samples=samples, coverageComplete=len(samples) == catalog["frameCount"],
            sourceSnapshotRef=catalog["sourceSnapshotRef"], proofScope=catalog.get("proofScope"))
        return result

    def close(self):
        with self._close_mutex:
            if self._disposed:
                return
            with self._mutex:
                self._abort_locked("SERVICE_CLOSED", "SDK client is closed", kill=False)
            # A blocked sender may need the service to drain output before it
            # accepts EOF. Bound that graceful shutdown even if the sender owns
            # the pipe lock; killing the peer releases the blocked OS write.
            deadline = time.monotonic() + 20
            def interrupt_shutdown():
                try:
                    self.process.kill()
                except OSError:
                    pass
            shutdown_timer = threading.Timer(20, interrupt_shutdown)
            shutdown_timer.daemon = True
            shutdown_timer.start()
            try:
                with self._send_mutex:
                    try:
                        self.process.stdin.close()
                    except (OSError, ValueError):
                        pass
                try:
                    self.process.wait(timeout=max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            finally:
                shutdown_timer.cancel()
            if threading.current_thread() is not self._reader:
                self._reader.join(2)
            if threading.current_thread() is not self._error_reader:
                self._error_reader.join(2)
            self.process.stdout.close()
            self.process.stderr.close()
            self._disposed = True

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
