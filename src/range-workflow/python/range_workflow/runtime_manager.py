"""Host proposals and durable observations for one live native attempt.

The transport's queued reply is an observation, never an accepted change.
Approval tokens and request correlations are process-local, so interrupted
proposals are never replayed after restoration.
"""
from __future__ import annotations

import copy
import hashlib
import math
import secrets
import struct
import uuid
import json
from pathlib import Path

from . import runtime_parameters as rp
from .runtime_control import RuntimeControlSession
from .service import WorkflowError, digest, exact, identifier, utc


_IDENTITY_FIELDS = {"jobId", "runId", "taskId", "modelSha256", "baseControllerRevision"}
_PROPOSAL_FIELDS = {"proposalId", "identity", "beforeParameters", "parameters", "sessionId",
                    "status", "createdAt", "previewSha256"}
_PROPOSAL_OPTIONAL = {"submittedAt", "receipt", "error", "requestId"}
_PROPOSAL_STATUSES = {"previewed", "expired", "intent", "queued", "frozen", "unknown",
                      "accepted", "rejected", "cancelled"}
_ATTEMPT_STATUSES = {"connecting", "ready", "disconnected", "closed", "interrupted"}
_RECEIPT_FIELDS = {"proposalId", "parameterRevision", "parameterSha256",
                   "runtimeControllerRevision", "status", "reason", "provenance"}


def _canonical_uuid(value):
    if type(value) is not str:
        raise ValueError("Invalid UUID")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, TypeError, AttributeError):
        raise ValueError("Invalid UUID") from None
    if parsed.int == 0 or str(parsed) != value:
        raise ValueError("Invalid UUID")
    return value


def _bounded_text(value, maximum=256):
    if type(value) is not str or not value or len(value) > maximum:
        raise ValueError("Invalid bounded text")
    return value


def _digest(value):
    if type(value) is not str or not rp._SHA.fullmatch(value):
        raise ValueError("Invalid lowercase SHA256")
    return value


def _immutable_limits(value):
    normalized = rp.limits(value)
    if len(normalized) != 1 or normalized != value:
        raise ValueError("Unsupported immutable runtime limits")
    return normalized


def _within_limits(parameters, immutable_limits):
    proposed = rp.limits(parameters["actuatorLimits"])
    if len(proposed) != 1 or len(immutable_limits) != 1:
        raise ValueError("Runtime actuator catalog changed")
    original, changed = immutable_limits[0], proposed[0]
    if (original["actuatorId"] != changed["actuatorId"]
            or changed["minN"] < original["minN"]
            or changed["maxN"] > original["maxN"]
            or (original["rateNPerS"] > 0
                and (changed["rateNPerS"] <= 0 or changed["rateNPerS"] > original["rateNPerS"]))):
        raise ValueError("Runtime limits exceed immutable model envelope")


def _validate_receipt(receipt, proposal_id, parameters, state=None, expected_status=None):
    if type(receipt) is not dict:
        raise ValueError("Invalid runtime receipt")
    status = receipt.get("status")
    if status not in {"queued", "frozen", "accepted", "rejected"}:
        raise ValueError("Invalid runtime receipt status")
    required = _RECEIPT_FIELDS
    accepted = status == "accepted"
    expected_fields = required | ({"appliedSeq", "acceptedTimeS"} if accepted else set())
    if set(receipt) != expected_fields:
        raise ValueError("Invalid runtime receipt fields")
    if expected_status is not None and status != expected_status:
        raise ValueError("Runtime receipt status disagrees with proposal")
    if _canonical_uuid(receipt["proposalId"]) != proposal_id:
        raise ValueError("Runtime receipt proposal identity mismatch")
    if type(receipt["reason"]) is not str or len(receipt["reason"]) > 256:
        raise ValueError("Invalid runtime receipt reason")
    if receipt["provenance"] not in {"currentAttempt", "restored"}:
        raise ValueError("Invalid runtime receipt provenance")
    if (rp.decimal(receipt["parameterRevision"]) != rp.decimal(parameters["revision"])
            or _digest(receipt["parameterSha256"]) != parameters["parameterSha256"]
            or _digest(receipt["runtimeControllerRevision"]) != parameters["runtimeControllerRevision"]):
        raise ValueError("Runtime receipt differs from submitted parameters")
    if accepted:
        if state is None:
            raise ValueError("Accepted runtime receipt lacks authoritative state")
        validated = rp.validate_state(state)
        if validated["parameters"]["fixedBaseControllerRevision"] != parameters["fixedBaseControllerRevision"]:
            raise ValueError("Accepted runtime state has another fixed base")
        applied_seq = rp.decimal(receipt["appliedSeq"])
        accepted_time = receipt["acceptedTimeS"]
        if (type(accepted_time) not in (int, float) or not math.isfinite(accepted_time)
                or accepted_time < 0):
            raise ValueError("Invalid accepted runtime time")
        matching = [change for change in validated["history"] if change["proposalId"] == proposal_id]
        if len(matching) != 1:
            raise ValueError("Accepted runtime receipt is absent from saved history")
        change = matching[0]
        if (change["revision"] != receipt["parameterRevision"]
                or change["parameterSha256"] != receipt["parameterSha256"]
                or change["appliedSeq"] != receipt["appliedSeq"]
                or change["acceptedTimeS"] != accepted_time
                or applied_seq != rp.decimal(change["appliedSeq"])):
            raise ValueError("Accepted runtime receipt disagrees with saved history")
    elif state is not None:
        raise ValueError("Unaccepted runtime receipt claims accepted state")
    return copy.deepcopy(receipt)


