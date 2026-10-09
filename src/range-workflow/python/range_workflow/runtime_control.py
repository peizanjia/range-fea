"""Ephemeral per-attempt native control channel; authority stays in the host.

The listener and connection token are private process resources. Requests are
asynchronous so the workspace lock is never held waiting for native replies.
"""
from __future__ import annotations

import copy
import hmac
import json
import math
import queue
import secrets
import socket
import threading
import uuid

from . import runtime_parameters as rp

SCHEMA = "range.runtime-control/v1"
MAX_FRAME_BYTES = 65536
_IDENTITY_KEYS = {"jobId", "runId", "taskId", "modelSha256", "baseControllerRevision"}


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate native runtime frame field")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError("Nonfinite native runtime number")


def _uuid(value):
    try:
        result = uuid.UUID(value)
    except (ValueError, TypeError, AttributeError):
        raise ValueError("Invalid runtime frame request identity") from None
    if result.int == 0 or str(result) != value:
        raise ValueError("Invalid runtime frame request identity")
    return value


class RuntimeControlSession:
    def __init__(self, job_id, run_id, task_id, model_sha256, gain_nsm, callback, cancel):
        for value in (job_id, run_id, task_id):
            _uuid(value)
        if type(gain_nsm) not in (int, float) or not math.isfinite(gain_nsm) or not 1e-9 <= gain_nsm <= 1e12:
            raise ValueError("Invalid runtime bootstrap gain")
        if type(model_sha256) is not str or not rp._SHA.fullmatch(model_sha256):
            raise ValueError("Invalid runtime source digest")
        self._expected = dict(jobId=job_id, runId=run_id, taskId=task_id, modelSha256=model_sha256)
        self._gain = float(gain_nsm)
        self._callback, self._cancel = callback, cancel
        self._token = secrets.token_hex(32)
        self._lock = threading.RLock()
        self._outgoing = queue.Queue(maxsize=128)
        self._pending = {}
        self._identity = None
        self._state = None
        self._status = "connecting"
        self._connection = None
        self._closed = threading.Event()
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self._listener.settimeout(0.1)
        self._thread = threading.Thread(target=self._run, name="native-runtime-control", daemon=True)
        self._thread.start()

    def environment(self):
        return {"RANGE_RUNTIME_CONTROL_PORT": str(self._listener.getsockname()[1]),
                "RANGE_RUNTIME_CONTROL_TOKEN": self._token,
                "RANGE_RUNTIME_CONTROL_RUN_ID": self._expected["runId"],
                "RANGE_RUNTIME_CONTROL_JOB_ID": self._expected["jobId"],
                "RANGE_RUNTIME_CONTROL_GAIN_NSM": format(self._gain, ".17g")}

    def snapshot(self):
        with self._lock:
            return {"status": self._status, "identity": copy.deepcopy(self._identity),
                    "acceptedState": copy.deepcopy(self._state), "pendingRequestCount": len(self._pending)}

    def request(self, operation, parameters=None):
        parameters = copy.deepcopy(parameters or {})
        allowed = {"enqueue": {"proposalId", "expectedParameterRevision", "parameters"},
                   "query": {"proposalId"}, "cancel-proposal": {"proposalId"},
                   "pause-next": set(), "resume": set(), "cancel-run": set()}
        if operation not in allowed or type(parameters) is not dict or set(parameters) != allowed[operation]:
            raise ValueError("Invalid native runtime operation")
        if "proposalId" in parameters:
            _uuid(parameters["proposalId"])
        if operation == "enqueue":
            rp.decimal(parameters["expectedParameterRevision"])
            rp.validate_parameters(parameters["parameters"])
        with self._lock:
            if self._status != "ready" or self._closed.is_set():
                raise ValueError("Native runtime connection is unavailable")
            if len(self._pending) >= 128:
                raise ValueError("Native runtime request capacity is full")
            request_id = str(uuid.uuid4())
            frame = {"schema": SCHEMA, "op": operation, "requestId": request_id,
                     "identity": copy.deepcopy(self._identity), **parameters}
            encoded = self._encode(frame)
            self._outgoing.put_nowait(encoded)
            self._pending[request_id] = {"operation": operation, "proposalId": parameters.get("proposalId")}
            return request_id

    @staticmethod
    def _encode(frame):
        encoded = json.dumps(frame, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")
        if len(encoded) > MAX_FRAME_BYTES:
            raise ValueError("Native runtime frame exceeds byte limit")
        return encoded + b"\n"

    def _receive(self, raw):
        frame = json.loads(raw, object_pairs_hook=_unique, parse_constant=_reject_constant)
        if type(frame) is not dict or frame.get("schema") != SCHEMA:
            raise ValueError("Unsupported native runtime frame")
        _uuid(frame.get("requestId"))
        identity = frame.get("identity")
        if type(identity) is not dict or set(identity) != _IDENTITY_KEYS:
            raise ValueError("Invalid native runtime identity shape")
        if any(identity.get(key) != value for key, value in self._expected.items()):
            raise ValueError("Native runtime attempt identity mismatch")
        base = identity["baseControllerRevision"]
        if type(base) is not str or not rp._SHA.fullmatch(base):
            raise ValueError("Invalid native controller revision")
        if frame.get("op") == "hello":
            if set(frame) != {"schema", "op", "requestId", "identity", "token", "acceptedState"}:
                raise ValueError("Invalid native runtime handshake")
            token = frame["token"]
            if type(token) is not str or not hmac.compare_digest(token, self._token):
                raise ValueError("Native runtime authentication failed")
            state = rp.validate_state(frame["acceptedState"])
            if state["parameters"]["fixedBaseControllerRevision"] != base:
                raise ValueError("Native runtime state base mismatch")
            if not state["history"] and state["parameters"]["velocityGainNsm"] != self._gain:
                raise ValueError("Native bootstrap gain disagrees with pinned host artifact")
            with self._lock:
                if self._identity is not None:
                    raise ValueError("Native runtime handshake was repeated")
                self._identity, self._state = copy.deepcopy(identity), state
                self._status = "ready"
            self._connection.sendall(self._encode({"schema": SCHEMA, "op": "ready",
                "requestId": frame["requestId"], "identity": identity, "ok": True}))
            self._callback({"op": "ready", "identity": copy.deepcopy(identity), "acceptedState": state})
            return
        with self._lock:
            if self._status != "ready" or identity != self._identity:
                raise ValueError("Native runtime reply precedes handshake")
        operation = frame.get("op")
        if operation == "reply":
            required = {"schema", "op", "requestId", "identity", "command", "ok", "reason"}
            if not required <= set(frame) or set(frame) - required - {"receipt", "acceptedState"}:
                raise ValueError("Invalid native runtime reply shape")
            if type(frame["ok"]) is not bool or type(frame["reason"]) is not str or len(frame["reason"]) > 256:
                raise ValueError("Invalid native runtime reply outcome")
            with self._lock:
                pending = self._pending.get(frame["requestId"])
                if pending is None or pending["operation"] != frame["command"]:
                    raise ValueError("Uncorrelated native runtime reply")
                if "receipt" in frame and pending["proposalId"] != frame["receipt"].get("proposalId"):
                    raise ValueError("Native runtime reply names another proposal")
                del self._pending[frame["requestId"]]
        elif operation == "receipt":
            if set(frame) - {"schema", "op", "requestId", "identity", "receipt", "acceptedState"} or "receipt" not in frame:
                raise ValueError("Invalid native runtime receipt frame")
        else:
            raise ValueError("Unsupported native runtime response")
        if "receipt" in frame:
            self._verify_receipt(frame)
        elif "acceptedState" in frame:
            raise ValueError("Native accepted state is missing its receipt")
        self._callback(copy.deepcopy(frame))

    def _verify_receipt(self, frame):
        receipt = frame["receipt"]
        required = {"proposalId", "parameterRevision", "parameterSha256", "runtimeControllerRevision",
                    "status", "reason", "provenance"}
        if type(receipt) is not dict or not required <= set(receipt):
            raise ValueError("Invalid native parameter receipt")
        _uuid(receipt["proposalId"])
        rp.decimal(receipt["parameterRevision"])
        for key in ("parameterSha256", "runtimeControllerRevision"):
            if type(receipt[key]) is not str or not rp._SHA.fullmatch(receipt[key]):
                raise ValueError("Invalid native receipt digest")
        if receipt["status"] not in {"queued", "frozen", "accepted", "rejected"} or receipt["provenance"] not in {"restored", "currentAttempt"}:
            raise ValueError("Invalid native receipt status")
        if type(receipt["reason"]) is not str or len(receipt["reason"]) > 256:
            raise ValueError("Invalid native receipt reason")
        base = self._identity["baseControllerRevision"]
        import hashlib
        import struct
        digest_bytes = b"".join(struct.pack(">I", len(text)) + text.encode("ascii")
                                for text in (base, receipt["parameterSha256"]))
        if hashlib.sha256(digest_bytes).hexdigest() != receipt["runtimeControllerRevision"]:
            raise ValueError("Native receipt runtime revision disagrees with parameter digest")
        if receipt["status"] != "accepted":
            if set(receipt) != required or "acceptedState" in frame:
                raise ValueError("Unaccepted native receipt claims an applied state")
            return
        if set(receipt) != required | {"appliedSeq", "acceptedTimeS"} or "acceptedState" not in frame:
            raise ValueError("Accepted native receipt lacks authoritative state")
        state = rp.validate_state(frame["acceptedState"])
        matching = [change for change in state["history"] if change["proposalId"] == receipt["proposalId"]]
        if len(matching) != 1:
            raise ValueError("Accepted receipt is absent from parameter history")
        change = matching[0]
        if any(receipt[key] != change[history_key] for key, history_key in
               (("parameterRevision", "revision"), ("parameterSha256", "parameterSha256"),
                ("appliedSeq", "appliedSeq"), ("acceptedTimeS", "acceptedTimeS"))):
            raise ValueError("Accepted receipt disagrees with parameter history")
        with self._lock:
            old = self._state
            if (state["parameters"]["fixedBaseControllerRevision"] != self._identity["baseControllerRevision"]
                    or state["history"][:len(old["history"])] != old["history"]
                    or len(state["history"]) < len(old["history"])):
                raise ValueError("Native runtime accepted history forked")
            self._state = state

    def _run(self):
        try:
            while not self._closed.is_set() and not self._cancel.is_set():
                try:
                    connection, _ = self._listener.accept()
                    self._connection = connection
                    break
                except socket.timeout:
                    continue
            if self._connection is None:
                return
            self._connection.settimeout(0.05)
            buffer = bytearray()
            while not self._closed.is_set() and not self._cancel.is_set():
                while True:
                    try:
                        self._connection.sendall(self._outgoing.get_nowait())
                    except queue.Empty:
                        break
                try:
                    chunk = self._connection.recv(8192)
                except socket.timeout:
                    continue
                if not chunk:
                    # Native also closes this channel after normal completion.
                    # EOF leaves pending outcomes unknown; it is not evidence
                    # that the independently monitored process was cancelled.
                    break
                buffer.extend(chunk)
                while b"\n" in buffer:
                    raw, _, remaining = buffer.partition(b"\n")
                    buffer = bytearray(remaining)
                    if not raw or len(raw) > MAX_FRAME_BYTES:
                        raise ValueError("Native runtime frame exceeds limit")
                    self._receive(raw)
                if len(buffer) > MAX_FRAME_BYTES:
                    raise ValueError("Native runtime frame exceeds limit")
        except (OSError, ValueError, TypeError, KeyError, OverflowError):
            self._cancel.set()
        finally:
            with self._lock:
                self._status = "disconnected"
            if not self._closed.is_set():
                try:
                    self._callback({"op": "disconnected", "unobservedRequestCount": len(self._pending)})
                except Exception:
                    self._cancel.set()
            self._close_sockets()

    def _close_sockets(self):
        for connection in (self._connection, self._listener):
            if connection is not None:
                try:
                    connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                connection.close()

    def close(self):
        self._closed.set()
        self._close_sockets()
        if threading.current_thread() is not self._thread:
            self._thread.join(2)
