"""Pure persistent task reducer; host is responsible for atomic persistence.

create_state takes a validated normalized plan and immutable source binding.
reduce_event returns a copy. A tick reserves calls and records outbox intents;
next_intents reads that outbox only AFTER the host commits the returned state.
Events are internal host DTOs, never model tools or an authorization RPC.

Host evidence events require exact binding, predicate and
trustedPredicateSatisfied=True. This boolean is an assertion by trusted host
code, not a substitute for the host's numerical/evidence verification.
preview: nodeId,intentId,previewHash,binding
approval: nodeId,previewHash (host has separately authorized it)
queued: nodeId,intentId,jobId,previewHash,binding
queryResult/jobObservation: nodeId,intentId,binding,predicate,
 trustedPredicateSatisfied,outputs,status; jobObservation also jobId
reconcile: nodeId,intentId,binding,status=accepted|unobserved,jobId?,previewHash
conditionResult: nodeId,binding,predicate,trustedPredicateSatisfied,outputs
clarification: nodeId,answer,binding,predicate,trustedPredicateSatisfied
unknown: nodeId,intentId; pause/resume/cancel/supersede/reopen/tick: type only
chargeModel: nodeId; time is a finite monotonic elapsed-seconds value from host.

No approval tokens, protocol credentials, I/O, threads or model calls here.
"""
from __future__ import annotations

import copy
import math
import re

from .task_plan import PlanError, canonical, digest

BINDING_KEYS = {"projectId", "sourceSha256", "capabilitySha256", "environmentSha256",
                "knowledgePackSha256"}
HALTED = {"paused", "cancelled", "superseded", "completed", "failed", "conflict"}
TERMINAL = {"completed", "failed", "blocked_dependency", "skipped_condition"}


def _time(now):
    if type(now) not in (int, float) or not math.isfinite(now) or now < 0:
        raise PlanError("invalid host time")
    return now


