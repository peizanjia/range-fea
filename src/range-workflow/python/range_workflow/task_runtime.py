"""Host adapter for bounded persisted plans; reducers never execute tools.

All plan records live in the service's existing atomic state/audit commit.
The service inserts the queued-node receipt into the very same commit as its
accepted job. Approval tokens remain in the service's live instance only.
"""
from __future__ import annotations

import copy
from functools import lru_cache
import math
import threading
import time
import uuid

from .task_plan import PlanError, digest as plan_digest, validate_plan
from .task_scheduler import create_state, next_intents, reduce_event, validate_state

MAX_PLANS = 16
STOPPED = {"validated", "paused", "cancelled", "superseded", "completed", "failed", "conflict"}


class TaskManager:
    def __init__(self, service):
        self.service = service
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.thread = None
        self.execution_context = None
        self.observed_job_states = {}

    def catalog(self):
        capabilities = self.service.capabilities()
        entries = {}
        commands = {item["id"]: item for item in capabilities["commands"]}
        operations = {item["id"]: item for item in capabilities["operations"]}
        for name in ("workflow.configure", "workflow.run"):
            descriptor = commands[name]
            entries[name] = {"kind": "command", "parameters": descriptor["parameters"],
                "resources": [{"key": "project", "access": "write"}],
                "outputs": {"jobId": {"type": "string"}},
                "successPredicates": ["configure.matches" if name.endswith("configure") else "native.run.verified"]}
        entries["workflow.run"]["outputs"].update({
            "maxDisplacementErrorM": {"type": "number", "unit": "m"},
            "maxVelocityErrorMPerSecond": {"type": "number", "unit": "m/s"}})
        for name in ("jobs.query", "results.query", "result.summary.query"):
            parameters = copy.deepcopy(operations[name]["parameters"])
            parameters["required"] = list(set(parameters.get("required", [])) | {"jobId"})
            entries[name] = {"kind": "query", "parameters": parameters,
                "resources": [{"key": "project", "access": "read"}],
                "outputs": {"jobId": {"type": "string"}}, "successPredicates": ["query.identity"]}
        return entries

    def binding(self, configuration):
        from .knowledge import KnowledgeError, knowledge_binding
        from .service import WorkflowError, digest
        files = self.service._binding("workflow.configure", configuration)
        source_key = "modelInput" if "modelInput" in files else "template"
        try:
            knowledge = knowledge_binding(self.service.capabilities())
        except KnowledgeError as error:
            raise WorkflowError(error.code, error.message) from None
        environment = {"sourceId": configuration.get("sourceId"), "modelKind": configuration["model"],
            "files": {key: value for key, value in files.items() if key not in {"modelInput", "template"}}}
        return {"projectId": self.service._state["projectId"],
                "sourceSha256": files[source_key]["sha256"],
                "capabilitySha256": digest(self.catalog()),
                "environmentSha256": digest(environment),
                "knowledgePackSha256": knowledge["packSha256"]}

    def _commit_record(self, plan_id, record, event):
        candidate = copy.deepcopy(self.service._state)
        candidate["plans"][plan_id] = record
        self.service._commit(candidate, "task.plan." + event, record["kernel"]["state"],
                             parameters={"planId": plan_id})

    def _apply(self, record, event):
        record = copy.deepcopy(record)
        record["kernel"] = reduce_event(record["kernel"], event, time.time())
        return record

    @staticmethod
    def public(record):
        state = record["kernel"]
        nodes = []
        for spec in state["plan"]["nodes"]:
            observed = state["nodes"][spec["id"]]
            item = {"id": spec["id"], "kind": spec["kind"], "state": observed["state"],
                    "dependsOn": spec["dependsOn"], "counts": observed["counts"],
                    "successPredicate": spec["successPredicate"]}
            for key in ("jobId", "previewHash", "failure", "answer", "outputs"):
                if key in observed:
                    item[key] = copy.deepcopy(observed[key])
            if "tool" in spec:
                item["tool"] = spec["tool"]
                item["parameters"] = copy.deepcopy(spec["parameters"])
            if "question" in spec:
                item["question"] = spec["question"]
            if spec["id"] in record["previews"]:
                item["preview"] = copy.deepcopy(record["previews"][spec["id"]].get("details", {}))
            nodes.append(item)
        return {"schema": state["schema"], "planId": state["planId"],
                "planSha256": state["planSha256"], "title": state["plan"]["title"],
                "state": state["state"], "binding": state["binding"],
                "sourceRef": {"sourceId": record["configuration"].get("sourceId"),
                    "modelKind": record["configuration"]["model"],
                    "modelSha256": state["binding"]["sourceSha256"]},
                "budgets": state["plan"]["budgets"], "counts": state["counts"],
                "activeSeconds": state["activeSeconds"], "nodes": nodes}

    def restore(self, candidate):
        from .service import WorkflowError
        for plan_id, record in candidate["plans"].items():
            try:
                if set(record) != {"candidate", "kernel", "configuration", "previews"}:
                    raise PlanError("invalid stored adapter fields")
                normalized = validate_plan(record["candidate"], self.catalog())
                kernel = record["kernel"]
                validate_state(kernel)
                config = record["configuration"]
                if (type(config) is not dict or set(config) != {"dt", "steps", "targetDamping", "model", "sourceId"}
                        or type(config["steps"]) is not int or not 1 <= config["steps"] <= 5000
                        or type(config["dt"]) not in (float, int) or not 0 < config["dt"] <= .02
                        or type(config["targetDamping"]) not in (float, int) or not 0 < config["targetDamping"] <= .5
                        or config["model"] not in {"four-rod", "cantilever-beam"}
                        or type(config["sourceId"]) is not str or not 1 <= len(config["sourceId"]) <= 128
                        or type(record["previews"]) is not dict or len(record["previews"]) > 64):
                    raise PlanError("invalid persisted source configuration")
                if normalized != kernel["plan"] or kernel["planId"] != plan_id:
                    raise PlanError("stored normalized plan mismatch")
                for spec in kernel["plan"]["nodes"]:
                    node = kernel["nodes"][spec["id"]]
                    if spec["kind"] != "command" or "jobId" not in node:
                        continue
                    job = candidate["jobs"].get(node["jobId"])
                    association = job.get("taskPlanRef") if type(job) is dict else None
                    if (type(job) is not dict or job.get("command") != spec["tool"]
                            or job.get("projectId") != kernel["binding"]["projectId"]
                            or type(association) is not dict or set(association) != {"planId", "nodeId", "intentId"}
                            or association["planId"] != plan_id or association["nodeId"] != spec["id"]
                            or (node["state"] == "completed" and job["state"] != "succeeded")):
                        raise PlanError("stored task job association does not match accepted ledger")
                before = kernel["state"]
                kernel = reduce_event(kernel, {"type": "reopen"}, time.time())
                if before in {"validated", "paused"}:
                    kernel["state"] = before
                record["kernel"] = kernel
                record["previews"] = {}
            except (PlanError, KeyError, TypeError, ValueError):
                raise WorkflowError("CORRUPT_PLAN", "Stored plan is invalid; workspace metadata was preserved") from None

    def dispatch(self, operation, parameters):
        from .service import WorkflowError, exact, identifier
        if operation == "task.plan.propose":
            exact(parameters, {"plan"})
            if len(self.service._state["plans"]) >= MAX_PLANS:
                raise WorkflowError("PLAN_LIMIT", "The workspace has reached its retained plan limit")
            normalized = validate_plan(parameters["plan"], self.catalog())
            configurations = [n for n in normalized["nodes"] if n.get("tool") == "workflow.configure"]
            if len(configurations) > 4:
                raise WorkflowError("SCAN_LIMIT", "A plan may contain at most four configuration points")
            frozen = copy.deepcopy(self.service._state["configuration"])
            if "sourceId" not in frozen:
                raise WorkflowError("PLAN_SOURCE_REQUIRED", "Register and configure a supported native source before staging a plan")
            binding = self.binding(frozen)
            for node in configurations:
                if node["inputRefs"]:
                    raise WorkflowError("INVALID_PLAN", "Configuration points must declare fixed parameters")
                effective = self.service._normalize_builtin("workflow.configure", node["parameters"],
                    state={**self.service._state, "configuration": frozen})
                if (effective.get("sourceId") != frozen.get("sourceId") or effective["model"] != frozen["model"]
                        or self.binding(effective) != binding):
                    raise WorkflowError("SOURCE_CHANGED", "A plan cannot change its frozen source or environment")
            plan_id = str(uuid.uuid4())
            record = {"candidate": copy.deepcopy(parameters["plan"]),
                "kernel": create_state(normalized, binding, plan_id, time.time()),
                "configuration": frozen, "previews": {}}
            self._commit_record(plan_id, record, "propose")
            return self.public(record)
        if operation == "task.plan.query":
            exact(parameters, set(), {"planId"})
            if "planId" not in parameters:
                return {"plans": [self.public(record) for record in self.service._state["plans"].values()]}
        else:
            required = {"planId", "expectedPlanSha256"}
            if operation == "host.task.node.approve":
                required |= {"nodeId", "previewHash"}
            if operation == "task.plan.clarify":
                required |= {"nodeId", "answer"}
            exact(parameters, required)
        plan_id = identifier(parameters["planId"], "planId")
        if plan_id not in self.service._state["plans"]:
            raise WorkflowError("PLAN_NOT_FOUND", "The requested plan is unavailable")
        record = copy.deepcopy(self.service._state["plans"][plan_id])
        if operation == "task.plan.query":
            return self.public(record)
        if parameters["expectedPlanSha256"] != record["kernel"]["planSha256"]:
            raise WorkflowError("STALE_PLAN", "The plan approval does not match its immutable definition")
        if operation == "host.task.node.approve":
            node_id = parameters["nodeId"]
            preview = record["previews"].get(node_id, {})
            if preview.get("previewHash") != parameters["previewHash"]:
                raise WorkflowError("STALE_PLAN", "The node requires a fresh matching preview")
            if self.binding(record["configuration"]) != record["kernel"]["binding"]:
                raise WorkflowError("SOURCE_CHANGED", "The plan source or environment changed")
            self.service._dispatch("host.approve", {"previewId": preview["previewId"], "previewHash": preview["previewHash"]})
            event = {"type": "approval", "nodeId": node_id, "previewHash": parameters["previewHash"]}
        elif operation == "task.plan.clarify":
            event = {"type": "clarification", "nodeId": parameters["nodeId"], "answer": parameters["answer"],
                     "binding": record["kernel"]["binding"], "predicate": "clarification.received",
                     "trustedPredicateSatisfied": True}
        else:
            actions = {"task.plan.start": "resume", "task.plan.resume": "resume",
                       "task.plan.pause": "pause", "task.plan.cancel": "cancel",
                       "task.plan.supersede": "supersede"}
            if operation not in actions:
                raise WorkflowError("UNKNOWN_OPERATION", "Unsupported plan operation")
            event = {"type": actions[operation]}
            if event["type"] == "resume":
                record["previews"] = {}
        record = self._apply(record, event)
        self._commit_record(plan_id, record, event["type"])
        self.wake.set()
        return self.public(record)

    def associate_accepted_job(self, candidate, job):
        """Called before the service commits acceptance; never after job launch."""
        if self.execution_context is None:
            return
        plan_id, intent = self.execution_context
        record = candidate["plans"][plan_id]
        event = {"type": "queued", "nodeId": intent["nodeId"], "intentId": intent["id"],
                 "binding": record["kernel"]["binding"], "previewHash": job["previewHash"], "jobId": job["jobId"]}
        candidate["plans"][plan_id] = self._apply(record, event)
        job["taskPlanRef"] = {"planId": plan_id, "nodeId": intent["nodeId"], "intentId": intent["id"]}

    def _verified_outputs(self, record, node, job):
        from .native_results import query_registered
        from .service import WorkflowError
        if job["state"] != "succeeded" or job.get("projectId") != record["kernel"]["binding"]["projectId"]:
            raise WorkflowError("PLAN_PREDICATE_FAILED", "The host has no matching successful job")
        outputs = {"jobId": job["jobId"]}
        preview = self.service._state["previews"].get(job.get("previewId"), {})
        if (job.get("command") != node["tool"]
                or preview.get("binding", {}).get("modelInput", {}).get("sha256")
                    != record["kernel"]["binding"]["sourceSha256"]):
            raise WorkflowError("PLAN_PREDICATE_FAILED", "The job does not match the planned command and native source")
        if node["tool"] == "workflow.configure":
            effective = self.service._normalize_builtin("workflow.configure", node["parameters"],
                state={**self.service._state, "configuration": record["configuration"]})
            if job.get("result", {}).get("configuration") != effective:
                raise WorkflowError("PLAN_PREDICATE_FAILED", "Accepted configuration differs from the frozen point")
            return outputs
        result = job.get("result", {})
        if job["configuration"] != self.expected_configuration(record, node):
            raise WorkflowError("PLAN_PREDICATE_FAILED", "The run differs from the declared configuration point")
        mode = node["parameters"]["mode"]
        variants = ("active", "passive") if mode == "both" else (mode,)
        errors = {"maxDisplacementErrorM": [], "maxVelocityErrorMPerSecond": []}
        for variant in variants:
            values = result.get(variant, {})
            if values.get("acceptedSeq") != job["configuration"]["steps"]:
                raise WorkflowError("PLAN_PREDICATE_FAILED", "The requested accepted trajectory is incomplete")
            for key, bound in (("maxDisplacementErrorM", 1e-8), ("maxVelocityErrorMPerSecond", 1e-7),
                               ("maxRotationErrorRad", 1e-8), ("maxAngularVelocityErrorRadPerSecond", 1e-7)):
                value = values.get(key)
                if type(value) not in (float, int) or not math.isfinite(value) or value < 0 or value > bound:
                    raise WorkflowError("PLAN_PREDICATE_FAILED", "The independent native reference check failed")
                if key in errors:
                    errors[key].append(value)
            catalog = query_registered(self.service, "catalog", {"jobId": job["jobId"], "mode": variant, "limit": 1})
            if catalog.get("sourceKind") != "native-" + variant + "-fe":
                raise WorkflowError("PLAN_PREDICATE_FAILED", "A real verified native result is required")
        outputs.update({key: max(values) for key, values in errors.items()})
        return outputs

    def expected_configuration(self, record, node):
        by_id = {item["id"]: item for item in record["kernel"]["plan"]["nodes"]}
        @lru_cache(maxsize=64)
        def ancestors(ident):
            result = set()
            for dependency in by_id[ident]["dependsOn"]:
                result.add(dependency)
                result.update(ancestors(dependency))
            return frozenset(result)
        configs = [by_id[ident] for ident in ancestors(node["id"])
                   if by_id[ident].get("tool") == "workflow.configure"]
        latest = [item for item in configs if not any(
            item["id"] in ancestors(other["id"]) for other in configs if other["id"] != item["id"])]
        if len(latest) > 1:
            raise PlanError("ambiguous configuration point")
        if not latest:
            return copy.deepcopy(record["configuration"])
        return self.service._normalize_builtin("workflow.configure", latest[0]["parameters"],
            state={**self.service._state, "configuration": record["configuration"]})

    def _process_intent(self, plan_id, record, intent):
        from .service import WorkflowError
        node = next(n for n in record["kernel"]["plan"]["nodes"] if n["id"] == intent["nodeId"])
        parameters = copy.deepcopy(node["parameters"])
        for key, ref in node["inputRefs"].items():
            parameters[key] = record["kernel"]["nodes"][ref["node"]]["outputs"][ref["output"]]
        event = {"nodeId": node["id"], "intentId": intent["id"], "binding": record["kernel"]["binding"]}
        if intent["kind"] == "preview":
            if self.service._state["configuration"].get("sourceId") != record["configuration"].get("sourceId"):
                raise WorkflowError("PLAN_INPUT_CONFLICT", "The configured native source changed after this plan was staged")
            before = copy.deepcopy(self.service._state["configuration"])
            if node["tool"] == "workflow.configure":
                parameters = self.service._normalize_builtin("workflow.configure", parameters,
                    state={**self.service._state, "configuration": record["configuration"]})
            if node["tool"] == "workflow.run" and self.service._state["configuration"] != self.expected_configuration(record, node):
                raise WorkflowError("PLAN_INPUT_CONFLICT", "The current configuration changed after the planned point")
            preview = self.service._dispatch("command.preview", {"command": node["tool"], "parameters": parameters,
                "expectedRevision": self.service._state["revision"]})
            record["previews"][node["id"]] = {"previewId": preview["previewId"], "previewHash": preview["previewHash"],
                "details": {"beforeConfiguration": before, "effectiveConfiguration": preview["effectiveConfiguration"],
                            "normalized": preview["normalized"], "modelBinding": preview.get("modelBinding", {})}}
            event.update(type="preview", previewHash=preview["previewHash"])
        elif intent["kind"] == "submit":
            preview = record["previews"][node["id"]]
            token = self.service._approval_tokens.get(preview["previewId"])
            if not token:
                raise WorkflowError("APPROVAL_REQUIRED", "A live separate node approval is required")
            self.execution_context = (plan_id, intent)
            try:
                self.service._dispatch("command.execute", {"previewId": preview["previewId"], "approvalToken": token})
            finally:
                self.execution_context = None
            return None  # Node/job association was already committed atomically.
        elif intent["kind"] == "executionQuery":
            receipt = self.service._dispatch("execution.query", {"requestSha256": intent["previewHash"]})
            event.update(type="reconcile", previewHash=intent["previewHash"],
                         status="accepted" if receipt["accepted"] else "unobserved")
            if receipt["accepted"]:
                event["jobId"] = receipt["jobId"]
        elif intent["kind"] == "jobQuery":
            job = self.service._job(intent["jobId"])
            key = (plan_id, node["id"], job["jobId"])
            if job["state"] not in {"succeeded", "failed", "cancelled", "interrupted"}:
                if self.observed_job_states.get(key) == job["state"]:
                    return None  # Wait for a host state change, not a query loop.
                self.observed_job_states[key] = job["state"]
                event.update(type="jobObservation", jobId=job["jobId"], status="running")
            else:
                event.update(type="jobObservation", jobId=job["jobId"], status="failed")
                if job["state"] == "succeeded":
                    try:
                        outputs = self._verified_outputs(record, node, job)
                    except WorkflowError:
                        event.update(status="completed", predicate=node["successPredicate"],
                                     trustedPredicateSatisfied=False)
                    else:
                        event.update(status="completed", predicate=node["successPredicate"],
                                     trustedPredicateSatisfied=True, outputs=outputs)
        elif intent["kind"] == "query":
            self.service._dispatch(node["tool"], parameters)
            job = self.service._job(parameters["jobId"])
            if job.get("projectId") != record["kernel"]["binding"]["projectId"]:
                raise WorkflowError("PLAN_PREDICATE_FAILED", "The query belongs to another project")
            event.update(type="queryResult", status="completed", predicate=node["successPredicate"],
                         trustedPredicateSatisfied=True, outputs={"jobId": job["jobId"]})
        else:
            raise WorkflowError("INVALID_PLAN", "Unknown host dispatch intent")
        return self._apply(record, event)

    def pump(self):
        from .service import WorkflowError
        with self.service._lock:
            if self.service._closed or self.service._fault:
                return
            for plan_id in list(self.service._state["plans"]):
                record = copy.deepcopy(self.service._state["plans"][plan_id])
                if record["kernel"]["state"] in STOPPED:
                    continue
                try:
                    # Waiting is driven by authoritative host job transitions.
                    # Avoid repeated hashing/polling while a job is unchanged,
                    # and never spend a model call on the wait.
                    current = record["kernel"]
                    waiting = [intent for intent in current["intents"] if intent["kind"] == "jobQuery"]
                    ready = [intent for intent in current["intents"] if intent["kind"] != "jobQuery"]
                    if waiting and not ready and all(
                        self.service._job(intent["jobId"])["state"] not in {"succeeded", "failed", "cancelled", "interrupted"}
                        and self.observed_job_states.get((plan_id, intent["nodeId"], intent["jobId"]))
                            == self.service._job(intent["jobId"])["state"] for intent in waiting):
                        if time.time() - current["createdAt"] <= current["plan"]["budgets"]["deadlineSeconds"]:
                            continue
                    if (not current["intents"] and current["state"] in {"awaiting_approval", "awaiting_clarification"}
                            and not any(node.get("approved") for node in current["nodes"].values())):
                        if time.time() - current["createdAt"] <= current["plan"]["budgets"]["deadlineSeconds"]:
                            continue
                    if self.binding(record["configuration"]) != record["kernel"]["binding"]:
                        record["kernel"]["state"] = "conflict"
                        self._commit_record(plan_id, record, "conflict")
                        continue
                    advanced = self._apply(record, {"type": "tick"})
                    if advanced["kernel"] != record["kernel"]:
                        self._commit_record(plan_id, advanced, "dispatch-reserved")
                    record = advanced
                    for intent in next_intents(record["kernel"], time.time()):
                        # A preceding receipt can expire another node and remove
                        # its intent. Never dispatch a stale snapshot of outbox.
                        record = copy.deepcopy(self.service._state["plans"][plan_id])
                        live_node = record["kernel"]["nodes"][intent["nodeId"]]
                        if (live_node.get("intentId") != intent["id"] or not any(
                                item["id"] == intent["id"] for item in record["kernel"]["intents"])):
                            continue
                        try:
                            updated = self._process_intent(plan_id, record, intent)
                        except WorkflowError as error:
                            if intent["kind"] == "submit":
                                latest = copy.deepcopy(self.service._state["plans"][plan_id])
                                persisted_node = latest["kernel"]["nodes"][intent["nodeId"]]
                                updated = None if persisted_node.get("jobId") else self._apply(latest,
                                    {"type": "unknown", "nodeId": intent["nodeId"], "intentId": intent["id"]})
                            else:
                                if error.code in {"SOURCE_CHANGED", "PLAN_INPUT_CONFLICT", "CORRUPT_STATE", "CORRUPT_PLAN"}:
                                    raise
                                updated = self._apply(record, {"type": "hostFailure", "nodeId": intent["nodeId"],
                                    "intentId": intent["id"], "binding": record["kernel"]["binding"], "code": error.code})
                        if updated is not None:
                            self._commit_record(plan_id, updated, "observation")
                        record = copy.deepcopy(self.service._state["plans"][plan_id])
                        if record["kernel"]["state"] in STOPPED:
                            break
                    if record["kernel"]["state"] in STOPPED:
                        continue
                    for node in record["kernel"]["plan"]["nodes"]:
                        if record["kernel"]["nodes"][node["id"]]["state"] != "waiting_condition":
                            continue
                        condition = node["condition"]
                        value = record["kernel"]["nodes"][condition["ref"]["node"]]["outputs"][condition["ref"]["output"]]
                        if type(value) not in (int, float) or not math.isfinite(value):
                            raise WorkflowError("PLAN_PREDICATE_FAILED", "The condition has no finite host scalar")
                        operators = {"eq": lambda a, b: a == b, "lt": lambda a, b: a < b, "le": lambda a, b: a <= b,
                                     "gt": lambda a, b: a > b, "ge": lambda a, b: a >= b}
                        record = self._apply(record, {"type": "conditionResult", "nodeId": node["id"],
                            "binding": record["kernel"]["binding"], "predicate": "condition.matches",
                            "trustedPredicateSatisfied": operators[condition["operator"]](value, condition["value"])})
                        self._commit_record(plan_id, record, "condition-observed")
                        if record["kernel"]["state"] in STOPPED:
                            break
                except (PlanError, WorkflowError, KeyError, TypeError, ValueError):
                    record = copy.deepcopy(self.service._state["plans"][plan_id])
                    record["kernel"]["state"] = "conflict"
                    self._commit_record(plan_id, record, "host-verification-failed")

    def start(self):
        def drive():
            while not self.stop.is_set():
                self.wake.wait(0.25)
                self.wake.clear()
                try:
                    self.pump()
                except Exception:
                    # Persistence faults are already fail-closed in the service;
                    # this driver never retries submission on an exception.
                    self.stop.set()
        self.thread = threading.Thread(target=drive, daemon=True, name="RangeTaskPlans")
        self.thread.start()

    def close(self):
        self.stop.set()
        self.wake.set()
        if self.thread:
            self.thread.join(15)