def _validate_proposal(proposal_id, proposal, identity, session_id, immutable_limits, accepted_state):
    if type(proposal) is not dict or not _PROPOSAL_FIELDS <= proposal.keys() \
            or proposal.keys() - _PROPOSAL_FIELDS - _PROPOSAL_OPTIONAL:
        raise ValueError("Invalid saved runtime proposal fields")
    if _canonical_uuid(proposal_id) != proposal_id or _canonical_uuid(proposal["proposalId"]) != proposal_id:
        raise ValueError("Runtime proposal map key mismatch")
    if proposal["identity"] != identity:
        raise ValueError("Runtime proposal identity mismatch")
    # Historical session identity is frozen provenance. Only live approval
    # checks compare it with the current service session after reopening.
    _canonical_uuid(proposal["sessionId"])
    _bounded_text(proposal["createdAt"], 128)
    status = proposal["status"]
    if type(status) is not str or status not in _PROPOSAL_STATUSES:
        raise ValueError("Invalid saved runtime proposal status")

    before = rp.validate_parameters(proposal["beforeParameters"])
    parameters = rp.validate_parameters(proposal["parameters"])
    base = identity["baseControllerRevision"]
    if (before["fixedBaseControllerRevision"] != base
            or parameters["fixedBaseControllerRevision"] != base
            or rp.decimal(parameters["revision"]) != rp.decimal(before["revision"]) + 1):
        raise ValueError("Runtime proposal revision or fixed base mismatch")
    _within_limits(before, immutable_limits)
    _within_limits(parameters, immutable_limits)

    preview = {key: copy.deepcopy(proposal[key]) for key in _PROPOSAL_FIELDS - {"previewSha256"}}
    preview["status"] = "previewed"
    if _digest(proposal["previewSha256"]) != digest(preview):
        raise ValueError("Runtime preview hash mismatch")

    submitted = "submittedAt" in proposal
    if submitted:
        _bounded_text(proposal["submittedAt"], 128)
    if "requestId" in proposal:
        _canonical_uuid(proposal["requestId"])
    if "error" in proposal:
        if type(proposal["error"]) is not str or len(proposal["error"]) > 256:
            raise ValueError("Invalid saved runtime proposal error")

    receipt_status = None
    if "receipt" in proposal:
        receipt = proposal["receipt"]
        receipt_status = receipt.get("status") if type(receipt) is dict else None
        receipt_state = accepted_state if receipt_status == "accepted" else None
        _validate_receipt(receipt, proposal_id, parameters, receipt_state)

    if status == "previewed":
        if submitted or any(key in proposal for key in ("receipt", "error", "requestId")):
            raise ValueError("Previewed proposal has submitted fields")
    elif status == "expired":
        if submitted or any(key in proposal for key in ("receipt", "error", "requestId")):
            raise ValueError("Expired proposal has submitted fields")
    elif status == "cancelled":
        if submitted or any(key in proposal for key in ("receipt", "error", "requestId")):
            raise ValueError("Cancelled proposal was not a local cancellation")
    elif status == "intent":
        if not submitted or "receipt" in proposal or "error" in proposal:
            raise ValueError("Runtime intent lacks a durable submission")
    elif status in {"queued", "frozen"}:
        if not submitted or receipt_status != status or "error" in proposal:
            raise ValueError("Runtime queue state lacks a matching receipt")
    elif status == "unknown":
        if not submitted or "error" in proposal or receipt_status not in (None, "queued", "frozen"):
            raise ValueError("Unknown runtime outcome has invalid evidence")
    elif status == "accepted":
        if not submitted or receipt_status != "accepted" or "error" in proposal:
            raise ValueError("Accepted runtime proposal lacks receipt proof")
    elif status == "rejected":
        if not submitted:
            raise ValueError("Rejected runtime proposal has no durable submission")
        if receipt_status == "rejected":
            if "error" in proposal:
                raise ValueError("Receipt rejection also claims a queue error")
        elif "receipt" not in proposal and "error" in proposal:
            if proposal["error"] == "LOCAL_NOT_SENT":
                if "requestId" in proposal:
                    raise ValueError("Locally unsent rejection has a request ID")
            elif "requestId" not in proposal:
                raise ValueError("Observed enqueue rejection lacks request correlation")
        else:
            raise ValueError("Rejected runtime proposal lacks rejection evidence")


