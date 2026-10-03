"""Versioned process SDK. approve() belongs to an explicitly authorized host."""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import uuid

from .service import API_VERSION, MAX_REQUEST_BYTES, WorkflowError, canonical


class WorkflowClient:
    def __init__(self, workspace, solver, native, examples, python=sys.executable, *, timeout=30,
                 trusted_extensions=False):
        self.timeout = timeout
        self._mutex = threading.Lock()
        self._responses = queue.Queue()
        self._closed = False
        allowed_environment = {"PATH", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "COMSPEC",
                               "PATHEXT", "NUMBER_OF_PROCESSORS", "OMP_NUM_THREADS", "QT_PLUGIN_PATH"}
        environment = {key: value for key, value in os.environ.items() if key.upper() in allowed_environment}
        package_root = str(Path(__file__).resolve().parent.parent)
        environment["PYTHONPATH"] = package_root
        environment["PYTHONNOUSERSITE"] = "1"
        arguments = [str(python), "-m", "range_workflow", "--stdio",
                    "--workspace", str(Path(workspace).absolute()), "--solver", str(solver),
                    "--native", str(native), "--examples", str(examples), "--python", str(python)]
        if trusted_extensions:
            arguments.append("--trusted-extensions")
        self.process = subprocess.Popen(arguments,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=environment, creationflags=0x08000000 if os.name == "nt" else 0)
        self._stderr = bytearray()
        def read_response():
            try:
                while True:
                    line = self.process.stdout.readline(16 * 1024 * 1024 + 1)
                    if not line:
                        break
                    if len(line) > 16 * 1024 * 1024:
                        self._responses.put(WorkflowError("RESPONSE_TOO_LARGE", "Runtime response exceeds SDK limit"))
                        try:
                            self.process.kill()
                        except OSError:
                            pass
                        break
                    self._responses.put(line)
            finally:
                self._responses.put(WorkflowError("SERVICE_EXITED", "Workflow service closed its output stream"))
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
            request_id = str(uuid.uuid4())
            raw = canonical({"apiVersion": API_VERSION, "requestId": request_id,
                             "operation": operation, "parameters": parameters if parameters is not None else {}}) + b"\n"
            if len(raw) > MAX_REQUEST_BYTES:
                raise WorkflowError("REQUEST_TOO_LARGE", "Request exceeds protocol size limit")
            try:
                self.process.stdin.write(raw)
                self.process.stdin.flush()
                response = self._responses.get(timeout=self.timeout)
            except queue.Empty:
                self.close()
                raise WorkflowError("SDK_TIMEOUT", "Workflow request timed out") from None
            except (OSError, ValueError):
                raise WorkflowError("SERVICE_EXITED", "Cannot write to the workflow service") from None
            if isinstance(response, Exception):
                raise response
            try:
                envelope = json.loads(response)
                if (set(envelope) != {"apiVersion", "requestId", "status", "code", "result"} or
                        envelope["apiVersion"] != API_VERSION or envelope["requestId"] != request_id or
                        envelope["status"] not in ("ok", "error") or not isinstance(envelope["code"], str)
                        or not isinstance(envelope["result"], dict)):
                    raise ValueError("invalid response envelope")
                canonical(envelope)
            except (ValueError, TypeError):
                self.close()
                raise WorkflowError("INVALID_RESPONSE", "Runtime returned an invalid response envelope") from None
            if envelope["status"] == "error":
                raise WorkflowError(envelope["code"], envelope["result"].get("message", "Workflow operation failed"))
            return envelope["result"]

    def preview(self, command, parameters, expected_revision=None):
        if expected_revision is None:
            expected_revision = self.request("project.query")["revision"]
        return self.request("command.preview", {"command": command, "parameters": parameters,
                                                "expectedRevision": expected_revision})

    def approve(self, preview):
        """Host API: call only after the user authorizes the displayed fixed preview."""
        return self.request("host.approve", {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})

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

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self._reader.join(2)
        self._error_reader.join(2)
        self.process.stdout.close()
        self.process.stderr.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