def create_state(plan, binding, plan_id, now):
    """plan must be produced by validate_plan; no caller-defined resources."""
    _time(now)
    canonical(plan)
    if not isinstance(binding, dict) or set(binding) != BINDING_KEYS:
        raise PlanError("invalid immutable binding")
    if not isinstance(binding["projectId"], str) or not binding["projectId"]:
        raise PlanError("invalid project binding")
    for key in BINDING_KEYS - {"projectId"}:
        if not isinstance(binding[key], str) or not re.fullmatch(r"[0-9a-f]{64}", binding[key]):
            raise PlanError("invalid hash binding")
    if not isinstance(plan_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", plan_id):
        raise PlanError("invalid plan id")
    return {"schema": "range.task-state/v1", "planId": plan_id,
            "plan": copy.deepcopy(plan), "planSha256": digest(plan),
            "binding": copy.deepcopy(binding), "bindingSha256": digest(binding),
            "state": "validated", "createdAt": now,
            "lastAt": now, "activeSeconds": 0, "counts": {"modelCalls": 0, "toolCalls": 0},
            "intentSequence": 0, "intents": [], "nodes": {
                node["id"]: {"state": "pending", "attemptId": f"{plan_id}:{node['id']}:1",
                             "counts": {"modelCalls": 0, "toolCalls": 0},
                             "activeSeconds": 0, "outputs": {}}
                for node in plan["nodes"]}}


def next_intents(state, now):
    """Read committed outbox. Repeated reads return identical IDs (host dedupes)."""
    _time(now)
    if digest(state["plan"]) != state["planSha256"] or digest(state["binding"]) != state["bindingSha256"]:
        raise PlanError("immutable plan or binding changed")
    if now < state["lastAt"]:
        raise PlanError("host time moved backwards")
    if state["state"] in HALTED or now - state["createdAt"] > state["plan"]["budgets"]["deadlineSeconds"]:
        return []
    return copy.deepcopy(state["intents"])


def validate_state(state):
    """Validate untrusted persisted mutable state before any restore transition."""
    keys = {"schema", "planId", "plan", "planSha256", "binding", "bindingSha256", "state", "createdAt",
            "lastAt", "activeSeconds", "counts", "intentSequence", "intents", "nodes"}
    if type(state) is not dict or set(state) != keys or state["schema"] != "range.task-state/v1":
        raise PlanError("invalid persisted task state fields")
    baseline = create_state(state["plan"], state["binding"], state["planId"], state["createdAt"])
    if state["planSha256"] != baseline["planSha256"] or state["bindingSha256"] != baseline["bindingSha256"]:
        raise PlanError("persisted immutable task binding changed")
    states = HALTED | {"validated", "active", "awaiting_approval", "awaiting_clarification"}
    if type(state["state"]) is not str or state["state"] not in states:
        raise PlanError("unknown persisted plan state")
    _time(state["lastAt"])
    _time(state["activeSeconds"])
    if state["lastAt"] < state["createdAt"] or type(state["intentSequence"]) is not int or not 0 <= state["intentSequence"] <= 128:
        raise PlanError("invalid persisted plan clock or intent counter")
    def counts(value, limits):
        if type(value) is not dict or set(value) != {"modelCalls", "toolCalls"}:
            raise PlanError("invalid persisted call counts")
        if any(type(value[key]) is not int or not 0 <= value[key] <= limits[key] for key in value):
            raise PlanError("persisted call counts exceed bounds")
    counts(state["counts"], state["plan"]["budgets"])
    if type(state["nodes"]) is not dict or set(state["nodes"]) != set(baseline["nodes"]):
        raise PlanError("persisted task node identities changed")
    node_keys = {"state", "attemptId", "counts", "activeSeconds", "outputs", "intentId", "previewHash", "jobId",
                 "approved", "reconciliationUnobserved", "failure", "answer"}
    required = {"state", "attemptId", "counts", "activeSeconds", "outputs"}
    node_states = {"pending", "needs_preview", "awaiting_approval", "submitted", "waiting_job", "outcome_unknown",
                   "completed", "failed", "blocked_dependency", "skipped_condition", "waiting_condition", "awaiting_clarification"}
    total = {"modelCalls": 0, "toolCalls": 0}
    for spec in state["plan"]["nodes"]:
        node = state["nodes"][spec["id"]]
        if type(node) is not dict or set(node) - node_keys or required - set(node):
            raise PlanError("invalid persisted node fields")
        if node["attemptId"] != baseline["nodes"][spec["id"]]["attemptId"] or type(node["state"]) is not str or node["state"] not in node_states:
            raise PlanError("invalid persisted node identity or state")
        _time(node["activeSeconds"])
        counts(node["counts"], spec["budgets"])
        for key in total:
            total[key] += node["counts"][key]
        if type(node["outputs"]) is not dict or len(canonical(node["outputs"])) > 32768:
            raise PlanError("invalid persisted node output")
        for key in ("approved", "reconciliationUnobserved"):
            if key in node and type(node[key]) is not bool:
                raise PlanError("invalid persisted node flag")
        for key in ("intentId", "previewHash", "jobId", "failure", "answer"):
            if key in node and (type(node[key]) is not str or not 1 <= len(node[key]) <= (4096 if key == "answer" else 128)):
                raise PlanError("invalid persisted node reference")
        if "previewHash" in node and not re.fullmatch(r"[0-9a-f]{64}", node["previewHash"]):
            raise PlanError("invalid persisted preview hash")
        if spec["kind"] == "command" and node["state"] in {"waiting_job", "completed"} and not node.get("jobId"):
            raise PlanError("command state has no accepted job reference")
    if total != state["counts"]:
        raise PlanError("plan and node counters disagree")
    if state["state"] == "completed" and any(node["state"] != "completed" for node in state["nodes"].values()):
        raise PlanError("plan claims completion before its nodes")
    if type(state["intents"]) is not list or len(state["intents"]) > 64:
        raise PlanError("invalid persisted outbox")
    seen = set()
    for intent in state["intents"]:
        if type(intent) is not dict or intent.get("nodeId") not in state["nodes"]:
            raise PlanError("invalid persisted intent owner")
        ident = intent.get("id")
        node = state["nodes"][intent["nodeId"]]
        if (type(ident) is not str or ident in seen or node.get("intentId") != ident
                or intent.get("attemptId") != node["attemptId"] or intent.get("binding") != state["binding"]
                or intent.get("kind") not in {"preview", "submit", "query", "executionQuery", "jobQuery"}):
            raise PlanError("invalid persisted intent identity")
        seen.add(ident)
    if any(node.get("intentId") not in seen for node in state["nodes"].values() if "intentId" in node):
        raise PlanError("persisted node has no matching outbox intent")
    return copy.deepcopy(state)


def _evidence(state, event, spec, predicate=True):
    if event.get("binding") != state["binding"]:
        state["state"] = "conflict"
        return False
    if predicate and (event.get("predicate") != spec["successPredicate"] or
                      event.get("trustedPredicateSatisfied") is not True):
        return False
    return True


def _reserve(state, spec, kind):
    node = state["nodes"][spec["id"]]
    if state["counts"]["toolCalls"] >= state["plan"]["budgets"]["toolCalls"] or node["counts"]["toolCalls"] >= spec["budgets"]["toolCalls"]:
        node["state"] = "failed"
        node["failure"] = "tool call budget exhausted"
        return
    state["counts"]["toolCalls"] += 1
    node["counts"]["toolCalls"] += 1
    state["intentSequence"] += 1
    intent = {"id": f"{state['planId']}:intent:{state['intentSequence']}",
              "nodeId": spec["id"], "attemptId": node["attemptId"], "kind": kind,
              "binding": copy.deepcopy(state["binding"])}
    if kind in {"preview", "query"}:
        intent.update(tool=spec["tool"], parameters=copy.deepcopy(spec["parameters"]),
                      inputRefs=copy.deepcopy(spec["inputRefs"]))
    if kind in {"submit", "executionQuery"}:
        intent["previewHash"] = node["previewHash"]
    if kind == "jobQuery":
        intent["jobId"] = node["jobId"]
    node["intentId"] = intent["id"]
    state["intents"].append(intent)


def _finish(state):
    if state["state"] in HALTED:
        return
    nodes = state["nodes"]
    for spec in state["plan"]["nodes"]:
        node = nodes[spec["id"]]
        if node["state"] == "failed" and spec["failurePolicy"] == "stop":
            state["state"] = "failed"
            return
    if all(node["state"] in TERMINAL for node in nodes.values()):
        state["state"] = "completed" if all(node["state"] == "completed" for node in nodes.values()) else "failed"
    elif any(node["state"] == "awaiting_approval" for node in nodes.values()):
        state["state"] = "awaiting_approval"
    elif any(node["state"] == "awaiting_clarification" for node in nodes.values()):
        state["state"] = "awaiting_clarification"
    else:
        state["state"] = "active"


def _tick(state):
    nodes = state["nodes"]
    # Existing accepted jobs continue to be observed; never re-submit unknown work.
    for spec in state["plan"]["nodes"]:
        node = nodes[spec["id"]]
        if node["state"] == "waiting_job" and not node.get("intentId"):
            _reserve(state, spec, "jobQuery")
        elif node["state"] in {"submitted", "outcome_unknown"} and not node.get("intentId") and not node.get("jobId") and node.get("previewHash") and not node.get("reconciliationUnobserved"):
            _reserve(state, spec, "executionQuery")
    write_busy = any(n["state"] in {"needs_preview", "awaiting_approval", "submitted", "waiting_job", "outcome_unknown"}
                     and s["kind"] == "command" for s in state["plan"]["nodes"] for n in [nodes[s["id"]]])
    read_busy = sum(n["state"] == "submitted" and s["kind"] == "query"
                    for s in state["plan"]["nodes"] for n in [nodes[s["id"]]])
    for spec in state["plan"]["nodes"]:
        node = nodes[spec["id"]]
        if node["state"] == "awaiting_approval" and node.get("approved") and not node.get("intentId"):
            _reserve(state, spec, "submit")
            if node.get("intentId"):
                node["state"] = "submitted"
            continue
        if node["state"] != "pending":
            continue
        deps = [nodes[dep]["state"] for dep in spec["dependsOn"]]
        if any(dep in {"failed", "blocked_dependency", "skipped_condition"} for dep in deps):
            node["state"] = "blocked_dependency"
            continue
        if not all(dep == "completed" for dep in deps):
            continue
        if spec["kind"] == "clarification":
            node["state"] = "awaiting_clarification"
        elif spec["kind"] == "condition":
            # Host resolves finite/unit-bound whitelist scalar; no expressions here.
            node["state"] = "waiting_condition"
        elif spec["kind"] == "command" and not write_busy:
            _reserve(state, spec, "preview")
            if node.get("intentId"):
                node["state"] = "needs_preview"
                write_busy = True
        elif spec["kind"] == "query" and read_busy < state["plan"]["budgets"]["readOnlyParallel"]:
            _reserve(state, spec, "query")
            if node.get("intentId"):
                node["state"] = "submitted"
                read_busy += 1


def reduce_event(original, event, now):
    """Internal host event reducer. Reject illegal transitions without mutation."""
    _time(now)
    if not isinstance(event, dict) or not isinstance(event.get("type"), str):
        raise PlanError("invalid event")
    state = copy.deepcopy(original)
    if digest(state["plan"]) != state["planSha256"] or digest(state["binding"]) != state["bindingSha256"]:
        raise PlanError("immutable plan or binding changed")
    if now < state["lastAt"]:
        raise PlanError("host time moved backwards")
    elapsed = now - state["lastAt"]
    if state["state"] not in HALTED and state["state"] not in {"awaiting_approval", "awaiting_clarification"}:
        busy = [n for n in state["nodes"].values() if n["state"] in {"needs_preview", "submitted", "waiting_condition"}]
        if busy:
            state["activeSeconds"] += elapsed
            for node in busy:
                node["activeSeconds"] += elapsed
    state["lastAt"] = now
    kind = event["type"]
    if kind in {"pause", "cancel", "supersede"}:
        if state["state"] not in {"completed", "failed", "cancelled", "superseded", "conflict"}:
            state["state"] = {"pause": "paused", "cancel": "cancelled", "supersede": "superseded"}[kind]
        return state
    if kind in {"resume", "reopen"}:
        if state["state"] in {"cancelled", "superseded", "completed", "failed", "conflict"}:
            return state
        state["intents"] = []
        for node in state["nodes"].values():
            node.pop("intentId", None)
            node.pop("approved", None)
            if node["state"] in {"needs_preview", "awaiting_approval"}:
                node["state"] = "pending"
                node.pop("previewHash", None)
            elif node["state"] == "submitted" and not node.get("jobId") and not node.get("previewHash"):
                node["state"] = "pending"  # read-only query can safely be reissued
        state["state"] = "active"
    elif kind == "tick":
        if state["state"] not in HALTED:
            _tick(state)
    else:
        ident = event.get("nodeId")
        spec = next((s for s in state["plan"]["nodes"] if s["id"] == ident), None)
        if not spec:
            raise PlanError("unknown node")
        node = state["nodes"][ident]
        if kind == "chargeModel":
            if state["state"] in HALTED or node["state"] in {"waiting_job", "outcome_unknown"}:
                raise PlanError("model call prohibited")
            if state["counts"]["modelCalls"] >= state["plan"]["budgets"]["modelCalls"] or node["counts"]["modelCalls"] >= spec["budgets"]["modelCalls"]:
                node.update(state="failed", failure="model call budget exhausted")
            else:
                state["counts"]["modelCalls"] += 1
                node["counts"]["modelCalls"] += 1
        elif kind == "approval":
            if state["state"] in HALTED or node["state"] != "awaiting_approval" or event.get("previewHash") != node.get("previewHash"):
                raise PlanError("approval does not match live preview")
            node["approved"] = True
        elif kind in {"conditionResult", "clarification"}:
            expected = "waiting_condition" if kind == "conditionResult" else "awaiting_clarification"
            if node["state"] != expected or state["state"] in HALTED:
                raise PlanError("unexpected control result")
            if _evidence(state, event, spec):
                if kind == "clarification":
                    answer = event.get("answer")
                    if not isinstance(answer, str) or not 1 <= len(answer) <= 4096:
                        raise PlanError("invalid clarification answer")
                    node["answer"] = answer
                node["state"] = "completed"
            elif state["state"] != "conflict":
                node["state"] = "failed"
                node["failure"] = "required predicate failed"
        elif kind in {"preview", "queued", "queryResult", "jobObservation", "reconcile", "unknown", "hostFailure"}:
            intent_id = event.get("intentId")
            intent = next((i for i in state["intents"] if i["id"] == intent_id and i["nodeId"] == ident), None)
            if intent is None or intent_id != node.get("intentId"):
                raise PlanError("unexpected or stale intent receipt")
            expected = {"preview": {"preview"}, "queued": {"submit"}, "queryResult": {"query"},
                        "jobObservation": {"jobQuery"}, "reconcile": {"executionQuery"},
                        "unknown": {"submit", "query", "preview", "jobQuery", "executionQuery"},
                        "hostFailure": {"query", "preview", "jobQuery", "executionQuery"}}
            if intent["kind"] not in expected[kind]:
                raise PlanError("receipt kind mismatch")
            state["intents"] = [i for i in state["intents"] if i["id"] != intent_id]
            node.pop("intentId", None)
            if kind == "hostFailure":
                if not _evidence(state, event, spec, predicate=False):
                    pass
                else:
                    code = event.get("code")
                    if type(code) is not str or not 1 <= len(code) <= 128:
                        raise PlanError("invalid host failure code")
                    node.update(state="failed", failure=code)
            elif kind == "unknown":
                node["state"] = "outcome_unknown"
            elif not _evidence(state, event, spec, predicate=False):
                pass
            elif kind == "preview":
                preview_hash = event.get("previewHash")
                if not isinstance(preview_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", preview_hash):
                    raise PlanError("invalid preview hash")
                node.update(state="awaiting_approval", previewHash=preview_hash)
            elif kind in {"queued", "reconcile"}:
                if event.get("previewHash") != node.get("previewHash"):
                    raise PlanError("receipt preview mismatch")
                if kind == "reconcile" and event.get("status") == "unobserved":
                    node["state"] = "outcome_unknown"
                    node["reconciliationUnobserved"] = True
                else:
                    if kind == "reconcile" and event.get("status") != "accepted":
                        raise PlanError("invalid reconciliation status")
                    job_id = event.get("jobId")
                    if not isinstance(job_id, str) or not 1 <= len(job_id) <= 128 or (node.get("jobId") and node["jobId"] != job_id):
                        raise PlanError("invalid or changed accepted job id")
                    node.update(state="waiting_job", jobId=job_id)
                    node.pop("approved", None)
            else:
                if kind == "jobObservation" and event.get("jobId") != node.get("jobId"):
                    raise PlanError("observation is for another job")
                status = event.get("status")
                if status == "running" and kind == "jobObservation":
                    node["state"] = "waiting_job"
                elif status == "completed" and _evidence(state, event, spec):
                    outputs = event.get("outputs", {})
                    if not isinstance(outputs, dict) or len(canonical(outputs)) > 32768:
                        raise PlanError("invalid output evidence")
                    node.update(state="completed", outputs=copy.deepcopy(outputs))
                elif status in {"completed", "failed", "cancelled", "interrupted"}:
                    node.update(state="failed", failure="required predicate failed")
                else:
                    raise PlanError("invalid observation status")
        else:
            raise PlanError("unknown internal event")
    if state["state"] not in {"cancelled", "superseded", "completed", "failed", "conflict"}:
        if now - state["createdAt"] > state["plan"]["budgets"]["deadlineSeconds"] or state["activeSeconds"] > state["plan"]["budgets"]["activeSeconds"]:
            state.update(state="failed", intents=[])
            for node in state["nodes"].values():
                node.pop("intentId", None)
        for spec in state["plan"]["nodes"]:
            node = state["nodes"][spec["id"]]
            if (node["activeSeconds"] > spec["budgets"]["activeSeconds"]
                    and original["nodes"][spec["id"]]["state"] not in TERMINAL):
                node.update(state="failed", failure="active time budget exhausted")
                node.pop("intentId", None)
                state["intents"] = [intent for intent in state["intents"] if intent["nodeId"] != spec["id"]]
        _finish(state)
    return state