def _validate_runtime(job_id, job, runtime, session_id):
    _canonical_uuid(job_id)
    if type(job) is not dict or job.get("jobId") != job_id:
        raise ValueError("Saved runtime job identity mismatch")
    if (type(runtime) is not dict or set(runtime) != {"version", "attempts"}
            or type(runtime["version"]) is not int or runtime["version"] != 1):
        raise ValueError("Invalid saved runtime control state")
    attempts = runtime["attempts"]
    if type(attempts) is not dict or len(attempts) > 8:
        raise ValueError("Invalid saved runtime attempt collection")
    _canonical_uuid(session_id)
    for run_id, attempt in attempts.items():
        if _canonical_uuid(run_id) != run_id:
            raise ValueError("Invalid saved runtime attempt key")
        if (type(attempt) is not dict or set(attempt) != {"identity", "acceptedState", "limits", "status", "proposals"}
                or type(attempt["status"]) is not str or attempt["status"] not in _ATTEMPT_STATUSES):
            raise ValueError("Invalid saved runtime attempt")
        immutable_limits = _immutable_limits(attempt["limits"])
        identity = attempt["identity"]
        accepted_state = attempt["acceptedState"]
        proposals = attempt["proposals"]
        if type(proposals) is not dict or len(proposals) > 128:
            raise ValueError("Invalid saved runtime proposal collection")
        if identity is None:
            if (attempt["status"] != "connecting" or accepted_state is not None or proposals):
                raise ValueError("Uninitialized runtime attempt contains state")
            continue
        if type(identity) is not dict or set(identity) != _IDENTITY_FIELDS:
            raise ValueError("Invalid saved runtime identity")
        if (_canonical_uuid(identity["jobId"]) != job_id
                or _canonical_uuid(identity["runId"]) != run_id
                or _canonical_uuid(identity["taskId"]) != identity["taskId"]
                or _digest(identity["modelSha256"]) != identity["modelSha256"]
                or _digest(identity["baseControllerRevision"]) != identity["baseControllerRevision"]):
            raise ValueError("Saved runtime identity disagrees with its attempt")
        if accepted_state is None:
            raise ValueError("Initialized runtime attempt lacks accepted state")
        accepted = rp.validate_state(accepted_state)
        if accepted["parameters"]["fixedBaseControllerRevision"] != identity["baseControllerRevision"]:
            raise ValueError("Saved accepted state has another fixed base")
        _within_limits(accepted["parameters"], immutable_limits)
        if attempt["status"] == "connecting":
            raise ValueError("Initialized runtime attempt is still connecting")
        for proposal_id, proposal in proposals.items():
            _validate_proposal(proposal_id, proposal, identity, session_id, immutable_limits, accepted)


class RuntimeManager:
    def __init__(self, service):
        self.service = service
        self.sessions = {}
        self.approvals = {}
        # requestId -> (jobId, runId, proposalId, operation); process-local only.
        self.requests = {}

    def restore(self, candidate):
        try:
            jobs = candidate["jobs"]
            session_id = candidate["sessionId"]
            if type(jobs) is not dict:
                raise ValueError("Invalid saved jobs collection")
            validated = []
            for job_id, job in jobs.items():
                if type(job) is not dict:
                    raise ValueError("Invalid saved job")
                runtime = job.get("runtimeControl")
                if runtime is None:
                    continue
                _validate_runtime(job_id, job, runtime, session_id)
                validated.append(runtime)
        except (KeyError, TypeError, ValueError, OverflowError, AttributeError, RecursionError):
            raise WorkflowError("CORRUPT_STATE", "Invalid saved runtime control state") from None

        # Validate the complete history before changing any saved statuses.
        for runtime in validated:
            for attempt in runtime["attempts"].values():
                if attempt["identity"] is None:
                    continue
                attempt["status"] = "interrupted"
                for proposal in attempt["proposals"].values():
                    if proposal["status"] == "previewed":
                        proposal["status"] = "expired"
                    elif proposal["status"] in {"intent", "queued", "frozen", "unknown"}:
                        proposal["status"] = "unknown"
        self.approvals.clear()
        self.requests.clear()

    def register(self, job_id, run_id, task_id, model_sha, gain, limits, cancel):
        service = self.service
        with service._lock:
            if job_id in self.sessions:
                raise WorkflowError("RUNTIME_BUSY", "This job already has a live runtime channel")
            candidate = copy.deepcopy(service._state)
            job = candidate["jobs"][job_id]
            runtime = job.setdefault("runtimeControl", {"version": 1, "attempts": {}})
            if len(runtime["attempts"]) >= 8 and run_id not in runtime["attempts"]:
                raise WorkflowError("RUNTIME_LIMIT", "Runtime attempt history is full")
            if run_id in runtime["attempts"]:
                raise WorkflowError("RUNTIME_CONFLICT", "A native attempt identity cannot be reused")
            runtime["attempts"][run_id] = {"identity": None, "acceptedState": None,
                "limits": rp.limits(limits), "status": "connecting", "proposals": {}}
            service._commit(candidate, "runtime.connect", "connecting", job_id=job_id,
                            parameters={"runId": run_id, "taskId": task_id, "modelSha256": model_sha})
            session = RuntimeControlSession(job_id, run_id, task_id, model_sha, gain,
                lambda frame: self.observe(job_id, run_id, frame), cancel)
            self.sessions[job_id] = (run_id, session)
            return session

    def unregister(self, job_id, session):
        session.close()
        with self.service._lock:
            pair = self.sessions.get(job_id)
            if pair and pair[1] is session:
                del self.sessions[job_id]
                candidate = copy.deepcopy(self.service._state)
                attempt = candidate["jobs"][job_id]["runtimeControl"]["attempts"][pair[0]]
                attempt["status"] = "closed" if attempt["identity"] is not None else "connecting"
                for proposal in attempt["proposals"].values():
                    if proposal["status"] in {"intent", "queued", "frozen"}:
                        proposal["status"] = "unknown"
                    elif proposal["status"] == "previewed":
                        proposal["status"] = "expired"
                self._drop_attempt_requests(job_id, pair[0])
                service = self.service
                service._commit(candidate, "runtime.disconnect", "closed", job_id=job_id,
                                parameters={"runId": pair[0]})

    def _drop_attempt_requests(self, job_id, run_id):
        for request_id, correlation in list(self.requests.items()):
            if correlation[0] == job_id and correlation[1] == run_id:
                del self.requests[request_id]

    def _request(self, job_id, run_id, session, operation, parameters, proposal_id=None):
        request_id = session.request(operation, parameters)
        self._track_request(job_id, run_id, request_id, operation, proposal_id)
        return request_id

    def _track_request(self, job_id, run_id, request_id, operation, proposal_id=None):
        _canonical_uuid(request_id)
        if request_id in self.requests:
            raise ValueError("Native runtime reused a request identity")
        self.requests[request_id] = (job_id, run_id, proposal_id, operation)

    def _validate_observed_receipt(self, attempt, proposal, receipt, state):
        proposal_id = receipt.get("proposalId") if type(receipt) is dict else None
        if proposal is None:
            if type(receipt) is not dict or receipt.get("provenance") != "restored" or receipt.get("status") != "accepted":
                raise ValueError("Native receipt has no durable host submission")
            validated = rp.validate_state(state)
            base = validated["parameters"]["fixedBaseControllerRevision"]
            parameter_sha = _digest(receipt.get("parameterSha256"))
            runtime_revision = hashlib.sha256(b"".join(
                struct.pack(">I", len(text)) + text.encode("ascii")
                for text in (base, parameter_sha))).hexdigest()
            parameters = {"fixedBaseControllerRevision": base,
                          "revision": receipt.get("parameterRevision"),
                          "parameterSha256": parameter_sha,
                          "runtimeControllerRevision": runtime_revision}
        else:
            if not proposal.get("submittedAt"):
                raise ValueError("Native receipt has no durable host submission")
            parameters = proposal["parameters"]
        accepted_state = state if type(receipt) is dict and receipt.get("status") == "accepted" else None
        _validate_receipt(receipt, proposal_id, parameters, accepted_state)
        if accepted_state is not None:
            previous = attempt["acceptedState"]
            validated = rp.validate_state(accepted_state)
            if (validated["parameters"]["fixedBaseControllerRevision"] != attempt["identity"]["baseControllerRevision"]
                    or validated["history"][:len(previous["history"])] != previous["history"]):
                raise ValueError("Native accepted history forked from durable state")
        return proposal_id, accepted_state

    def observe(self, job_id, run_id, frame):
        service = self.service
        with service._lock:
            if service._closed:
                return
            if type(frame) is not dict:
                raise ValueError("Invalid runtime observation")
            if "receipt" in frame and type(frame["receipt"]) is not dict:
                raise ValueError("Invalid native runtime receipt")
            if "acceptedState" in frame and "receipt" not in frame and frame.get("op") != "ready":
                raise ValueError("Native accepted state is missing its receipt")
            candidate = copy.deepcopy(service._state)
            attempt = candidate["jobs"][job_id]["runtimeControl"]["attempts"][run_id]
            op = frame.get("op")
            if op in {"reply", "receipt"} and frame.get("identity") != attempt["identity"]:
                raise ValueError("Native runtime observation identity mismatch")
            request_id = frame.get("requestId")
            correlation = None
            if request_id is not None:
                _canonical_uuid(request_id)
                correlation = self.requests.get(request_id)
                if correlation is not None and correlation[:2] != (job_id, run_id):
                    raise ValueError("Native runtime response belongs to another attempt")

            if op == "ready":
                identity = frame.get("identity")
                state = rp.validate_state(frame.get("acceptedState"))
                _validate_ready_identity(job_id, run_id, identity)
                if state["parameters"]["fixedBaseControllerRevision"] != identity["baseControllerRevision"]:
                    raise ValueError("Native runtime state base mismatch")
                if attempt["identity"] is not None:
                    raise ValueError("Native runtime identity was already established")
                attempt.update(identity=copy.deepcopy(identity), acceptedState=state, status="ready")
            elif op == "disconnected":
                attempt["status"] = "disconnected" if attempt["identity"] is not None else "connecting"
                for proposal in attempt["proposals"].values():
                    if proposal["status"] in {"intent", "queued", "frozen"}:
                        proposal["status"] = "unknown"
                    elif proposal["status"] == "previewed":
                        proposal["status"] = "expired"
                self._drop_attempt_requests(job_id, run_id)
            elif op == "reply":
                if correlation is None:
                    raise ValueError("Uncorrelated native runtime reply")
                mapped_job, mapped_run, mapped_proposal, mapped_operation = correlation
                if (mapped_job != job_id or mapped_run != run_id
                        or frame.get("command") != mapped_operation
                        or type(frame.get("ok")) is not bool
                        or type(frame.get("reason")) is not str or len(frame["reason"]) > 256):
                    raise ValueError("Native runtime reply correlation mismatch")
                if mapped_operation == "enqueue":
                    proposal = attempt["proposals"].get(mapped_proposal)
                    if (proposal is None or not proposal.get("submittedAt")
                            or ("requestId" in proposal and proposal["requestId"] != request_id)):
                        raise ValueError("Native enqueue reply has no matching durable proposal")
                    if "receipt" in frame:
                        receipt = frame["receipt"]
                        if receipt.get("proposalId") != mapped_proposal:
                            raise ValueError("Native enqueue reply names another proposal")
                        proposal_id, accepted_state = self._validate_observed_receipt(
                            attempt, proposal, receipt, frame.get("acceptedState"))
                        proposal.update(status=receipt["status"], receipt=copy.deepcopy(receipt))
                        if accepted_state is not None:
                            attempt["acceptedState"] = accepted_state
                    elif frame["ok"] is False:
                        proposal.update(status="rejected", error=frame["reason"])
                elif "receipt" in frame:
                    receipt = frame["receipt"]
                    if mapped_proposal is None or receipt.get("proposalId") != mapped_proposal:
                        raise ValueError("Native runtime reply names another proposal")
                    proposal = attempt["proposals"].get(mapped_proposal)
                    proposal_id, accepted_state = self._validate_observed_receipt(
                        attempt, proposal, receipt, frame.get("acceptedState"))
                    if proposal is not None:
                        proposal.update(status=receipt["status"], receipt=copy.deepcopy(receipt))
                    if accepted_state is not None:
                        attempt["acceptedState"] = accepted_state
                service._commit(candidate, "runtime.observe", frame.get("receipt", {}).get("status", op),
                                job_id=job_id, parameters={"runId": run_id, "observation": frame})
                self.requests.pop(request_id, None)
                return
            elif op == "receipt":
                receipt = frame.get("receipt")
                proposal_id = receipt.get("proposalId") if type(receipt) is dict else None
                if correlation is not None:
                    mapped_job, mapped_run, mapped_proposal, mapped_operation = correlation
                    if (mapped_job != job_id or mapped_run != run_id or mapped_proposal != proposal_id
                            or mapped_operation not in {"enqueue", "query", "cancel-proposal"}):
                        raise ValueError("Native runtime receipt correlation mismatch")
                proposal = attempt["proposals"].get(proposal_id)
                if proposal is None and (type(receipt) is not dict or receipt.get("provenance") != "restored"):
                    raise ValueError("Native receipt has no matching proposal")
                proposal_id, accepted_state = self._validate_observed_receipt(
                    attempt, proposal, receipt, frame.get("acceptedState"))
                if proposal is not None:
                    proposal.update(status=receipt["status"], receipt=copy.deepcopy(receipt))
                if accepted_state is not None:
                    attempt["acceptedState"] = accepted_state
            else:
                raise ValueError("Unsupported runtime observation")
            service._commit(candidate, "runtime.observe", frame.get("receipt", {}).get("status", op),
                            job_id=job_id, parameters={"runId": run_id, "observation": frame})
            if correlation is not None:
                self.requests.pop(request_id, None)

    def _live(self, job_id):
        identifier(job_id, "jobId")
        pair = self.sessions.get(job_id)
        if pair is None:
            raise WorkflowError("RUNTIME_UNAVAILABLE", "This job has no live native runtime channel")
        run_id, session = pair
        job = self.service._job(job_id)
        attempt = job["runtimeControl"]["attempts"][run_id]
        snapshot = session.snapshot()
        if (job["state"] != "running" or attempt["status"] != "ready" or snapshot["status"] != "ready"
                or snapshot["identity"] != attempt["identity"] or snapshot["acceptedState"] != attempt["acceptedState"]):
            raise WorkflowError("RUNTIME_CHANGED", "The accepted native runtime state is unavailable or changed")
        return run_id, session, attempt

    def execution(self, job):
        """Read only this job's exact native gate file; no caller paths enter."""
        unavailable = {"availability": "unavailable"}
        ref = job.get("runtimeExecution")
        if type(ref) is not dict or set(ref) != {"runId", "taskId", "path", "modelSha256", "context"}:
            return unavailable
        directory = self.service.workspace / "jobs" / job["jobId"]
        if "attemptId" in job:
            directory = directory / "attempts" / job["attemptId"]
        expected = directory / "active" / "runtime-execution.json"
        try:
            path = Path(ref["path"])
            if (path != expected or path.is_symlink() or not path.is_file()
                    or path.resolve() != expected.absolute() or path.stat().st_size > 256 * 1024):
                return unavailable
            from .runtime_control import _unique, _reject_constant
            value = json.loads(path.read_bytes(), object_pairs_hook=_unique, parse_constant=_reject_constant)
            if (type(value) is not dict or value.get("schema") != "range.native-execution-state/v1"
                    or value.get("jobId") != job["jobId"] or value.get("taskId") != ref["taskId"]
                    or value.get("modelSha256") != ref["modelSha256"]):
                return unavailable
            for key in ("sourceSha256", "modelSha256", "controlSha256", "runtimeSha256", "packageSha256"):
                if value.get(key) != ref["context"].get(key):
                    return unavailable
            seq, time = value.get("acceptedSeq"), value.get("timeS")
            if (type(seq) is not int or seq < 0 or type(time) not in (int, float)
                    or not math.isfinite(time) or time < 0 or value.get("state") not in
                    {"armed", "paused", "resumed", "cancel_requested", "cancelled", "finished", "failed"}):
                return unavailable
            return {"availability": "available", "state": value["state"], "acceptedSeq": seq,
                    "timeS": time, "taskId": ref["taskId"], "runId": ref["runId"],
                    "live": job.get("state") == "running" and job["jobId"] in self.sessions,
                    "scope": "last-published-native-boundary"}
        except (OSError, ValueError, TypeError, KeyError):
            return unavailable

    def dispatch(self, operation, p):
        service = self.service
        if operation == "runtime.parameters.query":
            exact(p, {"jobId"}, {"proposalId"})
            if "proposalId" in p:
                with service._lock:
                    try:
                        proposal_id = _canonical_uuid(p["proposalId"])
                    except ValueError:
                        raise WorkflowError("INVALID_PARAMETERS", "proposalId must be a canonical UUID") from None
                    job = service._job(p["jobId"])
                    runtime = job.get("runtimeControl")
                    if type(runtime) is not dict or type(runtime.get("attempts")) is not dict:
                        raise WorkflowError("NOT_FOUND", "Runtime proposal was not found")
                    located = next(((run_id, attempt, attempt["proposals"][proposal_id])
                                    for run_id, attempt in runtime["attempts"].items()
                                    if type(attempt) is dict and type(attempt.get("proposals")) is dict
                                    and proposal_id in attempt["proposals"]), None)
                    if located is None:
                        raise WorkflowError("NOT_FOUND", "Runtime proposal was not found")
                    run_id, attempt, proposal = located
                    if proposal is None:
                        raise WorkflowError("NOT_FOUND", "Runtime proposal was not found")
                    pair = self.sessions.get(p["jobId"])
                    if (proposal.get("submittedAt") and proposal["status"] not in {"accepted", "rejected", "cancelled"}
                            and pair is not None and pair[0] == run_id
                            and job.get("state") == "running" and attempt.get("status") == "ready"):
                        session = pair[1]
                        snapshot = session.snapshot()
                        if (snapshot["status"] == "ready" and snapshot["identity"] == attempt["identity"]
                                and snapshot["acceptedState"] == attempt["acceptedState"]):
                            try:
                                request_id = self._request(p["jobId"], run_id, session, "query",
                                                           {"proposalId": proposal_id}, proposal_id)
                            except ValueError:
                                return copy.deepcopy(proposal)
                            return {**copy.deepcopy(proposal), "requestId": request_id,
                                    "lookupPending": True}
                    return copy.deepcopy(proposal)
            with service._lock:
                job = service._job(p["jobId"])
                return {"jobId": job["jobId"], "jobState": job["state"], "runtimeControl": copy.deepcopy(job.get("runtimeControl")),
                        "connected": job["jobId"] in self.sessions, "execution": self.execution(job)}
        if operation == "runtime.parameters.preview":
            exact(p, {"jobId", "velocityGainNsm", "actuatorLimits"})
            with service._lock:
                run_id, _, attempt = self._live(p["jobId"])
                if len(attempt["proposals"]) >= 128:
                    raise WorkflowError("RUNTIME_LIMIT", "Runtime proposal history is full")
                try:
                    proposed = rp.candidate(attempt["acceptedState"], p["velocityGainNsm"],
                                            p["actuatorLimits"], attempt["limits"])
                except (ValueError, TypeError, KeyError):
                    raise WorkflowError("INVALID_PARAMETERS", "Runtime parameters violate the source capability envelope") from None
                proposal_id = str(uuid.uuid4())
                record = {"proposalId": proposal_id, "identity": copy.deepcopy(attempt["identity"]),
                          "beforeParameters": copy.deepcopy(attempt["acceptedState"]["parameters"]),
                          "parameters": proposed, "sessionId": service._state["sessionId"],
                          "status": "previewed", "createdAt": utc()}
                record["previewSha256"] = digest(record)
                candidate = copy.deepcopy(service._state)
                candidate["jobs"][p["jobId"]]["runtimeControl"]["attempts"][run_id]["proposals"][proposal_id] = record
                service._commit(candidate, operation, "previewed", job_id=p["jobId"], approval="pending", parameters=record)
                return copy.deepcopy(record)
        if operation in {"host.runtime.parameters.approve", "runtime.parameters.submit", "runtime.parameters.cancel"}:
            fields = {"jobId", "proposalId"} | ({"approvalToken"} if operation.endswith("submit") else set())
            exact(p, fields)
            with service._lock:
                run_id, session, attempt = self._live(p["jobId"])
                proposal = attempt["proposals"].get(p["proposalId"])
                if proposal is None:
                    raise WorkflowError("NOT_FOUND", "Runtime proposal was not found")
                if operation.endswith("cancel"):
                    if proposal["status"] in {"accepted", "rejected", "cancelled", "unknown"}:
                        return copy.deepcopy(proposal)
                    if proposal.get("submittedAt"):
                        request_id = self._request(p["jobId"], run_id, session, "cancel-proposal",
                                                   {"proposalId": p["proposalId"]}, p["proposalId"])
                        return {"requestId": request_id, "status": "pending"}
                    candidate = copy.deepcopy(service._state)
                    record = candidate["jobs"][p["jobId"]]["runtimeControl"]["attempts"][run_id]["proposals"][p["proposalId"]]
                    record["status"] = "cancelled"
                    self.approvals.pop(p["proposalId"], None)
                    service._commit(candidate, operation, "cancelled", job_id=p["jobId"])
                    return copy.deepcopy(record)
                if proposal.get("submittedAt"):
                    return copy.deepcopy(proposal)  # Durable receipt lookup; never resend.
                if (proposal["identity"] != attempt["identity"] or proposal["sessionId"] != service._state["sessionId"]
                        or proposal["beforeParameters"] != attempt["acceptedState"]["parameters"]):
                    raise WorkflowError("RUNTIME_CHANGED", "Runtime proposal is stale; preview it again")
                if proposal["status"] != "previewed":
                    raise WorkflowError("APPROVAL_REQUIRED", "Runtime proposal is no longer awaiting approval")
                if operation.startswith("host."):
                    token = self.approvals.setdefault(p["proposalId"], secrets.token_urlsafe(32))
                    return {"proposalId": p["proposalId"], "approvalToken": token,
                            "previewSha256": proposal["previewSha256"]}
                token = p["approvalToken"]
                if type(token) is not str or not token or not secrets.compare_digest(token, self.approvals.get(p["proposalId"], "")):
                    raise WorkflowError("APPROVAL_REQUIRED", "A matching explicit host approval is required")
                candidate = copy.deepcopy(service._state)
                record = candidate["jobs"][p["jobId"]]["runtimeControl"]["attempts"][run_id]["proposals"][p["proposalId"]]
                record.update(status="intent", submittedAt=utc())
                service._commit(candidate, operation, "intent", job_id=p["jobId"], approval="approved", parameters={
                    "proposalId": p["proposalId"], "previewSha256": proposal["previewSha256"], "runId": run_id})
                self.approvals.pop(p["proposalId"], None)
                enqueue_parameters = {
                    "proposalId": p["proposalId"],
                    "expectedParameterRevision": proposal["beforeParameters"]["revision"],
                    "parameters": proposal["parameters"]}
                try:
                    request_id = session.request("enqueue", enqueue_parameters)
                except ValueError:
                    failed = copy.deepcopy(service._state)
                    failed_record = failed["jobs"][p["jobId"]]["runtimeControl"]["attempts"][run_id]["proposals"][p["proposalId"]]
                    failed_record.update(status="rejected", error="LOCAL_NOT_SENT")
                    service._commit(failed, operation, "rejected", job_id=p["jobId"], parameters={
                        "proposalId": p["proposalId"], "reason": "LOCAL_NOT_SENT"})
                    return {"proposalId": p["proposalId"], "status": "rejected", "reason": "LOCAL_NOT_SENT"}
                self._track_request(p["jobId"], run_id, request_id, "enqueue", p["proposalId"])
                persisted = copy.deepcopy(service._state)
                persisted_record = persisted["jobs"][p["jobId"]]["runtimeControl"]["attempts"][run_id]["proposals"][p["proposalId"]]
                persisted_record["requestId"] = request_id
                service._commit(persisted, operation, "queued", job_id=p["jobId"], approval="approved", parameters={
                    "proposalId": p["proposalId"], "requestId": request_id, "runId": run_id})
                return {"proposalId": p["proposalId"], "requestId": request_id, "status": "intent"}
        if operation in {"runtime.execution.pause", "runtime.execution.resume", "runtime.execution.cancel"}:
            exact(p, {"jobId"})
            with service._lock:
                run_id, session, _ = self._live(p["jobId"])
                command = {"pause": "pause-next", "resume": "resume", "cancel": "cancel-run"}[operation.rsplit(".", 1)[1]]
                service._commit(copy.deepcopy(service._state), operation, "requested", job_id=p["jobId"],
                                actor="user", parameters={"runId": run_id})
                request_id = self._request(p["jobId"], run_id, session, command, {})
                return {"requestId": request_id, "status": "requested"}
        raise WorkflowError("UNKNOWN_OPERATION", "Unsupported runtime control operation")

    def close(self):
        for _, session in list(self.sessions.values()):
            session.close()
        self.sessions.clear()
        self.approvals.clear()
        self.requests.clear()


def _validate_ready_identity(job_id, run_id, identity):
    if type(identity) is not dict or set(identity) != _IDENTITY_FIELDS:
        raise ValueError("Invalid native runtime identity")
    if (_canonical_uuid(identity["jobId"]) != job_id
            or _canonical_uuid(identity["runId"]) != run_id
            or _canonical_uuid(identity["taskId"]) != identity["taskId"]):
        raise ValueError("Native runtime attempt identity mismatch")
    _digest(identity["modelSha256"])
    _digest(identity["baseControllerRevision"])
